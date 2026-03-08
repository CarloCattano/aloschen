#include "alo_util.h"
#include <math.h>

/* convert Bars control value to integer within [1..16] */
uint32_t alo_get_bars_i(const Alo* self)
{
  if (!self || !self->ports.bars) {
    return DEFAULT_NUM_BARS;
  }
  float bars_f = *(self->ports.bars);
  if (bars_f < 1.0f) {
    bars_f = 1.0f;
  }
  if (bars_f > 16.0f) {
    bars_f = 16.0f;
  }
  uint32_t bars_i = (uint32_t)lrintf(bars_f);
  if (!bars_i) {
    bars_i = 1u;
  }
  return bars_i;
}

/* read slices-per-bar control and clamp to [2..8] */
uint32_t alo_get_slices_per_bar_u(const Alo* self)
{
  if (!self || !self->ports.slices_per_bar) {
    return 4u;
  }
  int v = (int)floorf(*(self->ports.slices_per_bar));
  if (v < 2) {
    v = 2;
  } else if (v > 8) {
    v = 8;
  }
  return (uint32_t)v;
}

/* check whether track index is in valid range */
bool track_is_active(const Alo* self, int t)
{
  (void)self;
  return t >= 0 && t < NUM_TRACKS;
}

/* return true if track is busy (recording/armed) */
bool track_is_busy(const Alo* self, int t)
{
  if (!self) {
    return false;
  }
  return track_is_active(self, t) && self->track_state[t] != TRACK_IDLE;
}

/* reset state of a given track to idle */
void clear_track_state(Alo* self, int t)
{
  if (!self || !track_is_active(self, t)) {
    return;
  }
  self->track_state[t]           = TRACK_IDLE;
  self->rec_remaining_samples[t] = 0;
}

/* determine fade length (in samples) for loop edges (~1ms) */
uint32_t alo_edge_fade_samples_u32(const Alo* self)
{
  if (!self || !(self->rate > 1e-6)) {
    return 64u;
  }
  uint64_t fs = (uint64_t)llround((double)self->rate * 0.001); /* 1ms */
  if (fs < 16u) {
    fs = 16u;
  } else if (fs > 512u) {
    fs = 512u;
  }
  return (uint32_t)fs;
}

/* apply linear cross-fade to edges of a stereo loop buffer */
void alo_apply_edge_fade_stereo(float* buf, uint32_t loop_start, uint32_t loop_samples,
                                uint32_t fade_samples)
{
  if (!buf || loop_samples == 0u || fade_samples == 0u) {
    return;
  }

  const uint32_t half = loop_samples / 2u;
  if (fade_samples > half) {
    fade_samples = half;
  }
  if (fade_samples == 0u) {
    return;
  }

  const uint32_t s0 = loop_start;
  const uint32_t s1 = loop_start + loop_samples;

  if (fade_samples == 1u) {
    buf[s0]             = 0.0f;
    buf[s0 + LOOP_SIZE] = 0.0f;
    if (loop_samples >= 2u) {
      buf[s1 - 1u]             = 0.0f;
      buf[s1 - 1u + LOOP_SIZE] = 0.0f;
    }
    return;
  }

  const float inv = 1.0f / (float)(fade_samples - 1u);

  /* Start fade-in */
  for (uint32_t i = 0; i < fade_samples; ++i) {
    const float    g   = (float)i * inv;
    const uint32_t idx = s0 + i;
    buf[idx] *= g;
    buf[idx + LOOP_SIZE] *= g;
  }

  /* End fade-out */
  for (uint32_t i = 0; i < fade_samples; ++i) {
    const float    g   = (float)(fade_samples - 1u - i) * inv;
    const uint32_t idx = s1 - fade_samples + i;
    buf[idx] *= g;
    buf[idx + LOOP_SIZE] *= g;
  }
}
