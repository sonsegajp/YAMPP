"""Build the streaming RVZ/WIA verifier using the gameplay decoder."""
import os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def build():
 windows=os.name=='nt';cc='C:/msys64/mingw64/bin/gcc.exe' if windows else os.environ.get('CC','cc')
 out=ROOT/'build/native-game'/('YAMPP-disc-identity.exe' if windows else 'YAMPP-disc-identity')
 out.parent.mkdir(parents=True,exist_ok=True)
 env=os.environ.copy()
 if windows:env['PATH']=str(Path(cc).parent)+os.pathsep+env.get('PATH','')
 subprocess.run([cc,'-O2','-s','-I'+str(ROOT/'native/vendor/include'),str(ROOT/'native/tools/disc_identity.c'),str(ROOT/'native/vendor/src/rvz.c'),'-lcrypto','-lzstd','-o',str(out)],env=env,check=True)
 print(out)
 return out
if __name__=='__main__':build()
