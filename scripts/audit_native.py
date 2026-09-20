"""Compile every Melee game/HSD translation unit with the native Windows ABI.

This is a compilation gate, not a claim of gameplay compatibility. No missing
functions are replaced by stubs and failed files are retained in the report.
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--clang', required=True)
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--filter', default='')
    args = parser.parse_args()
    source = ROOT / 'upstream/melee/src'
    out = ROOT / 'build/native-audit'
    out.mkdir(parents=True, exist_ok=True)
    files = sorted(p for base in ('melee', 'sysdolphin')
                   for p in (source / base).rglob('*.c')
                   if args.filter in str(p.relative_to(source)))
    common = [args.clang, '-c', '-std=gnu99', '-fms-extensions',
              '-fno-strict-aliasing', '-fno-builtin', '-ffp-contract=off',
              '-D_CRT_SECURE_NO_WARNINGS', '-D_USE_MATH_DEFINES',
              '-DTARGET_PC=1', '-DAURORA=1',
              '-Wno-microsoft', '-Wno-unknown-pragmas',
              '-Wno-incompatible-function-pointer-types',
              '-Wno-incompatible-pointer-types', '-Wno-int-conversion',
              '-Wno-deprecated-non-prototype', '-ferror-limit=12',
              '-include', str(ROOT / 'src/platform/native_prelude.h'),
              '-I', str(ROOT / 'src/platform/include'),
              '-I', str(ROOT / 'build/generated'), '-I', str(source), '-I', str(ROOT / 'upstream/aurora/include'),
              '-I', str(ROOT / 'upstream/melee/extern/dolphin/include')]

    def compile_one(path):
        name = path.relative_to(source).as_posix()
        stem = name.replace('/', '__')
        command = common + [str(path), '-o', str(out / (stem + '.obj'))]
        run = subprocess.run(command, capture_output=True, text=True, errors='replace')
        (out / (stem + '.log')).write_text(run.stdout + run.stderr, encoding='utf-8')
        errors = [s for s in run.stderr.splitlines() if 'error:' in s]
        return {'file': name, 'ok': run.returncode == 0, 'errors': errors,
                'log': str(out / (stem + '.log'))}

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for i, result in enumerate(pool.map(compile_one, files), 1):
            results.append(result)
            if i % 100 == 0 or i == len(files):
                print(f'{i}/{len(files)} compiled; {sum(r["ok"] for r in results)} passed', flush=True)
    report = {'total': len(results), 'passed': sum(r['ok'] for r in results),
              'compiler': args.clang, 'results': results}
    (out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k != 'results'}))
    return 0 if report['passed'] == report['total'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
