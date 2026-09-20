from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=root/"upstream/aurora/include/dolphin/gx/GXAurora.h"
s=p.read_text()
if "#define GX_AURORA_INVALIDATE_ARRAY " not in s:
    s += "\n/* Native FIFO bridge: invalidate reused vertex-array snapshot addresses. */\n#define GX_AURORA_INVALIDATE_ARRAY 0x0042\n"
p.write_text(s)
p=root/"upstream/aurora/lib/gx/command_processor.cpp"
s=p.read_text()
key="  } else if (subCmd == GX_AURORA_DEBUG_GROUP_PUSH) {"
s=s if "subCmd == GX_AURORA_INVALIDATE_ARRAY" in s else s.replace(key,'''  } else if (subCmd == GX_AURORA_INVALIDATE_ARRAY) {
    const u32 cpIdx = reader.read<u8>();
    const u32 attrIdx = GX_VA_POS + cpIdx;
    const u32 revision = reader.read<u32>();
    CHECK(attrIdx < g_gxState.arrays.size(), "invalid translated array index {}", cpIdx);
    // This command marks changed content even when an allocator reuses a pointer.
    (void)revision;
    g_gxState.arrays[attrIdx].cachedRange = {};
    g_gxState.dirty |= DirtyImmediates;
'''+key)
p.write_text(s)
