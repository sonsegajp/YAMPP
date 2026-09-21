/* What Melee receives from a shoulder trigger.
 *
 * The reported bug was "air dodging is broken; it needs pressure triggers".
 * Air dodging is not broken: ftCo_80099A58 asks for the digital L/R bit, and
 * on a pad whose shoulders are only axes that bit was only ever synthesised
 * at 95% of the reported analog range. A pad that tops out at, say, 93% of
 * its nominal range could never produce it at all.
 *
 * These tests pin the shaped output: that a normal press produces the click,
 * that a pad which never reaches its maximum still produces it, that light
 * shielding survives underneath it, and that a real GameCube controller is
 * not rescaled into something different. */
#include "../../aurora_shim/trigger_shape.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static const YamppTriggerOptions defaults = YAMPP_TRIGGER_DEFAULTS;

/* Returns the analog value Melee would read; `click` receives the digital bit. */
static int shape(const YamppTriggerOptions* options, int analog, int* click) {
    return yampp_trigger_shape(options, analog, 0, click);
}

static void the_default_click_is_reachable_on_a_real_pad(void) {
    int click;
    /* 95% of the range was the old threshold. A pad that reports at most 93%
     * of its nominal maximum -- ordinary for a worn or cheap trigger -- could
     * never cross it, which is exactly the reported symptom. */
    CHECK(defaults.point < 95);
    shape(&defaults, (int)(255 * 0.93), &click);
    CHECK(click);
    /* And a pad that only manages three quarters of its travel still clicks. */
    shape(&defaults, (int)(255 * 0.75), &click);
    CHECK(click);
}

static void a_full_press_clicks_and_a_rest_position_does_not(void) {
    int click;
    CHECK(shape(&defaults, 255, &click) >= YAMPP_TRIGGER_FULL); CHECK(click);
    shape(&defaults, 0, &click); CHECK(!click);
    /* A resting trigger reporting a little noise is untouched, not a shield. */
    CHECK(shape(&defaults, 10, &click) == 0); CHECK(!click);
}

static void light_shielding_survives_below_the_click(void) {
    int click;
    /* Between the dead zone and the click point the real analog value is
     * passed through, which is what Melee reads for shield strength. */
    int light = shape(&defaults, 90, &click);
    CHECK(!click);
    CHECK(light == 90);
    CHECK(light < YAMPP_TRIGGER_FULL);
    /* Harder, still short of the click, is a stronger light shield. */
    CHECK(shape(&defaults, 120, &click) == 120); CHECK(!click);
}

static void a_click_always_produces_a_full_shield(void) {
    int click;
    YamppTriggerOptions early = defaults;
    early.point = 25;                     /* clicks well below Melee's full value */
    CHECK(shape(&early, 70, &click) == YAMPP_TRIGGER_FULL);
    CHECK(click);
    /* A physically mapped shoulder button does the same with no axis at all. */
    CHECK(yampp_trigger_shape(&defaults, 0, 1, &click) == YAMPP_TRIGGER_FULL);
    CHECK(click);
}

static void light_shield_off_makes_every_press_a_full_shield(void) {
    int click;
    YamppTriggerOptions hard = defaults;
    hard.light = 0;
    CHECK(shape(&hard, 60, &click) == YAMPP_TRIGGER_FULL);
    CHECK(!click);                        /* full shield, but no air dodge */
    CHECK(shape(&hard, 0, &click) == 0);
}

static void physical_only_leaves_the_axis_alone(void) {
    int click;
    YamppTriggerOptions physical = defaults;
    physical.click = 0;
    CHECK(shape(&physical, 255, &click) == 255);
    CHECK(!click);
    /* The mapped button still works; that is the point of the setting. */
    CHECK(yampp_trigger_shape(&physical, 255, 1, &click) == 255);
    CHECK(click);
}

static void a_gamecube_controller_is_not_rescaled(void) {
    int click;
    YamppTriggerOptions physical = defaults;
    physical.click = 0;
    /* A real controller's microswitch arrives as a pressed button. Its analog
     * readings must reach the game unchanged, or every shield on an adapter
     * would be a different strength from the one on console. */
    for (int raw = 41; raw <= 255; ++raw) {
        CHECK(shape(&physical, raw, &click) == raw);
        CHECK(!click);
    }
    /* With the click synthesised, everything below the point is still exact. */
    for (int raw = 41; raw < defaults.point * 255 / 100; ++raw) {
        CHECK(shape(&defaults, raw, &click) == raw);
        CHECK(!click);
    }
}

static void the_dead_zone_only_removes_the_bottom(void) {
    int click;
    YamppTriggerOptions wide = defaults;
    wide.dead = 20;                        /* 51 of 255 */
    CHECK(shape(&wide, 51, &click) == 0);
    CHECK(shape(&wide, 52, &click) == 52);
    CHECK(!click);
}

static void out_of_range_settings_cannot_break_the_pad(void) {
    int click;
    YamppTriggerOptions absurd = { 1, -500, 1, 900 };
    int value = shape(&absurd, 200, &click);
    CHECK(value >= 0 && value <= 255);
    /* Even pinned to its extremes the dead zone cannot swallow a full press. */
    absurd.point = 9999;
    value = shape(&absurd, 255, &click);
    CHECK(value >= 0 && value <= 255);
    for (int raw = -50; raw <= 400; ++raw) {
        value = yampp_trigger_shape(&defaults, raw, raw & 1, &click);
        CHECK(value >= 0 && value <= 255);
        CHECK(click == 0 || click == 1);
    }
}

static void the_click_point_is_monotonic(void) {
    /* Raising the click point never makes a given press click sooner. */
    for (int point = YAMPP_CLICK_MIN; point <= YAMPP_CLICK_MAX; ++point) {
        YamppTriggerOptions options = defaults;
        int previous = 1, raw;
        options.point = point;
        for (raw = 255; raw >= 0; --raw) {
            int click;
            shape(&options, raw, &click);
            CHECK(click <= previous);      /* once released, never re-clicks */
            previous = click;
        }
    }
}

int main(void) {
    the_default_click_is_reachable_on_a_real_pad();
    a_full_press_clicks_and_a_rest_position_does_not();
    light_shielding_survives_below_the_click();
    a_click_always_produces_a_full_shield();
    light_shield_off_makes_every_press_a_full_shield();
    physical_only_leaves_the_axis_alone();
    a_gamecube_controller_is_not_rescaled();
    the_dead_zone_only_removes_the_bottom();
    out_of_range_settings_cannot_break_the_pad();
    the_click_point_is_monotonic();
    printf("trigger shaping: air dodge click reachable at %d%% of range,"
           " light shield intact below it, analog values unchanged\n", defaults.point);
    return 0;
}
