#include "transient_detector.h"
#include <math.h>
#include <string.h>

/* Typical time constants for a lightweight onset detector. */
#define TD_FAST_ATTACK_MS 1.0f /* ~1ms for onset detection */
#define TD_SLOW_DECAY_MS 50.0f /* ~50ms to follow background */
#define TD_RATIO_EPS 1.0e-9f

static float td_absf(float x)
{
  return (x < 0.0f) ? -x : x;
}

static float td_safe_time_ms(float v, float fallback)
{
  return (v > 0.0f) ? v : fallback;
}

static uint32_t td_ms_to_samples(float sample_rate, float ms)
{
  if (!(sample_rate > 0.0f) || !(ms > 0.0f)) {
    return 0u;
  }
  return (uint32_t)((ms * 0.001f) * sample_rate + 0.5f);
}

static void td_reset_candidate(TransientCandidate* c)
{
  if (!c) {
    return;
  }
  memset(c, 0, sizeof(*c));
}

static float td_compute_ratio(float fast_env, float slow_env)
{
  const float denom = (slow_env > TD_RATIO_EPS) ? slow_env : TD_RATIO_EPS;
  return fast_env / denom;
}

static void td_begin_candidate(TransientDetector* td, uint32_t sample_index, float amplitude,
                               float ratio)
{
  td_reset_candidate(&td->candidate);
  td->candidate.active         = true;
  td->candidate.complete       = false;
  td->candidate.start_sample   = sample_index;
  td->candidate.peak_sample    = sample_index;
  td->candidate.end_sample     = sample_index;
  td->candidate.peak_amplitude = amplitude;
  td->candidate.peak_ratio     = ratio;
}

static void td_update_candidate_peak(TransientCandidate* c, uint32_t sample_index, float amplitude,
                                     float ratio)
{
  if (!c || !c->active) {
    return;
  }

  c->end_sample = sample_index;

  if (ratio > c->peak_ratio || amplitude > c->peak_amplitude) {
    c->peak_ratio     = ratio;
    c->peak_amplitude = amplitude;
    c->peak_sample    = sample_index;
  }
}

static bool td_candidate_long_enough(const TransientDetector* td, const TransientCandidate* c)
{
  if (!td || !c || !c->active) {
    return false;
  }

  const uint32_t dur =
      (c->end_sample >= c->start_sample) ? (c->end_sample - c->start_sample + 1u) : 0u;

  return dur >= td->min_burst_samples;
}

static void td_finish_candidate(TransientDetector* td, bool* candidate_complete_out)
{
  if (!td || !td->candidate.active) {
    return;
  }

  td->candidate.active   = false;
  td->candidate.complete = td_candidate_long_enough(td, &td->candidate);

  if (td->candidate.complete) {
    td->last_completed_candidate = td->candidate;
    if (candidate_complete_out) {
      *candidate_complete_out = true;
    }
  }

  td_reset_candidate(&td->candidate);
}

void td_init_ex(TransientDetector* td, float sample_rate, float threshold_ratio,
                float debounce_time_ms, float min_burst_time_ms, float end_ratio)
{
  if (!td) {
    return;
  }

  td->fast_env = 0.0f;
  td->slow_env = 0.0f;

  {
    const float fast_time_s = TD_FAST_ATTACK_MS * 0.001f;
    const float slow_time_s = TD_SLOW_DECAY_MS * 0.001f;
    const float sr          = (sample_rate > 1.0f) ? sample_rate : 48000.0f;

    td->fast_coeff = expf(-1.0f / (fast_time_s * sr));
    td->slow_coeff = expf(-1.0f / (slow_time_s * sr));
    td->debounce_samples =
        td_ms_to_samples(sr, td_safe_time_ms(debounce_time_ms, TD_DEFAULT_DEBOUNCE_MS));
    td->min_burst_samples =
        td_ms_to_samples(sr, td_safe_time_ms(min_burst_time_ms, TD_DEFAULT_MIN_BURST_MS));
  }

  td->threshold_ratio = (threshold_ratio > 1.0f) ? threshold_ratio : 1.0f;
  td->end_ratio       = (end_ratio > 0.0f) ? end_ratio : TD_DEFAULT_END_RATIO;

  td->debounce_counter  = 0u;
  td->sample_index      = 0u;
  td->current_amplitude = 0.0f;
  td->current_ratio     = 0.0f;

  td_reset_candidate(&td->candidate);
  td_reset_candidate(&td->last_completed_candidate);
}

void td_init(TransientDetector* td, float sample_rate, float threshold_ratio,
             float debounce_time_ms)
{
  td_init_ex(td, sample_rate, threshold_ratio, debounce_time_ms, TD_DEFAULT_MIN_BURST_MS,
             TD_DEFAULT_END_RATIO);
}

TransientDetectorEvent td_process_sample_ex(TransientDetector* td, float sample)
{
  TransientDetectorEvent ev;
  memset(&ev, 0, sizeof(ev));

  if (!td) {
    return ev;
  }

  {
    const uint32_t idx = td->sample_index;
    const float    x   = td_absf(sample);

    td->current_amplitude = x;

    td->fast_env      = td->fast_coeff * td->fast_env + (1.0f - td->fast_coeff) * x;
    td->slow_env      = td->slow_coeff * td->slow_env + (1.0f - td->slow_coeff) * x;
    td->current_ratio = td_compute_ratio(td->fast_env, td->slow_env);

    ev.amplitude    = td->current_amplitude;
    ev.onset_ratio  = td->current_ratio;
    ev.sample_index = idx;

    if (td->candidate.active) {
      td_update_candidate_peak(&td->candidate, idx, x, td->current_ratio);
    }

    if (td->debounce_counter == 0u) {
      if (td->current_ratio > td->threshold_ratio) {
        ev.triggered         = true;
        td->debounce_counter = td->debounce_samples;
        if (!td->candidate.active) {
          td_begin_candidate(td, idx, x, td->current_ratio);
        } else {
          td_update_candidate_peak(&td->candidate, idx, x, td->current_ratio);
        }
      }
    } else {
      td->debounce_counter--;
    }

    if (td->candidate.active) {
      const bool below_end_ratio = (td->current_ratio <= td->end_ratio);
      const bool in_quiet_tail   = (x <= (td->candidate.peak_amplitude * 0.25f));

      if (below_end_ratio && in_quiet_tail) {
        td->candidate.end_sample = idx;
        td_finish_candidate(td, &ev.candidate_complete);
      }
    }

    if (td->candidate.active) {
      ev.candidate = td->candidate;
    } else if (ev.candidate_complete) {
      ev.candidate = td->last_completed_candidate;
    }

    td->sample_index = idx + 1u;
  }

  return ev;
}

bool td_process_sample(TransientDetector* td, float sample)
{
  return td_process_sample_ex(td, sample).triggered;
}

const TransientCandidate* td_last_completed_candidate(const TransientDetector* td)
{
  return td ? &td->last_completed_candidate : (const TransientCandidate*)0;
}

const TransientCandidate* td_current_candidate(const TransientDetector* td)
{
  return td ? &td->candidate : (const TransientCandidate*)0;
}

float td_current_onset_ratio(const TransientDetector* td)
{
  return td ? td->current_ratio : 0.0f;
}

float td_current_amplitude(const TransientDetector* td)
{
  return td ? td->current_amplitude : 0.0f;
}