"""Contract integration check using real stock DATs and the strict repository."""
from pathlib import Path
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import struct
import zlib

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/modkit/community-check'
WORK.mkdir(parents=True,exist_ok=True)
TEMP=tempfile.TemporaryDirectory(prefix='run-',dir=WORK)
TEST=Path(TEMP.name)
os.environ['MELEE_WORKSHOP_TEST_MODS']=str(TEST/'mods')
os.environ['MELEE_MOD_UPLOAD_TOKEN']='local-community-integration-test'
sys.path.insert(0,str(ROOT/'server'))
import community
import service
from mod_repository import Repository


class Integration(unittest.TestCase):
    def test_visual_payload_integrity(self):
        folder=service.MODS/'visual-payload-check';(folder/'files').mkdir(parents=True,exist_ok=True);(folder/'art').mkdir()
        source=service.ASSETS/'PlLkNr.dat';(folder/'files/PlLkFD.dat').write_bytes(source.read_bytes())
        def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
        png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',24,24,8,6,0,0,0))+chunk(b'IDAT',zlib.compress((b'\0'+bytes([255,255,255,255])*24)*24))+chunk(b'IEND',b'')
        for name in ['portrait.png','stock.png']:(folder/'art'/name).write_bytes(png)
        gxts={'portrait.gxt':b'YAMPPTEX'+struct.pack('>4I',136,188,14,13056)+bytes(13056),'stock.gxt':b'YAMPPTEX'+struct.pack('>4I',24,24,14,288)+bytes(288)}
        for name,raw in gxts.items():(folder/'art'/name).write_bytes(raw)
        manifest={'schema':1,'kind':'costume','id':folder.name,'name':'Visual payload check','version':'1.0.1','base':'stock-Lk','additive':True,'enabled':False,'costumes':[{'id':'white','name':'White','file':'files/PlLkFD.dat','portrait':'art/portrait.png','stockIcon':'art/stock.png','portraitTexture':'art/portrait.gxt','stockIconTexture':'art/stock.gxt'}]}
        service.write_manifest(folder,manifest)
        prepared=community.prepare({'id':folder.name});raw=(community.CACHE/'community/prepared'/(prepared['sha256']+'.zip')).read_bytes()
        inspect,validate=community.validator();public,files=inspect(raw);metadata=validate(raw)
        community.stamp_install(folder,manifest,files,prepared['sha256'],len(raw))
        self.assertIsNotNone(community.verified_install(folder))
        slots=[row for row in service.costume_slots() if row['packid']==folder.name];self.assertEqual(len(slots),1)
        for field,name in [('portraitTexture','portrait.gxt'),('stockIconTexture','stock.gxt')]:
            self.assertEqual(slots[0][field],str((folder/'art'/name).resolve()));self.assertEqual(slots[0][field+'Sha256'],hashlib.sha256(gxts[name]).hexdigest())
        with patch.object(community,'download_verified',return_value=(metadata,raw,public,files)),patch.object(community,'INSPECTION_ROOT',TEST/'visual-inspection'):
            result=community.inspect_package(prepared['sha256'],0)
            self.assertEqual(Path(result['previewPath']).read_bytes(),png);self.assertEqual(Path(result['stockIconPath']).read_bytes(),png)
            self.assertEqual(result['previewSha256'],hashlib.sha256(png).hexdigest());self.assertEqual(result['stockIconSha256'],hashlib.sha256(png).hexdigest())
            self.assertEqual(Path(result['portraitTexturePath']).read_bytes(),gxts['portrait.gxt']);self.assertEqual(Path(result['stockIconTexturePath']).read_bytes(),gxts['stock.gxt'])
            Path(result['stockIconPath']).write_bytes(png+b'bad')
            with self.assertRaisesRegex(ValueError,'integrity'):community.inspect_package(prepared['sha256'],0)
        for name,raw in {'portrait.png':png,'stock.png':png,**gxts}.items():
            path=folder/'art'/name;path.write_bytes(raw+b'bad');self.assertIsNone(community.verified_install(folder));path.write_bytes(raw)
        self.assertIsNotNone(community.verified_install(folder))

    def test_round_trip(self):
        repository=Repository(TEST/'repository')
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def respond(self,doc,status=200):
                raw=json.dumps(doc).encode();self.send_response(status);self.send_header('Content-Type','application/json');self.end_headers();self.wfile.write(raw)
            def do_GET(self):
                parts=self.path.split('/')
                if self.path=='/api/mods':return self.respond(repository.catalog())
                sha=parts[3] if len(parts)>3 else ''
                item=repository.get(sha)
                if not item:return self.respond({'error':'missing'},404)
                if self.path.endswith('/package.zip'):
                    raw=repository.package_path(sha).read_bytes();self.send_response(200);self.end_headers();self.wfile.write(raw)
                else:self.respond(item)
            def do_POST(self):
                if self.headers.get('Authorization')!='Bearer local-community-integration-test':return self.respond({'error':'unauthorized'},401)
                raw=self.rfile.read(int(self.headers['Content-Length']))
                metadata,_=repository.publish(raw);self.respond(metadata,201)
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        threading.Thread(target=server.serve_forever,daemon=True).start()
        os.environ['MELEE_MOD_REPOSITORY_URL']='http://127.0.0.1:%d/api/mods'%server.server_port
        try:
            stock=service.get_fighter('stock-Lk')
            service.runtime_registry()
            stock_rows=(service.MODS/'registry.tsv').read_text().splitlines()
            self.assertEqual(len([r for r in stock_rows if r.startswith('F\t')]),26)
            before={c['file']:hashlib.sha256((service.ASSETS/c['file']).read_bytes()).hexdigest() for c in stock['costumes']}
            draft=service.mutate('/api/costume/clone',{'base':'stock-Lk','name':'Community Integration','costumes':[c['file'] for c in stock['costumes'][:4]]})
            self.assertEqual(draft['kind'],'costume');self.assertEqual(len(draft['costumes']),4)
            self.assertEqual((service.MODS/'registry.tsv').read_text().splitlines(),stock_rows)
            slots=service.costume_slots();self.assertEqual([c['baseColor'] for c in slots],[0,1,2,3]);self.assertTrue(all(c['sha256']=='-' for c in slots))
            registry=(service.MODS/'costumes.tsv').read_text().splitlines();self.assertEqual(registry[0],'MELEE_COSTUMES\t3');self.assertEqual(len(registry),5);self.assertTrue(all(len(row.split('\t'))==16 and row.split('\t')[12:]==['-']*4 for row in registry[1:]))
            view=service.get_fighter(draft['id']);self.assertEqual(view['kind'],'costume')
            self.assertEqual(service.asset_path(view,'PlLk.dat'),service.ASSETS/'PlLk.dat')
            with self.assertRaisesRegex(ValueError,'visual assets only'):
                service.mutate('/api/fighter/save',{'id':draft['id'],'attributes':{}})
            prepared=community.prepare({'id':draft['id']})
            self.assertEqual(prepared['sha256'],community.prepare({'id':draft['id']})['sha256'])
            self.assertNotIn('enabled',prepared['manifest'])
            published=community.upload(prepared['preparedId']);self.assertTrue(published['published'])
            rows=community.catalog()['mods'];self.assertEqual(len(rows),1);self.assertTrue(rows[0]['installed'])
            community.INSPECTION_ROOT=TEST/'inspection'
            mods_before={str(p.relative_to(service.MODS)):hashlib.sha256(p.read_bytes()).hexdigest() for p in service.MODS.rglob('*') if p.is_file()}
            inspection=community.inspect_package(prepared['sha256'],2)
            self.assertEqual(inspection['costumeCount'],4);self.assertEqual(inspection['costumeIndex'],2)
            self.assertEqual(inspection['baseColor'],2);self.assertEqual(inspection['internalKind'],6)
            self.assertIsNone(inspection['previewPath']);self.assertEqual(len(inspection['costumes']),4)
            self.assertEqual(Path(inspection['datPath']).read_bytes(),(service.ASSETS/stock['costumes'][2]['file']).read_bytes())
            self.assertEqual(inspection['datSha256'],hashlib.sha256(Path(inspection['datPath']).read_bytes()).hexdigest())
            self.assertEqual(community.inspect_package(prepared['sha256'],2),inspection)
            with self.assertRaisesRegex(ValueError,'costume index'):community.inspect_package(prepared['sha256'],4)
            with self.assertRaisesRegex(ValueError,'costume index'):community.inspect_package(prepared['sha256'],-1)
            modified=Path(inspection['datPath']);clean=modified.read_bytes();modified.write_bytes(clean[:-1]+bytes([clean[-1]^1]))
            with self.assertRaisesRegex(ValueError,'integrity verification'):community.inspect_package(prepared['sha256'],2)
            modified.write_bytes(clean)
            cached_manifest=Path(inspection['cachePath'])/'manifest.xml';clean=cached_manifest.read_bytes();cached_manifest.write_bytes(clean+b' ')
            with self.assertRaisesRegex(ValueError,'integrity verification'):community.inspect_package(prepared['sha256'],2)
            cached_manifest.write_bytes(clean)
            self.assertEqual(mods_before,{str(p.relative_to(service.MODS)):hashlib.sha256(p.read_bytes()).hexdigest() for p in service.MODS.rglob('*') if p.is_file()})

            (service.MODS/draft['id']/'install.json').unlink()
            with self.assertRaisesRegex(ValueError,'different local package'):
                community.install(prepared['sha256'])
            original=service.MODS/draft['id'];original.rename(TEST/'author-draft')
            installed=community.install(prepared['sha256']);self.assertEqual(installed['costumeCount'],4)
            self.assertTrue(community.catalog()['mods'][0]['installed'])
            self.assertTrue(community.install(prepared['sha256'])['alreadyInstalled'])
            self.assertEqual(community.active()['sha256s'],[prepared['sha256']])
            self.assertFalse(community.toggle(prepared['sha256'],False)['enabled'])
            self.assertEqual(community.active()['sha256s'],[])
            disabled=service.costume_slots();self.assertEqual(len(disabled),4);self.assertTrue(all(not c['enabled'] for c in disabled));self.assertTrue(all(c['sha256']==prepared['sha256'] for c in disabled))
            self.assertTrue(community.toggle(prepared['sha256'],True)['enabled'])
            self.assertEqual(community.active()['sha256s'],[prepared['sha256']])
            second=service.clone_costumes({'base':'stock-Lk','name':'ZZZ Disabled Order Check','costumes':[stock['costumes'][0]['file']]})
            service.mutate('/api/package/enabled',{'id':second['id'],'enabled':False})
            ordered=service.costume_slots();self.assertEqual([r['packid'] for r in ordered],[second['id']]+[draft['id']]*4)
            self.assertEqual([r['slotid'] for r in ordered[1:]],[c['id'] for c in draft['costumes']])
            first=original/draft['costumes'][0]['file'];raw=first.read_bytes();first.write_bytes(raw[:31]+bytes([raw[31]^1])+raw[32:])
            self.assertIsNone(community.verified_install(original))
            self.assertTrue(all(c['sha256']=='-' for c in service.costume_slots()))
            self.assertEqual((service.MODS/'registry.tsv').read_text().splitlines(),stock_rows)
            self.assertFalse(community.catalog()['mods'][0]['installed'])
            with self.assertRaisesRegex(ValueError,'must be published or downloaded'):community.active()
            self.assertEqual(before,{c['file']:hashlib.sha256((service.ASSETS/c['file']).read_bytes()).hexdigest() for c in stock['costumes']})
            print('Stock Link files unchanged; four additive slots; deterministic prepare; upload/list/install; collision and modified-content checks passed.')
        finally:
            server.shutdown();server.server_close()


if __name__=='__main__':
    try:unittest.main()
    finally:TEMP.cleanup()
