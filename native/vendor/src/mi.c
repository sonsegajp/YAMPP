// SPDX-License-Identifier: GPL-3.0-or-later
#include "gxruntime/mi.h"

#include <string.h>

void dol_mi_init(DolMi* mi) {
    if (mi != NULL)
        memset(mi, 0, sizeof(*mi));
}

bool dol_mi_mmio_read(DolMi* mi, u32 ea, u8 size, u64* value) {
    if (mi == NULL || value == NULL || ea < DOL_MI_BASE)
        return false;
    u32 off = ea - DOL_MI_BASE;
    if (off >= DOL_MI_REGISTER_BYTES || size == 0u ||
        (u32)size > DOL_MI_REGISTER_BYTES - off)
        return false;
    if (size == 4u && (off & 1u) == 0u) {
        u32 i = off / 2u;
        *value = (u64)mi->words[i] | ((u64)mi->words[i + 1u] << 16);
        return true;
    }
    if (size == 2u && (off & 1u) == 0u) {
        *value = (u64)mi->words[off / 2u];
        return true;
    }
    if (size == 1u) {
        u16 w = mi->words[off / 2u];
        *value = (u64)((off & 1u) ? (w >> 8) : (w & 0xFFu));
        return true;
    }
    return false;
}

bool dol_mi_mmio_write(DolMi* mi, u32 ea, u8 size, u64 value) {
    if (mi == NULL || ea < DOL_MI_BASE)
        return false;
    u32 off = ea - DOL_MI_BASE;
    if (off >= DOL_MI_REGISTER_BYTES || size == 0u ||
        (u32)size > DOL_MI_REGISTER_BYTES - off)
        return false;
    if (size == 4u && (off & 1u) == 0u) {
        u32 i = off / 2u;
        mi->words[i] = (u16)(value & 0xFFFFu);
        mi->words[i + 1u] = (u16)((value >> 16) & 0xFFFFu);
        return true;
    }
    if (size == 2u && (off & 1u) == 0u) {
        mi->words[off / 2u] = (u16)value;
        return true;
    }
    if (size == 1u) {
        u32 i = off / 2u;
        u16 w = mi->words[i];
        if (off & 1u)
            w = (u16)((w & 0x00FFu) | ((u16)(value & 0xFFu) << 8));
        else
            w = (u16)((w & 0xFF00u) | (u16)(value & 0xFFu));
        mi->words[i] = w;
        return true;
    }
    return false;
}
