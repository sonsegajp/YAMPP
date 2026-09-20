"""XML project configuration and ISO-derived asset preparation for every launcher."""
from pathlib import Path
import argparse, hashlib, json, os, struct, subprocess, sys, time
import xml.etree.ElementTree as ET
ROOT = Path(__file__).resolve().parents[1]

def read_xml(path, root_tag):
    data = Path(path).read_bytes()
    if len(data) > 8 * 1024 * 1024 or b"<!DOCTYPE" in data or b"<!ENTITY" in data:
        raise ValueError("Unsupported XML document: " + str(path))
    doc = ET.fromstring(data)
    if doc.tag != root_tag or doc.get("schema") != "1":
        raise ValueError("Unsupported configuration schema: " + str(path))
    return doc

def write_xml(path, doc):
    path = Path(path); path.parent.mkdir(parents=True, exist_ok=True)
    ET.indent(doc)
    temp = path.with_suffix(path.suffix + ".tmp")
    ET.ElementTree(doc).write(temp, encoding="utf-8", xml_declaration=True)
    temp.replace(path)

def configuration():
    doc = read_xml(ROOT / "config/project.xml", "melee-project")
    cfg = {node.tag: dict(node.attrib) for node in doc if node.tag != "dependencies"}
    cfg["identity"] = dict(doc.attrib)
    return cfg

def project_path(value):
    p = (ROOT / value).resolve()
    if not p.is_relative_to(ROOT): raise ValueError("Project path escapes the workspace")
    return p

def disc_path(cfg=None, choose=False):
    cfg = cfg or configuration()
    file = project_path(cfg["user"]["disc"])
    if file.exists():
        p = Path(read_xml(file, "disc").get("path", ""))
        if p.is_file(): return p.resolve()
    # Native Setup accepts compressed discs too and retains this plain-text
    # pointer for older portable builds. Never feed RVZ/WIA to the ISO extractor.
    remembered=ROOT/'user/disc.path'
    if remembered.is_file() and remembered.stat().st_size<=32768:
        candidate=Path(remembered.read_text(encoding='utf-8-sig').strip())
        if candidate.is_file():return candidate.resolve()
    if choose:
        import tkinter as tk
        from tkinter.filedialog import askopenfilename
        window = tk.Tk(); window.withdraw()
        selected = askopenfilename(title="Select your Melee USA 1.02 ISO", filetypes=[("GameCube disc", "*.iso *.gcm")])
        window.destroy()
        if selected:
            p = Path(selected).resolve()
            validate_disc(p, cfg)
            write_xml(file, ET.Element("disc", schema="1", path=str(p)))
            return p
    raise ValueError("Select a Melee USA 1.02 ISO with python scripts/project_config.py --iso PATH")

def validate_disc(path, cfg):
    from extract_disc import read_exact, be32
    with path.open("rb") as f:
        header = read_exact(f, 0, 0x440)
        ident = cfg["identity"]
        if header[:6].decode("ascii") != ident["discId"] or header[7] != int(ident["revision"]) or be32(header, 0x1c) != 0xc2339f3d:
            raise ValueError("Expected the Melee USA 1.02 GameCube disc")
        dol_off, fst_off, fst_size = struct.unpack_from(">3I", header, 0x420)
        dh = read_exact(f, dol_off, 0x100)
        dol_size = max(be32(dh, i*4) + be32(dh, 0x90+i*4) for i in range(18))
        dol = read_exact(f, dol_off, dol_size)
        if hashlib.sha1(dol).hexdigest() != ident["dolSha1"]: raise ValueError("Unexpected DOL hash")
        return dol_off, dol_size, fst_off, fst_size

def ensure_assets(cfg, disc):
    from extract_disc import read_exact, be32
    dol_off, dol_size, fst_off, fst_size = validate_disc(disc, cfg)
    folder = project_path(cfg["assets"]["directory"])
    manifest = project_path(cfg["assets"]["manifest"])
    old = {}
    if manifest.exists():
        old_doc = read_xml(manifest, "extracted-assets")
        if old_doc.get("dolSha1") == cfg["identity"]["dolSha1"]:
            old = {n.get("path"): n.attrib for n in old_doc.findall("file")}
    records = [("sys/main.dol", dol_off, dol_size), ("sys/fst.bin", fst_off, fst_size)]
    with disc.open("rb") as stream:
        fst = read_exact(stream, fst_off, fst_size)
        count = be32(fst, 8)
        if not 1 <= count <= len(fst)//12: raise ValueError("Invalid FST")
        names = fst[count*12:]; stack = [(count, Path("files"))]
        for i in range(1, count):
            while stack and i >= stack[-1][0]: stack.pop()
            if not stack: raise ValueError("Invalid FST directory")
            flags, offset, size = struct.unpack_from(">3I", fst, i*12)
            start = flags & 0xffffff; end = names.find(b"\0", start)
            if start >= len(names) or end < 0: raise ValueError("Invalid FST filename")
            name = names[start:end].decode("ascii")
            if name in ("", ".", "..") or any(c in name for c in '/\\:'): raise ValueError("Unsafe FST filename")
            relative = stack[-1][1] / name
            if flags >> 24:
                if not i < size <= stack[-1][0]: raise ValueError("Invalid FST directory range")
                stack.append((size, relative))
            else: records.append((relative.as_posix(), offset, size))
        doc = ET.Element("extracted-assets", schema="1", discId=cfg["identity"]["discId"], dolSha1=cfg["identity"]["dolSha1"])
        repaired = 0
        for relative, offset, size in records:
            target = (folder / relative).resolve()
            if not target.is_relative_to(folder): raise ValueError("Unsafe extraction target")
            prior = old.get(relative, {})
            digest = None
            if target.is_file() and target.stat().st_size == size:
                digest = hashlib.sha256(target.read_bytes()).hexdigest()
            if not digest or prior.get("sha256") != digest or prior.get("offset") != str(offset) or prior.get("size") != str(size):
                data = read_exact(stream, offset, size); source_hash = hashlib.sha256(data).hexdigest()
                if digest != source_hash:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    temp = target.with_name(target.name + ".extracting"); temp.write_bytes(data); temp.replace(target); repaired += 1
                digest = source_hash
            ET.SubElement(doc, "file", path=relative, offset=str(offset), size=str(size), sha256=digest)
    write_xml(manifest, doc)
    return {"files": len(records), "repaired": repaired, "manifest": str(manifest)}

def prepare(choose=False):
    cfg = configuration(); disc = disc_path(cfg, choose)
    report = ensure_assets(cfg, disc)
    user = cfg["user"]
    for key in ("memoryCard", "settings"):
        project_path(user[key]).parent.mkdir(parents=True, exist_ok=True)
    for key in ("mods", "cache"): project_path(user[key]).mkdir(parents=True, exist_ok=True)
    settings = project_path(user["settings"])
    if not settings.exists():
        write_xml(settings, ET.Element("melee-settings", schema="1", width="960", height="720", renderScale="1", widescreen="0", fullscreen="0", vsync="0", volume="100", mute="0", showFps="0"))
    sys.path.insert(0, str(ROOT / "tools/modkit"))
    from catalog import runtime_registry
    runtime_registry()
    return cfg, disc, report

def game_environment(cfg, disc, development=False):
    env = os.environ.copy()
    env.update(MELEE_DISC=str(disc), MELEE_FST=str(project_path(cfg["assets"]["directory"])/"sys/fst.bin"),
               MELEE_MEMORY_CARD=str(project_path(cfg["user"]["memoryCard"])), MELEE_SETTINGS=str(project_path(cfg["user"]["settings"])),
               MELEE_MOD_REGISTRY=str(project_path(cfg["user"]["mods"])/"registry.tsv"), MELEE_COSTUME_REGISTRY=str(project_path(cfg["user"]["mods"])/"costumes.tsv"), MELEE_MODS="1",
               MELEE_AURORA_DLL=str(project_path(cfg["runtime"]["developmentRenderer" if development else "renderer"])),
               GCN_AURORA_DATA_DIR=str(project_path(cfg["user"]["cache"])))
    if cfg["user"].get("modsEnabled", "1") == "0":
        # Custom packages (Diddy Kong, custom Falcon) stay installed but do not load.
        env.pop("MELEE_MODS", None)
    env["PATH"] = str(ROOT / "build/pc/bin") + os.pathsep + "C:/msys64/mingw64/bin" + os.pathsep + env["PATH"]
    return env

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", type=Path); parser.add_argument("--launch", choices=["game", "workshop"])
    parser.add_argument("--development", action="store_true"); parser.add_argument("--check", action="store_true")
    parser.add_argument("--enable-mods", action="store_true", help="Enable local fighter/stage mods for this playtest process only")
    parser.add_argument("--runtime-profile", help="Workspace XML describing a frozen test executable and renderer")
    args = parser.parse_args(); cfg = configuration()
    if args.iso:
        validate_disc(args.iso, cfg)
        write_xml(project_path(cfg["user"]["disc"]), ET.Element("disc", schema="1", path=str(args.iso.resolve())))
    cfg, disc, report = prepare(choose=bool(args.launch)); print(json.dumps(report))
    if args.launch == "game":
        env = game_environment(cfg, disc, args.development)
        if args.enable_mods: env["MELEE_MODS"] = "1"
        for key in list(env):
            if key.startswith(("MELEE_TEST_", "MELEE_CAPTURE_", "MELEE_AUDIO_", "MELEE_PERF_", "MELEE_TRACE_", "MELEE_RENDERDOC_", "GCN_GXT_", "MELEE_MOD_TEST_")) or key in ("MELEE_INPUT", "GCN_AURORA_HIDDEN", "MELEE_NETPLAY_AUTO", "MELEE_NETPLAY_INPUT", "MELEE_NETPLAY_RULES", "MELEE_RB_DIAG"): env.pop(key)
        exe = project_path(cfg["runtime"]["developmentExecutable" if args.development else "executable"])
        if args.runtime_profile:
            profile = read_xml(project_path(args.runtime_profile), "melee-runtime-profile")
            exe = project_path(profile.attrib["executable"])
            env["MELEE_AURORA_DLL"] = str(project_path(profile.attrib["renderer"]))
            if not exe.is_file() or not Path(env["MELEE_AURORA_DLL"]).is_file():
                raise ValueError("Build the selected runtime profile first")
        logs = ROOT / "build/playtest"; logs.mkdir(parents=True, exist_ok=True)
        log = logs / ("manual-" + time.strftime("%Y%m%d-%H%M%S") + ".log")
        with log.open("w") as stream:
            process = subprocess.Popen([str(exe), str(project_path(cfg["assets"]["directory"])/"sys/main.dol"), "0"], cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT)
        print("YAMPP PID", process.pid, "Log:", log)
    elif args.launch == "workshop":
        exe = ROOT / "build/workshop/MeleeWorkshop-win32-x64/Melee Workshop.exe"
        env = os.environ.copy(); env.pop("ELECTRON_RUN_AS_NODE", None)
        subprocess.Popen([str(exe)], cwd=ROOT, env=env)
if __name__ == "__main__":
    try: main()
    except (ValueError, OSError) as e:
        print(str(e), file=sys.stderr); sys.exit(1)
