// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_PE_H
#define GXRUNTIME_PE_H

#include "core/types.h"

// Pixel Engine MMIO window at 0xCC001000 (Dolphin VideoCommon/PixelEngine).
// Models the u16 register surface so live C2 tripwires stop reporting
// unknown-read/write on PE config regs. PE_CTRL (0x0A) remains owned by
// interrupts.c (DOL_PE_INTERRUPT_STATUS); pe_*_mmio returns false for that
// offset so the interrupt region claims it.

#define DOL_PE_BASE 0xCC001000u
#define DOL_PE_REGISTER_BYTES 0x20u

#define DOL_PE_ZCONF_OFF 0x00u
#define DOL_PE_ALPHACONF_OFF 0x02u
#define DOL_PE_DSTALPHACONF_OFF 0x04u
#define DOL_PE_ALPHAMODE_OFF 0x06u
#define DOL_PE_ALPHAREAD_OFF 0x08u
#define DOL_PE_CTRL_OFF 0x0Au /* owned by interrupts; not stored here */
#define DOL_PE_TOKEN_OFF 0x0Eu
#define DOL_PE_BBOX_LEFT_OFF 0x10u
#define DOL_PE_BBOX_RIGHT_OFF 0x12u
#define DOL_PE_BBOX_TOP_OFF 0x14u
#define DOL_PE_BBOX_BOTTOM_OFF 0x16u

struct DolEfbAccess; /* forward — bbox live source */

typedef struct DolPe {
    /* u16 words indexed by offset/2; index for CTRL (0x0A) is unused. */
    u16 words[DOL_PE_REGISTER_BYTES / 2u];
    /* When set, PE_BBOX_LEFT..BOTTOM reads pull live extents from efb bbox. */
    const struct DolEfbAccess* bbox_efb;
} DolPe;

void dol_pe_init(DolPe* pe);
/* Link PE bbox MMIO (0x10..0x16) to DolEfbAccess bbox substrate. NULL unlinks. */
void dol_pe_link_bbox(DolPe* pe, const struct DolEfbAccess* efb);
bool dol_pe_mmio_read(DolPe* pe, u32 ea, u8 size, u64* value);
bool dol_pe_mmio_write(DolPe* pe, u32 ea, u8 size, u64 value);

#endif /* GXRUNTIME_PE_H */
