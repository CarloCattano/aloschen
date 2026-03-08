#ifndef ALO_UTIL_H
#define ALO_UTIL_H

#include "alo_engine.h"
#include <stdbool.h> /* cppcheck-suppress missingIncludeSystem */
#include <stdint.h> /* cppcheck-suppress missingIncludeSystem */
#include <math.h> /* cppcheck-suppress missingIncludeSystem */

#ifdef __cplusplus
extern "C" {
#endif

/* Utility helpers that are shared across multiple engine components. */

/**
 * @name Track-state helpers
 * These functions are used by both the engine and button logic.
 * @{ */

/**
 * @brief Return true if track index is valid.
 */
bool track_is_active(const Alo* self, int t);

/**
 * @brief Return true if a track is busy recording or armed.
 */
bool track_is_busy(const Alo* self, int t);

/**
 * @brief Reset state for a given track to idle.
 */
void clear_track_state(Alo* self, int t);

/** @} */

/**
 * Read the current Bars value from the engine, clamped to [1..16] and
 * converted to an integer.  Returns at least 1.
 */
uint32_t alo_get_bars_i(const Alo* self);

/** Return the current slices-per-bar value clamped to [2..8]. */
uint32_t alo_get_slices_per_bar_u(const Alo* self);

/**
 * @brief Return whether transient-based slice splitting is enabled.
 *
 * The control port is treated as a boolean: any value greater than 0.5 is
 * considered true.  A NULL port defaults to false.
 */

/* convenience guards used across modules */
#define ALO_GUARD_VOID(ptr) do { if (!(ptr)) return; } while (0)
#define ALO_GUARD_BOOL(ptr) do { if (!(ptr)) return false; } while (0)
#define ALO_GUARD_NULL(ptr) do { if (!(ptr)) return NULL; } while (0)

/* magic-number constants */
#define ALO_SPLIT_THRESHOLD 0.5f           /* split toggle threshold value */
#define ALO_SENS_THRESH_BASE 1.0f          /* base when sensitivity=0 */
#define ALO_SENS_THRESH_RANGE 19.0f        /* added when sensitivity=1 */
#define ALO_MIN_SLICES_PER_BAR 4u          /* floor in transient mode */
#define ALO_SLICE_ENV_ATTACK_DEFAULT_MS 5.0f /* default attack time for slice envelopes */

/* user-visible control ranges */
#define ALO_BARS_MIN_F 1.0f                /* min value for Bars slider */
#define ALO_BARS_MAX_F 16.0f               /* max value for Bars slider */
#define ALO_SLICES_PER_BAR_MIN_U 2u        /* min slices-per-bar slider value */
#define ALO_SLICES_PER_BAR_MAX_U 8u        /* max slices-per-bar slider value */
#define ALO_SLICE_ENV_FRAC_MAX 100.0f      /* percent */

/* timing / transport constants */
#define ALO_MIN_BPM 1e-6f                  /* beat-rate safety floor */
#define ALO_MIN_SAMPLES_PER_BEAT 1e-9f     /* prevent division-by-zero */
#define ALO_BEAT_BOUNDARY_EPS 1e-3f        /* small epsilon in beat units (~0.5ms) */
#define ALO_PARAM_CHANGE_EPS 0.01f         /* threshold for parameter change notifications */

/* looper behaviour constants */
#define ALO_LOOP_BTN_HOLD_FRAMES 8u        /* frames required to treat toggle as held */
#define ALO_CLICK_AMP_SCALE 0.1f           /* multiplier for click samples */
#define ALO_DOWNBEAT_GRACE_BEATS 0.25f     /* grace window for downbeat edge detection */
#define ALO_RATE_EPS 1e-6f                 /* small epsilon used when testing sample rate */

/* constants used by alo_edge_fade_samples_u32() */
#define ALO_EDGE_FADE_SAMPLES_MIN 16u       /* lower clamp for fade size */
#define ALO_EDGE_FADE_SAMPLES_MAX 512u      /* upper clamp for fade size */
#define ALO_EDGE_FADE_DEFAULT_SAMPLES 64u   /* return value when input invalid */

/* MIDI / slice constants */
#define ALO_DEFAULT_SLICE_ROOT_NOTE 36     /* default root when port missing */
#define ALO_SLICE_ROOT_MIN 0
#define ALO_SLICE_ROOT_MAX 127

/* geometry constants used by UI code (usage suggested but not required) */
#define ALO_UI_TITLE_TEXT     "ALOSCHEN"
#define ALO_UI_SUBTITLE_TEXT  "LOOPER"

bool alo_get_use_transient_slices_b(const Alo* self);

/**
 * Compute the configured slice count from bars & slices-per-bar and
 * clamp into [1..ALO_SLICE_INFO_MAX].  Used by both sampler_cache and
 * loop_engine to keep behaviour consistent and reduce duplicated math.
 */
static inline uint32_t alo_get_slice_count_u(const Alo* self)
{
    uint32_t bars = alo_get_bars_i(self);
    uint32_t spb  = alo_get_slices_per_bar_u(self);
    uint32_t count = bars * spb;
    if (count < 1u) count = 1u;
    if (count > ALO_SLICE_INFO_MAX) count = ALO_SLICE_INFO_MAX;
    return count;
}

/**
 * Softly clip a signal sample to the (-1,1) range with a smooth curve.
 * This is a simple limiter used to tame occasional peaks without hard
 * distortion.  Equivalent to the old `soft_clip_unit` helper in
 * loop_engine.c.
 */
static inline float alo_soft_clip_unit(float x)
{
  return x / (1.0f + fabsf(x));
}

/**
 * Test whether a float control port pointer represents a "pressed" state.
 * NULL pointers or non-positive values return false.
 */
static inline bool alo_port_pressed(const float* p)
{
  return p && (*p > 0.0f);
}

/**
 * Read the beats-per-bar value (time signature numerator) from the engine.
 * Returns at least DEFAULT_BEATS_PER_BAR when the input is missing or <1.
 */
static inline uint32_t alo_get_bpb_i(const Alo* self)
{
  if (!self) {
    return DEFAULT_BEATS_PER_BAR;
  }
  float bpb_f = (self->bpb > 1e-6f) ? self->bpb : (float)DEFAULT_BEATS_PER_BAR;
  int   bpb_i = (int)lrintf(bpb_f);
  if (bpb_i < 1) {
    return DEFAULT_BEATS_PER_BAR;
  }
  return (uint32_t)bpb_i;
}

#ifdef ALO_MATH_CHECKS
/** Sanitize a float by replacing non-finite values with 0.0. */
static inline float alo_sanitize_f32(const float x)
{
  return isfinite(x) ? x : 0.0f;
}
#endif

/**
 * Determine how many samples should be used to fade loop boundaries to zero.
 * This is roughly 1ms at the current sample rate, clamped to [16..512].
 */
uint32_t alo_edge_fade_samples_u32(const Alo* self);

/** Read the global slice‑decay parameter.  The value is interpreted as a
 * percentage (0..100) of the slice length; 0 gives the shortest possible
 * release (~1 sample), 100 uses the whole slice.  A NULL pointer returns 0.0.
 */
static inline float alo_get_slice_env_frac(const Alo* self)
{
    if (!self || !self->ports.slice_env_frac) {
        return 0.0f;
    }
    float v = *(self->ports.slice_env_frac);
    if (v < 0.0f) v = 0.0f;
    if (v > 100.0f) v = 100.0f;
    return v;
}

/** Compute number of samples to fade at slice boundaries.  If a global decay
 * control is configured the returned value is a fraction of the slice length
 * based on the percent supplied; attack is always handled separately and kept
 * short (~1 ms).  The result is clamped to [1..slice_len].
 * otherwise fall back to the default ~1ms value. */
uint32_t alo_get_slice_fade_samples(const Alo* self, uint32_t slice_len);
uint32_t alo_get_slice_env_attack_samples(const Alo* self);

static inline float alo_get_slice_sensitivity(const Alo* self)
{
    if (!self || !self->ports.slice_sens) {
        return 0.0f;
    }
    float v = *(self->ports.slice_sens);
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

float alo_sensitivity_to_threshold(const Alo* self);

/** Total number of slices (bars * slices_per_bar). */
static inline uint32_t alo_get_slice_count(const Alo* self)
{
  /* Original value is bars * slicesPerBar.  Enforce hard limits so that the
     sampler and any UI elements never attempt to manage more than 32 slices,
     and there is always at least one. */
  uint32_t count = alo_get_bars_i(self) * alo_get_slices_per_bar_u(self);
  if (count < 1u) {
    count = 1u;
  } else if (count > 32u) {
    count = 32u;
  }
  return count;
}

/** Number of samples in one bar (loop_samples/bars). Guaranteed >=1. */
static inline uint32_t alo_get_bar_len_samples(const Alo* self)
{
  uint32_t bars_i = alo_get_bars_i(self);
  if (bars_i > 0 && self && self->loop_samples > 0) {
    uint32_t len = self->loop_samples / bars_i;
    return len ? len : 1u;
  }
  return self ? (self->loop_samples ? self->loop_samples : 1u) : 1u;
}

/* Fade the start/end of a stereo loop buffer to zero over the given number
 * of samples.  The buffer is assumed to be laid out as L-size,R-size.
 */
void alo_apply_edge_fade_stereo(float* buf, uint32_t loop_start, uint32_t loop_samples,
                                uint32_t fade_samples);

/**
 * Write @p value to output port @p port only when the port is connected.
 * Eliminates the recurring null-guard pattern at every port write site.
 */
static inline void alo_port_write(float* port, float value)
{
  if (port) {
    *port = value;
  }
}

/**
 * fmod that always returns a non-negative result in [0, m).
 * Equivalent to the three-line pattern used across transport.c and
 * loop_engine.c:
 *   double r = fmod(x, m);
 *   if (r < 0.0) r += m;
 */
static inline double alo_fmod_positive(double x, double m)
{
  double r = fmod(x, m);
  return (r < 0.0) ? r + m : r;
}

#ifdef __cplusplus
}
#endif

#endif // ALO_UTIL_H
