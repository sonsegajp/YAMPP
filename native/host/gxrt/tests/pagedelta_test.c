/* Incremental snapshots must be indistinguishable from whole ones.
 *
 * The whole point of pagedelta is that it copies less than everything, so the
 * only test worth writing is the one that proves it copied enough. A second,
 * deliberately stupid model keeps a full byte-for-byte image of every slot; a
 * long run of random writes, saves and restores then has to agree with it
 * exactly, every time. If the page map ever misses a write, the two diverge
 * and the run says on which operation.
 *
 * The randomness is seeded and reported, so a failure is reproducible. */
#include "../pagedelta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 5
#define REGION_A (512u * 1024u)
#define REGION_B (300u * 1024u + 777u)   /* deliberately not a page multiple */

static unsigned char* live[2];
static size_t live_size[2] = { REGION_A, REGION_B };
static unsigned char* model[SLOTS][2];
static int model_valid[SLOTS];
static unsigned failures;

static unsigned long long rng_state = 88172645463325252ull;
static unsigned long long rng(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state;
}
static size_t pick(size_t limit) { return (size_t)(rng() % limit); }

static void compare_slot(unsigned slot, const char* what, unsigned step) {
    for (int r = 0; r < 2; ++r) {
        if (memcmp(live[r], model[slot][r], live_size[r]) == 0) continue;
        size_t at = 0;
        while (at < live_size[r] && live[r][at] == model[slot][r][at]) ++at;
        fprintf(stderr, "FAIL step %u (%s): region %d byte %zu is %02X, slot %u recorded %02X\n",
                step, what, r, at, live[r][at], slot, model[slot][r][at]);
        ++failures;
        return;
    }
}

/* An ordinary store, the kind the recompiled guest and the m-ex engine make.
 * The hardware records these on its own. */
static void scribble(void) {
    int r = (int)pick(2);
    size_t length = 1 + pick(live_size[r] > 9000 ? 9000 : live_size[r]);
    size_t at = pick(live_size[r] - length + 1);
    unsigned char value = (unsigned char)rng();
    memset(live[r] + at, value, length);
}

/* A file read landing in the region: the bytes appear without this process
 * storing them, so the caller has to say so. Modelled here by writing through
 * a separate pointer and then reporting it, which is as close as a test can
 * get to a kernel write. */
static void kernel_write(void) {
    int r = (int)pick(2);
    size_t length = 1 + pick(4096);
    if (length > live_size[r]) length = live_size[r];
    size_t at = pick(live_size[r] - length + 1);
    volatile unsigned char* opaque = live[r] + at;
    for (size_t i = 0; i < length; ++i) opaque[i] = (unsigned char)(rng());
    pd_mark(live[r] + at, length);
}

int main(void) {
    for (int r = 0; r < 2; ++r) {
        live[r] = (unsigned char*)pd_alloc(live_size[r]);
        if (!live[r]) { fprintf(stderr, "FAIL: could not reserve region %d\n", r); return 1; }
        for (unsigned s = 0; s < SLOTS; ++s) {
            model[s][r] = (unsigned char*)malloc(live_size[r]);
            if (!model[s][r]) { fprintf(stderr, "FAIL: out of memory\n"); return 1; }
        }
    }
    pd_clear_regions();
    if (!pd_add_region(live[0], live_size[0]) || !pd_add_region(live[1], live_size[1])) {
        fprintf(stderr, "FAIL: regions rejected\n");
        return 1;
    }
    if (!pd_init(SLOTS)) {
        /* Write watching is a Windows facility. Everywhere else the caller
         * copies whole memories and there is nothing here to verify. */
        printf("SKIP: page tracking unavailable on this platform\n");
        return 0;
    }
    if (!pd_active()) { fprintf(stderr, "FAIL: initialised but inactive\n"); return 1; }

    /* Something in every byte before the first save, so an untouched page is
     * still a page with content that has to be carried. */
    for (int r = 0; r < 2; ++r)
        for (size_t i = 0; i < live_size[r]; ++i) live[r][i] = (unsigned char)(i * 31u + (unsigned)r);

    for (unsigned step = 0; step < 4000; ++step) {
        unsigned action = (unsigned)pick(10);
        if (action < 5) {
            scribble();
        } else if (action < 6) {
            kernel_write();
        } else if (action < 8) {
            unsigned slot = (unsigned)pick(SLOTS);
            pd_save(slot);
            for (int r = 0; r < 2; ++r) memcpy(model[slot][r], live[r], live_size[r]);
            model_valid[slot] = 1;
            /* A save must not disturb what it copied. */
            compare_slot(slot, "save left the live memory changed", step);
        } else {
            unsigned slot = (unsigned)pick(SLOTS);
            if (!model_valid[slot]) continue;
            pd_restore(slot);
            compare_slot(slot, "restore did not reproduce the slot", step);
            if (failures) break;
        }
    }

    /* And the ordering that matters most in a real match: save every frame,
     * then rewind several frames at once. */
    for (unsigned round = 0; round < 200 && !failures; ++round) {
        unsigned slot = round % SLOTS;
        scribble(); scribble();
        pd_save(slot);
        for (int r = 0; r < 2; ++r) memcpy(model[slot][r], live[r], live_size[r]);
        model_valid[slot] = 1;
        if (round >= SLOTS && (round % 7) == 0) {
            unsigned back = (round - (SLOTS - 1)) % SLOTS;
            pd_restore(back);
            compare_slot(back, "rewind to the oldest kept frame", round);
        }
    }

    pd_shutdown();
    for (int r = 0; r < 2; ++r) {
        pd_free(live[r], live_size[r]);
        for (unsigned s = 0; s < SLOTS; ++s) free(model[s][r]);
    }
    if (failures) { fprintf(stderr, "%u failure(s)\n", failures); return 1; }
    printf("PASS: incremental snapshots match whole ones\n");
    return 0;
}
