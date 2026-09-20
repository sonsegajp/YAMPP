/* Static costume artwork supplied by verified mod packages. No bundled pixels,
 * extra game heap, model instances, simulation callbacks, or animation state.
 * Live texture pointers are restored after each original draw. Pixel storage
 * is restored after the FIFO has become an owned renderer packet. */
#include "costume_art.h"
#include "recomp_funcs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ART_CSS_DRAW 0x817FFE00u
#define ART_STOCK_DRAW 0x817FFE10u
#define PORTRAIT_STRIDE 13088u
#define STOCK_STRIDE 320u
#define ART_BYTES (4u*PORTRAIT_STRIDE+6u*STOCK_STRIDE)
extern int costumes_slot_art(int,int,int,const unsigned char**,unsigned*);
extern uint32_t costumes_art_arena(unsigned*);
static unsigned char saved_arena[ART_BYTES];
static uint32_t dirty_arena;
static const unsigned char internal_kind[33]={2,3,1,24,4,5,6,17,0,18,16,8,9,12,10,15,13,14,19,7,22,20,21,26,23,25,27,29,30,31,28,32,10};
static int valid(uint32_t p,unsigned n){return p>=0x80000000u&&(uint64_t)p+n<=0x81800000u;}
static uint32_t be32(const unsigned char* p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static unsigned get(Context* c,RecFn fn,unsigned arg){Context save=*c;c->gpr[3]=arg;fn(c);unsigned value=c->gpr[3];uint64_t time=c->timebase;*c=save;c->timebase=time;return value;}

void costume_art_frame_done(Context* c){
 if(dirty_arena&&valid(dirty_arena,ART_BYTES))memcpy(c->ram+(dirty_arena&RAM_MASK),saved_arena,ART_BYTES);
 dirty_arena=0;
}
void costume_art_reset(Context* c){costume_art_frame_done(c);}

static uint32_t image(Context* c,int kind,int color,int stock,unsigned slot){
 const unsigned char* bytes=NULL;unsigned size=0,available=0;
 if(slot>=(stock?6u:4u)||!costumes_slot_art(kind,color,stock,&bytes,&size)||!bytes||size<24||memcmp(bytes,"YAMPPTEX",8))return 0;
 unsigned width=be32(bytes+8),height=be32(bytes+12),format=be32(bytes+16),length=be32(bytes+20);
 if(width!=(stock?24u:136u)||height!=(stock?24u:188u)||format!=14||length!=(stock?288u:13056u)||size!=24u+length)return 0;
 uint32_t arena=costumes_art_arena(&available);
 if(available<ART_BYTES||!valid(arena,ART_BYTES))return 0;
 if(!dirty_arena){memcpy(saved_arena,c->ram+(arena&RAM_MASK),ART_BYTES);dirty_arena=arena;}
 if(dirty_arena!=arena)return 0;
 uint32_t desc=arena+(stock?4u*PORTRAIT_STRIDE+slot*STOCK_STRIDE:slot*PORTRAIT_STRIDE);
 memset(c->ram+(desc&RAM_MASK),0,32);mem_write32(c,desc,desc+32);
 mem_write16(c,desc+4,(uint16_t)width);mem_write16(c,desc+6,(uint16_t)height);mem_write32(c,desc+8,14);
 memcpy(c->ram+((desc+32)&RAM_MASK),bytes+24,length);
 return desc;
}
typedef struct Swap {uint32_t at,previous;} Swap;
static void textures(Context* c,uint32_t joint,uint32_t desc,Swap* swaps,unsigned* count){
 if(!valid(joint,0x88)||(mem_read32(c,joint+0x14)&0x4020u))return;
 unsigned width=mem_read16(c,desc+4),height=mem_read16(c,desc+6);
 for(uint32_t d=mem_read32(c,joint+0x18),n=0;valid(d,0x18)&&n<64&&*count<64;d=mem_read32(c,d+4),n++){
  uint32_t m=mem_read32(c,d+8);if(!valid(m,0x10))continue;
  for(uint32_t t=mem_read32(c,m+8),k=0;valid(t,0x74)&&k<8&&*count<64;t=mem_read32(c,t+8),k++){
   uint32_t old=mem_read32(c,t+0x58);
   if(!valid(old,24)||mem_read16(c,old+4)!=width||mem_read16(c,old+6)!=height)continue;
   swaps[*count]=(Swap){t+0x58,old};(*count)++;mem_write32(c,t+0x58,desc);
  }
 }
}
static uint32_t nth(Context* c,uint32_t joint,unsigned target,unsigned* index,unsigned depth){
 for(unsigned guard=0;valid(joint,0x88)&&guard<256&&*index<256&&depth<64;guard++,joint=mem_read32(c,joint+8)){
  if((*index)++==target)return joint;
  if(!(mem_read32(c,joint+0x14)&0x1000u)){uint32_t found=nth(c,mem_read32(c,joint+0x10),target,index,depth+1);if(found)return found;}
 }return 0;
}
static void tree_textures(Context* c,uint32_t joint,uint32_t desc,Swap* swaps,unsigned* count,unsigned depth){
 for(unsigned guard=0;valid(joint,0x88)&&guard<192&&depth<64&&*count<64;guard++,joint=mem_read32(c,joint+8)){
  textures(c,joint,desc,swaps,count);
  if(!(mem_read32(c,joint+0x14)&0x1000u))tree_textures(c,mem_read32(c,joint+0x10),desc,swaps,count,depth+1);
 }
}
static void restore(Context* c,Swap* swaps,unsigned count){while(count){count--;mem_write32(c,swaps[count].at,swaps[count].previous);}}
static void css_draw(Context* c){
 Swap swaps[64];unsigned count=0;uint32_t g=c->gpr[3],root=valid(g,0x30)?mem_read32(c,g+0x28):0;
 if(valid(root,0x88))for(unsigned port=0;port<4;port++){
  uint32_t door=0x803F0DFCu+port*0x24u;unsigned icon=mem_read8(c,door+0xE),color=mem_read8(c,door+0xD);
  if(icon>=25||mem_read8(c,door+0xB)==3)continue;
  unsigned external=mem_read8(c,0x803F0B24u+icon*0x1Cu+1);if(external>=33)continue;
  uint32_t desc=image(c,internal_kind[external],color,0,port);if(!desc)continue;
  if(mem_read8(c,0x804D6CF5u)==1){
   if(root==mem_read32(c,0x804D6CC0u)&&port==0){unsigned at=0;textures(c,nth(c,root,0x2B,&at,0),desc,swaps,&count);}
   else if(root==mem_read32(c,0x804D6CC4u)&&port==1){unsigned at=0;textures(c,nth(c,root,4,&at,0),desc,swaps,&count);}
  }else{unsigned at=0;textures(c,nth(c,root,mem_read8(c,door+1),&at,0),desc,swaps,&count);}
 }
 func_80391070(c);restore(c,swaps,count);
}
static void stock_draw(Context* c){
 Swap swaps[64];unsigned count=0;uint32_t g=c->gpr[3],data=valid(g,0x30)?mem_read32(c,g+0x2C):0;
 if(valid(data,2)){
  unsigned port=mem_read8(c,data);
  if(port<6){unsigned external=get(c,func_80032330,port),color=get(c,func_80033198,port);
   if(external<33){uint32_t desc=image(c,internal_kind[external],color,1,port);if(desc)tree_textures(c,mem_read32(c,g+0x28),desc,swaps,&count,0);}
  }
 }
 func_802F94E0(c);restore(c,swaps,count);
}
void costume_art_gx_link(Context* c){
 if(c->gpr[4]==0x802F94E0u)c->gpr[4]=ART_STOCK_DRAW;
 else if(c->gpr[4]==0x80391070u&&c->gpr[5]==1&&c->lr>=0x802640A0u&&c->lr<0x8026688Cu){
  uint32_t g=c->gpr[3],root=valid(g,0x30)?mem_read32(c,g+0x28):0;
  if(root&&(root==mem_read32(c,0x804D6CC0u)||root==mem_read32(c,0x804D6CC4u)))c->gpr[4]=ART_CSS_DRAW;
 }
}
RecFn costume_art_lookup(uint32_t address){if(address==ART_CSS_DRAW)return css_draw;if(address==ART_STOCK_DRAW)return stock_draw;return NULL;}
