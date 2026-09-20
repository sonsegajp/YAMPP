// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_AURORA_BACKEND_H
#define GXRUNTIME_AURORA_BACKEND_H

#include "gxruntime/platform.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AuroraBackendConfig {
    const char* app_name;
    unsigned window_width;
    unsigned window_height;
    bool vsync;
    bool allow_texture_dumps;
    bool info_logging;
    bool graphics_logging;
    bool force_untextured;
} AuroraBackendConfig;

bool dol_aurora_initialize(int argc, char** argv,
                           const AuroraBackendConfig* config);
void dol_aurora_shutdown(void);

// Optional: after each present, push latest present-source RGBA8 into software
// EFB (e.g. Strikers mmio_efb + dol_efb_access_fill_color_rgba8). Enables
// continuous framebuffer readback while registered. NULL clears.
typedef void (*DolAuroraEfbFrameFillFn)(const uint8_t* rgba, uint32_t width,
                                        uint32_t height, void* user);
void dol_aurora_set_efb_frame_fill(DolAuroraEfbFrameFillFn fn, void* user);

// C2: walk nonzero gxcore GapCounters into the process-global gap_report.
// Call once before gap_report_write_json on the live Strikers path so
// --gap-report includes gxcore/* rows (same taxonomy as dolgx_replay --core).
// Idempotent for a given counter snapshot: safe to call from main then again
// from shutdown (second call only bumps if counters grew).
void dol_aurora_flush_gap_report(void);

#ifdef __cplusplus
}
#endif

#endif
