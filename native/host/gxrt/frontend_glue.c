/* Melee frontend glue for the GXRuntime host.
 *
 * Everything else the linker asked for is already supplied by
 * runtime/module_rt.c (lookup_function, ru_psq_load/store, hle_syscall,
 * hle_timebase, trace_enter) -- that file was written for the module ABI, so it
 * needed no conversion. These three are the remainder. */
#include "abi_recompcore.h"
#include <stdio.h>

int g_trace = 0;

/* Storage for the per-thread current context. abi_recompcore.h declares it
 * extern; host_rt.c used to define it, and nothing did after that file stopped
 * being linked -- which is what the unresolved __emutls_v.g_cur_ctx was. */
Context* g_cur_ctx = 0;

/* The emitter's ABI_CHECK: a callee must return with r1 as it found it. Kept as
 * a counter rather than an abort so a violation is reported, not fatal. */
long g_abi_violations = 0;
void abi_check(uint32_t callee, uint32_t sp_before, uint32_t sp_after)
{
    if (sp_before != sp_after) {
        if (g_abi_violations < 8)
            fprintf(stderr, "[abi] func_%08X returned r1=%08X, expected %08X\n",
                    callee, sp_after, sp_before);
        g_abi_violations++;
    }
}

/* Mid-function probe sink. The standalone probe machinery went with host_rt.c;
 * the emitter still emits calls for any address in the game config's PROBES. */
void probe_hit(uint32_t addr, Context* ctx) { (void)addr; (void)ctx; }
