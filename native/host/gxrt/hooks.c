/* Host-side extensions to the original menus. Nothing here draws its own UI:
 * every screen is Melee's own panel, cursor rows and font.
 *
 *   - every character is reported unlocked,
 *   - "Online" is appended to the main menu, with "Mod Browser" inside its submenu,
 *     built from the original menu panel and cursors (menu kinds 10 and 11,
 *     unused by the game),
 *   - the hidden Options slot becomes "Widescreen", reusing the Language
 *     submenu presentation with its two squares relabelled 4:3 and 16:9.
 *
 * Row labels are composed from glyph columns of the disc's own label textures
 * (tools/menu_labels.py); dynamic text uses the game's SIS font through the
 * description line. All guest access happens on the guest thread from dispatch
 * hooks (config/runtime-hooks.xml) or trace_enter.
 */
#include "abi_recompcore.h"
#include "recomp_funcs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_compat.h"
#include "../netplay_ui.h"
#include "menu_labels.inc"
#include "monitor_native.inc"
#include "controller_hover.inc"
#include "costume_art.h"

/* Guest addresses (GALE01 NTSC 1.02). */
#define MENU_FLOW 0x804A04F0u          /* cur_menu u8, prev u8, hovered u16, +8 buttons u64, +0x11 entering_menu */
#define MENU_INPUT_STATE 0x804D6BC8u   /* cooldown u16 */
#define MENU_KIND_TABLE 0x803EB6B0u    /* MenuKindData[0x22], 0x14 bytes each */
#define MENU_MAIN_ANIM 0x803EB3FCu     /* AnimLoopSettings[5], main menu hover preview */
#define CURSOR_MODEL 0x804A0538u       /* StaticModelDesc MenMainCursor_Top */
#define PANEL_MODEL 0x804A0518u        /* StaticModelDesc MenMainPanel_Top */
#define MENU_ARCHIVE 0x804D6BB8u       /* HSD_Archive* mn_804D6BB8 */
#define FN_MENU_INPUTS 0x80229624u
#define FN_SFX 0x80024030u
#define FN_SCENE_EXIT_DATA 0x801A4B9Cu
#define FN_SCENE_EXIT 0x801A4B60u
#define FN_GOBJ_DESTROY 0x80390228u
#define FN_GOBJ_KILL_PROCS 0x80390CD4u
#define FN_ARCHIVE_PUBLIC 0x80380358u
#define FN_LANGUAGE_OPEN 0x8024C5C0u
#define FN_BUILD_MENU 0x8022B3A0u
#define GOBJ_CURRENT 0x804D781Cu       /* HSD_GObj_804D781C */
#define LANGUAGE_CODE_BEGIN 0x8024BFE0u
#define LANGUAGE_CODE_END 0x8024C5C0u

#define MENU_KIND_MAIN 0
#define MENU_KIND_SETTINGS 4
#define MENU_KIND_NETPLAY 10           /* unused in the original menu table */
#define MENU_KIND_ROOM 11              /* unused in the original menu table */
/* A NULL generic menu table does not mean the kind is unused: kind 13 is
   original VS Rules, which has its own constructor. Kinds 14 and 22 have no
   original entry path; preserve every original screen when extending menus. */
#define MENU_KIND_BROWSER 14
#define MENU_KIND_CONTROLLERS 21   /* replaces the original deflicker screen */
#define SEL_SETTINGS_CONTROLLERS 2
#define MENU_KIND_MOD_BROWSER 22
#define SEL_MAIN_NETPLAY 5
#define SEL_ONLINE_MOD_BROWSER 2
#define SEL_SETTINGS_WIDESCREEN 3
#define MENU_INPUT_UP 1
#define MENU_INPUT_DOWN 2
#define MENU_INPUT_LEFT 4
#define MENU_INPUT_RIGHT 8
#define MENU_INPUT_CONFIRM 0x10
#define MENU_INPUT_BACK 0x20
#define MENU_INPUT_X 0x400
#define MENU_INPUT_Y 0x800

/* Panel header slots each menu kind selects, measured from the panel matanim
 * track at each kind's idle loop frame. */
#define HEADER_SLOT_LANGUAGE 14
#define HEADER_SLOT_NETPLAY 4
#define HEADER_SLOT_ROOM 5

extern void func_8015ED8C(Context*);
extern void func_80229938(Context*);
extern void func_8022B3A0(Context*);
extern void func_8022DB10(Context*);
extern void func_8022D104(Context*);
extern void func_8022C010(Context*);
extern void func_8022C128(Context*);
extern void func_8022AFEC(Context*);
extern void func_8024C5C0(Context*);
extern void func_8000ADF4(Context*);
extern void func_8000AE18(Context*);
extern void func_80229894(Context*);
extern void func_80229860(Context*);
extern void func_803A6368(Context*);
extern void func_8039069C(Context*);
extern void func_80342BEC(Context*);
extern void func_80342954(Context*);
extern void func_803676F8(Context*);
extern void func_800307D0(Context*);
extern void func_80369C0C(Context*);
extern void func_8015EDA4(Context*);
extern void func_802FCF38(Context*);
extern void func_80030CFC(Context*);
extern void func_80086A8C(Context*);
extern void func_80369FD8(Context*);
extern void func_80030BBC(Context*);
extern void func_80370E44(Context*);
extern int aurora_link_widescreen(int set, int value);
extern int aurora_link_aspect_lock(int command);
extern int aurora_link_controllers_back(void);
extern int aurora_link_profile_back(void);
extern void aurora_link_netplay_screen(const MeleeNetplayUi*, int, int);
extern int netplay_room_name_input(int, const char*);
extern void netplay_snapshot(MeleeNetplayUi* out);
extern int menu_pose_read(Context* ctx, float out[9]);
extern int netplay_post(int cmd, int arg, const char* text, const NetplayRules* rules);
extern void netplay_scene_event(Context* ctx, uint32_t addr);
extern void netplay_menu_tick(Context* ctx, int* start_request);
extern void netplay_menu_opened(void);

static uint32_t area_base, area_size, area_used;
static uint32_t custom_light_color;
static Context* area_ctx;
static int widescreen_cached = -1;
static int widescreen_get(void);
static int widescreen_mode;
static int pending_settings_hover;
/* A disconnect can unwind from CSS, stage select or a match. Wait for the
 * original main-menu scene to recreate its archive and live think proc before
 * opening our room panel. 1 = requested, 2 = main scene entered. */
static int pending_netplay_return;
void netplay_return_to_lobby(void) { pending_netplay_return = 1; }
static uint32_t label_archive;
static uint32_t bold_image[sizeof label_bold_recipes / sizeof(LabelRecipe)];
static uint32_t row_text_image[8];      /* runtime row labels, redrawn in place */
static uint32_t italic_image[sizeof label_italic_recipes / sizeof(LabelRecipe)];
static uint32_t option_narrow, option_wide;
static uint32_t bold_table, italic_table, parent_italic_table, panel_texanim;
/* Sources stay immutable when menu labels replace entries in the animation tables. */
static uint32_t bold_sources[58], italic_sources[21], parent_italic_sources[5];
static uint32_t mod_browser_label, profile_label;
static int profile_open;
static uint32_t patched_header_slot, patched_header_old;
static uint32_t patched_desc[2], patched_desc_old[2];
static uint32_t monitor_image[2], monitor_quad[2], monitor_material[2];
static uint32_t monitor_joint_desc;
static struct { uint32_t at, old; } patched_monitor[4];
static unsigned patched_monitor_count;
static uint32_t netplay_anim, netplay_descriptions;
static char sis_text[192], sis_shown[192];

/* The original scene owns these camera objects. Keep their exact masks while
 * browser/lobby content replaces layers 4-7; layers 0-3 remain Melee's
 * own light, fog, animated MenMainBack_Top and original menu panel. Never retain a freed camera. */
typedef struct MenuCameraMask {
  uint32_t gobj, high, low;
} MenuCameraMask;
static MenuCameraMask menu_foreground_cameras[2];
static struct { uint32_t gobj, dobj[16], flags[16]; unsigned count; } panel_headers;

static int valid_guest(uint32_t p, uint32_t size) { return p >= 0x80000000u && (uint64_t)p + size <= 0x81800000u; }

static void reset_menu_foreground(void) {
  memset(menu_foreground_cameras, 0, sizeof menu_foreground_cameras);
    memset(&panel_headers, 0, sizeof panel_headers);
}

static void apply_menu_foreground(Context* ctx, unsigned kind, int modal) {
  const uint32_t globals[2] = { 0x804D6BACu, 0x804D6BB0u };
  const uint32_t callbacks[2] = { 0x8022BDB4u, 0x803910D8u };
  int replace = kind == MENU_KIND_CONTROLLERS || kind == MENU_KIND_BROWSER || kind == MENU_KIND_ROOM || kind == MENU_KIND_MOD_BROWSER || (kind == MENU_KIND_NETPLAY && modal);
  for (int i = 0; i < 2; i++) {
    MenuCameraMask* saved = &menu_foreground_cameras[i];
    uint32_t camera = mem_read32(ctx, globals[i]);
    int live = valid_guest(camera, 0x38) && mem_read16(ctx, camera) == 2 &&
               mem_read8(ctx, camera + 2) == 3 &&
               mem_read32(ctx, camera + 0x1C) == callbacks[i];
    if (!live || (saved->gobj && saved->gobj != camera)) memset(saved, 0, sizeof *saved);
    if (!live) continue;
    if (replace) {
      if (!saved->gobj) {
        saved->gobj = camera;
        saved->high = mem_read32(ctx, camera + 0x20);
        saved->low = mem_read32(ctx, camera + 0x24);
        if (getenv("MELEE_TEST_UI_TRACE"))
          fprintf(stderr, "[hooks] native netplay background: camera %d %08X mask %08X:%08X -> 00000000:%08X\n",
                  i, camera, saved->high, saved->low, i ? 0u : saved->low & 15u);
      }
      mem_write32(ctx, camera + 0x20, 0);
      mem_write32(ctx, camera + 0x24, i ? 0u : saved->low & 15u);
    } else if (saved->gobj) {
      mem_write32(ctx, camera + 0x20, saved->high);
      mem_write32(ctx, camera + 0x24, saved->low);
      if (getenv("MELEE_TEST_UI_TRACE"))
        fprintf(stderr, "[hooks] native menu foreground restored: camera %d %08X mask %08X:%08X\n",
                i, camera, saved->high, saved->low);
      memset(saved, 0, sizeof *saved);
    }
  }
}

static uint32_t area_alloc(uint32_t size) {
  size = (size + 31u) & ~31u;
  if (!area_base || area_used + size > area_size) {
    /* Callers degrade by skipping their artwork, which is silent. The area
     * is sized close to what the menus actually use so the remainder stays
     * with the guest heap, so say something if it ever wants more. */
    static int warned;
    if (!warned) { warned = 1; fprintf(stderr, "[hooks] host menu area exhausted: %u of %u bytes used, wanted %u more\n", area_used, area_size, size); }
    return 0;
  }
  uint32_t at = area_base + area_used;
  area_used += size;
  memset(area_ctx->ram + (at & RAM_MASK), 0, size);
  return at;
}

static uint32_t call_guest(Context* ctx, uint32_t fn, uint32_t r3, uint32_t r4, uint32_t r5) {
  RecFn f = lookup_function(fn);
  if (lookup_is_stub(f)) return 0;
  Context saved = *ctx;
  ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5;
  f(ctx);
  uint32_t result = ctx->gpr[3];
  uint64_t tb = ctx->timebase;
  *ctx = saved;
  ctx->timebase = tb;
  return result;
}

/* Some guest calls need more than three arguments, or float arguments. */
static uint32_t call_guest_ex(Context* ctx, uint32_t fn, const uint32_t* gpr, int ngpr, const float* fpr, int nfpr) {
  RecFn f = lookup_function(fn);
  if (lookup_is_stub(f)) return 0;
  Context saved = *ctx;
  for (int i = 0; i < ngpr && i < 8; i++) ctx->gpr[3 + i] = gpr[i];
  for (int i = 0; i < nfpr && i < 8; i++) ctx->fpr[1 + i] = (double)fpr[i];
  f(ctx);
  uint32_t result = ctx->gpr[3];
  uint64_t tb = ctx->timebase;
  *ctx = saved;
  ctx->timebase = tb;
  return result;
}

/* Preorder walk, shared by the JObj tree (child +0x10, next +8) and the
 * MatAnimJoint tree (child +0, next +4). */
static uint32_t nth_node(Context* ctx, uint32_t node, uint32_t child_off, uint32_t next_off, int target, int* counter) {
  for (int guard = 0; valid_guest(node, 0x20) && guard < 4096; guard++, node = mem_read32(ctx, node + next_off)) {
    if ((*counter)++ == target) return node;
    uint32_t found = nth_node(ctx, mem_read32(ctx, node + child_off), child_off, next_off, target, counter);
    if (found) return found;
  }
  return 0;
}

static uint32_t texanim_of(Context* ctx, uint32_t matanim_root, int node_index, unsigned expected_entries) {
  int counter = 0;
  uint32_t node = nth_node(ctx, matanim_root, 0, 4, node_index, &counter);
  if (!node) return 0;
  uint32_t matanim = mem_read32(ctx, node + 8);
  if (!valid_guest(matanim, 0x10)) return 0;
  uint32_t texanim = mem_read32(ctx, matanim + 8);
  if (!valid_guest(texanim, 0x18)) return 0;
  if (mem_read16(ctx, texanim + 0x14) != expected_entries) return 0;
  return valid_guest(mem_read32(ctx, texanim + 0xC), expected_entries * 4) ? texanim : 0;
}

/* GX texel access for the two label formats. */
static unsigned ia4_index(unsigned w, unsigned x, unsigned y) { return ((y / 4) * (w / 8) + x / 8) * 32 + (y % 4) * 8 + (x % 8); }
static unsigned i4_index(unsigned w, unsigned x, unsigned y) { return ((y / 8) * ((w + 7) / 8) + x / 8) * 32 + (y % 8) * 4 + (x % 8) / 2; }
static unsigned texel_get(Context* ctx, uint32_t data, unsigned fmt, unsigned w, unsigned x, unsigned y) {
  if (fmt == 2) return mem_read8(ctx, data + ia4_index(w, x, y));
  unsigned b = mem_read8(ctx, data + i4_index(w, x, y));
  unsigned v = (x & 1) ? (b & 0xF) : (b >> 4);
  return (v << 4) | v;
}
static void texel_set(Context* ctx, uint32_t data, unsigned fmt, unsigned w, unsigned x, unsigned y, unsigned value) {
  if (fmt == 2) { mem_write8(ctx, data + ia4_index(w, x, y), (uint8_t)value); return; }
  uint32_t at = data + i4_index(w, x, y);
  unsigned b = mem_read8(ctx, at), v = value >> 4;
  mem_write8(ctx, at, (uint8_t)((x & 1) ? ((b & 0xF0) | v) : ((b & 0x0F) | (v << 4))));
}
static unsigned texture_bytes(unsigned fmt, unsigned w, unsigned h) {
  if (fmt == 6) return ((w + 3) / 4) * ((h + 3) / 4) * 64;
  return fmt == 2 ? ((w + 7) / 8) * ((h + 3) / 4) * 32 : ((w + 7) / 8) * ((h + 7) / 8) * 32;
}

static uint32_t make_image(Context* ctx, unsigned w, unsigned h, unsigned fmt) {
  uint32_t desc = area_alloc(0x20), data = area_alloc(texture_bytes(fmt, w, h));
  if (!desc || !data) return 0;
  mem_write32(ctx, desc, data); mem_write16(ctx, desc + 4, (uint16_t)w); mem_write16(ctx, desc + 6, (uint16_t)h);
  mem_write32(ctx, desc + 8, fmt); mem_write32(ctx, desc + 0xC, 0);
  mem_write32(ctx, desc + 0x10, 0); mem_write32(ctx, desc + 0x14, 0);
  return desc;
}

static unsigned compose_into(Context* ctx, uint32_t desc, const LabelRecipe* recipe, unsigned w, unsigned h, unsigned fmt, float shear) {
  uint32_t dst = mem_read32(ctx, desc);
  unsigned drawn = 0;
  memset(ctx->ram + (dst & RAM_MASK), 0, texture_bytes(fmt, w, h));
  for (unsigned g = 0; g < recipe->count; g++) {
    const LabelGlyph* glyph = &recipe->glyphs[g];
    uint32_t src_desc = glyph->table == LABEL_TABLE_BOLD ? (glyph->src < 58 ? bold_sources[glyph->src] : 0) :
                        glyph->table == LABEL_TABLE_PARENT_ITALIC ? (glyph->src < 5 ? parent_italic_sources[glyph->src] : 0) :
                        (glyph->src < 21 ? italic_sources[glyph->src] : 0);
    if (!valid_guest(src_desc, 0x18)) continue;
    uint32_t src = mem_read32(ctx, src_desc);
    unsigned sw = mem_read16(ctx, src_desc + 4), sh = mem_read16(ctx, src_desc + 6), sfmt = mem_read32(ctx, src_desc + 8);
    if (sfmt != fmt || sh != h || !valid_guest(src, texture_bytes(sfmt, sw, sh))) continue;
    int span = glyph->u1 - glyph->u0;
    for (unsigned y = 0; y < h; y++) {
      int row_shift = (int)lroundf((float)y * shear);
      for (int k = 0; k < span; k++) {
        int sx = glyph->u0 + k - row_shift;
        if (sx < 0 || sx >= (int)sw) continue;
        unsigned value = texel_get(ctx, src, sfmt, sw, (unsigned)sx, y);
        if (!(value >> 4)) continue;
        unsigned dy = glyph->rot ? (h - 1 - y) : y;
        int dk = glyph->rot ? (span - 1 - k) : k;
        int dx = glyph->dst + dk - (int)lroundf((float)dy * shear);
        if (dx < 0 || dx >= (int)w) continue;
        if ((value >> 4) > (texel_get(ctx, dst, fmt, w, (unsigned)dx, dy) >> 4)) {
          texel_set(ctx, dst, fmt, w, (unsigned)dx, dy, value);
          drawn++;
        }
      }
    }
  }
  return drawn;
}

/* A label composed before its glyph textures are in memory comes out empty, and
   caching that leaves the row blank for good; report it instead so it is built
   again on a later frame. */
static uint32_t compose(Context* ctx, const LabelRecipe* recipe, unsigned w, unsigned h, unsigned fmt, float shear) {
  uint32_t desc = make_image(ctx, w, h, fmt);
  if (!desc) return 0;
  if (!compose_into(ctx, desc, recipe, w, h, fmt, shear)) {
    if (getenv("MELEE_LABEL_LOG")) fprintf(stderr, "[label] %s composed empty\n", recipe->name);
    return 0;
  }
  return desc;
}

/* ---- labels for text only known at runtime ---------------------------------
 * Room names come from the server, so their labels are laid out here from the
 * same glyph columns the baked labels use, following tools/menu_labels.py. The
 * textures are allocated once and redrawn in place, because the patch area is
 * a bump allocator with no way to give memory back. */
#define LABEL_TEXT_MAX 22

/* The menu font has no f, j, k, q, x or z, in either case. Falling back from one
   case to the other has to give up rather than hand back to itself, or a word
   containing one of those letters never returns. */
static const LabelChar* label_char(const LabelChar* table, unsigned count, char ch) {
  for (unsigned i = 0; i < count; i++) if (table[i].ch == ch) return &table[i];
  char other = ch >= 'a' && ch <= 'z' ? (char)(ch - 32)
             : ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : 0;
  if (other)
    for (unsigned i = 0; i < count; i++) if (table[i].ch == other) return &table[i];
  return NULL;
}

/* Width in texels that `text` would take, so two columns can be told apart. */
static int measure_text(const char* text, const LabelChar* table, unsigned count, int gap, int space) {
  int total = 0; unsigned n = 0;
  for (const char* p = text; *p && n < LABEL_TEXT_MAX; p++) {
    const LabelChar* g = NULL;
    if (*p != ' ') { g = label_char(table, count, *p); if (!g) continue; }
    total += (g ? g->u1 - g->u0 : space) + (n ? gap : 0);
    n++;
  }
  return total;
}

/* `align`: 0 centres, -1 pins to the left edge, 1 pins to the right. */
static unsigned layout_text_at(const char* text, const LabelChar* table, unsigned count,
                               int gap, int space, unsigned width, int align,
                               unsigned margin, LabelGlyph* out) {
  const LabelChar* picked[LABEL_TEXT_MAX];
  unsigned n = 0; int total = 0;
  unsigned room = width > margin * 2 ? width - margin * 2 : width;
  for (const char* p = text; *p && n < LABEL_TEXT_MAX; p++) {
    const LabelChar* g = NULL;
    if (*p != ' ') { g = label_char(table, count, *p); if (!g) continue; }
    int advance = (g ? g->u1 - g->u0 : space) + (n ? gap : 0);
    if (total + advance > (int)room) break;
    picked[n++] = g;
    total += advance;
  }
  int cursor = align < 0 ? (int)margin
             : align > 0 ? (int)width - (int)margin - total
             : ((int)width - total) / 2;
  if (cursor < 0) cursor = 0;
  unsigned emitted = 0;
  for (unsigned i = 0; i < n; i++) {
    const LabelChar* g = picked[i];
    if (!g) { cursor += space + gap; continue; }
    out[emitted].table = g->table; out[emitted].src = g->src; out[emitted].rot = g->rot;
    out[emitted].u0 = g->u0; out[emitted].u1 = g->u1; out[emitted].dst = (short)cursor;
    cursor += (g->u1 - g->u0) + gap;
    emitted++;
  }
  return emitted;
}

static unsigned layout_text(const char* text, const LabelChar* table, unsigned count,
                            int gap, int space, unsigned width, LabelGlyph* out) {
  const LabelChar* picked[LABEL_TEXT_MAX];
  unsigned n = 0; int total = 0;
  for (const char* p = text; *p && n < LABEL_TEXT_MAX; p++) {
    const LabelChar* g = NULL;
    if (*p != ' ') { g = label_char(table, count, *p); if (!g) continue; }
    int advance = (g ? g->u1 - g->u0 : space) + (n ? gap : 0);
    if (total + advance > (int)width) break;      /* whatever fits on the row */
    picked[n++] = g;
    total += advance;
  }
  int cursor = ((int)width - total) / 2;
  if (cursor < 0) cursor = 0;
  unsigned emitted = 0;
  for (unsigned i = 0; i < n; i++) {
    const LabelChar* g = picked[i];
    if (!g) { cursor += space + gap; continue; }
    out[emitted].table = g->table; out[emitted].src = g->src; out[emitted].rot = g->rot;
    out[emitted].u0 = g->u0; out[emitted].u1 = g->u1; out[emitted].dst = (short)cursor;
    cursor += (g->u1 - g->u0) + gap;
    emitted++;
  }
  return emitted;
}

/* A row of the browser: the room's name along the left and its state along the
   right of the same label, so the list reads as a table rather than a stack of
   buttons with one word on each. */
static uint32_t text_label_columns(Context* ctx, uint32_t* desc, const char* left, const char* right) {
  LabelGlyph glyphs[LABEL_TEXT_MAX * 2];
  LabelRecipe recipe = { "", glyphs, 0 };
  if (!*desc) { *desc = make_image(ctx, 176, 30, 2); if (!*desc) return 0; }
  const LabelChar* chars = label_bold_chars;
  const unsigned nchars = sizeof label_bold_chars / sizeof(LabelChar);
  int left_w = measure_text(left, chars, nchars, LABEL_BOLD_GAP, LABEL_BOLD_SPACE);
  int right_w = right && *right ? measure_text(right, chars, nchars, LABEL_BOLD_GAP, LABEL_BOLD_SPACE) : 0;
  /* Only room for a value column when the two would not run into each other. */
  int both = right_w && left_w + right_w + 14 <= 176 - 12;
  unsigned n = layout_text_at(left, chars, nchars, LABEL_BOLD_GAP, LABEL_BOLD_SPACE, 176,
                              both ? -1 : 0, 6, glyphs);
  if (both)
    n += layout_text_at(right, chars, nchars, LABEL_BOLD_GAP, LABEL_BOLD_SPACE, 176, 1, 6, glyphs + n);
  recipe.count = n;
  compose_into(ctx, *desc, &recipe, 176, 30, 2, 0.f);
  return *desc;
}

/* Redraws `desc` with `text`; allocates it on first use. */
static uint32_t text_label(Context* ctx, uint32_t* desc, const char* text) {
  LabelGlyph glyphs[LABEL_TEXT_MAX];
  LabelRecipe recipe = { "", glyphs, 0 };
  if (!*desc) { *desc = make_image(ctx, 176, 30, 2); if (!*desc) return 0; }
  recipe.count = layout_text(text, label_bold_chars, sizeof label_bold_chars / sizeof(LabelChar),
                             LABEL_BOLD_GAP, LABEL_BOLD_SPACE, 176, glyphs);
  compose_into(ctx, *desc, &recipe, 176, 30, 2, 0.f);
  return *desc;
}

static uint32_t upload_i4(Context* ctx, const unsigned char* bytes, unsigned len, unsigned w, unsigned h) {
  uint32_t desc = make_image(ctx, w, h, 0);
  if (!desc) return 0;
  uint32_t data = mem_read32(ctx, desc);
  unsigned cap = texture_bytes(0, w, h);
  memcpy(ctx->ram + (data & RAM_MASK), bytes, len < cap ? len : cap);
  return desc;
}

static int recipe_index(const LabelRecipe* list, unsigned count, const char* name) {
  for (unsigned i = 0; i < count; i++) if (!strcmp(list[i].name, name)) return (int)i;
  return -1;
}
static uint32_t bold_label(Context* ctx, const char* name) {
  if (!strcmp(name,"profile")) {
    if (!profile_label) {
      profile_label=make_image(ctx,176,30,2);
      if (profile_label) memcpy(ctx->ram+(mem_read32(ctx,profile_label)&RAM_MASK),label_profile_ia4,sizeof label_profile_ia4);
    }
    return profile_label;
  }
  if (!strcmp(name, "mod_browser")) {
    if (!mod_browser_label) {
      mod_browser_label = make_image(ctx, 176, 30, 2);
      if (mod_browser_label)
        memcpy(ctx->ram + (mem_read32(ctx, mod_browser_label) & RAM_MASK),
               label_mod_browser_ia4, sizeof label_mod_browser_ia4);
    }
    return mod_browser_label;
  }
  int i = recipe_index(label_bold_recipes, sizeof label_bold_recipes / sizeof(LabelRecipe), name);
  if (i < 0) return 0;
  if (!bold_image[i]) bold_image[i] = compose(ctx, &label_bold_recipes[i], 176, 30, 2, 0.f);
  return bold_image[i];
}
static uint32_t italic_label(Context* ctx, const char* name) {
  int i = recipe_index(label_italic_recipes, sizeof label_italic_recipes / sizeof(LabelRecipe), name);
  if (i < 0) return 0;
  if (!italic_image[i]) italic_image[i] = compose(ctx, &label_italic_recipes[i], 168, 28, 0, LABEL_ITALIC_SHEAR);
  return italic_image[i];
}

/* SIS description strings, in the encoding decoded from SdMenu.usd. */
static uint32_t make_sis(Context* ctx, const char* text) {
  static const unsigned char prefix[] = {0x16, 0x10, 0x0C, 0xAA, 0xAA, 0xAA, 0x0E, 0x00, 0xB3, 0x00, 0xB3, 0x06, 0x00, 0x00, 0x00, 0x00, 0x18};
  static const unsigned char suffix[] = {0x19, 0x0F, 0x0D, 0x00};
  unsigned char buffer[512]; unsigned n = sizeof prefix;
  memcpy(buffer, prefix, sizeof prefix);
  for (const char* p = text; *p && n < sizeof buffer - 8; p++) {
    unsigned c = (unsigned char)*p, code;
    if (c == ' ') { buffer[n++] = 0x1A; continue; }
    if (c >= '0' && c <= '9') code = c - '0';
    else if (c >= 'A' && c <= 'Z') code = 0x0A + c - 'A';
    else if (c >= 'a' && c <= 'z') code = 0x24 + c - 'a';
    else if (c == '.') code = 0xE7; else if (c == ',') code = 0xE6; else if (c == '!') code = 0xEC;
    else if (c == '?') code = 0xE8; else if (c == ':') code = 0xE9; else if (c == '-') code = 0xFC;
    else continue;
    buffer[n++] = 0x20; buffer[n++] = (unsigned char)code;
  }
  memcpy(buffer + n, suffix, sizeof suffix); n += sizeof suffix;
  uint32_t at = area_alloc(n);
  if (at) memcpy(ctx->ram + (at & RAM_MASK), buffer, n);
  return at;
}

/* ---- netplay screen state ------------------------------------------------- */
/* `text` is set instead of `label` for a row whose wording is only known at
   runtime, such as a room in the browser. */
typedef struct { const char* label; int locked; const char* text; const char* right; } Row;
static MeleeNetplayUi ui;
static int room_cursor, rules_dirty;
static int mod_cursor, mod_prompt_cursor, mod_manager;
static int mod_visible(const MeleeNetplayUi* state,int index){return !mod_manager || state->mod_catalog[index].installed;}
static int mod_move(const MeleeNetplayUi* state,int index,int direction){
 int count=state->mod_catalog_count; if(count<=0||count>NETPLAY_MOD_CATALOG)return 0;
 for(int n=0;n<count;n++){index=(index+count+direction)%count;if(mod_visible(state,index))return index;}return 0;
}
static int room_action, room_rules_mode, room_rule_cursor;
static NetplayRules edited_rules;

static int is_netplay_menu(unsigned kind) { return kind == MENU_KIND_NETPLAY || kind == MENU_KIND_ROOM || kind == MENU_KIND_BROWSER; }

/* The wording a baked label shows, so a row with a value on the right can be
   rebuilt from scratch instead of using the pre-composed texture. */
static const char* label_words(const char* name) {
  if (!strcmp(name, "host_room")) return "Host Room";
  if (!strcmp(name, "start_match")) return "Start Match";
  if (!strcmp(name, "leave_room")) return "Leave Room";
  if (!strcmp(name, "not_ready")) return "Not Ready";
  if (!strcmp(name, "connect")) return "Connect";
  if (!strcmp(name, "ready")) return "Ready";
  if (!strcmp(name, "items")) return "Items";
  if (!strcmp(name, "mode")) return "Mode";
  return name;
}

static int local_ready(void);

/* MELEE_NETPLAY_FLAT=1 drops the button plates and leaves a plain list. */
/* The room browser is a list of rooms, not a stack of buttons: its rows lose the
   button plate and keep only their text, with the plate left showing on the row
   under the cursor to act as the highlight bar. The connect screen stays an
   ordinary Melee menu. MELEE_NETPLAY_FLAT=0 turns this off. */
static int flat_rows(unsigned kind) {
  const char* off = getenv("MELEE_NETPLAY_FLAT");
  if (off && *off == '0') return 0;
  return kind == MENU_KIND_BROWSER;
}

static char browser_names[NETPLAY_MAX_ROOMS][40];

static unsigned netplay_rows(unsigned kind, Row* rows) {
  unsigned n = 0;
  if (kind == MENU_KIND_NETPLAY) {
    int online = ui.phase >= NETPLAY_PHASE_LOBBY;
    rows[n++] = (Row){ online ? "host_room" : "connect", 0 };
    /* The original constructor allocates only unlocked rows. Keep all six
     * slots present so connection changes never relabel a shorter row tree. */
    rows[n++] = (Row){ "rooms", 0 };
    rows[n++] = (Row){ "mod_browser", 0 };
    rows[n++] = (Row){ "profile", 0 };
    rows[n++] = (Row){ "disconnect", 0 };
    rows[n++] = (Row){ "return", 0 };
  } else if (kind == MENU_KIND_BROWSER) {
    /* One row per room, named after the room itself. */
    unsigned count = (unsigned)ui.room_count;
    if (count > 6) count = 6;
    for (unsigned i = 0; i < count; i++) {
      NetplayRoom* r = &ui.rooms[i];
      snprintf(browser_names[i], sizeof browser_names[i], "%s", r->name);
      rows[n++] = (Row){ NULL, 0, browser_names[i],
                         r->state ? "Playing" : r->players >= r->max ? "Full" : "Open" };
    }
    rows[n++] = (Row){ NULL, 0, "Reload" };
    rows[n++] = (Row){ "return", 0 };
  } else {
    static const char* item_rates[] = { "None", "Very Low", "Low", "Medium", "High", "Very High" };
    int host = ui.is_host != 0;
    rows[n++] = (Row){ "ready", 0, NULL, local_ready() ? "Ready" : "Waiting" };
    rows[n++] = (Row){ "mode", !host, NULL, ui.rules.mode ? "Stock" : "Time" };
    rows[n++] = (Row){ ui.rules.mode ? "lives" : "time", !host };
    rows[n++] = (Row){ "items", !host, NULL, item_rates[ui.rules.items % 6] };
    rows[n++] = (Row){ "delay", !host };
    rows[n++] = (Row){ "start_match", !host, NULL, ui.all_ready ? "Ready" : NULL };
    rows[n++] = (Row){ "leave_room", 0 };
  }
  return n;
}

static int local_ready(void) {
  for (int p = 0; p < ui.player_count; p++) if (ui.players[p].id == ui.local_id) return ui.players[p].ready;
  return 0;
}

/* One status line for the hovered row, drawn in the game's own font. */
static void netplay_description(unsigned kind, unsigned row, char* out, unsigned cap) {
  if (kind == MENU_KIND_NETPLAY) {
    switch (row) {
    case 0:
      if (ui.phase < NETPLAY_PHASE_LOBBY) snprintf(out, cap, "%s", ui.phase == NETPLAY_PHASE_CONNECTING ? "Connecting to Online..." : "Connect to Online.");
      else snprintf(out, cap, "Create a room other players can join.");
      return;
    case 1:
      if (ui.phase < NETPLAY_PHASE_LOBBY) snprintf(out, cap, "%s", ui.phase == NETPLAY_PHASE_CONNECTING
          ? "Connecting to Online..." : "Connect and browse open rooms.");
      else if (!ui.room_count) snprintf(out, cap, "No rooms are open. Host one, or look again in a moment.");
      else snprintf(out, cap, "Browse the %d open room%s.", ui.room_count, ui.room_count == 1 ? "" : "s");
      return;
    case SEL_ONLINE_MOD_BROWSER: snprintf(out, cap, "Browse and install compatible costume packs."); return;
    case 3: snprintf(out, cap, "Set your Online name and profile picture."); return;
    case 4: snprintf(out, cap, "%s", ui.phase >= NETPLAY_PHASE_LOBBY
                                      ? "Disconnect from Online."
                                      : ui.phase == NETPLAY_PHASE_CONNECTING ? "Cancel the connection." : "Already disconnected."); return;
    default: snprintf(out, cap, "Back to the main menu."); return;
    }
  }
  if (kind == MENU_KIND_BROWSER) {
    unsigned count = (unsigned)(ui.room_count > 6 ? 6 : ui.room_count);
    if (row < count) {
      NetplayRoom* r = &ui.rooms[row];
      snprintf(out, cap, "%s   %d of %d players   host %s   %s", r->name, r->players, r->max, r->host,
               r->state ? "playing now" : "waiting to start");
    } else if (row == count) {
      snprintf(out, cap, "Look for rooms again.");
    } else {
      snprintf(out, cap, "Back to Online.");
    }
    return;
  }
  switch (row) {
  case 0: {
    int ready = 0;
    for (int p = 0; p < ui.player_count; p++) if (ui.players[p].ready) ready++;
    snprintf(out, cap, "%s   %d of %d players ready", ui.room_name, ready, ui.player_count);
    return;
  }
  case 1: snprintf(out, cap, "Match rule: %s", ui.rules.mode ? "Stock" : "Time"); return;
  case 2:
    if (ui.rules.mode) snprintf(out, cap, "Lives: %d", ui.rules.stock);
    else snprintf(out, cap, "Minutes: %d", ui.rules.minutes);
    return;
  case 3: {
    static const char* names[] = {"None", "Very low", "Low", "Medium", "High", "Very high"};
    snprintf(out, cap, "Items: %s", names[ui.rules.items % 6]);
    return;
  }
  case 4:
    /* 0 is not "no delay": it hands the choice to the clients, which measure
     * the round trip to each other and cover it exactly. */
    if (ui.rules.delay) snprintf(out, cap, "Input delay: %d frames", ui.rules.delay);
    else snprintf(out, cap, "Input delay: automatic, from the measured connection");
    return;
  case 5: snprintf(out, cap, "%s", ui.all_ready ? "Start the match." : "Waiting for every player to be ready."); return;
  default: snprintf(out, cap, "Leave this room."); return;
  }
}

static unsigned hovered_row(unsigned kind, Row* rows, unsigned count, unsigned hovered) {
  unsigned slot = 0;
  for (unsigned i = 0; i < count; i++) {
    if (rows[i].locked) continue;
    if (slot == hovered) return i;
    slot++;
  }
  return count ? count - 1 : 0;
}

/* Rebuild every custom texture from the menu archive currently loaded. */
static void ensure_labels(Context* ctx) {
  uint32_t archive = mem_read32(ctx, MENU_ARCHIVE);
  uint32_t cursor_matanim = mem_read32(ctx, CURSOR_MODEL + 8), panel_matanim = mem_read32(ctx, PANEL_MODEL + 8);
  if (!valid_guest(archive, 0x44) || !valid_guest(cursor_matanim, 0x10) || !valid_guest(panel_matanim, 0x10)) return;
  if (label_archive == archive && bold_table) return;
  uint32_t cursor_texanim = texanim_of(ctx, cursor_matanim, 3, 58);
  panel_texanim = texanim_of(ctx, panel_matanim, 85, 21);
  if (!cursor_texanim || !panel_texanim) { fprintf(stderr, "[hooks] menu texture tables not found\n"); return; }
  bold_table = mem_read32(ctx, cursor_texanim + 0xC);
  italic_table = mem_read32(ctx, panel_texanim + 0xC);
  uint32_t parent_texanim = texanim_of(ctx, panel_matanim, 84, 5);
  parent_italic_table = parent_texanim ? mem_read32(ctx, parent_texanim + 0xC) : 0;
  for (unsigned i=0;i<58;i++) bold_sources[i]=mem_read32(ctx,bold_table+i*4);
  for (unsigned i=0;i<21;i++) italic_sources[i]=mem_read32(ctx,italic_table+i*4);
  for (unsigned i=0;i<5;i++) parent_italic_sources[i]=parent_italic_table?mem_read32(ctx,parent_italic_table+i*4):0;
  mod_browser_label = profile_label = 0;
  area_used = 0;
  memset(monitor_image, 0, sizeof monitor_image);
  memset(monitor_quad, 0, sizeof monitor_quad);
  memset(monitor_material, 0, sizeof monitor_material);
  monitor_joint_desc = 0;
  patched_monitor_count = 0;
  memset(bold_image, 0, sizeof bold_image);
  memset(italic_image, 0, sizeof italic_image);
  memset(row_text_image, 0, sizeof row_text_image);   /* the area they lived in is gone */
  option_narrow = upload_i4(ctx, label_narrow_i4, sizeof label_narrow_i4, LABEL_NARROW_W, LABEL_NARROW_H);
  option_wide = upload_i4(ctx, label_wide_i4, sizeof label_wide_i4, LABEL_WIDE_W, LABEL_WIDE_H);
  /* The row borrows another row's hover preview; which one decides the colour
     of the panel behind it. MELEE_NETPLAY_PREVIEW picks a different row. */
  {
    /* Row 0's preview panel is the cyan one; row 1's reads as black behind the
       netplay rows. MELEE_NETPLAY_PREVIEW picks a different row. */
    const char* pick = getenv("MELEE_NETPLAY_PREVIEW");
    int source = pick && *pick ? atoi(pick) : 0;
    if (source < 0 || source > 5) source = 1;
    netplay_anim = area_alloc(10 * 12);
    if (netplay_anim) for (int i = 0; i < 10; i++)
      memcpy(ctx->ram + ((netplay_anim + i * 12) & RAM_MASK),
             ctx->ram + ((MENU_MAIN_ANIM + source * 12) & RAM_MASK), 12);
  }
  netplay_descriptions = area_alloc(10 * 2);
  label_archive = archive;
  /* The unused Options row selects image 22, an 8x4 transparent placeholder.
     Bind its label in the animation table itself: the normal 176x30 row search
     cannot find the placeholder, and hover animations restore this slot. */
  mem_write32(ctx, bold_table + 22u * 4u, bold_label(ctx, "widescreen"));
  mem_write32(ctx, bold_table + 21u * 4u, bold_label(ctx, "controllers"));
  /* Only the old Screen Display picture changes. Original preview meshes,
     layered backdrop and stretch/appearance animations remain untouched. */
  uint32_t hover_matanim=mem_read32(ctx,0x804A0528u+8);
  uint32_t hover_texanim=texanim_of(ctx,hover_matanim,40,23);
  if (hover_texanim) {
    uint32_t image=upload_i4(ctx,controller_hover_i4,sizeof controller_hover_i4,128,104);
    if (image) mem_write32(ctx,mem_read32(ctx,hover_texanim+0xC)+6*4,image);
  }
  sis_shown[0] = 0;
  fprintf(stderr, "[hooks] menu labels ready (archive %08X)\n", archive);
}

/* Field 4 is the frame the shared panel animation is parked on, and it is what
   makes each of the original submenus look like its own screen rather than the
   main menu: VS sits at 20, Trophies at 40, Data at 160, and so on. Ours were
   left at 0, which is the main menu's own frame. MELEE_NETPLAY_PANEL overrides. */
static float netplay_panel_frame(unsigned kind) {
  const char* pick = getenv("MELEE_NETPLAY_PANEL");
  if (pick && *pick) return (float)atof(pick);
  (void)kind;
  return 0.f;    /* the main menu's frame, whose header slots we already patch */
}

static void install_netplay_kind(Context* ctx, unsigned kind, unsigned rows) {
  uint32_t entry = MENU_KIND_TABLE + kind * 0x14u;
  float frame = netplay_panel_frame(kind);
  uint32_t bits;
  memcpy(&bits, &frame, sizeof bits);
  mem_write32(ctx, entry + 0, netplay_anim);
  mem_write32(ctx, entry + 4, bits);
  mem_write32(ctx, entry + 8, netplay_descriptions);
  mem_write8(ctx, entry + 0xC, (uint8_t)rows);
  mem_write32(ctx, entry + 0x10, 0x8022DB10u);   /* routed by cur_menu in hook_main_think */
}

static void patch_header(Context* ctx, unsigned slot, uint32_t image) {
  if (!panel_texanim || !image) return;
  uint32_t table = mem_read32(ctx, panel_texanim + 0xC);
  if (!valid_guest(table, 21 * 4)) return;
  if (patched_header_old && patched_header_slot != slot) return;
  if (!patched_header_old) { patched_header_slot = slot; patched_header_old = mem_read32(ctx, table + slot * 4u); }
  mem_write32(ctx, table + slot * 4u, image);
}
static void restore_header(Context* ctx) {
  if (!patched_header_old || !panel_texanim) return;
  uint32_t table = mem_read32(ctx, panel_texanim + 0xC);
  if (valid_guest(table, 21 * 4)) mem_write32(ctx, table + patched_header_slot * 4u, patched_header_old);
  patched_header_old = 0;
}

/* Replace the animated label texture of one visible cursor row. */
static uint32_t table_image;
static int label_texture(Context* ctx, uint32_t label, uint32_t image);
static void draw_browser_table(Context* ctx, int hovered);
static void probe_row_quad(Context* ctx, uint32_t node, int slot);

/* Show or hide every mesh of one node. */
static void set_node_hidden(Context* ctx, uint32_t node, int hidden) {
  for (uint32_t dobj = mem_read32(ctx, node + 0x18); valid_guest(dobj, 0x18); dobj = mem_read32(ctx, dobj + 4)) {
    uint32_t flags = mem_read32(ctx, dobj + 0x14);
    mem_write32(ctx, dobj + 0x14, hidden ? (flags | 1u) : (flags & ~1u));
  }
}

/* True when this node is the one carrying the row's text. */
static int node_is_label(Context* ctx, uint32_t node) {
  for (uint32_t dobj = mem_read32(ctx, node + 0x18); valid_guest(dobj, 0x18); dobj = mem_read32(ctx, dobj + 4)) {
    uint32_t mobj = mem_read32(ctx, dobj + 8);
    if (!valid_guest(mobj, 0x20)) continue;
    uint32_t tobj = mem_read32(ctx, mobj + 8);
    for (int t = 0; t < 8 && valid_guest(tobj, 0xAC); t++, tobj = mem_read32(ctx, tobj + 8)) {
      uint32_t desc = mem_read32(ctx, tobj + 0x58);
      if (!valid_guest(desc, 0x18)) continue;
      /* Its own size, or a picture already put on it - otherwise the quad stops
         being recognised the moment something else is drawn there. */
      if ((mem_read16(ctx, desc + 4) == 176 && mem_read16(ctx, desc + 6) == 30) ||
          (table_image && desc == table_image)) return 1;
    }
  }
  return 0;
}

/* The netplay screens are a list, not a stack of buttons: every row keeps its
   text but loses the button plate behind it, except the row under the cursor,
   whose plate is left showing and becomes the highlight bar. */
static void flatten_row(Context* ctx, uint32_t gobj, int slot, int highlighted) {
  if (!valid_guest(gobj, 0x30)) return;
  uint32_t data = mem_read32(ctx, gobj + 0x2C);
  if (!valid_guest(data, 0xB0)) return;
  uint32_t option = mem_read32(ctx, data + 4 + (4 + slot) * 4);
  if (!valid_guest(option, 0x88)) return;
  uint32_t cursor = mem_read32(ctx, option + 0x10);
  for (int guard = 0; guard < 64 && valid_guest(cursor, 0x88) && mem_read32(ctx, cursor + 8); guard++)
    cursor = mem_read32(ctx, cursor + 8);
  for (int index = 0; index < 10; index++) {
    int counter = 0;
    uint32_t node = nth_node(ctx, cursor, 0x10, 8, index, &counter);
    if (!valid_guest(node, 0x88)) continue;
    if (node_is_label(ctx, node)) probe_row_quad(ctx, node, slot);
    else set_node_hidden(ctx, node, !highlighted);
  }
}

/* Takes a row off the screen entirely. */
static void hide_row(Context* ctx, uint32_t gobj, int slot) {
  if (getenv("MELEE_HIDE_OFF")) return;
  if (!valid_guest(gobj, 0x30)) return;
  uint32_t data = mem_read32(ctx, gobj + 0x2C);
  if (!valid_guest(data, 0xB0)) return;
  uint32_t option = mem_read32(ctx, data + 4 + (4 + slot) * 4);
  if (!valid_guest(option, 0x88)) return;
  uint32_t cursor = mem_read32(ctx, option + 0x10);
  for (int guard = 0; guard < 64 && valid_guest(cursor, 0x88) && mem_read32(ctx, cursor + 8); guard++)
    cursor = mem_read32(ctx, cursor + 8);
  for (int index = 0; index < 10; index++) {
    int counter = 0;
    uint32_t node = nth_node(ctx, cursor, 0x10, 8, index, &counter);
    if (!valid_guest(node, 0x88)) continue;
    set_node_hidden(ctx, node, 1);
  }
}

/* ---- the room browser ------------------------------------------------------
 * Drawn as one picture and handed to the renderer, rather than built out of menu
 * rows: a row is animated by the game, which repaints its texture, material and
 * visibility every frame, so anything put there is undone. The picture is
 * composed here because this is where Melee's own menu font is reachable.
 */
#define TABLE_W 512
#define TABLE_H 320
#define TABLE_ROWS 6

extern void aurora_link_netplay_table(const void* rgba, int width, int height);
static unsigned char table_pixels[TABLE_W * TABLE_H * 4];
int table_trace;

static void table_box(int x0, int y0, int x1, int y1,
                      unsigned r, unsigned g, unsigned b, unsigned a) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > TABLE_W) x1 = TABLE_W;
  if (y1 > TABLE_H) y1 = TABLE_H;
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      unsigned char* p = &table_pixels[((size_t)y * TABLE_W + x) * 4];
      /* Straight alpha blend, so a panel can sit under the text. */
      p[0] = (unsigned char)((r * a + p[0] * (255 - a)) / 255);
      p[1] = (unsigned char)((g * a + p[1] * (255 - a)) / 255);
      p[2] = (unsigned char)((b * a + p[2] * (255 - a)) / 255);
      p[3] = (unsigned char)(a + p[3] * (255 - a) / 255);
    }
  }
}

static void table_frame(int x0, int y0, int x1, int y1, int thickness,
                        unsigned r, unsigned g, unsigned b, unsigned a) {
  table_box(x0, y0, x1, y0 + thickness, r, g, b, a);
  table_box(x0, y1 - thickness, x1, y1, r, g, b, a);
  table_box(x0, y0, x0 + thickness, y1, r, g, b, a);
  table_box(x1 - thickness, y0, x1, y1, r, g, b, a);
}

/* Draws `text` with the menu font. `scale` is a divisor: 2 gives half height. */
static void table_text(Context* ctx, const char* text, int x, int y, int width, int align,
                       int scale, unsigned r, unsigned g, unsigned b) {
  extern int table_trace;
  if (table_trace) fprintf(stderr, "[table]   text %s\n", text);
  LabelGlyph glyphs[LABEL_TEXT_MAX];
  unsigned count = layout_text_at(text, label_bold_chars,
                                  sizeof label_bold_chars / sizeof(LabelChar),
                                  LABEL_BOLD_GAP, LABEL_BOLD_SPACE,
                                  (unsigned)(width * scale), align, 0, glyphs);
  for (unsigned i = 0; i < count; i++) {
    const LabelGlyph* glyph = &glyphs[i];
    uint32_t src_desc = glyph->table == LABEL_TABLE_BOLD ? (glyph->src < 58 ? bold_sources[glyph->src] : 0) :
                        glyph->table == LABEL_TABLE_PARENT_ITALIC ? (glyph->src < 5 ? parent_italic_sources[glyph->src] : 0) :
                        (glyph->src < 21 ? italic_sources[glyph->src] : 0);
    if (!valid_guest(src_desc, 0x18)) continue;
    uint32_t src = mem_read32(ctx, src_desc);
    unsigned sw = mem_read16(ctx, src_desc + 4), sh = mem_read16(ctx, src_desc + 6);
    unsigned sfmt = mem_read32(ctx, src_desc + 8);
    if (!valid_guest(src, texture_bytes(sfmt, sw, sh))) continue;
    int span = glyph->u1 - glyph->u0;
    if (table_trace) fprintf(stderr, "[table]     glyph src %u %ux%u fmt %u span %d\n",
                             glyph->src, sw, sh, sfmt, span);
    /* A glyph sheet is small; anything else means the entry is not what we think
       it is, and walking it would take forever. */
    if (sw > 1024 || sh > 256 || span <= 0 || span > 256) continue;
    for (unsigned sy = 0; sy < sh; sy++) {
      for (int k = 0; k < span; k++) {
        unsigned sx = (unsigned)(glyph->u0 + k);
        if (sx >= sw) continue;
        unsigned coverage = texel_get(ctx, src, sfmt, sw, sx, sy) >> 4;
        if (!coverage) continue;
        int dx = x + (glyph->dst + k) / scale, dy = y + (int)sy / scale;
        if (dx < 0 || dx >= TABLE_W || dy < 0 || dy >= TABLE_H) continue;
        table_box(dx, dy, dx + 1, dy + 1, r, g, b, coverage * 17);
      }
    }
  }
}

/* Builds the browser picture and hands it over. */
static void draw_browser_table(Context* ctx, int hovered) {
  if (getenv("MELEE_TABLE_OFF")) return;
  static int traced;
  table_trace = traced < 3;
  if (traced < 3) { traced++; fprintf(stderr, "[table] drawing, hovered %d\n", hovered); }
  else if (traced == 3) { traced++; }
  ensure_labels(ctx);
  if (!bold_table) return;
  if (traced <= 3) fprintf(stderr, "[table] labels ok\n");
  memset(table_pixels, 0, sizeof table_pixels);
  if (traced <= 3) fprintf(stderr, "[table] cleared\n");

  table_box(0, 0, TABLE_W, TABLE_H, 10, 16, 34, 214);              /* panel */
  table_frame(0, 0, TABLE_W, TABLE_H, 2, 150, 190, 245, 235);      /* border */
  table_box(2, 2, TABLE_W - 2, 40, 28, 52, 96, 235);               /* title strip */
  table_box(2, 39, TABLE_W - 2, 41, 150, 190, 245, 200);
  if (traced <= 3) fprintf(stderr, "[table] panel drawn\n");
  table_text(ctx, "Rooms", 16, 10, TABLE_W - 32, -1, 2, 236, 244, 255);
  table_text(ctx, "Players", 16, 10, TABLE_W - 32, 1, 2, 150, 190, 245);
  if (traced <= 3) fprintf(stderr, "[table] titles drawn\n");

  /* Clamped at both ends: a negative count would wrap when cast and the loop
     below would never finish. */
  int rooms = ui.room_count;
  if (rooms < 0) rooms = 0;
  if (rooms > TABLE_ROWS) rooms = TABLE_ROWS;
  unsigned count = (unsigned)rooms;
  if (!count) {
    table_text(ctx, "No rooms open", 0, TABLE_H / 2 - 12, TABLE_W, 0, 2, 150, 170, 200);
    table_text(ctx, "Host one or come back later", 0, TABLE_H / 2 + 12, TABLE_W, 0, 3, 120, 140, 170);
  }
  for (unsigned i = 0; i < count; i++) {
    int y = 52 + (int)i * 40;
    NetplayRoom* room = &ui.rooms[i];
    int on = (int)i == hovered;
    if (on) {
      table_box(8, y - 6, TABLE_W - 8, y + 30, 70, 120, 200, 210);
      table_box(8, y - 6, 12, y + 30, 236, 244, 255, 240);
    }
    table_text(ctx, room->name, 20, y, TABLE_W - 170, -1, 2, 255, 255, 255);
    table_text(ctx, room->state ? "Playing" : room->players >= room->max ? "Full" : "Open",
               20, y, TABLE_W - 40, 1, 2,
               room->state ? 240 : 170, room->state ? 200 : 230, 140);
  }
  /* The two actions sit below the list and take the cursor after the rooms. */
  static const char* actions[2] = { "Reload", "Return" };
  for (int a = 0; a < 2; a++) {
    int y = TABLE_H - 64 + a * 30;
    int on = hovered == (int)count + a;
    if (on) {
      table_box(8, y - 5, TABLE_W - 8, y + 26, 70, 120, 200, 210);
      table_box(8, y - 5, 12, y + 26, 236, 244, 255, 240);
    }
    table_text(ctx, actions[a], 20, y, TABLE_W - 40, -1, 2,
               on ? 255 : 190, on ? 255 : 205, on ? 255 : 230);
  }
  if (traced <= 3) fprintf(stderr, "[table] pixels composed\n");
  aurora_link_netplay_table(table_pixels, TABLE_W, TABLE_H);
  if (traced <= 3) fprintf(stderr, "[table] handed to the renderer\n");
}

/* Groundwork probe, kept behind MELEE_TABLE_TEST=1. */
static void probe_row_quad(Context* ctx, uint32_t node, int slot) {
  if (!getenv("MELEE_TABLE_TEST") || slot != 0) return;
  static int done;
  if (done) return;
  done = 1;
  fprintf(stderr, "[table] row node %08X floats:\n", node);
  for (unsigned at = 0x1C; at < 0x58; at += 4) {
    float value = mem_rf32(node + at);
    fprintf(stderr, "[table]   +%02X = %.4f\n", at, value);
  }
  /* A wide image with a border drawn round it, to see how the quad maps it. */
  uint32_t desc = make_image(ctx, 256, 64, 2);
  if (!desc) return;
  uint32_t data = mem_read32(ctx, desc);
  memset(ctx->ram + (data & RAM_MASK), 0, texture_bytes(2, 256, 64));
  for (unsigned x = 0; x < 256; x++) {
    texel_set(ctx, data, 2, 256, x, 0, 0xFF);
    texel_set(ctx, data, 2, 256, x, 63, 0xFF);
  }
  for (unsigned y = 0; y < 64; y++) {
    texel_set(ctx, data, 2, 256, 0, y, 0xFF);
    texel_set(ctx, data, 2, 256, 255, y, 0xFF);
  }
  label_texture(ctx, node, desc);
}

/* Swap a row's 176x30 label texture, if this node carries one. */
static int label_texture(Context* ctx, uint32_t label, uint32_t image) {
  for (uint32_t dobj = mem_read32(ctx, label + 0x18); valid_guest(dobj, 0x18); dobj = mem_read32(ctx, dobj + 4)) {
    uint32_t mobj = mem_read32(ctx, dobj + 8);
    if (!valid_guest(mobj, 0x20)) continue;
    uint32_t tobj = mem_read32(ctx, mobj + 8);
    for (int t = 0; t < 8 && valid_guest(tobj, 0xAC); t++, tobj = mem_read32(ctx, tobj + 8)) {
      uint32_t desc = mem_read32(ctx, tobj + 0x58);
      if (!valid_guest(desc, 0x18)) continue;
      /* The label's own size, or something already put there - once a picture of
         another size is on the quad, it still has to be found again next frame. */
      if ((mem_read16(ctx, desc + 4) == 176 && mem_read16(ctx, desc + 6) == 30) ||
          (table_image && desc == table_image)) {
        mem_write32(ctx, tobj + 0x58, image);
        return 1;
      }
    }
  }
  return 0;
}

/* The main menu keeps a row's label on one particular child node; the options
   rows do not, so the whole row is searched for a label-sized texture. */
static void set_row_label(Context* ctx, uint32_t gobj, int slot, uint32_t image) {
  if (!image || !valid_guest(gobj, 0x30)) return;
  uint32_t data = mem_read32(ctx, gobj + 0x2C);
  if (!valid_guest(data, 0xB0)) return;
  uint32_t option = mem_read32(ctx, data + 4 + (4 + slot) * 4);   /* tree[mn_803EAE68[slot]] */
  if (!valid_guest(option, 0x88)) return;
  uint32_t cursor = mem_read32(ctx, option + 0x10);
  for (int guard = 0; guard < 64 && valid_guest(cursor, 0x88) && mem_read32(ctx, cursor + 8); guard++)
    cursor = mem_read32(ctx, cursor + 8);
  for (int index = 0; index < 10; index++) {
    int counter = 0;
    uint32_t label = nth_node(ctx, cursor, 0x10, 8, index, &counter);
    if (valid_guest(label, 0x88) && label_texture(ctx, label, image)) return;
  }
}

/* Apply the custom labels of the current menu. Runs when the menu is built and
 * again every frame: Melee re-animates a row's label whenever it is hovered. */
static void apply_labels(Context* ctx, uint32_t gobj) {
  unsigned kind = mem_read8(ctx, MENU_FLOW);
  int controller_preview = kind == MENU_KIND_SETTINGS && mem_read16(ctx,MENU_FLOW+2)==SEL_SETTINGS_CONTROLLERS;
  if (kind == MENU_KIND_CONTROLLERS || controller_preview) netplay_snapshot(&ui);
  apply_menu_foreground(ctx, kind, ui.room_name_open || profile_open);
  ui.menu_pose_valid = menu_pose_read(ctx, ui.menu_pose);
  ensure_labels(ctx);
  aurora_link_netplay_table(NULL, 0, 0);
  if (kind == MENU_KIND_CONTROLLERS) aurora_link_netplay_screen(&ui, 21, 0);
  else if (widescreen_mode) aurora_link_netplay_screen(NULL, 0, 0);
  else if (kind == MENU_KIND_NETPLAY && profile_open) aurora_link_netplay_screen(&ui, 24, 0);
  else if (kind == MENU_KIND_NETPLAY && ui.room_name_open) aurora_link_netplay_screen(&ui, 10, 0);
  else if (kind == MENU_KIND_BROWSER || kind == MENU_KIND_ROOM || kind == MENU_KIND_MOD_BROWSER)
    aurora_link_netplay_screen(&ui, kind == MENU_KIND_BROWSER ? 13 : kind == MENU_KIND_MOD_BROWSER ? (mod_manager ? 25 : 22) : 11,
        ui.mod_prompt ? 1000 + mod_prompt_cursor : kind == MENU_KIND_MOD_BROWSER ? mod_cursor :
        kind == MENU_KIND_BROWSER ? room_cursor : room_rules_mode ? 100 + room_rule_cursor : room_action);
  else aurora_link_netplay_screen(NULL, 0, 0);
  if (kind == MENU_KIND_MAIN) {
    set_row_label(ctx, gobj, SEL_MAIN_NETPLAY, bold_label(ctx, "netplay"));
  } else if (kind == MENU_KIND_CONTROLLERS || kind == MENU_KIND_MOD_BROWSER) {
    hide_row(ctx, gobj, 0);
  } else if (kind == MENU_KIND_SETTINGS) {
    set_row_label(ctx, gobj, SEL_SETTINGS_WIDESCREEN, bold_label(ctx, "widescreen"));
    set_row_label(ctx, gobj, SEL_SETTINGS_CONTROLLERS, bold_label(ctx, "controllers"));
  } else if (is_netplay_menu(kind)) {
    Row rows[10];
    unsigned count = netplay_rows(kind, rows), slot = 0;
    int hovered_slot = (int)mem_read16(ctx, MENU_FLOW + 2);
    /* The approved browser/lobby layout owns their complete presentation. */
    for (unsigned i = 0; i < count; i++) {
      if (rows[i].locked) continue;
      const char* name = rows[i].label;
      if (kind == MENU_KIND_ROOM && i == 0) name = local_ready() ? "not_ready" : "ready";
      unsigned cache = slot < 8 ? slot : 7;
      if (kind == MENU_KIND_BROWSER || kind == MENU_KIND_ROOM || profile_open) {
        /* Input remains in the guest; the renderer receives live state. */
        hide_row(ctx, gobj, (int)slot);
        slot++;
        continue;
      }
      if (flat_rows(kind)) flatten_row(ctx, gobj, (int)slot, (int)slot == hovered_slot);
      uint32_t image;
      if (rows[i].text && rows[i].right)
        image = text_label_columns(ctx, &row_text_image[cache], rows[i].text, rows[i].right);
      else if (rows[i].text)
        image = text_label(ctx, &row_text_image[cache], rows[i].text);
      else if (rows[i].right)
        image = text_label_columns(ctx, &row_text_image[cache], label_words(name), rows[i].right);
      else
        image = bold_label(ctx, name);
      set_row_label(ctx, gobj, (int)slot++, image);
    }
  }
}

/* Keep the description line in step with live lobby state. */
static void refresh_description(Context* ctx, uint32_t gobj) {
  unsigned kind = mem_read8(ctx, MENU_FLOW);
  int controls = kind == MENU_KIND_SETTINGS && mem_read16(ctx,MENU_FLOW+2)==SEL_SETTINGS_CONTROLLERS;
  if ((!is_netplay_menu(kind) && !controls) || !valid_guest(gobj, 0x30)) { if(kind==MENU_KIND_SETTINGS)sis_shown[0]=0; return; }
  uint32_t data = mem_read32(ctx, gobj + 0x2C);
  if (!valid_guest(data, 0xB0)) return;
  uint32_t text = mem_read32(ctx, data + 0xAC);
  if (!valid_guest(text, 0xA0)) return;
  Row rows[10];
  unsigned count = netplay_rows(kind, rows);
  if (controls) snprintf(sis_text,sizeof sis_text,"Map buttons, test sticks, and set tap jump.");
  else netplay_description(kind, hovered_row(kind, rows, count, mem_read16(ctx, MENU_FLOW + 2)), sis_text, sizeof sis_text);
  if (!strcmp(sis_text, sis_shown)) return;
  snprintf(sis_shown, sizeof sis_shown, "%s", sis_text);
  uint32_t line = make_sis(ctx, sis_text);
  if (line) mem_write32(ctx, text + 0x5C, line);
}

/* Enter a menu kind the way the original menus do.
 *
 * The original also destroys the running think proc and creates a fresh one
 * for the new menu kind. Every screen here is driven by the same think entry
 * (0x8022DB10, routed by cur_menu), so the live proc is kept instead:
 * destroying it without creating a replacement leaves nothing reading the
 * controller, which reads as dead input. The old panel removes itself once it
 * sees cur_menu change, exactly as it does between the original menus. */
static void open_menu(Context* ctx, unsigned kind, unsigned hovered, unsigned state, int entering) {
  mem_write16(ctx, MENU_INPUT_STATE, 5);
  mem_write8(ctx, MENU_FLOW + 0x11, (uint8_t)entering);
  mem_write8(ctx, MENU_FLOW + 1, mem_read8(ctx, MENU_FLOW));
  mem_write8(ctx, MENU_FLOW + 0, (uint8_t)kind);
  mem_write16(ctx, MENU_FLOW + 2, (uint16_t)hovered);
  sis_shown[0] = 0;
  uint32_t gobj = call_guest(ctx, FN_BUILD_MENU, state, 0, 0);
  call_guest(ctx, FN_GOBJ_KILL_PROCS, gobj, 0, 0);
  fprintf(stderr, "[hooks] menu kind %u opened (gobj %08X)\n", kind, gobj);
}

static int mod_prompt_think(Context* ctx, uint32_t buttons) {
  if (!ui.mod_prompt) { mod_prompt_cursor = 0; return 0; }
  int max_first = ui.mod_required_count > 5 ? ui.mod_required_count - 5 : 0;
  if (mod_prompt_cursor > max_first) mod_prompt_cursor = max_first;
  if (buttons & MENU_INPUT_BACK) { netplay_post(NETPLAY_CMD_MOD_CANCEL, 0, NULL, NULL); mod_prompt_cursor = 0; }
  else if ((buttons & MENU_INPUT_CONFIRM) && ui.mod_job != 2) netplay_post(NETPLAY_CMD_MOD_CONFIRM, 0, NULL, NULL);
  else if ((buttons & MENU_INPUT_UP) && mod_prompt_cursor > 0) mod_prompt_cursor--;
  else if ((buttons & MENU_INPUT_DOWN) && mod_prompt_cursor < max_first) mod_prompt_cursor++;
  return 1;
}

static void netplay_screen_think(Context* ctx, uint32_t buttons) {
  unsigned kind = mem_read8(ctx, MENU_FLOW), hovered = mem_read16(ctx, MENU_FLOW + 2);
  if (ui.room_name_open) {
    if (buttons & MENU_INPUT_BACK) netplay_room_name_input(NETPLAY_NAME_CANCEL, NULL);
    else if (buttons & MENU_INPUT_CONFIRM) netplay_room_name_input(NETPLAY_NAME_CONFIRM, NULL);
    return;
  }
  if (mod_prompt_think(ctx, buttons)) return;
  if (kind == MENU_KIND_BROWSER) {
    int count = ui.room_count < 0 ? 0 : ui.room_count > NETPLAY_MAX_ROOMS ? NETPLAY_MAX_ROOMS : ui.room_count;
    if (room_cursor >= count) room_cursor = count ? count - 1 : 0;
    if (buttons & MENU_INPUT_BACK) { call_guest(ctx, FN_SFX, 0, 0, 0); open_menu(ctx, MENU_KIND_NETPLAY, 1, 1, 1); }
    else if (buttons & MENU_INPUT_X) netplay_room_name_input(NETPLAY_NAME_OPEN, NULL);
    else if (buttons & MENU_INPUT_Y) netplay_post(NETPLAY_CMD_REFRESH, 0, NULL, NULL);
    else if ((buttons & MENU_INPUT_UP) && count) { room_cursor = (room_cursor + count - 1) % count; call_guest(ctx, FN_SFX, 2, 0, 0); }
    else if ((buttons & MENU_INPUT_DOWN) && count) { room_cursor = (room_cursor + 1) % count; call_guest(ctx, FN_SFX, 2, 0, 0); }
    else if ((buttons & MENU_INPUT_CONFIRM) && count) {
      const NetplayRoom* r = &ui.rooms[room_cursor];
      if (!r->state && r->players < r->max) { netplay_post(NETPLAY_CMD_JOIN, r->id, NULL, NULL); call_guest(ctx, FN_SFX, 1, 0, 0); }
      else call_guest(ctx, FN_SFX, 0, 0, 0);
    }
    return;
  }
  if (kind == MENU_KIND_ROOM) {
    if (buttons & MENU_INPUT_X) { netplay_post(NETPLAY_CMD_READY, !local_ready(), NULL, NULL); call_guest(ctx, FN_SFX, 1, 0, 0); }
    else if (buttons & MENU_INPUT_BACK) {
      if (room_rules_mode) { room_rules_mode = 0; room_action = 1; }
      else netplay_post(NETPLAY_CMD_LEAVE, 0, NULL, NULL);
      call_guest(ctx, FN_SFX, 0, 0, 0);
    } else if (room_rules_mode && ui.is_host) {
      if (buttons & MENU_INPUT_UP) { room_rule_cursor = (room_rule_cursor + 5) % 6; call_guest(ctx, FN_SFX, 2, 0, 0); }
      else if (buttons & MENU_INPUT_DOWN) { room_rule_cursor = (room_rule_cursor + 1) % 6; call_guest(ctx, FN_SFX, 2, 0, 0); }
      else if ((buttons & MENU_INPUT_CONFIRM) && room_rule_cursor == 5) room_rules_mode = 0;
      else if (buttons & (MENU_INPUT_LEFT | MENU_INPUT_RIGHT)) {
        int step = (buttons & MENU_INPUT_RIGHT) ? 1 : -1;
        edited_rules = ui.rules;
        if (room_rule_cursor == 0) edited_rules.mode = !edited_rules.mode;
        else if (room_rule_cursor == 1) { edited_rules.stock += step; if (edited_rules.stock < 1) edited_rules.stock = 1; if (edited_rules.stock > 99) edited_rules.stock = 99; }
        else if (room_rule_cursor == 2) { edited_rules.minutes += step; if (edited_rules.minutes < 0) edited_rules.minutes = 0; if (edited_rules.minutes > 99) edited_rules.minutes = 99; }
        else if (room_rule_cursor == 3) edited_rules.items = (edited_rules.items + step + 6) % 6;
        else if (room_rule_cursor == 4) { edited_rules.delay += step; if (edited_rules.delay < 0) edited_rules.delay = 0; if (edited_rules.delay > 10) edited_rules.delay = 10; }
        if (room_rule_cursor < 5) { rules_dirty = 1; call_guest(ctx, FN_SFX, 2, 0, 0); }
      }
    } else if (buttons & (MENU_INPUT_UP | MENU_INPUT_DOWN)) {
      if (ui.is_host) room_action = (room_action + ((buttons & MENU_INPUT_DOWN) ? 1 : 2)) % 3;
      else room_action = room_action == 2 ? 0 : 2;
      call_guest(ctx, FN_SFX, 2, 0, 0);
    } else if (buttons & MENU_INPUT_CONFIRM) {
      if (room_action == 0 && ui.is_host && ui.all_ready && ui.player_count >= 2) netplay_post(NETPLAY_CMD_START, 0, NULL, NULL);
      else if (room_action == 1 && ui.is_host) { room_rules_mode = 1; room_rule_cursor = 0; }
      else if (room_action == 2) netplay_post(NETPLAY_CMD_LEAVE, 0, NULL, NULL);
      call_guest(ctx, FN_SFX, 1, 0, 0);
    }
    return;
  }
  Row rows[10];
  unsigned count = netplay_rows(kind, rows), visible = 0;
  for (unsigned i = 0; i < count; i++) if (!rows[i].locked) visible++;
  if (!visible) return;
  if (hovered >= visible) { mem_write16(ctx, MENU_FLOW + 2, 0); hovered = 0; }
  unsigned row = hovered_row(kind, rows, count, hovered);
  if (buttons & MENU_INPUT_BACK) {
    call_guest(ctx, FN_SFX, 0, 0, 0);
    if (kind == MENU_KIND_ROOM) netplay_post(NETPLAY_CMD_LEAVE, 0, NULL, NULL);
    else if (kind == MENU_KIND_BROWSER) open_menu(ctx, MENU_KIND_NETPLAY, 1, 1, 1);
    else open_menu(ctx, MENU_KIND_MAIN, SEL_MAIN_NETPLAY, 3, 0);
    return;
  }
  if (buttons & MENU_INPUT_UP) {
    call_guest(ctx, FN_SFX, 2, 0, 0);
    mem_write16(ctx, MENU_FLOW + 2, (uint16_t)(hovered ? hovered - 1 : visible - 1));
    sis_shown[0] = 0;
    return;
  }
  if (buttons & MENU_INPUT_DOWN) {
    call_guest(ctx, FN_SFX, 2, 0, 0);
    mem_write16(ctx, MENU_FLOW + 2, (uint16_t)(hovered + 1 >= visible ? 0 : hovered + 1));
    sis_shown[0] = 0;
    return;
  }
  int step = (buttons & MENU_INPUT_RIGHT) ? 1 : (buttons & MENU_INPUT_LEFT) ? -1 : 0;
  if (step) {
    if (kind == MENU_KIND_ROOM && ui.is_host && row >= 1 && row <= 4) {
      edited_rules = ui.rules;
      if (row == 1) edited_rules.mode = edited_rules.mode ? 0 : 1;
      else if (row == 2) { if (edited_rules.mode) edited_rules.stock += step; else edited_rules.minutes += step; }
      else if (row == 3) edited_rules.items += step;
      else edited_rules.delay += step;
      rules_dirty = 1;
      call_guest(ctx, FN_SFX, 2, 0, 0);
    }
    sis_shown[0] = 0;
    return;
  }
  if (!(buttons & MENU_INPUT_CONFIRM)) return;
  call_guest(ctx, FN_SFX, 1, 0, 0);
  if (kind == MENU_KIND_BROWSER) {
    unsigned count = (unsigned)(ui.room_count > 6 ? 6 : ui.room_count);
    if (row < count) netplay_post(NETPLAY_CMD_JOIN, ui.rooms[row].id, NULL, NULL);
    else if (row == count) netplay_post(NETPLAY_CMD_REFRESH, 0, NULL, NULL);
    else open_menu(ctx, MENU_KIND_NETPLAY, 1, 1, 1);
    return;
  }
  if (kind == MENU_KIND_NETPLAY) {
    if (getenv("MELEE_TEST_UI_TRACE")) fprintf(stderr, "[online-menu] action row=%u phase=%d label=%s\n", row, ui.phase, rows[row].label);
    if (row == 0) {
      if (ui.phase == NETPLAY_PHASE_OFFLINE) netplay_post(NETPLAY_CMD_CONNECT, 0, NULL, NULL);
      else if (ui.phase >= NETPLAY_PHASE_LOBBY) netplay_room_name_input(NETPLAY_NAME_OPEN, NULL);
    } else if (row == 1) {
      if (ui.phase == NETPLAY_PHASE_OFFLINE) netplay_post(NETPLAY_CMD_CONNECT, 0, NULL, NULL);
      room_cursor = 0;
      open_menu(ctx, MENU_KIND_BROWSER, 0, 1, 1);
      return;
    }
    else if (row == SEL_ONLINE_MOD_BROWSER) {
      mod_cursor = mod_prompt_cursor = mod_manager = 0;
      netplay_post(NETPLAY_CMD_MOD_REFRESH, 0, NULL, NULL);
      open_menu(ctx, MENU_KIND_MOD_BROWSER, 0, 1, 1);
      return;
    }
    else if (row == 3) { profile_open = 1; sis_shown[0] = 0; return; }
    else if (row == 4) { if (ui.phase != NETPLAY_PHASE_OFFLINE) netplay_post(NETPLAY_CMD_DISCONNECT, 0, NULL, NULL); }
    else open_menu(ctx, MENU_KIND_MAIN, SEL_MAIN_NETPLAY, 3, 0);
  } else {
    if (row == 0) netplay_post(NETPLAY_CMD_READY, !local_ready(), NULL, NULL);
    else if (row == 5) netplay_post(NETPLAY_CMD_START, 0, NULL, NULL);
    else if (row == 6) netplay_post(NETPLAY_CMD_LEAVE, 0, NULL, NULL);
  }
  sis_shown[0] = 0;
}

/* ---- now-playing bar ------------------------------------------------------
 * Drawn by the game, not by an overlay: HSD_SisLib_803A611C creates a 640x480
 * orthographic text context (x 0..640, y 0..-480) whose camera renders with
 * the scene, and the title is an HSD_Text in Melee's own font. The string is
 * the SIS encoding built by make_sis, so no font work is needed. */
#define FN_SIS_CONTEXT 0x803A611Cu
#define FN_SIS_TEXT_NEW 0x803A5ACCu
#define FN_SIS_TEXT_FREE 0x803A5CC4u
#define MUSIC_BAR_X 44.f
#define MUSIC_BAR_Y -404.f
#define MUSIC_SLIDE 18
#define MUSIC_HOLD 330

/* The SIS library's text render callback: which GX link the game registers it
   on tells us the link the current scene actually draws text on. */
#define FN_SIS_RENDER 0x803A84BCu
static unsigned sis_gx_link, sis_gx_prio, sis_gx_known;
static int music_creating;
static uint32_t music_text;
static uint32_t probe_text;
static int link_probe_done;   /* a text the game made and draws, for comparison */
static int music_context = -1, music_timer, music_total, music_custom;
static char music_title[128];

/* The widget's guest objects are in the RAM snapshot, but its timer, pointers
 * and SIS arena cursor live on the host. Restore both sides together so replay
 * neither frees an object from a future frame nor consumes the arena twice.
 * ensure_labels() can run while recreating the widget, so its pointer caches
 * and allocation bookkeeping must accompany the widget state too. The bound
 * Context pointer and live network/menu requests are not timeline state. */
size_t hooks_rollback_snapshot(void* data, int restore) {
  unsigned char* bytes = (unsigned char*)data;
  size_t size = 0;
#define HOOK_STATE(field) do { \
    if (bytes) { \
      if (restore) memcpy(&(field), bytes + size, sizeof(field)); \
      else memcpy(bytes + size, &(field), sizeof(field)); \
    } \
    size += sizeof(field); \
  } while (0)
  HOOK_STATE(music_text);
  HOOK_STATE(music_context);
  HOOK_STATE(music_timer);
  HOOK_STATE(music_total);
  HOOK_STATE(music_custom);
  HOOK_STATE(music_title);
  HOOK_STATE(sis_gx_link);
  HOOK_STATE(sis_gx_prio);
  HOOK_STATE(sis_gx_known);
  HOOK_STATE(music_creating);
  HOOK_STATE(probe_text);
  HOOK_STATE(link_probe_done);
  HOOK_STATE(area_base);
  HOOK_STATE(area_size);
  HOOK_STATE(area_used);
  HOOK_STATE(custom_light_color);
  HOOK_STATE(label_archive);
  HOOK_STATE(bold_image);
  HOOK_STATE(italic_image);
  HOOK_STATE(row_text_image);
  HOOK_STATE(option_narrow);
  HOOK_STATE(option_wide);
  HOOK_STATE(bold_table);
  HOOK_STATE(bold_sources);
  HOOK_STATE(italic_sources);
  HOOK_STATE(parent_italic_sources);
  HOOK_STATE(italic_table);
  HOOK_STATE(parent_italic_table);
  HOOK_STATE(mod_browser_label);
  HOOK_STATE(profile_label);
  HOOK_STATE(profile_open);
  HOOK_STATE(panel_texanim);
  HOOK_STATE(patched_header_slot);
  HOOK_STATE(patched_header_old);
  HOOK_STATE(patched_desc);
  HOOK_STATE(patched_desc_old);
  HOOK_STATE(monitor_image);
  HOOK_STATE(monitor_quad);
  HOOK_STATE(monitor_material);
  HOOK_STATE(monitor_joint_desc);
  HOOK_STATE(patched_monitor);
  HOOK_STATE(patched_monitor_count);
  HOOK_STATE(netplay_anim);
  HOOK_STATE(netplay_descriptions);
  HOOK_STATE(table_image);
  HOOK_STATE(sis_text);
  HOOK_STATE(sis_shown);
#undef HOOK_STATE
  return size;
}

static void music_text_free(Context* ctx) {
  if (music_text) { call_guest(ctx, FN_SIS_TEXT_FREE, music_text, 0, 0); music_text = 0; }
  music_timer = 0;
}

/* Dropped whenever the scene heap goes: both objects belong to it. */
void music_text_reset(void) { music_text = 0; music_context = -1; link_probe_done = 0; sis_gx_known = 0; }
static int music_text_create(Context* ctx);

/* Requested by music.c when a track starts. The object itself is built on the
 * next frame, because a track usually starts during a scene change and the
 * scene heap that owns the text is rebuilt straight afterwards. */
int music_text_show(Context* ctx, const char* title, int custom) {
  music_text_free(ctx);
  snprintf(music_title, sizeof music_title, "%s", title);
  music_custom = custom;
  music_total = MUSIC_SLIDE * 2 + MUSIC_HOLD;
  music_timer = music_total;
  fprintf(stderr, "[music] bar queued: %s\n", music_title);
  return 1;
}

static int music_text_create(Context* ctx) {
  ensure_labels(ctx);
  if (!sis_gx_known) return 0;          /* nothing drawn yet in this scene */
  music_creating = 1;
  /* Join a context the game is already drawing when one exists: a context of
     our own is only rendered if the scene happens to walk that gx link. */
  if (music_context < 0 && valid_guest(probe_text, 0xA0)) music_context = mem_read8(ctx, probe_text + 0x4F);
  if (music_context < 0) {
    /* Same arguments the game itself passes at 0x801D2CE4: font 1, no parent,
       class 9, p_link 13, gx_link 1. A gx_link the scene does not draw leaves
       the text alive but invisible. */
    const uint32_t args[8] = { 1, 0, 9, 13, 0, 1, 0, 7 };
    music_context = (int)call_guest_ex(ctx, FN_SIS_CONTEXT, args, 8, NULL, 0);
    if (music_context < 0 || music_context > 32) { music_context = -1; music_creating = 0; return 0; }
  }
  const uint32_t gpr[2] = { (uint32_t)music_context, 0 };
  const float fpr[5] = { MUSIC_BAR_X, MUSIC_BAR_Y, 0.f, 600.f, 60.f };
  uint32_t text = call_guest_ex(ctx, FN_SIS_TEXT_NEW, gpr, 2, fpr, 5);
  music_creating = 0;
  if (!valid_guest(text, 0xA0)) return 0;
  /* The text is unusable until the game initialises its buffers; call the real
     function rather than the hooked entry so no menu substitution applies. */
  {
    Context saved = *ctx;
    /* The index picks the stock string this text starts out as, and that is what
       sizes its render buffers; index 0 is empty, which leaves nothing to draw
       into however long a string is put in afterwards. 0xBF is one of the long
       description lines, so the buffers are big enough for a track name. */
    ctx->gpr[3] = text; ctx->gpr[4] = 0xBF;
    func_803A6368(ctx);
    uint64_t tb = ctx->timebase;
    *ctx = saved;
    ctx->timebase = tb;
  }
  uint32_t string = make_sis(ctx, music_title);
  if (!string) { call_guest(ctx, FN_SIS_TEXT_FREE, text, 0, 0); return 0; }
  mem_write32(ctx, text + 0x5C, string);
  mem_wf32(text + 0x24, 0.62f);                    /* font size */
  mem_wf32(text + 0x28, 0.62f);
  mem_write8(ctx, text + 0x30, music_custom ? 0x96 : 0xFF);   /* text colour */
  mem_write8(ctx, text + 0x31, music_custom ? 0xDC : 0xFF);
  mem_write8(ctx, text + 0x32, music_custom ? 0x5A : 0xFF);
  mem_write8(ctx, text + 0x33, 0xFF);
  mem_write32(ctx, text + 0x88, 16);               /* the game's texts carry this count */
  mem_write8(ctx, text + 0x4A, 1);                 /* the game sets both of these on */
  mem_write8(ctx, text + 0x4F, 2);                 /* font index used by the menus */
  mem_write8(ctx, text + 0x4D, 0);                 /* not hidden */
  mem_write8(ctx, text + 0x2C, 0x08);              /* backing plate */
  mem_write8(ctx, text + 0x2D, 0x0A);
  mem_write8(ctx, text + 0x2E, 0x14);
  mem_write8(ctx, text + 0x2F, 0xC8);
  music_text = text;
  fprintf(stderr, "[music] bar shown: %s (context %d, text %08X, link %u prio %u, box %.0fx%.0f)\n",
          music_title, music_context, text, sis_gx_link, sis_gx_prio, fpr[3], fpr[4]);
  if (getenv("MELEE_MUSIC_PROBE")) {
    fprintf(stderr, "[probe] game text %08X\n", probe_text);
    if (!valid_guest(probe_text, 0xA0)) probe_text = text;
    for (int base = 0; base < 0xA0; base += 16) {
      char mine[64] = {0}, theirs[64] = {0};
      for (int i = 0; i < 16; i++) {
        snprintf(mine + i * 3, 4, "%02X ", mem_read8(ctx, text + base + i));
        snprintf(theirs + i * 3, 4, "%02X ", mem_read8(ctx, probe_text + base + i));
      }
      fprintf(stderr, "[probe] %02X mine %s| game %s\n", base, mine, theirs);
    }
  }
  return 1;
}

/* Diagnostic: one text per GX link, stacked down the screen, so a capture shows
   which link the current scene actually draws. MELEE_MUSIC_LINKS=1. */
static void music_link_probe(Context* ctx) {
  if (link_probe_done || !getenv("MELEE_MUSIC_LINKS")) return;
  link_probe_done = 1;
  ensure_labels(ctx);
  for (int link = 0; link < 8; link++) {
    const uint32_t args[8] = { 1, 0, 9, 13, 0, (uint32_t)link, 0, 7 };
    int context = (int)call_guest_ex(ctx, FN_SIS_CONTEXT, args, 8, NULL, 0);
    fprintf(stderr, "[probe] link %d -> raw context %d\n", link, context);
    if (context < 0 || context > 32) continue;
    const uint32_t gpr[2] = { (uint32_t)context, 0 };
    const float fpr[5] = { 40.f, -60.f - link * 44.f, 0.f, 560.f, 44.f };
    uint32_t text = call_guest_ex(ctx, FN_SIS_TEXT_NEW, gpr, 2, fpr, 5);
    if (!valid_guest(text, 0xA0)) continue;
    { Context saved = *ctx; ctx->gpr[3] = text; ctx->gpr[4] = 0; func_803A6368(ctx);
      uint64_t tb = ctx->timebase; *ctx = saved; ctx->timebase = tb; }
    char label[32];
    snprintf(label, sizeof label, "LINK %d CTX %d", link, context);
    uint32_t string = make_sis(ctx, label);
    if (string) mem_write32(ctx, text + 0x5C, string);
    mem_wf32(text + 0x24, 0.7f); mem_wf32(text + 0x28, 0.7f);
    mem_write32(ctx, text + 0x30, 0xFFFFFFFFu);
    fprintf(stderr, "[probe] link %d context %d text %08X\n", link, context, text);
  }
}

/* One frame of the slide in, hold, slide out. */
static void music_text_frame(Context* ctx) {
  if (!music_timer) return;
  music_link_probe(ctx);
  /* Wait, rather than give up, until the scene has drawn a text of its own and
     shown which link it uses. */
  if (!music_text && !sis_gx_known) return;
  if (!music_text && !music_text_create(ctx)) { music_timer = 0; return; }
  if (!valid_guest(music_text, 0xA0)) { music_text = 0; return; }
  int elapsed = music_total - music_timer;
  float t = elapsed < MUSIC_SLIDE ? (float)elapsed / MUSIC_SLIDE
          : music_timer <= MUSIC_SLIDE ? (float)music_timer / MUSIC_SLIDE : 1.f;
  if (t < 0.f) t = 0.f;
  if (t > 1.f) t = 1.f;
  float eased = t * t * (3.f - 2.f * t);
  mem_wf32(music_text + 0x00, MUSIC_BAR_X - 700.f * (1.f - eased));
  if (--music_timer == 0) music_text_free(ctx);
}

/* ---- true 16:9 --------------------------------------------------------------
 * Widening the projection matrix in the renderer leaves the game itself still
 * believing it draws 4:3, so everything that sizes itself from the camera - the
 * match camera's framing, screen-filling effects, reflections - keeps its 4:3
 * shape. Widen render and texture projections together, while retaining the
 * canonical stored camera aspect so camera copies cannot widen it twice. */
/* Melee stores its camera aspect as 1.2159, not 4/3, so widening by 4/3 falls
   short of 16:9 and the picture still reads as slightly stretched. The published
   widescreen code scales by 320/219, which takes 1.2159 to 1.7767 - 16:9. */
#define ASPECT_WIDEN (320.f / 219.f)
static int widescreen_get(void);

/* The rest of the published widescreen code: camera bounds, the magnifier and
   the bubbles are all sized by these constants, which are 4:3 measurements.
   [Dan Salvato, mirrorbender, Achilles1515, UnclePunch] */
static const struct { uint32_t at, wide, original; } widescreen_constants[] = {
  { 0x803BB05Cu, 0x3EB00000u, 0x3FAAAAAAu },
  { 0x804DDB28u, 0x43660000u, 0x4322B333u },   /* camera bound  162.7 -> 230 */
  { 0x804DDB2Cu, 0xC3660000u, 0xC322B333u },
  { 0x804DDB30u, 0x3F666666u, 0x3F24D31Eu },   /* 0.6438 -> 0.9 */
  { 0x804DDB34u, 0xBF666666u, 0xBF24D31Eu },
  { 0x804DDB4Cu, 0x3D916873u, 0x3DCCCCCDu },
  { 0x804DDB58u, 0x3E4CCCCDu, 0x3E000000u },
  { 0x804DDB84u, 0x3E89FEFAu, 0x3ECCCCCDu },
};
#define NAMETAG_CONSTANT 0x804DDB84u
#define NAMETAG_WIDE 0x40DC7AE1u             /* 6.89: what the nametag wants */

static void apply_widescreen_constants(Context* ctx, int on) {
  for (unsigned i = 0; i < sizeof widescreen_constants / sizeof *widescreen_constants; i++)
    mem_write32(ctx, widescreen_constants[i].at,
                on ? widescreen_constants[i].wide : widescreen_constants[i].original);
}

/* NameTag_Create reads the same constant but needs its own value, which the
   published code supplies by patching the instruction; here it is swapped in
   around the call instead. */
static void hook_nametag_create(Context* ctx) {
  int on = widescreen_get();
  if (on) mem_write32(ctx, NAMETAG_CONSTANT, NAMETAG_WIDE);
  func_802FCF38(ctx);
  if (on) mem_write32(ctx, NAMETAG_CONSTANT, widescreen_constants[7].wide);
}

/* The published code also patches two instructions. A static recompilation does
   not execute guest code bytes, so instead each is reproduced at the one call
   site it affects, scoped so nothing else changes.
   Camera_80030BBC asks HSD_CObjGetScissor for the screen area and the code
   replaces the answer with 100..540, which is what keeps the camera and the
   magnifier working to the edges of a wide screen instead of a 4:3 box. */
static int camera_bounds_scope, offscreen_scope;

static void hook_camera_bounds(Context* ctx) {
  camera_bounds_scope = 1;
  func_80030BBC(ctx);
  camera_bounds_scope = 0;
}
static void hook_cobj_get_scissor(Context* ctx) {
  uint32_t out = ctx->gpr[4];
  func_80369FD8(ctx);
  if (camera_bounds_scope && widescreen_get() && valid_guest(out, 4)) {
    static int logged;
    if (logged < 3) {
      logged++;
      fprintf(stderr, "[wide] camera screen area %u..%u -> 100..540\n",
              mem_read16(ctx, out + 0), mem_read16(ctx, out + 2));
    }
    mem_write16(ctx, out + 0, 100);
    mem_write16(ctx, out + 2, 540);
  }
}
/* ftLib_80086A8C ignores Camera_80030CFC's answer once widescreen is on. */
static void hook_ftlib_camera(Context* ctx) {
  offscreen_scope = 1;
  func_80086A8C(ctx);
  offscreen_scope = 0;
}
static void hook_camera_offscreen(Context* ctx) {
  func_80030CFC(ctx);
  if (offscreen_scope && widescreen_get()) ctx->gpr[3] = 1;
}

static void hook_mtx_perspective(Context* ctx) {
  static int logged;
  float before = (float)ctx->fpr[2];
  if (widescreen_get()) { ctx->fpr[2] *= ASPECT_WIDEN; ctx->ps1[2] = ctx->fpr[2]; }
  if (logged < 6) {
    logged++;
    fprintf(stderr, "[wide] fov %.4f aspect %.4f -> %.4f (widescreen %d)\n",
            (float)ctx->fpr[1], before, (float)ctx->fpr[2], widescreen_get());
  }
  func_80342BEC(ctx);
}
/* Projective texture coordinates must use the same aspect as the render that
   produced them (FoD reflection, refraction and perspective shadow cameras). */
static void hook_mtx_light_perspective(Context* ctx) {
  if (widescreen_get()) { ctx->fpr[2] *= ASPECT_WIDEN; ctx->ps1[2] = ctx->fpr[2]; }
  func_80342954(ctx);
}

/* Only the gameplay camera's ground-edge calculation asks for the displayed
   aspect. Ordinary queries stay canonical: FoD copies this value into another
   camera, and lb_80013B14 compares it against the original camera descriptor. */
static unsigned camera_ground_scope;
static void hook_camera_ground_bounds(Context* ctx) {
  camera_ground_scope++;
  func_800307D0(ctx);
  camera_ground_scope--;
}
static void hook_cobj_get_aspect(Context* ctx) {
  func_80369C0C(ctx);
  if (camera_ground_scope && widescreen_get()) {
    ctx->fpr[1] *= ASPECT_WIDEN; ctx->ps1[1] = ctx->fpr[1];
  }
}

/* HSD's clear quad reads the raw CObj aspect and is drawn with the already
   widened projection. Give this calculation the matching extent, then restore
   the exact original bits before returning to simulation or copying a camera. */
static void hook_cobj_erase_screen(Context* ctx) {
  uint32_t camera = ctx->gpr[3];
  int adjust = widescreen_get() && valid_guest(camera, 0x54) &&
               mem_read8(ctx, camera + 0x50) == 1;
  uint32_t original = 0;
  if (adjust) {
    union { uint32_t u; float f; } aspect;
    original = aspect.u = mem_read32(ctx, camera + 0x44);
    aspect.f *= ASPECT_WIDEN;
    mem_write32(ctx, camera + 0x44, aspect.u);
  }
  func_803676F8(ctx);
  if (adjust) mem_write32(ctx, camera + 0x44, original);
}

/* ---- widescreen option ---------------------------------------------------- */
static int widescreen_get(void) {
  int v = aurora_link_widescreen(0, 0);
  widescreen_cached = v < 0 ? 0 : v;
  return widescreen_cached;
}
static void widescreen_set(int on) {
  if (aurora_link_aspect_lock(-1) == 1) {
    fprintf(stderr, "[hooks] Disconnect from Online to change aspect ratio.\n"); return;
  }
  widescreen_cached = aurora_link_widescreen(1, on ? 1 : 0);
  if (widescreen_cached < 0)
    fprintf(stderr, "[hooks] renderer has no widescreen setting; option is display-only\n");
  fprintf(stderr, "[hooks] widescreen %s from the options menu\n", widescreen_cached == 1 ? "16:9" : "4:3");
}

/* The icon planes replace the flag DObjs on original JObj 1. Keeping the
 * complete nine-DObj chain preserves all original animation-track indices. */
static void monitor_float(Context* ctx, uint32_t at, float value) {
  uint32_t bits; memcpy(&bits, &value, sizeof bits); mem_write32(ctx, at, bits);
}
static int build_monitor_plane(Context* ctx, uint32_t circle_dobj, int index) {
  if (monitor_quad[index] && monitor_material[index]) return 1;
  uint32_t source_mobj = mem_read32(ctx, circle_dobj + 8);
  uint32_t source_pobj = mem_read32(ctx, circle_dobj + 12);
  uint32_t source_tobj = valid_guest(source_mobj, 0x18) ? mem_read32(ctx, source_mobj + 8) : 0;
  if (!valid_guest(source_pobj, 0x18) || !valid_guest(source_tobj, 0x5C)) return 0;
  uint32_t image = make_image(ctx, MONITOR_NATIVE_W, MONITOR_NATIVE_H, 6);
  uint32_t material = area_alloc(0x18), texture = area_alloc(0x5C);
  uint32_t polygon = area_alloc(0x18), vertices = area_alloc(3 * 0x18);
  uint32_t positions = area_alloc(4 * 12), texcoords = area_alloc(4 * 8), display = area_alloc(32);
  if (!image || !material || !texture || !polygon || !vertices || !positions || !texcoords || !display) return 0;
  memcpy(ctx->ram + (mem_read32(ctx, image) & RAM_MASK),
         index ? monitor_wide_native_rgba8 : monitor_narrow_native_rgba8,
         sizeof monitor_narrow_native_rgba8);
  memcpy(ctx->ram + (material & RAM_MASK), ctx->ram + (source_mobj & RAM_MASK), 0x18);
  memcpy(ctx->ram + (texture & RAM_MASK), ctx->ram + (source_tobj & RAM_MASK), 0x5C);
  mem_write32(ctx, material + 8, texture);
  mem_write32(ctx, texture + 4, 0);
  mem_write32(ctx, texture + 0x34, 0); mem_write32(ctx, texture + 0x38, 0); /* clamp UV */
  mem_write8(ctx, texture + 0x3C, 1); mem_write8(ctx, texture + 0x3D, 1);
  /* Preserve the native material's animated alpha, but use the icon's RGB. */
  mem_write32(ctx, texture + 0x40, 0x00350010u);
  mem_write32(ctx, texture + 0x4C, image);
  mem_write32(ctx, texture + 0x50, 0); mem_write32(ctx, texture + 0x54, 0); mem_write32(ctx, texture + 0x58, 0);
  memcpy(ctx->ram + (polygon & RAM_MASK), ctx->ram + (source_pobj & RAM_MASK), 0x18);
  mem_write32(ctx, polygon + 4, 0); mem_write32(ctx, polygon + 8, vertices);
  mem_write16(ctx, polygon + 0xE, 1); mem_write32(ctx, polygon + 0x10, display);
  for (unsigned i = 0; i < 2; i++) {
    uint32_t v = vertices + i * 0x18;
    mem_write32(ctx, v, i ? 13 : 9);              /* GX_VA_TEX0 / GX_VA_POS */
    mem_write32(ctx, v + 4, 2);                  /* GX_INDEX8 */
    mem_write32(ctx, v + 8, 1);                  /* ST / XYZ */
    mem_write32(ctx, v + 12, 4);                 /* GX_F32 */
    mem_write16(ctx, v + 18, i ? 8 : 12);
    mem_write32(ctx, v + 20, i ? texcoords : positions);
  }
  mem_write32(ctx, vertices + 0x30, 255);        /* GX_VA_NULL */
  float center = index ? 6.6f : -6.6f;
  for (unsigned i = 0; i < 4; i++) {
    float x = center + (i >= 2 ? 4.f : -4.f), y = (i & 1) ? 4.9f : -1.1f;
    monitor_float(ctx, positions + i * 12, x);
    monitor_float(ctx, positions + i * 12 + 4, y);
    monitor_float(ctx, positions + i * 12 + 8, 0.1f);
    monitor_float(ctx, texcoords + i * 8, i >= 2 ? 1.f : 0.f);
    monitor_float(ctx, texcoords + i * 8 + 4, (i & 1) ? 0.f : 1.f);
    mem_write8(ctx, display + 3 + i * 2, (uint8_t)i);
    mem_write8(ctx, display + 4 + i * 2, (uint8_t)i);
  }
  mem_write8(ctx, display, 0x98); mem_write16(ctx, display + 1, 4); /* strip */
  monitor_image[index] = image; monitor_quad[index] = polygon; monitor_material[index] = material;
  return 1;
}
static void set_widescreen_presentation(Context* ctx, int enable) {
  uint32_t archive = mem_read32(ctx, MENU_ARCHIVE);
  if (!valid_guest(archive, 0x44)) return;
  if (!enable) {
    aurora_link_netplay_screen(NULL, 0, 0);
    restore_header(ctx);
    for (int i = 0; i < 2; i++) if (patched_desc[i]) { mem_write32(ctx, patched_desc[i] + 0x4C, patched_desc_old[i]); patched_desc[i] = 0; }
    for (unsigned i = 0; i < patched_monitor_count; i++)
      mem_write32(ctx, patched_monitor[i].at, patched_monitor[i].old);
    patched_monitor_count = 0; monitor_joint_desc = 0;
    return;
  }
  ensure_labels(ctx);
  patch_header(ctx, HEADER_SLOT_LANGUAGE, italic_label(ctx, "widescreen"));
  aurora_link_netplay_screen(NULL, 0, 0);         /* native geometry owns icons */
  static const char symbol[] = "MenMainConLa_Top_joint";
  uint32_t name = area_alloc(sizeof symbol);
  if (!name) return;
  memcpy(ctx->ram + (name & RAM_MASK), symbol, sizeof symbol);
  uint32_t joint = call_guest(ctx, FN_ARCHIVE_PUBLIC, archive, name, 0);
  if (monitor_joint_desc == joint && patched_monitor_count == 4) return;
  int counter = 0;
  uint32_t node = nth_node(ctx, joint, 8, 0xC, 1, &counter);
  if (!valid_guest(node, 0x40)) return;
  uint32_t meshes[9]; unsigned mesh_count = 0;
  for (uint32_t dobj = mem_read32(ctx, node + 0x10); valid_guest(dobj, 0x10) && mesh_count < 9; dobj = mem_read32(ctx, dobj + 4))
    meshes[mesh_count++] = dobj;
  if (mesh_count != 9) return;                 /* verified original model */
  int found = 0;
  for (unsigned i = 0; i < mesh_count; i++) {
    uint32_t mobj = mem_read32(ctx, meshes[i] + 8);
    if (!valid_guest(mobj, 0x18)) continue;
    for (uint32_t tdesc = mem_read32(ctx, mobj + 8); valid_guest(tdesc, 0x5C) && found < 2; tdesc = mem_read32(ctx, tdesc + 4)) {
      uint32_t image = mem_read32(ctx, tdesc + 0x4C);
      if (!valid_guest(image, 0x18)) continue;
      unsigned w = mem_read16(ctx, image + 4), h = mem_read16(ctx, image + 6);
      uint32_t replacement = (w == LABEL_NARROW_W && h == LABEL_NARROW_H) ? option_narrow
                           : (w == LABEL_WIDE_W && h == LABEL_WIDE_H) ? option_wide : 0;
      if (!replacement) continue;
      patched_desc[found] = tdesc; patched_desc_old[found] = image; found++;
      mem_write32(ctx, tdesc + 0x4C, replacement);
    }
  }
  if (!build_monitor_plane(ctx, meshes[3], 0) || !build_monitor_plane(ctx, meshes[3], 1)) return;
  for (unsigned i = 0; i < 2; i++) {
    uint32_t dobj = meshes[i ? 7 : 3];
    for (unsigned part = 0; part < 2; part++) {
      unsigned slot = patched_monitor_count++;
      patched_monitor[slot].at = dobj + (part ? 12 : 8);
      patched_monitor[slot].old = mem_read32(ctx, patched_monitor[slot].at);
      mem_write32(ctx, patched_monitor[slot].at, part ? monitor_quad[i] : monitor_material[i]);
    }
  }
  monitor_joint_desc = joint;
  fprintf(stderr, "[hooks] widescreen: %d native labels and 2 animated monitor planes installed\n", found);
}

/* HSD_JObjLoadJoint. Scope edits to this exact original language model, leaving
 * other joints untouched. Original flags, boxes, animation indices and exit
 * lifetime remain under the game's ownership. Only obsolete flag parts hide. */
#define DOBJ_HIDDEN 1u
static void hook_load_joint(Context* ctx) {
  uint32_t source = ctx->gpr[3];
  func_80370E44(ctx);
  if (!widescreen_mode || !monitor_joint_desc || source != monitor_joint_desc) return;
  uint32_t jobj = ctx->gpr[3];
  if (!valid_guest(jobj, 0x88)) return;
  uint32_t node = mem_read32(ctx, jobj + 0x10);
  if (!valid_guest(node, 0x88)) return;
  unsigned i = 0;
  for (uint32_t dobj = mem_read32(ctx, node + 0x18); valid_guest(dobj, 0x18) && i < 9; dobj = mem_read32(ctx, dobj + 4), i++)
    if (i == 2 || i == 6 || i == 8)
      mem_write32(ctx, dobj + 0x14, mem_read32(ctx, dobj + 0x14) | DOBJ_HIDDEN);
}

/* ---- dispatch hooks ------------------------------------------------------- */
static void hook_unlocked_characters(Context* ctx) {
  func_8015ED8C(ctx);
  if (valid_guest(ctx->gpr[3], 2)) mem_write16(ctx, ctx->gpr[3], 0x7FF);
}
/* The stage bitmask sits two bytes after the character one and is read by the
   same unlock module; eleven stages are unlockable, as with the fighters. */
static void hook_unlocked_stages(Context* ctx) {
  func_8015EDA4(ctx);
  if (valid_guest(ctx->gpr[3], 2)) mem_write16(ctx, ctx->gpr[3], 0x7FF);
}

static void hook_menu_item_unlocked(Context* ctx) {
  unsigned kind = ctx->gpr[3], selection = ctx->gpr[4];
  if (kind == MENU_KIND_SETTINGS && selection == SEL_SETTINGS_WIDESCREEN) { ctx->gpr[3] = 1; return; }
  if (kind == MENU_KIND_MOD_BROWSER || kind == MENU_KIND_CONTROLLERS) { ctx->gpr[3] = selection == 0; return; }
  if (is_netplay_menu(kind)) {
    Row rows[10];
    unsigned count = netplay_rows(kind, rows);
    ctx->gpr[3] = (selection < count && !rows[selection].locked) ? 1u : 0u;
    return;
  }
  func_80229938(ctx);
}

static void hook_build_menu(Context* ctx) {
  unsigned kind = mem_read8(ctx, MENU_FLOW);
  ensure_labels(ctx);
  if (kind == MENU_KIND_MOD_BROWSER || kind == MENU_KIND_CONTROLLERS) install_netplay_kind(ctx, kind, 1);
  if (is_netplay_menu(kind)) {
    Row rows[10];
    install_netplay_kind(ctx, kind, netplay_rows(kind, rows));
    patch_header(ctx, kind == MENU_KIND_NETPLAY ? HEADER_SLOT_NETPLAY : HEADER_SLOT_ROOM,
                 italic_label(ctx, kind == MENU_KIND_NETPLAY ? "netplay"
                                 : kind == MENU_KIND_BROWSER ? "rooms" : "room"));
  } else if (!widescreen_mode) {
    restore_header(ctx);
  }
  func_8022B3A0(ctx);
  if (kind == MENU_KIND_NETPLAY && getenv("MELEE_TEST_UI_TRACE"))
    fprintf(stderr, "[online-menu] built six rows phase=%d\n", ui.phase);
  apply_labels(ctx, ctx->gpr[3]);
  { extern void aurora_link_content_ready(void);aurora_link_content_ready(); }
}

static void hook_menu_frame(Context* ctx) {
  uint32_t gobj = ctx->gpr[3];
  func_8022AFEC(ctx);
  apply_labels(ctx, gobj);
  refresh_description(ctx, gobj);
}

static char mod_preview_selection[65];
static unsigned mod_preview_stable_ticks;
static void hook_main_think(Context* ctx) {
  unsigned kind = mem_read8(ctx, MENU_FLOW);
  if (profile_open && kind == MENU_KIND_NETPLAY) {
    netplay_snapshot(&ui);
    if (aurora_link_profile_back()) {
      profile_open=0; aurora_link_netplay_screen(NULL,0,0);
      call_guest(ctx,FN_SFX,0,0,0);
      open_menu(ctx,MENU_KIND_NETPLAY,3,3,0);
    }
    return;
  }
  if (kind != MENU_KIND_NETPLAY) profile_open=0;
  if (kind == MENU_KIND_CONTROLLERS) {
    if (aurora_link_controllers_back()) {
      aurora_link_netplay_screen(NULL, 0, 0);
      call_guest(ctx, FN_SFX, 0, 0, 0);
      open_menu(ctx, MENU_KIND_SETTINGS, SEL_SETTINGS_CONTROLLERS, 3, 0);
    }
    return;
  }
  if (kind != MENU_KIND_MOD_BROWSER) { mod_preview_selection[0] = 0; mod_preview_stable_ticks = 0; }
  netplay_snapshot(&ui);
  if (pending_netplay_return == 2) {
    pending_netplay_return = 0;
    rules_dirty = 0;
    room_cursor = room_action = room_rules_mode = room_rule_cursor = 0;
    /* Do not auto-connect here: keep the session's disconnect reason visible. */
    open_menu(ctx, ui.room_id ? MENU_KIND_ROOM : MENU_KIND_NETPLAY, 0, 1, 1);
    fprintf(stderr, "[hooks] returned to netplay after session exit (room %d)\n", ui.room_id);
    return;
  }
  if (rules_dirty && ui.is_host) { netplay_post(NETPLAY_CMD_RULES, 0, NULL, &edited_rules); rules_dirty = 0; }
  int start_request = 0;
  netplay_menu_tick(ctx, &start_request);
  if (start_request) {
    aurora_link_netplay_screen(NULL, 0, 0);
    call_guest(ctx, FN_SFX, 1, 0, 0);
    mem_write16(ctx, MENU_INPUT_STATE, 5);
    mem_write8(ctx, MENU_FLOW + 0x11, 1);
    uint32_t exit_data = call_guest(ctx, FN_SCENE_EXIT_DATA, 0, 0, 0);
    if (valid_guest(exit_data, 4)) mem_write8(ctx, exit_data, 2);   /* GM_VS */
    call_guest(ctx, FN_SCENE_EXIT, 0, 0, 0);
    return;
  }
  if (kind == MENU_KIND_MOD_BROWSER) {
    uint32_t buttons = call_guest(ctx, FN_MENU_INPUTS, 4, 0, 0);
    mem_write64(ctx, MENU_FLOW + 8, buttons);
    if (mod_prompt_think(ctx, buttons)) return;
    int count = ui.mod_catalog_count;
    if (count < 0) count = 0;
    if (count > NETPLAY_MOD_CATALOG) count = NETPLAY_MOD_CATALOG;
    if (mod_cursor >= count) mod_cursor = count ? count - 1 : 0;
    if(count && !mod_visible(&ui,mod_cursor))mod_cursor=mod_move(&ui,mod_cursor,1);
    if (buttons & MENU_INPUT_BACK) { netplay_post(NETPLAY_CMD_MOD_CANCEL,0,NULL,NULL); call_guest(ctx, FN_SFX, 0, 0, 0); open_menu(ctx, MENU_KIND_NETPLAY, SEL_ONLINE_MOD_BROWSER, 3, 0); }
    else if (buttons & MENU_INPUT_X) { mod_manager=!mod_manager; mod_cursor=0; if(count&&!mod_visible(&ui,0))mod_cursor=mod_move(&ui,0,1); call_guest(ctx,FN_SFX,2,0,0); }
    else if (buttons & MENU_INPUT_Y) netplay_post(NETPLAY_CMD_MOD_REFRESH, 0, NULL, NULL);
    else if ((buttons & MENU_INPUT_UP) && count) { mod_cursor = mod_move(&ui,mod_cursor,-1); call_guest(ctx, FN_SFX, 2, 0, 0); }
    else if ((buttons & MENU_INPUT_DOWN) && count) { mod_cursor = mod_move(&ui,mod_cursor,1); call_guest(ctx, FN_SFX, 2, 0, 0); }
    else if ((buttons & MENU_INPUT_CONFIRM) && count && mod_visible(&ui,mod_cursor) && ui.mod_job != 2) {
      if(mod_manager || !ui.mod_catalog[mod_cursor].installed)
        netplay_post(mod_manager ? NETPLAY_CMD_MOD_TOGGLE : NETPLAY_CMD_MOD_PREPARE,mod_cursor,NULL,NULL);
      else mod_manager=1;
    }
    /* A small, verified catalog PNG is presentation only. Never fetch a full
     * costume archive merely by moving the cursor; installation retains its
     * explicit consent flow. The worker records failures for this hash until
     * Refresh, avoiding an automatic retry loop for packages without artwork. */
    if (mem_read8(ctx, MENU_FLOW) == MENU_KIND_MOD_BROWSER && count && !ui.mod_prompt
        && !ui.mod_job && !ui.mod_catalog_loading && !ui.room_id && !ui.session_active) {
      const char* selected = ui.mod_catalog[mod_cursor].sha256;
      if (strcmp(selected, mod_preview_selection)) {
        snprintf(mod_preview_selection, sizeof mod_preview_selection, "%s", selected);
        mod_preview_stable_ticks = 0;
      } else if (mod_preview_stable_ticks < 12) ++mod_preview_stable_ticks;
      if (mod_preview_stable_ticks >= 12 && !ui.mod_inspect_loading
          && strcmp(selected, ui.mod_inspect_package))
        netplay_post(NETPLAY_CMD_MOD_PREVIEW, mod_cursor, NULL, NULL);
    } else mod_preview_stable_ticks = 0;
    return;
  }
  if (is_netplay_menu(kind)) {
    uint32_t buttons = call_guest(ctx, FN_MENU_INPUTS, 4, 0, 0);   /* 0 while the menu cooldown runs */
    mem_write64(ctx, MENU_FLOW + 8, buttons);
    if ((kind == MENU_KIND_NETPLAY || kind == MENU_KIND_BROWSER) && ui.room_id) { room_action = room_rules_mode = room_rule_cursor = 0; open_menu(ctx, MENU_KIND_ROOM, 0, 1, 1); return; }
    if (kind == MENU_KIND_ROOM && !ui.room_id) { open_menu(ctx, MENU_KIND_NETPLAY, 0, 3, 0); return; }
    netplay_screen_think(ctx, buttons);
    return;
  }
  if (kind == MENU_KIND_MAIN && mem_read16(ctx, MENU_FLOW + 2) >= SEL_MAIN_NETPLAY) {
    unsigned selection = mem_read16(ctx, MENU_FLOW + 2);
    uint32_t buttons = call_guest(ctx, FN_MENU_INPUTS, 4, 0, 0);
    mem_write64(ctx, MENU_FLOW + 8, buttons);
    if (buttons & MENU_INPUT_CONFIRM) {
      call_guest(ctx, FN_SFX, 1, 0, 0);
      room_cursor = 0; netplay_menu_opened(); open_menu(ctx, MENU_KIND_NETPLAY, 0, 1, 1);
    }
    else if (buttons & MENU_INPUT_BACK) {
      call_guest(ctx, FN_SFX, 0, 0, 0);
      mem_write8(ctx, MENU_FLOW + 0x11, 0);
      mem_write16(ctx, MENU_INPUT_STATE, 5);
      uint32_t exit_data = call_guest(ctx, FN_SCENE_EXIT_DATA, 0, 0, 0);
      if (valid_guest(exit_data, 4)) mem_write8(ctx, exit_data, 0);   /* GM_TITLE */
      call_guest(ctx, FN_SCENE_EXIT, 0, 0, 0);
    } else if (buttons & MENU_INPUT_UP) { call_guest(ctx, FN_SFX, 2, 0, 0); mem_write16(ctx, MENU_FLOW + 2, (uint16_t)(selection - 1)); }
    else if (buttons & MENU_INPUT_DOWN) { call_guest(ctx, FN_SFX, 2, 0, 0); mem_write16(ctx, MENU_FLOW + 2, 0); }
    return;
  }
  func_8022DB10(ctx);
}

/* Diagnostic start-stage selection uses Adventure's own CSS exit and scene
 * constructors. It does not replace stage loading, fighters, AI or gameplay. */
extern void func_801B4350(Context*);
static void hook_adventure_css_leave(Context* ctx){
 func_801B4350(ctx);
 const char* text=getenv("MELEE_TEST_ADVENTURE_STAGE");
 if(text&&*text){char* end=NULL;long stage=strtol(text,&end,10);
  if(end&&!*end&&stage>=0&&stage<12){
   call_guest(ctx,0x801A42A0u,(uint32_t)stage*8u,0,0);
   fprintf(stderr,"[adventure-test] starting native stage block %ld\n",stage);
  }
 }
}

static void hook_settings_think(Context* ctx) {
  uint32_t gp = ctx->gpr[3];
  if (mem_read8(ctx, MENU_FLOW)==MENU_KIND_CONTROLLERS) { hook_main_think(ctx); return; }
  if (mem_read8(ctx, MENU_FLOW)==MENU_KIND_SETTINGS && mem_read16(ctx, MENU_FLOW+2)==SEL_SETTINGS_CONTROLLERS) {
    uint16_t cooldown=mem_read16(ctx,MENU_INPUT_STATE);
    uint32_t buttons=call_guest(ctx,FN_MENU_INPUTS,4,0,0);
    if (buttons & MENU_INPUT_CONFIRM) {
      mem_write64(ctx,MENU_FLOW+8,buttons);
      call_guest(ctx,FN_SFX,1,0,0);
      open_menu(ctx,MENU_KIND_CONTROLLERS,0,1,1);
      return;
    }
    /* The stock reader decrements cooldown. Let the stock think do it once. */
    mem_write16(ctx,MENU_INPUT_STATE,cooldown);
    ctx->gpr[3]=gp;
  }
  func_8022D104(ctx);
  if (mem_read8(ctx, MENU_FLOW) != MENU_KIND_SETTINGS || mem_read16(ctx, MENU_FLOW + 2) != SEL_SETTINGS_WIDESCREEN) return;
  if (!(mem_read64(ctx, MENU_FLOW + 8) & MENU_INPUT_CONFIRM)) return;
  call_guest(ctx, FN_SFX, 1, 0, 0);
  widescreen_mode = 1;
  set_widescreen_presentation(ctx, 1);
  call_guest(ctx, FN_LANGUAGE_OPEN, 1, 0, 0);
  call_guest(ctx, FN_GOBJ_DESTROY, gp, 0, 0);
}

static int from_language_code(Context* ctx) { return ctx->lr >= LANGUAGE_CODE_BEGIN && ctx->lr < LANGUAGE_CODE_END; }
static void hook_language_open(Context* ctx) {
  if (!widescreen_mode) set_widescreen_presentation(ctx, 0);
  func_8024C5C0(ctx);
}
static void hook_language_get(Context* ctx) {
  if (widescreen_mode && from_language_code(ctx)) { ctx->gpr[3] = widescreen_get() ? 1u : 0u; return; }
  func_8000ADF4(ctx);
}
static void hook_language_set(Context* ctx) {
  if (widescreen_mode && from_language_code(ctx)) { widescreen_set((int)ctx->gpr[3] == 1); return; }
  func_8000AE18(ctx);
}
static void hook_menu_change(Context* ctx) {
  if (widescreen_mode && from_language_code(ctx) && ctx->gpr[3] == MENU_KIND_SETTINGS && ctx->gpr[4] == 4) {
    ctx->gpr[4] = SEL_SETTINGS_WIDESCREEN;
    widescreen_mode = 0;
    set_widescreen_presentation(ctx, 0);
  }
  func_80229894(ctx);
}
static void hook_menu_reload(Context* ctx) {
  if (widescreen_mode && from_language_code(ctx)) { widescreen_mode = 0; pending_settings_hover = 1; set_widescreen_presentation(ctx, 0); }
  func_80229860(ctx);
}

/* Which GX link a scene draws its text on is the scene's business: the menus
   use link 7, a match uses link 9, and a link the scene does not walk renders
   nothing at all. Rather than guess, the link the game itself last used for a
   text object is remembered here and the now-playing bar is put on the same
   one. FN_SIS_RENDER is the SIS library's text render callback. */
/* Online borrows the original cyan hover animation, whose right-hand child
 * is the 1-P mode illustration/list. Omit that child only during this draw;
 * the original animation, live JObjs and every pointer are restored immediately.
 * MainMenuData.tree[14] is the hover root in mn_8022B3A0/8022ADD8. */
#define ONLINE_MENU_DRAW 0x817FFE20u
extern void func_80391070(Context*);
static void hook_online_menu_draw(Context* ctx) {
  uint32_t gobj = ctx->gpr[3];
  uint32_t data = valid_guest(gobj, 0x30) ? mem_read32(ctx, gobj + 0x2C) : 0;
  uint32_t hover = 0, child = 0, dobj = 0;
  if (valid_guest(data, 0xB0)) {
    unsigned kind = mem_read8(ctx, data), selected = mem_read8(ctx, data + 1);
    if (kind == MENU_KIND_NETPLAY || (kind == MENU_KIND_MAIN && selected == SEL_MAIN_NETPLAY))
      hover = mem_read32(ctx, data + 4 + 14 * 4);
  }
  if (valid_guest(hover, 0x88)) {
    child = mem_read32(ctx, hover + 0x10); dobj = mem_read32(ctx, hover + 0x18);
    mem_write32(ctx, hover + 0x10, 0); mem_write32(ctx, hover + 0x18, 0);
  } else hover = 0;
  func_80391070(ctx);
  if (hover) { mem_write32(ctx, hover + 0x10, child); mem_write32(ctx, hover + 0x18, dobj); }
}

static void hook_gx_link(Context* ctx) {
  costume_art_gx_link(ctx);
  if (ctx->gpr[4] == 0x80391070u && ctx->gpr[5] == 4 &&
      ctx->lr >= 0x8022B3A0u && ctx->lr < 0x8022BA1Cu)
    ctx->gpr[4] = ONLINE_MENU_DRAW;
  if (ctx->gpr[4] == FN_SIS_RENDER) {
    if (music_creating && sis_gx_known) { ctx->gpr[5] = sis_gx_link; ctx->gpr[6] = sis_gx_prio; }
    else { sis_gx_link = ctx->gpr[5] & 0xFF; sis_gx_prio = ctx->gpr[6] & 0xFF; sis_gx_known = 1; }
  }
  if (getenv("MELEE_GXLINK_LOG"))
    fprintf(stderr, "[gxlink] gobj %08X callback %08X link %u prio %u\n",
            ctx->gpr[3], ctx->gpr[4], ctx->gpr[5] & 0xFF, ctx->gpr[6] & 0xFF);
  func_8039069C(ctx);
}

static void hook_text_sis(Context* ctx) {
  uint32_t text = ctx->gpr[3], index = ctx->gpr[4];
  func_803A6368(ctx);
  if (!valid_guest(text, 0xA0)) return;
  if (text != music_text) probe_text = text;
  unsigned kind = mem_read8(ctx, MENU_FLOW), hovered = mem_read16(ctx, MENU_FLOW + 2);
  uint32_t replacement = 0;
  if (is_netplay_menu(kind)) {
    Row rows[10];
    unsigned count = netplay_rows(kind, rows);
    netplay_description(kind, hovered_row(kind, rows, count, hovered), sis_text, sizeof sis_text);
    snprintf(sis_shown, sizeof sis_shown, "%s", sis_text);
    replacement = make_sis(ctx, sis_text);
  } else if (index == 0 && kind == MENU_KIND_MAIN && hovered == SEL_MAIN_NETPLAY) {
    replacement = make_sis(ctx, "Play online against other YAMPP players.");
  } else if (kind == MENU_KIND_MOD_BROWSER) {
    replacement = make_sis(ctx, "Browse compatible fighters, stages, costumes and UI mods.");
  } else if (index == 0x9D && kind == MENU_KIND_SETTINGS && hovered == SEL_SETTINGS_WIDESCREEN) {
    replacement = make_sis(ctx, aurora_link_aspect_lock(-1) == 1 ? "Disconnect from Online to change aspect ratio." : "Choose the 4:3 or 16:9 screen shape.");
  } else if (index == 0xBF && widescreen_mode) {
    replacement = make_sis(ctx, aurora_link_aspect_lock(-1) == 1 ? "Disconnect from Online to change aspect ratio." : "Choose 4:3 or 16:9 for your display.");
  }
  if (replacement) mem_write32(ctx, text + 0x5C, replacement);
}

/* Retain the original frame, grid, materials and animation. Only the two
 * native header labels are replaced by the Online screen's native-font text.
 * Restore exact DObj flags before every original animation tick. */
extern void func_80229BF4(Context*);
static void hook_menu_panel_frame(Context* ctx) {
  uint32_t gobj = ctx->gpr[3];
  if (panel_headers.gobj == gobj) {
    for (unsigned i=0; i<panel_headers.count; ++i)
      if (valid_guest(panel_headers.dobj[i], 0x18))
        mem_write32(ctx, panel_headers.dobj[i]+0x14, panel_headers.flags[i]);
  }
  memset(&panel_headers, 0, sizeof panel_headers);
  func_80229BF4(ctx);
  unsigned kind = mem_read8(ctx, MENU_FLOW);
  if ((kind != MENU_KIND_CONTROLLERS && kind != MENU_KIND_ROOM && kind != MENU_KIND_BROWSER && kind != MENU_KIND_MOD_BROWSER && kind != MENU_KIND_NETPLAY) || !valid_guest(gobj, 0x2C)) return;
  uint32_t root = mem_read32(ctx, gobj+0x28);
  if (!valid_guest(root,0x80)) return;
  panel_headers.gobj=gobj;
  /* The Online submenu keeps its native right-hand Online title; only its
     inherited parent label (joint 84: 1-P Mode) is unrelated. */
  int first = kind == MENU_KIND_NETPLAY && !ui.room_name_open && !profile_open ? 84 : 82;
  int last = kind == MENU_KIND_NETPLAY && !ui.room_name_open && !profile_open ? 84 : 86;
  for (int index=first; index<=last; ++index) {
    int counter=0; uint32_t joint=nth_node(ctx,root,0x10,8,index,&counter);
    if (!valid_guest(joint,0x80)) continue;
    for (uint32_t dobj=mem_read32(ctx,joint+0x18);valid_guest(dobj,0x18)&&panel_headers.count<16;dobj=mem_read32(ctx,dobj+4)) {
      unsigned at=panel_headers.count++;panel_headers.dobj[at]=dobj;
      panel_headers.flags[at]=mem_read32(ctx,dobj+0x14);
      mem_write32(ctx,dobj+0x14,panel_headers.flags[at]|1u);
    }
  }
}

/* Original menus keep their palette. Custom screens use a light-cyan target
 * through the same native 16-frame light lerp, including restoration on exit. */
static void hook_menu_lights_frame(Context* ctx) {
  unsigned kind = mem_read8(ctx, MENU_FLOW);
  unsigned selection = mem_read16(ctx, MENU_FLOW + 2);
  int custom = (kind == MENU_KIND_MAIN && selection >= SEL_MAIN_NETPLAY) ||
               is_netplay_menu(kind) || kind == MENU_KIND_MOD_BROWSER;
  uint32_t original_color = mem_read32(ctx, 0x804D4B50u);
  if (custom && custom_light_color) {
    /* The original callback inlines both palette switches. Substitute only
       while it runs, then restore the stock color before any other caller. */
    uint32_t previous = mem_read32(ctx, MENU_FLOW + 0x14);
    mem_write32(ctx, MENU_FLOW + 0x14, previous == custom_light_color ? 0x804D4B50u : 0);
    mem_write32(ctx, 0x804D4B50u, mem_read32(ctx, custom_light_color));
    mem_write8(ctx, MENU_FLOW, 1); mem_write16(ctx, MENU_FLOW + 2, 0);
  }
  func_8022C128(ctx);
  if (custom && custom_light_color) {
    mem_write32(ctx, 0x804D4B50u, original_color);
    mem_write32(ctx, MENU_FLOW + 0x14, custom_light_color);
    mem_write8(ctx, MENU_FLOW, (uint8_t)kind); mem_write16(ctx, MENU_FLOW + 2, (uint16_t)selection);
  }
}

static void hook_menu_light_color(Context* ctx) {
  unsigned kind = ctx->gpr[3], selection = ctx->gpr[4];
  if ((kind == MENU_KIND_MAIN && selection >= SEL_MAIN_NETPLAY) ||
      is_netplay_menu(kind) || kind == MENU_KIND_MOD_BROWSER) { ctx->gpr[3] = 0; return; }
  func_8022C010(ctx);
}

static void hook_init_registers(Context* ctx) {
  /* __init_registers sets r1/r2/r13 from DOL constants. On real hardware this
   * runs before OSInit, but the native runtime already owns the stack (r1).
   * Preserve it; only apply r2 (SDA_BASE) and r13 (SDA2_BASE). */
  ctx->gpr[2]  = 0x804DF9E0u;
  ctx->gpr[13] = 0x804DB6A0u;
}

extern RecFn controller_gameplay_lookup(uint32_t);
/* HSD_MemAlloc (0x8037F1E4). The original asserts on a null result and stops
 * the game there, which is what a player sees as a freeze with no explanation:
 * a menu that will not open, a mode that will not continue to its next stage.
 *
 * The runtime reserves part of the guest arena for its own menu artwork and
 * costume data, so when this happens the first question is always whether that
 * reservation is why. Print the request and the reservation together so the
 * answer is in the log rather than in an investigation. */
extern void func_8037F1E4(Context*);
extern unsigned costume_area_bytes(void);
static void hook_hsd_memalloc(Context* ctx) {
  uint32_t size = ctx->gpr[3];
  func_8037F1E4(ctx);
  if (ctx->gpr[3]) return;
  static int reported;
  if (reported++ < 8)
    fprintf(stderr, "[heap] the game could not allocate %u bytes and will stop here."
                    " Host reservation: %u bytes for menus (%u used), %u for costumes.\n",
            size, area_size, area_used, costume_area_bytes());
}

RecFn hooks_lookup(uint32_t addr) {
  RecFn control = controller_gameplay_lookup(addr); if (control) return control;
  RecFn art = costume_art_lookup(addr); if (art) return art;
  switch (addr) {
  case 0x8037F1E4u: return hook_hsd_memalloc;
  case 0x80005340u: return hook_init_registers;
  case 0x8015ED8Cu: return hook_unlocked_characters;
  case 0x8015EDA4u: return hook_unlocked_stages;
  case 0x802FCF38u: return hook_nametag_create;
  case 0x80030BBCu: return hook_camera_bounds;
  case 0x800307D0u: return hook_camera_ground_bounds;
  case 0x80369FD8u: return hook_cobj_get_scissor;
  case 0x80086A8Cu: return hook_ftlib_camera;
  case 0x80030CFCu: return hook_camera_offscreen;
  case 0x80229938u: return hook_menu_item_unlocked;
  case 0x8022C010u: return hook_menu_light_color;
  case 0x8022C128u: return hook_menu_lights_frame;
  case 0x8022B3A0u: return hook_build_menu;
  case 0x80229BF4u: return hook_menu_panel_frame;
  case ONLINE_MENU_DRAW: return hook_online_menu_draw;
  case 0x8022AFECu: return hook_menu_frame;
  case 0x8022DB10u: return hook_main_think;
  case 0x8022D104u: return hook_settings_think;
  case 0x801B4350u: return hook_adventure_css_leave;
  case 0x8024C5C0u: return hook_language_open;
  case 0x8000ADF4u: return hook_language_get;
  case 0x8000AE18u: return hook_language_set;
  case 0x80229894u: return hook_menu_change;
  case 0x80229860u: return hook_menu_reload;
  case 0x803A6368u: return hook_text_sis;
  case 0x80370E44u: return hook_load_joint;
  case 0x8039069Cu: return hook_gx_link;
  case 0x80342BECu: return hook_mtx_perspective;
  case 0x80342954u: return hook_mtx_light_perspective;
  case 0x803676F8u: return hook_cobj_erase_screen;
  case 0x80369C0Cu: return hook_cobj_get_aspect;
  case 0x8037750Cu: { extern void netplay_master_status(Context*); return netplay_master_status; }
  case 0x801A5F64u: { extern void netplay_results_exit(Context*); return netplay_results_exit; }
  case 0x801A4D34u: { extern void netplay_scene_loop(Context*); return netplay_scene_loop; }
  case 0x80023F28u: { extern void music_bgm_play(Context*); return music_bgm_play; }
  default: return NULL;
  }
}

void hooks_trace(uint32_t addr, Context* ctx) {
  if(addr==0x801A43A0u){
    static int applied, opening_pending;
    if(!applied){
      applied=1;
      const char* resume=getenv("MELEE_CONTENT_RETURN");
      const char* vanilla=getenv("MELEE_VANILLA_BOOT");
      if(resume&&*resume){ctx->gpr[3]=1;mem_write8(ctx,0x80479D30u,1);}
      else if(vanilla&&atoi(vanilla)==1){
        /* gmboot.c: cold boot checks the card, then GM_OPENING_MV. The
         * original movie scene owns completion, title and A/Start skipping. */
        mem_write32(ctx,0x8046B0F0u,0);
        opening_pending=1;
        fprintf(stderr,"[boot] normal startup: opening movie then title\n");
      }
    }
    /* Selective m-ex content can redirect cold boot to MENU. Restore only
     * that first destination; hot content returns keep their room/menu. */
    if(opening_pending && (ctx->gpr[3]==0 || ctx->gpr[3]==1)){
      Context saved=*ctx;ctx->gpr[3]=0;func_801BF708(ctx);
      uint64_t tb=ctx->timebase;*ctx=saved;ctx->timebase=tb;
      ctx->gpr[3]=0x18;mem_write8(ctx,0x80479D30u,0x18);
    }
    if(opening_pending && ctx->gpr[3]==0x18){
      opening_pending=0;fprintf(stderr,"[boot] native opening movie entered\n");
    }
    return;
  }
  /* Isolated diagnostic: give Kirby the original Game & Watch copy after
   * spawning has finished. The guest loader binds the copied model itself. */
  extern int netplay_session_active(void);
  static uint32_t copy_test_gobj;
  static unsigned copy_test_frames;
  const char* copy_test=getenv("MELEE_TEST_KIRBY_GAMEWATCH");
  if(copy_test && strcmp(copy_test,"plain") && !netplay_session_active()) {
    if(addr==0x800EE5C0u) {copy_test_gobj=ctx->gpr[3];copy_test_frames=0;}
    if(addr==0x8037750Cu && copy_test_gobj && ++copy_test_frames==240) {
      uint32_t fp=valid_guest(copy_test_gobj,0x30)?mem_read32(ctx,copy_test_gobj+0x2c):0;
      if(valid_guest(fp,0x2300) && mem_read32(ctx,fp+4)==4) {
        Context saved=*ctx;
        ctx->gpr[3]=copy_test_gobj;ctx->gpr[4]=24;ctx->gpr[5]=1;
        func_800F1BAC(ctx);*ctx=saved;
        fprintf(stderr,"[render-test] native Game & Watch copy activated after spawn\n");
      }
      copy_test_gobj=0;
    }
  }

  if (addr == 0x8037750Cu) {
    /* MELEE_TEST_STAGE=<id> pins the versus stage so a capture can go straight
       to the stage being looked at. */
    const char* forced = getenv("MELEE_TEST_STAGE");
    if (forced && *forced && atoi(forced)>=0 && atoi(forced)<128) {
      uint32_t sss = mem_read32(ctx, 0x804D6C90u);
      if (valid_guest(sss, 0x140)) mem_write8(ctx, sss + 3, (unsigned char)atoi(forced));
    }
    apply_widescreen_constants(ctx, widescreen_get());
    { extern void music_silence_disc(Context*); music_silence_disc(ctx); }
    music_text_frame(ctx); return;                              /* one game frame */
  }
  if (addr == 0x80375428u) { costume_art_reset(ctx); reset_menu_foreground(); music_text_reset(); aurora_link_netplay_screen(NULL, 0, 0); }                  /* scene heap recreated */
  if (addr == 0x800236DCu) { extern void music_bgm_stop(void); music_bgm_stop(); }
  if (addr == 0x8022DDA8u) {         /* mnMain_Scene_OnEnter(MenuEnterData*) */
    reset_menu_foreground();
    label_archive = 0; bold_table = 0; widescreen_mode = 0;
    monitor_joint_desc = 0; patched_monitor_count = 0;
    patched_header_old = 0; patched_desc[0] = patched_desc[1] = 0; sis_shown[0] = 0;
    uint32_t enter = ctx->gpr[3];
    if (pending_netplay_return) {
      if (valid_guest(enter, 4)) {
        mem_write8(ctx, enter, MENU_KIND_MAIN);
        mem_write8(ctx, enter + 1, SEL_MAIN_NETPLAY);
      }
      pending_netplay_return = 2;
    }
    if (pending_settings_hover) {
      pending_settings_hover = 0;
      if (valid_guest(enter, 4) && mem_read8(ctx, enter) == MENU_KIND_SETTINGS) mem_write8(ctx, enter + 1, SEL_SETTINGS_WIDESCREEN);
    }
    { static int resume_applied;
      if(!resume_applied && getenv("MELEE_CONTENT_RETURN") && valid_guest(enter,4)){
        resume_applied=1;
        const char* room=getenv("MELEE_CONTENT_JOIN_ROOM");
        mem_write8(ctx,enter,room&&atoi(room)>0?MENU_KIND_BROWSER:MENU_KIND_MOD_BROWSER);mem_write8(ctx,enter+1,0);
        mod_manager=1;mod_cursor=0;
        if(room&&atoi(room)>0)netplay_menu_opened();
        else netplay_post(NETPLAY_CMD_MOD_REFRESH,0,NULL,NULL);
      }
    }
    /* Diagnostic: open a chosen menu straight away, so a scripted capture does
       not depend on boot timing. MELEE_TEST_MENU=kind[:selection]. */
    {
      const char* want = getenv("MELEE_TEST_MENU");
      if (!pending_netplay_return && want && *want && valid_guest(enter, 4)) {
        char* end = NULL;
        long kind = strtol(want, &end, 10);
        long selection = (end && *end == ':') ? strtol(end + 1, NULL, 10) : 0;
        if (kind >= 0 && kind < 0x22) {
          mem_write8(ctx, enter + 0, (uint8_t)kind);
          mem_write8(ctx, enter + 1, (uint8_t)selection);
          fprintf(stderr, "[hooks] diagnostic: opening menu kind %ld selection %ld\n", kind, selection);
        }
      }
    }
  }
  netplay_scene_event(ctx, addr);
}

void hooks_init(Context* ctx, uint32_t area, uint32_t size) {
  { extern void music_init(void); music_init(); }
  area_ctx = ctx;
  area_base = area; area_size = size; area_used = 0;
  /* Original five main entries plus Online; Mod Browser is inside Online. */
  uint32_t anims = area_alloc(6 * 12);
  if (anims) {
    /* The sixth row has no preview of its own, so it borrows one. Row 1's panel
       renders black behind it; row 0's is the cyan one. */
    const char* pick = getenv("MELEE_NETPLAY_PREVIEW");
    int source = pick && *pick ? atoi(pick) : 0;
    if (source < 0 || source > 4) source = 0;
    memcpy(ctx->ram + (anims & RAM_MASK), ctx->ram + (MENU_MAIN_ANIM & RAM_MASK), 5 * 12);
    memcpy(ctx->ram + ((anims + 60) & RAM_MASK), ctx->ram + ((MENU_MAIN_ANIM + source * 12) & RAM_MASK), 12);
    mem_write32(ctx, MENU_KIND_TABLE + 0, anims);
    mem_write8(ctx, MENU_KIND_TABLE + 0xC, 6);
  }
  { extern void controller_gameplay_init(Context*,uint32_t); controller_gameplay_init(ctx,area+100); }
  /* Controllers replaces the deflicker page, including its extra panel art.
   * Use the ordinary Options frame animation for all three transition phases. */
  memcpy(ctx->ram + ((0x803EAE8Cu + MENU_KIND_CONTROLLERS * 36u) & RAM_MASK),
         ctx->ram + ((0x803EAE8Cu + MENU_KIND_SETTINGS * 36u) & RAM_MASK), 36);
  custom_light_color = area + 96;
  mem_write32(ctx, custom_light_color, 0x80FFFFFFu);
  area_base = area + 128; area_size = size - 128; area_used = 0;   /* keep that table permanent */
  fprintf(stderr, "[hooks] guest patch area %08X (%u bytes); Online submenu with Mod Browser and widescreen option enabled\n", area, size);
}
