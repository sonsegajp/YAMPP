// Shares the Online frame, native bitmap fonts and menu-camera transform.
struct AvatarTexture { std::string key; wgpu::Texture texture; wgpu::TextureView view; };
std::vector<AvatarTexture> avatar_textures;
wgpu::TextureView avatar_image(const std::string& key,const std::vector<unsigned char>& pixels) {
 for(const auto& a:avatar_textures)if(a.key==key)return a.view;
 if(pixels.size()!=YAMPP_AVATAR_BYTES)return {};
 wgpu::TextureDescriptor desc{};desc.dimension=wgpu::TextureDimension::e2D;
 desc.size={YAMPP_AVATAR_SIDE,YAMPP_AVATAR_SIDE,1};desc.format=wgpu::TextureFormat::RGBA8Unorm;
 desc.mipLevelCount=desc.sampleCount=1;desc.usage=wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::CopyDst;
 AvatarTexture a{key,aurora::webgpu::g_device.CreateTexture(&desc),{}};a.view=a.texture.CreateView();
 wgpu::TexelCopyTextureInfo dst{};dst.texture=a.texture;wgpu::TexelCopyBufferLayout layout{};
 layout.bytesPerRow=YAMPP_AVATAR_SIDE*4;layout.rowsPerImage=YAMPP_AVATAR_SIDE;
 wgpu::Extent3D size{YAMPP_AVATAR_SIDE,YAMPP_AVATAR_SIDE,1};
 aurora::webgpu::g_queue.WriteTexture(&dst,pixels.data(),pixels.size(),&layout,&size);
 if(avatar_textures.size()>=12)avatar_textures.erase(avatar_textures.begin());
 avatar_textures.push_back(std::move(a));return avatar_textures.back().view;
}
void profile_panel(Canvas& c) {
 const auto& v=profile_menu_view();background(c,"Profile");
 c.box(96,134,232,284,IM_COL32(5,15,29,225),5);c.border(96,134,232,284,IM_COL32(134,206,220,220),1,5);
 if(v.pixels.empty())c.art(netplay_art::controller,117,168,190,151);
 else {auto image=avatar_image("draft-"+number(v.revision),v.pixels);if(image)c.d->AddImage((ImTextureID)image.Get(),c.p(135,159),c.p(289,313));}
 c.text(212,343,23,white,v.name.empty()?"Player":v.name,210,true);
 c.text(212,387,14,muted,"Your Online picture",208,true);
 const char* rows[]={"Username","Choose picture","Use controller","Save profile"};
 for(int row=0;row<4;row++){
  float y=142+row*66.f;bool active=row==v.row;
  c.box(351,y,349,55,active?gold:IM_COL32(6,17,31,230),3);
  c.border(351,y,349,55,active?gold:IM_COL32(96,137,151,220),1,3);
  c.text(367,y+7,row==0?16:23,active?ink:white,rows[row],316);
  if(!row){std::string name=v.name;if(v.editing&&std::fmod(ImGui::GetTime(),1.)<.65)name+="|";while(name.size()>1&&c.text_width(21,name)>308)name.erase(0,1);c.text(367,y+28,21,active?ink:white,name.empty()?"Player":name,316);}
 }
 if(v.editing)prompt_strip(c,{{"Start","Enter: Accept"},{"B","Esc: Cancel"}});
 else prompt_strip(c,{{"A","Change"},{"X","Save"},{"Y","Default"},{"B","Back"}});
 footer(c,v.message);
}
