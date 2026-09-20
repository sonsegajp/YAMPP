/* Synthetic art proves render-only binding leaves all guest RAM unchanged. */
#include <assert.h>
#include "../costume_art.c"
Context *g_cur_ctx;
static unsigned char texture[24+13056],stock_texture[24+288];
static unsigned draws;
static uint32_t observed_tobj;
u8 mem_read8(CPUState* c,u32 a){assert(valid(a,1));return c->ram[a&RAM_MASK];}
u16 mem_read16(CPUState* c,u32 a){return (u16)((mem_read8(c,a)<<8)|mem_read8(c,a+1));}
u32 mem_read32(CPUState* c,u32 a){assert(valid(a,4));return be32(c->ram+(a&RAM_MASK));}
void mem_write8(CPUState* c,u32 a,u8 v){assert(valid(a,1));c->ram[a&RAM_MASK]=v;}
void mem_write16(CPUState* c,u32 a,u16 v){mem_write8(c,a,v>>8);mem_write8(c,a+1,v);}
void mem_write32(CPUState* c,u32 a,u32 v){assert(valid(a,4));unsigned char* p=c->ram+(a&RAM_MASK);p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
uint32_t costumes_art_arena(unsigned* size){*size=55040;return 0x81700000;}
int costumes_slot_art(int kind,int color,int stock,const unsigned char** bytes,unsigned* size){
 if(kind!=6||color!=5)return 0;*bytes=stock?stock_texture:texture;*size=stock?sizeof stock_texture:sizeof texture;return 1;
}
void func_80032330(Context* c){c->gpr[3]=6;}
void func_80033198(Context* c){c->gpr[3]=5;}
void func_802F94E0(Context* c){draws++;uint32_t desc=mem_read32(c,observed_tobj+0x58);assert(desc==0x81700000u+4*PORTRAIT_STRIDE);assert(mem_read8(c,mem_read32(c,desc))==0x3C);}
void func_80391070(Context* c){
 draws++;uint32_t desc=mem_read32(c,observed_tobj+0x58);
 assert(desc==0x81700000u);assert(mem_read32(c,desc+8)==14);
 assert(mem_read8(c,mem_read32(c,desc))==0x5A);
}
static void write_be(unsigned char* p,unsigned v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
int main(void){
 Context c={0};c.ram=calloc(1,24u<<20);c.ram_size=24u<<20;assert(c.ram);
 memcpy(texture,"YAMPPTEX",8);write_be(texture+8,136);write_be(texture+12,188);write_be(texture+16,14);write_be(texture+20,13056);memset(texture+24,0x5A,13056);
 memcpy(stock_texture,"YAMPPTEX",8);write_be(stock_texture+8,24);write_be(stock_texture+12,24);write_be(stock_texture+16,14);write_be(stock_texture+20,288);memset(stock_texture+24,0x3C,288);
 const uint32_t g=0x80010000,j=0x80011000,d=0x80012000,m=0x80013000,t=0x80014000,old=0x80015000;
 observed_tobj=t;mem_write32(&c,g+0x28,j);mem_write32(&c,j+0x18,d);mem_write32(&c,d+8,m);mem_write32(&c,m+8,t);mem_write32(&c,t+0x58,old);
 mem_write16(&c,old+4,136);mem_write16(&c,old+6,188);mem_write32(&c,old+8,14);
 mem_write32(&c,0x804D6CC0,j);mem_write8(&c,0x803F0B24+1,6);mem_write8(&c,0x803F0DFC+0xD,5);
 for(unsigned p=1;p<4;p++)mem_write8(&c,0x803F0DFC+p*0x24+0xB,3);
 memset(c.ram+(0x81700000&RAM_MASK),0xC7,ART_BYTES);
 unsigned char* before=malloc(c.ram_size);assert(before);memcpy(before,c.ram,c.ram_size);
 c.gpr[3]=g;css_draw(&c);assert(draws==1);assert(mem_read32(&c,t+0x58)==old);assert(dirty_arena);
 costume_art_frame_done(&c);assert(!dirty_arena);assert(!memcmp(before,c.ram,c.ram_size));
 assert(!image(&c,6,0,0,0));assert(!dirty_arena);assert(!memcmp(before,c.ram,c.ram_size));
 write_be(texture+20,13055);assert(!image(&c,6,5,0,0));assert(!dirty_arena);
 write_be(texture+20,13056);assert(image(&c,6,5,0,3)==0x81700000u+3*PORTRAIT_STRIDE);
 costume_art_reset(&c);assert(!memcmp(before,c.ram,c.ram_size));
 mem_write16(&c,old+4,24);mem_write16(&c,old+6,24);mem_write32(&c,g+0x2C,0x80016000);
 memcpy(before,c.ram,c.ram_size);c.gpr[3]=g;stock_draw(&c);assert(draws==2);assert(mem_read32(&c,t+0x58)==old);
 costume_art_frame_done(&c);assert(!memcmp(before,c.ram,c.ram_size));
 puts("PASS: CSS and HUD original draws, exact pointer restore, full 24 MB RAM restore, stock fallback, malformed GXT rejection, reset cleanup");
 free(before);free(c.ram);return 0;
}
