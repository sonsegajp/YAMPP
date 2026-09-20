"""Run a stock Dolphin core headlessly with an isolated profile and capture its game output."""
from pathlib import Path
import argparse,subprocess,struct,os,json,time
ROOT=Path(__file__).resolve().parents[1]
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--name",default="startup")
    ap.add_argument("--seconds",type=int,default=10)
    ap.add_argument("--press",action="append",default=[],help="DTM poll:button (a/start/up/down)")
    ap.add_argument("--timeline")
    ap.add_argument("--fifo-frame",type=int)
    ap.add_argument("--second-controller",action="store_true")
    ap.add_argument("--maze",action="store_true")
    ap.add_argument("--fifo-replay",type=Path)
    ap.add_argument("--sparse-captures",action="store_true")
    a=ap.parse_args()
    out=ROOT/"build/comparisons"/a.name/"dolphin"
    user=out/"profile"
    frames=out/"frames"
    (user/"Config").mkdir(parents=True,exist_ok=True)
    frames.mkdir(parents=True,exist_ok=True)
    (user/"Config/Dolphin.ini").write_text("[Core]\nCPUThread = False\nEnableCheats = False\nSkipIPL = True\nSIDevice0 = 6\nSIDevice1 = 0\nSIDevice2 = 0\nSIDevice3 = 0\nSlotA = 255\nSlotB = 255\nEmulationSpeed = 1.0\n[Interface]\nConfirmStop = False\nUsePanicHandlers = False\n[Movie]\nDumpFrames = True\nDumpFramesSilent = True\n[DSP]\nBackend = No audio output\nDSPHLE = True\nDumpAudio = True\nDumpAudioSilent = True\n")
    if a.sparse_captures:
        ini=user/"Config/Dolphin.ini";ini.write_text(ini.read_text().replace("DumpFrames = True","DumpFrames = False").replace("DumpAudio = True","DumpAudio = False"))
    if a.second_controller:
        cfg=user/"Config/Dolphin.ini";cfg.write_text(cfg.read_text().replace("SIDevice1 = 0","SIDevice1 = 6"))
    (user/"Config/GFX.ini").write_text("[Settings]\nDumpFramesAsImages = True\nDumpPath = "+str(frames).replace("\\","/")+"/\nFrameDumpsResolutionType = 0\nInternalResolution = 1\nAspectRatio = 2\n[Hardware]\nVSync = False\n[Hacks]\nEFBToTextureEnable = True\nXFBToTextureEnable = True\n")
    h=bytearray(256)
    h[:4]=b"DTM\x1a";h[4:10]=b"GALE01";h[11]=1
    n=10000
    struct.pack_into("<QQ",h,13,n,n);struct.pack_into("<Q",h,237,0xffffffffffffffff)
    presses=[]
    for p in a.press:
        poll,button=p.split(":")
        presses.append((int(poll),{"a":2,"start":1,"down":128,"up":64}[button]))
    movie=out/"input.dtm"
    with movie.open("wb") as f:
        f.write(h)
        for i in range(n):
            bits=0x4000
            for first,button in presses:
                if first<=i<first+8: bits|=button
            f.write(struct.pack("<H6B",bits,0,0,128,128,128,128))
    exe=ROOT/"build/dolphin-oracle/DolphinNoGUI.exe"
    from project_config import disc_path
    disc=a.fifo_replay.resolve() if a.fifo_replay else disc_path()
    args=[str(exe),"-p","headless","-v","Vulkan","-u",str(user),"-m",str(movie),"-e",str(disc)]
    env=os.environ.copy()
    if a.maze: env["MELEE_ORACLE_MAZE"]="1"
    if a.sparse_captures: env["MELEE_ORACLE_CAPTURES"]=str(frames)
    if a.timeline is not None:
        args.remove("-m"); args.remove(str(movie))
        env["MELEE_ORACLE_INPUT"]=a.timeline
    if a.fifo_frame is not None:
        env["MELEE_ORACLE_FIFO"]=str(out/"reference.dff")
        env["MELEE_ORACLE_FIFO_FRAME"]=str(a.fifo_frame)
    with (out/"run.log").open("w") as log:
        proc=subprocess.Popen(args,stdout=log,stderr=subprocess.STDOUT,cwd=exe.parent,env=env)
        timed_out=False
        try: proc.wait(timeout=a.seconds)
        except subprocess.TimeoutExpired:
            timed_out=True;proc.terminate();proc.wait(timeout=10)
    screenshots=list(out.rglob("*.png"))
    report={"command":args,"duration_limit":a.seconds,"exit_code":proc.returncode,"frames":len(screenshots),"presses":a.press,"timeline":a.timeline,"timed_out":timed_out}
    (out/"report.json").write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
    print((out/"run.log").read_text(errors="replace")[-2000:])
if __name__=="__main__": main()
