/* Additive visual costumes. Stock fighter data, motion and skeletons are retained.
 * Slot tables and relocated archives live in guest RAM and are rolled back with it.
 * The host keeps only immutable archive bytes during a synchronized session. */
#include "abi_recompcore.h"
#include "recomp_funcs.h"
#include "platform_compat.h"
#ifdef _WIN32
#include <wincrypt.h>
#else
#include <openssl/evp.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CKINDS 33
#define CEXTERNAL 26
#define CCOLORS 64
#define CROWS 2048
#define CHASHES 16
static uint32_t costume_table_address=0x803C0EC0u, costume_strings_address=0x803C2360u, costume_counts_address=0x803D51A0u;
static unsigned string_stride=12,managed_kinds=CKINDS,reserved_size;
#define CTABLE costume_table_address
#define CSTRINGS costume_strings_address
#define CCOUNTS costume_counts_address
typedef struct {
 char path[1024],hash[65];
 unsigned char *bytes; unsigned size;
} CostumeArt;
typedef struct {
 char id[65],hash[65],dat_hash[65],slot[65],name[128],path[1024];
 int enabled,external,internal,base;
 unsigned char *bytes; unsigned size;
 CostumeArt art[2];
} Costume;
static Costume slots[CROWS];
static int nslots, mapping[CKINDS][CCOLORS], counts[CKINDS];
static unsigned char original_count[CKINDS], external_count[CEXTERNAL];
/* Original external roster -> internal fighter kind, from Player's ftMapping. */
static const unsigned char external_kind[CEXTERNAL]={2,3,1,24,4,5,6,17,0,18,16,8,9,12,10,15,13,14,19,7,22,20,21,26,23,25};
static const char *external_prefix[CEXTERNAL]={"Ca","Dk","Fx","Gw","Kb","Kp","Lk","Lg","Mr","Ms","Mt","Ns","Pe","Pk","Pp","Pr","Ss","Ys","Zd","Sk","Fc","Cl","Dr","Fe","Pc","Gn"};
static uint32_t original_strings[CKINDS], table_area, art_area;
static unsigned art_area_size;
static volatile LONG requested, readiness=1;
static SRWLOCK request_lock=SRWLOCK_INIT;
static char requested_hashes[CHASHES*65+1];
static int requested_local=1, initialized;
extern int netplay_session_active(void);
extern int netplay_simulating(void);
extern int netplay_costume_activation_allowed(void);
static int valid(uint32_t p,unsigned n){return p>=0x80000000u&&(uint64_t)p+n<=0x81800000u;}
static uint32_t be32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t table(int kind){return table_area+(unsigned)kind*CCOLORS*(24u+string_stride);}
static uint32_t strings(int kind){return table(kind)+CCOLORS*24u;}
static int base_color(int kind,int color){
 if(kind<0||kind>=CKINDS||color<0||color>=CCOLORS)return color;
 int i=mapping[kind][color];return i>=0&&i<nslots?slots[i].base:color;
}
static int base_external(int external,int color){
 for(int i=0;i<nslots;i++)if(slots[i].external==external)return base_color(slots[i].internal,color);
 return color;
}
void costumes_set_room_mods(const char *hashes){
 AcquireSRWLockExclusive(&request_lock);
 requested_local=hashes==NULL;
 if(hashes&&strlen(hashes)>=sizeof requested_hashes){requested_hashes[0]='!';requested_hashes[1]=0;}
 else snprintf(requested_hashes,sizeof requested_hashes,"%s",hashes?hashes:"");
 InterlockedExchange(&readiness,0);InterlockedIncrement(&requested);
 ReleaseSRWLockExclusive(&request_lock);
}
int costumes_active_ready(void){return (int)InterlockedCompareExchange(&readiness,0,0);}
static int hash_list(char *text,char hashes[CHASHES][65]){
 size_t len=strlen(text);if(len && ((len+1)%65 || text[0]==',' || text[len-1]==',' || strstr(text,",,")))return -1;
 int n=0;char *save=NULL;
 for(char *p=strtok_r(text,",",&save);p;p=strtok_r(NULL,",",&save)){
  if(n==CHASHES||strlen(p)!=64)return -1;
  for(int i=0;i<64;i++)if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')))return -1;
  if(n&&strcmp(hashes[n-1],p)>=0)return -1;
  strcpy(hashes[n++],p);
 }
 return n;
}
static int sha256_bytes(const unsigned char *bytes,unsigned size,char out[65]){
 #ifdef _WIN32
 HCRYPTPROV provider=0;HCRYPTHASH hash=0;unsigned char digest[32];DWORD n=32;int ok=0;
 if(CryptAcquireContextW(&provider,NULL,NULL,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)&&CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)&&CryptHashData(hash,bytes,size,0)&&CryptGetHashParam(hash,HP_HASHVAL,digest,&n,0)&&n==32){
  const char *hex="0123456789abcdef";for(unsigned i=0;i<32;i++){out[i*2]=hex[digest[i]>>4];out[i*2+1]=hex[digest[i]&15];}out[64]=0;ok=1;
 }
 if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);return ok;
 #else
 unsigned char digest[32];unsigned n=0;
 if(!EVP_Digest(bytes,size,digest,&n,EVP_sha256(),NULL)||n!=32)return 0;
 const char *hex="0123456789abcdef";for(unsigned i=0;i<32;i++){out[i*2]=hex[digest[i]>>4];out[i*2+1]=hex[digest[i]&15];}out[64]=0;return 1;
 #endif
}
static int read_archive(Costume *e){
 FILE *f=fopen(e->path,"rb");if(!f)return 0;
 fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
 if(size<32||size>(16<<20)){fclose(f);return 0;}
 e->bytes=malloc((size_t)size);if(!e->bytes){fclose(f);return 0;}
 size_t got=fread(e->bytes,1,(size_t)size,f);fclose(f);e->size=(unsigned)size;
 char digest[65];if(!sha256_bytes(e->bytes,e->size,digest)||strcmp(digest,e->dat_hash))return 0;
 unsigned char *b=e->bytes;uint32_t data=be32(b+4),relocs=be32(b+8),roots=be32(b+12),ext=be32(b+16);
 uint64_t symbols=32ull+data+relocs*4ull+(roots+ext)*8ull;
 if(got!=(size_t)size||be32(b)!=(unsigned)size||data>(unsigned)size-32||!roots||ext||symbols>=(unsigned)size)return 0;
 for(uint32_t i=0;i<relocs;i++){uint32_t at=be32(b+32+data+i*4);if(data<4||at>data-4||be32(b+32+at)>=data)return 0;}
 int joint=0;
 for(uint32_t i=0;i<roots;i++){
  unsigned char *r=b+32+data+relocs*4+i*8;uint32_t off=be32(r),name=be32(r+4);
  if(off>=data||symbols+name>=(unsigned)size)return 0;
  const char *s=(char*)b+symbols+name;if(!memchr(s,0,(unsigned)size-(symbols+name)))return 0;
  if(strstr(s,"_Share_joint"))joint=1;
 }
 return joint;
}
int costumes_slot_identity(int kind,int color,char *package,unsigned package_cap,char *costume,unsigned costume_cap){
 if(package&&package_cap)package[0]=0;if(costume&&costume_cap)costume[0]=0;
 if(!initialized||kind<0||kind>=CKINDS||color<0||color>=CCOLORS)return 0;
 int ix=mapping[kind][color];if(ix<0||ix>=nslots)return 0;
 if(package&&package_cap)snprintf(package,package_cap,"%s",slots[ix].id);
 if(costume&&costume_cap)snprintf(costume,costume_cap,"%s",slots[ix].slot);return 1;
}
/* These immutable textures are read only from the guest/render thread. Art is
 * validated and cached with the costume set; rendering never opens files. */
int costumes_slot_art(int kind,int color,int stock,const unsigned char **bytes,unsigned *size){
 if(bytes)*bytes=NULL;if(size)*size=0;
 if(!initialized||kind<0||kind>=CKINDS||color<0||color>=CCOLORS||(stock!=0&&stock!=1))return 0;
 int ix=mapping[kind][color];if(ix<0||ix>=nslots)return 0;
 CostumeArt *a=&slots[ix].art[stock];if(!a->bytes)return 0;
 if(bytes)*bytes=a->bytes;if(size)*size=a->size;return 1;
}
uint32_t costumes_art_arena(unsigned *size){
 if(size)*size=initialized?art_area_size:0;return initialized?art_area:0;
}
static int read_art(CostumeArt *a,int stock){
 if(!a->path[0])return 1;
 FILE *f=fopen(a->path,"rb");if(!f)return 0;
 fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
 unsigned width=stock?24u:136u,height=stock?24u:188u,payload=((width+7)/8)*((height+7)/8)*32;
 if(size!=(long)(payload+24)){fclose(f);return 0;}
 a->bytes=malloc((size_t)size);if(!a->bytes){fclose(f);return 0;}
 size_t got=fread(a->bytes,1,(size_t)size,f);fclose(f);a->size=(unsigned)size;
 unsigned char *b=a->bytes;char digest[65];
 return got==(size_t)size&&!memcmp(b,"YAMPPTEX",8)&&be32(b+8)==width&&be32(b+12)==height&&be32(b+16)==14&&be32(b+20)==payload&&sha256_bytes(b,a->size,digest)&&!strcmp(digest,a->hash);
}
static void free_rows(Costume *rows,int count){
 for(int i=0;i<count;i++){free(rows[i].bytes);for(int a=0;a<2;a++)free(rows[i].art[a].bytes);}
}
static int decimal_field(const char *s,int max,int *out){
 unsigned n=0;if(!*s)return 0;for(;*s;s++){if(*s<'0'||*s>'9')return 0;n=n*10u+(*s-'0');if(n>(unsigned)max)return 0;}*out=(int)n;return 1;
}
static int id_field(const char *s,int underscore){
 size_t n=strlen(s);if(!n||n>64||!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')))return 0;
 for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='-'||(underscore&&*s=='_')))return 0;return 1;
}
static int activate(Context *ctx,int local,char *csv){
 char hashes[CHASHES][65];int nh=hash_list(csv,hashes),seen[CHASHES]={0};
 if(nh<0)return 0;
 Costume *next=calloc(CROWS,sizeof *next);if(!next)return 0;
 int nn=0,ok=1,next_counts[CKINDS];unsigned long long total=0;
 for(int i=0;i<CKINDS;i++)next_counts[i]=original_count[i];
 const char *path=getenv("MELEE_COSTUME_REGISTRY");FILE *f=fopen(path?path:"user/mods/costumes.tsv","rb");
 if(f){
  char line[8192];int fields=0;if(!fgets(line,sizeof line,f))ok=0;else{line[strcspn(line,"\r\n")]=0;fields=!strcmp(line,"MELEE_COSTUMES\t2")?12:!strcmp(line,"MELEE_COSTUMES\t3")?16:0;if(!fields)ok=0;}
  while(ok&&fgets(line,sizeof line,f)){
   if(!strchr(line,'\n')){ok=0;break;}
   char *field[16],*save=NULL,*p=strtok_r(line,"\t\r\n",&save);int nf=0;
   while(p&&nf<fields){field[nf++]=p;p=strtok_r(NULL,"\t\r\n",&save);}
   if(nf!=fields||p||strcmp(field[0],"C")){ok=0;break;}
   int selected=local?atoi(field[3])!=0:0;
   if(!local)for(int h=0;h<nh;h++)if(!strcmp(hashes[h],field[2])){selected=1;seen[h]=1;}
   if(!selected)continue;
   if(nn==CROWS){ok=0;break;}
   Costume *e=&next[nn];
   if(!decimal_field(field[4],CEXTERNAL-1,&e->external)||!decimal_field(field[5],CKINDS-1,&e->internal)||!decimal_field(field[10],CCOLORS-1,&e->base)||!decimal_field(field[3],1,&e->enabled)||!id_field(field[1],0)||!id_field(field[7],1)||strlen(field[2])>64||strlen(field[11])!=64||strlen(field[8])>=sizeof e->name){ok=0;break;}
   if(e->internal!=external_kind[e->external]||strcmp(field[6],external_prefix[e->external])||e->base>=original_count[e->internal]||next_counts[e->internal]>=CCOLORS||strlen(field[9])>=sizeof e->path){ok=0;break;}
   snprintf(e->id,sizeof e->id,"%s",field[1]);snprintf(e->hash,sizeof e->hash,"%s",field[2]);snprintf(e->slot,sizeof e->slot,"%s",field[7]);snprintf(e->name,sizeof e->name,"%s",field[8]);snprintf(e->path,sizeof e->path,"%s",field[9]);
   snprintf(e->dat_hash,sizeof e->dat_hash,"%s",field[11]);
   if(fields==16)for(int a=0;a<2;a++){
    const char *path=field[12+a*2],*hash=field[13+a*2];
    if(!strcmp(path,"-")&&!strcmp(hash,"-"))continue;
    if(!strcmp(path,"-")||strlen(path)>=sizeof e->art[a].path||strlen(hash)!=64){ok=0;break;}
    for(int h=0;h<64;h++)if(!((hash[h]>='0'&&hash[h]<='9')||(hash[h]>='a'&&hash[h]<='f')))ok=0;
    snprintf(e->art[a].path,sizeof e->art[a].path,"%s",path);snprintf(e->art[a].hash,sizeof e->art[a].hash,"%s",hash);
   }
   if(!ok)break;
   /* The generator orders packs by immutable hash then ID, preserving declared
    * costume order. Never allocate the same pack/slot twice or alias one pack
    * ID/hash to different identities or fighters. Reject before replacing RAM. */
   if(nn&&(strcmp(next[nn-1].hash,e->hash)>0||(!strcmp(next[nn-1].hash,e->hash)&&strcmp(next[nn-1].id,e->id)>0))){ok=0;break;}
   for(int j=0;j<nn;j++){
    if(!strcmp(next[j].id,e->id)&&(strcmp(next[j].hash,e->hash)||next[j].internal!=e->internal||!strcmp(next[j].slot,e->slot)))ok=0;
    if(strcmp(e->hash,"-")&&!strcmp(next[j].hash,e->hash)&&strcmp(next[j].id,e->id))ok=0;
   }
   if(!ok)break;
   nn++;next_counts[e->internal]++;
   if(!read_archive(e)||!read_art(&e->art[0],0)||!read_art(&e->art[1],1)||(total+=e->size+e->art[0].size+e->art[1].size)>(512ull<<20)){ok=0;break;}
  }
  fclose(f);
 }else if(nh)ok=0;
 if(!local)for(int i=0;i<nh;i++)if(!seen[i])ok=0;
 if(!ok){free_rows(next,nn);free(next);fprintf(stderr,"[costumes] activation rejected; previous slot set retained\n");return 0;}
 free_rows(slots,nslots);memcpy(slots,next,(size_t)nn*sizeof *next);free(next);nslots=nn;
 memset(mapping,0xff,sizeof mapping);
 memset(ctx->ram+(table_area&0x1ffffffu),0,CKINDS*CCOLORS*(24u+string_stride));
 for(unsigned k=0;k<managed_kinds;k++){
  counts[k]=original_count[k];
  if(valid(original_strings[k],original_count[k]*string_stride))memcpy(ctx->ram+(strings(k)&0x1ffffffu),ctx->ram+(original_strings[k]&0x1ffffffu),original_count[k]*string_stride);
  mem_write32(ctx,CTABLE+k*8u,table(k));mem_write8(ctx,CTABLE+k*8u+4,original_count[k]);mem_write32(ctx,CSTRINGS+k*4u,strings(k));if(k<CEXTERNAL)mem_write8(ctx,CCOUNTS+k*4u,external_count[k]);
 }
 for(int i=0;i<nslots;i++){
  Costume *e=&slots[i];int color=counts[e->internal]++;mapping[e->internal][color]=i;
  /* Prefetch keeps using an existing disc filename. The load hook supplies the
   * immutable custom archive, so DVD lookups never see filesystem paths. */
  memcpy(ctx->ram+((strings(e->internal)+color*string_stride)&0x1ffffffu),ctx->ram+((original_strings[e->internal]+e->base*string_stride)&0x1ffffffu),string_stride);
  mem_write8(ctx,CTABLE+e->internal*8u+4,counts[e->internal]);mem_write8(ctx,CCOUNTS+e->external*4u,counts[e->internal]);
 }
 fprintf(stderr,"[costumes] activated %d additive slots (%s)\n",nslots,local?"local":"exact room set");return 1;
}
static int bind_mex_tables(Context* ctx){
 /* m-ex Header.s: rtoc+0x178 is mexData; all pointers reference its permanent heap. */
 uint32_t root=valid(ctx->gpr[2]+0x178,4)?mem_read32(ctx,ctx->gpr[2]+0x178):0;
 if(!valid(root,0x38))return 0;
 uint32_t meta=mem_read32(ctx,root),fd=mem_read32(ctx,root+8);
 if(!valid(meta,0x28)||!valid(fd,0x40)||mem_read32(ctx,meta+4)<27)return 0;
 uint32_t ct=mem_read32(ctx,fd+0x3c),cs=mem_read32(ctx,fd+0x14),cc=mem_read32(ctx,fd+0x10);
 if(!valid(ct,27*8)||!valid(cs,27*4)||!valid(cc,26*4))return 0;
 for(int i=0;i<27;i++){
  unsigned count=mem_read8(ctx,ct+i*8+4);uint32_t strings=mem_read32(ctx,cs+i*4);
  if(count>CCOLORS||(count&&!valid(strings,count*16)))return 0;
 }
 costume_table_address=ct;costume_strings_address=cs;costume_counts_address=cc;
 string_stride=16;managed_kinds=27;
 art_area=(table_area+CKINDS*CCOLORS*(24u+string_stride)+31u)&~31u;art_area_size=table_area+reserved_size-art_area;
 for(unsigned i=0;i<managed_kinds;i++){
  original_count[i]=mem_read8(ctx,CTABLE+i*8+4);original_strings[i]=mem_read32(ctx,CSTRINGS+i*4);
  if(i<CEXTERNAL)external_count[i]=mem_read8(ctx,CCOUNTS+i*4);
 }
 initialized=1;fprintf(stderr,"[costumes] bound m-ex stock-fighter costume tables\n");return 1;
}
void costumes_poll(Context *ctx){
 if(g_mex_active&&!initialized&&!bind_mex_tables(ctx))return;
 if(!initialized||!InterlockedCompareExchange(&requested,0,0)||!netplay_costume_activation_allowed()||netplay_simulating())return;
 AcquireSRWLockExclusive(&request_lock);
 if(requested){char csv[sizeof requested_hashes];memcpy(csv,requested_hashes,sizeof csv);int ok=activate(ctx,requested_local,csv);InterlockedExchange(&requested,0);InterlockedExchange(&readiness,ok?1:-1);}
 ReleaseSRWLockExclusive(&request_lock);
}
void costumes_initialize(Context *ctx,uint32_t area,unsigned size){
 if(size<CKINDS*CCOLORS*(24u+string_stride)||!valid(area,size))return;
 table_area=area;reserved_size=size;
 if(g_mex_active){memset(mapping,0xff,sizeof mapping);costumes_set_room_mods(NULL);return;}
 art_area=(area+CKINDS*CCOLORS*(24u+string_stride)+31u)&~31u;art_area_size=area+size-art_area;memset(mapping,0xff,sizeof mapping);
 for(int i=0;i<CKINDS;i++){original_count[i]=mem_read8(ctx,CTABLE+i*8u+4);if(i<CEXTERNAL)external_count[i]=mem_read8(ctx,CCOUNTS+i*4u);original_strings[i]=mem_read32(ctx,CSTRINGS+i*4u);if(original_count[i]>CCOLORS)return;}
 initialized=1;costumes_set_room_mods(NULL);costumes_poll(ctx);
}
static void load_costume(Context *ctx,RecFn original){
 unsigned kind=ctx->gpr[3],color=ctx->gpr[4];int ix=kind<CKINDS&&color<CCOLORS?mapping[kind][color]:-1;
 if(ix<0){original(ctx);return;}
 uint32_t entry=table(kind)+color*24u;if(mem_read32(ctx,entry))return;
 Costume *e=&slots[ix];Context saved=*ctx;ctx->gpr[3]=(e->size+0x60u+31u)&~31u;func_8037F1E4(ctx);uint32_t obj=ctx->gpr[3];*ctx=saved;
 if(!valid(obj,e->size+0x60u)){InterlockedExchange(&readiness,-1);ctx->exception|=1u;return;}
 uint32_t file=obj+0x60u,data=file+32,ds=be32(e->bytes+4),nr=be32(e->bytes+8),np=be32(e->bytes+12),pub=data+ds+nr*4u,sym=pub+np*8u;
 unsigned char *dst=ctx->ram+(obj&0x1ffffffu);memset(dst,0,0x60);memcpy(dst,e->bytes,32);memcpy(ctx->ram+(file&0x1ffffffu),e->bytes,e->size);
 mem_write32(ctx,obj+0x20,data);mem_write32(ctx,obj+0x24,data+ds);mem_write32(ctx,obj+0x28,pub);mem_write32(ctx,obj+0x2c,sym);mem_write32(ctx,obj+0x30,sym);mem_write32(ctx,obj+0x3c,1);mem_write32(ctx,obj+0x40,file);
 for(unsigned i=0;i<nr;i++){uint32_t at=be32(e->bytes+32+ds+i*4);mem_write32(ctx,data+at,data+be32(e->bytes+32+at));}
 uint32_t joint=0,mat=0;
 for(unsigned i=0;i<np;i++){const char *name=(char*)ctx->ram+((sym+mem_read32(ctx,pub+i*8+4))&0x1ffffffu);uint32_t p=data+mem_read32(ctx,pub+i*8);
  if(strstr(name,"_Share_joint"))joint=p;else if(strstr(name,"_matanim_joint"))mat=p;
 }
 mem_write32(ctx,entry,joint);mem_write32(ctx,entry+4,mat);mem_write32(ctx,entry+0x14,obj);
 fprintf(stderr,"[costumes] loaded %s/%s kind=%u slot=%u base=%d\n",e->id,e->slot,kind,color,e->base);
}
static void load_normal(Context *c){load_costume(c,func_80085820);}
static void load_demo(Context *c){load_costume(c,func_800858E4);}
static void fp_call(Context *ctx,RecFn fn,int gobj){
 if(gobj&&!valid(ctx->gpr[3],0x30)){fn(ctx);return;}
 uint32_t fp=gobj?mem_read32(ctx,ctx->gpr[3]+0x2c):ctx->gpr[3];
 if(!valid(fp,0x620)){fn(ctx);return;}
 unsigned color=mem_read8(ctx,fp+0x619);int base=base_color(mem_read32(ctx,fp+4),color);
 if(base!=(int)color)mem_write8(ctx,fp+0x619,base);fn(ctx);if(base!=(int)color)mem_write8(ctx,fp+0x619,color);
}
static void visibility(Context *c){fp_call(c,func_800749CC,1);}
static void texture_anim(Context *c){fp_call(c,func_80070200,0);}
/* The original compiler inlined 80070200 into 80070308. Give both the
 * material-animation binding and its indexed lookup the same base color,
 * borrowing only that slot's material descriptor for this synchronous call. */
static void bind_texture_anim(Context *c){
 uint32_t go=c->gpr[3];if(!valid(go,0x30)){func_80070308(c);return;}
 uint32_t fp=mem_read32(c,go+0x2c);if(!valid(fp,0x620)){func_80070308(c);return;}
 int kind=mem_read32(c,fp+4),color=mem_read8(c,fp+0x619),base=base_color(kind,color);
 if(base==color){func_80070308(c);return;}
 uint32_t dest=table(kind)+base*24u+4u,src=table(kind)+color*24u+4u,old=mem_read32(c,dest);
 mem_write32(c,dest,mem_read32(c,src));fp_call(c,func_80070308,1);mem_write32(c,dest,old);
}

/* Player creates Zelda/Sheik and Popo/Nana with one shared selected color.
 * Additive ordinals belong to the originating fighter only. Initialization
 * has not loaded the model yet (Fighter_Create does that immediately after),
 * so set only the secondary fighter's color to its corresponding stock base. */
static void initialize_fighter(Context *c){
 uint32_t go=c->gpr[3],arg=c->gpr[4];int remap=-1;
 if(valid(arg,7)){
  unsigned kind=mem_read32(c,arg),player=mem_read8(c,arg+4);
  if(player<6){
   uint32_t info=0x80453080u+player*0xe90u;unsigned external=mem_read32(c,info+4),color=mem_read8(c,info+0x44);
   if((external==18&&kind==7)||(external==19&&kind==19)||(external==14&&kind==11)){
    int base=base_external(external,color);if(base!=(int)color&&base>=0&&base<original_count[kind])remap=base;
   }
  }
 }
 func_80068914(c);
 if(remap>=0&&valid(go,0x30)){uint32_t fp=mem_read32(c,go+0x2c);if(valid(fp,0x620))mem_write8(c,fp+0x619,remap);}
}
/* Classic-mode enemy color assignment has a native colors[6] scratch array.
 * Its stock-only shuffle remains bounded; user selections keep all slots. */
static void classic_color_shuffle(Context *c){
 unsigned external=c->gpr[3];if(external>=CEXTERNAL){func_801695BC(c);return;}
 unsigned count=mem_read8(c,CCOUNTS+external*4u);
 mem_write8(c,CCOUNTS+external*4u,external_count[external]);
 if(c->gpr[4]==external)c->gpr[5]=base_external(external,c->gpr[5]);
 func_801695BC(c);mem_write8(c,CCOUNTS+external*4u,count);
}
/* Adventure preloads every enemy color (Link's maze, Yoshi/Kirby teams).
 * Those enemy spawners use the original palettes. Expanding this loop loads
 * all installed skins into the fixed scene heap, even for stock opponents.
 * Bound only this preload call; a selected custom costume still loads on
 * demand through load_normal and retains its independent table entry. */
static void preload_enemy_colors(Context *c){
 unsigned kind=c->gpr[3];
 if(kind>=managed_kinds){func_80087574(c);return;}
 unsigned count=mem_read8(c,CTABLE+kind*8u+4);
 mem_write8(c,CTABLE+kind*8u+4,original_count[kind]);
 func_80087574(c);
 mem_write8(c,CTABLE+kind*8u+4,count);
}
static void stock_icon(Context *c){c->gpr[5]=base_external(c->gpr[3],c->gpr[5]);func_80168B34(c);}
static void css_portrait(Context *c){
 unsigned door=c->gpr[3],frame=c->gpr[4];
 if(door<4&&!c->gpr[5]&&frame>=30){uint32_t d=0x803F0DFCu+door*0x24u;unsigned icon=mem_read8(c,d+0xe);if(icon<25){int ex=mem_read8(c,0x803F0B24u+icon*0x1cu+1);int color=frame/30;c->gpr[4]=frame%30+30*base_external(ex,color);}}
 func_8025D5AC(c);
}
/* Copy-hat archives and their filename lists have exactly the stock six Kirby
 * colors. Their arguments are Kirby's color, not the copied fighter's color.
 * Keep the expanded player/fighter slot intact and remap only these lookups. */
static unsigned kirby_hat_color(unsigned color){
 int base=base_color(4,(int)color);return base>=0&&base<original_count[4]?(unsigned)base:0;
}
static void kirby_hat_prefetch(Context *c){
 if(c->gpr[4]!=0xffu)c->gpr[4]=kirby_hat_color(c->gpr[4]);
 if(c->gpr[5]>original_count[4])c->gpr[5]=original_count[4];
 func_800EEC34(c);
}
static void kirby_hat_load(Context *c){c->gpr[4]=kirby_hat_color(c->gpr[4]);func_800EED50(c);}
static void kirby_hat_preload_list(Context *c){
 /* fn_80169C54 fills costumes[7] from the external Kirby count, then may
  * append one selected color. Only six distinct stock hat colors are needed. */
 unsigned count=mem_read8(c,CCOUNTS+4*4u);
 mem_write8(c,CCOUNTS+4*4u,original_count[4]);
 if(c->gpr[3]==4)c->gpr[4]=kirby_hat_color(c->gpr[4]);
 func_80169C54(c);mem_write8(c,CCOUNTS+4*4u,count);
}
static void color_80080144(Context *c){fp_call(c,func_80080144,0);}
static void color_8009DC54(Context *c){fp_call(c,func_8009DC54,0);}
static void color_8014A37C(Context *c){fp_call(c,func_8014A37C,1);}
static void color_8014A7F4(Context *c){fp_call(c,func_8014A7F4,1);}
static void color_800EF040(Context *c){fp_call(c,func_800EF040,1);}
static void color_800EF0E4(Context *c){fp_call(c,func_800EF0E4,1);}
static void color_800EF35C(Context *c){fp_call(c,func_800EF35C,1);}
static void color_8011B51C(Context *c){fp_call(c,func_8011B51C,1);}
static void color_80149EAC(Context *c){fp_call(c,func_80149EAC,1);}
static void color_800F0FC0(Context *c){fp_call(c,func_800F0FC0,1);}
static void color_800F10D4(Context *c){fp_call(c,func_800F10D4,1);}
static void color_800F11F0(Context *c){fp_call(c,func_800F11F0,1);}
static void color_800F130C(Context *c){fp_call(c,func_800F130C,1);}
static void color_800F14B4(Context *c){fp_call(c,func_800F14B4,1);}
static void purin_hat(Context *c){
 uint32_t go=c->gpr[3];if(!valid(go,0x30)){func_8013C360(c);return;}
 uint32_t fp=mem_read32(c,go+0x2c);if(!valid(fp,0x620)){func_8013C360(c);return;}
 int kind=mem_read32(c,fp+4),color=mem_read8(c,fp+0x619),base=base_color(kind,color);
 if(base==color){func_8013C360(c);return;}
 uint32_t dst=table(kind)+base*24,src=table(kind)+color*24,cache=0x8045A1E0u+base*4u;
 unsigned char backup[24];memcpy(backup,c->ram+(dst&0x1ffffffu),24);uint32_t cached=mem_read32(c,cache);
 memcpy(c->ram+(dst&0x1ffffffu),c->ram+(src&0x1ffffffu),24);mem_write32(c,cache,0);
 fp_call(c,func_8013C360,1);
 memcpy(c->ram+(dst&0x1ffffffu),backup,24);mem_write32(c,cache,cached);
}

RecFn costumes_lookup(uint32_t addr){
 if(!initialized)return NULL;
 switch(addr){case 0x80087574:return preload_enemy_colors;case 0x80068914:return initialize_fighter;case 0x801695BC:return classic_color_shuffle;case 0x800EEC34:return kirby_hat_prefetch;case 0x800EED50:return kirby_hat_load;case 0x80169C54:return kirby_hat_preload_list;case 0x80080144:return color_80080144;case 0x8009DC54:return color_8009DC54;case 0x8014A37C:return color_8014A37C;case 0x8014A7F4:return color_8014A7F4;case 0x800EF040:return color_800EF040;case 0x800EF0E4:return color_800EF0E4;case 0x800EF35C:return color_800EF35C;case 0x8011B51C:return color_8011B51C;case 0x80149EAC:return color_80149EAC;case 0x800F0FC0:return color_800F0FC0;case 0x800F10D4:return color_800F10D4;case 0x800F11F0:return color_800F11F0;case 0x800F130C:return color_800F130C;case 0x800F14B4:return color_800F14B4;case 0x8013C360:return purin_hat;case 0x80085820:return load_normal;case 0x800858E4:return load_demo;case 0x800749CC:return visibility;case 0x80070200:return texture_anim;case 0x80070308:return bind_texture_anim;case 0x80168B34:return stock_icon;case 0x8025D5AC:return css_portrait;default:return NULL;}
}

void costumes_release(void){free_rows(slots,nslots);nslots=0;}
