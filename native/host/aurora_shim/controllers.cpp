// Controller mapping and live diagnostics. SDL/PAD access stays on the render thread.
#include "controllers.h"
#include <SDL3/SDL.h>
#include <dolphin/pad.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace {
constexpr unsigned buttons[12]={PAD_BUTTON_A,PAD_BUTTON_B,PAD_BUTTON_X,PAD_BUTTON_Y,PAD_BUTTON_START,PAD_TRIGGER_Z,PAD_TRIGGER_L,PAD_TRIGGER_R,PAD_BUTTON_UP,PAD_BUTTON_DOWN,PAD_BUTTON_LEFT,PAD_BUTTON_RIGHT};
constexpr const char* names[22]={"A","B","X","Y","Start / Pause","Z","L click","R click","D-pad Up","D-pad Down","D-pad Left","D-pad Right","Stick Right","Stick Left","Stick Up","Stick Down","C-stick Right","C-stick Left","C-stick Up","C-stick Down","L analog","R analog"};
constexpr int default_keys[22]={SDL_SCANCODE_X,SDL_SCANCODE_Z,SDL_SCANCODE_C,SDL_SCANCODE_V,SDL_SCANCODE_RETURN,SDL_SCANCODE_LSHIFT,SDL_SCANCODE_Q,SDL_SCANCODE_E,SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT,SDL_SCANCODE_D,SDL_SCANCODE_A,SDL_SCANCODE_W,SDL_SCANCODE_S,SDL_SCANCODE_L,SDL_SCANCODE_J,SDL_SCANCODE_I,SDL_SCANCODE_K,SDL_SCANCODE_Q,SDL_SCANCODE_E};
std::array<std::array<int,22>,4> keys;
std::array<bool,4> tap_jump{true,true,true,true};
std::atomic<bool> requested{false},back{false},capture{false},menu_capture{false};
bool opened=false,release_gate=true,bind_armed=false;
ControllerMenuView view{};
unsigned prior=0;uint64_t repeat_at=0,opened_at=0,bind_deadline=0;
std::filesystem::path key_path;
bool key_down(int scan) {
 if(scan<=0||scan>=SDL_SCANCODE_COUNT)return false;
 int count=0;const bool* state=SDL_GetKeyboardState(&count);
 bool pressed=state&&scan<count&&state[scan];
#ifdef _WIN32
 SDL_Keycode code=SDL_GetKeyFromScancode((SDL_Scancode)scan,SDL_KMOD_NONE,false);int vk=0;
 if(code>='a'&&code<='z')vk=int(code-'a'+'A');else if(code>=32&&code<=126)vk=VkKeyScanA((char)code)&255;
 else switch(code){case SDLK_RETURN:vk=VK_RETURN;break;case SDLK_ESCAPE:vk=VK_ESCAPE;break;case SDLK_BACKSPACE:vk=VK_BACK;break;case SDLK_TAB:vk=VK_TAB;break;case SDLK_LSHIFT:vk=VK_LSHIFT;break;case SDLK_RSHIFT:vk=VK_RSHIFT;break;case SDLK_LCTRL:vk=VK_LCONTROL;break;case SDLK_RCTRL:vk=VK_RCONTROL;break;case SDLK_LALT:vk=VK_LMENU;break;case SDLK_RALT:vk=VK_RMENU;break;case SDLK_UP:vk=VK_UP;break;case SDLK_DOWN:vk=VK_DOWN;break;case SDLK_LEFT:vk=VK_LEFT;break;case SDLK_RIGHT:vk=VK_RIGHT;break;default:break;}
 DWORD foreground_process=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground_process);
 if(foreground_process==GetCurrentProcessId()&&vk>0&&vk<256)pressed=pressed||((GetAsyncKeyState(vk)&0x8000)!=0);
#endif
 return pressed;
}
void apply_keys(unsigned port) {
 PADKeyButtonBinding b[PAD_BUTTON_COUNT];PADKeyAxisBinding a[PAD_AXIS_COUNT];bool enabled=false;
 for(unsigned i=0;i<12;i++){b[i]={keys[port][i],(PADButton)buttons[i]};enabled|=keys[port][i]>0;}
 for(unsigned i=0;i<10;i++){a[i]={keys[port][12+i],(PADAxis)i,100};enabled|=keys[port][12+i]>0;}
 PADSetKeyButtonBindings(port,b);PADSetKeyAxisBindings(port,a);PADSetKeyboardActive(port,enabled);
}
bool save_keys() {
 std::error_code ec;std::filesystem::create_directories(key_path.parent_path(),ec);
 auto temp=key_path;temp += ".tmp";
 {std::ofstream f(temp,std::ios::trunc);if(!f)return false;f<<"YAMPP_KEYS 2\n";for(unsigned p=0;p<4;p++){for(int k:keys[p])f<<k<<' ';f<<tap_jump[p]<<'\n';}f.flush();if(!f)return false;}
#ifdef _WIN32
 return MoveFileExW(temp.c_str(),key_path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
 std::filesystem::rename(temp,key_path,ec);return !ec;
#endif
}
void saved() {PADSerializeMappings();view.message=save_keys()?"Mapping saved.":"Could not save keyboard settings.";}
SDL_Gamepad* device() {int index=PADGetIndexForPort(view.port);return index<0?nullptr:PADGetSDLGamepadForIndex(index);}
void fill_view() {
 view.tap_jump=tap_jump[view.port];
 const char* name=PADGetName(view.port);if(name&&!*name)name=nullptr;view.connected=name!=nullptr;
 view.device=view.keyboard?"Keyboard":name?name:"No controller connected";
 view.count=view.keyboard?26:31;
 for(int i=0;i<view.count;i++){view.labels[i].clear();view.values[i].clear();}
 view.labels[0]="Device";view.values[0]=view.device;
 u32 bn=0,an=0;auto* bm=PADGetButtonMappings(view.port,&bn);auto* am=PADGetAxisMappings(view.port,&an);
 for(int i=0;i<22;i++){
  view.labels[i+1]=names[i];view.values[i+1]="Unbound";
  if(view.keyboard){int k=keys[view.port][i];if(k>0)view.values[i+1]=SDL_GetScancodeName((SDL_Scancode)k);}
  else if(i<12&&bm){for(unsigned j=0;j<bn;j++)if(bm[j].padButton==buttons[i]&&bm[j].nativeButton!=PAD_NATIVE_BUTTON_INVALID){const char* n=PADGetNativeButtonName(bm[j].nativeButton);view.values[i+1]=n?n:"Button";}}
  else if(i>=12&&am){for(unsigned j=0;j<an;j++)if(am[j].padAxis==i-12){const char* n=am[j].nativeAxis.nativeAxis>=0?PADGetNativeAxisName(am[j].nativeAxis):am[j].nativeButton>=0?PADGetNativeButtonName(am[j].nativeButton):nullptr;if(n)view.values[i+1]=n;}}
 }
 if(view.keyboard){view.labels[23]="Restore default keys";view.labels[24]="Tap jump";view.values[24]=tap_jump[view.port]?"On":"Off";view.labels[25]="Test inputs";}
 else {
  auto* dz=PADGetDeadZones(view.port);
  view.labels[23]="Main dead zone";view.labels[24]="C-stick deadzone";
  view.labels[25]="Trigger clicks";view.labels[26]="Click threshold";
  view.labels[27]="Test rumble";view.labels[28]="Restore mapping";view.labels[29]="Tap jump";view.values[29]=tap_jump[view.port]?"On":"Off";view.labels[30]="Test inputs";
  if(dz){view.values[23]=std::to_string(dz->useDeadzones?int(dz->stickDeadZone)*100/32767:0)+"%";view.values[24]=std::to_string(dz->useDeadzones?int(dz->substickDeadZone)*100/32767:0)+"%";view.values[25]=dz->emulateTriggers?"Emulated":"Physical";view.values[26]=std::to_string(int(dz->leftTriggerActivationZone)*100/32767)+"%";}
 }
 view.row=std::clamp(view.row,0,view.count-1);view.first=std::clamp(view.row-3,0,std::max(0,view.count-8));
}
void adjust(int direction) {
 if(view.row==0){if(view.keyboard)return;int n=(int)PADCount();if(n){int next=(PADGetIndexForPort(view.port)+direction+n)%n;PADSetPortForIndex(next,view.port);view.message="Controller assigned to port "+std::to_string(view.port+1)+".";}return;}
 if(view.row==(view.keyboard?24:29)){tap_jump[view.port]=!tap_jump[view.port];saved();return;}
 if(view.keyboard)return;
 auto* d=PADGetDeadZones(view.port);if(!d)return;
 if(view.row==23||view.row==24){auto& v=view.row==23?d->stickDeadZone:d->substickDeadZone;v=(u16)std::clamp(int(v)+direction*655,0,13107);d->useDeadzones=true;}
 else if(view.row==25)d->emulateTriggers=!d->emulateTriggers;
 else if(view.row==26){d->leftTriggerActivationZone=(u16)std::clamp(int(d->leftTriggerActivationZone)+direction*1638,3276,32767);d->rightTriggerActivationZone=d->leftTriggerActivationZone;}
 else return;
 saved();
}
void accept() {
 if(view.row==0){adjust(1);return;}
 if(view.row<=22){if(!view.keyboard&&!device()){view.message="Connect a controller before mapping.";return;}view.binding=view.row;bind_armed=false;capture=true;bind_deadline=SDL_GetTicks()+15000;view.message="Release controls, then press the new input.";return;}
 if((view.keyboard&&view.row==23)||(!view.keyboard&&view.row==28)){
  if(view.keyboard){for(int i=0;i<22;i++)keys[view.port][i]=view.port==0?default_keys[i]:-1;apply_keys(view.port);}
  else PADRestoreDefaultMapping(view.port);
  saved();return;
 }
 if(view.row==view.count-1){view.live_test=1;view.message="Move sticks and press buttons.";release_gate=true;return;}
 if(!view.keyboard&&view.row==27){if(auto* g=device())SDL_RumbleGamepad(g,0x6000,0x6000,400);view.message="Rumble test: 0.4 seconds.";return;}
 adjust(1);
}
unsigned navigation(const PADStatus* pads) {
 unsigned bits=0;for(unsigned i=0;i<4;i++)if(pads[i].err==0){if(std::getenv("MELEE_TEST_SDL_INPUT")){const char* name=PADGetName(i);if(!name||std::strcmp(name,"YAMPP virtual GameCube test"))continue;}bits|=pads[i].button;if(pads[i].stickY>55)bits|=PAD_BUTTON_UP;if(pads[i].stickY<-55)bits|=PAD_BUTTON_DOWN;if(pads[i].stickX>55)bits|=PAD_BUTTON_RIGHT;if(pads[i].stickX<-55)bits|=PAD_BUTTON_LEFT;}
 if(key_down(SDL_SCANCODE_UP))bits|=PAD_BUTTON_UP;if(key_down(SDL_SCANCODE_DOWN))bits|=PAD_BUTTON_DOWN;if(key_down(SDL_SCANCODE_LEFT))bits|=PAD_BUTTON_LEFT;if(key_down(SDL_SCANCODE_RIGHT))bits|=PAD_BUTTON_RIGHT;
 if(key_down(SDL_SCANCODE_RETURN))bits|=PAD_BUTTON_A;
 if(key_down(SDL_SCANCODE_TAB))bits|=PAD_BUTTON_X;
 if(key_down(SDL_SCANCODE_F2))bits|=PAD_BUTTON_Y;
 return bits;
}
void capture_binding(unsigned nav) {
 const bool cancel=key_down(SDL_SCANCODE_ESCAPE)||((nav&(PAD_BUTTON_START|PAD_BUTTON_B))==(PAD_BUTTON_START|PAD_BUTTON_B));
 if(cancel||SDL_GetTicks()>bind_deadline||(!view.keyboard&&!device())){view.binding=0;capture=false;view.message=cancel?"Mapping canceled.":"Mapping ended: controller unavailable or timed out.";release_gate=true;return;}
 int key=-1;
 if(view.keyboard){int n=0;const bool* s=SDL_GetKeyboardState(&n);for(int i=1;i<n;i++)if(s[i]){key=i;break;}}
 int button=view.keyboard?-1:PADGetNativeButtonPressed(view.port);auto axis=PADGetNativeAxisPulled(view.port);
 bool neutral=view.keyboard?key<0:button<0&&axis.nativeAxis<0;
 if(!bind_armed){bind_armed=neutral;return;}
 int target=view.binding-1;
 if(view.keyboard&&key>=0){keys[view.port][target]=key;apply_keys(view.port);}
 else if(!view.keyboard&&target<12&&button>=0){PADSetButtonMapping(view.port,{(u32)button,(PADButton)buttons[target]});}
 else if(!view.keyboard&&target>=12&&(axis.nativeAxis>=0||button>=0)){PADAxisMapping m{{axis.nativeAxis,axis.sign},axis.nativeAxis>=0?-1:button,(PADAxis)(target-12)};PADSetAxisMapping(view.port,m);}
 else return;
 view.binding=0;capture=false;saved();release_gate=true;
}
}
extern "C" void aushim_controllers_init() {
 for(auto& p:keys)p.fill(-1);for(int i=0;i<22;i++)keys[0][i]=default_keys[i];
 const char* settings=std::getenv("MELEE_SETTINGS");key_path=std::filesystem::u8path(settings&&*settings?settings:"user/settings.xml").parent_path()/"controller-keyboard-v1.txt";
 std::ifstream f(key_path);std::string tag;int version=0;auto candidate=keys;
 if(f>>tag>>version&&tag=="YAMPP_KEYS"&&(version==1||version==2)){
  bool valid=true;auto jump=tap_jump;
  for(unsigned p=0;p<4;p++){
   for(int& k:candidate[p])if(!(f>>k)||k<-1||k>=SDL_SCANCODE_COUNT)valid=false;
   if(version==2){int value=-1;if(!(f>>value)||(value!=0&&value!=1))valid=false;else jump[p]=value!=0;}
  }
  std::string extra;if(f>>extra)valid=false;if(valid){keys=candidate;tap_jump=jump;}
 }
 for(unsigned p=0;p<4;p++)apply_keys(p);
}
extern "C" void aushim_controllers_open(int on){requested.store(on!=0);if(!on)back=false;}
extern "C" AUSHIM_API int aushim_controllers_back(){return back.load()?1:0;}
extern "C" int aushim_controllers_capturing(){return requested.load()&&menu_capture.load();}
extern "C" void aushim_controller_keyboard_fallback(unsigned port,AushimPadStatus* s){
 if(port>=4||!s)return;bool enabled=false;for(int k:keys[port])enabled|=k>0;if(!enabled)return;
 for(int i=0;i<12;i++)if(key_down(keys[port][i]))s->buttons|=(uint16_t)buttons[i];
 const int x=int(key_down(keys[port][12]))-int(key_down(keys[port][13]));const int y=int(key_down(keys[port][14]))-int(key_down(keys[port][15]));
 const int cx=int(key_down(keys[port][16]))-int(key_down(keys[port][17]));const int cy=int(key_down(keys[port][18]))-int(key_down(keys[port][19]));
 if(x)s->stick_x=(int8_t)(x*127);if(y)s->stick_y=(int8_t)(y*127);if(cx)s->cstick_x=(int8_t)(cx*127);if(cy)s->cstick_y=(int8_t)(cy*127);
 if(key_down(keys[port][20]))s->trigger_left=255;if(key_down(keys[port][21]))s->trigger_right=255;s->error=0;
}
extern "C" void aushim_controllers_update(){
 bool want=requested.load();if(want!=opened){opened=want;view={};view.binding=0;view.message="Select an input to remap.";release_gate=true;prior=0;capture=false;menu_capture=want;opened_at=SDL_GetTicks();}
 if(!opened||back.load())return;
 if(!SDL_GetKeyboardFocus()&&!std::getenv("MELEE_TEST_SDL_INPUT"))return;
 menu_capture=!view.live_test;
 PADStatus pads[4]{};PADRead(pads);
 auto& p=pads[view.port];view.pad={p.button,p.stickX,p.stickY,p.substickX,p.substickY,p.triggerLeft,p.triggerRight,p.analogA,p.analogB,p.err,0,0};
 aushim_controller_keyboard_fallback(view.port,&view.pad);
 unsigned nav=navigation(pads);bool esc=key_down(SDL_SCANCODE_ESCAPE);
 if(view.binding){capture_binding(nav);fill_view();return;}
 if(release_gate){if(!nav&&!esc&&SDL_GetTicks()-opened_at>200)release_gate=false;prior=nav;fill_view();return;}
 if(view.live_test){if(esc||((nav&(PAD_BUTTON_START|PAD_BUTTON_B))==(PAD_BUTTON_START|PAD_BUTTON_B))){view.live_test=0;release_gate=true;view.message="Input test finished.";}fill_view();return;}
 unsigned edge=nav&~prior;uint64_t now=SDL_GetTicks();const unsigned arrows=PAD_BUTTON_UP|PAD_BUTTON_DOWN|PAD_BUTTON_LEFT|PAD_BUTTON_RIGHT;
 if(nav!=prior)repeat_at=now+350;else if((nav&arrows)&&now>=repeat_at){edge|=nav&arrows;repeat_at=now+95;}prior=nav;
 if(esc||(edge&PAD_BUTTON_B)){PADSerializeMappings();back=true;return;}
 if(edge&PAD_BUTTON_X){view.port=(view.port+1)%4;view.row=0;view.message="Port "+std::to_string(view.port+1);release_gate=true;}
 else if(edge&PAD_BUTTON_Y){view.keyboard=!view.keyboard;view.row=0;view.message=view.keyboard?"Keyboard mapping":"Controller mapping";release_gate=true;}
 else if(edge&PAD_TRIGGER_Z){tap_jump[view.port]=!tap_jump[view.port];saved();}
 else if(edge&PAD_BUTTON_UP)view.row=(view.row+view.count-1)%view.count;
 else if(edge&PAD_BUTTON_DOWN)view.row=(view.row+1)%view.count;
 else if(edge&PAD_BUTTON_LEFT)adjust(-1);
 else if(edge&PAD_BUTTON_RIGHT)adjust(1);
 else if(edge&PAD_BUTTON_A)accept();
 fill_view();
}
const ControllerMenuView& controller_menu_view(){return view;}

extern "C" int aushim_controller_tap_jump(unsigned port){return port<4?tap_jump[port]:1;}

extern "C" int aushim_controller_test_ready(){return requested.load()&&opened&&view.count>0&&!back.load();}

extern "C" int aushim_controller_keyboard_enabled(unsigned port){if(port>=4)return 0;for(int k:keys[port])if(k>0)return 1;return 0;}
