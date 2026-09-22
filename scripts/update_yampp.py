"""Bring this installation up to the release the master server is running.

A room only admits clients whose build fingerprint matches its host's, so
playing online against anyone on a newer build is not merely degraded -- it is
refused, with a message that does not say what to do about it. This runs
before the game and closes that gap.

What is trusted, and what is not:

  * The server is asked one question -- which release it expects -- and its
    answer is treated as a version string and nothing else. It never supplies
    a download address. This file pins the repository, so a server that is
    compromised, impersonated, or simply wrong can make the launcher check
    GitHub for a release that does not exist, and cannot make it fetch code
    from anywhere else.
  * The archive is checked against the `.sha256` published beside it before a
    single file is unpacked, and the unpack refuses absolute or escaping paths.
  * `user/`, `data/` and `config/` are never replaced. Saves, settings, the
    extracted game and imported content belong to the player, survive every
    update, and are the reason this copies into the installation rather than
    replacing it wholesale.

The swap itself cannot be done from here, because this interpreter is one of
the files being replaced. The staged copy is handed to a short script that
runs after this process exits.

Run it by hand with --check to see what it would do.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import ssl
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from project_config import ROOT, project_path

# Pinned here on purpose; see the note above. Nothing the server says can
# change where an update comes from.
REPOSITORY = "sonsegajp/YAMPP"
ASSET = "YAMPP-Windows-x64-v%s-release.zip"
RELEASE_API = "https://api.github.com/repos/%s/releases/tags/v%%s" % REPOSITORY

# Directories whose contents belong to the player, not to the build.
KEEP = ("user", "data", "config")
TIMEOUT = 30
MAX_ARCHIVE = 2 * 1024 ** 3


def local_release():
    """The version recorded when this copy was packaged, or None."""
    manifest = ROOT / "distribution-manifest.json"
    if not manifest.is_file():
        return None
    try:
        return json.loads(manifest.read_text()).get("release")
    except (OSError, ValueError):
        return None


def configured_server():
    """The relay this installation talks to, as an HTTPS API base."""
    path = ROOT / "config/online.xml"
    address = ""
    if path.is_file():
        try:
            node = ET.parse(path).getroot().find("server")
            address = (node.text or "").strip() if node is not None else ""
        except ET.ParseError:
            address = ""
    if not address:
        address = "mmodx.fun/melee/"
    address = address.rstrip("/")
    if address.startswith(("http://", "https://")):
        return address + "/api/version"
    # A bare host, or a host with the reverse-proxy path the game uses.
    return "https://" + address + "/api/version"


def expected_release(url):
    """What the server says its players should be running, or None.

    Every failure here is silent and non-fatal: the update is a convenience,
    and a launcher that refuses to start the game because a version check
    timed out would be worse than one that is occasionally out of date.
    """
    try:
        context = ssl.create_default_context()
        request = urllib.request.Request(url, headers={"User-Agent": "YAMPP-updater"})
        with urllib.request.urlopen(request, timeout=TIMEOUT, context=context) as response:
            if response.status != 200:
                return None
            document = json.loads(response.read(64 * 1024).decode("utf-8", "replace"))
    except (urllib.error.URLError, OSError, ValueError, TimeoutError):
        return None
    release = document.get("release")
    if document.get("schema") != 1 or not isinstance(release, str):
        return None
    # A version string and nothing else, so it can only ever be pasted into
    # the pinned URL below.
    return release if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", release) else None


def asset_urls(release):
    """Where the pinned repository publishes that release, or None.

    The names are asked of GitHub rather than assumed, so a release whose
    archive was named differently is reported as unavailable instead of
    downloading the wrong thing.
    """
    try:
        request = urllib.request.Request(RELEASE_API % release,
                                         headers={"User-Agent": "YAMPP-updater",
                                                  "Accept": "application/vnd.github+json"})
        with urllib.request.urlopen(request, timeout=TIMEOUT,
                                    context=ssl.create_default_context()) as response:
            document = json.loads(response.read(1024 * 1024).decode("utf-8", "replace"))
    except (urllib.error.URLError, OSError, ValueError, TimeoutError):
        return None
    wanted = ASSET % release
    archive = digest = None
    for entry in document.get("assets", []):
        name, url = entry.get("name"), entry.get("browser_download_url", "")
        if not isinstance(url, str) or not url.startswith("https://"):
            continue
        if name == wanted:
            archive = url
        elif name == wanted + ".sha256":
            digest = url
    return (archive, digest) if archive and digest else None


def fetch(url, destination=None):
    request = urllib.request.Request(url, headers={"User-Agent": "YAMPP-updater"})
    with urllib.request.urlopen(request, timeout=TIMEOUT,
                                context=ssl.create_default_context()) as response:
        if destination is None:
            return response.read(64 * 1024)
        total = 0
        sha = hashlib.sha256()
        with destination.open("wb") as out:
            while True:
                block = response.read(1024 * 1024)
                if not block:
                    break
                total += len(block)
                if total > MAX_ARCHIVE:
                    raise ValueError("The release archive is larger than this updater will accept")
                sha.update(block)
                out.write(block)
                print("\r  %.0f MB" % (total / 1024 ** 2), end="", flush=True)
        print()
        return sha.hexdigest()


def stage(release, archive_url, digest_url, staging):
    """Download, verify and unpack one release. Returns its root directory."""
    expected = fetch(digest_url).decode("ascii", "replace").split()
    if not expected or not re.fullmatch(r"[0-9a-f]{64}", expected[0]):
        raise ValueError("The published checksum for v%s is not readable" % release)
    staging.mkdir(parents=True, exist_ok=True)
    archive = staging / (ASSET % release)
    print("Downloading YAMPP v%s..." % release)
    actual = fetch(archive_url, archive)
    if actual != expected[0]:
        archive.unlink(missing_ok=True)
        raise ValueError("The downloaded archive does not match its published checksum")
    print("Checksum verified. Unpacking...")
    root = staging / "unpacked"
    with zipfile.ZipFile(archive) as z:
        names = z.namelist()
        for name in names:
            if name.startswith("/") or ".." in Path(name).parts or ":" in name:
                raise ValueError("The archive contains an unsafe path: " + name)
        top = {Path(name).parts[0] for name in names if name.strip("/")}
        if len(top) != 1:
            raise ValueError("The archive does not have a single root folder")
        z.extractall(root)
    archive.unlink(missing_ok=True)
    return root / top.pop()


def write_swap_script(source, target, staging):
    """The part that runs once this interpreter is no longer in the way.

    robocopy is used rather than a Python copy for one reason: this process,
    its interpreter and its standard library are all inside `target`, and a
    running program cannot replace its own files.
    """
    script = staging / "apply-update.cmd"
    keep = " ".join('/XD "%s"' % (Path(target) / name) for name in KEEP)
    script.write_text("\r\n".join([
        "@echo off",
        "title Updating YAMPP",
        "echo Applying the update. Do not close this window.",
        # The launcher is still exiting; give its handles a moment to close.
        "ping -n 4 127.0.0.1 >nul",
        'robocopy "%s" "%s" /E /NFL /NDL /NJH /NJS /NP %s >nul' % (source, target, keep),
        "if errorlevel 8 (",
        "  echo The update could not be applied. Your installation is unchanged.",
        "  pause",
        "  exit /b 1",
        ")",
        'rmdir /s /q "%s" >nul 2>&1' % staging,
        'start "" "%s"' % (Path(target) / "Play YAMPP.cmd"),
    ]) + "\r\n", encoding="ascii")
    return script


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="report what would happen and change nothing")
    ap.add_argument("--server", help="version API to ask instead of the configured relay")
    args = ap.parse_args()

    if os.environ.get("YAMPP_NO_UPDATE"):
        return 0
    here = local_release()
    if here is None:
        # A source tree or a hand-assembled folder. Updating one of those from
        # a release archive would replace a developer's build with a stranger's.
        if args.check:
            print("This copy was not packaged from a release; nothing to update.")
        return 0
    wanted = expected_release(args.server or configured_server())
    if wanted is None:
        if args.check:
            print("The server did not answer; staying on v%s." % here)
        return 0
    if wanted == here:
        if args.check:
            print("Up to date: v%s." % here)
        return 0

    print("The server is running v%s; this copy is v%s." % (wanted, here))
    if args.check:
        return 0
    urls = asset_urls(wanted)
    if urls is None:
        print("v%s has not been published for Windows yet; starting the current build." % wanted)
        return 0
    staging = Path(tempfile.mkdtemp(prefix="yampp-update-", dir=str(ROOT.parent)))
    try:
        source = stage(wanted, urls[0], urls[1], staging)
    except (ValueError, OSError, urllib.error.URLError, zipfile.BadZipFile) as exc:
        print("Update failed: %s" % exc)
        print("Starting the current build instead.")
        subprocess.run(["cmd", "/c", "rmdir", "/s", "/q", str(staging)],
                       capture_output=True, check=False)
        return 0
    script = write_swap_script(source, ROOT, staging)
    print("Restarting to finish the update...")
    subprocess.Popen(["cmd", "/c", "start", "", "/min", str(script)],
                     cwd=str(ROOT.parent), close_fds=True)
    # 3 tells the launcher an update is being applied and it must not go on to
    # start the game from files that are about to be replaced underneath it.
    return 3


if __name__ == "__main__":
    raise SystemExit(main())
