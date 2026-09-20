"""Immutable, validated visual costume packages. No asset extraction or execution."""
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import threading
import xml.etree.ElementTree as ET
import zipfile
import zlib

MAX_PACKAGE_BYTES = 64 * 1024 * 1024
MAX_EXPANDED_BYTES = 128 * 1024 * 1024
MAX_MEMBERS = 64
MAX_REQUIRED_MODS = 8
HASH_RE = re.compile(r"^[0-9a-f]{64}$")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?$")
STOCK_SYMBOLS = dict(
    Ca="Captain", Cl="Clink", Dk="Donkey", Dr="Drmario", Fc="Falco",
    Fe="Emblem", Fx="Fox", Gn="Ganon", Gw="Gamewatch", Kb="Kirby",
    Kp="Koopa", Lg="Luigi", Lk="Link", Mr="Mario", Ms="Mars",
    Mt="Mewtwo", Ns="Ness", Pc="Pichu", Pe="Peach", Pk="Pikachu",
    Pp="Popo", Pr="Purin", Sk="Seak", Ss="Samus", Ys="Yoshi", Zd="Zelda",
)


STOCK_COLORS = dict(
    Ca="Bu Gr Gy Re Wh", Cl="Bk Bu Re Wh", Dk="Bk Bu Gr Re", Dr="Bk Bu Gr Re",
    Fc="Bu Gr Re", Fe="Bu Gr Re Ye", Fx="Gr La Or", Gn="Bu Gr La Re", Gw="",
    Kb="Bu Gr Re Wh Ye", Kp="Bk Bu Re", Lg="Aq Pi Wh", Lk="Bk Bu Re Wh",
    Mr="Bk Bu Gr Ye", Ms="Bk Gr Re Wh", Mt="Bu Gr Re", Ns="Bu Gr Ye",
    Pc="Bu Gr Re", Pe="Bu Gr Wh Ye", Pk="Bu Gr Re", Pp="Gr Or Re",
    Pr="Bu Gr Re Ye", Sk="Bu Gr Re Wh", Ss="Bk Gr La Pi", Ys="Aq Bu Pi Re Ye",
    Zd="Bu Gr Re Wh",
)


class PackageError(ValueError):
    pass


def safe_path(value):
    if (not isinstance(value, str) or not value or len(value) > 180
            or "\\" in value or ":" in value or any(ord(c) < 32 for c in value)):
        raise PackageError("Invalid package member path")
    parts = value.split("/")
    if any(p in ("", ".", "..") or p[-1:] in (".", " ") for p in parts):
        raise PackageError("Package paths must be relative and cannot traverse directories")
    # These names alias devices even when they carry a file extension on Windows.
    reserved = {"con", "prn", "aux", "nul", *('com%d' % n for n in range(1, 10)),
                *('lpt%d' % n for n in range(1, 10))}
    if any(p.split(".", 1)[0].lower() in reserved for p in parts):
        raise PackageError("Reserved package member name")
    return str(PurePosixPath(value))


def decode_manifest(raw):
    if len(raw) > 1024 * 1024 or b"<!DOCTYPE" in raw.upper() or b"<!ENTITY" in raw.upper():
        raise PackageError("Unsupported manifest XML")
    try:
        root = ET.fromstring(raw)
    except ET.ParseError as exc:
        raise PackageError("Invalid manifest XML") from exc
    if root.tag != "melee-package" or root.get("schema") != "1":
        raise PackageError("Unsupported package schema")

    def decode(node, depth=0):
        if depth > 8:
            raise PackageError("Manifest nesting is too deep")
        kind, text = node.get("type"), node.text or ""
        if kind == "object":
            result = {}
            for child in node:
                name = child.get("name")
                if child.tag != "field" or not name or name in result:
                    raise PackageError("Invalid or duplicate manifest field")
                result[name] = decode(child, depth + 1)
            return result
        if kind == "array":
            if any(child.tag != "item" for child in node):
                raise PackageError("Invalid manifest array")
            return [decode(child, depth + 1) for child in node]
        if len(node):
            raise PackageError("Invalid manifest value")
        if kind == "string":
            return text
        if kind == "bool" and text in ("true", "false"):
            return text == "true"
        if kind == "int" and re.fullmatch(r"-?[0-9]{1,10}", text):
            return int(text)
        raise PackageError("Unsupported manifest value type")

    result = decode(root)
    if not isinstance(result, dict):
        raise PackageError("The package manifest must be an object")
    return result


def text_field(value, label, limit, pattern=None):
    if (not isinstance(value, str) or not value or len(value) > limit
            or any(ord(c) < 32 for c in value) or (pattern and not pattern.fullmatch(value))):
        raise PackageError("Invalid " + label)
    return value


def validate_costume_dat(raw, base):
    if len(raw) < 32 or len(raw) > 32 * 1024 * 1024:
        raise PackageError("Costume DAT size is invalid")
    total, data_size, relocations, roots, refs = struct.unpack_from(">5I", raw)
    table = 32 + data_size + relocations * 4
    strings = table + (roots + refs) * 8
    if (total != len(raw) or data_size < 4 or not 1 <= roots <= 3 or refs
            or strings >= len(raw) or table < 32):
        raise PackageError("Invalid costume DAT archive")
    prefix = base[6:]
    symbol = "Ply" + STOCK_SYMBOLS[prefix] + "5K"
    names = set()
    for i in range(roots):
        offset, name_offset = struct.unpack_from(">II", raw, table + i * 8)
        begin = strings + name_offset
        end = raw.find(b"\0", begin, min(begin + 128, len(raw)))
        if offset >= data_size or begin >= len(raw) or end < 0:
            raise PackageError("Invalid costume DAT public root")
        try:
            name = raw[begin:end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise PackageError("Invalid costume DAT symbol") from exc
        if name in names:
            raise PackageError("Duplicate costume DAT public root")
        names.add(name)
    model_roots = [name for name in names if name.endswith("_Share_joint")]
    if len(model_roots) != 1 or not model_roots[0].startswith(symbol):
        raise PackageError("Costume DAT has no stock-compatible model root")
    color = model_roots[0][len(symbol):-len("_Share_joint")]
    if color not in [""] + STOCK_COLORS[prefix].split():
        raise PackageError("DAT must keep an original stock costume color symbol")
    stem = symbol + color + "_Share"
    allowed = {stem + "_joint", stem + "_matanim_joint"}
    if prefix == "Pr" and color:
        allowed.add("PlyPurin" + color + "Hat_TopN_joint")
    if not names <= allowed:
        raise PackageError("DAT must contain only the selected stock fighter's costume roots")
    for i in range(relocations):
        offset = struct.unpack_from(">I", raw, 32 + data_size + i * 4)[0]
        if offset > data_size - 4:
            raise PackageError("Invalid costume DAT relocation")
        if struct.unpack_from(">I", raw, 32 + offset)[0] >= data_size:
            raise PackageError("Costume DAT relocation points outside the model")


def validate_png(raw):
    if (len(raw) < 33 or len(raw) > 4 * 1024 * 1024
            or raw[:8] != b"\x89PNG\r\n\x1a\n" or raw[12:16] != b"IHDR"):
        raise PackageError("Invalid PNG artwork")
    width, height = struct.unpack_from(">II", raw, 16)
    if not (1 <= width <= 4096 and 1 <= height <= 4096):
        raise PackageError("PNG artwork exceeds the supported dimensions")


def validate_native_texture(raw, field):
    expected = (136, 188, 13056) if field == "portraitTexture" else (24, 24, 288)
    if len(raw) < 24 or raw[:8] != b"YAMPPTEX":
        raise PackageError("Invalid native costume texture header")
    width, height, format_id, size = struct.unpack(">4I", raw[8:24])
    if (width, height, size) != expected or format_id != 14 or len(raw) != 24 + size:
        raise PackageError("Native costume texture dimensions, format or size do not match")


def inspect_archive(raw):
    """Return (validated manifest, member bytes) for safe client installation."""
    if not isinstance(raw, bytes) or not raw or len(raw) > MAX_PACKAGE_BYTES:
        raise PackageError("Package exceeds the 64 MiB upload limit")
    try:
        archive = zipfile.ZipFile(io.BytesIO(raw))
    except zipfile.BadZipFile as exc:
        raise PackageError("Upload must be a ZIP package") from exc
    with archive:
        members = archive.infolist()
        if not members or len(members) > MAX_MEMBERS:
            raise PackageError("Package has too many files")
        if sum(m.file_size for m in members) > MAX_EXPANDED_BYTES:
            raise PackageError("Package exceeds the expanded size limit")
        files, folded = {}, set()
        for member in members:
            path = safe_path(member.filename)
            mode = member.external_attr >> 16
            if (member.is_dir() or stat.S_ISLNK(mode)
                    or (stat.S_IFMT(mode) not in (0, stat.S_IFREG))
                    or member.flag_bits & 1):
                raise PackageError("Only regular, unencrypted files are accepted")
            if path.casefold() in folded:
                raise PackageError("Duplicate package filename")
            folded.add(path.casefold())
            maximum = 32 * 1024 * 1024 if path.endswith(".dat") else 4 * 1024 * 1024
            if member.file_size > maximum:
                raise PackageError("Package member exceeds its size limit")
            try:
                files[path] = archive.read(member)
            except (zipfile.BadZipFile, RuntimeError, NotImplementedError, zlib.error, EOFError) as exc:
                raise PackageError("Invalid or unsupported ZIP member") from exc
        if "manifest.xml" not in files:
            raise PackageError("Package needs a root manifest.xml")
    manifest = decode_manifest(files["manifest.xml"])
    allowed_fields = {"schema", "id", "name", "version", "kind", "base", "additive",
                      "costumes", "author", "description", "enabled"}
    if set(manifest) - allowed_fields:
        raise PackageError("Visual costume packages cannot contain gameplay manifest fields")
    if manifest.get("kind") != "costume" or manifest.get("additive") is not True:
        raise PackageError("Only additive visual costume packs can be published")
    if "schema" in manifest and (type(manifest["schema"]) is not int or manifest["schema"] != 1):
        raise PackageError("Unsupported manifest schema")
    if "enabled" in manifest and type(manifest["enabled"]) is not bool:
        raise PackageError("Invalid enabled value")
    text_field(manifest.get("id"), "package ID", 64, ID_RE)
    text_field(manifest.get("name"), "package name", 64)
    text_field(manifest.get("version"), "package version", 48, VERSION_RE)
    base = manifest.get("base")
    if not isinstance(base, str) or not base.startswith("stock-") or base[6:] not in STOCK_SYMBOLS:
        raise PackageError("Costume base must be an original stock fighter")
    for key, limit in (("author", 64), ("description", 1000)):
        if key in manifest:
            text_field(manifest[key], key, limit)
    costumes = manifest.get("costumes")
    if not isinstance(costumes, list) or not 1 <= len(costumes) <= 16:
        raise PackageError("A pack must contain between 1 and 16 costumes")
    expected, ids = {"manifest.xml"}, set()
    for costume in costumes:
        if not isinstance(costume, dict) or set(costume) - {"id", "name", "file", "portrait", "stockIcon", "portraitTexture", "stockIconTexture"}:
            raise PackageError("Invalid costume entry")
        ident = text_field(costume.get("id"), "costume ID", 64, ID_RE)
        if ident in ids:
            raise PackageError("Duplicate costume ID")
        ids.add(ident)
        text_field(costume.get("name"), "costume name", 64)
        path = safe_path(costume.get("file"))
        if not path.endswith(".dat") or path not in files or path in expected:
            raise PackageError("Each costume must declare its own DAT file")
        validate_costume_dat(files[path], base)
        expected.add(path)
        for artwork in ("portrait", "stockIcon"):
            if artwork not in costume:
                continue
            image = safe_path(costume[artwork])
            if not image.endswith(".png") or image not in files:
                raise PackageError("Costume %s must name a packaged PNG" % artwork)
            validate_png(files[image])
            expected.add(image)
        for texture in ("portraitTexture", "stockIconTexture"):
            if texture not in costume:
                continue
            image = safe_path(costume[texture])
            if not image.endswith(".gxt") or image not in files:
                raise PackageError("Costume %s must name a packaged GXT" % texture)
            validate_native_texture(files[image], texture)
            expected.add(image)
    if "preview.png" in files:
        validate_png(files["preview.png"])
        expected.add("preview.png")
    if set(files) != expected:
        raise PackageError("Package contains undeclared files or unsupported assets")
    return manifest, files


def preview_member(manifest, files):
    if "preview.png" in files:
        return "preview.png"
    return next((costume["portrait"] for costume in manifest["costumes"] if costume.get("portrait") in files), None)


VALIDATION_POLICY = 2


def validate_package(raw):
    manifest, files = inspect_archive(raw)
    metadata = {key: value for key, value in manifest.items() if key not in ("schema", "enabled")}
    digest = hashlib.sha256(raw).hexdigest()
    metadata.update(sha256=digest, size=len(raw),
                    download_path="/melee/api/mods/%s/package.zip" % digest,
                    _policy=VALIDATION_POLICY)
    preview = preview_member(manifest, files)
    if preview:
        metadata.update(preview_path="/melee/api/mods/%s/preview.png" % digest,
                        preview_sha256=hashlib.sha256(files[preview]).hexdigest(),
                        preview_size=len(files[preview]))
    return metadata


def clean_hashes(value):
    if not isinstance(value, list) or len(value) > MAX_REQUIRED_MODS:
        raise PackageError("A room supports at most eight published costume packs")
    if any(not isinstance(h, str) or not HASH_RE.fullmatch(h) for h in value):
        raise PackageError("Invalid costume package hash")
    if len(set(value)) != len(value):
        raise PackageError("Duplicate costume package hash")
    return sorted(value)


SIDECAR_REQUIRED = {"sha256": str, "name": str, "id": str, "kind": str,
                    "size": int, "version": str, "base": str,
                    "download_path": str, "additive": bool}


def _valid_sidecar(metadata, digest):
    if not isinstance(metadata, dict):
        return False
    if metadata.get("sha256") != digest:
        return False
    for key, expected_type in SIDECAR_REQUIRED.items():
        if not isinstance(metadata.get(key), expected_type):
            return False
    if metadata.get("kind") != "costume" or metadata.get("additive") is not True:
        return False
    if not isinstance(metadata.get("costumes"), list):
        return False
    return True


class Repository:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self._backfill_lock = threading.Lock()
        self.entries = {}
        self._verified = {}
        for item in self.root.glob("*.json"):
            digest = item.stem
            if not HASH_RE.fullmatch(digest):
                continue
            try:
                archive = self.root / (digest + ".zip")
                if not archive.exists():
                    continue
                raw = archive.read_bytes()
                if hashlib.sha256(raw).hexdigest() != digest:
                    continue
                metadata = json.loads(item.read_text(encoding="utf-8"))
                if not _valid_sidecar(metadata, digest):
                    continue
                if metadata.get("_policy") != VALIDATION_POLICY:
                    metadata = validate_package(raw)
                    if metadata["sha256"] != digest:
                        continue
                    temporary = self.root / (digest + ".json.tmp")
                    temporary.write_text(json.dumps(metadata, indent=2),
                                         encoding="utf-8")
                    os.replace(temporary, item)
                self.entries[digest] = metadata
                st = archive.stat()
                self._verified[digest] = (st.st_size, st.st_mtime_ns,
                                          getattr(st, 'st_ino', 0))
            except (OSError, ValueError, json.JSONDecodeError, TypeError,
                    AttributeError, PackageError):
                continue

    def catalog(self):
        with self.lock:
            return {"schema": 1, "mods": sorted(self.entries.values(),
                    key=lambda m: (m["name"].lower(), m["version"], m["sha256"]))}

    def get(self, digest):
        with self.lock:
            return self.entries.get(digest)

    def require(self, hashes):
        hashes = clean_hashes(hashes)
        with self.lock:
            if any(h not in self.entries for h in hashes):
                raise PackageError("A required costume pack is not published on this server")
            return [self.entries[h] for h in hashes]

    def package_path(self, digest):
        if not self.get(digest):
            return None
        return self.root / (digest + ".zip")

    def verified_package_stream(self, digest):
        metadata = self.get(digest)
        if not metadata:
            return None, 0
        path = self.root / (digest + ".zip")
        try:
            stream = open(path, "rb")
        except FileNotFoundError:
            with self.lock:
                self.entries.pop(digest, None)
                self._verified.pop(digest, None)
            return None, 0
        try:
            st = os.fstat(stream.fileno())
            current = (st.st_size, st.st_mtime_ns, getattr(st, 'st_ino', 0))
            verified = self._verified.get(digest)
            if current != verified:
                stream.seek(0)
                h = hashlib.sha256()
                while True:
                    chunk = stream.read(256 * 1024)
                    if not chunk:
                        break
                    h.update(chunk)
                if h.hexdigest() != digest:
                    stream.close()
                    with self.lock:
                        self.entries.pop(digest, None)
                        self._verified.pop(digest, None)
                    return None, 0
                self._verified[digest] = current
                stream.seek(0)
        except Exception:
            stream.close()
            raise
        return stream, st.st_size

    def _backfill_preview(self, digest):
        with self._backfill_lock:
            cached = self.root / (digest + ".preview.png")
            if cached.exists():
                return cached.read_bytes()
            package = self.root / (digest + ".zip")
            if not package.exists():
                return None
            raw = package.read_bytes()
            if hashlib.sha256(raw).hexdigest() != digest:
                with self.lock:
                    self.entries.pop(digest, None)
                raise PackageError("Stored costume package failed integrity verification")
            manifest, files = inspect_archive(raw)
            member = preview_member(manifest, files)
            if not member:
                return None
            data = files[member]
            temporary = self.root / (digest + ".preview.tmp")
            temporary.write_bytes(data)
            os.replace(temporary, cached)
            return data

    def preview_bytes(self, digest):
        metadata = self.get(digest)
        if not metadata:
            return None
        cached = self.root / (digest + ".preview.png")
        if cached.exists():
            data = cached.read_bytes()
            expected_size = metadata.get("preview_size")
            expected_hash = metadata.get("preview_sha256")
            if ((expected_size is not None and len(data) != expected_size)
                    or (expected_hash is not None
                        and hashlib.sha256(data).hexdigest() != expected_hash)):
                cached.unlink(missing_ok=True)
                return self._backfill_preview(digest)
            return data
        if not metadata.get("preview_path"):
            return None
        return self._backfill_preview(digest)

    def publish(self, raw):
        metadata = validate_package(raw)
        digest = metadata["sha256"]
        with self.lock:
            if digest in self.entries:
                return self.entries[digest], False
            archive = self.root / (digest + ".zip")
            sidecar = self.root / (digest + ".json")
            preview_file = self.root / (digest + ".preview.png")
            temporary = self.root / (digest + ".tmp")
            temporary.write_bytes(raw)
            os.replace(temporary, archive)
            manifest, files = inspect_archive(raw)
            member = preview_member(manifest, files)
            if member:
                temporary.write_bytes(files[member])
                os.replace(temporary, preview_file)
            temporary.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
            os.replace(temporary, sidecar)
            self.entries[digest] = metadata
            st = archive.stat()
            self._verified[digest] = (st.st_size, st.st_mtime_ns,
                                      getattr(st, 'st_ino', 0))
            return metadata, True
