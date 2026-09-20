#!/usr/bin/env python3
"""
PowerPC 750 / Gekko (GameCube) instruction decoder.

Decodes a 32-bit big-endian instruction word into an `Instr` carrying:
  - mn      canonical mnemonic (capstone-style simplifications applied for validation)
  - fields  the raw operand fields the C emitter needs (rD, rA, rB, simm, d, bo, ...)
  - rc/oe/aa/lk flags
  - a .dis(addr) capstone-style disassembly string (for oracle cross-checking)

Design priority: correct FIELD extraction and correct SEMANTIC classification. The
disassembly text mirrors capstone only closely enough to validate; the C emitter
consumes `fields`, never the text.
"""
import struct

# ---- bit-field helpers (PowerPC numbers bits 0=MSB .. 31=LSB) -----------------


def bits(w, a, b):
    """Bits [a..b] inclusive, MSB=0 numbering."""
    width = b - a + 1
    return (w >> (31 - b)) & ((1 << width) - 1)


def bit(w, n):
    return (w >> (31 - n)) & 1


def sext(val, width):
    if val & (1 << (width - 1)):
        return val - (1 << width)
    return val


# Special-purpose register names (PPC750 / Gekko) for mt/mf spr simplification.
SPR_NAMES = {
    1: "xer", 8: "lr", 9: "ctr", 18: "dsisr", 19: "dar", 22: "dec", 25: "sdr1",
    26: "srr0", 27: "srr1", 272: "sprg0", 273: "sprg1", 274: "sprg2", 275: "sprg3",
    282: "ear", 287: "pvr",
    # BAT registers (capstone groups by U/L, index is an operand)
    528: "ibatu", 529: "ibatl", 530: "ibatu", 531: "ibatl",
    532: "ibatu", 533: "ibatl", 534: "ibatu", 535: "ibatl",
    536: "dbatu", 537: "dbatl", 538: "dbatu", 539: "dbatl",
    540: "dbatu", 541: "dbatl", 542: "dbatu", 543: "dbatl",
    # Gekko graphics quantization registers (paired singles) + HID/cache
    912: "gqr0", 913: "gqr1", 914: "gqr2", 915: "gqr3",
    916: "gqr4", 917: "gqr5", 918: "gqr6", 919: "gqr7",
    920: "hid2", 936: "ummcr0", 940: "ummcr1", 952: "mmcr0", 956: "mmcr1",
    953: "pmc1", 954: "pmc2", 957: "pmc3", 958: "pmc4", 939: "usia", 955: "sia",
    1008: "hid0", 1009: "hid1", 1010: "iabr", 1013: "dabr", 1017: "l2cr",
    1019: "ictc", 1011: "dmiss", 1012: "dcmp", 1014: "hash1", 1015: "hash2",
    1018: "thrm3", 984: "wpar", 921: "wpar",
}


class Instr:
    __slots__ = ("word", "mn", "form", "f", "rc", "oe", "aa", "lk", "illegal")

    def __init__(self, word):
        self.word = word
        self.mn = "illegal"
        self.form = None
        self.f = {}          # named operand fields
        self.rc = 0
        self.oe = 0
        self.aa = 0
        self.lk = 0
        self.illegal = False

    def __repr__(self):
        return f"<Instr {self.mn} {self.f}>"

    # ---- disassembly text (capstone-style, for validation only) ----
    def dis(self, addr):
        f = self.f
        m = self.mn
        suf = ("." if self.rc else "") + ("o" if self.oe else "")

        def r(k):
            return f"r{f[k]}"

        def fr(k):
            return f"f{f[k]}"

        if self.form == "d_load_store":       # lwz r3, 0x14(r1)
            return f"{m}{'' } {r('rD')}, {hexs(f['d'])}({r('rA')})"
        if self.form == "d_load_store_f":
            return f"{m} {fr('frD')}, {hexs(f['d'])}({r('rA')})"
        if self.form == "addi":
            return f"{m} {r('rD')}, {r('rA')}, {hexs(f['simm'])}"
        if self.form == "addis_lis":
            if f['rA'] == 0 and m == "lis":
                return f"lis {r('rD')}, {hexs(f['simm'])}"
            return f"{m} {r('rD')}, {r('rA')}, {hexs(f['simm'])}"
        if self.form == "d_logical":           # ori r3, r0, 0x1234 (uimm)
            return f"{m} {r('rA')}, {r('rD')}, {hexs(f['uimm'])}"
        if self.form == "cmp_i":               # cmpwi r3, 0
            crf = f['crfD']
            pre = "" if crf == 0 else f"cr{crf}, "
            return f"{m} {pre}{r('rA')}, {hexs(f['simm'])}"
        if self.form == "cmpl_i":
            crf = f['crfD']
            pre = "" if crf == 0 else f"cr{crf}, "
            return f"{m} {pre}{r('rA')}, {hexs(f['uimm'])}"
        if self.form == "branch":
            tgt = (f['li'] + (0 if self.aa else addr)) & 0xFFFFFFFF
            return f"{m} {hexs(tgt)}"
        if self.form == "branch_cond":
            tgt = (f['bd'] + (0 if self.aa else addr)) & 0xFFFFFFFF
            return f"{m} {hexs(tgt)}"
        if self.form == "branch_ctr" or self.form == "branch_lr":
            return f"{m}"
        if self.form == "rr":                  # add r3, r4, r5
            return f"{m}{suf} {r('rD')}, {r('rA')}, {r('rB')}"
        if self.form == "rr_ab":               # cmpw r3, r4  (rD is crf)
            crf = f.get('crfD', 0)
            pre = "" if crf == 0 else f"cr{crf}, "
            return f"{m} {pre}{r('rA')}, {r('rB')}"
        if self.form == "logical_rr":          # or rA, rS, rB
            if m == "mr":                       # mr rA, rS  (or rA,rS,rS)
                return f"mr{suf} {r('rA')}, {r('rD')}"
            return f"{m}{suf} {r('rA')}, {r('rD')}, {r('rB')}"
        if self.form == "shift_imm":           # srawi rA, rS, sh
            return f"{m}{suf} {r('rA')}, {r('rD')}, {f['sh']}"
        if self.form == "rlwinm":
            return f"{m}{suf} {r('rA')}, {r('rD')}, {f['sh']}, {f['mb']}, {f['me']}"
        if self.form == "unary":               # extsb rA, rS
            return f"{m}{suf} {r('rA')}, {r('rD')}"
        if self.form == "neg":
            return f"{m}{suf} {r('rD')}, {r('rA')}"
        if self.form == "x_load_store":        # lwzx rD, rA, rB
            return f"{m} {r('rD')}, {r('rA')}, {r('rB')}"
        if self.form == "mfspr":
            return f"{m} {r('rD')}"
        if self.form == "mtspr":
            return f"{m} {r('rD')}"
        if self.form == "fp_rr":
            return f"{m}{suf} {fr('frD')}, {fr('frA')}, {fr('frB')}"
        if self.form == "raw":
            return m
        return f"{m} ?"


def hexs(v):
    if v < 0:
        return f"-{hex(-v)}"
    return hex(v)


# ---- extended-opcode tables for primary 31 (XO form) -------------------------
# name, form-helper, rc?, oe? handled generically.

def decode(word):
    ins = Instr(word)
    op = bits(word, 0, 5)
    f = ins.f

    def d_form(mn, form):
        f['rD'] = bits(word, 6, 10)
        f['rA'] = bits(word, 11, 15)
        f['simm'] = sext(bits(word, 16, 31), 16)
        f['uimm'] = bits(word, 16, 31)
        f['d'] = sext(bits(word, 16, 31), 16)
        ins.mn = mn
        ins.form = form

    if op == 3:
        ins.mn, ins.form = "twi", "raw"
        return ins
    if op == 7:
        d_form("mulli", "addi")
        return ins
    if op == 8:
        d_form("subfic", "addi")
        return ins
    if op == 10:
        f['crfD'] = bits(word, 6, 8)
        f['rA'] = bits(word, 11, 15)
        f['uimm'] = bits(word, 16, 31)
        ins.mn, ins.form = "cmplwi", "cmpl_i"
        return ins
    if op == 11:
        f['crfD'] = bits(word, 6, 8)
        f['rA'] = bits(word, 11, 15)
        f['simm'] = sext(bits(word, 16, 31), 16)
        ins.mn, ins.form = "cmpwi", "cmp_i"
        return ins
    if op == 12:
        d_form("addic", "addi")
        return ins
    if op == 13:
        d_form("addic.", "addi")
        ins.rc = 1
        return ins
    if op == 14:
        d_form("addi", "addi")
        if f['rA'] == 0:
            ins.mn = "li"
        return ins
    if op == 15:
        d_form("addis", "addis_lis")
        if f['rA'] == 0:
            ins.mn = "lis"
        return ins
    if op == 16:   # bc
        ins.aa = bit(word, 30)
        ins.lk = bit(word, 31)
        f['bo'] = bits(word, 6, 10)
        f['bi'] = bits(word, 11, 15)
        f['bd'] = sext(bits(word, 16, 29) << 2, 16)
        ins.mn = _bc_name(f['bo'], f['bi'], ins.lk)
        ins.form = "branch_cond"
        return ins
    if op == 17:
        ins.mn, ins.form = "sc", "raw"
        return ins
    if op == 18:   # b
        ins.aa = bit(word, 30)
        ins.lk = bit(word, 31)
        f['li'] = sext(bits(word, 6, 29) << 2, 26)
        ins.mn = "b" + ("l" if ins.lk else "") + ("a" if ins.aa else "")
        ins.form = "branch"
        return ins
    if op == 19:
        return _decode_19(word, ins)
    if op == 20:
        _rlw(word, ins, "rlwimi")
        return ins
    if op == 21:
        _rlw(word, ins, "rlwinm")
        return ins
    if op == 23:
        _rlw(word, ins, "rlwnm")   # rlwnm uses rB not sh; handled below
        f['rB'] = bits(word, 16, 20)
        return ins
    if op in (24, 25, 26, 27, 28, 29):
        names = {24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis."}
        f['rD'] = bits(word, 6, 10)   # source rS
        f['rA'] = bits(word, 11, 15)  # dest rA
        f['uimm'] = bits(word, 16, 31)
        ins.mn = names[op]
        ins.form = "d_logical"
        if op in (28, 29):
            ins.rc = 1
        if op == 24 and f['rD'] == 0 and f['rA'] == 0 and f['uimm'] == 0:
            ins.mn, ins.form = "nop", "raw"
        return ins
    if op == 31:
        return _decode_31(word, ins)
    if op in _LOADSTORE_D:
        mn, isfp = _LOADSTORE_D[op]
        if isfp:
            f['frD'] = bits(word, 6, 10)
            f['rA'] = bits(word, 11, 15)
            f['d'] = sext(bits(word, 16, 31), 16)
            ins.mn, ins.form = mn, "d_load_store_f"
        else:
            f['rD'] = bits(word, 6, 10)
            f['rA'] = bits(word, 11, 15)
            f['d'] = sext(bits(word, 16, 31), 16)
            ins.mn, ins.form = mn, "d_load_store"
        return ins
    if op in (46, 47):
        f['rD'] = bits(word, 6, 10)
        f['rA'] = bits(word, 11, 15)
        f['d'] = sext(bits(word, 16, 31), 16)
        ins.mn, ins.form = ("lmw" if op == 46 else "stmw"), "d_load_store"
        return ins
    if op in (56, 57, 60, 61):   # psq_l/psq_lu/psq_st/psq_stu (Gekko)
        f['frD'] = bits(word, 6, 10)
        f['rA'] = bits(word, 11, 15)
        f['w'] = bit(word, 16)
        f['i'] = bits(word, 17, 19)
        f['ps_d'] = sext(bits(word, 20, 31), 12)
        names = {56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}
        ins.mn, ins.form = names[op], "raw"
        return ins
    if op == 4:
        return _decode_4(word, ins)   # Gekko paired-single ops
    if op == 59:
        return _decode_fp(word, ins, single=True)
    if op == 63:
        return _decode_fp(word, ins, single=False)

    ins.illegal = True
    return ins


_LOADSTORE_D = {
    32: ("lwz", False), 33: ("lwzu", False), 34: ("lbz", False), 35: ("lbzu", False),
    36: ("stw", False), 37: ("stwu", False), 38: ("stb", False), 39: ("stbu", False),
    40: ("lhz", False), 41: ("lhzu", False), 42: ("lha", False), 43: ("lhau", False),
    44: ("sth", False), 45: ("sthu", False),
    48: ("lfs", True), 49: ("lfsu", True), 50: ("lfd", True), 51: ("lfdu", True),
    52: ("stfs", True), 53: ("stfsu", True), 54: ("stfd", True), 55: ("stfdu", True),
}


def _bc_name(bo, bi, lk):
    # Simplified branch mnemonics matching capstone's common forms.
    # BO bits, MSB first: BO0=val16, BO1=val8, BO2=val4(don't-dec-CTR),
    #                     BO3=val2(CTR==0 select), BO4=val1.
    cond = bi & 3     # 0=lt,1=gt,2=eq,3=so
    suffix = "l" if lk else ""
    bo0 = (bo >> 4) & 1   # 1 => ignore CR (always / CTR-only)
    bo1 = (bo >> 3) & 1   # CR condition value to branch on (1=true, 0=false)
    bo2 = (bo >> 2) & 1   # 1 => do NOT decrement CTR
    bo3 = (bo >> 1) & 1   # when decrementing: 1 => branch if CTR==0, 0 => CTR!=0
    if bo2 == 1:  # CTR not used -> pure condition branch (or always)
        if bo0 == 1:  # branch always
            return "b" + suffix
        true_branch = bo1 == 1
        names_t = {0: "blt", 1: "bgt", 2: "beq", 3: "bso"}
        names_f = {0: "bge", 1: "ble", 2: "bne", 3: "bns"}
        return (names_t if true_branch else names_f)[cond] + suffix
    else:  # decrement CTR forms
        if bo0 == 1:   # CTR-only, no CR test
            return ("bdz" if bo3 == 1 else "bdnz") + suffix
        # decrement CTR *and* test CR
        base = ("bdz" if bo3 == 1 else "bdnz")
        return base + ("t" if bo1 == 1 else "f") + suffix


def _bcx_name(bo, bi, to, lk):
    """Condition-specific name for bclr/bcctr: blr, beqlr, bnectr, bctr, ..."""
    cond = bi & 3
    l = "l" if lk else ""
    bo0 = (bo >> 4) & 1
    bo1 = (bo >> 3) & 1
    bo2 = (bo >> 2) & 1
    if bo2 == 1:  # no CTR decrement (bcctr is always this way)
        if bo0 == 1:          # branch always
            return "b" + to + l
        tok_t = {0: "lt", 1: "gt", 2: "eq", 3: "so"}
        tok_f = {0: "ge", 1: "le", 2: "ne", 3: "ns"}
        tok = tok_t[cond] if bo1 == 1 else tok_f[cond]
        return "b" + tok + to + l
    # CTR-decrement forms targeting lr (rare) -> generic
    return ("bclr" if to == "lr" else "bcctr") + l


def _rlw(word, ins, mn):
    ins.f['rD'] = bits(word, 6, 10)    # rS
    ins.f['rA'] = bits(word, 11, 15)
    ins.f['sh'] = bits(word, 16, 20)
    ins.f['mb'] = bits(word, 21, 25)
    ins.f['me'] = bits(word, 26, 30)
    ins.rc = bit(word, 31)
    ins.mn = mn
    ins.form = "rlwinm"


def _decode_19(word, ins):
    xo = bits(word, 21, 30)
    f = ins.f
    if xo == 16:      # bclr
        ins.lk = bit(word, 31)
        f['bo'] = bits(word, 6, 10)
        f['bi'] = bits(word, 11, 15)
        ins.mn = _bcx_name(f['bo'], f['bi'], "lr", ins.lk)
        ins.form = "branch_lr"
        return ins
    if xo == 528:     # bcctr
        ins.lk = bit(word, 31)
        f['bo'] = bits(word, 6, 10)
        f['bi'] = bits(word, 11, 15)
        ins.mn = _bcx_name(f['bo'], f['bi'], "ctr", ins.lk)
        ins.form = "branch_ctr"
        return ins
    # CR logical ops
    crops = {257: "crand", 449: "cror", 193: "crxor", 225: "crnand",
             33: "crnor", 289: "creqv", 129: "crandc", 417: "crorc"}
    if xo in crops:
        f['crbD'] = bits(word, 6, 10)
        f['crbA'] = bits(word, 11, 15)
        f['crbB'] = bits(word, 16, 20)
        ins.mn = crops[xo]
        if xo == 193 and f['crbD'] == f['crbA'] == f['crbB']:
            ins.mn = "crclr"
        if xo == 289 and f['crbD'] == f['crbA'] == f['crbB']:
            ins.mn = "crset"
        ins.form = "raw"
        return ins
    if xo == 0:       # mcrf
        ins.mn, ins.form = "mcrf", "raw"
        return ins
    if xo == 50:
        ins.mn, ins.form = "rfi", "raw"
        return ins
    if xo == 150:
        ins.mn, ins.form = "isync", "raw"
        return ins
    ins.mn, ins.form = "op19?", "raw"
    ins.illegal = True
    return ins


# primary-31 extended opcodes (bits 22..30, 9-bit XO). We read XO at 21..30 (10 bit)
# but many use bit21 as OE; canonical is XO=bits[22..30]. We compute both.
def _decode_31(word, ins):
    f = ins.f
    ins.rc = bit(word, 31)
    ins.oe = bit(word, 21)
    xo10 = bits(word, 21, 30)   # 10-bit extended opcode (the canonical field)
    xo9 = xo10 & 0x1FF          # low 9 bits (OE-form arithmetic XO, OE bit stripped)
    rD = bits(word, 6, 10)
    rA = bits(word, 11, 15)
    rB = bits(word, 16, 20)
    f['rD'], f['rA'], f['rB'] = rD, rA, rB

    # arithmetic (XO form, OE+Rc): valid when the 10-bit field is xo9 (OE=0) or
    # xo9|0x200 (OE=1). This disambiguates from the 10-bit non-arith opcodes.
    arith = {
        266: "add", 40: "subf", 10: "addc", 8: "subfc", 138: "adde", 136: "subfe",
        234: "addme", 232: "subfme", 202: "addze", 200: "subfze",
        235: "mullw", 75: "mulhw", 11: "mulhwu", 491: "divw", 459: "divwu",
        104: "neg",
    }
    if xo9 in arith and xo10 in (xo9, xo9 | 0x200):
        ins.mn = arith[xo9]
        ins.form = "neg" if xo9 == 104 else "rr"
        return ins
    xo = xo10   # all remaining primary-31 ops key on the full 10-bit field

    # logical (Rc only, X form: rA = rS op rB)
    logical = {
        28: "and", 444: "or", 316: "xor", 476: "nand", 124: "nor", 284: "eqv",
        60: "andc", 412: "orc",
    }
    if xo in logical:
        ins.mn = logical[xo]
        ins.form = "logical_rr"
        if xo == 444 and rD == rB:   # or rA,rS,rS = mr
            ins.mn = "mr"
            ins.form = "logical_rr"
        return ins

    # shifts (X form, Rc)
    if xo == 24:
        ins.mn, ins.form = "slw", "logical_rr"; return ins
    if xo == 536:
        ins.mn, ins.form = "srw", "logical_rr"; return ins
    if xo == 792:
        ins.mn, ins.form = "sraw", "logical_rr"; return ins
    if xo == 824:
        f['sh'] = bits(word, 16, 20)
        ins.mn, ins.form = "srawi", "shift_imm"; return ins

    # unary (rA = op(rS))
    if xo == 954:
        ins.mn, ins.form = "extsb", "unary"; return ins
    if xo == 922:
        ins.mn, ins.form = "extsh", "unary"; return ins
    if xo == 26:
        ins.mn, ins.form = "cntlzw", "unary"; return ins

    # compares (X form): cmp/cmpl
    if xo == 0:
        f['crfD'] = bits(word, 6, 8)
        ins.mn, ins.form = "cmpw", "rr_ab"; return ins
    if xo == 32:
        f['crfD'] = bits(word, 6, 8)
        ins.mn, ins.form = "cmplw", "rr_ab"; return ins

    # indexed loads/stores
    ls_x = {
        23: "lwzx", 55: "lwzux", 87: "lbzx", 119: "lbzux",
        151: "stwx", 183: "stwux", 215: "stbx", 247: "stbux",
        279: "lhzx", 311: "lhzux", 343: "lhax", 375: "lhaux",
        407: "sthx", 439: "sthux",
        533: "lswx", 534: "lwbrx", 662: "stwbrx", 790: "lhbrx", 918: "sthbrx",
        20: "lwarx", 150: "stwcx.",
        535: "lfsx", 567: "lfsux", 599: "lfdx", 631: "lfdux",
        663: "stfsx", 695: "stfsux", 727: "stfdx", 759: "stfdux", 983: "stfiwx",
    }
    if xo in ls_x:
        ins.mn, ins.form = ls_x[xo], "x_load_store"
        return ins

    # special registers / misc
    if xo == 339:   # mfspr
        spr = (bits(word, 16, 20) << 5) | bits(word, 11, 15)
        f['spr'] = spr
        nm = SPR_NAMES.get(spr)
        ins.mn = ("mf" + nm) if nm else "mfspr"
        ins.form = "mfspr"; return ins
    if xo == 467:   # mtspr
        spr = (bits(word, 16, 20) << 5) | bits(word, 11, 15)
        f['spr'] = spr
        nm = SPR_NAMES.get(spr)
        ins.mn = ("mt" + nm) if nm else "mtspr"
        ins.form = "mtspr"; return ins
    if xo == 371:   # mftb / mftbu (spr 268=TBL, 269=TBU)
        spr = (bits(word, 16, 20) << 5) | bits(word, 11, 15)
        f['spr'] = spr
        ins.mn = "mftbu" if spr == 269 else "mftb"
        ins.form = "mfspr"; return ins
    if xo == 19:
        ins.mn, ins.form = "mfcr", "mfspr"; return ins
    if xo == 144:   # mtcrf
        f['crm'] = bits(word, 12, 19)
        ins.mn, ins.form = "mtcrf", "mtspr"; return ins
    if xo == 512:
        ins.mn, ins.form = "mcrxr", "raw"; return ins
    if xo in (83, 146, 210, 242, 163, 371, 595, 598, 854, 982, 470, 54, 86, 4, 124 if False else 9999):
        pass  # placeholder to keep structure clear
    misc = {
        83: "mfmsr", 146: "mtmsr", 210: "mtsr", 242: "mtsrin",
        163: "mtsrin", 595: "mfsr", 598: "sync",
        854: "eieio", 982: "icbi", 470: "dcbi", 54: "dcbst",
        86: "dcbf", 246: "dcbtst", 278: "dcbt", 1014: "dcbz",
        306: "tlbie", 566: "tlbsync", 370: "tlbia", 4: "tw",
        512: "mcrxr", 200 if False else 9998: "x",
    }
    if xo in misc:
        ins.mn, ins.form = misc[xo], "raw"
        return ins

    ins.mn = f"op31.{xo}?"
    ins.form = "raw"
    ins.illegal = True
    return ins


def _decode_fp(word, ins, single):
    f = ins.f
    ins.rc = bit(word, 31)
    xo5 = bits(word, 26, 30)    # A-form ops use 5-bit sub-opcode
    xo10 = bits(word, 21, 30)
    frD = bits(word, 6, 10)
    frA = bits(word, 11, 15)
    frB = bits(word, 16, 20)
    frC = bits(word, 21, 25)
    f['frD'], f['frA'], f['frB'], f['frC'] = frD, frA, frB, frC
    s = "s" if single else ""

    aform = {18: "fdiv", 20: "fsub", 21: "fadd", 25: "fmul",
             29: "fmadd", 28: "fmsub", 31: "fnmadd", 30: "fnmsub", 24: "fres",
             26: "frsqrte", 23: "fsel"}
    if xo5 in aform and (single or xo5 != 22):
        ins.mn = aform[xo5] + (s if aform[xo5] not in ("fsel", "fres", "frsqrte") else "")
        ins.form = "fp_rr"
        return ins

    # X-form (10-bit) FP ops (mostly opcode 63)
    if not single:
        xmap = {0: "fcmpu", 32: "fcmpo", 12: "frsp", 14: "fctiw", 15: "fctiwz",
                40: "fneg", 72: "fmr", 136: "fnabs", 264: "fabs",
                583: "mffs", 711: "mtfsf", 64: "mcrfs", 38: "mtfsb1",
                70: "mtfsb0", 134: "mtfsfi"}
        if xo10 in xmap:
            ins.mn, ins.form = xmap[xo10], "fp_rr"
            return ins

    ins.mn = f"fp{'s' if single else ''}.{xo5}?"
    ins.form = "raw"
    ins.illegal = True
    return ins


def _decode_4(word, ins):
    # Gekko paired-single ops (primary 4). Recognize, mark for later emit.
    xo = bits(word, 21, 30)
    xo5 = bits(word, 26, 30)
    ins.f['frD'] = bits(word, 6, 10)
    ins.f['frA'] = bits(word, 11, 15)
    ins.f['frB'] = bits(word, 16, 20)
    ins.f['frC'] = bits(word, 21, 25)
    ps = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1",
          14: "ps_madds0", 15: "ps_madds1", 18: "ps_div", 20: "ps_sub",
          21: "ps_add", 23: "ps_sel", 24: "ps_res", 25: "ps_mul",
          26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub",
          31: "ps_nmadd"}
    if xo5 in ps:
        ins.mn, ins.form = ps[xo5], "raw"
        return ins
    xps = {40: "ps_neg", 72: "ps_mr", 136: "ps_nabs", 264: "ps_abs",
           0: "ps_cmpu0", 32: "ps_cmpo0", 64: "ps_cmpu1", 96: "ps_cmpo1",
           528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10",
           624: "ps_merge11", 1014: "dcbz_l"}
    if xo in xps:
        ins.mn, ins.form = xps[xo], "raw"
        return ins
    ins.mn, ins.form = f"ps.{xo}?", "raw"
    ins.illegal = True
    return ins


if __name__ == "__main__":
    import sys
    sys.path.insert(0, ".")
    from dol import Dol
    dol = Dol(sys.argv[1] if len(sys.argv) > 1 else "extracted/main.dol")
    addr = 0x800056E0
    for i in range(26):
        w = dol.word(addr)
        ins = decode(w)
        print(f"{addr:08X}: {w:08X}  {ins.dis(addr)}")
        addr += 4
