/* Melee AX DSP command lists and AI DMA at the GameCube sample clock.
 * CPU-side AX synthesis and callbacks remain the original recompiled game. */
#include "abi_recompcore.h"
#include "gxruntime/audio_dma.h"
#include "gxruntime/ax_host.h"
#include <stdio.h>
#include <stdlib.h>
extern DolAudioDma g_adma;
static DolAxHost mixer;
static int initialized, expecting_list, pumping;
static uint64_t last_tick, fractional;
static FILE* capture;
long g_audio_chunks, g_audio_callbacks, g_audio_lists;
void frontend_audio_mail(Context* ctx, uint32_t mail) {
    if (!initialized) { dol_ax_host_init(&mixer); mixer.legacy_ax=1; dol_ax_host_set_log(&mixer,1); initialized=1; }
    if (expecting_list) {
        expecting_list=0;
        if ((mail & RAM_MASK)<ctx->ram_size) {
            dol_ax_host_process_command_list(&mixer,ctx,mail);
            g_audio_lists++;
            /* Synchronous host DSP has finished this list and yielded. */
            mem_w32(0x804D753Cu,1);
        }
    } else if (mail==0xBABE0180u) expecting_list=1;
}
void frontend_service_audio(Context* ctx) {
    extern int netplay_devices_deferred(void);
    if (netplay_devices_deferred()) return;
    extern int aurora_link_audio(const int16_t*,uint32_t,uint32_t);
    if (!ctx || pumping || !(ctx->msr&0x8000u)) return;
    extern uint64_t frontend_guest_timebase(void);
    uint64_t now=frontend_guest_timebase();
    if (!(g_adma.control&DOL_AUDIO_DMA_ENABLE)) { last_tick=now;fractional=0;return; }
    if (!last_tick) { last_tick=now;return; }
    uint64_t elapsed=now-last_tick;
    last_tick=now;
    uint32_t rate=dol_audio_dma_sample_rate(&g_adma);
    fractional+=elapsed*(rate/8u);
    uint64_t chunks=fractional/40500000u;
    fractional%=40500000u;
    if (!chunks) return;
    if (chunks>512) chunks=512;
    pumping=1;
    Context saved=*ctx;
    ctx->msr &= ~0x8000u;
    int16_t pcm[512*16];
    uint32_t samples=0;
    for (uint32_t k=0;k<chunks;k++) {
        uint32_t source=0;
        if (!dol_audio_dma_poll(&g_adma,&source)) continue;
        for (uint32_t i=0;i<8;i++) {
            pcm[samples++]=(int16_t)mem_r16(source+i*4+2);
            pcm[samples++]=(int16_t)mem_r16(source+i*4);
        }
        g_audio_chunks++;
        if (dol_audio_dma_dsp_interrupt_pending(&g_adma)) {
            uint32_t osctx=mem_r32(0x800000E4u);
            RecFn handler=lookup_function(0x803509C0u);
            if (!lookup_is_stub(handler) && osctx) {
                ctx->gpr[3]=5;ctx->gpr[4]=osctx;handler(ctx);g_audio_callbacks++;
            }
        }
    }
    uint64_t advanced=ctx->timebase;
    *ctx=saved;ctx->timebase=advanced;
    extern int netplay_replaying(void);
    if (samples && !netplay_replaying()) {
        extern void mods_mix_audio(int16_t*,unsigned,unsigned);
        extern void music_mix(short*,unsigned,unsigned);
        mods_mix_audio(pcm,samples/2,rate);
        music_mix(pcm,samples/2,rate);
        aurora_link_audio(pcm,samples/2,rate);
        static int capture_checked;
        if (!capture_checked) {
            const char* path=getenv("MELEE_AUDIO_CAPTURE");
            if (path && *path) capture=fopen(path,"wb");
            capture_checked=1;
        }
        if (capture) { fwrite(pcm,sizeof(int16_t),samples,capture);fflush(capture); }
    }
    pumping=0;
}
void frontend_audio_report(void) {
    fprintf(stderr,"audio: chunks=%ld callbacks=%ld lists=%ld frames=%llu nonzero=%llu peak=%u unknown=%u\n",
        g_audio_chunks,g_audio_callbacks,g_audio_lists,(unsigned long long)mixer.stats.frames,
        (unsigned long long)mixer.stats.nonzero_frames,mixer.stats.peak_mix,mixer.stats.unknown_cmd_count);
}

void frontend_audio_trace(Context* ctx,uint32_t fn) {
    static int debug=-1;
    if(debug<0) debug=getenv("MELEE_AUDIO_TRACE")!=NULL;
    if(!debug) return;
    uint32_t handle=mem_r32(0x804D7760),node=0x804C2C64+(handle&63)*80;
    if(fn==0x8038912C) node=ctx->gpr[3];
    fprintf(stderr,"[stream] fn=%08X lr=%08X frame=%llu handle=%08X node=%08X id=%08X flags=%02X count=%u arg=%08X\n",fn,ctx->lr,(unsigned long long)mixer.stats.frames,handle,node,mem_r32(node),mem_r8(node+9),mem_r8(node+10),ctx->gpr[3]);
    for(unsigned i=0;i<mem_r8(node+10) && i<2;i++) {
        uint32_t v=mem_r32(node+12+i*4),pb=v+0x138;
        fprintf(stderr,"[stream] voice=%08X state=%u loop=%u fmt=%u cur=%08X end=%08X loopaddr=%08X vol=%u mix=%04X ratio=%08X\n",v,mem_r16(pb+14),mem_r16(pb+0x6e),mem_r16(pb+0x70),mem_r32(pb+0x7a),mem_r32(pb+0x76),mem_r32(pb+0x72),mem_r16(pb+0x64),mem_r16(pb+12),mem_r32(pb+0xa6));
    }
}

/* Guest-thread snapshot; no handles or host call stacks are serialized. */
size_t frontend_audio_snapshot(void* data, int restore) {
  unsigned char* bytes = data; size_t offset = 0;
#define RB_COPY(field) do { if (bytes) { if (restore) memcpy(&(field), bytes + offset, sizeof(field)); else memcpy(bytes + offset, &(field), sizeof(field)); } offset += sizeof(field); } while (0)
  RB_COPY(mixer);
  RB_COPY(initialized);
  RB_COPY(expecting_list);
  RB_COPY(last_tick);
  RB_COPY(fractional);
  RB_COPY(g_adma);
#undef RB_COPY
  return offset;
}

void frontend_audio_rebase(uint64_t clock) { last_tick = clock; fractional = 0; }

void frontend_audio_release(void){if(capture){fclose(capture);capture=NULL;}}
