"""Prove relocated Workshop Test Game arguments without opening a game window."""
from pathlib import Path
import hashlib
import os
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

import lua_scripts
import project_config


class PackagedLaunch(unittest.TestCase):
    def setUp(self):
        parent = lua_scripts.ROOT / 'build/modkit/packaged-launch-check'
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for directory in ('bin', 'config', 'tools/python', 'data/GALE01/sys', 'user'):
            (self.root / directory).mkdir(parents=True, exist_ok=True)
        for name in ('bin/YAMPP.exe', 'bin/melee_aurora_yampp.dll', 'tools/python/python.exe'):
            (self.root / name).write_bytes(b'test fixture; never executed')
        self.dol = self.root / 'data/GALE01/sys/main.dol'
        self.dol.write_bytes(b'validated test extraction')
        (self.dol.parent / 'fst.bin').write_bytes(b'FST fixture')
        self.disc = self.root / 'my-own-disc.rvz'
        self.disc.write_bytes(b'RVZ\x01compressed fixture; no Python ISO reader should run')
        doc = ET.Element('melee-project', schema='1', discId='GALE01', revision='2', dolSha1=hashlib.sha1(self.dol.read_bytes()).hexdigest())
        ET.SubElement(doc, 'assets', directory='data/GALE01', manifest='data/GALE01/manifest.xml')
        ET.SubElement(doc, 'runtime', executable='bin/YAMPP.exe', developmentExecutable='bin/YAMPP.exe', renderer='bin/melee_aurora_yampp.dll', developmentRenderer='bin/melee_aurora_yampp.dll')
        ET.SubElement(doc, 'user', disc='user/disc.xml', settings='user/settings.xml', memoryCard='user/saves/card.raw', mods='user/mods', modsEnabled='0', cache='user/cache')
        ET.ElementTree(doc).write(self.root / 'config/project.xml')
        self.config_before = (self.root / 'config/project.xml').read_bytes()

    def test_relocated_rvz_and_explicit_local_mods(self):
        ET.ElementTree(ET.Element('disc', schema='1', path=str(self.disc))).write(self.root / 'user/disc.xml')
        with patch.object(lua_scripts, 'ROOT', self.root), patch.object(project_config, 'ROOT', self.root), \
                patch.object(lua_scripts, 'runtime_registry') as registry, \
                patch.object(lua_scripts.subprocess, 'Popen', return_value=SimpleNamespace(pid=123)) as launch, \
                patch.dict(os.environ, {'MELEE_TEST_INPUT': 'unsafe-for-manual', 'MELEE_NETPLAY_AUTO': 'host', 'MELEE_MODS': '0'}, clear=True):
            result = lua_scripts.launch()
            args, options = launch.call_args
            self.assertEqual(args[0], [str(self.root / 'bin/YAMPP.exe'), str(self.dol), '0'])
            self.assertEqual(options['cwd'], self.root)
            self.assertEqual(options['env']['MELEE_DISC'], str(self.disc))
            self.assertEqual(options['env']['MELEE_MODS'], '1')
            self.assertEqual(options['env']['MELEE_MOD_REGISTRY'], str(self.root / 'user/mods/registry.tsv'))
            self.assertEqual(options['env']['MELEE_COSTUME_REGISTRY'], str(self.root / 'user/mods/costumes.tsv'))
            self.assertEqual(options['env']['MELEE_WORKSHOP_PYTHON'], str(self.root / 'tools/python/python.exe'))
            self.assertTrue(options['env']['PATH'].startswith(str(self.root / 'bin') + os.pathsep))
            self.assertNotIn('MELEE_TEST_INPUT', options['env'])
            self.assertNotIn('MELEE_NETPLAY_AUTO', options['env'])
            self.assertEqual(result['pid'], 123)
            registry.assert_called_once()
        self.assertEqual((self.root / 'config/project.xml').read_bytes(), self.config_before)

    def test_disc_path_fallback_and_missing_setup(self):
        remembered = self.root / 'user/disc.path'
        with patch.object(lua_scripts, 'ROOT', self.root), patch.object(project_config, 'ROOT', self.root), \
                patch.object(lua_scripts, 'runtime_registry'), patch.object(lua_scripts.subprocess, 'Popen', return_value=SimpleNamespace(pid=123)) as launch:
            with self.assertRaisesRegex(ValueError, 'Setup.cmd'):
                lua_scripts.launch()
            launch.assert_not_called()
            remembered.write_text(str(self.disc), encoding='utf-8')
            self.assertTrue(lua_scripts.launch()['launched'])
            self.dol.write_bytes(b'changed extraction')
            with self.assertRaisesRegex(ValueError, 'does not match'):
                lua_scripts.launch()

    def test_source_checkout_keeps_source_launcher(self):
        with patch.object(lua_scripts, 'ROOT', self.root / 'source-checkout'), \
                patch.object(lua_scripts.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='source launch', stderr='')) as launch:
            self.assertTrue(lua_scripts.launch()['launched'])
            self.assertEqual(launch.call_args.args[0][-4:], ['--launch', 'game', '--development', '--enable-mods'])


if __name__ == '__main__':
    unittest.main()
