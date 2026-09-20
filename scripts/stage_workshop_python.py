"""Stage an offline, private Windows Python runtime for Melee Workshop.

Only the standard library, NumPy, Pillow, the content archive reader, and notices
are copied. No pip, user site packages, downloads, or installer are needed.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE_PACKAGES = {'py7zr':'1.1.3','pycryptodomex':'3.23.0','brotli':'1.2.0',
                    'psutil':'7.2.2','backports.zstd':'1.6.0','pyppmd':'1.3.1',
                    'pybcj':'1.0.7','multivolumefile':'0.2.3','inflate64':'1.0.4','texttable':'1.7.0'}
IGNORED = {'__pycache__', 'tests', 'test', 'site-packages', 'ensurepip',
           'idlelib', 'turtledemo', 'tkinter'}


def ignore_unused(directory, names):
    return [name for name in names if name in IGNORED or name.endswith(('.pyc', '.pyo'))]


def run_json(executable, code):
    result = subprocess.run([str(executable), '-I', '-B', '-c', code], check=True,
                            capture_output=True, text=True)
    return json.loads(result.stdout)


def stage(source, output):
    source = source.resolve()
    output = output.resolve()
    if not output.is_relative_to((ROOT / 'build').resolve()):
        raise ValueError('The staging directory must be inside the project build directory')
    if output.exists():
        raise ValueError('The staging directory already exists; select a new --output directory')
    info = run_json(source / 'python.exe',
        'import json,sys,struct,importlib.metadata as m;'
        'print(json.dumps(dict(python=sys.version.split()[0],bits=struct.calcsize("P")*8,'
        'platform=sys.platform,numpy=m.version("numpy"),pillow=m.version("pillow"))))')
    if info['platform'] != 'win32' or info['bits'] != 64 or not info['python'].startswith('3.10.'):
        raise ValueError('Expected an installed 64-bit Windows Python 3.10 runtime')

    site = source / 'Lib/site-packages'
    packages = ['numpy', 'numpy.libs', 'numpy-' + info['numpy'] + '.dist-info',
                'PIL', 'pillow-' + info['pillow'] + '.dist-info']
    if (site / 'pillow.libs').is_dir():
        packages.append('pillow.libs')
    for name in packages:
        if not (site / name).is_dir():
            raise ValueError('Required installed package is missing: ' + name)

    archive_info = run_json(source / 'python.exe',
        'import json,importlib.metadata as m;names='+repr(list(ARCHIVE_PACKAGES))+';'
        'print(json.dumps({n:dict(version=m.version(n),files=[str(p) for p in m.files(n)]) for n in names}))')
    for name, expected in ARCHIVE_PACKAGES.items():
        if archive_info[name]['version'] != expected:
            raise ValueError('Unexpected archive dependency version: ' + name)
    output.mkdir(parents=True)
    for name in ['python.exe', 'pythonw.exe', 'python3.dll', 'python310.dll',
                 'vcruntime140.dll', 'vcruntime140_1.dll', 'LICENSE.txt']:
        shutil.copy2(source / name, output / name)
    shutil.copytree(source / 'Lib', output / 'Lib', ignore=ignore_unused)
    (output / 'DLLs').mkdir()
    for item in (source / 'DLLs').iterdir():
        if item.suffix.lower() not in ('.dll', '.pyd'):
            continue
        if item.name.startswith(('_test', '_ctypes_test', '_tkinter', 'tcl', 'tk')):
            continue
        shutil.copy2(item, output / 'DLLs' / item.name)
    for name in packages:
        shutil.copytree(site / name, output / 'Lib/site-packages' / name, ignore=ignore_unused)

    # Copy only files listed by these pinned distributions. Namespace packages
    # (backports) must not pull unrelated installed packages into the build.
    for name, package_info in archive_info.items():
        for value in package_info['files']:
            relative = Path(value)
            if relative.is_absolute() or '..' in relative.parts:
                continue  # Installed command-line wrappers are not needed.
            if any(part.lower() in ('__pycache__', 'tests', 'test', 'selftest') for part in relative.parts):
                continue
            origin = site / relative
            if not origin.is_file() or origin.is_symlink():
                raise ValueError('Missing or linked archive dependency: ' + str(origin))
            target = output / 'Lib/site-packages' / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(origin, target)

    # No `import site`: user packages, PYTHONPATH, registry paths and .pth startup
    # code cannot leak into the distributed interpreter. The modkit sibling is
    # the final tools/python + tools/modkit layout; desktop_worker also adds its
    # own source directory explicitly so the staging runtime can test it.
    (output / 'python310._pth').write_text(
        '# YAMPP private Workshop runtime\n.\nDLLs\nLib\nLib/site-packages\n../modkit\n',
        encoding='utf-8')
    notices = output / 'licenses'
    notices.mkdir()
    shutil.copy2(source / 'LICENSE.txt', notices / 'Python-LICENSE.txt')
    shutil.copy2(site / ('numpy-' + info['numpy'] + '.dist-info') / 'LICENSE.txt',
                 notices / 'NumPy-LICENSE.txt')
    shutil.copytree(site / ('pillow-' + info['pillow'] + '.dist-info') / 'licenses',
                    notices / 'Pillow')
    (notices / 'README.txt').write_text(
        'Python ' + info['python'] + ': Python-LICENSE.txt (including bundled-library notices)\n'
        'NumPy ' + info['numpy'] + ': NumPy-LICENSE.txt (including OpenBLAS/runtime notices)\n'
        'Pillow ' + info['pillow'] + ': Pillow/LICENSE (including bundled-library notices)\n'
        'Original package metadata and notices are also retained in Lib/site-packages.\n',
        encoding='utf-8')

    with (notices/'README.txt').open('a',encoding='utf-8') as note:
        for name, version in ARCHIVE_PACKAGES.items():
            note.write(name+' '+version+': exact license notices retained in Lib/site-packages distribution metadata.\n')

    verified = run_json(output / 'python.exe',
        'import json,sys,ssl,sqlite3,ctypes,io,numpy as np;from PIL import Image;'
        'import py7zr;archive=io.BytesIO();'
        'writer=py7zr.SevenZipFile(archive,mode="w");writer.writestr(b"YAMPP archive smoke","test.txt");writer.close();'
        'archive.seek(0);reader=py7zr.SevenZipFile(archive,mode="r");assert reader.testzip() is None;reader.close();'
        'b=io.BytesIO();Image.new("RGBA",(2,2),(1,2,3,255)).save(b,format="PNG");'
        'assert Image.open(io.BytesIO(b.getvalue())).getpixel((0,0))==(1,2,3,255);'
        'assert float(np.linalg.det(np.eye(3)))==1.0;'
        'print(json.dumps(dict(isolated=sys.flags.isolated,no_site=sys.flags.no_site,'
        'numpy=np.__version__,pillow=Image.__version__,openssl=ssl.OPENSSL_VERSION,'
        'paths=sys.path)))')
    if not verified['isolated'] or not verified['no_site']:
        raise ValueError('Staged interpreter is not isolated')
    if any(not Path(p).resolve().is_relative_to(output.parent) for p in verified['paths']):
        raise ValueError('Staged interpreter imported an external search path')
    files = []
    for path in sorted(output.rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            raw = path.read_bytes()
            files.append({'path': path.relative_to(output).as_posix(), 'size': len(raw),
                          'sha256': hashlib.sha256(raw).hexdigest()})
    manifest = {'schema': 1, 'python': info['python'], 'architecture': 'win-amd64',
                'packages': {'numpy': info['numpy'], 'pillow': info['pillow'], **ARCHIVE_PACKAGES},
                'files': files, 'bytes': sum(f['size'] for f in files),
                'isolated': True, 'thirdPartyNotices': 'licenses/README.txt'}
    (output / 'distribution-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'output': str(output), 'python': info['python'], 'packages': manifest['packages'],
                      'files': len(files), 'bytes': manifest['bytes'], 'smoke': verified}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(sys.base_prefix))
    parser.add_argument('--output', type=Path, default=ROOT / 'build/distribution-python')
    arguments = parser.parse_args()
    stage(arguments.source, arguments.output)
