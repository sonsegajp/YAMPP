/* AX sample address edge cases observed by Dolphin's hardware-tested accelerator. */
#include <assert.h>
#include <stdio.h>
#include "../../../vendor/src/ax_host.c"
static u8 bytes[256];
u16 mem_read16(CPUState* c,u32 a){(void)c;(void)a;assert(0);return 0;}
u32 mem_read32(CPUState* c,u32 a){(void)c;(void)a;assert(0);return 0;}
void mem_write16(CPUState* c,u32 a,u16 v){(void)c;(void)a;(void)v;assert(0);}
void mem_write32(CPUState* c,u32 a,u32 v){(void)c;(void)a;(void)v;assert(0);}
u64 aram_read(u32 address,u8 size){assert(address+size<=sizeof bytes);u64 v=0;for(u8 i=0;i<size;i++)v=(v<<8)|bytes[address+i];return v;}
static SampleReader voice(u32 end){SampleReader r={0};r.running=1;r.format=DOL_AX_FMT_ADPCM;r.current=0x1f;r.end=end;r.loop=2;r.loop_flag=1;r.pred_scale=0;r.loop_pred_scale=2;r.yn1=10;r.yn2=9;return r;}
int main(void){
 memset(bytes,0x11,sizeof bytes);bytes[0x10]=0x37;
 SampleReader r=voice(0x20);assert(sample_reader_next(&r)==1);assert(r.current==3&&r.pred_scale==0&&r.yn1==1&&r.running==1);
 r=voice(0x21);assert(sample_reader_next(&r)==1);assert(r.current==2&&r.pred_scale==0&&r.yn1==1&&r.running==1);
 r=voice(0x1f);r.loop_yn1=42;r.loop_yn2=41;assert(sample_reader_next(&r)==1);assert(r.current==2&&r.pred_scale==2&&r.yn1==42&&r.yn2==41);
 r=voice(0x1f);r.stream=1;assert(sample_reader_next(&r)==1);assert(r.current==2&&r.yn1==1&&r.yn2==10&&r.loop_counter==1);
 r=voice(0x40);assert(sample_reader_next(&r)==1);assert(r.current==0x22&&r.pred_scale==0x37);
 r=voice(0x22);r.current=0x22;r.loop_flag=0;assert(sample_reader_next(&r)==1);assert(r.running==0);
 r=voice(0x21);r.loop_flag=0;sample_reader_next(&r);assert(r.running==1&&r.current==2);
 r=voice(3);r.format=DOL_AX_FMT_PCM16;r.current=3;sample_reader_next(&r);assert(r.current==2&&r.running==1);
 puts("PASS: ADPCM header-end loops, normal wraps, header prefetch, stream history, nonloop stop and PCM wrap");
}
