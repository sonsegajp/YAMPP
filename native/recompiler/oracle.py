#!/usr/bin/env python3
"""
Oracle: replay a function from a recomp snapshot in Unicorn (independent, correct PPC
executor) and single-step it, logging PC + registers. Diff against our recomp's own
per-instruction trace to find the EXACT instruction where the emit diverges.

Usage: python oracle.py <func_addr_hex> [max_steps]
Reads build/snap_ram.bin (24MB) + build/snap_ctx.bin (Context) captured by the runtime.
"""
import struct
import sys

from unicorn import (Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN,
                     UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_UNMAPPED,
                     UC_HOOK_CODE, UcError)
from unicorn import ppc_const as P

sys.path.insert(0, "recompiler")
import ppc as ourppc
from symbols import parse_map

RAM_BASE = 0x80000000
RAM_SIZE = 0x01800000
MMIO_BASE = 0xCC000000
MMIO_SIZE = 0x00010000


def load_ctx(path):
    d = open(path, "rb").read()
    r = list(struct.unpack_from(">32I", d, 0))       # careful: struct is host-endian!
    # NOTE: the C struct was written in HOST (little-endian) order; re-read LE.
    r = list(struct.unpack_from("<32I", d, 0))
    cr, xer, lr, ctr, msr, fpscr = struct.unpack_from("<6I", d, 640)
    f = list(struct.unpack_from("<32d", d, 128))
    return r, cr, xer, lr, ctr, msr, fpscr, f


def main():
    func = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x800836E0
    max_steps = int(sys.argv[2]) if len(sys.argv) > 2 else 200000

    ram = open("build/snap_ram.bin", "rb").read()
    r, cr, xer, lr, ctr, msr, fpscr, f = load_ctx("build/snap_ctx.bin")

    funcs, _ = parse_map("extracted/maps/framework.map")
    fl = sorted(funcs, key=lambda x: x.vaddr)
    addrs = [x.vaddr for x in fl]
    import bisect
    def name(a):
        i = bisect.bisect_right(addrs, a) - 1
        if 0 <= i and fl[i].vaddr <= a < fl[i].vaddr + fl[i].size:
            return f"{fl[i].name}+0x{a-fl[i].vaddr:X}"
        return "?"

    uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
    uc.mem_map(RAM_BASE, RAM_SIZE)
    uc.mem_write(RAM_BASE, ram)
    uc.mem_map(MMIO_BASE, MMIO_SIZE)

    # --- MMIO hook: mirror the recomp's HLE (done-bits) so only CPU/emit diffs show ---
    def mmio_read(uc, access, addr, size, value, user):
        off = addr & 0x00FFFFFF
        v = 0
        if off == 0x500A: v = 0x20
        elif off == 0x5016: v = 1
        elif 0x2000 <= off < 0x2080: v = 0
        uc.mem_write(addr, v.to_bytes(size, "big"))
    def mmio_write(uc, access, addr, size, value, user):
        pass
    uc.hook_add(UC_HOOK_MEM_READ, mmio_read, begin=MMIO_BASE, end=MMIO_BASE + MMIO_SIZE)
    uc.hook_add(UC_HOOK_MEM_WRITE, mmio_write, begin=MMIO_BASE, end=MMIO_BASE + MMIO_SIZE)

    unmapped = []
    def on_unmapped(uc, access, addr, size, value, user):
        unmapped.append(addr)
        print(f"  UNMAPPED access 0x{addr:08X} size={size} (mapping a mirror page)")
        page = addr & ~0xFFF
        try:
            uc.mem_map(page, 0x1000)
        except UcError:
            pass
        return True
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    # set registers
    for i in range(32):
        uc.reg_write(getattr(P, f"UC_PPC_REG_{i}"), r[i])
    uc.reg_write(P.UC_PPC_REG_PC, func)
    uc.reg_write(P.UC_PPC_REG_LR, lr)
    uc.reg_write(P.UC_PPC_REG_CTR, ctr)
    uc.reg_write(P.UC_PPC_REG_XER, xer)
    uc.reg_write(P.UC_PPC_REG_CR, cr)
    uc.reg_write(P.UC_PPC_REG_MSR, (msr | 0x00002000) & 0xFFFFFFFF)  # FP available (our recomp always does FP)
    try:
        for i in range(32):
            uc.reg_write(getattr(P, f"UC_PPC_REG_FPR{i}"), struct.unpack("<Q", struct.pack("<d", f[i]))[0])
    except Exception:
        pass

    ret_lr = lr
    log = open("build/oracle_trace.txt", "w")
    steps = [0]
    deepest = [r[1]]
    def on_code(uc, address, size, user):
        steps[0] += 1
        sp = uc.reg_read(P.UC_PPC_REG_1)
        if sp < deepest[0]:
            deepest[0] = sp
        # detailed register log through the mutex region (the divergence)
        if (0x80305E94 <= address <= 0x80306040) or (0x802B3460 <= address <= 0x802B3700):
            log.write(f"{steps[0]:6} PC=0x{address:08X} r0=0x{uc.reg_read(P.UC_PPC_REG_0):08X} "
                      f"r3=0x{uc.reg_read(P.UC_PPC_REG_3):08X} r29=0x{uc.reg_read(P.UC_PPC_REG_29):08X} "
                      f"r30=0x{uc.reg_read(P.UC_PPC_REG_30):08X} cr=0x{uc.reg_read(P.UC_PPC_REG_CR):08X} {name(address)}\n")
        elif steps[0] <= 200:
            log.write(f"{steps[0]:6} PC=0x{address:08X} r1=0x{sp:08X} r3=0x{uc.reg_read(P.UC_PPC_REG_3):08X} {name(address)}\n")
        if address == ret_lr and steps[0] > 1:
            print(f"RETURNED at step {steps[0]} (PC hit entry LR 0x{ret_lr:08X})")
            uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, on_code)

    print(f"oracle: running {name(func)} @0x{func:08X}, entry r1=0x{r[1]:08X} lr=0x{lr:08X}")
    try:
        uc.emu_start(func, ret_lr, count=max_steps)
    except UcError as e:
        pc = uc.reg_read(P.UC_PPC_REG_PC)
        print(f"UC ERROR: {e} at PC=0x{pc:08X} ({name(pc)}) after {steps[0]} steps")
    log.close()
    print(f"steps={steps[0]} deepest_r1=0x{deepest[0]:08X} (used {r[1]-deepest[0]} bytes) "
          f"unmapped={len(unmapped)}")
    print("trace -> build/oracle_trace.txt")


if __name__ == "__main__":
    main()
