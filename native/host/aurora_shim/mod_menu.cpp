#include "aurora_shim.h"
#include <imgui.h>
#include <dolphin/pad.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include "webgpu/gpu.hpp"
extern "C" int aushim_settings_captures_input();
struct Icon {wgpu::Texture texture;wgpu::TextureView view;};
static std::unordered_map<std::string,Icon> icons;
static ImTextureID icon_texture(const std::string& path){
 auto it=icons.find(path);if(it!=icons.end())return (ImTextureID)it->second.view.Get();
 Icon icon;FILE* f=fopen(path.c_str(),"rb");if(!f)return 0;unsigned dimensions[2];if(fread(dimensions,4,2,f)!=2||!dimensions[0]||!dimensions[1]||dimensions[0]>1024||dimensions[1]>1024){fclose(f);return 0;}
 std::vector<unsigned char> bytes(dimensions[0]*dimensions[1]*4);bool ok=fread(bytes.data(),1,bytes.size(),f)==bytes.size();fclose(f);if(!ok)return 0;
 wgpu::TextureDescriptor desc{};desc.dimension=wgpu::TextureDimension::e2D;desc.size={dimensions[0],dimensions[1],1};desc.format=wgpu::TextureFormat::RGBA8Unorm;desc.mipLevelCount=1;desc.sampleCount=1;desc.usage=wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::CopyDst;
 icon.texture=aurora::webgpu::g_device.CreateTexture(&desc);icon.view=icon.texture.CreateView();
 wgpu::TexelCopyTextureInfo dst{};dst.texture=icon.texture;wgpu::TexelCopyBufferLayout layout{};layout.bytesPerRow=dimensions[0]*4;layout.rowsPerImage=dimensions[1];
 aurora::webgpu::g_queue.WriteTexture(&dst,bytes.data(),bytes.size(),&layout,&desc.size);
 auto id=(ImTextureID)icon.view.Get();icons.emplace(path,std::move(icon));return id;
}

static std::vector<std::string> split(const char* s){std::vector<std::string> out;const char* p=s;while(*p){const char* n=strchr(p,'\n');if(!n)break;out.emplace_back(p,n);p=n+1;}return out;}
extern "C" AUSHIM_API void aushim_mod_menu(int scene,const char* fighters,const char* stages,const int* state,int* picks,int* action){
 static int previousScene=0,selected[4]={8,0,0,0},costumes[4]={},types[4]={0,1,3,3},hover[4]={8,0,0,0},editPort=0;
 static unsigned prevButtons[4]={};static double repeat[4]={};
 if(!scene){previousScene=0;return;}
 auto names=split(scene==1?fighters:stages);if(names.empty())return;
 std::vector<std::string> paths(names.size());std::vector<int> colorCounts(names.size(),1);
 if(scene==1)for(size_t i=0;i<names.size();i++){auto tab=names[i].find('\t');if(tab==std::string::npos)continue;auto end=names[i].find('\t',tab+1);paths[i]=names[i].substr(tab+1,end-tab-1);if(end!=std::string::npos)colorCounts[i]=std::max(1,atoi(names[i].c_str()+end+1));names[i].resize(tab);}

 ImGuiIO& io=ImGui::GetIO();const float w=io.DisplaySize.x,h=io.DisplaySize.y;
 bool entered=scene!=previousScene;previousScene=scene;
 if(scene==1)for(int p=0;p<4;p++){selected[p]=state[p]&255;costumes[p]=(state[p]>>8)&15;types[p]=(state[p]>>12)&3;if(entered)hover[p]=selected[p];}
 ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(w,h));
 ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.055f,.075f,.10f,1));
 ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.15f,.20f,.26f,1));ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.27f,.37f,.27f,1));
 ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(20,18));ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8,8));
 ImGui::Begin("Melee Workshop Roster",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
 ImGui::SetWindowFontScale(std::max(.85f,w/900.f));
 ImGui::TextColored(ImVec4(.68f,.91f,.43f,1),scene==1?"MELEE PC  /  CHARACTER SELECT":"MELEE PC  /  STAGE SELECT");
 ImGui::TextDisabled(scene==1?"Stock fighters + installed character packages":"Choose an imported stage or return to the original stage screen");ImGui::Spacing();
 const int columns=std::max(4,(int)std::ceil(std::sqrt(names.size()*1.9)));const float cellW=(w-40-(columns-1)*8)/columns;
 const int rows=(names.size()+columns-1)/columns;const float cellH=std::clamp((h-230.f)/std::max(1,rows),35.f,78.f);
 const float gridHeight=scene==1?h-286.f:h-142.f;
 bool capturing=aushim_settings_captures_input()!=0;
 ImGui::BeginDisabled(capturing);
 PADStatus pads[4]{};if(!capturing)PADRead(pads);else for(auto& pad:pads)pad.err=-1;double now=ImGui::GetTime();
 for(int p=0;p<4;p++){
  unsigned buttons=pads[p].button,edge=buttons&~prevButtons[p];prevButtons[p]=buttons;
  if(scene==1&&p>0&&pads[p].err==0&&types[p]==3){types[p]=0;picks[p]=selected[p]|(costumes[p]<<8);}
  int dx=(pads[p].stickX>40||(edge&PAD_BUTTON_RIGHT))?1:(pads[p].stickX<-40||(edge&PAD_BUTTON_LEFT))?-1:0;
  int dy=(pads[p].stickY>40||(edge&PAD_BUTTON_UP))?-1:(pads[p].stickY<-40||(edge&PAD_BUTTON_DOWN))?1:0;
  if((dx||dy)&&now>=repeat[p]){hover[p]=(hover[p]+dx+dy*columns+(int)names.size()*columns)%(int)names.size();repeat[p]=now+.18;editPort=p;}
  if(!dx&&!dy)repeat[p]=0;
  if(edge&PAD_BUTTON_A){int index=std::clamp(hover[p],0,(int)names.size()-1);selected[p]=index;picks[scene==1?p:0]=index|(costumes[p]<<8)|(types[p]<<12);editPort=p;}
  if(scene==1&&(edge&PAD_BUTTON_START))*action=1;
  if(edge&PAD_BUTTON_B)*action=2;
 }
 if(scene==1){ImGui::Text("Editing Player %d",editPort+1);ImGui::SameLine();for(int p=0;p<4;p++){if(p)ImGui::SameLine();ImGui::PushID(900+p);if(ImGui::SmallButton(("P"+std::to_string(p+1)).c_str()))editPort=p;ImGui::PopID();}}
 ImGui::BeginChild("Roster",ImVec2(0,gridHeight),false);
 for(int i=0;i<(int)names.size();i++){
  if(i%columns)ImGui::SameLine();ImGui::PushID(i);
  bool chosen=scene==1?selected[editPort]==i:hover[editPort]==i;
  if(chosen)ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.29f,.40f,.20f,1));
  ImVec2 position=ImGui::GetCursorScreenPos();
  if(ImGui::Button(scene==1?"##fighter":names[i].c_str(),ImVec2(cellW,cellH))){selected[editPort]=hover[editPort]=i;picks[scene==1?editPort:0]=i|(costumes[editPort]<<8)|(types[editPort]<<12);}
  if(scene==1){auto tex=icon_texture(paths[i]);auto* draw=ImGui::GetWindowDrawList();float iconH=std::max(16.f,cellH-23.f),iconW=iconH*64.f/56.f;if(tex)draw->AddImage(tex,ImVec2(position.x+(cellW-iconW)/2,position.y+2),ImVec2(position.x+(cellW+iconW)/2,position.y+2+iconH));
   ImVec2 text=ImGui::CalcTextSize(names[i].c_str());draw->AddText(ImVec2(position.x+std::max(2.f,(cellW-text.x)/2),position.y+cellH-19.f),IM_COL32(240,245,250,255),names[i].c_str());}
  if(chosen)ImGui::PopStyleColor();ImGui::PopID();
 }
 ImGui::EndChild();
 if(scene==1){
  for(int p=0;p<4;p++){if(p)ImGui::SameLine();ImGui::PushID(1000+p);ImGui::BeginGroup();ImGui::TextColored(p==editPort?ImVec4(.68f,.91f,.43f,1):ImVec4(.7f,.75f,.8f,1),"P%d  %s",p+1,types[p]==3?"CLOSED":types[p]==1?"CPU":"HUMAN");
   ImGui::TextUnformatted(names[std::clamp(selected[p],0,(int)names.size()-1)].c_str());
   if(ImGui::SmallButton("Type")){types[p]=types[p]==0?1:types[p]==1?3:0;picks[p]=selected[p]|(costumes[p]<<8)|(types[p]<<12);}
   ImGui::SameLine();if(ImGui::SmallButton("Color")){costumes[p]=(costumes[p]+1)%colorCounts[std::clamp(selected[p],0,(int)names.size()-1)];picks[p]=selected[p]|(costumes[p]<<8)|(types[p]<<12);}ImGui::TextDisabled("Color %d",costumes[p]+1);ImGui::Dummy(ImVec2((w-64)/4,0));ImGui::EndGroup();ImGui::PopID();
  }
  if(ImGui::Button("READY TO FIGHT  -  START",ImVec2(w-40,30)))*action=1;
 }else if(ImGui::Button("Back to characters",ImVec2(w-40,28)))*action=2;
 ImGui::EndDisabled();ImGui::End();ImGui::PopStyleVar(2);ImGui::PopStyleColor(3);
}

extern "C" AUSHIM_API void aushim_mod_release_resources(){icons.clear();}
