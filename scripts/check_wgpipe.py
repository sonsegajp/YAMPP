"""Differentially check production FIFO stores and MMIO registration order."""
import json
import os
from pathlib import Path
import platform
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/tests/wgpipe'


def function(source, declaration):
    start = source.index(declaration)
    pos = source.index('{', start) + 1
    depth = 1
    while depth:
        if source[pos] == '{': depth += 1
        elif source[pos] == '}': depth -= 1
        pos += 1
    return source[start:pos]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    frontend = (ROOT / 'native/host/gxrt/frontend_bus.c').read_text(encoding='utf-8')
    gx = (ROOT / 'native/vendor/src/gx_recomp.c').read_text(encoding='utf-8')
    parts = [function(gx, 'static void trace_event('),
             function(gx, 'bool dol_gx_recomp_push_fifo('),
             function(frontend, 'void wgseg_inline_dl('),
             function(frontend, 'static bool wr_wgpipe(')]
    # Read real production device ranges, substituting only device callbacks.
    registrations = re.findall(r'dol_mmio_bus_register\(&g_bus,\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^\)]+)\);', frontend)
    assert len(registrations) == 11
    declarations = '\n'.join(f'{{{base},{size}}},' for base,size,*_ in registrations)
    (OUT/'production.inc').write_text('\n\n'.join(parts)+'\nstatic const u32 ranges[][2] = {\n'+declarations+'\n};\n', encoding='utf-8')
    windows = platform.system() == 'Windows'
    compiler = 'C:/msys64/mingw64/bin/gcc.exe' if windows else os.environ.get('CC', 'gcc')
    env = os.environ.copy()
    if windows: env['PATH'] = str(Path(compiler).parent)+os.pathsep+env['PATH']
    executable = OUT / ('wgpipe-test.exe' if windows else 'wgpipe-test')
    subprocess.run([compiler, '-O2', '-fno-strict-aliasing', '-I'+str(ROOT/'native/vendor/include'),
                    '-I'+str(OUT), str(ROOT/'native/host/gxrt/tests/wgpipe_test.c'),
                    str(ROOT/'native/vendor/src/mmio_bus.c'), '-o', str(executable)],
                   env=env, cwd=ROOT, check=True)
    reports=[]
    for diagnostics in (False, True):
        trial=env.copy()
        trial.pop('MELEE_TRACE_DIAGNOSTICS',None)
        if diagnostics: trial['MELEE_TRACE_DIAGNOSTICS']='1'
        result=subprocess.run([str(executable)],env=trial,cwd=ROOT,capture_output=True,text=True)
        reports.append({'diagnostics':diagnostics,'passed':result.returncode==0,'output':result.stdout+result.stderr})
    report={'platform':platform.system(),'passed':all(r['passed'] for r in reports),'runs':reports}
    (OUT/('report-windows.json' if windows else 'report-linux.json')).write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
    if not report['passed']: raise SystemExit(1)


if __name__ == '__main__': main()
