import base64
import hashlib
import json
import unittest
from unittest.mock import patch
from melee_netplay_server import Server, Client, PROTOCOL_VERSION, SYNC_ENGINE, MAX_LINE_BYTES
from test_netplay_server import RecordingWriter
from profile_protocol import SIZE, CHUNK, PARTS

class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.server=Server()
        self.host=self.client("Host")
        self.guest=self.client("Guest")
        self.command(self.host,"create",name="Profiles",max=4)
        self.command(self.guest,"join",room=self.host.room.id)
        self.pixels=bytes(range(256))*(SIZE//256)
        self.hash=hashlib.sha256(self.pixels).hexdigest()
    def client(self,name):
        c=Client(self.server,None,RecordingWriter());self.server.clients[c.id]=c
        self.command(c,"hello",name=name,version=PROTOCOL_VERSION,sync=SYNC_ENGINE,features=["profile-v1"])
        return c
    def command(self,c,op,**kw):self.server.handle(c,{"op":op,**kw})
    def upload(self,c,name="New Host",pixels=None):
        pixels=self.pixels if pixels is None else pixels
        digest=hashlib.sha256(pixels).hexdigest()
        self.command(c,"profile",name=name,avatar=digest)
        for part in range(PARTS):self.command(c,"avatar_part",hash=digest,part=part,data=base64.b64encode(pixels[part*CHUNK:(part+1)*CHUNK]).decode())
        return digest
    def test_profile_transfer_and_room_names(self):
        self.upload(self.host)
        self.assertEqual(self.host.name,"New Host")
        self.assertEqual(self.host.public()["avatar"],self.hash)
        self.assertEqual(self.server.room_list()["rooms"][0]["host"],"New Host")
        self.guest.writer.messages.clear()
        self.command(self.guest,"avatar_get",id=self.host.id,hash=self.hash)
        replies=self.guest.writer.messages
        self.assertEqual(len(replies),PARTS)
        self.assertEqual(b"".join(base64.b64decode(m["data"]) for m in replies),self.pixels)
        self.assertTrue(all(len(json.dumps(m).encode())<MAX_LINE_BYTES for m in replies))
    def test_default_and_ascii_name(self):
        self.upload(self.host)
        self.command(self.host,"profile",name="  <P&1>\n",avatar="")
        self.assertEqual(self.host.name,"P1")
        self.assertEqual(self.host.profile.pixels,b"")
        self.assertEqual(self.host.public()["avatar"],"")
    def test_only_current_room_can_request(self):
        self.upload(self.host);visitor=self.client("Visitor");visitor.writer.messages.clear()
        self.command(visitor,"avatar_get",id=self.host.id,hash=self.hash)
        self.assertEqual(visitor.writer.messages,[])
        self.guest.writer.messages.clear();self.command(self.guest,"avatar_get",id=self.host.id,hash="0"*64)
        self.assertEqual(self.guest.writer.messages,[])
    def test_match_blocks_all_profile_traffic(self):
        self.upload(self.host);self.host.room.session=123
        self.guest.writer.messages.clear();self.command(self.guest,"avatar_get",id=self.host.id,hash=self.hash)
        self.command(self.host,"profile",name="changed",avatar="")
        self.assertEqual(self.guest.writer.messages,[])
        self.assertEqual(self.host.name,"New Host")
    def test_incomplete_and_corrupt_upload_keep_previous_profile(self):
        self.upload(self.host)
        self.command(self.host,"profile",name="Corrupt",avatar="0"*64)
        for part in range(PARTS):self.command(self.host,"avatar_part",hash="0"*64,part=part,data=base64.b64encode(self.pixels[part*CHUNK:(part+1)*CHUNK]).decode())
        self.assertEqual(self.host.profile.hash,self.hash);self.assertEqual(self.host.name,"New Host")
        self.assertEqual(self.host.profile.pending,b"")
    def test_bad_parts_cannot_allocate_or_advance(self):
        self.command(self.host,"profile",name="X",avatar=self.hash)
        data=base64.b64encode(self.pixels[:CHUNK]).decode()
        for part,b64 in ((-1,data),(PARTS,data),(True,data),(0,"!"*2732),(0,data+"AAAA"),(0,"AAAA")):
            self.command(self.host,"avatar_part",hash=self.hash,part=part,data=b64)
        self.assertEqual(self.host.profile.pending,b"")
        self.command(self.host,"avatar_part",hash=self.hash,part=0,data=data)
        self.command(self.host,"avatar_part",hash=self.hash,part=0,data=data)
        self.assertEqual(len(self.host.profile.pending),CHUNK)
    def test_upload_timeout_and_rate_limit(self):
        with patch("profile_protocol.time.monotonic",return_value=100.):
            self.command(self.host,"profile",name="X",avatar=self.hash)
        with patch("profile_protocol.time.monotonic",return_value=111.):
            self.command(self.host,"avatar_part",hash=self.hash,part=0,data=base64.b64encode(self.pixels[:CHUNK]).decode())
            self.assertEqual(self.host.profile.pending,b"")
            for i in range(10):self.command(self.host,"profile",name=str(i),avatar="")
            self.assertEqual(self.host.name,"3")
    def test_old_clients_keep_working_and_cannot_upload(self):
        self.host.features.clear();self.upload(self.host)
        self.assertEqual(self.host.name,"Host");self.assertEqual(self.host.profile.pixels,b"")

if __name__=="__main__":unittest.main()
