"""Room negotiation keeps upstream mods on GitHub and asks before acquisition."""
import hashlib
import unittest
from melee_netplay_server import Client, Server, PROTOCOL_VERSION, SYNC_ENGINE
from test_netplay_server import RecordingWriter
from upstream_builds import BUILDS, FEATURE, identity, clean_build, download_offer


def fingerprint(seed):
    runtime = hashlib.sha256(b"runtime").hexdigest()
    game = hashlib.sha256(seed.encode()).hexdigest()
    joined = hashlib.sha256(b"MeleePC-compatibility-v1\0" + bytes.fromhex(runtime) + bytes.fromhex(game)).hexdigest()
    return {"schema": 1, "runtime": runtime, "game": game, "fingerprint": joined}


class UpstreamRooms(unittest.TestCase):
    def setUp(self): self.server = Server(); self.build = identity(BUILDS[0])
    def client(self, name, build=None, modern=True):
        c = Client(self.server, None, RecordingWriter()); self.server.clients[c.id] = c
        self.server.handle(c, {"op":"hello", "name":name, "version":PROTOCOL_VERSION, "sync":SYNC_ENGINE,
            "features":["compat-v1"]+([FEATURE] if modern else []), "compatibility":fingerprint("akaneia" if build else "stock"), "upstream_build":build})
        return c
    def host(self):
        h = self.client("Host", self.build); self.server.handle(h, {"op":"create", "name":"Akaneia room"});return h
    def test_missing_build_offered_before_data_mismatch(self):
        h=self.host(); c=self.client("Guest")
        self.server.handle(c,{"op":"join","room":h.room.id})
        message=c.writer.messages[-1]
        self.assertEqual(message,{"op":"build_required","room":h.room.id,"build":download_offer(self.build)})
        self.assertIsNone(c.room);self.assertEqual(len(h.room.players),1)
        self.assertEqual(message["build"]["repository"],"akaneia/akaneia-build")
        self.assertNotIn("download_path",message["build"])
    def test_akaneia_guest_must_confirm_disabling_for_stock_host(self):
        h=self.client("Stock");self.server.handle(h,{"op":"create","name":"Original Melee"})
        c=self.client("Guest",self.build);self.server.handle(c,{"op":"join","room":h.room.id})
        self.assertEqual(c.writer.messages[-1],{"op":"build_required","room":h.room.id,"build":None})
        self.assertIsNone(c.room)
    def test_repeat_join_still_requires_consent(self):
        h=self.host();c=self.client("Guest")
        for _ in range(3):self.server.handle(c,{"op":"join","room":h.room.id,"confirmed":True})
        self.assertIsNone(c.room);self.assertEqual(c.writer.messages[-1]["op"],"build_required")
    def test_exact_build_and_fingerprint_can_join_start_leave_rejoin(self):
        h=self.host();c=self.client("Guest",self.build);room=h.room
        self.server.handle(c,{"op":"join","room":room.id});self.assertIs(c.room,room)
        for p in (h,c):self.server.handle(p,{"op":"ready","ready":True})
        self.server.handle(h,{"op":"start"});self.assertTrue(room.session)
        self.assertEqual(c.writer.messages[-1]["upstream_build"],self.build)
        self.server.handle(c,{"op":"leave"});self.assertIsNone(c.room);self.assertFalse(room.session)
        self.server.handle(c,{"op":"join","room":room.id});self.assertIs(c.room,room)
    def test_same_label_never_bypasses_game_hash(self):
        h=self.host();c=self.client("Guest",self.build);c.compatibility=fingerprint("modified")
        self.server.handle(c,{"op":"join","room":h.room.id})
        self.assertIsNone(c.room);self.assertEqual(c.writer.messages[-1]["code"],"compatibility_mismatch")
    def test_old_client_gets_actionable_message(self):
        h=self.host();c=self.client("Guest",modern=False)
        self.server.handle(c,{"op":"join","room":h.room.id})
        self.assertIn("Akaneia",c.writer.messages[-1]["message"]);self.assertIsNone(c.room)
    def test_room_full_never_offers_pointless_download(self):
        h=self.host();c=self.client("Guest",self.build);self.server.handle(c,{"op":"join","room":h.room.id})
        extra=self.client("Third");self.server.handle(extra,{"op":"join","room":h.room.id})
        self.assertEqual(extra.writer.messages[-1]["message"],"That room is full")
    def test_unknown_version_and_injected_url_rejected(self):
        for value in ({**self.build,"version":"latest"},{**self.build,"sha256":"a"*64},{**self.build,"url":"https://example.com/mod"},[]):
            with self.assertRaises(ValueError):clean_build(value)
    def test_ready_and_start_revalidate_build(self):
        h=self.host();c=self.client("Guest",self.build);self.server.handle(c,{"op":"join","room":h.room.id})
        c.upstream_build=None;self.server.handle(c,{"op":"ready","ready":True});self.assertFalse(c.ready)
        c.ready=True;h.ready=True;self.server.handle(h,{"op":"start"});self.assertFalse(h.room.session)

if __name__=="__main__":unittest.main()
