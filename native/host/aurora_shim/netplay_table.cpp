// Original-menu netplay presentation. Input and all room state remain owned by
// the runtime; this renderer receives an immutable snapshot each menu frame.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <openssl/evp.h>
#endif
#include "aurora_shim.h"
#include "controllers.h"
#include "profile.h"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sstream>
#include <vector>
#include "webgpu/gpu.hpp"
#include "gfx/png_io.hpp"
#include "../netplay_ui.h"
#include "netplay_art.inc"
#include "netplay_fonts.inc"

extern "C" int aushim_settings_widescreen(int set, int value);

namespace {
std::mutex state_lock;
MeleeNetplayUi current{};
int screen_kind = 0, screen_selection = 0;
constexpr ImU32 white = IM_COL32(232, 235, 240, 255);
constexpr ImU32 muted = IM_COL32(158, 172, 190, 255);
constexpr ImU32 gold = IM_COL32(255, 218, 35, 255);
constexpr ImU32 ink = IM_COL32(8, 12, 23, 255);
constexpr ImU32 red = IM_COL32(244, 66, 51, 255);

struct ArtTexture {
  const netplay_art::Image* source;
  wgpu::Texture texture;
  wgpu::TextureView view;
};
std::vector<ArtTexture> art_textures;
wgpu::TextureView art_view(const netplay_art::Image& asset) {
  for (const auto& cached : art_textures) if (cached.source == &asset) return cached.view;
  ArtTexture cached{&asset, {}, {}};
  auto decoded = aurora::gfx::png::parse_png_bytes({asset.data, asset.size});
  if (decoded && decoded->width == asset.width && decoded->height == asset.height) {
    wgpu::TextureDescriptor desc{};
    desc.dimension = wgpu::TextureDimension::e2D;
    desc.size = {asset.width, asset.height, 1};
    desc.format = wgpu::TextureFormat::RGBA8Unorm;
    desc.mipLevelCount = desc.sampleCount = 1;
    desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    cached.texture = aurora::webgpu::g_device.CreateTexture(&desc);
    cached.view = cached.texture.CreateView();
    wgpu::TexelCopyTextureInfo dst{}; dst.texture = cached.texture;
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = asset.width * 4; layout.rowsPerImage = asset.height;
    wgpu::Extent3D size{asset.width, asset.height, 1};
    aurora::webgpu::g_queue.WriteTexture(&dst, decoded->data.data(), decoded->data.size(), &layout, &size);
  } else std::fprintf(stderr, "[netplay-art] Failed to decode embedded foreground asset\n");
  art_textures.push_back(std::move(cached));
  return art_textures.back().view;
}
struct PreviewTexture {
  std::string hash, attempted;
  unsigned width = 0, height = 0;
  wgpu::Texture texture;
  wgpu::TextureView view;
} preview_texture;
bool preview_hash(const std::vector<uint8_t>& bytes, const char* expected) {
  if (std::strlen(expected) != 64) return false;
  unsigned char digest[32];
#ifdef _WIN32
  HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
  DWORD size = sizeof digest;
  bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)
      && CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)
      && CryptHashData(hash, bytes.data(), (DWORD)bytes.size(), 0)
      && CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) && size == 32;
  if (hash) CryptDestroyHash(hash);
  if (provider) CryptReleaseContext(provider, 0);
  if (!ok) return false;
#else
  unsigned size = 0;
  if (!EVP_Digest(bytes.data(), bytes.size(), digest, &size, EVP_sha256(), nullptr) || size != 32) return false;
#endif
  const char* digits = "0123456789abcdef";
  for (unsigned i = 0; i < 32; ++i)
    if (expected[2*i] != digits[digest[i] >> 4] || expected[2*i+1] != digits[digest[i] & 15]) return false;
  return true;
}
wgpu::TextureView preview_view(const MeleeNetplayUi& ui, const char* package) {
  if (ui.mod_inspect_loading) preview_texture.attempted.clear();
  if (ui.mod_inspect_ready != 1 || std::strcmp(package, ui.mod_inspect_package)
      || !ui.mod_preview_path[0] || std::strlen(ui.mod_inspect_sha256) != 64) return {};
  if (preview_texture.hash == ui.mod_inspect_sha256) return preview_texture.view;
  if (preview_texture.attempted == ui.mod_inspect_sha256) return {};
  preview_texture.attempted = ui.mod_inspect_sha256;
 #ifdef _WIN32
  wchar_t path[1024];
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ui.mod_preview_path, -1, path, 1024)) return {};
  FILE* file = _wfopen(path, L"rb");
 #else
  FILE* file = std::fopen(ui.mod_preview_path, "rb");
 #endif
  if (!file) return {};
  std::fseek(file, 0, SEEK_END); long length = std::ftell(file); std::rewind(file);
  if (length < 33 || length > 4*1024*1024) { std::fclose(file); return {}; }
  std::vector<uint8_t> bytes((size_t)length);
  size_t got = std::fread(bytes.data(), 1, bytes.size(), file); std::fclose(file);
  const unsigned char signature[] = {137,80,78,71,13,10,26,10};
  if (got != bytes.size() || std::memcmp(bytes.data(), signature, 8)
      || std::memcmp(bytes.data()+12, "IHDR", 4) || !preview_hash(bytes, ui.mod_inspect_sha256)) return {};
  auto be32 = [&](unsigned offset) { return (unsigned)bytes[offset]<<24 | (unsigned)bytes[offset+1]<<16 | (unsigned)bytes[offset+2]<<8 | bytes[offset+3]; };
  unsigned width = be32(16), height = be32(20);
  if (!width || width > 4096 || !height || height > 4096) return {};
  auto decoded = aurora::gfx::png::parse_png_bytes({bytes.data(), bytes.size()});
  if (!decoded || decoded->width != width || decoded->height != height
      || decoded->data.size() != (size_t)width*height*4) return {};
  wgpu::TextureDescriptor desc{};
  desc.dimension = wgpu::TextureDimension::e2D; desc.size = {width, height, 1};
  desc.format = wgpu::TextureFormat::RGBA8Unorm; desc.mipLevelCount = desc.sampleCount = 1;
  desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  auto texture = aurora::webgpu::g_device.CreateTexture(&desc); auto view = texture.CreateView();
  wgpu::TexelCopyTextureInfo dst{}; dst.texture = texture;
  wgpu::TexelCopyBufferLayout layout{}; layout.bytesPerRow = width*4; layout.rowsPerImage = height;
  wgpu::Extent3D size{width, height, 1};
  aurora::webgpu::g_queue.WriteTexture(&dst, decoded->data.data(), decoded->data.size(), &layout, &size);
  preview_texture.texture = std::move(texture); preview_texture.view = std::move(view);
  preview_texture.hash = ui.mod_inspect_sha256; preview_texture.width = width; preview_texture.height = height;
  std::fprintf(stderr, "[mod-preview] verified PNG loaded %ux%u (%ld bytes)\n", width, height, length);
  return preview_texture.view;
}
const netplay_art::Image* key_art(const char* key) {
  if (!std::strcmp(key,"A")) return &netplay_art::glyph_a;
  if (!std::strcmp(key,"B")) return &netplay_art::glyph_b;
  if (!std::strcmp(key,"X")) return &netplay_art::glyph_x;
  if (!std::strcmp(key,"Y")) return &netplay_art::glyph_y;
  if (!std::strcmp(key,"Start")) return &netplay_art::glyph_start_pause;
  return nullptr;
}

enum class NativeFont { Body, Menu, Panel };
std::vector<unsigned> text_codes(const std::string& text) {
  std::vector<unsigned> codes;
  for(size_t i=0;i<text.size();) {
    unsigned ch=(unsigned char)text[i++];
    if(ch<0x80) { codes.push_back(ch); continue; }
    unsigned continuation=ch>=0xF0&&ch<=0xF4?3:ch>=0xE0&&ch<0xF0?2:ch>=0xC2&&ch<0xE0?1:0;
    unsigned code=continuation?ch&((1u<<(6-continuation))-1):'?';
    if(!continuation||i+continuation>text.size()) { codes.push_back('?'); continue; }
    bool valid=true;
    for(unsigned j=0;j<continuation;j++) {
      unsigned next=(unsigned char)text[i+j];
      if((next&0xC0)!=0x80) { valid=false; break; }
      code=(code<<6)|(next&0x3F);
    }
    if(valid) i+=continuation;
    codes.push_back(valid?code:'?');
  }
  return codes;
}
const netplay_font::Glyph* native_glyph(const netplay_font::Font& font,unsigned code) {
  const auto* end=font.glyphs+font.count;
  const auto* found=std::lower_bound(font.glyphs,end,code,
      [](const netplay_font::Glyph& glyph,unsigned value){return glyph.code<value;});
  return found!=end&&found->code==code?found:nullptr;
}
const netplay_font::Font& native_font(const std::vector<unsigned>& codes,NativeFont kind) {
  const auto& chosen=kind==NativeFont::Panel?netplay_font::italic:kind==NativeFont::Menu?netplay_font::bold:netplay_font::sis;
  // The disc's menu-label textures contain only part of the alphabet. Use the
  // complete original SIS face for a whole arbitrary label rather than invent
  // missing letterforms or mix two faces inside one name.
  for(unsigned ch:codes) if(!native_glyph(chosen,ch)) return netplay_font::sis;
  return chosen;
}
float native_advance(const netplay_font::Font& font,const std::vector<unsigned>& codes) {
  float advance=0;
  for(unsigned ch:codes) {
    const auto* glyph=native_glyph(font,ch);
    if(!glyph) glyph=native_glyph(netplay_font::sis,'?');
    advance+=glyph?glyph->advance:16;
  }
  return advance;
}

struct Canvas {
  // Before other ImGui windows, so F1 settings remain usable over netplay.
  ImDrawList* d = ImGui::GetBackgroundDrawList();
  float scale, ox, oy;
  int first_vertex;
  Canvas() { first_vertex=d->VtxBuffer.Size; const auto size = ImGui::GetIO().DisplaySize; scale = std::min(size.x / 800.f, size.y / 600.f); ox = (size.x - 800.f * scale) / 2; oy = (size.y - 600.f * scale) / 2; }
  ImVec2 p(float x, float y) const { return {ox + x * scale, oy + y * scale}; }
  void follow_menu(const float* pose) {
    // Melee's menu projection uses a 320:219 camera basis. The native wide
    // perspective hook multiplies that camera's aspect by 320/219, whereas
    // this 800x600 canvas grows into a 16:9 viewport by only (16/9)/(4/3).
    // Match that projection after the camera homography, including every
    // vertex of text, images and rounded shapes. Window shape alone must not
    // enable the correction: a wide desktop can still run the 4:3 game.
    constexpr float native_aspect_widen = 320.f / 219.f;
    constexpr float canvas_aspect_widen = (16.f / 9.f) / (800.f / 600.f);
    const float horizontal = aushim_settings_widescreen(0, 0)
        ? canvas_aspect_widen / native_aspect_widen : 1.f;
    // Transform only vertices appended by this screen. The native scene and
    // unrelated F1 windows keep their own cameras and draw data.
    for(int i=first_vertex;i<d->VtxBuffer.Size;i++) {
      auto& pos=d->VtxBuffer[i].pos;
      const float x=(pos.x-ox)/scale,y=(pos.y-oy)/scale;
      float px=x,py=y;
      if(pose) {
        const float denominator=pose[6]*x+pose[7]*y+pose[8];
        if(std::abs(denominator)<0.0001f) continue;
        px=(pose[0]*x+pose[1]*y+pose[2])/denominator;
        py=(pose[3]*x+pose[4]*y+pose[5])/denominator;
      }
      if(std::isfinite(px)&&std::isfinite(py)) pos=p(400.f+(px-400.f)*horizontal,py);
    }
  }
  bool art(const netplay_art::Image& asset,float x,float y,float w,float h) {
    auto view=art_view(asset); if(!view) return false;
    d->AddImage((ImTextureID)view.Get(),p(x,y),p(x+w,y+h)); return true;
  }
  void box(float x, float y, float w, float h, ImU32 color, float radius = 0) { d->AddRectFilled(p(x,y), p(x+w,y+h), color, radius*scale); }
  void border(float x,float y,float w,float h,ImU32 color,float thickness=1,float radius=0) { d->AddRect(p(x,y),p(x+w,y+h),color,radius*scale,0,thickness*scale); }
  void line(float x,float y,float xx,float yy,ImU32 color,float thickness=1) { d->AddLine(p(x,y),p(xx,yy),color,thickness*scale); }
  void circle(float x,float y,float r,ImU32 color) { d->AddCircleFilled(p(x,y),r*scale,color,24); }
  void polygon(std::initializer_list<ImVec2> points,ImU32 color,ImU32 edge=0,float thickness=1) {
    std::vector<ImVec2> v; for (auto q:points) v.push_back(p(q.x,q.y)); d->AddConvexPolyFilled(v.data(),(int)v.size(),color);
    if(edge) d->AddPolyline(v.data(),(int)v.size(),edge,ImDrawFlags_Closed,thickness*scale);
  }
  void text(float x,float y,float size,ImU32 color,const std::string& raw,float max_width=0,bool center=false,bool heading=false,bool panel=false) {
    auto codes=text_codes(raw);
    const auto* font=&native_font(codes,panel?NativeFont::Panel:heading?NativeFont::Menu:NativeFont::Body);
    unsigned r=(color>>IM_COL32_R_SHIFT)&255, g=(color>>IM_COL32_G_SHIFT)&255, b=(color>>IM_COL32_B_SHIFT)&255;
    if(font==&netplay_font::bold&&std::max({r,g,b})<128) font=&netplay_font::bold_core;
    float unit=size/font->height;
    if(max_width>0&&native_advance(*font,codes)*unit>max_width) {
      if(!native_glyph(*font,'.')) { font=&netplay_font::sis; unit=size/font->height; }
      const auto* dot=native_glyph(*font,'.');
      while(!codes.empty()&&(native_advance(*font,codes)+3*dot->advance)*unit>max_width) codes.pop_back();
      codes.insert(codes.end(),3,'.');
    }
    float width=native_advance(*font,codes)*unit;
    if(center) x-=width/2;
    auto view=art_view(*font->image); if(!view) return;
    for(unsigned ch:codes) {
      const auto* glyph=native_glyph(*font,ch);
      if(!glyph) glyph=native_glyph(*font,'?');
      if(!glyph) { x+=16*unit; continue; }
      const float px=x+glyph->dx*unit,py=y+glyph->dy*unit;
      ImVec2 uv0{(float)glyph->x/font->image->width,(float)glyph->y/font->image->height};
      ImVec2 uv1{(float)(glyph->x+glyph->w)/font->image->width,(float)(glyph->y+glyph->h)/font->image->height};
      d->AddImage((ImTextureID)view.Get(),p(px,py),p(px+glyph->w*unit,py+glyph->h*unit),uv0,uv1,color);
      x+=glyph->advance*unit;
    }
  }
  void button(float x,float y,float w,float h,const char* label,bool selected,bool enabled=true) {
    auto bg=selected&&enabled?gold:IM_COL32(10,17,27,225); auto edge=selected?IM_COL32(255,247,113,255):IM_COL32(190,154,52,235);
    polygon({{x+15,y},{x+w,y},{x+w-10,y+h},{x,y+h},{x-8,y+h/2}},bg,edge,2);
    if(selected&&enabled) line(x+18,y+3,x+w-4,y+3,IM_COL32(255,255,191,255),2);
    text(x+w/2,y+3,h*.70f,!enabled?muted:selected?ink:gold,label,w-38,true,true);
  }
  float text_width(float size,const std::string& text) const {
    return native_advance(netplay_font::sis,text_codes(text))*size/netplay_font::sis.height;
  }
  void paragraph(float x,float y,float size,ImU32 color,const std::string& raw,float width,int max_lines,float line_height=0) {
    if(line_height<=0) line_height=size+3;
    std::istringstream words(raw); std::string word,line; std::vector<std::string> lines;
    while(words>>word) {
      std::string candidate=line.empty()?word:line+" "+word;
      if(!line.empty()&&text_width(size,candidate)>width) { lines.push_back(line); line=word; }
      else line=candidate;
    }
    if(!line.empty()) lines.push_back(line);
    bool more=(int)lines.size()>max_lines;
    for(int i=0;i<std::min((int)lines.size(),max_lines);i++)
      text(x,y+i*line_height,size,color,lines[i]+(more&&i==max_lines-1?"...":""),width);
  }
  void key(float x,float y,const char* key,const char* action,float width) {
    // Center each complete glyph/label group inside its padded cell. Keep the
    // common 32px glyph canvas intact, including smaller B and oval X/Y buttons.
    float label_width=std::min(text_width(20,action),width-40);
    x+=(width-40-label_width)/2;
    if(const auto* glyph=key_art(key)) art(*glyph,x,y,32,32);
    text(x+40,y+5,20,white,action,width-40);
  }
};

std::string number(int n) { return std::to_string(n); }
const char* item_name(int n) { static const char* names[]={"Items Off","Items Very Low","Items Low","Items Medium","Items High","Items Very High"}; return names[std::clamp(n,0,5)]; }
std::string ping(const MeleeNetplayUi& ui) { return ui.ping_ms>=0?number(ui.ping_ms)+" ms":"--"; }
std::string public_status(const char* status,const char* fallback) {
  std::string text=status?status:"";
  if(text.empty()||text.find("://")!=std::string::npos) return fallback;
  // Connection addresses are configuration details, never menu copy.
  for(size_t start=0;start<text.size();start++) {
    if(text[start]<'0'||text[start]>'9') continue;
    size_t at=start; bool address=true;
    for(int part=0;part<4;part++) {
      unsigned value=0,digits=0;
      while(at<text.size()&&text[at]>='0'&&text[at]<='9'&&digits<4) { value=value*10+text[at++]-'0'; digits++; }
      if(!digits||digits>3||value>255) { address=false; break; }
      if(part<3&&(at>=text.size()||text[at++]!='.')) { address=false; break; }
    }
    if(address) return fallback;
  }
  return text;
}
std::string download_size(int64_t bytes) {
  char text[40];
  if(bytes>=1000000) std::snprintf(text,sizeof text,"%.1f MB",bytes/1000000.0);
  else if(bytes>=1000) std::snprintf(text,sizeof text,"%.0f KB",bytes/1000.0);
  else std::snprintf(text,sizeof text,"%lld bytes",(long long)std::max<int64_t>(0,bytes));
  return text;
}

void background(Canvas& c,const std::string& title) {
  // The original game renders the animated backdrop, frame, and footer.
  // Only its old header words are hidden; these labels use the same disc face.
  c.text(110,58,30,IM_COL32(130,134,147,255),"Online",136,false,false,true);
  c.text(284,58,30,IM_COL32(130,134,147,255),title,402,false,false,true);
}
void footer(Canvas& c,const std::string& message) {
  c.text(400,504,20,white,message,430,true);
}
void signal(Canvas& c,float x,float y,ImU32 color) { for(int i=0;i<4;i++) c.box(x+i*5,y-i*4,3,5+i*4,color); }
struct Prompt { const char* key; const char* action; };
void prompt_strip(Canvas& c,std::initializer_list<Prompt> prompts) {
  // Both screens share one inset footer: every 32px glyph has six pixels of
  // vertical padding, and the whole strip clears the native frame.
  c.box(84,430,632,44,IM_COL32(4,10,22,225),6);
  c.border(84,430,632,44,IM_COL32(131,149,165,220),1,6);
  float width=608.f/prompts.size(); int index=0;
  for(const auto& prompt:prompts) {
    float x=96+index*width;
    if(index) c.line(x,441,x,463,IM_COL32(131,149,165,120));
    c.key(x,436,prompt.key,prompt.action,width); index++;
  }
}

void browser(Canvas& c,const MeleeNetplayUi& ui,int selected) {
  background(c,"Rooms");
  int count=std::clamp(ui.room_count,0,NETPLAY_MAX_ROOMS); selected=std::clamp(selected,0,std::max(0,count-1));
  int first=std::clamp(selected-2,0,std::max(0,count-6));
  c.text(74,112,16,muted,number(count)+(count==1?" room":" rooms"),650);
  c.box(70,142,480,232,IM_COL32(8,14,22,214)); c.border(70,142,480,232,IM_COL32(170,151,40,220),2);
  c.box(71,143,478,30,IM_COL32(218,192,24,240));
  c.text(80,146,20,ink,"Room"); c.text(280,146,17,ink,"Players"); c.text(362,146,18,ink,"Rules"); c.text(497,146,18,ink,"Ping");
  for(int row=0;row<6;row++) {
    int index=first+row; if(index>=count) break; const auto& r=ui.rooms[index]; float y=176+row*32.f; bool focus=index==selected;
    if(focus) c.polygon({{76,y},{548,y},{548,y+29},{70,y+29},{67,y+16}},gold,IM_COL32(255,241,87,255),2);
    else { c.box(72,y,476,29,IM_COL32(9,22,27,190)); c.line(72,y+30,548,y+30,IM_COL32(170,150,43,140)); }
    ImU32 color=focus?ink:r.state||r.players>=r.max?muted:white;
    c.text(80,y+4,19,color,r.name,198); c.text(307,y+4,19,color,number(r.players)+" / "+number(r.max),67,true);
    std::string rules="--";
    if(r.rules_known) rules=r.rules.mode?number(r.rules.stock)+" stock": "Time";
    c.text(357,y,15,color,rules,128);
    if(r.rules_known) c.text(357,y+15,13,color,r.rules.minutes?number(r.rules.minutes)+" min":"No time limit",128);
    c.text(520,y+5,16,color,ping(ui),54,true);
  }
  if(!count) { c.text(302,261,26,white,"No rooms open",455,true,true); c.text(302,300,19,muted,"Press X to create a room.",455,true); }
  if(count>6) c.text(74,383,14,muted,number(first+1)+"-"+number(std::min(first+6,count))+" of "+number(count),150,false);
  c.box(568,168,160,222,IM_COL32(10,34,49,225),8); c.border(568,168,160,222,IM_COL32(89,159,175,220),2,8);
  if(count) {
    const auto& r=ui.rooms[selected]; c.text(579,182,20,white,r.name,137); c.line(579,218,717,218,IM_COL32(142,185,193,220));
    c.text(579,231,18,white,r.rules_known?item_name(r.rules.items):"Rules on join",138);
    signal(c,583,280,IM_COL32(97,220,191,255)); c.text(608,266,18,white,"Rollback",109);
    c.text(579,302,20,r.state||r.players>=r.max?muted:gold,r.state?"In Match":r.players>=r.max?"Room Full":"Open",145);
    c.text(579,337,14,muted,std::string("Host: ")+r.host,138);
  } else { c.text(648,236,18,muted,"No rooms yet",138,true); c.text(648,268,17,muted,"Create one",138,true); }
  c.text(579,366,12,muted,"Ping to relay server",140);
  prompt_strip(c,{{"A","Join"},{"X","Create"},{"Y","Refresh"},{"B","Back"}});
  footer(c,public_status(ui.status,"Choose a room to join."));
}

wgpu::TextureView avatar_image(const std::string&,const std::vector<unsigned char>&);
void gamepad(Canvas& c,float x,float y,float w) {
  c.art(netplay_art::controller,x,y,w,w*netplay_art::controller.height/netplay_art::controller.width);
}
void player_card(Canvas& c,const MeleeNetplayUi& ui,int index,float x,float y,float w) {
  int count=std::clamp(ui.player_count,0,NETPLAY_MAX_PLAYERS); bool occupied=index<count; const auto& p=ui.players[index];
  const ImU32 colors[]={IM_COL32(237,65,81,255),IM_COL32(62,122,254,255),IM_COL32(232,188,45,255),IM_COL32(71,186,105,255)}; ImU32 tint=colors[index];
  c.polygon({{x+18,y},{x+w-12,y},{x+w,y+13},{x+w,y+155},{x+w-12,y+166},{x+12,y+166},{x,y+153},{x,y+20}},IM_COL32(9,15,30,235),tint,2);
  c.polygon({{x+18,y},{x+79,y},{x+61,y+32},{x,y+32},{x,y+20}},tint);
  c.text(x+35,y-1,30,ink,"P"+number(index+1),0,true);
  c.text(x+76,y+3,w<170?16:19,white,occupied?p.name:"Open Slot",w-82);
  c.text(x+w/2,y+30,12,muted,occupied?(std::string(index==0?"HOST":"GUEST")+(p.id==ui.local_id?"  /  YOU":"")):"Waiting for a player",w-12,true);
  if(occupied) {
    std::vector<unsigned char> pixels;auto picture=profile_avatar(p.avatar,pixels)?avatar_image(p.avatar,pixels):wgpu::TextureView{};
    if(picture)c.d->AddImage((ImTextureID)picture.Get(),c.p(x+(w-76)/2,y+43),c.p(x+(w+76)/2,y+119));
    else gamepad(c,x+(w-104)/2,y+40,104);
  }
  else { c.text(x+w/2,y+65,40,IM_COL32(78,90,110,255),"?",0,true,true); }
  c.polygon({{x+30,y+123},{x+w-9,y+123},{x+w-22,y+145},{x+17,y+145}},IM_COL32(4,8,18,250),tint,2);
  c.text(x+w/2,y+123,23,occupied&&p.ready?gold:muted,occupied?(p.ready?"Ready":"Not Ready"):"Open",w-32,true,true);
  signal(c,x+16,y+158,occupied?IM_COL32(56,216,109,255):muted);
  c.text(x+42,y+149,13,white,occupied?ping(ui):"--",w-55);
  c.text(x+w-78,y+149,13,muted,"Rollback",71,false);
}
void rules_editor(Canvas& c,const MeleeNetplayUi& ui,int selected) {
  c.box(206,117,388,344,IM_COL32(5,12,26,246),8); c.border(206,117,388,344,IM_COL32(183,161,59,255),2,8);
  c.text(400,128,30,gold,"Change Rules",350,true,true);
  const char* labels[]={"Mode","Stock","Time","Items","Input Delay","Return"};
  const std::string values[]={ui.rules.mode?"Stock":"Time",number(ui.rules.stock),ui.rules.minutes?number(ui.rules.minutes)+" min":"No limit",item_name(ui.rules.items),ui.rules.delay?number(ui.rules.delay)+" frames":std::string("Auto"),""};
  for(int i=0;i<6;i++) { float y=169+i*45.f; bool focus=i==selected; if(focus) c.box(220,y-2,360,39,gold,3); c.text(233,y+3,22,focus?ink:white,labels[i],170); if(i<5)c.text(484,y+3,21,focus?ink:gold,values[i],170,true); }
  footer(c,"Left / Right: change. B: back.");
}
void lobby(Canvas& c,const MeleeNetplayUi& ui,int selected) {
  background(c,ui.room_name[0]?ui.room_name:"Online Room");
  if(selected>=100) { rules_editor(c,ui,selected-100); return; }
  int slots=ui.player_count>2?4:2; float width=slots==2?206.f:156.f; float gap=slots==2?30.f:12.f; float start=(800-slots*width-(slots-1)*gap)/2;
  for(int i=0;i<slots;i++) player_card(c,ui,i,start+i*(width+gap),112,width);
  c.polygon({{114,288},{686,288},{695,296},{695,314},{105,314},{105,296}},IM_COL32(4,10,18,230),IM_COL32(166,184,186,255),2);
  const std::string bands[]={ui.rules.mode?number(ui.rules.stock)+" STOCK":"TIME",ui.rules.minutes?number(ui.rules.minutes)+" MIN":"NO LIMIT",item_name(ui.rules.items),ui.rules.delay?number(ui.rules.delay)+"F DELAY":std::string("AUTO DELAY")};
  for(int i=0;i<4;i++) { c.text(180+i*146.f,291,19,white,bands[i],137,true,true); if(i<3)c.line(254+i*146.f,292,241+i*146.f,310,muted,2); }
  c.button(237,326,330,28,ui.is_host?"Start Match":"Waiting for Host",selected==0,ui.is_host&&ui.all_ready&&ui.player_count>=2);
  c.button(245,360,314,26,"Change Rules",selected==1,ui.is_host);
  c.button(245,393,314,26,"Leave Room",selected==2);
  bool ready=false; for(int i=0;i<std::clamp(ui.player_count,0,NETPLAY_MAX_PLAYERS);i++) if(ui.players[i].id==ui.local_id) ready=ui.players[i].ready!=0;
  prompt_strip(c,{{"A","Confirm"},{"X",ready?"Unready":"Ready"},{"B","Back"}});
  footer(c,ui.player_count<2?"Waiting for another player to join.":ui.all_ready?(ui.is_host?"Everyone is ready. Press A to start.":"Ready. Waiting for the host."):"Press X when you are ready.");
}
const char* costume_base_name(const char* id) {
  static constexpr struct { const char* id; const char* name; } names[] = {
    {"stock-Ca","Captain Falcon"},{"stock-Dk","Donkey Kong"},{"stock-Fx","Fox"},
    {"stock-Gw","Mr. Game & Watch"},{"stock-Kb","Kirby"},{"stock-Kp","Bowser"},
    {"stock-Lk","Link"},{"stock-Lg","Luigi"},{"stock-Mr","Mario"},{"stock-Ms","Marth"},
    {"stock-Mt","Mewtwo"},{"stock-Ns","Ness"},{"stock-Pe","Peach"},{"stock-Pk","Pikachu"},
    {"stock-Pp","Ice Climbers"},{"stock-Pr","Jigglypuff"},{"stock-Ss","Samus"},
    {"stock-Ys","Yoshi"},{"stock-Zd","Zelda"},{"stock-Sk","Sheik"},{"stock-Fc","Falco"},
    {"stock-Cl","Young Link"},{"stock-Dr","Dr. Mario"},{"stock-Fe","Roy"},
    {"stock-Pc","Pichu"},{"stock-Gn","Ganondorf"}
  };
  for(const auto& entry:names)if(!std::strcmp(id,entry.id))return entry.name;
  return nullptr;
}
void mod_browser(Canvas& c,const MeleeNetplayUi& source,int selected,bool manager=false) {
  MeleeNetplayUi ui=source;
  if(manager){
    int filtered=0,focus=0;
    for(int i=0;i<std::clamp(source.mod_catalog_count,0,NETPLAY_MOD_CATALOG);++i)if(source.mod_catalog[i].installed){
      if(i==selected)focus=filtered; ui.mod_catalog[filtered++]=source.mod_catalog[i];
    }ui.mod_catalog_count=filtered;selected=focus;
  }
  background(c,manager?"Mod Manager":"Mod Browser");
  int count=std::clamp(ui.mod_catalog_count,0,NETPLAY_MOD_CATALOG);
  selected=std::clamp(selected,0,std::max(0,count-1));
  const auto& mod=ui.mod_catalog[selected];
  int first=std::clamp(selected-2,0,std::max(0,count-6));
  c.text(318,113,16,muted,ui.mod_catalog_loading?"Loading mods...":number(count)+(count==1?" mod":" mods"),400);
  c.border(78,141,222,230,IM_COL32(109,196,207,210),2);
  bool content=count&&!std::strcmp(mod.id,"akaneia");
  c.text(189,116,20,white,content?"Content Mod":"Skin Preview",216,true,true);
  auto preview = count ? preview_view(ui, mod.sha256) : wgpu::TextureView{};
  if (preview) {
    float scale = std::min(214.f/preview_texture.width, 222.f/preview_texture.height);
    float width = preview_texture.width*scale, height = preview_texture.height*scale;
    c.d->AddImage((ImTextureID)preview.Get(),c.p(189-width/2,256-height/2),c.p(189+width/2,256+height/2));
  } else if(content){
    c.text(189,190,23,gold,"Akaneia",202,true);
    c.text(189,239,18,white,"7 fighters",202,true);
    c.text(189,270,18,white,"17 stages",202,true);
    c.text(189,301,18,white,"41 music tracks",202,true);
    c.text(189,338,13,muted,"Official GitHub release",202,true);
  } else c.text(189,232,18,white,!count?"No costume selected":
      ui.mod_inspect_loading&&!std::strcmp(mod.sha256,ui.mod_inspect_package)?"Loading preview...":"Preview unavailable",202,true);
  if(count)c.paragraph(86,380,16,white,mod.name,206,2,18);
  c.box(314,142,414,239,IM_COL32(8,14,22,214));c.border(314,142,414,239,IM_COL32(170,151,40,220),2);
  c.box(315,143,412,29,IM_COL32(218,192,24,240));
  c.text(326,146,20,ink,"Mod");c.text(648,149,15,ink,"Version");
  for(int row=0;row<6;row++) {
    int index=first+row;if(index>=count)break;const auto& entry=ui.mod_catalog[index];
    float y=175+row*34.f;bool focus=index==selected;
    if(focus)c.polygon({{320,y},{726,y},{726,y+31},{314,y+31},{310,y+17}},gold,IM_COL32(255,241,87,255),2);
    else c.line(316,y+32,726,y+32,IM_COL32(170,150,43,120));
    ImU32 color=focus?ink:white;
    c.text(326,y+1,17,color,entry.name,304);
    c.text(326,y+20,12,focus?ink:muted,entry.installed?(entry.enabled?"Installed / Enabled":"Installed / Disabled"):"Available",304);
    c.text(677,y+7,14,color,entry.version,87,true);
  }
  if(!count) {
    c.text(521,235,22,white,ui.mod_catalog_loading?"Loading...":"No mods available",388,true);
    c.text(521,276,17,muted,"Press Y to refresh.",388,true);
  }
  const char* action="Install";
  if(count) {
    const char* base=costume_base_name(mod.base);
    c.text(318,387,14,muted,(base?std::string("For ")+base+"   ":"")+download_size(mod.bytes),398);
    if(count>6)c.text(690,408,12,muted,number(first+1)+"-"+number(std::min(first+6,count))+" / "+number(count),80,true);
    if(mod.installed)action=manager?(mod.enabled?"Disable":"Enable"):"Manage";
  }
  prompt_strip(c,{{"A",action},{"X",manager?"Browse":"Manage"},{"Y","Refresh"},{"B","Back"}});
  footer(c,public_status(ui.mod_status,"Choose a costume to install or enable."));
}

void room_name_dialog(Canvas& c,const MeleeNetplayUi& ui) {
  c.box(67,110,666,366,IM_COL32(3,8,18,170));
  c.box(106,145,588,258,IM_COL32(7,18,34,248),6); c.border(106,145,588,258,gold,2,6);
  c.text(400,167,28,gold,"Name Your Room",540,true,true);
  c.text(400,213,17,white,"Type a name with your keyboard.",520,true);
  c.box(133,248,534,59,IM_COL32(2,7,16,255),4); c.border(133,248,534,59,gold,1,4);
  std::string draft=ui.room_name_draft;
  std::string visible = draft;
  bool scrolled = false;
  while (!visible.empty() && c.text_width(22,(scrolled?"...":"")+visible) > 472) {
    visible.erase(0,1); scrolled = true;
  }
  if (scrolled) visible = "..." + visible;
  c.text(151,265,22,draft.empty()?muted:white,draft.empty()?"YAMPP room":visible,490);
  if (std::fmod(ImGui::GetTime(),1.0) < .65) {
    float caret = draft.empty()?151.f:std::min(644.f,154.f+c.text_width(22,visible));
    c.line(caret,264,caret,289,gold,2);
  }
  c.text(647,320,14,muted,number((int)draft.size())+" / 47",90,true);
  c.text(400,352,16,muted,"Enter: Create    Esc: Cancel    Backspace: Delete",526,true);
  prompt_strip(c,{{"A","Create"},{"B","Cancel"}});
  footer(c,"Choose a name for your room.");
}

void mod_confirmation(Canvas& c,const MeleeNetplayUi& ui,int first) {
  int count=std::clamp(ui.mod_required_count,0,NETPLAY_MAX_MODS);
  first=std::clamp(first,0,std::max(0,count-5));
  bool downloading=ui.mod_job==2; int64_t bytes=0;
  if(ui.mod_prompt==4){
    c.box(106,155,588,250,IM_COL32(7,18,34,248),6);c.border(106,155,588,250,gold,2,6);
    c.text(400,183,26,gold,"Original Melee Room",540,true,true);
    c.paragraph(137,242,18,white,"Disable Akaneia for this room? YAMPP will reload the menu and join automatically.",526,3,26);
    c.text(400,354,16,muted,"Your installed mod stays available.",526,true);
    prompt_strip(c,{{"A","Disable & Join"},{"B","Cancel"}});footer(c,ui.mod_status);return;
  }
  if(ui.mod_prompt==3) {
    const bool joining=ui.mod_pending_room>0;
    c.box(67,110,666,366,IM_COL32(3,8,18,150));
    c.box(106,125,588,290,IM_COL32(7,18,34,248),6);c.border(106,125,588,290,gold,2,6);
    c.text(400,140,27,gold,downloading?"Preparing Akaneia":"Akaneia",548,true,true);
    c.text(400,187,18,white,"Add Akaneia fighters, stages and music.",536,true);
    c.text(400,222,17,gold,"Official GitHub: akaneia/akaneia-build",536,true);
    c.text(400,254,16,white,"Version "+std::string(ui.mod_required[0].version)+"  |  "+download_size(ui.mod_required[0].bytes),536,true);
    if(downloading) {
      /* Preparing Akaneia patches a disc image, extracts it, imports the
       * content and verifies the result: minutes of work behind one screen.
       * Without a position the player cannot tell it apart from a hang, and
       * the stage name is what makes a long pause legible rather than
       * alarming. Both come from the importer itself. */
      const int percent=std::clamp(ui.mod_progress,0,100);
      c.paragraph(130,290,16,white,public_status(ui.mod_status,"Preparing verified Akaneia content..."),535,2,22);
      c.box(130,346,540,6,IM_COL32(37,51,70,255),3);
      c.box(130,346,540*percent/100.f,6,gold,3);
      c.text(400,362,15,gold,number(percent)+"%",536,true);
      c.text(400,385,14,muted,"This takes several minutes. Leaving now cancels it.",536,true);
    } else {
      c.paragraph(130,293,16,muted,joining?"Download from official GitHub and join? YAMPP will prepare the content and continue.":"Place Akaneia.Builder.1.0.1.7z in user/imports, then install. YAMPP does not bundle Akaneia.",535,3,22);
      c.text(400,379,15,muted,"Your original Melee files stay separate.",536,true);
    }
    prompt_strip(c,downloading?std::initializer_list<Prompt>{{"B","Cancel"}}:
                                   std::initializer_list<Prompt>{{"A",joining?"Download":"Install"},{"B","Cancel"}});
    footer(c,downloading?public_status(ui.mod_status,"Preparing verified Akaneia content..."):joining?"Download Akaneia from official GitHub?":"Install the official archive from user/imports.");
    return;
  }
  for(int i=0;i<count;i++) bytes+=std::max(0,ui.mod_required[i].bytes);
  c.box(67,110,666,366,IM_COL32(3,8,18,150));
  c.box(106,125,588,290,IM_COL32(7,18,34,248),6); c.border(106,125,588,290,gold,2,6);
  c.text(400,137,27,gold,downloading?"Installing Mods":"Download Mods",548,true,true);
  c.text(400,172,17,white,ui.mod_prompt==1?"This room needs the following mods.":"Install the following mod?",548,true);
  for(int row=0;row<5;row++) {
    int index=first+row; if(index>=count) break; const auto& mod=ui.mod_required[index]; float y=202+row*34.f;
    if(row) c.line(122,y-4,678,y-4,IM_COL32(107,138,153,100));
    c.paragraph(124,y,15,white,mod.name,365,2,16);
    c.paragraph(513,y+1,13,muted,mod.version,160,2,15);
  }
  c.text(124,385,16,gold,"Total: "+download_size(bytes),310);
  if(count>5) c.text(596,385,14,muted,number(first+1)+"-"+number(std::min(first+5,count))+" of "+number(count),160,true);
  if(downloading) {
    c.box(124,405,552,3,IM_COL32(37,51,70,255));
    c.box(124,405,552*std::clamp(ui.mod_progress,0,100)/100.f,3,gold);
  }
  prompt_strip(c,downloading?std::initializer_list<Prompt>{{"B","Cancel"}}:
                                std::initializer_list<Prompt>{{"A","Download"},{"B","Cancel"}});
  footer(c,downloading?public_status(ui.mod_status,"Installing selected mods..."):
           ui.mod_prompt==1?"Download mods to join this room.":"Download and install this mod.");
}

#include "controller_panel.inc"
#include "profile_panel.inc"
}

extern "C" void aushim_netplay_fonts_ready() {
  // Netplay text uses original disc bitmap atlases. F1 settings keep ImGui's
  // separate default face; no Windows font files are loaded by this renderer.
  auto& io=ImGui::GetIO();
  if(io.Fonts->Fonts.empty()) io.FontDefault=io.Fonts->AddFontDefault();
}
extern "C" AUSHIM_API void aushim_netplay_screen(const MeleeNetplayUi* state,int kind,int selection) {
  std::lock_guard<std::mutex> guard(state_lock);
  if(!state || state->size!=sizeof(MeleeNetplayUi) || state->version!=NETPLAY_UI_VERSION || (kind!=10&&kind!=11&&kind!=13&&kind!=21&&kind!=22&&kind!=24&&kind!=25)) {
    if(current.room_name_open&&std::getenv("MELEE_TEST_UI_TRACE"))std::fprintf(stderr,"[room-name] open=0 text=%s\n",current.room_name_draft);
    current.room_name_open=0;screen_kind=0;aushim_controllers_open(0);aushim_profile_open(0);return;
  }
  if(std::getenv("MELEE_TEST_UI_TRACE") && (screen_kind!=kind || screen_selection!=selection || current.room_count!=state->room_count || current.room_id!=state->room_id || current.player_count!=state->player_count || current.all_ready!=state->all_ready || std::memcmp(&current.rules,&state->rules,sizeof state->rules)))
    std::fprintf(stderr,"[netplay-ui] kind=%d selected=%d rooms=%d players=%d ready=%d host=%d room=%d mode=%d stock=%d minutes=%d items=%d delay=%d\n",kind,selection,state->room_count,state->player_count,state->all_ready,state->is_host,state->room_id,state->rules.mode,state->rules.stock,state->rules.minutes,state->rules.items,state->rules.delay);
  if(std::getenv("MELEE_TEST_MENU_POSE") && state->menu_pose_valid &&
     (screen_kind!=kind || std::memcmp(current.menu_pose,state->menu_pose,sizeof state->menu_pose)))
    std::fprintf(stderr,"[menu-pose] kind=%d H=%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g\n",kind,
      state->menu_pose[0],state->menu_pose[1],state->menu_pose[2],state->menu_pose[3],state->menu_pose[4],
      state->menu_pose[5],state->menu_pose[6],state->menu_pose[7],state->menu_pose[8]);
  if(std::getenv("MELEE_TEST_UI_TRACE") && (current.room_name_open!=state->room_name_open || std::strcmp(current.room_name_draft,state->room_name_draft)))
    std::fprintf(stderr,"[room-name] open=%d text=%s\n",state->room_name_open,state->room_name_draft);
  current=*state; screen_kind=kind; screen_selection=selection; aushim_controllers_open(kind==21); aushim_profile_open(kind==24);
}
// Retained for older runtime builds; the new layout is rendered from state.
extern "C" AUSHIM_API void aushim_netplay_table(const void*,int,int) {}
extern "C" AUSHIM_API void aushim_netplay_table_draw() {
  MeleeNetplayUi state; int kind,selected;
  { std::lock_guard<std::mutex> guard(state_lock); state=current; kind=screen_kind; selected=screen_selection; }
  if(!kind)return; Canvas canvas;
  {
    if(kind==24) { profile_panel(canvas); }
    else if(kind==21) { controller_panel(canvas); } else if(state.room_name_open) {
      background(canvas,"Create Room"); room_name_dialog(canvas,state);
    } else if(state.mod_prompt) {
      background(canvas,kind==25?"Mod Manager":kind==22?"Mod Browser":kind==13?"Rooms":state.room_name);
      mod_confirmation(canvas,state,selected>=1000?selected-1000:0);
    } else if(kind==13) browser(canvas,state,selected);
    else if(kind==11) lobby(canvas,state,selected);
    else if(kind==22 || kind==25) mod_browser(canvas,state,selected,kind==25);
    canvas.follow_menu(state.menu_pose_valid ? state.menu_pose : nullptr);
  }
}

extern "C" void aushim_content_caption(void){
 Canvas c;c.d=ImGui::GetForegroundDrawList();
 c.box(238,268,324,58,IM_COL32(4,14,22,242),4);
 c.border(238,268,324,58,IM_COL32(109,196,207,230),2,4);
 c.text(400,283,24,gold,"Applying mods...",296,true);
}
