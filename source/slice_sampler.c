#include "slice_sampler.h"

#include "alo_engine.h"

#include <math.h>
#include <string.h>

#ifdef ALO_MATH_CHECKS
static inline float alo_sanitize_f32(const float x) {
  return isfinite(x) ? x : 0.0f;
}
#endif

static inline bool alo_voice_is_active(const AloSliceVoice* v) {
  return v && v->active && (v->remaining_samples > 0);
}

static void alo_slice_sampler_start_voice(AloSliceSampler* s,
                                         uint32_t key,
                                         uint32_t start_delay_samples,
                                         uint32_t phase_samples,
                                         uint32_t length_samples,
                                         uint32_t fade_samples,
                                         float gain) {
  if (!s || length_samples == 0) {
    return;
  }

  /* Polyphonic voice allocation: grab a free voice, otherwise steal the one
   * closest to finishing (smallest remaining). This is bounded and RT-safe.
   */
  AloSliceVoice* v = NULL;
  for (uint32_t i = 0; i < (uint32_t)ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
    if (!alo_voice_is_active(&s->voices[i])) {
      v = &s->voices[i];
      break;
    }
  }

  if (!v) {
    uint32_t best_i = 0u;
    uint32_t best_rem = UINT32_MAX;
    for (uint32_t i = 0; i < (uint32_t)ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
      const uint32_t rem = s->voices[i].remaining_samples;
      if (rem < best_rem) {
        best_rem = rem;
        best_i = i;
      }
    }
    v = &s->voices[best_i];
  }

  memset(v, 0, sizeof(*v));
  v->active = true;
  v->key = key;
  v->start_delay_samples = start_delay_samples;
  v->phase_samples = phase_samples;
  v->remaining_samples = length_samples;
  v->total_samples = length_samples;
  v->elapsed_samples = 0;
  if (fade_samples * 2u > length_samples) {
    fade_samples = length_samples / 2u;
  }
  v->fade_samples = fade_samples;
  v->fade_inv = (fade_samples > 0u) ? (1.0f / (float)fade_samples) : 0.0f;
  v->gain = gain;
}

void alo_slice_sampler_reset(AloSliceSampler* s) {
  if (!s) {
    return;
  }
  memset(s, 0, sizeof(*s));
}

void alo_slice_sampler_schedule(AloSliceSampler* s,
                               const uint32_t start_offset_samples,
                               const uint32_t phase_samples,
                               const uint32_t length_samples,
                               uint32_t fade_samples,
                               const float gain) {
  if (!s || length_samples == 0) {
    return;
  }

  /* Per-slice key: used for retrigger-kill, but that kill is applied at the
   * exact trigger sample (in process_block), not here. This allows sequential
   * Note-Ons for the same slice within one block to retrigger correctly.
   */
  const uint32_t key = phase_samples;

  /* Polyphonic scheduling: append into the pending queue.
   * If full, deterministically overwrite slot 0.
   */
  uint32_t slot = 0u;
  bool found = false;
  for (uint32_t i = 0; i < (uint32_t)ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
    if (!s->pending[i].active) {
      slot = i;
      found = true;
      break;
    }
  }
  if (!found) {
    slot = 0u;
  }

  AloSlicePending* const p = &s->pending[slot];
  memset(p, 0, sizeof(*p));
  p->active = true;
  p->key = key;
  p->offset_samples = start_offset_samples;
  p->phase_samples = phase_samples;
  p->length_samples = length_samples;
  if (fade_samples * 2u > length_samples) {
    fade_samples = length_samples / 2u;
  }
  p->fade_samples = fade_samples;
  p->gain = gain;
}

void alo_slice_sampler_begin_block(AloSliceSampler* s, const uint32_t n_samples) {
  if (!s) {
    return;
  }

  for (uint32_t i = 0; i < (uint32_t)ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
    AloSlicePending* const p = &s->pending[i];
    if (!p->active) {
      continue;
    }

    if (p->offset_samples >= n_samples) {
      p->offset_samples -= n_samples;
    }
  }
}

bool alo_slice_sampler_is_busy(const AloSliceSampler* s) {
  if (!s) {
    return false;
  }

  for (uint32_t i = 0; i < (uint32_t)ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
    if (alo_voice_is_active(&s->voices[i])) {
      return true;
    }
  }

  for (uint32_t i = 0; i < (uint32_t)ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
    if (s->pending[i].active) {
      return true;
    }
  }

  return false;
}

static inline void alo_mix_looper_at_phase(const Alo* alo,
                                          const float track_gain_3[3],
                                          const uint32_t phase_samples,
                                          float* out_l,
                                          float* out_r) {
  *out_l = 0.0f;
  *out_r = 0.0f;

  if (!alo || !track_gain_3 || alo->loop_samples == 0) {
    return;
  }

  (void)track_gain_3;

  /* Sampler reads from the committed-mix cache buffer.
   * If the cache isn't valid yet, output silence.
   */
  if (!alo->sampler_src_valid || !alo->sampler_src_buf) {
    return;
  }

  const uint32_t phase = (phase_samples < alo->loop_samples)
                             ? phase_samples
                             : (phase_samples % alo->loop_samples);

  float l = alo->sampler_src_buf[phase];
  float r = alo->sampler_src_buf[phase + LOOP_SIZE];

#ifdef ALO_MATH_CHECKS
  l = alo_sanitize_f32(l);
  r = alo_sanitize_f32(r);
#endif

  const float g = alo->sampler_src_norm_gain;
  if (g > 0.0f && g < 1.0f) {
    l *= g;
    r *= g;
  }

  *out_l = l;
  *out_r = r;
}

static inline float alo_voice_env(const AloSliceVoice* v) {
  if (!v || v->fade_samples == 0u) {
    return 1.0f;
  }

  float env = 1.0f;
  const uint32_t f = v->fade_samples;
  const float inv = v->fade_inv;

  if (v->elapsed_samples < f) {
    env *= (float)v->elapsed_samples * inv;
  }

  const uint32_t rem = (v->remaining_samples > 0u) ? (v->remaining_samples - 1u) : 0u;
  if (rem < f) {
    env *= (float)rem * inv;
  }
  return env;
}

void alo_slice_sampler_process_block(AloSliceSampler* s,
                                    const Alo* alo,
                                    const float track_gain_3[3],
                                    const uint32_t n_samples,
                                    float* out_l,
                                    float* out_r) {
  alo_slice_sampler_process_chunk(s, alo, track_gain_3, 0u, n_samples, out_l, out_r);
}

void alo_slice_sampler_process_chunk(AloSliceSampler* s,
                                    const Alo* alo,
                                    const float track_gain_3[3],
                                    const uint32_t block_offset_samples,
                                    const uint32_t n_samples,
                                    float* out_l,
                                    float* out_r) {
  if (!out_l || !out_r || n_samples == 0u) {
    return;
  }

  memset(out_l, 0, sizeof(float) * (size_t)n_samples);
  memset(out_r, 0, sizeof(float) * (size_t)n_samples);

  if (!s || !alo || alo->loop_samples == 0) {
    return;
  }

  for (uint32_t pos = 0; pos < n_samples; ++pos) {
    const uint32_t pos_in_block = block_offset_samples + pos;

    /* Fire pending triggers scheduled for this sample (relative to full block). */
    for (uint32_t p_i = 0; p_i < (uint32_t)ALO_SLICE_SAMPLER_MAX_PENDING; ++p_i) {
      AloSlicePending* const p = &s->pending[p_i];
      if (!p->active) {
        continue;
      }
      if (p->offset_samples != pos_in_block) {
        continue;
      }

      /* Same-slice retrigger: kill any active voice with this key, but do not
       * affect other keys (polyphonic lanes).
       */
      for (uint32_t v_i = 0; v_i < (uint32_t)ALO_SLICE_SAMPLER_MAX_VOICES; ++v_i) {
        AloSliceVoice* const v = &s->voices[v_i];
        if (alo_voice_is_active(v) && v->key == p->key) {
          v->active = false;
          v->remaining_samples = 0u;
        }
      }

      alo_slice_sampler_start_voice(s, p->key, 0u, p->phase_samples, p->length_samples,
                                   p->fade_samples, p->gain);
      p->active = false;
    }

    /* Mix all active voices for this sample. */
    for (uint32_t i = 0; i < (uint32_t)ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
      AloSliceVoice* const v = &s->voices[i];
      if (!alo_voice_is_active(v)) {
        v->active = false;
        continue;
      }

      if (v->start_delay_samples > 0u) {
        v->start_delay_samples--;
        continue;
      }

      float l = 0.0f;
      float r = 0.0f;
      alo_mix_looper_at_phase(alo, track_gain_3, v->phase_samples, &l, &r);

      const float env = alo_voice_env(v);
      const float g = v->gain * env;
      out_l[pos] += g * l;
      out_r[pos] += g * r;

      v->phase_samples++;
      if (v->phase_samples >= alo->loop_samples) {
        v->phase_samples = 0u;
      }

      if (v->elapsed_samples < v->total_samples) {
        v->elapsed_samples++;
      }

      if (v->remaining_samples > 0u) {
        v->remaining_samples--;
      }
      if (v->remaining_samples == 0u) {
        v->active = false;
      }
    }
  }
}

void alo_slice_sampler_process_sample(AloSliceSampler* s,
                                     const Alo* alo,
                                     const float track_gain_3[3],
                                     float* out_l,
                                     float* out_r) {
  if (!out_l || !out_r) {
    return;
  }

  *out_l = 0.0f;
  *out_r = 0.0f;

  if (!s || !alo || alo->loop_samples == 0) {
    return;
  }

  for (uint32_t i = 0; i < (uint32_t)ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
    AloSliceVoice* const v = &s->voices[i];
    if (!alo_voice_is_active(v)) {
      v->active = false;
      continue;
    }

    if (v->start_delay_samples > 0) {
      v->start_delay_samples--;
      continue;
    }

    float l = 0.0f;
    float r = 0.0f;
    alo_mix_looper_at_phase(alo, track_gain_3, v->phase_samples, &l, &r);

    const float env = alo_voice_env(v);
    *out_l += (v->gain * env) * l;
    *out_r += (v->gain * env) * r;

    v->phase_samples++;
    if (v->phase_samples >= alo->loop_samples) {
      v->phase_samples = 0;
    }

    if (v->elapsed_samples < v->total_samples) {
      v->elapsed_samples++;
    }

    if (v->remaining_samples > 0) {
      v->remaining_samples--;
    }
    if (v->remaining_samples == 0) {
      v->active = false;
    }
  }
}
