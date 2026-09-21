"""One real game client against the scripted practice peer.

The peer in `server/netplay_input_bot.py` is what a player connects to when
they want to measure a connection rather than find an opponent. It does not
simulate Melee; it only feeds inputs and answers pings. This check proves the
part that matters: that a real client accepts it as an opponent, starts a
match, and keeps running frames against it -- because a peer that stalls the
match or trips desync detection would be worse than no peer at all.

It cannot check simulation agreement. There is only one simulation.
"""
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

from check_netplay_local import BOOT, free_port, launch
from project_config import ROOT

HOST_INPUT = ('1:120:0:26:0:80,1:200:100:8,1:420:1000:8,'
              '2:150:0:20:0:80,2:190:0:20:80:0,2:260:100:8,'
              '3:240:0:32:-80:0,3:282:100:12,3:350:200:18,3:500:100:12')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', default='netplay-bot')
    ap.add_argument('--frames', type=int, default=6000)
    ap.add_argument('--match-frames', type=int, default=600,
                    help='simulated match frames required against the peer')
    ap.add_argument('--server', help='test an already-running relay and peer instead of local ones')
    args = ap.parse_args()
    out = (ROOT / 'build/comparisons' / args.name).resolve()
    base = (ROOT / 'build/comparisons').resolve()
    if out == base or base not in out.parents:
        ap.error('--name must identify a directory inside build/comparisons')
    (out / 'host').mkdir(parents=True, exist_ok=True)
    sandbox = (ROOT / 'build/modkit/netplay-tests' / args.name).resolve()
    (sandbox / 'host').mkdir(parents=True, exist_ok=True)

    port = free_port()
    room = 'bot check %d' % port
    external = args.server is not None
    server = bot = client = None
    server_log = bot_log = client_log = None
    if not external:
        server_log = (out / 'server.log').open('w')
        bot_log = (out / 'bot.log').open('w')
        server = subprocess.Popen(
            [sys.executable, str(ROOT / 'scripts/netplay_test_server.py'), '--port', str(port)],
            cwd=ROOT, stdout=server_log, stderr=subprocess.STDOUT)
    try:
        if not external:
            time.sleep(1.5)
            bot = subprocess.Popen(
                [sys.executable, str(ROOT / 'server/netplay_input_bot.py'),
                 '--server', '127.0.0.1:%d' % port, '--name', 'Practice Peer', '--match', 'bot check'],
                cwd=ROOT, stdout=bot_log, stderr=subprocess.STDOUT)
            time.sleep(1.0)
        client, client_log = launch('Host', out / 'host', port, 'host', HOST_INPUT,
                                    args.frames, False, 0, sandbox / 'host',
                                    False, 0, args.server, room)
        client.wait(timeout=max(240, args.frames // 8))
    except subprocess.TimeoutExpired:
        client.kill()
    finally:
        for process in (bot, server):
            if process is not None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
        for handle in (client_log, server_log, bot_log):
            if handle is not None:
                handle.close()

    text = (out / 'host/run.log').read_text(errors='replace')
    frames = [int(m.group(2)) for m in re.finditer(r'\[netplay\] epoch (\d+) frame (\d+) hash', text)
              if int(m.group(1)) >= 3]
    started = 'started as port' in text
    reached = '[netplay] epoch 3 begins at match' in text
    stalled = re.findall(r'\[netplay\] match held: (.*)', text)
    desync = 'DESYNC' in text
    ended = re.findall(r'\[netplay\] Session ended: (.*)', text)
    pings = re.findall(r'peer (\d+) ms', text)
    simulated = max(frames) if frames else 0
    passed = (started and reached and not desync and simulated >= args.match_frames
              and all(reason == 'Shutdown' for reason in ended))
    print('started            :', started)
    print('reached a match    :', reached)
    print('match frames run   :', simulated)
    print('peer ping samples  :', pings[:5])
    print('holds              :', stalled[:5] or 'none')
    print('desync             :', desync)
    print('session endings    :', ended or 'none')
    print('PASS' if passed else 'FAIL')
    return 0 if passed else 1


if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    raise SystemExit(main())
