"""Actual bytes are hashed anew, independently of install path and timestamps."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compatibility import fingerprint, RUNTIME_ASPECT_DOMAIN, RUNTIME_IDENTITY_SCHEMA


class FingerprintTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.records = []
        for index, key in enumerate(("runtime", "sys/main.dol", "sys/fst.bin", "disc-image", "files/PlLkNr.dat")):
            path = self.directory / str(index)
            path.write_bytes((key + " original bytes").encode())
            self.records.append(dict(key=key, path=str(path)))
        self.doc = dict(schema=1, files=self.records, expected_dol_sha1=hashlib.sha1(Path(self.records[1]["path"]).read_bytes()).hexdigest())

    def test_install_paths_and_enumeration_order_do_not_change_identity(self):
        expected = fingerprint(self.doc)
        relocated = []
        for index, record in enumerate(reversed(self.records)):
            path = self.directory / ("relocated" + str(index))
            path.write_bytes(Path(record["path"]).read_bytes())
            relocated.append(dict(key=record["key"], path=str(path)))
        self.assertEqual(fingerprint({**self.doc, "files": relocated}), expected)

    def test_changed_bytes_with_restored_timestamp_are_detected(self):
        expected = fingerprint(self.doc)
        path = Path(self.records[-1]["path"])
        previous = path.stat()
        value = path.read_bytes()
        path.write_bytes(value[:-1] + b"!")
        os.utime(path, ns=(previous.st_atime_ns, previous.st_mtime_ns))
        actual = fingerprint(self.doc)
        self.assertEqual(actual["runtime"], expected["runtime"])
        self.assertNotEqual(actual["game"], expected["game"])
        self.assertNotEqual(actual["fingerprint"], expected["fingerprint"])

    def test_native_build_changes_identity_separately(self):
        expected = fingerprint(self.doc)
        Path(self.records[0]["path"]).write_bytes(b"different native build")
        actual = fingerprint(self.doc)
        self.assertNotEqual(actual["runtime"], expected["runtime"])
        self.assertEqual(actual["game"], expected["game"])
        self.assertNotEqual(actual["fingerprint"], expected["fingerprint"])

    def test_aspect_is_domain_separated_without_changing_verified_byte_identity(self):
        actual = fingerprint(self.doc)
        for wide, key in ((0, "aspect4x3"), (1, "aspect16x9")):
            identity = hashlib.sha256(RUNTIME_ASPECT_DOMAIN + bytes.fromhex(actual["runtimeFileSha256"]) + bytes([wide])).hexdigest()
            self.assertEqual(actual["runtimeIdentity16x9" if wide else "runtimeIdentity4x3"], identity)
            expected = hashlib.sha256(b"MeleePC-compatibility-v1\0" + bytes.fromhex(identity) + bytes.fromhex(actual["game"])).hexdigest()
            self.assertEqual(actual[key], expected)
            self.assertNotEqual(actual[key], actual["fingerprint"])
        self.assertEqual(actual["runtimeFileSha256"], actual["runtime"])
        self.assertEqual(actual["runtimeIdentitySchema"], RUNTIME_IDENTITY_SCHEMA)
        self.assertNotEqual(actual["aspect4x3"], actual["aspect16x9"])
        Path(self.records[-1]["path"]).write_bytes(b"different base data")
        changed = fingerprint(self.doc)
        self.assertNotEqual(changed["aspect4x3"], actual["aspect4x3"])
        self.assertNotEqual(changed["aspect16x9"], actual["aspect16x9"])

    def test_both_aspect_identities_pass_actual_relay_validator(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "server"))
        from melee_netplay_server import clean_compatibility
        actual = fingerprint(self.doc)
        for label in ("4x3", "16x9"):
            wire = dict(schema=1, fingerprint=actual["aspect" + label],
                        runtime=actual["runtimeIdentity" + label], game=actual["game"])
            self.assertEqual(clean_compatibility(wire), wire)
            with self.assertRaisesRegex(ValueError, "Inconsistent"):
                clean_compatibility({**wire, "runtime": actual["runtimeFileSha256"]})

    def test_upstream_code_engine_and_base_are_all_verified(self):
        original=Path(self.records[1]["path"]).read_bytes()
        baseline=self.directory/"baseline.dol";baseline.write_bytes(original)
        engine=self.directory/"engine.dll";engine.write_bytes(b"execution engine")
        doc={**self.doc,"base_dol_key":"baseline/main.dol","files":self.records+[
            {"key":"baseline/main.dol","path":str(baseline)},
            {"key":"runtime/mex-engine","path":str(engine)}]}
        Path(self.records[1]["path"]).write_bytes(b"upstream code")
        first=fingerprint(doc)
        Path(self.records[1]["path"]).write_bytes(b"different upstream code")
        second=fingerprint(doc);self.assertNotEqual(first["fingerprint"],second["fingerprint"])
        engine.write_bytes(b"changed execution engine")
        self.assertNotEqual(second["fingerprint"],fingerprint(doc)["fingerprint"])
        baseline.write_bytes(b"wrong base revision")
        with self.assertRaisesRegex(ValueError,"pinned Melee revision"):fingerprint(doc)
        with self.assertRaisesRegex(ValueError,"Pinned base executable"):
            fingerprint({**doc,"files":self.records})

    def test_reloadable_core_bytes_are_part_of_online_identity(self):
        core = self.directory / "game.core.dll"
        core.write_bytes(b"first game core")
        doc = {**self.doc, "files": self.records + [{"key": "runtime/native-core", "path": str(core)}]}
        before = fingerprint(doc)
        core.write_bytes(b"second game core")
        self.assertNotEqual(before["aspect4x3"], fingerprint(doc)["aspect4x3"])
        self.assertNotEqual(before["aspect16x9"], fingerprint(doc)["aspect16x9"])
        self.assertNotEqual(before["fingerprint"], fingerprint(self.doc)["fingerprint"])

    def test_pinned_dol_and_duplicate_keys_are_required(self):
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            fingerprint({**self.doc, "files": self.records + [self.records[0]]})
        Path(self.records[1]["path"]).write_bytes(b"unexpected revision")
        with self.assertRaisesRegex(ValueError, "pinned Melee revision"):
            fingerprint(self.doc)


if __name__ == "__main__":
    unittest.main()
