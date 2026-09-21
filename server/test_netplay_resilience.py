"""Regressions for the parts of the relay that decide whether a match survives.

Covers the unreliable input path, the reconnect window that a dropped player's
place is held open for, and the automatic input delay. No game process, public
server, or third-party dependencies are required.

Run from the repository root: python -m unittest discover -s server -p "test_*.py"
"""
import base64
import json
import struct
import time
import unittest

from melee_netplay_server import (
    Client, DEFAULT_RULES, HEADER, MAGIC, MAX_AUTO_DELAY, MIN_AUTO_DELAY, PKT_BYE,
    PROTOCOL_VERSION, SYNC_ENGINE, UDP_PACKETS_PER_SECOND, UDP_TOKEN_CHARS, Server,
    automatic_delay, clean_rules,
)


class RecordingWriter:
    def __init__(self):
        self.messages = []
        self.closed = False
        self.transport = self

    def is_closing(self):
        return self.closed

    def get_write_buffer_size(self):
        return 0

    def write(self, data):
        self.messages.append(json.loads(data))

    def close(self):
        self.closed = True


class RecordingDatagrams:
    """Stands in for the UDP transport; records what was sent where."""

    def __init__(self, failing=()):
        self.sent = []
        self.failing = set(failing)

    def sendto(self, data, addr):
        if addr in self.failing:
            raise OSError("unreachable")
        self.sent.append((addr, data))


class Base(unittest.TestCase):
    def setUp(self):
        self.server = Server()
        self.udp = RecordingDatagrams()
        self.server.udp = self.udp
        self.server.udp_port = 7420
        self.host = self.client("Host")
        self.guest = self.client("Guest")

    def client(self, name):
        client = Client(self.server, None, RecordingWriter())
        client.name = name
        self.server.clients[client.id] = client
        self.command(client, "hello", name=name, version=PROTOCOL_VERSION, sync=SYNC_ENGINE)
        return client

    def command(self, client, op, **fields):
        self.server.handle(client, {"op": op, **fields})

    def welcome_of(self, client):
        return next(m for m in client.writer.messages if m["op"] == "welcome")

    def started_room(self):
        self.command(self.host, "create", name="Test room", max=2)
        room = self.host.room
        self.command(self.guest, "join", room=room.id)
        for player in room.players:
            self.command(player, "ready", ready=1)
        self.command(room.host, "start")
        self.assertNotEqual(room.session, 0)
        self.clear()
        return room

    def clear(self):
        for client in self.server.clients.values():
            client.writer.messages.clear()
        self.udp.sent.clear()

    def datagram(self, client, session, payload=b"\x01\x00", target=0):
        header = HEADER.pack(MAGIC, session, client.id, target)
        return client.udp_token.encode() + header + payload

    def messages(self, client, op):
        return [m for m in client.writer.messages if m["op"] == op]


class DatagramRelayTests(Base):
    def test_welcome_offers_the_side_channel(self):
        welcome = self.welcome_of(self.host)
        self.assertEqual(welcome["udp_port"], 7420)
        self.assertEqual(len(welcome["udp_token"]), UDP_TOKEN_CHARS)
        self.assertNotEqual(welcome["udp_token"], self.welcome_of(self.guest)["udp_token"])

    def test_token_not_source_address_identifies_the_sender(self):
        room = self.started_room()
        # The guest has proven a datagram path; the host has not.
        self.server.relay_datagram(self.datagram(self.guest, room.session), ("203.0.113.9", 5000))
        self.clear()
        self.server.relay_datagram(self.datagram(self.host, room.session, b"\x01\xAA"),
                                   ("198.51.100.4", 6000))
        self.assertEqual(len(self.udp.sent), 1)
        addr, data = self.udp.sent[0]
        self.assertEqual(addr, ("203.0.113.9", 5000))
        self.assertEqual(data[HEADER.size:], b"\x01\xAA")
        # Nothing was mirrored back to the sender.
        self.assertEqual(self.messages(self.host, "r"), [])

    def test_a_renumbered_nat_mapping_follows_the_player(self):
        room = self.started_room()
        self.server.relay_datagram(self.datagram(self.guest, room.session), ("203.0.113.9", 5000))
        self.server.relay_datagram(self.datagram(self.guest, room.session), ("203.0.113.9", 7777))
        self.clear()
        self.server.relay_datagram(self.datagram(self.host, room.session), ("198.51.100.4", 6000))
        self.assertEqual([addr for addr, _ in self.udp.sent], [("203.0.113.9", 7777)])

    def test_each_recipient_gets_the_path_that_works_for_them(self):
        room = self.started_room()
        # Only the guest is reachable by datagram; the host must stay on the stream.
        self.server.relay_datagram(self.datagram(self.guest, room.session), ("203.0.113.9", 5000))
        self.clear()
        self.server.relay(self.guest, base64.b64encode(
            HEADER.pack(MAGIC, room.session, self.guest.id, 0) + b"\x01\x02").decode())
        self.assertEqual(len(self.messages(self.host, "r")), 1)
        self.assertEqual(self.udp.sent, [])

    def test_an_unreachable_recipient_falls_back_to_the_stream(self):
        room = self.started_room()
        self.server.relay_datagram(self.datagram(self.guest, room.session), ("203.0.113.9", 5000))
        self.clear()
        self.server.udp = self.udp = RecordingDatagrams(failing={("203.0.113.9", 5000)})
        self.server.relay_datagram(self.datagram(self.host, room.session), ("198.51.100.4", 6000))
        self.assertEqual(len(self.messages(self.guest, "r")), 1)
        self.assertIsNone(self.guest.udp_addr)

    def test_unknown_forged_and_oversized_datagrams_are_ignored(self):
        room = self.started_room()
        self.server.relay_datagram(self.datagram(self.guest, room.session), ("203.0.113.9", 5000))
        self.clear()
        unknown = b"f" * UDP_TOKEN_CHARS + HEADER.pack(MAGIC, room.session, self.host.id, 0) + b"\x01"
        self.server.relay_datagram(unknown, ("198.51.100.4", 6000))
        # A valid token cannot be used to speak as another player.
        forged = (self.host.udp_token.encode()
                  + HEADER.pack(MAGIC, room.session, self.guest.id, 0) + b"\x01")
        self.server.relay_datagram(forged, ("198.51.100.4", 6000))
        self.server.relay_datagram(b"short", ("198.51.100.4", 6000))
        self.server.relay_datagram(self.host.udp_token.encode() + b"\x00" * 4000,
                                   ("198.51.100.4", 6000))
        self.assertEqual(self.udp.sent, [])
        self.assertEqual(self.messages(self.guest, "r"), [])

    def test_a_datagram_flood_is_capped(self):
        room = self.started_room()
        self.server.relay_datagram(self.datagram(self.guest, room.session), ("203.0.113.9", 5000))
        self.clear()
        packet = self.datagram(self.host, room.session)
        for _ in range(UDP_PACKETS_PER_SECOND + 500):
            self.server.relay_datagram(packet, ("198.51.100.4", 6000))
        self.assertLessEqual(len(self.udp.sent), UDP_PACKETS_PER_SECOND)
        # The budget refills, so a capped client is throttled and never banned.
        self.host.udp_window = time.monotonic() - 2
        self.server.relay_datagram(packet, ("198.51.100.4", 6000))
        self.assertLessEqual(len(self.udp.sent), UDP_PACKETS_PER_SECOND + 1)

    def test_goodbye_over_a_datagram_still_ends_the_session(self):
        room = self.started_room()
        self.server.relay_datagram(self.datagram(self.host, room.session, bytes([PKT_BYE, 0])),
                                   ("198.51.100.4", 6000))
        self.assertEqual(room.session, 0)
        self.assertEqual(len(self.messages(self.guest, "ended")), 1)

    def test_no_side_channel_is_offered_when_it_is_not_running(self):
        server = Server()
        client = Client(server, None, RecordingWriter())
        server.clients[client.id] = client
        server.handle(client, {"op": "hello", "name": "Solo", "version": PROTOCOL_VERSION, "sync": SYNC_ENGINE})
        welcome = next(m for m in client.writer.messages if m["op"] == "welcome")
        self.assertEqual(welcome["udp_port"], 0)


class ReconnectTests(Base):
    def drop(self, client):
        """What serve_client does when a connection goes away."""
        held = self.server.hold_slot(client)
        if not held:
            self.server.leave_room(client, "disconnected")
        self.server.clients.pop(client.id, None)
        self.server.by_token.pop(client.udp_token, None)
        return held

    def test_dropping_mid_match_holds_the_place_instead_of_ending_it(self):
        room = self.started_room()
        session = room.session
        self.assertTrue(self.drop(self.guest))
        self.assertEqual(room.session, session)
        self.assertEqual(self.messages(self.host, "ended"), [])
        held = self.messages(self.host, "held")
        self.assertEqual(len(held), 1)
        self.assertEqual(held[0]["port"], 1)

    def test_the_same_player_is_seated_back_on_the_same_port(self):
        room = self.started_room()
        session = room.session
        self.drop(self.guest)
        self.clear()
        returning = self.client("Guest")
        self.command(returning, "resume", session=session)
        resumed = self.messages(returning, "resumed")
        self.assertEqual(len(resumed), 1)
        self.assertEqual(resumed[0]["ok"], 1)
        self.assertEqual(resumed[0]["port"], 1)
        self.assertEqual(returning.port, 1)
        self.assertEqual(room.session, session)
        self.assertEqual(room.holds, {})
        # The player still in the match is told the rejoining player's new id,
        # which is what its relay uses to recognise their packets.
        seen = self.messages(self.host, "resumed")
        self.assertEqual(len(seen), 1)
        self.assertEqual(sorted(p["port"] for p in seen[0]["players"]), [0, 1])
        self.assertEqual({p["port"]: p["id"] for p in seen[0]["players"]}[1], returning.id)

    def test_a_resumed_player_relays_again_under_the_new_id(self):
        room = self.started_room()
        session = room.session
        self.drop(self.guest)
        returning = self.client("Guest")
        self.command(returning, "resume", session=session)
        self.clear()
        self.command(returning, "r", d=base64.b64encode(
            HEADER.pack(MAGIC, session, returning.id, 0) + b"\x01\x07").decode())
        self.assertEqual(len(self.messages(self.host, "r")), 1)

    def test_a_stranger_cannot_take_a_held_place(self):
        room = self.started_room()
        session = room.session
        self.drop(self.guest)
        self.clear()
        stranger = self.client("Somebody Else")
        self.command(stranger, "resume", session=session)
        self.assertEqual(self.messages(stranger, "resumed")[0]["ok"], 0)
        self.assertIn(1, room.holds)
        self.assertIsNone(stranger.room)

    def test_the_window_closes_and_the_session_ends(self):
        room = self.started_room()
        self.drop(self.guest)
        self.server.expire_holds(time.monotonic() + self.server.hold_seconds + 1)
        self.assertEqual(room.session, 0)
        self.assertEqual(room.holds, {})
        self.assertEqual(len(self.messages(self.host, "ended")), 1)
        self.assertEqual(self.host.port, 0)
        self.assertIs(room.host, self.host)

    def test_resuming_after_the_window_is_refused_cleanly(self):
        room = self.started_room()
        session = room.session
        self.drop(self.guest)
        self.server.expire_holds(time.monotonic() + self.server.hold_seconds + 1)
        self.clear()
        returning = self.client("Guest")
        self.command(returning, "resume", session=session)
        self.assertEqual(self.messages(returning, "resumed")[0]["ok"], 0)
        self.assertIsNone(returning.room)

    def test_a_room_with_nobody_connected_is_not_advertised(self):
        room = self.started_room()
        self.drop(self.host)
        self.drop(self.guest)
        self.assertEqual(room.players, [])
        self.assertNotEqual(room.session, 0)
        self.assertEqual(self.server.room_list()["rooms"], [])

    def test_leaving_on_purpose_is_not_held(self):
        room = self.started_room()
        room_id = room.id
        self.command(self.guest, "leave")
        self.assertEqual(room.session, 0)
        self.assertEqual(room.holds, {})
        self.assertEqual(len(self.messages(self.host, "ended")), 1)
        self.assertIn(room_id, self.server.rooms)

    def test_a_drop_outside_a_match_is_an_ordinary_departure(self):
        self.command(self.host, "create", name="Test room", max=2)
        room = self.host.room
        self.command(self.guest, "join", room=room.id)
        self.assertFalse(self.drop(self.guest))
        self.assertEqual(room.holds, {})
        self.assertEqual(room.players, [self.host])

    def test_an_invalid_resume_is_rejected_without_disturbing_the_room(self):
        room = self.started_room()
        self.drop(self.guest)
        self.clear()
        returning = self.client("Guest")
        self.command(returning, "resume", session="not a session")
        self.command(returning, "resume", session=0)
        self.assertEqual([m["ok"] for m in self.messages(returning, "resumed")], [0, 0])
        self.assertIn(1, room.holds)


class AutomaticDelayTests(unittest.TestCase):
    """The delay has to be one number, and it has to come from the real path.

    Inputs travel client -> server -> client, so the trip the delay must cover
    is half of each client's round trip to this server -- not the round trip
    between the clients and this server's own ping to either of them.

    It also has to be identical for both players. A match seeds the frames
    before the delay as known-empty input on every port, so two clients
    disagreeing about where that boundary sits would contradict each other the
    first time somebody held a direction on frame zero. That is why the number
    is resolved here, once, rather than measured independently on each side.
    """

    class Peer:
        def __init__(self, rtt_ms):
            self.rtt_ms = rtt_ms

    def delay_for(self, *rtts):
        return automatic_delay([self.Peer(r) for r in rtts])

    def test_zero_means_automatic_and_survives_the_room_rules(self):
        self.assertEqual(DEFAULT_RULES["delay"], 0)
        self.assertEqual(clean_rules({"delay": 0})["delay"], 0)

    def test_an_explicit_delay_is_still_honoured_and_clamped(self):
        self.assertEqual(clean_rules({"delay": 4})["delay"], 4)
        self.assertEqual(clean_rules({"delay": 99})["delay"], 10)
        self.assertEqual(clean_rules({"delay": -5})["delay"], 0)

    def test_the_delay_covers_the_one_way_trip_between_the_players(self):
        # Both on a 4 ms link: 4 ms one way, a single frame covers it.
        self.assertEqual(self.delay_for(4, 4), 1)
        # 34 ms each way through the relay is 34 ms, just over two frames.
        self.assertEqual(self.delay_for(34, 34), 3)
        # An asymmetric pair is covered by the sum of the two halves, not by
        # either one alone: 10 + 40 is 50 ms, which one frame would not cover.
        self.assertEqual(self.delay_for(20, 80), 3)

    def test_the_delay_stays_inside_usable_bounds(self):
        self.assertEqual(self.delay_for(0, 0), MIN_AUTO_DELAY)
        self.assertEqual(self.delay_for(5000, 5000), MAX_AUTO_DELAY)

    def test_an_unmeasured_player_does_not_drag_the_delay_to_zero(self):
        # A client that has not reported yet is treated like the ones that
        # have, rather than as a zero-latency player.
        self.assertEqual(self.delay_for(80, None), self.delay_for(80, 80))
        self.assertEqual(self.delay_for(None, None), 3)

    def test_the_furthest_pair_sets_the_delay_in_a_bigger_room(self):
        self.assertEqual(self.delay_for(4, 4, 120), self.delay_for(4, 120))

    def test_both_players_are_told_the_same_resolved_delay(self):
        server = Server()
        clients = []
        for name, rtt in (("Host", 30), ("Guest", 70)):
            client = Client(server, None, RecordingWriter())
            client.name = name
            server.clients[client.id] = client
            server.handle(client, {"op": "hello", "name": name, "version": PROTOCOL_VERSION, "sync": SYNC_ENGINE})
            server.handle(client, {"op": "ping", "rtt": rtt})
            clients.append(client)
        host, guest = clients
        self.assertEqual(host.rtt_ms, 30)
        server.handle(host, {"op": "create", "name": "Room", "max": 2, "rules": {**DEFAULT_RULES, "delay": 0}})
        server.handle(guest, {"op": "join", "room": host.room.id})
        for client in clients:
            server.handle(client, {"op": "ready", "ready": 1})
        server.handle(host, {"op": "start"})
        starts = [next(m for m in c.writer.messages if m["op"] == "start") for c in clients]
        self.assertEqual(starts[0]["delay"], starts[1]["delay"])
        self.assertEqual(starts[0]["delay"], self.delay_for(30, 70))
        # The lobby still reports the room as automatic.
        self.assertEqual(starts[0]["rules"]["delay"], 0)

    def test_a_fixed_room_delay_is_passed_through_untouched(self):
        server = Server()
        clients = []
        for name in ("Host", "Guest"):
            client = Client(server, None, RecordingWriter())
            client.name = name
            server.clients[client.id] = client
            server.handle(client, {"op": "hello", "name": name, "version": PROTOCOL_VERSION, "sync": SYNC_ENGINE})
            server.handle(client, {"op": "ping", "rtt": 250})
            clients.append(client)
        host, guest = clients
        server.handle(host, {"op": "create", "name": "Room", "max": 2, "rules": {**DEFAULT_RULES, "delay": 2}})
        server.handle(guest, {"op": "join", "room": host.room.id})
        for client in clients:
            server.handle(client, {"op": "ready", "ready": 1})
        server.handle(host, {"op": "start"})
        start = next(m for m in host.writer.messages if m["op"] == "start")
        self.assertEqual(start["delay"], 2)
        self.assertEqual(start["rules"]["delay"], 2)

    def test_a_nonsense_reported_round_trip_is_ignored(self):
        server = Server()
        client = Client(server, None, RecordingWriter())
        server.clients[client.id] = client
        server.handle(client, {"op": "hello", "name": "P", "version": PROTOCOL_VERSION, "sync": SYNC_ENGINE})
        for bad in (-1, 10001, "40", 12.5, None, True):
            server.handle(client, {"op": "ping", "rtt": bad})
            self.assertIsNone(client.rtt_ms)
        server.handle(client, {"op": "ping", "rtt": 44})
        self.assertEqual(client.rtt_ms, 44)


if __name__ == "__main__":
    unittest.main()
