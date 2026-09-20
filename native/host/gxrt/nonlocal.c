/* Guest nonlocal jumps must unwind the matching native C call stack too. */
#include "abi_recompcore.h"
#include <stdio.h>
#include <stdlib.h>
#include "platform_compat.h"
typedef struct { uint32_t address;DWORD thread;jmp_buf state; } JumpTarget;
static JumpTarget targets[128];
jmp_buf* recomp_jmpbuf(uint32_t address) {
    DWORD thread=GetCurrentThreadId();
    for(unsigned i=0;i<128;i++)
        if(targets[i].address==address && targets[i].thread==thread) return &targets[i].state;
    for(unsigned i=0;i<128;i++) if(!targets[i].address) {
        targets[i].address=address;targets[i].thread=thread;return &targets[i].state;
    }
    fprintf(stderr,"Guest setjmp target table exhausted\n");exit(7);
}
void func_80322840(Context* ctx) {
    uint32_t address=ctx->gpr[3],value=ctx->gpr[4];
    jmp_buf* target=NULL;
    for(unsigned i=0;i<128;i++)
        if(targets[i].address==address && targets[i].thread==GetCurrentThreadId()) target=&targets[i].state;
    if(!target) { fprintf(stderr,"Guest longjmp has no active target at %08X\n",address);exit(7); }
    ctx->lr=mem_r32(address);ctx->cr=mem_r32(address+4);
    ctx->gpr[1]=mem_r32(address+8);ctx->gpr[2]=mem_r32(address+12);
    for(unsigned i=13;i<32;i++) ctx->gpr[i]=mem_r32(address+20+(i-13)*4);
    for(unsigned i=14;i<32;i++) ctx->fpr[i]=mem_rf64(address+96+(i-14)*8);
    ctx->fpr[0]=mem_rf64(address+240);
    ctx->gpr[3]=value?value:1;
    longjmp(*target,1);
}
