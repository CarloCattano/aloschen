/* slice-based sampler implementation for the looper engine */

#include "slice_sampler.h"
#include "alo_engine.h"
#include "alo_util.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>

/* handy guard macro to reduce redundant null checks */
#define ALO_GUARD(ptr) if (!(ptr)) return

/* helper to clear a single slice buffer structure */
static void alo_slice_buffers_reset(AloSliceBuffer* buf)
{
    if (!buf) {
        return;
    }
    buf->data     = NULL;
    buf->length   = 0;
    buf->valid    = false;
    buf->borrowed = true;
}

/* Internal helpers (optimized for realtime) */

static inline uint32_t alo_wrap_phase(uint32_t phase, uint32_t limit)
{
  if (limit == 0u) {
    return 0u;
  }
  if (phase >= limit) {
    phase -= limit;
  }
  return phase;
}

/* allocate a voice according to current play mode and initialise it.
 * returns the voice pointer so callers can attach slice buffers without a
 * costly second scan. */
static AloSliceVoice* alo_slice_sampler_start_voice(AloSliceSampler* s, const struct Alo* alo,
                                         uint32_t key, uint32_t start_delay_samples,
                                         uint32_t phase_samples, uint32_t length_samples,
                                         uint32_t fade_samples, float gain);

/* reset sampler state and deactivate voices/pending triggers */
void alo_slice_sampler_reset(AloSliceSampler* s)
{
  ALO_GUARD(s);
  s->next_voice = 0;
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
    s->voices[i].active            = false;
    s->voices[i].remaining_samples = 0u;
    s->voices[i].total_samples     = 0u;
    s->voices[i].elapsed_samples   = 0u;
    s->voices[i].start_delay_samples = 0u;
    s->voices[i].phase_samples     = 0u;
    s->voices[i].fade_samples      = 0u;
    s->voices[i].fade_inv          = 0.0f;
    s->voices[i].gain              = 0.0f;
    s->voices[i].midi_note         = 0u;
    s->voices[i].slice_buf_l       = NULL;
    s->voices[i].slice_buf_r       = NULL;
    s->voices[i].slice_buf_len     = 0u;
    s->voices[i].env.running       = false;
    s->voices[i].env.total_frames  = 0u;
  }
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
    s->pending[i].active = false;
  }
}

/* mark all slice buffers invalid and begin incremental clearing */
void alo_slice_sampler_clear_buffers(AloSliceSampler* s)
{
  ALO_GUARD(s);
  for (uint32_t i = 0; i < ALO_SLICE_INFO_MAX; ++i) {
    alo_slice_buffers_reset(&s->slice_buffers[i]);
    alo_slice_buffers_reset(&s->slice_buffers_shadow[i]);
  }
  s->clear_in_progress = false;
  s->clear_slice_idx   = 0u;
  s->clear_offset      = 0u;
}

/* advance in-flight buffer clear job by up to max_samples zeros */
void alo_slice_sampler_step_clear(AloSliceSampler* s, uint32_t max_samples)
{
  ALO_GUARD(s);
  /* no-op placeholder; clearing is handled by the caller's buffer management
     logic. leaving the stub in place to preserve API. */
  (void)max_samples;
}

bool alo_slice_sampler_is_busy(const AloSliceSampler* s)
{
  if (!s) {
    return false;
  }
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
    if (s->voices[i].active && s->voices[i].remaining_samples > 0u) {
      return true;
    }
  }
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
    if (s->pending[i].active) {
      return true;
    }
  }
  return false;
}

void alo_slice_sampler_begin_block(AloSliceSampler* s, const uint32_t n_samples)
{
  if (!s) {
    return;
  }
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
    AloSlicePending* p = &s->pending[i];
    if (p->active && p->offset_samples >= n_samples) {
      p->offset_samples -= n_samples;
    }
  }
}

void alo_slice_sampler_schedule(AloSliceSampler* s, const struct Alo* alo,
                                uint32_t start_offset_samples, uint32_t phase_samples,
                                uint32_t length_samples, uint32_t fade_samples, float gain,
                                uint8_t midi_note)
{
  (void)alo;
  ALO_GUARD(s);

  if (length_samples == 0u) {
    length_samples = 1u;
  }

  /* Find free pending slot */
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
    if (!s->pending[i].active) {
      AloSlicePending* p = &s->pending[i];
      p->active         = true;
      p->key            = phase_samples; /* use phase as identity */
      p->offset_samples = start_offset_samples;
      p->phase_samples  = phase_samples;
      p->length_samples = length_samples;
      p->fade_samples   = fade_samples;
      p->gain           = gain;
      p->midi_note      = midi_note;
      if (s->slice_buffer_len > 0u) {
        p->slice_idx = phase_samples / s->slice_buffer_len;
      } else {
        p->slice_idx = 0u;
      }
      break;
    }
  }
}

/* DSP logic */

void alo_slice_sampler_process_chunk(AloSliceSampler* s, const struct Alo* alo,
                                     const uint32_t block_offset_samples, const uint32_t n_samples,
                                     float* out_l, float* out_r)
{
  ALO_GUARD(s);
  if (!alo || !out_l || !out_r || n_samples == 0u || alo->loop_samples == 0u) {
    return;
  }

  if (s->clear_in_progress) {
    alo_slice_sampler_step_clear(s, n_samples *
                                        (s->slice_buffer_channels ? s->slice_buffer_channels : 1u));
  }

  memset(out_l, 0, n_samples * sizeof(float));
  memset(out_r, 0, n_samples * sizeof(float));

  if (!alo->sampler_src_valid || !alo->sampler_src_buf) {
    return;
  }

  const float global_gain = alo->sampler_src_norm_gain;
  const uint32_t loop_len = alo->loop_samples;

  const AloSliceBuffer* buffers_active =
      (s->slice_buffers_using_primary ? s->slice_buffers : s->slice_buffers_shadow);

  for (uint32_t pos = 0u; pos < n_samples; ++pos) {
    const uint32_t current_time = block_offset_samples + pos;

    for (uint32_t p_i = 0u; p_i < ALO_SLICE_SAMPLER_MAX_PENDING; ++p_i) {
      AloSlicePending* p = &s->pending[p_i];
      if (p->active && p->offset_samples == current_time) {
        const float* buf_l   = NULL;
        const float* buf_r   = NULL;
        uint32_t     buf_len = 0u;

        if (p->phase_samples < alo->loop_samples && p->slice_idx < ALO_SLICE_INFO_MAX) {
          const AloSliceBuffer* sb = &buffers_active[p->slice_idx];
          if (sb->valid) {
            buf_len = sb->length;
            buf_l   = sb->data;
            if (s->slice_buffer_channels == 2u) {
              buf_r = sb->data + sb->length;
            }
          }
        }

        AloSliceVoice* v = alo_slice_sampler_start_voice(
            s, alo, p->key, 0u, p->phase_samples, p->length_samples, p->fade_samples, p->gain);

        if (v) {
          v->midi_note = p->midi_note;
          if (buf_l) {
            v->slice_buf_l   = buf_l;
            v->slice_buf_r   = buf_r;
            v->slice_buf_len = buf_len;
          }
        }
        p->active = false;
      }
    }

    for (uint32_t i = 0u; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
      AloSliceVoice* v = &s->voices[i];
      if (!v->active) {
        continue;
      }

      if (v->start_delay_samples > 0u) {
        v->start_delay_samples--;
        continue;
      }

      uint32_t ph = v->phase_samples;
      float l;
      float r;

      if (v->slice_buf_l && ph < v->slice_buf_len) {
        l = v->slice_buf_l[ph];
        r = v->slice_buf_r ? v->slice_buf_r[ph] : l;
      } else {
        l = alo->sampler_src_buf[ph];
        r = alo->sampler_src_buf[ph + LOOP_SIZE];
      }

      /* Apply a simple per-voice attack ramp plus the existing end fade so
       * both user-facing Attack and Decay controls shape the audible slice.
       * Attack ramps from 0 -> 1 over the configured attack window.
       * Decay shortens the voice duration upstream; fade_samples still tapers
       * the final part of the shortened voice to avoid clicks.
       */
      float attack_gain = 1.0f;
      const uint32_t attack_samples = alo_get_slice_env_attack_samples(alo);
      if (attack_samples > 1u && v->elapsed_samples < attack_samples) {
        attack_gain = (float)v->elapsed_samples / (float)(attack_samples - 1u);
        if (attack_gain < 0.0f) {
          attack_gain = 0.0f;
        } else if (attack_gain > 1.0f) {
          attack_gain = 1.0f;
        }
      }

      float fade_gain = 1.0f;
      if (v->fade_samples > 0u && v->remaining_samples <= v->fade_samples) {
        fade_gain = (float)v->remaining_samples * v->fade_inv;
        if (fade_gain < 0.0f) {
          fade_gain = 0.0f;
        } else if (fade_gain > 1.0f) {
          fade_gain = 1.0f;
        }
      }

      const float total_gain = v->gain * attack_gain * fade_gain * global_gain;
      out_l[pos] += l * total_gain;
      out_r[pos] += r * total_gain;

      v->phase_samples = alo_wrap_phase(ph + 1u, loop_len);
      v->elapsed_samples++;

      if (--v->remaining_samples == 0u) {
        v->active = false;
      }
    }
  }
}

static AloSliceVoice* alo_slice_sampler_start_voice(AloSliceSampler* s, const struct Alo* alo, uint32_t key,
                                   uint32_t start_delay_samples, uint32_t phase_samples,
                                   uint32_t length_samples, uint32_t fade_samples, float gain)
{
  (void)alo;
  if (!s) {
    return NULL;
  }

  AloSliceVoice* target = NULL;

  /* polyphonic behaviour: prefer unused voices, otherwise round-robin reuse */
  for (uint32_t i = 0u; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
    if (!s->voices[i].active) {
      target = &s->voices[i];
      break;
    }
  }
  if (!target) {
    uint32_t idx = s->next_voice % ALO_SLICE_SAMPLER_MAX_VOICES;
    s->next_voice = idx + 1u;
    target = &s->voices[idx];
  }

  if (target) {
    if (length_samples == 0u) {
      length_samples = 1u;
    }

    if (fade_samples > length_samples) {
      fade_samples = length_samples;
    }
    if (fade_samples == 0u && length_samples > 1u) {
      fade_samples = 1u;
    }

    target->active              = true;
    target->key                 = key;
    target->start_delay_samples = start_delay_samples;
    target->phase_samples       = phase_samples;
    target->remaining_samples   = length_samples;
    target->total_samples       = length_samples;
    target->elapsed_samples     = 0u;
    target->fade_samples        = fade_samples;
    target->fade_inv            = (fade_samples > 0u) ? (1.0f / (float)fade_samples) : 0.0f;
    target->gain                = gain;
    target->midi_note           = 0u;

    target->slice_buf_l   = NULL;
    target->slice_buf_r   = NULL;
    target->slice_buf_len = 0u;

    target->env.running      = false;
    target->env.total_frames = length_samples;
  }

  return target;
}

/* ------------------------------------------------------------------------
 * Memory & Initialization (Non-RT)
 * ------------------------------------------------------------------------ */

void alo_slice_sampler_free_buffers(AloSliceSampler* s)
{
  if (!s) {
    return;
  }
  for (uint32_t i = 0u; i < ALO_SLICE_INFO_MAX; ++i) {
    s->slice_buffers[i].data          = NULL;
    s->slice_buffers[i].length        = 0u;
    s->slice_buffers[i].valid         = false;
    s->slice_buffers[i].borrowed      = true;
    s->slice_buffers_shadow[i].data   = NULL;
    s->slice_buffers_shadow[i].length = 0u;
    s->slice_buffers_shadow[i].valid  = false;
    s->slice_buffers_shadow[i].borrowed = true;
  }
  s->slice_buffers_using_primary = true;
  s->clear_in_progress           = false;
  s->clear_slice_idx             = 0u;
  s->clear_offset                = 0u;
}

bool alo_slice_sampler_alloc_buffers(AloSliceSampler* s, uint32_t max_len, uint32_t channels)
{
  if (!s || channels < 1u || channels > 2u || max_len == 0u) {
    return false;
  }
  alo_slice_sampler_free_buffers(s);
  s->slice_buffer_channels = channels;
  s->slice_buffer_len      = max_len;
  for (uint32_t i = 0u; i < ALO_SLICE_INFO_MAX; ++i) {
    alo_slice_buffers_reset(&s->slice_buffers[i]);
    alo_slice_buffers_reset(&s->slice_buffers_shadow[i]);
  }
  s->slice_buffers_using_primary = true;
  s->clear_in_progress           = false;
  s->clear_slice_idx             = 0u;
  s->clear_offset                = 0u;
  return true;
}
