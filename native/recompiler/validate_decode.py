#!/usr/bin/env python3
"""
Oracle-validate our PPC decoder against capstone across the whole DOL.

For every 4-byte word in the executable sections we decode with both and compare
the mnemonic token. Capstone's simplifications are normalized to ours. Gekko
paired-single / cache ops that stock capstone cannot decode are tracked separately
(expected: capstone fails, we succeed).

Reports: coverage, mnemonic match rate, and the top mismatching (mine, capstone) pairs.
"""
import collections
import re
import sys

sys.path.insert(0, ".")
from dol import Dol
import ppc
from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN

# Primary opcodes that are Gekko-specific (paired singles / psq). Stock capstone
# mis-decodes these as modern VSX, so we treat our decode as authoritative and skip
# the cross-check.
GEKKO_PRIMARY = {4, 56, 57, 60, 61}

# Map capstone (and our) simplified mnemonics to a canonical family token so that
# equivalent decodes (mr==or, clrlwi==slwi==rlwinm, sub==subf) compare equal.
FAM = {
    "or": "or", "mr": "or",
    "nor": "nor", "not": "nor",
    "subf": "subf", "sub": "subf",
    "rlwinm": "rlwinm", "rotlwi": "rlwinm", "rotrwi": "rlwinm", "slwi": "rlwinm",
    "srwi": "rlwinm", "clrlwi": "rlwinm", "clrrwi": "rlwinm", "clrlslwi": "rlwinm",
    "extlwi": "rlwinm", "extrwi": "rlwinm", "clrslwi": "rlwinm",
    "rlwimi": "rlwimi", "inslwi": "rlwimi", "insrwi": "rlwimi",
    "rlwnm": "rlwnm", "rotlw": "rlwnm",
    "cror": "cror", "crmove": "cror",
    "crnor": "crnor", "crnot": "crnor",
    "mtcrf": "mtcrf", "mtcr": "mtcrf",
    "tw": "tw", "trap": "tw",
    # branch hint spellings capstone may use
    "bng": "ble", "bnl": "bge", "bun": "bso", "bnu": "bns", "bnz": "bne", "bz": "beq",
}


# SPR names that mt/mf can move to/from (distinct from mtmsr/mtsr/mtcrf/mtfs*,
# which are separate opcodes and are NOT folded). Our decoder and capstone name
# different subsets of these, but the underlying op is the same mtspr/mfspr.
_SPR = (r"spr|lr|ctr|xer|dsisr|dar|dec|sdr1|srr0|srr1|sprg[0-3]|ear|pvr|"
        r"[id]batu|[id]batl|[id]bat[0-3][ul]|gqr[0-7]|hid[0-2]|ummcr[01]|mmcr[01]|"
        r"pmc[1-4]|usia|sia|iabr|dabr|l2cr|ictc|tb|tbl|tbu|dmiss|dcmp|hash[12]|"
        r"thrm[1-3]|wpar|iccr|esr|dear|dbatu|ibatu")
_SPR_MOVE = re.compile(r"^(m[tf])(" + _SPR + r")$")


def base(mn):
    """Reduce a mnemonic to its canonical family for equivalence comparison."""
    mn = mn.strip().lower().rstrip("+-")
    if mn.endswith("."):
        mn = mn[:-1]
    m = _SPR_MOVE.match(mn)
    if m:
        return m.group(1) + "spr"   # mtlr/mtsrr0/... -> mtspr ; mf... -> mfspr
    return FAM.get(mn, mn)


def main():
    dol = Dol(sys.argv[1] if len(sys.argv) > 1 else "extracted/main.dol")
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    md.detail = False

    total = 0
    match = 0
    ours_only = 0     # capstone couldn't decode, we could (Gekko/PS expected)
    cs_only = 0       # we couldn't, capstone could (our gap -> bug)
    both_fail = 0
    mism = collections.Counter()
    our_illegal = collections.Counter()

    for sec in dol.text_sections():
        addr = sec.addr
        end = sec.addr + sec.size
        while addr + 4 <= end:
            w = dol.word(addr)
            total += 1
            ins = ppc.decode(w)
            # Gekko paired-single/psq: our decode is authoritative (capstone wrong).
            if (w >> 26) in GEKKO_PRIMARY:
                if not ins.illegal:
                    ours_only += 1
                else:
                    our_illegal[ins.mn] += 1
                    both_fail += 1
                addr += 4
                continue
            # capstone decode of this single word
            cs_list = list(md.disasm(w.to_bytes(4, "big"), addr))
            cs_mn = cs_list[0].mnemonic if cs_list else None

            if ins.illegal and cs_mn is None:
                both_fail += 1
            elif ins.illegal and cs_mn is not None:
                cs_only += 1
                mism[("<illegal>", base(cs_mn))] += 1
            elif not ins.illegal and cs_mn is None:
                ours_only += 1
            else:
                if base(ins.mn) == base(cs_mn):
                    match += 1
                else:
                    mism[(base(ins.mn), base(cs_mn))] += 1
            if ins.illegal:
                our_illegal[ins.mn] += 1
            addr += 4

    decoded = total - both_fail
    print(f"total instrs        : {total}")
    print(f"mnemonic match      : {match}  ({100.0*match/total:.3f}%)")
    print(f"ours-only (Gekko/PS): {ours_only}  (capstone can't; we can)")
    print(f"capstone-only (BUG) : {cs_only}  (we can't; capstone can)")
    print(f"both failed         : {both_fail}")
    print(f"true mismatches     : {sum(v for k,v in mism.items())}")
    print("--- top mismatches (ours -> capstone) x count ---")
    for (a, b), c in mism.most_common(25):
        print(f"  {a:14} -> {b:14} x{c}")
    print("--- our 'illegal' buckets ---")
    for k, c in our_illegal.most_common(15):
        print(f"  {k:16} x{c}")


if __name__ == "__main__":
    main()
