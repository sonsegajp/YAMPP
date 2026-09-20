// Online profile editor. SDL and draft state belong to the render thread.
#include "profile.h"
#include "controllers.h"
#include <SDL3/SDL.h>
#include <dolphin/pad.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#else
#include <openssl/evp.h>
#endif
extern "C" void aushim_settings_netplay(char*,unsigned,char*,unsigned);
extern "C" void aushim_settings_set_netplay(const char*,const char*);
namespace {
std::atomic<bool> requested{false},back{false},typing{false};
std::atomic<void (*)(const char*,const unsigned char*,unsigned,const char*)> changed{nullptr};
ProfileMenuView view;
std::string saved_name="Player",saved_hash,edit_before;
std::vector<unsigned char> saved_pixels;
std::filesystem::path profile_path;
std::mutex image_lock;
struct Avatar { std::string hash; std::vector<unsigned char> pixels; };
std::vector<Avatar> avatars;
std::mutex dialog_lock;
std::string dialog_path;
bool dialog_finished=false,opened=false,release_gate=true;
unsigned generation=0;
unsigned prior=0; uint64_t repeat_at=0;
SDL_Window* text_window=nullptr;
bool allowed(unsigned char c) {return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||std::strchr(" .-_[]/:",c);}
void append(const char* text) {if(text)for(;*text&&view.name.size()<31;text++)if((unsigned char)*text<128&&allowed((unsigned char)*text))view.name+=*text;}
std::string digest(const std::vector<unsigned char>& pixels) {
 if(pixels.empty())return {};
 unsigned char bytes[32];
#ifdef _WIN32
 HCRYPTPROV provider=0;HCRYPTHASH hash=0;DWORD size=32;
 bool ok=CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)&&CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)&&CryptHashData(hash,pixels.data(),(DWORD)pixels.size(),0)&&CryptGetHashParam(hash,HP_HASHVAL,bytes,&size,0)&&size==32;
 if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);if(!ok)return {};
#else
 unsigned size=0;if(!EVP_Digest(pixels.data(),pixels.size(),bytes,&size,EVP_sha256(),nullptr)||size!=32)return {};
#endif
 std::string out;const char* hex="0123456789abcdef";for(auto b:bytes){out+=hex[b>>4];out+=hex[b&15];}return out;
}
void publish() {if(auto fn=changed.load())fn(saved_name.c_str(),saved_pixels.data(),(unsigned)saved_pixels.size(),saved_hash.c_str());}
void text_stop() {if(text_window)SDL_StopTextInput(text_window);text_window=nullptr;view.editing=false;typing=false;release_gate=true;}
bool SDLCALL text_event(void*,SDL_Event* event) {
 if(!view.editing||!text_window)return true;
 auto id=SDL_GetWindowID(text_window);
 if(event->type==SDL_EVENT_TEXT_INPUT&&event->text.windowID==id)append(event->text.text);
 else if(event->type==SDL_EVENT_KEY_DOWN&&event->key.windowID==id){
  if(event->key.key==SDLK_BACKSPACE){if(!view.name.empty())view.name.pop_back();}
  else if(!event->key.repeat&&(event->key.key==SDLK_RETURN||event->key.key==SDLK_KP_ENTER)){text_stop();view.message="Name edited. X: Save.";}
  else if(!event->key.repeat&&event->key.key==SDLK_ESCAPE){view.name=edit_before;text_stop();view.message="Name edit canceled.";}
  else if(!event->key.repeat&&(event->key.mod&SDL_KMOD_CTRL)&&event->key.key==SDLK_A)view.name.clear();
  else if(!event->key.repeat&&(event->key.mod&SDL_KMOD_CTRL)&&event->key.key==SDLK_V){char* s=SDL_GetClipboardText();append(s);SDL_free(s);}
 }
 return true;
}
SDL_Window* window() {int n=0;auto** windows=SDL_GetWindows(&n);auto* w=n?windows[0]:nullptr;SDL_free(windows);return w;}
void edit_name() {text_window=window();if(!text_window)return;edit_before=view.name;view.editing=true;typing=true;SDL_StartTextInput(text_window);view.message="Enter: Accept. Esc: Cancel.";}
void SDLCALL picked(void*,const char* const* files,int) {std::lock_guard guard(dialog_lock);dialog_path=files&&files[0]?files[0]:"";dialog_finished=true;}
void choose_picture() {
 view.picking=true;view.message="Choose a PNG or BMP picture.";
 // Replay selects a test fixture through the same import path; normal builds
 // always display the OS file chooser and never read arbitrary environment paths.
 if(const char* fixture=std::getenv("MELEE_TEST_PROFILE_IMAGE")){std::lock_guard guard(dialog_lock);dialog_path=fixture;dialog_finished=true;return;}
 static const SDL_DialogFileFilter filter[]={ {"Pictures (PNG, BMP)","png;bmp"} };
 SDL_ShowOpenFileDialog(picked,nullptr,window(),filter,1,nullptr,false);
}
bool import_picture(const std::string& path) {
 auto file=std::filesystem::u8path(path);std::error_code ec;auto size=std::filesystem::file_size(file,ec);
 if(ec||size<32||size>8*1024*1024)return false;
 unsigned char h[32]{};std::ifstream input(file,std::ios::binary);if(!input.read((char*)h,32))return false;
 auto be=[&](int p){return unsigned(h[p])<<24|unsigned(h[p+1])<<16|unsigned(h[p+2])<<8|h[p+3];};
 auto le=[&](int p){return unsigned(h[p+3])<<24|unsigned(h[p+2])<<16|unsigned(h[p+1])<<8|h[p];};
 unsigned w=0,ht=0;const unsigned char png[]={137,80,78,71,13,10,26,10};
 if(!std::memcmp(h,png,8)&&!std::memcmp(h+12,"IHDR",4)){w=be(16);ht=be(20);}
 else if(h[0]=='B'&&h[1]=='M'&&le(14)>=40){w=le(18);int sy=(int)le(22);if(sy!=INT32_MIN)ht=sy<0?-sy:sy;}
 if(!w||!ht||w>4096||ht>4096||uint64_t(w)*ht>4194304)return false;
 SDL_Surface* loaded=SDL_LoadSurface(path.c_str());if(!loaded)return false;
 SDL_Surface* dst=SDL_CreateSurface(YAMPP_AVATAR_SIDE,YAMPP_AVATAR_SIDE,SDL_PIXELFORMAT_RGBA32);
 const int side=std::min(loaded->w,loaded->h);SDL_Rect crop{(loaded->w-side)/2,(loaded->h-side)/2,side,side};
 SDL_SetSurfaceBlendMode(loaded,SDL_BLENDMODE_NONE);
 bool ok=dst&&SDL_BlitSurfaceScaled(loaded,&crop,dst,nullptr,SDL_SCALEMODE_LINEAR);
 if(ok){view.pixels.resize(YAMPP_AVATAR_BYTES);for(int y=0;y<YAMPP_AVATAR_SIDE;y++)std::memcpy(view.pixels.data()+y*YAMPP_AVATAR_SIDE*4,(unsigned char*)dst->pixels+y*dst->pitch,YAMPP_AVATAR_SIDE*4);view.revision=++generation;}
 SDL_DestroySurface(dst);SDL_DestroySurface(loaded);return ok;
}
bool save() {
 std::string name=view.name;auto start=name.find_first_not_of(' ');name=start==name.npos?"Player":name.substr(start,name.find_last_not_of(' ')-start+1);
 auto hash=digest(view.pixels);if(!view.pixels.empty()&&hash.empty())return false;
 std::vector<unsigned char> bytes(44,0);std::memcpy(bytes.data(),"YAMPPPR1",8);std::memcpy(bytes.data()+8,name.c_str(),name.size());
 unsigned n=(unsigned)view.pixels.size();for(int i=0;i<4;i++)bytes[40+i]=(unsigned char)(n>>(8*i));bytes.insert(bytes.end(),view.pixels.begin(),view.pixels.end());
 std::error_code ec;std::filesystem::create_directories(profile_path.parent_path(),ec);auto temp=profile_path;temp+=".tmp";
 {std::ofstream out(temp,std::ios::binary|std::ios::trunc);if(!out.write((const char*)bytes.data(),bytes.size()).flush())return false;}
#ifdef _WIN32
 if(!MoveFileExW(temp.c_str(),profile_path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return false;
#else
 std::filesystem::rename(temp,profile_path,ec);if(ec)return false;
#endif
 saved_name=view.name=name;saved_pixels=view.pixels;saved_hash=hash;
 char server[128];aushim_settings_netplay(server,sizeof server,nullptr,0);aushim_settings_set_netplay(server,name.c_str());
 if(!hash.empty())aushim_profile_receive(hash.c_str(),saved_pixels.data(),(unsigned)saved_pixels.size());
 publish();return true;
}
}
extern "C" void aushim_profile_init() {
 const char* settings=std::getenv("MELEE_SETTINGS");profile_path=std::filesystem::u8path(settings&&*settings?settings:"user/settings.xml").parent_path()/"profile-v1.dat";
 char name[32];aushim_settings_netplay(nullptr,0,name,sizeof name);saved_name=name[0]?name:"Player";
 std::ifstream f(profile_path,std::ios::binary|std::ios::ate);auto length=f?f.tellg():std::streampos(-1);
 if(length==44||length==44+YAMPP_AVATAR_BYTES){std::vector<unsigned char> bytes((size_t)length);f.seekg(0);f.read((char*)bytes.data(),bytes.size());unsigned n=0;for(int i=0;i<4;i++)n|=unsigned(bytes[40+i])<<(8*i);
  bool valid=f&&std::memcmp(bytes.data(),"YAMPPPR1",8)==0&&bytes[39]==0&&n==bytes.size()-44;
  for(int i=8;i<40&&bytes[i];i++)if(!allowed(bytes[i])||bytes[i]>=128)valid=false;
  if(valid){saved_name=(const char*)bytes.data()+8;if(saved_name.empty())saved_name="Player";saved_pixels.assign(bytes.begin()+44,bytes.end());saved_hash=digest(saved_pixels);}
 }
 if(!saved_hash.empty())aushim_profile_receive(saved_hash.c_str(),saved_pixels.data(),(unsigned)saved_pixels.size());
 SDL_AddEventWatch(text_event,nullptr);
}
extern "C" AUSHIM_API void aushim_profile_bind(void (*fn)(const char*,const unsigned char*,unsigned,const char*)) {changed=fn;publish();}
extern "C" void aushim_profile_open(int on) {requested=on!=0;if(!on)back=false;}
extern "C" AUSHIM_API int aushim_profile_back() {return back.load();}
extern "C" int aushim_profile_capturing() {return requested.load();}
extern "C" int aushim_profile_typing() {return typing.load();}
extern "C" void aushim_profile_update() {
 bool want=requested.load();if(want!=opened){opened=want;text_stop();view={};view.name=saved_name;view.pixels=saved_pixels;view.revision=++generation;view.message="Set your Online name and picture.";prior=0;release_gate=true;}
 if(!opened||back.load())return;
 if(view.picking){std::string path;{std::lock_guard guard(dialog_lock);if(!dialog_finished)return;path=dialog_path;dialog_finished=false;dialog_path.clear();}view.picking=false;view.message=path.empty()?"Picture selection canceled.":import_picture(path)?"Picture ready. X: Save.":"PNG/BMP: up to 4 megapixels.";release_gate=true;return;}
 if(view.editing)return;
 if(!SDL_GetKeyboardFocus()&&!std::getenv("MELEE_TEST_SDL_INPUT"))return;
 PADStatus pads[4]{};PADRead(pads);unsigned nav=0;for(unsigned i=0;i<4;i++){if(std::getenv("MELEE_TEST_SDL_INPUT")){const char* name=PADGetName(i);if(!name||std::strcmp(name,"YAMPP virtual GameCube test"))continue;}AushimPadStatus p{};if(pads[i].err==0){p.buttons=pads[i].button;p.stick_y=pads[i].stickY;}aushim_controller_keyboard_fallback(i,&p);nav|=p.buttons;if(p.stick_y>55)nav|=PAD_BUTTON_UP;if(p.stick_y<-55)nav|=PAD_BUTTON_DOWN;}
 auto* keys=SDL_GetKeyboardState(nullptr);bool esc=keys[SDL_SCANCODE_ESCAPE];if(keys[SDL_SCANCODE_RETURN])nav|=PAD_BUTTON_A;
 if(release_gate){if(!nav&&!esc){release_gate=false;if(std::getenv("MELEE_TEST_UI_TRACE"))std::fprintf(stderr,"[profile-input] released time=%llu\n",(unsigned long long)SDL_GetTicks());}prior=nav;return;}
 unsigned edge=nav&~prior;auto now=SDL_GetTicks();if(nav!=prior)repeat_at=now+350;else if(now>=repeat_at){edge|=nav&(PAD_BUTTON_UP|PAD_BUTTON_DOWN);repeat_at=now+110;}prior=nav;
 if(edge&&std::getenv("MELEE_TEST_UI_TRACE"))std::fprintf(stderr,"[profile-input] row=%d buttons=%x edge=%x time=%llu\n",view.row,nav,edge,(unsigned long long)now);
 if(esc||(edge&PAD_BUTTON_B)){back=true;return;}
 if(edge&PAD_BUTTON_UP)view.row=(view.row+3)%4;
 else if(edge&PAD_BUTTON_DOWN)view.row=(view.row+1)%4;
 else if(edge&PAD_BUTTON_Y){view.pixels.clear();view.revision=++generation;view.message="Default selected. X: Save.";}
 else if((edge&PAD_BUTTON_X)||((edge&PAD_BUTTON_A)&&view.row==3)){view.message=save()?"Profile saved.":"Could not save. Try again.";release_gate=true;}
 else if(edge&PAD_BUTTON_A){if(view.row==0)edit_name();else if(view.row==1)choose_picture();else {view.pixels.clear();view.revision=++generation;view.message="Default selected. X: Save.";}}
}
extern "C" AUSHIM_API int aushim_profile_receive(const char* hash,const unsigned char* pixels,unsigned size) {
 if(!hash||std::strlen(hash)!=64||!pixels||size!=YAMPP_AVATAR_BYTES)return 0;
 std::vector<unsigned char> copy(pixels,pixels+size);if(digest(copy)!=hash)return 0;
 std::lock_guard guard(image_lock);for(auto& a:avatars)if(a.hash==hash)return 1;
 if(avatars.size()>=8)avatars.erase(avatars.begin());avatars.push_back({hash,std::move(copy)});return 1;
}
bool profile_avatar(const char* hash,std::vector<unsigned char>& pixels) {std::lock_guard guard(image_lock);for(auto& a:avatars)if(a.hash==hash){pixels=a.pixels;return true;}return false;}
const ProfileMenuView& profile_menu_view(){return view;}

extern "C" int aushim_profile_test_ready(){return requested.load()&&opened&&!back.load();}
