"""Drive a VS match with scripted input and look for the now-playing bar.

The bar is an in-game SIS text object, so it only shows up in the rendered
frames; this captures the frames around the start of a match and reports the
[music] lines the host printed, so a missing bar can be told apart from a
missing track.
"""
import os, re, subprocess, sys
from pathlib import Path
from project_config import ROOT, configuration, disc_path, project_path

# Boot, title, character select, stage select, then the match itself.
TIMELINE = ('300:100:8,420:100:8,600:1000:8,840:1000:8,1020:4:8,1110:100:8,1200:100:8,'
            '1600:0:22:0:80,1700:100:8,1600:0:33:0:80:1,1650:0:27:80:0:1,1740:100:8:0:0:1,'
            '2000:1000:8,2190:0:20:0:80,2230:0:20:80:0,2310:100:8,2600:100:5,3000:100:5')


def main():
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 4200
    cfg = configuration()
    out = ROOT / 'build/comparisons/music-bar'
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob('frame_*.ppm'):
        old.unlink()
    settings = out / 'settings.xml'
    wide = os.environ.get('MELEE_TEST_WIDE') == '1'
    settings.write_text('<?xml version="1.0" encoding="utf-8"?>\n<melee-settings schema="1" width="%d" height="720" '
                        'renderScale="1" widescreen="%d" fullscreen="0" vsync="0" volume="100" mute="1" showFps="0" />'
                        % (1280 if wide else 960, 1 if wide else 0))
    env = os.environ.copy()
    stage = env.get('MELEE_TEST_STAGE')
    for key in list(env):
        if key.startswith(('MELEE_TEST_', 'MELEE_MOD_TEST_', 'MELEE_TRACE_', 'MELEE_NETPLAY_')):
            env.pop(key)
    if stage:
        env['MELEE_TEST_STAGE'] = stage
    env.update(MELEE_DISC=str(disc_path(cfg)), MELEE_FST=str(project_path(cfg['assets']['directory']) / 'sys/fst.bin'),
               MELEE_AURORA_DLL=str(project_path(cfg['runtime']['developmentRenderer'])), GCN_AURORA_HIDDEN='1',
               GCN_AURORA_DATA_DIR=str(ROOT / 'user/diagnostic-cache'), MELEE_MEMORY_CARD=str(out / 'card.raw'),
               MELEE_SETTINGS=str(settings), MELEE_INPUT=TIMELINE, MELEE_CAPTURE_AURORA='1',
               MELEE_CAPTURE_DIR=str(out), MELEE_TEST_FAST_EXIT='1')
    env['PATH'] = str(ROOT / 'build/pc/bin') + ';C:/msys64/mingw64/bin;' + env['PATH']
    exe = os.environ.get('MELEE_TEST_EXE', str(ROOT / 'build/native-game/melee-mod.exe'))
    with (out / 'run.log').open('w') as log:
        subprocess.run([exe, str(project_path(cfg['assets']['directory']) / 'sys/main.dol'), str(frames)],
                       cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=max(240, frames / 60 * 5))
    text = (out / 'run.log').read_text(errors='replace')
    for line in re.findall(r'^\[(?:music|hooks)\].*$', text, re.MULTILINE)[:40]:
        print(line)
    print('captures:', len(list(out.glob('frame_*.ppm'))))


if __name__ == '__main__':
    main()
