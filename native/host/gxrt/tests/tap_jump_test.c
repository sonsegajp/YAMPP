#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../controller_gameplay.c"
Context* g_cur_ctx;int g_trace=0;static int online,cpu,entered,hammer;
void trace_enter(uint32_t a,Context*c){(void)a;(void)c;}
void abi_check(uint32_t a,uint32_t before,uint32_t after){(void)a;assert(before==after);}
u8 mem_read8(CPUState*c,u32 a){(void)c;return mem_r8(a);}
u16 mem_read16(CPUState*c,u32 a){(void)c;return mem_r16(a);}
u32 mem_read32(CPUState*c,u32 a){(void)c;return mem_r32(a);}
void mem_write8(CPUState*c,u32 a,u8 v){(void)c;mem_w8(a,v);}
void mem_write16(CPUState*c,u32 a,u16 v){(void)c;mem_w16(a,v);}
void mem_write32(CPUState*c,u32 a,u32 v){(void)c;mem_w32(a,v);}
int netplay_session_active(void){return online;}
void func_800A2040(Context*c){c->gpr[3]=cpu;}
void func_800C5240(Context*c){c->gpr[3]=hammer;}
void func_800C5A50(Context*c){c->gpr[3]=0;}
void func_800CB4E0(Context*c){entered=c->gpr[4];}
void func_800D2D0C(Context*c){c->gpr[3]=0;}
void func_800D72A0(Context*c){c->gpr[3]=0;}
void func_8007D5D4(Context*c){(void)c;}
#define ENTER(addr) void func_##addr(Context*c){(void)c;entered=9;}
ENTER(800CBBC0) ENTER(800CBD18) ENTER(800CBE98) ENTER(800CC0E8) ENTER(800CC238) ENTER(800D2E7C) ENTER(800D74A4)
RecFn lookup_function(uint32_t a){RecFn f=controller_gameplay_lookup(a);assert(f);return f;}
#define FP 0x80020000u
#define GO 0x80010000u
#define COMMON 0x80030000u
static Context ctx;static unsigned char ram[0x1800000];
static void setup(int off,unsigned buttons,float y) {
 memset(&ctx,0,sizeof(ctx));ctx.ram=ram;ctx.ram_size=sizeof(ram);g_cur_ctx=&ctx;
 memset(ram+0x10000,0,0x30000);memset(ram+0x500000,0,0x1000);
 ctx.gpr[1]=0x80500F00;ctx.gpr[13]=0x804DB6A0u;ctx.gpr[3]=GO;
 controller_gameplay_init(&ctx,0x80010100);online=cpu=entered=hammer=0;
 controller_local_input(&ctx,0,off?YAMPP_PAD_TAP_JUMP_OFF:0);
 mem_w32(GO+0x2c,FP);mem_w32(0x804D6554u,COMMON);mem_wf32(COMMON+0x70,.8f);mem_wf32(COMMON+0x80,.7f);
 mem_w32(COMMON+0x74,4);mem_w32(COMMON+0x1c,3);
 mem_wf32(FP+0x624,y);mem_w32(FP+0x668,buttons);mem_w32(FP+0x65c,buttons);
 mem_w32(FP+0x168,6);mem_w8(FP+0x1968,1);mem_w8(FP+0x68a,4);mem_w32(FP+0x2d0,0x80034000);
}
static void unchanged(float y){assert(mem_rf32(COMMON+0x70)==.8f&&mem_rf32(COMMON+0x80)==.7f&&mem_rf32(FP+0x624)==y);assert(ctx.gpr[1]==0x80500F00);}
int main(void){
 unsigned ground[]={0x800CAE80,0x800CAED0,0x800CAF78,0x800CB024};
 for(unsigned k=0;k<4;k++)for(int off=0;off<2;off++)for(int press=0;press<3;press++)for(int iy=-10;iy<=10;iy++){
  unsigned b=press==1?0x400:press==2?0x800:0;float y=iy/10.f;setup(off,b,y);
  controller_gameplay_lookup(ground[k])(&ctx);
  float limit=k==2?.7f:.8f;int expected=(!off&&y>=limit)||b;assert(!!ctx.gpr[3]==!!expected);unchanged(y);
  if(k==0&&b&&(off||y<limit))assert(ctx.gpr[3]==3);
 }
 setup(1,0,1);mem_wf32(FP+0x63c,1);jump_800CB024(&ctx);assert(ctx.gpr[3]==1&&entered==2);unchanged(1);
 setup(1,0,1);cpu=1;jump_800CAED0(&ctx);assert(ctx.gpr[3]==1&&entered==1);
 for(int off=0;off<2;off++)for(int b=0;b<2;b++)for(int jumps=1;jumps<7;jumps++){
  setup(off,b?0x400:0,1);mem_w8(FP+0x1968,jumps);ctx.gpr[3]=FP;jump_800CB804(&ctx);assert(!!ctx.gpr[3]==(jumps<6&&(!off||b)));unchanged(1);
  setup(off,b?0x400:0,1);jump_800CB950(&ctx);assert(!!ctx.gpr[3]==(!off||b));unchanged(1);
  setup(off,b?0x400:0,1);mem_w8(FP+0x1968,jumps);jump_800D730C(&ctx);assert(!!ctx.gpr[3]==(jumps<6&&(!off||b)));unchanged(1);
 }
 /* Metadata is replayed per port, removed before native menu processing,
  * and cannot be overwritten by this host's asynchronous local poll Online. */
 setup(0,0,1);online=1;mem_w32(0x804C1F80u,0x80040000u);mem_w8(0x804C1F79u,0);mem_w8(0x804C1F7bu,1);
 for(unsigned p=0;p<4;p++)mem_w16(0x80040000u+p*12,0x100|((p&1)?YAMPP_PAD_TAP_JUMP_OFF:0));
 controller_online_queue(&ctx);
 for(unsigned p=0;p<4;p++){assert(mem_r8(preference_address+p)==(p&1));assert(mem_r16(0x80040000u+p*12)==0x100);controller_local_input(&ctx,p,0);assert(mem_r8(preference_address+p)==(p&1));}
 puts("Passed: ground/relaxed/C-stick/aerial/multi-jump, CPU preservation, unmodified axes/thresholds, and Online per-port metadata isolation.");return 0;
}
