"""Apply a real update to a sandbox installation and check what survived.

The version check is the easy half. The half that matters is the swap: this
process is replacing the interpreter it is running on, so it hands the work to
a script that runs after it exits, and nothing about that is exercised by
asking a server what version it is.

So this builds a throwaway v0.0.1 installation and a throwaway v0.0.2 release
archive, serves the version and the archive from a local HTTP server, and runs
the updater for real. Then it checks the three things that would each be a
different kind of disaster:

  * the new files arrived,
  * the player's own files -- saves, settings, extracted disc, imported
    content -- are untouched,
  * an archive whose checksum does not match is refused before anything is
    unpacked.

Nothing here touches the real installation; everything happens in a temporary
directory.
"""
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OLD, NEW = "0.0.1", "0.0.2"
ASSET = "YAMPP-Windows-x64-v%s-release.zip" % NEW


def build_install(root, release):
    """A minimal installation: the updater's own files, a manifest, and the
    player's directories with something recognisable in each."""
    (root / "scripts").mkdir(parents=True, exist_ok=True)
    for name in ("project_config.py", "update_yampp.py"):
        shutil.copy2(ROOT / "scripts" / name, root / "scripts" / name)
    (root / "config").mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "config/project.xml", root / "config/project.xml")
    (root / "distribution-manifest.json").write_text(json.dumps(
        {"name": "Yet Another Melee PC Port", "shortName": "YAMPP",
         "release": release, "files": []}, indent=2))
    (root / "bin").mkdir(exist_ok=True)
    (root / "bin/YAMPP.exe").write_text("old executable")
    (root / "user").mkdir(exist_ok=True)
    (root / "user/settings.xml").write_text("the player's settings")
    (root / "user/imports").mkdir(exist_ok=True)
    (root / "user/imports/Akaneia.7z").write_text("the player's imported archive")
    (root / "data/GALE01/sys").mkdir(parents=True, exist_ok=True)
    (root / "data/GALE01/sys/main.dol").write_text("the player's extracted disc")


def build_release(staging, name):
    """The archive the pinned repository would publish for the new version."""
    tree = staging / name
    build_install(tree, NEW)
    (tree / "bin/YAMPP.exe").write_text("new executable")
    (tree / "NEWFILE.txt").write_text("added by the update")
    # A release archive carries no player state; that is the point of the test.
    shutil.rmtree(tree / "user")
    shutil.rmtree(tree / "data")
    archive = staging / ASSET
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(tree.rglob("*")):
            if path.is_file():
                z.write(path, str(Path(name) / path.relative_to(tree)))
    return archive


class Handler(http.server.BaseHTTPRequestHandler):
    archive = digest = None

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path.endswith("/version"):
            body = json.dumps({"schema": 1, "release": NEW,
                               "protocol": 2, "sync": "rollback-v2"}).encode()
            kind = "application/json"
        elif self.path.endswith(".sha256"):
            body = (Handler.digest + "  " + ASSET + "\n").encode()
            kind = "text/plain"
        elif self.path.endswith(".zip"):
            body = Handler.archive.read_bytes()
            kind = "application/zip"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def run_updater(install, port, expect):
    """Run the updater as the launcher does, then let the swap script finish."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("YAMPP_")}
    result = subprocess.run(
        [sys.executable, str(install / "scripts/update_yampp.py"),
         "--server", "http://127.0.0.1:%d/api/version" % port],
        cwd=str(install), env=env, capture_output=True, text=True, timeout=180)
    print(result.stdout.strip())
    if result.stderr.strip():
        print(result.stderr.strip())
    if result.returncode != expect:
        raise SystemExit("updater returned %d, expected %d" % (result.returncode, expect))
    return result


def main():
    failures = []
    with tempfile.TemporaryDirectory(prefix="yampp-updater-check-") as temporary:
        area = Path(temporary)
        staging = area / "publish"
        staging.mkdir()
        archive = build_release(staging, "YAMPP-Windows-x64-v" + NEW)
        Handler.archive = archive
        Handler.digest = hashlib.sha256(archive.read_bytes()).hexdigest()

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        port = server.server_address[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()

        # --- an archive whose checksum does not match is refused ------------
        install = area / "bad"
        build_install(install, OLD)
        patch_repository(install, port, corrupt=True)
        run_updater(install, port, 0)          # reports, does not apply
        if (install / "NEWFILE.txt").exists():
            failures.append("a mismatched checksum still unpacked the archive")
        if (install / "bin/YAMPP.exe").read_text() != "old executable":
            failures.append("a mismatched checksum still replaced files")

        # --- the real thing -------------------------------------------------
        install = area / "good"
        build_install(install, OLD)
        patch_repository(install, port, corrupt=False)
        run_updater(install, port, 3)          # 3 = restarting to finish

        deadline = time.time() + 120
        while time.time() < deadline and not (install / "NEWFILE.txt").exists():
            time.sleep(1)
        # The swap script relaunches the game, which does not exist here.
        time.sleep(2)

        if not (install / "NEWFILE.txt").exists():
            failures.append("the update never applied")
        if (install / "bin/YAMPP.exe").read_text() != "new executable":
            failures.append("the executable was not replaced")
        for kept, expected in (("user/settings.xml", "the player's settings"),
                               ("user/imports/Akaneia.7z", "the player's imported archive"),
                               ("data/GALE01/sys/main.dol", "the player's extracted disc")):
            path = install / kept
            if not path.exists():
                failures.append("the update deleted " + kept)
            elif path.read_text() != expected:
                failures.append("the update overwrote " + kept)
        release = json.loads((install / "distribution-manifest.json").read_text()).get("release")
        if release != NEW:
            failures.append("the installation still reports v%s" % release)

        server.shutdown()

    for line in failures:
        print("FAIL:", line)
    if failures:
        return 1
    print("PASS: the update applied, the player's own files survived, and a bad checksum was refused")
    return 0


def patch_repository(install, port, corrupt):
    """Point the sandbox updater at the local server instead of GitHub.

    Only the release lookup is redirected. Everything else -- the checksum
    check, the path validation, the swap -- runs exactly as shipped.
    """
    path = install / "scripts/update_yampp.py"
    text = path.read_text()
    base = "http://127.0.0.1:%d" % port
    text = text.replace(
        'RELEASE_API = "https://api.github.com/repos/%s/releases/tags/v%%s" % REPOSITORY',
        'RELEASE_API = "%s/releases/%%s"' % base, 1)
    digest_name = ASSET + (".bad" if corrupt else ".sha256")
    text = text.replace(
        "def asset_urls(release):",
        "def asset_urls(release):\n"
        "    return ('%s/%s', '%s/%s')\n"
        "def _unused(release):" % (base, ASSET, base, digest_name), 1)
    path.write_text(text)


if __name__ == "__main__":
    raise SystemExit(main())
