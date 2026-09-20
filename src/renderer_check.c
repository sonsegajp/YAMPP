/* Synthetic renderer control; this is not the game executable. */
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/vi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int melee_write_frame(const char* path);
int melee_prepare_user_paths(void);
#ifdef MELEE_HSD_CONTROL
float DrawASCII(int chr, float x, float y, GXColor* color);
#endif
static void logger(AuroraLogLevel level, const char* module, const char* message, unsigned int length) {
    fprintf(stderr, "[%s] %.*s\n", module, (int)length, message);
    if (level == LOG_FATAL) { fflush(stderr); abort(); }
}
static void draw(void) {
    Mtx model;
    Mtx44 projection;
    PSMTXIdentity(model);
    C_MTXOrtho(projection, 1, -1, -1, 1, 0, 10);
    GXSetProjection(projection, GX_ORTHOGRAPHIC);
    GXLoadPosMtxImm(model, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetViewport(0, 0, 640, 480, 0, 1);
    GXSetScissor(0, 0, 640, 480);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX,
                  GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GXPosition3f32(-0.8f, -0.7f, -1); GXColor4u8(255, 50, 40, 255);
    GXPosition3f32(0.8f, -0.7f, -1); GXColor4u8(40, 255, 80, 255);
    GXPosition3f32(0, 0.8f, -1); GXColor4u8(50, 100, 255, 255);
    GXEnd();

#ifdef MELEE_HSD_CONTROL
    /* Exercise the game's original line-font renderer against Aurora. */
    C_MTXOrtho(projection, 0, 480, 0, 640, -1, 10);
    GXSetProjection(projection, GX_ORTHOGRAPHIC);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetLineWidth(12, GX_TO_ZERO);
    GXColor text_color = {240, 240, 255, 255};
    float text_x = 100;
    const char* text = "MELEE HSD SOURCE RENDER CHECK";
    for (; *text; ++text) text_x += DrawASCII(*text, text_x, 38, &text_color);
#endif

}
int main(int argc, char** argv) {
    unsigned frames = 180;
    const char* capture = "renderer-check.ppm";
    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i+1<argc) frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--capture") && i+1<argc) capture = argv[++i];
        else { fprintf(stderr, "Usage: %s [--frames N] [--capture path.ppm]\n", argv[0]); return 2; }
    }
    if (!melee_prepare_user_paths()) { fprintf(stderr, "Cannot create user/cache\n"); return 1; }
    AuroraConfig config = {0};
    config.appName = "Melee PC - Aurora renderer check";
    config.userPath = "user";
    config.cachePath = "user/cache";
    config.desiredBackend = BACKEND_VULKAN;
    config.windowWidth = 640; config.windowHeight = 480;
    config.msaa = 1; config.vsync = true;
    config.logCallback = logger; config.logLevel = LOG_INFO;
    AuroraInfo info = aurora_initialize(argc, argv, &config);
    fprintf(stderr, "Aurora backend=%d\n", info.backend);
    VIInit();
    VIConfigure(&GXNtsc480IntDf);
    GXInit(NULL, 0);
    GXSetCopyClear((GXColor){16, 20, 32, 255}, GX_MAX_Z24);
    bool exiting = false;
    unsigned rendered = 0;
    while (!exiting && rendered < frames) {
        const AuroraEvent* event = aurora_update();
        for (; event && event->type != AURORA_NONE; ++event)
            if (event->type == AURORA_EXIT) exiting = true;
        if (exiting || !aurora_begin_frame()) continue;
        draw();
        aurora_end_frame();
        ++rendered;
    }
    int ok = rendered > 0 && melee_write_frame(capture);
    fprintf(stderr, "Rendered %u frames; capture=%s success=%d\n", rendered, capture, ok);
    aurora_shutdown();
    return ok ? 0 : 1;
}
