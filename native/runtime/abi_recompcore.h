/* MeleeRecomp -> RecompCore / ModernGekko / GXRuntime module ABI (StaticRecompABI v2).
 *
 * Same generated function bodies as the standalone runtime; only the CPU-state
 * spelling and the RAM base differ (recompiler/emit.py set_abi("recompcore")).
 * Guest RAM stays big-endian, so every accessor byteswaps for the host.
 *
 * Memory routing: normal RAM goes straight to ctx->ram. Anything outside the RAM
 * aperture (MMIO, MEM2, locked cache) is handed to the chassis' external_read/
 * external_write callbacks, so Dolphin's own hardware models service it — that is
 * what lets an uncovered path fall back cleanly instead of reading garbage.
 */
#ifndef MELEE_ABI_RECOMPCORE_H
#define MELEE_ABI_RECOMPCORE_H

#include <stdint.h>
#include <setjmp.h>
jmp_buf* recomp_jmpbuf(uint32_t guest_buffer);
#include <string.h>
#include "core/cpu.h"

typedef CPUState Context;

#ifdef _MSC_VER
#include <intrin.h>
#define BSWAP16 _byteswap_ushort
#define BSWAP32 _byteswap_ulong
#define BSWAP64 _byteswap_uint64
#define INLINE  static __forceinline
#define TLS     __declspec(thread)
#else
#define BSWAP16 __builtin_bswap16
#define BSWAP32 __builtin_bswap32
#define BSWAP64 __builtin_bswap64
#define INLINE  static inline
#define TLS     __thread
#endif

#define RAM_MASK 0x01FFFFFFu

/* The CPU state currently being dispatched. Set on entry to dispatch(); the guest
 * CPU is single-threaded per chassis thread, so thread-local is the correct scope
 * and costs nothing on the hot path. Generated code calls the ctx-less accessors
 * below (unchanged from the native target), which read it. */
/* NOT thread-local. Measured: a guest MMIO write faulted in mem_w32's ext_w
 * path with g_cur_ctx=NULL while &s_cpu was valid -- the TLS slot was unset on
 * the thread that executed it. This frontend has exactly one guest context, so
 * TLS bought nothing and cost a null deref at 0x1d88 (the external_write field
 * offset off a null CPUState). A plain global cannot be unset per-thread. */
extern Context* g_cur_ctx;

/* In the RAM aperture? GameCube physical RAM is 24 MB mirrored at 0x8000_0000
 * (cached) and 0xC000_0000 (uncached); everything else is the chassis' problem. */
INLINE int in_ram(uint32_t a) {
    uint32_t p = a & 0x3FFFFFFFu;
    return p < g_cur_ctx->ram_size;
}

INLINE uint64_t ext_r(uint32_t a, uint8_t sz) { return g_cur_ctx->external_read(g_cur_ctx, a, sz); }
INLINE void     ext_w(uint32_t a, uint64_t v, uint8_t sz) { g_cur_ctx->external_write(g_cur_ctx, a, v, sz); }

#define RAMP(a) (g_cur_ctx->ram + ((a) & RAM_MASK))

INLINE uint8_t  mem_r8 (uint32_t a) { if (!in_ram(a)) return (uint8_t)ext_r(a,1); return *RAMP(a); }
INLINE uint16_t mem_r16(uint32_t a) { if (!in_ram(a)) return (uint16_t)ext_r(a,2); uint16_t v; memcpy(&v,RAMP(a),2); return BSWAP16(v); }
INLINE uint32_t mem_r32(uint32_t a) { if (!in_ram(a)) return (uint32_t)ext_r(a,4); uint32_t v; memcpy(&v,RAMP(a),4); return BSWAP32(v); }
INLINE uint64_t mem_r64(uint32_t a) { if (!in_ram(a)) return ext_r(a,8); uint64_t v; memcpy(&v,RAMP(a),8); return BSWAP64(v); }
INLINE void mem_w8 (uint32_t a, uint8_t v)  { if (!in_ram(a)) { ext_w(a,v,1); return; } *RAMP(a) = v; }
INLINE void mem_w16(uint32_t a, uint16_t v) { if (!in_ram(a)) { ext_w(a,v,2); return; } v = BSWAP16(v); memcpy(RAMP(a),&v,2); }
INLINE void mem_w32(uint32_t a, uint32_t v) { if (!in_ram(a)) { ext_w(a,v,4); return; } v = BSWAP32(v); memcpy(RAMP(a),&v,4); }
INLINE void mem_w64(uint32_t a, uint64_t v) { if (!in_ram(a)) { ext_w(a,v,8); return; } v = BSWAP64(v); memcpy(RAMP(a),&v,8); }

INLINE float  mem_rf32(uint32_t a) { uint32_t u = mem_r32(a); float f;  memcpy(&f,&u,4); return f; }
INLINE double mem_rf64(uint32_t a) { uint64_t u = mem_r64(a); double d; memcpy(&d,&u,8); return d; }
INLINE void   mem_wf32(uint32_t a, float f)  { uint32_t u; memcpy(&u,&f,4); mem_w32(a,u); }
INLINE void   mem_wf64(uint32_t a, double d) { uint64_t u; memcpy(&u,&d,8); mem_w64(a,u); }

/* ---- condition register / XER helpers (field names match CPUState verbatim) ---- */
INLINE void cr_set_field(Context* c, int n, uint32_t val4) {
    int sh = (7 - n) * 4;
    c->cr = (c->cr & ~(0xFu << sh)) | ((val4 & 0xF) << sh);
}
INLINE uint32_t cr_signed(int32_t a, uint32_t so) {
    uint32_t v = (a < 0) ? 8u : (a > 0) ? 4u : 2u;
    return v | (so & 1);
}
INLINE uint32_t cr_unsigned(uint32_t a, uint32_t b, uint32_t so) {
    uint32_t v = (a < b) ? 8u : (a > b) ? 4u : 2u;
    return v | (so & 1);
}
INLINE void cr0_from(Context* c, int32_t res) { cr_set_field(c, 0, cr_signed(res, c->xer >> 31)); }
INLINE uint32_t cr_bit(Context* c, int b) { return (c->cr >> (31 - b)) & 1u; }
INLINE void cr_set_bit(Context* c, int b, uint32_t v) {
    uint32_t m = 1u << (31 - b);
    c->cr = v ? (c->cr | m) : (c->cr & ~m);
}
INLINE uint32_t xer_ca(Context* c) { return (c->xer >> 29) & 1u; }
INLINE void set_ca(Context* c, uint32_t ca) {
    c->xer = ca ? (c->xer | (1u << 29)) : (c->xer & ~(1u << 29));
}
INLINE uint32_t clz32(uint32_t x) {
    if (x == 0) return 32;
#ifdef _MSC_VER
    unsigned long i; _BitScanReverse(&i, x); return 31u - i;
#else
    return (uint32_t)__builtin_clz(x);
#endif
}
INLINE uint32_t rotl32(uint32_t x, int n) { n &= 31; return (x << n) | (x >> ((32 - n) & 31)); }
INLINE uint32_t ppc_mask(int mb, int me) {
    uint32_t begin = 0xFFFFFFFFu >> mb;
    uint32_t end   = (me >= 31) ? 0xFFFFFFFFu : (0xFFFFFFFFu << (31 - me));
    return (mb <= me) ? (begin & end) : (begin | end);
}

/* Backward branches must still service devices in leaf-function polling loops. */
extern uint32_t g_recomp_poll_budget;
void recomp_poll(Context* ctx);
#define RECOMP_POLL(c) do { if (--g_recomp_poll_budget == 0) recomp_poll(c); } while (0)

/* ---- dispatch + hooks ---- */
typedef void (*RecFn)(Context*);
#include "mex_runtime.h"
RecFn    lookup_function(uint32_t addr);
int      lookup_is_stub(RecFn fn);
/* Emitted by call_target when ABI_CHECK is on: verifies a callee restored r1.
 * Declared here as well as in recomp.h so the recompcore-ABI output, which
 * includes only this header, links. */
void     abi_check(uint32_t callee, uint32_t sp_before, uint32_t sp_after);
/* Mid-function probe sink, emitted for addresses in the game config's PROBES.
 * Same omission as abi_check: declared only in recomp.h, so recompcore output
 * that contained a probe failed to compile. */
void     probe_hit(uint32_t addr, Context* ctx);
void     hle_syscall(Context* ctx);
void hle_write_spr(Context* ctx, uint32_t spr, uint32_t value);
uint32_t hle_timebase(int upper);
void     ru_psq_load(Context* ctx, int frD, uint32_t ea, int w, int gqr);
void     ru_psq_store(Context* ctx, int frS, uint32_t ea, int w, int gqr);

extern int g_trace;
void trace_enter(uint32_t addr, Context* ctx);
#define TRACE_ENTER(a, c) do { if (g_trace) trace_enter(a, c); } while (0)

#endif /* MELEE_ABI_RECOMPCORE_H */
