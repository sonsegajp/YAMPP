"""Small lobby portraits, shared only with current room peers.

Canonical RGBA pixels avoid remote image parsing and metadata exposure. Each
connection owns one portrait and one bounded in-progress upload. Nothing is
written to disk, uploaded to the Workshop, or put in match relay packets.
"""
import base64
import binascii
import hashlib
import re
import time

SIDE = 96
SIZE = SIDE * SIDE * 4
CHUNK = 2048
PARTS = SIZE // CHUNK
HASH = re.compile(r"[0-9a-f]{64}\Z")
NAME = re.compile(r"[^A-Za-z0-9 ._\[\]/:-]")


class Profile:
    def __init__(self):
        self.hash = ""
        self.pixels = b""
        self.pending_hash = ""
        self.pending_name = ""
        self.pending = bytearray()
        self.started = 0.
        self.changes = []
        self.requests = []

    @staticmethod
    def allow(history, limit, now):
        history[:] = [stamp for stamp in history if now - stamp < 10.]
        if len(history) >= limit:
            return False
        history.append(now)
        return True

    def reset_upload(self):
        self.pending_hash = self.pending_name = ""
        self.pending.clear()


def update_room(server, client):
    if client.room:
        client.room.broadcast()
        server.broadcast_rooms()


def handle(server, client, message):
    op = message.get("op")
    if op not in ("profile", "avatar_part", "avatar_get"):
        return False
    if "profile-v1" not in client.features or (client.room and client.room.session):
        return True
    profile = client.profile
    now = time.monotonic()
    if profile.pending_hash and now - profile.started > 10.:
        profile.reset_upload()
    if op == "profile":
        name, photo = message.get("name"), message.get("avatar")
        if (not isinstance(name, str) or not isinstance(photo, str)
                or len(name) > 124 or (photo and not HASH.fullmatch(photo))
                or not profile.allow(profile.changes, 4, now)):
            return True
        name = NAME.sub("", name)[:31].strip() or "Player"
        profile.reset_upload()
        if not photo:
            client.name = name
            profile.hash, profile.pixels = "", b""
            update_room(server, client)
        else:
            profile.pending_name, profile.pending_hash = name, photo
            profile.started = now
    elif op == "avatar_part":
        photo, part, data = message.get("hash"), message.get("part"), message.get("data")
        if (not profile.pending_hash or photo != profile.pending_hash
                or type(part) is not int or part != len(profile.pending) // CHUNK
                or not 0 <= part < PARTS or not isinstance(data, str) or len(data) != 2732):
            return True
        try:
            chunk = base64.b64decode(data, validate=True)
        except (ValueError, binascii.Error):
            return True
        if len(chunk) != CHUNK:
            return True
        profile.pending.extend(chunk)
        if len(profile.pending) == SIZE:
            pixels = bytes(profile.pending)
            if hashlib.sha256(pixels).hexdigest() == profile.pending_hash:
                profile.hash, profile.pixels = profile.pending_hash, pixels
                client.name = profile.pending_name
                update_room(server, client)
            profile.reset_upload()
    else:
        ident, photo = message.get("id"), message.get("hash")
        if (not client.room or type(ident) is not int or not isinstance(photo, str)
                or not HASH.fullmatch(photo) or not profile.allow(profile.requests, 16, now)):
            return True
        target = next((peer for peer in client.room.players if peer.id == ident), None)
        if not target or target.profile.hash != photo or len(target.profile.pixels) != SIZE:
            return True
        for part in range(PARTS):
            data = target.profile.pixels[part*CHUNK:(part+1)*CHUNK]
            client.send({"op": "avatar_part", "id": ident, "hash": photo, "part": part,
                         "data": base64.b64encode(data).decode("ascii")})
    return True
