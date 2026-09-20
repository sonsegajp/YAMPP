import struct
import unittest
from extract_disc import archive_info

def fixture(site=2, target=0):
    # Packed pointer at byte 2: real Melee archives contain unaligned relocations.
    body = bytearray(8)
    struct.pack_into(">I", body, 2, target)
    name = b"root\0"
    size = 32 + len(body) + 4 + 8 + len(name)
    header = struct.pack(">5I", size, len(body), 1, 1, 0) + bytes(12)
    return header + body + struct.pack(">3I", site, 0, 0) + name

class ArchiveValidation(unittest.TestCase):
    def test_packed_relocation_and_symbol(self):
        result = archive_info(fixture())
        self.assertEqual(result["relocations"], 1)
        self.assertEqual(result["symbols"], [{"name": "root", "offset": 0, "external": False}])

    def test_relocation_out_of_bounds(self):
        with self.assertRaisesRegex(ValueError, "relocation site"):
            archive_info(fixture(site=6))

    def test_target_out_of_bounds(self):
        with self.assertRaisesRegex(ValueError, "relocation target"):
            archive_info(fixture(target=9))

    def test_symbol_without_terminator(self):
        data = fixture()[:-1] + b"x"
        with self.assertRaisesRegex(ValueError, "symbol string"):
            archive_info(data)

    def test_tables_out_of_bounds(self):
        data = bytearray(fixture())
        struct.pack_into(">I", data, 8, 0xffffffff)
        with self.assertRaisesRegex(ValueError, "tables exceed"):
            archive_info(data)

if __name__ == "__main__":
    unittest.main()
