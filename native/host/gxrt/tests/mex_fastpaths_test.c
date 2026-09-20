#include "abi_recompcore.h"
#include "gxruntime/loader.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
Context* g_cur_ctx;int g_trace=0;uint32_t g_recomp_poll_budget=4096;
void recomp_poll(Context* c){(void)c;g_recomp_poll_budget=4096;}
void trace_enter(uint32_t a,Context* c){(void)a;(void)c;}
void abi_check(uint32_t a,uint32_t before,uint32_t after){(void)a;assert(before==after);}
uint32_t hle_timebase(int upper){return upper?0:1234;}
void ru_psq_load(Context* c,int d,uint32_t ea,int w,int q){ppc_psq_load(c,d,ea,w,q,0,c->pc);}
void ru_psq_store(Context* c,int d,uint32_t ea,int w,int q){ppc_psq_store(c,d,ea,w,q,0,c->pc);}
#define mex_native_lookup compiled_matcher
#include "mex_native_generated.c"
#undef mex_native_lookup
static int dynamic_only;
RecFn mex_native_lookup(Context* c,uint32_t address){return dynamic_only?NULL:compiled_matcher(c,address);}
#define STUB(addr) void func_##addr(Context* c){(void)c;}
STUB(8033E9B0) STUB(8033EC24) STUB(8033EC6C) STUB(8033EFD0)
STUB(8033F024) STUB(8033F06C) STUB(8035F0EC) STUB(80362478)
STUB(80342204) STUB(80342AA8) STUB(80373078) STUB(8037A250)
STUB(8037A43C) STUB(8037A65C)
void func_8037A610(Context* c){c->gpr[3]=0x81200800;}
void func_803421A4(Context* c){for(unsigned i=0;i<12;i++)mem_wf32(c->gpr[3]+i*4,i%5==0?1.0f:0.0f);}
void func_803606F0(Context* c){c->gpr[3]=1;}
void func_80360DE4(Context* c){c->gpr[3]=0;}
void func_80388220(Context* c){(void)c;assert(!"Unexpected guest assertion");}
RecFn hooks_lookup(uint32_t a){switch(a){
#define TARGET(addr) case 0x##addr: return func_##addr;
TARGET(8033E9B0) TARGET(8033EC24) TARGET(8033EC6C) TARGET(8033EFD0)
TARGET(8033F024) TARGET(8033F06C) TARGET(8035F0EC) TARGET(80362478)
TARGET(803606F0) TARGET(80360DE4) TARGET(80388220)
TARGET(80342204) TARGET(80342AA8) TARGET(80373078) TARGET(8037A250)
TARGET(8037A43C) TARGET(8037A65C) TARGET(8037A610) TARGET(803421A4)
default:return NULL;}}
RecFn costumes_lookup(uint32_t a){(void)a;return NULL;}
RecFn lookup_function(uint32_t a){RecFn f=hooks_lookup(a);if(!f)f=mex_runtime_lookup(a);assert(f);return f;}
int main(int argc,char** argv){
 assert(argc==2);Context c;DolLayout layout;assert(cpu_init(&c));g_cur_ctx=&c;
 assert(dol_load_into_ram(&c,argv[1],&layout));assert(mex_runtime_init(&c));
 memcpy(c.ram+0x35E860,expected_8035E860,sizeof expected_8035E860);
 memcpy(c.ram+0x360950,expected_80360950,sizeof expected_80360950);
 memcpy(c.ram+0x36F1F8,expected_8036F1F8,sizeof expected_8036F1F8);
 memcpy(c.ram+0x1400000,matrix_hook,sizeof matrix_hook);
 mem_w32(0x8036F3FC,0x48000000u|((0x81400000u-0x8036F3FCu)&0x03FFFFFCu));
 mem_w32(0x81400000u+sizeof matrix_hook,0x60000000);
 mem_w32(0x81400000u+sizeof matrix_hook+4,0x48000000u|((0x8036F400u-(0x81400000u+sizeof matrix_hook+4))&0x03FFFFFCu));
 assert(compiled_matcher(&c,0x8036F1F8));
 c.ram[0x1400010]^=1;assert(!compiled_matcher(&c,0x8036F1F8));c.ram[0x1400010]^=1;
 assert(compiled_matcher(&c,0x8035E860));
 c.ram[0x35E864]^=1;assert(!compiled_matcher(&c,0x8035E860));c.ram[0x35E864]^=1;
 unsigned char* initial=malloc(c.ram_size);unsigned char* expected=malloc(c.ram_size);assert(initial&&expected);
 for(unsigned kind=0;kind<3;kind++)for(unsigned n=0;n<48;n++){
  memset(c.ram+0x1200000,0,0x20000);memset(c.ram+0x16FFF00,0,0x200);
  memset(c.gpr,0,sizeof c.gpr);memset(c.fpr,0,sizeof c.fpr);memset(c.ps1,0,sizeof c.ps1);
  c.gpr[1]=0x81700000;c.gpr[2]=0x81308000;c.gpr[13]=0x81300000;c.lr=0x80002000;c.msr=0x2000;
  c.hid2=PPC_HID2_LSQE|PPC_HID2_PSE;
  c.gpr[3]=0x81200000;c.gpr[4]=n;c.gpr[5]=0x81200100;
  mem_wf32(0x81200100,300.25f);mem_wf64(c.gpr[2]-5632,255.0);
  mem_w32(0x81200068,0x81201000);mem_w32(0x8120006C,0x81201000);
  mem_w32(0x81201000+4*300,0x81203000);
  mem_w32(0x81200060,0x81200400);mem_w32(0x812000A8,0x81200500);
  if(kind==1){
   c.gpr[4]=0;mem_w32(0x81200008,0);mem_w32(0x81200058,0x81204000);
   mem_w32(0x81204000,0x81205000);mem_w16(0x81204004,8);mem_w16(0x81204006,8);mem_w32(0x81204008,9);
   mem_w32(0x81200070,n&1?300:0xFFFFFFFFu);mem_w32(0x8120005C,0x81203000);
   mem_w32(0x81201000+4*300,0x81203000);mem_w32(0x81203000,0x81206000);mem_w16(0x8120300C,256);
  }
  if(kind==2){
   memset(c.ram+0x1200000,0,0x10000);c.gpr[3]=0x81200000;
   mem_w32(0x8120000C,n&1?0x81200200:0);
   mem_w32(0x81200014,(n&2?0x04000000:0)|(n&4?8:0)|(n&8?0x20000:0));
   mem_w32(0x81200078,0x81200800);mem_w32(0x81200278,0x81200900);
   for(unsigned i=0;i<3;i++){
    mem_wf32(0x8120002C+i*4,1.0f+i+n*.1f);mem_wf32(0x8120022C+i*4,1.5f+i);
    mem_wf32(0x81200038+i*4,3.0f+i);mem_wf32(0x81200900+i*4,1.5f+i);
   }
   mem_wf32(c.gpr[2]-0x16D0,1.0f);
  }
  Context saved=c;memcpy(initial,c.ram,c.ram_size);uint32_t address=kind==2?0x8036F1F8:kind?0x80360950:0x8035E860;
  dynamic_only=1;RecFn fn=mex_runtime_lookup(address);assert(fn);fn(&c);memcpy(expected,c.ram,c.ram_size);
  c=saved;memcpy(c.ram,initial,c.ram_size);dynamic_only=0;fn=mex_runtime_lookup(address);assert(fn);fn(&c);
  if(memcmp(expected,c.ram,c.ram_size)){
   for(unsigned i=0;i<c.ram_size;i++)if(expected[i]!=c.ram[i]){fprintf(stderr,"variant %u case %u RAM %X dynamic %02X native %02X\n",kind,n,i,expected[i],c.ram[i]);break;}
   return 1;
  }
 }
 puts("m-ex texture fast paths: 144 native/dynamic RAM comparisons, including scale compensation; extra-patch fallback passed");return 0;
}
