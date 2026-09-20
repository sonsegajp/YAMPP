/* Fighter operations backed by Melee's native animation/collision routines. */
extern void func_800693AC(Context*);
extern void func_80085CD8(Context*);
extern void func_8006EBE8(Context*);
extern void func_800768A0(Context*);
extern void func_8007ABD0(Context*);
extern void func_8007AC9C(Context*);
extern void func_8007AFF8(Context*);
extern void func_8007AFC8(Context*);
extern void func_80080484(Context*);
extern void func_800704F0(Context*);
static int lf_motion(lua_State* L){
 LuaFighter* f=lf_self(L);int state=(int)luaL_checkinteger(L,2);if(state<0||state>340)return luaL_error(L,"Motion must be a common Melee state (0..340)");
 Context saved=*f->ctx;f->ctx->gpr[3]=f->gobj;f->ctx->gpr[4]=state;f->ctx->gpr[5]=0;f->ctx->gpr[6]=0;f->ctx->fpr[1]=0;f->ctx->fpr[2]=1;f->ctx->fpr[3]=0;func_800693AC(f->ctx);*f->ctx=saved;return 0;
}
static int lf_animation(lua_State* L){
 LuaFighter* f=lf_self(L);int id=(int)luaL_checkinteger(L,2);float speed=lua_isnoneornil(L,3)?1:lf_number(L,3,8);unsigned count=mem_read32(f->ctx,f->fp+0x58c);
 if(id<0||(unsigned)id>=count||speed<0)return luaL_error(L,"Animation is outside the package table");
 Context saved=*f->ctx;f->ctx->gpr[3]=f->fp;f->ctx->gpr[4]=f->fp;f->ctx->gpr[5]=id;func_80085CD8(f->ctx);
 mem_write32(f->ctx,f->fp+0x14,id);lf_write_float(f,0x894,0);lf_write_float(f,0x898,0);lf_write_float(f,0x89c,speed);
 f->ctx->gpr[3]=f->gobj;f->ctx->fpr[1]=0;f->ctx->fpr[2]=speed;f->ctx->fpr[3]=0;func_8006EBE8(f->ctx);*f->ctx=saved;return 0;
}
static int lf_interrupt(lua_State* L){LuaFighter* f=lf_self(L);unsigned v=mem_read8(f->ctx,f->fp+0x2218);mem_write8(f->ctx,f->fp+0x2218,lua_toboolean(L,2)?v|0x80:v&~0x80);return 0;}
static double lf_option(lua_State* L,const char* key,double fallback,double low,double high){
 lua_getfield(L,2,key);double v=lua_isnil(L,-1)?fallback:luaL_checknumber(L,-1);lua_pop(L,1);if(!isfinite(v)||v<low||v>high)luaL_error(L,"Invalid hitbox %s",key);return v;
}
static int lf_hitbox(lua_State* L){
 LuaFighter* f=lf_self(L);luaL_checktype(L,2,LUA_TTABLE);
 unsigned id=(unsigned)lf_option(L,"id",0,0,3),group=(unsigned)lf_option(L,"group",0,0,7),bone=(unsigned)lf_option(L,"bone",0,0,199);
 unsigned table=mem_read32(f->ctx,0x804D6544u),kind=mem_read32(f->ctx,f->fp+4),parts=mem_read32(f->ctx,f->fp+0x5e8),count=mem_read32(f->ctx,mem_read32(f->ctx,table+kind*4)+8);
 if(bone>=count||!valid(parts,count*16))return luaL_error(L,"Hitbox bone is outside the fighter rig");
 unsigned joint=mem_read32(f->ctx,parts+bone*16);if(!valid(joint,0x80))return luaL_error(L,"Hitbox bone has no joint");
 unsigned damage=(unsigned)lf_option(L,"damage",0,0,999),angle=(unsigned)lf_option(L,"angle",361,0,367),growth=(unsigned)lf_option(L,"growth",100,0,1000),base=(unsigned)lf_option(L,"base",0,0,1000),weight=(unsigned)lf_option(L,"weight",0,0,1000),element=(unsigned)lf_option(L,"element",0,0,16),shield=(unsigned)lf_option(L,"shield",0,0,100),severity=(unsigned)lf_option(L,"sound_level",1,0,2),sound=(unsigned)lf_option(L,"sound_kind",0,0,31);
 float x=lf_option(L,"x",0,-500,500),y=lf_option(L,"y",0,-500,500),z=lf_option(L,"z",0,-500,500),size=lf_option(L,"size",1,0,100);
 unsigned flags=(lf_option(L,"clank",1,0,1)?0x80:0)|(lf_option(L,"rebound",0,0,1)?0x40:0)|(lf_option(L,"air",1,0,1)?0x20:0)|(lf_option(L,"ground",1,0,1)?0x10:0);
 uint32_t hit=f->fp+0x914+id*0x138;Context saved=*f->ctx;
 if(!mem_read32(f->ctx,hit)||mem_read32(f->ctx,hit+4)!=group){mem_write32(f->ctx,hit,1);mem_write32(f->ctx,hit+4,group);f->ctx->gpr[3]=f->fp;f->ctx->gpr[4]=hit;func_800768A0(f->ctx);}
 mem_write8(f->ctx,f->fp+0x2219,mem_read8(f->ctx,f->fp+0x2219)|0x10);
 mem_write32(f->ctx,hit+0x48,joint);f->ctx->gpr[3]=hit;f->ctx->gpr[4]=damage;f->ctx->gpr[5]=f->gobj;func_8007ABD0(f->ctx);
 mem_wf32(hit+0x10,x);mem_wf32(hit+0x14,y);mem_wf32(hit+0x18,z);mem_wf32(hit+0x1c,size);
 f->ctx->gpr[3]=hit;f->ctx->gpr[4]=angle;f->ctx->gpr[5]=f->gobj;func_8007AC9C(f->ctx);
 mem_write32(f->ctx,hit+0x24,growth);mem_write32(f->ctx,hit+0x28,weight);mem_write32(f->ctx,hit+0x2c,base);mem_write32(f->ctx,hit+0x30,element);mem_write32(f->ctx,hit+0x34,shield);mem_write32(f->ctx,hit+0x38,severity);mem_write32(f->ctx,hit+0x3c,sound);
 mem_write8(f->ctx,hit+0x40,flags);mem_write8(f->ctx,hit+0x41,0);mem_write8(f->ctx,hit+0x42,0x45);mem_write8(f->ctx,hit+0x43,0);mem_write32(f->ctx,hit+0x134,0);
 f->ctx->gpr[3]=f->fp;func_80080484(f->ctx);*f->ctx=saved;return 0;
}
static int lf_clear_hitboxes(lua_State* L){LuaFighter* f=lf_self(L);int id=lua_isnoneornil(L,2)?-1:(int)luaL_checkinteger(L,2);if(id< -1||id>3)return luaL_error(L,"Hitbox ID must be 0..3");Context saved=*f->ctx;f->ctx->gpr[3]=f->gobj;f->ctx->gpr[4]=id;if(id<0)func_8007AFF8(f->ctx);else func_8007AFC8(f->ctx);*f->ctx=saved;return 0;}

static int lf_command(lua_State* L){LuaFighter* f=lf_self(L);int id=(int)luaL_checkinteger(L,2),v=(int)luaL_checkinteger(L,3);if(id<0||id>3||v<0||v>65535)return luaL_error(L,"Command variable requires index 0..3 and value 0..65535");mem_write32(f->ctx,f->fp+0x2200+4*id,v);return 0;}
static int lf_jab(lua_State* L){LuaFighter* f=lf_self(L);unsigned v=mem_read8(f->ctx,f->fp+0x2218);mem_write8(f->ctx,f->fp+0x2218,lua_toboolean(L,2)?v|0x40:v&~0x40);return 0;}
extern void func_8007B0C0(Context*);
static int lf_hurt(lua_State* L){LuaFighter* f=lf_self(L);int state=(int)luaL_checkinteger(L,2);if(state<0||state>2)return luaL_error(L,"Hurtbox state is 0 normal, 1 invincible, or 2 intangible");Context saved=*f->ctx;f->ctx->gpr[3]=f->gobj;f->ctx->gpr[4]=state;func_8007B0C0(f->ctx);*f->ctx=saved;return 0;}
static int lf_face(lua_State* L){LuaFighter* f=lf_self(L);float v=lf_number(L,2,1);if(v!=1&&v!=-1)return luaL_error(L,"Facing must be -1 or 1");lf_write_float(f,0x2c,v);return 0;}

extern void func_80034110(Context*);
static int lf_players(lua_State* L){
 LuaFighter* f=lf_self(L);Context saved=*f->ctx;lua_newtable(L);
 for(int p=0;p<4;p++){
  f->ctx->gpr[3]=p;func_80034110(f->ctx);uint32_t g=f->ctx->gpr[3];if(!valid(g,0x30))continue;uint32_t fp=mem_read32(f->ctx,g+0x2c);if(!valid(fp,0x2400))continue;
  lua_newtable(L);lf_field(L,"port",p+1);lf_field(L,"x",mem_rf32(fp+0xb0));lf_field(L,"y",mem_rf32(fp+0xb4));lf_field(L,"damage",mem_rf32(fp+0x1830));lf_field(L,"motion",mem_read32(f->ctx,fp+0x10));lua_pushboolean(L,mem_read32(f->ctx,fp+0xe0)!=0);lua_setfield(L,-2,"airborne");lua_rawseti(L,-2,p+1);
 }
 *f->ctx=saved;return 1;
}
static int lf_hitboxes(lua_State* L){
 LuaFighter* f=lf_self(L);lua_newtable(L);for(int i=0;i<4;i++){uint32_t h=f->fp+0x914+i*0x138;if(!mem_read32(f->ctx,h))continue;lua_newtable(L);lf_field(L,"id",i);lf_field(L,"damage",mem_rf32(h+0xc));lf_field(L,"x",mem_rf32(h+0x4c));lf_field(L,"y",mem_rf32(h+0x50));lf_field(L,"z",mem_rf32(h+0x54));lf_field(L,"size",mem_rf32(h+0x1c));lf_field(L,"victims",mem_read8(f->ctx,h+0x44));lua_rawseti(L,-2,i+1);}return 1;
}
static int lf_stop_sound(lua_State* L){LuaFighter* f=lf_self(L);int channel=(int)luaL_checkinteger(L,2)-1;if(channel<0||channel>7)return luaL_error(L,"Sound channel must be 1..8");LuaVoice* voice=&lua_voices[f->port][channel];free(voice->pcm);memset(voice,0,sizeof *voice);return 0;}
