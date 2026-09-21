/* Peer clock agreement for rollback netplay.
 *
 * Two machines running the same 60 Hz simulation never tick at the same rate:
 * one display runs at 60.000 Hz and the other at 59.940, a background task
 * steals a vsync, a laptop drops to a power-saving clock. Without a correction
 * the faster peer walks ahead until every one of its frames waits on the
 * network, which is felt as constant micro-stutter and eaten inputs long
 * before any timeout fires.
 *
 * The correction is GGPO's, which Slippi also uses: each peer continuously
 * measures how far *behind* the other it believes itself to be (its "frame
 * advantage" -- behind is an advantage, because the other side is the one
 * mispredicting), reports that number to the peer, and the two compare. When
 * both agree on who is ahead, the peer that is ahead gives back half the
 * difference by skipping that many frames. Splitting the difference means the
 * two converge instead of overshooting past each other.
 *
 * No clock synchronisation is required: only frame numbers and a round-trip
 * time, both of which each side measures for itself. */
#ifndef YAMPP_TIMESYNC_H
#define YAMPP_TIMESYNC_H
#include <stdint.h>

#define TS_RTT_SAMPLES 8    /* round-trip samples kept; median rejects spikes */
#define TS_HISTORY 30       /* frames of advantage averaged, ~half a second */
#define TS_MIN_SKIP 2       /* drift below this is noise, not a correction */
#define TS_MAX_SKIP 5
#define TS_FRAME_US 16683   /* one 60 Hz frame; the GameCube's actual 59.94 */

typedef struct TimeSync {
  uint16_t rtt[TS_RTT_SAMPLES];
  uint8_t rtt_count, rtt_next;
  int8_t local[TS_HISTORY], remote[TS_HISTORY];
  uint8_t count, next;
} TimeSync;

static void timesync_reset(TimeSync* ts) {
  uint8_t i;
  for (i = 0; i < TS_RTT_SAMPLES; ++i) ts->rtt[i] = 0;
  for (i = 0; i < TS_HISTORY; ++i) ts->local[i] = ts->remote[i] = 0;
  ts->rtt_count = ts->rtt_next = ts->count = ts->next = 0;
}

/* Frame numbers restart at zero on every scene change, so the advantage
 * history built against the previous scene is meaningless and would read as a
 * huge drift. The measured round trip is a property of the link, not of the
 * scene, and is deliberately kept. */
static void timesync_new_epoch(TimeSync* ts) {
  uint8_t i;
  for (i = 0; i < TS_HISTORY; ++i) ts->local[i] = ts->remote[i] = 0;
  ts->count = ts->next = 0;
}

/* One completed ping. Samples are kept unsorted; the reader takes the median
 * so a single retransmitted packet cannot move the estimate. */
static void timesync_rtt_sample(TimeSync* ts, unsigned ms) {
  if (ms > 0xFFFFu) ms = 0xFFFFu;
  ts->rtt[ts->rtt_next] = (uint16_t)ms;
  ts->rtt_next = (uint8_t)((ts->rtt_next + 1u) % TS_RTT_SAMPLES);
  if (ts->rtt_count < TS_RTT_SAMPLES) ++ts->rtt_count;
}

/* Median round-trip in milliseconds, or 0 before the first sample. */
static unsigned timesync_rtt(const TimeSync* ts) {
  uint16_t sorted[TS_RTT_SAMPLES];
  uint8_t n = ts->rtt_count, i, j;
  if (!n) return 0;
  for (i = 0; i < n; ++i) sorted[i] = ts->rtt[i];
  for (i = 1; i < n; ++i) {
    uint16_t key = sorted[i];
    for (j = i; j && sorted[j - 1] > key; --j) sorted[j] = sorted[j - 1];
    sorted[j] = key;
  }
  return sorted[n / 2];
}

/* How far behind the peer we estimate we are, given the newest frame number
 * they have told us about. Positive means they are ahead of us. */
static int timesync_advantage(const TimeSync* ts, uint32_t local_frame, uint32_t peer_frame) {
  unsigned one_way_us = timesync_rtt(ts) * 1000u / 2u;
  int latency_frames = (int)((one_way_us + TS_FRAME_US / 2u) / TS_FRAME_US);
  long advantage = (long)peer_frame + latency_frames - (long)local_frame;
  if (advantage > 127) advantage = 127;
  if (advantage < -128) advantage = -128;
  return (int)advantage;
}

/* Record this frame's pair of measurements: ours, and the one the peer sent. */
static void timesync_frame(TimeSync* ts, int local_advantage, int remote_advantage) {
  ts->local[ts->next] = (int8_t)(local_advantage > 127 ? 127 : local_advantage < -128 ? -128 : local_advantage);
  ts->remote[ts->next] = (int8_t)(remote_advantage > 127 ? 127 : remote_advantage < -128 ? -128 : remote_advantage);
  ts->next = (uint8_t)((ts->next + 1u) % TS_HISTORY);
  if (ts->count < TS_HISTORY) ++ts->count;
}

/* Frames this peer should skip to let the other catch up, at most `max`.
 *
 * Returns 0 unless both sides' own measurements agree that we are the one
 * ahead. Requiring agreement is what stops the two peers from both deciding
 * to wait for each other and stalling the match outright. */
static int timesync_skip_frames(const TimeSync* ts, int max) {
  long local_sum = 0, remote_sum = 0;
  int skip;
  uint8_t i;
  if (ts->count < TS_HISTORY) return 0;   /* not enough evidence yet */
  for (i = 0; i < TS_HISTORY; ++i) { local_sum += ts->local[i]; remote_sum += ts->remote[i]; }
  /* Our advantage counts us as behind; if it is not the smaller of the two we
   * are not the peer that is ahead, so there is nothing for us to give back. */
  if (local_sum >= remote_sum) return 0;
  /* Round the halved difference to nearest, in the averaged (x TS_HISTORY) scale. */
  skip = (int)((remote_sum - local_sum + TS_HISTORY) / (2 * TS_HISTORY));
  if (skip < TS_MIN_SKIP) return 0;
  if (max > TS_MAX_SKIP) max = TS_MAX_SKIP;
  return skip > max ? max : skip;
}
#endif
