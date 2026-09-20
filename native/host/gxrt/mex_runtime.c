/* m-ex guest-code compatibility. Stock game functions remain recompiled;
 * instructions added or patched by an upstream build execute in shared guest RAM.
 * This experimental path is opt-in until fighter/stage/rollback validation passes. */
#include "abi_recompcore.h"
#include "recomp_funcs.h"
#include "gxruntime/loader.h"
#include <unicorn/unicorn.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "mex_functions.inc"
int g_mex_active;
static unsigned char* baseline;
static unsigned char* code_shadow;
static unsigned char code_seen[0x1800];
static int debug_history;
static int scalar_batch;
static unsigned short* entry_indices;
static uc_engine* engine;
static void* library_handle;
static unsigned run_depth, bypass_lookup;
static uint32_t pending_entry;
static Context* machine;
extern RecFn mex_native_lookup(Context*,uint32_t);
#define UC_FN(name) static __typeof__(&name) api_##name
UC_FN(uc_open); UC_FN(uc_close); UC_FN(uc_reg_read); UC_FN(uc_reg_write);
UC_FN(uc_reg_read_batch); UC_FN(uc_reg_write_batch);
UC_FN(uc_mem_map_ptr); UC_FN(uc_mem_map); UC_FN(uc_mem_write);
UC_FN(uc_hook_add); UC_FN(uc_emu_start); UC_FN(uc_emu_stop); UC_FN(uc_strerror); UC_FN(uc_ctl);
static void check(uc_err error) {
    if(error!=UC_ERR_OK){fprintf(stderr,"[mex-runtime] %s\n",api_uc_strerror(error));fflush(stderr);exit(12);}
}
/* These reserved addresses name host UI callbacks, not guest instructions. */
static RecFn native_callback(uint32_t address) {
    extern RecFn hooks_lookup(uint32_t);
    return address>=0x817FFE00u&&address<0x817FFF00u?hooks_lookup(address):NULL;
}
static int index_for(uint32_t address) {
    uint32_t phys=address & RAM_MASK;
    if((address&3) || phys>=0x400000u || !entry_indices) return -1;
    return (int)entry_indices[phys>>2]-1;
}
static int modified(int i) {
    uint32_t phys=mex_functions[i].address&RAM_MASK, size=mex_functions[i].size;
    return memcmp(baseline+phys,machine->ram+phys,size)!=0;
}
/* Transfer the same registers at the same execution boundaries in one API
 * call. This reduces DLL/API overhead without combining guest instructions. */
static void transfer(Context* ctx,int to_engine) {
    int regs[70];void* values[70];
    for(int i=0;i<32;i++) {
        regs[2*i]=UC_PPC_REG_0+i;values[2*i]=&ctx->gpr[i];
        regs[2*i+1]=UC_PPC_REG_FPR0+i;values[2*i+1]=&ctx->fpr[i];
    }
    int control[]={UC_PPC_REG_PC,UC_PPC_REG_LR,UC_PPC_REG_CTR,UC_PPC_REG_CR,UC_PPC_REG_XER,UC_PPC_REG_FPSCR};
    void* fields[]={&ctx->pc,&ctx->lr,&ctx->ctr,&ctx->cr,&ctx->xer,&ctx->fpscr};
    for(int i=0;i<6;i++){regs[64+i]=control[i];values[64+i]=fields[i];}
    check(to_engine?api_uc_reg_write_batch(engine,regs,values,70):api_uc_reg_read_batch(engine,regs,values,70));
    /* Effective addresses are already translated by the native guest-memory ABI. */
    if(to_engine){uint32_t msr=0x2000;check(api_uc_reg_write(engine,UC_PPC_REG_MSR,&msr));}
}
/* One active emulation slice at a time; native calls occur after uc_emu_stop.
 * Nested m-ex calls reuse the engine only after the enclosing slice has stopped. */
typedef struct {uint32_t stop,sp,address,word;unsigned count;int reason,first;} Slice;
static Slice* slice;
static uint32_t history_pc[128],history_word[128],history_lr[128],history_sp[128],history_index;
static void history_dump(Context* ctx){
    fprintf(stderr,"[mex-runtime] registers pc=%08X lr=%08X ctr=%08X sp=%08X cr=%08X\n",ctx->pc,ctx->lr,ctx->ctr,ctx->gpr[1],ctx->cr);
    for(unsigned i=0;i<128;i++){unsigned j=(history_index+i)&127;fprintf(stderr,"[mex-history] %08X %08X lr=%08X sp=%08X\n",history_pc[j],history_word[j],history_lr[j],history_sp[j]);}
}
static int special(uint32_t word) {
    unsigned op=word>>26, xo=(word>>1)&1023;
    return op==48 || op==49 || op==52 || op==53 || op==4 || op==56 || op==57 || op==60 || op==61 || op==59 || op==63 ||
      (op==31 && (xo==339 || xo==467 || xo==371 || xo==146 || xo==83 || xo==982 || xo==1014 || xo==535 || xo==567 || xo==663 || xo==695));
}
static void code_hook(uc_engine* uc,uint64_t address,uint32_t size,void* user) {
    (void)size;(void)user;Slice* s=slice;uint32_t sp;
    s->address=(uint32_t)address;s->word=mem_r32((uint32_t)address);
    if(debug_history){unsigned hi=history_index++&127;history_pc[hi]=(uint32_t)address;history_word[hi]=s->word;
      api_uc_reg_read(uc,UC_PPC_REG_LR,&history_lr[hi]);api_uc_reg_read(uc,UC_PPC_REG_1,&history_sp[hi]);}
    /* AOT loaders and m-ex patches write shared RAM outside Unicorn's memory
     * API. Detect changed executable words before executing a cached block. */
    uint32_t phys=(uint32_t)address&RAM_MASK,page=phys>>12;
    if(phys+4<=machine->ram_size){
      if(!code_seen[page]){memcpy(code_shadow+(phys&~4095u),machine->ram+(phys&~4095u),4096);code_seen[page]=1;}
      else if(memcmp(code_shadow+phys,machine->ram+phys,4)){s->reason=5;api_uc_emu_stop(uc);return;}
    }
    static unsigned long long steps;
    if(debug_history && (++steps%1000000)==0)fprintf(stderr,"[mex-runtime] progress pc=%08X word=%08X depth=%u\n",s->address,s->word,run_depth);
    if((uint32_t)address==s->stop){check(api_uc_reg_read(uc,UC_PPC_REG_1,&sp));if(sp==s->sp){s->reason=1;api_uc_emu_stop(uc);return;}}
    /* A wrapper calls the patched original through run(): skip its hook on
     * this first instruction only. Subsequent guest calls still use YAMPP's
     * menu, input and music wrappers, even when their original is patched. */
    int first=s->first;s->first=0;
    int i=index_for((uint32_t)address);
    extern RecFn hooks_lookup(uint32_t),costumes_lookup(uint32_t);
    if(native_callback((uint32_t)address) || (!first && i>=0 && (hooks_lookup((uint32_t)address)||costumes_lookup((uint32_t)address)))){s->reason=2;api_uc_emu_stop(uc);return;}
    if(i>=0 && (!modified(i)||mex_native_lookup(machine,(uint32_t)address))){s->reason=2;api_uc_emu_stop(uc);return;}
    if(!first && i>=0){s->reason=6;api_uc_emu_stop(uc);return;}
    if(special(s->word)){s->reason=3;api_uc_emu_stop(uc);return;}
    if(++s->count>=4096){s->reason=4;api_uc_emu_stop(uc);}
}
static void mmio_hook(uc_engine* uc,uc_mem_type type,uint64_t address,int size,int64_t value,void* user) {
    (void)user;
    if(type==UC_MEM_WRITE){machine->external_write(machine,(uint32_t)address,(uint64_t)value,(uint8_t)size);return;}
    uint64_t v=machine->external_read(machine,(uint32_t)address,(uint8_t)size);unsigned char b[8];
    for(int i=0;i<size;i++)b[i]=(unsigned char)(v>>(8*(size-i-1)));
    check(api_uc_mem_write(uc,address,b,(size_t)size));
}
/* Keep SPR storage identical to the AOT ABI, including its generic SPR bank. */
static uint32_t* spr_field(Context* ctx,unsigned spr) {
    if(spr==1)return &ctx->xer;
    if(spr==8)return &ctx->lr;
    if(spr==9)return &ctx->ctr;
    if(spr>=912&&spr<=919)return &ctx->gqr[spr-912];
    return &ctx->spr[spr&1023];
}
static int gekko_step(Context* ctx,uint32_t w) {
    unsigned op=w>>26,d=(w>>21)&31,a=(w>>16)&31,b=(w>>11)&31,c=(w>>6)&31,xo=(w>>1)&1023;
    uint32_t ea;double x=ctx->fpr[a],y=ctx->fpr[b],z=ctx->fpr[c],v;
    if(op==48||op==49){ea=(a?ctx->gpr[a]:0)+(int16_t)w;ctx->fpr[d]=mem_rf32(ea);ctx->ps1[d]=ctx->fpr[d];if(op&1)ctx->gpr[a]=ea;return 1;}
    if(op==52||op==53){ea=(a?ctx->gpr[a]:0)+(int16_t)w;mem_wf32(ea,(float)ctx->fpr[d]);if(op&1)ctx->gpr[a]=ea;return 1;}
    if(op==56||op==57||op==60||op==61){
        int32_t off=(int32_t)(w<<20)>>20;ea=(a?ctx->gpr[a]:0)+off;
        if(op<60)ru_psq_load(ctx,d,ea,(w>>15)&1,(w>>12)&7);else ru_psq_store(ctx,d,ea,(w>>15)&1,(w>>12)&7);
        if(op&1)ctx->gpr[a]=ea;return 1;
    }
    if(op==31){unsigned spr=((w>>16)&31)|((w>>6)&0x3e0);
        if(xo==535||xo==567){ea=(a?ctx->gpr[a]:0)+ctx->gpr[b];ctx->fpr[d]=mem_rf32(ea);ctx->ps1[d]=ctx->fpr[d];if(xo==567)ctx->gpr[a]=ea;return 1;}
        if(xo==663||xo==695){ea=(a?ctx->gpr[a]:0)+ctx->gpr[b];mem_wf32(ea,(float)ctx->fpr[d]);if(xo==695)ctx->gpr[a]=ea;return 1;}
        if(xo==339){ctx->gpr[d]=*spr_field(ctx,spr);return 1;}
        if(xo==467){if(spr==922||spr==923)hle_write_spr(ctx,spr,ctx->gpr[d]);else *spr_field(ctx,spr)=ctx->gpr[d];return 1;}
        if(xo==371){ctx->gpr[d]=hle_timebase(spr==269);return 1;}
        if(xo==146){ctx->msr=ctx->gpr[d];return 1;}if(xo==83){ctx->gpr[d]=ctx->msr;return 1;}
        if(xo==982)return 1;
        if(xo==1014){ea=((a?ctx->gpr[a]:0)+ctx->gpr[b])&~31u;for(int i=0;i<32;i+=4)mem_w32(ea+i,0);return 1;}
        return 0;
    }
    /* Match the native recompiler's scalar FP/paired-lane representation. */
    if(op==59||op==63){
        if(op==63){switch(xo){
          case 0:case 32:cr_set_field(ctx,(w>>23)&7,x<y?8:x>y?4:x==y?2:1);return 1;
          case 12:ctx->fpr[d]=(float)y;ctx->ps1[d]=ctx->fpr[d];return 1;
          case 14:case 15:{int32_t i=(int32_t)y;memcpy(&ctx->fpr[d],&i,4);return 1;}
          case 40:ctx->fpr[d]=-y;return 1;case 72:ctx->fpr[d]=y;return 1;
          case 136:ctx->fpr[d]=-fabs(y);return 1;case 264:ctx->fpr[d]=fabs(y);return 1;
          case 583:memcpy(&ctx->fpr[d],&ctx->fpscr,4);return 1;
          case 38:case 64:case 70:case 134:case 711:return 1;
        }}
        switch(xo&31){case 18:v=x/y;break;case 20:v=x-y;break;case 21:v=x+y;break;
          case 23:v=x>=0?z:y;break;case 24:v=1.0/y;break;case 25:v=x*z;break;
          case 26:v=1.0/sqrt(y);break;case 28:v=x*z-y;break;case 29:v=x*z+y;break;
          case 30:v=-(x*z-y);break;case 31:v=-(x*z+y);break;default:return 0;}
        ctx->fpr[d]=op==59?(double)(float)v:v;if(op==59)ctx->ps1[d]=ctx->fpr[d];return 1;
    }
    if(op==4){
        unsigned q=xo&63;
        if(q==6||q==7||q==38||q==39){ea=(a?ctx->gpr[a]:0)+ctx->gpr[b];if(q==6||q==38)ru_psq_load(ctx,d,ea,(w>>10)&1,(w>>7)&7);else ru_psq_store(ctx,d,ea,(w>>10)&1,(w>>7)&7);if(q&32)ctx->gpr[a]=ea;return 1;}
        double x1=ctx->ps1[a],y1=ctx->ps1[b],z1=ctx->ps1[c],v1;
        switch(xo){
         case 0:case 32:cr_set_field(ctx,(w>>23)&7,x<y?8:x>y?4:x==y?2:1);return 1;
         case 64:case 96:cr_set_field(ctx,(w>>23)&7,x1<y1?8:x1>y1?4:x1==y1?2:1);return 1;
         case 40:v=-y;v1=-y1;goto pair;case 72:v=y;v1=y1;goto pair;
         case 136:v=-fabs(y);v1=-fabs(y1);goto pair;case 264:v=fabs(y);v1=fabs(y1);goto pair;
         case 528:v=x;v1=y;goto pair;case 560:v=x;v1=y1;goto pair;
         case 592:v=x1;v1=y;goto pair;case 624:v=x1;v1=y1;goto pair;
        }
        switch(xo&31){
         case 10:v=x+y1;v1=z1;break;case 11:v=z;v1=x+y1;break;
         case 12:v=x*z;v1=x1*z;break;case 13:v=x*z1;v1=x1*z1;break;
         case 14:v=x*z+y;v1=x1*z+y1;break;case 15:v=x*z1+y;v1=x1*z1+y1;break;
         case 18:v=x/y;v1=x1/y1;break;case 20:v=x-y;v1=x1-y1;break;case 21:v=x+y;v1=x1+y1;break;
         case 23:v=x>=0?z:y;v1=x1>=0?z1:y1;break;case 24:v=1/y;v1=1/y1;break;
         case 25:v=x*z;v1=x1*z1;break;case 26:v=1/sqrt(y);v1=1/sqrt(y1);break;
         case 28:v=x*z-y;v1=x1*z1-y1;break;case 29:v=x*z+y;v1=x1*z1+y1;break;
         case 30:v=-(x*z-y);v1=-(x1*z1-y1);break;case 31:v=-(x*z+y);v1=-(x1*z1+y1);break;
         default:return 0;
        }
        pair:ctx->fpr[d]=(float)v;ctx->ps1[d]=(float)v1;return 1;
    }
    return 0;
}
static void run(Context* ctx,uint32_t address) {
    Slice local={.stop=ctx->lr,.sp=ctx->gpr[1],.first=1};Slice* parent=slice;
    ++run_depth;ctx->pc=address;
    if(index_for(address)>=0)TRACE_ENTER(address,ctx);
    if(run_depth>256){fprintf(stderr,"[mex-runtime] callback nesting limit\n");exit(12);}
    static unsigned shown;if(shown++<40)fprintf(stderr,"[mex-runtime] execute %08X return=%08X\n",address,local.stop);
    for(;;){
        local.reason=0;slice=&local;transfer(ctx,1);
        uc_err err=api_uc_emu_start(engine,ctx->pc,0,0,0);
        transfer(ctx,0);
        if(err!=UC_ERR_OK){history_dump(ctx);fprintf(stderr,"[mex-runtime] pc=%08X instruction=%08X\n",ctx->pc,mem_r32(ctx->pc));check(err);}
        if(local.reason==1)break;
        if(local.reason==2){
            uint32_t target=ctx->pc;
            ++bypass_lookup;RecFn fn=lookup_function(target);--bypass_lookup;
            fn(ctx);ctx->pc=ctx->lr;

        } else if(local.reason==6){
            TRACE_ENTER(ctx->pc,ctx);local.first=1;
        } else if(local.reason==5){
            uint32_t page=local.address&RAM_MASK&~4095u;
            memcpy(code_shadow+page,ctx->ram+page,4096);
            for(unsigned i=0;i<3;i++){uint64_t start=page+(i==1?0x80000000ull:i==2?0xC0000000ull:0);
              check(api_uc_ctl(engine,UC_CTL_WRITE(UC_CTL_TB_REMOVE_CACHE,2),start,start+4096));}
        } else if(local.reason==3){
            /* Gekko FP/paired instructions already execute in this ABI, not in
             * Unicorn. Keep adjacent such instructions here rather than
             * transferring all 70 registers out and back for each instruction.
             * Never cross a return target, native callback, function entry or
             * slice budget. All branch/integer instructions still pass through
             * the engine and its code-cache validation hook. */
            unsigned batched=0;uint32_t word=local.word;
            for(;;){
                if(!gekko_step(ctx,word)){fprintf(stderr,"[mex-runtime] unsupported Gekko %08X at %08X\n",word,ctx->pc);exit(12);}
                ctx->pc+=4;
                if(!scalar_batch || debug_history || ++batched>=32 || local.count>=4095
                    || ctx->pc==local.stop || index_for(ctx->pc)>=0
                    || native_callback(ctx->pc)
                    || (ctx->pc&RAM_MASK)+4>ctx->ram_size)break;
                word=mem_r32(ctx->pc);
                if(!special(word))break;
                ++local.count;
            }
        } else if(local.reason==4){local.count=0;recomp_poll(ctx);}
        else {fprintf(stderr,"[mex-runtime] unexpected execution stop\n");exit(12);}
    }
    --run_depth;slice=parent;
}
static void dynamic_entry(Context* ctx){uint32_t address=pending_entry;run(ctx,address);}
RecFn mex_runtime_lookup(uint32_t address){
    if(!g_mex_active||bypass_lookup||!address)return NULL;
    RecFn host=native_callback(address);if(host)return host;
    int i=index_for(address);if(i>=0&&!modified(i))return NULL;
    RecFn fast=mex_native_lookup(machine,address);if(fast)return fast;
    pending_entry=address;return dynamic_entry;
}
int mex_runtime_entry(Context* ctx,uint32_t address,uint32_t size){
    if(!g_mex_active||bypass_lookup)return 0;
    uint32_t phys=address&RAM_MASK;
    if(phys+size>ctx->ram_size||!memcmp(baseline+phys,ctx->ram+phys,size))return 0;
    RecFn fast=mex_native_lookup(ctx,address);
    if(fast)fast(ctx);else run(ctx,address);return 1;
}
int mex_runtime_init(Context* ctx){
    const char* path=getenv("MELEE_MEX_BASE_DOL");if(!path||!*path)return 1;
    const char* library=getenv("MELEE_UNICORN_LIBRARY");if(!library||!*library){fprintf(stderr,"[mex-runtime] Execution library is required\n");return 0;}
#ifdef _WIN32
    fprintf(stderr,"[mex-runtime] init: load execution library\n");
    HMODULE handle=LoadLibraryExA(library,NULL,LOAD_WITH_ALTERED_SEARCH_PATH);
#define LOAD(name) api_##name=(__typeof__(api_##name))GetProcAddress(handle,#name);if(!api_##name)return 0
#else
    void* handle=dlopen(library,RTLD_NOW|RTLD_LOCAL);
#define LOAD(name) api_##name=(__typeof__(api_##name))dlsym(handle,#name);if(!api_##name)return 0
#endif
    library_handle=(void*)handle;
    if(!handle){fprintf(stderr,"[mex-runtime] Could not open execution library\n");return 0;}
    LOAD(uc_open);LOAD(uc_close);LOAD(uc_reg_read);LOAD(uc_reg_write);LOAD(uc_reg_read_batch);LOAD(uc_reg_write_batch);LOAD(uc_mem_map_ptr);LOAD(uc_mem_map);LOAD(uc_mem_write);LOAD(uc_hook_add);LOAD(uc_emu_start);LOAD(uc_emu_stop);LOAD(uc_strerror);LOAD(uc_ctl);
    fprintf(stderr,"[mex-runtime] init: load baseline\n");
    Context base={0};DolLayout layout;
    if(!cpu_init(&base))return 0;
    if(!dol_load_into_ram(&base,path,&layout)){cpu_free(&base);return 0;}
    baseline=base.ram;machine=ctx;
    code_shadow=calloc(1,ctx->ram_size);if(!code_shadow)return 0;
    debug_history=getenv("MELEE_MEX_TRACE")!=NULL;
    const char* batch=getenv("MELEE_MEX_SCALAR_BATCH");
    scalar_batch=!batch || strcmp(batch,"0");
    entry_indices=calloc(0x400000u/4,sizeof(*entry_indices));if(!entry_indices)return 0;
    for(unsigned i=0;i<sizeof mex_functions/sizeof mex_functions[0];i++){uint32_t p=mex_functions[i].address&RAM_MASK;if(p<0x400000)entry_indices[p>>2]=(unsigned short)(i+1);}
    fprintf(stderr,"[mex-runtime] init: open engine\n");
    check(api_uc_open(UC_ARCH_PPC,UC_MODE_PPC32|UC_MODE_BIG_ENDIAN,&engine));
    fprintf(stderr,"[mex-runtime] init: select Gekko CPU\n");
    check(api_uc_ctl(engine,UC_CTL_WRITE(UC_CTL_CPU_MODEL,1),UC_CPU_PPC32_750_V3_1));
    fprintf(stderr,"[mex-runtime] init: map shared RAM\n");
    check(api_uc_mem_map_ptr(engine,0x80000000u,ctx->ram_size,UC_PROT_ALL,ctx->ram));
    check(api_uc_mem_map_ptr(engine,0xC0000000u,ctx->ram_size,UC_PROT_ALL,ctx->ram));
    check(api_uc_mem_map_ptr(engine,0,ctx->ram_size,UC_PROT_ALL,ctx->ram));
    check(api_uc_mem_map(engine,0xCC000000u,0x10000,UC_PROT_READ|UC_PROT_WRITE));
    uc_hook hook;check(api_uc_hook_add(engine,&hook,UC_HOOK_CODE,(void*)code_hook,NULL,1,0));
    check(api_uc_hook_add(engine,&hook,UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,(void*)mmio_hook,NULL,0xCC000000u,0xCC00FFFFu));
    g_mex_active=1;fprintf(stderr,"[mex-runtime] Dynamic upstream execution enabled\n");return 1;
}

void mex_runtime_shutdown(void){
 if(engine){check(api_uc_close(engine));engine=NULL;}
 free(baseline);baseline=NULL;free(code_shadow);code_shadow=NULL;free(entry_indices);entry_indices=NULL;
 if(library_handle){
#ifdef _WIN32
  FreeLibrary((HMODULE)library_handle);
#else
  dlclose(library_handle);
#endif
  library_handle=NULL;
 }
 g_mex_active=0;
}
