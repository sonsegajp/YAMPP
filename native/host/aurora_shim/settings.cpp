// Native F1 settings. The XML document contains only bounded scalar settings.
#include <windows.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include "window.hpp"
#include "aspect_preference.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
using Microsoft::WRL::ComPtr;
extern "C" void melee_set_display_aspect(float);
extern "C" void aushim_test_input();
extern "C" void melee_set_widescreen(int);
namespace {
struct Settings {int width=960,height=720,scale=1,volume=100;bool wide=false,fullscreen=false,vsync=false,mute=false,fps=false;} current;
std::string netplayServer,netplayName="Player";
std::string sanitized(const wchar_t* value,size_t cap){std::string out;for(;*value&&out.size()<cap;value++){wchar_t c=*value;if((c>=L'0'&&c<=L'9')||(c>=L'A'&&c<=L'Z')||(c>=L'a'&&c<=L'z')||c==L'.'||c==L':'||c==L'-'||c==L'_'||c==L' '||c==L'['||c==L']'||c==L'/')out.push_back((char)c);}return out;}
std::string sanitized(const char* value,size_t cap){std::wstring wide;for(;value&&*value;value++)wide.push_back((wchar_t)(unsigned char)*value);return sanitized(wide.c_str(),cap);}
std::atomic<float> gain{1.f};std::atomic<bool> opened{false};
AspectPreference aspect;
std::atomic<bool> applyPending{false}, savePending{false};std::string path;std::string error;
int integer(const wchar_t* s,int low,int high,int fallback){wchar_t* end=nullptr;long n=wcstol(s,&end,10);return end&&!*end&&n>=low&&n<=high?(int)n:fallback;}
void load(){
 const char* p=getenv("MELEE_SETTINGS");path=p&&*p?p:"user/settings.xml";
 ComPtr<IStream> stream;std::wstring wide(path.begin(),path.end());
 if(FAILED(SHCreateStreamOnFileEx(wide.c_str(),STGM_READ|STGM_SHARE_DENY_WRITE,0,FALSE,nullptr,&stream)))return;
 ComPtr<IXmlReader> reader;if(FAILED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(reader.GetAddressOf()),nullptr)))return;
 reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit);reader->SetInput(stream.Get());XmlNodeType node;
 while(reader->Read(&node)==S_OK){if(node!=XmlNodeType_Element)continue;const wchar_t* name;reader->GetLocalName(&name,nullptr);if(wcscmp(name,L"melee-settings")){error="Invalid settings XML root";return;}
  if(reader->MoveToFirstAttribute()==S_OK)do{const wchar_t* value;reader->GetLocalName(&name,nullptr);reader->GetValue(&value,nullptr);
   if(!wcscmp(name,L"width"))current.width=integer(value,640,7680,960);
   else if(!wcscmp(name,L"height"))current.height=integer(value,480,4320,720);
   else if(!wcscmp(name,L"renderScale"))current.scale=integer(value,0,4,1);
   else if(!wcscmp(name,L"volume"))current.volume=integer(value,0,100,100);
   else if(!wcscmp(name,L"widescreen"))current.wide=integer(value,0,1,0)!=0;
   else if(!wcscmp(name,L"fullscreen"))current.fullscreen=integer(value,0,1,0)!=0;
   else if(!wcscmp(name,L"vsync"))current.vsync=integer(value,0,1,0)!=0;
   else if(!wcscmp(name,L"mute"))current.mute=integer(value,0,1,0)!=0;
   else if(!wcscmp(name,L"showFps"))current.fps=integer(value,0,1,0)!=0;
   else if(!wcscmp(name,L"netplayServer"))netplayServer=sanitized(value,120);
   else if(!wcscmp(name,L"netplayName"))netplayName=sanitized(value,31);
  }while(reader->MoveToNextAttribute()==S_OK);break;
 }
 gain.store(current.mute?0.f:current.volume/100.f);
}
void save(){
 std::string temp=path+".tmp";FILE* f=fopen(temp.c_str(),"wb");if(!f){error="Cannot save settings XML";return;}
 int n=fprintf(f,"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<melee-settings schema=\"1\" width=\"%d\" height=\"%d\" renderScale=\"%d\" widescreen=\"%d\" fullscreen=\"%d\" vsync=\"%d\" volume=\"%d\" mute=\"%d\" showFps=\"%d\" netplayServer=\"%s\" netplayName=\"%s\" />\n",current.width,current.height,current.scale,aspect.value(),current.fullscreen,current.vsync,current.volume,current.mute,current.fps,netplayServer.c_str(),netplayName.c_str());
 bool ok=n>0;if(fclose(f))ok=false;if(!ok||!MoveFileExA(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))error="Cannot commit settings XML";else error.clear();
}
void apply(){
 aurora::window::set_fullscreen(current.fullscreen);
 if(!current.fullscreen)aurora::window::set_window_size(current.width,current.height);
 int wide=aspect.value();
 melee_set_display_aspect(wide?16.f/9.f:4.f/3.f);
 melee_set_widescreen(wide);
 aurora::window::set_frame_buffer_scale((float)current.scale);
 aurora_enable_vsync(current.vsync);
 gain.store(current.mute?0.f:current.volume/100.f);
 fprintf(stderr,"[settings] %dx%d scale=%d wide=%d fullscreen=%d vsync=%d volume=%d mute=%d\n",current.width,current.height,current.scale,aspect.value(),current.fullscreen,current.vsync,current.volume,current.mute);
}
}
extern "C" void aushim_settings_configure(AuroraConfig* cfg){load();aspect.initialize(current.wide);cfg->windowWidth=current.width;cfg->windowHeight=current.height;cfg->vsync=current.vsync;cfg->startFullscreen=current.fullscreen;}
extern "C" void aushim_settings_ready(){apply();}
extern "C" void aushim_settings_update(){aushim_test_input();if(savePending.exchange(false))save();if(applyPending.exchange(false))apply();}
extern "C" float aushim_settings_gain(){return gain.load();}
extern "C" int aushim_settings_captures_input(){return opened.load()?1:0;}
// Widescreen toggle shared with the original Options menu (native/host/gxrt/hooks.c).
extern "C" __declspec(dllexport) int aushim_settings_widescreen(int set,int value){if(set&&aspect.set(value!=0)){applyPending.store(true);savePending.store(true);}return aspect.value();}
extern "C" __declspec(dllexport) int aushim_settings_aspect_lock(int command){if(command<0)return aspect.locked()?1:0;if(command)return aspect.acquire();aspect.release();return aspect.value();}
extern "C" __declspec(dllexport) void aushim_settings_netplay(char* server,unsigned cap_s,char* name,unsigned cap_n){if(server&&cap_s)snprintf(server,cap_s,"%s",netplayServer.c_str());if(name&&cap_n)snprintf(name,cap_n,"%s",netplayName.c_str());}
extern "C" __declspec(dllexport) void aushim_settings_set_netplay(const char* server,const char* name){netplayServer=sanitized(server,120);netplayName=sanitized(name,31);save();}
extern "C" __declspec(dllexport) void aushim_settings_menu(){
 if(ImGui::IsKeyPressed(ImGuiKey_F1,false))opened.store(!opened.load());
 static bool tested=false;if(!tested){tested=true;if(getenv("MELEE_TEST_SETTINGS_OPEN"))opened=true;}
 if(current.fps){ImGui::SetNextWindowPos(ImVec2(10,10));ImGui::SetNextWindowBgAlpha(.6f);ImGui::Begin("Performance",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs);ImGui::Text("%.1f FPS",aurora_get_fps());ImGui::End();}
 if(!opened)return;
 bool visible=true;ImGui::SetNextWindowSize(ImVec2(450,470),ImGuiCond_FirstUseEver);ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x/2,ImGui::GetIO().DisplaySize.y/2),ImGuiCond_FirstUseEver,ImVec2(.5f,.5f));
 ImGui::Begin("YAMPP Settings - F1",&visible,ImGuiWindowFlags_NoCollapse);bool changed=false;
 const int resolutions[][2]={{640,480},{960,720},{1280,720},{1280,960},{1600,900},{1920,1080},{2560,1440},{3840,2160}};
 char label[64];snprintf(label,sizeof label,"%d x %d",current.width,current.height);
 if(ImGui::BeginCombo("Window resolution",label)){for(auto& r:resolutions){snprintf(label,sizeof label,"%d x %d",r[0],r[1]);if(ImGui::Selectable(label,current.width==r[0]&&current.height==r[1])){current.width=r[0];current.height=r[1];changed=true;}}ImGui::EndCombo();}
 const char* scales[]={"Match window","1x (480p)","2x (960p)","3x (1440p)","4x (1920p)"};changed|=ImGui::Combo("Render resolution",&current.scale,scales,5);
 bool wide=aspect.value()!=0;ImGui::BeginDisabled(aspect.locked());
 if(ImGui::Checkbox("Widescreen (16:9)",&wide))aushim_settings_widescreen(1,wide);
 ImGui::EndDisabled();if(aspect.locked())ImGui::TextDisabled("Disconnect from Online to change aspect ratio.");
 changed|=ImGui::Checkbox("Fullscreen",&current.fullscreen);changed|=ImGui::Checkbox("VSync",&current.vsync);
 ImGui::Separator();changed|=ImGui::SliderInt("Master volume",&current.volume,0,100,"%d%%");changed|=ImGui::Checkbox("Mute",&current.mute);changed|=ImGui::Checkbox("Show FPS",&current.fps);
 ImGui::Separator();
 // Connection routing is private configuration. Only the player's display
 // name belongs in the visible settings panel.
 static char player[32];static bool loadedOnline=false;
 if(!loadedOnline){loadedOnline=true;snprintf(player,sizeof player,"%s",netplayName.c_str());}
 ImGui::TextUnformatted("Online");
 if(ImGui::InputText("Player name",player,sizeof player)){netplayName=sanitized(player,31);save();}
 ImGui::TextDisabled("Choose Online on the main menu to connect.");
 ImGui::Separator();ImGui::TextWrapped("Keyboard: WASD move, X attack, Z special, C/V jump, Q/E shield, Enter start.");ImGui::TextWrapped("F1 closes this menu. Settings save automatically.");
 if(!error.empty())ImGui::TextColored(ImVec4(1,.4f,.4f,1),"%s",error.c_str());
 if(ImGui::Button("Close settings"))visible=false;
 if(changed){gain.store(current.mute?0.f:current.volume/100.f);applyPending=true;save();}
 ImGui::End();if(!visible)opened=false;
}
