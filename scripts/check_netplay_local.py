"""Two headless game instances play a synchronised match through a local server.

Both instances boot with the same scripted title/menu input, connect to a
throwaway copy of the netplay server, and drive the character select, stage
select and a match with epoch-keyed scripted input. The check passes when both
instances start the session, reach the match, never report a desync and log
identical state hashes for identical frames.
"""
import argparse, json, os, re, socket, subprocess, sys, time
from pathlib import Path
from project_config import ROOT, configuration, disc_path, project_path

TEMPLATE_CARD = ROOT / 'build/netplay-work/template-card.raw'
# Dismiss unlock-award prompts in the isolated card, then leave the title.
# Once the session begins, epoch-keyed netplay inputs replace these boot inputs.
BOOT = '300:100:6,420:100:6,540:100:6,660:100:6,780:100:6,900:1000:6'
# epoch 1 = character select (both players drop their token above their door),
# epoch 2 = stage select (P1 picks), epoch 3 = match (both attack).
HOST_INPUT = '1:120:0:26:0:80,1:200:100:8,1:420:1000:8,2:150:0:20:0:80,2:190:0:20:80:0,2:260:100:8,3:240:0:32:-80:0,3:282:100:12,3:316:100:12,3:350:200:18,3:410:400:8,3:430:100:12,3:500:100:12,3:550:200:12'
JOIN_INPUT = '1:120:0:26:0:80,1:200:100:8,3:240:0:32:80:0,3:288:100:12,3:322:100:12,3:358:200:18,3:420:400:8,3:440:100:12,3:510:100:12,3:560:200:12'


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def content_plan(out):
    """Boot inputs for whatever content is currently enabled, or None.

    Produced by the same helper the real launcher uses, so a run here boots the
    disc, executable and file table a player with that content enabled would
    boot -- rather than a hand-assembled approximation of one.
    """
    plan = (out / 'content-plan.env').resolve()
    plan.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / 'scripts/prepare_content_runtime.py'),
                    '--output', str(plan)], cwd=ROOT, check=True)
    values = {}
    for line in plan.read_text(encoding='utf-8').splitlines():
        key, _, value = line.partition(chr(9))
        if value:
            values[key] = value
    return values if values.get('MELEE_MEX_BASE_DOL') else None


def launch(name, out, port, role, script, frames, capture, input_delay=3, mods=None, fast_exit=False, widescreen=0, endpoint=None, room_name="localtest", compare_scalar_batch=False, capture_interval=30, relay_only=False, content=None):
    cfg = configuration()
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('MELEE_TEST_', 'MELEE_MOD_TEST_', 'MELEE_TRACE_', 'MELEE_RENDERDOC_', 'MELEE_CAPTURE_', 'MELEE_AUDIO_', 'MELEE_NETPLAY_')) or key in ('MELEE_INPUT', 'MELEE_MODS', 'MELEE_MEMORY_CARD', 'MELEE_SETTINGS'):
            env.pop(key)
    env.update(MELEE_DISC=str(disc_path(cfg)), MELEE_FST=str(project_path(cfg['assets']['directory']) / 'sys/fst.bin'),
               MELEE_AURORA_DLL=os.environ.get('MELEE_TEST_RENDERER',str(project_path(cfg['runtime']['developmentRenderer']))), GCN_AURORA_HIDDEN='1',
               GCN_AURORA_DATA_DIR=str(out / 'cache'), MELEE_MEMORY_CARD=str(out / 'card.raw'), MELEE_SETTINGS=str(out / 'settings.xml'),
               MELEE_INPUT=BOOT, MELEE_NETPLAY_SERVER=endpoint or ('127.0.0.1:%d' % port), MELEE_NETPLAY_NAME=name,
               MELEE_NETPLAY_AUTO='%s:%s' % (role,room_name), MELEE_NETPLAY_INPUT=script, MELEE_NETPLAY_RULES='"stock":3,"minutes":0,"items":0,"delay":%d' % input_delay)
    if relay_only:
        env['MELEE_NETPLAY_RELAY_ONLY'] = '1'
    if content:
        # Applied after the stock defaults above, which name the base disc.
        for key in ('MELEE_DISC', 'MELEE_FST', 'MELEE_RUNTIME_DOL',
                    'MELEE_MEX_BASE_DOL', 'MELEE_UNICORN_LIBRARY', 'MELEE_MOD_REGISTRY'):
            if content.get(key):
                env[key] = content[key]
        # The additive-costume system stands down entirely while m-ex content is
        # loaded, so a room that waits for costume activation waits forever and
        # cancels its own entry. A player with content enabled is given an empty
        # registry for the same reason; match it rather than a test sandbox.
        for key in ('MELEE_COSTUME_REGISTRY', 'MELEE_WORKSHOP_TEST_MODS',
                    'MELEE_MOD_REPOSITORY_URL'):
            env.pop(key, None)
    if mods:
        env.update(MELEE_WORKSHOP_TEST_MODS=str(mods),MELEE_COSTUME_REGISTRY=str(mods/'costumes.tsv'),MELEE_MOD_REGISTRY=str(mods/'registry.tsv'),MELEE_MOD_REPOSITORY_URL='http://127.0.0.1:%d/api/mods'%port)
    env['MELEE_TEST_MENU']='10:0'
    env['PATH'] = str(ROOT / 'build/pc/bin') + ';C:/msys64/mingw64/bin;' + env['PATH']
    if TEMPLATE_CARD.exists():
        import shutil
        shutil.copy2(TEMPLATE_CARD, out / 'card.raw')
    (out / 'settings.xml').write_text('<?xml version="1.0" encoding="utf-8"?>\n<melee-settings schema="1" width="%d" height="720" renderScale="1" widescreen="%d" fullscreen="0" vsync="0" volume="100" mute="1" showFps="0" />' % (1280 if widescreen else 960, widescreen))
    if os.environ.get('MELEE_NETPLAY_TEST_STAGE'):
        env['MELEE_TEST_STAGE'] = os.environ['MELEE_NETPLAY_TEST_STAGE']
    # Set for both clients, which is what keeps the two stage selects agreeing.
    if os.environ.get('MELEE_NETPLAY_TEST_STAGE_EXTERNAL'):
        env['MELEE_TEST_STAGE_EXTERNAL'] = os.environ['MELEE_NETPLAY_TEST_STAGE_EXTERNAL']
    if os.environ.get('MELEE_NETPLAY_TEST_AUDIO') == '1':
        env['MELEE_AUDIO_STATS'] = str(out/'audio-stats.csv')
        env['MELEE_AUDIO_CAPTURE'] = str(out/'audio.pcm')
    if compare_scalar_batch:env['MELEE_MEX_SCALAR_BATCH']='1' if role=='host' else '0'
    if os.environ.get('MELEE_NETPLAY_TEST_COMPARE_RENDER')=='1':env['MELEE_OVERLAP_RENDER']='1' if role=='host' else '0'
    if fast_exit:env['MELEE_TEST_FAST_EXIT']='1'
    if capture:
        env['MELEE_CAPTURE_AURORA'] = '1'
        env['MELEE_CAPTURE_UI'] = '1'
        env['MELEE_CAPTURE_DIR'] = str(out)
        env['MELEE_CAPTURE_INTERVAL'] = str(capture_interval)
    log = (out / 'run.log').open('w')
    exe = Path(os.environ.get('MELEE_TEST_EXE', str(ROOT / 'build/native-game/melee-mod.exe')))
    dol = project_path(cfg["assets"]["directory"]) / "sys/main.dol"
    # Content mods replace the executable as well as the disc. Booting the
    # stock one against a content disc starts the game, and even brings up
    # the dynamic engine, but m-ex's own data root never exists -- so the
    # costume tables never bind and room entry cancels itself.
    if content and content.get("MELEE_RUNTIME_DOL"):
        dol = Path(content["MELEE_RUNTIME_DOL"])
    content = os.environ.get("MELEE_NETPLAY_TEST_CONTENT")
    if content:
        sys.path.insert(0,str(ROOT/"tools/modkit"))
        from content_mods import boot_files
        disc = Path(content).resolve() / "yampp-content.iso"
        dol, fst = boot_files(disc, out / "disc/sys")
        env.update(MELEE_DISC=str(disc),MELEE_FST=str(fst),MELEE_MEX_BASE_DOL=str(project_path(cfg["assets"]["directory"])/"sys/main.dol"),MELEE_UNICORN_LIBRARY=str(ROOT/"build/pc/bin/unicorn.dll"),MELEE_TEST_MEX_ONLINE="1")
    if exe.with_suffix(".core.dll").is_file():
        # Every peer gets isolated activation state. The joining peer starts
        # with the opposite content, accepts the real prompt, reloads in the
        # same process, and then plays through the normal rollback path.
        import shutil
        record=json.loads((ROOT/"user/content-mods.json").read_text())["akaneia"].copy()
        target=bool(content)
        switch=role=="join" and os.environ.get("MELEE_NETPLAY_TEST_HOTJOIN")=="1"
        record["enabled"]=not target if switch else target
        state=out/"content-state.json";state.write_text(json.dumps({"schema":1,"akaneia":record}))
        env.update(MELEE_CONTENT_STATE=str(state),MELEE_CONTENT_LAUNCHER="1",MELEE_CONTENT_RESTART=str(out/"restart.json"),
                   MELEE_CONTENT_BOOT_PLAN=str(out/"boot-plan.env"),MELEE_CONTENT_PLAN_SCRIPT=str(ROOT/"scripts/prepare_content_runtime.py"),
                   MELEE_WORKSHOP_PYTHON=sys.executable,MELEE_CONTENT_GENERATION="1",MELEE_CONTENT_TEST_MOD_REGISTRY=str(mods/"registry.tsv"))
        if switch:
            generations=out/"generations.json"
            generations.write_text(json.dumps({"generations":[{"MELEE_INPUT":BOOT+",1500:100:8,2100:100:8"},{"MELEE_INPUT":""}]}))
            env["MELEE_CONTENT_TEST_PLAN"]=str(generations)
        subprocess.run([sys.executable,env["MELEE_CONTENT_PLAN_SCRIPT"],"--output",env["MELEE_CONTENT_BOOT_PLAN"]],cwd=ROOT,env=env,check=True)
    process = subprocess.Popen([str(exe), str(dol), str(frames)], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    return process, log


def analyse(path):
    text = path.read_text(errors='replace')
    hashes = {}; reused_hashes=[]
    for m in re.finditer(r'\[netplay\] epoch (\d+) frame (\d+) hash ([0-9A-F]{8})', text):
        key=(int(m.group(1)),int(m.group(2)))
        if key in hashes and hashes[key]!=m.group(3):reused_hashes.append([*key,hashes[key],m.group(3)])
        hashes[key]=m.group(3)
    epochs = [(m.group(1), m.group(2)) for m in re.finditer(r'\[netplay\] epoch (\d+) begins at (.*)', text)]
    return {
        'reused_hashes':reused_hashes,
        'started': '[netplay] session' in text and 'started as port' in text,
        'aspect_mismatch': 'Match the host build, game data and aspect ratio.' in text,
        'renderer_aspects': [int(value) for value in re.findall(r'\[settings\].*?wide=(0|1)\b', text)],
        'max_logged_host_frame': max((int(n) for n in re.findall(r'^  \[(\d+)\] calls=', text, re.MULTILINE)), default=0),
        'epochs': epochs,
        'desync': 'DESYNC' in text,
        'fault': '[fault]' in text,
        'compatibility': re.findall(r'\[netplay-mods\] compatibility ([a-f0-9]{64})', text),
        'costume_loads': re.findall(r'\[costumes\] loaded (.*) kind=(\d+) slot=(\d+) base=(\d+)', text),
        'rollback_enabled': '[netplay] rollback enabled:' in text,
        'rollbacks': len(re.findall(r'\[netplay\] rollback frame \d+ -> \d+', text)),
        'cancelled_to_menu': '[netplay] canceled scene; returning to the netplay menu' in text,
        'direct': '[netplay] direct path to port' in text,
        'custom_music': re.findall(r'\[music\] track \S+ \S+ -> custom[^:]*: (.+)', text),
        'external_stage': [int(v) for v in re.findall(r'\[stage-test\] native SSS selected external stage (\d+)', text)],
        'dynamic_engine': '[mex-runtime] Dynamic upstream execution enabled' in text,
        'tracked_snapshots': '[rollback] tracked snapshots:' in text,
        'ended': re.findall(r'\[netplay\] Session ended: (.*)', text),
        'assertions': re.findall(r'^.*assertion .*failed.*$', text, re.MULTILINE),
        'hashes': hashes,
        'max_frame_by_epoch': {e: max(f for (ee, f) in hashes if ee == e) for e in {ee for (ee, f) in hashes}},
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--name', default='netplay-local')
    parser.add_argument('--require-rematch', action='store_true', help='Exercise normal no-contest/results and require a second synchronized match')
    parser.add_argument('--compare-scalar-batch', action='store_true', help='Run the host with batching and the guest instruction by instruction')
    parser.add_argument('--frames', type=int, default=8000)
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--server', help='External relay endpoint for an actual native two-peer check')
    parser.add_argument('--room-name', default='YAMPP connection check', help='Unique temporary room name for external checks')
    parser.add_argument('--host-widescreen', type=int, choices=(0,1), default=0)
    parser.add_argument('--join-widescreen', type=int, choices=(0,1), default=0)
    parser.add_argument('--expect-aspect-reject', action='store_true', help='Require opposite-aspect clients to be rejected before room entry or gameplay')
    parser.add_argument('--fast-exit', action='store_true', help='Skip known renderer DLL-detach fault after card flush and readback; gameplay checks remain unchanged')
    parser.add_argument('--input-delay', type=int, choices=range(0, 11), default=3,
                        help='0 lets each peer pick its own delay from the measured round trip')
    parser.add_argument('--content', action='store_true',
                        help='Boot whatever content mod is enabled in user/content-mods.json, '
                             'so rollback is checked on the m-ex path as well as the stock disc')
    parser.add_argument('--stage', type=int,
                        help='Pin a stock versus stage by its 8-bit id')
    parser.add_argument('--external-stage', type=int,
                        help="Pin a stage by m-ex's 16-bit external id, which is how a stage "
                             'that content added is named. Requires --content.')
    parser.add_argument('--host-music', type=Path,
                        help='Audio file to install as the host\'s own stage music. The guest does '
                             'not get it, so the run checks that two players hearing different '
                             'things still simulate identical frames.')
    parser.add_argument('--join-music', type=Path,
                        help="Same, for the guest.")
    parser.add_argument('--music-track',
                        help='Track the custom audio replaces, as its file stem or display name '
                             '(the game looks for a folder of that name). Required with --host-music '
                             'or --join-music.')
    parser.add_argument('--relay-only', action='store_true',
                        help='Keep both clients on the server path. Implied by the impairment options, '
                             'which impair the server and would otherwise measure a path nobody is using.')
    parser.add_argument('--require-direct', action='store_true',
                        help='Require both clients to reach each other without the server')
    parser.add_argument('--relay-delay-ms', type=int, default=0)
    parser.add_argument('--relay-jitter-ms', type=int, default=0)
    parser.add_argument('--relay-stall-ms', type=int, default=0)
    parser.add_argument('--require-rollback', action='store_true')
    parser.add_argument('--costume-package', help='Package SHA-256 to install only in isolated test folders')
    parser.add_argument('--costume-package-file', type=Path, help='Prepared local ZIP to verify before publication; requires its --costume-package SHA-256')
    parser.add_argument('--costume-slot', type=int, help='Require both peers to actually load this Link costume slot')
    parser.add_argument('--host-input', default=HOST_INPUT)
    parser.add_argument('--join-input', default=JOIN_INPUT)
    parser.add_argument('--capture-interval', type=int, default=30, help='Host frames between diagnostic captures')
    args = parser.parse_args()
    if args.require_rematch:
        args.host_input += ',3:1800:1000:8,3:1860:1160:10,4:180:1000:8,4:360:1000:8,4:540:1000:8,5:240:1000:8,6:150:0:20:0:80,6:190:0:20:80:0,6:260:100:8,7:240:100:12,7:350:200:18,7:500:100:12'
        args.join_input += ',4:180:1000:8,4:360:1000:8,4:540:1000:8,7:260:100:12,7:380:200:18,7:550:100:12'
    if args.costume_package_file and not args.costume_package:
        parser.error('--costume-package-file requires --costume-package')
    if args.expect_aspect_reject and args.host_widescreen == args.join_widescreen:
        parser.error('Aspect rejection requires different peer aspect settings')
    if not (0 <= args.relay_delay_ms <= 1000 and 0 <= args.relay_jitter_ms <= 1000):
        parser.error('relay delay and jitter must be between 0 and 1000 ms')
    if not 0 <= args.relay_stall_ms <= 10000:parser.error('invalid relay stall')
    base = (ROOT / 'build/comparisons').resolve()
    out = (base / args.name).resolve()
    if out == base or base not in out.parents:
        parser.error('--name must identify a directory inside build/comparisons')
    (out / 'host').mkdir(parents=True, exist_ok=True)
    (out / 'join').mkdir(parents=True, exist_ok=True)
    if args.costume_package and not re.fullmatch('[a-f0-9]{64}',args.costume_package):parser.error('Invalid package SHA-256')
    sandbox=(ROOT/'build/modkit/netplay-tests'/args.name).resolve()
    if not sandbox.is_relative_to((ROOT/'build/modkit/netplay-tests').resolve()):parser.error('Invalid test name')
    repository=out/'repository'
    repository.mkdir(parents=True,exist_ok=True)
    if args.costume_package:
        sys.path.insert(0,str(ROOT/'tools/modkit'));sys.path.insert(0,str(ROOT/'server'))
        import community
        from mod_repository import Repository
        import hashlib
        limit=64*1024*1024
        if args.costume_package_file:
            if args.costume_package_file.stat().st_size > limit:raise ValueError('Candidate package exceeds 64 MiB')
            with args.costume_package_file.open('rb') as response:raw=response.read(limit+1)
        else:
            with community.request('/'+args.costume_package+'/package.zip') as response:raw=response.read(limit+1)
        if len(raw)>limit:raise ValueError('Candidate package exceeds 64 MiB')
        if hashlib.sha256(raw).hexdigest()!=args.costume_package:raise ValueError('Candidate package does not match --costume-package SHA-256')
        metadata,_=Repository(repository).publish(raw)
        if metadata['sha256']!=args.costume_package:raise ValueError('Repository returned a different costume package')
    if args.server and (args.relay_delay_ms or args.relay_jitter_ms or args.relay_stall_ms or args.costume_package):
        parser.error('External checks do not host an impairment proxy or costume repository')
    # Impairing the relay only measures something while the clients are
    # still using it.
    relay_only = args.relay_only or bool(args.relay_delay_ms or args.relay_jitter_ms or args.relay_stall_ms)
    if relay_only and args.require_direct:
        parser.error('--require-direct cannot be combined with --relay-only or the relay impairment options')
    content = content_plan(out) if args.content else None
    if args.content and content is None:
        parser.error('No content mod is enabled; enable one in user/content-mods.json first')
    if args.stage is not None:
        os.environ['MELEE_NETPLAY_TEST_STAGE'] = str(args.stage)
    if args.external_stage is not None:
        if not args.content:
            parser.error('--external-stage names a stage that content added; pass --content')
        os.environ['MELEE_NETPLAY_TEST_STAGE_EXTERNAL'] = str(args.external_stage)
    port = free_port()
    server_log = (out / 'server.log').open('w')
    server = None if args.server else subprocess.Popen([sys.executable, str(ROOT / 'scripts/netplay_test_server.py'),
                               '--port', str(port), '--delay-ms', str(args.relay_delay_ms),
                               '--jitter-ms', str(args.relay_jitter_ms),'--stall-ms',str(args.relay_stall_ms),'--mods-dir',str(repository)],
                              stdout=server_log, stderr=subprocess.STDOUT)
    started = time.perf_counter()
    host = join = None
    host_log = join_log = None
    timed_out = False
    timeout = max(120, args.frames / 60 * 5)
    try:
        deadline = time.monotonic() + 10
        while server is not None:
            if server.poll() is not None:
                raise RuntimeError('Test relay exited; see ' + str(out / 'server.log'))
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=.2):
                    break
            except OSError:
                if time.monotonic() >= deadline:
                    raise RuntimeError('Test relay did not start')
                time.sleep(.1)
        for role in ('host','join'):
            mods=sandbox/role;mods.mkdir(parents=True,exist_ok=True)
            setup=os.environ.copy();setup.update(MELEE_WORKSHOP_TEST_MODS=str(mods),MELEE_MOD_REPOSITORY_URL='http://127.0.0.1:%d/api/mods'%port)
            if args.costume_package:
                subprocess.run([sys.executable,str(ROOT/'tools/modkit/community.py'),'install','--sha256',args.costume_package,'--output',str(out/role/'install.json')],cwd=ROOT,env=setup,check=True)
                subprocess.run([sys.executable,str(ROOT/'tools/modkit/community.py'),'toggle','--sha256',args.costume_package,'--enabled','1','--output',str(out/role/'enabled.json')],cwd=ROOT,env=setup,check=True)
            else:
                if any(mods.iterdir()):raise ValueError('Vanilla tests require a new empty isolated mods folder')
                subprocess.run([sys.executable,'-c','import sys;sys.path.insert(0,"tools/modkit");from catalog import runtime_registry;runtime_registry()'],cwd=ROOT,env=setup,check=True)
        for role, source in (('host', args.host_music), ('join', args.join_music)):
            if source is None:
                continue
            if not source.is_file():
                parser.error('No such audio file: %s' % source)
            if not args.music_track:
                parser.error('--host-music/--join-music need --music-track')
            folder = sandbox / role / 'music' / 'stage' / args.music_track
            folder.mkdir(parents=True, exist_ok=True)
            import shutil as _shutil
            _shutil.copy2(source, folder / source.name)
            print('installed %s music: %s' % (role, source.name))
        host, host_log = launch('Host', out / 'host', port, 'host', args.host_input, args.frames, args.capture, args.input_delay, sandbox/'host', args.fast_exit, args.host_widescreen, args.server, args.room_name if args.server else 'localtest',args.compare_scalar_batch,args.capture_interval,relay_only,content)
        time.sleep(2.0)
        join, join_log = launch('Guest', out / 'join', port, 'join', args.join_input, args.frames, args.capture, args.input_delay, sandbox/'join', args.fast_exit, args.join_widescreen, args.server, args.room_name if args.server else 'localtest',args.compare_scalar_batch,args.capture_interval,relay_only,content)
        deadline = time.monotonic() + timeout
        for process in (host, join):
            try:
                process.wait(timeout=max(.1, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                timed_out = True
                break
    finally:
        for process in (host, join, server):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for log in (host_log, join_log, server_log):
            if log is not None:
                log.close()
    a, b = analyse(out / 'host/run.log'), analyse(out / 'join/run.log')
    common = sorted(set(a['hashes']) & set(b['hashes']))
    mismatches = [(e, f, a['hashes'][(e, f)], b['hashes'][(e, f)]) for (e, f) in common if a['hashes'][(e, f)] != b['hashes'][(e, f)]]
    match_epoch = [e for e in a['epochs'] if e[1].strip() == 'match']
    match_epochs = {int(e[0]) for e in match_epoch}
    match_compared = sum(e in match_epochs for e, f in common)
    require_rollback = args.require_rollback or bool(args.relay_delay_ms or args.relay_jitter_ms or args.relay_stall_ms)
    impairment_exercised = 'Injected latency:' in (out / 'server.log').read_text(errors='replace')
    if relay_only:
        # The impairment lives in the relay, so a run that measures it has to
        # be a run that still goes through the relay.
        impairment_exercised = impairment_exercised and not a['direct'] and not b['direct']
    expected_endings = {'Shutdown', 'Opponent left', 'Server ended the session'}
    unexpected_endings = [reason for peer in (a, b) for reason in peer['ended'] if reason not in expected_endings]
    report = {
        'external_server':args.server is not None,
        'fast_exit':args.fast_exit,
        'host_widescreen':args.host_widescreen,'join_widescreen':args.join_widescreen,
        'expected_aspect_rejection': args.expect_aspect_reject,
        'renderer_aspects_match_request': bool(a['renderer_aspects']) and bool(b['renderer_aspects']) and a['renderer_aspects'][-1] == args.host_widescreen and b['renderer_aspects'][-1] == args.join_widescreen,
        'requested_host_frames':args.frames,
        'premature_exit': any(peer['max_logged_host_frame'] < args.frames - 60 for peer in (a,b)),
        'costume_package':args.costume_package,'costume_slot':args.costume_slot,
        'costume_package_file':str(args.costume_package_file.resolve()) if args.costume_package_file else None,
        'compatibility_matched': bool(a['compatibility']) and bool(b['compatibility']) and a['compatibility'][-1]==b['compatibility'][-1],
        'requested_costume_loaded': args.costume_slot is None or all(any(int(kind)==6 and int(slot)==args.costume_slot for _,kind,slot,_ in peer['costume_loads']) for peer in (a,b)),
        'unexpected_endings': unexpected_endings,
        'wall_seconds': time.perf_counter() - started, 'timed_out': timed_out, 'exit_codes': [host.returncode, join.returncode],
        'host': {k: v for k, v in a.items() if k != 'hashes'}, 'join': {k: v for k, v in b.items() if k != 'hashes'},
        'compared_frames': len(common), 'hash_mismatches': mismatches[:20],
        'compared_match_frames': match_compared,
        'input_delay': args.input_delay, 'relay_delay_ms': args.relay_delay_ms,
        'relay_jitter_ms': args.relay_jitter_ms, 'require_rollback': require_rollback,
        'impairment_exercised': impairment_exercised,
        'relay_only': relay_only,
        'direct_paths': [a['direct'], b['direct']],
        'tracked_snapshots': [a['tracked_snapshots'], b['tracked_snapshots']],
        'content': bool(content),
        'content_disc': content.get('MELEE_DISC') if content else None,
        'dynamic_engine': [a['dynamic_engine'], b['dynamic_engine']],
        'custom_music': [a['custom_music'], b['custom_music']],
        'external_stage': [a['external_stage'], b['external_stage']],
        'reached_match': bool(match_epoch) and any(e[1].strip() == 'match' for e in b['epochs']),
    }
    report['passed'] = (not timed_out and not report['premature_exit'] and host.returncode == 0 and join.returncode == 0 and a['started'] and b['started']
                        and report['compatibility_matched'] and report['requested_costume_loaded']
                        and not a['desync'] and not b['desync'] and not mismatches and not a['reused_hashes'] and not b['reused_hashes'] and report['reached_match']
                        and not a['fault'] and not b['fault'] and not unexpected_endings
                        and not a['assertions'] and not b['assertions'] and match_compared >= 20
                        and (not require_rollback or (a['rollback_enabled'] and b['rollback_enabled']
                             and a['rollbacks'] + b['rollbacks'] > 0))
                        and (not (args.relay_delay_ms or args.relay_jitter_ms or args.relay_stall_ms) or impairment_exercised)
                        and (not args.require_direct or (a['direct'] and b['direct']))
                        and (not args.content or (a['dynamic_engine'] and b['dynamic_engine']))
                        and (args.host_music is None or bool(a['custom_music']))
                        and (args.join_music is None or bool(b['custom_music']))
                        and (args.external_stage is None
                             or (a['external_stage'] and b['external_stage']
                                 and set(a['external_stage']) == {args.external_stage}
                                 and set(b['external_stage']) == {args.external_stage})))
    report['renderOverlapComparison']=os.environ.get('MELEE_NETPLAY_TEST_COMPARE_RENDER')=='1'
    report['scalarBatchComparison']=args.compare_scalar_batch
    report['relay_stall_ms']=args.relay_stall_ms
    if args.relay_stall_ms:
        report['stall_exercised']='Injected traffic stall:' in (out/'server.log').read_text(errors='replace')
        report['passed']=report['passed'] and report['stall_exercised']
    if args.require_rematch:
        expected=['character select','stage select','match','results','character select','stage select','match']
        report['rematch']={
            'hostScenes':[scene.strip() for _,scene in a['epochs']],
            'joinScenes':[scene.strip() for _,scene in b['epochs']],
            'secondMatchCheckpoints':sum(e==7 for e,f in common)}
        report['rematch']['passed']=(report['rematch']['hostScenes'][:7]==expected
            and report['rematch']['joinScenes'][:7]==expected
            and report['rematch']['secondMatchCheckpoints']>=20)
        report['passed']=report['passed'] and report['rematch']['passed']
    if args.expect_aspect_reject:
        report['passed'] = (not timed_out and not report['premature_exit'] and host.returncode == 0 and join.returncode == 0
                            and report['renderer_aspects_match_request'] and bool(a['compatibility']) and bool(b['compatibility'])
                            and a['compatibility'][-1] != b['compatibility'][-1] and b['aspect_mismatch']
                            and not a['started'] and not b['started'] and not a['epochs'] and not b['epochs']
                            and not a['fault'] and not b['fault'] and not a['assertions'] and not b['assertions'])
    else:
        report['passed'] = report['passed'] and report['renderer_aspects_match_request']
    if os.environ.get("MELEE_NETPLAY_TEST_HOTJOIN")=="1":
        jointext=(out/'join/run.log').read_text(errors='replace')
        boots=re.findall(r"\[content-host\] generation=(\d+) pid=(\d+) enabled=(\d+)",jointext)
        windows=re.findall(r"\[content-host\] window=(\d+) generation=(\d+)",jointext)
        report['hotjoin']={'generations':boots,'windows':windows,
            'sameProcess':len(boots)==2 and len({p for _,p,_ in boots})==1,
            'sameWindow':len(windows)==2 and len({w for w,_ in windows})==1 and int(windows[0][0])!=0,
            'consentOffered':('This room uses Akaneia' if os.environ.get('MELEE_NETPLAY_TEST_CONTENT') else 'original Melee') in jointext}
        report['passed']=report['passed'] and all(report['hotjoin'][k] for k in ('sameProcess','sameWindow','consentOffered'))
    (out / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
