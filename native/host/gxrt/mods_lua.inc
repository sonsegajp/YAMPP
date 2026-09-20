/* Per-player Lua behavior runs on the guest thread, using bounded host memory. */
#include "../../../upstream/lua-5.5.1/src/lua.h"
#include "../../../upstream/lua-5.5.1/src/lauxlib.h"
#include "../../../upstream/lua-5.5.1/src/lualib.h"
#include <math.h>
#define LUA_FIGHTER_MEMORY (32u*1024u*1024u)
typedef struct {
 lua_State* L;Context* ctx;uint32_t gobj,fp;int entry,self,failed,loaded,port;unsigned frame,instructions;size_t bytes;char directory[512];
} LuaFighter;
static LuaFighter lua_fighters[4];
/* PCM assets use the same output stream and volume controls as Melee's DSP. */
typedef struct {int16_t* pcm;unsigned frames,rate,channels;double cursor;float gain;} LuaVoice;
static LuaVoice lua_voices[4][8];
static unsigned lf_le16(const unsigned char* b){return b[0]|(b[1]<<8);}
static unsigned lf_le32(const unsigned char* b){return b[0]|(b[1]<<8)|(b[2]<<16)|((unsigned)b[3]<<24);}
void mods_mix_audio(int16_t* pcm,unsigned frames,unsigned rate){
 for(int p=0;p<4;p++)for(int slot=0;slot<8;slot++){
  LuaVoice* v=&lua_voices[p][slot];if(!v->pcm)continue;
  for(unsigned i=0;i<frames;i++){
   unsigned at=(unsigned)v->cursor;if(at>=v->frames){free(v->pcm);v->pcm=NULL;break;}
   unsigned next=at+1<v->frames?at+1:at;float blend=(float)(v->cursor-at);
   for(unsigned c=0;c<2;c++){unsigned channel=v->channels==1?0:c;float a=v->pcm[at*v->channels+channel],b=v->pcm[next*v->channels+channel];int value=pcm[i*2+c]+(int)((a+(b-a)*blend)*v->gain);pcm[i*2+c]=(int16_t)(value>32767?32767:value< -32768? -32768:value);}
   v->cursor+=(double)v->rate/rate;
  }
 }
}
static void lf_stop_sounds(int port){for(int i=0;i<8;i++){free(lua_voices[port][i].pcm);memset(&lua_voices[port][i],0,sizeof(LuaVoice));}}

extern void func_8006AD10(Context*);
static void* lf_alloc(void* ud,void* ptr,size_t old,size_t size){
 LuaFighter* f=ud;if(!ptr)old=0;
 if(!size){free(ptr);f->bytes-=old;return NULL;}
 if(size>old&&size-old>LUA_FIGHTER_MEMORY-f->bytes)return NULL;
 void* p=realloc(ptr,size);if(p)f->bytes=f->bytes-old+size;return p;
}
static LuaFighter* lf_current(lua_State* L){return *(LuaFighter**)lua_getextraspace(L);}
static LuaFighter* lf_self(lua_State* L){
 LuaFighter* f=*(LuaFighter**)luaL_checkudata(L,1,"melee.fighter");
 if(!f->ctx||!valid(f->fp,0x2400))luaL_error(L,"Fighter is unavailable outside its callback");return f;
}
static void lf_budget(lua_State* L,lua_Debug* ar){(void)ar;LuaFighter* f=lf_current(L);if(++f->instructions>20)luaL_error(L,"Lua instruction budget exceeded");}
static float lf_number(lua_State* L,int at,float limit){double n=luaL_checknumber(L,at);if(!isfinite(n)||fabs(n)>limit)luaL_argerror(L,at,"Value is outside the supported range");return (float)n;}
static float lf_float(LuaFighter* f,uint32_t offset){uint32_t u=mem_read32(f->ctx,f->fp+offset);float v;memcpy(&v,&u,4);return v;}
static void lf_write_float(LuaFighter* f,uint32_t offset,float v){uint32_t u;memcpy(&u,&v,4);mem_write32(f->ctx,f->fp+offset,u);}
static void lf_field(lua_State* L,const char* key,double value){lua_pushnumber(L,value);lua_setfield(L,-2,key);}
static int lf_state(lua_State* L){LuaFighter* f=lf_self(L);lua_newtable(L);
 lf_field(L,"frame",f->frame);lf_field(L,"port",f->port+1);lf_field(L,"motion",mem_read32(f->ctx,f->fp+0x10));lf_field(L,"animation",mem_read32(f->ctx,f->fp+0x14));
 lf_field(L,"animation_frame",lf_float(f,0x894));lf_field(L,"x",lf_float(f,0xb0));lf_field(L,"y",lf_float(f,0xb4));lf_field(L,"facing",lf_float(f,0x2c));lf_field(L,"damage",lf_float(f,0x1830));
 lf_field(L,"stick_x",lf_float(f,0x620));lf_field(L,"stick_y",lf_float(f,0x624));lf_field(L,"held",mem_read32(f->ctx,f->fp+0x65c));lf_field(L,"pressed",mem_read32(f->ctx,f->fp+0x668));
 lua_pushboolean(L,mem_read32(f->ctx,f->fp+0xe0)!=0);lua_setfield(L,-2,"airborne");return 1;
}
static int lf_log(lua_State* L){LuaFighter* f=lf_self(L);fprintf(stderr,"[lua] P%d %s\n",f->port+1,luaL_checkstring(L,2));return 0;}
static int lf_velocity(lua_State* L){LuaFighter* f=lf_self(L);float x=lf_number(L,2,100),y=lf_number(L,3,100);lf_write_float(f,0x80,x);lf_write_float(f,0x84,y);if(!mem_read32(f->ctx,f->fp+0xe0))lf_write_float(f,0xec,x);return 0;}
static int lf_damage(lua_State* L){LuaFighter* f=lf_self(L);float value=lf_number(L,2,999);if(value<0)luaL_argerror(L,2,"Damage cannot be negative");lf_write_float(f,0x1830,value);return 0;}
static int lf_speed(lua_State* L){LuaFighter* f=lf_self(L);float value=lf_number(L,2,8);if(value<0)luaL_argerror(L,2,"Animation speed cannot be negative");lf_write_float(f,0x89c,value);return 0;}
static int lf_sound(lua_State* L){
 LuaFighter* f=lf_self(L);const char* name=luaL_checkstring(L,2);int slot=(int)luaL_optinteger(L,3,1)-1;float gain=lua_isnoneornil(L,4)?1:lf_number(L,4,2);
 if(slot<0||slot>=8||gain<0)return luaL_error(L,"Sound channel must be 1..8 and gain 0..2");
 size_t n=strlen(name);if(!n||n>200||name[0]=='/'||strstr(name,".."))return luaL_error(L,"Invalid package sound path");
 for(size_t i=0;i<n;i++){char c=name[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'||c=='/'))return luaL_error(L,"Invalid package sound path");}
 char path[800];snprintf(path,sizeof path,"%s/../%s",fighters[chosen[f->port]].path,name);FILE* file=fopen(path,"rb");if(!file)return luaL_error(L,"Missing sound asset: %s",name);
 fseek(file,0,SEEK_END);long size=ftell(file);rewind(file);if(size<44||size>8*1024*1024){fclose(file);return luaL_error(L,"WAV must be smaller than 8 MiB");}
 unsigned char* bytes=malloc((size_t)size);if(!bytes){fclose(file);return luaL_error(L,"Sound allocation failed");}size_t got=fread(bytes,1,(size_t)size,file);fclose(file);
 unsigned channels=0,rate=0,bits=0,format=0,length=0;unsigned char* data=NULL;
 if(got==(size_t)size&&!memcmp(bytes,"RIFF",4)&&!memcmp(bytes+8,"WAVE",4))for(size_t at=12;at+8<=(size_t)size;){
  unsigned len=lf_le32(bytes+at+4);if(len>(size_t)size-at-8)break;
  if(!memcmp(bytes+at,"fmt ",4)&&len>=16){format=lf_le16(bytes+at+8);channels=lf_le16(bytes+at+10);rate=lf_le32(bytes+at+12);bits=lf_le16(bytes+at+22);}
  if(!memcmp(bytes+at,"data",4)){data=bytes+at+8;length=len;}
  at+=8ull+len+(len&1);
 }
 if(format!=1||bits!=16||(channels!=1&&channels!=2)||rate<8000||rate>96000||!data||length%(channels*2)){free(bytes);return luaL_error(L,"Sound must be 16-bit mono or stereo PCM WAV");}
 int16_t* samples=malloc(length?length:2);if(!samples){free(bytes);return luaL_error(L,"Sound allocation failed");}memcpy(samples,data,length);free(bytes);
 LuaVoice* voice=&lua_voices[f->port][slot];free(voice->pcm);*voice=(LuaVoice){samples,length/(channels*2),rate,channels,0,gain};return 0;
}
#include "mods_lua_game.inc"
static int lf_require(lua_State* L){
 LuaFighter* f=lf_current(L);const char* name=luaL_checkstring(L,1);size_t n=strlen(name);char relative[256],path[800];
 if(!n||n>200)return luaL_error(L,"Invalid module name");
 for(size_t i=0;i<n;i++){char c=name[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return luaL_error(L,"Invalid module name");relative[i]=c=='.'?'/':c;}
 if(name[0]=='.'||name[n-1]=='.'||strstr(name,".."))return luaL_error(L,"Invalid module path");relative[n]=0;
 lua_getfield(L,LUA_REGISTRYINDEX,"melee.modules");lua_getfield(L,-1,name);if(!lua_isnil(L,-1))return 1;lua_pop(L,1);
 snprintf(path,sizeof path,"%s/%s.lua",f->directory,relative);
 if(luaL_loadfilex(L,path,"t")!=LUA_OK)return lua_error(L);
 lua_pushboolean(L,1);lua_setfield(L,-3,name); // Detect recursive modules.
 if(lua_pcall(L,0,1,0)!=LUA_OK)return lua_error(L);
 if(lua_isnil(L,-1)){lua_pop(L,1);lua_pushboolean(L,1);}lua_pushvalue(L,-1);lua_setfield(L,-3,name);return 1;
}
static void lf_close(LuaFighter* f){lf_stop_sounds(f->port);if(f->L)lua_close(f->L);memset(f,0,sizeof *f);}
static void mods_lua_reset(void){for(int p=0;p<4;p++)lf_close(&lua_fighters[p]);}
static void lf_error(LuaFighter* f,const char* stage){fprintf(stderr,"[lua] P%d %s: %s\n",f->port+1,stage,lua_tostring(f->L,-1));lua_settop(f->L,0);f->failed=1;}
static void lf_callback(LuaFighter* f,const char* name){
 lua_State* L=f->L;if(!L||f->failed)return;lua_rawgeti(L,LUA_REGISTRYINDEX,f->entry);lua_getfield(L,-1,name);lua_remove(L,-2);
 if(lua_isnil(L,-1)){lua_pop(L,1);return;}lua_rawgeti(L,LUA_REGISTRYINDEX,f->self);f->instructions=0;
 if(lua_pcall(L,1,0,0)!=LUA_OK)lf_error(f,name);
}
static void lf_load(LuaFighter* f,const char* path){
 f->loaded=1;snprintf(f->directory,sizeof f->directory,"%s",path);char* slash=strrchr(f->directory,'/');if(!slash)slash=strrchr(f->directory,'\\');if(!slash){f->failed=1;return;}*slash=0;
 lua_State* L=f->L=lua_newstate(lf_alloc,f,0);if(!L){f->failed=1;return;}*(LuaFighter**)lua_getextraspace(L)=f;
 const luaL_Reg libs[]={{LUA_GNAME,luaopen_base},{LUA_TABLIBNAME,luaopen_table},{LUA_STRLIBNAME,luaopen_string},{LUA_MATHLIBNAME,luaopen_math},{LUA_UTF8LIBNAME,luaopen_utf8},{LUA_COLIBNAME,luaopen_coroutine},{NULL,NULL}};
 for(const luaL_Reg* l=libs;l->name;l++){luaL_requiref(L,l->name,l->func,1);lua_pop(L,1);}
 const char* removed[]={"dofile","loadfile","load","collectgarbage",NULL};for(int i=0;removed[i];i++){lua_pushnil(L);lua_setglobal(L,removed[i]);}
 lua_pushcfunction(L,lf_require);lua_setglobal(L,"require");lua_newtable(L);lua_setfield(L,LUA_REGISTRYINDEX,"melee.modules");
 luaL_newmetatable(L,"melee.fighter");lua_newtable(L);
 const luaL_Reg methods[]={{"state",lf_state},{"log",lf_log},{"velocity",lf_velocity},{"set_damage",lf_damage},{"animation_speed",lf_speed},{"sound",lf_sound},{"motion",lf_motion},{"animation",lf_animation},{"interrupt",lf_interrupt},{"hitbox",lf_hitbox},{"clear_hitboxes",lf_clear_hitboxes},{"players",lf_players},{"hitboxes",lf_hitboxes},{"stop_sound",lf_stop_sound},{"command",lf_command},{"jab_transition",lf_jab},{"hurtbox_state",lf_hurt},{"facing",lf_face},{NULL,NULL}};
 luaL_setfuncs(L,methods,0);lua_setfield(L,-2,"__index");lua_pop(L,1);
 LuaFighter** self=lua_newuserdatauv(L,sizeof *self,0);*self=f;luaL_setmetatable(L,"melee.fighter");f->self=luaL_ref(L,LUA_REGISTRYINDEX);
 lua_sethook(L,lf_budget,LUA_MASKCOUNT,10000);f->instructions=0;
 if(luaL_loadfilex(L,path,"t")!=LUA_OK||lua_pcall(L,0,1,0)!=LUA_OK){lf_error(f,"load");return;}
 if(!lua_istable(L,-1)){lua_pushliteral(L,"Entry must return a fighter table");lf_error(f,"load");return;}
 lua_getfield(L,-1,"api_version");int version=(int)lua_tointeger(L,-1);lua_pop(L,1);if(version!=1){lua_pushliteral(L,"Expected api_version = 1");lf_error(f,"load");return;}
 f->entry=luaL_ref(L,LUA_REGISTRYINDEX);fprintf(stderr,"[lua] P%d loaded %s (%s)\n",f->port+1,path,LUA_RELEASE);lf_callback(f,"on_spawn");
}
static void mods_lua_frame(Context* ctx){
 uint32_t gobj=ctx->gpr[3];func_8006AD10(ctx);
 uint32_t fp=mem_read32(ctx,gobj+0x2c);if(!valid(fp,0x2400))return;unsigned port=mem_read8(ctx,fp+0xc);if(port>=4)return;
 int idx=chosen[port];if(idx<0||idx>=nf||fighters[idx].script[0]=='-'||!fighters[idx].script[0])return;
 LuaFighter* f=&lua_fighters[port];if(f->fp&&f->fp!=fp)lf_close(f);f->ctx=ctx;f->gobj=gobj;f->fp=fp;f->port=port;
 Context saved=*ctx;if(!f->loaded)lf_load(f,fighters[idx].script);
 if(!(mem_read8(ctx,fp+0x221f)&0x10)&&!(mem_read8(ctx,fp+0x2219)&0x04)){lf_callback(f,"on_frame");f->frame++;}
 *ctx=saved;f->ctx=NULL;
}

static void mods_lua_expression(Context* ctx){
 uint32_t gobj=ctx->gpr[3],fp=mem_read32(ctx,gobj+0x2c);ModEntry* e=mod_for_fp(ctx,fp);
 if(!e||!e->has_parts){func_800704F0(ctx);return;}
 /* Imported rigs own their facial channels. DK's native damage callback must
  * not index a texture-animation table belonging to a different skeleton. */
 unsigned port=mem_read8(ctx,fp+0xc);LuaFighter* f=&lua_fighters[port];if(!f->L||f->failed||f->fp!=fp)return;
 lua_State* L=f->L;lua_rawgeti(L,LUA_REGISTRYINDEX,f->entry);lua_getfield(L,-1,"on_expression");lua_remove(L,-2);if(lua_isnil(L,-1)){lua_pop(L,1);return;}
 Context saved=*ctx;Context* old=f->ctx;f->ctx=ctx;f->instructions=0;
 lua_rawgeti(L,LUA_REGISTRYINDEX,f->self);lua_pushinteger(L,ctx->gpr[4]);lua_pushnumber(L,ctx->fpr[1]);if(lua_pcall(L,3,0,0)!=LUA_OK)lf_error(f,"on_expression");f->ctx=old;*ctx=saved;
}
