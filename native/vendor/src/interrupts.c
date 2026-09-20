// SPDX-License-Identifier: GPL-3.0-or-later
#include "gxruntime/interrupts.h"

#include "gxruntime/platform.h"

#include <string.h>

static u64 read_reg(const u8* regs, u32 off, u8 size) {
    u64 value = 0;
    for (u8 i = 0; i < size; i++)
        value = (value << 8) | regs[off + i];
    return value;
}

static void write_reg(u8* regs, u32 off, u8 size, u64 value) {
    for (u8 i = 0; i < size; i++)
        regs[off + (size - 1u - i)] = (u8)(value >> (8u * i));
}

static bool range_contains(u32 base, u32 length, u32 ea) {
    return ea >= base && ea < base + length;
}

static u16 vi_u16(const u8* regs, u32 off) {
    return (u16)(((u16)regs[off] << 8) | regs[off + 1u]);
}

static void vi_store_u16(u8* regs, u32 off, u16 value) {
    regs[off] = (u8)(value >> 8);
    regs[off + 1u] = (u8)value;
}

/* UVIFBInfoRegister Hex = Lo | (Hi<<16): FBB:24 XOFF:4 POFF:1 CLRPOFF:3. */
static u32 vi_fb_hex(const u8* regs, u32 hi_off, u32 lo_off) {
    return ((u32)vi_u16(regs, hi_off) << 16) | (u32)vi_u16(regs, lo_off);
}

static u32 vi_decode_fbb(u32 hex) {
    const u32 fbb = hex & 0x00FFFFFFu;
    if (((hex >> 28) & 1u) != 0u)
        return fbb << 5;
    return fbb;
}

/* Dolphin: write to *_HI with CLRPOFF (bits 29-31) clears POFF (bit 28). */
static void vi_apply_clrpoff(u8* regs, u32 hi_off) {
    u16 hi = vi_u16(regs, hi_off);
    if ((hi & 0xE000u) != 0u) {
        hi = (u16)(hi & (u16)~0x1000u);
        vi_store_u16(regs, hi_off, hi);
    }
}

static bool vi_write_touches(u32 off, u8 size, u32 target_off, u32 target_bytes) {
    const u32 end = off + (u32)size;
    const u32 t_end = target_off + target_bytes;
    return off < t_end && end > target_off;
}

/* E7: TFBL MMIO selects next XFB (and latches current). Pure-MMIO games have
 * no VIFlush; SDK path re-writes the same address on flush — safe/idempotent. */
static void vi_tfbl_side_effects(DolInterrupts* interrupts) {
    const u32 addr =
        dol_interrupts_vi_tfbl_address(interrupts) & 0x1FFFFFFFu;
    if (addr == 0u)
        return;
    dol_platform_set_next_xfb(addr);
    dol_platform_flush_xfb();
}

static void sync_vi(DolInterrupts* interrupts) {
    if (interrupts == NULL)
        return;
    const u8 di0 = interrupts->vi_regs[DOL_VI_DI0_STATUS_BYTE];
    dol_interrupts_set_source(
        interrupts, DOL_PI_CAUSE_VI, (di0 & DOL_VI_DI0_STATUS_BIT) != 0u &&
                                         (di0 & DOL_VI_DI0_MASK_BIT) != 0u);
}

void dol_interrupts_init(DolInterrupts* interrupts) {
    if (interrupts == NULL)
        return;
    memset(interrupts, 0, sizeof(*interrupts));
    // The physical reset switch is released by default. Dolphin models this as
    // INT_CAUSE_RST_BUTTON set (1 = unpressed); it is guest-visible state, not
    // a periodic device interrupt.
    interrupts->pi_cause = DOL_PI_CAUSE_RST_BUTTON;
}

bool dol_interrupts_mmio_contains(u32 ea) {
    return range_contains(DOL_VI_BASE, DOL_VI_REGISTER_BYTES, ea) ||
           range_contains(DOL_PI_BASE, 0x40u, ea) ||
           ea == DOL_PE_INTERRUPT_STATUS;
}

u64 dol_interrupts_mmio_read(DolInterrupts* interrupts, u32 ea, u8 size) {
    if (interrupts == NULL || size == 0)
        return 0;

    sync_vi(interrupts);
    if (ea == DOL_PI_INTERRUPT_CAUSE)
        return interrupts->pi_cause;
    if (ea == DOL_PI_INTERRUPT_MASK)
        return interrupts->pi_mask;
    if (ea == DOL_PE_INTERRUPT_STATUS)
        return (interrupts->pi_cause & DOL_PI_CAUSE_PE_FINISH)
                   ? DOL_PE_FINISH_ACK_BIT
                   : 0u;

    if (range_contains(DOL_VI_BASE, DOL_VI_REGISTER_BYTES, ea)) {
        const u32 off = ea - DOL_VI_BASE;
        if (off + size > DOL_VI_REGISTER_BYTES)
            return 0;
        return read_reg(interrupts->vi_regs, off, size);
    }

    return 0;
}

void dol_interrupts_mmio_write(DolInterrupts* interrupts, u32 ea, u8 size,
                               u64 value) {
    if (interrupts == NULL || size == 0)
        return;

    if (ea == DOL_PI_INTERRUPT_MASK) {
        interrupts->pi_mask = (u32)value;
        return;
    }
    if (ea == DOL_PI_INTERRUPT_CAUSE) {
        // PI interrupt cause is write-one-to-clear in Dolphin's hardware model.
        interrupts->pi_cause &= ~(u32)value;
        return;
    }
    if (ea == DOL_PE_INTERRUPT_STATUS) {
        if ((value & DOL_PE_FINISH_ACK_BIT) != 0u)
            dol_interrupts_set_source(interrupts, DOL_PI_CAUSE_PE_FINISH,
                                      false);
        return;
    }

    if (range_contains(DOL_VI_BASE, DOL_VI_REGISTER_BYTES, ea)) {
        const u32 off = ea - DOL_VI_BASE;
        if (off + size > DOL_VI_REGISTER_BYTES)
            return;
        write_reg(interrupts->vi_regs, off, size, value);
        /* CLRPOFF on TFBL/BFBL HI writes (Dolphin VideoInterface). */
        if (vi_write_touches(off, size, DOL_VI_TFBL_HI_OFF, 2u))
            vi_apply_clrpoff(interrupts->vi_regs, DOL_VI_TFBL_HI_OFF);
        if (vi_write_touches(off, size, DOL_VI_BFBL_HI_OFF, 2u))
            vi_apply_clrpoff(interrupts->vi_regs, DOL_VI_BFBL_HI_OFF);
        /* Latch on TFBL HI (or any write covering it). SDK writes LO then HI. */
        if (vi_write_touches(off, size, DOL_VI_TFBL_HI_OFF, 2u))
            vi_tfbl_side_effects(interrupts);
        sync_vi(interrupts);
    }
}

u32 dol_interrupts_vi_tfbl_address(const DolInterrupts* interrupts) {
    if (interrupts == NULL)
        return 0u;
    return vi_decode_fbb(
        vi_fb_hex(interrupts->vi_regs, DOL_VI_TFBL_HI_OFF, DOL_VI_TFBL_LO_OFF));
}

u32 dol_interrupts_vi_bfbl_address(const DolInterrupts* interrupts) {
    if (interrupts == NULL)
        return 0u;
    /* Bottom field inherits POFF from top (Dolphin GetXFBAddressBottom). */
    const u32 top = vi_fb_hex(interrupts->vi_regs, DOL_VI_TFBL_HI_OFF,
                              DOL_VI_TFBL_LO_OFF);
    const u32 bot = vi_fb_hex(interrupts->vi_regs, DOL_VI_BFBL_HI_OFF,
                              DOL_VI_BFBL_LO_OFF);
    const u32 fbb = bot & 0x00FFFFFFu;
    if (((top >> 28) & 1u) != 0u)
        return fbb << 5;
    return fbb;
}

u32 dol_interrupts_pi_cause(const DolInterrupts* interrupts) {
    return interrupts != NULL ? interrupts->pi_cause : 0u;
}

u32 dol_interrupts_pi_mask(const DolInterrupts* interrupts) {
    return interrupts != NULL ? interrupts->pi_mask : 0u;
}

bool dol_interrupts_external_pending(const DolInterrupts* interrupts) {
    return interrupts != NULL &&
           ((interrupts->pi_cause & interrupts->pi_mask) != 0u);
}

void dol_interrupts_set_source(DolInterrupts* interrupts, u32 cause_mask,
                               bool pending) {
    if (interrupts == NULL)
        return;
    if (pending)
        interrupts->pi_cause |= cause_mask;
    else
        interrupts->pi_cause &= ~cause_mask;
}

void dol_interrupts_assert_vi_retrace(DolInterrupts* interrupts) {
    if (interrupts == NULL)
        return;
    interrupts->vi_regs[DOL_VI_DI0_STATUS_BYTE] |= DOL_VI_DI0_STATUS_BIT;
    sync_vi(interrupts);
}

void dol_interrupts_commit_pe_finish(DolInterrupts* interrupts) {
    dol_interrupts_set_source(interrupts, DOL_PI_CAUSE_PE_FINISH, true);
}
