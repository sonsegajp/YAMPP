#!/usr/bin/env python3
"""
Emit the text0 bootstrap functions (not in framework.map) into their own TU with a
separate function table, so integrating them doesn't force a rebuild of the main
2.67M-line body. The main map functions must be in `funcset` too so text0->text1
calls emit as direct func_XXXX calls.

Usage: python gen_text0.py <main.dol> <framework.map> <out.c>
"""
import sys

sys.path.insert(0, ".")
from dol import Dol
from symbols import parse_map
from discover import discover_text0, discover_unmapped_calls
from emit import Emitter


def main():
    dol = Dol(sys.argv[1])
    funcs, _ = parse_map(sys.argv[2])
    out_path = sys.argv[3]

    text0 = discover_text0(dol)
    mapset = {f.vaddr for f in funcs}
    text0set = {a for a, _, _ in text0}
    helpers = discover_unmapped_calls(dol, mapset | text0set)   # for forward-decls/funcset
    # funcset = map + text0 + unmapped helpers (so every direct call resolves)
    funcset = mapset | text0set | {a for a, _, _ in helpers}
    em = Emitter(funcset)

    with open(out_path, "w", encoding="utf-8") as out:
        out.write('#include "recomp.h"\n#include <string.h>\n#include <math.h>\n\n')
        # forward-declare every function this TU may call directly (text0 + map)
        for a, _, _ in text0:
            out.write(f"void func_{a:08X}(Context*);\n")
        # map functions referenced by text0 are declared in the main header, but this
        # TU doesn't include it; declare all map funcs here too (cheap).
        for f in funcs:
            out.write(f"void func_{f.vaddr:08X}(Context*);\n")
        for a, _, _ in helpers:                        # unmapped helpers (defined in chunks)
            out.write(f"void func_{a:08X}(Context*);\n")
        out.write("\n")
        for a, sz, name in text0:
            try:
                out.write(em.emit_function(dol, a, sz, name))
            except Exception as e:
                out.write(f"/* STUB {name}: {e} */\nvoid func_{a:08X}(Context* ctx){{(void)ctx;}}")
            out.write("\n\n")
        # separate text0 table
        out.write("typedef struct { uint32_t addr; RecFn fn; } FnEnt;\n")
        out.write("FnEnt g_fnents_text0[] = {\n")
        for a, _, _ in text0:
            out.write(f"  {{0x{a:08X}u, func_{a:08X}}},\n")
        out.write("};\n")
        out.write("const int g_fnents_text0_count = (int)(sizeof(g_fnents_text0)/sizeof(g_fnents_text0[0]));\n")
    print(f"text0: emitted {len(text0)} functions -> {out_path} (entry 0x{dol.entry:08X})")


if __name__ == "__main__":
    main()
