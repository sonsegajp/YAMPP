"""Regenerate compiler output from the DOL and recorded function boundaries."""
from pathlib import Path
import argparse,json,sys,hashlib,xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"native/recompiler"))
from dol import Dol
import emit
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--unit",action="append")
    a=ap.parse_args()
    dol_path=ROOT/"data/GALE01/sys/main.dol"
    assert hashlib.sha1(dol_path.read_bytes()).hexdigest()=="08e0bf20134dfcb260699671004527b2d6bb1a45"
    manifest=json.loads((ROOT/"native/function_catalog.json").read_text())
    addresses={f["address"] for u in manifest for f in u["functions"]}
    generated=ROOT/"build/native-game/generated"
    generated.mkdir(parents=True,exist_ok=True)
    ordered=[f for u in manifest for f in u["functions"]]
    declarations='#ifndef RECOMP_FUNCS_H\n#define RECOMP_FUNCS_H\n#include "abi_recompcore.h"\n'
    declarations+=''.join('void func_%08X(Context*);\n'%f['address'] for f in ordered)
    declarations+='typedef struct { uint32_t addr; RecFn fn; } FnEnt;\nextern FnEnt g_fnents[];\nextern const int g_fnents_count;\n#endif\n'
    dispatch='#include "recomp_funcs.h"\n\nFnEnt g_fnents[] = {\n'
    dispatch+=''.join('  {0x%08Xu, func_%08X},\n'%(f['address'],f['address']) for f in ordered)
    dispatch+='};\nconst int g_fnents_count = (int)(sizeof(g_fnents)/sizeof(g_fnents[0]));\n'
    mex_catalog='/* Generated from function_catalog.json; contains no guest instructions. */\n'
    mex_catalog+='static const struct { uint32_t address,size; } mex_functions[] = {\n'
    mex_catalog+=''.join('  {0x%08Xu, %du},\n'%(f['address'],f.get('size',4)) for f in ordered)
    mex_catalog+='};\n'
    for name,content in [('recomp_funcs.h',declarations),('recomp_table.c',dispatch),('mex_functions.inc',mex_catalog)]:
        output=generated/name
        if not output.exists() or output.read_text()!=content:output.write_text(content)
    emit.set_abi("recompcore")
    em=emit.Emitter(addresses,tuple(range(0x8038F4F0,0x8038F6C0,4)))
    em.setjmp_address=0x803227CC
    hooks=ET.parse(ROOT/"config/runtime-hooks.xml").getroot()
    assert hooks.get("dol-sha1")==hashlib.sha1(dol_path.read_bytes()).hexdigest()
    em.runtime_hooks={int(e.attrib["address"],16) for e in hooks.findall("function")}
    assert em.runtime_hooks.issubset(addresses)
    dol=Dol(str(dol_path))
    from generate_mex_fastpaths import generate
    generate(em,dol,ordered,generated)
    for unit in manifest:
        if a.unit and unit["unit"] not in a.unit: continue
        chunks=['#include "recomp_funcs.h"\n#include <string.h>\n#include <math.h>\n\n']
        for f in unit["functions"]:
            if f.get("override"):
                chunks.append(f'/* {f["name"]} @0x{f["address"]:08X}: HLE override */\n\n')
            else:
                chunks.append(em.emit_function(dol,f["address"],f["size"],f["name"])+"\n\n")
        out=ROOT/"build/native-game/generated"/unit["unit"]
        content="".join(chunks)
        if out.exists() and out.read_text()==content: continue
        out.write_text(content)
        # Force this unit to rebuild, without modifying any generated code by hand.
        print("regenerated",out.name,len(unit["functions"]))
if __name__=="__main__": main()
