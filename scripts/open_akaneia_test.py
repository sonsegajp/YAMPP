"""Open the verified fighters/stages/music-only development build with real input."""
from pathlib import Path
import argparse,ctypes,hashlib,json,os,shutil,struct,subprocess,time
from project_config import ROOT,configuration,disc_path,game_environment

def digest(path):
    h=hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda:f.read(1024*1024),b""):h.update(chunk)
    return h.hexdigest()

def running_test(pid,exe):
    if os.name!="nt":return False
    kernel=ctypes.WinDLL("kernel32",use_last_error=True)
    kernel.OpenProcess.argtypes=[ctypes.c_uint32,ctypes.c_int,ctypes.c_uint32];kernel.OpenProcess.restype=ctypes.c_void_p
    kernel.QueryFullProcessImageNameW.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_uint32)]
    kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    handle=kernel.OpenProcess(0x1000,False,pid)
    if not handle:return False
    try:
        buffer=ctypes.create_unicode_buffer(32768);length=ctypes.c_uint32(len(buffer))
        return bool(kernel.QueryFullProcessImageNameW(handle,0,buffer,ctypes.byref(length))) and Path(buffer.value).resolve()==exe.resolve()
    finally:kernel.CloseHandle(handle)

def main():
    ap=argparse.ArgumentParser();ap.add_argument("--content-root",default="build/akaneia-content/content-r3");ap.add_argument("--prepare-only",action="store_true");a=ap.parse_args()
    content=(ROOT/a.content_root).resolve()
    if not content.is_relative_to((ROOT/"build/akaneia-content").resolve()):raise ValueError("Invalid content directory")
    audit=json.loads((content/"content-audit.json").read_text());verification=json.loads((content/"verification.json").read_text())
    if audit["scope"]!=["fighters","stages","music"] or not verification.get("passed"):raise ValueError("Run the selective content audit first")
    disc=content/"yampp-content.iso"
    if digest(disc)!=verification["isoSHA256"]:raise ValueError("Content image changed after verification")
    out=ROOT/"build/akaneia-content-user-test";out.mkdir(parents=True,exist_ok=True)
    exe=ROOT/"build/native-game/YAMPP-akaneia-content-test.exe";renderer=ROOT/"build/pc/bin/melee_aurora_akaneia_content_test.dll"
    launch=out/"launch.json"
    if launch.exists() and running_test(json.loads(launch.read_text()).get("pid",0),exe):
        print("The content-only test is already running; its files were left untouched.");return
    assets=out/"disc/sys";assets.mkdir(parents=True,exist_ok=True)
    with disc.open("rb") as f:
        f.seek(0x420);do,fo,fs=struct.unpack(">3I",f.read(12));f.seek(do);dh=f.read(0x100)
        ds=max(struct.unpack_from(">I",dh,i*4)[0]+struct.unpack_from(">I",dh,0x90+i*4)[0] for i in range(18))
        if not (0<ds<32*1024*1024 and 0<fs<16*1024*1024):raise ValueError("Invalid content boot metadata")
        for name,start,size in [("main.dol",do,ds),("fst.bin",fo,fs)]:
            f.seek(start);data=f.read(size)
            if len(data)!=size:raise ValueError("Truncated content image")
            (assets/name).write_bytes(data)
    shutil.copy2(ROOT/"build/native-game/YAMPP-akaneia-probe.exe",exe)
    shutil.copy2(ROOT/"build/pc/bin/melee_aurora_controllers.dll",renderer)
    if not (out/"card.raw").exists():shutil.copy2(ROOT/"build/comparisons/akaneia-content-menu-hooks/card.raw",out/"card.raw")
    if not (out/"settings.xml").exists():(out/"settings.xml").write_text('<melee-settings schema="1" width="960" height="720" renderScale="1" widescreen="0" fullscreen="0" vsync="1" mute="0" volume="100" />')
    prepared={"scope":audit["scope"],"isoSHA256":verification["isoSHA256"],"executableSHA256":digest(exe),"rendererSHA256":digest(renderer),"offlineDevelopment":True}
    (out/"prepared.json").write_text(json.dumps(prepared,indent=2))
    if a.prepare_only:print("Prepared content-only test:",exe);return
    cfg=configuration();env=game_environment(cfg,disc_path(cfg))
    for k in list(env):
        if k.startswith(("MELEE_TEST_","MELEE_CAPTURE_","MELEE_NETPLAY_","MELEE_MOD_TEST_","MELEE_AUDIO_","MELEE_PERF_","MELEE_TRACE_","GCN_GXT_")) or k in ("MELEE_INPUT","GCN_AURORA_HIDDEN","MELEE_MEX_TRACE"):env.pop(k)
    env.update(MELEE_DISC=str(disc),MELEE_FST=str(assets/"fst.bin"),MELEE_MEX_BASE_DOL=str(ROOT/"data/GALE01/sys/main.dol"),MELEE_UNICORN_LIBRARY=str(ROOT/"build/pc/bin/unicorn.dll"),MELEE_AURORA_DLL=str(renderer),MELEE_SETTINGS=str(out/"settings.xml"),MELEE_MEMORY_CARD=str(out/"card.raw"),MELEE_COSTUME_REGISTRY=str(out/"empty-costumes.tsv"),MELEE_MOD_REGISTRY=str(out/"empty-registry.tsv"),GCN_AURORA_DATA_DIR=str(out/"cache"))
    env["PATH"]=str(ROOT/"build/pc/bin")+";C:/msys64/mingw64/bin;"+env["PATH"]
    with (out/"run.log").open("w") as log:
        proc=subprocess.Popen([str(exe),str(assets/"main.dol"),"0"],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
    launch.write_text(json.dumps({"pid":proc.pid,"executable":str(exe),"renderer":str(renderer),"started":time.time(),**prepared},indent=2))
    print("Opened content-only Akaneia development PID",proc.pid)
if __name__=="__main__":main()
