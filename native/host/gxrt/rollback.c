#include "rollback.h"
#include <stdlib.h>
#include <string.h>

static RbFrame* frame_at(Rollback* rb, uint32_t frame) {
    RbFrame* slot = &rb->history[frame % RB_HISTORY];
    if (!slot->valid || slot->frame != frame) {
        memset(slot, 0, sizeof *slot); slot->valid = 1; slot->frame = frame;
    }
    return slot;
}
static const RbFrame* find_frame(const Rollback* rb, uint32_t frame) {
    const RbFrame* slot = &rb->history[frame % RB_HISTORY];
    return slot->valid && slot->frame == frame ? slot : NULL;
}
void rb_reset(Rollback* rb, uint8_t ports) {
    memset(rb->history, 0, sizeof rb->history);
    rb->ports = ports; rb->next_frame = rb->confirmed = 0; rb->dirty = RB_NONE;
    rb->rollback_count = rb->replayed_frames = 0;
    for (unsigned i = 0; i <= RB_WINDOW; ++i) rb->state_frame[i] = RB_NONE;
}
int rb_init(Rollback* rb, uint8_t ports, size_t state_size) {
    memset(rb, 0, sizeof *rb);
    if (!state_size || state_size > SIZE_MAX / (RB_WINDOW + 1)) return 0;
    rb->states = malloc(state_size * (RB_WINDOW + 1));
    if (!rb->states) return 0;
    rb->state_size = state_size; rb_reset(rb, ports); return 1;
}
void rb_destroy(Rollback* rb) { free(rb->states); memset(rb, 0, sizeof *rb); }
int rb_receive(Rollback* rb, unsigned port, uint32_t frame, const RbInput* input) {
    if (port >= RB_PORTS || !(rb->ports & (1u << port)) || frame == RB_NONE) return 0;
    if (frame < rb->next_frame && rb->next_frame - frame >= RB_HISTORY / 2) return 0;
    if (frame >= rb->next_frame && frame - rb->next_frame >= RB_HISTORY / 2) return 0;
    RbFrame* slot = frame_at(rb, frame); uint8_t bit = (uint8_t)(1u << port);
    if (slot->actual_mask & bit) return memcmp(&slot->actual[port], input, sizeof *input) ? -1 : 1;
    slot->actual[port] = *input; slot->actual_mask |= bit;
    if ((slot->used_mask & bit) && memcmp(&slot->used[port], input, sizeof *input) && frame < rb->dirty)
        rb->dirty = frame;
    while (rb->confirmed < RB_NONE) {
        const RbFrame* current = find_frame(rb, rb->confirmed);
        if (!current || (current->actual_mask & rb->ports) != rb->ports) break;
        ++rb->confirmed;
    }
    return 1;
}
int rb_inputs(Rollback* rb, uint32_t frame, RbInput output[RB_PORTS]) {
    if (frame >= rb->confirmed && frame - rb->confirmed >= RB_WINDOW) return 0;
    RbFrame* slot = frame_at(rb, frame);
    memset(output, 0, sizeof(RbInput) * RB_PORTS);
    for (unsigned port = 0; port < RB_PORTS; ++port) {
        uint8_t bit = (uint8_t)(1u << port);
        if (!(rb->ports & bit)) continue;
        if (slot->actual_mask & bit) output[port] = slot->actual[port];
        else {
            /* Predict from the most recent earlier actual/used input. An input
             * from the future must never leak backwards through reordering. */
            for (uint32_t back = 1; back <= frame && back < RB_HISTORY / 2; ++back) {
                const RbFrame* previous = find_frame(rb, frame - back);
                if (!previous) continue;
                if (previous->actual_mask & bit) { output[port] = previous->actual[port]; break; }
                if (previous->used_mask & bit) { output[port] = previous->used[port]; break; }
            }
        }
        slot->used[port] = output[port]; slot->used_mask |= bit;
    }
    return 1;
}
void rb_advance(Rollback* rb, uint32_t next_frame) { rb->next_frame = next_frame; }
void* rb_save_buffer(Rollback* rb, uint32_t frame) {
    if (!rb->states) return NULL;
    unsigned slot = frame % (RB_WINDOW + 1); rb->state_frame[slot] = frame;
    return rb->states + slot * rb->state_size;
}
const void* rb_load_buffer(const Rollback* rb, uint32_t frame) {
    unsigned slot = frame % (RB_WINDOW + 1);
    if (!rb->states || rb->state_frame[slot] != frame) return NULL;
    return rb->states + slot * rb->state_size;
}
