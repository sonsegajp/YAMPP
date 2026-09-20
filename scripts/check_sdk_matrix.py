"""Compare the native SDK matrix path to the original DOL instruction sequence."""
import json,subprocess,sys,os
from pathlib import Path
os.environ["PATH"]="C:/msys64/mingw64/bin;"+os.environ["PATH"]
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'native/recompiler'));import emit;from dol import Dol
out=ROOT/'build/sdk-math-check';out.mkdir(parents=True,exist_ok=True)
catalog=json.loads((ROOT/'native/function_catalog.json').read_text());f=next(f for u in catalog for f in u['functions'] if f['address']==0x80342204)
emit.set_abi('recompcore');em=emit.Emitter({f['address'] for u in catalog for f in u['functions']},())
reference=em.emit_function(Dol(str(ROOT/'data/GALE01/sys/main.dol')),f['address'],f['size'],f['name']).replace('void func_80342204(', 'void reference_concat(',1)
rt=(ROOT/'native/runtime/module_rt.c').read_text();quant=rt[rt.index('static const int kTypeSize'):rt.index('/* ---- hooks')]
source = '#include "recomp_funcs.h"\n#include <stdlib.h>\n#include <stdio.h>\n#include <math.h>\nContext* g_cur_ctx;\nvoid trace_enter(uint32_t a,Context* c){}\n'+quant+'\n'+reference+'\n'+(ROOT/'native/host/gxrt/hle_sdk_math.c').read_text()+r'''
int main(void){
 Context ctx={0};g_cur_ctx=&ctx;ctx.ram_size=24*1024*1024;ctx.ram=calloc(1,ctx.ram_size);
 mem_wf32(0x804D5C00,0);mem_wf32(0x804D5C04,1);
 unsigned seed=123456789;unsigned char input[96],want[48];
 for(unsigned n=0;n<10000;n++){
  for(unsigned j=0;j<24;j++){seed=1664525*seed+1013904223;mem_wf32(0x80001000+j*4,(int32_t)seed/1234567.0f);}
  memcpy(input,ctx.ram+0x1000,96);ctx.gpr[1]=0x81700000;ctx.gpr[3]=0x80001000;ctx.gpr[4]=0x80001030;ctx.gpr[5]=n%3==0?0x80001000:n%3==1?0x80001030:0x80002000;
  reference_concat(&ctx);memcpy(want,RAMP(ctx.gpr[5]),48);memcpy(ctx.ram+0x1000,input,96);func_80342204(&ctx);
  if(memcmp(want,RAMP(ctx.gpr[5]),48)){fprintf(stderr,"Mismatch matrix %u alias mode %u\n",n,n%3);return 1;}
 }
 puts("10000 matrices: bit-exact including in-place A/B concatenation");free(ctx.ram);return 0;
}
'''
(out/'check.c').write_text(source)
cmd=['C:/msys64/mingw64/bin/gcc.exe','-O2','-fno-strict-aliasing',*[ '-I'+str(ROOT/p) for p in ['native/runtime','native/vendor/include','build/native-game/generated']],str(out/'check.c'),'-lm','-o',str(out/'check.exe')]
subprocess.run(cmd,check=True);subprocess.run([str(out/'check.exe')],check=True)
