/* Melee Workshop: optional packages; the stock disc is never modified.
 * All guest mutations run on the guest thread. UI requests cross via atomics. */
#include "abi_recompcore.h"
#include "platform_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_MODS 256
#define MOD_DISC 0x70000000u
#define MOD_ANIMATION_ARAM 0x71000000u
static FILE* animation_files[4];
static unsigned animation_sizes[4];
int g_mods_enabled;
typedef struct {char id[65],name[64],prefix[8],path[260],icon[512],script[512],costumes[8][32];unsigned char parts[54];int has_parts,external,internal,costume,costume_count;unsigned size,action_count,demo_count,animation_limit;} ModEntry;
static ModEntry fighters[MAX_MODS],stages[MAX_MODS];
static int nf,ns,active_stage=-1,original_sss;
static unsigned mod_animation_limit=0x8000;
static volatile LONG chosen[4]={-1,-1,-1,-1},colors[4],types[4]={0,1,3,3};
static uint32_t clone_data[4],clone_costume[4],clone_icon[4],pending_costume_fp;
static uint32_t stock_tobj[64];static int stock_port[64],stock_count,pending_stock_port=-1;
static volatile LONG ui_scene,pending_pick[4]={-1,-1,-1,-1},pending_action;
static char fighter_names[150000],stage_names[16384];
static uint32_t stage_disc_original,stage_length_original,stage_max_size,stage_fst_entry;
extern void func_8037F1E4(Context*);
extern void func_802669F4(Context*);
extern void func_8025B850(Context*);
extern void func_801A4B60(Context*);
extern void func_801C14D0(Context*);
extern void func_8039069C(Context*);
extern void func_801C39C0(Context*);
extern void func_801C3BB4(Context*);
extern void func_800311DC(Context*);
extern void func_800311CC(Context*);
extern void func_8021A740(Context*);
static uint32_t be32(const unsigned char* p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static int valid(uint32_t p,uint32_t size){return p>=0x80000000u && (uint64_t)p+size<=0x81800000u;}
static unsigned file_size(const char* path){FILE* f=fopen(path,"rb");if(!f)return 0;fseek(f,0,SEEK_END);long n=ftell(f);fclose(f);return n>0&&n<(16<<20)?(unsigned)n:0;}
void mods_initialize(Context* ctx,uint32_t fst_addr,uint32_t fst_size) {
 if(!getenv("MELEE_MODS"))return;
 const char* registry=getenv("MELEE_MOD_REGISTRY");
 FILE* f=fopen(registry?registry:"user/mods/registry.tsv","rb");if(!f)return;
 char line[2048];if(!fgets(line,sizeof line,f)||strncmp(line,"MELEE_MODS\t3",12)){fclose(f);return;}
 while(fgets(line,sizeof line,f)) {
  char *fields[11],*save=NULL,*p=strtok_r(line,"\t\r\n",&save);int n=0;
  while(p&&n<11){fields[n++]=p;p=strtok_r(NULL,"\t\r\n",&save);}if(n!=11)continue;
  ModEntry* e=NULL;if(fields[0][0]=='F'&&nf<MAX_MODS)e=&fighters[nf++];else if(fields[0][0]=='S'&&ns<MAX_MODS)e=&stages[ns++];else continue;
  snprintf(e->id,sizeof e->id,"%s",fields[1]);snprintf(e->name,sizeof e->name,"%s",fields[2]);e->external=atoi(fields[3]);e->internal=atoi(fields[4]);snprintf(e->prefix,sizeof e->prefix,"%s",fields[5]);snprintf(e->path,sizeof e->path,"%s",fields[6]);e->costume=atoi(fields[7]);snprintf(e->icon,sizeof e->icon,"%s",fields[8]);
  snprintf(e->script,sizeof e->script,"%s",fields[10]);
  char* color_save=NULL;for(char* color=strtok_r(fields[9],",",&color_save);color&&e->costume_count<8;color=strtok_r(NULL,",",&color_save))snprintf(e->costumes[e->costume_count++],32,"%s",color);

  if(e->external<0||e->external>32||strstr(e->path,"..")||strchr(e->path,':')){if(fields[0][0]=='F')nf--;else ns--;continue;}
  if(fields[0][0]=='F'&&e->path[0]!='-'){char path[512];snprintf(path,sizeof path,"%s/parts-map.bin",e->path);FILE* parts=fopen(path,"rb");if(parts){unsigned char bytes[58];if(fread(bytes,1,58,parts)==58&&!memcmp(bytes,"MP01",4)){memcpy(e->parts,bytes+4,54);e->has_parts=1;}fclose(parts);}}
  if(fields[0][0]=='F'&&e->path[0]!='-'){char path[512];snprintf(path,sizeof path,"%s/fighter-runtime.bin",e->path);FILE* meta=fopen(path,"rb");if(meta){unsigned char bytes[16];if(fread(bytes,1,16,meta)==16&&!memcmp(bytes,"MF01",4)){unsigned count=be32(bytes+4),limit=be32(bytes+8),demo=be32(bytes+12);if(count<=1024&&limit<=0x20000&&demo<=64){e->action_count=count;e->animation_limit=(limit+31)&~31u;e->demo_count=demo;if(e->animation_limit>mod_animation_limit)mod_animation_limit=e->animation_limit;}}fclose(meta);}}
  if(fields[0][0]=='S'){e->size=file_size(e->path);if(!e->size){ns--;continue;}if(e->size>stage_max_size)stage_max_size=e->size;}
 }
 fclose(f);if(!nf)return;g_mods_enabled=1;
 {size_t pos=0;for(int i=0;i<nf&&pos<sizeof fighter_names-1;i++){int n=snprintf(fighter_names+pos,sizeof fighter_names-pos,"%s\t%s\t%d\n",fighters[i].name,fighters[i].icon,fighters[i].costume_count);if(n>0)pos+=(size_t)n;}fighter_names[sizeof fighter_names-1]=0;}
 {size_t pos=(size_t)snprintf(stage_names,sizeof stage_names,"Original stages\n");for(int i=0;i<ns&&pos<sizeof stage_names-1;i++){int n=snprintf(stage_names+pos,sizeof stage_names-pos,"%s\n",stages[i].name);if(n>0)pos+=(size_t)n;}}
 if(valid(fst_addr,fst_size)) {
  unsigned char* fst=ctx->ram+(fst_addr&0x1ffffffu);uint32_t count=be32(fst+8);
  if((uint64_t)count*12<fst_size)for(uint32_t i=1;i<count;i++) {
   unsigned char* entry=fst+i*12;uint32_t name=be32(entry)&0xffffffu;
   if(entry[0] || (uint64_t)count*12+name+10>fst_size)continue;
   if(!strcmp((char*)fst+count*12+name,"GrNLa.dat")) {
    stage_disc_original=be32(entry+4);stage_length_original=be32(entry+8);
    if(stage_max_size<stage_length_original)stage_max_size=stage_length_original;
    stage_fst_entry=fst_addr+i*12;mem_write32(ctx,stage_fst_entry+4,MOD_DISC);mem_write32(ctx,stage_fst_entry+8,stage_length_original);break;
   }
  }
 }
 fprintf(stderr,"[mods] registry: %d fighters, %d custom stages\n",nf,ns);
}
int mods_dvd_info(uint32_t* offset,uint32_t* length){if(g_mods_enabled&&stage_disc_original&&(*offset==stage_disc_original||*offset==MOD_DISC)){*offset=MOD_DISC;*length=active_stage>=0?stages[active_stage].size:stage_length_original;return 1;}return 0;}
int mods_animation_read(unsigned char* ram,uint32_t guest,uint32_t source,uint32_t length){
 if(g_mods_enabled&&source>=MOD_ANIMATION_ARAM&&source<MOD_ANIMATION_ARAM+0x04000000u){
  unsigned relative=source-MOD_ANIMATION_ARAM,port=relative>>24;relative&=0xffffffu;
  if(valid(guest,length)){unsigned char* dst=ram+(guest&0x1ffffffu);memset(dst,0,length);
   if(animation_files[port]&&relative<animation_sizes[port]){unsigned amount=animation_sizes[port]-relative;if(amount>length)amount=length;fseek(animation_files[port],relative,SEEK_SET);fread(dst,1,amount,animation_files[port]);}
  }return 1;
 }
 return 0;
}
int mods_disc_read(Context* ctx,uint32_t guest,uint32_t* offset,uint32_t length) {
 if(!g_mods_enabled||*offset<MOD_DISC||(uint64_t)*offset+length>MOD_DISC+stage_max_size+32u)return 0;
 uint32_t relative=*offset-MOD_DISC;
 if(active_stage<0){*offset=stage_disc_original+relative;return 0;}
 if(!valid(guest,length))return 1;
 FILE* f=fopen(stages[active_stage].path,"rb");unsigned char* dst=ctx->ram+(guest&0x1ffffffu);memset(dst,0,length);
 if(f){fseek(f,relative,SEEK_SET);fread(dst,1,length,f);fclose(f);}else fprintf(stderr,"[mods] missing stage: %s\n",stages[active_stage].path);
 return 1;
}
static uint32_t load_archive(Context* ctx,const char* path) {
 unsigned size=file_size(path);if(size<32)return 0;FILE* f=fopen(path,"rb");if(!f)return 0;
 unsigned char* bytes=(unsigned char*)malloc(size);if(!bytes){fclose(f);return 0;}size_t got=fread(bytes,1,size,f);fclose(f);
 uint32_t data=be32(bytes+4),relocs=be32(bytes+8),roots=be32(bytes+12);
 if(got!=size||data>size-32||!roots||(uint64_t)32+data+relocs*4ull+roots*8ull>size){free(bytes);return 0;}
 uint32_t root=be32(bytes+32+data+relocs*4);if(root>=data){free(bytes);return 0;}
 for(uint32_t i=0;i<relocs;i++){uint32_t at=be32(bytes+32+data+i*4);if(at+4>data||be32(bytes+32+at)>=data){free(bytes);return 0;}}
 Context saved=*ctx;ctx->gpr[3]=(data+31)&~31u;func_8037F1E4(ctx);uint32_t base=ctx->gpr[3];*ctx=saved;
 if(!valid(base,data)){free(bytes);return 0;}
 memcpy(ctx->ram+(base&0x1ffffffu),bytes+32,data);
 for(uint32_t i=0;i<relocs;i++){uint32_t at=be32(bytes+32+data+i*4);mem_write32(ctx,base+at,base+be32(bytes+32+at));}
 free(bytes);return base+root;
}
static void prepare_animation_table(Context* ctx,int port,ModEntry* entry,uint32_t root){
 char path[320];snprintf(path,sizeof path,"%s/Pl%sAJ.dat",entry->path,entry->prefix);
 animation_sizes[port]=file_size(path);animation_files[port]=fopen(path,"rb");
 unsigned count=entry->action_count?entry->action_count:mem_read32(ctx,0x803C0FC8u+entry->internal*8+4),table=mem_read32(ctx,root+0xc);
 if(!animation_files[port]||!animation_sizes[port]||count>1024||!valid(table,count*24))return;
 for(unsigned i=0;i<count;i++){unsigned action=table+i*24,offset=mem_read32(ctx,action+4),size=mem_read32(ctx,action+8);
  mem_write32(ctx,action+0x14,size&&size<=mod_animation_limit&&(uint64_t)offset+size<=animation_sizes[port]?MOD_ANIMATION_ARAM+port*0x01000000u+offset:0);
 }
 fprintf(stderr,"[mods] P%d independent animations: %u actions, %u bytes\n",port+1,count,animation_sizes[port]);
}
static void call0(Context* ctx,void(*fn)(Context*)){Context saved=*ctx;fn(ctx);*ctx=saved;}
static void choose_css(Context* ctx,int port) {
 int index=chosen[port];if(index<0||index>=nf)return;
 uint32_t css=mem_read32(ctx,0x804D6CB0),door=0x803F0DFCu+port*0x24,chip=mem_read32(ctx,0x804A0BD0u+port*4);
 if(!valid(css,0x130))return;
 ModEntry* e=&fighters[index];int icon=-1;for(int i=0;i<25;i++)if(mem_read8(ctx,0x803F0B24u+i*0x1c+1)==e->external){icon=i;break;}
 if(icon<0&&e->external==19)icon=15;if(icon<0)return;
 uint32_t player=css+0x70+port*0x24;
 mem_write8(ctx,player,e->external);mem_write8(ctx,player+1,types[port]);mem_write8(ctx,player+3,colors[port]);mem_write8(ctx,player+0xf,7);
 mem_write8(ctx,door+9,1);mem_write8(ctx,door+0xb,types[port]);mem_write8(ctx,door+0xd,colors[port]);mem_write8(ctx,door+0xe,icon);
 if(valid(chip,0x18)){mem_write8(ctx,chip+5,0);mem_write32(ctx,chip+8,mem_read32(ctx,0x803F0B24u+icon*0x1c+0xc));mem_write32(ctx,chip+0xc,mem_read32(ctx,0x803F0B24u+icon*0x1c+0x14));}
}
#include "mods_vanilla.inc"
void mods_css_frame(Context* ctx) {
 css_install(ctx);
 InterlockedExchange(&ui_scene,1);
 // Original Melee owns player doors, token placement, colors and Ready to Fight.
 // Forced picks are restricted to explicit diagnostics.
 const char* force=getenv("MELEE_MOD_TEST_FIGHTER");
 if(force){for(int p=0;p<2;p++){if(p==0){for(int i=0;i<nf;i++)if(!strcmp(force,fighters[i].id))chosen[p]=i;}else chosen[p]=0;types[p]=p;choose_css(ctx,p);}}
 else css_sync(ctx);
 func_802669F4(ctx);
}
void mods_sss_frame(Context* ctx) {
 const char* stock_test=getenv("MELEE_MOD_TEST_STOCK_STAGE");if(stock_test){uint32_t sss=mem_read32(ctx,0x804D6C90);if(valid(sss,0x140))mem_write8(ctx,sss+3,atoi(stock_test));}
 InterlockedExchange(&ui_scene,original_sss?0:2);
 const char* test=getenv("MELEE_MOD_TEST_STAGE");if(test&&active_stage<0)for(int i=0;i<ns;i++)if(!strcmp(test,stages[i].id))InterlockedExchange(&pending_pick[0],i+1);
 LONG pick=InterlockedExchange(&pending_pick[0],-1);
 if(pick>=0){int index=pick&255;active_stage=index?index-1:-1;if(!index)original_sss=1;
  if(active_stage>=ns)active_stage=-1;
  if(stage_fst_entry)mem_write32(ctx,stage_fst_entry+8,active_stage>=0?stages[active_stage].size:stage_length_original);
  if(active_stage>=0){uint32_t sss=mem_read32(ctx,0x804D6C90);if(valid(sss,0x140)){mem_write8(ctx,sss+3,32);fprintf(stderr,"[mods] stage=%s\n",stages[active_stage].id);}}
 }
 LONG action=InterlockedExchange(&pending_action,0);if(action==2){mem_write8(ctx,0x804D6CAF,0);call0(ctx,func_801A4B60);return;}
 func_8025B850(ctx);
}
void mods_stage_init(Context* ctx) {
 Context saved=*ctx;
 ctx->gpr[3]=0;func_801C14D0(ctx);uint32_t gobj=ctx->gpr[3];
 if(gobj){ctx->gpr[3]=gobj;ctx->gpr[4]=0x801C5DB0;ctx->gpr[5]=3;ctx->gpr[6]=0;func_8039069C(ctx);}
 func_801C39C0(ctx);func_801C3BB4(ctx);ctx->gpr[3]=1;func_800311DC(ctx);ctx->gpr[3]=30000;func_800311CC(ctx);
 fprintf(stderr,"[mods] custom stage model gobj=%08X\n",gobj);*ctx=saved;
}
static ModEntry* mod_for_fp(Context* ctx,uint32_t fp){
 if(!valid(fp,0x620))return NULL;unsigned port=mem_read8(ctx,fp+0xc);if(port>=4)return NULL;int index=chosen[port];if(index<0||index>=nf||fighters[index].path[0]=='-'||mem_read32(ctx,fp+4)!=(unsigned)fighters[index].internal)return NULL;return &fighters[index];
}
extern void func_80085B10(Context*);
extern void func_80085B98(Context*);
static void mods_animation_init(Context* ctx){uint32_t fp=ctx->gpr[3];func_80085B10(ctx);ModEntry* e=mod_for_fp(ctx,fp);if(e&&e->action_count)mem_write32(ctx,fp+0x58c,e->action_count);}
static void mods_demo_animation_init(Context* ctx){
 uint32_t fp=ctx->gpr[3];ModEntry* e=mod_for_fp(ctx,fp);if(!e||!e->demo_count){func_80085B98(ctx);return;}
 uint32_t pair=0x803C25F4u+e->internal*8,old=mem_read32(ctx,pair);mem_write32(ctx,pair,0);func_80085B98(ctx);mem_write32(ctx,pair,old);
 unsigned port=mem_read8(ctx,fp+0xc),root=mem_read32(ctx,fp+0x10c),table=mem_read32(ctx,root+0x14);if(!valid(table,e->demo_count*24))return;
 mem_write32(ctx,fp+0x58c,e->demo_count);
 for(unsigned i=0;i<e->demo_count;i++){uint32_t a=table+i*24;unsigned offset=mem_read32(ctx,a+4),size=mem_read32(ctx,a+8);mem_write32(ctx,a+0x14,size&&size<=mod_animation_limit&&(uint64_t)offset+size<=animation_sizes[port]?MOD_ANIMATION_ARAM+port*0x01000000u+offset:0);}
}
extern void func_8007500C(Context*);
static void mods_bone_index(Context* ctx){
 uint32_t fp=ctx->gpr[3],part=ctx->gpr[4];
 if(valid(fp,0x620)&&part<54){unsigned port=mem_read8(ctx,fp+0xc);if(port<4){int index=chosen[port];if(index>=0&&index<nf&&fighters[index].has_parts&&mem_read32(ctx,fp+4)==(unsigned)fighters[index].internal){ctx->gpr[3]=fighters[index].parts[part];return;}}}
 func_8007500C(ctx);
}
#include "mods_lua.inc"
RecFn mods_lookup(uint32_t addr) {
 if(addr==0x80085B10)return mods_animation_init;
 if(addr==0x80085B98)return mods_demo_animation_init;
 if(addr==0x8007500C)return mods_bone_index;
 if(addr==0x800704F0)return mods_lua_expression;
 if(addr==0x8006AD10)return mods_lua_frame;
 if(addr==0x802669F4)return mods_css_frame;
 if(addr==0x802602A0)return css_cursor;
 if(addr==0x80262648)return css_chip;
 if(addr==0x8025B850)return mods_sss_frame;
 if(addr==0x8021A740)return (g_mods_enabled&&active_stage>=0)?mods_stage_init:func_8021A740;
 return NULL;
}
static void mods_forget_heap(void) {
 mods_lua_reset();
 // These archives and TObj addresses belong to HSD's current scene heap.
 // Match -> results recreates that heap just as CSS -> match does.
 memset(clone_data,0,sizeof clone_data);memset(clone_costume,0,sizeof clone_costume);
 memset(clone_icon,0,sizeof clone_icon);stock_count=0;pending_stock_port=-1;pending_costume_fp=0;
 for(int p=0;p<4;p++){
  if(animation_files[p])fclose(animation_files[p]);animation_files[p]=NULL;animation_sizes[p]=0;
 }
}
void mods_trace(uint32_t addr,Context* ctx) {
 if(addr==0x8037AD48&&g_mods_enabled&&ctx->gpr[3]==0x804590ACu&&ctx->gpr[4]<mod_animation_limit){ctx->gpr[4]=mod_animation_limit;fprintf(stderr,"[mods] fighter animation buffers: %u bytes each\n",mod_animation_limit);}
 else if(addr==0x80375428){
  mods_forget_heap();fprintf(stderr,"[mods] scene heap reset; released custom archive references\n");
 } else if(addr==0x80177368){
  fprintf(stderr,"[mods] results enter; selected=%d,%d,%d,%d\n",chosen[0],chosen[1],chosen[2],chosen[3]);
 } else if(addr==0x80177704){
  fprintf(stderr,"[mods] results exit\n");
 } else if(addr==0x8026688C){css_reset();fprintf(stderr,"[mods] CSS enter arg=%08X\n",ctx->gpr[3]);InterlockedExchange(&ui_scene,0);active_stage=-1;original_sss=0;if(stage_fst_entry)mem_write32(ctx,stage_fst_entry+8,stage_length_original);
  const char* force=getenv("MELEE_MOD_TEST_FIGHTER");if(force)for(int i=0;i<nf;i++)if(!strcmp(force,fighters[i].id))chosen[0]=i;
 } else if(addr==0x80266D70||addr==0x8025BB5C){InterlockedExchange(&ui_scene,0);
  /* Offline capture fixture for m-ex's 16-bit external stage IDs. The native
   * SSS exit still preloads the stage, sound bank and playlist normally. */
  if(addr==0x8025BB5C){
   const char* text=getenv("MELEE_TEST_STAGE_EXTERNAL");
   extern int netplay_session_active(void);
   if(text&&*text&&!netplay_session_active()){
    char* end=NULL;long stage=strtol(text,&end,10);uint32_t sss=mem_read32(ctx,0x804D6C90u);
    if(end&&!*end&&stage>=0&&stage<=65535&&valid(sss,0x140)&&mem_read8(ctx,0x804D6CAFu)==2){
     mem_write16(ctx,sss+0x1e,(uint16_t)stage);
     fprintf(stderr,"[stage-test] native SSS selected external stage %ld\n",stage);
    }
   }
  }
 } else if(addr==0x80168BF8){pending_stock_port=(ctx->lr>=0x802F7EFC&&ctx->lr<0x802FB6AC&&ctx->gpr[3]<4)?(int)ctx->gpr[3]:-1;
 } else if(addr==0x8035E800&&pending_stock_port>=0){
  int port=pending_stock_port;pending_stock_port=-1;int idx=chosen[port];if(idx<0||idx>=nf||fighters[idx].path[0]=='-')return;
  uint32_t tobj=ctx->gpr[3];if(!valid(tobj,0x80))return;
  int found=0;for(int i=0;i<stock_count;i++)if(stock_tobj[i]==tobj){stock_port[i]=port;found=1;break;}
  if(!found&&stock_count<64){stock_tobj[stock_count]=tobj;stock_port[stock_count++]=port;}
 } else if(addr==0x80360950){
  uint32_t tobj=ctx->gpr[3];for(int chain=0;chain<8&&valid(tobj,0x80);chain++,tobj=mem_read32(ctx,tobj+8)){
   for(int i=0;i<stock_count;i++)if(stock_tobj[i]==tobj){int port=stock_port[i],idx=chosen[port];if(idx<0||idx>=nf)continue;
    if(!clone_icon[port]){char path[320];snprintf(path,sizeof path,"%s/../stock-icon.dat",fighters[idx].path);clone_icon[port]=load_archive(ctx,path);fprintf(stderr,"[mods] P%d stock icon=%08X tobj=%08X\n",port+1,clone_icon[port],tobj);}
    if(clone_icon[port]){mem_write32(ctx,tobj+0x58,clone_icon[port]);mem_write32(ctx,tobj+0x5c,0);}
   }
  }
 } else if(addr==0x800D0FA0){uint32_t fp=mem_read32(ctx,ctx->gpr[3]+0x2c);if(!valid(fp,0x620))return;int port=mem_read8(ctx,fp+0xc);if(port>=4||chosen[port]<0||chosen[port]>=nf)return;ModEntry* e=&fighters[chosen[port]];
  if(e->path[0]=='-'||mem_read32(ctx,fp+4)!=(unsigned)e->internal)return;
  if(!clone_data[port]){char path[320];snprintf(path,sizeof path,"%s/Pl%s.dat",e->path,e->prefix);clone_data[port]=load_archive(ctx,path);if(clone_data[port])prepare_animation_table(ctx,port,e,clone_data[port]);fprintf(stderr,"[mods] P%d independent data=%08X package=%s\n",port+1,clone_data[port],e->id);}
  if(clone_data[port])mem_write32(ctx,fp+0x10c,clone_data[port]);
 } else if(addr==0x800686E4){pending_costume_fp=mem_read32(ctx,ctx->gpr[3]+0x2c);fprintf(stderr,"[mods] costume begin gobj=%08X fp=%08X kind=%u port=%u\n",ctx->gpr[3],pending_costume_fp,mem_read32(ctx,pending_costume_fp+4),mem_read8(ctx,pending_costume_fp+0xc));
 } else if(addr==0x80370E44&&(pending_costume_fp||ctx->lr==0x80068F8Cu)){uint32_t fp=ctx->lr==0x80068F8Cu?ctx->gpr[28]:pending_costume_fp;pending_costume_fp=0;fprintf(stderr,"[mods] costume joint fp=%08X kind=%u port=%u chosen=%d\n",fp,mem_read32(ctx,fp+4),mem_read8(ctx,fp+0xc),chosen[0]);if(!valid(fp,0x620))return;int port=mem_read8(ctx,fp+0xc);if(port>=4||chosen[port]<0||chosen[port]>=nf)return;ModEntry* e=&fighters[chosen[port]];
  if(e->path[0]=='-'||mem_read32(ctx,fp+4)!=(unsigned)e->internal)return;
  /* The selected costume name is read from the original per-kind string table
     by the package compiler; default clone uses the unchanged Nr model. */
  if(!clone_costume[port]){char path[320];unsigned color=mem_read8(ctx,fp+0x619);if(color>=(unsigned)e->costume_count)color=0;snprintf(path,sizeof path,"%s/%s",e->path,e->costumes[color]);clone_costume[port]=load_archive(ctx,path);fprintf(stderr,"[mods] P%d independent costume=%08X package=%s\n",port+1,clone_costume[port],e->id);}
  if(clone_costume[port]){ctx->gpr[3]=clone_costume[port];mem_write32(ctx,fp+0x108,clone_costume[port]);}
 }
}
void mods_host_menu(void* dll) {
 // Character and stage selection are rendered and controlled by Melee.
 // Dear ImGui is reserved for the F1 settings panel.
 (void)dll;
}

/* Native/Lua workshop objects hold host state outside the guest snapshot. */
int mods_netplay_supported(void) {
 if (!g_mods_enabled) return 1;
 if (active_stage >= 0) return 0;
 for (int p = 0; p < 4; ++p) if (chosen[p] >= 0 || lua_fighters[p].L) return 0;
 return 1;
}

void mods_release(void){mods_forget_heap();}
