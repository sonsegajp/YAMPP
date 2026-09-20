// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_AX_HOST_H
#define GXRUNTIME_AX_HOST_H

#include "core/cpu.h"
#include "core/types.h"

// Host model of the Nintendo AX DSP command processor used by MusyX (and the
// retail AX library). Game CPU-side sequencing, voice allocation, streaming
// callbacks, and 3D emitters stay in guest code; this module replaces only the
// DSP mixer that consumes AX parameter blocks and writes stereo/surround AID
// buffers back into guest memory.
//
// DR-7 B2.2 transitional accelerant (certificate MusyX 54-fn contract scope for
// Strikers). Not DSP-LLE (B2.3). Command opcodes and PB layout match MusyX
// sal*/AX DSP command lists observed on G4QE01 and Dolphin AXOut.

#define DOL_AX_SAMPLES_PER_MS 32u
#define DOL_AX_FRAME_MS 5u
#define DOL_AX_FRAME_SAMPLES (DOL_AX_SAMPLES_PER_MS * DOL_AX_FRAME_MS)
#define DOL_AX_CHANNELS 9u
#define DOL_AX_MAX_VOICES 64u
#define DOL_AX_MAX_COMMANDS 4096u

// PB sample formats (AX ADPCM/PCM).
#define DOL_AX_FMT_ADPCM 0x0000u
#define DOL_AX_FMT_PCM16 0x000Au
#define DOL_AX_FMT_PCM8 0x0019u

// Channel indices into the per-frame mix bus (MAIN / AUXA / AUXB × L/R/S).
enum {
    DOL_AX_MAIN_L = 0,
    DOL_AX_MAIN_R,
    DOL_AX_MAIN_S,
    DOL_AX_AUXA_L,
    DOL_AX_AUXA_R,
    DOL_AX_AUXA_S,
    DOL_AX_AUXB_L,
    DOL_AX_AUXB_R,
    DOL_AX_AUXB_S,
};

typedef struct DolAxHostStats {
    u64 frames;
    u64 nonzero_frames;
    u32 active_voices;
    u32 peak_voices;
    u32 peak_mix;
    u32 last_unknown_cmd;
    u32 unknown_cmd_count;
} DolAxHostStats;

typedef struct DolAxHost {
    s32 mix[DOL_AX_CHANNELS][DOL_AX_FRAME_SAMPLES];
    u32 active_voices;
    DolAxHostStats stats;
    bool log;
    bool legacy_ax; /* Melee AX PB ends at 0xC0, before LPF/loop-counter extensions. */
} DolAxHost;

void dol_ax_host_init(DolAxHost* ax);
void dol_ax_host_set_log(DolAxHost* ax, bool enabled);
void dol_ax_host_reset_stats(DolAxHost* ax);
const DolAxHostStats* dol_ax_host_stats(const DolAxHost* ax);

// Run one AX command list starting at guest address `list`. Sample data is
// read from ARAM via aram_read; PB/command/state live in guest MEM1.
// Writes stereo (R/L interleaved s16) and surround (s32) output buffers as
// directed by the command list.
void dol_ax_host_process_command_list(DolAxHost* ax, CPUState* cpu, u32 list);

#endif
