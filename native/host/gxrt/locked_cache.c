/* GameCube locked-cache DMA, matching Dolphin Interpreter_SystemRegisters.cpp.
 * The DMA completes synchronously; the transfer-trigger bit clears on completion. */
#include "abi_recompcore.h"
void hle_write_spr(Context* ctx, uint32_t spr, uint32_t value)
{
    ctx->spr[spr] = value;
    if (spr != 923 || !(value & 2u)) return;
    uint32_t upper=ctx->spr[922];
    uint32_t blocks=((upper&31u)<<2)|((value>>2)&3u);
    if (!blocks) blocks=128;
    uint32_t memory=upper&~31u;
    uint32_t cache=0xE0000000u|(value&0x3FE0u);
    for (uint32_t i=0; i<blocks*32; ++i) {
        uint32_t lc=0xE0000000u|((cache+i)&0x3FFFu);
        if (value&16u) mem_w8(lc,mem_r8(memory+i));
        else mem_w8(memory+i,mem_r8(lc));
    }
    ctx->spr[923] &= ~2u;
}
