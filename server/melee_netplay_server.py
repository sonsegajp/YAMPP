#!/usr/bin/env python3
"""Melee PC master server: room lobby and input relay.

Run on a VPS with Python 3.8+ and no other dependencies:

    python3 melee_netplay_server.py --port 7420

Clients speak newline-delimited JSON. A connection that opens with an HTTP
request is answered with a 101 upgrade and then speaks the same protocol, so
the server can sit behind nginx on a port that is already open rather than
needing one of its own. Match traffic can ride that same connection, so a
player behind any NAT can always get a game.

Riding the stream is the fallback, not the goal. TCP delivers in order, which
means one lost segment holds back every input queued behind it until the
retransmission lands -- a freeze followed by a burst of rollbacks, on a link
that never actually stopped working. Rollback input does not need ordering:
every packet names the frames it carries and repeats the previous few. So the
server also relays those packets over UDP when it can (--udp-port, on by
default). Clients register by sending a token they were issued over the
stream; the address the datagram arrives from is where their traffic is sent,
which keeps NAT mappings working without any port forwarding. A client that
never gets a datagram through simply stays on the stream.

Losing the stream mid-match no longer ends the game either: the player's slot
in the session is held (--hold-seconds) while they reconnect and ask to
resume, and the other players see a pause rather than a disconnect.
"""
import argparse, asyncio, base64, hashlib, hmac, json, logging, os, random, secrets, struct, time
from pathlib import Path
from urllib.parse import urlsplit

from upstream_builds import FEATURE as UPSTREAM_FEATURE, clean_build, download_offer
from profile_protocol import Profile, handle as handle_profile
from mod_repository import Repository, PackageError, HASH_RE, MAX_PACKAGE_BYTES, clean_hashes

MAGIC = 0x4D4C4E50
HEADER = struct.Struct("<IIHH")  # magic, session, from id, to id
MAX_CLIENT_ID = 0xFFFF  # relay sender/recipient are unsigned 16-bit fields
MAX_PACKET_BYTES = 1400
MAX_LINE_BYTES = 8192
MAX_WRITE_BUFFER = 256 * 1024
PKT_BYE = 2
PROTOCOL_VERSION = 2
# Bumped from rollback-v1: input packets now carry the sender's frame
# advantage, which the peers use to keep their clocks together.
SYNC_ENGINE = "rollback-v2"
# The release this server is running, which is the one its players need: a
# room only admits clients whose build fingerprint matches, so an out-of-date
# client cannot play here at all. Reported over the HTTP API so a launcher can
# update before the player finds that out from a rejection.
#
# It is a version, not a location. The client builds the download address from
# its own pinned repository, so a server -- compromised, impersonated or merely
# misconfigured -- can say "you are out of date" and nothing more.
CLIENT_RELEASE = "0.2.4"
UDP_TOKEN_CHARS = 32
# The stream is framed; a datagram is not, so it carries the token in front.
UDP_MIN_BYTES = UDP_TOKEN_CHARS + HEADER.size + 1
UDP_MAX_BYTES = UDP_TOKEN_CHARS + MAX_PACKET_BYTES
DEFAULT_HOLD_SECONDS = 40
UDP_PACKETS_PER_SECOND = 1000
UPGRADE = (b"HTTP/1.1 101 Switching Protocols\r\n"
           b"Upgrade: melee-netplay\r\nConnection: Upgrade\r\n\r\n")
log = logging.getLogger("melee-netplay")


class Client:
    def __init__(self, server, reader, writer):
        self.server, self.reader, self.writer = server, reader, writer
        self.id = server.next_client_id()
        self.name = "Player"
        self.room = None
        self.ready = False
        self.port = 0
        self.compatible = False
        self.features = set()
        self.installed_mods = ()
        self.compatibility = None
        self.upstream_build = None
        self.profile = Profile()
        # Issued over the stream, presented on every datagram. The address a
        # token arrives from is the only thing that tells us where to send
        # this client's match traffic, and it may change at any time.
        self.udp_token = secrets.token_hex(UDP_TOKEN_CHARS // 2)
        self.udp_addr = None
        # What a datagram sent straight to this client must carry in front of
        # it. Given only to the players sharing its session, and reissued for
        # every session, so leaving a match retires the right to send to it.
        self.peer_token = secrets.token_hex(UDP_TOKEN_CHARS // 2)
        # The address this client has on its own network. Two players behind
        # one router reach each other here and nowhere else, because the
        # public address of both is the same router, which will usually not
        # send a packet back in through itself.
        self.local_addr = None
        self.udp_window = 0.0
        self.udp_in_window = 0
        # Round trip this client last measured to this server. Input is
        # relayed through here, so the two clients' values are the two halves
        # of the path their inputs actually take to each other.
        self.rtt_ms = None

    def datagram_allowed(self, now):
        """Rate cap for the connectionless path.

        The stream path is bounded by the client's own write buffer; a
        datagram has no such back pressure, so the budget is explicit. A match
        sends about one packet a frame plus resends, so the cap is an order of
        magnitude above anything a real client produces.
        """
        if now - self.udp_window >= 1.0:
            self.udp_window, self.udp_in_window = now, 0
        self.udp_in_window += 1
        return self.udp_in_window <= UDP_PACKETS_PER_SECOND

    def send(self, message):
        if self.writer.is_closing():
            return
        # A stalled receiver must not accumulate every other player's inputs
        # forever. Closing it follows the ordinary room/session cleanup path.
        if self.writer.transport.get_write_buffer_size() > MAX_WRITE_BUFFER:
            self.writer.close()
            return
        self.writer.write((json.dumps(message, separators=(",", ":")) + "\n").encode())

    def public(self):
        return {"id": self.id, "name": self.name, "ready": self.ready, "port": self.port, "avatar": self.profile.hash}


class Room:
    def __init__(self, server, host, name, rules, max_players, required_mods=None):
        self.server, self.id = server, server.next_id()
        self.name, self.rules, self.max = name, rules, max_players
        self.players = [host]
        self.host = host
        self.compatibility = host.compatibility
        self.upstream_build = host.upstream_build
        self.session = 0
        # Slots kept open for players whose connection dropped mid-match. Keyed
        # by port; each value is the deadline and the state needed to seat the
        # same player again without disturbing the others.
        self.holds = {}
        # Detailed costume lists stay on the HTTP API; lobby lines must fit the
        # native client's 64 KiB receive buffer even for multi-pack rooms.
        fields = ("sha256", "id", "name", "version", "kind", "base", "additive", "size", "download_path")
        self.required_mods = [{key: mod[key] for key in fields} for mod in (required_mods or [])]

    def summary(self):
        return {"id": self.id, "name": self.name, "host": self.host.name, "players": len(self.players),
                "max": self.max, "state": 1 if self.session else 0, "rules": self.rules,
                "required_mods": self.required_mods, "compatibility": self.compatibility, "upstream_build": self.upstream_build}

    def detail(self):
        return {"id": self.id, "name": self.name, "host_id": self.host.id, "rules": self.rules,
                "players": [p.public() for p in self.players], "required_mods": self.required_mods, "compatibility": self.compatibility, "upstream_build": self.upstream_build}

    def broadcast(self):
        for p in self.players:
            p.send({"op": "room", "room": self.detail()})


# delay 0 in the room rules means automatic; the concrete number is resolved
# here, once, and sent to both clients in the same start message. It has to be
# one number: a match seeds the frames before the delay as known-empty input
# on every port, so two clients disagreeing about where that boundary is would
# contradict each other on the first frame someone held a direction.
DEFAULT_RULES = {"mode": 1, "stock": 4, "minutes": 8, "items": 0, "delay": 0, "pause": 1, "damage": 100, "friendly_fire": 0}


FRAME_MS = 16.683          # one 60 Hz frame, the GameCube's actual 59.94
MIN_AUTO_DELAY = 1
MAX_AUTO_DELAY = 6


def automatic_delay(players):
    """Frames of input delay that cover the one-way trip between two players.

    Their inputs travel client -> server -> client, so the one-way time is
    half of one client's round trip to us plus half of the other's. Rollback
    absorbs the jitter on top of this, so the delay only has to cover the
    steady state; anything larger is latency the players feel for nothing.

    A player who has not reported a measurement yet contributes the median of
    those who have, or the default when nobody has.
    """
    measured = [p.rtt_ms for p in players if p.rtt_ms is not None]
    if not measured:
        return 3
    measured.sort()
    fallback = measured[len(measured) // 2]
    one_way = sum((p.rtt_ms if p.rtt_ms is not None else fallback) / 2 for p in players)
    # Two players is the ordinary case; with more, the pair that is furthest
    # apart is the one the delay has to cover.
    if len(players) > 2:
        halves = sorted((p.rtt_ms if p.rtt_ms is not None else fallback) / 2 for p in players)
        one_way = halves[-1] + halves[-2]
    frames = int(-(-one_way // FRAME_MS))
    return max(MIN_AUTO_DELAY, min(MAX_AUTO_DELAY, frames))


def clean_endpoint(text):
    """`text` if it is a bare numeric IPv4 address and port, else None.

    A client sends this about itself and it is forwarded to its opponent, so
    it is checked here rather than trusted: anything that is not four numbers
    and a port is not an address the other client will dial, and a name would
    turn into a lookup on someone else's machine.
    """
    if not isinstance(text, str) or len(text) > 32:
        return None
    host, _, port = text.rpartition(":")
    parts = host.split(".")
    if len(parts) != 4 or not port.isdigit() or not 0 < int(port) <= 65535:
        return None
    for part in parts:
        if not part.isdigit() or len(part) > 3 or int(part) > 255:
            return None
    return text


def clean_rules(rules):
    out = dict(DEFAULT_RULES)
    if isinstance(rules, dict):
        for key in out:
            value = rules.get(key)
            if isinstance(value, bool):
                value = int(value)
            if isinstance(value, int):
                out[key] = value
    out["mode"] = 1 if out["mode"] else 0
    out["stock"] = min(99, max(1, out["stock"]))
    out["minutes"] = min(99, max(0, out["minutes"]))
    out["items"] = min(5, max(0, out["items"]))
    out["delay"] = min(10, max(0, out["delay"]))
    out["damage"] = min(200, max(50, out["damage"]))
    out["pause"] = 1 if out["pause"] else 0
    out["friendly_fire"] = 1 if out["friendly_fire"] else 0
    return out


def clean_compatibility(value):
    if value is None:
        return None
    if not isinstance(value, dict) or type(value.get("schema")) is not int or value.get("schema") != 1:
        raise ValueError("Invalid game compatibility fingerprint")
    keys = ("fingerprint", "runtime", "game")
    if any(not isinstance(value.get(key), str) or not HASH_RE.fullmatch(value[key]) for key in keys):
        raise ValueError("Invalid game compatibility fingerprint")
    combined = hashlib.sha256(b"MeleePC-compatibility-v1\0" + bytes.fromhex(value["runtime"]) + bytes.fromhex(value["game"])).hexdigest()
    if combined != value["fingerprint"]:
        raise ValueError("Inconsistent game compatibility fingerprint")
    return {"schema": 1, **{key: value[key] for key in keys}}


def compatibility_error(client, room):
    if client.upstream_build != room.upstream_build:
        return "Use the same upstream build before joining this room"
    if client.compatibility == room.compatibility:
        return None
    if (not client.compatibility or not room.compatibility
            or client.compatibility["runtime"] != room.compatibility["runtime"]):
        return "Update to the same game build before joining this room"
    return "Base game data differs. Use the same original game data before joining"


class Server:
    def __init__(self, repository=None, mod_api="", upload_token=None):
        self.repository = repository
        self.mod_api = mod_api
        self.upload_token = upload_token if upload_token is not None else os.environ.get("MELEE_MOD_UPLOAD_TOKEN", "")
        self.upload_slots = None
        self.counter = 100
        self.client_counter = 100
        self.clients = {}
        self.rooms = {}
        self.sessions = {}    # session -> room
        self.by_token = {}    # udp token -> client
        self.udp = None       # DatagramTransport once the side channel is up
        self.udp_port = 0     # advertised to clients; 0 disables the channel
        self.hold_seconds = DEFAULT_HOLD_SECONDS
        # Lets a co-located practice peer ask which build the players are on.
        self.admin_token = os.environ.get("MELEE_NETPLAY_ADMIN_TOKEN", "")

    def next_id(self):
        while True:
            self.counter = self.counter % 0x7FFFFFFF + 1
            if self.counter not in self.rooms and self.counter not in self.sessions:
                return self.counter

    def next_client_id(self):
        for _ in range(MAX_CLIENT_ID):
            self.client_counter = self.client_counter % MAX_CLIENT_ID + 1
            if self.client_counter not in self.clients:
                return self.client_counter
        raise ConnectionError("The server is full")

    def room_list(self):
        result = {"op": "rooms", "rooms": []}
        # A room whose players are all inside their reconnect window still
        # exists and can still be rejoined, but there is nothing there for
        # anyone else to join, so it is not offered in the lobby.
        for room in self.rooms.values():
            if not room.players:
                continue
            summary = room.summary()
            if len(result["rooms"]) >= 32:
                result["more"] = True
                break
            result["rooms"].append(summary)
            if len(json.dumps(result, separators=(",", ":")).encode()) > 60000:
                result["rooms"].pop()
                result["more"] = True
                break
        return result

    def broadcast_rooms(self):
        message = self.room_list()
        for c in self.clients.values():
            if c.compatible and c.room is None:
                c.send(message)

    def end_session(self, room, reason):
        if not room.session:
            return
        self.sessions.pop(room.session, None)
        room.session = 0
        room.holds.clear()
        for player in room.players:
            player.ready = False
            player.send({"op": "ended", "reason": reason})

    def expire_holds(self, now=None):
        """Give up on players whose reconnect window has run out."""
        now = time.monotonic() if now is None else now
        for room in list(self.rooms.values()):
            expired = [port for port, hold in room.holds.items() if hold["until"] <= now]
            for port in expired:
                room.holds.pop(port, None)
                log.info("room %d: held slot %d expired", room.id, port + 1)
            if expired and room.session:
                self.end_session(room, "A player did not come back")
                if room.players and room.host not in room.players:
                    room.host = room.players[0]
                for i, player in enumerate(room.players):
                    player.port = i
                room.broadcast()
                self.broadcast_rooms()
            if not room.players and not room.holds:
                self.rooms.pop(room.id, None)

    def hold_slot(self, client):
        """Keep a disconnected player's place in a live match.

        A dropped TCP connection and a player walking away look identical from
        here, and the difference matters enormously to the other player: one
        deserves a few seconds of held match, the other does not deserve to
        have the game silently continue. Holding is the recoverable reading,
        and it is bounded, so being wrong costs a short pause and nothing
        else. The session, its id and everyone else's state are untouched.
        """
        room = client.room
        if room is None or not room.session:
            return False
        room.holds[client.port] = {
            "until": time.monotonic() + self.hold_seconds,
            "name": client.name,
            "compatibility": client.compatibility,
            "upstream_build": client.upstream_build,
            "installed_mods": client.installed_mods,
            "features": client.features,
            # A deployment may run without profile sharing, so the hold keeps
            # whatever optional per-client state exists rather than assuming it.
            "profile": getattr(client, "profile", None),
            "old_id": client.id,
        }
        room.players.remove(client)
        client.room, client.ready = None, False
        log.info("room %d: holding slot %d for %s for %ds",
                 room.id, client.port + 1, client.name, self.hold_seconds)
        for player in room.players:
            player.send({"op": "held", "session": room.session, "port": client.port,
                         "seconds": self.hold_seconds})
        return True

    def resume_session(self, client, session):
        """Seat a reconnecting player back in the match they were holding."""
        self.expire_holds()
        room = self.sessions.get(session)
        if room is None or not room.holds:
            client.send({"op": "resumed", "ok": 0, "message": "That match is no longer running"})
            return
        port = next((p for p, hold in room.holds.items()
                     if hold["name"] == client.name and hold["compatibility"] == client.compatibility), None)
        if port is None:
            client.send({"op": "resumed", "ok": 0, "message": "No held place in that match"})
            return
        hold = room.holds.pop(port)
        if client.room is not None:
            self.leave_room(client)
        client.room, client.port, client.ready = room, port, True
        client.installed_mods = hold["installed_mods"]
        if hold["profile"] is not None:
            client.profile = hold["profile"]
        room.players.append(client)
        room.players.sort(key=lambda pl: pl.port)
        if room.host not in room.players:
            room.host = room.players[0]
        message = {"op": "resumed", "ok": 1, "session": session, "id": client.id, "port": port,
                   "players": [{"id": pl.id, "port": pl.port} for pl in room.players],
                   "udp_port": self.udp_port, "udp_token": client.udp_token}
        for player in room.players:
            player.send(message)
        # The rejoining client has a new id and a new socket, so the address
        # and token its opponent had for it are both stale.
        self.announce_peers(room)
        log.info("room %d: %s rejoined session %d on port %d",
                 room.id, client.name, session, port + 1)

    def leave_room(self, client, reason="left"):
        room = client.room
        if room is None:
            return
        client.room, client.ready = None, False
        client.installed_mods = ()
        room.players.remove(client)
        self.end_session(room, reason)
        if not room.players:
            self.rooms.pop(room.id, None)
        else:
            if room.host is client:
                room.host = room.players[0]
            for i, p in enumerate(room.players):
                p.port = i
                p.ready = False
            room.broadcast()
        client.send({"op": "room", "room": None})
        client.send(self.room_list())
        self.broadcast_rooms()

    @staticmethod
    def mods_confirmed(client, room):
        hashes = tuple(m["sha256"] for m in room.required_mods)
        return client.installed_mods == hashes and (not hashes or "mods-v1" in client.features)

    def handle(self, client, message):
        op = message.get("op")
        if op != "hello" and not client.compatible:
            client.send({"op": "error", "message": "Send a compatible rollback hello before using netplay"})
            return
        if client.compatible and handle_profile(self, client, message):
            return
        if op == "hello":
            if (type(message.get("version")) is not int or message.get("version") != PROTOCOL_VERSION
                    or message.get("sync") != SYNC_ENGINE):
                client.send({"op": "error", "message": "Incompatible netplay version. Update to the rollback build"})
                client.writer.close()
                return
            features = message.get("features", [])
            if (not isinstance(features, list) or len(features) > 16
                    or any(not isinstance(f, str) or len(f) > 32 for f in features)):
                client.send({"op": "error", "message": "Invalid client feature list"})
                client.writer.close()
                return
            if client.compatible and client.room:
                client.send({"op": "error", "message": "Leave the room before renegotiating client features"})
                return
            try:
                compatibility = clean_compatibility(message.get("compatibility"))
                upstream_build = clean_build(message.get("upstream_build"))
                if upstream_build is not None and (UPSTREAM_FEATURE not in features or compatibility is None):
                    raise ValueError("Upstream rooms require build negotiation and a verified game fingerprint")
                if ("compat-v1" in features) != (compatibility is not None):
                    raise ValueError("The compatibility feature requires a verified game fingerprint")
            except ValueError as exc:
                client.send({"op": "error", "message": str(exc)})
                client.writer.close()
                return
            client.compatibility = compatibility
            client.upstream_build = upstream_build
            client.features = set(features)
            client.compatible = True
            client.name = str(message.get("name", "Player"))[:31] or "Player"
            self.by_token[client.udp_token] = client
            client.send({"op": "welcome", "id": client.id, "version": PROTOCOL_VERSION, "sync": SYNC_ENGINE,
                         "features": ["compat-v1", "profile-v1", UPSTREAM_FEATURE] + (["mods-v1"] if self.repository else []),
                         "mod_api": self.mod_api,
                         "udp_port": self.udp_port, "udp_token": client.udp_token})
            client.send(self.room_list())
        elif op == "ping":
            reported = message.get("rtt")
            if type(reported) is int and 0 <= reported <= 10000:
                client.rtt_ms = reported
            local = message.get("local")
            if local is None or clean_endpoint(local) is not None:
                # Held as the client wrote it, and only ever handed to the
                # players in its own session. A private address is not secret,
                # but it is not something to give to the whole lobby either.
                if client.local_addr != local:
                    client.local_addr = local
                    if client.room is not None and client.room.session:
                        self.announce_peers(client.room)
            client.send({"op": "pong"})
        elif op == "list":
            client.send(self.room_list())
        elif op == "create":
            capacity = message.get("max", 2)
            if type(capacity) is not int:
                client.send({"op": "error", "message": "Room size must be a number"})
                return
            try:
                hashes = clean_hashes(message.get("required_mods", []))
                installed = clean_hashes(message.get("installed_mods", []))
                if hashes and "mods-v1" not in client.features:
                    raise PackageError("Update to a mods-v1 client before hosting a costume room")
                if hashes and not self.repository:
                    raise PackageError("This server has no costume repository")
                required = self.repository.require(hashes) if hashes else []
                if hashes != installed:
                    raise PackageError("Activate the exact required costume packs before hosting")
            except PackageError as exc:
                client.send({"op": "error", "message": str(exc)})
                return
            if client.room:
                self.leave_room(client)
            name = str(message.get("name", client.name))[:47] or client.name
            room = Room(self, client, name, clean_rules(message.get("rules")), min(4, max(2, capacity)), required)
            self.rooms[room.id] = room
            client.room, client.port, client.ready = room, 0, False
            client.installed_mods = tuple(installed)
            room.broadcast()
            self.broadcast_rooms()
            log.info("room %d '%s' created by %s", room.id, name, client.name)
        elif op == "join":
            room_id = message.get("room", 0)
            if type(room_id) is not int:
                client.send({"op": "error", "message": "Room ID must be a number"})
                return
            room = self.rooms.get(room_id)
            if room is None:
                client.send({"op": "error", "message": "That room no longer exists"})
            elif client.room is room:
                # Repeated join commands are harmless, including a full room.
                client.send({"op": "room", "room": room.detail()})
            elif room.session:
                client.send({"op": "error", "message": "That room is already playing"})
            elif len(room.players) >= room.max:
                client.send({"op": "error", "message": "That room is full"})
            elif client.upstream_build != room.upstream_build:
                if UPSTREAM_FEATURE not in client.features:
                    client.send({"op": "error", "code": "upstream_update_required",
                                 "message": "This room uses Akaneia. Update YAMPP for the GitHub download prompt"})
                else:
                    # Metadata only. No URLs from the host, mirrored assets, or
                    # implicit join: the client must ask for download consent.
                    client.send({"op": "build_required", "room": room.id,
                                 "build": download_offer(room.upstream_build) if room.upstream_build else None})
            elif compatibility_error(client, room):
                reason = compatibility_error(client, room)
                log.warning("join rejected client=%d room=%d reason=%s client_compat=%s host_compat=%s",
                            client.id, room.id, reason, client.compatibility, room.compatibility)
                client.send({"op": "error", "code": "compatibility_mismatch", "message": reason})
            else:
                try:
                    installed = clean_hashes(message.get("installed_mods", []))
                except PackageError as exc:
                    client.send({"op": "error", "message": str(exc)})
                    return
                required_hashes = sorted(m["sha256"] for m in room.required_mods)
                if required_hashes and "mods-v1" not in client.features:
                    client.send({"op": "error", "message": "Update to a mods-v1 client before joining a costume room"})
                    return
                if installed != required_hashes:
                    client.send({"op": "mods_required", "room": room.id, "mods": room.required_mods})
                    return
                if client.room:
                    self.leave_room(client)
                client.room, client.port, client.ready = room, len(room.players), False
                client.installed_mods = tuple(installed)
                room.players.append(client)
                room.broadcast()
                self.broadcast_rooms()
                log.info("%s joined room %d", client.name, room.id)
        elif op == "leave":
            self.leave_room(client)
        elif op == "ready":
            ready = message.get("ready")
            if type(ready) not in (bool, int) or ready not in (0, 1):
                client.send({"op": "error", "message": "Ready must be true or false"})
                return
            if client.room and not client.room.session:
                if ready and compatibility_error(client, client.room):
                    client.send({"op": "error", "code": "compatibility_mismatch", "message": compatibility_error(client, client.room)})
                    return
                if ready and not self.mods_confirmed(client, client.room):
                    client.send({"op": "error", "message": "Verify the room costume packs before Ready"})
                    return
                client.ready = bool(ready)
                client.room.broadcast()
        elif op == "rules":
            room = client.room
            if room and room.host is client and not room.session:
                rules = clean_rules(message.get("rules"))
                if rules != room.rules:
                    room.rules = rules
                    for player in room.players:
                        player.ready = False
                    self.broadcast_rooms()
                room.broadcast()
        elif op == "peers":
            # A practice peer cannot invent a game fingerprint: it is a hash
            # of real bytes it does not have. Rather than guess, it asks what
            # the players actually connected right now are using, so a room
            # it puts up is one they can actually join. Restricted to a
            # shared secret because it reports what builds are in use.
            token = message.get("token")
            if not self.admin_token or not isinstance(token, str) or not hmac.compare_digest(token, self.admin_token):
                client.send({"op": "error", "message": "Not permitted"})
                return
            counts = {}
            for other in self.clients.values():
                if other is client or not other.compatible or not other.compatibility:
                    continue
                key = other.compatibility["fingerprint"]
                counts[key] = counts.get(key, (0, other.compatibility))
                counts[key] = (counts[key][0] + 1, other.compatibility)
            best = max(counts.values(), key=lambda entry: entry[0], default=None)
            client.send({"op": "peers", "players": sum(n for n, _ in counts.values()),
                         "compatibility": best[1] if best else None})
        elif op == "resume":
            session = message.get("session")
            if type(session) is not int:
                client.send({"op": "resumed", "ok": 0, "message": "Invalid session"})
            else:
                self.resume_session(client, session)
        elif op == "r":
            self.relay(client, message.get("d", ""))
        elif op == "start":
            room = client.room
            if room is None or room.host is not client:
                client.send({"op": "error", "message": "Only the host can start"})
            elif room.session:
                client.send({"op": "error", "message": "That room is already playing"})
            elif any(compatibility_error(p, room) for p in room.players):
                client.send({"op": "error", "code": "compatibility_mismatch", "message": "Every player must use the same game build and base data"})
            elif any(not self.mods_confirmed(p, room) for p in room.players):
                client.send({"op": "error", "message": "Every player must verify the exact room costume packs"})
            elif len(room.players) < 2 or not all(p.ready for p in room.players):
                client.send({"op": "error", "message": "Everyone must be ready first"})
            else:
                room.session = self.next_id()
                room.holds.clear()
                self.sessions[room.session] = room
                # One delay for everyone, resolved here when the room asked
                # for automatic. `rules` still reports 0 so the lobby keeps
                # showing Auto; `delay` is what the match runs on.
                delay = room.rules["delay"] or automatic_delay(room.players)
                start = {"op": "start", "version": PROTOCOL_VERSION, "sync": SYNC_ENGINE, "session": room.session, "seed": random.getrandbits(31) | 1, "delay": delay,
                         "rules": room.rules, "players": [{"id": p.id, "port": p.port} for p in room.players],
                         "required_mods": room.required_mods, "compatibility": room.compatibility, "upstream_build": room.upstream_build}
                for p in room.players:
                    # A fresh token for every session: the right to send
                    # straight to a player lasts exactly as long as the match
                    # they agreed to, and a past opponent keeps nothing.
                    p.peer_token = secrets.token_hex(UDP_TOKEN_CHARS // 2)
                for p in room.players:
                    p.send(start)
                self.announce_peers(room)
                self.broadcast_rooms()
                log.info("session %d started in room %d, delay %d%s", room.session, room.id,
                         delay, " (automatic)" if not room.rules["delay"] else "")

    async def http_json(self, writer, status, value, head=False):
        body = json.dumps(value, separators=(",", ":")).encode()
        reasons = {200: "OK", 201: "Created", 400: "Bad Request", 401: "Unauthorized",
                   404: "Not Found", 405: "Method Not Allowed", 411: "Length Required",
                   413: "Content Too Large", 415: "Unsupported Media Type", 503: "Unavailable"}
        writer.write(("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                      "Content-Length: %d\r\nCache-Control: no-store\r\n"
                      "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n"
                      % (status, reasons.get(status, "Error"), len(body))).encode())
        if not head:
            writer.write(body)
        await writer.drain()

    async def serve_http(self, first, headers, reader, writer):
        try:
            method, target, version = first.decode("ascii").strip().split(" ")
            path = urlsplit(target).path
        except (ValueError, UnicodeDecodeError):
            return await self.http_json(writer, 400, {"error": "Invalid HTTP request"})
        if path.startswith("/melee/api/"):
            path = path[len("/melee/api"):]
        elif path.startswith("/api/"):
            path = path[len("/api"):]
        else:
            return await self.http_json(writer, 404, {"error": "Unknown API route"})
        if path == "/version" and method in ("GET", "HEAD"):
            # Answered before anything optional, because a client that needs
            # updating is exactly the client whose other requests will fail.
            return await self.http_json(writer, 200, {
                "schema": 1, "release": CLIENT_RELEASE,
                "protocol": PROTOCOL_VERSION, "sync": SYNC_ENGINE,
            }, method == "HEAD")
        if not self.repository:
            return await self.http_json(writer, 503, {"error": "Costume repository is unavailable"})
        if path == "/mods" and method in ("GET", "HEAD"):
            return await self.http_json(writer, 200, self.repository.catalog(), method == "HEAD")
        if path == "/mods" and method == "POST":
            if not self.upload_token:
                return await self.http_json(writer, 503, {"error": "Publishing has not been configured"})
            supplied = headers.get("authorization", "")
            expected = "Bearer " + self.upload_token
            if not hmac.compare_digest(supplied.encode(), expected.encode()):
                return await self.http_json(writer, 401, {"error": "A valid publisher token is required"})
            if headers.get("content-type", "").split(";", 1)[0].strip().lower() != "application/zip":
                return await self.http_json(writer, 415, {"error": "Upload a ZIP with application/zip content type"})
            if "transfer-encoding" in headers:
                return await self.http_json(writer, 400, {"error": "Use a Content-Length upload"})
            length_text = headers.get("content-length", "")
            if not length_text.isdigit():
                return await self.http_json(writer, 411, {"error": "Content-Length is required"})
            length = int(length_text)
            if not 0 < length <= MAX_PACKAGE_BYTES:
                return await self.http_json(writer, 413, {"error": "Package exceeds the 64 MiB upload limit"})
            if self.upload_slots is None:
                self.upload_slots = asyncio.Semaphore(2)
            async with self.upload_slots:
                raw = await asyncio.wait_for(reader.readexactly(length), timeout=120)
                try:
                    metadata, created = await asyncio.get_running_loop().run_in_executor(
                        None, self.repository.publish, raw)
                except PackageError as exc:
                    return await self.http_json(writer, 400, {"error": str(exc)})
                except OSError:
                    log.exception("Could not store a validated costume package")
                    return await self.http_json(writer, 503, {"error": "Package storage is unavailable"})
            return await self.http_json(writer, 201 if created else 200, metadata)
        parts = path.strip("/").split("/")
        if len(parts) in (2, 3) and parts[0] == "mods" and HASH_RE.fullmatch(parts[1]):
            metadata = self.repository.get(parts[1])
            if not metadata or (len(parts) == 3 and parts[2] not in ("package.zip", "preview.png")):
                return await self.http_json(writer, 404, {"error": "Costume package was not found"})
            if method not in ("GET", "HEAD"):
                return await self.http_json(writer, 405, {"error": "Method not allowed"})
            if len(parts) == 2:
                return await self.http_json(writer, 200, metadata, method == "HEAD")
            if parts[2] == "preview.png":
                try:
                    preview = await asyncio.get_running_loop().run_in_executor(None, self.repository.preview_bytes, parts[1])
                except (OSError, PackageError):
                    return await self.http_json(writer, 503, {"error": "Costume preview is unavailable"})
                if preview is None:
                    return await self.http_json(writer, 404, {"error": "This costume pack has no preview"})
                writer.write(("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %d\r\n"
                              "ETag: \"%s\"\r\nCache-Control: public, max-age=31536000, immutable\r\n"
                              "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n"
                              % (len(preview), hashlib.sha256(preview).hexdigest())).encode())
                if method != "HEAD":
                    writer.write(preview)
                await writer.drain()
                return
            try:
                stream, length = self.repository.verified_package_stream(parts[1])
            except OSError:
                return await self.http_json(writer, 503, {"error": "Package storage is unavailable"})
            if stream is None:
                return await self.http_json(writer, 404, {"error": "Costume package was not found"})
            try:
                writer.write(("HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n"
                              "Content-Length: %d\r\nETag: \"%s\"\r\n"
                              "Cache-Control: public, max-age=31536000, immutable\r\n"
                              "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n"
                              % (length, parts[1])).encode())
                if method != "HEAD":
                    while True:
                        chunk = stream.read(64 * 1024)
                        if not chunk:
                            break
                        writer.write(chunk)
                        await writer.drain()
                await writer.drain()
            except OSError:
                pass
            finally:
                stream.close()
            return
        return await self.http_json(writer, 404, {"error": "Unknown API route"})

    async def serve_client(self, reader, writer):
        try:
            client = Client(self, reader, writer)
        except ConnectionError:
            writer.close()
            return
        self.clients[client.id] = client
        peer = writer.get_extra_info("peername")
        log.info("client %d connected from %s", client.id, peer)
        try:
            first = await asyncio.wait_for(reader.readline(), timeout=30)
            if first.startswith((b"GET ", b"POST ", b"HEAD ", b"PUT ", b"DELETE ")):
                header_bytes, headers = len(first), {}
                while True:
                    header = await asyncio.wait_for(reader.readline(), timeout=30)
                    header_bytes += len(header)
                    if header_bytes > MAX_LINE_BYTES or not header:
                        return
                    if header in (b"\r\n", b"\n"):
                        break
                    if b":" not in header:
                        return
                    key, value = header.decode("latin-1").split(":", 1)
                    key, value = key.strip().lower(), value.strip()
                    if key in headers:
                        return
                    headers[key] = value
                if headers.get("upgrade", "").lower() == "melee-netplay":
                    writer.write(UPGRADE)
                    await writer.drain()
                else:
                    await self.serve_http(first, headers, reader, writer)
                    return
            elif first.strip():
                try:
                    message = json.loads(first.decode("utf-8", "replace"))
                    if isinstance(message, dict):
                        self.handle(client, message)
                except (ValueError, RecursionError):
                    pass
            while True:
                line = await asyncio.wait_for(reader.readline(), timeout=60)
                if not line:
                    break
                try:
                    message = json.loads(line.decode("utf-8", "replace"))
                except (ValueError, RecursionError):
                    continue
                if isinstance(message, dict):
                    self.handle(client, message)
                await writer.drain()
        except (asyncio.TimeoutError, ConnectionError, asyncio.IncompleteReadError, ValueError):
            pass
        finally:
            # A drop in the middle of a match holds the slot; anything else is
            # an ordinary departure.
            if not self.hold_slot(client):
                self.leave_room(client, "disconnected")
            self.clients.pop(client.id, None)
            self.by_token.pop(client.udp_token, None)
            writer.close()
            try:
                await writer.wait_closed()
            except ConnectionError:
                pass
            log.info("client %d disconnected", client.id)

    def announce_peers(self, room):
        """Give each player in a live session the others' addresses and tokens.

        Sent when a session starts and again whenever an address is learned or
        changes, because the public one is only known once a client's first
        datagram has arrived and a NAT may renumber it at any time. A player
        whose address is not known yet is simply left out of the list and
        appears in a later one.
        """
        if not room.session:
            return
        for client in room.players:
            peers = []
            for other in room.players:
                if other is client:
                    continue
                entry = {"id": other.id, "port": other.port, "token": other.peer_token}
                if other.udp_addr is not None:
                    entry["addr"] = "%s:%d" % (other.udp_addr[0], other.udp_addr[1])
                if other.local_addr:
                    entry["local"] = other.local_addr
                if "addr" in entry or "local" in entry:
                    peers.append(entry)
            if peers:
                client.send({"op": "peers", "session": room.session,
                             "token": client.peer_token, "peers": peers})

    def forward(self, client, data, payload=None):
        """Forward one packed input packet to the rest of the session.

        `data` is the decoded packet and `payload` its base64 form when the
        caller already has one. Delivery is per recipient: whoever has proved
        a working datagram path gets a datagram, and everyone else gets the
        stream, so one player behind a UDP-hostile network does not push the
        other back onto TCP as well.
        """
        if not HEADER.size < len(data) <= MAX_PACKET_BYTES:
            return
        magic, session, sender, target = HEADER.unpack_from(data)
        room = self.sessions.get(session)
        if (magic != MAGIC or room is None or room.session != session
                or client.room is not room or sender != client.id):
            return
        for player in room.players:
            if player is client or (target and player.id != target):
                continue
            if player.udp_addr is not None and self.udp is not None:
                try:
                    self.udp.sendto(data, player.udp_addr)
                    continue
                except OSError:
                    player.udp_addr = None
            if payload is None:
                payload = base64.b64encode(data).decode()
            player.send({"op": "r", "d": payload})
        if data[HEADER.size] == PKT_BYE and len(data) == HEADER.size + 2:
            # Returning to menus sends BYE without leaving the lobby. Release
            # the session and ready flags so the same room can play again.
            self.end_session(room, "A player ended the session")
            room.broadcast()
            self.broadcast_rooms()

    def relay(self, client, payload):
        """One packet that arrived over the stream."""
        if not isinstance(payload, str) or len(payload) > ((MAX_PACKET_BYTES + 2) // 3) * 4:
            return
        try:
            data = base64.b64decode(payload, validate=True)
        except (ValueError, TypeError):
            return
        self.forward(client, data, payload)

    def relay_datagram(self, datagram, addr):
        """One packet that arrived over the side channel.

        Nothing here trusts the source address: the token decides who the
        sender is, and the address is only recorded as the place to answer.
        That is what lets a client behind a NAT keep playing when its mapping
        is renumbered mid-match, and what stops a forged source address from
        redirecting someone else's inputs.
        """
        if not UDP_MIN_BYTES <= len(datagram) <= UDP_MAX_BYTES:
            return
        token = datagram[:UDP_TOKEN_CHARS].decode("ascii", "replace")
        client = self.by_token.get(token)
        if client is None or not client.compatible:
            return
        if not client.datagram_allowed(time.monotonic()):
            return
        if client.udp_addr != addr:
            client.udp_addr = addr
            log.info("client %d reachable by datagram at %s", client.id, addr[0])
            # The address the other player needs in order to stop going
            # through here is the one that just arrived.
            if client.room is not None and client.room.session:
                self.announce_peers(client.room)
        self.forward(client, datagram[UDP_TOKEN_CHARS:])


class UdpRelay(asyncio.DatagramProtocol):
    """The unreliable half of the relay. Loss is the normal case, not an error."""

    def __init__(self, server):
        self.server = server

    def connection_made(self, transport):
        self.server.udp = transport

    def datagram_received(self, data, addr):
        try:
            self.server.relay_datagram(data, addr)
        except Exception:                      # one bad packet must not stop the relay
            log.exception("dropping malformed datagram from %s", addr[0])

    def error_received(self, exc):
        # An ICMP rejection for one recipient says nothing about the others.
        pass


async def sweep_holds(server, period=5):
    """Retire reconnect windows that have run out."""
    while True:
        await asyncio.sleep(period)
        try:
            server.expire_holds()
        except Exception:
            log.exception("hold sweep failed")


async def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=7420)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--udp-port", type=int, default=-1,
                        help="port for the unreliable input relay (default: --port; 0 disables it)")
    parser.add_argument("--advertise-udp-port", type=int, default=0,
                        help="port clients should send datagrams to, when it differs from --udp-port "
                             "because of a firewall or port forward")
    parser.add_argument("--hold-seconds", type=int, default=DEFAULT_HOLD_SECONDS,
                        help="how long a disconnected player's place in a live match is kept")
    parser.add_argument("--mods-dir", default=str(Path(__file__).resolve().parent / "packages"))
    parser.add_argument("--mod-api", default="https://mmodx.fun/melee/api")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    server = Server(Repository(args.mods_dir), args.mod_api)
    server.hold_seconds = max(0, min(300, args.hold_seconds))
    udp_port = args.port if args.udp_port < 0 else args.udp_port
    udp = None
    if udp_port:
        loop = asyncio.get_running_loop()
        try:
            udp, _ = await loop.create_datagram_endpoint(lambda: UdpRelay(server),
                                                         local_addr=(args.bind, udp_port))
            server.udp_port = args.advertise_udp_port or udp_port
            log.info("input relay also listening on %s:%d (udp, advertised as %d)",
                     args.bind, udp_port, server.udp_port)
        except OSError as exc:
            # The stream path is complete on its own; say so and carry on.
            log.warning("no datagram relay on port %d (%s); inputs will use the stream only",
                        udp_port, exc)
    tcp = await asyncio.start_server(server.serve_client, args.bind, args.port, limit=MAX_LINE_BYTES)
    log.info("Melee PC netplay server listening on %s:%d (tcp)", args.bind, args.port)
    sweeper = asyncio.ensure_future(sweep_holds(server))
    try:
        async with tcp:
            await tcp.serve_forever()
    finally:
        sweeper.cancel()
        if udp is not None:
            udp.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
