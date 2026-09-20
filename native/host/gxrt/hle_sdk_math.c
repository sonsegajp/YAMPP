/* SDK matrix operations, derived from the local GALE01 Dolphin SDK source.
 * Keep the original operation order and stage operands for in-place concatenation.
 * Guest matrices remain big endian. No frame or animation data is skipped. */
#include "abi_recompcore.h"
void func_80342204(Context* ctx) {
    trace_enter(0x80342204u, ctx);
    double a[12], b[12];float out[12];
    for(unsigned i=0;i<12;i++){a[i]=mem_rf32(ctx->gpr[3]+i*4);b[i]=mem_rf32(ctx->gpr[4]+i*4);}
    for(unsigned row=0;row<3;row++)for(unsigned col=0;col<4;col++){
        double v=(float)(b[col]*a[row*4]);
        v=(float)(b[4+col]*a[row*4+1]+v);
        v=(float)(b[8+col]*a[row*4+2]+v);
        if(col>=2)v=(float)((col==3?1.0:0.0)*a[row*4+3]+v);
        out[row*4+col]=(float)v;
    }
    for(unsigned i=0;i<12;i++)mem_wf32(ctx->gpr[5]+i*4,out[i]);
}

/* Original GXTransform.c matrix writers use GQR0 float transfers only. Keeping
 * the six paired registers explicit avoids the generic quantization dispatch
 * on every lane, while retaining the load-before-store and MMIO write order.
 * Do not memcpy raw source bits to the FIFO: float-to-double-to-float conversion
 * intentionally preserves the original handling of signaling NaNs. */
void sdk_write_mtx4x3(Context* ctx) {
    TRACE_ENTER(0x80341408u, ctx);
    for (unsigned i = 0; i < 6; ++i) {
        uint32_t source = ctx->gpr[3] + i * 8u;
        ctx->fpr[i] = (double)mem_rf32(source);
        ctx->ps1[i] = (double)mem_rf32(source + 4u);
    }
    for (unsigned i = 0; i < 6; ++i) {
        uint32_t destination = ctx->gpr[4];
        mem_wf32(destination, (float)ctx->fpr[i]);
        mem_wf32(destination + 4u, (float)ctx->ps1[i]);
    }
}

void sdk_write_nrm_mtx3x3(Context* ctx) {
    TRACE_ENTER(0x8034143Cu, ctx);
    for (unsigned row = 0; row < 3; ++row) {
        unsigned i = row * 2u;
        uint32_t source = ctx->gpr[3] + row * 16u;
        ctx->fpr[i] = (double)mem_rf32(source);
        ctx->ps1[i] = (double)mem_rf32(source + 4u);
        ctx->fpr[i + 1u] = (double)mem_rf32(ctx->gpr[3] + row * 16u + 8u);
        ctx->ps1[i + 1u] = ctx->fpr[i + 1u];
    }
    for (unsigned row = 0; row < 3; ++row) {
        unsigned i = row * 2u;
        uint32_t destination = ctx->gpr[4];
        mem_wf32(destination, (float)ctx->fpr[i]);
        mem_wf32(destination + 4u, (float)ctx->ps1[i]);
        mem_wf32(ctx->gpr[4], (float)ctx->fpr[i + 1u]);
    }
}
