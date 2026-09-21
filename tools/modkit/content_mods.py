"""Locally prepared upstream content. No assets are served by the relay."""
from pathlib import Path
import hashlib, json, os, struct
ROOT=Path(__file__).resolve().parents[2]

def boot_files(disc, destination):
    destination=Path(destination);destination.mkdir(parents=True,exist_ok=True)
    with Path(disc).open("rb") as source:
        source.seek(0x420);dol,fst,fst_size=struct.unpack(">3I",source.read(12))
        source.seek(dol);header=source.read(0x100)
        size=max(struct.unpack_from(">I",header,i*4)[0]+struct.unpack_from(">I",header,0x90+i*4)[0] for i in range(18))
        if not 0<size<32*1024*1024 or not 0<fst_size<16*1024*1024:raise ValueError("Invalid content boot files")
        for name,offset,length in (("main.dol",dol,size),("fst.bin",fst,fst_size)):
            source.seek(offset);data=source.read(length)
            if len(data)!=length:raise ValueError("Truncated content image")
            target=destination/name
            if not target.exists() or target.read_bytes()!=data:
                temporary=target.with_suffix(target.suffix+".tmp");temporary.write_bytes(data);temporary.replace(target)
    return destination/"main.dol",destination/"fst.bin"


def state_path():
    override=os.environ.get("MELEE_CONTENT_STATE")
    path=Path(override).resolve() if override else ROOT/"user/content-mods.json"
    if override and not path.is_relative_to(ROOT/"build"):raise ValueError("Invalid isolated content state")
    return path

def read_state():
    path=state_path()
    if not path.exists():return {"schema":1,"akaneia":None}
    if path.is_symlink():raise ValueError("Linked content state is not supported")
    value=json.loads(path.read_text(encoding="utf-8"))
    if value.get("schema")!=1:raise ValueError("Unsupported content state")
    return value

def save_state(value):
    path=state_path();path.parent.mkdir(parents=True,exist_ok=True)
    tmp=path.with_suffix(".tmp");tmp.write_bytes(json.dumps(value,indent=2).encode());tmp.replace(path)

def checked_content(record,full=False):
    if not isinstance(record,dict):return None
    root=(ROOT/record.get("directory","")).resolve()
    if not root.is_relative_to(ROOT/"build/akaneia-content"):raise ValueError("Invalid content directory")
    disc=root/"yampp-content.iso"
    if not disc.is_file() or disc.is_symlink():return None
    audit=json.loads((root/"content-audit.json").read_text())
    verification=json.loads((root/"verification.json").read_text())
    if audit.get("scope")!=["fighters","stages","music"] or not verification.get("passed"):raise ValueError("Content audit failed")
    if verification.get("isoSHA256")!=record.get("isoSHA256"):raise ValueError("Content receipt changed")
    if full:
        from upstream_download import digest_file
        if digest_file(disc)!=record["isoSHA256"]:raise ValueError("Installed content changed; prepare it again")
    return disc

def register(directory,enabled=False):
    root=Path(directory).resolve();verification=json.loads((root/"verification.json").read_text())
    record={"directory":root.relative_to(ROOT).as_posix(),"isoSHA256":verification["isoSHA256"],"enabled":bool(enabled)}
    if not checked_content(record,True):raise ValueError("Verified content is missing")
    value=read_state();value["akaneia"]=record;save_state(value);return record

def catalog_entry():
    from upstream_download import BUILDS
    build=BUILDS[0];record=read_state().get("akaneia");installed=bool(checked_content(record))
    # Report the running profile, never a checkbox that has not actually reloaded.
    enabled=installed and (bool(os.environ.get("MELEE_MEX_BASE_DOL")) if os.environ.get("MELEE_RUNTIME_DOL") else bool(record.get("enabled")))
    return {"id":"akaneia","name":"Akaneia","version":build["version"],"kind":"content","base":"GALE01",
            "description":"7 fighters, 17 stages and 41 music tracks. Official GitHub release.",
            "sha256":build["archive"]["sha256"],"size":build["archive"]["size"],"costumeCount":0,
            "installed":installed,"enabled":enabled,"source":"github"}

def stage_files_current(record):
    if not checked_content(record):return False
    root=ROOT/record["directory"]
    audit=json.loads((root/"content-audit.json").read_text())
    verification=json.loads((root/"verification.json").read_text())
    return audit.get("stageFileRevision")==1 and verification.get("stageFilesVerified")==len(audit.get("stageFiles",{})) and bool(audit.get("stageFiles"))


def toggle(enabled):
    if os.environ.get("MELEE_RUNTIME_DOL") and os.environ.get("MELEE_CONTENT_LAUNCHER")!="1":
        raise ValueError("Open the current YAMPP launcher before switching content mods")
    value=read_state();record=value.get("akaneia")
    if not checked_content(record,True):raise ValueError("Install Akaneia first")
    # Existing tester installs may predate supplemental stage-file collection.
    # Explicitly enabling already-acquired content rebuilds it with this importer.
    if enabled and not stage_files_current(record):return prepare(confirmed=True)
    record["enabled"]=bool(enabled);save_state(value)
    from upstream_download import BUILDS
    return {"sha256":BUILDS[0]["archive"]["sha256"],"enabled":bool(enabled),"restartRequired":True}

class ContentImportError(ValueError):
    def __init__(self, message, code):
        super().__init__(message); self.ui_code = code


def _cleanup_import(work, keep_content=False):
    """Remove only this invocation's disposable files; retain its diagnostic log."""
    import shutil
    boundary=(ROOT/'build/akaneia-content').resolve()
    if work.is_symlink() or work.resolve().parent != boundary or not work.name.startswith('import-'):
        raise ValueError('Invalid import cleanup directory')
    for child in work.iterdir():
        if child.name in ('prepare.log','failure.json') or (keep_content and child.name=='content'):continue
        # Never traverse a link/junction planted inside the working directory.
        if child.is_symlink():child.unlink()
        elif getattr(child.lstat(),'st_file_attributes',0)&0x400:continue
        elif child.is_dir():shutil.rmtree(child)
        else:child.unlink()


def prepare(confirmed=False,allow_download=False,progress=None):
    from upstream_download import _lock, _ordinary
    boundary=ROOT/'build/akaneia-content';boundary.mkdir(parents=True,exist_ok=True)
    for part in (boundary,*boundary.parents):
        _ordinary(part)
        if part==ROOT:break
    with _lock(boundary):
        return _prepare(confirmed,allow_download,progress)


def _prepare(confirmed=False,allow_download=False,progress=None):
    """Acquire only from official GitHub, then compose content against the user's ISO."""
    import shutil,subprocess,sys,tempfile
    from upstream_download import acquire,identity,BUILDS,_download,_verified,digest_file
    build=BUILDS[0]
    from job_progress import Progress
    if progress is None: progress=Progress(None)
    progress.begin("verify")
    result=acquire(identity(build),confirmed=confirmed,allow_download=allow_download,progress=progress)
    # acquire verifies the download; this helper also supplies the native importer.
    result.update(runtimeSupported=True,message="Akaneia content verified. Reloading the menu.")
    if checked_content(read_state().get("akaneia"),True) and stage_files_current(read_state().get("akaneia")):return dict(result,**toggle(True))
    sys.path.insert(0,str(ROOT/"scripts"))
    from project_config import configuration,disc_path
    base=disc_path(configuration())
    if digest_file(base,"md5")!=build["baseMD5"]:raise ValueError("Use your original Melee 1.02 ISO")
    portable=ROOT/"tools/content-importer/MexContentBridge.exe"
    bridge=ROOT/"build/modkit/mex-content-bridge/MexContentBridge.dll"
    if portable.is_file():bridge_command=[str(portable)]
    elif bridge.is_file():bridge_command=["dotnet",str(bridge)]
    else:raise ValueError("The content importer is missing from this build")
    folder=Path(result["archivePath"]).parent
    progress.begin("metadata")
    core=_download(folder,{"name":"mex-core.json","url":"https://raw.githubusercontent.com/akaneia/akaneia-build/"+build["commit"]+"/data/mex.json","size":168004,"sha256":"7883224a083444cf99b2c335ab372503bef6d02242ce10ad950a35e36b5c1d3d"})
    boundary=ROOT/"build/akaneia-content";boundary.mkdir(parents=True,exist_ok=True)
    # Patched disc, extracted source, composed workspace and final ISO coexist.
    if shutil.disk_usage(boundary).free < 6*1024**3:
        raise ContentImportError('Akaneia preparation needs at least 6 GB of free space on the YAMPP drive.', 'import_space')
    work=Path(tempfile.mkdtemp(prefix="import-",dir=boundary))
    try:
        import py7zr
        progress.begin("unpack")
        members=[build["patch"]["path"],build["windowsPatcher"]["path"]]
        with py7zr.SevenZipFile(result["archivePath"],"r") as archive:
            archive.extract(path=work,targets=members)
        patch=work/members[0];patcher=work/members[1]
        for path,spec in ((patch,build["patch"]),(patcher,build["windowsPatcher"])):
            if path.is_symlink() or not path.resolve().is_relative_to(work) or digest_file(path)!=spec["sha256"]:raise ValueError("Upstream patch integrity failed")
        full=work/"official.iso"
        if os.name!="nt":patcher=shutil.which("xdelta3") or "xdelta3"
        creationflags=getattr(subprocess,"CREATE_NO_WINDOW",0)
        with (work/"prepare.log").open("w") as log:
            progress.begin("patch")
            subprocess.run([str(patcher),"-d","-s",str(base),str(patch),str(full)],check=True,stdout=log,stderr=log,creationflags=creationflags)
            if digest_file(full,"md5")!=build["resultMD5"]:raise ValueError("Official patch result failed verification")
            # The extractor accepts the pinned patched DOL only in this process.
            import extract_disc
            extract_disc.DOL_SHA1=hashlib.sha1(boot_files(full,work/"boot")[0].read_bytes()).hexdigest()
            if digest_file(work/"boot/main.dol")!=build["resultDolSHA256"]:raise ValueError("Unexpected upstream executable")
            progress.begin("extract")
            source=work/"source";extract_disc.extract(full,source)
            output=work/"content"
            progress.begin("compose")
            subprocess.run(bridge_command+["compose",str(source),str(core),str(base),str(output)],cwd=ROOT,check=True,stdout=log,stderr=log,creationflags=creationflags)
            progress.begin("check")
            subprocess.run([sys.executable,str(ROOT/"scripts/check_akaneia_content.py"),str(output),"--source",str(source)],cwd=ROOT,check=True,stdout=log,stderr=log,creationflags=creationflags)
        register(output,True)
        progress.finish()
    except Exception as error:
        low_space=shutil.disk_usage(boundary).free < 512*1024**2 or getattr(error,'errno',None)==28 or getattr(error,'winerror',None)==112
        (work/'failure.json').write_text(json.dumps({'error':str(error)},indent=2),encoding='utf-8')
        try:_cleanup_import(work)
        except OSError:pass # Preserve the original failure; another process may still hold a diagnostic file.
        if low_space:raise ContentImportError('Akaneia preparation ran out of disk space. Free at least 6 GB and try again.', 'import_space') from error
        raise ContentImportError('Akaneia could not be prepared. See the latest build/akaneia-content/import-*/prepare.log.', 'import_failed') from error
    try:_cleanup_import(work,keep_content=True)
    except OSError:pass # A successful installation must not be rejected for cleanup alone.
    return dict(result,**toggle(True))
