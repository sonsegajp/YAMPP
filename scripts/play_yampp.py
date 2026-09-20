"""Launch the portable game with a verified content plan and local user state."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

# The distributed interpreter ignores ambient Python paths and site packages.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from project_config import ROOT, configuration, disc_path, project_path


def launch(wait=False):
    cfg = configuration()
    runtime, user = cfg['runtime'], cfg['user']
    exe = project_path(runtime['executable'])
    renderer = project_path(runtime['renderer'])
    dol = project_path(cfg['assets']['directory']) / 'sys/main.dol'
    fst = dol.with_name('fst.bin')
    if not dol.is_file() or not fst.is_file():
        raise ValueError('Run Setup.cmd with your Melee USA 1.02 disc first.')
    if hashlib.sha1(dol.read_bytes()).hexdigest() != cfg['identity']['dolSha1']:
        raise ValueError('Game extraction does not match this build. Run Setup.cmd again.')
    if not exe.is_file() or not renderer.is_file():
        raise ValueError('The game or renderer is missing. Extract the complete YAMPP ZIP.')
    core = exe.with_suffix('.core.dll')
    if runtime.get('contentReload') == '1' and not core.is_file():
        raise ValueError('The game core is missing. Extract the complete YAMPP ZIP.')
    try:
        disc = disc_path(cfg)
    except ValueError:
        raise ValueError('The saved disc is missing. Run Setup.cmd to select it again.') from None
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('MELEE_', 'GCN_'))}
    for key in ('settings', 'memoryCard'):
        project_path(user[key]).parent.mkdir(parents=True, exist_ok=True)
    cache = project_path(user['cache']); cache.mkdir(parents=True, exist_ok=True)
    mods = project_path(user['mods'])
    root = ROOT / 'build/play'; root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='session-', dir=root))
    env.update(MELEE_VANILLA_BOOT='1', MELEE_DISC=str(disc), MELEE_FST=str(fst),
               MELEE_AURORA_DLL=str(renderer), MELEE_SETTINGS=str(project_path(user['settings'])),
               MELEE_MEMORY_CARD=str(project_path(user['memoryCard'])),
               MELEE_MOD_REGISTRY=str(mods/'registry.tsv'), MELEE_COSTUME_REGISTRY=str(mods/'costumes.tsv'),
               GCN_AURORA_DATA_DIR=str(cache), MELEE_WORKSHOP_PYTHON=sys.executable)
    env['PATH'] = str(ROOT/'bin') + os.pathsep + str(Path(sys.executable).parent) + os.pathsep + env.get('PATH', '')
    if user.get('modsEnabled') == '1': env['MELEE_MODS'] = '1'
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    log_path = out / 'game.log'
    with log_path.open('w', encoding='utf-8') as log:
        if core.is_file():
            env.update(MELEE_CONTENT_LAUNCHER='1', MELEE_CONTENT_RESTART=str(out/'restart.json'),
                       MELEE_CONTENT_BOOT_PLAN=str(out/'boot-plan.env'),
                       MELEE_CONTENT_PLAN_SCRIPT=str(ROOT/'scripts/prepare_content_runtime.py'))
            result = subprocess.run([sys.executable, env['MELEE_CONTENT_PLAN_SCRIPT'], '--output', env['MELEE_CONTENT_BOOT_PLAN']],
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
            if result.returncode:
                raise ValueError('Could not prepare installed content. Details: ' + str(log_path))
        child = subprocess.Popen([str(exe), str(dol), '0'], cwd=ROOT, env=env,
                                 stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
        record = dict(pid=child.pid, executable=str(exe), log=str(log_path))
        (out/'launch.json').write_text(json.dumps(record, indent=2))
        try:
            code = child.wait(timeout=None if wait else 2)
        except subprocess.TimeoutExpired:
            return record
        if code:
            raise ValueError('YAMPP could not start. Details: ' + str(log_path))
        return dict(record, exit=code)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--wait', action='store_true')
    args = ap.parse_args()
    try:
        result = launch(args.wait)
        if sys.stdout: print(json.dumps(result))
    except Exception as error:
        if sys.stderr: print(str(error), file=sys.stderr)
        if os.name == 'nt':
            import ctypes
            ctypes.windll.user32.MessageBoxW(None, str(error), 'YAMPP could not start', 0x10)
        raise SystemExit(1)
