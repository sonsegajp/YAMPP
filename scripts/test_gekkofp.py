"""Execute emitted Gekko scalar/paired instruction sequences against known register semantics.
Expected lane behavior follows Dolphin Interpreter_LoadStore.cpp and Interpreter_FloatingPoint.cpp.
"""
from pathlib import Path
import os, subprocess, sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"native/recompiler"))
import emit,ppc
emit.set_abi("recompcore")
em=emit.Emitter(set())
out=ROOT/"build/tests/gekkofp"
out.mkdir(parents=True,exist_ok=True)
parts=['''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { uint32_t gpr[32]; double fpr[32],ps1[32]; } Context;
static double mem_rf32(uint32_t a) { assert(a==12); return 3.25; }
static double mem_rf64(uint32_t a) { assert(a==12); return 3.25; }
int main(void) { Context state={0}, *ctx=&state;
''']
def op(word):
 ins=ppc.decode(word)
 assert not ins.illegal
 parts.extend(em.emit_instr(ins,0,0,4))
def lanes(n,value):
 parts.append(f"assert(ctx->fpr[{n}]=={value} && ctx->ps1[{n}]=={value});")
# Both direct and indexed loads, including update forms, must overwrite stale lane 1.
for opcode,xo,update in [(48,None,False),(49,None,True),(31,535,False),(31,567,True)]:
 parts.append("ctx->gpr[2]=8;ctx->gpr[3]=4;ctx->ps1[1]=-99;")
 word=(opcode<<26)|(1<<21)|(2<<16)|(4 if xo is None else ((3<<11)|(xo<<1)))
 op(word);lanes(1,3.25)
 parts.append(f"assert(ctx->gpr[2]=={12 if update else 8});")
# Double loads preserve the upper paired lane.
parts.append("ctx->gpr[2]=8;ctx->ps1[1]=-99;")
op((50<<26)|(1<<21)|(2<<16)|4)
parts.append("assert(ctx->fpr[1]==3.25 && ctx->ps1[1]==-99);")
# Single arithmetic must broadcast even when destination aliases an operand.
for xo,expected in [(21,5.0),(20,-1.0),(25,8.0),(18,2.0/3.0),(29,11.0),(28,5.0)]:
 parts.append("ctx->fpr[1]=2;ctx->fpr[2]=3;ctx->fpr[3]=4;ctx->ps1[1]=-99;")
 op((59<<26)|(1<<21)|(1<<16)|(2<<11)|(3<<6)|(xo<<1))
 lanes(1,f"(double)(float)({expected})")
# frsp also broadcasts; fmr (double move) must preserve destination lane 1.
parts.append("ctx->fpr[2]=1.25;ctx->ps1[1]=-99;")
op((63<<26)|(1<<21)|(2<<11)|(12<<1));lanes(1,1.25)
parts.append("ctx->ps1[1]=-99;")
op((63<<26)|(1<<21)|(2<<11)|(72<<1))
parts.append("assert(ctx->fpr[1]==1.25 && ctx->ps1[1]==-99);")
# A geometry-style load followed by paired multiply consumes the replicated second lane.
parts.append("ctx->gpr[2]=8;ctx->fpr[4]=2;ctx->ps1[4]=4;")
op((48<<26)|(1<<21)|(2<<16)|4)
op((4<<26)|(5<<21)|(4<<16)|(1<<6)|(13<<1))
parts.append('assert(ctx->fpr[5]==6.5 && ctx->ps1[5]==13.0); puts("Gekko scalar/paired register regression passed"); return 0;}')
(out/"test.c").write_text("\n".join(parts))
cc=Path("C:/msys64/mingw64/bin/gcc.exe")
env=os.environ.copy();env["PATH"]=str(cc.parent)+os.pathsep+env["PATH"]
subprocess.run([str(cc),"-O2",str(out/"test.c"),"-o",str(out/"test.exe")],check=True,env=env)
subprocess.run([str(out/"test.exe")],check=True,env=env)
