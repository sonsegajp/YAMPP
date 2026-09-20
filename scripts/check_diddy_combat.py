"""Exercise Lua attack collision against an idle human player in a sandbox package."""
import sys,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1];package=root/'build/modkit/diddy-pipeline-check/mods/diddy-kong';scripts=package/'scripts'
original=(root/'tools/modkit/examples/diddy/fighter.lua').read_text();(scripts/'behavior.lua').write_text(original)
entry=scripts/'fighter.lua';backup=entry.read_bytes()
wrapper='''local f = require("behavior")
local gameplay = f.on_frame
local last_attack = -100
function f.on_frame(self)
    gameplay(self)
    local s = self:state()
    local other = self:players()[2]
    if other and s.frame % 15 == 0 then
        self:log(string.format("PROBE f=%d a=%d af=%.2f x=%.2f y=%.2f facing=%.1f target=%.2f,%.2f damage=%.1f", s.frame,s.animation,s.animation_frame,s.x,s.y,s.facing,other.x,other.y,other.damage))
    end
    for _,h in pairs(self:hitboxes()) do
        self:log(string.format("HIT f=%d id=%d pos=%.2f,%.2f,%.2f size=%.1f damage=%.1f victims=%d",s.frame,h.id,h.x,h.y,h.z,h.size,h.damage,h.victims))
    end

end
return f
'''
env=os.environ.copy();env['MELEE_WORKSHOP_TEST_MODS']=str(package.parent);sys.path.insert(0,str(root/'tools/modkit'));os.environ['MELEE_WORKSHOP_TEST_MODS']=str(package.parent);import catalog;catalog.runtime_registry()
env.update(MELEE_MODS='1',MELEE_MOD_REGISTRY=str(package.parent/'registry.tsv'),MELEE_AURORA_DLL=str(root/'build/pc/bin/melee_aurora_mod.dll'))
for k in ['MELEE_MEMORY_CARD','MELEE_SETTINGS','MELEE_MOD_TEST_FIGHTER','MELEE_MOD_TEST_STAGE']:env.pop(k,None)
timeline='300:100:8,420:100:8,600:1000:8,840:1000:8,1020:4:8,1110:100:8,1200:100:8,1600:0:22:0:80,1700:100:8,1600:0:33:0:80:1,1650:0:27:80:0:1,1740:100:8:0:0:1,2000:1000:8,2190:0:20:0:80,2230:0:20:80:0,2310:100:8,2600:0:67:-80:0,2690:100:5,2700:100:5,2710:100:5,2800:100:5,2810:100:5,2820:100:5,2950:8:5,3120:100:6:0:80,3260:100:6:0:-50,3400:400:5,3410:100:5,3560:0:25:80:0,3680:0:25:-80:0,3800:100:5,3810:100:5,4400:1000:8,4460:1160:10,5330:1000:8,5690:1000:8,5330:1000:8:0:0:1,5690:1000:8:0:0:1'
try:
 entry.write_text(wrapper)
 result=subprocess.run([sys.executable,str(root/'scripts/run_native_check.py'),'--name',sys.argv[1],'--frames','6000','--executable','melee-mod.exe','--normal-exit','--capture','--timeline',timeline],cwd=root,env=env)
finally:entry.write_bytes(backup)
raise SystemExit(result.returncode)
