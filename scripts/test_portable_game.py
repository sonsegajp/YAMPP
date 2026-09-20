"""Portable launch regression: relocated paths, compressed discs and verified reload plans."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch, Mock
import xml.etree.ElementTree as ET

import project_config
import play_yampp


class PortableGame(unittest.TestCase):
    def setUp(self):
        parent=project_config.ROOT/'build/tests/portable-game';parent.mkdir(parents=True,exist_ok=True)
        temp=tempfile.TemporaryDirectory(prefix='relocated game ',dir=parent)
        self.addCleanup(temp.cleanup);self.root=Path(temp.name)
        for directory in ('bin','config','data/GALE01/sys','user','scripts'):
            (self.root/directory).mkdir(parents=True,exist_ok=True)
        for file in ('bin/YAMPP.exe','bin/YAMPP.core.dll','bin/renderer.dll','data/GALE01/sys/fst.bin'):
            (self.root/file).write_bytes(b'fixture only')
        self.dol=self.root/'data/GALE01/sys/main.dol';self.dol.write_bytes(b'original validated DOL')
        self.disc=self.root/'my disc.rvz';self.disc.write_bytes(b'compressed fixture - must not use ISO reader')
        (self.root/'user/disc.path').write_text(str(self.disc),encoding='utf-8-sig')
        self.settings=self.root/'user/settings.xml';self.settings.write_bytes(b'preserve user settings')
        self.content=self.root/'user/content-mods.json';self.content.write_bytes(b'preserve content selection')
        doc=ET.Element('melee-project',schema='1',dolSha1=hashlib.sha1(self.dol.read_bytes()).hexdigest())
        ET.SubElement(doc,'assets',directory='data/GALE01')
        ET.SubElement(doc,'runtime',executable='bin/YAMPP.exe',renderer='bin/renderer.dll',contentReload='1')
        ET.SubElement(doc,'user',disc='user/disc.xml',settings='user/settings.xml',memoryCard='user/card.raw',mods='user/mods',cache='user/cache',modsEnabled='0')
        ET.ElementTree(doc).write(self.root/'config/project.xml')
        for target in (project_config,play_yampp):
            mock=patch.object(target,'ROOT',self.root);mock.start();self.addCleanup(mock.stop)
        self.child=Mock(pid=123);self.child.wait.side_effect=subprocess.TimeoutExpired('test',2)
        self.spawn=patch.object(play_yampp.subprocess,'Popen',return_value=self.child).start();self.addCleanup(patch.stopall)
        self.prepare=patch.object(play_yampp.subprocess,'run',return_value=SimpleNamespace(returncode=0)).start()

    def test_relocated_rvz_uses_host_plan_and_preserves_user_state(self):
        with patch.dict(os.environ,{'MELEE_CONTENT_STATE':'wrong','MELEE_MEX_BASE_DOL':'wrong',
                                   'MELEE_INPUT':'unsafe replay','GCN_AURORA_HIDDEN':'1','MELEE_NETPLAY_AUTO':'host','MELEE_MODS':'1'}):
            result=play_yampp.launch()
        args,kw=self.spawn.call_args
        self.assertEqual(args[0],[str(self.root/'bin/YAMPP.exe'),str(self.dol),'0'])
        env=kw['env']
        self.assertEqual(env['MELEE_DISC'],str(self.disc))
        self.assertEqual(env['MELEE_CONTENT_LAUNCHER'],'1')
        self.assertTrue(Path(env['MELEE_CONTENT_BOOT_PLAN']).is_relative_to(self.root/'build/play'))
        self.assertEqual(self.prepare.call_args.kwargs['env'],env)
        for key in ('MELEE_CONTENT_STATE','MELEE_MEX_BASE_DOL','MELEE_INPUT','GCN_AURORA_HIDDEN','MELEE_NETPLAY_AUTO','MELEE_MODS'):
            self.assertNotIn(key,env)
        self.assertEqual(self.settings.read_bytes(),b'preserve user settings')
        self.assertEqual(self.content.read_bytes(),b'preserve content selection')
        self.assertEqual(result['pid'],123)

    def test_missing_core_fails_before_launch(self):
        (self.root/'bin/YAMPP.core.dll').unlink()
        with self.assertRaisesRegex(ValueError,'core is missing'):play_yampp.launch()
        self.prepare.assert_not_called();self.spawn.assert_not_called()

    def test_failed_content_plan_does_not_open_wrong_game(self):
        self.prepare.return_value.returncode=1
        with self.assertRaisesRegex(ValueError,'prepare installed content'):play_yampp.launch()
        self.spawn.assert_not_called()

    def test_wrong_disc_extraction_does_not_launch(self):
        self.dol.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'does not match'):play_yampp.launch()
        self.spawn.assert_not_called()

    def test_immediate_native_failure_is_reported(self):
        self.child.wait.side_effect=None;self.child.wait.return_value=2
        with self.assertRaisesRegex(ValueError,'could not start'):play_yampp.launch()


if __name__=='__main__':unittest.main()
