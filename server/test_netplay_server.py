"""Regressions for lobby lifecycle and the actual TCP/HTTP relay protocol.

Run from the repository root: python -m unittest discover -s server -p "test_*.py" -v
No game process, public server, or third-party dependencies are required.
"""
import asyncio
import base64
import json
import time
import unittest

from melee_netplay_server import (
    Client, DEFAULT_RULES, HEADER, MAGIC, MAX_CLIENT_ID, MAX_LINE_BYTES,
    MAX_PACKET_BYTES, MAX_WRITE_BUFFER, PKT_BYE, PROTOCOL_VERSION, SYNC_ENGINE, Server, clean_rules,
)


class RecordingWriter:
    def __init__(self):
        self.messages = []
        self.closed = False
        self.buffered = 0
        self.transport = self

    def is_closing(self):
        return self.closed

    def get_write_buffer_size(self):
        return self.buffered

    def write(self, data):
        self.messages.append(json.loads(data))

    def close(self):
        self.closed = True


class LobbyTests(unittest.TestCase):
    def setUp(self):
        self.server = Server()
        self.host = self.client('Host')
        self.guest = self.client('Guest')

    def client(self, name):
        client = Client(self.server, None, RecordingWriter())
        client.name = name
        self.server.clients[client.id] = client
        self.command(client, 'hello', name=name, version=PROTOCOL_VERSION, sync=SYNC_ENGINE)
        client.writer.messages.clear()
        return client

    def command(self, client, op, **fields):
        self.server.handle(client, {'op': op, **fields})

    def room(self, capacity=2):
        self.command(self.host, 'create', name='Test room', max=capacity)
        room = self.host.room
        self.command(self.guest, 'join', room=room.id)
        return room

    def start(self, room):
        for player in room.players:
            self.command(player, 'ready', ready=1)
        self.command(room.host, 'start')
        self.assertNotEqual(room.session, 0)
        return room.session

    def packet(self, client, session, payload=b'\x00\x00', target=0, sender=None):
        data = HEADER.pack(MAGIC, session, client.id if sender is None else sender, target) + payload
        return base64.b64encode(data).decode('ascii')

    def clear_messages(self):
        for client in self.server.clients.values():
            client.writer.messages.clear()

    def test_room_summary_exposes_current_rules(self):
        room = self.room()
        self.assertEqual(room.summary()['rules'], room.rules)
        browser = self.client('Browsing')
        self.command(self.host, 'rules', rules={**room.rules, 'stock': 2})
        self.assertEqual(self.server.room_list()['rooms'][0]['rules']['stock'], 2)
        self.assertEqual(browser.writer.messages[-1]['rooms'][0]['rules']['stock'], 2)

    def test_unnegotiated_client_cannot_create_or_join(self):
        visitor = Client(self.server, None, RecordingWriter())
        self.server.clients[visitor.id] = visitor
        self.command(visitor, 'create')
        self.assertIsNone(visitor.room)
        self.assertEqual(visitor.writer.messages[-1]['op'], 'error')
        room = self.room()
        self.command(visitor, 'join', room=room.id)
        self.assertIsNone(visitor.room)

    def test_old_or_unknown_sync_builds_are_rejected(self):
        for version, sync in ((1, None), (2, None), (2, 'lockstep'), (3, SYNC_ENGINE), (2.0, SYNC_ENGINE)):
            with self.subTest(version=version, sync=sync):
                visitor = Client(self.server, None, RecordingWriter())
                self.command(visitor, 'hello', version=version, sync=sync)
                self.assertFalse(visitor.compatible)
                self.assertTrue(visitor.writer.closed)
                self.assertEqual(visitor.writer.messages[-1]['op'], 'error')

    def test_duplicate_start_keeps_existing_session(self):
        room = self.room()
        session = self.start(room)
        self.clear_messages()
        self.command(self.host, 'start')
        self.assertEqual(room.session, session)
        self.assertEqual(self.server.sessions, {session: room})
        self.assertEqual(self.host.writer.messages[-1]['op'], 'error')
        self.assertEqual(self.guest.writer.messages, [])

    def test_rejoining_own_single_player_room_does_not_orphan_it(self):
        self.command(self.host, 'create')
        room = self.host.room
        self.command(self.host, 'join', room=room.id)
        self.assertIs(self.server.rooms[room.id], room)
        self.assertEqual(room.players, [self.host])

    def test_rejoining_full_room_is_idempotent(self):
        room = self.room()
        self.command(self.guest, 'ready', ready=True)
        self.command(self.guest, 'join', room=room.id)
        self.assertEqual(room.players, [self.host, self.guest])
        self.assertTrue(self.guest.ready)
        self.assertEqual(self.guest.writer.messages[-1]['op'], 'room')

    def test_changed_rules_require_everyone_to_ready_again(self):
        room = self.room()
        for player in room.players:
            self.command(player, 'ready', ready=1)
        self.command(self.host, 'rules', rules={**room.rules, 'stock': 3})
        self.assertFalse(any(player.ready for player in room.players))
        self.assertEqual(room.rules['stock'], 3)
        self.command(self.host, 'start')
        self.assertEqual(room.session, 0)

    def test_identical_rules_preserve_ready(self):
        room = self.room()
        self.command(self.guest, 'ready', ready=True)
        self.command(self.host, 'rules', rules=room.rules)
        self.assertTrue(self.guest.ready)

    def test_guest_cannot_change_rules_or_start(self):
        room = self.room()
        self.command(self.guest, 'rules', rules={'stock': 99})
        self.command(self.guest, 'start')
        self.assertEqual(room.rules, DEFAULT_RULES)
        self.assertEqual(room.session, 0)
        self.assertEqual(self.guest.writer.messages[-1]['op'], 'error')

    def test_malformed_numeric_requests_preserve_membership(self):
        room = self.room()
        for value in (None, [], {}, 'two', True, 2.0):
            for op, key in (('create', 'max'), ('join', 'room')):
                with self.subTest(op=op, value=value):
                    self.command(self.host, op, **{key: value})
                    self.assertIs(self.host.room, room)
                    self.assertEqual(self.host.writer.messages[-1]['op'], 'error')

    def test_ready_accepts_only_boolean_or_zero_one(self):
        self.room()
        for value in (None, [], {}, 'false', 2, -1, 1.0):
            with self.subTest(value=value):
                self.command(self.guest, 'ready', ready=value)
                self.assertFalse(self.guest.ready)
                self.assertEqual(self.guest.writer.messages[-1]['op'], 'error')
        self.command(self.guest, 'ready', ready=True)
        self.assertTrue(self.guest.ready)

    def test_active_session_ignores_rule_and_ready_changes(self):
        room = self.room()
        self.start(room)
        self.command(self.host, 'rules', rules={'stock': 2})
        self.command(self.host, 'ready', ready=False)
        self.assertEqual(room.rules, DEFAULT_RULES)
        self.assertTrue(self.host.ready)

    def test_host_departure_ends_session_promotes_guest_and_clears_ready(self):
        room = self.room()
        session = self.start(room)
        self.command(self.host, 'leave')
        self.assertIs(room.host, self.guest)
        self.assertEqual(room.players, [self.guest])
        self.assertEqual(self.guest.port, 0)
        self.assertFalse(self.guest.ready)
        self.assertIsNone(self.host.room)
        self.assertNotIn(session, self.server.sessions)
        self.assertEqual(room.session, 0)
        self.assertTrue(any(m['op'] == 'ended' for m in self.guest.writer.messages))
        self.command(self.guest, 'leave')
        self.assertNotIn(room.id, self.server.rooms)

    def test_bye_allows_rematch_and_rejects_old_session_traffic(self):
        room = self.room()
        session = self.start(room)
        self.server.relay(self.host, self.packet(self.host, session, bytes([PKT_BYE, self.host.port])))
        self.assertEqual(room.session, 0)
        self.assertEqual(self.server.sessions, {})
        self.assertFalse(any(player.ready for player in room.players))
        self.assertEqual(room.players, [self.host, self.guest])
        replacement = self.start(room)
        self.assertNotEqual(replacement, session)
        self.clear_messages()
        self.server.relay(self.host, self.packet(self.host, session))
        self.assertEqual(self.guest.writer.messages, [])

    def test_relay_authenticates_sender_and_session_membership(self):
        room = self.room()
        session = self.start(room)
        stranger = self.client('Stranger')
        self.clear_messages()
        self.server.relay(self.host, self.packet(self.host, session, sender=self.guest.id))
        self.server.relay(stranger, self.packet(stranger, session))
        self.server.relay(self.host, self.packet(self.host, session + 1))
        self.assertEqual(self.guest.writer.messages, [])
        packet = self.packet(self.host, session)
        self.server.relay(self.host, packet)
        self.assertEqual(self.guest.writer.messages, [{'op': 'r', 'd': packet}])
        self.assertEqual(self.host.writer.messages, [])

    def test_targeted_relay_only_reaches_requested_player(self):
        room = self.room(capacity=4)
        third = self.client('Third')
        self.command(third, 'join', room=room.id)
        session = self.start(room)
        self.clear_messages()
        packet = self.packet(self.host, session, target=third.id)
        self.server.relay(self.host, packet)
        self.assertEqual(self.guest.writer.messages, [])
        self.assertEqual(third.writer.messages, [{'op': 'r', 'd': packet}])

    def test_malformed_and_oversized_relay_packets_are_dropped(self):
        room = self.room()
        session = self.start(room)
        self.clear_messages()
        for packet in (None, [], {}, '!not base64!', '',
                       self.packet(self.host, session, b''),
                       self.packet(self.host, session, b'\x00' * MAX_PACKET_BYTES)):
            with self.subTest(packet_type=type(packet).__name__):
                self.server.relay(self.host, packet)
                self.assertEqual(self.guest.writer.messages, [])

    def test_client_ids_wrap_without_colliding_or_exceeding_wire_width(self):
        self.server.client_counter = MAX_CLIENT_ID - 1
        last = self.client('Last')
        wrapped = self.client('Wrapped')
        self.assertEqual(last.id, MAX_CLIENT_ID)
        self.assertEqual(wrapped.id, 1)
        self.server.client_counter = MAX_CLIENT_ID
        self.assertEqual(self.client('Next').id, 2)
        HEADER.pack(MAGIC, 1, last.id, wrapped.id)

    def test_exhausted_client_ids_fail_without_reusing_an_active_id(self):
        self.server.clients = dict.fromkeys(range(1, MAX_CLIENT_ID + 1))
        with self.assertRaises(ConnectionError):
            self.server.next_client_id()

    def test_slow_client_write_buffer_is_bounded(self):
        self.host.writer.buffered = MAX_WRITE_BUFFER + 1
        self.host.send({'op': 'pong'})
        self.assertTrue(self.host.writer.closed)
        self.assertEqual(self.host.writer.messages, [])

    def test_rules_are_clamped_consistently(self):
        rules = clean_rules({'stock': -1, 'minutes': -1, 'delay': 999, 'items': 999,
                             'damage': 1, 'pause': False, 'friendly_fire': True})
        self.assertEqual(rules, {**DEFAULT_RULES, 'stock': 1, 'minutes': 0,
                                'delay': 10, 'items': 5, 'damage': 50,
                                'pause': 0, 'friendly_fire': 1})


class TcpProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.server = Server()
        self.handlers = set()
        self.writers = []
        self.errors = []
        self.loop = asyncio.get_running_loop()
        self.previous_handler = self.loop.get_exception_handler()
        self.loop.set_exception_handler(lambda loop, context: self.errors.append(context))

        async def connected(reader, writer):
            task = asyncio.current_task()
            self.handlers.add(task)
            try:
                await self.server.serve_client(reader, writer)
            finally:
                self.handlers.discard(task)

        self.listener = await asyncio.start_server(connected, '127.0.0.1', 0, limit=MAX_LINE_BYTES)
        self.port = self.listener.sockets[0].getsockname()[1]

    async def asyncTearDown(self):
        for writer in self.writers:
            writer.close()
        await asyncio.gather(*(writer.wait_closed() for writer in self.writers), return_exceptions=True)
        self.listener.close()
        await self.listener.wait_closed()
        if self.handlers:
            await asyncio.wait_for(asyncio.gather(*tuple(self.handlers)), timeout=3)
        self.loop.set_exception_handler(self.previous_handler)
        # Dropping out of a live match holds the player's place rather than
        # ending the session, so a test that disconnects mid-match leaves the
        # room behind until the reconnect window closes.
        self.server.expire_holds(time.monotonic() + self.server.hold_seconds + 1)
        self.assertEqual(self.server.clients, {})
        self.assertEqual(self.server.rooms, {})
        self.assertEqual(self.server.sessions, {})
        self.assertEqual(self.errors, [])

    async def connect(self, upgrade=False):
        reader, writer = await asyncio.open_connection('127.0.0.1', self.port)
        self.writers.append(writer)
        if upgrade:
            writer.write(b'GET /netplay HTTP/1.1\r\nHost: localhost\r\nUpgrade: melee-netplay\r\nConnection: Upgrade\r\n\r\n')
            response = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), 3)
            self.assertTrue(response.startswith(b'HTTP/1.1 101 '))
        writer.write(b'{"op":"hello","name":"Tester","version":2,"sync":"rollback-v2"}\n')
        welcome = await self.receive(reader, 'welcome')
        await self.receive(reader, 'rooms')
        return reader, writer, welcome['id']

    async def receive(self, reader, op):
        async def read():
            while True:
                line = await reader.readline()
                self.assertTrue(line, 'connection ended while waiting for ' + op)
                message = json.loads(line)
                if message['op'] == op:
                    return message
        return await asyncio.wait_for(read(), 3)

    async def test_legacy_client_gets_version_error_then_disconnects(self):
        reader, writer = await asyncio.open_connection('127.0.0.1', self.port)
        self.writers.append(writer)
        writer.write(b'{"op":"hello","version":1}\n')
        message = await self.receive(reader, 'error')
        self.assertIn('rollback', message['message'])
        self.assertEqual(await asyncio.wait_for(reader.read(), 3), b'')

    async def test_fragmented_and_coalesced_messages_over_http_upgrade(self):
        reader, writer, _ = await self.connect(upgrade=True)
        writer.write(b'{"op":"cre')
        await writer.drain()
        writer.write(b'ate","name":"Split room"}\n{"op":"ping"}\n')
        room = await self.receive(reader, 'room')
        self.assertEqual(room['room']['name'], 'Split room')
        self.assertEqual(await self.receive(reader, 'pong'), {'op': 'pong'})

    async def test_malformed_requests_do_not_drop_the_connection(self):
        reader, writer, _ = await self.connect()
        for request in (b'{"op":"create","max":null}\n', b'{"op":"join","room":[]}\n'):
            writer.write(request)
            await self.receive(reader, 'error')
            writer.write(b'{"op":"ping"}\n')
            await self.receive(reader, 'pong')
        writer.write(b'not-json\n[1,2]\n' + b'[' * 1500 + b']' * 1500 + b'\n{"op":"ping"}\n')
        await self.receive(reader, 'pong')

    async def test_oversized_line_disconnects_cleanly(self):
        reader, writer, _ = await self.connect()
        writer.write(b'x' * (MAX_LINE_BYTES + 1) + b'\n')
        await writer.drain()
        self.assertEqual(await asyncio.wait_for(reader.read(), 3), b'')

    async def test_live_relay_match_end_and_rematch(self):
        host_reader, host_writer, host_id = await self.connect()
        guest_reader, guest_writer, guest_id = await self.connect()
        host_writer.write(b'{"op":"create","name":"Rematch"}\n')
        room_id = (await self.receive(host_reader, 'room'))['room']['id']
        guest_writer.write(json.dumps({'op': 'join', 'room': room_id}).encode() + b'\n')
        await self.receive(guest_reader, 'room')
        for writer in (host_writer, guest_writer):
            writer.write(b'{"op":"ready","ready":true}\n')
        # Wait for the lobby state that acknowledges both Ready commands.
        while True:
            room = (await self.receive(host_reader, 'room'))['room']
            if len(room['players']) == 2 and all(p['ready'] for p in room['players']):
                break
        host_writer.write(b'{"op":"start"}\n')
        start = await self.receive(host_reader, 'start')
        self.assertEqual(start, await self.receive(guest_reader, 'start'))
        packet = base64.b64encode(HEADER.pack(MAGIC, start['session'], host_id, guest_id) + b'\x00\x00').decode()
        host_writer.write(json.dumps({'op': 'r', 'd': packet}).encode() + b'\n')
        self.assertEqual((await self.receive(guest_reader, 'r'))['d'], packet)
        bye = base64.b64encode(HEADER.pack(MAGIC, start['session'], host_id, 0) + bytes([PKT_BYE, 0])).decode()
        host_writer.write(json.dumps({'op': 'r', 'd': bye}).encode() + b'\n')
        await self.receive(host_reader, 'ended')
        await self.receive(guest_reader, 'ended')
        room = (await self.receive(host_reader, 'room'))['room']
        self.assertFalse(any(p['ready'] for p in room['players']))
        for writer in (host_writer, guest_writer):
            writer.write(b'{"op":"ready","ready":1}\n')
        while True:
            room = (await self.receive(host_reader, 'room'))['room']
            if all(p['ready'] for p in room['players']):
                break
        host_writer.write(b'{"op":"start"}\n')
        rematch = await self.receive(host_reader, 'start')
        self.assertNotEqual(rematch['session'], start['session'])
        self.assertEqual(rematch, await self.receive(guest_reader, 'start'))


if __name__ == '__main__':
    unittest.main()
