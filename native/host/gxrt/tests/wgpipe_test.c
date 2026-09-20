/* Byte-exact transfer and device-routing regression; no guest data fixtures. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gxruntime/gx_recomp.h"
#include "gxruntime/mmio_bus.h"
#include "gxruntime/cp.h"
#include "gxruntime/interrupts.h"
#include "gxruntime/di.h"
#include "gxruntime/si.h"
#include "gxruntime/exi.h"
#include "gxruntime/pe.h"
#include "gxruntime/mi.h"

#define SEG_CAP 128u
#define SEG_GUARD 32u
static u8 segment[SEG_CAP+SEG_GUARD], list[32];
static u8* g_seg[1];
static u32 g_seg_len[1],g_seg_cap[1];
static int g_seg_cur;
static const u8* s_pending_list;
static u32 s_pending_len,s_swallow;
static long g_wg_bytes,g_wg_writes;
static DolGxRecompState g_gx;
static unsigned scans,fail_alloc,diagnostic_output;
static void wgseg_scan_list(const u8* p,u32 n,int depth) { assert(p==list);assert(n<=sizeof list);assert(depth==0);scans++; }
static void* test_malloc(size_t n) { assert(n==sizeof segment);return fail_alloc?NULL:segment; }
static int quiet_printf(const char* fmt,...) { (void)fmt;diagnostic_output++;return 0; }
static int quiet_flush(FILE* f) { (void)f;diagnostic_output++;return 0; }
#define malloc test_malloc
#define printf quiet_printf
#define fflush quiet_flush
#include "production.inc"
#include "wgpipe_reference.inc"
#undef fflush
#undef printf
#undef malloc

static u32 seed=0x9e3779b9;
static u32 random32(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
struct Result {
    u8 segment[sizeof segment],fifo[256];
    DolGxRecompTraceEvent trace[32];
    u32 len,cap,pending,swallow,fifo_size,trace_count,scans;
    long bytes,writes;
    bool pointer,pending_pointer;
};
static void reset(unsigned scenario) {
    memset(segment,0xa5,sizeof segment);memset(&g_gx,0,sizeof g_gx);
    g_seg[0]=(scenario&1)?NULL:segment;
    g_seg_cap[0]=g_seg[0]?((scenario>>1)%129):0;
    g_seg_len[0]=g_seg[0]?(scenario% (g_seg_cap[0]+1)):0;
    s_pending_list=(scenario&2)?list:NULL;s_pending_len=(scenario&4)?sizeof list:0;
    s_swallow=(scenario>>3)%10;fail_alloc=(scenario&16)!=0;
    scans=0;g_wg_bytes=123;g_wg_writes=7;
    /* Also exercise the diagnostic accumulator's full-buffer rejection. */
    if(scenario&32)g_gx.fifo.size=DOL_GX_RECOMP_MAX_FIFO_BYTES;
}
static void snapshot(struct Result* r) {
    memset(r,0,sizeof *r);memcpy(r->segment,segment,sizeof segment);
    r->len=g_seg_len[0];r->cap=g_seg_cap[0];r->pending=s_pending_len;r->swallow=s_swallow;
    r->pointer=g_seg[0]!=NULL;r->pending_pointer=s_pending_list!=NULL;
    r->bytes=g_wg_bytes;r->writes=g_wg_writes;r->scans=scans;
    r->fifo_size=g_gx.fifo.size;r->trace_count=g_gx.trace_count;
    memcpy(r->fifo,g_gx.fifo.bytes,sizeof r->fifo);memcpy(r->trace,g_gx.trace,sizeof r->trace);
}
static void fifo_cases(void) {
    CPUState cpu={0},before=cpu;
    for(unsigned scenario=0;scenario<2048;scenario++) {
        u64 values[24];u8 sizes[24];struct Result expected,actual;
        for(unsigned i=0;i<24;i++) {
            values[i]=((u64)random32()<<32)|random32();sizes[i]=1+(random32()%8);
            /* Force CP diagnostics and CALL_DL headers across every width. */
            if(i<8)values[i]=(i&1)?0x4000000040000040ull:0x0850aabbccdd0840ull;
        }
        reset(scenario);
        for(unsigned i=0;i<24;i++)assert(wr_wgpipe_reference(NULL,&cpu,0xCC008000,sizes[i],values[i]));
        snapshot(&expected);assert(!memcmp(&cpu,&before,sizeof cpu));
        reset(scenario);diagnostic_output=0;
        for(unsigned i=0;i<24;i++)assert(wr_wgpipe(NULL,&cpu,0xCC008000,sizes[i],values[i]));
        snapshot(&actual);assert(!memcmp(&cpu,&before,sizeof cpu));
        assert(!memcmp(&expected,&actual,sizeof actual));
        if(!getenv("MELEE_TRACE_DIAGNOSTICS"))assert(diagnostic_output==0);
    }
}
static int route,misses;
void gap_report_note(const char* a,const char* b,const char* c,u32 pc,const char* d) { (void)a;(void)b;(void)c;(void)pc;(void)d;misses++; }
static bool route_write(void* user,CPUState* cpu,u32 ea,u8 n,u64 value) { (void)cpu;(void)ea;(void)n;(void)value;route=(int)(size_t)user;return true; }
static bool route_read(void* user,CPUState* cpu,u32 ea,u8 n,u64* value) { (void)cpu;(void)ea;(void)n;route=(int)(size_t)user;*value=(u64)route;return true; }
static void route_cases(void) {
    DolMmioBus ordered,original;dol_mmio_bus_init(&ordered);dol_mmio_bus_init(&original);
    assert(ranges[0][0]==0xCC008000u);assert(ranges[0][1]==0x100u);
    for(unsigned i=0;i<11;i++) {
        for(unsigned j=i+1;j<11;j++)assert((u64)ranges[i][0]+ranges[i][1]<=ranges[j][0] || (u64)ranges[j][0]+ranges[j][1]<=ranges[i][0]);
        assert(dol_mmio_bus_register(&ordered,ranges[i][0],ranges[i][1],route_read,route_write,(void*)(size_t)(i+1)));
        unsigned old=(i+1)%11;
        assert(dol_mmio_bus_register(&original,ranges[old][0],ranges[old][1],route_read,route_write,(void*)(size_t)(old+1)));
    }
    for(unsigned test=0;test<20000;test++) {
        u32 ea=random32();u8 n=(u8)random32();
        if(test<19000) { unsigned region=test%11;ea=ranges[region][0]+(test&1?ranges[region][1]:0)+(int)(test%19)-9;n=(u8)(test%10); }
        if(test>=19990)ea=0xffffffffu-(test%10);
        route=misses=0;bool a=dol_mmio_bus_write(&ordered,NULL,ea,n,0xabcd);int ar=route,am=misses;
        route=misses=0;bool b=dol_mmio_bus_write(&original,NULL,ea,n,0xabcd);assert(a==b&&ar==route&&am==misses);
        u64 av=0,bv=0;route=misses=0;a=dol_mmio_bus_read(&ordered,NULL,ea,n,&av);ar=route;am=misses;
        route=misses=0;b=dol_mmio_bus_read(&original,NULL,ea,n,&bv);assert(a==b&&av==bv&&ar==route&&am==misses);
    }
}
int main(void) { for(unsigned i=0;i<sizeof list;i++)list[i]=(u8)i;fifo_cases();route_cases();puts("PASS: 49,152 FIFO writes, pending/split lists, capacity/allocation failure, GX accumulator, 40,000 edge-width MMIO routes");return 0; }
