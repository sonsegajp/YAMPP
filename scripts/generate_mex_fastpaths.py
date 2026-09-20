"""Generate guarded native variants for m-ex texture and joint-matrix patches.

The original instructions come only from the user's verified DOL. Generated
code and expected bytes stay in build/, never in source or mod packages.
Any additional patch makes the runtime fall back to dynamic PPC execution.
"""
import copy
import os
from pathlib import Path
import shutil
import struct
import subprocess


def generate(emitter, dol, functions, directory):
    variants = {
        0x80360950: [(0xE8, 0x88160070, 0x80160070),
                     (0xEC, 0x280000FF, 0x2C00FFFF)],
        0x8035E860: [(0xB8, 0x981F0070, 0x901F0070)],
    }
    compiler = copy.copy(emitter)
    compiler.dynamic_entry_guard = False
    chunks = ['#include "recomp_funcs.h"\n#include <string.h>\n#include <math.h>\n']
    entries = []
    for address, patches in variants.items():
        function = next(f for f in functions if f['address'] == address)
        patched = copy.copy(dol)
        patched.data = bytearray(dol.data)
        for offset, expected, replacement in patches:
            assert dol.word(address + offset) == expected
            section = dol.section_at(address + offset)
            position = section.file_off + address + offset - section.addr
            patched.data[position:position + 4] = replacement.to_bytes(4, 'big')
        name = 'mex_native_%08X' % address
        chunks.append(compiler.emit_function(patched, address, function['size'],
                                             function['name'], c_name=name))
        data = patched.read(address, function['size'])
        chunks.append('static const unsigned char expected_%08X[] = {%s};' %
                      (address, ','.join('0x%02X' % byte for byte in data)))
        entries.append('case 0x%08Xu: return memcmp(ctx->ram+0x%Xu,expected_%08X,'
                       'sizeof expected_%08X)==0 ? %s : NULL;' %
                       (address, address & 0x1FFFFFF, address, address, name))
    matrix = generate_matrix(compiler, dol, functions, directory)
    if matrix:
        chunks.extend(matrix[0]);entries.append(matrix[1])
    chunks.append('RecFn mex_native_lookup(Context* ctx,uint32_t address) {\n'
                  'switch(address){\n' + '\n'.join(entries) +
                  '\ndefault:return NULL;}\n}\n')
    target = directory / 'mex_native_generated.c'
    content = '\n\n'.join(chunks)
    if not target.exists() or target.read_text() != content:
        target.write_bytes(content.encode())
        print('regenerated', target.name)


def generate_matrix(compiler, dol, functions, directory):
    # Ploaj's complete scale-compensation patch, assembled from pinned m-ex
    # source. No saved game RAM or third-party costume/stage payload is used.
    root=Path(__file__).resolve().parents[1]
    assembler=shutil.which('powerpc-eabi-as')
    if not assembler and os.name=='nt':
        candidate=Path('C:/devkitPro/devkitPPC/bin/powerpc-eabi-as.exe')
        if candidate.is_file():assembler=str(candidate)
    if not assembler:return None
    folder=root/'upstream/m-ex/asm/m-ex/HSD/JObj/ScaleCompensate'
    if not (folder/'MakeMTX.asm').is_file():return None
    work=root/'build/mex-fast-matrix';work.mkdir(parents=True,exist_ok=True)
    objcopy=Path(assembler).with_name('powerpc-eabi-objcopy'+('.exe' if os.name=='nt' else ''))
    subprocess.run([assembler,'-mgekko','-mregnames','MakeMTX.asm','-o',str(work/'hook.o')],cwd=folder,check=True,capture_output=True)
    subprocess.run([str(objcopy),'-O','binary','-j','.text',str(work/'hook.o'),str(work/'hook.bin')],check=True)
    hook=(work/'hook.bin').read_bytes()
    address=0x8036F1F8;extra=0x817F0000;size=0x2D0
    patched=copy.copy(dol);patched.data=bytearray(dol.data)
    changes=[(8,0x9421FFE0,0x9421FFA0),(0x204,0x387F0044,0x48000000|((extra-(address+0x204))&0x3FFFFFC)),
             (0x2B8,0x80010024,0x80010064),(0x2C4,0x38210020,0x38210060)]
    for offset,expected,replacement in changes:
        assert dol.word(address+offset)==expected
        section=dol.section_at(address+offset);position=section.file_off+address+offset-section.addr
        patched.data[position:position+4]=replacement.to_bytes(4,'big')
    # The upstream loader aligns this block with a NOP, then appends its return.
    tail=0x48000000|((address+0x208-(extra+len(hook)+4))&0x3FFFFFC)
    extra_bytes=hook+struct.pack('>II',0x60000000,tail)
    class Overlay:
        def word(self,a):
            return struct.unpack_from('>I',extra_bytes,a-extra)[0] if extra<=a<extra+len(extra_bytes) else patched.word(a)
    name='mex_native_8036F1F8'
    compiler=copy.copy(compiler)
    compiler.function_preamble=['uint32_t mex_hook=0x8036F3FCu+(uint32_t)((int32_t)(mem_r32(0x8036F3FCu)<<6)>>6);']
    compiler.code_address_mapper=lambda a: '(mex_hook+0x%Xu)'%(a-extra) if extra<=a<extra+len(extra_bytes) else '0x%08Xu'%a
    source=compiler.emit_function(Overlay(),address,size,'HSD_JObjMakeMatrix_m_ex',c_name=name,extra_regions=[(extra,len(extra_bytes))])
    expected=patched.read(address,size)
    arrays=['static const unsigned char expected_8036F1F8[]={%s};'%(','.join('0x%02X'%b for b in expected)),
            'static const unsigned char matrix_hook[]={%s};'%(','.join('0x%02X'%b for b in hook))]
    check="""case 0x8036F1F8u: {
      uint32_t branch=mem_r32(0x8036F3FCu);
      if((branch&0xFC000003u)!=0x48000000u)return NULL;
      uint32_t target=0x8036F3FCu+(uint32_t)((int32_t)(branch<<6)>>6);
      uint32_t physical=target&RAM_MASK;
      if(physical+sizeof matrix_hook+8>ctx->ram_size)return NULL;
      if(memcmp(ctx->ram+0x36F1F8,expected_8036F1F8,0x204)
          || memcmp(ctx->ram+0x36F400,expected_8036F1F8+0x208,sizeof expected_8036F1F8-0x208)
          || memcmp(ctx->ram+physical,matrix_hook,sizeof matrix_hook)
          || mem_r32(target+sizeof matrix_hook)!=0x60000000u)return NULL;
      uint32_t exit=mem_r32(target+sizeof matrix_hook+4);
      if((exit&0xFC000003u)!=0x48000000u
          || (uint32_t)(target+sizeof matrix_hook+4+(uint32_t)((int32_t)(exit<<6)>>6))!=0x8036F400u)return NULL;
      return mex_native_8036F1F8;
    }"""
    return [source,*arrays],check
