"""Assemble a local YAMPP distribution; never publishes or contacts GitHub."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MINGW = Path('C:/msys64/mingw64/bin')
FORBIDDEN = {'.iso', '.gcm', '.rvz', '.wia', '.dol', '.hps', '.dat', '.usd', '.raw', '.gxt'}
RUNTIME = ('SDL3.dll', 'webgpu_dawn.dll', 'libpng16.dll', 'zlib1.dll')
LF = chr(13) + chr(10)   # the note is read in Notepad on a fresh install
EXTERNAL = ('libturbojpeg.dll', 'libzstd.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll', 'libstdc++-6.dll', 'libssl-3-x64.dll', 'libcrypto-3-x64.dll')


def copy(source, dest):
    source = Path(source)
    if not source.exists():
        raise FileNotFoundError(source)
    if source.is_file() and source.suffix.lower() == '.pdb':
        return
    if source.is_symlink():
        raise ValueError('Refusing symlink: ' + str(source))
    dest.parent.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        for child in sorted(source.iterdir()):
            if child.name not in ('__pycache__', '.git'):
                copy(child, dest / child.name)
    else:
        shutil.copy2(source, dest)


def check_setup_entrypoint(out):
    """Exercise the shipped EXE without a game, launch plan, or renderer.

    A tiny invalid disc must reach the native extractor and be rejected there.
    This catches packaging a gameplay-only host that blocks first-run setup.
    """
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith(('MELEE_', 'GCN_'))}
    env['PATH'] = str(out / 'bin') + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32')
    with tempfile.TemporaryDirectory(prefix='yampp-setup-check-') as temporary:
        root = Path(temporary)
        image = root / 'invalid test image.iso'
        image.write_bytes(b'YAMPP setup regression fixture')
        result = subprocess.run([str(out / 'bin/YAMPP.exe'), '--extract', str(image), str(root / 'output')],
                                cwd=root, env=env, capture_output=True, text=True, timeout=30)
        if result.returncode != 1 or 'image is too short' not in result.stderr:
            raise ValueError('Packaged first-run extraction failed: ' + result.stdout + result.stderr)
    print('Verified packaged first-run extraction entry point')


def audit(out):
    files = sorted(p for p in out.rglob('*') if p.is_file())
    for p in files:
        relative = p.relative_to(out)
        # Electron's ICU Unicode tables are runtime data, not a Melee DAT archive.
        runtime_icu = relative.as_posix() == 'workshop/icudtl.dat'
        # The import folder must exist in a fresh install or there is nowhere
        # to put the Akaneia archive, and a ZIP cannot carry an empty folder.
        # Its note is the one thing allowed under user/.
        import_note = relative.as_posix() == 'user/imports/README.txt'
        if ((p.suffix.lower() in FORBIDDEN and not runtime_icu)
                or (not import_note and any(part.lower() in ('user', 'data', 'mods', 'examples') for part in relative.parts))):
            raise ValueError('Game/mod/user data in distribution: ' + str(relative))
        if p.name.lower() in ('community.xml', 'online.xml', 'disc.xml', 'disc.path'):
            raise ValueError('Private configuration in distribution: ' + str(relative))
    # Check the native pair and every DLL they bring along. System components
    # remain OS prerequisites; VC++ runtime requirements are documented.
    objdump = MINGW / 'objdump.exe'
    if not objdump.is_file():
        raise ValueError('objdump is required to verify the native distribution')
    available = {p.name.lower() for p in (out / 'bin').iterdir()}
    system = Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32'
    missing = []
    for binary in (out / 'bin').iterdir():
        if binary.suffix.lower() not in ('.exe', '.dll'):
            continue
        run = subprocess.run([str(objdump), '-p', str(binary)], capture_output=True, text=True, check=True)
        for line in run.stdout.splitlines():
            if 'DLL Name:' not in line:
                continue
            name = line.split('DLL Name:', 1)[1].strip()
            if name.lower().startswith(('api-ms-', 'ext-ms-')) or name.lower() in available or (system / name).is_file():
                continue
            missing.append(binary.name + ' needs ' + name)
    if missing:
        raise ValueError('Missing runtime DLLs: ' + '; '.join(sorted(set(missing))))
    return [{'path': p.relative_to(out).as_posix(), 'size': p.stat().st_size,
             'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in files]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', default='dist/YAMPP')
    ap.add_argument('--executable', default='build/native-game/YAMPP.exe')
    ap.add_argument('--renderer', default='build/pc/bin/melee_aurora_yampp.dll')
    ap.add_argument('--python-runtime', default='build/distribution-python-content')
    ap.add_argument('--content-importer', default='build/modkit/mex-content-portable')
    ap.add_argument('--skip-workshop', action='store_true')
    ap.add_argument('--version', help='Release this build is published as, e.g. 0.2.4. Recorded in the '
                                     'manifest so the launcher can tell whether it is behind the server.')
    ap.add_argument('--zip', action='store_true')
    args = ap.parse_args()
    out = (ROOT / args.output).resolve()
    dist = (ROOT / 'dist').resolve()
    if not out.is_relative_to(dist) or out == dist or out.is_relative_to(dist / 'template'):
        raise SystemExit('Output must be a new directory beneath dist/, outside dist/template')
    if out.exists():
        raise SystemExit('Output already exists; choose a new name to preserve it: ' + str(out))
    exe, renderer = ROOT / args.executable, ROOT / args.renderer
    core=exe.with_suffix('.core.dll')
    python_runtime=ROOT/args.python_runtime
    importer=ROOT/args.content_importer
    requirements = [exe, renderer, ROOT/'build/native-game/lua55.dll', ROOT/'CREDITS.md',
                    python_runtime/'python.exe']
    if core.is_file():
        requirements += [core,ROOT/'build/pc/bin/unicorn.dll',importer/'MexContentBridge.exe',importer/'coreclr.dll',importer/'hostfxr.dll']
        manifest=json.loads((python_runtime/'distribution-manifest.json').read_text())
        if 'py7zr' not in manifest['packages']:raise SystemExit('The portable Python runtime needs the content archive reader')
    requirements += [ROOT/'build/pc/bin'/name for name in RUNTIME]
    requirements += [MINGW/name for name in EXTERNAL]
    if not args.skip_workshop:
        requirements += [ROOT/'build/workshop/MeleeWorkshop-win32-x64/Melee Workshop.exe',
                         ROOT/'build/modkit/bridge/ModBridge.exe']
    for source in requirements:
        if not source.is_file():
            raise SystemExit('Required build artifact is missing: ' + str(source))
    out.mkdir(parents=True)
    from build_disc_identity import build as build_disc_identity
    copy(build_disc_identity(), out/'bin/YAMPP-disc-identity.exe')
    copy(exe, out/'bin/YAMPP.exe')
    if core.is_file():
        copy(core,out/'bin/YAMPP.core.dll')
        copy(ROOT/'build/pc/bin/unicorn.dll',out/'bin/unicorn.dll')
        copy(importer,out/'tools/content-importer')
        copy(ROOT/'native/vendor/unicorn',out/'licenses/third-party/Unicorn')
    copy(renderer, out/'bin/melee_aurora_yampp.dll')
    copy(ROOT/'build/native-game/lua55.dll', out/'bin/lua55.dll')
    for name in RUNTIME:
        copy(ROOT/'build/pc/bin'/name, out/'bin'/name)
    for name in EXTERNAL:
        copy(MINGW/name, out/'bin'/name)
    copy(ROOT/'dist/template', out)
    for name in ('README.md', 'CREDITS.md', 'dependencies.json'):
        copy(ROOT/name, out/name)
    copy(ROOT/'docs', out/'docs')
    copy(ROOT/'assets/branding', out/'assets/branding')
    copy(ROOT/'licenses', out/'licenses')
    copy(ROOT/'assets/ui/gamecube-original/README.md', out/'assets/ui/gamecube-original/README.md')
    # Online compatibility, catalog and consented downloads need this worker
    # even when the optional Electron authoring application is omitted.
    copy(python_runtime, out/'tools/python')
    for source in (ROOT/'tools/modkit').iterdir():
        if source.is_file() and source.suffix in ('.py', '.json') and not source.name.startswith('test_'):
            copy(source, out/'tools/modkit'/source.name)
    copy(ROOT/'tools/modkit/README.md', out/'tools/modkit/README.md')
    copy(ROOT/'tools/modkit/requirements.txt', out/'tools/modkit/requirements.txt')
    for name in ('project_config.py','play_yampp.py','prepare_content_runtime.py','extract_disc.py',
                 'check_akaneia_content.py','update_yampp.py'):
        copy(ROOT/'scripts'/name,out/'scripts'/name)
    copy(ROOT/'server/mod_repository.py', out/'server/mod_repository.py')
    for name in ('upstream_builds.py', 'upstream_builds.json'):
        copy(ROOT/'server'/name, out/'server'/name)
    copy(ROOT/'server/README.md', out/'server/README.md')
    if not args.skip_workshop:
        copy(ROOT/'build/workshop/MeleeWorkshop-win32-x64', out/'workshop')
        copy(ROOT/'build/modkit/bridge', out/'build/modkit/bridge')
        for name in ('lua55.dll', 'luac.exe'):
            copy(ROOT/'build/native-game'/name, out/'build/native-game'/name)
    config = ET.parse(ROOT/'config/project.xml')
    runtime = config.getroot().find('runtime')
    runtime.set('contentReload','1' if core.is_file() else '0')
    for field in ('executable', 'developmentExecutable'):
        runtime.set(field, 'bin/YAMPP.exe')
    for field in ('renderer', 'developmentRenderer'):
        runtime.set(field, 'bin/melee_aurora_yampp.dll')
    config.getroot().find('user').set('modsEnabled', '0')
    (out/'config').mkdir(exist_ok=True)
    config.write(out/'config/project.xml', encoding='utf-8', xml_declaration=True)
    copy(ROOT/'config/character-assets.xml', out/'config/character-assets.xml')
    # A fresh install needs somewhere obvious to drop the Akaneia archive.
    # Without this the folder does not exist, the importer reports the archive
    # as missing, and nothing on screen says where it was looked for.
    (out/'user/imports').mkdir(parents=True, exist_ok=True)
    (out/'user/imports/README.txt').write_text(LF.join([
        'Put optional content archives here.', '',
        'Akaneia: download Akaneia.Builder.1.0.1.7z from the official release at',
        'https://github.com/akaneia/akaneia-build/releases/tag/1.0.1 and place it in',
        'this folder, keeping the file name and leaving it compressed. Then open',
        'Online > Mod Browser, choose Akaneia and confirm Install.', '',
        'YAMPP does not distribute Akaneia and never mirrors it. Installing applies',
        'the official patch to your own verified disc in a separate workspace, which',
        'takes several minutes and needs about 6 GB free.', '',
    ]), encoding='utf-8')
    check_setup_entrypoint(out)
    entries = audit(out)
    manifest={'name':'Yet Another Melee PC Port','shortName':'YAMPP'}
    # The launcher compares this with what the master server reports. Without
    # it the updater does nothing at all, which is the right answer for a
    # folder that was assembled by hand rather than published.
    if args.version: manifest['release']=args.version
    manifest['files']=entries
    (out/'distribution-manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print('Verified local distribution:', out, 'files:', len(entries))
    if args.zip:
        archive = out.parent / (out.name + '.zip')
        if archive.exists():
            raise SystemExit('ZIP already exists: ' + str(archive))
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6, strict_timestamps=False) as z:
            for p in sorted(out.rglob('*')):
                if p.is_file():
                    z.write(p, str(Path(out.name)/p.relative_to(out)))
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        archive.with_suffix('.zip.sha256').write_text(digest+'  '+archive.name+'\n')
        print('Local ZIP:', archive, digest)

if __name__ == '__main__':
    main()
