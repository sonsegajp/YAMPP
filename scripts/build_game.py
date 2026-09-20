"""Build the native Melee executable. Reuses unchanged generated translation units."""
import argparse, concurrent.futures, pathlib, subprocess, os, sys, hashlib, json, platform
ROOT = pathlib.Path(__file__).resolve().parents[1]
IS_WINDOWS = platform.system() == "Windows"
BUILD = ROOT / ("build/native-game" if IS_WINDOWS else "build/native-game-linux")
GENERATED = ROOT / "build/native-game/generated"
if IS_WINDOWS:
    CC = pathlib.Path("C:/msys64/mingw64/bin/gcc.exe")
    os.environ["PATH"] = str(CC.parent) + os.pathsep + os.environ["PATH"]
    EXE_SUFFIX = ".exe"
else:
    CC = pathlib.Path(os.environ.get("CC", "gcc"))
    EXE_SUFFIX = ""
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--rebuild-game", action="store_true")
    ap.add_argument("--hot-reload", action="store_true", help="Build the persistent Windows host and reloadable native core")
    ap.add_argument("--optimization", choices=["0","1","2","3"], default="2")
    ap.add_argument("--output-name", default="melee" + EXE_SUFFIX)
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    a=ap.parse_args()
    if a.jobs < 1: ap.error("--jobs must be positive")
    if not (ROOT/"upstream/lua-5.5.1/src/lua.h").is_file():
        subprocess.run([sys.executable,str(ROOT/"scripts/build_lua.py")],check=True)
    includes=[ROOT/"native/runtime", ROOT/"native/vendor/include", GENERATED]
    flags=["-O"+a.optimization, "-g", "-fno-strict-aliasing"] + ["-I"+str(p) for p in includes]
    if not IS_WINDOWS:
        flags += ["-D_GNU_SOURCE", "-I"+str(ROOT/"native/host/gxrt")]
    if a.hot_reload and not IS_WINDOWS: ap.error("Persistent host currently requires Windows")
    config_path=BUILD/"build-config.json"
    # Standalone tests have substitute ABI headers that are never included by
    # the game. Changing them must not invalidate every generated game unit.
    tests_dir=ROOT/"native/host/gxrt/tests"
    headers=sorted({p for d in includes for p in d.rglob("*.h")})
    host_headers=sorted({p for p in (ROOT/"native/host").rglob("*.h")
                         if tests_dir not in p.parents and "aurora_shim" not in p.parts})
    hashes=lambda paths:{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    config={"flags":flags,"headers":hashes(headers),"host_headers":hashes(host_headers)}
    previous=json.loads(config_path.read_text()) if config_path.exists() else None
    if previous:
        # Migrate the old mixed fingerprint without recompiling unchanged game C.
        previous["headers"]={k:v for k,v in previous.get("headers",{}).items()
                             if not k.replace(chr(92),"/").startswith("native/host/")}
    rebuild_all=not previous or previous.get("flags")!=flags or previous.get("headers")!=config["headers"]
    rebuild_host=rebuild_all or previous.get("host_headers")!=config["host_headers"]
    sources=list((ROOT/"native/host/gxrt").glob("*.c"))+[ROOT/"native/runtime/module_rt.c"]
    names=["core/cpu","loader","boot","aram","dvd","event_clock","guest_memory","gx_recomp",
           "interrupts","si","exi","di","mmio_bus","cp","pe","mi","efb_access","yuyv_encode",
           "yuyv_decode","gap_report","platform","audio_dma","ax_host","vi_clock","memory_card",
           "headless_backend","savestate","rvz"]
    sources += [ROOT/"native/vendor/src"/(n+".c") for n in names]
    (BUILD/"host-objects").mkdir(parents=True,exist_ok=True)
    (BUILD/"objects").mkdir(parents=True,exist_ok=True)
    objects=[]
    jobs=[]
    for src in sources:
        obj=BUILD/"host-objects"/(src.stem+".o")
        objects.append(obj)
        source_time=max([src.stat().st_mtime]+[p.stat().st_mtime for p in src.parent.glob("*.inc")])
        if src.name == "mex_runtime.c":
            source_time=max(source_time,(GENERATED/"mex_functions.inc").stat().st_mtime)
        if rebuild_host or not obj.exists() or source_time>obj.stat().st_mtime:
            jobs.append((src,obj))
    for src in sorted(GENERATED.glob("*.c")):
        obj=BUILD/"objects"/(src.stem+".o")
        objects.append(obj)
        if rebuild_all or a.rebuild_game or not obj.exists() or src.stat().st_mtime > obj.stat().st_mtime:
            jobs.append((src,obj))
    def compile(job):
        src,obj=job
        r=subprocess.run([str(CC),"-c",*flags,str(src),"-o",str(obj)],capture_output=True,text=True)
        if r.returncode: raise RuntimeError(str(src)+"\n"+r.stdout+r.stderr)
        print("compiled",src.relative_to(ROOT),flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        list(pool.map(compile,jobs))
    if IS_WINDOWS:
        lua_lib=BUILD/"liblua55.dll.a"
        if not lua_lib.exists():subprocess.run([sys.executable,str(ROOT/"scripts/build_lua.py")],check=True)
        objects.append(lua_lib)
    else:
        lua_lib=BUILD/"liblua55.a"
        lua_source=ROOT/"upstream/lua-5.5.1/src"
        lua_files=sorted(p for p in lua_source.glob("*.c") if p.name not in ("lua.c", "luac.c", "onelua.c"))
        if not lua_files: raise RuntimeError("Run scripts/build_lua.py to download the pinned Lua sources first")
        if not lua_lib.exists() or any(p.stat().st_mtime>lua_lib.stat().st_mtime for p in lua_files):
            lua_objects=BUILD/"lua-objects";lua_objects.mkdir(exist_ok=True)
            compiled=[]
            for source in lua_files:
                obj=lua_objects/(source.stem+".o")
                subprocess.run([str(CC),"-O2","-fPIC","-DLUA_USE_LINUX","-c",str(source),"-o",str(obj)],check=True)
                compiled.append(str(obj))
            subprocess.run(["ar","rcs",str(lua_lib),*compiled],check=True)
        objects.append(lua_lib)
    rsp=BUILD/"link.rsp"
    if IS_WINDOWS:
        link_libs="-lm -luser32 -lws2_32 -lmfplat -lmfreadwrite -lmfuuid -lole32 -lturbojpeg -lzstd -lssl -lcrypto -lcrypt32"
    else:
        link_libs="-lm -lpthread -ldl -lturbojpeg -lzstd -lavformat -lavcodec -lavutil -lswresample -lssl -lcrypto"
    rsp.write_text("\n".join('"'+str(o).replace("\\","/")+'"' for o in objects)+"\n"+link_libs+"\n")
    if a.hot_reload:
        core=(BUILD/a.output_name).with_suffix(".core.dll")
        exports=BUILD/"hotload.def";exports.write_text("EXPORTS\n yampp_runtime_main=main\n")
        subprocess.run([str(CC),"-shared","-Wl,--exclude-all-symbols","-o",str(core),"@"+str(rsp),str(exports)],check=True)
        subprocess.run([str(CC),"-O2","-g",str(ROOT/"native/host/hotload_main.c"),"-o",str(BUILD/a.output_name),"-luser32"],check=True)
    else:
        subprocess.run([str(CC),"-o",str(BUILD/a.output_name),"@"+str(rsp)],check=True)
    config_path.write_text(json.dumps(config,indent=2))
    print(BUILD/a.output_name)
if __name__=="__main__": main()
