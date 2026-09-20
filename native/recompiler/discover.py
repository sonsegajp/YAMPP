#!/usr/bin/env python3
"""
Discover functions in DOL text regions NOT covered by the symbol map (chiefly text0,
the CodeWarrior __start bootstrap). framework.map only covers text1.

Heuristic function starts within an uncovered region:
  - the section start and the DOL entry point
  - any branch/call target (from anywhere in the DOL) that lands in the region
  - the instruction right after a terminator (blr / unconditional b / rfi)
Then the region is sliced into [start, next_start) functions covering it fully.
"""
import sys

sys.path.insert(0, ".")
import ppc


def discover_region(dol, lo, hi, extra_starts=()):
    """Function starts = section start, entry, CALL (bl) targets, and the address
    after a function-ending terminator. Branch (b/bc) targets are INTERNAL labels
    and must NOT start a new function — treating them as starts fragments real
    functions and breaks their control flow."""
    starts = set([lo]) | set(a for a in extra_starts if lo <= a < hi)

    # bl/bla call targets anywhere in the DOL that land in [lo,hi) are real functions
    for sec in dol.text_sections():
        a = sec.addr
        end = sec.addr + sec.size
        while a + 4 <= end:
            ins = ppc.decode(dol.word(a))
            if ins.form == "branch" and ins.lk:      # bl / bla only
                tgt = (ins.f["li"] + (0 if ins.aa else a)) & 0xFFFFFFFF
                if lo <= tgt < hi:
                    starts.add(tgt)
            a += 4

    # start a new function only after a real return (blr/rfi), skipping alignment nops
    a = lo
    prev_ret = False
    while a < hi:
        ins = ppc.decode(dol.word(a))
        if prev_ret and ins.mn != "nop":
            starts.add(a)
        if ins.mn in ("blr", "rfi"):
            prev_ret = True
        elif ins.mn != "nop":
            prev_ret = False
        a += 4

    starts = sorted(s for s in starts if lo <= s < hi)
    funcs = []
    for i, s in enumerate(starts):
        nxt = starts[i + 1] if i + 1 < len(starts) else hi
        funcs.append((s, nxt - s, f"boot_{s:08X}"))
    return funcs


def discover_text0(dol):
    """text0 = the first text section (bootstrap). Entry is inside it."""
    t0 = dol.text_sections()[0]
    return discover_region(dol, t0.addr, t0.addr + t0.size, extra_starts=[dol.entry])


def discover_unmapped_calls(dol, funcset):
    """Find every bl/bla target that is NOT already a known function and synthesize a
    function for it. Chiefly the CodeWarrior _savegpr/_restgpr/_savefpr/_restfpr helpers
    (which the linker map omits) — without these, `bl _savegpr` no-ops and non-volatile
    registers get silently clobbered across the whole program.

    Each synthesized function spans [target, first terminator]. Helper entry points that
    share a common blr just produce overlapping single-run functions — each correct."""
    targets = set()
    for sec in dol.text_sections():
        a, end = sec.addr, sec.addr + sec.size
        while a + 4 <= end:
            ins = ppc.decode(dol.word(a))
            if ins.form == "branch" and ins.lk:            # bl / bla
                tgt = (ins.f["li"] + (0 if ins.aa else a)) & 0xFFFFFFFF
                if tgt not in funcset and dol.section_at(tgt):
                    targets.add(tgt)
            a += 4

    out = []
    for t in sorted(targets):
        sec = dol.section_at(t)
        end = sec.addr + sec.size
        a = t
        while a < end:                                     # extend to first terminator
            ins = ppc.decode(dol.word(a))
            a += 4
            if ins.mn in ("blr", "bctr", "rfi") or (ins.mn in ("b", "ba") and not ins.lk):
                break
        out.append((t, a - t, f"helper_{t:08X}"))
    return out


if __name__ == "__main__":
    from dol import Dol
    dol = Dol(sys.argv[1] if len(sys.argv) > 1 else "extracted/main.dol")
    fns = discover_text0(dol)
    print(f"text0: {len(fns)} functions discovered, entry=0x{dol.entry:08X}")
    for a, sz, n in fns[:12]:
        print(f"  {a:08X} size=0x{sz:X} {n}")
