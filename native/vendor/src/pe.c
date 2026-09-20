// SPDX-License-Identifier: GPL-3.0-or-later
#include "gxruntime/pe.h"

#include "gxruntime/efb_access.h"

#include <string.h>

void dol_pe_init(DolPe* pe) {
    if (pe != NULL)
        memset(pe, 0, sizeof(*pe));
}

void dol_pe_link_bbox(DolPe* pe, const DolEfbAccess* efb) {
    if (pe != NULL)
        pe->bbox_efb = efb;
}

// Word index for even PE offsets. CTRL (0x0A) is not stored here.
static bool pe_word_index(u32 off, u32* idx) {
    if ((off & 1u) != 0u || off >= DOL_PE_REGISTER_BYTES || off == DOL_PE_CTRL_OFF)
        return false;
    *idx = off / 2u;
    return true;
}

// Live bbox from linked efb when reading PE_BBOX_LEFT..BOTTOM (0x10..0x16).
static bool pe_bbox_live(const DolPe* pe, u32 off, u16* out) {
    if (pe == NULL || pe->bbox_efb == NULL || out == NULL)
        return false;
    if (off == DOL_PE_BBOX_LEFT_OFF) {
        *out = dol_efb_bbox_get(pe->bbox_efb, DOL_EFB_BBOX_LEFT);
        return true;
    }
    if (off == DOL_PE_BBOX_RIGHT_OFF) {
        *out = dol_efb_bbox_get(pe->bbox_efb, DOL_EFB_BBOX_RIGHT);
        return true;
    }
    if (off == DOL_PE_BBOX_TOP_OFF) {
        *out = dol_efb_bbox_get(pe->bbox_efb, DOL_EFB_BBOX_TOP);
        return true;
    }
    if (off == DOL_PE_BBOX_BOTTOM_OFF) {
        *out = dol_efb_bbox_get(pe->bbox_efb, DOL_EFB_BBOX_BOTTOM);
        return true;
    }
    return false;
}

bool dol_pe_mmio_read(DolPe* pe, u32 ea, u8 size, u64* value) {
    if (pe == NULL || value == NULL || ea < DOL_PE_BASE)
        return false;
    u32 off = ea - DOL_PE_BASE;
    if (off == DOL_PE_CTRL_OFF || off + size > DOL_PE_REGISTER_BYTES)
        return false;
    if (size == 4u && (off & 1u) == 0u) {
        u16 b0 = 0, b1 = 0;
        u32 i0, i1;
        if (!pe_word_index(off, &i0) || !pe_word_index(off + 2u, &i1))
            return false;
        if (!pe_bbox_live(pe, off, &b0))
            b0 = pe->words[i0];
        if (!pe_bbox_live(pe, off + 2u, &b1))
            b1 = pe->words[i1];
        *value = (u64)b0 | ((u64)b1 << 16);
        return true;
    }
    if (size == 2u && (off & 1u) == 0u) {
        u16 b = 0;
        u32 i;
        if (!pe_word_index(off, &i))
            return false;
        if (pe_bbox_live(pe, off, &b))
            *value = (u64)b;
        else
            *value = (u64)pe->words[i];
        return true;
    }
    if (size == 1u) {
        u16 w = 0;
        u32 i;
        if (!pe_word_index(off & ~1u, &i))
            return false;
        if (!pe_bbox_live(pe, off & ~1u, &w))
            w = pe->words[i];
        *value = (u64)((off & 1u) ? (w >> 8) : (w & 0xFFu));
        return true;
    }
    return false;
}

bool dol_pe_mmio_write(DolPe* pe, u32 ea, u8 size, u64 value) {
    if (pe == NULL || ea < DOL_PE_BASE)
        return false;
    u32 off = ea - DOL_PE_BASE;
    if (off == DOL_PE_CTRL_OFF || off + size > DOL_PE_REGISTER_BYTES)
        return false;
    if (size == 4u && (off & 1u) == 0u) {
        u32 i0, i1;
        if (!pe_word_index(off, &i0) || !pe_word_index(off + 2u, &i1))
            return false;
        pe->words[i0] = (u16)(value & 0xFFFFu);
        pe->words[i1] = (u16)((value >> 16) & 0xFFFFu);
        return true;
    }
    if (size == 2u && (off & 1u) == 0u) {
        u32 i;
        if (!pe_word_index(off, &i))
            return false;
        pe->words[i] = (u16)value;
        return true;
    }
    if (size == 1u) {
        u32 i;
        if (!pe_word_index(off & ~1u, &i))
            return false;
        u16 w = pe->words[i];
        if (off & 1u)
            w = (u16)((w & 0x00FFu) | ((u16)(value & 0xFFu) << 8));
        else
            w = (u16)((w & 0xFF00u) | (u16)(value & 0xFFu));
        pe->words[i] = w;
        return true;
    }
    return false;
}
