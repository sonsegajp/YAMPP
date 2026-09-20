"""Apply idempotent runtime fixes to the pinned Aurora checkout."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=root/"upstream/aurora/lib/gx/gx.cpp"
s=p.read_text()
if "  sEmptyTextureView = {};" not in s:
 s=s.replace("void shutdown() noexcept {\n", "void shutdown() noexcept {\n  // Release fallback GPU objects while the WebGPU device is still alive.\n  g_emptyTextureBindGroup = {};\n  sPipelineLayout = {};\n  sEmptySampler = {};\n  sEmptyTextureView = {};\n  sEmptyTexture = {};\n",1)
p.write_text(s)
p=root/"upstream/aurora/lib/webgpu/gpu.cpp"
s=p.read_text()
if "  g_adapterInfo = {};" not in s:
 s=s.replace("  g_adapter = {};\n", "  // WebGPU output structures own allocated members; free before DLL teardown.\n  g_adapterInfo = {};\n  g_surfaceCapabilities = {};\n  g_adapter = {};\n",1)
p.write_text(s)

# GXSetViewportJitter adds 342 to both origins, just like GXSetScissor.
# Subtracting 340 shifts rendering by two pixels and leaves black shadow-map
# borders that clamp-to-edge sampling spreads across the stage materials.
p=root/"upstream/aurora/lib/gx/regs.cpp"
s=p.read_text()
s=s.replace(".left = ox - 340.0f - width / 2.0f,", ".left = ox - 342.0f - width / 2.0f,")
s=s.replace(".top = oy - 340.0f - height / 2.0f,", ".top = oy - 342.0f - height / 2.0f,")
p.write_text(s)

# Preserve physical GameCube trigger switches, including SDL-recognized
# third-party adapters. Melee already clamps and dead-zones raw pad axes.
p=root/"upstream/aurora/lib/input.cpp"
s=p.read_text()
s=s.replace("controller.m_isGameCube = controller.m_vid == 0x057E && controller.m_pid == 0x0337;",
 "controller.m_isGameCube = (controller.m_vid == 0x057E && controller.m_pid == 0x0337) || SDL_GetGamepadType(ctrl) == SDL_GAMEPAD_TYPE_GAMECUBE;")
if "if (controller.m_isGameCube) controller.m_deadZones.useDeadzones = false;" not in s:
 s=s.replace("    const auto props = SDL_GetGamepadProperties(ctrl);", "    if (controller.m_isGameCube) controller.m_deadZones.useDeadzones = false;\n    const auto props = SDL_GetGamepadProperties(ctrl);")
p.write_text(s)

# Missing GX texture coordinates keep the default texgen input.
# Reference: Dolphin VertexShaderGen.cpp initializes coord=(0,0,1,1) and
# only substitutes raw texture coordinates when that vertex component exists.
# Akaneia Wolf's effects exercise this legal missing-attribute path.
p=root/"upstream/aurora/lib/gx/shader.cpp"
s=p.read_text()
old='    UNLIKELY FATAL("unmapped vtx attr {}", underlying(attr));'
new='    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {\n      return "vec2f(0.0, 0.0)"s;\n    }\n'+old
if 'return "vec2f(0.0, 0.0)"s;' not in s:
 assert s.count(old)==1, "Aurora missing-attribute fallback changed upstream"
 s=s.replace(old,new,1)
p.write_text(s)
