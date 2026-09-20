// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_MI_H
#define GXRUNTIME_MI_H

#include "core/types.h"

// Memory Interface MMIO window at 0xCC004000 (Dolphin Core/HW/MemoryInterface).
// Scratch register bank matching the Dolphin direct-mapped surface so live C2
// PE/MI tiny unknowns clear. Protection/IRQ side-effects are not modelled
// (reads return last write; no MI interrupt raise).

#define DOL_MI_BASE 0xCC004000u
#define DOL_MI_REGISTER_BYTES 0x80u

typedef struct DolMi {
    u16 words[DOL_MI_REGISTER_BYTES / 2u];
} DolMi;

void dol_mi_init(DolMi* mi);
bool dol_mi_mmio_read(DolMi* mi, u32 ea, u8 size, u64* value);
bool dol_mi_mmio_write(DolMi* mi, u32 ea, u8 size, u64 value);

#endif /* GXRUNTIME_MI_H */
