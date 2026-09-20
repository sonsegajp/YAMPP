// Opt-in SDL event replay for off-screen integration checks. Normal launchers
// remove MELEE_TEST_* variables, so no test input enters user sessions.
#include <SDL3/SDL.h>
#include "window.hpp"
#include <vector>
#include <fstream>
#include <string>
#include <cstdlib>
#include <cstdio>
struct TestEvent {unsigned frame;char kind[16];int a,b,c;std::string text;};
extern "C" int aushim_room_name_text_ready();
extern "C" int aushim_controller_test_ready();
extern "C" int aushim_profile_test_ready();
extern "C" int aushim_profile_typing();
static SDL_Joystick* test_pads[4]{};
static void virtual_pad(const TestEvent& e) {
 if(e.a<0||e.a>=4)return;
 auto& joy=test_pads[e.a];std::string kind=e.kind;
 if(kind=="pad_attach"&&!joy){
  SDL_VirtualJoystickDesc desc;SDL_INIT_INTERFACE(&desc);
  desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.naxes=6;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
  desc.vendor_id=0x1209;desc.product_id=0x5941;desc.name="YAMPP virtual GameCube test";
  desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;desc.axis_mask=63;
  auto id=SDL_AttachVirtualJoystick(&desc);
  if(id){
   SDL_SetGamepadMapping(id,"03000000594d00005050000000000000,YAMPP virtual GameCube test,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,misc3:b17,misc4:b18,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,type:gamecube,");
   joy=SDL_OpenJoystick(id);
   if(joy){SDL_SetJoystickVirtualAxis(joy,4,-32768);SDL_SetJoystickVirtualAxis(joy,5,-32768);}
  }
  std::fprintf(stderr,"[controller-test] attached port=%d ok=%d\n",e.a,joy!=nullptr);
 }else if(kind=="pad_button"&&joy)SDL_SetJoystickVirtualButton(joy,e.b,e.c!=0);
 else if(kind=="pad_axis"&&joy)SDL_SetJoystickVirtualAxis(joy,e.b,(Sint16)e.c);
 else if(kind=="pad_detach"&&joy){auto id=SDL_GetJoystickID(joy);SDL_CloseJoystick(joy);joy=nullptr;SDL_DetachVirtualJoystick(id);}
}
extern "C" void aushim_test_input(){
 static unsigned frame=0,frame_base=0;static size_t next=0;static std::vector<TestEvent> events;static bool loaded=false;static float x=0,y=0;
 if(!loaded){loaded=true;const char* path=getenv("MELEE_TEST_SDL_INPUT");if(path){std::ifstream file(path);std::string line;while(std::getline(file,line)){TestEvent event{};int offset=0;if(sscanf(line.c_str(),"%u,%15[^,],%n",&event.frame,event.kind,&offset)>=2&&offset>0){if(std::string(event.kind)=="text"){event.text=line.substr(offset);events.push_back(event);}else if(sscanf(line.c_str()+offset,"%d,%d,%d",&event.a,&event.b,&event.c)>=2)events.push_back(event);}}}}
 ++frame;const auto window=SDL_GetWindowID(aurora::window::get_sdl_window());
 while(next<events.size()&&events[next].frame+frame_base<=frame){
  if(std::string(events[next].kind)=="wait_name"){if(!aushim_room_name_text_ready())break;frame_base=frame;++next;continue;}
  if(std::string(events[next].kind)=="wait_controls"){if(!aushim_controller_test_ready())break;frame_base=frame;++next;continue;}
  if(std::string(events[next].kind)=="wait_profile"){if(!aushim_profile_test_ready())break;frame_base=frame;++next;continue;}
  if(std::string(events[next].kind)=="wait_typing"){if(!aushim_profile_typing())break;frame_base=frame;++next;continue;}
  auto& input=events[next++];SDL_Event event{};
  if(std::getenv("MELEE_TEST_UI_TRACE"))std::fprintf(stderr,"[ui-test] frame=%u kind=%s a=%d b=%d c=%d time=%llu\n",frame,input.kind,input.a,input.b,input.c,(unsigned long long)SDL_GetTicks());
  if(std::string(input.kind).rfind("pad_",0)==0){virtual_pad(input);SDL_UpdateJoysticks();continue;}
  if(std::string(input.kind)=="text"){event.type=SDL_EVENT_TEXT_INPUT;event.text.windowID=window;event.text.text=input.text.c_str();}
  else if(std::string(input.kind)=="key"){event.type=input.b?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;event.key.windowID=window;event.key.scancode=(SDL_Scancode)input.a;event.key.key=SDL_GetKeyFromScancode(event.key.scancode,SDL_KMOD_NONE,false);event.key.down=input.b!=0;}
  else if(std::string(input.kind)=="mouse"){x=(float)input.a;y=(float)input.b;event.type=SDL_EVENT_MOUSE_MOTION;event.motion.windowID=window;event.motion.x=x;event.motion.y=y;}
  else if(std::string(input.kind)=="button"){event.type=input.b?SDL_EVENT_MOUSE_BUTTON_DOWN:SDL_EVENT_MOUSE_BUTTON_UP;event.button.windowID=window;event.button.button=(Uint8)input.a;event.button.down=input.b!=0;event.button.x=x;event.button.y=y;}
  else continue;
  SDL_PushEvent(&event);
 }
}
