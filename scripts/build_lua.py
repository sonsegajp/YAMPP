"""Build the pinned Lua VM and syntax checker for game and Workshop."""
from pathlib import Path
import concurrent.futures,hashlib,os,subprocess,urllib.request,tarfile
ROOT=Path(__file__).resolve().parents[1]
IS_WINDOWS=os.name=="nt"
VERSION='5.5.1'
SHA='1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce'
def main():
 src=ROOT/'upstream'/('lua-'+VERSION)/'src';out=ROOT/('build/native-game' if IS_WINDOWS else 'build/native-game-linux');objdir=out/'lua-objects';objdir.mkdir(parents=True,exist_ok=True)
 if not src.exists():
  archive=ROOT/'build/dependency-cache'/('lua-'+VERSION+'.tar.gz');archive.parent.mkdir(parents=True,exist_ok=True)
  if not archive.exists():urllib.request.urlretrieve('https://www.lua.org/ftp/'+archive.name,archive)
  if hashlib.sha256(archive.read_bytes()).hexdigest()!=SHA:raise ValueError('Lua source checksum mismatch')
  with tarfile.open(archive) as tf:
   base=(ROOT/'upstream').resolve()
   for m in tf.getmembers():
    if not (base/m.name).resolve().is_relative_to(base) or m.issym() or m.islnk():raise ValueError('Invalid archive path')
   tf.extractall(base)
 cc=Path('C:/msys64/mingw64/bin/gcc.exe') if IS_WINDOWS else Path(os.environ.get('CC','gcc'));env=os.environ.copy()
 if IS_WINDOWS:env['PATH']=str(cc.parent)+os.pathsep+env['PATH']
 sources=[p for p in sorted(src.glob('*.c')) if p.name not in ('lua.c','luac.c')]
 def compile(p):
  obj=objdir/(p.stem+'.o');subprocess.run([str(cc),'-c','-O2',*(['-DLUA_BUILD_AS_DLL'] if IS_WINDOWS else ['-fPIC','-DLUA_USE_LINUX']),str(p),'-o',str(obj)],check=True,env=env);return obj
 with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:objects=list(pool.map(compile,sources))
 if IS_WINDOWS:
  subprocess.run([str(cc),'-shared','-o',str(out/'lua55.dll'),*map(str,objects),'-Wl,--out-implib,'+str(out/'liblua55.dll.a'),'-lm'],check=True,env=env)
  subprocess.run([str(cc),'-O2',str(src/'luac.c'),*map(str,objects),'-o',str(out/'luac.exe'),'-lm'],check=True,env=env)
  print(out/'lua55.dll')
 else:
  subprocess.run(['ar','rcs',str(out/'liblua55.a'),*map(str,objects)],check=True,env=env)
  subprocess.run([str(cc),'-O2',str(src/'luac.c'),*map(str,objects),'-o',str(out/'luac'),'-lm','-ldl'],check=True,env=env)
  print(out/'liblua55.a')
if __name__=='__main__':main()
