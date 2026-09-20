#!/usr/bin/env python3
"""Classify undecodable words: inside a known function body, or in padding/gaps?

With an exact function table we can tell a real decoder gap from data-in-text.
Words outside every function extent are not code and never get emitted.
"""
import collections, sys
sys.path.insert(0, ".")
from dol import Dol
from dtk_symbols import parse_symbols
import ppc

dol = Dol(sys.argv[1])
funcs, _ = parse_symbols(sys.argv[2])

# address -> owning function, via sorted extents
import bisect
starts = [f.vaddr for f in funcs]
def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and funcs[i].vaddr <= a < funcs[i].vaddr + funcs[i].size:
        return funcs[i]
    return None

inside, outside = [], 0
covered = 0
for sec in dol.text_sections():
    a, end = sec.addr, sec.addr + sec.size
    while a + 4 <= end:
        w = dol.word(a)
        f = owner(a)
        if f: covered += 1
        ins = ppc.decode(w)
        if ins.illegal:
            if f: inside.append((a, w, f))
            else: outside += 1
        a += 4

total = sum(s.size for s in dol.text_sections()) // 4
print(f"text words                  : {total}")
print(f"words inside a known func   : {covered}  ({100.0*covered/total:.2f}%)")
print(f"undecodable INSIDE a func   : {len(inside)}   <-- real decoder gaps")
print(f"undecodable outside (pad/data): {outside}")
if inside:
    byfunc = collections.Counter(f.name for _,_,f in inside)
    print("--- top functions containing undecodable words ---")
    for n, c in byfunc.most_common(15):
        print(f"  {n:40} x{c}")
    print("--- sample words ---")
    for a, w, f in inside[:15]:
        print(f"  0x{a:08X}  {w:08X}  op={w>>26:<3} in {f.name}")
