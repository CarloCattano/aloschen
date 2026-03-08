/* slice-based sampler implementation for the looper engine */

#include "slice_sampler.h"
#include "alo_engine.h"
#include "alo_util.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <math.h> /* cosf() used by envelope */

/* handy guard macro to reduce redundant null checks */
#define ALO_GUARD(ptr) if (!(ptr)) return

/* helper to clear a single slice buffer structure */
static void alo_slice_buffers_reset(AloSliceBuffer* buf)
{
    buf->data   = NULL;
    buf->length = 0;
    buf->valid  = false;
}

/* Internal helpers (optimized for realtime) */

/* inline helper removed; callers simply test active and remaining_samples */

static inline uint32_t alo_wrap_phase(uint32_t phase, uint32_t limit)
{
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
    s->voices[i].remaining_samples = 0;
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
  s->clear_slice_idx   = 0;
  s->clear_offset      = 0;
}

/* advance in-flight buffer clear job by up to max_samples zeros */
void alo_slice_sampler_step_clear(AloSliceSampler* s, uint32_t max_samples)
{
  ALO_GUARD(s);
  /* no-op placeholder; clearing is handled by the caller's buffer management
     logic.  leaving the stub in place to preserve API. */
  (void)max_samples;
}

bool alo_slice_sampler_is_busy(const AloSliceSampler* s)
{
  if (!s)
    return false;
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
    if (s->voices[i].active && s->voices[i].remaining_samples > 0)
      return true;
  }
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
    if (s->pending[i].active)
      return true;
  }
  return false;
}

void alo_slice_sampler_begin_block(AloSliceSampler* s, const uint32_t n_samples)
{
  if (!s)
    return;
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
    AloSlicePending* p = &s->pending[i];
    if (p->active && p->offset_samples >= n_samples) {
      p->offset_samples -= n_samples;
    }
  }
}

void alo_slice_sampler_schedule(AloSliceSampler* s, const struct Alo* alo,
                                uint32_t start_offset_samples, uint32_t phase_samples,
                                uint32_t length_samples, uint32_t fade_samples, float gain)
{
  ALO_GUARD(s);
  // Find free pending slot
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
    if (!s->pending[i].active) {
      AloSlicePending* p = &s->pending[i];
      p->active         = true;
      p->key            = phase_samples; // Use phase as key for retrigger logic
      p->offset_samples = start_offset_samples;
      p->phase_samples  = phase_samples;
      p->length_samples = length_samples;
      p->fade_samples   = fade_samples;
      p->gain           = gain;
      /* precompute slice index if we know buffer length */
      if (s->slice_buffer_len > 0) {
        p->slice_idx = phase_samples / s->slice_buffer_len;
      } else {
        p->slice_idx = 0;
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
  if (!alo || !out_l || !out_r || n_samples == 0 || alo->loop_samples == 0)
    return;

  /* Perform incremental buffer clearing if requested.  We budget one float per
   * sample per channel; the exact rate is unimportant as long as work is
   * distributed.  Use n_samples*channels as a simple proxy for effort.
   */
  if (s->clear_in_progress) {
    alo_slice_sampler_step_clear(s, n_samples *
                                        (s->slice_buffer_channels ? s->slice_buffer_channels : 1));
  }

  memset(out_l, 0, n_samples * sizeof(float));
  memset(out_r, 0, n_samples * sizeof(float));

  if (!alo->sampler_src_valid || !alo->sampler_src_buf)
    return;

  const float* env_port = (alo ? alo->ports.slice_env_frac : NULL);
  const float    global_gain = alo->sampler_src_norm_gain;
  const uint32_t loop_len    = alo->loop_samples;

  /* choose active slice buffer set for voice assignment */
  const AloSliceBuffer* buffers_active =
      (s->slice_buffers_using_primary ? s->slice_buffers : s->slice_buffers_shadow);

  const uint32_t max_pending = ALO_SLICE_SAMPLER_MAX_PENDING;

  for (uint32_t pos = 0; pos < n_samples; ++pos) {
    const uint32_t current_time = block_offset_samples + pos;

    for (uint32_t p_i = 0; p_i < max_pending; ++p_i) {
      AloSlicePending* p = &s->pending[p_i];
      if (p->active && p->offset_samples == current_time) {
        /* pass slice buffer pointers to voice for sample-accurate playback */
        const float* buf_l   = NULL;
        const float* buf_r   = NULL;
        uint32_t     buf_len = 0;
        if (p->phase_samples < alo->loop_samples && p->slice_idx < ALO_SLICE_INFO_MAX) {
          const AloSliceBuffer* sb = &buffers_active[p->slice_idx];
          if (sb->valid) {
            buf_len = sb->length;
            buf_l   = sb->data;
            if (s->slice_buffer_channels == 2)
              buf_r = sb->data + sb->length;
          }
        }
        AloSliceVoice* v = alo_slice_sampler_start_voice(s, alo, p->key, 0u, p->phase_samples, p->length_samples,
                                      p->fade_samples, p->gain);
        if (v && buf_l) {
          v->slice_buf_l   = buf_l;
          v->slice_buf_r   = buf_r;
          v->slice_buf_len = buf_len;
        }
        p->active = false;
      }
    }

    for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
      AloSliceVoice* v = &s->voices[i];
      if (!v->active)
        continue;

      if (v->start_delay_samples > 0) {
        v->start_delay_samples--;
        continue;
      }

      uint32_t ph = v->phase_samples;
      float    l;
      float    r;
      if (v->slice_buf_l && ph < v->slice_buf_len) {
        l = v->slice_buf_l[ph];
        r = v->slice_buf_r ? v->slice_buf_r[ph] : l;
      } else {
        l = alo->sampler_src_buf[ph];
        r = alo->sampler_src_buf[ph + LOOP_SIZE];
      }

      /* envelope tick replaces the ad-hoc fades */
      /* update release window on-the-fly if user changed decay slider */
      if (env_port) {
        uint32_t new_fade = alo_get_slice_fade_samples(alo, v->total_samples);
        if (new_fade != (uint32_t)v->env.c0) {
          v->env.c0    = (float)new_fade;
          v->env.delta = (new_fade > 0) ? (1.0f / (float)new_fade) : 0.0f;
        }
      }
      const float env_gain   = alo_env_tick(&v->env);
      const float total_gain = v->gain * env_gain * global_gain;

      out_l[pos] += l * total_gain;
      out_r[pos] += r * total_gain;

      v->phase_samples = alo_wrap_phase(ph + 1, loop_len);
      v->elapsed_samples++;

      if (--v->remaining_samples == 0) {
        v->active = false;
      }
    }
  }
}

static AloSliceVoice* alo_slice_sampler_start_voice(AloSliceSampler* s, const struct Alo* alo, uint32_t key,
                                   uint32_t start_delay_samples, uint32_t phase_samples,
                                   uint32_t length_samples, uint32_t fade_samples, float gain)
{
  if (!s)
    return NULL;
  AloSliceVoice* target = NULL;
  int mode = 0;
  if (alo && alo->ports.slice_play_mode) {
    mode = (int)lrintf(*(alo->ports.slice_play_mode));
  }
  if (mode == 2) {
    /* round-robin: pick next voice index (wrapping) */
    uint32_t idx = s->next_voice % ALO_SLICE_SAMPLER_MAX_VOICES;
    s->next_voice = idx + 1;
    target = &s->voices[idx];
  } else {
    /* polyphonic default behaviour */
    /* polyphonic-default behaviour: allocate unused voice first */
    for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
      if (!s->voices[i].active) {
        target = &s->voices[i];
        break;
      }
    }
    if (!target) {
      uint32_t idx = s->next_voice % ALO_SLICE_SAMPLER_MAX_VOICES;
      s->next_voice = idx + 1;
      target = &s->voices[idx];
    }
  }

  if (target) {
    target->active              = true;
    target->key                 = key;
    target->start_delay_samples = start_delay_samples;
    target->phase_samples       = phase_samples;
    target->remaining_samples   = length_samples;
    target->total_samples       = length_samples;
    target->elapsed_samples     = 0;
    target->fade_samples        = fade_samples;
    target->fade_inv            = (fade_samples > 0) ? (1.0f / (float)fade_samples) : 0.0f;
    target->gain                = gain;
    /* reset any previously assigned slice buffer pointers */
    target->slice_buf_l   = NULL;
    target->slice_buf_r   = NULL;
    target->slice_buf_len = 0;
    /* initialise envelope state. */
    target->env.running = true;
    target->env.stage   = ENV_ATTACK;
    target->env.phase   = 0.0f;
    target->env.frames  = 0;
    target->env.value   = 0.0f;
    uint32_t attack = alo_edge_fade_samples_u32(alo);
    target->env.c1           = (float)attack;
    target->env.c0           = (float)fade_samples;
    target->env.delta        = (fade_samples > 0) ? (1.0f / (float)fade_samples) : 0.0f;
    target->env.total_frames = length_samples;
  }
  return target;
}

/* Envelope tick: attack-sustain-release window using a cosine curve for
 * the attack and release segments.  The previous linear ramp produced
 * noticeable high‑frequency energy when fade lengths were very short; the
 * cosine window has a continuous derivative which helps eliminate subtle
 * crackle on quick fades.
 */
float alo_env_tick(AloEnvState* e)
{
  if (!e || !e->running)
    return 1.0f;

  float out;
  switch (e->stage) {
  case ENV_ATTACK:
    if (e->frames < (uint32_t)e->c1) {
      float t = (float)e->frames / (e->c1 > 0.0f ? e->c1 : 1.0f);
      out = 0.5f * (1.0f - cosf((float)M_PI * t));
      e->frames++;
      e->value = out;
    } else {
      /* move to sustain phase */
      e->stage  = ENV_SUSTAIN;
      e->frames = 0;
      e->value  = 1.0f;
      out       = 1.0f;
    }
    break;
  case ENV_SUSTAIN:
    /* stay at full level until release window is reached.  increment
     * the frame counter so that the transition eventually occurs; the
     * counter starts at zero when entering sustain.
     */
    if (e->frames >= (uint32_t)(e->total_frames - (uint32_t)e->c0)) {
      e->stage  = ENV_RELEASE;
      e->frames = 0;
    } else {
      e->frames++;
    }
    out = 1.0f;
    break;
  case ENV_RELEASE:
    if (e->frames < (uint32_t)e->c0) {
      float t = (float)e->frames / (e->c0 > 0.0f ? e->c0 : 1.0f);
      out = 0.5f * (1.0f + cosf((float)M_PI * t));
      e->frames++;
      e->value = out;
    } else {
      e->stage   = ENV_END;
      e->running = false;
      out        = 0.0f;
    }
    break;
  case ENV_END:
    out = 0.0f;
    break;
  default:
    out = e->value;
    break;
  }
  return out;
}

/* ------------------------------------------------------------------------
 * Memory & Initialization (Non-RT)
 * ------------------------------------------------------------------------ */

void alo_slice_sampler_free_buffers(AloSliceSampler* s)
{
  if (!s)
    return;
  for (uint32_t i = 0; i < ALO_SLICE_INFO_MAX; ++i) {
    s->slice_buffers[i].data         = NULL;
    s->slice_buffers[i].length       = 0;
    s->slice_buffers[i].valid        = false;
    s->slice_buffers_shadow[i].data  = NULL;
    s->slice_buffers_shadow[i].length = 0;
    s->slice_buffers_shadow[i].valid = false;
  }
  s->slice_buffers_using_primary = true;
  s->clear_in_progress           = false;
  s->clear_slice_idx             = 0;
  s->clear_offset                = 0;
}

bool alo_slice_sampler_alloc_buffers(AloSliceSampler* s, uint32_t max_len, uint32_t channels)
{
  if (!s || channels < 1 || channels > 2 || max_len == 0)
    return false;
  alo_slice_sampler_free_buffers(s);
  s->slice_buffer_channels = channels;
  s->slice_buffer_len      = max_len;
  for (uint32_t i = 0; i < ALO_SLICE_INFO_MAX; ++i) {
    alo_slice_buffers_reset(&s->slice_buffers[i]);
    alo_slice_buffers_reset(&s->slice_buffers_shadow[i]);
  }
  s->slice_buffers_using_primary = true;
  s->clear_in_progress           = false;
  s->clear_slice_idx             = 0;
  s->clear_offset                = 0;
  return true;
}

