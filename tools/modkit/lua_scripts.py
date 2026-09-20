"""Lua authoring operations for the private Workshop desktop worker."""
import hashlib,os,re,subprocess,tempfile
from pathlib import Path
from catalog import ROOT,MODS,find_fighter,runtime_registry
from xml_manifest import read_manifest,write_manifest
MAX_BYTES=1024*1024
TEMPLATE='-- Runs in Melee\'s native Lua VM. Each player owns an isolated instance.\nlocal fighter = { api_version = 1 }\n\nfunction fighter.on_spawn(self)\n    self:log("Lua fighter loaded")\nend\n\nfunction fighter.on_frame(self)\n    -- Read self:state() and use the fighter methods documented in Lua API.\nend\n\nreturn fighter\n'

def package(id):
    f=find_fighter(id)
    if not f:raise ValueError('Unknown fighter')
    if f['stock']:raise ValueError('Clone this fighter before adding Lua scripts')
    return MODS/f['id']

def script_path(folder,name):
    if not re.fullmatch(r'scripts/[A-Za-z0-9_-]+(?:/[A-Za-z0-9_-]+)*\.lua',name):raise ValueError('Use a .lua file inside scripts/')
    path=(folder/name).resolve()
    if not path.is_relative_to(folder.resolve()):raise ValueError('Script path escapes package')
    return path

def validate(source,name='scripts/fighter.lua'):
    if not isinstance(source,str) or len(source.encode('utf-8'))>MAX_BYTES:raise ValueError('Lua source exceeds 1 MiB')
    checker=ROOT/'build/native-game/luac.exe'
    if not checker.exists():raise ValueError('Build Lua with python scripts/build_lua.py first')
    cache=ROOT/'build/modkit/lua-check';cache.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=cache) as temp:
        path=Path(temp)/Path(name).name;path.write_text(source,encoding='utf-8')
        r=subprocess.run([str(checker),'-p',str(path)],capture_output=True,text=True,timeout=10,creationflags=subprocess.CREATE_NO_WINDOW)
        diagnostic=(r.stderr+r.stdout).strip().replace(str(path),name)
        if r.returncode:
            # luac abbreviates long chunk paths; expose the script and line.
            match=re.search(r':(\d+): (.*)',diagnostic)
            if match:diagnostic=f'{name}:{match.group(1)}: {match.group(2)}'
        return {'valid':r.returncode==0,'diagnostic':diagnostic or 'Lua syntax is valid.','version':'5.5.1'}

def read(id,name=None):
    folder=package(id);manifest=read_manifest(folder);files=sorted(p.relative_to(folder).as_posix() for p in (folder/'scripts').rglob('*.lua'))
    entry=manifest.get('lua',{}).get('entry','scripts/fighter.lua');name=name or entry
    p=script_path(folder,name)
    return {'files':files or [entry],'file':name,'entry':entry,'source':p.read_text(encoding='utf-8') if p.exists() else TEMPLATE,'enabled':bool(manifest.get('lua')),'api':1,'language':'Lua 5.5.1','reference':(ROOT/'docs/lua-api.md').read_text(encoding='utf-8')}

def save(data):
    folder=package(data['id']);name=data.get('file','scripts/fighter.lua');path=script_path(folder,name);result=validate(data['source'],name)
    if not result['valid']:raise ValueError(result['diagnostic'])
    path.parent.mkdir(parents=True,exist_ok=True);temp=path.with_suffix('.lua.tmp');temp.write_text(data['source'],encoding='utf-8');temp.replace(path)
    manifest=read_manifest(folder)
    if data.get('entry',name=='scripts/fighter.lua'):manifest['lua']={'api':1,'entry':name}
    write_manifest(folder,manifest);runtime_registry()
    return dict(result,saved=True,file=name)

def launch_packaged():
    # Native Setup already supports ISO/GCM/RVZ/WIA. Reuse its validated local
    # extraction rather than importing the source-only ISO extractor or Tk.
    from project_config import configuration,project_path,disc_path
    cfg=configuration();assets=project_path(cfg['assets']['directory'])
    dol=assets/'sys/main.dol';fst=assets/'sys/fst.bin'
    exe=project_path(cfg['runtime']['developmentExecutable'])
    renderer=project_path(cfg['runtime']['developmentRenderer'])
    if not dol.is_file() or not fst.is_file():raise ValueError('Run Setup.cmd before testing a fighter.')
    if hashlib.sha1(dol.read_bytes()).hexdigest()!=cfg['identity']['dolSha1']:
        raise ValueError('The extracted game does not match this build. Run Setup.cmd again.')
    if not exe.is_file() or not renderer.is_file():raise ValueError('The packaged game or renderer is missing.')
    try:disc=disc_path(cfg)
    except ValueError:
        remembered=ROOT/'user/disc.path'
        if not remembered.is_file() or remembered.stat().st_size>32768:
            raise ValueError('Run Setup.cmd to select your disc before testing a fighter.') from None
        disc=Path(remembered.read_text(encoding='utf-8-sig').strip())
        if not disc.is_file():raise ValueError('The saved disc image is missing. Run Setup.cmd again.') from None
    runtime_registry()
    user=cfg['user'];mods=project_path(user['mods']);cache=project_path(user['cache'])
    cache.mkdir(parents=True,exist_ok=True)
    for key in ('settings','memoryCard'):project_path(user[key]).parent.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy()
    for key in list(env):
        if key.startswith(('MELEE_TEST_','MELEE_CAPTURE_','MELEE_AUDIO_','MELEE_PERF_','MELEE_TRACE_','MELEE_RENDERDOC_','GCN_GXT_','MELEE_MOD_TEST_')) or key in ('MELEE_INPUT','GCN_AURORA_HIDDEN','MELEE_NETPLAY_AUTO','MELEE_NETPLAY_INPUT','MELEE_NETPLAY_RULES','MELEE_RB_DIAG'):
            env.pop(key)
    # Test Game explicitly opts into enabled local fighter/stage packages for
    # this child process; it does not alter the normal launcher's preferences.
    env.update(MELEE_DISC=str(disc),MELEE_FST=str(fst),MELEE_AURORA_DLL=str(renderer),
        MELEE_SETTINGS=str(project_path(user['settings'])),MELEE_MEMORY_CARD=str(project_path(user['memoryCard'])),
        MELEE_MOD_REGISTRY=str(mods/'registry.tsv'),MELEE_COSTUME_REGISTRY=str(mods/'costumes.tsv'),
        MELEE_MODS='1',GCN_AURORA_DATA_DIR=str(cache))
    env.setdefault('MELEE_WORKSHOP_PYTHON',str(ROOT/'tools/python/python.exe'))
    env['PATH']=str(ROOT/'bin')+os.pathsep+str(ROOT/'tools/python')+os.pathsep+env.get('PATH','')
    logs=ROOT/'build/playtest';logs.mkdir(parents=True,exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix='workshop-',suffix='.log',dir=logs,delete=False) as log:
        child=subprocess.Popen([str(exe),str(dol),'0'],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW)
    return {'launched':True,'pid':child.pid,'message':'Game opened with enabled local mods. Select your character in the normal CSS.'}


def launch():
    if (ROOT/'bin/YAMPP.exe').is_file():return launch_packaged()
    p=subprocess.run([os.sys.executable,str(ROOT/'scripts/project_config.py'),'--launch','game','--development','--enable-mods'],cwd=ROOT,capture_output=True,text=True,timeout=120,creationflags=subprocess.CREATE_NO_WINDOW)
    if p.returncode:raise ValueError((p.stderr+p.stdout)[-2000:])
    return {'launched':True,'message':'Game opened with enabled local mods. Select your character in the normal CSS.','output':p.stdout.strip()}
