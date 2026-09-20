#!/usr/bin/env python3
"""
Parse a decomp-toolkit `symbols.txt` into our function table.

decomp-toolkit rows look like:
    memset = .init:0x80003100; // type:function size:0x30 scope:global
    gTRKInterruptVectorTable = .init:0x80003298; // type:label scope:global

This is a richer source than a CodeWarrior .map: every symbol carries an explicit
`type:` and (for functions) an exact `size:`, so the function table is exact rather
than inferred. Symbols with no size are kept in `all_syms` for data/label lookups.

Also parses the companion `splits.txt` for section layout and TU boundaries, which
gives us the original translation-unit grouping to shard emitted C by.
"""
import re
import sys

from symbols import Symbol

# name = section:0xADDR; // type:X size:0xN scope:Y ...
_ROW = re.compile(
    r"^\s*([^\s=]+)\s*=\s*([^:]+):0x([0-9a-fA-F]+)\s*;\s*(?://\s*(.*))?$"
)
_ATTR = re.compile(r"(\w+):(\S+)")

EXEC_SECTIONS = {".text", ".init"}


def parse_symbols(path):
    """Return (funcs, all_syms) — funcs are exec-section symbols with size > 0."""
    all_syms = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = _ROW.match(line)
        if not m:
            continue
        name, section, vaddr_h, attrs = m.groups()
        a = dict(_ATTR.findall(attrs or ""))
        size = int(a["size"], 16) if "size" in a else 0
        s = Symbol(int(vaddr_h, 16), size, name.strip(), a.get("type", ""), section.strip())
        all_syms.append(s)

    seen = {}
    for s in all_syms:
        # s.obj holds the dtk `type:` field for this loader.
        if s.section not in EXEC_SECTIONS or s.size == 0 or s.obj != "function":
            continue
        if s.vaddr not in seen or seen[s.vaddr].size < s.size:
            seen[s.vaddr] = s
    return sorted(seen.values(), key=lambda s: s.vaddr), all_syms


_SPLIT_TU = re.compile(r"^(\S.*?):\s*$")
_SPLIT_SEC = re.compile(r"^\s+(\S+)\s+start:0x([0-9a-fA-F]+)\s+end:0x([0-9a-fA-F]+)")


def parse_splits(path):
    """Return {tu_name: {section: (start, end)}} from a decomp-toolkit splits.txt."""
    tus, cur = {}, None
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("Sections:") or not line.strip():
            continue
        m = _SPLIT_TU.match(line)
        if m and not line[0].isspace():
            cur = m.group(1)
            tus[cur] = {}
            continue
        m = _SPLIT_SEC.match(line)
        if m and cur:
            tus[cur][m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return tus


def main():
    funcs, all_syms = parse_symbols(sys.argv[1])
    print(f"total symbols parsed : {len(all_syms)}")
    print(f"executable functions : {len(funcs)}")
    total = sum(f.size for f in funcs)
    print(f"code covered by funcs: {total} bytes (0x{total:X})")
    print(f"first : {funcs[0]}")
    print(f"last  : {funcs[-1]}")
    overlaps = [(a, b) for a, b in zip(funcs, funcs[1:]) if a.vaddr + a.size > b.vaddr]
    print(f"overlapping funcs    : {len(overlaps)}")
    if len(sys.argv) > 2:
        tus = parse_splits(sys.argv[2])
        ntext = sum(1 for t in tus.values() if ".text" in t)
        print(f"translation units    : {len(tus)}  ({ntext} with .text)")


if __name__ == "__main__":
    main()
