"""Differentially check optimized SDK transfers against actual emitted code."""
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/tests/sdk-matrix-transfer'

def function(source, name):
    start=source.index('void '+name+'(')
    cursor=source.index('{',start);depth=1;end=cursor+1
    while depth:
        if source[end]=='{':depth+=1
        elif source[end]=='}':depth-=1
        end+=1
    return source[start:end]

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    generated=(ROOT/'build/native-game/generated/recomp_0036.c').read_text(encoding='utf-8')
    body='\n\n'.join(function(generated,name) for name in ['func_80341408','func_8034143C'])
    (OUT/'original.c').write_text('#include "abi_recompcore.h"\n'+body+'\n')
    runtime=(ROOT/'native/runtime/module_rt.c').read_text(encoding='utf-8')
    begin=runtime.index('static const int kTypeSize[8]')
    end=runtime.index('/* ---- hooks',begin)
    generic=runtime[begin:end]
    (OUT/'generic-psq.c').write_text('#include "abi_recompcore.h"\n#include <math.h>\n'+generic)
    windows=platform.system()=='Windows'
    compiler='C:/msys64/mingw64/bin/gcc.exe' if windows else os.environ.get('CC','gcc')
    env=os.environ.copy()
    if windows:env['PATH']=str(Path(compiler).parent)+os.pathsep+env['PATH']
    executable=OUT/('matrix-test.exe' if windows else 'matrix-test')
    command=[compiler,'-O2','-fno-strict-aliasing','-I'+str(ROOT/'native/runtime'),'-I'+str(ROOT/'native/vendor/include'),
             str(ROOT/'native/host/gxrt/tests/sdk_matrix_transfer_test.c'),str(ROOT/'native/host/gxrt/hle_sdk_math.c'),
             str(OUT/'original.c'),str(OUT/'generic-psq.c'),'-lm','-o',str(executable)]
    subprocess.run(command,cwd=ROOT,env=env,check=True)
    result=subprocess.run([str(executable)],cwd=ROOT,env=env,capture_output=True,text=True)
    report={'passed':result.returncode==0,'platform':platform.system(),'output':result.stdout+result.stderr,
            'originalSha256':hashlib.sha256(body.encode()).hexdigest(),
            'genericPsqSha256':hashlib.sha256(generic.encode()).hexdigest(),'cases':98304,
            'checks':['FIFO bytes and ordered MMIO reads/writes','complete guest registers/Context',
                      'entire affected RAM','cached/uncached/unaligned/external source',
                      'overlapping RAM destinations','signed zero/subnormal/infinity/quiet and signaling NaNs',
                      'random float bit patterns and GQR contents','trace enabled and disabled']}
    (OUT/'report.json').write_text(json.dumps(report,indent=2))
    (OUT/('report-windows.json' if windows else 'report-linux.json')).write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
    if result.returncode:raise SystemExit(result.returncode)

if __name__=='__main__':main()
