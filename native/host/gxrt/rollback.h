/* Bounded rollback bookkeeping. Callers serialize access and provide complete
 * simulation snapshots; input receipt never mutates a snapshot or a prediction. */
#ifndef MELEE_ROLLBACK_H
#define MELEE_ROLLBACK_H
#include <stdint.h>
#include <stddef.h>
#define RB_PORTS 4
#define RB_WINDOW 8
#define RB_HISTORY 1024
#define RB_NONE UINT32_MAX
typedef struct RbInput { uint16_t buttons; int8_t sx, sy, cx, cy; uint8_t lt, rt; } RbInput;
typedef struct RbFrame {
    uint32_t frame;
    uint8_t valid, actual_mask, used_mask;
    RbInput actual[RB_PORTS], used[RB_PORTS];
} RbFrame;
typedef struct Rollback {
    RbFrame history[RB_HISTORY];
    uint32_t next_frame, confirmed, dirty, rollback_count, replayed_frames;
    uint8_t ports;
    size_t state_size;
    unsigned char* states;
    uint32_t state_frame[RB_WINDOW + 1];
} Rollback;
int rb_init(Rollback* rb, uint8_t ports, size_t state_size);
void rb_destroy(Rollback* rb);
void rb_reset(Rollback* rb, uint8_t ports);
/* -1: conflicting immutable input, 0: outside bounded receive window, 1: stored. */
int rb_receive(Rollback* rb, unsigned port, uint32_t frame, const RbInput* input);
/* 0 means wait for a confirmation; never predict beyond RB_WINDOW. */
int rb_inputs(Rollback* rb, uint32_t frame, RbInput output[RB_PORTS]);
void rb_advance(Rollback* rb, uint32_t next_frame);
void* rb_save_buffer(Rollback* rb, uint32_t frame);
const void* rb_load_buffer(const Rollback* rb, uint32_t frame);
#endif
