"""Room-version isolation and integrity, using real Melee costume archives."""
from pathlib import Path
import copy
import hashlib
import io
import os
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "build/modkit/room-cache-tests"
WORK.mkdir(parents=True, exist_ok=True)
TEMP = tempfile.TemporaryDirectory(prefix="run-", dir=WORK)
TEST = Path(TEMP.name)
os.environ["MELEE_WORKSHOP_TEST_MODS"] = str(TEST / "mods")
sys.path.insert(0, str(Path(__file__).resolve().parent))
import catalog
import community
import room_cache
from xml_manifest import write_manifest


def tree(folder):
    return {str(p.relative_to(folder)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in folder.rglob("*") if p.is_file()}


class RoomVersions(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp(dir=TEST))
        self.mods = self.work / "mods"
        self.mods.mkdir()
        self.cache = self.work / "room-cache"
        self.patches = [patch.object(catalog, "MODS", self.mods),
                        patch.object(community, "MODS", self.mods),
                        patch.object(room_cache, "ROOM_ROOT", self.cache)]
        for item in self.patches:
            item.start()
        self.addCleanup(lambda: [item.stop() for item in reversed(self.patches)])
        self.packages = {}
        for version, color in (("1.0.0", "Nr"), ("1.0.1", "Re")):
            manifest = {"schema": 1, "kind": "costume", "id": "room-link",
                        "name": "Room Link", "version": version, "base": "stock-Lk", "additive": True,
                        "costumes": [{"id": "color", "name": "Version " + version, "file": "files/PlLkRoom.dat"}]}
            write_manifest(self.work / version, manifest)
            files = {"manifest.xml": (self.work / version / "manifest.xml").read_bytes(),
                     "files/PlLkRoom.dat": (catalog.ASSETS / ("PlLk" + color + ".dat")).read_bytes()}
            stream = io.BytesIO()
            with zipfile.ZipFile(stream, "w", zipfile.ZIP_DEFLATED) as archive:
                for name, raw in files.items():
                    archive.writestr(name, raw)
            raw = stream.getvalue()
            sha = hashlib.sha256(raw).hexdigest()
            inspect, validate = community.validator()
            checked, payloads = inspect(raw)
            self.packages[sha] = (validate(raw), raw, checked, payloads)
        self.old, self.new = self.packages
        self.local = self.mods / "room-link"
        _, raw, manifest, files = self.packages[self.new]
        for name, data in files.items():
            target = self.local / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        local_manifest = dict(manifest, enabled=True)
        write_manifest(self.local, local_manifest)
        community.stamp_install(self.local, local_manifest, files, self.new, len(raw))
        catalog.costume_registry()
        self.download = patch.object(community, "download_verified", side_effect=lambda sha, *args: copy.deepcopy(self.packages[sha]))
        self.mock_download = self.download.start()
        self.addCleanup(self.download.stop)

    def rows(self):
        return [line.split("\t") for line in (self.mods / "costumes.tsv").read_text().splitlines()[1:]]

    def test_old_new_room_and_offline_restore(self):
        before = tree(self.local)
        initial = (self.mods / "costumes.tsv").read_bytes()
        result = room_cache.prepare_room([self.old])
        self.assertEqual(result["sha256s"], [self.old])
        self.assertTrue(result["roomOnly"])
        rows = self.rows()
        self.assertEqual([r[2] for r in rows if r[3] == "1"], [self.new])
        self.assertEqual([r[2] for r in rows if r[3] == "0"], [self.old])
        self.assertEqual(len([r for r in rows if r[2] == self.old]), 1)
        self.assertEqual(room_cache.prepare_room([self.old]), result)
        self.assertEqual(self.mock_download.call_count, 1)
        self.assertEqual(tree(self.local), before)
        result = room_cache.prepare_room([self.new])
        self.assertEqual(result["cached"], 0)
        self.assertEqual((self.mods / "costumes.tsv").read_bytes(), initial)
        room_cache.prepare_room([])
        self.assertEqual((self.mods / "costumes.tsv").read_bytes(), initial)
        self.assertEqual(tree(self.local), before)

    def test_tamper_and_duplicate_versions_preserve_registry(self):
        room_cache.prepare_room([self.old])
        before = tree(self.local)
        registry = (self.mods / "costumes.tsv").read_bytes()
        for relative in ("package.zip", "manifest.xml", "files/PlLkRoom.dat"):
            path = self.cache / self.old / relative
            original = path.read_bytes()
            path.write_bytes(original + b"tampered")
            with self.assertRaisesRegex(ValueError, "integrity"):
                room_cache.prepare_room([self.old])
            self.assertEqual((self.mods / "costumes.tsv").read_bytes(), registry)
            self.assertEqual(tree(self.local), before)
            path.write_bytes(original)
        with self.assertRaisesRegex(ValueError, "two versions"):
            room_cache.prepare_room(sorted([self.old, self.new]))
        self.assertEqual((self.mods / "costumes.tsv").read_bytes(), registry)

    def test_edited_author_files_are_preserved_and_failure_is_atomic(self):
        manifest = community.read_manifest(self.local)
        manifest["enabled"] = False
        manifest["description"] = "My unpublished author edits"
        write_manifest(self.local, manifest)
        before = tree(self.local)
        room_cache.prepare_room([self.new])
        self.assertEqual(tree(self.local), before)
        rows = self.rows()
        self.assertEqual([r[2] for r in rows if r[2] != "-"], [self.new])
        self.assertTrue(all(r[3] == "0" for r in rows))
        registry = (self.mods / "costumes.tsv").read_bytes()
        missing = "f" * 64
        with self.assertRaises(KeyError):
            room_cache.prepare_room(sorted([self.old, missing]))
        self.assertEqual((self.mods / "costumes.tsv").read_bytes(), registry)
        self.assertEqual(tree(self.local), before)

    def test_cache_limits_prune_only_unselected_cache(self):
        room_cache.prepare_room([self.old])
        untouched = self.work / "keep.txt"
        untouched.write_text("outside cache")
        fake = self.cache / ("0" * 64)
        fake.mkdir()
        (fake / "old.bin").write_bytes(bytes(64))
        with patch.object(room_cache, "MAX_ENTRIES", 1):
            room_cache.prepare_room([self.old])
        self.assertFalse(fake.exists())
        self.assertTrue((self.cache / self.old).is_dir())
        self.assertEqual(untouched.read_text(), "outside cache")
        with patch.object(room_cache, "MAX_BYTES", 1):
            with self.assertRaisesRegex(ValueError, "cache limit"):
                room_cache.prepare_room([self.old])
        self.assertTrue((self.cache / self.old).is_dir())

    def test_native_consent_worker_uses_room_only_cache(self):
        import json
        import subprocess
        import threading
        from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
        executable = ROOT / "build/tests/netplay-mods/netplay_mods_test.exe"
        if not executable.exists():
            self.skipTest("Compile the focused native worker test first")
        metadata, raw, _, _ = self.packages[self.old]
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_GET(self):
                body = raw if self.path.endswith("/package.zip") else json.dumps(metadata).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        before = tree(self.local)
        env = os.environ.copy()
        env.update(MELEE_WORKSHOP_TEST_MODS=str(self.mods), MELEE_WORKSHOP_PYTHON=sys.executable,
                   MELEE_MOD_REPOSITORY_URL="http://127.0.0.1:%d/api/mods" % server.server_port)
        try:
            result = subprocess.run([str(executable), "--room-version", self.old], cwd=ROOT,
                                    env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Consented room-only version prepared", result.stdout)
        finally:
            server.shutdown()
            server.server_close()
        self.assertEqual(tree(self.local), before)
        self.assertEqual([r[2] for r in self.rows() if r[3] == "1"], [self.new])
        self.assertEqual([r[2] for r in self.rows() if r[3] == "0"], [self.old])

    def test_platform_registry_output_precedence_and_room_overlay(self):
        default = self.mods / "costumes.tsv"
        original = default.read_bytes()
        selected = self.work / "platform/costumes.tsv"
        explicit = self.work / "explicit/costumes.tsv"
        with patch.dict(os.environ, {"MELEE_COSTUME_REGISTRY": str(selected)}), \
                patch.object(catalog, "roster_icons", side_effect=AssertionError("No legacy Bridge work")), \
                patch("subprocess.run", side_effect=AssertionError("No external helper needed")):
            self.assertEqual(catalog.costume_registry(output=explicit), explicit)
            self.assertEqual(explicit.read_bytes(), original)
            self.assertFalse(selected.exists())
            self.assertEqual(catalog.costume_registry(), selected)
            self.assertEqual(selected.read_bytes(), original)
            community.toggle(self.new, False)
            self.assertEqual(community.active()["sha256s"], [])
            self.assertTrue(all(row.split("\t")[3] == "0" for row in selected.read_text().splitlines()[1:]))
            room_cache.prepare_room([self.old])
            hashes = {row.split("\t")[2] for row in selected.read_text().splitlines()[1:]}
            self.assertEqual(hashes, {self.old, self.new})
            self.assertEqual(default.read_bytes(), original)
            community.toggle(self.new, True)
            self.assertEqual(community.active()["sha256s"], [self.new])
            self.assertEqual(selected.read_bytes(), original)
            self.assertEqual(default.read_bytes(), original)
            self.assertEqual(explicit.read_bytes(), original)
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop("MELEE_COSTUME_REGISTRY", None)
            self.assertEqual(catalog.costume_registry(), default)
        self.assertFalse(any(name == "PIL" or name.startswith("PIL.") for name in sys.modules))

    def test_cosmetic_operations_need_no_legacy_registry_or_bridge(self):
        with patch.object(catalog, "roster_icons", side_effect=AssertionError("Legacy roster bridge invoked")):
            self.assertEqual(community.active()["sha256s"], [self.new])
            self.assertTrue(community.install(self.new)["alreadyInstalled"])
            self.assertFalse(community.toggle(self.new, False)["enabled"])
            self.assertTrue(community.toggle(self.new, True)["enabled"])
        self.assertFalse((self.mods / "registry.tsv").exists())
        self.assertFalse((self.mods / "catalog").exists())

    def test_unsafe_prune_paths_are_rejected(self):
        root = room_cache._root()
        outside = self.work / "outside"
        outside.mkdir()
        (outside / "keep.txt").write_text("preserve")
        with self.assertRaisesRegex(ValueError, "Unsafe room cache removal"):
            room_cache._remove(root, outside)
        with patch.object(room_cache, "ROOM_ROOT", ROOT / "user"):
            with self.assertRaisesRegex(ValueError, "Invalid room cache"):
                room_cache.prepare_room([])
        self.assertEqual((outside / "keep.txt").read_text(), "preserve")

    def test_cancel_before_prepare_and_invalid_sets_do_not_mutate(self):
        before = tree(self.mods)
        # Declining the native prompt invokes no helper at all. Validation of
        # an invalid requested set must likewise occur before any cache writes.
        for hashes in ([self.old, self.old], [self.new, self.old] if self.new > self.old else [self.old, self.new], ["no"]):
            with self.assertRaises(ValueError):
                room_cache.prepare_room(hashes)
        self.assertFalse(self.cache.exists())
        self.mock_download.assert_not_called()
        self.assertEqual(tree(self.mods), before)


if __name__ == "__main__":
    try:
        unittest.main()
    finally:
        TEMP.cleanup()
