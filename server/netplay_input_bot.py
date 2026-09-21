#!/usr/bin/env python3
"""A standing practice opponent for measuring a real connection.

This is not a Melee AI and it does not simulate the game. It is a peer that
speaks the relay protocol: it answers pings, reports frame numbers, and feeds
a deterministic stream of controller inputs. The player's own machine runs the
whole simulation, exactly as it would against a person, so what gets measured
is the real thing -- round trip to the relay, the delay that is chosen from
it, rollback corrections, and the clock correction -- against an opponent
sitting next to the relay. That is the best case a remote opponent can have,
which makes it a useful floor to compare a real match against.

Two consequences follow from it not simulating anything:

  * It publishes no state hashes, sending the "no hash yet" sentinel instead,
    so desync detection stays quiet. A divergence cannot be detected here
    because there is no second simulation to diverge from.
  * The fighter it appears to control will do fixed, slightly silly things.
    It is a moving target to watch the netcode against, not a sparring
    partner.

Compatibility: the relay only lets players into a room whose game build and
data fingerprint match, and that fingerprint is a hash of real bytes this
process does not have. So by default the bot waits in the lobby and joins a
room a real player creates, adopting that room's fingerprint. Once it has
seen one it remembers it (--remember) and can then host a standing room of
its own, which is what makes a test room simply be there.

Run it next to the relay:

    python3 netplay_input_bot.py --server 127.0.0.1:7420 --name "Ping Test"
"""
import argparse
import base64
import json
import logging
import os
import random
import socket
import struct
import sys
import time

MAGIC = 0x4D4C4E50
HEADER = struct.Struct("<IIHH")          # magic, session, from id, to id
INPUT_HEADER = struct.Struct("<4B4Ib")   # type, epoch, port, count, last, ack, hash_frame, hash, advantage
NET_INPUT = struct.Struct("<H4b2B")      # buttons, sx, sy, cx, cy, lt, rt
PING = struct.Struct("<2BI")             # type, port, tick
REQUEST = struct.Struct("<BI")           # type, from frame
PKT_HELLO, PKT_INPUT, PKT_BYE, PKT_REQUEST, PKT_PING, PKT_PONG = range(6)
NO_HASH = 0xFFFFFFFF
PROTOCOL_VERSION = 2
SYNC_ENGINE = "rollback-v2"
FRAME_SECONDS = 1.0 / 59.94

# Buttons, as the pad reports them.
A, B, X, Y, START, Z, R_TRIG, L_TRIG = 0x100, 0x200, 0x400, 0x800, 0x1000, 0x10, 0x20, 0x40

log = logging.getLogger("netplay-bot")


def pad(buttons=0, sx=0, sy=0, cx=0, cy=0, lt=0, rt=0):
    return NET_INPUT.pack(buttons, sx, sy, cx, cy, lt, rt)


NEUTRAL = pad()


def scripted(epoch, frame):
    """The controller state for one frame, as a pure function.

    Purity is not a style preference here. An input the peer has already
    accepted is immutable: sending a different value for a frame it has
    already seen is precisely what a desync looks like, and the peer will say
    so. Every frame this bot ever sends has to be reproducible, including the
    ones it resends when asked.
    """
    if epoch == 1:                                   # character select
        # Drop the token onto a character, choose it, then sit still. The
        # human is picking their own fighter at their own pace.
        if 120 <= frame < 146:
            return pad(sy=80)
        if 200 <= frame < 208:
            return pad(A)
        return NEUTRAL
    if epoch == 2:                                   # stage select
        return NEUTRAL                               # let the other player choose
    if epoch >= 3:                                   # a match, results, anything after
        return match_input(epoch, frame)
    return NEUTRAL


def match_input(epoch, frame):
    """Something visibly alive, on a fixed cycle so it stays reproducible.

    The cycle is deliberately busy: movement makes the rollback corrections
    visible on screen, which is the whole point of watching a test match.
    """
    if frame < 60:
        return NEUTRAL                               # let the countdown finish
    phase = (frame - 60) % 240
    if phase < 40:
        return pad(sx=90)                            # walk right
    if phase < 56:
        return pad(X, sx=40)                         # jump forward
    if phase < 72:
        return pad(A, sx=70)                         # attack
    if phase < 112:
        return pad(sx=-90)                           # walk back
    if phase < 128:
        return pad(X, sx=-40)
    if phase < 144:
        return pad(B, sy=-60)                        # something with a bit of shape
    if phase < 168:
        return pad(rt=180, sy=-40)                   # shield
    if phase < 184:
        return pad(A, cx=90)                         # smash
    if phase < 200:
        return pad(Z)                                # grab attempt
    if phase < 216:
        # Press Start only outside a match; in one it would pause the game
        # for the human, which is not a thing a practice partner should do.
        return NEUTRAL if epoch == 3 else pad(A)
    return NEUTRAL


class Bot:
    def __init__(self, args):
        self.args = args
        self.sock = None
        self.buf = b""
        self.id = 0
        self.session = 0
        self.port = -1
        self.peers = {}                  # port -> client id
        self.epoch = 0
        self.peer_frame = 0              # newest frame a peer has reported
        self.sent_through = -1           # newest frame we have put on the wire
        self.room = None
        self.ready = False
        self.udp_port, self.udp_token = 0, ""
        self.compat = self.remembered()
        self.presented = None            # fingerprint this connection said hello with
        self.reconnect = False
        self.hosting = False
        self.pending = None              # a join or create we have not heard back on
        self.pending_at = 0.0
        self.peer_count = 0
        self.asked_at = 0.0
        self.pinged_at = 0.0             # when our own lobby ping went out
        self.rtt_ms = None               # our round trip to the relay
        self.last_ping_reply = 0.0

    # ---- persistence of the learned fingerprint ---------------------------
    def remembered(self):
        if not self.args.remember or not os.path.exists(self.args.remember):
            return None
        try:
            with open(self.args.remember) as f:
                value = json.load(f)
            if isinstance(value, dict) and value.get("schema") == 1:
                log.info("using remembered game fingerprint %s", value["fingerprint"][:12])
                return value
        except (OSError, ValueError):
            pass
        return None

    def remember(self, compat):
        if not compat or compat == self.compat:
            return
        stale = self.compat is not None
        self.compat = compat
        log.info("learned game fingerprint %s", compat["fingerprint"][:12])
        if stale and self.hosting:
            # Our standing room advertises the old one; it cannot be joined
            # by this build, so take it down rather than leave a dead entry.
            log.info("retiring the standing room; it advertised an older build")
            self.hosting, self.ready, self.room = False, False, None
            self.send({"op": "leave"})
        if self.presented != compat:
            # The relay fixes a client's fingerprint at hello and will not let
            # it renegotiate inside a room, so the only way to present a newly
            # learned one is on a fresh connection.
            log.info("reconnecting to present it")
            self.reconnect = True
        if self.args.remember:
            try:
                with open(self.args.remember, "w") as f:
                    json.dump(compat, f)
            except OSError as exc:
                log.warning("could not save the fingerprint: %s", exc)

    # ---- transport --------------------------------------------------------
    def connect(self):
        host, _, port = self.args.server.partition(":")
        path = ""
        if "/" in host:
            host, _, rest = host.partition("/")
            path = "/" + rest
        self.sock = socket.create_connection((host, int(port or 7420)), timeout=15)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if path:
            self.sock.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: melee-netplay\r\n"
                               "Connection: Upgrade\r\n\r\n" % (path, host)).encode())
            reply = self.sock.recv(4096)
            if b" 101" not in reply:
                raise ConnectionError("relay refused the upgrade: %r" % reply[:120])
        self.sock.settimeout(0.05)
        self.buf = b""
        self.presented = self.compat
        hello = {"op": "hello", "name": self.args.name, "version": PROTOCOL_VERSION,
                 "sync": SYNC_ENGINE, "features": []}
        if self.compat:
            hello["features"] = ["compat-v1"]
            hello["compatibility"] = self.compat
        self.send(hello)

    def send(self, message):
        self.sock.sendall((json.dumps(message, separators=(",", ":")) + "\n").encode())

    def relay(self, payload, to=0):
        if not self.session:
            return
        packet = HEADER.pack(MAGIC, self.session, self.id, to) + payload
        self.send({"op": "r", "d": base64.b64encode(packet).decode()})

    def poll(self):
        try:
            chunk = self.sock.recv(65536)
        except socket.timeout:
            return
        except OSError:
            raise ConnectionError("relay connection closed")
        if not chunk:
            raise ConnectionError("relay connection closed")
        self.buf += chunk
        while b"\n" in self.buf:
            raw, self.buf = self.buf.split(b"\n", 1)
            if raw.strip():
                try:
                    self.handle(json.loads(raw))
                except ValueError:
                    pass

    # ---- lobby ------------------------------------------------------------
    def handle(self, message):
        op = message.get("op")
        if op == "welcome":
            self.id = message.get("id", 0)
            self.udp_port = message.get("udp_port", 0)
            self.udp_token = message.get("udp_token", "")
            log.info("connected as client %d", self.id)
            self.send({"op": "list"})
        elif op == "pong":
            # Our own round trip to the relay. Reporting it matters: the relay
            # sizes the automatic delay from both players' trips, and with
            # ours missing it substitutes the other player's, which doubles
            # the estimate and hands them more delay than the link needs.
            # Sitting beside the relay, ours is close to nothing, and saying
            # so is what makes the measured delay the real one.
            if self.pinged_at:
                sample = int((time.monotonic() - self.pinged_at) * 1000)
                self.rtt_ms = sample if self.rtt_ms is None else min(self.rtt_ms, sample)
                self.pinged_at = 0.0
        elif op == "peers":
            # The relay has told us what the connected players are running,
            # which is the only way a room we host can be joinable by them.
            self.remember(message.get("compatibility"))
            self.peer_count = message.get("players", 0)
        elif op == "rooms":
            self.yield_to_real_players(message.get("rooms", []))
            self.consider(message.get("rooms", []))
        elif op == "room":
            room = message.get("room")
            self.room = room
            self.pending = None
            if room is None:
                self.ready = self.hosting = False
                self.send({"op": "list"})
                return
            self.remember(room.get("compatibility"))
            if not self.ready:
                self.ready = True
                self.send({"op": "ready", "ready": 1})
                log.info("ready in room %d (%s)", room.get("id"), room.get("name"))
            if self.hosting and self.everyone_ready(room):
                log.info("starting the match")
                self.send({"op": "start"})
        elif op == "start":
            self.begin(message)
        elif op == "r":
            try:
                self.relayed(base64.b64decode(message.get("d", ""), validate=True))
            except (ValueError, TypeError):
                pass
        elif op in ("ended", "held"):
            if op == "ended":
                log.info("session ended: %s", message.get("reason"))
                self.finish()
        elif op == "error":
            log.warning("relay: %s", message.get("message"))
            self.pending = None
            self.hosting = False
            # Back off before looking again; the room list is about to change
            # anyway and retrying instantly only repeats the same refusal.
            self.pending, self.pending_at = ("backoff", 0), time.monotonic()

    def yield_to_real_players(self, rooms):
        """Give up our own room for one a real player is waiting in.

        A standing room is only joinable by a build whose fingerprint matches
        the one we published, and we can only have learned that from some
        earlier player. So a room of ours is a guess that may already be
        stale, and a player sitting in their own room is the ground truth.
        Going to them costs nothing and means a mismatched standing room can
        never become a dead end that nobody can enter.
        """
        if not self.hosting or self.session or not self.room:
            return
        if len(self.room.get("players", [])) > 1:
            return                       # somebody is already here with us
        mine = self.room.get("id")
        for room in rooms:
            if room.get("id") == mine or room.get("state"):
                continue
            if room.get("players", 0) < 1 or room.get("players", 0) >= room.get("max", 2):
                continue
            if self.args.match and self.args.match.lower() not in room.get("name", "").lower():
                continue
            if room.get("required_mods"):
                continue
            log.info("leaving our room for room %d (%s)", room["id"], room.get("name"))
            self.hosting, self.ready, self.room = False, False, None
            self.pending = None
            self.send({"op": "leave"})
            self.remember(room.get("compatibility"))
            if self.reconnect:
                return                   # rejoin on the next connection
            self.pending, self.pending_at = ("join", room["id"]), time.monotonic()
            self.send({"op": "join", "room": room["id"]})
            return

    @staticmethod
    def everyone_ready(room):
        players = room.get("players", [])
        return len(players) >= 2 and all(p.get("ready") for p in players)

    def consider(self, rooms):
        """Join a waiting room, or put one up once we know what to publish."""
        if self.room or self.session or self.reconnect:
            return
        if self.pending and time.monotonic() - self.pending_at < 3.0:
            return                       # a join or create is still in flight
        self.pending = None
        for room in rooms:
            if room.get("state") or room.get("players", 0) >= room.get("max", 2):
                continue
            if self.args.match and self.args.match.lower() not in room.get("name", "").lower():
                continue
            if room.get("required_mods"):
                continue                 # only plain rooms; no costume packages
            self.remember(room.get("compatibility"))
            if self.reconnect:
                return                   # join on the next connection, with the fingerprint
            log.info("joining room %d (%s)", room["id"], room.get("name"))
            self.pending, self.pending_at = ("join", room["id"]), time.monotonic()
            self.send({"op": "join", "room": room["id"]})
            return
        if self.args.host and self.compat and not self.hosting and (self.peer_count or not self.args.token):
            # Publishing needs the fingerprint of a real build, which is why
            # this only happens once one has been seen.
            self.hosting = True
            log.info("hosting '%s'", self.args.room_name)
            self.pending, self.pending_at = ("create", 0), time.monotonic()
            self.send({"op": "create", "name": self.args.room_name, "max": 2,
                       "rules": {"mode": 1, "stock": 4, "minutes": 8, "items": 0,
                                 "delay": 0, "pause": 0, "damage": 100, "friendly_fire": 0}})

    def begin(self, message):
        self.session = message.get("session", 0)
        self.peers = {}
        self.port = -1
        for player in message.get("players", []):
            self.peers[player["port"]] = player["id"]
            if player["id"] == self.id:
                self.port = player["port"]
        if self.port < 0 or not self.session:
            log.warning("start did not include us")
            self.session = 0
            return
        self.epoch, self.peer_frame, self.sent_through = 1, 0, -1
        log.info("session %d started on port %d, relay delay %d",
                 self.session, self.port + 1, message.get("delay", 0))

    def finish(self):
        if self.session:
            self.relay(struct.pack("<2B", PKT_BYE, max(self.port, 0)))
        self.session, self.room, self.ready, self.hosting = 0, None, False, False
        self.epoch, self.peer_frame, self.sent_through = 0, 0, -1
        self.pending, self.pending_at = ("backoff", 0), time.monotonic()
        self.send({"op": "leave"})

    # ---- match ------------------------------------------------------------
    def relayed(self, data):
        if len(data) <= HEADER.size:
            return
        magic, session, sender, _ = HEADER.unpack_from(data)
        if magic != MAGIC or session != self.session or not self.session:
            return
        payload = data[HEADER.size:]
        kind = payload[0]
        if kind == PKT_PING and len(payload) >= PING.size:
            _, _, tick = PING.unpack_from(payload)
            self.relay(PING.pack(PKT_PONG, max(self.port, 0), tick), sender)
            self.last_ping_reply = time.monotonic()
        elif kind == PKT_REQUEST and len(payload) >= REQUEST.size:
            _, wanted = REQUEST.unpack_from(payload)
            self.emit(wanted, self.sent_through)
        elif kind == PKT_INPUT and len(payload) >= INPUT_HEADER.size:
            _, epoch, _, _, _, ack, _, _, _ = INPUT_HEADER.unpack_from(payload)
            if epoch != self.epoch:
                # The other player has entered a new scene; frames restart
                # there, so follow rather than keep sending for the old one.
                log.info("following into epoch %d", epoch)
                self.epoch, self.peer_frame, self.sent_through = epoch, 0, -1
            self.peer_frame = max(self.peer_frame, ack)

    def emit(self, first, last):
        """Send inputs for [first, last], in batches the peer will accept."""
        if last < first:
            return
        first = max(0, first)
        while first <= last:
            count = min(48, last - first + 1)
            frames = b"".join(scripted(self.epoch, first + i) for i in range(count))
            # We claim to be a little ahead and report the matching advantage,
            # so the peer never concludes it is the one running ahead and
            # gives frames back to a bot that has no clock to catch up with.
            lead = self.args.lead
            header = INPUT_HEADER.pack(PKT_INPUT, self.epoch & 0xFF, self.port, count,
                                       first + count - 1, self.peer_frame + lead,
                                       NO_HASH, 0, -lead)
            self.relay(header + frames)
            first += count

    def tick(self):
        if not self.session or self.port < 0:
            return
        # Stay ahead of whatever frame the player has reached. Their client
        # holds inputs until it needs them, so running ahead costs nothing and
        # means a late packet is already covered by the next one.
        target = self.peer_frame + self.args.lead + self.args.ahead
        if self.sent_through < 0:
            self.emit(0, target)
            self.sent_through = target
        elif target > self.sent_through:
            # Repeat a few already-sent frames: over a lossy path that is what
            # repairs a dropped packet without waiting to be asked.
            self.emit(max(0, self.sent_through - self.args.repeat + 1), target)
            self.sent_through = target

    def run(self):
        backoff = 1.0
        while True:
            try:
                self.connect()
                backoff = 1.0
                hello_at = listed_at = pinged = 0.0
                while True:
                    self.poll()
                    if self.reconnect:
                        self.reconnect = False
                        raise ConnectionError("presenting a newly learned fingerprint")
                    now = time.monotonic()
                    if self.session and now - hello_at > 0.2:
                        hello_at = now
                        self.relay(struct.pack("<2B", PKT_HELLO, max(self.port, 0)))
                    if not self.session and not self.room and now - listed_at > 2.0:
                        listed_at = now
                        self.send({"op": "list"})
                    if self.args.token and not self.session and now - self.asked_at > 5.0:
                        self.asked_at = now
                        self.send({"op": "peers", "token": self.args.token})
                    if now - pinged > 2.0:
                        pinged = now
                        self.pinged_at = now
                        message = {"op": "ping"}
                        if self.rtt_ms is not None:
                            message["rtt"] = max(0, min(10000, self.rtt_ms))
                        self.send(message)
                    self.tick()
                    time.sleep(0.004)
            except ConnectionError as exc:
                if "newly learned" in str(exc):
                    log.info("%s", exc)
                    backoff = 0.2
                else:
                    log.warning("disconnected (%s); retrying in %.0fs", exc, backoff)
            except OSError as exc:
                log.warning("disconnected (%s); retrying in %.0fs", exc, backoff)
            except KeyboardInterrupt:
                try:
                    self.finish()
                except Exception:
                    pass
                return
            finally:
                if self.sock:
                    try:
                        self.sock.close()
                    except OSError:
                        pass
                self.sock = None
            self.session, self.room, self.ready, self.hosting = 0, None, False, False
            self.pending = None
            time.sleep(backoff)
            backoff = min(30.0, max(1.0, backoff) * 2)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="127.0.0.1:7420",
                    help="relay endpoint; host:port, or host/path for one behind a web server")
    ap.add_argument("--name", default="Ping Test", help="name shown in the lobby")
    ap.add_argument("--room-name", default="Ping test (bot)")
    ap.add_argument("--match", default="",
                    help="only join rooms whose name contains this text")
    ap.add_argument("--host", action="store_true",
                    help="also put up a standing room, once a game fingerprint is known")
    ap.add_argument("--remember", default="",
                    help="file to keep the learned game fingerprint in, so --host works after a restart")
    ap.add_argument("--lead", type=int, default=2,
                    help="frames this peer claims to be ahead; keeps the player's clock correction quiet")
    ap.add_argument("--token", default=os.environ.get("MELEE_NETPLAY_ADMIN_TOKEN", ""),
                    help="shared secret letting the relay report which build the players use, "
                         "so a hosted room is one they can join")
    ap.add_argument("--ahead", type=int, default=8, help="frames of input to keep queued ahead")
    ap.add_argument("--repeat", type=int, default=8, help="recent frames repeated in every packet")
    args = ap.parse_args()
    if not 0 <= args.lead <= 8 or not 1 <= args.ahead <= 60 or not 1 <= args.repeat <= 48:
        ap.error("lead/ahead/repeat are out of range")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    Bot(args).run()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
