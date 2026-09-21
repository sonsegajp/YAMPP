/* Physical button edges belong to the host input clock, not replayed frames.
 * Keep a short tap until the next forward frame, without extending a release
 * merely because an older guest PAD alarm observed the button held.
 *
 * Analog triggers are latched the same way, at their peak. The runtime samples
 * pads on an independent input clock that runs faster than the simulation, so
 * without a peak the frame keeps whichever of the two-or-more samples happened
 * to land last -- which silently swallows the squeeze that would have crossed
 * the shield or airdodge threshold. Sticks are left at their newest value: a
 * stick is a position, not an edge, and holding its extreme for an extra frame
 * would lengthen every dash and smash. */
#ifndef YAMPP_PAD_LATCH_H
#define YAMPP_PAD_LATCH_H
#include "../aurora_shim/aurora_shim.h"
typedef struct PadLatch { AushimPadStatus latest; uint16_t pressed; uint8_t lt, rt; } PadLatch;
static void pad_latch_update(PadLatch* state, const AushimPadStatus* input) {
    state->pressed |= (uint16_t)(input->buttons & ~state->latest.buttons & 0x1fffu);
    if (input->trigger_left > state->lt) state->lt = input->trigger_left;
    if (input->trigger_right > state->rt) state->rt = input->trigger_right;
    state->latest = *input;
    if (input->error) { state->pressed = 0; state->lt = state->rt = 0; }
}
static AushimPadStatus pad_latch_consume(PadLatch* state) {
    AushimPadStatus input = state->latest;
    input.buttons |= state->pressed;
    if (state->lt > input.trigger_left) input.trigger_left = state->lt;
    if (state->rt > input.trigger_right) input.trigger_right = state->rt;
    /* Start the next frame's peak empty. Samples taken after this point
     * rebuild it, and a frame with no new sample falls back to `latest`, so a
     * trigger that is easing off is never held up by the frame before it. */
    state->pressed = 0;
    state->lt = state->rt = 0;
    return input;
}
#endif
