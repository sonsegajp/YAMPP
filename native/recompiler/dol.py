#!/usr/bin/env python3
"""GameCube DOL loader: maps virtual addresses <-> file bytes.

DOL header (big-endian):
  0x00  7 text section file offsets
  0x1C  11 data section file offsets
  0x48  7 text section load addresses
  0x64  11 data section load addresses
  0x90  7 text section sizes
  0xAC  11 data section sizes
  0xD8  bss load address
  0xDC  bss size
  0xE0  entry point
"""
import struct


class Section:
    def __init__(self, index, kind, file_off, addr, size):
        self.index = index
        self.kind = kind          # 'text' or 'data'
        self.file_off = file_off
        self.addr = addr
        self.size = size

    def contains(self, vaddr):
        return self.addr <= vaddr < self.addr + self.size

    def __repr__(self):
        return (f"<{self.kind}{self.index} file=0x{self.file_off:X} "
                f"addr=0x{self.addr:08X} size=0x{self.size:X}>")


class Dol:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        self.sections = []
        for i in range(7):
            off = struct.unpack_from(">I", d, 0x00 + i * 4)[0]
            addr = struct.unpack_from(">I", d, 0x48 + i * 4)[0]
            size = struct.unpack_from(">I", d, 0x90 + i * 4)[0]
            if off and size:
                self.sections.append(Section(i, "text", off, addr, size))
        for i in range(11):
            off = struct.unpack_from(">I", d, 0x1C + i * 4)[0]
            addr = struct.unpack_from(">I", d, 0x64 + i * 4)[0]
            size = struct.unpack_from(">I", d, 0xAC + i * 4)[0]
            if off and size:
                self.sections.append(Section(i, "data", off, addr, size))
        self.bss_addr = struct.unpack_from(">I", d, 0xD8)[0]
        self.bss_size = struct.unpack_from(">I", d, 0xDC)[0]
        self.entry = struct.unpack_from(">I", d, 0xE0)[0]

    def text_sections(self):
        return [s for s in self.sections if s.kind == "text"]

    def section_at(self, vaddr):
        for s in self.sections:
            if s.contains(vaddr):
                return s
        return None

    def read(self, vaddr, length):
        s = self.section_at(vaddr)
        if not s:
            raise KeyError(f"vaddr 0x{vaddr:08X} not mapped")
        fo = s.file_off + (vaddr - s.addr)
        return self.data[fo:fo + length]

    def word(self, vaddr):
        return struct.unpack(">I", self.read(vaddr, 4))[0]


if __name__ == "__main__":
    import sys
    dol = Dol(sys.argv[1] if len(sys.argv) > 1 else "extracted/main.dol")
    print(f"entry 0x{dol.entry:08X}  bss 0x{dol.bss_addr:08X}+0x{dol.bss_size:X}")
    for s in dol.sections:
        print("  ", s)
