/* Deterministic rollback regression tests: no renderer, game assets, or sockets.
 * The simulation test compares corrected replay to an independent lockstep run
 * under late, reordered, and duplicate inputs across multiple history wraps. */
#include "rollback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static RbInput input(int stick, unsigned buttons) {
    RbInput value = {0}; value.sx = (int8_t)stick; value.buttons = (uint16_t)buttons; return value;
}
static void init(Rollback* rb) { CHECK(rb_init(rb, 3, sizeof(uint32_t))); }

static void prediction_and_dirty(void) {
    Rollback rb; RbInput pads[RB_PORTS], neutral = {0}, right = input(40, 1), left = input(-20, 2);
    init(&rb);
    CHECK(rb_receive(&rb, 0, 0, &neutral) == 1);
    CHECK(rb_receive(&rb, 1, 0, &right) == 1);
    CHECK(rb_inputs(&rb, 0, pads)); rb_advance(&rb, 1);
    CHECK(rb_inputs(&rb, 1, pads)); CHECK(!memcmp(&pads[1], &right, sizeof right)); rb_advance(&rb, 2);
    CHECK(rb_inputs(&rb, 2, pads)); rb_advance(&rb, 3);
    CHECK(rb_receive(&rb, 1, 2, &left) == 1); CHECK(rb.dirty == 2);
    CHECK(rb_receive(&rb, 1, 1, &left) == 1); CHECK(rb.dirty == 1);
    CHECK(rb_receive(&rb, 1, 1, &left) == 1); /* identical duplicate is harmless */
    CHECK(rb_receive(&rb, 1, 1, &right) == -1); /* immutable confirmed input */
    CHECK(rb_inputs(&rb, 1, pads)); CHECK(!memcmp(&pads[1], &left, sizeof left));
    rb_destroy(&rb);
}

static void future_inputs_do_not_predict_backwards(void) {
    Rollback rb; RbInput pads[RB_PORTS], future = input(77, 3), zero = {0}; init(&rb);
    CHECK(rb_receive(&rb, 1, 5, &future) == 1);
    CHECK(rb_inputs(&rb, 0, pads)); CHECK(!memcmp(&pads[1], &zero, sizeof zero));
    CHECK(rb_inputs(&rb, 4, pads)); CHECK(!memcmp(&pads[1], &zero, sizeof zero));
    CHECK(rb_inputs(&rb, 5, pads)); CHECK(!memcmp(&pads[1], &future, sizeof future));
    CHECK(rb_inputs(&rb, 6, pads)); CHECK(!memcmp(&pads[1], &future, sizeof future));
    rb_destroy(&rb);
}

static void bounded_window_and_confirmation(void) {
    Rollback rb; RbInput pads[RB_PORTS], neutral = {0}; init(&rb);
    for (uint32_t frame = 0; frame < RB_WINDOW; ++frame) {
        CHECK(rb_receive(&rb, 0, frame, &neutral) == 1);
        CHECK(rb_inputs(&rb, frame, pads)); rb_advance(&rb, frame + 1);
    }
    CHECK(!rb_inputs(&rb, RB_WINDOW, pads));
    CHECK(rb_receive(&rb, 1, 1, &neutral) == 1); CHECK(rb.confirmed == 0);
    CHECK(rb_receive(&rb, 1, 0, &neutral) == 1); CHECK(rb.confirmed == 2);
    CHECK(rb_inputs(&rb, RB_WINDOW, pads)); CHECK(rb.dirty == RB_NONE);
    CHECK(rb_receive(&rb, 4, 2, &neutral) == 0);
    CHECK(rb_receive(&rb, 2, 2, &neutral) == 0);
    CHECK(rb_receive(&rb, 0, rb.next_frame + RB_HISTORY / 2, &neutral) == 0);
    CHECK(rb_receive(&rb, 0, RB_NONE, &neutral) == 0);
    rb_destroy(&rb);
}

static void snapshots_and_epoch_reset(void) {
    Rollback rb; RbInput pads[RB_PORTS], right = input(1, 1), zero = {0}; init(&rb);
    for (uint32_t frame = 0; frame <= RB_WINDOW; ++frame) {
        uint32_t* state = rb_save_buffer(&rb, frame); CHECK(state); *state = frame * 123 + 7;
    }
    for (uint32_t frame = 0; frame <= RB_WINDOW; ++frame) {
        const uint32_t* state = rb_load_buffer(&rb, frame); CHECK(state); CHECK(*state == frame * 123 + 7);
    }
    CHECK(rb_save_buffer(&rb, RB_WINDOW + 1)); CHECK(!rb_load_buffer(&rb, 0));
    CHECK(rb_receive(&rb, 1, 0, &right)); CHECK(rb_inputs(&rb, 0, pads)); rb_advance(&rb, 1);
    rb_reset(&rb, 3);
    CHECK(rb.next_frame == 0 && rb.confirmed == 0 && rb.dirty == RB_NONE);
    CHECK(!rb_load_buffer(&rb, RB_WINDOW)); CHECK(rb_inputs(&rb, 0, pads));
    CHECK(!memcmp(&pads[1], &zero, sizeof zero)); rb_destroy(&rb);
    CHECK(!rb_load_buffer(&rb, 0));
    CHECK(!rb_init(&rb, 3, 0));
    CHECK(!rb_init(&rb, 3, SIZE_MAX));
}

typedef struct FighterState {
    int32_t x[2], velocity[2];
    uint32_t damage[2], random, frame;
} FighterState;

static void simulate(FighterState* state, const RbInput pads[RB_PORTS]) {
    for (unsigned p = 0; p < 2; ++p) {
        state->velocity[p] = (state->velocity[p] * 3 + pads[p].sx) / 4;
        state->x[p] += state->velocity[p];
        if (pads[p].buttons & 1) {
            state->random = state->random * 1664525u + 1013904223u;
            state->damage[1 - p] += 1 + (state->random >> 28);
        }
    }
    ++state->frame;
}
static RbInput scripted(unsigned port, uint32_t frame) {
    return input((int)((frame / (port + 3)) % 5) * 24 - 48,
                 (frame % (port + 5)) == 0 ? 1u : 0u);
}
static void run_frame(Rollback* rb, FighterState* state, uint32_t frame) {
    RbInput pads[RB_PORTS]; void* snapshot = rb_save_buffer(rb, frame); CHECK(snapshot);
    memcpy(snapshot, state, sizeof *state); CHECK(rb_inputs(rb, frame, pads)); simulate(state, pads);
}

static void delayed_reordered_replay_matches_lockstep(void) {
    enum { FRAMES = RB_HISTORY * 3 + 37 };
    static const unsigned delays[] = {8, 3, 1, 6, 2, 5, 0, 4};
    Rollback rb; FighterState actual = {{0}, {0}, {0}, 0x12345678u, 0}, expected = actual;
    uint32_t rollbacks = 0, replayed = 0, stalls = 0;
    CHECK(rb_init(&rb, 3, sizeof actual));
    for (uint32_t frame = 0; frame < FRAMES; ++frame) {
        RbInput pads[RB_PORTS] = {{0}};
        pads[0] = scripted(0, frame); pads[1] = scripted(1, frame); simulate(&expected, pads);
    }
    for (uint32_t tick = 0; tick < FRAMES + 100u; ++tick) {
        if (tick < FRAMES) { RbInput local = scripted(0, tick); CHECK(rb_receive(&rb, 0, tick, &local) == 1); }
        uint32_t earliest = tick > RB_WINDOW ? tick - RB_WINDOW : 0;
        for (uint32_t frame = earliest; frame <= tick && frame < FRAMES; ++frame) {
            if (frame + delays[frame % 8] != tick) continue;
            RbInput remote = scripted(1, frame);
            CHECK(rb_receive(&rb, 1, frame, &remote) == 1);
            CHECK(rb_receive(&rb, 1, frame, &remote) == 1);
        }
        if (rb.dirty != RB_NONE) {
            uint32_t first = rb.dirty, end = rb.next_frame;
            const void* snapshot = rb_load_buffer(&rb, first); CHECK(snapshot);
            memcpy(&actual, snapshot, sizeof actual); rb.dirty = RB_NONE;
            for (uint32_t frame = first; frame < end; ++frame) run_frame(&rb, &actual, frame);
            ++rollbacks; replayed += end - first;
        }
        if (rb.next_frame < FRAMES) {
            RbInput pads[RB_PORTS];
            if (rb_inputs(&rb, rb.next_frame, pads)) {
                run_frame(&rb, &actual, rb.next_frame); rb_advance(&rb, rb.next_frame + 1);
            } else ++stalls;
        }
        if (rb.next_frame == FRAMES && rb.confirmed == FRAMES && rb.dirty == RB_NONE) break;
    }
    CHECK(rb.next_frame == FRAMES && rb.confirmed == FRAMES);
    CHECK(rollbacks > 100 && replayed > rollbacks);
    CHECK(!memcmp(&actual, &expected, sizeof actual));
    CHECK(!rb_load_buffer(&rb, 0));
    RbInput stale = scripted(1, 0); CHECK(rb_receive(&rb, 1, 0, &stale) == 0);
    printf("rollback replay: %u frames, %u corrections, %u replayed frames, %u stalls; exact lockstep state\n",
           (unsigned)FRAMES, (unsigned)rollbacks, (unsigned)replayed, (unsigned)stalls);
    rb_destroy(&rb);
}

int main(void) {
    prediction_and_dirty();
    future_inputs_do_not_predict_backwards();
    bounded_window_and_confirmation();
    snapshots_and_epoch_reset();
    delayed_reordered_replay_matches_lockstep();
    puts("rollback tests passed");
    return 0;
}
