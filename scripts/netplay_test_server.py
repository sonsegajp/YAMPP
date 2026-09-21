"""Local test relay with deterministic input latency and reordering.

Uses the production server's validation and relay code. Only match input
messages are delayed; lobby messages and scene loading keep their normal path.
Latency is injected at the point where a packet is handed to its recipient, so
it applies equally to the stream and to the datagram side channel and the two
paths are exercised under the same conditions.

This is a test fixture, never the server used by Run-Netplay-Server.cmd.
"""
import argparse
import asyncio
import logging
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'server'))
from melee_netplay_server import HEADER, MAX_LINE_BYTES, Server, UdpRelay
from mod_repository import Repository


class ImpairedServer(Server):
    def __init__(self, delay_ms=0, jitter_ms=0, repository=None, mod_api="", stall_frame=0, stall_ms=0):
        super().__init__(repository, mod_api)
        self.delay_ms = delay_ms
        self.jitter_ms = jitter_ms
        self.delayed_packets = 0
        self.stall_frame, self.stall_ms = stall_frame, stall_ms
        self.stall_until = None

    def forward(self, client, data, payload=None):
        # RelayHeader + InputHeader. Epochs 1/2 are CSS/stage select in the
        # scripted integration test; epoch 3 is the first actual match.
        if (len(data) >= HEADER.size + 20 and data[HEADER.size] == 1
                and data[HEADER.size + 1] >= 3
                and (self.delay_ms or self.jitter_ms or self.stall_ms)):
            frame = struct.unpack_from('<I', data, HEADER.size + 4)[0]
            phase = (frame * 37 + client.id * 13) % 101
            delay = max(0, self.delay_ms + self.jitter_ms * (2 * phase / 100 - 1))
            if self.stall_ms and frame >= self.stall_frame:
                if self.stall_until is None:
                    self.stall_until = time.monotonic() + self.stall_ms / 1000
                    logging.info('Injected traffic stall: frame=%d duration=%dms', frame, self.stall_ms)
                delay = max(delay, (self.stall_until-time.monotonic())*1000)
            self.delayed_packets += 1
            if self.delayed_packets == 1 or self.delayed_packets % 100 == 0:
                logging.info('Injected latency: packets=%d delay=%.1fms frame=%d',
                             self.delayed_packets, delay, frame)
            # Production relay revalidates the client/session at delivery, so
            # delayed packets cannot cross a leave/rematch session boundary.
            asyncio.get_running_loop().call_later(
                delay / 1000, super().forward, client, data, payload)
            return
        return super().forward(client, data, payload)


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--delay-ms', type=int, default=0)
    parser.add_argument('--jitter-ms', type=int, default=0)
    parser.add_argument('--mods-dir', type=Path)
    parser.add_argument('--stall-frame', type=int, default=500)
    parser.add_argument('--stall-ms', type=int, default=0)
    parser.add_argument('--udp-port', type=int, default=-1,
                        help='datagram relay port (default: --port; 0 keeps the run on the stream)')
    args = parser.parse_args()
    if not (0 <= args.delay_ms <= 1000 and 0 <= args.jitter_ms <= 1000):
        parser.error('latency and jitter must be between 0 and 1000 ms')
    if not (0 <= args.stall_ms <= 10000 and args.stall_frame >= 0):
        parser.error('invalid traffic stall')
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s')
    server = ImpairedServer(args.delay_ms, args.jitter_ms, Repository(args.mods_dir) if args.mods_dir else None, 'http://127.0.0.1:%d/api' % args.port, args.stall_frame, args.stall_ms)
    udp_port = args.port if args.udp_port < 0 else args.udp_port
    udp = None
    if udp_port:
        loop = asyncio.get_running_loop()
        udp, _ = await loop.create_datagram_endpoint(lambda: UdpRelay(server),
                                                     local_addr=(args.bind, udp_port))
        server.udp_port = udp_port
    tcp = await asyncio.start_server(server.serve_client, args.bind, args.port,
                                    limit=MAX_LINE_BYTES)
    logging.info('Test relay listening: delay=%dms jitter=%dms transport=%s',
                 args.delay_ms, args.jitter_ms, 'stream+datagram' if udp_port else 'stream')
    try:
        async with tcp:
            await tcp.serve_forever()
    finally:
        if udp is not None:
            udp.close()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
