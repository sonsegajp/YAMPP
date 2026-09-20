/* Production costume loader against the original DOL and costume archives.
 * Guest allocator checkpoints are included in the RAM replay assertions. */
#include <assert.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <stdlib.h>
#define _mkdir(path) mkdir(path, 0755)
#define _putenv putenv
#endif
#include "../costumes.c"
Context *g_cur_ctx;int g_mex_active;
static int session,observed_color=-1;
int netplay_session_active(void){return session;}
int netplay_costume_activation_allowed(void){return !session;}
int netplay_simulating(void){return 0;}
u8 mem_read8(CPUState *c,u32 a){assert(valid(a,1));return c->ram[a&0x1ffffffu];}
void mem_write8(CPUState *c,u32 a,u8 v){assert(valid(a,1));c->ram[a&0x1ffffffu]=v;}
u32 mem_read32(CPUState *c,u32 a){assert(valid(a,4));return be32(c->ram+(a&0x1ffffffu));}
void mem_write32(CPUState *c,u32 a,u32 v){assert(valid(a,4));unsigned char *p=c->ram+(a&0x1ffffffu);p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
void func_8037F1E4(Context *c){unsigned n=c->gpr[3],p=mem_read32(c,0x80010000);mem_write32(c,0x80010000,p+n);c->gpr[3]=p;}
void func_800749CC(Context *c){observed_color=mem_read8(c,mem_read32(c,c->gpr[3]+0x2c)+0x619);}
void func_80070200(Context *c){observed_color=mem_read8(c,c->gpr[3]+0x619);}
static uint32_t observed_mat;
void func_80070308(Context *c){uint32_t fp=mem_read32(c,c->gpr[3]+0x2c);observed_color=mem_read8(c,fp+0x619);observed_mat=mem_read32(c,table(mem_read32(c,fp+4))+observed_color*24u+4);}
static unsigned observed_hat_kind,observed_hat_color,observed_hat_count;
void func_800EEC34(Context *c){observed_hat_kind=c->gpr[3];observed_hat_color=c->gpr[4];observed_hat_count=c->gpr[5];assert(observed_hat_color==0xffu||observed_hat_color<6);assert(observed_hat_count<=6);}
void func_800EED50(Context *c){observed_hat_kind=c->gpr[3];observed_hat_color=c->gpr[4];assert(observed_hat_color<6);}
void func_80169C54(Context *c){
 /* Mirror the original fixed-array bound: six enumerated colors plus one
  * optional selected Kirby. An expanded count would overflow this array. */
 unsigned hats[7],n=mem_read8(c,CCOUNTS+4*4u);observed_hat_count=n;assert(n<=6);
 for(unsigned i=0;i<n;i++)hats[i]=i;
 if(c->gpr[3]==4){assert(n<7);hats[n++]=c->gpr[4];}
 for(unsigned i=0;i<n;i++)assert(hats[i]<6);
 observed_hat_kind=c->gpr[3];observed_hat_color=c->gpr[4];
}
void func_80068914(Context *c){
 uint32_t fp=mem_read32(c,c->gpr[3]+0x2c),arg=c->gpr[4];unsigned kind=mem_read32(c,arg),player=mem_read8(c,arg+4),color=mem_read8(c,0x80453080u+player*0xe90u+0x44);
 mem_write32(c,fp+4,kind);mem_write8(c,fp+0x619,color<counts[kind]?color:0);
}
void func_801695BC(Context *c){
 unsigned external=c->gpr[3],n=mem_read8(c,CCOUNTS+external*4u);observed_hat_count=n;observed_hat_color=c->gpr[5];assert(n<=6);if(c->gpr[4]==external)assert(c->gpr[5]<n);
}
static unsigned preload_count;
void func_80087574(Context *c){
 unsigned kind=c->gpr[3];preload_count=mem_read8(c,CTABLE+kind*8u+4);
 assert(preload_count==original_count[kind]);
 for(unsigned i=0;i<preload_count;i++)assert(mapping[kind][i]<0);
}
#define STUB(a) void func_##a(Context *c){(void)c;}
STUB(80085820) STUB(800858E4) STUB(80168B34) STUB(8025D5AC)
STUB(80080144) STUB(8009DC54) STUB(8014A37C) STUB(8014A7F4)
STUB(800EF040) STUB(800EF0E4) STUB(800EF35C) STUB(8011B51C)
STUB(80149EAC) STUB(800F0FC0) STUB(800F10D4) STUB(800F11F0)
STUB(800F130C) STUB(800F14B4) STUB(8013C360)
static void dol(Context *c){
 FILE *f=fopen("data/GALE01/sys/main.dol","rb");assert(f);unsigned char h[256];assert(fread(h,1,256,f)==256);
 for(int i=0;i<18;i++){unsigned off=be32(h+i*4),addr=be32(h+0x48+i*4),size=be32(h+0x90+i*4);if(!size)continue;assert(valid(addr,size));fseek(f,off,SEEK_SET);assert(fread(c->ram+(addr&0x1ffffffu),1,size,f)==size);}fclose(f);
}
static const char *first="1111111111111111111111111111111111111111111111111111111111111111";
static const char *second="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static void file_hash(const char *path,char out[65]){FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);unsigned n=ftell(f);rewind(f);unsigned char *b=malloc(n);assert(fread(b,1,n,f)==n);fclose(f);assert(sha256_bytes(b,n,out));free(b);}
static void registry(int bad){
 FILE *f=fopen("build/tests/costumes/registry.tsv","wb");assert(f);fputs(bad==3?"MELEE_COSTUMES\t2\r\n":"MELEE_COSTUMES\t2\n",f);
 const char *colors[]={"Nr","Re","Bu","Bk"};
 for(int i=0;i<4;i++){char path[256],hash[65];snprintf(path,sizeof path,"data/GALE01/files/PlLk%s.dat",colors[i]);file_hash(path,hash);if(bad==2&&i==0)hash[0]=hash[0]=='0'?'1':'0';fprintf(f,"C\ttest-link\t%s\t1\t6\t6\tLk\tcolor-%d\tCostume %d\tdata/GALE01/files/PlLk%s.dat\t%d\t%s\n",first,i,i,colors[i],i,hash);}
 char hash[65];file_hash("data/GALE01/files/PlLkRe.dat",hash);fprintf(f,"C\tdisabled-link\t%s\t0\t6\t6\tLk\tred\tRed\tdata/GALE01/files/PlLkRe.dat\t1\t%s\n",second,hash);
 if(bad==1)fputs("broken record\n",f);fclose(f);
}
static void version_registry(void){
 FILE *f=fopen("build/tests/costumes/registry.tsv","wb");assert(f);fputs("MELEE_COSTUMES\t2\n",f);
 const char *versions[]={first,second},*colors[]={"Nr","Re"};
 for(int i=0;i<2;i++){char path[256],hash[65];snprintf(path,sizeof path,"data/GALE01/files/PlLk%s.dat",colors[i]);file_hash(path,hash);fprintf(f,"C\tsame-link-id\t%s\t%d\t6\t6\tLk\tone-color\tVersion %d\t%s\t%d\t%s\n",versions[i],!i,i,path,i,hash);}
 fclose(f);
}
static void kirby_registry(void){
 FILE *f=fopen("build/tests/costumes/registry.tsv","wb");assert(f);fputs("MELEE_COSTUMES\t2\n",f);
 const char *colors[]={"Bu","Wh"};const int bases[]={2,5};
 for(int i=0;i<2;i++){char path[256],hash[65];snprintf(path,sizeof path,"data/GALE01/files/PlKb%s.dat",colors[i]);file_hash(path,hash);fprintf(f,"C\ttest-kirby\t%s\t1\t4\t4\tKb\tcolor-%d\tKirby %d\t%s\t%d\t%s\n",first,i,i,path,bases[i],hash);}
 fclose(f);
}
static void paired_registry(void){
 FILE *f=fopen("build/tests/costumes/registry.tsv","wb");assert(f);fputs("MELEE_COSTUMES\t2\n",f);
 const char *names[]={"popo","sheik","zelda"},*prefixes[]={"Pp","Sk","Zd"},*files[]={"PlPpGr.dat","PlSkWh.dat","PlZdBu.dat"};
 const int external[]={14,19,18},internal[]={10,7,19},base[]={1,4,2};const char *hashes[]={first,second,"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"};
 for(int i=0;i<3;i++){char path[256],hash[65];snprintf(path,sizeof path,"data/GALE01/files/%s",files[i]);file_hash(path,hash);fprintf(f,"C\ttest-%s\t%s\t1\t%d\t%d\t%s\tadditional\tAdditional\t%s\t%d\t%s\n",names[i],hashes[i],external[i],internal[i],prefixes[i],path,base[i],hash);}
 fclose(f);
}
static void invalid_registry(int mode){
 FILE *f=fopen("build/tests/costumes/registry.tsv","wb");assert(f);fputs("MELEE_COSTUMES\t2\n",f);char hash[65];file_hash("data/GALE01/files/PlLkNr.dat",hash);
 int external=mode==1?26:6,internal=mode==2?7:6;
 const char *id=mode==3?"other-link":"test-link";
 fprintf(f,"C\ttest-link\t%s\t1\t%d\t%d\tLk\tduplicate\tAdditional\tdata/GALE01/files/PlLkNr.dat\t0\t%s\n",mode==4?second:first,external,internal,hash);
 if(mode==0||mode==3||mode==4)fprintf(f,"C\t%s\t%s\t1\t6\t6\tLk\tduplicate\tAdditional\tdata/GALE01/files/PlLkNr.dat\t0\t%s\n",id,first,hash);
 fclose(f);
}
static void texture_file(const char *path,unsigned width,unsigned height){
 unsigned payload=((width+7)/8)*((height+7)/8)*32;
 FILE *f=fopen(path,"wb");assert(f);fwrite("YAMPPTEX",1,8,f);
 unsigned values[]={width,height,14,payload};for(int i=0;i<4;i++){unsigned char b[]={values[i]>>24,values[i]>>16,values[i]>>8,values[i]};fwrite(b,1,4,f);}
 for(unsigned i=0;i<payload;i++)fputc(i&255,f);fclose(f);
}
static void art_registry(int malformed){
 const char *portrait="build/tests/costumes/portrait.gxt",*stock="build/tests/costumes/stock.gxt";
 texture_file(portrait,136,188);texture_file(stock,malformed==1?32:24,24);
 char dat[65],ph[65],sh[65];file_hash("data/GALE01/files/PlLkNr.dat",dat);file_hash(portrait,ph);file_hash(stock,sh);
 if(malformed==2)sh[0]=sh[0]=='0'?'1':'0';
 FILE *f=fopen("build/tests/costumes/registry.tsv","wb");assert(f);fputs("MELEE_COSTUMES\t3\r\n",f);
 fprintf(f,"C\ttest-link\t%s\t1\t6\t6\tLk\tpainted\tPainted\tdata/GALE01/files/PlLkNr.dat\t0\t%s\t%s\t%s\t%s\t%s\r\n",first,dat,portrait,ph,stock,sh);fclose(f);
}
int main(void){
 Context c={0};c.ram=calloc(1,24<<20);c.ram_size=24<<20;assert(c.ram);g_cur_ctx=&c;dol(&c);
 _mkdir("build/tests");_mkdir("build/tests/costumes");registry(0);_putenv("MELEE_COSTUME_REGISTRY=build/tests/costumes/registry.tsv");
 unsigned old[CKINDS];for(int i=0;i<CKINDS;i++)old[i]=mem_read8(&c,CTABLE+i*8+4);
 assert(old[6]==5);costumes_initialize(&c,0x81700000,0x20000);assert(costumes_active_ready()==1&&nslots==4&&counts[6]==9);
 for(int k=0;k<CKINDS;k++)if(k!=6)assert(counts[k]==(int)old[k]);
 for(int i=0;i<5;i++)assert(base_color(6,i)==i);for(int i=5;i<9;i++)assert(base_color(6,i)==i-5);
 /* Static art is optional; vanilla and v2 packs use original portraits. */
 const unsigned char *art=NULL;unsigned art_size=0;
 assert(!costumes_slot_art(6,5,0,&art,&art_size)&&!art&&!art_size);
 assert(!costumes_slot_art(6,0,1,&art,&art_size));
 unsigned arena_size=0;uint32_t arena=costumes_art_arena(&arena_size);
 assert(arena>=table_area+CKINDS*CCOLORS*36u&&!(arena&31u)&&arena_size>=54272&&arena+arena_size==table_area+0x20000);
 /* These two indexed native helpers see the stock variant, while persistent
  * player/fighter state retains additive slot 7 for network synchronization. */
 uint32_t fp=0x80A00000,go=0x80A03000;mem_write32(&c,go+0x2c,fp);mem_write32(&c,fp+4,6);mem_write8(&c,fp+0x619,7);
 c.gpr[3]=go;visibility(&c);assert(observed_color==2&&mem_read8(&c,fp+0x619)==7);
 c.gpr[3]=fp;texture_anim(&c);assert(observed_color==2&&mem_read8(&c,fp+0x619)==7);
 mem_write32(&c,table(6)+7*24+4,0x81234560);mem_write32(&c,table(6)+2*24+4,0x81123450);
 c.gpr[3]=go;bind_texture_anim(&c);assert(observed_color==2&&observed_mat==0x81234560&&mem_read32(&c,table(6)+2*24+4)==0x81123450&&mem_read8(&c,fp+0x619)==7);
 /* Adventure's stock-enemy preloader must not allocate unused installed
  * costumes, mutate the expanded count permanently, or remove chosen roots. */
 assert(costumes_lookup(0x80087574)==preload_enemy_colors);
 c.gpr[3]=6;preload_enemy_colors(&c);assert(preload_count==5&&counts[6]==9&&mem_read8(&c,CTABLE+6*8+4)==9);
 assert(mem_read32(&c,table(6)+7*24+4)==0x81234560);
 /* Restoring all guest RAM restores loaded roots AND allocator state. */
 mem_write32(&c,0x80010000,0x81000000);unsigned char *before=malloc(24<<20),*after=malloc(24<<20);assert(before&&after);memcpy(before,c.ram,24<<20);
 c.gpr[3]=6;c.gpr[4]=5;load_normal(&c);uint32_t joint=mem_read32(&c,table(6)+5*24),archive=mem_read32(&c,table(6)+5*24+0x14);assert(valid(joint,0x40)&&valid(archive,0x44));memcpy(after,c.ram,24<<20);
 memcpy(c.ram,before,24<<20);c.gpr[3]=6;c.gpr[4]=5;load_normal(&c);assert(!memcmp(c.ram,after,24<<20));
 /* Missing, malformed and noncanonical exact sets fail without changing the
  * previous slots. Disabled local packages can be selected by a room hash. */
 costumes_set_room_mods("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");assert(!costumes_active_ready());costumes_poll(&c);assert(costumes_active_ready()==-1&&counts[6]==9);
 char invalid[140];snprintf(invalid,sizeof invalid,"%s,",first);costumes_set_room_mods(invalid);costumes_poll(&c);assert(costumes_active_ready()==-1&&counts[6]==9);
 costumes_set_room_mods(second);session=1;costumes_poll(&c);assert(!costumes_active_ready()&&counts[6]==9);session=0;costumes_poll(&c);assert(costumes_active_ready()==1&&counts[6]==6&&base_color(6,5)==1);
 char both[140];snprintf(both,sizeof both,"%s,%s",first,second);costumes_set_room_mods(both);costumes_poll(&c);assert(costumes_active_ready()==1&&counts[6]==10&&nslots==5);assert(mapping[6][5]!=mapping[6][9]&&base_color(6,9)==1);
 costumes_set_room_mods("");costumes_poll(&c);assert(costumes_active_ready()==1&&counts[6]==5&&nslots==0);
 registry(2);costumes_set_room_mods(NULL);costumes_poll(&c);assert(costumes_active_ready()==-1&&counts[6]==5);
 registry(1);costumes_set_room_mods(NULL);costumes_poll(&c);assert(costumes_active_ready()==-1&&counts[6]==5);
 registry(3);costumes_set_room_mods(NULL);costumes_poll(&c);assert(costumes_active_ready()==1&&counts[6]==9);
 /* Additional Kirby slots must never expand the six-color copy-hat tables.
  * Check direct loading, one-color and all-color prefetch, and the native
  * seven-entry preload list while preserving the actual eight body slots. */
 kirby_registry();costumes_set_room_mods(NULL);costumes_poll(&c);assert(costumes_active_ready()==1&&counts[4]==8&&counts[6]==5);
 assert(original_count[4]==6&&base_color(4,6)==2&&base_color(4,7)==5);
 assert(costumes_lookup(0x800EEC34)==kirby_hat_prefetch&&costumes_lookup(0x800EED50)==kirby_hat_load&&costumes_lookup(0x80169C54)==kirby_hat_preload_list);
 for(unsigned color=0;color<8;color++){
  unsigned expected=color<6?color:color==6?2:5;
  c.gpr[3]=3;c.gpr[4]=color;c.gpr[5]=8;kirby_hat_prefetch(&c);assert(observed_hat_kind==3&&observed_hat_color==expected&&observed_hat_count==6);
  c.gpr[3]=15;c.gpr[4]=color;kirby_hat_load(&c);assert(observed_hat_kind==15&&observed_hat_color==expected);
 }
 c.gpr[3]=16;c.gpr[4]=0xff;c.gpr[5]=8;kirby_hat_prefetch(&c);assert(observed_hat_kind==16&&observed_hat_color==0xff&&observed_hat_count==6);
 c.gpr[3]=3;c.gpr[4]=64;kirby_hat_load(&c);assert(observed_hat_color==0);
 c.gpr[3]=4;c.gpr[4]=7;kirby_hat_preload_list(&c);assert(observed_hat_count==6&&observed_hat_color==5&&mem_read8(&c,CCOUNTS+4*4u)==8&&counts[4]==8);
 c.gpr[3]=6;c.gpr[4]=8;kirby_hat_preload_list(&c);assert(observed_hat_kind==6&&observed_hat_color==8&&mem_read8(&c,CCOUNTS+4*4u)==8);
 /* Stock-only scratch shuffles cannot index their six-entry stack array
  * with an additive excluded color, and the CSS count must be restored. */
 c.gpr[3]=4;c.gpr[4]=4;c.gpr[5]=7;classic_color_shuffle(&c);assert(observed_hat_count==6&&observed_hat_color==5&&mem_read8(&c,CCOUNTS+4*4u)==8);
 /* No activation may touch the results filename immediately after the 26
  * external records. Distinct transformed packs must not alias ordinal five. */
 mem_write8(&c,CCOUNTS+CEXTERNAL*4u,0xa5);paired_registry();costumes_set_room_mods(NULL);costumes_poll(&c);
 assert(costumes_active_ready()==1&&counts[10]==5&&counts[7]==6&&counts[19]==6&&mem_read8(&c,CCOUNTS+CEXTERNAL*4u)==0xa5);
 uint32_t arg=0x80A04000,player=0x80453080u;mem_write8(&c,arg+4,0);
 const unsigned externals[]={18,18,19,19,14,14,18},kinds[]={19,7,7,19,10,11,7},selected[]={5,5,5,5,4,4,3},expected[]={5,2,5,4,4,1,3};
 for(unsigned i=0;i<7;i++){
  mem_write32(&c,player+4,externals[i]);mem_write8(&c,player+0x44,selected[i]);mem_write32(&c,arg,kinds[i]);c.gpr[3]=go;c.gpr[4]=arg;initialize_fighter(&c);
  assert(mem_read8(&c,fp+0x619)==expected[i]&&mem_read8(&c,player+0x44)==selected[i]);
 }
 for(int mode=0;mode<5;mode++){invalid_registry(mode);costumes_set_room_mods(NULL);costumes_poll(&c);assert(costumes_active_ready()==-1&&nslots==3&&counts[19]==6&&mem_read8(&c,CCOUNTS+CEXTERNAL*4u)==0xa5);}
 /* Native portrait/icon payloads are exact-size, immutable and hash checked.
  * A declared malformed or changed texture rejects the whole activation. */
 art_registry(0);costumes_set_room_mods(NULL);costumes_poll(&c);assert(costumes_active_ready()==1&&counts[6]==6);
 assert(costumes_slot_art(6,5,0,&art,&art_size)&&art_size==13080&&!memcmp(art,"YAMPPTEX",8));
 assert(costumes_slot_art(6,5,1,&art,&art_size)&&art_size==312&&be32(art+8)==24);
 const unsigned char *immutable=art;unsigned char saved_icon[312];memcpy(saved_icon,art,312);
 texture_file("build/tests/costumes/stock.gxt",32,24);
 assert(costumes_slot_art(6,5,1,&art,&art_size)&&art==immutable&&!memcmp(art,saved_icon,312));
 memcpy(before,c.ram,24<<20);
 for(int mode=1;mode<=2;mode++){
  art_registry(mode);costumes_set_room_mods(NULL);costumes_poll(&c);
  assert(costumes_active_ready()==-1&&counts[6]==6&&!memcmp(before,c.ram,24<<20));
  assert(costumes_slot_art(6,5,1,&art,&art_size)&&art==immutable&&!memcmp(art,saved_icon,312));
 }
 assert(!costumes_slot_art(6,5,2,&art,&art_size)&&!art&&!art_size);
 /* Two immutable versions of the same local package may coexist as rows.
  * A room selects precisely one hash; returning offline restores enabled rows. */
 version_registry();costumes_set_room_mods(NULL);costumes_poll(&c);assert(costumes_active_ready()==1&&nslots==1&&base_color(6,5)==0);
 costumes_set_room_mods(second);costumes_poll(&c);assert(costumes_active_ready()==1&&nslots==1&&base_color(6,5)==1&&!strcmp(slots[0].hash,second));
 costumes_set_room_mods(first);costumes_poll(&c);assert(costumes_active_ready()==1&&nslots==1&&base_color(6,5)==0&&!strcmp(slots[0].hash,first));
 costumes_set_room_mods(NULL);costumes_poll(&c);assert(costumes_active_ready()==1&&nslots==1&&base_color(6,5)==0);
 memcpy(before,c.ram,24<<20);costumes_set_room_mods(both);costumes_poll(&c);assert(costumes_active_ready()==-1&&nslots==1&&!memcmp(before,c.ram,24<<20));
 free(before);free(after);free_rows(slots,nslots);free(c.ram);
 puts("Costume production-loader checks passed: stock preservation, additive counts, native color indices, Kirby hats, partner colors, stock table bounds, duplicate/order rejection, archive relocation, RAM replay, immutable portrait/icon validation, exact sets and epoch guards.");return 0;
}
