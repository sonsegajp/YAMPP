// SPDX-License-Identifier: GPL-3.0-or-later
#include "gxruntime/cp.h"

#include <string.h>

// CP MMIO surface (Dolphin CommandProcessor::RegisterMMIO). Metrics stub to 0
// (CLKS_PER_VTX_OUT = 4). FIFO pointer storage is retained for later E6 ring
// ingestion; reads/writes do not yet drive GP-FIFO decode.

static u16 load_u16_pair(u32 lo, u32 hi) {
    (void)hi;
    return (u16)(lo & 0xFFFFu);
}

static u16 load_u16_hi(u32 value) {
    return (u16)((value >> 16) & 0xFFFFu);
}

static void store_lo(u32* reg, u16 value, u16 wmask) {
    *reg = (*reg & 0xFFFF0000u) | (u32)(value & wmask);
}

static void store_hi(u32* reg, u16 value, u16 wmask) {
    *reg = (*reg & 0x0000FFFFu) | ((u32)(value & wmask) << 16);
}

static u16 status_value(const DolCp* cp) {
    // Idle bits always set: no CP-FIFO consumer is running yet, so the GPU is
    // always idle from the CPU's point of view (matches Dolphin init defaults).
    u16 s = (u16)(DOL_CP_STATUS_READ_IDLE | DOL_CP_STATUS_COMMAND_IDLE);
    if (cp != NULL)
        s |= (u16)(cp->status &
                   (DOL_CP_STATUS_OVERFLOW | DOL_CP_STATUS_UNDERFLOW |
                    DOL_CP_STATUS_BREAKPOINT));
    return s;
}

static u16 read_u16_reg(const DolCp* cp, u32 off) {
    switch (off) {
    case DOL_CP_STATUS_OFF:
        return status_value(cp);
    case DOL_CP_CTRL_OFF:
        return cp->ctrl;
    case DOL_CP_CLEAR_OFF:
        return 0; // write-only
    case DOL_CP_PERF_SELECT_OFF:
        return cp->perf_select;
    case DOL_CP_UNK_0A_OFF:
        return cp->unk_0a;
    case DOL_CP_FIFO_BASE_LO:
        return load_u16_pair(cp->fifo_base, 0);
    case DOL_CP_FIFO_BASE_HI:
        return load_u16_hi(cp->fifo_base);
    case DOL_CP_FIFO_END_LO:
        return load_u16_pair(cp->fifo_end, 0);
    case DOL_CP_FIFO_END_HI:
        return load_u16_hi(cp->fifo_end);
    case DOL_CP_FIFO_HI_WATERMARK_LO:
        return load_u16_pair(cp->fifo_hi_watermark, 0);
    case DOL_CP_FIFO_HI_WATERMARK_HI:
        return load_u16_hi(cp->fifo_hi_watermark);
    case DOL_CP_FIFO_LO_WATERMARK_LO:
        return load_u16_pair(cp->fifo_lo_watermark, 0);
    case DOL_CP_FIFO_LO_WATERMARK_HI:
        return load_u16_hi(cp->fifo_lo_watermark);
    case DOL_CP_FIFO_RW_DISTANCE_LO:
        return load_u16_pair(cp->fifo_rw_distance, 0);
    case DOL_CP_FIFO_RW_DISTANCE_HI:
        return load_u16_hi(cp->fifo_rw_distance);
    case DOL_CP_FIFO_WRITE_POINTER_LO:
        return load_u16_pair(cp->fifo_write_pointer, 0);
    case DOL_CP_FIFO_WRITE_POINTER_HI:
        return load_u16_hi(cp->fifo_write_pointer);
    case DOL_CP_FIFO_READ_POINTER_LO:
        return load_u16_pair(cp->fifo_read_pointer, 0);
    case DOL_CP_FIFO_READ_POINTER_HI:
        return load_u16_hi(cp->fifo_read_pointer);
    case DOL_CP_FIFO_BP_LO:
        return load_u16_pair(cp->fifo_breakpoint, 0);
    case DOL_CP_FIFO_BP_HI:
        return load_u16_hi(cp->fifo_breakpoint);
    case DOL_CP_CLKS_PER_VTX_OUT:
        return 4; // Dolphin metrics stub
    case DOL_CP_XF_RASBUSY_L:
    case DOL_CP_XF_RASBUSY_H:
    case DOL_CP_XF_CLKS_L:
    case DOL_CP_XF_CLKS_H:
    case DOL_CP_XF_WAIT_IN_L:
    case DOL_CP_XF_WAIT_IN_H:
    case DOL_CP_XF_WAIT_OUT_L:
    case DOL_CP_XF_WAIT_OUT_H:
    case DOL_CP_VCACHE_METRIC_CHECK_L:
    case DOL_CP_VCACHE_METRIC_CHECK_H:
    case DOL_CP_VCACHE_METRIC_MISS_L:
    case DOL_CP_VCACHE_METRIC_MISS_H:
    case DOL_CP_VCACHE_METRIC_STALL_L:
    case DOL_CP_VCACHE_METRIC_STALL_H:
    case DOL_CP_CLKS_PER_VTX_IN_L:
    case DOL_CP_CLKS_PER_VTX_IN_H:
        return 0;
    default:
        return 0;
    }
}

static void write_u16_reg(DolCp* cp, u32 off, u16 value) {
    switch (off) {
    case DOL_CP_STATUS_OFF:
        return; // read-only
    case DOL_CP_CTRL_OFF:
        cp->ctrl = (u16)(value & DOL_CP_CTRL_WRITABLE_MASK);
        return;
    case DOL_CP_CLEAR_OFF:
        cp->clear = value;
        if ((value & DOL_CP_CLEAR_OVERFLOW) != 0u)
            cp->status = (u16)(cp->status & ~DOL_CP_STATUS_OVERFLOW);
        if ((value & DOL_CP_CLEAR_UNDERFLOW) != 0u)
            cp->status = (u16)(cp->status & ~DOL_CP_STATUS_UNDERFLOW);
        // CLEAR_METRICS: metrics are already stubbed to constants.
        return;
    case DOL_CP_PERF_SELECT_OFF:
        cp->perf_select = (u16)(value & 0x0007u);
        return;
    case DOL_CP_UNK_0A_OFF:
        cp->unk_0a = (u16)(value & 0x00FFu);
        return;
    case DOL_CP_FIFO_BASE_LO:
        store_lo(&cp->fifo_base, value, DOL_CP_FIFO_LO_WMASK);
        return;
    case DOL_CP_FIFO_BASE_HI:
        store_hi(&cp->fifo_base, value, DOL_CP_FIFO_HI_WMASK);
        return;
    case DOL_CP_FIFO_END_LO:
        store_lo(&cp->fifo_end, value, DOL_CP_FIFO_LO_WMASK);
        return;
    case DOL_CP_FIFO_END_HI:
        store_hi(&cp->fifo_end, value, DOL_CP_FIFO_HI_WMASK);
        return;
    case DOL_CP_FIFO_HI_WATERMARK_LO:
        store_lo(&cp->fifo_hi_watermark, value, DOL_CP_FIFO_LO_WMASK);
        return;
    case DOL_CP_FIFO_HI_WATERMARK_HI:
        store_hi(&cp->fifo_hi_watermark, value, DOL_CP_FIFO_HI_WMASK);
        return;
    case DOL_CP_FIFO_LO_WATERMARK_LO:
        store_lo(&cp->fifo_lo_watermark, value, DOL_CP_FIFO_LO_WMASK);
        return;
    case DOL_CP_FIFO_LO_WATERMARK_HI:
        store_hi(&cp->fifo_lo_watermark, value, DOL_CP_FIFO_HI_WMASK);
        return;
    case DOL_CP_FIFO_RW_DISTANCE_LO:
        store_lo(&cp->fifo_rw_distance, value, DOL_CP_FIFO_LO_WMASK);
        return;
    case DOL_CP_FIFO_RW_DISTANCE_HI:
        store_hi(&cp->fifo_rw_distance, value, DOL_CP_FIFO_HI_WMASK);
        return;
    case DOL_CP_FIFO_WRITE_POINTER_LO:
        store_lo(&cp->fifo_write_pointer, value, DOL_CP_FIFO_LO_WMASK);
        return;
    case DOL_CP_FIFO_WRITE_POINTER_HI:
        store_hi(&cp->fifo_write_pointer, value, DOL_CP_FIFO_HI_WMASK);
        return;
    case DOL_CP_FIFO_READ_POINTER_LO:
        store_lo(&cp->fifo_read_pointer, value, DOL_CP_FIFO_LO_WMASK);
        return;
    case DOL_CP_FIFO_READ_POINTER_HI:
        store_hi(&cp->fifo_read_pointer, value, DOL_CP_FIFO_HI_WMASK);
        return;
    case DOL_CP_FIFO_BP_LO:
        store_lo(&cp->fifo_breakpoint, value, DOL_CP_FIFO_LO_WMASK);
        return;
    case DOL_CP_FIFO_BP_HI:
        store_hi(&cp->fifo_breakpoint, value, DOL_CP_FIFO_HI_WMASK);
        return;
    // Metrics + status-side reads are write-invalid (Dolphin InvalidWrite).
    default:
        return;
    }
}

void dol_cp_init(DolCp* cp) {
    if (cp == NULL)
        return;
    memset(cp, 0, sizeof(*cp));
    // Dolphin Init: CommandIdle=1, ReadIdle=1.
    cp->status = (u16)(DOL_CP_STATUS_READ_IDLE | DOL_CP_STATUS_COMMAND_IDLE);
}

bool dol_cp_mmio_contains(u32 ea) {
    return ea >= DOL_CP_BASE && ea < DOL_CP_BASE + DOL_CP_REGISTER_BYTES;
}

u64 dol_cp_mmio_read(const DolCp* cp, u32 ea, u8 size) {
    if (cp == NULL || size == 0u || !dol_cp_mmio_contains(ea))
        return 0;
    const u32 off = ea - DOL_CP_BASE;
    if (off + size > DOL_CP_REGISTER_BYTES)
        return 0;

    // Hardware is natively 16-bit; synthesise multi-byte reads as big-endian
    // concatenation of successive u16 regs (and byte slices of a u16).
    if (size == 1u) {
        const u16 reg = read_u16_reg(cp, off & ~1u);
        if ((off & 1u) == 0u)
            return (u64)((reg >> 8) & 0xFFu);
        return (u64)(reg & 0xFFu);
    }
    if (size == 2u) {
        if ((off & 1u) != 0u)
            return 0;
        return (u64)read_u16_reg(cp, off);
    }
    if (size == 4u) {
        if ((off & 1u) != 0u)
            return 0;
        const u16 hi = read_u16_reg(cp, off);
        const u16 lo = read_u16_reg(cp, off + 2u);
        return ((u64)hi << 16) | (u64)lo;
    }
    return 0;
}

void dol_cp_mmio_write(DolCp* cp, u32 ea, u8 size, u64 value) {
    if (cp == NULL || size == 0u || !dol_cp_mmio_contains(ea))
        return;
    const u32 off = ea - DOL_CP_BASE;
    if (off + size > DOL_CP_REGISTER_BYTES)
        return;

    if (size == 1u) {
        const u32 reg_off = off & ~1u;
        u16 reg = read_u16_reg(cp, reg_off);
        if ((off & 1u) == 0u)
            reg = (u16)((reg & 0x00FFu) | ((u16)(value & 0xFFu) << 8));
        else
            reg = (u16)((reg & 0xFF00u) | (u16)(value & 0xFFu));
        write_u16_reg(cp, reg_off, reg);
        return;
    }
    if (size == 2u) {
        if ((off & 1u) != 0u)
            return;
        write_u16_reg(cp, off, (u16)value);
        return;
    }
    if (size == 4u) {
        if ((off & 1u) != 0u)
            return;
        write_u16_reg(cp, off, (u16)((value >> 16) & 0xFFFFu));
        write_u16_reg(cp, off + 2u, (u16)(value & 0xFFFFu));
    }
}
