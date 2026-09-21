"""Exact runtime/base-data identity gates before membership and session start."""
import hashlib
import unittest

from melee_netplay_server import Client, Server, clean_compatibility
from test_netplay_server import RecordingWriter


def identity(runtime="1", game="2"):
    runtime, game = runtime * 64, game * 64
    digest = hashlib.sha256(b"MeleePC-compatibility-v1\0" + bytes.fromhex(runtime) + bytes.fromhex(game)).hexdigest()
    return dict(schema=1, fingerprint=digest, runtime=runtime, game=game)


class CompatibilityTests(unittest.TestCase):
    def setUp(self):
        self.server = Server()
        self.host = self.client(identity())
        self.command(self.host, "create", name="Verified room")
        self.room = self.host.room

    def command(self, client, op, **fields):
        self.server.handle(client, dict(op=op, **fields))

    def client(self, compatibility):
        client = Client(self.server, None, RecordingWriter())
        self.server.clients[client.id] = client
        self.command(client, "hello", version=2, sync="rollback-v2", compatibility=compatibility,
                     features=["compat-v1"] if compatibility else [])
        return client

    def test_runtime_base_and_legacy_mismatch_never_change_membership(self):
        for value, text in ((identity(runtime="3"), "build"), (identity(game="4"), "data"), (None, "build")):
            with self.subTest(value=value):
                client = self.client(value)
                self.command(client, "create", name="Keep this room")
                previous = client.room
                self.command(client, "ready", ready=True)
                self.command(client, "join", room=self.room.id)
                self.assertIs(client.room, previous)
                self.assertTrue(client.ready)
                self.assertEqual(client.writer.messages[-1]["code"], "compatibility_mismatch")
                self.assertIn(text, client.writer.messages[-1]["message"])
                self.assertEqual(self.room.players, [self.host])

    def test_exact_identity_joins_and_is_repeated_in_start(self):
        guest = self.client(identity())
        self.command(guest, "join", room=self.room.id)
        self.assertIs(guest.room, self.room)
        self.assertEqual(self.room.summary()["compatibility"], identity())
        for client in (self.host, guest):
            self.command(client, "ready", ready=True)
        self.command(self.host, "start")
        self.assertNotEqual(self.room.session, 0)
        self.assertEqual(guest.writer.messages[-1]["compatibility"], identity())

    def test_identity_cannot_be_renegotiated_after_membership(self):
        self.command(self.host, "hello", version=2, sync="rollback-v2", compatibility=identity(game="4"))
        self.assertEqual(self.host.compatibility, identity())
        self.assertEqual(self.room.compatibility, identity())
        self.assertEqual(self.host.writer.messages[-1]["op"], "error")

    def test_ready_and_start_recheck_identity(self):
        guest = self.client(identity())
        self.command(guest, "join", room=self.room.id)
        guest.compatibility = identity(game="4")
        self.command(guest, "ready", ready=True)
        self.assertFalse(guest.ready)
        self.host.ready = guest.ready = True
        self.command(self.host, "start")
        self.assertEqual(self.room.session, 0)
        self.assertEqual(self.host.writer.messages[-1]["code"], "compatibility_mismatch")

    def test_legacy_rooms_only_accept_legacy_clients(self):
        first, second = self.client(None), self.client(None)
        self.command(first, "create")
        self.command(second, "join", room=first.room.id)
        self.assertIs(second.room, first.room)
        self.command(self.host, "join", room=first.room.id)
        self.assertIs(self.host.room, self.room)

    def test_feature_and_fingerprint_must_be_negotiated_together(self):
        for features, value in ((["compat-v1"], None), ([], identity())):
            client = Client(self.server, None, RecordingWriter())
            self.command(client, "hello", version=2, sync="rollback-v2", features=features, compatibility=value)
            self.assertFalse(client.compatible)
            self.assertEqual(client.writer.messages[-1]["op"], "error")

    def test_malformed_or_inconsistent_fingerprint_is_rejected(self):
        for value in ({}, {**identity(), "schema": True}, {**identity(), "runtime": "x" * 64},
                      {**identity(), "fingerprint": "0" * 64}, {**identity(), "game": []}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                clean_compatibility(value)


if __name__ == "__main__":
    unittest.main()
