#include <stdlib.h>
#include "platform_compat.h"
#include <gxruntime/gx_recomp.h>
#include <string.h>
/* MMIO bus assembly for the Melee frontend.
 *
 * GXRuntime deliberately leaves this to the frontend: each device ships its own
 * handlers and base constant, but nothing in the runtime binds them together.
 * Bases come from the device headers rather than from constants I derive by
 * hand -- hand-derived GameCube MMIO addresses were wrong twice in the old
 * standalone host, and there is no reason to repeat that here.
 */
#include "abi_recompcore.h"
#include "gxruntime/mmio_bus.h"
#include "gxruntime/interrupts.h"
#include "gxruntime/cp.h"
#include "gxruntime/pe.h"
#include "gxruntime/mi.h"
#include "gxruntime/di.h"
#include "gxruntime/si.h"
#include "gxruntime/exi.h"
#include "gxruntime/audio_dma.h"
#include "gxruntime/efb_access.h"
#include "gxruntime/gx_recomp.h"
#include "gxruntime/dvd.h"
#include "gxruntime/guest_memory.h"
#include <stdio.h>

DolMmioBus     g_bus;
DolInterrupts  g_irq;      /* covers both VI (0xCC002000) and PI (0xCC003000) */
DolCp          g_cp;
DolPe          g_pe;
DolMi          g_mi;
DolDi          g_di;
DolSiDevice    g_si;
DolExi         g_exi;
DolAudioDma    g_adma;
DolEfbAccess   g_efb;
DolGxRecompState g_gx;
static DolGuestAddressResolver s_resolver;

/* Thunks: the bus wants one uniform callback shape, the devices each have their
 * own. Adapting here keeps the device APIs untouched. */
#define RD_THUNK(name, dev, call) \
    static bool name(void* u, CPUState* c, u32 ea, u8 sz, u64* out) \
    { (void)u; (void)c; *out = call; return true; }
#define WR_THUNK(name, dev, call) \
    static bool name(void* u, CPUState* c, u32 ea, u8 sz, u64 v) \
    { (void)u; (void)c; call; return true; }

RD_THUNK(rd_irq, g_irq, dol_interrupts_mmio_read(&g_irq, ea, sz))
WR_THUNK(wr_irq, g_irq, dol_interrupts_mmio_write(&g_irq, ea, sz, v))
RD_THUNK(rd_cp,  g_cp,  dol_cp_mmio_read(&g_cp, ea, sz))
WR_THUNK(wr_cp,  g_cp,  dol_cp_mmio_write(&g_cp, ea, sz, v))
RD_THUNK(rd_di,  g_di,  dol_di_mmio_read(&g_di, ea, sz))
WR_THUNK(wr_di,  g_di,  dol_di_mmio_write(&g_di, c, ea, sz, v))
RD_THUNK(rd_si,  g_si,  dol_si_mmio_read(&g_si, ea, sz))
WR_THUNK(wr_si,  g_si,  dol_si_mmio_write(&g_si, ea, sz, v))
RD_THUNK(rd_exi, g_exi, dol_exi_mmio_read(&g_exi, ea, sz))
static bool wr_exi(void* u,CPUState* c,u32 ea,u8 sz,u64 v) {
    (void)u;
    if(ea==DOL_EXI_BASE && ((g_exi.channels[0].status^(u32)v)&DOL_EXI_STATUS_CHIP_SELECT_MASK)) {
        extern void frontend_ipl_deselect(void);frontend_ipl_deselect();
        extern void frontend_card_select(unsigned,unsigned);frontend_card_select((g_exi.channels[0].status>>7)&7,((u32)v>>7)&7);
    }
    dol_exi_mmio_write(&g_exi,c,ea,sz,v);dol_interrupts_set_source(&g_irq,DOL_PI_CAUSE_EXI,dol_exi_interrupt_pending(&g_exi));return true;
}
RD_THUNK(rd_efb, g_efb, dol_efb_access_mmio_read(&g_efb, ea, sz))
WR_THUNK(wr_efb, g_efb, dol_efb_access_mmio_write(&g_efb, ea, sz, v))

/* DI command executor. The DI device models the register interface and
 * delegates execution to the frontend -- registering its MMIO region is not the
 * same as connecting it. Without this the guest issued read commands, DI
 * accepted them, nothing performed the transfer, and the guest's own FST read
 * came back empty, so DVDReadAsync rejected the resulting range and the SDK
 * halted.
 *
 * DVD command 0xA8 is read: command[1] is the disc offset in 4-byte units,
 * command[2] the byte length. */
long g_di_reads = 0, g_di_bytes = 0;
long g_wg_bytes = 0, g_wg_writes = 0;
/* Two command segments: the guest appends to one while the host submits the
 * other. Closing on the guest thread at GXSetDrawDone means a segment always
 * ends on a real frame boundary -- closing it from the host loop cut streams
 * mid-draw ("need 1940 bytes at pos 3, have 472"). */
/* Four segments, not two.
 *
 * With two, the guest returns to the buffer the host is copying after a single
 * close -- and splicing display lists makes the guest write far more per frame,
 * which is what turned this into a hard crash. Four means three closes must
 * elapse first, which cannot happen while the host is inside one memcpy. */
#define SEG_N 4
static u8* g_seg[SEG_N];
static u32 g_seg_len[SEG_N], g_seg_cap[SEG_N];
static volatile int g_seg_cur = 0;
/* Offset of the first inlined list in the segment being built, so a run can
 * submit only the bytes *before* any splice. If that parses cleanly while the
 * full segment misaligns, the divergence is introduced exactly at the
 * insertion point rather than by the list contents. */
u32 g_first_splice_off[SEG_N];

/* Pending display list, recorded when the guest enters GXCallDisplayList and
 * consumed when its GX_CMD_CALL_DL opcode actually reaches WGPIPE.
 *
 * Splicing at function entry put the list *before* the commands
 * GXCallDisplayList itself emits (vertex-cache invalidate and friends), so
 * aurora executed it at a different point in the stream than the GP would --
 * measured as a misalignment that only ever appeared at or after the insertion
 * offset. Substituting at the opcode puts it exactly where the GP runs it. */
static const u8* s_pending_list = NULL;
static u32       s_pending_len = 0;
static u32       s_swallow = 0;      /* bytes of the CALL_DL command still to drop */
void wgseg_pending_dl(const u8* host_list, u32 nbytes)
{ s_pending_list = host_list; s_pending_len = nbytes; }
#define SEG_GUARD 256u
#define SEG_CAP (8u << 20)          /* pre-sized: a realloc from the guest thread
                                     * while the host submits the same buffer is
                                     * what turned "no fatals" into a hard crash */
volatile int g_seg_ready = -1;
static u32 g_seg_sent = 0;   /* bytes already handed to aurora */
long g_seg_guard_hits = 0;
long g_close_calls = 0;

/* Inline a display list into the stream. The recursive/patching version was
 * lost when the submission path was simplified; this is the plain form --
 * append the bytes. Recursion and CP rewriting can come back once submissions
 * are verified working again, which is the point of this simplification. */
/* Re-added after the simplification dropped them: walk the list's register
 * prefix, record CP arraybase writes for the frame-start registration path,
 * and resolve nested CALL_DL. Draws are sized with gx_recomp's own layout
 * derivation, so the walk reaches the whole list rather than stopping at the
 * first draw. Depth-limited against malformed pointers. */
extern unsigned char* aurora_link_mem1_base;
extern u32 aurora_link_mem1_size;

void wgseg_scan_list(const u8* w, u32 nbytes, int depth)
{
    extern void aurora_link_note_cp_array(u32 cp_idx, u32 phys);
    u32 i = 0;
    u8 stop_op = 0xFFu;
    if (depth > 2) return;
    if (depth == 0) {
        static long nhead = 0;
        if (nhead < 6) {
            u32 d, hn = (nbytes < 24u) ? nbytes : 24u;
            nhead++;
            printf("[scan] HEAD len=%u:", nbytes);
            for (d = 0; d < hn; d++) printf(" %02X", w[d]);
            printf("\n");
        }
    }
    while (i + 6 <= nbytes) {
        u8 op = w[i];
        if (op == 0x00) { i += 1; continue; }
        if (op == 0x08) {
            if ((w[i+1] & 0xF0u) == 0xA0u) {
                u32 v = ((u32)w[i+2]<<24)|((u32)w[i+3]<<16)|((u32)w[i+4]<<8)|w[i+5];
                aurora_link_note_cp_array(w[i+1] & 0x0Fu, v);
            }
            i += 6; continue;
        }
        if (op == 0x61) { if (i + 5 > nbytes) { stop_op = op; break; } i += 5; continue; }
        if (op == 0x10) {
            u32 cnt, step, xfaddr;
            if (i + 5 > nbytes) break;
            cnt = (((u32)w[i+1]<<8)|w[i+2]) + 1u;
            xfaddr = (((u32)w[i+3]<<8)|w[i+4]);
            step = 5u + cnt * 4u;
            if (step < 5u || i + step > nbytes) { stop_op = op; break; }
            /* Texgen params live at XF 0x1040-0x104F. aurora reads the source
             * field from these; a value past GX_TG_COLOR1 (20) means it is
             * reading something that is not a texgen descriptor. Log what the
             * spliced stream actually carries there. */
            if (xfaddr >= 0x0040u && xfaddr <= 0x0047u) {   /* correct range: aurora uses reg-0x40 */
                static long nx = 0;
                if (nx < 8) {
                    u32 v = ((u32)w[i+5]<<24)|((u32)w[i+6]<<16)|((u32)w[i+7]<<8)|w[i+8];
                    nx++;
                    /* Mirror aurora's decode exactly (regs.cpp:785):
                     * tgType = bits 4..6, srcRow = bits 7..11. srcRow >= 13
                     * leaves tcg.src unassigned, which is how a stale 21
                     * survives to the shader. */
                    printf("[xftg] addr=%04X cnt=%u val=%08X tgType=%u srcRow=%u%s\n",
                           xfaddr, cnt, v, (v >> 4) & 7u, (v >> 7) & 0x1Fu,
                           (((v >> 7) & 0x1Fu) >= 13u) ? "  <-- OUT OF RANGE" : "");
                    fflush(stdout);
                }
            }
            i += step; continue;
        }
        if (op == 0x40) {
            u32 ea, sz2, off;
            if (i + 9 > nbytes) break;
            ea  = ((u32)w[i+1]<<24)|((u32)w[i+2]<<16)|((u32)w[i+3]<<8)|w[i+4];
            sz2 = ((u32)w[i+5]<<24)|((u32)w[i+6]<<16)|((u32)w[i+7]<<8)|w[i+8];
            off = ea & 0x0FFFFFFFu;
            if (aurora_link_mem1_base && sz2 && off + sz2 <= aurora_link_mem1_size)
                wgseg_scan_list(aurora_link_mem1_base + off, sz2, depth + 1);
            i += 9; continue;
        }
        if ((op & 0xF8u) >= 0x80u && (op & 0xF8u) <= 0xB8u) {
            u32 cnt, vsz; u8 fmt = (u8)(op & 0x07u);
            if (i + 3 > nbytes) break;
            cnt = ((u32)w[i+1] << 8) | w[i+2];
            if (!dol_gx_recomp_derive_vertex_layout(&g_gx, fmt)) break;
            vsz = g_gx.vertex_layouts[fmt].vertex_size;
            if (!vsz || vsz == 0xFFFFFFFFu || i + 3u + cnt * vsz > nbytes) { stop_op = op; break; }
            i += 3u + cnt * vsz; continue;
        }
        stop_op = op;
        break;
    }
    /* Report coverage. Without this, "nothing found" and "never looked" are
     * indistinguishable -- which made six negative results this session
     * unfalsifiable. */
    if (depth == 0) {
        static long nrep = 0;
        if (nrep < 4) {
            nrep++;
            u32 d, lo = (i > 8u) ? i - 8u : 0u, hi = (i + 16u < nbytes) ? i + 16u : nbytes;
            printf("[scan] walked %u of %u bytes, stopped on opcode %02X\n",
                   i, nbytes, stop_op);
            printf("[scan]  vcd_lo=%08X(v%d) vcd_hi=%08X(v%d) vat0=%08X vsz[0..3]=%u,%u,%u,%u\n",
                   g_gx.vcd_lo, (int)g_gx.vcd_lo_valid, g_gx.vcd_hi,
                   (int)g_gx.vcd_hi_valid, g_gx.vat_group0[0],
                   g_gx.vertex_layouts[0].vertex_size, g_gx.vertex_layouts[1].vertex_size,
                   g_gx.vertex_layouts[2].vertex_size, g_gx.vertex_layouts[3].vertex_size);
            printf("[scan]  bytes %u..%u:", lo, hi);
            for (d = lo; d < hi; d++)
                printf("%s%02X", (d == i) ? " >" : " ", w[d]);
            printf("\n");
            fflush(stdout);
        }
    }
}

void wgseg_inline_dl(const u8* host_list, u32 nbytes)
{
    if (!host_list || !nbytes) return;
    wgseg_scan_list(host_list, nbytes, 0);
    if (!g_seg[0]) {
        g_seg[0] = (u8*)malloc(SEG_CAP + SEG_GUARD);
        if (g_seg[0]) { memset(g_seg[0] + SEG_CAP, 0xCC, SEG_GUARD); g_seg_cap[0] = SEG_CAP; }
    }
    if (!g_seg[0] || g_seg_len[0] + nbytes > g_seg_cap[0]) return;
    memcpy(g_seg[0] + g_seg_len[0], host_list, nbytes);
    g_seg_len[0] += nbytes;
}
static u32 g_seg_mark = 0;   /* bytes the guest had written at last draw-done */
long g_seg_drops = 0;              /* index holding a complete frame */
u8* g_wgseg = NULL; u32 g_wgseg_len = 0, g_wgseg_cap = 0;

/* Called from the guest thread when the game signals draw-done. */
/* One continuous stream, submitted as deltas.
 *
 * Rotating buffers made each frame's submission independent, which meant
 * anything the game emitted before the first draw-done -- 2,725 bytes of
 * initialisation, including vertex descriptors -- was never handed to aurora at
 * all. Priming fixed POS and immediately exposed TEX0 as the next casualty of
 * the same hole. Keeping one buffer and submitting everything appended since
 * the last submission makes aurora's view of the stream complete by
 * construction, splices included. */
/* One complete guest frame in flight. The guest stays parked until the GPU
 * has consumed its commands and all referenced RAM, preventing frame mixing. */
static volatile LONG s_frame_ready=0;
static HANDLE s_frame_available;
static HANDLE s_frame_released;
int wgseg_wait_ready(DWORD timeout) {
    if(InterlockedCompareExchange(&s_frame_ready,0,0)==1) return 1;
    if(WaitForSingleObject(s_frame_available,timeout)!=WAIT_OBJECT_0) return 0;
    return InterlockedCompareExchange(&s_frame_ready,0,0)==1;
}
volatile LONGLONG g_fifo_ready_ticks,g_fifo_wait_ticks;
void wgseg_close(void)
{
    g_close_calls++;
    g_seg_mark=g_seg_len[0];
    LARGE_INTEGER ready,done;
    QueryPerformanceCounter(&ready);g_fifo_ready_ticks=ready.QuadPart;
    InterlockedExchange(&s_frame_ready,1);
    SetEvent(s_frame_available);
    while (InterlockedCompareExchange(&s_frame_ready,0,0)!=0) {
        extern void frontend_guest_checkpoint(void);frontend_guest_checkpoint();WaitForSingleObject(s_frame_released,10);
    }
    QueryPerformanceCounter(&done);g_fifo_wait_ticks+=done.QuadPart-ready.QuadPart;
}
const u8* wgseg_take(u32* len)
{
    if (InterlockedCompareExchange(&s_frame_ready,2,1)!=1) {
        *len=0; return NULL;
    }
    *len=g_seg_mark;
    return g_seg[0];
}
void wgseg_release(void)
{
    g_seg_len[0]=0;
    g_seg_sent=0;
    g_seg_mark=0;
    InterlockedExchange(&s_frame_ready,0);
    SetEvent(s_frame_released);
}

static const u8* wgseg_take_unused(u32* len)
{
    int r = g_seg_ready;
    if (r < 0) { *len = 0; return NULL; }
    *len = g_seg_len[r];
    if (getenv("MELEE_TRUNC_AT_SPLICE") && g_first_splice_off[r] != 0xFFFFFFFFu)
        *len = g_first_splice_off[r];       /* bytes before any inlined list */
    g_seg_ready = -1;
    return g_seg[r];
}
static DolDiCommandResult di_execute(void* user, DolDiCommand* c)
{
    (void)user;
    if (c == NULL) return DOL_DI_COMMAND_ERROR;
    u8 op = (u8)(c->command[0] >> 24);
    if (op == 0xA8u && c->dma && !c->write) {
        u32 disc_off = c->command[1] << 2;
        /* The dropped byte-writes start exactly at the DevCom relay buffer, and
         * the HLE's own context is healthy, so this is the other caller. */
        if (g_di_reads < 3)
            printf("[di] cpu=%p ram=%p size=%08x ext=%p  dma=%08x len=%x\n",
                   (void*)c->cpu, c->cpu ? (void*)c->cpu->ram : 0,
                   c->cpu ? (unsigned)c->cpu->ram_size : 0,
                   c->cpu ? (void*)(size_t)c->cpu->external_write : 0,
                   c->dma_address, c->dma_length);
        dvd_read_to_guest(c->cpu, c->dma_address, disc_off, c->dma_length);
        g_di_reads++; g_di_bytes += c->dma_length;
        /* Raise the DI interrupt. dol_di_complete_command sets the device's
         * TCINT status bit but does not touch PI, so without this the guest's
         * DI handler never runs, the SDK's DVD queue never advances, and boot
         * sleeps on __DVDThreadQueue forever after a single 32-byte read.
         * Telling the device a command finished is not telling the guest. */
        dol_interrupts_set_source(&g_irq, DOL_PI_CAUSE_DI, true);
        return DOL_DI_COMMAND_COMPLETE;
    }
    /* Anything else (inquiry, seek, audio) reports done without transferring;
     * Melee's boot path is read-driven. */
    return DOL_DI_COMMAND_COMPLETE;
}

/* Let an HLE that bypasses the DI device keep its state consistent.
 *
 * DVDReadAsyncPrio is HLE'd to read straight from the mounted image, which
 * fixed the data path (3 -> 1,188 reads) and broke the status path: DI's
 * registers were never touched, so DVDGetDriveStatus polled a device that had
 * no idea a transfer occurred. A runtime that models devices faithfully notices
 * when the frontend short-circuits them; each HLE has to maintain the
 * device-visible state it skips, not merely produce the right data. */
uint32_t frontend_di_dma_address(void) { return g_di.dma_address; }

void frontend_di_mark_complete(void)
{
    g_di.status |= DOL_DI_STATUS_TCINT;
    g_di.control &= ~DOL_DI_CONTROL_TSTART;
    g_di.dma_length = 0u;
}

/* Write-gather pipe at 0xCC008000. Every GX command the game issues goes
 * through this one address; CP models the MMIO surface but explicitly does not
 * ingest the FIFO ring, so WGPIPE traffic belongs to gx_recomp. Missing this
 * region meant 776 GX writes went nowhere -- nothing could ever have rendered,
 * independently of the boot halt. */
static bool wr_wgpipe(void* u, CPUState* c, u32 ea, u8 sz, u64 v)
{
    (void)u; (void)c; (void)ea;
    u8 buf[8];
    for (u8 i = 0; i < sz && i < 8; i++)          /* big-endian, MSB first */
        buf[i] = (u8)(v >> (8 * (sz - 1 - i)));
    /* Optional byte-level CP watch. This diagnostic does not parse command
     * boundaries and must not scan/flush every production matrix word.
     *
     * Byte-level CP watch.
     *
     * The SDK writes WGPIPE with GX_WRITE_U8 -- one byte per store -- so a
     * 6-byte CP command never appears as a single multi-byte write. The
     * previous probe tested the first two bytes of a store and could not have
     * produced a hit; it reported zero and that zero meant nothing. This walks
     * bytes and can demonstrably fire (any CP write at all trips it). */
    static int cp_watch = -1;
    if (cp_watch < 0) cp_watch = getenv("MELEE_TRACE_DIAGNOSTICS") != NULL;
    if (cp_watch) {
        static int st = 0; static u8 cp[6]; static long ncp = 0, nvcd = 0;
        for (u8 bi = 0; bi < sz && bi < 8; bi++) {
            u8 b = buf[bi];
            if (st == 0) { if (b == 0x08u) { cp[0] = b; st = 1; } continue; }
            cp[st++] = b;
            if (st == 6) {
                st = 0; ncp++;
                if (cp[1] == 0x50u || cp[1] == 0x60u) {
                    nvcd++;
                    if (nvcd <= 6)
                        printf("[cpw] VCD reg=%02X val=%02X%02X%02X%02X  (cp writes so far %ld)\n",
                               cp[1], cp[2], cp[3], cp[4], cp[5], ncp);
                } else if (ncp <= 3) {
                    printf("[cpw] cp reg=%02X val=%02X%02X%02X%02X\n",
                           cp[1], cp[2], cp[3], cp[4], cp[5]);
                }
                fflush(stdout);
            }
        }
    }
    dol_gx_recomp_push_fifo(&g_gx, buf, sz);
    /* Own copy of this frame's command stream.
     *
     * gx_recomp's buffer is a diagnostic accumulator: capped at 262144 bytes,
     * spanning many frames, starting wherever it starts. Replaying that into
     * aurora landed mid-command and died on "unknown opcode 0x1C at pos 128".
     * aurora executes what it is given, so it needs exactly the bytes written
     * since the last draw-done, from a command boundary. */
    /* Byte-at-a-time so a CALL_DL command spanning several WGPIPE writes is
     * handled: swallow its 9 bytes and emit the referenced list in its place. */
    {   int cur = 0;
        /* Allocate here. The original append block -- which owned the
         * allocation -- was left disabled behind `if (0)` when this
         * byte-at-a-time loop replaced it, so g_seg[0] stayed NULL and every
         * append silently dropped. That is why 2.5 MB of captured GX traffic
         * reached aurora as 2,725 bytes. */
        if (!g_seg[cur]) {
            g_seg[cur] = (u8*)malloc(SEG_CAP + SEG_GUARD);
            if (g_seg[cur]) { memset(g_seg[cur] + SEG_CAP, 0xCC, SEG_GUARD); g_seg_cap[cur] = SEG_CAP; }
        }
        if (!s_swallow && !(s_pending_list && s_pending_len)) {
            /* The ordinary path has no display-list marker to intercept.
             * Preserve the old partial append at the capacity boundary. */
            u32 count = sz < sizeof buf ? sz : (u32)sizeof buf;
            if (g_seg[cur] && g_seg_len[cur] < g_seg_cap[cur]) {
                u32 available = g_seg_cap[cur] - g_seg_len[cur];
                if (count > available) count = available;
                memcpy(g_seg[cur] + g_seg_len[cur], buf, count);
                g_seg_len[cur] += count;
            }
        } else for (u8 bi = 0; bi < sz && bi < 8; bi++) {
            u8 b = buf[bi];
            if (s_swallow) { s_swallow--; continue; }
            if (b == 0x40 && s_pending_list && s_pending_len) {
                wgseg_inline_dl(s_pending_list, s_pending_len);
                s_pending_list = NULL; s_pending_len = 0;
                s_swallow = 8;                  /* drop the opcode's 8-byte payload */
                continue;
            }
            if (g_seg[cur] && g_seg_len[cur] + 1u <= g_seg_cap[cur])
                g_seg[cur][g_seg_len[cur]++] = b;
        }
    }
    if (0) {   int cur = g_seg_cur;
        if (!g_seg[cur]) {
            g_seg[cur] = (u8*)malloc(SEG_CAP + SEG_GUARD);
            if (g_seg[cur]) { memset(g_seg[cur] + SEG_CAP, 0xCC, SEG_GUARD); g_seg_cap[cur] = SEG_CAP; }
        }
        if (g_seg[cur] && g_seg_len[cur] + sz <= g_seg_cap[cur]) {
            memcpy(g_seg[cur] + g_seg_len[cur], buf, sz);
            g_seg_len[cur] += sz;
        }
    }
    g_wg_bytes += sz; g_wg_writes++;
    return true;
}
static bool rd_wgpipe(void* u, CPUState* c, u32 ea, u8 sz, u64* out)
{ (void)u; (void)c; (void)ea; (void)sz; *out = 0; return true; }

/* MI already returns the bus's boolean shape, so it needs no adaptation beyond
 * the user-pointer argument. Registering dol_mi_init without its region was the
 * gap: __OSInitAudioSystem writes MI at 0xCC00401C and polls, and an unbacked
 * write went nowhere. */
static bool rd_mi(void* u, CPUState* c, u32 ea, u8 sz, u64* out)
{ (void)u; (void)c; return dol_mi_mmio_read(&g_mi, ea, sz, out); }
static bool wr_mi(void* u, CPUState* c, u32 ea, u8 sz, u64 v)
{ (void)u; (void)c; return dol_mi_mmio_write(&g_mi, ea, sz, v); }

/* Audio takes a register offset rather than an effective address. */
static bool rd_dsp(void* u, CPUState* c, u32 ea, u8 sz, u64* out)
{ (void)u; (void)c; *out = 0; dol_audio_dma_dsp_mmio_read(&g_adma, ea - 0xCC005000u, sz, out); return true; }
static bool wr_dsp(void* u, CPUState* c, u32 ea, u8 sz, u64 v)
{ (void)u;
  dol_audio_dma_dsp_mmio_write(&g_adma, ea - 0xCC005000u, sz, v);
  if ((ea==0xCC005002u && sz==2) || (ea==0xCC005000u && sz==4)) {
      extern void frontend_audio_mail(Context*,uint32_t);
      uint32_t mail=((uint32_t)g_adma.dsp_regs[0]<<24)|((uint32_t)g_adma.dsp_regs[1]<<16)|((uint32_t)g_adma.dsp_regs[2]<<8)|g_adma.dsp_regs[3];
      g_adma.dsp_regs[0] &= 0x7f;
      frontend_audio_mail(c,mail);
  }
  return true;
}

/* AI control block at 0xCC006C00. Registering DSP but not AI is what stalled
 * __OSInitAudioSystem: it configures both, and an unbacked AI read returned
 * nothing the SDK would accept. */
static bool rd_ai(void* u, CPUState* c, u32 ea, u8 sz, u64* out)
{ (void)u; (void)c; *out = 0; dol_audio_dma_ai_mmio_read(&g_adma, ea - 0xCC006C00u, sz, out); return true; }
static bool wr_ai(void* u, CPUState* c, u32 ea, u8 sz, u64 v)
{ (void)u; (void)c; dol_audio_dma_ai_mmio_write(&g_adma, ea - 0xCC006C00u, sz, v); return true; }

/* Route every guest hardware access through the bus. */
/* Report unhandled accesses rather than swallowing them. Ignoring the bus's
 * boolean return made an unregistered region read as 0, so a guest polling an
 * unbacked status register spins forever in silence -- which is exactly the
 * symptom __OSInitAudioSystem is showing. */
long g_mmio_unhandled = 0;
static void note_unhandled(const char* dir, u32 ea, u8 size)
{
    static u32 seen[16]; static int n = 0;
    for (int i = 0; i < n; i++) if (seen[i] == ea) { g_mmio_unhandled++; return; }
    if (n < 16) {
        seen[n++] = ea;
        printf("[mmio] unhandled %s 0x%08X size=%u\n", dir, ea, size);
    }
    g_mmio_unhandled++;
}

/* Hot-read census. A poll loop shows up as one address read millions of times
 * with an unchanging value -- that names both the register and what the SDK
 * keeps rejecting, without assuming which device is at fault. */
#define HOTN 24
u32  g_hot_ea[HOTN]; u64 g_hot_val[HOTN]; long g_hot_cnt[HOTN]; int g_hot_n = 0;

static void note_read(u32 ea, u64 v)
{
    for (int i = 0; i < g_hot_n; i++)
        if (g_hot_ea[i] == ea) { g_hot_cnt[i]++; g_hot_val[i] = v; return; }
    if (g_hot_n < HOTN) {
        g_hot_ea[g_hot_n] = ea; g_hot_val[g_hot_n] = v; g_hot_cnt[g_hot_n] = 1;
        g_hot_n++;
    }
}

void frontend_bus_report(void)
{
    printf("[gx] wgpipe writes=%ld bytes=%ld  fifo=%u\n",
           g_wg_writes, g_wg_bytes, dol_gx_recomp_fifo_size(&g_gx));
    /* What does the parser actually see? The XFB is addressed but empty, so
     * either no copy is issued, or one is issued and nothing consumes it.
     * gx_recomp only emits a trace event for a copy -- it does not write
     * pixels, and this frontend never reads those events. Count them by kind
     * before wiring a consumer, so the two cases stay separable. */
    {   unsigned long kinds[16]; int i;
        for (i = 0; i < 16; i++) kinds[i] = 0;
        for (u32 k = 0; k < g_gx.trace_count; k++) {
            unsigned kk = (unsigned)g_gx.trace[k].kind;
            if (kk < 16) kinds[kk]++;
        }
        printf("[gx] trace events=%u  draw=%lu  copy_dest=%lu  bp=%lu  texture=%lu  dlist=%lu\n",
               g_gx.trace_count, kinds[12], kinds[8], kinds[9], kinds[6], kinds[2]);
        { unsigned prim[256] = {0}; unsigned long verts[256] = {0};
          for (u32 k = 0; k < g_gx.trace_count; k++) {
              if (g_gx.trace[k].kind == DOL_GX_RECOMP_EVENT_DRAW) {
                  unsigned op = g_gx.trace[k].a & 0xFFu;
                  prim[op]++;
                  verts[op] += g_gx.trace[k].c;
              }
          }
          printf("[gx] primitives:");
          for (unsigned op = 0; op < 256; op++)
              if (prim[op]) printf(" %02X=%u/%lu", op, prim[op], verts[op]);
          printf("\n"); }
        for (u32 k = 0; k < g_gx.trace_count && k < 4000u; k++)
            if (g_gx.trace[k].kind == DOL_GX_RECOMP_EVENT_COPY_DESTINATION)
                { printf("[gx] COPY dest=%08X bytes=%u fmt=%u clear=%u dims=%08X\n",
                         g_gx.trace[k].a, g_gx.trace[k].b, g_gx.trace[k].c,
                         g_gx.trace[k].d, g_gx.trace[k].g); break; }
    }
    printf("[di] reads=%ld bytes=%ld\n", g_di_reads, g_di_bytes);
    printf("[mmio] read census (unhandled=%ld):\n", g_mmio_unhandled);
    for (int i = 0; i < g_hot_n; i++)
        if (g_hot_cnt[i] > 100)
            printf("   0x%08X  reads=%-10ld last=0x%llX\n",
                   g_hot_ea[i], g_hot_cnt[i], (unsigned long long)g_hot_val[i]);
}

static u8 s_locked_cache[0x4000];
static u64 fe_ext_read(CPUState* cpu, u32 ea, u8 size)
{
    if (ea >= 0xE0000000u && ea - 0xE0000000u + size <= sizeof s_locked_cache) {
        u64 value = 0;
        for (u8 i=0; i<size; ++i) value=(value<<8)|s_locked_cache[ea-0xE0000000u+i];
        return value;
    }
    u64 v = 0;
    /* Advance virtual time on MMIO reads as well.
     *
     * Guest-driven time is what makes runs reproducible, but it deadlocks
     * wherever the guest spins *inside* one function: no call, no TRACE_ENTER,
     * no tick, so the register it is polling never changes. The guest halted at
     * exactly 6,585 calls every run for that reason. A poll loop has to read
     * MMIO to make progress, so ticking here closes the hole and stays a pure
     * function of guest behaviour. */
    {   extern void frontend_tick_guest_time(void);
        static long rd = 0;
        if (++rd >= 400) { rd = 0; frontend_tick_guest_time(); }
    }
    if (!dol_mmio_bus_read(&g_bus, cpu, ea, size, &v)) note_unhandled("read ", ea, size);
    note_read(ea, v);
    return v;
}
static void fe_ext_write(CPUState* cpu, u32 ea, u64 value, u8 size)
{
    if (ea >= 0xE0000000u && ea - 0xE0000000u + size <= sizeof s_locked_cache) {
        for (u8 i=0; i<size; ++i) s_locked_cache[ea-0xE0000000u+i]=(u8)(value>>(8*(size-1-i)));
        return;
    }
    if (!dol_mmio_bus_write(&g_bus, cpu, ea, size, value)) note_unhandled("write", ea, size);
}

void frontend_bus_init(CPUState* cpu)
{
    s_frame_available=CreateEventA(NULL,FALSE,FALSE,NULL);
    s_frame_released=CreateEventA(NULL,FALSE,FALSE,NULL);
    if(!s_frame_available || !s_frame_released) { fprintf(stderr,"Cannot create frame handoff events\n");ExitProcess(4); }
    dol_mmio_bus_init(&g_bus);
    dol_interrupts_init(&g_irq);
    dol_cp_init(&g_cp);  dol_pe_init(&g_pe);  dol_mi_init(&g_mi);
    dol_di_init(&g_di);  dol_si_init(&g_si);  dol_exi_init(&g_exi);
    { extern void aram_init(void); aram_init(); }
    { extern void frontend_card_init(void);frontend_card_init(); }
    dol_audio_dma_init(&g_adma);  dol_efb_access_init(&g_efb);
    { extern bool frontend_ipl_transfer(void*,DolExiTransfer*);dol_exi_set_transfer_callback(&g_exi,frontend_ipl_transfer,NULL); }
    dol_di_set_command_callback(&g_di, di_execute, NULL);
    dol_di_set_disc_present(&g_di, true);
    dol_guest_address_resolver_init(&s_resolver, NULL, cpu);
    dol_gx_recomp_init(&g_gx, &s_resolver);

    /* Disjoint FIFO traffic dominates MMIO stores; match it before devices. */
    dol_mmio_bus_register(&g_bus, 0xCC008000u,  0x100u,  rd_wgpipe, wr_wgpipe, &g_gx);
    dol_mmio_bus_register(&g_bus, DOL_CP_BASE,  0x1000u, rd_cp,  wr_cp,  &g_cp);
    dol_mmio_bus_register(&g_bus, DOL_VI_BASE,  0x1000u, rd_irq, wr_irq, &g_irq);
    dol_mmio_bus_register(&g_bus, DOL_PI_BASE,  0x1000u, rd_irq, wr_irq, &g_irq);
    dol_mmio_bus_register(&g_bus, 0xCC005000u,  0x400u,  rd_dsp, wr_dsp, &g_adma);
    dol_mmio_bus_register(&g_bus, DOL_DI_BASE,  0x400u,  rd_di,  wr_di,  &g_di);
    dol_mmio_bus_register(&g_bus, DOL_SI_BASE,  0x400u,  rd_si,  wr_si,  &g_si);
    dol_mmio_bus_register(&g_bus, DOL_EXI_BASE, 0x400u,  rd_exi, wr_exi, &g_exi);
    dol_mmio_bus_register(&g_bus, DOL_PE_BASE,  0x1000u, rd_efb, wr_efb, &g_efb);
    dol_mmio_bus_register(&g_bus, 0xCC006C00u,  0x100u,  rd_ai,  wr_ai,  &g_adma);
    dol_mmio_bus_register(&g_bus, DOL_MI_BASE,  0x1000u, rd_mi,  wr_mi,  &g_mi);

    cpu->external_read  = fe_ext_read;
    cpu->external_write = fe_ext_write;
}

/* Guest-thread snapshot; no handles or host call stacks are serialized. */
size_t frontend_bus_snapshot(void* data, int restore) {
  unsigned char* bytes = data; size_t offset = 0;
#define RB_COPY(field) do { if (bytes) { if (restore) memcpy(&(field), bytes + offset, sizeof(field)); else memcpy(bytes + offset, &(field), sizeof(field)); } offset += sizeof(field); } while (0)
  RB_COPY(s_locked_cache);
  RB_COPY(g_di);
#undef RB_COPY
  return offset;
}

void frontend_bus_release(void) {
 for(unsigned i=0;i<SEG_N;i++){free(g_seg[i]);g_seg[i]=NULL;}
 if(s_frame_available)CloseHandle(s_frame_available);
 if(s_frame_released)CloseHandle(s_frame_released);
 dol_efb_access_shutdown(&g_efb);
}
