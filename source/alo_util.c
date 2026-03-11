#include "alo_util.h"
#include <math.h>

/* convert Bars control value to integer within [1..16] */
uint32_t alo_get_bars_i(const Alo* self)
{
  if (!self || !self->ports.bars) {
    return DEFAULT_NUM_BARS;
  }
  float bars_f = *(self->ports.bars);
  if (bars_f < ALO_BARS_MIN_F) {
    bars_f = ALO_BARS_MIN_F;
  }
  if (bars_f > ALO_BARS_MAX_F) {
    bars_f = ALO_BARS_MAX_F;
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
  if (v < (int)ALO_SLICES_PER_BAR_MIN_U) {
    v = (int)ALO_SLICES_PER_BAR_MIN_U;
  } else if (v > (int)ALO_SLICES_PER_BAR_MAX_U) {
    v = (int)ALO_SLICES_PER_BAR_MAX_U;
  }
  return (uint32_t)v;
}

bool alo_get_use_transient_slices_b(const Alo* self)
{
  ALO_GUARD_BOOL(self);
  ALO_GUARD_BOOL(self->ports.split_by_transient);
  return (*(self->ports.split_by_transient)) > 0.5f;
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
  if (!self || !(self->rate > ALO_RATE_EPS)) {
    return ALO_EDGE_FADE_DEFAULT_SAMPLES;
  }
  uint64_t fs = (uint64_t)llround((double)self->rate * 0.001); /* 1ms */
  if (fs < ALO_EDGE_FADE_SAMPLES_MIN) {
    fs = ALO_EDGE_FADE_SAMPLES_MIN;
  } else if (fs > ALO_EDGE_FADE_SAMPLES_MAX) {
    fs = ALO_EDGE_FADE_SAMPLES_MAX;
  }
  return (uint32_t)fs;
}

float alo_sensitivity_to_threshold(const Alo* self)
{
  float s = 0.0f;
  if (self && self->ports.slice_sens) {
    s = *(self->ports.slice_sens);
    if (s < 0.0f) {
      s = 0.0f;
    } else if (s > 10.0f) {
      s = 10.0f;
    }
  }

  /* The UI now exposes a wider 0..10 sensitivity range.
   * Map it back to the dense trigger behavior that previously felt good:
   *
   *   0.0  -> strictest useful trigger threshold
   *   10.0 -> most permissive trigger threshold
   *
   * Lower returned values produce more transient triggers.
   */
  return 20.0f + (s * -1.9f);
}

float alo_get_transient_threshold_ratio(const Alo* self)
{
  float ratio = alo_sensitivity_to_threshold(self);

  if (self && self->ports.transient_threshold) {
    float thr = *(self->ports.transient_threshold);
    if (thr < 1.0f) {
      thr = 1.0f;
    } else if (thr > 20.0f) {
      thr = 20.0f;
    }

    /* Preserve the legacy behavior where the threshold control directly
     * acts as detector strictness when hosts expose it. */
    ratio = thr;
  }

  if (ratio < 1.0f) {
    ratio = 1.0f;
  } else if (ratio > 20.0f) {
    ratio = 20.0f;
  }

  return ratio;
}

uint32_t alo_get_slice_fade_samples(const Alo* self, uint32_t slice_len)
{
  (void)self;

  if (slice_len == 0u) {
    return 1u;
  }

  uint32_t       fs       = alo_edge_fade_samples_u32(self);
  const uint32_t max_fade = (slice_len > 1u) ? (slice_len / 8u) : 1u;

  if (fs > max_fade) {
    fs = max_fade;
  }
  if (fs < 1u) {
    fs = 1u;
  }
  if (fs > slice_len) {
    fs = slice_len;
  }

  return fs;
}

uint32_t alo_get_slice_release_samples(const Alo* self, uint32_t slice_len)
{
  if (slice_len == 0u) {
    return 1u;
  }

  if (self && self->ports.slice_env_frac) {
    float pct = *(self->ports.slice_env_frac);
    if (pct < 0.0f) {
      pct = 0.0f;
    } else if (pct > 100.0f) {
      pct = 100.0f;
    }

    if (pct >= 100.0f) {
      return slice_len;
    }

    uint32_t rs = (uint32_t)((float)slice_len * (pct * 0.01f));
    if (rs < 1u) {
      rs = 1u;
    }
    if (rs > slice_len) {
      rs = slice_len;
    }
    return rs;
  }

  return alo_edge_fade_samples_u32(self);
}

uint32_t alo_get_slice_env_attack_samples(const Alo* self)
{
  /* provide an attack window for slice voices.  The port value is interpreted
   * in milliseconds; hosts are free to expose it or leave it hidden.  Default
   * is a short 5 ms window converted to samples using the current rate. */
  const float default_ms = ALO_SLICE_ENV_ATTACK_DEFAULT_MS;
  float       ms         = default_ms;
  if (self && self->ports.slice_env_attack) {
    ms = *(self->ports.slice_env_attack);
    if (ms < 0.0f) {
      ms = 0.0f;
    }
    /* clamp to a sensible upper bound (e.g. 100ms) just to avoid overflow */
    if (ms > 100.0f) {
      ms = 100.0f;
    }
  }
  if (!self || !(self->rate > 1e-6)) {
    return (uint32_t)lrintf((double)ms * 0.001);
  }
  uint64_t fs = (uint64_t)llround((double)self->rate * ((double)ms * 0.001));
  if (fs < 1u) {
    fs = 1u;
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

  /* Use a raised-cosine envelope for both fade-in and fade-out.  The
     derivative at the endpoints is zero, which avoids tiny discontinuities
     that a linear ramp can leave. */
  const float pi  = 3.14159265358979323846f;
  const float inv = 1.0f / (float)(fade_samples - 1u);

  /* Start fade-in */
  for (uint32_t i = 0; i < fade_samples; ++i) {
    float          t   = (float)i * inv;
    float          g   = 0.5f * (1.0f - cosf(pi * t));
    const uint32_t idx = s0 + i;
    buf[idx] *= g;
    buf[idx + LOOP_SIZE] *= g;
  }

  /* End fade-out */
  for (uint32_t i = 0; i < fade_samples; ++i) {
    float          t   = (float)(fade_samples - 1u - i) * inv;
    float          g   = 0.5f * (1.0f - cosf(pi * t));
    const uint32_t idx = s1 - fade_samples + i;
    buf[idx] *= g;
    buf[idx + LOOP_SIZE] *= g;
  }
}
