#!/usr/bin/env python3
"""
Parse a CodeWarrior linker map (framework.map) into a function table.

CodeWarrior '<section> section layout' rows look like:
    00000068 000064 80005748  4 CheckHeap1__9HeapCheckFv 	m_Do_main.o
     ^fileoff ^size  ^vaddr    ^align ^symbol            \t ^object

We keep executable-section symbols with size > 0 as our function table:
each function is exactly [vaddr, vaddr + size).
"""
import re
import sys

# fileoff(8) size(4-8) vaddr(8) align(int) name <tab-or-2+spaces> object
_ROW = re.compile(
    r"^\s*([0-9a-fA-F]{8})\s+([0-9a-fA-F]{4,8})\s+([0-9a-fA-F]{8})\s+(\d+)\s+(.+?)\s{2,}(\S.*?)\s*$"
)
_SECTION = re.compile(r"^(\S+) section layout\s*$")

# Sections whose symbols are executable code.
EXEC_SECTIONS = {".text", ".init"}


class Symbol:
    __slots__ = ("vaddr", "size", "name", "obj", "section")

    def __init__(self, vaddr, size, name, obj, section):
        self.vaddr = vaddr
        self.size = size
        self.name = name
        self.obj = obj
        self.section = section

    def __repr__(self):
        return f"Symbol(0x{self.vaddr:08X}, size=0x{self.size:X}, {self.name!r}, {self.obj})"


def parse_map(path):
    """Return (funcs, all_syms).

    funcs: executable symbols with size > 0, sorted by vaddr, de-duplicated.
    all_syms: every parsed symbol (any section), for data/label lookups later.
    """
    section = None
    all_syms = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = _SECTION.match(line.rstrip())
        if m:
            section = m.group(1)
            continue
        if section is None:
            continue
        r = _ROW.match(line)
        if not r:
            continue
        _fileoff, size_h, vaddr_h, _align, name, obj = r.groups()
        name = name.strip()
        if name.startswith("."):  # section self-entry, e.g. ".text" for an object
            continue
        all_syms.append(Symbol(int(vaddr_h, 16), int(size_h, 16), name, obj.strip(), section))

    # Function table: executable sections, nonzero size, unique start addr.
    seen = {}
    for s in all_syms:
        if s.section not in EXEC_SECTIONS or s.size == 0:
            continue
        # Prefer the first (largest wins on tie won't matter — sizes match extents).
        if s.vaddr not in seen:
            seen[s.vaddr] = s
    funcs = sorted(seen.values(), key=lambda s: s.vaddr)
    return funcs, all_syms


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "extracted/maps/framework.map"
    funcs, all_syms = parse_map(path)
    print(f"total symbols parsed : {len(all_syms)}")
    print(f"executable functions : {len(funcs)}")
    if funcs:
        print(f"first : {funcs[0]}")
        print(f"last  : {funcs[-1]}")
        total = sum(f.size for f in funcs)
        print(f"code covered by funcs: {total} bytes (0x{total:X})")
        # Show a few from the entry region.
        for f in funcs[:5]:
            print("   ", f)
    # sanity: any overlaps / gaps?
    overlaps = 0
    for a, b in zip(funcs, funcs[1:]):
        if a.vaddr + a.size > b.vaddr:
            overlaps += 1
    print(f"overlapping funcs    : {overlaps}")


if __name__ == "__main__":
    main()
