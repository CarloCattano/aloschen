#include "slice_sampler.h"
#include "alo_engine.h"
#include <string.h>
#include <math.h>
#ifdef ALO_MATH_CHECKS
static inline float alo_sanitize_f32(const float x) {
  return isfinite(x) ? x : 0.0f;
}
#endif

static inline bool alo_voice_is_active(const AloSliceVoice* v) {
  return v && v->active && (v->remaining_samples > 0);
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

// Implementation for missing API functions to resolve linker errors

void alo_slice_sampler_reset(AloSliceSampler* s) {
    if (!s) return;
    // Reset all voices and pending triggers
    for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
        s->voices[i].active = false;
        s->voices[i].phase_samples = 0;
        s->voices[i].elapsed_samples = 0;
        s->voices[i].remaining_samples = 0;
    }
    for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
        s->pending[i].active = false;
    }
    // Optionally clear slice buffers/validity
    for (uint32_t i = 0; i < ALO_SLICE_INFO_MAX; ++i) {
        s->slice_audio_valid[i] = false;
        s->slice_audio_len[i] = 0;
    }
}

void alo_slice_sampler_schedule(AloSliceSampler* s, uint32_t start_offset_samples,
                               uint32_t phase_samples, uint32_t length_samples,
                               uint32_t fade_samples, float gain) {
    if (!s) return;
    // Use key = phase_samples for now (could be improved)
    alo_slice_sampler_start_voice(s, phase_samples, start_offset_samples, phase_samples, length_samples, fade_samples, gain);
}

void alo_slice_sampler_start_voice(AloSliceSampler* s, uint32_t key,
                                  uint32_t start_delay_samples, uint32_t phase_samples,
                                  uint32_t length_samples, uint32_t fade_samples, float gain) {
    if (!s) return;

    /* Retrigger behavior: if any existing voice already using the same key
     * (phase_samples), kill it immediately so the new note can reuse that slot.
     * This mirrors the design from samplv1 and ensures zero-latency retrigger
     * within the same block.
     */
    for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
        AloSliceVoice* v = &s->voices[i];
        if (v->active && v->key == key) {
            v->active = false;
            break;
        }
    }

    /* Find a free voice slot after killing any retrigger candidate. */
    for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
        AloSliceVoice* v = &s->voices[i];
        if (!v->active) {
            v->active = true;
            v->key = key;
            v->start_delay_samples = start_delay_samples;
            v->phase_samples = phase_samples;
            v->remaining_samples = length_samples;
            v->total_samples = length_samples;
            v->elapsed_samples = 0;
            v->fade_samples = fade_samples;
            v->fade_inv = (fade_samples > 0) ? (1.0f / (float)fade_samples) : 1.0f;
            v->gain = gain;
            // slice buffers will be assigned by caller when integrating with looper
            break;
        }
    }
}
