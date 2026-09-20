#!/usr/bin/env python3
"""
Recompiler driver: DOL + symbol map -> a set of C translation units that link.

Emits ALL functions from the map, split into chunk files (a single multi-million-line
TU would choke the compiler), a shared forward-declaration header, and one table file.

Usage:
  python recompile.py <main.dol> <framework.map> <out_dir> [chunk_size]
"""
import os
import sys

sys.path.insert(0, ".")
from dol import Dol
from symbols import parse_map, Symbol
from discover import discover_text0, discover_unmapped_calls
from emit import Emitter
from overrides import OVERRIDES


def main():
    dol_path, map_path, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    chunk = int(sys.argv[4]) if len(sys.argv) > 4 else 1000
    os.makedirs(out_dir, exist_ok=True)

    dol = Dol(dol_path)
    funcs, _ = parse_map(map_path)
    mapset = {fn.vaddr for fn in funcs}
    # Emit unmapped bl-target helpers (CW _savegpr/_restgpr etc.), excluding text0 (emitted
    # separately). Without these, `bl _savegpr` no-ops and non-volatile regs get clobbered.
    text0set = {a for a, _, _ in discover_text0(dol)}
    helpers = discover_unmapped_calls(dol, mapset | text0set)
    helper_syms = [Symbol(a, sz, nm, "helpers", ".text") for a, sz, nm in helpers]
    funcs = funcs + helper_syms
    funcset = mapset | text0set | {a for a, _, _ in helpers}
    em = Emitter(funcset)
    print(f"emitting {len(funcs)} functions ({len(helper_syms)} unmapped helpers) "
          f"in chunks of {chunk} -> {out_dir}")

    # shared header: forward declarations + table type
    with open(os.path.join(out_dir, "recomp_funcs.h"), "w", encoding="utf-8") as h:
        h.write("#ifndef RECOMP_FUNCS_H\n#define RECOMP_FUNCS_H\n#include \"recomp.h\"\n")
        for fn in funcs:
            h.write(f"void func_{fn.vaddr:08X}(Context*);\n")
        h.write("typedef struct { uint32_t addr; RecFn fn; } FnEnt;\n")
        h.write("extern FnEnt g_fnents[];\n")
        h.write("extern const int g_fnents_count;\n")
        h.write("#endif\n")

    stubbed = []
    nfiles = 0
    for ci, base in enumerate(range(0, len(funcs), chunk)):
        sel = funcs[base:base + chunk]
        path = os.path.join(out_dir, f"recomp_{ci:03d}.c")
        with open(path, "w", encoding="utf-8") as out:
            out.write('#include "recomp_funcs.h"\n#include <string.h>\n#include <math.h>\n\n')
            for fn in sel:
                if fn.vaddr in OVERRIDES:
                    out.write(f"/* {fn.name} @0x{fn.vaddr:08X}: HLE override (see runtime/hle_*.c) */\n\n")
                    continue
                try:
                    out.write(em.emit_function(dol, fn.vaddr, fn.size, fn.name))
                except Exception as e:
                    # linkable stub so one bad function can't break the whole build
                    stubbed.append((fn.vaddr, fn.name, repr(e)))
                    out.write(f"/* STUB {fn.name} @0x{fn.vaddr:08X}: {e} */\n")
                    out.write(f"void func_{fn.vaddr:08X}(Context* ctx) {{ (void)ctx; }}")
                out.write("\n\n")
        nfiles += 1

    # table file
    with open(os.path.join(out_dir, "recomp_table.c"), "w", encoding="utf-8") as t:
        t.write('#include "recomp_funcs.h"\n\n')
        t.write("FnEnt g_fnents[] = {\n")
        for fn in funcs:
            t.write(f"  {{0x{fn.vaddr:08X}u, func_{fn.vaddr:08X}}},\n")
        t.write("};\n")
        t.write("const int g_fnents_count = (int)(sizeof(g_fnents)/sizeof(g_fnents[0]));\n")

    print(f"done: {len(funcs)} functions across {nfiles} chunk files + table")
    print(f"stubbed (emit exceptions): {len(stubbed)}")
    for a, n, e in stubbed[:20]:
        print(f"  0x{a:08X} {n}: {e}")


if __name__ == "__main__":
    main()
