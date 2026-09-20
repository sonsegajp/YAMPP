/* Persistent raw GameCube memory card on EXI slot A.
 * Protocol/layout reference: Dolphin EXI_DeviceMemoryCard and GCMemcard.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "abi_recompcore.h"
#include "gxruntime/exi.h"
#include "gxruntime/interrupts.h"
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
static unsigned long long GetTickCount64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif
extern DolExi g_exi;
extern DolInterrupts g_irq;
static unsigned char* card;
static unsigned card_size,position,address,command,program_count;
static unsigned char program[128];
static int irq_enabled,irq_pending,dirty;
static char card_path[1024];
static unsigned long long last_flush;
#ifdef _WIN32
static CRITICAL_SECTION card_mutex;
#define CARD_LOCK_INIT() InitializeCriticalSection(&card_mutex)
#define CARD_LOCK()   EnterCriticalSection(&card_mutex)
#define CARD_UNLOCK() LeaveCriticalSection(&card_mutex)
#else
static pthread_mutex_t card_mutex = PTHREAD_MUTEX_INITIALIZER;
#define CARD_LOCK_INIT() ((void)0)
#define CARD_LOCK()   pthread_mutex_lock(&card_mutex)
#define CARD_UNLOCK() pthread_mutex_unlock(&card_mutex)
#endif
static int card_mutex_init;
#ifdef _WIN32
static HANDLE card_lock=INVALID_HANDLE_VALUE;
#else
static int card_lock_fd = -1;
#endif
static unsigned reads,writes,erases;
static void put16(unsigned char* p,unsigned n){p[0]=n>>8;p[1]=n;}
static void put32(unsigned char* p,unsigned n){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static void checksum(unsigned char* dst,const unsigned char* p,unsigned size){unsigned sum=0,inv=0;for(unsigned i=0;i<size;i+=2){unsigned n=(p[i]<<8)|p[i+1];sum+=n;inv+=(~n)&65535;}sum&=65535;inv&=65535;if(sum==65535)sum=0;if(inv==65535)inv=0;put16(dst,sum);put16(dst+2,inv);}
static void format_card(void){
 memset(card,255,card_size);unsigned char* h=card;uint64_t seed=(uint64_t)time(NULL)*40500000u,random=seed;
 for(unsigned i=0;i<12;i++){random=(random*0x41c64e6dull+0x3039ull)>>16;h[i]=(unsigned char)random;random=((random*0x41c64e6dull+0x3039ull)>>16)&0x7fff;}
 for(unsigned i=0;i<8;i++)h[12+i]=(unsigned char)(seed>>(56-8*i));
 memset(h+20,0,14);put16(h+34,card_size/131072u);put16(h+36,0);checksum(h+508,h,508);
 for(unsigned b=1;b<=2;b++){unsigned char* d=card+b*8192u;put16(d+8186,0);checksum(d+8188,d,8188);}
 for(unsigned b=3;b<=4;b++){unsigned char* d=card+b*8192u;memset(d,0,8192);put16(d+6,card_size/8192-5);put16(d+8,4);checksum(d,d+4,8188);}
 dirty=1;
}
void frontend_card_flush(void){
 if(!card||!dirty)return;
 if(card_mutex_init)CARD_LOCK();
 char temporary[1050];snprintf(temporary,sizeof temporary,"%s.tmp",card_path);
 FILE* f=fopen(temporary,"wb");if(!f){fprintf(stderr,"[card] cannot write %s\n",temporary);if(card_mutex_init)CARD_UNLOCK();return;}
 int ok=fwrite(card,1,card_size,f)==card_size;if(fflush(f))ok=0;
#ifdef _WIN32
 intptr_t handle=_get_osfhandle(_fileno(f));if(handle!=-1&&!FlushFileBuffers((HANDLE)handle))ok=0;
#else
 if(fsync(fileno(f))!=0)ok=0;
#endif
 if(fclose(f))ok=0;
#ifdef _WIN32
 if(ok&&MoveFileExA(temporary,card_path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){dirty=0;last_flush=GetTickCount64();fprintf(stderr,"[card] persisted %u bytes reads=%u writes=%u erases=%u\n",card_size,reads,writes,erases);}else fprintf(stderr,"[card] save commit failed: %lu\n",GetLastError());
#else
 if(ok&&rename(temporary,card_path)==0){
  int dir_synced=0;
  char dir[1050];snprintf(dir,sizeof dir,"%s",card_path);
  char* slash=strrchr(dir,'/');
  if(slash&&slash!=dir){*slash=0;int fd=open(dir,O_RDONLY);if(fd>=0){dir_synced=fsync(fd)==0;close(fd);}}
  else if(!slash){int fd=open(".",O_RDONLY);if(fd>=0){dir_synced=fsync(fd)==0;close(fd);}}
  else dir_synced=1;
  if(dir_synced){dirty=0;last_flush=GetTickCount64();fprintf(stderr,"[card] persisted %u bytes reads=%u writes=%u erases=%u\n",card_size,reads,writes,erases);}
  else fprintf(stderr,"[card] rename ok but directory fsync failed; will retry\n");
 }else fprintf(stderr,"[card] save commit failed: %d\n",(int)errno);
#endif
 if(card_mutex_init)CARD_UNLOCK();
}
void frontend_card_poll(void){if(dirty&&GetTickCount64()-last_flush>=1000)frontend_card_flush();}
void frontend_card_init(void){
 const char* path=getenv("MELEE_MEMORY_CARD");if(!path||!*path)return;
 if(strlen(path)>=sizeof card_path){fprintf(stderr,"[card] path too long\n");return;}
 snprintf(card_path,sizeof card_path,"%s",path);
 char lock_path[1050];snprintf(lock_path,sizeof lock_path,"%s.lock",path);
#ifdef _WIN32
 card_lock=CreateFileA(lock_path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_DELETE_ON_CLOSE,NULL);
 if(card_lock==INVALID_HANDLE_VALUE){fprintf(stderr,"[card] memory card is already in use or not writable: %s\n",path);return;}
#else
 card_lock_fd=open(lock_path,O_CREAT|O_RDWR,0644);
 if(card_lock_fd<0||flock(card_lock_fd,LOCK_EX|LOCK_NB)!=0){fprintf(stderr,"[card] memory card is already in use or not writable: %s\n",path);if(card_lock_fd>=0){close(card_lock_fd);card_lock_fd=-1;}return;}
#endif
 FILE* f=fopen(path,"rb");card_size=16*131072u;
 if(f){fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);if(size<524288||size>16777216||(size&(size-1))){fprintf(stderr,"[card] invalid raw card size: %ld\n",size);fclose(f);return;}card_size=(unsigned)size;}
 card=(unsigned char*)malloc(card_size);if(!card){if(f)fclose(f);return;}
 if(f){size_t size=fread(card,1,card_size,f);fclose(f);if(size!=card_size){free(card);card=NULL;return;}}
 else{format_card();frontend_card_flush();if(dirty){free(card);card=NULL;return;}}
 CARD_LOCK_INIT();card_mutex_init=1;
 dol_exi_set_device_present(&g_exi,0,true,true);fprintf(stderr,"[card] slot A: %s (%u Mbit)\n",card_path,card_size/131072u);
}
void frontend_card_sram(unsigned char* sram){
 if(!card)return;uint64_t random=0;for(unsigned i=12;i<20;i++)random=(random<<8)|card[i];unsigned sum=0;
 for(unsigned i=0;i<12;i++){random=(random*0x41c64e6dull+0x3039ull)>>16;unsigned char value=card[i]-(unsigned char)random;sram[20+i]=value;sum+=value;random=((random*0x41c64e6dull+0x3039ull)>>16)&0x7fff;}
 sram[58]=(unsigned char)(sum^255);
}
static void update_irq(void){dol_exi_set_device_interrupt(&g_exi,0,irq_enabled&&irq_pending);dol_interrupts_set_source(&g_irq,DOL_PI_CAUSE_EXI,dol_exi_interrupt_pending(&g_exi));}
void frontend_card_select(unsigned old_cs,unsigned new_cs){
 if(!card)return;
 if(old_cs==1&&new_cs!=1){
  if(card_mutex_init)CARD_LOCK();
  if(command==0xf1&&position>2){memset(card+(address&(card_size-1)&~8191u),255,8192);dirty=1;erases++;irq_pending=1;}
  if(command==0xf2&&position>=5){for(unsigned i=0;i<program_count;i++){card[address&(card_size-1)]=program[i&127];address=(address&~511u)|((address+1)&511);}if(program_count){dirty=1;writes++;}irq_pending=1;}
  if(card_mutex_init)CARD_UNLOCK();
  update_irq();
 }
 if(new_cs==1&&old_cs!=1){position=0;program_count=0;}
}
static unsigned char transfer_byte(unsigned char byte){
 unsigned pos=position++;if(pos==0){command=byte;if(command==0x89){irq_pending=0;update_irq();}return 255;}
 switch(command){
 case 0x00:return pos==1?0x80:(unsigned char)((card_size/131072u)>>(24-((pos-2)&3)*8));
 case 0x83:return 0x41;
 case 0x85:return pos==1||!(pos&1)?0xc2:0x21;
 case 0x81:if(pos==1){irq_enabled=byte;update_irq();}return 255;
 case 0x52:case 0xf2:
  if(pos==1)address=(unsigned)byte<<17;else if(pos==2)address|=(unsigned)byte<<9;else if(pos==3)address|=(byte&3)<<7;else if(pos==4)address|=byte&127;
  if(command==0xf2){if(pos>=5){program[(pos-5)&127]=byte;program_count=pos-4;}return 255;}
  byte=card[address&(card_size-1)];if(pos>=9)address=(address&~511u)|((address+1)&511);return byte;
 case 0xf1:if(pos==1)address=(unsigned)byte<<17;else if(pos==2)address|=(unsigned)byte<<9;return 255;
 default:return 255;
 }
}
bool frontend_card_transfer(DolExiTransfer* t){
 if(!card||t->channel!=0||t->chip_select!=1)return false;
 if(t->dma){
  if(command==0x52&&t->direction==DOL_EXI_TRANSFER_READ){for(unsigned i=0;i<t->length;i++)mem_w8(t->dma_address+i,card[(address+i)&(card_size-1)]);reads++;}
  else if(command==0xf2&&t->direction==DOL_EXI_TRANSFER_WRITE){if(card_mutex_init)CARD_LOCK();for(unsigned i=0;i<t->length;i++)card[(address+i)&(card_size-1)]=mem_r8(t->dma_address+i);dirty=1;writes++;irq_pending=1;if(card_mutex_init)CARD_UNLOCK();update_irq();}
  return true;
 }
 uint32_t value=0;for(unsigned i=0;i<t->length&&i<4;i++){unsigned char input=(unsigned char)(*t->immediate_data>>(24-i*8));unsigned char output=transfer_byte(input);value|=(uint32_t)output<<(24-i*8);}
 if(t->direction!=DOL_EXI_TRANSFER_WRITE)*t->immediate_data=value;
 return true;
}

void frontend_card_release(void) {
 frontend_card_flush();free(card);card=NULL;
#ifdef _WIN32
 if(card_lock!=INVALID_HANDLE_VALUE){CloseHandle(card_lock);card_lock=INVALID_HANDLE_VALUE;}
 if(card_mutex_init)DeleteCriticalSection(&card_mutex);
#else
 if(card_lock_fd>=0){close(card_lock_fd);card_lock_fd=-1;}
#endif
 card_mutex_init=0;
}
