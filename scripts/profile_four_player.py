"""Profile normal native two/four-controller matches with isolated user files.

Run captures separately from timing: GPU readback changes the measured workload.
No guest gameplay, fighter, AI, or rendering code is bypassed by this driver.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import struct
import subprocess
import sys
import time

from project_config import ROOT, configuration, disc_path, game_environment, project_path


def summary(values):
    values = sorted(values)
    return {"mean": statistics.mean(values), "p50": values[len(values)//2],
            "p95": values[min(len(values)-1, int(len(values)*.95))], "max": values[-1]}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--name', required=True)
    ap.add_argument('--content-root', help='Verified selective content directory under build/akaneia-content')
    ap.add_argument('--players', type=int, choices=(2, 4), default=4)
    ap.add_argument('--frames', type=int, default=4800)
    ap.add_argument('--start', type=int, default=2900)
    ap.add_argument('--capture', action='store_true')
    ap.add_argument('--audio', action='store_true', help='Capture guest PCM and playback-buffer diagnostics')
    ap.add_argument('--capture-interval', type=int, default=600)
    ap.add_argument('--fighters', help='Offline native CSS exit fixture: external IDs separated by commas')
    ap.add_argument('--stage', type=int, help='Stock forced stage ID')
    ap.add_argument('--external-stage', type=int, help='Offline m-ex SSS external stage ID (16-bit)')
    ap.add_argument('--special-moves', action='store_true')
    ap.add_argument('--alternate-music', action='store_true')
    ap.add_argument('--custom-music-test', action='store_true', help='Isolated generated WAV in the Saria alternate-track folder')
    ap.add_argument('--scalar-batch', choices=('0','1'), help='Compare bounded Gekko batching with instruction-by-instruction execution')
    ap.add_argument('--overlap-render', choices=('0','1'), help='Compare the guest release boundary')
    ap.add_argument('--no-sample', action='store_true', help='Disable optional guest CPU sampling for timing-only runs')
    ap.add_argument('--kirby-gamewatch', choices=('plain','copy'), help='Stock Kirby/G&W fixture, with or without Kirby copy material')
    ap.add_argument('--warm-cache-from', help='Copy a previous isolated performance cache into this run')
    ap.add_argument('--wide', action='store_true')
    ap.add_argument('--width', type=int, default=0)
    ap.add_argument('--height', type=int, default=720)
    ap.add_argument('--scale', type=int, choices=range(5), default=1)
    ap.add_argument('--cpu-opponents', action='store_true', help='Use native player-type toggles for ports 2 through 4')
    ap.add_argument('--executable', default='build/native-game/YAMPP.exe')
    ap.add_argument('--renderer', default='build/pc/bin/melee_aurora_yampp.dll')
    a = ap.parse_args()
    out = (ROOT/'build/performance'/a.name).resolve()
    if not out.is_relative_to((ROOT/'build/performance').resolve()):
        ap.error('Output must remain below build/performance')
    out.mkdir(parents=True, exist_ok=True)
    cache_seed = None
    if a.warm_cache_from:
        cache_seed = (ROOT/a.warm_cache_from).resolve()
        if not cache_seed.is_relative_to((ROOT/'build/performance').resolve()):
            ap.error('Warm cache source must remain below build/performance')
        if cache_seed == out/'cache': ap.error('Use a new run name to preserve the cold baseline')
        shutil.copytree(cache_seed, out/'cache', dirs_exist_ok=True)
    cfg = configuration()
    env = game_environment(cfg, disc_path(cfg))
    for key in list(env):
        if key.startswith(('MELEE_TEST_', 'MELEE_CAPTURE_', 'MELEE_NETPLAY_', 'MELEE_MOD_TEST_', 'MELEE_CONTENT_', 'MELEE_MEX_', 'MELEE_RUNTIME_', 'MELEE_SAMPLE_', 'MELEE_PERF_', 'MELEE_AUDIO_')) or key in ('MELEE_INPUT', 'MELEE_VANILLA_BOOT', 'MELEE_START_TITLE', 'MELEE_WORKSHOP_TEST_MODS', 'MELEE_SERIAL_RENDER', 'MELEE_TRACE_DIAGNOSTICS'):
            env.pop(key)
    settings = out/'settings.xml'
    settings.write_text('<melee-settings schema="1" width="%d" height="%d" renderScale="%d" widescreen="%d" fullscreen="0" vsync="0" volume="100" mute="1" showFps="0" />' % (a.width or (1280 if a.wide else 960), a.height, a.scale, int(a.wide)))
    shutil.copy2(ROOT/'build/netplay-work/template-card.raw', out/'card.raw')
    boot = '300:100:6,420:100:6,540:100:6,660:100:6,780:100:6,900:1000:6,1080:4:8,1170:100:8,1260:100:8'
    # Each port starts above its own player panel. The same upward motion drops
    # the native token on the character under that controller's initial column.
    pick = ','.join(f'1600:0:26:0:80:{p},1750:100:8:0:0:{p}' for p in range(a.players))
    if a.kirby_gamewatch:
        # Use the verified ordinary VS-menu route. Forcing menu 2 while keeping
        # its navigation presses can select Classic or the wrong fighters.
        pick='1600:0:26:0:80:0,1640:0:22:80:0:0,1750:100:8:0:0:0,1600:0:22:0:80:1,1640:0:17:80:0:1,1750:100:8:0:0:1'
    if a.fighters:
        if not a.content_root: ap.error('--fighters navigation currently requires the selective Akaneia CSS')
        icons=json.loads((ROOT/a.content_root/'data/css.json').read_text())['fighterIcons']
        picks=[]
        for port,kind in enumerate(map(int,a.fighters.split(','))):
            icon=next(x for x in icons if x['fighter']==kind)
            dx=icon['x']-(-31+15*port);dy=icon['y']-(-21.5)
            picks += [f'1600:0:{round(abs(dy)/1.24)}:0:{80 if dy>=0 else -80}:{port}',f'1660:0:{round(abs(dx)/1.24)}:{80 if dx>=0 else -80}:0:{port}',f'1800:100:8:0:0:{port}']
        pick=','.join(picks)
    if a.cpu_opponents:
        pick += ',' + ','.join(f'1800:0:10:0:-80:{p},1840:100:8:0:0:{p}' for p in range(1, a.players))
    stage = '1950:1000:8,2140:0:20:0:80,2180:0:20:80:0,2260:100:8'
    # Native ground attacks/specials/jumps exercise four animated fighters.
    # No repeated directional drift, so the players remain on the same stage.
    combat = ','.join(f'{frame+p*5}:{buttons}:5:0:0:{p}'
                      for frame in range(2800, a.frames-20, 120)
                      for p in range(a.players)
                      for buttons in ['100' if (frame//120)%3 else '200'])
    if a.special_moves:
        moves=[('200',0,0),('200',80,0),('200',0,-80),('400',0,0),('200',0,80),('100',0,0)]
        combat=','.join(f'{frame+p*5}:{moves[i%len(moves)][0]}:12:{moves[i%len(moves)][1]}:{moves[i%len(moves)][2]}:{p}' for i,frame in enumerate(range(2700,a.frames-20,100)) for p in range(a.players))
    if a.alternate_music: stage+=','+','.join(f'1900:40:1000:0:0:{p}' for p in range(a.players))
    timeline = ','.join((boot, pick, stage, combat))
    exe = (ROOT/a.executable).resolve()
    renderer = (ROOT/a.renderer).resolve()
    env.update(MELEE_AURORA_DLL=str(renderer), MELEE_SETTINGS=str(settings),
               MELEE_MEMORY_CARD=str(out/'card.raw'), GCN_AURORA_HIDDEN='1',
               GCN_AURORA_DATA_DIR=str(out/'cache'), MELEE_INPUT=timeline,
               MELEE_PERF_CSV=str(out/'frames.csv'),
               MELEE_SAMPLE_START=str(a.start), MELEE_SAMPLE_END=str(a.frames),
               MELEE_SAMPLE_TOP='128', MELEE_SAMPLE_PATH=str(out/'guest-prof.txt'))
    dol=project_path(cfg['assets']['directory'])/'sys/main.dol'
    if a.content_root:
        sys.path.insert(0,str(ROOT/'tools/modkit'))
        from content_mods import boot_files,checked_content
        content=(ROOT/a.content_root).resolve()
        if not content.is_relative_to(ROOT/'build/akaneia-content'):ap.error('Invalid content directory')
        verification=json.loads((content/'verification.json').read_text())
        record={'directory':content.relative_to(ROOT).as_posix(),'isoSHA256':verification['isoSHA256']}
        disc=checked_content(record,True);dol,fst=boot_files(disc,out/'disc/sys')
        env.update(MELEE_DISC=str(disc),MELEE_FST=str(fst),MELEE_MEX_BASE_DOL=str(project_path(cfg['assets']['directory'])/'sys/main.dol'),MELEE_UNICORN_LIBRARY=str(ROOT/'build/pc/bin/unicorn.dll'),MELEE_MOD_REGISTRY=str(out/'empty-registry.tsv'),MELEE_COSTUME_REGISTRY=str(out/'empty-costumes.tsv'),MELEE_TEST_MENU='2:0')
        timeline=timeline.replace(',1080:4:8','').replace(',1260:100:8','').replace(',1170:100:8','')
        env['MELEE_INPUT']=timeline
    if a.custom_music_test:
        env['MELEE_TEST_MENU']='2:0'
        env['MELEE_INPUT']=timeline.replace(',1080:4:8',',1120:100:8').replace(',1260:100:8','').replace(',1170:100:8','')
        if a.content_root or not a.alternate_music or a.stage != 13:
            ap.error('--custom-music-test requires stock Great Bay (stage13) and --alternate-music')
        import math, wave
        music_dir=out/'music/stage/saria'
        music_dir.mkdir(parents=True,exist_ok=True)
        with wave.open(str(music_dir/'YAMPP Test Tone.wav'),'wb') as wav:
            wav.setparams((2,2,32000,0,'NONE','not compressed'))
            wav.writeframes(b''.join(struct.pack('<hh',v,v) for i in range(32000*3) for v in [int(2000*math.sin(i*440*2*math.pi/32000))]))
        env['MELEE_CONTENT_TEST_MOD_REGISTRY']=str(out/'empty-registry.tsv')
        env['MELEE_MOD_REGISTRY']=str(out/'empty-registry.tsv')
    if exe.with_suffix('.core.dll').is_file():
        # Native reload builds require a verified plan even for an ordinary
        # benchmark. Isolate activation state from the user's open game.
        state_record=dict(record,enabled=True) if a.content_root else None
        state=out/'content-state.json'
        state.write_text(json.dumps({'schema':1,'akaneia':state_record}))
        plan=out/'boot-plan.env'
        env.update(MELEE_CONTENT_STATE=str(state),MELEE_CONTENT_LAUNCHER='1',
                   MELEE_CONTENT_RESTART=str(out/'restart.json'),MELEE_CONTENT_BOOT_PLAN=str(plan),
                   MELEE_CONTENT_PLAN_SCRIPT=str(ROOT/'scripts/prepare_content_runtime.py'),
                   MELEE_WORKSHOP_PYTHON=sys.executable)
        subprocess.run([sys.executable,env['MELEE_CONTENT_PLAN_SCRIPT'],'--output',str(plan)],
                       cwd=ROOT,env=env,check=True)
    if a.stage is not None: env['MELEE_TEST_STAGE']=str(a.stage)
    if a.external_stage is not None:
        if not a.content_root or not 0 <= a.external_stage <= 65535:ap.error('External stage requires selective content and a 16-bit ID')
        env['MELEE_TEST_STAGE_EXTERNAL']=str(a.external_stage)
    if a.scalar_batch is not None:env['MELEE_MEX_SCALAR_BATCH']=a.scalar_batch
    if a.overlap_render is not None:env['MELEE_OVERLAP_RENDER']=a.overlap_render
    if a.no_sample:
        for key in list(env):
            if key.startswith('MELEE_SAMPLE_'):env.pop(key)
    if a.kirby_gamewatch:env['MELEE_TEST_KIRBY_GAMEWATCH']=a.kirby_gamewatch
    if a.audio:
        env.update(MELEE_AUDIO_CAPTURE=str(out/'audio.pcm'),MELEE_AUDIO_STATS=str(out/'audio-timing.csv'))
    env['MELEE_TEST_FAST_EXIT']='1'
    if a.capture:
        env.update(MELEE_CAPTURE_AURORA='1', MELEE_CAPTURE_UI='1', MELEE_CAPTURE_INTERVAL=str(max(1,a.capture_interval)), MELEE_CAPTURE_DIR=str(out))
    inputs = {'contentRoot':a.content_root,'kirbyGameWatch':a.kirby_gamewatch,'players': a.players, 'cpuOpponents': a.cpu_opponents, 'timeline': timeline, 'capture': a.capture, 'cpuSampler': not a.no_sample,
              'wide': a.wide, 'width':a.width or (1280 if a.wide else 960), 'height':a.height, 'scale':a.scale, 'executable': str(exe), 'renderer': str(renderer),
              'executableSha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'rendererSha256': hashlib.sha256(renderer.read_bytes()).hexdigest()}
    inputs.update(audioCapture=a.audio,fighters=a.fighters,stage=a.stage,externalStage=a.external_stage,specialMoves=a.special_moves,alternateMusic=a.alternate_music,customMusicTest=a.custom_music_test)
    inputs['overlapRender'] = a.overlap_render
    inputs['scalarBatch'] = a.scalar_batch
    if exe.with_suffix('.core.dll').is_file():inputs['coreSha256']=hashlib.sha256(exe.with_suffix('.core.dll').read_bytes()).hexdigest()
    inputs['cacheSeed'] = str(cache_seed) if cache_seed else None
    (out/'inputs.json').write_text(json.dumps(inputs, indent=2))
    began = time.monotonic()
    began_wall = time.time()
    with (out/'run.log').open('w') as log:
        process = subprocess.Popen([str(exe), str(dol), str(a.frames)], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        print('Owned isolated profiling PID', process.pid, flush=True)
        try:
            process.wait(timeout=max(150, a.frames/60*4))
        except subprocess.TimeoutExpired:
            process.terminate(); process.wait(15)
            raise
    if not (out/'guest-prof.txt').exists() and (ROOT/'build/prof.txt').exists():
        shutil.copy2(ROOT/'build/prof.txt', out/'guest-prof.txt')
    final_image=ROOT/'build/frame_aurora.ppm'
    if final_image.exists() and final_image.stat().st_mtime>=began_wall:shutil.copy2(final_image,out/'final.ppm')
    log_text = (out/'run.log').read_text(errors='replace')
    report = {'exit': process.returncode, 'wallSeconds': time.monotonic()-began,
              'assertions': re.findall(r'^.*assertion .*failed.*$', log_text, re.M),
              'fault': '[fault]' in log_text or '[content-host-fault]' in log_text, 'captures': len(list(out.glob('frame_*.ppm'))),
              'measurementHostRange': [a.start, a.frames], 'captureAffectsTiming': a.capture}
    memory = ROOT/'build/native-game/last-memory.bin'
    if memory.exists() and memory.stat().st_mtime >= began_wall:
        shutil.copy2(memory, out/'last-memory.bin')
        data = memory.read_bytes()
        # Original GALE01 player_slots, documented in pl/player.h; observation
        # only, used to prove this fixture reached actual gameplay.
        report['nativePlayers'] = []
        for slot in range(a.players):
            base = 0x453080 + slot*0xe90
            state, fighter, kind = struct.unpack_from('>III', data, base)
            report['nativePlayers'].append({'slot':slot,'state':state,
                'externalKind':fighter,'playerType':kind,'cpuLevel':data[base+0x49],
                'costume':data[base+0x44],'transformed':data[base+0x0c]})
    rows = [{k: float(v) for k, v in r.items()} for r in csv.DictReader((out/'frames.csv').open()) if None not in r.values()]
    draws = [r for r in rows if a.start <= r['host'] <= a.frames and r['rendered']]
    if len(draws) > 1:
        presented = [r['seconds']+(r['begin_ms']+r['translate_ms']+r['end_ms'])/1000 for r in draws]
        report['renderedFrames'] = len(draws)
        report['renderFps'] = (len(draws)-1)/(presented[-1]-presented[0])
        report['frameIntervalMs'] = summary([(b-x)*1000 for x,b in zip(presented,presented[1:])])
        report['workMs'] = {k:summary([r[k] for r in draws]) for k in ('begin_ms','translate_ms','end_ms','ready_age_ms','guest_wait_ms')}
    (out/'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    if a.custom_music_test and ('custom track ready: YAMPP Test Tone' not in log_text or not all(p['state']==2 for p in report.get('nativePlayers',[]))):
        raise SystemExit('Custom music fixture did not reach the intended track in gameplay')
    if a.fighters and [p['externalKind'] for p in report.get('nativePlayers',[])] != [int(x) for x in a.fighters.split(',')]:
        raise SystemExit('Fighter fixture did not reach the requested characters')
    if a.kirby_gamewatch and [p['externalKind'] for p in report.get('nativePlayers',[])] != [4,3]:
        raise SystemExit('Invalid Kirby fixture: expected Kirby and Game & Watch in gameplay')
    if len(draws) < 60:
        raise SystemExit('Too few gameplay frames for a valid performance sample')
    if process.returncode or report['assertions'] or report['fault']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
