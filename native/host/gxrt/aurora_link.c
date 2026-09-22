/* Dynamic link to the aurora shim.
 *
 * The shim is MSVC/C++ with Dawn inside it; this frontend is mingw gcc. The C
 * ABI across a DLL boundary is the seam (proven working: aushim_probe and a
 * DRAW_SIZED submission both cross it). Loaded at runtime rather than linked so
 * the frontend still builds and runs when the shim is absent. */
#include "platform_compat.h"
#include <stdio.h>
#include <stdint.h>
#include "../aurora_shim/aurora_shim.h"
#include "../netplay_ui.h"
#include "pad_latch.h"

static HMODULE s_dll = NULL;
static int (*p_poll_pad)(uint32_t, AushimPadStatus*) = NULL;
static int (*p_quit)(void) = NULL;
static SRWLOCK s_pad_lock = SRWLOCK_INIT;
static AushimPadStatus s_pads[4];
static PadLatch s_pad_latches[4];
static void refresh_pads(void) {
    if (!p_poll_pad) return;
    AcquireSRWLockExclusive(&s_pad_lock);
    for (unsigned i=0; i<4; ++i) {
        p_poll_pad(i,&s_pads[i]);
        pad_latch_update(&s_pad_latches[i], &s_pads[i]);
    }
    ReleaseSRWLockExclusive(&s_pad_lock);
}
int aurora_link_get_pad(unsigned port, AushimPadStatus* pad) {
    if (port>=4 || !p_poll_pad) return 0;
    AcquireSRWLockShared(&s_pad_lock);
    *pad=s_pads[port];
    ReleaseSRWLockShared(&s_pad_lock);
    return 1;
}
int aurora_link_consume_pad(unsigned port, AushimPadStatus* pad) {
    if (port>=4 || !p_poll_pad) return 0;
    AcquireSRWLockExclusive(&s_pad_lock);
    *pad=pad_latch_consume(&s_pad_latches[port]);
    ReleaseSRWLockExclusive(&s_pad_lock);
    return 1;
}
void aurora_link_clear_pad_events(void) {
    AcquireSRWLockExclusive(&s_pad_lock);
    /* A new synchronized scene starts from live controller state; latched
     * edges and trigger peaks from the previous one are not its input. */
    for(unsigned i=0;i<4;i++) { s_pad_latches[i].pressed=0; s_pad_latches[i].lt=0; s_pad_latches[i].rt=0; }
    ReleaseSRWLockExclusive(&s_pad_lock);
}
void aurora_link_audio_reset(void) {
    if(s_dll) {
        void (*reset)(void)=(void(*)(void))GetProcAddress(s_dll,"aushim_audio_reset");
        if(reset) reset();
    }
}
int aurora_link_quit_requested(void) { return p_quit && p_quit(); }

static int  (*p_init)(unsigned, unsigned) = NULL;
static int  (*p_begin)(void) = NULL;
static void (*p_settings_menu)(void) = NULL;
static void (*p_end)(void) = NULL;
static void (*p_update)(void) = NULL;
static void (*p_synchronize)(void) = NULL;
static void (*p_shutdown)(void) = NULL;
static void (*p_setmem)(void*, uint32_t) = NULL;
static int  (*p_setup_flat)(void) = NULL;
static int  (*p_draw_sized)(unsigned char, const void*, uint32_t) = NULL;
static int  (*p_call_dl)(const void*, uint32_t) = NULL;
static int  (*p_arraybase)(uint32_t,uint32_t,uint32_t,unsigned char) = NULL;
static int  (*p_arraystride)(uint32_t,uint32_t) = NULL;
static int  (*p_dl_arrays)(const uint32_t*, const uint32_t*, const void*, uint32_t) = NULL;
static int  (*p_settings_widescreen)(int,int) = NULL;
static int  (*p_settings_aspect_lock)(int) = NULL;
static void (*p_settings_netplay)(char*,unsigned,char*,unsigned) = NULL;
static void (*p_settings_set_netplay)(const char*,const char*) = NULL;
static void (*p_netplay_bind)(void*) = NULL;
static void (*p_netplay_menu)(void) = NULL;
static int  (*p_display_refresh)(void) = NULL;
static void (*p_set_vsync)(int) = NULL;
static void (*p_music_bar)(const char*,int) = NULL;
static void (*p_music_draw)(void) = NULL;
static void (*p_table)(const void*,int,int) = NULL;
static void (*p_table_draw)(void) = NULL;
static void (*p_netplay_screen)(const MeleeNetplayUi*,int,int) = NULL;

/* Now-playing bar shown when a stage track starts. */
/* The display's refresh rate in hundredths of a hertz, 0 when unknown. */
int aurora_link_display_refresh(void) { return p_display_refresh ? p_display_refresh() : 0; }
void aurora_link_set_vsync(int enabled) { if (p_set_vsync) p_set_vsync(enabled); }
int aurora_link_music_bar(const char* text, int custom) { if (!p_music_bar) return 0; p_music_bar(text, custom); return 1; }

/* Widescreen setting owned by the renderer settings; -1 when unavailable. */
int aurora_link_widescreen(int set, int value) { return p_settings_widescreen ? p_settings_widescreen(set, value) : -1; }
int aurora_link_aspect_lock(int command) { return p_settings_aspect_lock ? p_settings_aspect_lock(command) : -1; }
void aurora_link_netplay_settings(char* server, unsigned server_cap, char* name, unsigned name_cap) {
    if (server_cap) server[0] = 0;
    if (name_cap) name[0] = 0;
    if (p_settings_netplay) p_settings_netplay(server, server_cap, name, name_cap);
}
void aurora_link_netplay_save_settings(const char* server, const char* name) { if (p_settings_set_netplay) p_settings_set_netplay(server, name); }
int aurora_link_netplay_bind(void* ui) {
    if (!p_netplay_bind) return 0;
    p_netplay_bind(ui);
    typedef void (*RoomNameBind)(int (*)(int,const char*));
    RoomNameBind bind = (RoomNameBind)GetProcAddress(s_dll,"aushim_room_name_bind");
    extern int netplay_room_name_input(int,const char*);
    if (bind) bind(netplay_room_name_input);
    typedef void (*ProfileBind)(void (*)(const char*,const unsigned char*,unsigned,const char*));
    ProfileBind profile_bind = (ProfileBind)GetProcAddress(s_dll,"aushim_profile_bind");
    extern void netplay_profile_changed(const char*,const unsigned char*,unsigned,const char*);
    if (profile_bind) profile_bind(netplay_profile_changed);
    return 1;
}

#define AU_ARRAY_CAP (256u * 1024u)
long g_au_frames = 0, g_au_draws = 0;
/* Pending vertex array bases, flushed inside the frame (see set_array). */
static struct { uint32_t ea, extent; int valid; } s_pending[16];
static int (*p_tri)(void) = 0;
static int (*p_readback)(unsigned char*, uint32_t*, uint32_t*, uint32_t) = 0;
unsigned char* aurora_link_mem1_base = 0; uint32_t aurora_link_mem1_size = 0;
int  g_au_ready  = 0;

int aurora_link_init(void* mem1_base, uint32_t mem1_size, unsigned w, unsigned h)
{
    const char* path = getenv("MELEE_AURORA_DLL");
#ifdef _WIN32
    if (!path || !*path) path = "build/pc/bin/melee_aurora_v2.dll";
#else
    if (!path || !*path) path = "build/pc-linux/libmelee_aurora_v2.so";
#endif
    s_dll = LoadLibraryA(path);
    if (!s_dll) { printf("[au] shim not loaded (%lu) -- running headless\n", GetLastError()); return 0; }
    p_poll_pad = (int(*)(uint32_t,AushimPadStatus*))GetProcAddress(s_dll,"aushim_poll_pad");
    p_quit = (int(*)(void))GetProcAddress(s_dll,"aushim_quit_requested");
    p_init       = (int(*)(unsigned,unsigned))GetProcAddress(s_dll, "aushim_init");
    p_settings_menu = (void(*)(void))GetProcAddress(s_dll,"aushim_settings_menu");
    p_begin      = (int(*)(void))GetProcAddress(s_dll, "aushim_begin_frame");
    p_end        = (void(*)(void))GetProcAddress(s_dll, "aushim_end_frame");
    p_update     = (void(*)(void))GetProcAddress(s_dll, "aushim_update");
    p_synchronize= (void(*)(void))GetProcAddress(s_dll, "aushim_synchronize");
    p_shutdown   = (void(*)(void))GetProcAddress(s_dll, "aushim_shutdown");
    p_setmem     = (void(*)(void*,uint32_t))GetProcAddress(s_dll, "aushim_set_mem1");
    p_tri        = (int(*)(void))GetProcAddress(s_dll, "aushim_draw_test_triangle");
    p_readback   = (int(*)(unsigned char*,uint32_t*,uint32_t*,uint32_t))GetProcAddress(s_dll, "aushim_readback");
    p_setup_flat = (int(*)(void))GetProcAddress(s_dll, "aushim_setup_flat");
    p_draw_sized = (int(*)(unsigned char,const void*,uint32_t))GetProcAddress(s_dll, "aushim_draw_sized");
    p_call_dl    = (int(*)(const void*,uint32_t))GetProcAddress(s_dll, "aushim_call_dl");
    p_arraybase  = (int(*)(uint32_t,uint32_t,uint32_t,unsigned char))GetProcAddress(s_dll, "aushim_set_arraybase");
    p_arraystride= (int(*)(uint32_t,uint32_t))GetProcAddress(s_dll, "aushim_set_arraystride");
    p_dl_arrays  = (int(*)(const uint32_t*,const uint32_t*,const void*,uint32_t))GetProcAddress(s_dll, "aushim_call_dl_with_arrays");
    p_settings_widescreen = (int(*)(int,int))GetProcAddress(s_dll, "aushim_settings_widescreen");
    p_settings_aspect_lock = (int(*)(int))GetProcAddress(s_dll, "aushim_settings_aspect_lock");
    p_settings_netplay = (void(*)(char*,unsigned,char*,unsigned))GetProcAddress(s_dll, "aushim_settings_netplay");
    p_settings_set_netplay = (void(*)(const char*,const char*))GetProcAddress(s_dll, "aushim_settings_set_netplay");
    p_netplay_bind = (void(*)(void*))GetProcAddress(s_dll, "aushim_netplay_bind");
    p_netplay_menu = (void(*)(void))GetProcAddress(s_dll, "aushim_netplay_menu");
    p_display_refresh = (int(*)(void))GetProcAddress(s_dll, "aushim_display_refresh_hz");
    p_set_vsync = (void(*)(int))GetProcAddress(s_dll, "aushim_set_vsync");
    p_music_bar = (void(*)(const char*,int))GetProcAddress(s_dll, "aushim_music_bar");
    p_music_draw = (void(*)(void))GetProcAddress(s_dll, "aushim_music_draw");
    p_table = (void(*)(const void*,int,int))GetProcAddress(s_dll, "aushim_netplay_table");
    p_table_draw = (void(*)(void))GetProcAddress(s_dll, "aushim_netplay_table_draw");
    p_netplay_screen = (void(*)(const MeleeNetplayUi*,int,int))GetProcAddress(s_dll, "aushim_netplay_screen");
    if (!p_init || !p_begin || !p_end) { printf("[au] shim missing exports\n"); return 0; }
    aurora_link_mem1_base = (unsigned char*)mem1_base; aurora_link_mem1_size = mem1_size;
    if (p_setmem) p_setmem(mem1_base, mem1_size);   /* guest RAM stays ours; shim translates */
    if (p_init(w, h) < 0) { printf("[au] aurora_initialize failed\n"); return 0; }
    g_au_ready = 1;
    printf("[au] aurora ready %ux%u\n", w, h);
    return 1;
}

void aurora_link_frame_begin(void)
{
    if (!g_au_ready) return;
    if (p_update) p_update();
    refresh_pads();
    /* Do NOT impose our own vertex/texgen state here.
     *
     * setup_flat was a bring-up helper for the synthetic triangle: it forces
     * GXSetNumTexGens(0) and a POS+CLR0 format. Melee's replayed stream then
     * configures texgens on top of that half-initialised state, and aurora saw
     * a texgen source of 21 -- past GX_TG_COLOR1 (20), i.e. out of range
     * entirely rather than merely unsupported. The game's own stream is the
     * authority on this state. */
    if (p_begin && p_begin()) {g_au_frames++;extern void mods_host_menu(void*);mods_host_menu(s_dll);if(p_settings_menu)p_settings_menu();if(p_netplay_menu)p_netplay_menu();if(p_music_draw)p_music_draw();if(p_table_draw)p_table_draw();}
}

void aurora_link_update(void) { if (g_au_ready && p_update) { p_update(); refresh_pads(); } }
void aurora_link_frame_end(void)   { if (g_au_ready && p_end) p_end(); }
void aurora_link_shutdown(void)    { if (g_au_ready && p_shutdown) { p_shutdown(); g_au_ready = 0; } }

int aurora_link_draw(unsigned char opcode, const void* verts, uint32_t len)
{
    if (!g_au_ready || !p_draw_sized) return 0;
    if (p_draw_sized(opcode, verts, len)) { g_au_draws++; return 1; }
    return 0;
}

/* Replay a whole frame's FIFO into aurora. */
long g_au_dl_bytes = 0;
int aurora_link_call_dl(const void* data, uint32_t len)
{
    if (!g_au_ready || !p_call_dl || !data || !len) return 0;
    if (p_call_dl(data, len)) { g_au_draws++; g_au_dl_bytes += (long)len; return 1; }
    return 0;
}

/* Melee's FIFO carries CP_REG_ARRAYBASE_ID (0xA0|idx) with guest addresses;
 * aurora refuses those and then dies on the first indexed XF load. Rewrite:
 * pull each array base out, re-emit it through aurora's own opcode with a
 * translated host pointer, and NOP the original CP write so the stream aurora
 * finally sees contains nothing it rejects. CP write layout is
 * [0x08][u8 reg][u32 value] = 6 bytes. */
long g_au_arraybases = 0;
static unsigned char* translated_fifo;
static uint32_t translated_capacity;
int aurora_link_call_dl_translated(const unsigned char* fifo, uint32_t len,
                                   unsigned char* scratch, uint32_t mem1_size)
{
    /* Was: a byte-scan over the stream that NOPed out CP arraybase writes and
     * called p_arraybase() out of band. Two faults -- it could not reliably find
     * command boundaries, so 3,168 arraybase writes per run still reached aurora
     * and were rejected; and an out-of-band call loses ordering against the draws
     * that consume the array. gx_translate() walks with reference-verified sizing
     * and rewrites in place instead. */
    extern uint32_t gx_translate(const unsigned char*, uint32_t,
                                 unsigned char*, uint32_t,
                                 unsigned char*, uint32_t);
    extern long g_gxt_arraybase_emitted, g_gxt_dl_inlined, g_gxt_draws;
    extern long g_gxt_texobjs_emitted;
    extern long g_gxt_tluts_emitted;
    extern long g_gxt_display_copies, g_gxt_texture_copies;
    extern long g_gxt_stop_op, g_gxt_stop_at;
    uint32_t n;

    (void)scratch;
    if (!g_au_ready || !p_call_dl || !fifo || !len) return 0;
    if (!aurora_link_mem1_base) return 0;

    /* Output can exceed input: display lists are inlined and each 6-byte CP
     * arraybase becomes a 16-byte GX_AURORA command. Grow until it fits. */
    if (translated_capacity < len * 4u + (1u << 20)) {
        translated_capacity = len * 4u + (1u << 20);
        translated_fifo = (unsigned char*)realloc(translated_fifo, translated_capacity);
    }
    if (!translated_fifo) return 0;

    for (;;) {
        n = gx_translate(fifo, len, aurora_link_mem1_base, mem1_size, translated_fifo, translated_capacity);
        if (n) break;
        if (translated_capacity > (64u << 20)) return 0;          /* refuse to grow forever */
        translated_capacity *= 2u;
        translated_fifo = (unsigned char*)realloc(translated_fifo, translated_capacity);
        if (!translated_fifo) { translated_capacity = 0; return 0; }
    }

    { static int shown = 0;
      if (shown < 3) { shown++;
        printf("[xlat] in=%u out=%u arraybase=%ld texobj=%ld tlut=%ld copy=%ld/%ld dl=%ld draws=%ld stop_op=%ld@%ld\n",
               len, n, g_gxt_arraybase_emitted, g_gxt_texobjs_emitted, g_gxt_tluts_emitted,
               g_gxt_display_copies, g_gxt_texture_copies,
               g_gxt_dl_inlined, g_gxt_draws,
               g_gxt_stop_op, g_gxt_stop_at);
        fflush(stdout); } }

    g_au_arraybases = g_gxt_arraybase_emitted;
    p_call_dl(translated_fifo, n);
    g_au_dl_bytes += (long)n;
    g_au_draws++;
    return 1;
}

/* GXAttr -> CP array index: cpIdx = attr - GX_VA_POS, and GX_VA_POS is 9
 * (GXEnum.h ordering: PNMTXIDX 0, TEX0..7MTXIDX 1-8, POS 9, NRM 10, CLR0 11,
 * ... LIGHT_ARRAY 24). The crash was on array 24 = GX_VA_LIGHT_ARRAY. */
unsigned char g_au_attr_seen[64];
int aurora_link_set_array(uint32_t attr, uint32_t ea, uint32_t size)
{
    /* Print on first sight: the run aborts inside aurora before any summary,
     * so end-of-run reporting never appears. */
    if (attr < 64 && !g_au_attr_seen[attr]) {
        g_au_attr_seen[attr] = 1;
        printf("[au] GXSetArray attr=%u ea=%08X size=%u\n", attr, ea, size);
        fflush(stdout);
    }
    if (!g_au_ready || !p_arraybase) return 0;
    if (attr < 9u) return 0;
    uint32_t cp_idx = attr - 9u;
    if (cp_idx > 0xFu) return 0;
    /* r5 is the STRIDE, not a size: Melee calls the 3-argument SDK signature
     * GXSetArray(attr, base_ptr, stride). aurora's 5-arg form is its own
     * extension. Passing stride as size gave aurora 4-6 byte arrays, so every
     * indexed access past the first vertex was "unmapped". The real extent is
     * not knowable from this call, so bound it by the rest of MEM1. */
    (void)size;
    { extern unsigned char* aurora_link_mem1_base; extern uint32_t aurora_link_mem1_size;
      uint32_t off = ea & 0x0FFFFFFFu;
      uint32_t extent = (off < aurora_link_mem1_size) ? (aurora_link_mem1_size - off) : 0;
      /* Cap the reported size.
       *
       * aurora uploads each array to the GPU in full every frame
       * (command_processor.cpp: push_storage(array.data, array.size)). Reporting
       * "rest of MEM1" meant ~20 MB per array across up to 16 arrays -- hundreds
       * of MB of uploads per frame, which is what killed the process silently.
       * 256 KB is larger than any plausible Melee vertex array and trivially
       * uploadable. */
      if (extent > AU_ARRAY_CAP) extent = AU_ARRAY_CAP;
      if (!extent) return 0;
      /* Queue, do not emit here. GXSetArray is called from the guest thread at
       * arbitrary points, outside aurora_begin_frame -- a GXCallDisplayList
       * issued there has no active pass to attach to. Flush inside the frame,
       * immediately before the FIFO replay. */
      s_pending[cp_idx].ea = ea; s_pending[cp_idx].extent = extent; s_pending[cp_idx].valid = 1;
      return 1; }
    return 0;
}

/* Register a CP array base taken from gx_recomp's parse of the same stream.
 * phys is a physical address (0x0xxxxxxx); the shim masks and translates. */
long g_au_cp_arrays = 0;
int aurora_link_set_array_stride(uint32_t cp_idx, uint32_t stride)
{
    if (!g_au_ready || !p_arraystride || cp_idx > 0xFu || !stride) return 0;
    return p_arraystride(cp_idx, stride);
}

int aurora_link_set_array_cp(uint32_t cp_idx, uint32_t phys)
{
    if (!g_au_ready || !p_arraybase || cp_idx > 0xFu) return 0;
    uint32_t off = phys & 0x0FFFFFFFu;
    uint32_t extent;
    if (off >= aurora_link_mem1_size) return 0;
    extent = aurora_link_mem1_size - off;
    if (extent > AU_ARRAY_CAP) extent = AU_ARRAY_CAP;   /* see set_array: uploaded per frame */
    if (p_arraybase(cp_idx, 0x80000000u | off, extent, 0)) {
        g_au_cp_arrays++; return 1;
    }
    return 0;
}

/* Record an array base seen inside a spliced display list. Registered at frame
 * start with everything else, so it lands inside aurora's frame. */
void aurora_link_note_cp_array(uint32_t cp_idx, uint32_t phys)
{
    if (cp_idx > 0xFu) return;
    uint32_t off = phys & 0x0FFFFFFFu;
    if (off >= aurora_link_mem1_size) return;
    s_pending[cp_idx].ea = 0x80000000u | off;
    s_pending[cp_idx].extent = (aurora_link_mem1_size - off > AU_ARRAY_CAP)
                             ? AU_ARRAY_CAP : (aurora_link_mem1_size - off);
    s_pending[cp_idx].valid = 1;
}

/* Instrument check: draw a known-good triangle through aurora's own GX API.
 * If the window capture shows it, the capture path is sound and a black frame
 * means aurora really drew nothing. If it does not, the capture is lying --
 * which is the failure mode that wasted most of this session. */
int aurora_link_test_triangle(void) { return (g_au_ready && p_tri) ? p_tri() : 0; }

/* Read back what aurora actually rendered, so the screenshot stops describing a
 * buffer aurora never writes. */
int aurora_link_readback(unsigned char* dst, uint32_t* w, uint32_t* h, uint32_t cap)
{
    /* end_frame is asynchronous.  Ensure this readback observes the frame
     * which the caller just ended, not the previous texture contents. */
    if (g_au_ready && p_synchronize) p_synchronize();
    return (g_au_ready && p_readback) ? p_readback(dst, w, h, cap) : 0;
}

void aurora_link_synchronize(void) { if (g_au_ready && p_synchronize) p_synchronize(); }

int aurora_link_audio(const int16_t* samples, uint32_t frames, uint32_t rate) {
    static int (*push)(const int16_t*,uint32_t,uint32_t);
    if (!push && s_dll) push=(void*)GetProcAddress(s_dll,"aushim_audio_push");
    return push ? push(samples,frames,rate) : 0;
}

/* Diagnostic capture only; the Vulkan layer must already be injected. */
#include "../aurora_shim/renderdoc_app.h"
int aurora_link_renderdoc_capture(const char* path) {
#ifdef _WIN32
    HMODULE module=GetModuleHandleA("renderdoc.dll");
#else
    void* module=dlopen("librenderdoc.so", RTLD_NOW|RTLD_NOLOAD);
#endif
    if(!module) return 0;
    pRENDERDOC_GetAPI get=(pRENDERDOC_GetAPI)(void*)GetProcAddress(module,"RENDERDOC_GetAPI");
    RENDERDOC_API_1_1_2* api=NULL;
    if(!get || !get(eRENDERDOC_API_Version_1_1_2,(void**)&api)) return 0;
    api->SetCaptureFilePathTemplate(path);api->TriggerCapture();return 1;
}

/* The room browser picture, composed by the menu code in hooks.c. */
void aurora_link_netplay_table(const void* rgba, int width, int height) {
    if (p_table) p_table(rgba, width, height);
}

/* Snapshot and selection for the native room browser and lobby presentation. */
void aurora_link_netplay_screen(const MeleeNetplayUi* state, int kind, int selection) {
    if (p_netplay_screen) p_netplay_screen(state, kind, selection);
}

/* Render-thread-owned controller UI: only the close request crosses threads. */
int aurora_link_controllers_back(void) {
    static int (*back)(void);
    if (!back && s_dll) back=(void*)GetProcAddress(s_dll,"aushim_controllers_back");
    return back ? back() : 0;
}

int aurora_link_profile_back(void) {
    static int (*back)(void);
    if (!back && s_dll) back=(void*)GetProcAddress(s_dll,"aushim_profile_back");
    return back ? back() : 0;
}
int aurora_link_profile_receive(const char* hash,const unsigned char* pixels,unsigned size) {
    static int (*receive)(const char*,const unsigned char*,unsigned);
    if (!receive && s_dll) receive=(void*)GetProcAddress(s_dll,"aushim_profile_receive");
    return receive ? receive(hash,pixels,size) : 0;
}

void aurora_link_detach_runtime(void) {
 if(p_synchronize)p_synchronize();
 free(translated_fifo);translated_fifo=NULL;translated_capacity=0;
 if(p_netplay_bind)p_netplay_bind(NULL);
 if(p_netplay_screen)p_netplay_screen(NULL,0,0);
 if(p_setmem)p_setmem(NULL,0);
 if(s_dll){
  void (*detach)(void)=(void(*)(void))GetProcAddress(s_dll,"aushim_content_detach");
  if(detach)detach();
  FreeLibrary(s_dll);s_dll=NULL;
 }
 g_au_ready=0;
}
void aurora_link_content_ready(void){
 if(s_dll){void (*ready)(void)=(void(*)(void))GetProcAddress(s_dll,"aushim_content_ready");if(ready)ready();}
}
