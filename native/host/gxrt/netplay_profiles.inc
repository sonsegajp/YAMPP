/* Presentation data never enters guest memory, input packets or rollback state. */
#include "../profile_data.h"
extern int aurora_link_profile_receive(const char*,const unsigned char*,unsigned);
static struct {
 int supported,dirty,initialized;
 unsigned size;
 char hash[65];
 unsigned char pixels[YAMPP_AVATAR_BYTES];
} profile_local;
static struct {
 int id;
 char hash[65];
 unsigned mask;
 unsigned char pixels[YAMPP_AVATAR_BYTES];
} profile_incoming[NETPLAY_MAX_PLAYERS];
void netplay_profile_changed(const char* name,const unsigned char* pixels,unsigned size,const char* hash) {
 if(size!=0&&size!=YAMPP_AVATAR_BYTES)return;
 if(size&&(!pixels||!hash||strlen(hash)!=64))return;
 lock();
 if(!(getenv("MELEE_NETPLAY_NAME")&&!profile_local.initialized)) {
  snprintf(np.ui.name,sizeof np.ui.name,"%s",name&&*name?name:"Player");
  snprintf(np.ui.edit_name,sizeof np.ui.edit_name,"%s",np.ui.name);
 }
 profile_local.initialized=1;profile_local.size=size;snprintf(profile_local.hash,sizeof profile_local.hash,"%s",size?hash:"");
 if(size)memcpy(profile_local.pixels,pixels,size);profile_local.dirty=1;
 unlock();
}
static void profile_send(void) {
 if(!profile_local.supported||!np.connected||np.armed||np.active)return;
 char name[32],hash[65];unsigned size;unsigned char pixels[YAMPP_AVATAR_BYTES];
 lock();if(!profile_local.dirty){unlock();return;}
 snprintf(name,sizeof name,"%s",np.ui.name);snprintf(hash,sizeof hash,"%s",profile_local.hash);
 size=profile_local.size;if(size)memcpy(pixels,profile_local.pixels,size);profile_local.dirty=0;unlock();
 char escaped[80],line[3100],encoded[2800];json_escape(name,escaped,sizeof escaped);
 snprintf(line,sizeof line,"{\"op\":\"profile\",\"name\":\"%s\",\"avatar\":\"%s\"}",escaped,hash);tcp_send_line(line);
 for(unsigned part=0;part<size/YAMPP_AVATAR_CHUNK;part++){
  b64_encode(pixels+part*YAMPP_AVATAR_CHUNK,YAMPP_AVATAR_CHUNK,encoded,sizeof encoded);
  snprintf(line,sizeof line,"{\"op\":\"avatar_part\",\"hash\":\"%s\",\"part\":%u,\"data\":\"%s\"}",hash,part,encoded);tcp_send_line(line);
 }
}
static int profile_hash_valid(const char* hash) {
 if(strlen(hash)!=64)return 0;for(unsigned i=0;i<64;i++)if(!((hash[i]>='0'&&hash[i]<='9')||(hash[i]>='a'&&hash[i]<='f')))return 0;return 1;
}
static void profile_room_pictures(void) {
 if(!profile_local.supported||np.armed||np.active)return;
 NetplayPlayer players[4];int count;lock();count=np.ui.player_count;memcpy(players,np.ui.players,sizeof players);unlock();
 for(int i=0;i<4;i++){
  if(i>=count||!profile_hash_valid(players[i].avatar)){memset(&profile_incoming[i],0,sizeof profile_incoming[i]);continue;}
  if(profile_incoming[i].id==players[i].id&&!strcmp(profile_incoming[i].hash,players[i].avatar))continue;
  memset(&profile_incoming[i],0,sizeof profile_incoming[i]);profile_incoming[i].id=players[i].id;
  snprintf(profile_incoming[i].hash,sizeof profile_incoming[i].hash,"%s",players[i].avatar);
  char line[180];snprintf(line,sizeof line,"{\"op\":\"avatar_get\",\"id\":%d,\"hash\":\"%s\"}",players[i].id,players[i].avatar);tcp_send_line(line);
 }
}
static void profile_picture_part(const char* line) {
 if(np.active||np.armed)return;
 int id=(int)json_int(line,"id",-1),part=(int)json_int(line,"part",-1);
 char hash[65],data[2800];json_str(line,"hash",hash,sizeof hash);json_str(line,"data",data,sizeof data);
 if(part<0||part>=YAMPP_AVATAR_PARTS||!profile_hash_valid(hash)||strlen(data)!=2732)return;
 unsigned char decoded[YAMPP_AVATAR_CHUNK+1];unsigned bytes=b64_decode(data,decoded,sizeof decoded);if(bytes!=YAMPP_AVATAR_CHUNK)return;
 for(int i=0;i<4;i++)if(profile_incoming[i].id==id&&!strcmp(profile_incoming[i].hash,hash)){
  if(profile_incoming[i].mask&(1u<<part))return;
  memcpy(profile_incoming[i].pixels+part*YAMPP_AVATAR_CHUNK,decoded,bytes);profile_incoming[i].mask|=1u<<part;
  if(profile_incoming[i].mask==((1u<<YAMPP_AVATAR_PARTS)-1u))
   aurora_link_profile_receive(hash,profile_incoming[i].pixels,YAMPP_AVATAR_BYTES);
  return;
 }
}
