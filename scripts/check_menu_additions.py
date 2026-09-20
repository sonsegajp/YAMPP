"""Drive the original menus with scripted pad input and capture what they show.

Scenarios (all headless, development executable and renderer):
  netplay-item      main menu with the added Netplay item hovered
  widescreen-option Options menu with the Widescreen item, its submenu, and a
                    confirmed switch to Wide (verified in the settings XML)
  css-unlocked      the character select screen with every fighter unlocked
  widescreen-view   main menu and character select rendered in a 16:9 window

Captures are written every 30 host frames; a contact sheet is produced per
scenario for visual review.
"""
import argparse, json, os, re, subprocess, sys, time
from pathlib import Path
from project_config import ROOT, configuration, disc_path, project_path

# A saved card that already recorded the unlock prizes keeps the boot deterministic.
TEMPLATE_CARD = ROOT / 'build/netplay-work/template-card.raw'
# Press Start on the title for a while: boot length varies with shader compilation.
# A for any early prompt, then Start on the title (boot length varies with
# shader compilation, so press it repeatedly rather than at a fixed frame).
TITLE = ','.join('%d:100:6' % f for f in range(300, 900, 120)) + ',' + ','.join('%d:1000:6' % f for f in range(900, 2400, 90))
BOOT = '300:100:8,420:100:8,600:1000:8,840:1000:8'
# Direct-menu captures stop confirming once the title opens the requested menu.
# Repeated Start presses in TITLE otherwise activate the selected Options row.
OPTIONS_BOOT = ','.join('%d:100:6' % f for f in range(300, 900, 120)) + ',900:1000:6'
SCENARIOS = {
    # Up from the top item wraps onto Netplay (six rows), A opens the netplay
    # screen, then Down/Up/A/B must keep moving the cursor: dead input here is
    # the failure this scenario exists to catch.
    'netplay-item': {'timeline': BOOT + ',1080:8:8', 'frames': 1500, 'wide': 0},
    # Down three times from the top of the main menu selects Options, A opens
    # it: the capture shows every row so the added Widescreen row is visible.
    'options-rows': {'timeline': OPTIONS_BOOT + ',1410:4:8,1530:8:8', 'frames': 1760, 'wide': 0, 'menu': '4:3'},
    'options-list': {'timeline': OPTIONS_BOOT, 'frames': 1500, 'wide': 0, 'menu': '4:0'},
    'netplay-rows': {'timeline': TITLE, 'frames': 2600, 'wide': 0, 'menu': '10:0'},
    'browser-rows': {'timeline': TITLE, 'frames': 2600, 'wide': 0, 'menu': '13:0'},
    'main-rows': {'timeline': TITLE, 'frames': 2600, 'wide': 0, 'menu': '0:5'},
    'netplay-screen': {'timeline': BOOT + ',1080:8:8,1200:100:8,1380:4:8,1470:4:8,1560:8:8,1650:100:8,1800:200:8,1920:4:8', 'frames': 2400, 'wide': 0},
    'widescreen-option': {'timeline': OPTIONS_BOOT + ',1320:100:8,1440:2:8,1560:100:8', 'frames': 1740, 'wide': 0, 'menu': '4:3', 'expected_wide': 1},
    'widescreen-art-4x3': {'timeline': OPTIONS_BOOT + ',1320:100:8', 'frames': 1530, 'wide': 0, 'menu': '4:3'},
    'widescreen-art-16x9': {'timeline': OPTIONS_BOOT + ',1200:1000:8,1500:1000:8,1800:1000:8,2100:1000:8', 'frames': 2340, 'wide': 1, 'menu': '4:3', 'expected_submenu': True},
    'css-unlocked': {'timeline': BOOT + ',1080:4:8,1170:100:8,1260:100:8', 'frames': 2000, 'wide': 0},
    'widescreen-view': {'timeline': BOOT + ',1080:4:8,1170:100:8,1260:100:8', 'frames': 2000, 'wide': 1},
}


def run(name, spec, out):
    """Run one scenario. 'menu' opens that menu kind directly (kind[:selection])."""
    cfg = configuration()
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('MELEE_TEST_', 'MELEE_MOD_TEST_', 'MELEE_TRACE_', 'MELEE_RENDERDOC_', 'MELEE_NETPLAY_', 'MELEE_AUDIO_')) or key in ('MELEE_MODS', 'MELEE_MEMORY_CARD'):
            env.pop(key)
    settings = out / 'settings.xml'
    width, height = (1280, 720) if spec['wide'] else (960, 720)
    settings.write_text('<?xml version="1.0" encoding="utf-8"?>\n<melee-settings schema="1" width="%d" height="%d" renderScale="1" widescreen="%d" fullscreen="0" vsync="0" volume="100" mute="1" showFps="0" />' % (width, height, spec['wide']))
    env.update(MELEE_DISC=str(disc_path(cfg)), MELEE_FST=str(project_path(cfg['assets']['directory']) / 'sys/fst.bin'),
               MELEE_AURORA_DLL=str(project_path(cfg['runtime']['developmentRenderer'])), GCN_AURORA_HIDDEN='1',
               GCN_AURORA_DATA_DIR=str(ROOT / 'user/diagnostic-cache'), MELEE_MEMORY_CARD=str(out / 'card.raw'), MELEE_SETTINGS=str(settings),
               MELEE_INPUT=spec['timeline'], MELEE_CAPTURE_AURORA='1', MELEE_CAPTURE_DIR=str(out), MELEE_TEST_FAST_EXIT='1')
    if spec.get('menu'):
        env['MELEE_TEST_MENU'] = spec['menu']
    else:
        env.pop('MELEE_TEST_MENU', None)
    env['PATH'] = str(ROOT / 'build/pc/bin') + ';C:/msys64/mingw64/bin;' + env['PATH']
    for old in out.glob('frame_*.ppm'):
        old.unlink()
    if TEMPLATE_CARD.exists():
        import shutil
        shutil.copy2(TEMPLATE_CARD, out / 'card.raw')
    elif (out / 'card.raw').exists():
        (out / 'card.raw').unlink()
    with (out / 'run.log').open('w') as log:
        process = subprocess.Popen([os.environ.get('MELEE_TEST_EXE', str(ROOT / 'build/native-game/melee-mod.exe')), str(project_path(cfg['assets']['directory']) / 'sys/main.dol'), str(spec['frames'])], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            process.wait(timeout=max(120, spec['frames'] / 60 * 4))
            timed_out = False
        except subprocess.TimeoutExpired:
            process.terminate(); process.wait(); timed_out = True
    text = (out / 'run.log').read_text(errors='replace')
    frames = sorted(out.glob('frame_*.ppm'))
    sheet = contact_sheet(frames, out / 'sheet.jpg')
    checks = {}
    if spec.get('expected_submenu'):
        checks['submenu_opened'] = '[hooks] widescreen submenu:' in text
    if 'expected_wide' in spec:
        import xml.etree.ElementTree as ET
        checks['widescreen_saved'] = ET.fromstring(settings.read_text()).get('widescreen') == str(spec['expected_wide'])
    return {'scenario': name, 'exit': process.returncode, 'timed_out': timed_out, 'captures': len(frames), 'sheet': str(sheet) if sheet else None,
            'settings_after': settings.read_text(), 'assertions': re.findall(r'^.*assertion .*failed.*$', text, re.MULTILINE),
            'hook_lines': re.findall(r'^\[hooks\].*$', text, re.MULTILINE)[:12], 'fault': '[fault]' in text, 'checks': checks}


def contact_sheet(frames, path, columns=4, width=320):
    if not frames:
        return None
    from PIL import Image
    images = []
    for frame in frames[-16:]:
        try:
            im = Image.open(frame); im.load()
        except OSError:
            continue
        ratio = width / im.width
        images.append((frame.stem, im.resize((width, int(im.height * ratio)))))
    if not images:
        return None
    h = max(im.height for _, im in images) + 14
    rows = (len(images) + columns - 1) // columns
    sheet = Image.new('RGB', (columns * (width + 4), rows * h), (40, 40, 60))
    from PIL import ImageDraw
    draw = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(images):
        x, y = (i % columns) * (width + 4), (i // columns) * h
        sheet.paste(im, (x, y + 12))
        draw.text((x + 2, y), name, fill=(255, 80, 80))
    sheet.save(path, quality=88)
    return path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--scenario', choices=sorted(SCENARIOS), action='append')
    args = parser.parse_args()
    names = args.scenario or sorted(SCENARIOS)
    reports = []
    for name in names:
        out = ROOT / 'build/comparisons/menu-additions' / name
        out.mkdir(parents=True, exist_ok=True)
        report = run(name, SCENARIOS[name], out)
        (out / 'report.json').write_text(json.dumps(report, indent=2))
        print(json.dumps({k: v for k, v in report.items() if k != 'settings_after'}, indent=2))
        reports.append(report)
    failed = [r['scenario'] for r in reports if r['exit'] != 0 or r['timed_out'] or r['assertions'] or r['fault'] or not all(r['checks'].values())]
    if failed:
        print('FAILED:', failed)
        raise SystemExit(1)


if __name__ == '__main__':
    main()
