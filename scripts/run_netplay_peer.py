"""Run a second copy of the game as a netplay opponent on this machine.

It connects to the master server on its own, hosts (or joins) the named room,
marks itself ready and starts the match once everybody else is ready, so the
whole netplay path can be exercised without a second player.

    python scripts/run_netplay_peer.py "Test Room"          # hosts the room
    python scripts/run_netplay_peer.py "Test Room" --join   # joins it instead

It keeps its own settings file and memory card so it cannot fight with the copy
you are playing.
"""
import os, subprocess, sys, time
from pathlib import Path
from project_config import ROOT, configuration, disc_path, project_path

PEER = ROOT / "build/netplay-peer"


def main():
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    names = [a for a in sys.argv[1:] if not a.startswith("--")]
    room = names[0] if names else "Test Room"
    host = "--join" not in flags
    manual = "--manual" in flags     # a plain second copy, driven by hand
    cfg = configuration()
    PEER.mkdir(parents=True, exist_ok=True)
    settings = PEER / "settings.xml"
    # Windowed and small, so it sits beside the copy being played.
    settings.write_text('<?xml version="1.0" encoding="utf-8"?>\n<melee-settings schema="1" width="853" height="480" '
                        'renderScale="1" widescreen="1" fullscreen="0" vsync="0" volume="40" mute="0" showFps="0" '
                        'netplayServer="" netplayName="Opponent" />')   # empty: use the built-in server
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(("MELEE_TEST_", "MELEE_CAPTURE_", "MELEE_MOD_TEST_", "MELEE_TRACE_")):
            env.pop(key)
    exe = os.environ.get("MELEE_PEER_EXE", str(ROOT / "build/native-game/melee-mod-next3.exe"))
    env.update(MELEE_DISC=str(disc_path(cfg)), MELEE_FST=str(project_path(cfg["assets"]["directory"]) / "sys/fst.bin"),
               MELEE_AURORA_DLL=str(project_path(cfg["runtime"]["developmentRenderer"])),
               MELEE_MEMORY_CARD=str(PEER / "card.raw"), MELEE_SETTINGS=str(settings),
               GCN_AURORA_DATA_DIR=str(ROOT / "user/diagnostic-cache"),
               MELEE_NETPLAY_AUTO=("" if manual else ("host:" if host else "join:") + room))
    if manual:
        env.pop("MELEE_NETPLAY_AUTO", None)
    env["PATH"] = str(ROOT / "build/pc/bin") + os.pathsep + "C:/msys64/mingw64/bin" + os.pathsep + env["PATH"]
    log = PEER / ("peer-" + time.strftime("%Y%m%d-%H%M%S") + ".log")
    with log.open("w") as stream:
        process = subprocess.Popen([exe, str(project_path(cfg["assets"]["directory"]) / "sys/main.dol"), "0"],
                                   cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT)
    print("second copy PID", process.pid,
          "(driven by hand)" if manual else ("hosting " if host else "joining ") + repr(room), "log:", log)


if __name__ == "__main__":
    main()
