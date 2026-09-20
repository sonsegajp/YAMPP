"""Run an isolated native Melee check without input to the user's playtest window."""
import argparse,os,subprocess,time,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser()
p.add_argument("--name",required=True);p.add_argument("--frames",type=int,default=900)
p.add_argument("--timeline",default="90:100:8,180:100:8")
p.add_argument("--capture",action="store_true")
p.add_argument("--executable",choices=["melee.exe","melee-mod.exe"],default="melee.exe")
p.add_argument("--normal-exit",action="store_true")
a=p.parse_args()
out=ROOT/"build/comparisons"/a.name;out.mkdir(parents=True,exist_ok=True)
from project_config import configuration, disc_path, project_path
cfg=configuration()
env=os.environ.copy()
env["MELEE_DISC"]=str(disc_path(cfg))
env["MELEE_FST"]=str(project_path(cfg["assets"]["directory"])/"sys/fst.bin")
env["PATH"]=str(ROOT/"build/pc/bin")+";C:/msys64/mingw64/bin;"+env["PATH"]
env["MELEE_INPUT"]=a.timeline
if a.normal_exit: env.pop("MELEE_TEST_FAST_EXIT",None)
else: env["MELEE_TEST_FAST_EXIT"]="1"
env["GCN_AURORA_HIDDEN"]="1"
env["GCN_AURORA_DATA_DIR"]=str(ROOT/"user/diagnostic-cache")
env["MELEE_AUDIO_CAPTURE"]=str(out/"audio-s16le-32000-stereo.raw")
if a.capture:
 env["MELEE_CAPTURE_AURORA"]="1";env["MELEE_CAPTURE_DIR"]=str(out)
else:
 env.pop("MELEE_CAPTURE_AURORA",None);env.pop("MELEE_CAPTURE_DIR",None)
started=time.perf_counter()
with (out/"run.log").open("w") as log:
 proc=subprocess.Popen([str(ROOT/"build/native-game"/a.executable),str(project_path(cfg["assets"]["directory"])/"sys/main.dol"),str(a.frames)],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
 timed_out=False
 try:proc.wait(timeout=max(60,a.frames/60*4))
 except subprocess.TimeoutExpired:
  timed_out=True;proc.terminate();proc.wait()
report={"native_executable":str(ROOT/"build/native-game"/a.executable),"timeline":a.timeline,"host_frames_requested":a.frames,"wall_seconds":time.perf_counter()-started,"exit_code":proc.returncode,"timed_out":timed_out,"captures":len(list(out.glob("frame_*.ppm")))}
log_text=(out/"run.log").read_text(errors="replace")
report["guest_assertions"]=re.findall(r'^.*assertion .*failed.*$',log_text,re.MULTILINE)
report["passed"]=not timed_out and proc.returncode==0 and not report["guest_assertions"]
(out/"report.json").write_text(json.dumps(report,indent=2))
print(json.dumps(report,indent=2))
print((out/"run.log").read_text(errors="replace")[-1600:])

if not report["passed"]:raise SystemExit(1)
