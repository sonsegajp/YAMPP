/* Virtual IPL SRAM. Keep SDK configuration reads/writes on the EXI device path. */
#include "abi_recompcore.h"
#include "gxruntime/exi.h"
static uint8_t sram[64];
static uint32_t command,cursor;
static int initialized,command_ready;
void frontend_ipl_deselect(void) { command_ready=0;cursor=0; }
bool frontend_ipl_transfer(void* user,DolExiTransfer* t) {
    (void)user;
    { extern bool frontend_card_transfer(DolExiTransfer*);if(frontend_card_transfer(t))return true; }
    if(t->channel!=0 || t->chip_select!=2) return true;
    if(!initialized) {
        /* Default console setup: English, stereo, NTSC. */
        sram[0x12]=0;sram[0x13]=0x2c;
        uint16_t sum=0,inverse=0;
        for(unsigned i=0xc;i<0x14;i+=2) {
            uint16_t v=((uint16_t)sram[i]<<8)|sram[i+1];sum+=v;inverse+=(uint16_t)~v;
        }
        sram[0]=sum>>8;sram[1]=sum;sram[2]=inverse>>8;sram[3]=inverse;
        { extern void frontend_card_sram(unsigned char*);frontend_card_sram(sram); }
        initialized=1;
    }
    if(!command_ready && !t->dma && t->direction==DOL_EXI_TRANSFER_WRITE && t->length==4) {
        command=*t->immediate_data;cursor=0;command_ready=1;return true;
    }
    uint32_t address=((command&0x7fffffffu)>>6)+cursor;
    uint32_t value=0;
    for(uint32_t i=0;i<t->length;i++) {
        uint8_t byte=0;
        if(address+i>=0x800004u && address+i<0x800044u) {
            unsigned index=address+i-0x800004u;
            if(command&0x80000000u) sram[index]=t->dma?mem_r8(t->dma_address+i):(uint8_t)(*t->immediate_data>>(24-i*8));
            byte=sram[index];
        }
        if(t->direction==DOL_EXI_TRANSFER_READ) {
            if(t->dma) mem_w8(t->dma_address+i,byte);
            else value|=(uint32_t)byte<<(24-i*8);
        }
    }
    if(!t->dma && t->direction==DOL_EXI_TRANSFER_READ) *t->immediate_data=value;
    cursor+=t->length;
    return true;
}
