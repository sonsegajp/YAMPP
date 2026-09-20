// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GXRUNTIME_CP_H
#define GXRUNTIME_CP_H

#include "core/types.h"

// GameCube Command Processor (CP) MMIO window at 0xCC000000.
// Grounded in Dolphin VideoCommon/CommandProcessor.{h,cpp}. This is the
// MMIO *surface* only — full CP-FIFO ring ingestion (E6) is out of scope;
// status/FIFO pointer/metrics/clear regs are modelled so live tripwires stop
// reporting unknown-read/write on the CP block.

#define DOL_CP_BASE 0xCC000000u
#define DOL_CP_REGISTER_BYTES 0x80u

// Internal offsets (u16-addressed on hardware; we accept 1/2/4-byte accesses).
#define DOL_CP_STATUS_OFF 0x00u
#define DOL_CP_CTRL_OFF 0x02u
#define DOL_CP_CLEAR_OFF 0x04u
#define DOL_CP_PERF_SELECT_OFF 0x06u
#define DOL_CP_UNK_0A_OFF 0x0Au
#define DOL_CP_FIFO_BASE_LO 0x20u
#define DOL_CP_FIFO_BASE_HI 0x22u
#define DOL_CP_FIFO_END_LO 0x24u
#define DOL_CP_FIFO_END_HI 0x26u
#define DOL_CP_FIFO_HI_WATERMARK_LO 0x28u
#define DOL_CP_FIFO_HI_WATERMARK_HI 0x2Au
#define DOL_CP_FIFO_LO_WATERMARK_LO 0x2Cu
#define DOL_CP_FIFO_LO_WATERMARK_HI 0x2Eu
#define DOL_CP_FIFO_RW_DISTANCE_LO 0x30u
#define DOL_CP_FIFO_RW_DISTANCE_HI 0x32u
#define DOL_CP_FIFO_WRITE_POINTER_LO 0x34u
#define DOL_CP_FIFO_WRITE_POINTER_HI 0x36u
#define DOL_CP_FIFO_READ_POINTER_LO 0x38u
#define DOL_CP_FIFO_READ_POINTER_HI 0x3Au
#define DOL_CP_FIFO_BP_LO 0x3Cu
#define DOL_CP_FIFO_BP_HI 0x3Eu
#define DOL_CP_XF_RASBUSY_L 0x40u
#define DOL_CP_XF_RASBUSY_H 0x42u
#define DOL_CP_XF_CLKS_L 0x44u
#define DOL_CP_XF_CLKS_H 0x46u
#define DOL_CP_XF_WAIT_IN_L 0x48u
#define DOL_CP_XF_WAIT_IN_H 0x4Au
#define DOL_CP_XF_WAIT_OUT_L 0x4Cu
#define DOL_CP_XF_WAIT_OUT_H 0x4Eu
#define DOL_CP_VCACHE_METRIC_CHECK_L 0x50u
#define DOL_CP_VCACHE_METRIC_CHECK_H 0x52u
#define DOL_CP_VCACHE_METRIC_MISS_L 0x54u
#define DOL_CP_VCACHE_METRIC_MISS_H 0x56u
#define DOL_CP_VCACHE_METRIC_STALL_L 0x58u
#define DOL_CP_VCACHE_METRIC_STALL_H 0x5Au
#define DOL_CP_CLKS_PER_VTX_IN_L 0x60u
#define DOL_CP_CLKS_PER_VTX_IN_H 0x62u
#define DOL_CP_CLKS_PER_VTX_OUT 0x64u

// Status register bits (UCPStatusReg).
#define DOL_CP_STATUS_OVERFLOW 0x0001u
#define DOL_CP_STATUS_UNDERFLOW 0x0002u
#define DOL_CP_STATUS_READ_IDLE 0x0004u
#define DOL_CP_STATUS_COMMAND_IDLE 0x0008u
#define DOL_CP_STATUS_BREAKPOINT 0x0010u

// Control register bits (UCPCtrlReg); only low 6 bits are writable.
#define DOL_CP_CTRL_GP_READ_ENABLE 0x0001u
#define DOL_CP_CTRL_BP_ENABLE 0x0002u
#define DOL_CP_CTRL_FIFO_OVERFLOW_INT 0x0004u
#define DOL_CP_CTRL_FIFO_UNDERFLOW_INT 0x0008u
#define DOL_CP_CTRL_GP_LINK_ENABLE 0x0010u
#define DOL_CP_CTRL_BP_INT 0x0020u
#define DOL_CP_CTRL_WRITABLE_MASK 0x003Fu

// Clear register bits (UCPClearReg); write-only, reads as 0.
#define DOL_CP_CLEAR_OVERFLOW 0x0001u
#define DOL_CP_CLEAR_UNDERFLOW 0x0002u
#define DOL_CP_CLEAR_METRICS 0x0004u

// GCN physical-address mask for FIFO pointer HI halves (Dolphin
// GetPhysicalAddressMask(is_wii=false) >> 16).
#define DOL_CP_FIFO_HI_WMASK 0x03FFu
#define DOL_CP_FIFO_LO_WMASK 0xFFE0u

typedef struct DolCp {
    u16 status;     // read-only computed; idle bits set at init
    u16 ctrl;
    u16 clear;      // last written clear value (not readable)
    u16 perf_select;
    u16 unk_0a;
    u32 fifo_base;
    u32 fifo_end;
    u32 fifo_hi_watermark;
    u32 fifo_lo_watermark;
    u32 fifo_rw_distance;
    u32 fifo_write_pointer;
    u32 fifo_read_pointer;
    u32 fifo_breakpoint;
} DolCp;

void dol_cp_init(DolCp* cp);
bool dol_cp_mmio_contains(u32 ea);
u64 dol_cp_mmio_read(const DolCp* cp, u32 ea, u8 size);
void dol_cp_mmio_write(DolCp* cp, u32 ea, u8 size, u64 value);

#endif /* GXRUNTIME_CP_H */
