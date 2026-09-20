"""Consent-only, pinned downloads from official GitHub releases.

No room-provided URL, shell command, executable, or repository is accepted.
Acquisition is separate from activating a guest runtime; a downloaded project
is never claimed playable merely because its checksums match.
"""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
from urllib.parse import urlsplit
from urllib.request import Request, HTTPRedirectHandler, ProxyHandler, build_opener

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "server"))
from upstream_builds import BUILDS, identity, release, download_offer

CACHE = ROOT / "user/upstream-builds"
ALLOWED_HOSTS = {"github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com", "raw.githubusercontent.com"}


def _url(value):
    parsed = urlsplit(value)
    if (parsed.scheme != "https" or parsed.hostname not in ALLOWED_HOSTS
            or parsed.username or parsed.password or parsed.port not in (None, 443)):
        raise ValueError("Akaneia downloads must remain on official GitHub HTTPS hosts")
    return value


class GitHubRedirects(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        return super().redirect_request(request, fp, code, msg, headers, _url(newurl))


def _ordinary(path):
    info = path.lstat()
    if path.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
        raise ValueError("The upstream cache must not contain linked paths")
    return info


def _directory():
    root = CACHE.absolute()
    boundary = ROOT.resolve()
    if root.resolve() == boundary or not root.resolve().is_relative_to(boundary):
        raise ValueError("Upstream cache must remain inside YAMPP")
    for part in (root, *root.parents):
        if part.exists(): _ordinary(part)
        if part == boundary: break
    root.mkdir(parents=True, exist_ok=True)
    return root.resolve()


def digest_file(path, algorithm="sha256"):
    digest = hashlib.new(algorithm)
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""): digest.update(chunk)
    return digest.hexdigest()


def _verified(path, spec):
    if not path.exists(): return False
    info = _ordinary(path)
    return path.is_file() and info.st_size == spec["size"] and digest_file(path) == spec["sha256"]


@contextmanager
def _lock(folder):
    path = folder / ".download.lock"
    if path.exists(): _ordinary(path)
    with path.open("a+b") as stream:
        if not stream.tell(): stream.write(b"0"); stream.flush()
        stream.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise ValueError("An upstream download is already running") from None
        try: yield
        finally:
            if os.name == "nt":
                stream.seek(0); msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else: fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def _download(folder, spec, progress=None):
    target = folder / spec["name"]
    if target.parent != folder: raise ValueError("Invalid upstream filename")
    if _verified(target, spec): return target
    opener = build_opener(ProxyHandler({}), GitHubRedirects())
    request = Request(_url(spec["url"]), headers={"User-Agent": "YAMPP-Upstream/1", "Accept-Encoding": "identity"})
    fd, name = tempfile.mkstemp(prefix=".download-", suffix=".part", dir=folder)
    partial = Path(name)
    try:
        with os.fdopen(fd, "wb") as output, opener.open(request, timeout=30) as response:
            _url(response.geturl())
            if response.status != 200: raise ValueError("GitHub did not return a complete release file")
            length = response.headers.get("Content-Length")
            if length is not None and int(length) != spec["size"]: raise ValueError("GitHub release size does not match this version")
            count = 0; digest = hashlib.sha256()
            while True:
                chunk = response.read(min(1024 * 1024, spec["size"] + 1 - count))
                if not chunk: break
                count += len(chunk)
                if count > spec["size"]: raise ValueError("GitHub release exceeds its pinned size")
                digest.update(chunk); output.write(chunk)
                if progress: progress(count, spec["size"])
            if count != spec["size"] or digest.hexdigest() != spec["sha256"]:
                raise ValueError("GitHub release verification failed; nothing was installed")
            output.flush(); os.fsync(output.fileno())
        if target.exists(): _ordinary(target)
        partial.replace(target)
        return target
    finally:
        # A unique direct child created above; never remove a computed tree.
        partial.unlink(missing_ok=True)


def plan(value):
    build = release(value)
    return {**download_offer(value), "confirmationRequired": True,
            "runtimeSupported": False, "files": [{"name": build[key]["name"], "size": build[key]["size"]} for key in ("archive", "project")]}


def acquire(value, *, confirmed=False, progress=None):
    build = release(value)
    if confirmed is not True: raise ValueError("Confirm the GitHub download before acquiring Akaneia")
    root = _directory()
    folder = root / (build["id"] + "-" + build["version"])
    if folder.exists(): _ordinary(folder)
    folder.mkdir(exist_ok=True)
    with _lock(folder):
        archive = _download(folder, build["archive"], progress)
        project = _download(folder, build["project"], progress)
        result = {**identity(build), "downloaded": True, "runtimeSupported": False,
                  "archivePath": str(archive), "projectPath": str(project),
                  "message": "Akaneia downloaded from GitHub. Play support is not ready in this build."}
        path = folder / "download.json"
        if path.exists(): _ordinary(path)
        fd, name = tempfile.mkstemp(prefix=".receipt-", dir=folder)
        temporary = Path(name)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as output: json.dump(result, output, indent=2)
            temporary.replace(path)
        finally: temporary.unlink(missing_ok=True)
        return result
