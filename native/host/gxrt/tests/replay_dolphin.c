/* Replay actual recorded Melee GPU commands through the native Aurora path. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "gx_translate.h"
int aurora_link_init(void*,uint32_t,unsigned,unsigned);
void aurora_link_frame_begin(void),aurora_link_frame_end(void),aurora_link_synchronize(void);
int aurora_link_call_dl(const void*,uint32_t);
int aurora_link_readback(unsigned char*,uint32_t*,uint32_t*,uint32_t);
/* This standalone GPU fixture has no menus or network worker. */
int netplay_room_name_input(int unused,const char* name) {(void)unused;(void)name;return 0;}
void netplay_profile_changed(const char* name,const unsigned char* data,unsigned size,const char* type) {(void)name;(void)data;(void)size;(void)type;}
void mods_host_menu(void* module) {(void)module;}
static uint32_t read32(FILE* f) { uint32_t v; if(fread(&v,4,1,f)!=1) ExitProcess(2);return v; }
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    FILE* f=fopen(argv[1],"rb");
    if(!f || read32(f)!=0x5258474d) return 2;
    uint32_t events=read32(f);
    uint8_t* ram=calloc(1,0x1800000);
    uint8_t* output=malloc(32u<<20);
    GxTranslateContext* context=gx_translate_create();
    uint32_t total=0;
    if(!aurora_link_init(ram,0x1800000,640,480)) return 3;
    if (getenv("MELEE_REPLAY_EARLY_MEMORY")) {
        long begin=ftell(f);
        for(uint32_t i=0;i<events;i++) {
            uint32_t kind=read32(f),address=read32(f),size=read32(f);
            if(kind==1) fread(ram+address,1,size,f);
            else fseek(f,size,SEEK_CUR);
        }
        fseek(f,begin,SEEK_SET);
    }
    /* Let a fresh surface complete initial resize before recording. */
    for(unsigned warm=0;warm<3;warm++) {aurora_link_frame_begin();aurora_link_frame_end();aurora_link_synchronize();}
    aurora_link_frame_begin();
    gx_translate_begin_frame(context);
    for(uint32_t i=0;i<events;i++) {
        uint32_t kind=read32(f),addr=read32(f),size=read32(f);
        uint8_t* data=malloc(size?size:1);
        if(size && fread(data,1,size,f)!=size) return 2;
        if(kind==1) {
            if(addr+size>0x1800000) return 2;
            memcpy(ram+addr,data,size);
            gx_translate_invalidate_ram(context,addr,size);
        } else if(kind==2 && size) {
            uint32_t n=gx_translate_stream(context,data,size,ram,0x1800000,output+total,(32u<<20)-total);
            if(!n) {
                fprintf(stderr,"FIFO translation failed event=%u error=%d offset=%u opcode=%02X\n",i,gx_translate_last_error(context),gx_translate_last_error_offset(context),gx_translate_last_error_opcode(context));
                return 4;
            }
            total+=n;
        } else if(kind==3) {
            if (!aurora_link_call_dl(output,total)) {fprintf(stderr,"GPU frame rejected bytes=%u\n",total);return 6;}
            const GxTranslateStats* stats=gx_translate_stats(context);
            fprintf(stderr,"Replay: draws=%llu textures=%llu palettes=%llu bytes=%u\n",(unsigned long long)stats->draws,(unsigned long long)stats->texture_objects_emitted,(unsigned long long)stats->tluts_emitted,total); total=0;
            aurora_link_frame_end();aurora_link_synchronize();
            if(i+1<events) { gx_translate_frame_drained(context);aurora_link_frame_begin();gx_translate_begin_frame(context); }
        }
        free(data);
    }
    fclose(f);
    uint8_t* rgb=malloc(1280*960*3);uint32_t w=0,h=0;
    if(!aurora_link_readback(rgb,&w,&h,1280*960*3)) return 5;
    f=fopen(argv[2],"wb");if(!f)return 5;
    fprintf(f,"P6\n%u %u\n255\n",w,h);fwrite(rgb,3,w*h,f);fclose(f);
    printf("Recorded Melee frame replayed %ux%u to %s\n",w,h,argv[2]);fflush(NULL);
    TerminateProcess(GetCurrentProcess(),0);return 0;
}
