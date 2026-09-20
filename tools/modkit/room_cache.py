"""Consent-only room versions, isolated from the user's installed packages."""
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import tempfile
import time

import catalog

MAX_ENTRIES = 16
MAX_BYTES = 2 * 1024 * 1024 * 1024
ROOM_ROOT = catalog.CACHE / "community/rooms" / hashlib.sha256(
    str(catalog.MODS.resolve()).casefold().encode("utf-8")).hexdigest()[:16]


def _ordinary(path):
    info = path.lstat()
    if path.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
        raise ValueError("Room cache contains an unsafe linked path")
    return info


def _root():
    root = ROOM_ROOT.absolute()
    boundary = (catalog.ROOT / "build/modkit").resolve()
    if not root.resolve().is_relative_to(boundary) or root.resolve() == boundary:
        raise ValueError("Invalid room cache directory")
    for part in (root, *root.parents):
        if part.exists():
            _ordinary(part)
        if part == boundary:
            break
    root.mkdir(parents=True, exist_ok=True)
    return root.resolve()


def _size(path):
    info = _ordinary(path)
    if not path.is_dir():
        return info.st_size
    return sum(_size(Path(entry.path)) for entry in os.scandir(path))


def _remove(root, path):
    # Only direct children created by this cache may be pruned. Never follow
    # junctions/symlinks, including a linked file buried inside an old entry.
    if path.parent.resolve() != root or path.resolve().parent != root:
        raise ValueError("Unsafe room cache removal")
    _size(path)
    shutil.rmtree(path)


@contextmanager
def _locked(root):
    path = root / ".lock"
    if path.exists():
        _ordinary(path)
    with path.open("a+b") as stream:
        if not stream.tell():
            stream.write(b"0")
            stream.flush()
        deadline = time.monotonic() + 15
        while True:
            stream.seek(0)
            try:
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise ValueError("Another room costume request is still running") from None
                time.sleep(.05)
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == "nt":
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream, fcntl.LOCK_UN)


def _reserve(root, requested, extra_bytes=0, extra_entries=0):
    entries = []
    for path in root.iterdir():
        if path.name == ".lock":
            continue
        if path.name.startswith("staging-"):
            _remove(root, path)
            continue
        if not re.fullmatch("[a-f0-9]{64}", path.name) or not path.is_dir():
            raise ValueError("Unexpected room cache entry")
        entries.append((path, _size(path), path.stat().st_mtime_ns))
    total = sum(item[1] for item in entries) + extra_bytes
    count = len(entries) + extra_entries
    for path, size, _ in sorted(entries, key=lambda item: item[2]):
        if total <= MAX_BYTES and count <= MAX_ENTRIES:
            break
        if path.name in requested:
            continue
        _remove(root, path)
        total -= size
        count -= 1
    if total > MAX_BYTES or count > MAX_ENTRIES:
        raise ValueError("The selected room costumes exceed the room cache limit")


def _cached(root, sha, requested):
    import community
    destination = root / sha
    inspect, _ = community.validator()
    if not destination.exists():
        _, raw, _, files = community.download_verified(sha)
        _reserve(root, requested, len(raw) + sum(len(data) for data in files.values()), 1)
        staging = Path(tempfile.mkdtemp(prefix="staging-", dir=root))
        try:
            (staging / "package.zip").write_bytes(raw)
            for name, data in files.items():
                target = staging.joinpath(*PurePosixPath(name).parts)
                if not target.resolve().is_relative_to(staging.resolve()):
                    raise ValueError("Unsafe room package path")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            staging.rename(destination)
        finally:
            if staging.exists():
                _remove(root, staging)
    _ordinary(destination)
    if destination.resolve().parent != root:
        raise ValueError("Unsafe room cache package")
    archive = community.safe_source(destination, "package.zip")
    if not 0 < archive.stat().st_size <= community.MAX_ZIP:
        raise ValueError("Room cache package integrity verification failed")
    raw = archive.read_bytes()
    if hashlib.sha256(raw).hexdigest() != sha:
        raise ValueError("Room cache package integrity verification failed")
    manifest, files = inspect(raw)
    # Do not trust a stamp: reuse checks the original ZIP and every extracted
    # byte, including manifest order and any PNG/GX texture payload.
    _size(destination)
    actual = {p.relative_to(destination).as_posix() for p in destination.rglob("*") if p.is_file()}
    if actual != set(files) | {"package.zip"}:
        raise ValueError("Room cache package integrity verification failed")
    for name, data in files.items():
        path = community.safe_source(destination, name)
        if path.stat().st_size != len(data) or path.read_bytes() != data:
            raise ValueError("Room cache package integrity verification failed")
    os.utime(destination, None)
    pack = dict(manifest, folder=str(destination), enabled=False)
    record = {"sha256": sha, "id": manifest["id"], "enabled": False,
              "files": {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}
    return pack, record


def prepare_room(hashes):
    """Prepare one exact, consented room set without installing/enabling it."""
    import community
    if isinstance(hashes, str):
        hashes = hashes.split(",") if hashes else []
    if not isinstance(hashes, list) or len(hashes) > 8:
        raise ValueError("A room supports at most eight costume packages")
    hashes = [community.digest(value) for value in hashes]
    if hashes != sorted(set(hashes)):
        raise ValueError("Room package hashes must be unique and sorted")
    root = _root()
    with _locked(root):
        _reserve(root, set(hashes))
        local = {}
        for folder in community.package_folders(catalog.MODS):
            record = community.verified_install(folder)
            if record:
                local[record["sha256"]] = (dict(community.read_manifest(folder), folder=str(folder)), record)
        extra = []
        identities = set()
        totals = {"stock-" + prefix: len(order) for prefix, order in catalog.COSTUME_ORDER.items()}
        for sha in hashes:
            if sha in local:
                pack, record = local[sha]
            else:
                pack, record = _cached(root, sha, set(hashes))
                extra.append((pack, record))
            if pack["id"] in identities:
                raise ValueError("A room cannot use two versions of the same costume pack")
            identities.add(pack["id"])
            totals[pack["base"]] += len(pack["costumes"])
        if any(count > 64 for count in totals.values()):
            raise ValueError("A room fighter exceeds 64 combined costume slots")
        # The sole visible commit is an atomic TSV replacement after the whole
        # set verifies. Cached rows are always disabled for offline selection.
        catalog.costume_registry(extra)
    return {"prepared": True, "roomOnly": True, "sha256s": hashes, "mods": [{"sha256": sha} for sha in hashes], "cached": len(extra)}
