// SPDX-License-Identifier: GPL-3.0-or-later
#include "gxruntime/efb_access.h"

#include "gxruntime/gap_report.h"

#include <stdlib.h>
#include <string.h>

void dol_efb_access_init(DolEfbAccess* efb) {
    if (efb == NULL)
        return;
    memset(efb, 0, sizeof(*efb));
    dol_efb_bbox_reset(efb);
}

void dol_efb_access_shutdown(DolEfbAccess* efb) {
    if (efb == NULL)
        return;
    dol_efb_access_disable_backing(efb);
    memset(efb, 0, sizeof(*efb));
    dol_efb_bbox_reset(efb);
}

bool dol_efb_access_contains(u32 ea) {
    return ea >= DOL_EFB_ACCESS_BASE &&
           ea < DOL_EFB_ACCESS_BASE + DOL_EFB_ACCESS_BYTES;
}

bool dol_efb_access_mmio_contains(u32 ea) {
    return dol_efb_access_contains(ea);
}

bool dol_efb_access_decode(u32 ea, u16* x, u16* y, u8* type) {
    if (!dol_efb_access_contains(ea))
        return false;
    if (x != NULL)
        *x = (u16)((ea >> 2) & 0x3FFu);
    if (y != NULL)
        *y = (u16)((ea >> 12) & 0x3FFu);
    if (type != NULL)
        *type = (u8)((ea >> 22) & 0x3u);
    return true;
}

bool dol_efb_access_has_backing(const DolEfbAccess* efb) {
    return efb != NULL && efb->color != NULL && efb->depth != NULL &&
           efb->width > 0u && efb->height > 0u;
}

bool dol_efb_access_enable_backing(DolEfbAccess* efb, u16 width, u16 height,
                                   u32* color, u32* depth) {
    if (efb == NULL || width == 0u || height == 0u)
        return false;
    if (width > DOL_EFB_WIDTH_MAX)
        width = (u16)DOL_EFB_WIDTH_MAX;
    if (height > DOL_EFB_HEIGHT_MAX)
        height = (u16)DOL_EFB_HEIGHT_MAX;

    dol_efb_access_disable_backing(efb);

    const size_t n = (size_t)width * (size_t)height;
    bool owned = false;
    if (color == NULL || depth == NULL) {
        u32* c = (u32*)calloc(n, sizeof(u32));
        u32* z = (u32*)calloc(n, sizeof(u32));
        if (c == NULL || z == NULL) {
            free(c);
            free(z);
            return false;
        }
        color = c;
        depth = z;
        owned = true;
    }

    efb->color = color;
    efb->depth = depth;
    efb->width = width;
    efb->height = height;
    efb->backing_owned = owned;
    return true;
}

void dol_efb_access_disable_backing(DolEfbAccess* efb) {
    if (efb == NULL)
        return;
    if (efb->backing_owned) {
        free(efb->color);
        free(efb->depth);
    }
    efb->color = NULL;
    efb->depth = NULL;
    efb->width = 0;
    efb->height = 0;
    efb->backing_owned = false;
}

static size_t pixel_index(const DolEfbAccess* efb, u16 x, u16 y) {
    return (size_t)y * (size_t)efb->width + (size_t)x;
}

static bool in_bounds(const DolEfbAccess* efb, u16 x, u16 y) {
    return dol_efb_access_has_backing(efb) && x < efb->width && y < efb->height;
}

bool dol_efb_access_poke_color(DolEfbAccess* efb, u16 x, u16 y, u32 color) {
    if (!in_bounds(efb, x, y))
        return false;
    efb->color[pixel_index(efb, x, y)] = color;
    efb->last_x = x;
    efb->last_y = y;
    efb->last_type = DOL_EFB_ACCESS_TYPE_COLOR;
    efb->last_color = color;
    efb->poke_color_count += 1u;
    if (efb->bbox_active)
        dol_efb_bbox_update(efb, x, y);
    return true;
}

bool dol_efb_access_peek_color(DolEfbAccess* efb, u16 x, u16 y, u32* color) {
    if (!in_bounds(efb, x, y))
        return false;
    const u32 c = efb->color[pixel_index(efb, x, y)];
    efb->last_x = x;
    efb->last_y = y;
    efb->last_type = DOL_EFB_ACCESS_TYPE_COLOR;
    efb->last_color = c;
    efb->peek_color_count += 1u;
    if (color != NULL)
        *color = c;
    return true;
}

bool dol_efb_access_poke_z(DolEfbAccess* efb, u16 x, u16 y, u32 z) {
    if (!in_bounds(efb, x, y))
        return false;
    const u32 z24 = z & 0x00FFFFFFu;
    efb->depth[pixel_index(efb, x, y)] = z24;
    efb->last_x = x;
    efb->last_y = y;
    efb->last_type = DOL_EFB_ACCESS_TYPE_Z;
    efb->last_z = z24;
    efb->poke_z_count += 1u;
    if (efb->bbox_active)
        dol_efb_bbox_update(efb, x, y);
    return true;
}

bool dol_efb_access_peek_z(DolEfbAccess* efb, u16 x, u16 y, u32* z) {
    if (!in_bounds(efb, x, y))
        return false;
    const u32 z24 = efb->depth[pixel_index(efb, x, y)] & 0x00FFFFFFu;
    efb->last_x = x;
    efb->last_y = y;
    efb->last_type = DOL_EFB_ACCESS_TYPE_Z;
    efb->last_z = z24;
    efb->peek_z_count += 1u;
    if (z != NULL)
        *z = z24;
    return true;
}

bool dol_efb_access_fill_color_rgba8(DolEfbAccess* efb, const u8* rgba,
                                     u16 width, u16 height) {
    if (efb == NULL || rgba == NULL || width == 0u || height == 0u)
        return false;
    if (width > DOL_EFB_WIDTH_MAX)
        width = (u16)DOL_EFB_WIDTH_MAX;
    if (height > DOL_EFB_HEIGHT_MAX)
        height = (u16)DOL_EFB_HEIGHT_MAX;
    if (!dol_efb_access_has_backing(efb) || efb->width != width ||
        efb->height != height) {
        if (!dol_efb_access_enable_backing(efb, width, height, NULL, NULL))
            return false;
    }
    const size_t n = (size_t)width * (size_t)height;
    for (size_t i = 0; i < n; ++i) {
        const u8* p = rgba + i * 4u;
        const u32 r = p[0];
        const u32 g = p[1];
        const u32 b = p[2];
        const u32 a = p[3];
        efb->color[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    efb->fill_count += 1u;
    return true;
}

void dol_efb_bbox_reset(DolEfbAccess* efb) {
    if (efb == NULL)
        return;
    // SDK / Dolphin fallback "no pixels drawn": left=1023 right=0 top=1023 bottom=0
    efb->bbox[DOL_EFB_BBOX_LEFT] = 1023u;
    efb->bbox[DOL_EFB_BBOX_RIGHT] = 0u;
    efb->bbox[DOL_EFB_BBOX_TOP] = 1023u;
    efb->bbox[DOL_EFB_BBOX_BOTTOM] = 0u;
}

void dol_efb_bbox_set_active(DolEfbAccess* efb, bool active) {
    if (efb != NULL)
        efb->bbox_active = active;
}

void dol_efb_bbox_clear_bp(DolEfbAccess* efb, u8 reg, u32 value) {
    if (efb == NULL)
        return;
    // BPMEM_CLEARBBOX1=0x55, CLEARBBOX2=0x56 — two 10-bit fields.
    const u16 a = (u16)(value & 0x3FFu);
    const u16 b = (u16)((value >> 10) & 0x3FFu);
    if (reg == 0x55u) {
        efb->bbox[DOL_EFB_BBOX_LEFT] = a;
        efb->bbox[DOL_EFB_BBOX_RIGHT] = b;
    } else if (reg == 0x56u) {
        efb->bbox[DOL_EFB_BBOX_TOP] = a;
        efb->bbox[DOL_EFB_BBOX_BOTTOM] = b;
    }
}

void dol_efb_bbox_set(DolEfbAccess* efb, u32 index, u16 value) {
    if (efb == NULL || index >= 4u)
        return;
    efb->bbox[index] = value;
}

u16 dol_efb_bbox_get(const DolEfbAccess* efb, u32 index) {
    if (efb == NULL || index >= 4u)
        return 0u;
    return efb->bbox[index];
}

void dol_efb_bbox_update(DolEfbAccess* efb, u16 x, u16 y) {
    if (efb == NULL)
        return;
    if (x < efb->bbox[DOL_EFB_BBOX_LEFT])
        efb->bbox[DOL_EFB_BBOX_LEFT] = x;
    if (x > efb->bbox[DOL_EFB_BBOX_RIGHT])
        efb->bbox[DOL_EFB_BBOX_RIGHT] = x;
    if (y < efb->bbox[DOL_EFB_BBOX_TOP])
        efb->bbox[DOL_EFB_BBOX_TOP] = y;
    if (y > efb->bbox[DOL_EFB_BBOX_BOTTOM])
        efb->bbox[DOL_EFB_BBOX_BOTTOM] = y;
    efb->bbox_update_count += 1u;
}

static void note_stub(const char* key) {
    gap_report_note("efb", "cpu-access-stub", key, 0u,
                    "policy/efb_stub until U8 peek-poke");
}

u64 dol_efb_access_read(DolEfbAccess* efb, u32 ea, u8 size) {
    (void)size;
    if (efb == NULL)
        return 0u;
    u16 x = 0, y = 0;
    u8 type = 0;
    if (!dol_efb_access_decode(ea, &x, &y, &type))
        return 0u;
    efb->last_x = x;
    efb->last_y = y;
    efb->last_type = type;

    if (type == DOL_EFB_ACCESS_TYPE_COLOR) {
        u32 color = 0;
        if (dol_efb_access_peek_color(efb, x, y, &color))
            return color;
        // Out of backing bounds or no backing → stub.
        efb->peek_color_count += 1u;
        if (!dol_efb_access_has_backing(efb))
            note_stub("peek-color");
        return 0u;
    }
    if (type == DOL_EFB_ACCESS_TYPE_Z) {
        u32 z = 0;
        if (dol_efb_access_peek_z(efb, x, y, &z))
            return z;
        efb->peek_z_count += 1u;
        if (!dol_efb_access_has_backing(efb))
            note_stub("peek-z");
        return 0u;
    }
    efb->unimplemented_count += 1u;
    note_stub("unimplemented");
    return 0u;
}

void dol_efb_access_write(DolEfbAccess* efb, u32 ea, u8 size, u64 value) {
    (void)size;
    if (efb == NULL)
        return;
    u16 x = 0, y = 0;
    u8 type = 0;
    if (!dol_efb_access_decode(ea, &x, &y, &type))
        return;
    efb->last_x = x;
    efb->last_y = y;
    efb->last_type = type;

    if (type == DOL_EFB_ACCESS_TYPE_COLOR) {
        if (dol_efb_access_poke_color(efb, x, y, (u32)value))
            return;
        efb->last_color = (u32)value;
        efb->poke_color_count += 1u;
        if (!dol_efb_access_has_backing(efb))
            note_stub("poke-color");
        return;
    }
    if (type == DOL_EFB_ACCESS_TYPE_Z) {
        if (dol_efb_access_poke_z(efb, x, y, (u32)value))
            return;
        efb->last_z = (u32)value;
        efb->poke_z_count += 1u;
        if (!dol_efb_access_has_backing(efb))
            note_stub("poke-z");
        return;
    }
    efb->unimplemented_count += 1u;
    note_stub("unimplemented");
}

u64 dol_efb_access_mmio_read(DolEfbAccess* efb, u32 ea, u8 size) {
    return dol_efb_access_read(efb, ea, size);
}

void dol_efb_access_mmio_write(DolEfbAccess* efb, u32 ea, u8 size, u64 value) {
    dol_efb_access_write(efb, ea, size, value);
}
