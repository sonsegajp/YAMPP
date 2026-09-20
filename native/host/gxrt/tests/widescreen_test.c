/* Exercise production hooks: copied cameras, texture projection and clear state. */
#include <assert.h>
#include "../hooks.c"
static int wide;
static float render_aspect, texture_aspect, erase_aspect, bounds_aspect;
static unsigned erased;
static uint32_t camera;
static float read_float(Context* c, uint32_t at) {
  union { uint32_t u; float f; } v = { .u = mem_read32(c, at) }; return v.f;
}
static void write_float(Context* c, uint32_t at, float f) {
  union { uint32_t u; float f; } v = { .f = f }; mem_write32(c, at, v.u);
}
u8 mem_read8(CPUState* c,u32 a){assert(valid_guest(a,1));return c->ram[a&RAM_MASK];}
u32 mem_read32(CPUState* c,u32 a){return ((u32)mem_read8(c,a)<<24)|((u32)mem_read8(c,a+1)<<16)|((u32)mem_read8(c,a+2)<<8)|mem_read8(c,a+3);}
void mem_write8(CPUState* c,u32 a,u8 v){assert(valid_guest(a,1));c->ram[a&RAM_MASK]=v;}
void mem_write32(CPUState* c,u32 a,u32 v){mem_write8(c,a,v>>24);mem_write8(c,a+1,v>>16);mem_write8(c,a+2,v>>8);mem_write8(c,a+3,v);}
int aurora_link_widescreen(int set,int value){if(set)wide=value;return wide;}
void func_80369C0C(Context* c){c->fpr[1]=read_float(c,c->gpr[3]+0x44);c->ps1[1]=c->fpr[1];}
void func_80342BEC(Context* c){render_aspect=(float)c->fpr[2];assert(c->ps1[2]==c->fpr[2]);}
void func_80342954(Context* c){texture_aspect=(float)c->fpr[2];assert(c->ps1[2]==c->fpr[2]);}
void func_803676F8(Context* c){erased++;erase_aspect=c->gpr[3]?read_float(c,c->gpr[3]+0x44):0;c->gpr[3]=0;}
void func_800307D0(Context* c){c->gpr[3]=camera;hook_cobj_get_aspect(c);bounds_aspect=(float)c->fpr[1];}
static void close_to(float a,float b){assert(fabsf(a-b)<0.000001f);}
int main(void){
  Context c={0};c.ram_size=24u<<20;c.ram=calloc(1,c.ram_size);assert(c.ram);
  camera=0x80010000;const uint32_t copy=0x80011000;
  const float canonical=1.2173333168029785f;
  write_float(&c,camera+0x44,canonical);mem_write8(&c,camera+0x50,1);
  for(wide=0;wide<2;wide++){
    float expected=canonical*(wide?ASPECT_WIDEN:1.f);
    c.gpr[3]=camera;hook_cobj_get_aspect(&c);close_to((float)c.fpr[1],canonical);
    /* FoD copies GetAspect into its reflection camera. Neither stored value changes. */
    write_float(&c,copy+0x44,(float)c.fpr[1]);mem_write8(&c,copy+0x50,1);
    c.fpr[2]=c.ps1[2]=read_float(&c,copy+0x44);hook_mtx_perspective(&c);
    c.fpr[2]=c.ps1[2]=read_float(&c,copy+0x44);hook_mtx_light_perspective(&c);
    close_to(render_aspect,expected);close_to(texture_aspect,expected);
    hook_camera_ground_bounds(&c);close_to(bounds_aspect,expected);assert(!camera_ground_scope);
    c.gpr[3]=camera;hook_cobj_get_aspect(&c);close_to((float)c.fpr[1],canonical);
    unsigned char* before=malloc(c.ram_size);assert(before);memcpy(before,c.ram,c.ram_size);
    c.gpr[3]=camera;hook_cobj_erase_screen(&c);close_to(erase_aspect,expected);
    assert(!memcmp(before,c.ram,c.ram_size));free(before);
    /* Orthographic clears, and the original NULL no-op, stay untouched. */
    mem_write8(&c,camera+0x50,3);c.gpr[3]=camera;hook_cobj_erase_screen(&c);
    close_to(erase_aspect,canonical);mem_write8(&c,camera+0x50,1);
    c.gpr[3]=0;hook_cobj_erase_screen(&c);assert(!erase_aspect);
  }
  assert(erased==6);free(c.ram);
  puts("PASS: canonical camera copies, exactly-once render/texture aspect, scoped ground bounds, full 24 MB restore, 4:3/ortho/NULL unchanged");return 0;
}
