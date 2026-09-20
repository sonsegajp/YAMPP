"""Exercise real browser/lobby controls against a temporary localhost server.

Captures include the finalized ImGui draw commands (MELEE_CAPTURE_UI=1).
No production server or player settings are used. Run after building both the
runtime and development renderer: python scripts/check_netplay_ui.py
"""
import argparse, json, os, re, shutil, socket, subprocess, sys, threading, time
from pathlib import Path
from project_config import ROOT, configuration, disc_path, project_path
from check_menu_additions import OPTIONS_BOOT, contact_sheet

class Fixture:
    def __init__(self, port, name, room=None, follow=False, rules=None, compatibility=None, upstream_build=None):
        self.sock=socket.create_connection(('127.0.0.1',port),5)
        self.sock.settimeout(.2); self.stop=False; self.follow=follow; self.name=name; self.room=0; self.joining=0; self.messages=[]
        self.send(op='hello',name=name,version=2,sync='rollback-v1',features=(['compat-v1'] if compatibility else [])+(['upstream-builds-v1'] if upstream_build else []),compatibility=compatibility,upstream_build=upstream_build)
        if room:self.send(op='create',name=room,max=2,rules=rules or {'stock':4,'minutes':8,'items':0})
        self.thread=threading.Thread(target=self.listen,daemon=True); self.thread.start()
    def send(self,**obj):
        try:self.sock.sendall((json.dumps(obj)+'\n').encode())
        except OSError:pass
    def listen(self):
        buf=b'';next_ping=time.monotonic()+10
        while not self.stop:
            if time.monotonic()>=next_ping:
                self.send(op='ping');next_ping=time.monotonic()+10
            try:data=self.sock.recv(8192)
            except socket.timeout:continue
            except OSError:break
            if not data:break
            buf+=data
            while b'\n' in buf:
                line,buf=buf.split(b'\n',1)
                try:m=json.loads(line)
                except ValueError:continue
                self.messages.append(m)
                if m.get('op')=='room':
                    room=m.get('room'); self.room=room['id'] if room else 0
                    if room:
                        if self.follow and not any(p.get('name')=='UI Tester' for p in room['players']):
                            self.send(op='leave'); self.joining=0
                        elif not any(p.get('name')==self.name and p.get('ready') for p in room['players']):self.send(op='ready',ready=True)
                    else:self.joining=0
                if self.follow and m.get('op')=='rooms' and not self.room and not self.joining:
                    rooms=[r for r in m.get('rooms',[]) if r.get('name')=='UI Tester' and r['players']<r['max']]
                    if rooms:self.joining=rooms[-1]['id'];self.send(op='join',room=self.joining)
    def close(self):
        self.stop=True
        try:self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:pass
        self.sock.close();self.thread.join(2)

def run(wide,base,port,frames,expected_join_room,room_observer):
    out=base/('16x9' if wide else '4x3');out.mkdir(parents=True,exist_ok=True)
    for old in out.glob('frame_*.ppm'):old.unlink()
    for name in ('browser','guest-ready','host-ready','rules','created-from-browser'):
        (out/(name+'.png')).unlink(missing_ok=True)
    cfg=configuration();env=os.environ.copy()
    for key in list(env):
        if key.startswith(('MELEE_TEST_','MELEE_CAPTURE_','MELEE_NETPLAY_','MELEE_RENDERDOC_','MELEE_TRACE_','MELEE_AUDIO_')) or key in ('MELEE_MODS','MELEE_MEMORY_CARD','MELEE_INPUT'):env.pop(key)
    settings=out/'settings.xml'; settings.write_text('<melee-settings schema="1" width="%d" height="720" renderScale="1" widescreen="%d" fullscreen="0" vsync="0" volume="100" mute="1" showFps="0" />'% (1280 if wide else 960,wide))
    template=Path(os.environ.get('MELEE_UI_TEMPLATE_CARD',str(ROOT/'build/netplay-work/template-card.raw')))
    if template.exists():shutil.copy2(template,out/'card.raw')
    timeline=OPTIONS_BOOT+',1080:100:8,1240:4:8,1360:100:8,1480:800:8,1600:4:8,1720:100:8,1840:400:8,1960:200:8,2080:100:8,2180:100:8,2240:400:30,2360:4:8,2480:100:8,2540:100:8,2600:2:8,2720:4:8,2840:2:8,2960:200:8,3080:4:8,3200:100:8,3320:4:8,3440:100:8,3560:400:8,3640:100:8,3820:400:8'
    # Allow the native menu's entry animation and save indicator to finish.
    boot_count=len(OPTIONS_BOOT.split(','))
    parts=timeline.split(','); timeline=','.join(parts[:boot_count]+[str(int(p.split(':',1)[0])+240+(360 if int(p.split(':',1)[0])>=2240 else 0))+':'+p.split(':',1)[1] for p in parts[boot_count:]])
    env.update(MELEE_DISC=str(disc_path(cfg)),MELEE_FST=str(project_path(cfg['assets']['directory'])/'sys/fst.bin'),MELEE_AURORA_DLL=os.environ.get('MELEE_TEST_RENDERER',str(project_path(cfg['runtime']['developmentRenderer']))),GCN_AURORA_HIDDEN='1',GCN_AURORA_DATA_DIR=str(out/'cache'),MELEE_MEMORY_CARD=str(out/'card.raw'),MELEE_SETTINGS=str(settings),MELEE_INPUT=timeline,MELEE_NETPLAY_SERVER='127.0.0.1:%d'%port,MELEE_NETPLAY_NAME='UI Tester',MELEE_TEST_MENU='10:0',MELEE_TEST_FAST_EXIT='1',MELEE_TEST_UI_TRACE='1',MELEE_CAPTURE_UI='1',MELEE_CAPTURE_AURORA='1',MELEE_CAPTURE_DIR=str(out))
    mods=ROOT/'build/modkit'/base.name/out.name; mods.mkdir(parents=True,exist_ok=True)
    if any(mods.iterdir()): raise ValueError('Use a fresh isolated UI test folder')
    env.update(MELEE_WORKSHOP_TEST_MODS=str(mods),MELEE_COSTUME_REGISTRY=str(mods/'costumes.tsv'),MELEE_MOD_REGISTRY=str(mods/'registry.tsv'),MELEE_MOD_REPOSITORY_URL='http://127.0.0.1:%d/api/mods'%port)
    subprocess.run([sys.executable,'-c','import sys;sys.path.insert(0,"tools/modkit");from catalog import runtime_registry;runtime_registry()'],cwd=ROOT,env=env,check=True,stdout=subprocess.DEVNULL)
    events=out/'text-input.csv';events.write_text('1,wait_name,0,0\n5,text,UI Tester\n100,wait_name,0,0\n5,text,UI Tester\n',encoding='utf-8')
    env['MELEE_TEST_SDL_INPUT']=str(events)
    env['PATH']=str(ROOT/'build/pc/bin')+';C:/msys64/mingw64/bin;'+env['PATH']
    with (out/'run.log').open('w') as log:
        proc=subprocess.Popen([os.environ.get('MELEE_TEST_EXE',str(ROOT/'build/native-game/melee-mod.exe')),str(project_path(cfg['assets']['directory'])/'sys/main.dol'),str(frames)],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
        try:proc.wait(timeout=max(180,frames/60*5));timeout=False
        except subprocess.TimeoutExpired:proc.terminate();proc.wait(10);timeout=True
    text=(out/'run.log').read_text(errors='replace');trace=re.findall(r'^\[netplay-ui\].*$',text,re.MULTILINE)
    captures=sorted(out.glob('frame_*.ppm'));contact_sheet(captures,out/'sheet.jpg')
    from PIL import Image
    for name,start,end in [('browser',1450,1590),('guest-ready',1870,1950),('host-ready',2270,2350),('rules',2860,2950),('created-from-browser',3720,3900)]:
        candidates=[p for p in captures if start+240+(360 if start>=2240 else 0)<=int(p.stem[6:])<=end+240+(360 if start>=2240 else 0)]
        if candidates:Image.open(candidates[-1]).save(out/(name+'.png'))
    guest_room_ids=sorted({int(re.search(r'room=(\d+)',s).group(1)) for s in trace if 'kind=11' in s and 'host=0' in s and 'players=2' in s})
    checks={
        'browser_populated':any('kind=13' in s and re.search(r'rooms=[1-9]',s) for s in trace),
        'browser_selection_moved':any('kind=13 selected=1' in s for s in trace),
        'joined_room':bool(guest_room_ids),
        'joined_selected_room':guest_room_ids == [expected_join_room['id']],
        'server_confirmed_selected_room':any(m.get('op')=='room' and m.get('room') and m['room']['id']==expected_join_room['id'] and m['room']['name']==expected_join_room['name'] and any(p['name']=='UI Tester' for p in m['room']['players']) for m in room_observer.messages),
        'guest_ready':any('kind=11' in s and 'host=0' in s and 'ready=1' in s for s in trace),
        'hosted_room':any('kind=11' in s and 'host=1' in s for s in trace),
        'rules_editor_opened':any('kind=11 selected=100' in s for s in trace),
        'rule_mode_changed':any('kind=11' in s and 'host=1' in s and 'mode=0' in s for s in trace),
        'stock_changed':any('kind=11' in s and 'host=1' in s and 'stock=5' in s for s in trace),
        'browser_create':len({re.search(r'room=(\d+)',s).group(1) for s in trace if 'kind=11' in s and 'host=1' in s}) >= 2,
        'host_ready':any('kind=11' in s and 'host=1' in s and 'ready=1' in s for s in trace),
        'rule_change_clears_ready':any('kind=11' in before and 'host=1' in before and 'ready=1' in before and any('selected=100' in after and 'mode=0' in after and 'ready=0' in after and re.search(r'room=(\d+)',before).group(1)==re.search(r'room=(\d+)',after).group(1) for after in trace[index+1:]) for index,before in enumerate(trace)),
    }
    report={'aspect':'16:9' if wide else '4:3','exit':proc.returncode,'timeout':timeout,'fault':'[fault]' in text,'assertions':re.findall(r'^.*assertion .*failed.*$',text,re.MULTILINE),'checks':checks,'expected_join_room':expected_join_room,'observed_guest_room_ids':guest_room_ids,'ui_trace':trace,'captures':len(captures),'path':str(out)}
    (out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2),flush=True);return report

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--aspect',choices=['4:3','16:9','both'],default='both');ap.add_argument('--frames',type=int,default=4500);args=ap.parse_args()
    compatibility_manifest=None
    if os.environ.get('MELEE_TEST_COMPATIBILITY') == '1':
        sys.path.insert(0,str(ROOT/'tools/modkit'))
        from compatibility import fingerprint,plan
        compatibility_manifest=fingerprint(plan(Path(os.environ.get('MELEE_TEST_EXE',str(ROOT/'build/native-game/melee-mod.exe')))))
    base=ROOT/'build/comparisons'/os.environ.get('MELEE_UI_OUTPUT','netplay-ui');base.mkdir(parents=True,exist_ok=True)
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    with (base/'server.log').open('w') as log:
        server=subprocess.Popen([sys.executable,str(ROOT/'server/melee_netplay_server.py'),'--bind','127.0.0.1','--port',str(port)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        fixtures=[];reports=[]
        try:
            deadline=time.monotonic()+10
            while True:
                try:
                    with socket.create_connection(('127.0.0.1',port),.1):break
                except OSError:
                    if time.monotonic()>deadline:raise RuntimeError('Local server did not start')
                    time.sleep(.05)
            for wide in ([0,1] if args.aspect=='both' else [int(args.aspect=='16:9')]):
                aspect='16x9' if wide else '4x3'
                compatibility=None if compatibility_manifest is None else {'schema':1,'fingerprint':compatibility_manifest['aspect'+aspect],'runtime':compatibility_manifest['runtimeIdentity'+aspect],'game':compatibility_manifest['game']}
                fixtures=[Fixture(port,'Practice Partner','Friendly Melee',compatibility=compatibility),Fixture(port,'Practice Partner','Final Destination',compatibility=compatibility),Fixture(port,'Practice Partner','Late Night Sets',rules={'stock':3,'minutes':6},compatibility=compatibility),Fixture(port,'Practice Partner',follow=True,compatibility=compatibility)]
                # Verify the selected browser row by identity, not only that a lobby appeared.
                deadline=time.monotonic()+5
                while True:
                    listings=[m['rooms'] for m in fixtures[-1].messages if m.get('op')=='rooms' and len(m.get('rooms',[]))==3]
                    if listings and all(f.room for f in fixtures[:3]):break
                    if time.monotonic()>deadline:raise RuntimeError('Fixture room list did not arrive')
                    time.sleep(.02)
                selected=listings[-1][1]  # The scripted D-pad moves to the second browser row.
                expected_join_room={key:selected[key] for key in ('id','name')}
                observer=next(f for f in fixtures[:3] if f.room==selected['id'])
                reports.append(run(wide,base,port,args.frames,expected_join_room,observer))
                for fixture in fixtures:fixture.close()
                fixtures=[]
        finally:
            for fixture in fixtures:fixture.close()
            server.terminate();server.wait(10)
    if any(r['exit'] or r['timeout'] or r['fault'] or r['assertions'] or not all(r['checks'].values()) for r in reports):raise SystemExit(1)
if __name__=='__main__':main()
