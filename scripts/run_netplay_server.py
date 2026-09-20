"""Run the netplay lobby server, for local testing or on a VPS.

Locally this is all you need to see every netplay screen: start it, then open
Netplay in the game (it connects to 127.0.0.1:7420 by default). Launch a second
copy of the game to host and join a room from both sides.

    python scripts/run_netplay_server.py                # this machine
    python scripts/run_netplay_server.py --port 7420    # pick a port

For a VPS, copy server/melee_netplay_server.py there and run it with the
systemd unit in server/ (see server/README.md), then set the address in the
game's F1 settings.
"""
import argparse, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=7420)
    parser.add_argument("--bind", default="0.0.0.0")
    args = parser.parse_args()
    server = ROOT / "server/melee_netplay_server.py"
    print("Netplay server on %s:%d  (the game connects to 127.0.0.1:%d by default)"
          % (args.bind, args.port, args.port))
    print("Leave this window open while you play. Ctrl+C stops it.")
    raise SystemExit(subprocess.call([sys.executable, str(server), "--port", str(args.port), "--bind", args.bind]))


if __name__ == "__main__":
    main()
