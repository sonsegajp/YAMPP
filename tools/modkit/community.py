"""Workshop's private community client and native Mod Browser command helper.

No publisher credential crosses the desktop IPC boundary. Downloads are bound
to the SHA-256 chosen from the catalog and pass the server's strict validator.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import http.client
import io
import ipaddress
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import socket
import struct
import zlib
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import xml.etree.ElementTree as ET
import zipfile

# Embedded Python may use an isolated ._pth; import our adjacent modules explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parent))

from catalog import ROOT, MODS, CACHE, validate_id, costume_registry, costumes
from xml_manifest import read_manifest, write_manifest, package_folders

DEFAULT_REPOSITORY = "https://mmodx.fun/melee/api/mods"
MAX_ZIP = 64 * 1024 * 1024
MAX_PREVIEW = 4 * 1024 * 1024
PREVIEW_ROOT = CACHE / "community/previews"
PUBLIC_FIELDS = {"schema", "kind", "id", "name", "version", "base", "additive", "costumes", "author", "description"}
JOBS = {}
JOB_LOCK = threading.RLock()
INSTALL_LOCK = threading.RLock()


def config():
    repository = DEFAULT_REPOSITORY
    token = os.environ.get("MELEE_MOD_UPLOAD_TOKEN", "")
    path = ROOT / "user/community.xml"
    if path.exists():
        raw = path.read_bytes()
        if len(raw) > 65536 or b"<!DOCTYPE" in raw.upper() or b"<!ENTITY" in raw.upper():
            raise ValueError("Invalid community configuration")
        tree = ET.fromstring(raw)
        repository = tree.findtext("repository", default=repository).strip()
        token = token or tree.findtext("publisher-token", default="").strip()
    # A process-owned override supports local integration tests and deployments;
    # the renderer cannot submit a URL or a publisher credential.
    repository = os.environ.get("MELEE_MOD_REPOSITORY_URL", repository).rstrip("/")
    if repository == "https://soulmon.fun/melee/api/mods":
        repository = DEFAULT_REPOSITORY
    parsed = urllib.parse.urlsplit(repository)
    local = parsed.hostname in ("127.0.0.1", "localhost", "::1")
    if parsed.scheme != "https" and not (local and parsed.scheme == "http"):
        raise ValueError("The mod repository must use HTTPS (HTTP is allowed only on localhost)")
    if not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError("Invalid mod repository address")
    return repository, token


def settings():
    repository, token = config()
    return {"repository": repository, "publisherConfigured": bool(token)}


def digest(value):
    if not isinstance(value, str) or not re.fullmatch(r"[a-f0-9]{64}", value):
        raise ValueError("Choose a valid package SHA-256")
    return value


def repository_address():
    """Private deployment routing; never included in catalog/settings output."""
    value = os.environ.get("MELEE_MOD_REPOSITORY_ADDRESS", "")
    if not value:
        path = ROOT / "user/community.xml"
        if path.exists():
            raw = path.read_bytes()
            if len(raw) > 65536 or b"<!DOCTYPE" in raw.upper() or b"<!ENTITY" in raw.upper():
                raise ValueError("Invalid community configuration")
            value = ET.fromstring(raw).findtext("repository-address", default="").strip()
    return str(ipaddress.ip_address(value)) if value else None


class RepositoryHTTPSConnection(http.client.HTTPSConnection):
    """Keep certificate checks when the known repository's DNS record is absent."""
    def connect(self):
        try:
            return super().connect()
        except socket.gaierror:
            # This is the already configured Melee relay, not a redirect or a
            # generic DNS override. TLS still authenticates the configured public hostname normally.
            if self.host not in ("soulmon.fun", "mmodx.fun") or self.port != 443 or self._tunnel_host:
                raise
            address = repository_address()
            if not address:
                raise
            connection = socket.create_connection((address, 443), self.timeout, self.source_address)
            try:
                self.sock = self._context.wrap_socket(connection, server_hostname=self.host)
            except Exception:
                connection.close()
                raise


class RepositoryHTTPSHandler(urllib.request.HTTPSHandler):
    def https_open(self, req):
        # Modern Python removed the constructor check_hostname keyword.
        # The SSLContext owns hostname/certificate validation on every version.
        return self.do_open(RepositoryHTTPSConnection, req, context=self._context)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError("The mod repository attempted to redirect the request")


def request(suffix="", *, body=None, token=None, accept="application/json"):
    repository, _ = config()
    headers = {"Accept": accept, "User-Agent": "MeleePC-Workshop/1"}
    if body is not None:
        headers["Content-Type"] = "application/zip"
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(repository + suffix, data=body, headers=headers)
    try:
        return urllib.request.build_opener(NoRedirect, RepositoryHTTPSHandler).open(req, timeout=45)
    except urllib.error.HTTPError as e:
        if e.code == 401:
            raise ValueError("Publisher authorization was rejected; update the private publisher token") from None
        if e.code == 404:
            raise ValueError("The selected mod or repository endpoint was not found") from None
        raise ValueError("Mod repository request failed (HTTP %d)" % e.code) from None
    except urllib.error.URLError as e:
        raise ValueError("Cannot reach the mod repository: " + str(e.reason)) from None


def json_request(suffix="", *, body=None, token=None):
    with request(suffix, body=body, token=token) as response:
        raw = response.read(2 * 1024 * 1024 + 1)
    if len(raw) > 2 * 1024 * 1024:
        raise ValueError("Repository metadata exceeds the supported size")
    return json.loads(raw)


def validator():
    if str(ROOT / "server") not in sys.path:
        sys.path.insert(0, str(ROOT / "server"))
    from mod_repository import inspect_archive, validate_package
    return inspect_archive, validate_package


def local_packages():
    result = []
    for folder in package_folders(MODS):
        try:
            m = read_manifest(folder)
            if m.get("kind") != "costume" or m.get("additive") is not True:
                continue
            result.append({k: m.get(k) for k in ("id", "name", "kind", "base", "version", "description", "author", "costumes")} | {"enabled": m.get("enabled", True)})
        except (ValueError, OSError, KeyError):
            continue
    return {"packages": result, **settings()}


def manifest_hash(manifest):
    public = {k:v for k,v in manifest.items() if k in PUBLIC_FIELDS}
    return hashlib.sha256(json.dumps(public,sort_keys=True,separators=(",",":"),ensure_ascii=False).encode("utf-8")).hexdigest()


def verified_install(folder):
    folder=Path(folder)
    try:
        stamp=json.loads((folder/"install.json").read_text())
        sha=digest(stamp["sha256"])
        manifest=read_manifest(folder)
        if stamp.get("manifestHash") != manifest_hash(manifest):return None
        expected={c["file"] for c in manifest["costumes"]}
        expected.update(c[key] for c in manifest["costumes"] for key in ("portrait", "stockIcon", "portraitTexture", "stockIconTexture") if c.get(key))
        if (folder/"preview.png").exists():expected.add("preview.png")
        if set(stamp.get("files",{})) != expected:return None
        for name,value in stamp["files"].items():
            if hashlib.sha256(safe_source(folder,name).read_bytes()).hexdigest()!=value:return None
        return {"sha256":sha,"enabled":manifest.get("enabled",True),"id":manifest["id"],"files":dict(stamp["files"])}
    except (ValueError,OSError,KeyError,TypeError):
        return None


def installed_packages():
    result={}
    for folder in package_folders(MODS):
        record=verified_install(folder)
        if record:
            result[record["sha256"]]={"installed":True,"enabled":record["enabled"],"installedId":record["id"]}
    return result


def catalog(query="", kind=""):
    try: doc = json_request()
    except Exception:
        # Installed mods remain manageable when the repository is offline.
        doc={"schema":1,"mods":[]}
        for folder in package_folders(MODS):
            record=verified_install(folder)
            if record:
                manifest=read_manifest(folder);manifest.update(sha256=record["sha256"],size=0);doc["mods"].append(manifest)
    if not isinstance(doc, dict) or doc.get("schema") != 1 or not isinstance(doc.get("mods"), list):
        raise ValueError("Unsupported mod repository catalog")
    installed = installed_packages()
    rows = []
    for raw in doc["mods"][:4096]:
        sha = digest(raw.get("sha256"))
        if raw.get("kind") != "costume" or raw.get("additive") is not True:
            continue
        if query and query.casefold() not in (str(raw.get("name", "")) + " " + str(raw.get("description", ""))).casefold():
            continue
        if kind and raw.get("kind") != kind:
            continue
        row = {key: raw.get(key) for key in ("id", "name", "version", "base", "kind", "description", "author", "sha256", "size", "costumes", "preview_path", "preview_sha256", "preview_size")}
        row.update(additive=True, costumeCount=len(raw.get("costumes", [])), installed=False, enabled=False)
        row.update(installed.get(sha, {}))
        rows.append(row)
    from content_mods import catalog_entry
    content=catalog_entry()
    if (not kind or kind=="content") and (not query or query.casefold() in content["name"].casefold()):rows.insert(0,content)
    return {"schema": 1, "mods": rows, **settings()}


def preview_png(raw):
    """Bounded stdlib validation; no Pillow or package extraction is required."""
    if not 33 <= len(raw) <= MAX_PREVIEW or raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Invalid preview PNG")
    offset = 8
    width = height = depth = color = interlace = 0
    compressed = bytearray()
    ended = False
    seen_data = False
    for index in range(4096):
        if offset + 12 > len(raw):
            raise ValueError("Truncated preview PNG")
        size = struct.unpack_from(">I", raw, offset)[0]
        kind = raw[offset + 4:offset + 8]
        end = offset + 12 + size
        if end > len(raw) or zlib.crc32(raw[offset + 4:end - 4]) & 0xffffffff != struct.unpack_from(">I", raw, end - 4)[0]:
            raise ValueError("Preview PNG chunk integrity failed")
        data = raw[offset + 8:end - 4]
        if index == 0:
            if kind != b"IHDR" or size != 13:
                raise ValueError("Invalid preview PNG header")
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", data)
            allowed = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}
            if not 1 <= width <= 4096 or not 1 <= height <= 4096 or depth not in allowed.get(color, ()) or compression or filtering or interlace not in (0, 1):
                raise ValueError("Unsupported preview PNG dimensions or format")
        elif kind == b"IHDR":
            raise ValueError("Duplicate preview PNG header")
        elif kind == b"IDAT":
            seen_data = True
            compressed.extend(data)
        elif kind == b"IEND":
            if size or end != len(raw) or not seen_data:
                raise ValueError("Invalid preview PNG end")
            ended = True
            break
        elif kind not in (b"PLTE",) and not kind[0] & 32:
            raise ValueError("Unsupported critical preview PNG chunk")
        offset = end
    if not ended:
        raise ValueError("Incomplete preview PNG")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    passes = [(0, 0, 1, 1)] if not interlace else [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
    expected = 0
    for x, y, dx, dy in passes:
        w = max(0, (width - x + dx - 1) // dx)
        h = max(0, (height - y + dy - 1) // dy)
        if w and h:
            expected += h * (1 + (w * channels * depth + 7) // 8)
    if expected > 64 * 1024 * 1024:
        raise ValueError("Decoded preview PNG exceeds the supported size")
    try:
        decoder = zlib.decompressobj()
        decoded = decoder.decompress(compressed, expected + 1)
        if len(decoded) != expected or not decoder.eof or decoder.unconsumed_tail or decoder.unused_data:
            raise ValueError("Invalid preview PNG image data")
    except zlib.error:
        raise ValueError("Invalid preview PNG compression") from None
    return width, height


def preview(sha):
    """Fetch only public preview artwork, never a package or installed state."""
    sha = digest(sha)
    from upstream_download import BUILDS
    if sha==BUILDS[0]["archive"]["sha256"]:return {"sha256":sha,"previewAvailable":False}
    metadata = json_request("/" + sha)
    if not isinstance(metadata, dict) or metadata.get("sha256") != sha or metadata.get("kind") != "costume" or metadata.get("additive") is not True:
        raise ValueError("Preview metadata does not match the selected package")
    result = {"sha256": sha, "previewAvailable": False}
    if not any(metadata.get(key) is not None for key in ("preview_path", "preview_sha256", "preview_size")):
        return result
    png_hash = digest(metadata.get("preview_sha256"))
    size = metadata.get("preview_size")
    if type(size) is not int or not 33 <= size <= MAX_PREVIEW:
        raise ValueError("Preview artwork exceeds the supported size")
    folder = PREVIEW_ROOT / sha
    path = folder / (png_hash + ".png")
    if path.is_file():
        if path.stat().st_size != size:
            raise ValueError("Cached preview integrity verification failed")
        raw = path.read_bytes()
    else:
        # Construct from our configured origin and selected immutable hash. Do
        # not follow metadata preview_path to a different origin or download ZIPs.
        with request("/" + sha + "/preview.png", accept="image/png") as response:
            length = response.headers.get("Content-Length")
            if length is not None and (not length.isdecimal() or int(length) != size):
                raise ValueError("Preview response size does not match metadata")
            raw = response.read(size + 1)
    if len(raw) != size or hashlib.sha256(raw).hexdigest() != png_hash:
        raise ValueError("Preview integrity verification failed")
    width, height = preview_png(raw)
    if not path.exists():
        folder.mkdir(parents=True, exist_ok=True)
        temporary = folder / (png_hash + "." + uuid.uuid4().hex + ".tmp")
        try:
            temporary.write_bytes(raw)
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)
    return dict(result, previewAvailable=True, previewPath=str(path.resolve()),
                previewSha256=png_hash, previewSize=size, width=width, height=height)


def active():
    rows=[]
    for folder in package_folders(MODS):
        manifest=read_manifest(folder)
        if manifest.get("kind")!="costume" or not manifest.get("enabled",True):continue
        record=verified_install(folder)
        if not record:
            raise ValueError("Enabled costume pack '"+str(manifest.get("name",folder.name))+"' must be published or downloaded and must match its original files before netplay")
        stamp=json.loads((folder/"install.json").read_text())
        rows.append({"sha256":record["sha256"],"id":manifest["id"],"name":manifest["name"],"base":manifest["base"],"version":manifest.get("version","1.0.0"),"size":stamp.get("size",0)})
    rows.sort(key=lambda m:m["sha256"])
    if len(rows)>8:raise ValueError("Enable at most eight costume packs for a netplay room")
    # Rebuild from verified manifests before native exact-set activation so a
    # stale or hand-reordered TSV cannot assign different room color ordinals.
    costume_registry()
    return {"schema":1,"mods":rows,"sha256s":[m["sha256"] for m in rows]}


def check_slot_limit(manifest):
    base=manifest["base"]
    from catalog import COSTUME_ORDER
    total=len(COSTUME_ORDER[base[6:]])+len(manifest["costumes"])
    for folder in package_folders(MODS):
        other=read_manifest(folder)
        if other.get("kind")=="costume" and other.get("enabled",True) and other.get("base")==base and other.get("id")!=manifest["id"]:
            total+=len(other.get("costumes",[]))
    if total>64:raise ValueError("This fighter supports at most 64 combined original and additional costumes")


def toggle(sha,enabled):
    sha=digest(sha)
    from upstream_download import BUILDS
    if sha==BUILDS[0]["archive"]["sha256"]:
        from content_mods import toggle as toggle_content
        return toggle_content(enabled)
    with INSTALL_LOCK:
        for folder in package_folders(MODS):
            record=verified_install(folder)
            if record and record["sha256"]==sha:
                manifest=read_manifest(folder)
                if enabled:check_slot_limit(manifest)
                manifest["enabled"]=bool(enabled);write_manifest(folder,manifest);costume_registry()
                return {"sha256":sha,"enabled":bool(enabled)}
    raise ValueError("The installed package does not match the selected original content hash")


def safe_source(folder, relative):
    if not isinstance(relative, str) or "\\" in relative or ":" in relative:
        raise ValueError("Unsafe package filename")
    logical = PurePosixPath(relative)
    if logical.is_absolute() or not logical.parts or any(p in ("", ".", "..") for p in logical.parts):
        raise ValueError("Unsafe package filename")
    path = folder.joinpath(*logical.parts)
    if path.is_symlink() or not path.resolve().is_relative_to(folder.resolve()) or not path.is_file():
        raise ValueError("A declared package asset is missing or points outside its package")
    return path


def prepare(data):
    ident = validate_id(data["id"])
    folder = MODS / ident
    source = read_manifest(folder)
    if source.get("kind") != "costume" or source.get("additive") is not True:
        raise ValueError("Publish an additive costume package; fighter replacements are not accepted")
    public = {key: value for key, value in source.items() if key in PUBLIC_FIELDS}
    for key in ("version", "description", "author"):
        if key in data:
            public[key] = str(data[key]).strip()
    public.setdefault("version", "1.0.0")
    public["schema"] = 1
    files = {}
    for costume in public.get("costumes", []):
        for key in ("file", "portrait", "stockIcon", "portraitTexture", "stockIconTexture"):
            if costume.get(key):
                name = costume[key]
                files[name] = safe_source(folder, name).read_bytes()
    if (folder / "preview.png").is_file():
        files["preview.png"] = safe_source(folder, "preview.png").read_bytes()
    work = CACHE / "community"
    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="manifest-", dir=work) as tmp:
        write_manifest(tmp, public)
        files["manifest.xml"] = (Path(tmp) / "manifest.xml").read_bytes()
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, raw in sorted(files.items()):
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, raw, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    raw = stream.getvalue()
    if len(raw) > MAX_ZIP:
        raise ValueError("Package exceeds the 64 MiB upload limit")
    _, validate = validator()
    metadata = validate(raw)
    sha = hashlib.sha256(raw).hexdigest()
    prepared = work / "prepared"
    prepared.mkdir(exist_ok=True)
    (prepared / (sha + ".zip")).write_bytes(raw)
    review = {"preparedId": sha, "sha256": sha, "size": len(raw), "manifest": public, "files": [{"path": k, "size": len(v), "sha256": hashlib.sha256(v).hexdigest()} for k, v in sorted(files.items())], "metadata": metadata}
    (prepared / (sha + ".json")).write_text(json.dumps(review, indent=2), encoding="utf-8")
    return review


def update(job, phase, done=0, total=0, **extra):
    if job is None:
        return
    with JOB_LOCK:
        job.update(phase=phase, completedBytes=done, totalBytes=total, **extra)


def stamp_install(folder,manifest,files,sha,size):
    record={"sha256":sha,"size":size,"repository":config()[0],"installedAt":datetime.now(timezone.utc).isoformat(),"manifestHash":manifest_hash(manifest),"files":{name:hashlib.sha256(payload).hexdigest() for name,payload in files.items() if name!="manifest.xml"}}
    (Path(folder)/"install.json").write_text(json.dumps(record,indent=2),encoding="utf-8")


def upload(prepared_id, job=None):
    sha = digest(prepared_id)
    _, token = config()
    if not token:
        raise ValueError("Publisher access is not configured. Set MELEE_MOD_UPLOAD_TOKEN in the Workshop service environment")
    path = CACHE / "community/prepared" / (sha + ".zip")
    if not path.is_file():
        raise ValueError("Prepare this package before publishing")
    raw = path.read_bytes()
    if len(raw) > MAX_ZIP or hashlib.sha256(raw).hexdigest() != sha:
        raise ValueError("The prepared package changed; prepare it again")
    _, validate = validator()
    validate(raw)
    update(job, "Uploading", 0, len(raw))
    response = json_request(body=raw, token=token)
    # Some repository versions wrap the published record in 'mod'.
    published = response.get("mod", response)
    if published.get("sha256") != sha:
        raise ValueError("The repository did not confirm the prepared package hash")
    # A publisher's unchanged local source is also an installed copy. Stamp
    # only after checking each payload still matches the reviewed archive.
    inspect,_=validator();manifest,files=inspect(raw);folder=MODS/validate_id(manifest["id"])
    with INSTALL_LOCK:
        try:
            unchanged=folder.is_dir() and all(safe_source(folder,name).read_bytes()==payload for name,payload in files.items() if name!="manifest.xml")
        except (OSError,ValueError):unchanged=False
        if unchanged:
            manifest["enabled"]=read_manifest(folder).get("enabled",True)
            write_manifest(folder,manifest);stamp_install(folder,manifest,files,sha,len(raw));costume_registry()
    update(job, "Published", len(raw), len(raw))
    return {"published": True, "mod": published}


def download_verified(sha, job=None):
    sha = digest(sha)
    from upstream_download import BUILDS
    if sha==BUILDS[0]["archive"]["sha256"]:return {"sha256":sha,"previewAvailable":False}
    metadata = json_request("/" + sha)
    metadata = metadata.get("mod", metadata)
    if metadata.get("sha256") != sha:
        raise ValueError("Repository metadata did not match the selected package hash")
    expected = metadata.get("size")
    if not isinstance(expected, int) or not 0 < expected <= MAX_ZIP:
        raise ValueError("Repository package size is invalid")
    update(job, "Downloading", 0, expected)
    chunks = []
    received = 0
    with request("/" + sha + "/package.zip") as response:
        while True:
            chunk = response.read(128 * 1024)
            if not chunk:
                break
            received += len(chunk)
            if received > expected or received > MAX_ZIP:
                raise ValueError("Downloaded package exceeds its advertised size")
            chunks.append(chunk)
            update(job, "Downloading", received, expected)
    raw = b"".join(chunks)
    if received != expected or hashlib.sha256(raw).hexdigest() != sha:
        raise ValueError("Downloaded package failed SHA-256 verification")
    update(job, "Validating", received, expected)
    inspect, _ = validator()
    manifest, files = inspect(raw)
    ident = validate_id(manifest["id"])
    if manifest.get("id") != metadata.get("id") or manifest.get("base") != metadata.get("base"):
        raise ValueError("Package metadata differs from the verified manifest")
    return metadata, raw, manifest, files


def install(sha, job=None):
    sha=digest(sha)
    metadata,raw,manifest,files=download_verified(sha,job)
    expected=len(raw);received=expected;ident=validate_id(manifest["id"])
    destination = MODS / ident
    with INSTALL_LOCK:
        if destination.exists():
            stamp = destination / "install.json"
            if (record := verified_install(destination)) and record["sha256"] == sha:
                costume_registry()
                return {"installed": True, "alreadyInstalled": True, "id": ident, "sha256": sha}
            raise ValueError("A different local package already uses this ID; its files were preserved")
        check_slot_limit(manifest)
        update(job, "Installing", received, expected)
        MODS.mkdir(parents=True, exist_ok=True)
        staging_parent = CACHE / "community/staging"
        staging_parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix="install-", dir=staging_parent))
        try:
            for relative, payload in files.items():
                target = staging.joinpath(*PurePosixPath(relative).parts)
                if not target.resolve().is_relative_to(staging.resolve()):
                    raise ValueError("Unsafe archive path")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(payload)
            manifest["enabled"] = True
            write_manifest(staging, manifest)
            stamp_install(staging,manifest,files,sha,expected)
            staging.rename(destination)
            costume_registry()
        finally:
            # Only this freshly-created staging directory can be removed.
            if staging.exists() and staging.resolve().is_relative_to(staging_parent.resolve()):
                shutil.rmtree(staging)
    update(job, "Installed", received, expected)
    return {"installed": True, "id": ident, "sha256": sha, "costumeCount": len(manifest["costumes"]), "restartRequired": True}


INSPECTION_ROOT = ROOT / "build/modkit/inspection"
INSPECTION_LOCK = threading.RLock()


def inspect_package(sha, costume_index=0, job=None):
    """Read a hash-verified costume cache without installing or enabling it."""
    sha=digest(sha)
    if isinstance(costume_index,bool) or not isinstance(costume_index,int) or costume_index<0:
        raise ValueError("Choose a valid costume index")
    root=INSPECTION_ROOT
    destination=root/sha
    inspect_archive,_=validator()
    with INSPECTION_LOCK:
        if not destination.exists():
            _,raw,manifest,files=download_verified(sha,job)
            if costume_index>=len(manifest["costumes"]):raise ValueError("Choose a valid costume index")
            root.mkdir(parents=True,exist_ok=True)
            staging=Path(tempfile.mkdtemp(prefix="inspection-",dir=root))
            try:
                (staging/"package.zip").write_bytes(raw)
                for relative,payload in files.items():
                    target=staging.joinpath(*PurePosixPath(relative).parts)
                    if not target.resolve().is_relative_to(staging.resolve()):raise ValueError("Unsafe archive path")
                    target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(payload)
                try:staging.rename(destination)
                except FileExistsError:pass
            finally:
                if staging.exists() and staging.resolve().is_relative_to(root.resolve()):shutil.rmtree(staging)
        if destination.is_symlink() or not destination.resolve().is_relative_to(root.resolve()):
            raise ValueError("Unsafe inspection cache path")
        archive=safe_source(destination,"package.zip")
        if not 0<archive.stat().st_size<=MAX_ZIP:raise ValueError("Cached preview package has an invalid size")
        raw=archive.read_bytes()
        if hashlib.sha256(raw).hexdigest()!=sha:raise ValueError("Cached preview package failed SHA-256 verification")
        manifest,files=inspect_archive(raw)
        if costume_index>=len(manifest["costumes"]):raise ValueError("Choose a valid costume index")
        for relative,payload in files.items():
            cached=safe_source(destination,relative)
            if cached.read_bytes()!=payload:raise ValueError("Cached preview files failed integrity verification")
        costume=manifest["costumes"][costume_index]
        dat=safe_source(destination,costume["file"])
        preview=costume.get("portrait") or ("preview.png" if "preview.png" in files else None)
        from catalog import NAMES,COSTUME_ORDER,costume_reference
        _,prefix,internal=next(row for row in NAMES if "stock-"+row[1]==manifest["base"])
        reference=costume_reference(prefix,dat)
        result={key:manifest[key] for key in ("id","name","version","base")}
        result.update(sha256=sha,costumeCount=len(manifest["costumes"]),costumeIndex=costume_index,
            costumes=[{"id":c["id"],"name":c["name"]} for c in manifest["costumes"]],costume=dict(costume),
            datPath=str(dat.resolve()),datSha256=hashlib.sha256(files[costume["file"]]).hexdigest(),
            previewPath=str(safe_source(destination,preview).resolve()) if preview else None,
            previewSha256=hashlib.sha256(files[preview]).hexdigest() if preview else None,
            stockIconPath=str(safe_source(destination,costume["stockIcon"]).resolve()) if costume.get("stockIcon") else None,
            stockIconSha256=hashlib.sha256(files[costume["stockIcon"]]).hexdigest() if costume.get("stockIcon") else None,
            portraitTexturePath=str(safe_source(destination,costume["portraitTexture"]).resolve()) if costume.get("portraitTexture") else None,
            portraitTextureSha256=hashlib.sha256(files[costume["portraitTexture"]]).hexdigest() if costume.get("portraitTexture") else None,
            stockIconTexturePath=str(safe_source(destination,costume["stockIconTexture"]).resolve()) if costume.get("stockIconTexture") else None,
            stockIconTextureSha256=hashlib.sha256(files[costume["stockIconTexture"]]).hexdigest() if costume.get("stockIconTexture") else None,
            cachePath=str(destination.resolve()),internalKind=internal,baseColor=COSTUME_ORDER[prefix].index(reference))
        return result


def start_job(operation, value):
    if operation not in ("upload", "install"):
        raise ValueError("Unknown transfer operation")
    ident = uuid.uuid4().hex
    record = {"id": ident, "operation": operation, "status": "queued", "phase": "Queued", "completedBytes": 0, "totalBytes": 0}
    with JOB_LOCK:
        if sum(j["status"] in ("queued","running") for j in JOBS.values())>=4:
            raise ValueError("Wait for the current transfers to finish")
        # Bound retained progress records without dropping active jobs.
        for old in list(JOBS):
            if len(JOBS) < 64:
                break
            if JOBS[old]["status"] in ("complete", "failed"):
                JOBS.pop(old)
        JOBS[ident] = record
    def run():
        update(record, "Starting", status="running")
        try:
            result = upload(value, record) if operation == "upload" else install(value, record)
            update(record, record["phase"], record["completedBytes"], record["totalBytes"], status="complete", result=result)
        except Exception as e:
            update(record, "Failed", status="failed", error=str(e))
    threading.Thread(target=run, name="mod-" + operation, daemon=True).start()
    with JOB_LOCK:
        return dict(record)


def job(ident):
    with JOB_LOCK:
        if ident not in JOBS:
            raise ValueError("Unknown transfer")
        return dict(JOBS[ident])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    listing = sub.add_parser("list")
    listing.add_argument("--output", type=Path)
    active_parser = sub.add_parser("active")
    active_parser.add_argument("--output", type=Path)
    toggling = sub.add_parser("toggle")
    toggling.add_argument("--sha256", required=True)
    toggling.add_argument("--enabled", choices=("0","1"), required=True)
    toggling.add_argument("--output", type=Path)
    installation = sub.add_parser("install")
    installation.add_argument("--sha256", required=True)
    installation.add_argument("--output", type=Path)
    room_preparation = sub.add_parser("room-prepare")
    room_preparation.add_argument("--hashes", required=True)
    room_preparation.add_argument("--output", type=Path)
    inspection = sub.add_parser("inspect")
    inspection.add_argument("--sha256", required=True)
    inspection.add_argument("--costume", type=int, default=0)
    inspection.add_argument("--output", type=Path)
    previewing = sub.add_parser("preview")
    previewing.add_argument("--sha256", required=True)
    previewing.add_argument("--output", type=Path)
    preparation = sub.add_parser("prepare")
    preparation.add_argument("--id", required=True)
    preparation.add_argument("--output", type=Path)
    publishing = sub.add_parser("upload")
    publishing.add_argument("--sha256", required=True)
    publishing.add_argument("--output", type=Path)
    upstream = sub.add_parser("upstream-download")
    upstream.add_argument("--sha256", required=True)
    upstream.add_argument("--confirm", action="store_true")
    upstream.add_argument("--room-join", action="store_true", help="Allow an archive download after room-join consent")
    upstream.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "upstream-download":
            from upstream_download import acquire, BUILDS, identity
            match = next((b for b in BUILDS if b["archive"]["sha256"] == args.sha256), None)
            if match is None: raise ValueError("Unknown upstream release; update YAMPP")
            from content_mods import prepare as prepare_content
            from job_progress import Progress, sidecar
            # The runtime watches this file so the player can see a long
            # import advancing instead of guessing whether it has hung.
            result = prepare_content(confirmed=args.confirm,allow_download=args.room_join,
                                     progress=Progress(sidecar(args.output)))
        elif args.command == "list":
            result = catalog()
        elif args.command == "active":
            result = active()
        elif args.command == "toggle":
            result = toggle(args.sha256,args.enabled=="1")
        elif args.command == "install":
            result = install(args.sha256)
        elif args.command == "room-prepare":
            from room_cache import prepare_room
            result = prepare_room(args.hashes)
        elif args.command == "inspect":
            result = inspect_package(args.sha256,args.costume)
        elif args.command == "preview":
            result = preview(args.sha256)
        elif args.command == "prepare":
            result = prepare({"id": args.id})
        else:
            result = upload(args.sha256)
        code = 0
    except Exception as e:
        result = {"error": str(e)}
        if getattr(e,"ui_code",None):result["uiCode"]=e.ui_code
        code = 1
    encoded = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        tmp = args.output.with_suffix(args.output.suffix + ".tmp")
        tmp.write_text(encoded + "\n", encoding="utf-8")
        tmp.replace(args.output)
    else:
        print(encoded)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
