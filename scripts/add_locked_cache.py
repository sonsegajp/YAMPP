from pathlib import Path
import re,json,sys
root=Path(__file__).resolve().parents[1]
p=root/"native/recompiler/emit.py"
s=p.read_text()
s=s.replace('return [f"memset({RAM()} + ({ea} & RAM_MASK), 0, 32);"]',
            'return [f"{{ uint32_t cache_ea = {ea}; for (uint32_t k = 0; k < 32; k += 4) mem_w32(cache_ea + k, 0); }}"]')
needle='''            if f.get('spr') is not None:
                return [f"ctx->{self._spr_field(f['spr'])} = {R(f['rD'])};"]'''
replacement='''            if f.get('spr') is not None:
                if f['spr'] in (922, 923):
                    return [f"hle_write_spr(ctx, {f['spr']}, {R(f['rD'])});"]
                return [f"ctx->{self._spr_field(f['spr'])} = {R(f['rD'])};"]'''
assert needle in s
p.write_text(s.replace(needle,replacement))
p=root/"native/runtime/abi_recompcore.h"
s=p.read_text().replace("void     hle_syscall(Context* ctx);","void     hle_syscall(Context* ctx);\nvoid hle_write_spr(Context* ctx, uint32_t spr, uint32_t value);")
p.write_text(s)
p=root/"native/host/gxrt/frontend_bus.c"
s=p.read_text().replace("static u64 fe_ext_read(CPUState* cpu, u32 ea, u8 size)\n{","static u8 s_locked_cache[0x4000];\nstatic u64 fe_ext_read(CPUState* cpu, u32 ea, u8 size)\n{\n    if (ea >= 0xE0000000u && ea - 0xE0000000u + size <= sizeof s_locked_cache) {\n        u64 value = 0;\n        for (u8 i=0; i<size; ++i) value=(value<<8)|s_locked_cache[ea-0xE0000000u+i];\n        return value;\n    }")
s=s.replace("static void fe_ext_write(CPUState* cpu, u32 ea, u64 value, u8 size)\n{","static void fe_ext_write(CPUState* cpu, u32 ea, u64 value, u8 size)\n{\n    if (ea >= 0xE0000000u && ea - 0xE0000000u + size <= sizeof s_locked_cache) {\n        for (u8 i=0; i<size; ++i) s_locked_cache[ea-0xE0000000u+i]=(u8)(value>>(8*(size-1-i)));\n        return;\n    }")
p.write_text(s)
# Preserve the original emission manifest, independent of future symbol-map changes.
generated=root/"build/native-game/generated"
manifest=[]
for f in sorted(generated.glob("recomp_[0-9]*.c")):
    entries=[]
    for m in re.finditer(r'/\* (.*?)  0x([0-9A-Fa-f]+)\.\.0x([0-9A-Fa-f]+) \*/|/\* (.*?) @0x([0-9A-Fa-f]+): HLE override \*/',f.read_text()):
        if m[1]: entries.append({"name":m[1],"address":int(m[2],16),"size":int(m[3],16)-int(m[2],16)})
        else: entries.append({"name":m[4],"address":int(m[5],16),"override":True})
    manifest.append({"unit":f.name,"functions":entries})
(root/"native/function_catalog.json").write_text(json.dumps(manifest,indent=2))
