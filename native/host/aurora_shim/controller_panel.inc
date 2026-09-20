// Uses the same photographed Nintendo controller as the Online room cards.
void controller_panel(Canvas& c) {
 const auto& v=controller_menu_view();
 c.text(110,58,30,IM_COL32(130,134,147,255),"Options",136,false,false,true);
 c.text(284,58,30,IM_COL32(130,134,147,255),"Controller",402,false,false,true);
 for(int port=0;port<4;port++){
  float x=100+port*70.f;bool active=port==v.port;
  c.box(x,113,61,27,active?gold:IM_COL32(12,20,35,220),3);
  c.text(x+30,116,20,active?ink:white,"P"+number(port+1),55,true);
 }
 c.text(236,149,17,white,v.device,275,true);
 constexpr float x=91,y=171,w=286,h=226.67f,k=w/429.f;
 c.art(netplay_art::controller,x,y,w,h);
 auto point=[&](float px,float py){return ImVec2{x+(px-233)*k,y+(py-691)*k};};
 auto lamp=[&](float px,float py,float radius,unsigned button){
  if(!(v.pad.buttons&button))return;
  auto q=point(px,py);
  c.d->AddCircle(c.p(q.x,q.y),radius*k*c.scale,IM_COL32(255,243,103,255),24,3*c.scale);
  c.circle(q.x,q.y,2.5f,white);
 };
 lamp(581,856,28,0x100);lamp(532,879,19,0x200);
 lamp(617,843,20,0x400);lamp(568,808,23,0x800);lamp(449,850,13,0x1000);
 // Stick caps visibly travel inside their gates. The small central marker is
 // retained at rest so both neutral and full travel are legible on the photo.
 auto stick=[&](float px,float py,int sx,int sy,float r){
  auto q=point(px,py);float dx=std::clamp(sx/127.f,-1.f,1.f)*r,dy=-std::clamp(sy/127.f,-1.f,1.f)*r;
  c.d->AddCircle(c.p(q.x,q.y),r*c.scale,IM_COL32(167,218,229,180),24,c.scale);
  c.line(q.x,q.y,q.x+dx,q.y+dy,white,2);
  c.circle(q.x+dx,q.y+dy,4.5f,gold);c.border(q.x+dx-2,q.y+dy-2,4,4,white,1,2);
 };
 stick(320,852,v.pad.stick_x,v.pad.stick_y,13);
 stick(523,932,v.pad.cstick_x,v.pad.cstick_y,11);
 auto dpad=point(388,931);
 if(v.pad.buttons&1)c.box(dpad.x-12,dpad.y-3,8,6,gold);
 if(v.pad.buttons&2)c.box(dpad.x+4,dpad.y-3,8,6,gold);
 if(v.pad.buttons&4)c.box(dpad.x-3,dpad.y+4,6,8,gold);
 if(v.pad.buttons&8)c.box(dpad.x-3,dpad.y-12,6,8,gold);
 for(int side=0;side<2;side++){
  float tx=106+side*151.f;int amount=side?v.pad.trigger_right:v.pad.trigger_left;
  bool click=(v.pad.buttons&(side?0x20:0x40))!=0;
  c.text(tx,378,16,click?gold:white,side?"R":"L");
  c.box(tx+20,382,95,9,IM_COL32(27,37,53,235),3);
  c.box(tx+20,382,95*amount/255.f,9,gold,3);
  c.border(tx+20,382,95,9,click?white:muted,click?2:1,3);
 }
 c.text(237,405,15,(v.pad.buttons&0x10)?gold:white,v.live_test?((v.pad.buttons&0x10)?"Z pressed":"Live input test"):std::string("Z: Tap jump ")+(v.tap_jump?"On":"Off"),280,true);
 c.box(394,112,316,275,IM_COL32(5,13,25,223),4);
 c.border(394,112,316,275,IM_COL32(140,162,175,200),1,4);
 for(int visible=0;visible<8;visible++){
  int row=v.first+visible;if(row>=v.count)break;float ry=120+visible*31.f;bool selected=row==v.row;
  if(selected)c.box(401,ry-2,302,29,gold,2);
  ImU32 color=selected?ink:white;
  c.text(411,ry+3,17,color,v.labels[row],170);
  c.text(586,ry+3,16,color,v.binding==row?"Press input...":v.values[row],108);
 }
 c.text(550,395,14,muted,number(v.row+1)+" / "+number(v.count)+"   Up / Down to scroll",302,true);
 if(v.binding)prompt_strip(c,{{"B","Start + B: Cancel"}});
 else if(v.live_test)prompt_strip(c,{{"B","Start + B: End test"}});
 else prompt_strip(c,{{"A","Change"},{"X","Port"},{"Y",v.keyboard?"Gamepad":"Keyboard"},{"B","Back"}});
 footer(c,v.message);
}
