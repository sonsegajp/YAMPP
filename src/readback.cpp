#include "gfx/frame.hpp"
#include "webgpu/gpu.hpp"
#include <cstdio>
#include <memory>
#include <filesystem>
extern "C" int melee_prepare_user_paths() {
    std::error_code error;
    std::filesystem::create_directories("user/cache", error);
    return !error;
}
extern "C" int melee_write_frame(const char* path) {
    using namespace aurora;
    gfx::gpu_synchronize();
    auto& fb = webgpu::g_graphicsConfig.msaaSamples > 1
        ? webgpu::g_frameBufferResolved : webgpu::g_frameBuffer;
    if (!fb.texture) return 0;
    const uint32_t width = fb.size.width, height = fb.size.height;
    const uint32_t stride = (width * 4 + 255) & ~255u;
    const uint64_t bytes = uint64_t(stride) * height;
    wgpu::BufferDescriptor desc{};
    desc.label = "Melee PC renderer capture";
    desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    desc.size = bytes;
    auto buffer = webgpu::g_device.CreateBuffer(&desc);
    wgpu::TexelCopyTextureInfo src{};
    src.texture = fb.texture;
    wgpu::TexelCopyBufferInfo dst{};
    dst.buffer = buffer;
    dst.layout.bytesPerRow = stride;
    dst.layout.rowsPerImage = height;
    wgpu::Extent3D extent{width, height, 1};
    auto encoder = webgpu::g_device.CreateCommandEncoder();
    encoder.CopyTextureToBuffer(&src, &dst, &extent);
    auto command = encoder.Finish();
    webgpu::g_queue.Submit(1, &command);
    auto success = std::make_shared<bool>(false);
    auto future = buffer.MapAsync(wgpu::MapMode::Read, 0, bytes,
        wgpu::CallbackMode::WaitAnyOnly,
        [success](wgpu::MapAsyncStatus status, wgpu::StringView) {
            *success = status == wgpu::MapAsyncStatus::Success;
        });
    auto status = webgpu::g_instance.WaitAny(future, UINT64_MAX);
    if (status != wgpu::WaitStatus::Success || !*success) return 0;
    auto* pixels = static_cast<const unsigned char*>(buffer.GetConstMappedRange());
    FILE* file = fopen(path, "wb");
    if (!file) { buffer.Unmap(); return 0; }
    fprintf(file, "P6\n%u %u\n255\n", width, height);
    bool ok = true;
    const bool bgra = fb.format == wgpu::TextureFormat::BGRA8Unorm ||
                      fb.format == wgpu::TextureFormat::BGRA8UnormSrgb;
    for (uint32_t y=0; y<height; ++y)
        for (uint32_t x=0; x<width; ++x)
            {
                const auto* pixel = pixels + y*stride + x*4;
                const unsigned char rgb[3] = {pixel[bgra ? 2 : 0], pixel[1], pixel[bgra ? 0 : 2]};
                if (fwrite(rgb, 1, 3, file) != 3) ok = false;
            }
    if (fclose(file) != 0) ok = false;
    buffer.Unmap();
    return ok;
}
