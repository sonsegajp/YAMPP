/* Extend the original CSS HSD scene, preserving its cursor/token callbacks.
 * The engine's 25-entry table is temporarily mapped to a custom tile during
 * one player's callback; package identity is kept separately per player. */
extern void func_802602A0(Context*);
extern void func_80262648(Context*);
extern void func_80370E44(Context*);
extern void func_803717A8(Context*);
extern void func_803732E8(Context*);
#define CSS_ICONS 0x803F0B24u
#define CSS_DOORS 0x803F0DFCu
typedef struct {int fighter,base,joint;float l,r,u,d;uint32_t jobj;} CssTile;
static CssTile css_tiles[MAX_MODS];static int css_tiles_count,css_installed,css_custom[4]={-1,-1,-1,-1};
static float css_float(Context* ctx,uint32_t a){uint32_t n=mem_read32(ctx,a);float f;memcpy(&f,&n,4);return f;}
static void css_write_float(Context* ctx,uint32_t a,float f){uint32_t n;memcpy(&n,&f,4);mem_write32(ctx,a,n);}
static void css_reset(void){css_installed=css_tiles_count=0;for(int p=0;p<4;p++)css_custom[p]=-1;}
static int css_base(Context* ctx,int external){if(external==19)external=18;for(int i=0;i<25;i++)if(mem_read8(ctx,CSS_ICONS+i*28+1)==external)return i;return -1;}
static void css_walk(Context* ctx,uint32_t j,uint32_t* joints,int* count){
 for(int guard=0;valid(j,0x80)&&*count<256&&guard<256;guard++,j=mem_read32(ctx,j+8)){joints[(*count)++]=j;css_walk(ctx,mem_read32(ctx,j+0x10),joints,count);}
}
static void css_move(Context* ctx,uint32_t j,float x,float y,float sx,float sy){
 css_write_float(ctx,j+0x38,x);css_write_float(ctx,j+0x3c,y);css_write_float(ctx,j+0x2c,sx);css_write_float(ctx,j+0x30,sy);
 Context saved=*ctx;ctx->gpr[3]=j;func_803732E8(ctx);*ctx=saved;
}
static void css_install(Context* ctx){
 if(css_installed||mem_read8(ctx,0x804D6CF5)==1)return;
 uint32_t root=mem_read32(ctx,0x804D6CC0);if(!valid(root,0x80))return;
 uint32_t joints[256];int count=0;css_walk(ctx,root,joints,&count);if(count!=173)return;
 css_installed=1;Context saved=*ctx;
 for(int i=0;i<nf&&count<256;i++)if(fighters[i].path[0]!='-'){
  int base=css_base(ctx,fighters[i].external);if(base<0)continue;
  char path[320];snprintf(path,sizeof path,"%s/../css-portrait.dat",fighters[i].path);
  uint32_t desc=load_archive(ctx,path);if(!desc)continue;
  ctx->gpr[3]=desc;func_80370E44(ctx);uint32_t j=ctx->gpr[3];if(!valid(j,0x80))continue;
  ctx->gpr[3]=root;ctx->gpr[4]=j;func_803717A8(ctx);
  CssTile* t=&css_tiles[css_tiles_count++];t->fighter=i;t->base=base;t->joint=count++;t->jobj=j;
 }
 if(css_tiles_count<=2){
  for(int i=0;i<css_tiles_count;i++){
   CssTile* t=&css_tiles[i];t->l=i?24.4f:-30.0f;t->r=i?30.2f:-24.4f;t->u=6;t->d=-1;
   css_move(ctx,t->jobj,t->l,5.8f,.8f,1.25f);
  }
 }else{
  // Keep all existing joints and their indices; fit added rows above the doors.
  int rows=(25+css_tiles_count+8)/9;float h=21.0f/rows,w=60.2f/9;
  for(int i=0;i<25+css_tiles_count;i++){
   float l=-30.0f+(i%9)*w,u=20.0f-(i/9)*h;uint32_t j;
   if(i<25){uint32_t icon=CSS_ICONS+i*28;j=joints[mem_read8(ctx,icon+4)];
    css_write_float(ctx,icon+0xc,l);css_write_float(ctx,icon+0x10,l+w);css_write_float(ctx,icon+0x14,u);css_write_float(ctx,icon+0x18,u-h);
   }else{CssTile* t=&css_tiles[i-25];t->l=l;t->r=l+w;t->u=u;t->d=u-h;j=t->jobj;}
   float px=0,py=0,psx=1,psy=1;uint32_t parent=mem_read32(ctx,j+0xc);
   // CSS portrait parents have translation/scale only.
   for(int depth=0;parent&&parent!=root&&depth<8;depth++,parent=mem_read32(ctx,parent+0xc)){
    px=css_float(ctx,parent+0x38)+css_float(ctx,parent+0x2c)*px;py=css_float(ctx,parent+0x3c)+css_float(ctx,parent+0x30)*py;
    psx*=css_float(ctx,parent+0x2c);psy*=css_float(ctx,parent+0x30);
   }
   css_move(ctx,j,(l-px)/psx,(u+.2f-py)/psy,(w/7)/psx,(1.25f*h/7)/psy);
  }
 }
 // Restore a previous custom selection after results, without changing the
 // original player door or choosing a fighter for a newly opened slot.
 for(int p=0;p<4;p++)for(int i=0;i<css_tiles_count;i++){
  CssTile* t=&css_tiles[i];uint32_t door=CSS_DOORS+p*0x24,chip=mem_read32(ctx,0x804A0BD0u+p*4);
  if(chosen[p]==t->fighter&&mem_read8(ctx,door+0xe)==t->base&&mem_read8(ctx,door+0xb)!=3&&valid(chip,0x18)){
   css_custom[p]=i;css_write_float(ctx,chip+8,(t->l+t->r)*.5f);css_write_float(ctx,chip+12,(t->u+t->d)*.5f);
   css_write_float(ctx,chip+16,(t->l+t->r)*.5f);css_write_float(ctx,chip+20,(t->u+t->d)*.5f);
  }
 }
 *ctx=saved;fprintf(stderr,"[mods] vanilla CSS: %d additional portraits, %d original joints retained\n",css_tiles_count,173);
}
static int css_hit(Context* ctx,uint32_t chip){
 if(!valid(chip,0x18))return -1;float x=css_float(ctx,chip+8),y=css_float(ctx,chip+12);
 for(int i=0;i<css_tiles_count;i++){CssTile* t=&css_tiles[i];if(x>t->l&&x<t->r&&y<t->u&&y>t->d)return i;}return -1;
}
static uint32_t css_proxy(Context* ctx,int tile,unsigned char* backup){
 CssTile* t=&css_tiles[tile];uint32_t at=CSS_ICONS+t->base*28;
 memcpy(backup,ctx->ram+(at&0x1ffffff),28);
 mem_write8(ctx,at+2,2);mem_write8(ctx,at+4,t->joint);mem_write8(ctx,at+5,t->joint);
 css_write_float(ctx,at+12,t->l);css_write_float(ctx,at+16,t->r);css_write_float(ctx,at+20,t->u);css_write_float(ctx,at+24,t->d);return at;
}
static void css_sync(Context* ctx){
 for(int p=0;p<4;p++){
  uint32_t door=CSS_DOORS+p*0x24;int icon=mem_read8(ctx,door+0xe);
  colors[p]=mem_read8(ctx,door+0xd);types[p]=mem_read8(ctx,door+0xb);
  if(icon>=25||types[p]==3){chosen[p]=-1;css_custom[p]=-1;continue;}
  if(css_custom[p]>=0&&css_tiles[css_custom[p]].base==icon){chosen[p]=css_tiles[css_custom[p]].fighter;continue;}
  css_custom[p]=-1;int external=mem_read8(ctx,CSS_ICONS+icon*28+1);chosen[p]=-1;
  for(int i=0;i<nf;i++)if(fighters[i].path[0]=='-'&&fighters[i].external==external){chosen[p]=i;break;}
 }
}
static void css_cursor(Context* ctx){
 css_install(ctx);uint32_t cursor=mem_read32(ctx,ctx->gpr[3]+0x2c);int door=-1,tile=-1;
 if(valid(cursor,0x14)&&mem_read8(ctx,cursor+5)==1){door=mem_read8(ctx,cursor+6);if(door<4)tile=css_hit(ctx,mem_read32(ctx,0x804A0BD0u+door*4));}
 unsigned char backup[28];uint32_t at=0;int random_guard=-1;unsigned char random_state=0;
 if(tile>=0){at=css_proxy(ctx,tile,backup);
  // The vanilla all-unlocked random hot spot occupies the two new portrait gaps.
  for(int i=0;i<25;i++)if(i!=css_tiles[tile].base&&mem_read8(ctx,CSS_ICONS+i*28+2)>=2){random_guard=i;random_state=mem_read8(ctx,CSS_ICONS+i*28+2);mem_write8(ctx,CSS_ICONS+i*28+2,1);break;}
 }
 func_802602A0(ctx);
 if(at)memcpy(ctx->ram+(at&0x1ffffff),backup,28);
 if(random_guard>=0)mem_write8(ctx,CSS_ICONS+random_guard*28+2,random_state);
 if(door>=0&&door<4){int previous=css_custom[door];css_custom[door]=tile;
  if(previous!=tile)fprintf(stderr,"[mods] vanilla P%d hover=%s\n",door+1,tile>=0?fighters[css_tiles[tile].fighter].id:"original");
 }
 if(getenv("MELEE_TRACE_CSS")&&valid(cursor,0x14)&&mem_read8(ctx,cursor+4)==0){static int frame;if((frame++%60)==0){uint32_t chip=mem_read32(ctx,0x804A0BD0);fprintf(stderr,"[css] tick=%d cursor=%.2f,%.2f state=%u held=%u chip=%.2f,%.2f icon=%u custom=%d\n",frame,css_float(ctx,cursor+12),css_float(ctx,cursor+16),mem_read8(ctx,cursor+5),mem_read8(ctx,cursor+6),css_float(ctx,chip+8),css_float(ctx,chip+12),mem_read8(ctx,CSS_DOORS+14),css_custom[0]);}}
}
static void css_chip(Context* ctx){
 uint32_t chip=mem_read32(ctx,ctx->gpr[3]+0x2c);int port=valid(chip,0x18)?mem_read8(ctx,chip+4):4;
 unsigned char backup[28];uint32_t at=0;
 if(port<4&&css_custom[port]>=0)at=css_proxy(ctx,css_custom[port],backup);
 func_80262648(ctx);if(at)memcpy(ctx->ram+(at&0x1ffffff),backup,28);
}
