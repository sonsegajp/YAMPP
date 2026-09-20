"""Compare costume metadata with the shipped game's actual native tables."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import catalog
import community


class NativeCostumeParity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        raw = (catalog.ROOT / "data/GALE01/sys/main.dol").read_bytes()
        cls.ram = bytearray(24 << 20)
        word = lambda at: struct.unpack_from(">I", raw, at)[0]
        for index in range(18):
            offset, address, size = word(index * 4), word(0x48 + index * 4), word(0x90 + index * 4)
            if size:
                address &= 0x1ffffff
                cls.ram[address:address + size] = raw[offset:offset + size]

    def word(self, address):
        return struct.unpack_from(">I", self.ram, address & 0x1ffffff)[0]

    def string(self, address):
        address &= 0x1ffffff
        return bytes(self.ram[address:self.ram.index(0, address)]).decode("ascii")

    def test_all_26_stock_orders_and_original_roots(self):
        self.assertEqual(len(catalog.NAMES), 26)
        for external, (name, prefix, internal) in enumerate(catalog.NAMES):
            with self.subTest(fighter=name):
                count = self.ram[(0x803c0ec0 + internal * 8 + 4) & 0x1ffffff]
                self.assertEqual(count, self.ram[(0x803d51a0 + external * 4) & 0x1ffffff])
                table = self.word(0x803c2360 + internal * 4)
                native = [self.string(self.word(table + color * 12)) for color in range(count)]
                # Falcon red's suffix is filled by the game's language selector.
                # The model's original color remains native slot two in either locale.
                native = ["PlCaRe.dat" if item == "PlCaRe." else item for item in native]
                self.assertEqual(catalog.COSTUME_ORDER[prefix], native)
                for filename in set(native):
                    self.assertEqual(catalog.costume_reference(prefix, catalog.ASSETS / filename), filename)
                for costume in catalog.costumes(prefix):
                    self.assertEqual(costume["index"], native.index(costume["file"]))

    def test_native_registry_without_site_packages(self):
        work = catalog.ROOT / "build/modkit/catalog-stdlib-check"
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as folder:
            code = """import sys, pathlib, xml.etree.ElementTree as ET
sys.path.insert(0, 'tools/modkit')
import catalog
catalog.MODS = pathlib.Path(sys.argv[1])
icons = catalog.MODS / 'catalog'
icons.mkdir()
for entry in ET.parse(catalog.ROOT / 'config/character-assets.xml').getroot():
    (icons / (entry.get('prefix') + '.rgba')).write_bytes(b'cached-icon')
catalog.runtime_registry()
assert sum(row.startswith('F\\t') for row in (catalog.MODS / 'registry.tsv').read_text().splitlines()) == 26
assert not any(name == 'PIL' or name.startswith('PIL.') for name in sys.modules)
"""
            subprocess.run([sys.executable, "-S", "-c", code, folder], cwd=catalog.ROOT, check=True, capture_output=True)

    def test_gamewatch_limit_counts_four_slots_sharing_one_archive(self):
        self.assertEqual(len(catalog.costumes("Gw")), 1)
        self.assertEqual(len(catalog.COSTUME_ORDER["Gw"]), 4)
        with patch.object(community, "package_folders", return_value=[]):
            community.check_slot_limit({"base": "stock-Gw", "costumes": [{}] * 60})
            with self.assertRaisesRegex(ValueError, "at most 64"):
                community.check_slot_limit({"base": "stock-Gw", "costumes": [{}] * 61})


if __name__ == "__main__":
    unittest.main()
