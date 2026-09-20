/* Headless readback of aurora's actual output.
 *
 * Why this exists: the host's screenshot path decodes the *guest* XFB out of
 * MEM1. aurora never writes guest memory -- it renders to its own GPU texture
 * and presents to an SDL window. So every "frame is black" reading was taken
 * from a surface aurora does not touch, and proved nothing either way.
 *
 * This copies g_frameBufferResolved back to the CPU so the frame we screenshot
 * is the frame aurora actually drew. Resolved, not g_frameBuffer: the latter is
 * multisampled and cannot be the source of a texture-to-buffer copy.
 */
#include "aurora_shim.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#ifdef AUSHIM_SETTINGS
#include <imgui.h>
#include <backends/imgui_impl_wgpu.h>
#endif
#include <memory>
#include <string_view>

#include <aurora/gfx.h>
#include "gfx/frame.hpp"
#include "webgpu/gpu.hpp"
#include "gx/gx.hpp"
#include "gx/texture.hpp"
#include "gx/command_processor.hpp"
#include <atomic>
#include <vector>

#ifndef AURORA_ALIGN
#define AURORA_ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#endif

using namespace aurora;

namespace {
void copy_string(char* dst, size_t capacity, wgpu::StringView value)
{
  if (capacity == 0) {
    return;
  }
  const std::string_view src = value.IsUndefined()
                                   ? std::string_view{"Unknown"}
                                   : std::string_view{value};
  const size_t count = std::min(capacity - 1, src.size());
  std::memcpy(dst, src.data(), count);
  dst[count] = '\0';
}
} // namespace

extern "C" int aushim_gpu_is_hardware_vulkan_internal(void)
{
  return webgpu::g_device &&
         webgpu::g_backendType == wgpu::BackendType::Vulkan &&
         webgpu::g_adapterInfo.adapterType != wgpu::AdapterType::CPU;
}

extern "C" AUSHIM_API
int aushim_get_gpu_info(AushimGpuInfo* out_info, uint32_t out_info_size)
{
  if (out_info == nullptr || out_info_size < 4 * sizeof(uint32_t)) {
    return 0;
  }

  AushimGpuInfo info{};
  info.struct_size = sizeof(info);
  info.abi_version = AUSHIM_ABI_VERSION;
  info.backend = webgpu::g_backendType == wgpu::BackendType::Vulkan
                     ? AUSHIM_BACKEND_VULKAN
                     : -1;
  if (aushim_is_initialized()) {
    info.flags |= AUSHIM_GPU_INITIALIZED;
  }
  if (aushim_is_frame_active()) {
    info.flags |= AUSHIM_GPU_FRAME_ACTIVE;
  }
  if (webgpu::g_backendType == wgpu::BackendType::Vulkan) {
    info.flags |= AUSHIM_GPU_VULKAN;
  }
  if (webgpu::g_adapterInfo.adapterType == wgpu::AdapterType::CPU) {
    info.flags |= AUSHIM_GPU_CPU_ADAPTER;
  } else if (aushim_is_initialized()) {
    info.flags |= AUSHIM_GPU_HARDWARE_ADAPTER;
  }
  copy_string(info.adapter_name, sizeof(info.adapter_name),
              webgpu::g_adapterInfo.device);
  copy_string(info.driver_description, sizeof(info.driver_description),
              webgpu::g_adapterInfo.description);

  const size_t copy_size =
      std::min(static_cast<size_t>(out_info_size), sizeof(info));
  std::memcpy(out_info, &info, copy_size);
  return 1;
}

/* aurora_end_frame() hands the frame to a render worker.  A readback issued
 * immediately afterwards otherwise races that worker and can copy the old
 * contents (or a texture which is still being rendered). */
extern "C" AUSHIM_API
void aushim_synchronize(void)
{
  if (!aushim_is_initialized() || aushim_is_frame_active()) {
    return;
  }
  gfx::gpu_synchronize();
}

extern "C" AUSHIM_API
int aushim_readback(uint8_t* dst, uint32_t* out_w, uint32_t* out_h, uint32_t cap)
{
  if (!aushim_is_initialized() || aushim_is_frame_active() || dst == nullptr) {
    return 0;
  }
  /* aurora_end_frame hands work to the render worker. Synchronize here as well
   * as exposing aushim_synchronize so every readback is race-free on its own. */
  gfx::gpu_synchronize();

  /* recording.cpp only sets resolveView when sampleCount > 1, so with MSAA at
   * the default 1 sample nothing is ever resolved and g_frameBufferResolved
   * stays blank. Pick whichever texture the render pass actually targeted. */
  const bool msaa = webgpu::g_graphicsConfig.msaaSamples > 1;
  auto& fb = msaa ? webgpu::g_frameBufferResolved : webgpu::g_frameBuffer;
  if (!fb.texture || !webgpu::g_device || !webgpu::g_queue) {
    return 0;
  }

  const uint32_t w = fb.size.width;
  const uint32_t h = fb.size.height;
  if (w == 0 || h == 0) {
    return 0;
  }

  /* CopyTextureToBuffer requires bytesPerRow aligned to 256. */
  const uint32_t bpp = 4;
  const uint32_t bytesPerRow = (uint32_t)AURORA_ALIGN(w * bpp, 256);
  const uint64_t byteSize = (uint64_t)bytesPerRow * h;

  const wgpu::BufferDescriptor bd{
      .label = "Shim Readback Buffer",
      .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
      .size = byteSize,
  };
  wgpu::Buffer buf = webgpu::g_device.CreateBuffer(&bd);
  if (!buf) {
    return 0;
  }

  wgpu::TexelCopyTextureInfo src{
      .texture = fb.texture,
      .mipLevel = 0,
      .origin = wgpu::Origin3D{0, 0, 0},
      .aspect = wgpu::TextureAspect::All,
  };
  const wgpu::TexelCopyBufferInfo dstInfo{
      .layout = wgpu::TexelCopyBufferLayout{
          .offset = 0,
          .bytesPerRow = bytesPerRow,
          .rowsPerImage = h,
      },
      .buffer = buf,
  };
  const wgpu::Extent3D extent{w, h, 1};

  wgpu::CommandEncoder enc = webgpu::g_device.CreateCommandEncoder();
#ifdef AUSHIM_SETTINGS
  /* Optional full-UI diagnostics: render the same completed ImGui commands as
     the swapchain onto a private copy. The gameplay framebuffer stays intact.
     Synchronization above also makes the backend's upload buffers safe here. */
  wgpu::Texture captured_ui;
  if (const char* capture = std::getenv("MELEE_CAPTURE_UI"); capture && *capture == '1') {
    ImDrawData* draw = ImGui::GetDrawData();
    if (draw && draw->Valid && draw->CmdListsCount && draw->DisplaySize.x > 0 && draw->DisplaySize.y > 0) {
      wgpu::TextureDescriptor td{};
      td.label = "Diagnostic game and UI capture";
      td.dimension = wgpu::TextureDimension::e2D;
      td.size = extent;
      td.format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
      td.mipLevelCount = 1;
      td.sampleCount = 1;
      td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc;
      captured_ui = webgpu::g_device.CreateTexture(&td);
      wgpu::TexelCopyTextureInfo target{};
      target.texture = captured_ui;
      enc.CopyTextureToTexture(&src, &target, &extent);
      wgpu::RenderPassColorAttachment attachment{};
      attachment.view = captured_ui.CreateView();
      attachment.loadOp = wgpu::LoadOp::Load;
      attachment.storeOp = wgpu::StoreOp::Store;
      wgpu::RenderPassDescriptor pass_desc{};
      pass_desc.colorAttachmentCount = 1;
      pass_desc.colorAttachments = &attachment;
      auto pass = enc.BeginRenderPass(&pass_desc);
      ImDrawData scaled = *draw;
      scaled.FramebufferScale = ImVec2(w / draw->DisplaySize.x, h / draw->DisplaySize.y);
      ImGui_ImplWGPU_RenderDrawData(&scaled, pass.Get());
      pass.End();
      src.texture = captured_ui;
    }
  }
#endif
  enc.CopyTextureToBuffer(&src, &dstInfo, &extent);
  wgpu::CommandBuffer cmd = enc.Finish();
  webgpu::g_queue.Submit(1, &cmd);

  struct MapState {
    bool done = false;
    bool ok = false;
  };
  auto state = std::make_shared<MapState>();
  const wgpu::Future f = buf.MapAsync(
      wgpu::MapMode::Read, 0, byteSize, wgpu::CallbackMode::WaitAnyOnly,
      [state](wgpu::MapAsyncStatus status, wgpu::StringView) {
        state->ok = (status == wgpu::MapAsyncStatus::Success);
        state->done = true;
      });
  const auto waitStatus = webgpu::g_instance.WaitAny(f, 5000000000ULL);
  if (waitStatus != wgpu::WaitStatus::Success || !state->done || !state->ok) {
    buf.Destroy();
    return 0;
  }

  const uint8_t* mapped =
      static_cast<const uint8_t*>(buf.GetConstMappedRange(0, byteSize));
  if (mapped == nullptr) {
    buf.Unmap();
    buf.Destroy();
    return 0;
  }

  /* Emit tightly packed RGB, dropping the row padding the 256 alignment added.
   * Surface format is BGRA on D3D, so swizzle rather than trusting byte order. */
  const bool bgra = (webgpu::g_graphicsConfig.surfaceConfiguration.format ==
                     wgpu::TextureFormat::BGRA8Unorm) ||
                    (webgpu::g_graphicsConfig.surfaceConfiguration.format ==
                     wgpu::TextureFormat::BGRA8UnormSrgb);
  const uint32_t need = w * h * 3;
  if (cap < need) {
    buf.Unmap();
    buf.Destroy();
    return 0;
  }
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = mapped + (size_t)y * bytesPerRow;
    uint8_t* o = dst + (size_t)y * w * 3;
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t a = row[x * 4 + 0];
      const uint8_t b = row[x * 4 + 1];
      const uint8_t c = row[x * 4 + 2];
      o[x * 3 + 0] = bgra ? c : a;   /* R */
      o[x * 3 + 1] = b;              /* G */
      o[x * 3 + 2] = bgra ? a : c;   /* B */
    }
  }

  buf.Unmap();
  buf.Destroy();
  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  return 1;
}

namespace {
std::atomic<bool> content_loading{false}, content_ready{false};
wgpu::Texture content_backdrop;
wgpu::TextureView content_backdrop_view;
}
extern "C" void aushim_content_caption(void);
extern "C" AUSHIM_API void aushim_content_draw(void) {
 if(!content_loading.load())return;
#ifdef AUSHIM_SETTINGS
 auto* draw=ImGui::GetForegroundDrawList();const auto size=ImGui::GetIO().DisplaySize;
 if(content_backdrop_view)draw->AddImage((ImTextureID)content_backdrop_view.Get(),{0,0},size);
 draw->AddRectFilled({0,0},size,IM_COL32(0,0,0,80));
 aushim_content_caption();
#endif
}
extern "C" AUSHIM_API void aushim_content_ready(void){content_ready.store(true);}
extern "C" void aushim_content_frame_done(void){
 if(content_ready.exchange(false)){
  content_loading.store(false);
  /* Screenshot readback can replay this completed ImGui draw list after the
   * renderer worker finishes. Keep its view alive until the next content
   * capture (after a new menu frame) or explicit application shutdown. */
 }
}
extern "C" void aushim_content_release(void){
 gfx::gpu_synchronize();
 content_loading.store(false);content_ready.store(false);
 content_backdrop_view={};content_backdrop={};
}
extern "C" void aushim_content_capture(void){
 if(!aushim_is_initialized())return;
 aushim_synchronize();
 std::vector<uint8_t> rgb(4096u*2160u*3u);uint32_t width=0,height=0;
 if(aushim_readback(rgb.data(),&width,&height,(uint32_t)rgb.size())&&width&&height){
  std::vector<uint8_t> rgba((size_t)width*height*4);
  for(size_t i=0;i<(size_t)width*height;i++){std::memcpy(rgba.data()+i*4,rgb.data()+i*3,3);rgba[i*4+3]=255;}
  wgpu::TextureDescriptor desc{};desc.dimension=wgpu::TextureDimension::e2D;desc.size={width,height,1};
  desc.format=wgpu::TextureFormat::RGBA8Unorm;desc.mipLevelCount=desc.sampleCount=1;
  desc.usage=wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::CopyDst;
  content_backdrop=webgpu::g_device.CreateTexture(&desc);content_backdrop_view=content_backdrop.CreateView();
  wgpu::TexelCopyTextureInfo dest{};dest.texture=content_backdrop;
  wgpu::TexelCopyBufferLayout layout{};layout.bytesPerRow=width*4;layout.rowsPerImage=height;
  wgpu::Extent3D extent{width,height,1};webgpu::g_queue.WriteTexture(&dest,rgba.data(),rgba.size(),&layout,&extent);
 }
 content_ready.store(false);content_loading.store(true);
 gx::g_gxState={};gx::g_gxState.dirty=gx::DirtyAll;gx::clear_static_texture_cache();gx::fifo::clear_draw_cache();gx::texture::invalidate_bindings();
}
