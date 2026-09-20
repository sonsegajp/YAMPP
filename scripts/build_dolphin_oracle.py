from pathlib import Path
import os,re,subprocess,shutil
root=Path(__file__).resolve().parents[1]
src=Path(os.environ.get("MELEE_DOLPHIN_SOURCE", str(root/"upstream/dolphin")))
out=root/"build/dolphin-oracle"
out.mkdir(parents=True,exist_ok=True)
vs=Path("C:/Program Files/Microsoft Visual Studio/2022/Community")
# Use the existing local library build; compile only Dolphin's unmodified NoGUI frontend.
clline=next(l for l in (src/"Build/x64/Release/DolphinLib/DolphinLib.tlog/CL.command.1.tlog").read_text(encoding="utf-16").splitlines() if l.startswith("/c "))
incs=re.findall(r'/I"([^"]+)"',clline)
defs=re.findall(r'/D ([A-Za-z_][A-Za-z_0-9]*(?:=[^ ]+)?)',clline)
incflags=['/I"'+p.rstrip("\\")+'"' for p in incs]
flags=["/nologo","/c","/O2","/MD","/EHsc","/GR-","/std:c++latest","/utf-8","/Zc:__cplusplus","/Zc:preprocessor"]+incflags+["/D"+d for d in defs]
files=["MainNoGUI","Platform","PlatformHeadless","PlatformWin32"]
commands=[]
local=out/"frontend"
local.mkdir(exist_ok=True)
main=(src/"Source/Core/DolphinNoGUI/MainNoGUI.cpp").read_text()
main=main.replace('#include "Core/System.h"', '#include "Core/System.h"\n#include "Core/Movie.h"')
main=main.replace('  DolphinAnalytics::Instance().ReportDolphinStart("nogui");', '''  if (options.is_set("movie")) {
    const std::string movie_path = static_cast<const char*>(options.get("movie"));
    if (!Core::System::GetInstance().GetMovie().PlayInput(movie_path, &save_state_path)) {
      fprintf(stderr, "Could not load oracle input movie\\n");
      return 1;
    }
    fprintf(stdout, "Oracle movie loaded: %s\\n", movie_path.c_str());
    fflush(stdout);
  }
  DolphinAnalytics::Instance().ReportDolphinStart("nogui");''')
main=main.replace('#include "Core/Movie.h"', '#include "Core/Movie.h"\n#include "Core/CoreTiming.h"\n#include "Core/HW/SystemTimers.h"\n#include "Core/HW/GCPad.h"\n#include "InputCommon/InputConfig.h"\n#include "InputCommon/ControllerEmu/ControllerEmu.h"\n#include <cstdlib>')
main=main.replace('  DolphinAnalytics::Instance().ReportDolphinStart("nogui");', """
  if (const char* timeline = std::getenv("MELEE_ORACLE_INPUT")) {
    const std::string script(timeline);
    for (unsigned port=0;port<2;port++) Pad::GetConfig()->GetController(port)->SetInputOverrideFunction(
      [script,port](std::string_view group, std::string_view control, ControlState state) -> std::optional<ControlState> {
        auto& system=Core::System::GetInstance();
        const auto frame=system.GetCoreTiming().GetTicks()*60/system.GetSystemTimers().GetTicksPerSecond();
        unsigned buttons=0;
        double stick_x=0,stick_y=0;
        const char* cursor=script.c_str();
        while (*cursor) {
          char* next=nullptr;
          unsigned long first=strtoul(cursor,&next,10);
          if (*next!=':') break;
          unsigned long mask=strtoul(next+1,&next,16);
          if (*next!=':') break;
          unsigned long length=strtoul(next+1,&next,10);
          long x=0,y=0,input_port=0;
          if (*next==':') x=strtol(next+1,&next,10);
          if (*next==':') y=strtol(next+1,&next,10);
          if (*next==':') input_port=strtol(next+1,&next,10);
          if (input_port==(long)port && frame>=first && frame<first+length) {
            buttons|=mask;stick_x=x/127.0;stick_y=y/127.0;
          }
          if (*next!=',') break;
          cursor=next+1;
        }
        if (group=="Main Stick") {
          if(control=="X") return stick_x;
          if(control=="Y") return stick_y;
        }
        if (group=="Buttons") {
          unsigned mask=control=="A"?0x100:control=="B"?0x200:control=="X"?0x400:
            control=="Y"?0x800:control=="Start"?0x1000:control=="Z"?0x10:0;
          return (buttons&mask)?1.0:0.0;
        }
        if (group=="D-Pad") {
          unsigned mask=control=="Up"?8:control=="Down"?4:control=="Left"?1:control=="Right"?2:0;
          return (buttons&mask)?1.0:0.0;
        }
        return std::nullopt;
      });
    fprintf(stdout,"Oracle timed input connected\\n"); fflush(stdout);
  }
  DolphinAnalytics::Instance().ReportDolphinStart("nogui");
""")
(local/"MainNoGUI.cpp").write_text(main)
headless=(src/"Source/Core/DolphinNoGUI/PlatformHeadless.cpp").read_text()
headless=headless.replace('#include "Core/System.h"', '#include "Core/System.h"\n#include "Core/CoreTiming.h"\n#include "Core/HW/SystemTimers.h"\n#include "Core/FifoPlayer/FifoRecorder.h"\n#include "Core/FifoPlayer/FifoDataFile.h"\n#include "Core/PowerPC/MMU.h"\n#include <cstdlib>')
headless=headless.replace('    UpdateRunningFlag();', """
    if (Core::IsRunning(Core::System::GetInstance())) {
      auto& system=Core::System::GetInstance();
      Core::CPUThreadGuard guard(system);
      const auto frame=system.GetCoreTiming().GetTicks()*60/system.GetSystemTimers().GetTicksPerSecond();
      static unsigned long long reported=~0ull;
      if (frame/60!=reported) {
        reported=frame/60; fprintf(stdout,"Oracle frame %llu\\n",(unsigned long long)frame); fflush(stdout);
      }
      // Optional deterministic match setup only. The original game still runs
      // its CSS, stage loading, rendering, and EFB copy code unchanged.
      if (std::getenv("MELEE_ORACLE_STADIUM")) {
        using PowerPC::MMU;
        if (frame>=1400 && frame<1610) {
          const auto css=MMU::HostRead<u32>(guard,0x804D6CB0);
          if(css>=0x80000000 && css<0x817FF000) for(unsigned port=0;port<2;port++) {
            const auto player=css+0x70+port*0x24,door=0x803F0DFC+port*0x24;
            MMU::HostWrite<u8>(guard,port?0:8,player);
            MMU::HostWrite<u8>(guard,port?1:0,player+1);
            MMU::HostWrite<u8>(guard,0,player+3);
            MMU::HostWrite<u8>(guard,7,player+0xf);
            MMU::HostWrite<u8>(guard,1,door+9);
            MMU::HostWrite<u8>(guard,port?1:0,door+0xb);
          }
        }
        if(frame>=1650 && frame<1660) MMU::HostWrite<u8>(guard,1,0x804D6CF6);
        if(frame>=1800 && frame<1870) {
          const auto sss=MMU::HostRead<u32>(guard,0x804D6C90);
          if(sss>=0x80000000 && sss<0x817FF000) MMU::HostWrite<u8>(guard,3,sss+3);
        }
      }
      // Oracle-only Adventure start block. No game logic/rendering is replaced.
      if (std::getenv("MELEE_ORACLE_MAZE") && frame>1200 && frame<2400) {
        using PowerPC::MMU;
        const auto base=MMU::HostRead<u32>(guard,0x804D3EE0);
        if(base>=0x80000000 && base<0x817F0000)
          MMU::HostWrite<u8>(guard,2,base+0x522+5);
        if(frame>=1500 && frame<1800) {
          const auto css=MMU::HostRead<u32>(guard,0x804D6CB0);
          if(css>=0x80000000 && css<0x817FF000) {
            MMU::HostWrite<u8>(guard,6,css+0x70);
            MMU::HostWrite<u8>(guard,0,css+0x71);
            MMU::HostWrite<u8>(guard,0,css+0x73);
            MMU::HostWrite<u8>(guard,7,css+0x7f);
            MMU::HostWrite<u8>(guard,1,0x803F0DFC+9);
            MMU::HostWrite<u8>(guard,0,0x803F0DFC+0xb);
          }
        }
        if(frame>=1800 && frame<1810) MMU::HostWrite<u8>(guard,1,0x804D6CF6);
      }
      const char* screenshot_dir=std::getenv("MELEE_ORACLE_CAPTURES");
      static unsigned long long last_capture=~0ull;
      if(screenshot_dir && frame/120!=last_capture) {
        last_capture=frame/120;
        Core::SaveScreenShot("frame_"+std::to_string(frame));
      }
      static bool recorded=false;
      const char* record_path=std::getenv("MELEE_ORACLE_FIFO");
      const char* record_frame=std::getenv("MELEE_ORACLE_FIFO_FRAME");
      if (!recorded && record_path && frame>=strtoul(record_frame?record_frame:"60",nullptr,10)) {
        recorded=true;
        const std::string path(record_path);
        system.GetFifoRecorder().StartRecording(1,[path] {
          auto* file=Core::System::GetInstance().GetFifoRecorder().GetRecordedFile();
          if (file) {
            const bool ok=file->Save(path);
            fprintf(stdout,"Oracle FIFO saved %d: %s\\n",ok,path.c_str()); fflush(stdout);
          }
        });
      }
    }
    UpdateRunningFlag();
""")
(local/"PlatformHeadless.cpp").write_text(headless)

for f in files:
    commands.append('cl.exe '+' '.join(flags)+' /Fo"'+str(out/(f+".obj"))+'" "'+str(local/(f+".cpp") if f in ("MainNoGUI","PlatformHeadless") else src/"Source/Core/DolphinNoGUI"/(f+".cpp"))+'"')
linktext=(src/"Build/x64/Release/Dolphin/Dolphin.tlog/link.command.1.tlog").read_text(encoding="utf-16")
line=next(l for l in linktext.splitlines() if l.startswith("/OUT:"))
libs=re.findall(r'(?<![A-Za-z0-9_])([A-Za-z0-9_-]+\.LIB)\b',line,re.I)
libs=[l for l in libs if not l.upper().startswith(("QT6","DOLPHIN"))]
full_libs=re.findall(r'^"([^"]+\.LIB)"$',linktext,re.M|re.I)
libpaths=re.findall(r'/LIBPATH:"([^"]+)"',line)
link=['/NOLOGO','/OUT:"'+str(out/"DolphinNoGUI.exe")+'"','/SUBSYSTEM:CONSOLE','/OPT:REF','/OPT:ICF','/MACHINE:X64','/INCLUDE:enableCompatPatches']
link+=['/LIBPATH:"'+p.rstrip("\\")+'"' for p in libpaths if "\\QT\\" not in p.upper()]
link+=[str(src/"Build/x64/Release/pch/pch.obj")]
link+=libs+['"'+p+'"' for p in full_libs]+['"'+str(out/(f+".obj"))+'"' for f in files]
(out/"link.rsp").write_text("\n".join(link))
commands+=['link.exe @"'+str(out/"link.rsp")+'"']
bat='@echo off\ncall "'+str(vs/"VC/Auxiliary/Build/vcvars64.bat")+'"\n'
for c in commands: bat+=c+'\nif errorlevel 1 exit /b %errorlevel%\n'
(out/"build.cmd").write_text(bat)
with (out/"build.log").open("w") as log:
    r=subprocess.run(["cmd","/c",str(out/"build.cmd")],stdout=log,stderr=subprocess.STDOUT)
print((out/"build.log").read_text(errors="replace")[-9000:])
if r.returncode: raise SystemExit(r.returncode)
for dll in (src/"Binary/x64").glob("*.dll"):
    shutil.copy2(dll,out/dll.name)
shutil.copytree(src/"Data/Sys",out/"Sys",dirs_exist_ok=True)
print(out/"DolphinNoGUI.exe")
