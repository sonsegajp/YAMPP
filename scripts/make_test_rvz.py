"""Write a small RVZ image with known contents, to check the reader against.

The interesting part of the format is not the compression - that is a library
call - but the packed stream inside a group, where stretches of padding are
replaced by a seed and regenerated on read. This builds an image that uses both
stored bytes and regenerated padding, plus the expected disc contents, so the
reader can be compared against them byte for byte.

    python scripts/make_test_rvz.py build/rvz-test
"""
import struct, sys
from pathlib import Path

LFG_K, LFG_J, SEED_WORDS = 521, 32, 17
JUNK_BLOCK = 0x8000
CHUNK = 0x20000


class Lfg:
    """The same generator Dolphin uses for regenerated padding."""

    def __init__(self, seed_words):
        self.buf = list(seed_words) + [0] * (LFG_K - SEED_WORDS)
        for i in range(SEED_WORDS, LFG_K):
            self.buf[i] = ((self.buf[i - 17] << 23) ^ (self.buf[i - 16] >> 9) ^ self.buf[i - 1]) & 0xFFFFFFFF
        for i in range(LFG_K):
            x = self.buf[i]
            self.buf[i] = ((x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000)) & 0xFFFFFFFF
        self.bytes = bytearray()
        for i in range(4):
            self.step()
        self.position = 0

    def step(self):
        for i in range(LFG_J):
            self.buf[i] ^= self.buf[i + LFG_K - LFG_J]
        for i in range(LFG_J, LFG_K):
            self.buf[i] ^= self.buf[i - LFG_J]

    def _raw(self):
        return b''.join(struct.pack('>I', w) for w in self.buf)

    def skip(self, count):
        self.position += count
        while self.position >= LFG_K * 4:
            self.step()
            self.position -= LFG_K * 4

    def take(self, count):
        out = bytearray()
        while count > 0:
            block = self._raw()
            length = min(count, LFG_K * 4 - self.position)
            out += block[self.position:self.position + length]
            self.position += length
            count -= length
            if self.position == LFG_K * 4:
                self.step()
                self.position = 0
        return bytes(out)


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/rvz-test')
    out.mkdir(parents=True, exist_ok=True)

    seed_words = [0x13579BDF ^ (i * 0x01010101) & 0xFFFFFFFF for i in range(SEED_WORDS)]
    seed_bytes = b''.join(struct.pack('>I', w) for w in seed_words)

    # One chunk of disc: stored bytes, regenerated padding, stored bytes again.
    head = bytes((i * 7 + 3) & 0xFF for i in range(0x4000))
    padding_length = 0x18000
    lfg = Lfg(seed_words)
    lfg.skip(len(head) % JUNK_BLOCK)
    padding = lfg.take(padding_length)
    tail = bytes((i * 13 + 5) & 0xFF for i in range(CHUNK - len(head) - padding_length))
    disc = head + padding + tail
    assert len(disc) == CHUNK

    packed = (struct.pack('>I', len(head)) + head +
              struct.pack('>I', padding_length | 0x80000000) + seed_bytes +
              struct.pack('>I', len(tail)) + tail)

    # Tables are stored uncompressed here, which the reader supports and which
    # keeps this script free of a compression dependency.
    raw_entry = struct.pack('>QQII', 0, CHUNK, 0, 1)
    group_entry = struct.pack('>III', 0, len(packed), len(packed))   # not compressed

    header1_size, header2_size = 0x48, 0xDC
    group_at = header1_size + header2_size
    raw_at = group_at + len(group_entry)
    data_at = raw_at + len(raw_entry)

    header2 = bytearray(header2_size)
    struct.pack_into('>I', header2, 0x00, 1)          # disc type: GameCube
    struct.pack_into('>I', header2, 0x04, 0)          # compression: none
    struct.pack_into('>i', header2, 0x08, 0)
    struct.pack_into('>I', header2, 0x0C, CHUNK)
    header2[0x10:0x16] = b'GALE01'
    struct.pack_into('>I', header2, 0xB4, 1)          # one raw data entry
    struct.pack_into('>Q', header2, 0xB8, raw_at)
    struct.pack_into('>I', header2, 0xC0, len(raw_entry))
    struct.pack_into('>I', header2, 0xC4, 1)          # one group
    struct.pack_into('>Q', header2, 0xC8, group_at)
    struct.pack_into('>I', header2, 0xD0, len(group_entry))

    header1 = bytearray(header1_size)
    struct.pack_into('>I', header1, 0x00, 0x52565A01)  # "RVZ\1"
    struct.pack_into('>I', header1, 0x04, 0x01000000)
    struct.pack_into('>I', header1, 0x08, 0x00090000)
    struct.pack_into('>I', header1, 0x0C, header2_size)
    struct.pack_into('>Q', header1, 0x20, CHUNK)       # disc size

    image = out / 'test.rvz'
    with image.open('wb') as f:
        f.write(header1)
        f.write(header2)
        f.write(group_entry)
        f.write(raw_entry)
        f.write(packed)
        # The group's data offset is in 4-byte units, so place the payload there.
    with image.open('r+b') as f:
        f.seek(group_at)
        f.write(struct.pack('>III', data_at >> 2, len(packed), len(packed)))

    (out / 'expected.bin').write_bytes(disc)
    print('wrote %s (%d bytes) and expected.bin (%d bytes)' % (image, image.stat().st_size, len(disc)))
    print('padding covers 0x%X..0x%X' % (len(head), len(head) + padding_length))


if __name__ == '__main__':
    main()
