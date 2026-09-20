#!/usr/bin/env python3
"""
Recompiler driver (game-agnostic): DOL + symbol map -> C translation units that link.

Game specifics live in games/<id>/game.py; this file has no per-game knowledge.
Symbol sources: "dtk" (decomp-toolkit symbols.txt) or "map" (CodeWarrior .map).

Usage:  python recomp_main.py <game_id> <out_dir> [chunk_size]
"""
import importlib.util
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol import Dol
from symbols import Symbol
from discover import discover_text0, discover_unmapped_calls
from emit import Emitter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_game(game_id):
    path = os.path.join(ROOT, "games", game_id, "game.py")
    spec = importlib.util.spec_from_file_location(f"game_{game_id}", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_symbols(g):
    if g.SYMFMT == "dtk":
        from dtk_symbols import parse_symbols
        return parse_symbols(g.SYMBOLS)
    from symbols import parse_map
    return parse_map(g.SYMBOLS)


def main():
    game_id, out_dir = sys.argv[1], sys.argv[2]
    chunk = int(sys.argv[3]) if len(sys.argv) > 3 else 500
    # Target ABI: "native" (our standalone runtime) or "recompcore" (StaticRecompABI v2
    # module loadable by RecompCore / ModernGekko / GXRuntime).
    abi = sys.argv[4] if len(sys.argv) > 4 else "native"
    import emit as _emit
    _emit.set_abi(abi)
    hdr = "abi_recompcore.h" if abi == "recompcore" else "recomp.h"
    g = load_game(game_id)
    os.makedirs(out_dir, exist_ok=True)

    dol = Dol(g.DOL)
    funcs, _ = load_symbols(g)
    mapset = {fn.vaddr for fn in funcs}
    print(f"{g.NAME} [{g.ID}]  entry=0x{dol.entry:08X}  abi={abi}  mapped funcs={len(funcs)}")

    # text0 bootstrap: emit only what the symbol map doesn't already cover.
    t0 = [t for t in discover_text0(dol) if t[0] not in mapset]
    # CodeWarrior _savegpr/_restgpr et al: bl targets with no symbol. Without these,
    # `bl _savegpr` no-ops and non-volatile registers get silently clobbered.
    known = mapset | {a for a, _, _ in t0}
    helpers = discover_unmapped_calls(dol, known)

    extra = ([Symbol(a, sz, nm, "boot", ".init") for a, sz, nm in t0] +
             [Symbol(a, sz, nm, "helper", ".text") for a, sz, nm in helpers])
    funcs = sorted(funcs + extra, key=lambda s: s.vaddr)
    funcset = known | {a for a, _, _ in helpers}
    print(f"  + {len(t0)} text0 boot funcs, + {len(helpers)} unmapped call helpers "
          f"=> {len(funcs)} total")

    em = Emitter(funcset, getattr(g, 'PROBES', ()))

    with open(os.path.join(out_dir, "recomp_funcs.h"), "w", encoding="utf-8") as h:
        h.write(f'#ifndef RECOMP_FUNCS_H\n#define RECOMP_FUNCS_H\n#include "{hdr}"\n')
        for fn in funcs:
            h.write(f"void func_{fn.vaddr:08X}(Context*);\n")
        h.write("typedef struct { uint32_t addr; RecFn fn; } FnEnt;\n")
        h.write("extern FnEnt g_fnents[];\nextern const int g_fnents_count;\n#endif\n")

    stubbed, nfiles = [], 0
    for ci, base in enumerate(range(0, len(funcs), chunk)):
        path = os.path.join(out_dir, f"recomp_{ci:04d}.c")
        with open(path, "w", encoding="utf-8") as out:
            out.write('#include "recomp_funcs.h"\n#include <string.h>\n#include <math.h>\n\n')
            for fn in funcs[base:base + chunk]:
                if fn.vaddr in g.OVERRIDES:
                    out.write(f"/* {fn.name} @0x{fn.vaddr:08X}: HLE override */\n\n")
                    continue
                try:
                    out.write(em.emit_function(dol, fn.vaddr, fn.size, fn.name))
                except Exception as e:
                    stubbed.append((fn.vaddr, fn.name, repr(e)))
                    out.write(f"/* STUB {fn.name} @0x{fn.vaddr:08X}: {e} */\n")
                    out.write(f"void func_{fn.vaddr:08X}(Context* ctx) {{ (void)ctx; }}")
                out.write("\n\n")
        nfiles += 1

    with open(os.path.join(out_dir, "recomp_table.c"), "w", encoding="utf-8") as t:
        t.write('#include "recomp_funcs.h"\n\nFnEnt g_fnents[] = {\n')
        for fn in funcs:
            t.write(f"  {{0x{fn.vaddr:08X}u, func_{fn.vaddr:08X}}},\n")
        t.write("};\nconst int g_fnents_count = (int)(sizeof(g_fnents)/sizeof(g_fnents[0]));\n")

    print(f"done: {len(funcs)} functions across {nfiles} chunk files + table")
    print(f"stubbed (emit exceptions): {len(stubbed)}")
    for a, n, e in stubbed[:20]:
        print(f"  0x{a:08X} {n}: {e}")


if __name__ == "__main__":
    main()
