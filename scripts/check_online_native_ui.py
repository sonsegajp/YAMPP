"""Capture the actual native Online menus, camera motion and monitor animation.

Uses a private localhost repository/relay, card, settings and empty mod folder.
The scripts open download consent and cancel; no package is installed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time

from project_config import ROOT, configuration, disc_path, project_path
from check_menu_additions import OPTIONS_BOOT, contact_sheet
from check_netplay_ui import Fixture

SCENARIOS = {
    "main-mods": {
        "menu": "0:5", "frames": 3300,
        "input": ",1320:100:8,1590:4:8,1710:4:8,1890:100:8,2130:0:120:0:0:0:80:0,2370:0:120:0:0:0:-70:70,2610:100:8,2790:200:8,2910:200:8,3090:200:8",
        "shots": {"online-main": 1260, "online-submenu": 1830, "catalog": 2070,
                  "catalog-yaw": 2190, "catalog-pitch-yaw": 2430,
                  "catalog-consent": 2700, "online-restored": 3030, "main-restored": 3240},
    },
    "host-name": {
        "menu": "10:0", "frames": 4050,
        "input": ",1320:100:8,1740:100:8,1920:0:120:0:0:0:60:40,2940:4:8,3060:100:8,3330:2:8,3510:200:8,3750:200:8",
        "sdl": '0,wait_name,0,0\n1,text,Friday Night MeleeZ\n2,key,42,1\n3,key,42,0\n420,key,40,1\n421,key,40,0\n',
        "room": "Friday Night Melee",
        "shots": {"online-submenu": 1650, "room-name": 1860, "room-name-rotated": 1980,
                  "host-lobby": 2820, "rules": 3210, "rules-changed": 3390,
                  "lobby-restored": 3630, "online-restored": 3930},
    },
    "name-cancel": {
        "menu": "10:0", "frames": 2910,
        "input": ",1320:100:8,1740:100:8,2310:200:8",
        "sdl": '0,wait_name,0,0\n1,text,Canceled Room\n',
        "room": "Canceled Room",
        "shots": {"room-name": 2130, "canceled": 2670},
    },
    "monitors": {
        "menu": "4:3", "frames": 3480,
        "input": ",1320:100:8,1650:2:8,1920:0:120:0:0:0:80:0,2160:0:120:0:0:0:-70:70,2460:200:8,2760:100:8,3000:2:8,3180:100:8",
        "shots": {"options-label": 1260, "entry-1": 1350, "entry-2": 1380,
                  "entry-3": 1410, "monitors-neutral": 1590,
                  "selection-changed": 1830, "monitors-yaw": 1980,
                  "monitors-pitch-yaw": 2220, "exit-animation": 2490,
                  "options-restored": 2670, "aspect-saved": 3420},
    },
    "rooms": {
        "menu": "10:0", "frames": 4590,
        "input": ",1320:100:8,1560:4:8,1680:100:8,2010:0:120:0:0:0:80:0,2220:0:120:0:0:0:-70:70,2460:100:8,2820:0:120:0:0:0:80:0,3060:400:8,3180:200:8,3420:4:8,3540:100:8,3660:4:8,3780:100:8,4140:200:8,4380:200:8",
        "shots": {"browser": 1920, "browser-yaw": 2070,
                  "browser-pitch-yaw": 2280, "lobby": 2730,
                  "lobby-yaw": 2880, "lobby-ready": 3120,
                  "required-consent": 4080, "consent-canceled": 4260,
                  "online-restored": 4500},
    },
}


SCENARIOS["long-name"] = dict(SCENARIOS["host-name"], room='Friday Night Melee - Friends and Rivals Round01',
    sdl='0,wait_name,0,0\n1,text,Friday Night Melee - Friends and Rivals Round01\n420,key,40,1\n421,key,40,0\n')

SCENARIOS["workshop-preview"] = {
    "menu": "0:5", "frames": 4530,
    "input": ",1320:100:8,1590:4:8,1710:4:8,1890:100:8,2700:0:120:0:0:0:80:0,3000:0:120:0:0:0:-70:70,3300:100:8,3600:200:8,3900:200:8,4200:200:8,4350:4:8",
    "shots": {"online-main":1260,"online-submenu":1830,"catalog":2550,
              "catalog-yaw":2760,"catalog-pitch-yaw":3060,"catalog-consent":3450,
              "online-restored":4110,"main-restored":4290,"stock-restored":4470},
}


SCENARIOS["workshop-latest"] = {
    "menu": "0:5", "frames": 3690,
    "input": ",1320:100:8,1590:4:8,1710:4:8,1890:100:8,2040:4:8,2550:0:120:0:0:0:80:0,2850:0:120:0:0:0:-70:70,3150:100:8,3450:200:8",
    "shots": {"catalog":2460,"catalog-yaw":2610,"catalog-pitch-yaw":2910,"catalog-consent":3300},
}


SCENARIOS["workshop-quick"] = {
    "menu": "10:2", "frames": 3000,
    "input": ",1320:100:8,1800:0:120:0:0:0:80:0,2100:0:120:0:0:0:-70:70,2400:100:8,2700:200:8",
    "shots": {"catalog":1710,"catalog-yaw":1860,"catalog-pitch-yaw":2160,
              "catalog-consent":2550,"catalog-restored":2880},
}

SCENARIOS["fresh-online"] = {
    "menu": "10:1", "frames": 3810,
    "input": ",1320:100:8,1980:200:8,2100:4:8,2220:4:8,2340:100:8,2580:8:8,2700:8:8,2820:8:8,2940:100:8,3300:100:8,3540:200:8",
    "shots": {"offline-five-rows":1260,"offline-rooms-connected":1800,"connected-five-rows":2040,
              "disconnected-five-rows":2490,"reconnected-five-rows":3180,"host-dialog":3450,"host-canceled":3690},
}


def wait_server(port):
    deadline = time.monotonic()+15
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), .2):
                return
        except OSError:
            time.sleep(.05)
    raise RuntimeError("The isolated server did not start")


def prepare_repository(folder, candidate_package=None):
    sys.path.insert(0, str(ROOT/"server"))
    from mod_repository import Repository
    from test_mod_repository import fixture_package
    repo = Repository(folder)
    if candidate_package is not None:
        raw = candidate_package.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if re.fullmatch(r"[0-9a-f]{64}", candidate_package.stem) and candidate_package.stem != digest:
            raise ValueError("Candidate ZIP does not match its prepared SHA-256 filename")
        entry = repo.publish(raw)[0]
        if entry["sha256"] != digest:
            raise ValueError("Private repository returned a different candidate identity")
        return [entry]
    model = (project_path(configuration()["assets"]["directory"])/"files/PlLkNr.dat").read_bytes()
    entries = []
    names = ["Classic Link Costume", "Forest Practice Costume", "Moonlight Link",
             "Royal Guard Costume Collection", "Silver Training Costume",
             "Tournament Costume Preview", "Very Long Costume Collection Name For Layout Review"]
    for i, name in enumerate(names):
        package = fixture_package({"id": "ui-preview-link-%d" % i, "name": name,
            "version": "1.2.%d" % i, "description": "A local preview costume using the original Link model. This entry checks readable names, details and download consent."}, model=model)
        entries.append(repo.publish(package)[0])
    return entries


def run_scenario(name, wide, out, exe, renderer, port, repository, observer, settle, repository_url=None):
    spec = dict(SCENARIOS[name])
    if settle:
        spec["frames"] += settle
        spec["input"] = "," + ",".join(str(int(event.split(":", 1)[0])+settle)+":"+event.split(":", 1)[1]
                                           for event in spec["input"].lstrip(",").split(","))
        spec["shots"] = {label: frame+settle for label, frame in spec["shots"].items()}
    out.mkdir(parents=True, exist_ok=True)
    def artifact_digest(path):
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024*1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    executable_sha256 = artifact_digest(exe)
    renderer_sha256 = artifact_digest(renderer)
    cfg = configuration()
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(("MELEE_TEST_", "MELEE_CAPTURE_", "MELEE_NETPLAY_", "MELEE_MOD_TEST_", "MELEE_AUDIO_", "MELEE_RENDERDOC_", "MELEE_TRACE_")) or key in ("MELEE_MODS", "MELEE_INPUT", "MELEE_MEMORY_CARD"):
            env.pop(key)
    settings = out/"settings.xml"
    settings.write_text('<melee-settings schema="1" width="%d" height="720" renderScale="1" widescreen="%d" fullscreen="0" vsync="0" volume="100" mute="1" showFps="0" />' % (1280 if wide else 960, wide))
    template = ROOT/"build/netplay-work/template-card.raw"
    shutil.copy2(template, out/"card.raw")
    empty_mods = ROOT/"build/modkit/online-native-ui"/("16x9" if wide else "4x3")/name
    empty_mods.mkdir(parents=True, exist_ok=True)
    (empty_mods/"costumes.tsv").write_text("MELEE_COSTUMES\t2\n", newline="\n")
    env.update(MELEE_DISC=str(disc_path(cfg)), MELEE_FST=str(project_path(cfg["assets"]["directory"])/"sys/fst.bin"),
        MELEE_AURORA_DLL=str(renderer), MELEE_SETTINGS=str(settings), MELEE_MEMORY_CARD=str(out/"card.raw"),
        GCN_AURORA_HIDDEN="1", GCN_AURORA_DATA_DIR=str(out/"cache"), MELEE_INPUT=OPTIONS_BOOT+spec["input"],
        MELEE_TEST_MENU=spec["menu"], MELEE_TEST_FAST_EXIT="1", MELEE_TEST_UI_TRACE="1", MELEE_TEST_MENU_POSE="1",
        MELEE_CAPTURE_UI="1", MELEE_CAPTURE_AURORA="1", MELEE_CAPTURE_DIR=str(out),
        MELEE_NETPLAY_SERVER="127.0.0.1:%d" % port, MELEE_NETPLAY_NAME="UI Tester",
        MELEE_MOD_REPOSITORY_URL=repository_url or ("http://127.0.0.1:%d/api/mods" % port),
        MELEE_WORKSHOP_TEST_MODS=str(empty_mods), MELEE_COSTUME_REGISTRY=str(empty_mods/"costumes.tsv"))
    env["PATH"] = str(ROOT/"build/pc/bin")+";C:/msys64/mingw64/bin;"+env["PATH"]
    if spec.get("sdl"):
        events = out/"keyboard.csv"
        events.write_text(spec["sdl"], newline="\n")
        env["MELEE_TEST_SDL_INPUT"] = str(events)
    first_message = len(observer.messages)
    started = time.monotonic()
    with (out/"run.log").open("w") as log:
        proc = subprocess.Popen([str(exe), str(project_path(cfg["assets"]["directory"])/"sys/main.dol"), str(spec["frames"])], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            proc.wait(timeout=max(240, spec["frames"]/60*5))
            timeout = False
        except subprocess.TimeoutExpired:
            proc.terminate(); proc.wait(15); timeout = True
    text = (out/"run.log").read_text(errors="replace")
    captures = sorted(out.glob("frame_*.ppm"))
    from PIL import Image
    selected = []
    for title, frame in spec["shots"].items():
        source = min(captures,key=lambda p:abs(int(p.stem[6:])-frame)) if captures else None
        if source and abs(int(source.stem[6:])-frame) <= 60:
            Image.open(source).save(out/(title+".png"))
            selected.append(source)
    contact_sheet(selected, out/"review.jpg", columns=3, width=400)
    poses = re.findall(r"^\[menu-pose\].*$", text, re.MULTILINE)
    trace = re.findall(r"^\[netplay-ui\].*$", text, re.MULTILINE)
    checks = {"exit_clean": proc.returncode == 0 and not timeout,
              "no_fault_or_assertion": "[fault]" not in text and not re.search(r"assertion .*failed", text),
              "all_requested_captures": len(selected) == len(spec["shots"])}
    if name == "monitors":
        import xml.etree.ElementTree as ET
        checks["native_monitor_planes_loaded"] = "2 animated monitor planes installed" in text
        checks["aspect_change_saved"] = ET.fromstring(settings.read_text()).get("widescreen") == str(1-wide)
    else:
        checks["pose_matrices_recorded"] = len(poses) >= (1 if name in ("name-cancel", "fresh-online") else 3)
        if name == "fresh-online":
            checks["five_native_rows_built"] = "[online-menu] built five rows" in text
            checks["offline_rooms_connected"] = any("kind=13" in row for row in trace)
            checks["disconnect_and_reconnect"] = "[netplay] Disconnected" in text and text.count("[netplay] Connected to server") >= 2
            checks["host_open_and_cancel"] = "[room-name] open=1" in text and "[room-name] open=0" in text
        elif name in ("host-name", "name-cancel", "long-name"):
            expected = spec["room"]
            observed = {room["name"] for message in observer.messages[first_message:]
                        for room in message.get("rooms", [])}
            checks["real_sdl_text_received"] = "open=1 text="+expected in text
            checks["dialog_closed"] = "[room-name] open=0" in text
            checks["cancel_created_no_room" if name == "name-cancel" else "server_room_name_correct"] = (expected in observed) == (name != "name-cancel")
            if name != "name-cancel":
                checks["host_lobby_opened"] = any("kind=11" in row for row in trace)
                checks["rules_opened"] = any("kind=11 selected=100" in row for row in trace)
        elif name == "main-mods" or name.startswith("workshop-"):
            checks["catalog_opened"] = any("kind=22" in row for row in trace)
            checks["download_consent_opened"] = any("kind=22 selected=1000" in row for row in trace)
            if name.startswith("workshop-"):
                checks["verified_png_displayed"] = "[mod-preview] verified PNG loaded" in text
        else:
            checks["browser_opened"] = any("kind=13" in row for row in trace)
            checks["lobby_joined"] = any("kind=11" in row and "players=2" in row for row in trace)
            checks["room_consent_opened"] = any("kind=13 selected=1000" in row for row in trace)
    report = {"scenario": name, "aspect": "16:9" if wide else "4:3", "seconds": round(time.monotonic()-started,1),
              "checks": checks, "ui_trace": trace, "pose_trace": poses, "captures": len(captures),
              "path": str(out), "executable": str(exe), "renderer": str(renderer),
              "executable_sha256": executable_sha256, "renderer_sha256": renderer_sha256,
              "catalog_fixture": [{key: entry.get(key) for key in ("id", "version", "sha256", "preview_sha256", "preview_size")} for entry in repository],
              "visual_review_required": "Inspect panel/text tracking and monitor entry/exit in the named PNGs and review.jpg."}
    (out/"report.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({k:v for k,v in report.items() if k not in ("ui_trace","pose_trace")}),flush=True)
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, default=ROOT/"build/native-game/melee-online-next.exe")
    ap.add_argument("--renderer", type=Path, default=ROOT/"build/pc/bin/melee_aurora_online_next.dll")
    ap.add_argument("--aspect", choices=("4:3","16:9","both"), default="both")
    ap.add_argument("--scenario", choices=SCENARIOS, action="append")
    ap.add_argument("--repository-url", help="Read-only catalog source override for published PNG capture")
    ap.add_argument("--candidate-package", type=Path, help="Validate and serve only this ZIP in the isolated localhost fixture; no public upload or installation")
    ap.add_argument("--settle-frames", type=int, default=1800, help="Allow native file verification to finish before Online inputs")
    ap.add_argument("--output", type=Path, default=ROOT/"build/comparisons/online-native-next")
    args = ap.parse_args()
    if not args.exe.is_file() or not args.renderer.is_file():
        raise SystemExit("Build the isolated executable and renderer before this capture")
    args.output.mkdir(parents=True,exist_ok=True)
    sys.path.insert(0, str(ROOT/"tools/modkit"))
    from compatibility import fingerprint, plan
    identity = fingerprint(plan(args.exe))
    if args.candidate_package and args.repository_url:
        ap.error("Use either a private candidate package or an external repository URL")
    entries = prepare_repository(args.output/"repository", args.candidate_package)
    (args.output/"catalog-fixture.json").write_text(json.dumps(entries, indent=2)+"\n")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1",0)); port=sock.getsockname()[1]
    reports=[]; fixtures=[]
    with (args.output/"server.log").open("w") as log:
        server=subprocess.Popen([sys.executable,str(ROOT/"server/melee_netplay_server.py"),"--bind","127.0.0.1","--port",str(port),"--mods-dir",str(args.output/"repository"),"--mod-api","http://127.0.0.1:%d/api" % port],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        try:
            wait_server(port)
            for wide in ([0,1] if args.aspect=="both" else [int(args.aspect=="16:9")]):
                label="16x9" if wide else "4x3"
                wire_identity={"schema":1,"runtime":identity["runtimeIdentity"+label],
                               "game":identity["game"],"fingerprint":identity["aspect"+label]}
                observer = Fixture(port,"Catalog Observer",compatibility=wire_identity); fixtures.append(observer)
                fixtures.append(Fixture(port,"Practice Partner","Friendly Melee",compatibility=wire_identity))
                host=Fixture(port,"Practice Partner",compatibility=wire_identity);fixtures.append(host)
                host.send(op="hello",name="Practice Partner",version=2,sync="rollback-v1",features=["mods-v1","compat-v1"],compatibility=wire_identity)
                required=[entries[0]["sha256"]]
                host.send(op="create",name="Costume Practice",max=2,required_mods=required,installed_mods=required,rules={"stock":4,"minutes":8,"items":0})
                deadline=time.monotonic()+10
                while not all(f.room for f in fixtures[1:]):
                    if time.monotonic()>deadline: raise RuntimeError("Fixture rooms did not initialize")
                    time.sleep(.05)
                for name in args.scenario or SCENARIOS:
                    reports.append(run_scenario(name,wide,args.output/label/name,args.exe,args.renderer,port,entries,observer,args.settle_frames,args.repository_url))
                for fixture in fixtures: fixture.close()
                fixtures=[]
        finally:
            for fixture in fixtures: fixture.close()
            server.terminate();server.wait(15)
    (args.output/"report.json").write_text(json.dumps(reports,indent=2)+"\n")
    if any(not all(report["checks"].values()) for report in reports): raise SystemExit(1)


if __name__ == "__main__":
    main()
