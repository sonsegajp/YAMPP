/* MSVC-built shim over Aurora's GX layer.
 *
 * aurora_gx.lib is MSVC/COFF with C++ and the Dawn runtime inside it; the
 * recomp is mingw gcc. Rather than link them directly, keep every C++ symbol
 * behind a DLL boundary and export a stable C surface the MinGW runtime can
 * LoadLibrary. */
#include "aurora_shim.h"
#include "controllers.h"
#include "profile.h"
#include "../controller_input.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/pad.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_properties.h>
#include <stdlib.h>

/* Defined in readback.cpp, where Dawn's C++ adapter types are available. */
int aushim_gpu_is_hardware_vulkan_internal(void);

#if defined(AUSHIM_ENABLE_DIAGNOSTICS)
/* Declared explicitly because this diagnostic-only probe touches the Dolphin
 * facade rather than submitting a translated FIFO. */
void GXSetNumChans(uint8_t num);
void GXSetNumTexGens(uint8_t num);
#endif

AUSHIM_API uint32_t aushim_get_abi_version(void) { return AUSHIM_ABI_VERSION; }
AUSHIM_API int aushim_probe(void) { return 0xA0A0A001; }

#if defined(AUSHIM_ENABLE_DIAGNOSTICS)
AUSHIM_API int aushim_touch_gx(void)
{
    /* Calls into aurora_gx.lib. If this links and runs, the GX layer is
     * reachable across the boundary. */
    GXSetNumChans(1);
    GXSetNumTexGens(1);
    return 1;
}
#endif

static int s_initialized = 0;
static int s_frame_active = 0;
static int s_updated_since_frame = 0;
static int s_quit_requested = 0;
static int s_backend = -1;
/* With more than one copy of the game open, only the one the player is looking
 * at should move: controllers are read through the SDK rather than the window,
 * so they reach every process regardless of focus. Scripted runs are exempt,
 * since a headless capture never has focus at all. */
static int focus_gate_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char* timeline = getenv("MELEE_INPUT");
        const char* hidden = getenv("GCN_AURORA_HIDDEN");
        cached = ((timeline && *timeline) || (hidden && *hidden)) ? 0 : 1;
    }
    return cached;
}

static SDL_Window* s_sdl_window;
static int window_has_focus(void)
{
#ifdef _WIN32
    DWORD owner = 0;
    HWND foreground = GetForegroundWindow();
    if (!foreground) return 0;
    GetWindowThreadProcessId(foreground, &owner);
    return owner == GetCurrentProcessId();
#else
    if (!s_sdl_window) return 1;
    return (SDL_GetWindowFlags(s_sdl_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
#endif
}

static int s_pad_ready = 0;
static int s_pad_snapshot_valid = 0;
static PADStatus s_pad_status[PAD_CHANMAX];

/* Aurora opens its Dawn and GX pipeline databases during initialization, but
 * it assumes the configured directory already exists. SDL_GetPrefPath has
 * not reliably created that directory in the Windows builds used here,
 * leaving both caches disabled and forcing multi-second shader compilation
 * hitches every launch. Keep the path explicit and create it before Aurora
 * touches SQLite. GCN_AURORA_DATA_DIR gives headless/build runs a writable,
 * isolated location; normal launches use the per-user roaming directory. */
static void configure_data_paths(AuroraConfig* cfg, char* path,
                                 size_t path_size)
{
    const char* configured = getenv("GCN_AURORA_DATA_DIR");
    int needed;
    if (configured && configured[0]) {
        needed = snprintf(path, path_size, "%s", configured);
    } else {
#ifdef _WIN32
        const char* appdata = getenv("APPDATA");
        if (!appdata || !appdata[0]) return;
        needed = snprintf(path, path_size, "%s\\Melee PC", appdata);
#else
        const char* xdg = getenv("XDG_DATA_HOME");
        const char* home = getenv("HOME");
        if (xdg && xdg[0])
            needed = snprintf(path, path_size, "%s/melee-pc", xdg);
        else if (home && home[0])
            needed = snprintf(path, path_size, "%s/.local/share/melee-pc", home);
        else return;
#endif
    }
    if (needed < 0 || (size_t)needed >= path_size) {
        fprintf(stderr, "[aurora] data path is too long; caches disabled\n");
        return;
    }
#ifdef _WIN32
    if (!CreateDirectoryA(path, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr,
                "[aurora] could not create data/cache directory '%s' "
                "(win32=%lu)\n",
                path, (unsigned long)GetLastError());
        return;
    }
#else
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr,
                "[aurora] could not create data/cache directory '%s' "
                "(errno=%d)\n",
                path, errno);
        return;
    }
#endif
    cfg->userPath = path;
    cfg->cachePath = path;
}

static int configure_default_keyboard(void)
{
    PADKeyButtonBinding buttons[PAD_BUTTON_COUNT] = {
        {SDL_SCANCODE_X, PAD_BUTTON_A},
        {SDL_SCANCODE_Z, PAD_BUTTON_B},
        {SDL_SCANCODE_C, PAD_BUTTON_X},
        {SDL_SCANCODE_V, PAD_BUTTON_Y},
        {SDL_SCANCODE_RETURN, PAD_BUTTON_START},
        {SDL_SCANCODE_LSHIFT, PAD_TRIGGER_Z},
        {SDL_SCANCODE_Q, PAD_TRIGGER_L},
        {SDL_SCANCODE_E, PAD_TRIGGER_R},
        {SDL_SCANCODE_UP, PAD_BUTTON_UP},
        {SDL_SCANCODE_DOWN, PAD_BUTTON_DOWN},
        {SDL_SCANCODE_LEFT, PAD_BUTTON_LEFT},
        {SDL_SCANCODE_RIGHT, PAD_BUTTON_RIGHT},
    };
    PADKeyAxisBinding axes[PAD_AXIS_COUNT] = {
        {SDL_SCANCODE_D, PAD_AXIS_LEFT_X_POS, 100},
        {SDL_SCANCODE_A, PAD_AXIS_LEFT_X_NEG, 100},
        {SDL_SCANCODE_W, PAD_AXIS_LEFT_Y_POS, 100},
        {SDL_SCANCODE_S, PAD_AXIS_LEFT_Y_NEG, 100},
        {SDL_SCANCODE_L, PAD_AXIS_RIGHT_X_POS, 100},
        {SDL_SCANCODE_J, PAD_AXIS_RIGHT_X_NEG, 100},
        {SDL_SCANCODE_I, PAD_AXIS_RIGHT_Y_POS, 100},
        {SDL_SCANCODE_K, PAD_AXIS_RIGHT_Y_NEG, 100},
        {SDL_SCANCODE_Q, PAD_AXIS_TRIGGER_L, 100},
        {SDL_SCANCODE_E, PAD_AXIS_TRIGGER_R, 100},
    };

    /* This shim is the PC port's input layer, so its documented keyboard map
     * must not depend on whatever keyboard_bindings.dat another Aurora title
     * last wrote.  The previous early return accepted any saved map, including
     * one with Start unbound, which made Enter silently do nothing.  Reapply
     * all twelve buttons and ten axes on every launch, then explicitly enable
     * keyboard merging. Native SDL gamepads remain active alongside it. */
    if (!PADSetKeyButtonBindings(PAD_CHAN0, buttons) ||
        !PADSetKeyAxisBindings(PAD_CHAN0, axes)) {
        return 0;
    }
    PADSetKeyboardActive(PAD_CHAN0, TRUE);
    return 1;
}

/* Bring aurora up.
 *
 * mem1Size/mem2Size are left 0 on purpose: aurora can allocate the GameCube
 * memory regions itself, but this project already owns guest memory through
 * GXRuntime, and aurora warns its allocation is not at 0x80000000 anyway. That
 * makes guest->host pointer translation the shim's job for every GX call that
 * takes a pointer (GXSetArray, GXInitTexObj, display lists).
 *
 * Returns the backend actually selected, or -1 on failure, so the caller can
 * tell "aurora refused" from "aurora came up headless". */
#ifdef AUSHIM_SETTINGS
void aushim_settings_configure(AuroraConfig*);
void aushim_settings_ready(void);
void aushim_netplay_fonts_ready(void);
void aushim_settings_update(void);
float aushim_settings_gain(void);
int aushim_settings_captures_input(void);
int aushim_netplay_captures_input(void);
int aushim_netplay_keyboard_captured(void);
void aushim_mod_release_resources(void);
#endif

AUSHIM_API int aushim_init_backend(unsigned width, unsigned height,
                                    int backend, int allow_cpu)
{
    AuroraConfig cfg;
    char data_path[MAX_PATH];

    if (backend != AUSHIM_BACKEND_VULKAN || allow_cpu) {
        fprintf(stderr,
                "[aurora] unsupported request backend=%d allow_cpu=%d; "
                "this build requires hardware Vulkan\n",
                backend, allow_cpu);
        fflush(stderr);
        return -1;
    }
    if (s_initialized) return s_backend;

    memset(&cfg, 0, sizeof(cfg));
    cfg.appName       = "Yet Another Melee PC Port (YAMPP)";
    configure_data_paths(&cfg, data_path, sizeof(data_path));
    cfg.desiredBackend = BACKEND_VULKAN;
    cfg.windowWidth   = width  ? width  : 640;
    cfg.windowHeight  = height ? height : 480;
    cfg.msaa          = 1;
    cfg.maxTextureAnisotropy = 1;
    /* vsync OFF: the emulation core already paces to 60Hz (GCN_THROTTLE),
     * and stacking swapchain vsync on top makes every frame that misses a
     * vblank wait for the next one â€” on a visible window this snapped
     * delivered frame times from ~17-25ms capacity to ~46ms (measured in
     * the user's sessions), while occluded bench windows dodged the wait
     * and hid the whole effect.  One pacer, not two.  GCN_AURORA_VSYNC=1
     * re-enables for A/B. */
    {
        const char* vs = getenv("GCN_AURORA_VSYNC");
        cfg.vsync = (vs && vs[0] == '1');
    }
    cfg.allowCpuAdapter = false;
    /* XInput/controller input regardless of window focus: SDL's gamepad
     * subsystem (XInput-backed on Windows) is already initialized by
     * aurora; without this hint it drops events whenever the game window
     * loses focus, which reads as "my controller doesn't work". */
    cfg.allowJoystickBackgroundEvents = true;
    cfg.pauseOnFocusLost = false;
    cfg.mem1Size      = 0;
    cfg.mem2Size      = 0;

#ifdef AUSHIM_SETTINGS
    aushim_settings_configure(&cfg);
#endif
    char  arg0[] = "Yet Another Melee PC Port (YAMPP)";
    char* argv[] = { arg0, NULL };
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "1");
    AuroraInfo info = aurora_initialize(1, argv, &cfg);
    s_sdl_window = (SDL_Window*)info.window;

    /* GCN_AURORA_HIDDEN=1: keep the window off the user's desktop for
     * diagnostic/bench sessions.  SDL_HideWindow makes the Vulkan surface
     * non-presentable (begin_frame fails and REQUIRE_VULKAN aborts), so the
     * window instead moves far off-screen and drops its taskbar button:
     * fully invisible to the user, still a live presentable swapchain. */
    {
        const char* hidden = getenv("GCN_AURORA_HIDDEN");
#ifdef _WIN32
        if (hidden && hidden[0] == '1' && info.window) {
            HWND hwnd = (HWND)SDL_GetPointerProperty(
                SDL_GetWindowProperties((SDL_Window*)info.window),
                SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
            if (hwnd) {
                /* Move only â€” no style/frame changes.  WS_EX_TOOLWINDOW +
                 * SWP_FRAMECHANGED forced a frame-metric recalc that
                 * reconfigured the swapchain mid-run and killed the first
                 * readback (device lost).  An off-screen position alone
                 * keeps the surface untouched; the taskbar button that
                 * remains points at a window nobody can see. */
                SetWindowPos(hwnd, NULL, -32000, -32000, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        } else if (info.window) {
            SDL_SetWindowPosition((SDL_Window*)info.window,
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            SDL_ShowWindow((SDL_Window*)info.window);
        }
#else
        if (hidden && hidden[0] == '1' && info.window) {
            SDL_SetWindowPosition((SDL_Window*)info.window, -32000, -32000);
        } else if (info.window) {
            SDL_SetWindowPosition((SDL_Window*)info.window,
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            SDL_ShowWindow((SDL_Window*)info.window);
        }
#endif
    }

    /* Do not call GXInit here. It emits a large synthetic SDK default-state
     * stream which contaminates the first guest frame. The production path is
     * raw translated FIFO; Aurora's command processor owns decoded GX state. */
    if (info.backend != BACKEND_VULKAN ||
        !aushim_gpu_is_hardware_vulkan_internal()) {
        fprintf(stderr,
                "[aurora] refusing backend=%d: Vulkan hardware adapter required\n",
                (int)info.backend);
        fflush(stderr);
        aurora_shutdown();
        return -1;
    }
    if (!PADInit()) {
        fprintf(stderr, "[aurora] PADInit failed\n");
        fflush(stderr);
        aurora_shutdown();
        return -1;
    }
    if (!configure_default_keyboard()) {
        fprintf(stderr, "[aurora] default keyboard mapping failed\n");
        fflush(stderr);
        aurora_shutdown();
        return -1;
    }

    aushim_controllers_init();
    aushim_profile_init();
    s_initialized = 1;
#ifdef AUSHIM_SETTINGS
    aushim_settings_ready();
    aushim_netplay_fonts_ready();
#endif
    s_frame_active = 0;
    s_updated_since_frame = 0;
    s_quit_requested = 0;
    s_backend = (int)info.backend;
    s_pad_ready = 1;
    s_pad_snapshot_valid = 0;
    printf("[aurora] backend=%d window=%p size=%ux%u\n",
           (int)info.backend, (void*)info.window,
           info.windowSize.width, info.windowSize.height);
    fflush(stdout);
    return (int)info.backend;
}

AUSHIM_API int aushim_init(unsigned width, unsigned height)
{
    return aushim_init_backend(width, height, AUSHIM_BACKEND_VULKAN, 0);
}

/* ---- guest memory window -------------------------------------------------
 * aurora was initialised with mem1Size=0, so guest RAM stays owned by
 * GXRuntime. Every GX call that takes a pointer needs translating from a guest
 * effective address (0x80xxxxxx cached / 0xC0xxxxxx uncached) to the host
 * pointer the frontend allocated. The frontend hands us the base once. */
static unsigned char* s_mem1 = 0;
static uint32_t       s_mem1_size = 0;

AUSHIM_API void aushim_set_mem1(void* host_base, uint32_t size)
{
    s_mem1 = (unsigned char*)host_base;
    s_mem1_size = size;
}

AUSHIM_API void* aushim_guest_to_host(uint32_t ea)
{
    uint32_t off = ea & 0x0FFFFFFFu;
    if (!s_mem1 || off >= s_mem1_size) return 0;
    return s_mem1 + off;
}

/* ---- frame driving -------------------------------------------------------
 * aurora owns the window and the begin/end frame pair. Expose them so the
 * frontend decides the cadence rather than aurora driving the guest. */
AUSHIM_API int aushim_is_initialized(void) { return s_initialized; }
AUSHIM_API int aushim_is_frame_active(void) { return s_frame_active; }
AUSHIM_API int aushim_quit_requested(void) { return s_quit_requested; }

AUSHIM_API int aushim_update_status(void)
{
    const AuroraEvent* event;
    if (!s_initialized) return -1;
    if (s_frame_active) return s_quit_requested;
#ifdef AUSHIM_SETTINGS
    aushim_settings_update();
#endif
    event = aurora_update();
    s_updated_since_frame = 1;
    if (event) {
        while (event->type != AURORA_NONE) {
            if (event->type == AURORA_EXIT) s_quit_requested = 1;
            ++event;
        }
    }
    if (s_pad_ready) {
        (void)PADRead(s_pad_status);
        s_pad_snapshot_valid = 1;
    }
    aushim_controllers_update();
    aushim_profile_update();
    return s_quit_requested;
}

AUSHIM_API void aushim_update(void) { (void)aushim_update_status(); }

AUSHIM_API int aushim_begin_frame(void)
{
    int began;
    if (!s_initialized || s_frame_active || s_quit_requested) return 0;
    /* Preserve Aurora's documented update-before-begin lifecycle even for a
     * consumer which has not yet adopted the explicit update export. */
    if (!s_updated_since_frame && aushim_update_status() < 0) return 0;
    if (s_quit_requested) return 0;
    s_updated_since_frame = 0;
    began = aurora_begin_frame() ? 1 : 0;
    s_frame_active = began;
    return began;
}

AUSHIM_API void aushim_end_frame(void)
{
    if (!s_initialized || !s_frame_active) return;
    extern void aushim_content_draw(void),aushim_content_frame_done(void);
    aushim_content_draw();
    aurora_end_frame();
    s_frame_active = 0;
    aushim_content_frame_done();
}

AUSHIM_API int aushim_poll_pad(uint32_t port, AushimPadStatus* out_status)
{
    const PADStatus* src;
    if (!s_initialized || !s_pad_ready || port >= PAD_CHANMAX ||
        !out_status) {
        return 0;
    }
#ifdef AUSHIM_SETTINGS
    if (aushim_settings_captures_input() || aushim_netplay_captures_input()) { memset(out_status,0,sizeof(*out_status));out_status->error=PAD_ERR_NONE;return 1; }
#endif
    if (focus_gate_enabled() && !window_has_focus()) {
        memset(out_status, 0, sizeof(*out_status));
        out_status->error = PAD_ERR_NONE;
        return 1;
    }
    /* Input must not be sampled only at frame boundaries: shader compilation
     * or a heavy scene can make one rendered frame take long enough to miss a
     * Start tap entirely. Pump SDL and refresh the SDK pad snapshot whenever
     * the runtime's independent 120 Hz input clock asks for it. */
    SDL_PumpEvents();
#ifdef AUSHIM_SETTINGS
    /* Text input must not press gameplay keys, while real controllers retain
     * A/B confirmation. Restore every keyboard binding after this snapshot. */
    int name_keyboard = aushim_netplay_keyboard_captured();
    BOOL keyboard_active[PAD_CHANMAX] = {0};
    if (name_keyboard) for (unsigned i=0; i<PAD_CHANMAX; ++i) {
        keyboard_active[i] = aushim_controller_keyboard_enabled(i);
        PADSetKeyboardActive(i,FALSE);
    }
#endif
    (void)PADRead(s_pad_status);
#ifdef AUSHIM_SETTINGS
    if (name_keyboard) for (unsigned i=0; i<PAD_CHANMAX; ++i) PADSetKeyboardActive(i,keyboard_active[i]);
#endif
    s_pad_snapshot_valid = 1;
    src = &s_pad_status[port];
    memset(out_status, 0, sizeof(*out_status));
    out_status->buttons = src->button & 0x1fffu;
    out_status->stick_x = src->stickX;
    out_status->stick_y = src->stickY;
    out_status->cstick_x = src->substickX;
    out_status->cstick_y = src->substickY;
    out_status->trigger_left = src->triggerLeft;
    out_status->trigger_right = src->triggerRight;
    out_status->analog_a = src->analogA;
    out_status->analog_b = src->analogB;
    out_status->error = src->err;
#ifdef AUSHIM_SETTINGS
    if (!aushim_netplay_keyboard_captured())
#endif
        aushim_controller_keyboard_fallback(port, out_status);
    if (!aushim_controller_tap_jump(port)) out_status->buttons |= YAMPP_PAD_TAP_JUMP_OFF;
    if (aushim_controllers_capturing() || aushim_profile_capturing()) {
        /* Host UI consumes navigation; C-stick still moves the native camera. */
        out_status->buttons=0;out_status->stick_x=out_status->stick_y=0;
        out_status->trigger_left=out_status->trigger_right=0;
    }
#if defined(TARGET_PC)
    out_status->extended_buttons = src->extButton;
#endif
    return 1;
}

/* ---- first triangle ------------------------------------------------------
 * Flat colour, no Tev or textures: the smallest thing that proves geometry
 * reaches the GPU through aurora. Vertex data is written the way Melee writes
 * it -- straight after GXBegin -- rather than via GXPosition* calls. */
#if defined(AUSHIM_ENABLE_DIAGNOSTICS)
AUSHIM_API int aushim_draw_test_triangle(void)
{
    /* Match aurora's documented loop.  The old control path bypassed update(),
     * so it never performed first-frame input/window initialization. */
    aushim_update();
    if (!aushim_begin_frame()) return 0;
    /* Initialize the Dolphin facade only inside this synthetic diagnostic
     * frame. Production initialization never injects SDK GX defaults. */
    GXInit(NULL, 0);
    {
      /* GXInit establishes model/view and viewport defaults, but it does not
       * establish a projection matrix.  Make this a genuinely self-contained
       * control draw instead of relying on stale game state. */
      const float identity[3][4] = {
        { 1.0f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f, 0.0f },
      };
      const float projection[4][4] = {
        { 1.0f, 0.0f,  0.0f, 0.0f },
        { 0.0f, 1.0f,  0.0f, 0.0f },
        { 0.0f, 0.0f, -1.0f, 0.0f },
        { 0.0f, 0.0f,  0.0f, 1.0f },
      };
      GXLoadPosMtxImm(identity, GX_PNMTX0);
      GXSetCurrentMtx(GX_PNMTX0);
      GXSetProjection(projection, GX_ORTHOGRAPHIC);
      GXSetViewport(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
      GXSetScissor(0, 0, 640, 480);
      GXSetCullMode(GX_CULL_NONE);
      GXSetCopyClear((GXColor){ 16, 32, 160, 255 }, GX_MAX_Z24);
    }
    GXSetNumChans(1);
    GXSetNumTexGens(0);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
                  GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanMatColor(GX_COLOR0A0, (GXColor){ 240, 64, 32, 255 });
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL,
                  GX_COLOR0A0);
    /* Use a TEV register constant so the control tests geometry without
     * depending on vertex-color/channel interpolation state. */
    GXSetTevColor(GX_TEVREG0, (GXColor){ 240, 64, 32, 255 });
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO,
                    GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO,
                    GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    /* Input selectors and operations share the same BP registers. Set the
     * selectors last so this control also catches stale SDK-side register
     * shadows while Aurora's direct GX facade is being brought up. */
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO,
                    GX_CC_ZERO, GX_CC_C0);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO,
                    GX_CA_ZERO, GX_CA_A0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
      GXPosition3f32(-0.5f, -0.5f, 0.0f);
      GXPosition3f32( 0.5f, -0.5f, 0.0f);
      GXPosition3f32( 0.0f,  0.5f, 0.0f);
    GXEnd();
    aushim_end_frame();
    { static unsigned submitted = 0;
      if (submitted++ < 3) {
        printf("[shim] triangle submitted\n");
        fflush(stdout);
      } }
    return 1;
}
#endif

static SDL_AudioStream* s_audio_stream;
static uint32_t s_audio_rate;
/* Opt-in playback timing, separate from guest PCM capture. The device callback
 * only updates atomics; disk I/O stays on the producer thread. */
static FILE* s_audio_stats;
static int s_audio_stats_checked;
static SDL_AtomicInt s_audio_requests, s_audio_short_requests, s_audio_missing;
static uint64_t s_audio_push_frames, s_audio_last_push, s_audio_stats_next, s_audio_max_gap;
static void SDLCALL audio_observe_pull(void* user, SDL_AudioStream* stream, int extra, int total) {
    (void)user; (void)stream; (void)total;
    SDL_AddAtomicInt(&s_audio_requests,1);
    if(extra>0){SDL_AddAtomicInt(&s_audio_short_requests,1);SDL_AddAtomicInt(&s_audio_missing,extra);}
}
static void audio_observe_push(uint32_t frames) {
    if(!s_audio_stats)return;
    uint64_t now=SDL_GetTicksNS(),gap=s_audio_last_push?now-s_audio_last_push:0;
    if(gap>s_audio_max_gap)s_audio_max_gap=gap;
    s_audio_last_push=now;s_audio_push_frames+=frames;
    if(now>=s_audio_stats_next){
        fprintf(s_audio_stats,"%llu,%d,%llu,%d,%d,%d,%llu\n",(unsigned long long)now,
            SDL_GetAudioStreamQueued(s_audio_stream),(unsigned long long)s_audio_push_frames,
            SDL_GetAtomicInt(&s_audio_requests),SDL_GetAtomicInt(&s_audio_short_requests),
            SDL_GetAtomicInt(&s_audio_missing),(unsigned long long)s_audio_max_gap);
        fflush(s_audio_stats);s_audio_max_gap=0;s_audio_stats_next=now+500000000;
    }
}
AUSHIM_API int aushim_audio_push(const int16_t* samples, uint32_t frames, uint32_t rate) {
    if (!s_initialized || !samples || !frames) return 0;
    if(!s_audio_stats_checked){
        const char* path=getenv("MELEE_AUDIO_STATS");s_audio_stats_checked=1;
        if(path&&*path)s_audio_stats=fopen(path,"w");
        if(s_audio_stats)fprintf(s_audio_stats,"ns,queued_bytes,pushed_frames,requests,short_requests,missing_bytes,max_push_gap_ns\n");
    }
    if (!s_audio_stream || s_audio_rate!=rate) {
        if (s_audio_stream) SDL_DestroyAudioStream(s_audio_stream);
        s_audio_stream=NULL;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return 0;
        SDL_AudioSpec spec={0}; spec.format=SDL_AUDIO_S16; spec.channels=2; spec.freq=(int)rate;
        s_audio_stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
        if (!s_audio_stream) { fprintf(stderr,"[audio] SDL device: %s\n",SDL_GetError());return 0; }
        s_audio_rate=rate;
        if(s_audio_stats)SDL_SetAudioStreamGetCallback(s_audio_stream,audio_observe_pull,NULL);
        SDL_ResumeAudioStreamDevice(s_audio_stream);
        fprintf(stderr,"[audio] SDL playback %u Hz stereo\n",rate);
    }
#ifdef AUSHIM_SETTINGS
    SDL_SetAudioStreamGain(s_audio_stream,aushim_settings_gain());
#endif
    audio_observe_push(frames);
    return SDL_PutAudioStreamData(s_audio_stream,samples,(int)(frames*4))?1:0;
}

AUSHIM_API void aushim_shutdown(void)
{
    if (!s_initialized) return;
    if (s_frame_active) aushim_end_frame();
    aushim_synchronize();
    if (s_audio_stream) { SDL_DestroyAudioStream(s_audio_stream); s_audio_stream=NULL; }
    if(s_audio_stats){fclose(s_audio_stats);s_audio_stats=NULL;}
    #ifdef AUSHIM_SETTINGS
    aushim_mod_release_resources();
    #endif
    { extern void aushim_content_release(void);aushim_content_release(); }
    aurora_shutdown();
    s_initialized = 0;
    s_frame_active = 0;
    s_updated_since_frame = 0;
    s_quit_requested = 0;
    s_backend = -1;
    s_pad_ready = 0;
    s_pad_snapshot_valid = 0;
    memset(s_pad_status, 0, sizeof(s_pad_status));
    s_mem1 = NULL;
    s_mem1_size = 0;
}

/* ---- Melee's real geometry path ------------------------------------------
 * Melee never calls GXPosition* -- it writes vertex data straight to WGPIPE
 * after GXBegin, exactly as on hardware (confirmed: GXPosition* is absent from
 * the 105 GX entry points it calls). aurora accepts that shape directly via the
 * GX_AURORA_DRAW_SIZED subcommand:
 *
 *   [u8 GX_AURORA=0x50][u16 0x0040][u8 vtxfmt|prim][u32 byte_len][bytes...]
 *
 * Display-list streams are big-endian (GameCube convention), so the u16/u32
 * headers are written MSB first. */
static void put_u16be(unsigned char* p, unsigned v) { p[0]=(unsigned char)(v>>8); p[1]=(unsigned char)v; }
static void put_u32be(unsigned char* p, uint32_t v) { p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16); p[2]=(unsigned char)(v>>8); p[3]=(unsigned char)v; }

AUSHIM_API int aushim_draw_sized(unsigned char draw_opcode,
                                 const void* verts, uint32_t len)
{
    if (!verts || !len) return 0;
    uint32_t hdr = 1 + 2 + 1 + 4;
    unsigned char* dl = (unsigned char*)malloc(hdr + len);
    if (!dl) return 0;
    dl[0] = GX_AURORA;
    put_u16be(dl + 1, GX_AURORA_DRAW_SIZED);
    dl[3] = draw_opcode;                 /* vtxfmt | prim, as GXBegin would emit */
    put_u32be(dl + 4, len);
    memcpy(dl + hdr, verts, len);
    if (!aushim_submit_fifo(dl, hdr + len)) {
        free(dl);
        return 0;
    }
    free(dl);
    return 1;
}

/* Same, but the vertex bytes live in guest memory: translate first. */
AUSHIM_API int aushim_draw_sized_guest(unsigned char draw_opcode,
                                       uint32_t guest_ea, uint32_t len)
{
    void* host = aushim_guest_to_host(guest_ea);
    if (!host) { printf("[shim] draw_sized: guest %08X not mapped\n", guest_ea); return 0; }
    return aushim_draw_sized(draw_opcode, host, len);
}

/* Vertex descriptor/format for the DRAW_SIZED test: direct POS(f32 XYZ) +
 * CLR0(RGBA8), flat colour, no Tev stages beyond the default. */
#if defined(AUSHIM_ENABLE_DIAGNOSTICS)
AUSHIM_API int aushim_setup_flat(void)
{
    GXSetNumChans(1);
    GXSetNumTexGens(0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    return 1;
}
#endif

/* Hand aurora a raw GX command stream.
 *
 * Melee's WGPIPE traffic is already a GX display list -- CP/XF/BP register
 * loads interleaved with draw opcodes -- which is precisely what
 * GXCallDisplayList consumes. So the whole frame's FIFO can be replayed in one
 * call, rather than reconstructing individual DRAW_SIZED batches and having to
 * re-derive vertex sizes from the descriptor state. */
AUSHIM_API int aushim_submit_fifo(const void* data, uint32_t len)
{
    if (!s_initialized || !s_frame_active || !data || !len) return 0;
    GXCallDisplayList(data, len);
    return 1;
}

AUSHIM_API int aushim_call_dl(const void* data, uint32_t len)
{
    return aushim_submit_fifo(data, len);
}

/* Diagnostic: when the process is running under RenderDoc's injected layer,
 * queue a capture of the next presented frame through the in-application API
 * (official renderdoc_app.h, vendored beside this file).  Returns 0 when
 * RenderDoc is not present.  Never active in normal runs. */
#include "renderdoc_app.h"
AUSHIM_API int aushim_renderdoc_trigger(void)
{
    static RENDERDOC_API_1_1_2* s_api;
    if (!s_api) {
#ifdef _WIN32
        HMODULE rdoc = GetModuleHandleA("renderdoc.dll");
        pRENDERDOC_GetAPI get_api;
        if (!rdoc) return 0;
        get_api = (pRENDERDOC_GetAPI)(void*)GetProcAddress(rdoc, "RENDERDOC_GetAPI");
#else
        void* rdoc = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
        pRENDERDOC_GetAPI get_api;
        if (!rdoc) return 0;
        get_api = (pRENDERDOC_GetAPI)dlsym(rdoc, "RENDERDOC_GetAPI");
#endif
        if (!get_api ||
            !get_api(eRENDERDOC_API_Version_1_1_2, (void**)&s_api))
            return 0;
    }
    s_api->TriggerCapture();
    return 1;
}

/* Narrow ordering fence: publish everything submitted so far and block until
 * Aurora's FIFO worker has processed it.  This is the visibility point for
 * machine-side PE token/finish latches; it does no presentation and no
 * framebuffer readback. */
AUSHIM_API int aushim_fifo_drain(void)
{
    if (!s_initialized || !s_frame_active) return 0;
    AuroraGXSync();
    return 1;
}

/* Set a vertex array base the way aurora requires.
 *
 * Encoding taken from aurora's own GXSetArray (lib/dolphin/gx/GXGeometry.cpp):
 *   GX_WRITE_AURORA(GX_AURORA_LOAD_ARRAYBASE | cpIdx)
 *   u64 host address, u32 size, u8 little-endian flag
 * and confirmed against its consumer in lib/gx/command_processor.cpp, which
 * reads exactly that sequence. Melee instead writes CP_REG_ARRAYBASE_ID with a
 * *guest* address, which aurora rejects outright -- this is the translation. */
AUSHIM_API int aushim_set_arraybase(uint32_t cp_idx, uint32_t guest_ea,
                                    uint32_t size, unsigned char le)
{
    void* host = aushim_guest_to_host(guest_ea);
    if (!host) return 0;
    unsigned char dl[1 + 2 + 8 + 4 + 1];
    uint64_t a = (uint64_t)(uintptr_t)host;
    dl[0] = GX_AURORA;
    put_u16be(dl + 1, (unsigned)(GX_AURORA_LOAD_ARRAYBASE | (cp_idx & 0xF)));
    for (int i = 0; i < 8; i++) dl[3 + i] = (unsigned char)(a >> (56 - 8 * i));
    put_u32be(dl + 11, size);
    dl[15] = le;
    return aushim_submit_fifo(dl, sizeof dl);
}

/* Set a vertex array stride (CP_REG_ARRAYSTRIDE_ID | idx = 0xB0 | idx).
 *
 * aurora's own GXSetArray writes base *and* stride; our registration path only
 * ever wrote the base. With a stale or zero stride, an indexed fetch computes
 * base + index*stride and lands anywhere -- which is an out-of-bounds read on
 * geometry that is otherwise perfectly well-formed. Unlike the base, the stride
 * is a plain CP register write aurora accepts as-is. */
AUSHIM_API int aushim_set_arraystride(uint32_t cp_idx, uint32_t stride)
{
    unsigned char dl[6];
    dl[0] = 0x08;                          /* GX_LOAD_CP_REG */
    dl[1] = (unsigned char)(0xB0u | (cp_idx & 0xFu));
    put_u32be(dl + 2, stride);
    return aushim_submit_fifo(dl, sizeof dl);
}

/* Submit array bases and a command stream as ONE display list.
 *
 * bases[] holds up to 16 (guest_ea, size) pairs; slot k is skipped when its
 * ea is 0. Emitting these as their own GXCallDisplayList left the arrays empty
 * by the time a spliced draw executed -- concatenating removes the second
 * submission entirely. */
AUSHIM_API int aushim_call_dl_with_arrays(const uint32_t* eas,
                                          const uint32_t* sizes,
                                          const void* stream, uint32_t len)
{
    unsigned char hdr[16 * 16];
    uint32_t hn = 0;
    if (!s_initialized || !s_frame_active || !eas || !sizes) return 0;
    { static int shown = 0;
      if (!shown) { shown = 1;
        for (uint32_t k = 0; k < 16u; k++)
            if (eas[k]) printf("[base] slot %2u ea=%08X size=%u -> host=%p\n",
                               k, eas[k], sizes[k], aushim_guest_to_host(eas[k]));
        fflush(stdout); } }
    for (uint32_t k = 0; k < 16u; k++) {
        void* host;
        if (!eas[k]) continue;
        host = aushim_guest_to_host(eas[k]);
        if (!host) continue;
        {   uint64_t a = (uint64_t)(uintptr_t)host;
            unsigned char* d = hdr + hn;
            d[0] = GX_AURORA;
            put_u16be(d + 1, (unsigned)(GX_AURORA_LOAD_ARRAYBASE | (k & 0xF)));
            for (int i = 0; i < 8; i++) d[3 + i] = (unsigned char)(a >> (56 - 8 * i));
            put_u32be(d + 11, sizes[k]);
            d[15] = 0;
            hn += 16;
        }
    }
    if (!stream || !len) return hn ? aushim_submit_fifo(hdr, hn) : 1;
    {   unsigned char* all = (unsigned char*)malloc(hn + len);
        if (!all) return 0;
        if (hn) memcpy(all, hdr, hn);
        memcpy(all + hn, stream, len);
        if (!aushim_submit_fifo(all, hn + len)) {
            free(all);
            return 0;
        }
        free(all);
    }
    return 1;
}

AUSHIM_API void aushim_content_detach(void){
 extern void aushim_content_capture(void);
 extern void aushim_room_name_bind(int (*)(int,const char*));
 extern void aushim_profile_bind(void (*)(const char*,const unsigned char*,unsigned,const char*));
 aushim_content_capture();
 aushim_room_name_bind(NULL);aushim_profile_bind(NULL);
 if(s_audio_stream)SDL_ClearAudioStream(s_audio_stream);
 s_pad_snapshot_valid=0;s_updated_since_frame=0;
}

AUSHIM_API uintptr_t aushim_content_window_id(void){
#ifdef _WIN32
 return s_sdl_window?(uintptr_t)SDL_GetPointerProperty(SDL_GetWindowProperties(s_sdl_window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,NULL):0;
#else
 return (uintptr_t)s_sdl_window;
#endif
}
