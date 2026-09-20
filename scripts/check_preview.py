"""Capture the main menu with Netplay hovered, for a given borrowed preview row."""
import os, subprocess, sys
from pathlib import Path
from project_config import ROOT, configuration, disc_path, project_path

def main():
    source = sys.argv[1] if len(sys.argv) > 1 else "1"
    cfg = configuration()
    out = ROOT / 'build/comparisons/preview' / source
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob('frame_*.ppm'):
        old.unlink()
    settings = out / 'settings.xml'
    settings.write_text('<?xml version="1.0" encoding="utf-8"?>\n<melee-settings schema="1" width="960" height="720" '
                        'renderScale="1" widescreen="0" fullscreen="0" vsync="0" volume="100" mute="1" showFps="0" />')
    env = os.environ.copy()
    env.update(MELEE_DISC=str(disc_path(cfg)), MELEE_FST=str(project_path(cfg['assets']['directory']) / 'sys/fst.bin'),
               MELEE_AURORA_DLL=str(project_path(cfg['runtime']['developmentRenderer'])), GCN_AURORA_HIDDEN='1',
               GCN_AURORA_DATA_DIR=str(ROOT / 'user/diagnostic-cache'), MELEE_MEMORY_CARD=str(out / 'card.raw'),
               MELEE_SETTINGS=str(settings), MELEE_CAPTURE_AURORA='1', MELEE_CAPTURE_DIR=str(out),
               MELEE_TEST_FAST_EXIT='1', MELEE_TEST_MENU='0:5', MELEE_NETPLAY_PREVIEW=source,
               MELEE_INPUT=','.join('%d:1000:6' % f for f in range(300, 1800, 90)))
    env['PATH'] = str(ROOT / 'build/pc/bin') + ';C:/msys64/mingw64/bin;' + env['PATH']
    exe = os.environ.get('MELEE_TEST_EXE', str(ROOT / 'build/native-game/melee-mod-next4.exe'))
    with (out / 'run.log').open('w') as log:
        subprocess.run([exe, str(project_path(cfg['assets']['directory']) / 'sys/main.dol'), '2400'],
                       cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=240)
    print(source, 'captures:', len(list(out.glob('frame_*.ppm'))))

if __name__ == '__main__':
    main()
