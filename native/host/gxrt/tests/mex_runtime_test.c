#include "abi_recompcore.h"
#include "gxruntime/loader.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
Context* g_cur_ctx;int g_trace=1;uint32_t g_recomp_poll_budget=4096;
void recomp_poll(Context* c){(void)c;g_recomp_poll_budget=4096;}
uint32_t hle_timebase(int upper){return upper?0:1234;}
void ru_psq_load(Context* c,int d,uint32_t ea,int w,int gqr){ppc_psq_load(c,d,ea,w,gqr,0,c->pc);}
void ru_psq_store(Context* c,int d,uint32_t ea,int w,int gqr){ppc_psq_store(c,d,ea,w,gqr,0,c->pc);}
extern void mex_runtime_shutdown(void);
static int wrapper_enabled,wrapper_calls,trace_calls;
void trace_enter(uint32_t address,Context* c){assert(address==0x800163D8);(void)c;++trace_calls;}
static void native_wrapper(Context* c){++wrapper_calls;assert(mex_runtime_entry(c,0x800163D8,8));c->gpr[3]+=100;}
RecFn hooks_lookup(uint32_t a){return wrapper_enabled&&a==0x800163D8?native_wrapper:NULL;}
RecFn costumes_lookup(uint32_t a){(void)a;return NULL;}
RecFn mex_native_lookup(Context* ctx,uint32_t address){(void)ctx;(void)address;return NULL;}
static int native_calls;
static void native_add(Context* c){c->gpr[3]+=5;++native_calls;}
RecFn lookup_function(uint32_t a){assert(a==0x800163D8);RecFn h=hooks_lookup(a);return h?h:native_add;}
int main(int argc,char** argv){
 assert(argc==2);Context c;DolLayout layout;assert(cpu_init(&c));g_cur_ctx=&c;
 assert(dol_load_into_ram(&c,argv[1],&layout));assert(mex_runtime_init(&c));assert(g_mex_active);
 c.gpr[1]=0x81700000;c.lr=0x80002000;c.msr=0x2000;c.hid2=PPC_HID2_LSQE|PPC_HID2_PSE;
 /* Relocated code saves LR, calls an unchanged recompiled function, restores
  * the caller frame and returns. This is the boundary used by m-ex callbacks. */
 uint32_t code[]={0x9421FFF0,0x7C0802A6,0x90010014,0x38600007,0,0x80010014,0x7C0803A6,0x38210010,0x4E800020};
 code[4]=0x48000001|((0x800163D8u-0x81000010u)&0x03FFFFFC);
 for(unsigned i=0;i<sizeof(code)/sizeof(*code);i++)mem_w32(0x81000000+4*i,code[i]);
 RecFn f=mex_runtime_lookup(0x81000000);assert(f);f(&c);
 assert(c.gpr[3]==12&&native_calls==1&&c.gpr[1]==0x81700000&&c.lr==0x80002000);
 /* Patching a stock entry must execute the replacement, never stale AOT. */
 unsigned char saved[8];memcpy(saved,c.ram+0x163D8,8);mem_w32(0x800163D8,0x38600009);mem_w32(0x800163DC,0x4E800020);
 f=mex_runtime_lookup(0x800163D8);assert(f);f(&c);assert(c.gpr[3]==9&&native_calls==1&&trace_calls==1);
 /* A native wrapper around patched code must run once, then resume the
  * caller. Repatching an executed block must invalidate Unicorn's cache. */
 wrapper_enabled=1;f=mex_runtime_lookup(0x81000000);assert(f);f(&c);
 assert(c.gpr[3]==109&&wrapper_calls==1&&c.gpr[1]==0x81700000);
 mem_w32(0x800163D8,0x3860000B);f=mex_runtime_lookup(0x81000000);assert(f);f(&c);
 assert(c.gpr[3]==111&&wrapper_calls==2&&trace_calls==3&&c.gpr[1]==0x81700000);
 wrapper_enabled=0;f=mex_runtime_lookup(0x81000000);f(&c);assert(c.gpr[3]==11&&trace_calls==4);
 wrapper_enabled=0;
 memcpy(c.ram+0x163D8,saved,8);assert(mex_runtime_lookup(0x800163D8)==NULL);
 /* Scalar lfs duplicates the single into both Gekko lanes. */
 mem_wf32(0x81200000,3.5f);c.gpr[4]=0x81200000;c.ps1[1]=-42;
 mem_w32(0x81100000,0xC0240000);mem_w32(0x81100004,0x4E800020);
 f=mex_runtime_lookup(0x81100000);assert(f);f(&c);assert(c.fpr[1]==3.5&&c.ps1[1]==3.5);
 /* Register transfers preserve updated operands and the second Gekko lane
  * across a sequence of scalar instructions. */
 mem_w32(0x81102000,0xC0240000); /* lfs f1,0(r4) */
 mem_w32(0x81102004,(59u<<26)|(2u<<21)|(1u<<16)|(1u<<11)|(21u<<1));
 mem_w32(0x81102008,(59u<<26)|(3u<<21)|(2u<<16)|(1u<<6)|(25u<<1));
 mem_w32(0x8110200C,0x4E800020);
 f=mex_runtime_lookup(0x81102000);f(&c);
 assert(c.fpr[2]==7.0&&c.ps1[2]==7.0&&c.fpr[3]==24.5&&c.ps1[3]==24.5);
 /* GQR writes must reach the paired-single helpers, not the generic SPR bank. */
 c.gpr[5]=0x00040004;
 mem_w32(0x81101000,(31u<<26)|(5u<<21)|(17u<<16)|(28u<<11)|(467u<<1));
 mem_w32(0x81101004,0x4E800020);
 f=mex_runtime_lookup(0x81101000);assert(f);f(&c);assert(c.gqr[1]==0x00040004);
 /* Adjacent host-stepped instructions must stop at a native entry and at
  * the caller return address, even when the next word is another FP op. */
 const uint32_t fadd=(59u<<26)|(2u<<21)|(1u<<16)|(1u<<11)|(21u<<1);
 mem_w32(0x800163D4,fadd);c.gpr[3]=20;c.lr=0x80002000;
 int before=native_calls;f=mex_runtime_lookup(0x800163D4);f(&c);
 assert(c.gpr[3]==25&&native_calls==before+1&&c.fpr[2]==7.0);
 mem_w32(0x81103000,fadd);mem_w32(0x81103004,0xC0240004);
 c.lr=0x81103004;c.fpr[1]=3.5;c.ps1[1]=3.5;
 f=mex_runtime_lookup(0x81103000);f(&c);
 assert(c.pc==0x81103004&&c.fpr[1]==3.5&&c.ps1[1]==3.5);
 /* Long runs cross the batch bound and alternate host FP with engine integer
  * instructions. Repatching a previously executed FP word must take effect. */
 uint32_t start=0x81104000;unsigned n=0;
 for(unsigned i=0;i<96;i++){
   mem_w32(start+4*n++,(59u<<26)|(2u<<21)|(2u<<16)|(1u<<11)|(21u<<1));
   if(i%11==10)mem_w32(start+4*n++,0x38A50001); /* addi r5,r5,1 */
 }
 mem_w32(start+4*n,0x4E800020);c.lr=0x80002000;c.fpr[1]=0.25;c.ps1[1]=0.25;
 c.fpr[2]=c.ps1[2]=0;c.gpr[5]=0;f=mex_runtime_lookup(start);f(&c);
 assert(c.fpr[2]==24.0&&c.ps1[2]==24.0&&c.gpr[5]==8);
 mem_w32(start+4,(59u<<26)|(2u<<21)|(2u<<16)|(1u<<11)|(20u<<1));
 c.fpr[2]=c.ps1[2]=0;c.gpr[5]=0;f=mex_runtime_lookup(start);f(&c);
 assert(c.fpr[2]==23.5&&c.ps1[2]==23.5&&c.gpr[5]==8);
 clock_t began=clock();
 for(unsigned i=0;i<2000;i++){c.fpr[2]=c.ps1[2]=0;f=mex_runtime_lookup(start);f(&c);}
 assert(c.fpr[2]==23.5&&c.ps1[2]==23.5);
 printf("m-ex scalar benchmark: %.3f seconds\n",(double)(clock()-began)/CLOCKS_PER_SEC);
 /* Compare guest state between separate batched and single-step processes. */
 unsigned hash=2166136261u;
 #define HASH_BYTES(ptr,len) do{const unsigned char* bytes=(const unsigned char*)(ptr);for(size_t j=0;j<(len);j++)hash=(hash^bytes[j])*16777619u;}while(0)
 HASH_BYTES(c.ram,c.ram_size);HASH_BYTES(c.gpr,sizeof c.gpr);HASH_BYTES(c.fpr,sizeof c.fpr);
 HASH_BYTES(c.ps1,sizeof c.ps1);HASH_BYTES(c.gqr,sizeof c.gqr);HASH_BYTES(c.spr,sizeof c.spr);
 uint32_t control[]={c.pc,c.lr,c.ctr,c.cr,c.xer,c.fpscr,c.msr};HASH_BYTES(control,sizeof control);
 printf("m-ex state: %08x\n",hash);
 printf("m-ex runtime: native handoff, nested wrappers, repatching, Gekko lanes, batch limits and return boundaries passed\n");
 mex_runtime_shutdown();cpu_free(&c);return 0;
}
