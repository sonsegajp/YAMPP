/* Turning a shoulder axis into what Melee actually reads.
 *
 * Melee asks for the digital L/R *click* to air dodge, wave dash and hard
 * shield (ftCo_80099A58 tests `pressed_buttons & (HSD_PAD_R | HSD_PAD_L)`);
 * it never derives that bit from how hard the trigger is squeezed. A
 * GameCube controller has a real microswitch under the analog travel, so the
 * bit exists in hardware. A gamepad whose shoulders are only axes has no such
 * switch, and the renderer only synthesised one at 95% of the reported range
 * -- which a pad that never quite reports its maximum can never reach. That
 * is the whole reason air dodging appeared to need a crushing press.
 *
 * Separately, Melee reads the analog value for shield strength, clamped so
 * that 140 is a full shield (gmmain.c: clamp_analogLRMax = 140, scale = 140).
 * So the analog value is passed through rather than rescaled: a real
 * controller keeps reporting its own numbers, and everything below the click
 * point stays a light shield exactly as it does on hardware.
 *
 * Pure and free of SDL and of the renderer, so it can be tested directly. */
#ifndef YAMPP_TRIGGER_SHAPE_H
#define YAMPP_TRIGGER_SHAPE_H
#include <stdint.h>

/* Melee's clamp_analogLRMax: the analog value at which a shield is full. */
#define YAMPP_TRIGGER_FULL 140
#define YAMPP_CLICK_MIN 20
#define YAMPP_CLICK_MAX 100
#define YAMPP_DEAD_MAX 40

typedef struct YamppTriggerOptions {
  int click;   /* 1 synthesise the click from the analog axis, 0 physical only */
  int point;   /* percent of the reported analog range where the click engages */
  int light;   /* 1 partial presses light-shield, 0 any press is a full shield */
  int dead;    /* percent below which the trigger reads as untouched */
} YamppTriggerOptions;

/* 55% of the reported range is 140, the value Melee already treats as a full
 * shield, so any pad that can hard-shield can also air dodge. */
#define YAMPP_TRIGGER_DEFAULTS { 1, 55, 1, 8 }

static int yampp_trigger_clamp(int value, int low, int high) {
  return value < low ? low : value > high ? high : value;
}

/* Shape one shoulder. `analog` is the raw 0..255 reading and `pressed` says
 * whether a physically mapped shoulder *button* is already down. Returns the
 * analog value to report and sets *click to whether the digital bit should
 * be set. */
static uint8_t yampp_trigger_shape(const YamppTriggerOptions* options,
                                   int analog, int pressed, int* click) {
  int dead = yampp_trigger_clamp(options->dead, 0, YAMPP_DEAD_MAX) * 255 / 100;
  int point = yampp_trigger_clamp(options->point, YAMPP_CLICK_MIN, YAMPP_CLICK_MAX) * 255 / 100;
  int value = yampp_trigger_clamp(analog, 0, 255);
  int down = pressed != 0;
  if (value <= dead) value = 0;
  if (options->click && value >= point) down = 1;
  /* A click means the shoulder is all the way down however the axis reports
   * it, so the shield it produces is a full one. */
  if (down) { if (value < YAMPP_TRIGGER_FULL) value = YAMPP_TRIGGER_FULL; }
  else if (!options->light && value > 0) value = YAMPP_TRIGGER_FULL;
  *click = down;
  return (uint8_t)yampp_trigger_clamp(value, 0, 255);
}
#endif
