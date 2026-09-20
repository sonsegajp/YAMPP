// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_EFB_ACCESS_H
#define GXRUNTIME_EFB_ACCESS_H

#include "core/types.h"

// GameCube CPU↔EFB access window (GXPeek/GXPoke*) + bounding-box substrate.
//
// Hardware: physical 0x08000000..0x0BFFFFFF maps uncached as
// 0xC8000000..0xCBFFFFFF (Dolphin MMU.cpp EFB_Read/EFB_Write;
// SDK GXMisc.c GXPokeARGB/GXPeekARGB via OSPhysicalToUncached(0x08000000)).
// Address encoding: x = (addr >> 2) & 0x3FF, y = (addr >> 12) & 0x3FF,
// type = (addr >> 22) & 3  (0=color, 1=Z, 2/3=unimplemented Z+color).
//
// U8 substrate:
//   * Optional software color+Z backing (enable via dol_efb_access_enable_backing).
//     With backing, peeks/pokes are real pixel R/W for the CPU path (certificate
//     GXPeekARGB/GXPokeARGB). Without backing, accesses stay absorbed and
//     tripwire-tagged policy/efb_stub (C818 residual until a host attaches pixels).
//   * Bounding box (BP 0x55/0x56 clear; PE BBOX left/right/top/bottom readback):
//     software min/max counters, expandable from host/draw code. GPU storage-
//     buffer atomics live in the Aurora fork (gfx/bounding_box.*); this device
//     is the CPU-visible PE surface + fixture substrate.
//
// Residual (honest): GPU poke into MSAA unresolved targets is not attempted —
// WriteTexture hits the present-source (resolved) texture only. Continuous
// present-source → software fill is host-armed via fill_color_rgba8.

#define DOL_EFB_ACCESS_BASE 0xC8000000u
#define DOL_EFB_ACCESS_BYTES 0x04000000u  // through 0xCBFFFFFF

#define DOL_EFB_ACCESS_TYPE_COLOR 0u
#define DOL_EFB_ACCESS_TYPE_Z 1u

// Hardware EFB max (Dolphin EFB_WIDTH/EFB_HEIGHT).
#define DOL_EFB_WIDTH_MAX 640u
#define DOL_EFB_HEIGHT_MAX 528u

// Bounding-box indices (PE_BBOX_LEFT..BOTTOM / BPMEM_CLEARBBOX packing).
#define DOL_EFB_BBOX_LEFT 0u
#define DOL_EFB_BBOX_RIGHT 1u
#define DOL_EFB_BBOX_TOP 2u
#define DOL_EFB_BBOX_BOTTOM 3u

typedef struct DolEfbAccess {
    u16 last_x;
    u16 last_y;
    u8 last_type;
    u32 last_color;
    u32 last_z;
    u64 poke_color_count;
    u64 poke_z_count;
    u64 peek_color_count;
    u64 peek_z_count;
    u64 unimplemented_count;
    u64 fill_count;

    // Software backing (NULL = stub / policy residual).
    u32* color;  // ARGB8888, row-major width*height
    u32* depth;  // 24-bit Z in low 24 bits
    u16 width;
    u16 height;
    bool backing_owned;  // true if color/depth were malloc'd by enable_backing

    // Bounding box: left, right, top, bottom (u16 extents; SDK default 1023,0,1023,0).
    u16 bbox[4];
    bool bbox_active;
    u64 bbox_update_count;
} DolEfbAccess;

void dol_efb_access_init(DolEfbAccess* efb);
void dol_efb_access_shutdown(DolEfbAccess* efb);

// Allocate (or attach) a software color+Z store. width/height clamped to max.
// If color/depth are non-NULL they are borrowed (not freed on shutdown);
// otherwise malloc's internal buffers. Returns false on OOM / bad size.
bool dol_efb_access_enable_backing(DolEfbAccess* efb, u16 width, u16 height,
                                   u32* color, u32* depth);
void dol_efb_access_disable_backing(DolEfbAccess* efb);
bool dol_efb_access_has_backing(const DolEfbAccess* efb);

// Direct pixel API (same semantics as MMIO peeks/pokes; for fixtures/HLE).
bool dol_efb_access_poke_color(DolEfbAccess* efb, u16 x, u16 y, u32 color);
bool dol_efb_access_peek_color(DolEfbAccess* efb, u16 x, u16 y, u32* color);
bool dol_efb_access_poke_z(DolEfbAccess* efb, u16 x, u16 y, u32 z);
bool dol_efb_access_peek_z(DolEfbAccess* efb, u16 x, u16 y, u32* z);

// Fill color backing from tightly packed RGBA8 rows (top-left). Auto-enables
// owned backing when size differs or no store yet. Depth is left untouched
// (zeros on fresh enable). Returns false on OOM / null / zero size.
bool dol_efb_access_fill_color_rgba8(DolEfbAccess* efb, const u8* rgba,
                                     u16 width, u16 height);

// Bounding box (software substrate). Clear packs match BP 0x55/0x56:
//   0x55: index0 = bits0..9, index1 = bits10..19
//   0x56: index2 = bits0..9, index3 = bits10..19
void dol_efb_bbox_reset(DolEfbAccess* efb);  // 1023,0,1023,0 (SDK "no pixels")
void dol_efb_bbox_clear_bp(DolEfbAccess* efb, u8 reg, u32 value);  // reg 0x55/0x56
void dol_efb_bbox_set(DolEfbAccess* efb, u32 index, u16 value);
u16 dol_efb_bbox_get(const DolEfbAccess* efb, u32 index);
// Expand to include pixel (x,y). Used by software draws / poke path when active.
void dol_efb_bbox_update(DolEfbAccess* efb, u16 x, u16 y);
void dol_efb_bbox_set_active(DolEfbAccess* efb, bool active);

bool dol_efb_access_contains(u32 ea);
bool dol_efb_access_decode(u32 ea, u16* x, u16* y, u8* type);

// MMIO surface.
u64 dol_efb_access_read(DolEfbAccess* efb, u32 ea, u8 size);
void dol_efb_access_write(DolEfbAccess* efb, u32 ea, u8 size, u64 value);

bool dol_efb_access_mmio_contains(u32 ea);
u64 dol_efb_access_mmio_read(DolEfbAccess* efb, u32 ea, u8 size);
void dol_efb_access_mmio_write(DolEfbAccess* efb, u32 ea, u8 size, u64 value);

#endif
