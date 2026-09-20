/* Native MTH/THP entropy decoding; original Melee owns streaming and presentation.
 * THP omits JPEG entropy byte stuffing. Restore it for libjpeg-turbo, then tile
 * the decoded YUV420 planes into the GX I8 layout expected by the game. */
#include "abi_recompcore.h"
#include <turbojpeg.h>
#include <stdio.h>
#include <stdlib.h>
static tjhandle decoder;
static unsigned char jpeg[262144], planes[640*480*3/2];
long g_video_decoded;
static void decode_frame(Context* ctx) {
    uint32_t info=ctx->gpr[3], src=mem_r32(info+12);
    uint32_t width=mem_r16(info+0x50), height=mem_r16(info+0x52);
    uint32_t cap=mem_r32(0x804333ECu); /* MoviePlayer.buf_size includes the next-size word. */
    if (!cap || cap>131072 || cap<4 || width>640 || height>480 ||
        (src & RAM_MASK)+cap-4>ctx->ram_size) goto fail;
    cap-=4;
    const unsigned char* data=ctx->ram+(src&RAM_MASK);
    uint32_t scan=2;
    while (scan+4<cap) {
        if (data[scan]!=0xff) goto fail;
        unsigned marker=data[scan+1];
        unsigned size=((unsigned)data[scan+2]<<8)|data[scan+3];
        if (size<2 || scan+2+size>cap) goto fail;
        scan+=2+size;
        if (marker==0xda) break;
    }
    if (scan>=cap) goto fail;
    memcpy(jpeg,data,scan);
    uint32_t bytes=scan;
    /* The decoder knows the MCU count from SOF. Stuff even the original EOI;
     * unused ring-buffer tail is harmless extra entropy and cannot terminate
     * the image early when an unescaped FF D9 occurs inside a valid block. */
    for (uint32_t i=scan;i<cap;i++) {
        if (bytes+3>=sizeof jpeg) goto fail;
        jpeg[bytes++]=data[i];
        if (data[i]==0xff) jpeg[bytes++]=0;
    }
    jpeg[bytes++]=0xff;jpeg[bytes++]=0xd9;
    if (!decoder) decoder=tjInitDecompress();
    unsigned char* out[3]={planes,planes+width*height,planes+width*height*5/4};
    int strides[3]={(int)width,(int)width/2,(int)width/2};
    int result=tjDecompressToYUVPlanes(decoder,jpeg,bytes,out,width,strides,height,0);
    if (result<0 && tjGetErrorCode(decoder)!=TJERR_WARNING) goto fail;
    for (unsigned p=0;p<3;p++) {
        uint32_t w=p?width/2:width, h=p?height/2:height;
        uint32_t dest=ctx->gpr[4+p];
        if ((dest&RAM_MASK)+w*h>ctx->ram_size || w%8 || h%4) goto fail;
        uint8_t* tile=ctx->ram+(dest&RAM_MASK);
        for (unsigned y=0;y<h;y+=4)
            for (unsigned x=0;x<w;x+=8)
                for (unsigned row=0;row<4;row++) {
                    memcpy(tile,out[p]+(y+row)*w+x,8);tile+=8;
                }
        mem_w32(info+0x8f0+p*4,dest);
    }
    ctx->gqr[5]=0x00070007u;ctx->gqr[6]=0x3d043d04u;
    g_video_decoded++;
    return;
fail:
    fprintf(stderr,"[video] native THP decode failed info=%08X input=%08X size=%u dims=%ux%u: %s\n",
        info,src,cap,width,height,decoder?tjGetErrorStr2(decoder):"invalid frame");
    fflush(stderr);exit(6);
}
void func_80331340(Context* ctx) { decode_frame(ctx); }
void func_803313D0(Context* ctx) { decode_frame(ctx); }
