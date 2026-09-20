/* Optional tap jump. All effective preferences live in snapshotted guest RAM;
 * no gameplay path reads renderer settings during rollback. */
#include "abi_recompcore.h"
#include "recomp_funcs.h"
#include "../controller_input.h"
static uint32_t preference_address;
extern int netplay_session_active(void);
void controller_gameplay_init(Context* c,uint32_t address) {
 preference_address=address;mem_write32(c,address,0);
}
void controller_local_input(Context* c,unsigned port,unsigned buttons) {
 if(preference_address&&port<4&&!netplay_session_active())
  mem_write8(c,preference_address+port,!!(buttons&YAMPP_PAD_TAP_JUMP_OFF));
}
/* The Online input queue has already been filled with confirmed/predicted
 * inputs when this runs, including on every replayed frame. */
void controller_online_queue(Context* c) {
 const uint32_t padlib=0x804C1F78u;
 if(!preference_address||!netplay_session_active()||!mem_read8(c,padlib+3))return;
 uint32_t entry=mem_read32(c,padlib+8)+mem_read8(c,padlib+1)*48;
 if((entry&0x3fffffff)>c->ram_size-48)return;
 for(unsigned p=0;p<4;p++){
  uint32_t at=entry+p*12;unsigned buttons=mem_read16(c,at);
  mem_write8(c,preference_address+p,!!(buttons&YAMPP_PAD_TAP_JUMP_OFF));
  mem_write16(c,at,buttons&~YAMPP_PAD_TAP_JUMP_OFF);
 }
}
static int disabled(Context* c,uint32_t fp) {
 if(!preference_address||(fp&0x3fffffff)>c->ram_size-0x2400)return 0;
 unsigned port=mem_read8(c,fp+0x618);
 if(port>=4||!mem_read8(c,preference_address+port))return 0;
 /* Original predicate also handles Nana and temporarily human-driven CPUs. */
 Context saved=*c;c->gpr[3]=fp;func_800A2040(c);int cpu=c->gpr[3]!=0;*c=saved;
 return !cpu;
}
static void jump_check(Context* c,RecFn original,int gobj) {
 uint32_t fp=gobj?mem_read32(c,c->gpr[3]+0x2c):c->gpr[3];
 if(!disabled(c,fp)){original(c);return;}
 uint32_t common=mem_read32(c,0x804D6554u);
 uint32_t threshold=mem_read32(c,common+0x70),relaxed=mem_read32(c,common+0x80);
 /* Only jump predicates see these thresholds. Original stick values remain
  * available to jump velocity, upward attacks, specials, float and wall input. */
 mem_write32(c,common+0x70,0x40000000u);mem_write32(c,common+0x80,0x40000000u);
 original(c);
 mem_write32(c,common+0x70,threshold);mem_write32(c,common+0x80,relaxed);
}
#define JUMP_HOOK(addr,gobj) static void jump_##addr(Context* c){jump_check(c,func_##addr,gobj);}
JUMP_HOOK(800CAE80,1)
JUMP_HOOK(800CAED0,1)
JUMP_HOOK(800CAF78,1)
JUMP_HOOK(800CB804,0)
JUMP_HOOK(800CB950,1)
JUMP_HOOK(800D730C,1)
static void jump_800CB024(Context* c) {
 uint32_t go=c->gpr[3],fp=mem_read32(c,go+0x2c);
 if(!disabled(c,fp)){func_800CB024(c);return;}
 /* This entry inlines the ground predicate followed by a C-stick predicate.
  * Restore the original threshold before checking C-stick jumping. */
 jump_800CAED0(c);if(c->gpr[3])return;
 c->gpr[3]=fp;func_800DF910(c);
 if(c->gpr[3]){c->gpr[3]=go;c->gpr[4]=2;func_800CB4E0(c);c->gpr[3]=1;}
}
RecFn controller_gameplay_lookup(uint32_t address) {
 switch(address){
#define CASE(addr) case 0x##addr##u:return jump_##addr;
 CASE(800CAE80) CASE(800CAED0) CASE(800CAF78) CASE(800CB024)
 CASE(800CB804) CASE(800CB950) CASE(800D730C)
 default:return 0;
 }
}
