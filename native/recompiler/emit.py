#!/usr/bin/env python3
"""
Code generator: decoded PPC function -> native C targeting runtime/recomp.h.

Control flow model (standard static-recomp): every instruction address gets a label
`L_xxxxxxxx:`; intra-function branches become `goto`; calls (bl) call the target C
function; blr returns. Indirect calls/branches go through lookup_function().
"""
import ppc

# Wrap every direct call so the stack pointer is verified across it.
#
# The ABI requires a callee to return with r1 as it found it. Checking that one
# invariant generically finds a violation at any depth in a single run, instead of
# descending one stack frame per investigation -- which is how the current bug has
# been chased for several rounds, verifying each level clean and going deeper.
ABI_CHECK = True

# ---- target ABI naming -------------------------------------------------------
# "native"     : our own runtime/recomp.h Context (r[]/f[], global g_ram)
# "recompcore" : GXRuntime/DolRecomp CPUState (gpr[]/fpr[], ctx->ram), the shape
#                RecompCore's StaticRecompABI v2 and ModernGekko both consume.
# Only register spelling and the RAM base differ; every mem_* accessor is a macro
# the target's header supplies, so the generated bodies are otherwise identical.
ABI = {
    "native":     {"gpr": "r",   "fpr": "f",   "ram": "g_ram"},
    "recompcore": {"gpr": "gpr", "fpr": "fpr", "ram": "ctx->ram"},
}
_T = ABI["native"]


def set_abi(name):
    global _T
    _T = ABI[name]


def R(n):
    return f"ctx->{_T['gpr']}[{n}]"


def ra0(n):
    """RA operand: literal 0 when the field is r0 (PowerPC address-form rule)."""
    return "0u" if n == 0 else f"ctx->{_T['gpr']}[{n}]"


def F(n):
    return f"ctx->{_T['fpr']}[{n}]"


def RAM():
    return _T["ram"]


def hexu(v):
    return f"0x{v & 0xFFFFFFFF:08X}u"


# BO/BI -> C boolean condition for a conditional branch (non-CTR common case).
def cond_expr(bo, bi):
    bo0 = (bo >> 4) & 1
    bo1 = (bo >> 3) & 1
    bo2 = (bo >> 2) & 1
    if bo2 == 1:
        if bo0 == 1:
            return "1"                      # branch always
        want = bo1
        return f"(cr_bit(ctx,{bi})=={want})"
    # CTR-decrement forms: caller emits the --ctr; here just the combined test.
    bo3 = (bo >> 1) & 1
    ctr_test = "ctx->ctr==0" if bo3 == 1 else "ctx->ctr!=0"
    if bo0 == 1:
        return f"({ctr_test})"
    want = bo1
    return f"({ctr_test} && cr_bit(ctx,{bi})=={want})"


def uses_ctr_decrement(bo):
    return ((bo >> 2) & 1) == 0


class Emitter:
    def __init__(self, funcset, probes=None):
        self.setjmp_address = None
        self.runtime_hooks = set()  # Direct calls to these targets use runtime dispatch.
        self.dynamic_entry_guard = True
        self.funcset = funcset   # set of known function start addresses
        self.probes  = set(probes or ())   # guest addresses to instrument

    def code_address(self, address):
        mapper=getattr(self,"code_address_mapper",None)
        return mapper(address) if mapper else hexu(address)

    def call_target(self, tgt, ret_addr, tail):
        """Emit a call (tail=return after) to an absolute address."""
        lines = []
        if not tail:
            lines.append(f"    ctx->lr = {self.code_address(ret_addr)};")
        if tgt == self.setjmp_address:
            if tail:
                raise ValueError("setjmp tail call needs a caller-owned continuation")
            lines.append(f"    {{ jmp_buf* _env = recomp_jmpbuf({R(3)});")
            lines.append(f"      func_{tgt:08X}(ctx);")
            lines.append("      if (setjmp(*_env) != 0) { /* longjmp restored guest registers */ }")
            lines.append("    }")
        elif tgt in self.runtime_hooks:
            lines.append(f"    {{ uint32_t _sp = {R(1)}; lookup_function({hexu(tgt)})(ctx);"
                         f" abi_check({hexu(tgt)}, _sp, {R(1)}); }}")
        elif tgt in self.funcset:
            if ABI_CHECK:
                lines.append(f"    {{ uint32_t _sp = {R(1)}; func_{tgt:08X}(ctx);"
                             f" abi_check({hexu(tgt)}, _sp, {R(1)}); }}")
            else:
                lines.append(f"    func_{tgt:08X}(ctx);")
        else:
            lines.append(f"    lookup_function({hexu(tgt)})(ctx);")
        if tail:
            lines.append("    return;")
        return "\n".join(lines)

    def emit_function(self, dol, addr, size, name, c_name=None, extra_regions=()):
        # self.probes is a set of guest addresses to instrument (may be empty)
        end = addr + size
        # First pass: which addresses are branch targets (need labels)? All, simplest.
        out = []
        out.append(f"/* {name}  0x{addr:08X}..0x{end:08X} */")
        out.append(f"void {c_name or ('func_%08X' % addr)}(Context* ctx) {{")
        if _T["gpr"] == "gpr" and self.dynamic_entry_guard:
            out.append(f"    if (g_mex_active && mex_runtime_entry(ctx, 0x{addr:08X}u, {size}u)) return;")
        out.extend("    "+line for line in getattr(self,"function_preamble",()))
        out.append(f"    TRACE_ENTER(0x{addr:08X}u, ctx);")
        self.extra_labels = {a for start,length in extra_regions for a in range(start,start+length,4)}
        for a in sorted(set(range(addr,end,4)) | self.extra_labels):
            try:
                w = dol.word(a)
            except KeyError:
                break
            ins = ppc.decode(w)
            out.append(f"L_{a:08X}: {{")
            # Address-keyed mid-function probe.
            #
            # The tracer hooks function entries only, so any value that matters
            # inside a routine -- a pointer just loaded, the target of an indexed
            # store, which of several exits is taken -- has been unobservable.
            # That limit is what stalled the DVD-side investigation. A probe here
            # costs nothing when the set is empty and gives full register
            # visibility at any chosen instruction.
            if a in self.probes:
                out.append(f"    probe_hit(0x{a:08X}u, ctx);")
            body = self.emit_instr(ins, a, addr, end)
            for ln in body:
                out.append("    " + ln)
            out.append("    }")
        out.append("}")
        self.extra_labels = set()
        return "\n".join(out)

    # ---- per-instruction emission -> list of C statements ----
    def emit_instr(self, ins, a, fstart, fend):
        mn = ins.mn
        f = ins.f
        nxt = a + 4
        rc = ins.rc

        def rec(res_expr, dst):
            """emit assignment + optional CR0 update for record form."""
            s = [f"{dst} = {res_expr};"]
            if rc:
                s.append(f"cr0_from(ctx, (int32_t){dst});")
            return s

        # --- no-ops / barriers ---
        if mn in ("nop", "isync", "sync", "eieio", "icbi", "dcbi", "dcbst",
                  "dcbf", "dcbtst", "dcbt", "tlbie", "tlbsync", "tlbia", "mcrxr"):
            return ["/* %s */" % mn]
        if mn == "dcbz":
            ea = f"(({ra0(f['rA'])} + {R(f['rB'])}) & ~31u)"
            return [f"{{ uint32_t cache_ea = {ea}; for (uint32_t k = 0; k < 32; k += 4) mem_w32(cache_ea + k, 0); }}"]
        if mn == "dcbz_l":   # Gekko (opcode 4): regs are in frA/frB fields
            ea = f"(({ra0(f['frA'])} + {R(f['frB'])}) & ~31u)"
            return [f"{{ uint32_t cache_ea = {ea}; for (uint32_t k = 0; k < 32; k += 4) mem_w32(cache_ea + k, 0); }}"]

        # --- immediate arithmetic/logical ---
        if mn in ("addi", "li"):
            src = "0" if mn == "li" else ra0(f['rA'])
            return [f"{R(f['rD'])} = {src} + {f['simm']};"]
        if mn in ("addis", "lis"):
            if mn == "lis":
                return [f"{R(f['rD'])} = {hexu((f['simm'] << 16))};"]
            return [f"{R(f['rD'])} = {ra0(f['rA'])} + {hexu((f['simm'] << 16))};"]
        if mn == "addic" or mn == "addic.":
            # simm must be ZERO-extended from its 32-bit two's-complement form for the carry-out
            # (bit 32) to be correct — sign-extending to 64 bits made XER[CA] wrong for negative
            # immediates (e.g. addic rX,rY,-1), breaking the addic/subfe "!= 0" idiom.
            r = f"(uint64_t){R(f['rA'])} + (uint64_t)(uint32_t){f['simm']}"
            s = [f"{{ uint64_t _t = {r}; set_ca(ctx, _t >> 32 & 1); {R(f['rD'])} = (uint32_t)_t; }}"]
            if mn.endswith("."):
                s.append(f"cr0_from(ctx, (int32_t){R(f['rD'])});")
            return s
        if mn == "subfic":
            r = f"(uint64_t)(~{R(f['rA'])}) + (uint64_t)(uint32_t){f['simm']} + 1"
            return [f"{{ uint64_t _t = {r}; set_ca(ctx, (_t >> 32) & 1); {R(f['rD'])} = (uint32_t)_t; }}"]
        if mn == "mulli":
            return [f"{R(f['rD'])} = (uint32_t)((int32_t){R(f['rA'])} * {f['simm']});"]
        if mn == "ori":
            return [f"{R(f['rA'])} = {R(f['rD'])} | {hexu(f['uimm'])};"]
        if mn == "oris":
            return [f"{R(f['rA'])} = {R(f['rD'])} | {hexu(f['uimm'] << 16)};"]
        if mn == "xori":
            return [f"{R(f['rA'])} = {R(f['rD'])} ^ {hexu(f['uimm'])};"]
        if mn == "xoris":
            return [f"{R(f['rA'])} = {R(f['rD'])} ^ {hexu(f['uimm'] << 16)};"]
        if mn == "andi.":
            return [f"{R(f['rA'])} = {R(f['rD'])} & {hexu(f['uimm'])};",
                    f"cr0_from(ctx, (int32_t){R(f['rA'])});"]
        if mn == "andis.":
            return [f"{R(f['rA'])} = {R(f['rD'])} & {hexu(f['uimm'] << 16)};",
                    f"cr0_from(ctx, (int32_t){R(f['rA'])});"]

        # --- immediate compares ---
        if mn == "cmpwi":
            return [f"cr_set_field(ctx, {f['crfD']}, cr_signed((int32_t){R(f['rA'])} - ({f['simm']}) ? "
                    f"((int32_t){R(f['rA'])} < {f['simm']}) ? 8 : ((int32_t){R(f['rA'])} > {f['simm']}) ? 4 : 2 : 2, ctx->xer>>31));"] \
                if False else \
                   [f"cr_set_field(ctx, {f['crfD']}, "
                    f"((int32_t){R(f['rA'])} < {f['simm']}) ? (8|(ctx->xer>>31&1)) : "
                    f"((int32_t){R(f['rA'])} > {f['simm']}) ? (4|(ctx->xer>>31&1)) : (2|(ctx->xer>>31&1)));"]
        if mn == "cmplwi":
            return [f"cr_set_field(ctx, {f['crfD']}, cr_unsigned({R(f['rA'])}, {hexu(f['uimm'])}, ctx->xer>>31));"]

        # --- register-register arithmetic (31) ---
        if mn == "add":
            return rec(f"{R(f['rA'])} + {R(f['rB'])}", R(f['rD']))
        if mn == "subf":
            return rec(f"{R(f['rB'])} - {R(f['rA'])}", R(f['rD']))
        if mn == "neg":
            return rec(f"(uint32_t)(-(int32_t){R(f['rA'])})", R(f['rD']))
        if mn == "mullw":
            return rec(f"(uint32_t)((int32_t){R(f['rA'])} * (int32_t){R(f['rB'])})", R(f['rD']))
        if mn == "mulhw":
            return rec(f"(uint32_t)(((int64_t)(int32_t){R(f['rA'])} * (int64_t)(int32_t){R(f['rB'])}) >> 32)", R(f['rD']))
        if mn == "mulhwu":
            return rec(f"(uint32_t)(((uint64_t){R(f['rA'])} * (uint64_t){R(f['rB'])}) >> 32)", R(f['rD']))
        if mn == "divw":
            return rec(f"({R(f['rB'])}==0)?0:(uint32_t)((int32_t){R(f['rA'])} / (int32_t){R(f['rB'])})", R(f['rD']))
        if mn == "divwu":
            return rec(f"({R(f['rB'])}==0)?0:({R(f['rA'])} / {R(f['rB'])})", R(f['rD']))
        if mn in ("addc", "adde", "subfc", "subfe", "addme", "addze", "subfme", "subfze"):
            return self.emit_carry(mn, f, rc)

        # --- register-register logical (31) ---
        logic = {"and": "&", "or": "|", "xor": "^"}
        if mn in logic:
            return rec(f"{R(f['rD'])} {logic[mn]} {R(f['rB'])}", R(f['rA']))
        if mn == "mr":
            return rec(f"{R(f['rD'])}", R(f['rA']))
        if mn == "nand":
            return rec(f"~({R(f['rD'])} & {R(f['rB'])})", R(f['rA']))
        if mn == "nor":
            return rec(f"~({R(f['rD'])} | {R(f['rB'])})", R(f['rA']))
        if mn == "eqv":
            return rec(f"~({R(f['rD'])} ^ {R(f['rB'])})", R(f['rA']))
        if mn == "andc":
            return rec(f"{R(f['rD'])} & ~{R(f['rB'])}", R(f['rA']))
        if mn == "orc":
            return rec(f"{R(f['rD'])} | ~{R(f['rB'])}", R(f['rA']))
        if mn == "extsb":
            return rec(f"(uint32_t)(int32_t)(int8_t){R(f['rD'])}", R(f['rA']))
        if mn == "extsh":
            return rec(f"(uint32_t)(int32_t)(int16_t){R(f['rD'])}", R(f['rA']))
        if mn == "cntlzw":
            return rec(f"clz32({R(f['rD'])})", R(f['rA']))

        # --- shifts ---
        if mn == "slw":
            return rec(f"(({R(f['rB'])}&0x20)?0:({R(f['rD'])} << ({R(f['rB'])}&31)))", R(f['rA']))
        if mn == "srw":
            return rec(f"(({R(f['rB'])}&0x20)?0:({R(f['rD'])} >> ({R(f['rB'])}&31)))", R(f['rA']))
        if mn == "sraw":
            return [f"{{ uint32_t _sh = {R(f['rB'])}&0x3F; int32_t _v=(int32_t){R(f['rD'])}; "
                    f"uint32_t _ca; if(_sh>31){{ {R(f['rA'])}=(_v<0)?0xFFFFFFFFu:0; _ca=(_v<0)?1:0;}} "
                    f"else {{ {R(f['rA'])}=(uint32_t)(_v>>_sh); _ca=(_v<0 && ((_v & ((1<<_sh)-1))!=0))?1:0;}} "
                    f"set_ca(ctx,_ca);" + (f" cr0_from(ctx,(int32_t){R(f['rA'])});" if rc else "") + " }"]
        if mn == "srawi":
            sh = f['sh']
            ca = f"((int32_t){R(f['rD'])}<0 && (({R(f['rD'])} & {hexu((1<<sh)-1)})!=0))?1:0" if sh else "0"
            return [f"set_ca(ctx, {ca});",
                    f"{R(f['rA'])} = (uint32_t)((int32_t){R(f['rD'])} >> {sh});"] + \
                   ([f"cr0_from(ctx,(int32_t){R(f['rA'])});"] if rc else [])

        # --- rotates ---
        if mn in ("rlwinm", "rlwnm"):
            sh = f"{f['sh']}" if mn == "rlwinm" else f"({R(f['rB'])}&31)"
            mask = f"ppc_mask({f['mb']},{f['me']})"
            return rec(f"rotl32({R(f['rD'])}, {sh}) & {mask}", R(f['rA']))
        if mn == "rlwimi":
            mask = f"ppc_mask({f['mb']},{f['me']})"
            return rec(f"(rotl32({R(f['rD'])}, {f['sh']}) & {mask}) | ({R(f['rA'])} & ~{mask})", R(f['rA']))

        # --- register compares ---
        if mn == "cmpw":
            return [f"cr_set_field(ctx, {f['crfD']}, cr_signed((int32_t){R(f['rA'])} - (int32_t){R(f['rB'])} ? 0:0, 0));"] \
                if False else \
                   [f"cr_set_field(ctx, {f['crfD']}, "
                    f"((int32_t){R(f['rA'])} < (int32_t){R(f['rB'])})?(8|(ctx->xer>>31&1)):"
                    f"((int32_t){R(f['rA'])} > (int32_t){R(f['rB'])})?(4|(ctx->xer>>31&1)):(2|(ctx->xer>>31&1)));"]
        if mn == "cmplw":
            return [f"cr_set_field(ctx, {f['crfD']}, cr_unsigned({R(f['rA'])}, {R(f['rB'])}, ctx->xer>>31));"]

        # --- loads / stores (D-form) ---
        r = self.emit_loadstore(ins, mn)
        if r is not None:
            return r

        # --- moves to/from special registers ---
        r = self.emit_spr(ins, mn)
        if r is not None:
            return r

        # --- branches ---
        r = self.emit_branch(ins, mn, a, fstart, fend)
        if r is not None:
            return r

        # --- CR logical ---
        r = self.emit_crlogical(ins, mn)
        if r is not None:
            return r

        # --- floating point (double math; refine widths later) ---
        r = self.emit_fp(ins, mn)
        if r is not None:
            return r

        # --- Gekko paired singles ---
        r = self.emit_ps(ins, mn)
        if r is not None:
            return r

        # --- system ---
        if mn == "sc":
            return ["hle_syscall(ctx);"]
        if mn in ("rfi",):
            return ["return; /* rfi */"]
        if mn == "twi" or mn == "tw":
            return ["/* trap %s (ignored) */" % mn]

        # unknown / paired-single / data
        return [f"/* TODO {mn} 0x{ins.word:08X} */"]

    def emit_carry(self, mn, f, rc):
        A, B, D = R(f['rA']), R(f['rB']), R(f['rD'])
        if mn == "addc":
            body = f"uint64_t _t=(uint64_t){A}+(uint64_t){B};"
        elif mn == "adde":
            body = f"uint64_t _t=(uint64_t){A}+(uint64_t){B}+xer_ca(ctx);"
        elif mn == "subfc":
            body = f"uint64_t _t=(uint64_t)(~{A})+(uint64_t){B}+1;"
        elif mn == "subfe":
            body = f"uint64_t _t=(uint64_t)(~{A})+(uint64_t){B}+xer_ca(ctx);"
        elif mn == "addme":
            body = f"uint64_t _t=(uint64_t){A}+xer_ca(ctx)+0xFFFFFFFFu;"
        elif mn == "addze":
            body = f"uint64_t _t=(uint64_t){A}+xer_ca(ctx);"
        elif mn == "subfme":
            body = f"uint64_t _t=(uint64_t)(~{A})+xer_ca(ctx)+0xFFFFFFFFu;"
        elif mn == "subfze":
            body = f"uint64_t _t=(uint64_t)(~{A})+xer_ca(ctx);"
        s = [f"{{ {body} set_ca(ctx,(_t>>32)&1); {D}=(uint32_t)_t;" +
             (f" cr0_from(ctx,(int32_t){D});" if rc else "") + " }"]
        return s

    def emit_loadstore(self, ins, mn):
        f = ins.f
        # D-form integer
        D_INT = {
            "lbz": ("mem_r8", 0), "lbzu": ("mem_r8", 1),
            "lhz": ("mem_r16", 0), "lhzu": ("mem_r16", 1),
            "lha": ("mem_r16s", 0), "lhau": ("mem_r16s", 1),
            "lwz": ("mem_r32", 0), "lwzu": ("mem_r32", 1),
        }
        if mn in D_INT:
            fn, upd = D_INT[mn]
            ea = f"({ra0(f['rA'])} + {f['d']})"
            cast = "(uint32_t)(int32_t)(int16_t)mem_r16" if fn == "mem_r16s" else fn
            val = f"(uint32_t)(int16_t)mem_r16({ea})" if fn == "mem_r16s" else f"{fn}({ea})"
            s = [f"{R(f['rD'])} = {val};"]
            if upd:
                s.append(f"{R(f['rA'])} += {f['d']};")
            return s
        D_ST = {"stb": "mem_w8", "stbu": "mem_w8", "sth": "mem_w16", "sthu": "mem_w16",
                "stw": "mem_w32", "stwu": "mem_w32"}
        if mn in D_ST:
            fn = D_ST[mn]
            width = {"mem_w8": "(uint8_t)", "mem_w16": "(uint16_t)", "mem_w32": ""}[fn]
            ea = f"({ra0(f['rA'])} + {f['d']})"
            s = [f"{fn}({ea}, {width}{R(f['rD'])});"]
            if mn.endswith("u"):
                s.append(f"{R(f['rA'])} += {f['d']};")
            return s
        # FP D-form
        if mn in ("lfs", "lfsu", "lfd", "lfdu"):
            ea = f"({ra0(f['rA'])} + {f['d']})"
            rd = "mem_rf32" if mn.startswith("lfs") else "mem_rf64"
            s = [f"{F(f['frD'])} = {rd}({ea});"]
            # Gekko scalar single loads fill both paired-single lanes.
            if mn.startswith("lfs"):
                s.append(f"ctx->ps1[{f['frD']}] = {F(f['frD'])};")
            if mn.endswith("u"):
                s.append(f"{R(f['rA'])} += {f['d']};")
            return s
        if mn in ("stfs", "stfsu", "stfd", "stfdu"):
            ea = f"({ra0(f['rA'])} + {f['d']})"
            wr = "mem_wf32" if mn.startswith("stfs") else "mem_wf64"
            cast = "(float)" if mn.startswith("stfs") else ""
            s = [f"{wr}({ea}, {cast}{F(f['frD'])});"]
            if mn.endswith("u"):
                s.append(f"{R(f['rA'])} += {f['d']};")
            return s
        # X-form indexed
        X_INT = {"lbzx": "mem_r8", "lbzux": "mem_r8", "lhzx": "mem_r16", "lhzux": "mem_r16",
                 "lwzx": "mem_r32", "lwzux": "mem_r32"}
        if mn in X_INT:
            fn = X_INT[mn]
            ea = f"({ra0(f['rA'])} + {R(f['rB'])})"
            s = [f"{R(f['rD'])} = {fn}({ea});"]
            if mn.endswith("ux"):
                s.append(f"{R(f['rA'])} += {R(f['rB'])};")
            return s
        if mn in ("lhax", "lhaux"):
            ea = f"({ra0(f['rA'])} + {R(f['rB'])})"
            s = [f"{R(f['rD'])} = (uint32_t)(int16_t)mem_r16({ea});"]
            if mn.endswith("ux"):
                s.append(f"{R(f['rA'])} += {R(f['rB'])};")
            return s
        X_ST = {"stbx": ("mem_w8", "(uint8_t)"), "stbux": ("mem_w8", "(uint8_t)"),
                "sthx": ("mem_w16", "(uint16_t)"), "sthux": ("mem_w16", "(uint16_t)"),
                "stwx": ("mem_w32", ""), "stwux": ("mem_w32", "")}
        if mn in X_ST:
            fn, cast = X_ST[mn]
            ea = f"({ra0(f['rA'])} + {R(f['rB'])})"
            s = [f"{fn}({ea}, {cast}{R(f['rD'])});"]
            if mn.endswith("ux"):
                s.append(f"{R(f['rA'])} += {R(f['rB'])};")
            return s
        if mn in ("lfsx", "lfsux", "lfdx", "lfdux"):
            ea = f"({ra0(f['rA'])} + {R(f['rB'])})"
            rd = "mem_rf32" if mn.startswith("lfs") else "mem_rf64"
            s = [f"{F(f['rD'])} = {rd}({ea});"]   # rD field holds the FP reg here
            if mn.startswith("lfs"):
                s.append(f"ctx->ps1[{f['rD']}] = {F(f['rD'])};")
            if "u" in mn[3:]:
                s.append(f"{R(f['rA'])} += {R(f['rB'])};")
            return s
        if mn in ("stfsx", "stfsux", "stfdx", "stfdux", "stfiwx"):
            ea = f"({ra0(f['rA'])} + {R(f['rB'])})"
            if mn == "stfiwx":
                return [f"{{ uint32_t _u; memcpy(&_u,&{F(f['rD'])},4); mem_w32({ea}, _u); }}"]
            wr = "mem_wf32" if mn.startswith("stfs") else "mem_wf64"
            cast = "(float)" if mn.startswith("stfs") else ""
            s = [f"{wr}({ea}, {cast}{F(f['rD'])});"]
            if "u" in mn[4:]:
                s.append(f"{R(f['rA'])} += {R(f['rB'])};")
            return s
        # byte-reverse and misc
        if mn == "lwbrx":
            return [f"{R(f['rD'])} = BSWAP32(mem_r32(({ra0(f['rA'])} + {R(f['rB'])})));"]
        if mn == "stwbrx":
            return [f"mem_w32(({ra0(f['rA'])} + {R(f['rB'])}), BSWAP32({R(f['rD'])}));"]
        if mn == "lhbrx":
            return [f"{R(f['rD'])} = BSWAP16((uint16_t)mem_r16(({ra0(f['rA'])} + {R(f['rB'])})));"]
        if mn == "sthbrx":
            return [f"mem_w16(({ra0(f['rA'])} + {R(f['rB'])}), BSWAP16((uint16_t){R(f['rD'])}));"]
        if mn == "lwarx":
            ea = f"({ra0(f['rA'])} + {R(f['rB'])})"
            return [f"ctx->reserve_addr = {ea}; ctx->reserve_valid = 1; {R(f['rD'])} = mem_r32({ea});"]
        if mn == "stwcx.":
            ea = f"({ra0(f['rA'])} + {R(f['rB'])})"
            return [f"if(ctx->reserve_valid){{ mem_w32({ea}, {R(f['rD'])}); cr_set_field(ctx,0,2|(ctx->xer>>31&1)); ctx->reserve_valid=0;}} "
                    f"else cr_set_field(ctx,0,(ctx->xer>>31&1));"]
        if mn in ("lmw", "stmw"):
            # multiple-word; expand as a loop over registers rD..31
            ea0 = f"({ra0(f['rA'])} + {f['d']})"
            if mn == "lmw":
                return [f"for(int _i={f['rD']},_o=0;_i<32;_i++,_o+=4) ctx->{_T['gpr']}[_i]=mem_r32({ea0}+_o);"]
            return [f"for(int _i={f['rD']},_o=0;_i<32;_i++,_o+=4) mem_w32({ea0}+_o, ctx->{_T['gpr']}[_i]);"]
        return None

    def emit_spr(self, ins, mn):
        f = ins.f
        if mn == "mflr":
            return [f"{R(f['rD'])} = ctx->lr;"]
        if mn == "mtlr":
            return [f"ctx->lr = {R(f['rD'])};"]
        if mn == "mfctr":
            return [f"{R(f['rD'])} = ctx->ctr;"]
        if mn == "mtctr":
            return [f"ctx->ctr = {R(f['rD'])};"]
        if mn == "mfxer":
            return [f"{R(f['rD'])} = ctx->xer;"]
        if mn == "mtxer":
            return [f"ctx->xer = {R(f['rD'])};"]
        if mn == "mfcr":
            return [f"{R(f['rD'])} = ctx->cr;"]
        if mn == "mtcrf":
            crm = f['crm']
            mask = 0
            for i in range(8):
                if crm & (1 << (7 - i)):
                    mask |= 0xF << ((7 - i) * 4)
            return [f"ctx->cr = (ctx->cr & ~{hexu(mask)}) | ({R(f['rD'])} & {hexu(mask)});"]
        if mn in ("mftb", "mftbu"):
            return [f"{R(f['rD'])} = hle_timebase({0 if mn=='mftb' else 1});"]
        if mn.startswith("mf") and mn not in ("mfmsr", "mfsr"):   # mf<spr> generic
            return [f"{R(f['rD'])} = ctx->{self._spr_field(f.get('spr'))};" if f.get('spr') is not None
                    else f"/* {mn} */"]
        if mn.startswith("mt") and mn not in ("mtmsr", "mtsr", "mtsrin"):
            if f.get('spr') is not None:
                if f['spr'] in (922, 923):
                    return [f"hle_write_spr(ctx, {f['spr']}, {R(f['rD'])});"]
                return [f"ctx->{self._spr_field(f['spr'])} = {R(f['rD'])};"]
            return [f"/* {mn} */"]
        if mn == "mfmsr":
            return [f"{R(f['rD'])} = ctx->msr;"]
        if mn == "mtmsr":
            return [f"ctx->msr = {R(f['rD'])};"]
        if mn in ("mtsr", "mtsrin", "mfsr"):
            return [f"/* {mn} (segment reg, ignored) */"]
        return None

    def _spr_field(self, spr):
        """Map an SPR number to a Context field.

        Everything not modelled by name lands in the generic spr[] array. It used
        to alias gqr[0], which silently corrupted it: SRR0/SRR1 are written on
        every exception path, and GQR0 is float-by-convention -- compilers assume
        it and emit psq_l/psq_st that depend on it, so clobbering it turns every
        paired-single load in the program into garbage."""
        m = {1: "xer", 8: "lr", 9: "ctr"}
        if spr in m:
            return m[spr]
        if 912 <= spr <= 919:
            return f"gqr[{spr - 912}]"
        return f"spr[{spr & 1023}]"

    def emit_branch(self, ins, mn, a, fstart, fend):
        f = ins.f
        nxt = a + 4
        # unconditional b / bl
        if mn in ("b", "bl", "ba", "bla"):
            tgt = (f['li'] + (0 if ins.aa else a)) & 0xFFFFFFFF
            if not ins.lk and (fstart <= tgt < fend or tgt in getattr(self,"extra_labels",())):
                return [f"RECOMP_POLL(ctx); goto L_{tgt:08X};" if tgt <= a else f"goto L_{tgt:08X};"]
            return [self.call_target(tgt, nxt, tail=not ins.lk)]
        # blr / bctr and conditional variants
        if ins.form == "branch_lr":
            if ins.lk:
                # blrl / bclrl: indirect CALL through LR (LR holds the target; the call sets LR to
                # the return address). The old code fell through and emitted a plain 'return',
                # silently skipping the call (e.g. __VIRetraceHandler's post-retrace callback).
                calllr = f"{{ uint32_t _t = ctx->lr; ctx->lr = {self.code_address(nxt)}; lookup_function(_t)(ctx); }}"
                cond = cond_expr(f['bo'], f['bi'])
                pre = "ctx->ctr--; " if uses_ctr_decrement(f['bo']) else ""
                if cond == "1":
                    return [f"{pre}{calllr}"]
                return [f"{pre}if({cond}){{ {calllr} }}"]
            if mn == "blr":
                return ["return;"]
            cond = cond_expr(f['bo'], f['bi'])
            if uses_ctr_decrement(f['bo']):
                return [f"ctx->ctr--; if({cond}) return;"]
            return [f"if({cond}) return;"]
        if ins.form == "branch_ctr":
            call = f"lookup_function(ctx->ctr)(ctx);"
            if mn.startswith("bctr") or mn == "bctr":
                if ins.lk:
                    return [f"ctx->lr = {self.code_address(nxt)}; {call}"]
                # bctr (no link) is usually a COMPUTED INTRA-FUNCTION JUMP (a switch/jump table,
                # e.g. GXGetTexBufferSize), not a call. Emitting it as call+return skips the
                # function epilogue -> the stack frame leaks. Map each in-function address to its
                # label via a switch; fall back to an indirect tail call for real function pointers.
                cases = " ".join(f"case 0x{t:08X}u: goto L_{t:08X};" for t in sorted(set(range(fstart, fend, 4)) | set(getattr(self,"extra_labels",()))))
                return [f"switch(ctx->ctr) {{ {cases} default: {call} return; }}"]
            cond = cond_expr(f['bo'], f['bi'])
            if ins.lk:
                return [f"if({cond}){{ ctx->lr={self.code_address(nxt)}; {call} }}"]
            return [f"if({cond}){{ {call} return; }}"]
        # conditional relative branch
        if ins.form == "branch_cond":
            tgt = (f['bd'] + (0 if ins.aa else a)) & 0xFFFFFFFF
            cond = cond_expr(f['bo'], f['bi'])
            pre = "ctx->ctr--; " if uses_ctr_decrement(f['bo']) else ""
            if ins.lk:
                return [f"{pre}if({cond}){{ {self.call_target(tgt, nxt, tail=False)} }}"]
            if fstart <= tgt < fend or tgt in getattr(self,"extra_labels",()):
                return [f"{pre}if({cond}) {{ " + ("RECOMP_POLL(ctx); " if tgt <= a else "") + f"goto L_{tgt:08X}; }}"]
            return [f"{pre}if({cond}){{ {self.call_target(tgt, nxt, tail=True)} }}"]
        return None

    def emit_crlogical(self, ins, mn):
        f = ins.f
        ops = {"crand": "&", "cror": "|", "crxor": "^"}
        if mn in ops:
            return [f"cr_set_bit(ctx, {f['crbD']}, cr_bit(ctx,{f['crbA']}) {ops[mn]} cr_bit(ctx,{f['crbB']}));"]
        if mn == "crnand":
            return [f"cr_set_bit(ctx, {f['crbD']}, !(cr_bit(ctx,{f['crbA']}) & cr_bit(ctx,{f['crbB']})));"]
        if mn == "crnor":
            return [f"cr_set_bit(ctx, {f['crbD']}, !(cr_bit(ctx,{f['crbA']}) | cr_bit(ctx,{f['crbB']})));"]
        if mn == "creqv":
            return [f"cr_set_bit(ctx, {f['crbD']}, !(cr_bit(ctx,{f['crbA']}) ^ cr_bit(ctx,{f['crbB']})));"]
        if mn == "crandc":
            return [f"cr_set_bit(ctx, {f['crbD']}, cr_bit(ctx,{f['crbA']}) & !cr_bit(ctx,{f['crbB']}));"]
        if mn == "crorc":
            return [f"cr_set_bit(ctx, {f['crbD']}, cr_bit(ctx,{f['crbA']}) | !cr_bit(ctx,{f['crbB']}));"]
        if mn == "crclr":
            return [f"cr_set_bit(ctx, {f['crbD']}, 0);"]
        if mn == "crset":
            return [f"cr_set_bit(ctx, {f['crbD']}, 1);"]
        if mn == "mcrf":
            return ["/* mcrf TODO */"]
        return None

    def emit_fp(self, ins, mn):
        f = ins.f
        A, B, C, D = (f.get('frA'), f.get('frB'), f.get('frC'), f.get('frD'))
        def fr(n): return F(n)
        single = mn.endswith("s") and mn not in ("fabs", "fnabs")
        base = mn[:-1] if single else mn
        two = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/"}
        if base in two:
            r = f"{fr(A)} {two[base]} {fr(B)}" if base != "fmul" else f"{fr(A)} * {fr(C)}"
            expr = f"(float)({r})" if single else f"({r})"
            return [f"{fr(D)} = {expr};"] + ([f"ctx->ps1[{D}] = {fr(D)};"] if single else [])
        madd = {"fmadd": ("+", False), "fmsub": ("-", False), "fnmadd": ("+", True), "fnmsub": ("-", True)}
        if base in madd:
            sign, neg = madd[base]
            r = f"({fr(A)} * {fr(C)}) {sign} {fr(B)}"
            if neg:
                r = f"-({r})"
            return [f"{fr(D)} = {('(float)(' + r + ')') if single else r};"] + ([f"ctx->ps1[{D}] = {fr(D)};"] if single else [])
        if mn in ("fmr",):
            return [f"{fr(D)} = {fr(B)};"]
        if mn == "fneg":
            return [f"{fr(D)} = -{fr(B)};"]
        if mn == "fabs":
            return [f"{fr(D)} = (({fr(B)}) < 0) ? -({fr(B)}) : ({fr(B)});"]
        if mn == "fnabs":
            return [f"{fr(D)} = (({fr(B)}) < 0) ? ({fr(B)}) : -({fr(B)});"]
        if mn == "frsp":
            return [f"{fr(D)} = (float){fr(B)}; ctx->ps1[{D}] = {fr(D)};"]
        if mn in ("fctiw", "fctiwz"):
            return [f"{{ int32_t _i = (int32_t){fr(B)}; uint32_t _u=(uint32_t)_i; memcpy(&{fr(D)}, &_u, 4); }}"]
        if mn in ("fres",):
            return [f"{fr(D)} = (float)(1.0 / {fr(B)}); ctx->ps1[{D}] = {fr(D)};"]
        if mn == "frsqrte":
            # A precise reciprocal square root is sufficient here: games commonly
            # refine the hardware estimate with Newton-Raphson iterations.  Passing
            # the operand through corrupts every non-unit vector normalization.
            return [f"{fr(D)} = 1.0 / sqrt({fr(B)});"]
        if mn == "fsel":
            return [f"{fr(D)} = ({fr(A)} >= 0.0) ? {fr(C)} : {fr(B)};"]
        if mn in ("fcmpu", "fcmpo"):
            crf = (ins.word >> 23) & 7
            return [f"cr_set_field(ctx, {crf}, ({fr(A)} < {fr(B)})?8:({fr(A)} > {fr(B)})?4:({fr(A)}=={fr(B)})?2:1);"]
        if mn in ("mffs",):
            return [f"{{ uint32_t _u=ctx->fpscr; memcpy(&{fr(D)}, &_u, 4); }}"]
        if mn in ("mtfsf", "mtfsb0", "mtfsb1", "mtfsfi", "mcrfs"):
            return [f"/* {mn} (fpscr, ignored) */"]
        return None

    def emit_ps(self, ins, mn):
        f = ins.f
        if not mn.startswith(("ps_", "psq_")):
            return None

        def P0(n): return f"ctx->{_T['fpr']}[{n}]"
        def P1(n): return f"ctx->ps1[{n}]"

        # quantized paired load/store
        if mn in ("psq_l", "psq_lu", "psq_st", "psq_stu"):
            ea = f"({ra0(f['rA'])} + {f['ps_d']})"
            w, i = f['w'], f['i']
            upd = mn.endswith("u")
            fn = "ru_psq_load" if mn.startswith("psq_l") else "ru_psq_store"
            s = [f"{{ uint32_t _ea = {ea}; {fn}(ctx, {f['frD']}, _ea, {w}, {i});" +
                 (f" ctx->{_T['gpr']}[{f['rA']}] = _ea;" if upd else "") + " }"]
            return s

        A, B, C, D = f.get('frA'), f.get('frB'), f.get('frC'), f['frD']

        def two(e0, e1):
            return [f"{{ double _0=(float)({e0}); double _1=(float)({e1}); "
                    f"{P0(D)}=_0; {P1(D)}=_1; }}"]

        if mn == "ps_add":  return two(f"{P0(A)}+{P0(B)}", f"{P1(A)}+{P1(B)}")
        if mn == "ps_sub":  return two(f"{P0(A)}-{P0(B)}", f"{P1(A)}-{P1(B)}")
        if mn == "ps_mul":  return two(f"{P0(A)}*{P0(C)}", f"{P1(A)}*{P1(C)}")
        if mn == "ps_div":  return two(f"{P0(A)}/{P0(B)}", f"{P1(A)}/{P1(B)}")
        if mn == "ps_madd": return two(f"{P0(A)}*{P0(C)}+{P0(B)}", f"{P1(A)}*{P1(C)}+{P1(B)}")
        if mn == "ps_msub": return two(f"{P0(A)}*{P0(C)}-{P0(B)}", f"{P1(A)}*{P1(C)}-{P1(B)}")
        if mn == "ps_nmadd":return two(f"-({P0(A)}*{P0(C)}+{P0(B)})", f"-({P1(A)}*{P1(C)}+{P1(B)})")
        if mn == "ps_nmsub":return two(f"-({P0(A)}*{P0(C)}-{P0(B)})", f"-({P1(A)}*{P1(C)}-{P1(B)})")
        if mn == "ps_madds0":return two(f"{P0(A)}*{P0(C)}+{P0(B)}", f"{P1(A)}*{P0(C)}+{P1(B)}")
        if mn == "ps_madds1":return two(f"{P0(A)}*{P1(C)}+{P0(B)}", f"{P1(A)}*{P1(C)}+{P1(B)}")
        if mn == "ps_muls0":return two(f"{P0(A)}*{P0(C)}", f"{P1(A)}*{P0(C)}")
        if mn == "ps_muls1":return two(f"{P0(A)}*{P1(C)}", f"{P1(A)}*{P1(C)}")
        if mn == "ps_sum0": return two(f"{P0(A)}+{P1(B)}", f"{P1(C)}")
        if mn == "ps_sum1": return two(f"{P0(C)}", f"{P0(A)}+{P1(B)}")
        if mn == "ps_neg":  return two(f"-{P0(B)}", f"-{P1(B)}")
        if mn == "ps_abs":  return two(f"({P0(B)}<0?-{P0(B)}:{P0(B)})", f"({P1(B)}<0?-{P1(B)}:{P1(B)})")
        if mn == "ps_nabs": return two(f"({P0(B)}<0?{P0(B)}:-{P0(B)})", f"({P1(B)}<0?{P1(B)}:-{P1(B)})")
        if mn == "ps_mr":   return two(f"{P0(B)}", f"{P1(B)}")
        if mn == "ps_merge00": return two(f"{P0(A)}", f"{P0(B)}")
        if mn == "ps_merge01": return two(f"{P0(A)}", f"{P1(B)}")
        if mn == "ps_merge10": return two(f"{P1(A)}", f"{P0(B)}")
        if mn == "ps_merge11": return two(f"{P1(A)}", f"{P1(B)}")
        if mn == "ps_res":  return two(f"1.0f/{P0(B)}", f"1.0f/{P1(B)}")
        if mn == "ps_rsqrte": return two(f"1.0f/(float)sqrt({P0(B)})", f"1.0f/(float)sqrt({P1(B)})")
        if mn == "ps_sel":  return two(f"({P0(A)}>=0?{P0(C)}:{P0(B)})", f"({P1(A)}>=0?{P1(C)}:{P1(B)})")
        if mn in ("ps_cmpu0", "ps_cmpo0", "ps_cmpu1", "ps_cmpo1"):
            crf = (ins.word >> 23) & 7
            la, lb = (P0(A), P0(B)) if mn.endswith("0") else (P1(A), P1(B))
            return [f"cr_set_field(ctx, {crf}, ({la}<{lb})?8:({la}>{lb})?4:({la}=={lb})?2:1);"]
        return [f"/* TODO {mn} 0x{ins.word:08X} */"]
