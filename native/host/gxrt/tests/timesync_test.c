/* Peer clock agreement tests: no sockets, no simulation, no wall clock.
 *
 * The drift test is the one that matters. It runs two peers whose hosts tick
 * at genuinely different rates through the same exchange the match uses, and
 * checks that the pair stays locked together, that only the peer which is
 * actually ahead ever gives frames back, and that a steady link is left alone. */
#include "../timesync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static void rtt_median_rejects_spikes(void) {
    TimeSync ts; unsigned i;
    timesync_reset(&ts);
    CHECK(timesync_rtt(&ts) == 0);
    for (i = 0; i < 7; ++i) timesync_rtt_sample(&ts, 30);
    timesync_rtt_sample(&ts, 900);           /* one retransmitted packet */
    CHECK(timesync_rtt(&ts) == 30);
    for (i = 0; i < TS_RTT_SAMPLES; ++i) timesync_rtt_sample(&ts, 120);
    CHECK(timesync_rtt(&ts) == 120);          /* the ring forgets the old link */
}

static void advantage_accounts_for_flight_time(void) {
    TimeSync ts; timesync_reset(&ts);
    /* With no measured latency the peer's reported frame is taken at face value. */
    CHECK(timesync_advantage(&ts, 100, 100) == 0);
    CHECK(timesync_advantage(&ts, 100, 97) == -3);
    /* 100 ms round trip is three frames of one-way flight: a peer that said
     * "frame 97" 50 ms ago has since reached about frame 100, so we are level. */
    { unsigned i; for (i = 0; i < TS_RTT_SAMPLES; ++i) timesync_rtt_sample(&ts, 100); }
    CHECK(timesync_advantage(&ts, 100, 97) == 0);
}

static void steady_link_is_never_corrected(void) {
    TimeSync ts; unsigned i;
    timesync_reset(&ts);
    for (i = 0; i < TS_HISTORY * 4; ++i) {
        timesync_frame(&ts, 0, 0);
        CHECK(timesync_skip_frames(&ts, TS_MAX_SKIP) == 0);
    }
    /* A one-frame jitter either way is noise and must not produce a hitch. */
    for (i = 0; i < TS_HISTORY; ++i) timesync_frame(&ts, (int)(i & 1) - 1, 0);
    CHECK(timesync_skip_frames(&ts, TS_MAX_SKIP) == 0);
}

static void only_the_peer_that_is_ahead_gives_frames_back(void) {
    TimeSync ahead, behind; unsigned i;
    timesync_reset(&ahead); timesync_reset(&behind);
    /* We are four frames ahead: our own advantage is -4, theirs is +4. */
    for (i = 0; i < TS_HISTORY; ++i) { timesync_frame(&ahead, -4, 4); timesync_frame(&behind, 4, -4); }
    CHECK(timesync_skip_frames(&ahead, TS_MAX_SKIP) == 4);
    CHECK(timesync_skip_frames(&behind, TS_MAX_SKIP) == 0);
    /* Half the difference, so the correction converges instead of overshooting. */
    CHECK(timesync_skip_frames(&ahead, 1) == 1);
}

static void partial_history_waits_for_evidence(void) {
    TimeSync ts; unsigned i;
    timesync_reset(&ts);
    for (i = 0; i + 1 < TS_HISTORY; ++i) {
        timesync_frame(&ts, -9, 9);
        CHECK(timesync_skip_frames(&ts, TS_MAX_SKIP) == 0);
    }
    timesync_frame(&ts, -9, 9);
    CHECK(timesync_skip_frames(&ts, TS_MAX_SKIP) == TS_MAX_SKIP);
}

/* Two peers, two host clocks, one link. Peer A's host produces 60.30 frames a
 * second and peer B's 59.80 -- a half-hertz split, well inside what two real
 * displays differ by. Inputs cross the link with a fixed one-way delay. */
static void divergent_host_clocks_stay_locked(void) {
    static const double rate[2] = { 60.30, 59.80 };
    const double step = 1.0 / 1000.0;         /* one millisecond of wall time */
    const unsigned latency_ms = 25;
    TimeSync ts[2];
    double credit[2] = { 0, 0 };
    uint32_t frame[2] = { 0, 0 };
    uint32_t seen[2] = { 0, 0 };              /* newest frame heard from the peer */
    int reported[2] = { 0, 0 };               /* advantage the peer last sent us */
    unsigned skip[2] = { 0, 0 };
    unsigned both_skipped = 0, worst_gap = 0, corrections[2] = { 0, 0 };
    unsigned ms, peer;
    /* A 1024-entry pipeline of (arrival time -> frame number) per direction. */
    static uint32_t wire_frame[2][4096]; static int wire_advantage[2][4096];
    memset(wire_frame, 0, sizeof wire_frame); memset(wire_advantage, 0, sizeof wire_advantage);

    for (peer = 0; peer < 2; ++peer) {
        unsigned i;
        timesync_reset(&ts[peer]);
        for (i = 0; i < TS_RTT_SAMPLES; ++i) timesync_rtt_sample(&ts[peer], latency_ms * 2);
    }
    for (ms = 0; ms < 60000; ++ms) {          /* one minute of play */
        for (peer = 0; peer < 2; ++peer) {
            unsigned other = peer ^ 1u;
            /* Deliver anything that arrives this millisecond. */
            if (wire_frame[peer][ms % 4096]) {
                seen[peer] = wire_frame[peer][ms % 4096] - 1u;
                reported[peer] = wire_advantage[peer][ms % 4096];
                wire_frame[peer][ms % 4096] = 0;
            }
            credit[peer] += rate[peer] * step;
            if (credit[peer] < 1.0) continue;
            credit[peer] -= 1.0;
            if (skip[peer]) { --skip[peer]; continue; }   /* a given-back frame */
            {
                int advantage = timesync_advantage(&ts[peer], frame[peer], seen[peer]);
                unsigned arrival = (ms + latency_ms) % 4096;
                timesync_frame(&ts[peer], advantage, reported[peer]);
                wire_frame[other][arrival] = frame[peer] + 1u;
                wire_advantage[other][arrival] = advantage;
            }
            ++frame[peer];
            if (frame[peer] % TS_HISTORY == 0) {
                unsigned give = (unsigned)timesync_skip_frames(&ts[peer], 1);
                if (give) { skip[peer] = give; ++corrections[peer]; }
            }
        }
        if (skip[0] && skip[1]) ++both_skipped;
        {
            unsigned gap = frame[0] > frame[1] ? frame[0] - frame[1] : frame[1] - frame[0];
            if (gap > worst_gap) worst_gap = gap;
        }
    }
    /* Without a correction a 0.5 Hz split is 30 frames of drift a minute, far
     * past the eight-frame rollback window; the pair must stay well inside it. */
    CHECK(worst_gap < 8);
    /* The faster host is the only one that should ever be giving frames back. */
    CHECK(corrections[0] > 0);
    CHECK(corrections[1] == 0);
    CHECK(both_skipped == 0);
    printf("clock drift: 60.30 Hz vs 59.80 Hz held to %u frames over 60 s"
           " (%u corrections by the fast peer, %u by the slow one)\n",
           worst_gap, corrections[0], corrections[1]);
}

static void a_new_scene_forgets_the_drift_but_keeps_the_link(void) {
    TimeSync ts; unsigned i;
    timesync_reset(&ts);
    for (i = 0; i < TS_RTT_SAMPLES; ++i) timesync_rtt_sample(&ts, 64);
    for (i = 0; i < TS_HISTORY; ++i) timesync_frame(&ts, -9, 9);
    CHECK(timesync_skip_frames(&ts, TS_MAX_SKIP) == TS_MAX_SKIP);
    timesync_new_epoch(&ts);
    /* Frame numbers restarted, so the old gap must not be corrected for... */
    CHECK(timesync_skip_frames(&ts, TS_MAX_SKIP) == 0);
    /* ...but the link has not changed and must not be re-measured from zero. */
    CHECK(timesync_rtt(&ts) == 64);
}

int main(void) {
    rtt_median_rejects_spikes();
    a_new_scene_forgets_the_drift_but_keeps_the_link();
    advantage_accounts_for_flight_time();
    steady_link_is_never_corrected();
    only_the_peer_that_is_ahead_gives_frames_back();
    partial_history_waits_for_evidence();
    divergent_host_clocks_stay_locked();
    puts("timesync tests passed");
    return 0;
}
