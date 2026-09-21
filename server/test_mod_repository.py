"""Costume package validation, actual HTTP transfers, and pre-lobby hash gates."""
import asyncio
import hashlib
import io
import json
from pathlib import Path
import stat
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile

from melee_netplay_server import Client, Server, MAX_LINE_BYTES
from mod_repository import (MAX_PACKAGE_BYTES, PackageError, Repository,
                            inspect_archive, validate_package)
from test_netplay_server import RecordingWriter


def fixture_dat(symbol="PlyLink5K_Share_joint"):
    data = bytes(64)
    names = symbol.encode() + b"\0"
    size = 32 + len(data) + 8 + len(names)
    return struct.pack(">8I", size, len(data), 0, 1, 0, 0, 0, 0) + data + struct.pack(">II", 0, 0) + names


def manifest_xml(value):
    def encode(node, item):
        if isinstance(item, dict):
            node.set("type", "object")
            for key in sorted(item):
                encode(ET.SubElement(node, "field", name=key), item[key])
        elif isinstance(item, list):
            node.set("type", "array")
            for child in item:
                encode(ET.SubElement(node, "item"), child)
        elif isinstance(item, bool):
            node.set("type", "bool"); node.text = "true" if item else "false"
        elif isinstance(item, int):
            node.set("type", "int"); node.text = str(item)
        else:
            node.set("type", "string"); node.text = item
    root = ET.Element("melee-package", schema="1")
    encode(root, value)
    return ET.tostring(root, encoding="utf-8", xml_declaration=True)


def fixture_package(changes=None, extra=None, model=None):
    manifest = dict(schema=1, id="test-link", name="Test Link", version="1.0.0",
                    kind="costume", base="stock-Lk", additive=True,
                    costumes=[dict(id="silver", name="Silver", file="files/PlLkSilver.dat")])
    manifest.update(changes or {})
    files = {"manifest.xml": manifest_xml(manifest),
             "files/PlLkSilver.dat": model if model is not None else fixture_dat()}
    files.update(extra or {})
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(files):
            item = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            item.external_attr = (stat.S_IFREG | 0o644) << 16
            item.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(item, files[name])
    return output.getvalue()


class PackageTests(unittest.TestCase):
    def test_reproducible_package_and_original_bytes_are_preserved(self):
        raw = fixture_package()
        self.assertEqual(raw, fixture_package())
        manifest, files = inspect_archive(raw)
        self.assertEqual(manifest["base"], "stock-Lk")
        self.assertEqual(files["files/PlLkSilver.dat"], fixture_dat())
        metadata = validate_package(raw)
        self.assertEqual(metadata["sha256"], hashlib.sha256(raw).hexdigest())

    def test_declared_portrait_and_stock_icon_are_hash_bound_visual_payloads(self):
        import base64
        png = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a84sAAAAASUVORK5CYII=")
        costume = dict(id="silver", name="Silver", file="files/PlLkSilver.dat",
                       portrait="art/portrait.png", stockIcon="art/stock.png")
        raw = fixture_package({"costumes": [costume]}, {"art/portrait.png": png, "art/stock.png": png})
        manifest, files = inspect_archive(raw)
        self.assertEqual(manifest["costumes"][0]["stockIcon"], "art/stock.png")
        self.assertEqual(files["art/stock.png"], png)
        self.assertEqual(validate_package(raw)["sha256"], hashlib.sha256(raw).hexdigest())
        for path, payload in (("art/stock.png", b"invalid"), ("art/stock.dat", png), ("../stock.png", png)):
            changed = dict(costume, stockIcon=path)
            with self.subTest(path=path), self.assertRaises(PackageError):
                inspect_archive(fixture_package({"costumes": [changed]}, {"art/portrait.png": png, path: payload}))
        with self.assertRaises(PackageError):
            inspect_archive(fixture_package({"costumes": [costume]}, {"art/portrait.png": png}))

    def test_native_texture_sidecars_require_exact_typed_geometry_and_payload(self):
        portrait = b"YAMPPTEX" + struct.pack(">4I", 136, 188, 14, 13056) + bytes(13056)
        stock = b"YAMPPTEX" + struct.pack(">4I", 24, 24, 14, 288) + bytes(288)
        costume = dict(id="silver", name="Silver", file="files/PlLkSilver.dat",
                       portraitTexture="art/portrait.gxt", stockIconTexture="art/stock.gxt")
        raw = fixture_package({"costumes": [costume]}, {"art/portrait.gxt": portrait, "art/stock.gxt": stock})
        manifest, files = inspect_archive(raw)
        self.assertEqual(files[manifest["costumes"][0]["stockIconTexture"]], stock)
        for invalid in (stock[:-1], stock + b"x", b"BADMAGIC" + stock[8:], portrait,
                        b"YAMPPTEX" + struct.pack(">4I", 24, 24, 8, 288) + bytes(288)):
            with self.subTest(size=len(invalid)), self.assertRaises(PackageError):
                inspect_archive(fixture_package({"costumes": [costume]}, {"art/portrait.gxt": portrait, "art/stock.gxt": invalid}))
        with self.assertRaises(PackageError):
            inspect_archive(fixture_package(extra={"art/undeclared.gxt": stock}))

    def test_gameplay_fields_scripts_and_nonadditive_packs_are_rejected(self):
        for changes, extra in [({"additive": False}, None), ({"kind": "fighter"}, None),
                               ({"lua": {"entry": "script.lua"}}, None),
                               ({"attributes": {}}, None), (None, {"script.lua": b"return {}"}),
                               (None, {"files/PlLk.dat": fixture_dat()}),
                               ({"costumes": []}, None)]:
            with self.subTest(changes=changes, extra=extra), self.assertRaises(PackageError):
                inspect_archive(fixture_package(changes, extra))

    def test_traversal_windows_aliases_and_case_collisions_are_rejected(self):
        for name in ("../escape.dat", "/absolute.dat", "C:/x.dat", "files\\x.dat",
                     "files/CON.dat", "files/model.dat:stream", "files/x. ",
                     "files/./x.dat", "FILES/PLLKSilVER.DAT"):
            with self.subTest(name=name), self.assertRaises(PackageError):
                inspect_archive(fixture_package(extra={name: b"x"}))

    def test_symlink_and_corrupt_archive_are_rejected(self):
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as archive:
            item = zipfile.ZipInfo("manifest.xml")
            item.create_system = 3
            item.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(item, "../outside")
        for raw in (b"not a zip", output.getvalue(), fixture_package()[:-22]):
            with self.assertRaises(PackageError):
                inspect_archive(raw)

    def test_costume_must_keep_stock_fighter_model_symbols(self):
        for model in (fixture_dat("ftDataLink"), fixture_dat("PlyFox5K_Share_joint"), b"broken"):
            with self.assertRaises(PackageError):
                inspect_archive(fixture_package(model=model))

    def test_original_color_symbol_is_preserved(self):
        metadata = validate_package(fixture_package(model=fixture_dat("PlyLink5KRe_Share_joint")))
        self.assertEqual(metadata["base"], "stock-Lk")
        with self.assertRaises(PackageError):
            inspect_archive(fixture_package(model=fixture_dat("PlyLink5KUnknown_Share_joint")))

    def test_multiple_costumes_require_unique_ids_and_distinct_dat_files(self):
        costumes = [dict(id="silver", name="Silver", file="files/PlLkSilver.dat"),
                    dict(id="gold", name="Gold", file="files/PlLkGold.dat")]
        raw = fixture_package({"costumes": costumes}, {"files/PlLkGold.dat": fixture_dat()})
        self.assertEqual(len(validate_package(raw)["costumes"]), 2)
        costumes[1]["id"] = "silver"
        with self.assertRaises(PackageError):
            inspect_archive(fixture_package({"costumes": costumes}, {"files/PlLkGold.dat": fixture_dat()}))

    def test_xml_entities_and_duplicate_fields_are_rejected(self):
        for xml in (b'<!DOCTYPE x [<!ENTITY x "test">]><melee-package schema="1"/>',
                    b'<melee-package schema="1" type="object"><field name="id" type="string">x</field><field name="id" type="string">y</field></melee-package>'):
            with self.assertRaises(PackageError):
                inspect_archive(fixture_package(extra={"manifest.xml": xml}))

    def test_repository_survives_restart_and_does_not_trust_corrupt_stored_bytes(self):
        with tempfile.TemporaryDirectory() as root:
            repo = Repository(root)
            raw = fixture_package()
            metadata, created = repo.publish(raw)
            self.assertTrue(created)
            self.assertFalse(repo.publish(raw)[1])
            digest = metadata["sha256"]
            self.assertEqual(repo.package_path(digest).read_bytes(), raw)
            self.assertEqual(Repository(root).get(digest), metadata)
            repo.package_path(digest).write_bytes(b"tampered")
            self.assertIsNone(Repository(root).get(digest))


class ModRoomTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.repo = Repository(self.directory.name)
        self.metadata, _ = self.repo.publish(fixture_package())
        self.digest = self.metadata["sha256"]
        self.server = Server(self.repo)
        self.host, self.guest = self.client("Host"), self.client("Guest")

    def client(self, name):
        client = Client(self.server, None, RecordingWriter())
        self.server.clients[client.id] = client
        self.command(client, "hello", name=name, version=2, sync="rollback-v2", features=["mods-v1"])
        return client

    def command(self, client, op, **fields):
        self.server.handle(client, dict(op=op, **fields))

    def room(self):
        self.command(self.host, "create", required_mods=[self.digest], installed_mods=[self.digest])
        return self.host.room

    def test_missing_pack_prompts_before_membership_and_exact_hash_joins(self):
        room = self.room()
        self.command(self.guest, "join", room=room.id)
        self.assertIsNone(self.guest.room)
        self.assertEqual(room.players, [self.host])
        self.assertEqual(self.guest.writer.messages[-1],
                         dict(op="mods_required", room=room.id, mods=room.required_mods))
        self.command(self.guest, "join", room=room.id, installed_mods=[self.digest])
        self.assertIs(self.guest.room, room)
        self.assertEqual(self.guest.writer.messages[-1]["op"], "room")
        self.assertEqual(room.summary()["required_mods"][0]["sha256"], self.digest)
        self.assertNotIn("costumes", room.summary()["required_mods"][0])

    def test_failed_join_keeps_existing_room_and_readiness(self):
        target = self.room()
        self.command(self.guest, "create", name="Keep my room")
        previous = self.guest.room
        self.command(self.guest, "ready", ready=True)
        self.command(self.guest, "join", room=target.id, installed_mods=[])
        self.assertIs(self.guest.room, previous)
        self.assertTrue(self.guest.ready)

    def test_unknown_hash_and_unverified_host_do_not_replace_existing_room(self):
        self.command(self.host, "create")
        original = self.host.room
        for required, installed in [(["f" * 64], ["f" * 64]), ([self.digest], []), ([self.digest] * 2, [])]:
            self.command(self.host, "create", required_mods=required, installed_mods=installed)
            self.assertIs(self.host.room, original)
            self.assertEqual(self.host.writer.messages[-1]["op"], "error")

    def test_extra_active_pack_cannot_silently_enter_unmodded_room(self):
        self.command(self.host, "create")
        self.command(self.guest, "join", room=self.host.room.id, installed_mods=[self.digest])
        self.assertIsNone(self.guest.room)
        self.assertEqual(self.guest.writer.messages[-1]["mods"], [])

    def test_legacy_feature_handshake_cannot_host_or_join_modded_room(self):
        room = self.room()
        self.command(self.guest, "hello", version=2, sync="rollback-v2", features=[])
        self.command(self.guest, "join", room=room.id, installed_mods=[self.digest])
        self.assertIsNone(self.guest.room)
        self.assertIn("mods-v1", self.guest.writer.messages[-1]["message"])
        self.command(self.guest, "create", required_mods=[self.digest], installed_mods=[self.digest])
        self.assertIsNone(self.guest.room)

    def test_features_cannot_change_after_join(self):
        room = self.room()
        self.command(self.host, "hello", version=2, sync="rollback-v2", features=[])
        self.assertIs(self.host.room, room)
        self.assertIn("mods-v1", self.host.features)
        self.assertEqual(self.host.installed_mods, (self.digest,))
        self.assertEqual(self.host.writer.messages[-1]["op"], "error")

    def test_unconfirmed_state_blocks_ready_and_start(self):
        room = self.room()
        self.command(self.guest, "join", room=room.id, installed_mods=[self.digest])
        self.guest.installed_mods = ()
        self.command(self.guest, "ready", ready=True)
        self.assertFalse(self.guest.ready)
        self.host.ready = self.guest.ready = True
        self.command(self.host, "start")
        self.assertEqual(room.session, 0)
        self.assertEqual(self.host.writer.messages[-1]["op"], "error")

    def test_room_list_stays_within_native_receive_limit(self):
        self.metadata["name"] = "\u8a9e" * 64
        self.metadata["id"] = "x" * 64
        for _ in range(40):
            client = self.client("Browser fixture")
            self.command(client, "create", required_mods=[self.digest], installed_mods=[self.digest])
        result = self.server.room_list()
        self.assertLess(len(json.dumps(result).encode()), 65536)
        self.assertLessEqual(len(result["rooms"]), 32)
        self.assertTrue(result["more"])

    def test_session_start_repeats_fixed_required_hashes(self):
        room = self.room()
        self.command(self.guest, "join", room=room.id, installed_mods=[self.digest])
        for client in (self.host, self.guest):
            self.command(client, "ready", ready=True)
        self.command(self.host, "start")
        start = self.host.writer.messages[-1]
        self.assertEqual(start["op"], "start")
        self.assertEqual(start["sync"], "rollback-v2")
        self.assertEqual(start["required_mods"], room.required_mods)


class RepositoryHttpTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.repo = Repository(self.directory.name)
        self.server = Server(self.repo, upload_token="local-test-token")
        self.handlers = set()
        async def handle(reader, writer):
            task = asyncio.current_task(); self.handlers.add(task)
            try:
                await self.server.serve_client(reader, writer)
            finally:
                self.handlers.remove(task)
        self.listener = await asyncio.start_server(handle, "127.0.0.1", 0, limit=MAX_LINE_BYTES)
        self.port = self.listener.sockets[0].getsockname()[1]

    async def asyncTearDown(self):
        self.listener.close(); await self.listener.wait_closed()
        if self.handlers:
            await asyncio.wait_for(asyncio.gather(*tuple(self.handlers)), 3)
        self.assertEqual(self.server.clients, {})
        self.directory.cleanup()

    async def request(self, method, path, body=b"", token=None, declared_length=None):
        reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
        headers = {"Host": "localhost", "Content-Type": "application/zip",
                   "Content-Length": str(len(body) if declared_length is None else declared_length)}
        if token:
            headers["Authorization"] = "Bearer " + token
        writer.write((method + " " + path + " HTTP/1.1\r\n" +
                      "".join(k + ": " + v + "\r\n" for k, v in headers.items()) + "\r\n").encode() + body)
        await writer.drain()
        raw = await asyncio.wait_for(reader.read(), 5)
        writer.close(); await writer.wait_closed()
        head, data = raw.split(b"\r\n\r\n", 1)
        status = int(head.split(b" ", 2)[1])
        return status, head, data

    async def test_upload_catalog_metadata_and_exact_download(self):
        raw = fixture_package()
        status, _, data = await self.request("POST", "/melee/api/mods", raw, "local-test-token")
        self.assertEqual(status, 201)
        metadata = json.loads(data); digest = metadata["sha256"]
        self.assertEqual(digest, hashlib.sha256(raw).hexdigest())
        status, _, data = await self.request("GET", "/api/mods")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(data)["mods"], [metadata])
        status, _, data = await self.request("GET", "/api/mods/" + digest)
        self.assertEqual(json.loads(data), metadata)
        status, headers, data = await self.request("GET", metadata["download_path"])
        self.assertEqual(status, 200); self.assertEqual(data, raw)
        self.assertIn(('ETag: "' + digest + '"').encode(), headers)
        self.assertEqual((await self.request("POST", "/api/mods", raw, "local-test-token"))[0], 200)

    async def test_preview_serves_exact_validated_png_and_rejects_tampered_package(self):
        import base64
        png = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a84sAAAAASUVORK5CYII=")
        metadata, _ = self.repo.publish(fixture_package(extra={"preview.png": png}))
        status, headers, body = await self.request("GET", metadata["preview_path"])
        self.assertEqual(status, 200)
        self.assertEqual(body, png)
        self.assertIn(b"Content-Type: image/png", headers)
        self.assertEqual(metadata["preview_sha256"], hashlib.sha256(png).hexdigest())
        status, _, body = await self.request("HEAD", metadata["preview_path"])
        self.assertEqual(status, 200)
        self.assertEqual(body, b"")
        digest = metadata["sha256"]
        preview_cache = self.repo.root / (digest + ".preview.png")
        self.assertTrue(preview_cache.exists())
        preview_cache.unlink()
        status2, _, body2 = await self.request("GET", metadata["preview_path"])
        self.assertEqual(status2, 200)
        self.assertEqual(body2, png)
        self.assertTrue(preview_cache.exists())
        preview_cache.unlink()
        self.repo.package_path(digest).write_bytes(b"corrupt")
        self.assertEqual((await self.request("GET", metadata["preview_path"]))[0], 503)
        self.assertIsNone(self.repo.get(digest))
        plain, _ = self.repo.publish(fixture_package())
        self.assertNotIn("preview_path", plain)
        self.assertEqual((await self.request("GET", "/api/mods/" + plain["sha256"] + "/preview.png"))[0], 404)

    async def test_unauthorized_invalid_and_oversized_uploads_do_not_publish(self):
        self.assertEqual((await self.request("POST", "/api/mods", fixture_package()))[0], 401)
        self.assertEqual((await self.request("POST", "/api/mods", b"invalid", "local-test-token"))[0], 400)
        self.assertEqual((await self.request("POST", "/api/mods", b"", "local-test-token", MAX_PACKAGE_BYTES + 1))[0], 413)
        self.assertEqual(self.repo.catalog()["mods"], [])
        self.assertEqual((await self.request("GET", "/api/mods/" + "f" * 64))[0], 404)

    async def test_uploads_disabled_without_token_but_catalog_stays_available(self):
        self.server.upload_token = ""
        self.assertEqual((await self.request("POST", "/api/mods", fixture_package()))[0], 503)
        self.assertEqual((await self.request("GET", "/api/mods"))[0], 200)


if __name__ == "__main__":
    unittest.main()
