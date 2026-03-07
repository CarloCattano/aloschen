#include "slice_sampler.h"
#include "alo_engine.h"
#include "alo_util.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------
 * Internal Helpers (Optimized for RT)
 * ------------------------------------------------------------------------ */

static inline bool alo_voice_is_active(const AloSliceVoice* v)
{
  return v && v->active && (v->remaining_samples > 0);
}

static inline uint32_t alo_wrap_phase(uint32_t phase, uint32_t limit)
{
  return (phase < limit) ? phase : (phase - limit);
}

static void alo_slice_sampler_start_voice(AloSliceSampler* s, const struct Alo* alo,
                                         uint32_t key, uint32_t start_delay_samples,
                                         uint32_t phase_samples, uint32_t length_samples,
                                         uint32_t fade_samples, float gain);
static float alo_env_tick(AloEnvState* e);

void alo_slice_sampler_reset(AloSliceSampler* s)
{
  if (!s)
    return;
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
    s->voices[i].active            = false;
    s->voices[i].remaining_samples = 0;
  }
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
    s->pending[i].active = false;
  }
}

void alo_slice_sampler_clear_buffers(AloSliceSampler* s)
{
  if (!s)
    return;
  /* mark everything invalid immediately; actual zeroing will happen over
   * subsequent audio callbacks via alo_slice_sampler_step_clear().
   */
  for (uint32_t i = 0; i < ALO_SLICE_INFO_MAX; ++i) {
    s->slice_buffers[i].valid        = false;
    s->slice_buffers_shadow[i].valid = false;
    s->slice_audio_valid[i]          = false;
    s->slice_audio_valid_shadow[i]   = false;
  }
  /* kick off incremental clearing state */
  s->clear_in_progress = true;
  s->clear_slice_idx   = 0;
  s->clear_offset      = 0;
}

void alo_slice_sampler_step_clear(AloSliceSampler* s, uint32_t max_samples)
{
  if (!s || !s->clear_in_progress || max_samples == 0)
    return;

  const uint32_t channels = s->slice_buffer_channels ? s->slice_buffer_channels : 1;
  while (max_samples > 0 && s->clear_slice_idx < ALO_SLICE_INFO_MAX) {
    AloSliceBuffer* sb =
        (s->slice_buffers_using_primary ? &s->slice_buffers[s->clear_slice_idx]
                                        : &s->slice_buffers_shadow[s->clear_slice_idx]);
    if (sb->data && sb->length > 0) {
      uint32_t total   = sb->length * channels;
      uint32_t remain  = total - s->clear_offset;
      uint32_t toclear = (remain < max_samples) ? remain : max_samples;
      memset(&sb->data[s->clear_offset], 0, sizeof(float) * toclear);
      s->clear_offset += toclear;
      max_samples -= toclear;
      if (s->clear_offset >= total) {
        s->clear_slice_idx++;
        s->clear_offset = 0;
      }
    } else {
      /* nothing to clear, skip ahead */
      s->clear_slice_idx++;
      s->clear_offset = 0;
    }
  }
  if (s->clear_slice_idx >= ALO_SLICE_INFO_MAX) {
    s->clear_in_progress = false;
  }
}

bool alo_slice_sampler_is_busy(const AloSliceSampler* s)
{
  if (!s)
    return false;
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
    if (alo_voice_is_active(&s->voices[i]))
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
  if (!s)
    return;
  // Find free pending slot
  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_PENDING; ++i) {
    if (!s->pending[i].active) {
      s->pending[i].active         = true;
      s->pending[i].key            = phase_samples; // Use phase as key for retrigger logic
      s->pending[i].offset_samples = start_offset_samples;
      s->pending[i].phase_samples  = phase_samples;
      s->pending[i].length_samples = length_samples;
      s->pending[i].fade_samples   = fade_samples;
      s->pending[i].gain           = gain;
      break;
    }
  }
}

/* ------------------------------------------------------------------------
 * DSP Logic
 * ------------------------------------------------------------------------ */

void alo_slice_sampler_process_chunk(AloSliceSampler* s, const struct Alo* alo,
                                     const uint32_t block_offset_samples, const uint32_t n_samples,
                                     float* out_l, float* out_r)
{
  if (!s || !alo || !out_l || !out_r || n_samples == 0 || alo->loop_samples == 0)
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

  const float    global_gain = alo->sampler_src_norm_gain;
  const uint32_t loop_len    = alo->loop_samples;

  /* choose active slice buffer set for voice assignment */
  AloSliceBuffer* buffers_active =
      (s->slice_buffers_using_primary ? s->slice_buffers : s->slice_buffers_shadow);

  for (uint32_t pos = 0; pos < n_samples; ++pos) {
    const uint32_t current_time = block_offset_samples + pos;

    for (uint32_t p_i = 0; p_i < ALO_SLICE_SAMPLER_MAX_PENDING; ++p_i) {
      AloSlicePending* p = &s->pending[p_i];
      if (p->active && p->offset_samples == current_time) {
        /* pass slice buffer pointers to voice for sample-accurate playback */
        const float* buf_l   = NULL;
        const float* buf_r   = NULL;
        uint32_t     buf_len = 0;
        /* determine which slice index corresponds to this phase */
        if (p->phase_samples < alo->loop_samples && s->slice_buffer_len > 0) {
          uint32_t slice_idx = p->phase_samples / s->slice_buffer_len;
          if (slice_idx < ALO_SLICE_INFO_MAX) {
            const AloSliceBuffer* sb = &buffers_active[slice_idx];
            if (sb->valid) {
              buf_len = sb->length;
              buf_l   = sb->data;
              if (s->slice_buffer_channels == 2)
                buf_r = sb->data + sb->length;
            }
          }
        }
        alo_slice_sampler_start_voice(s, alo, p->key, 0u, p->phase_samples, p->length_samples,
                                      p->fade_samples, p->gain);
        if (buf_l) {
          AloSliceVoice* v = NULL;
          /* last-started voice; loop through voices to find matching key */
          for (uint32_t vi = 0; vi < ALO_SLICE_SAMPLER_MAX_VOICES; ++vi) {
            if (s->voices[vi].active && s->voices[vi].key == p->key) {
              v = &s->voices[vi];
              break;
            }
          }
          if (v) {
            v->slice_buf_l   = buf_l;
            v->slice_buf_r   = buf_r;
            v->slice_buf_len = buf_len;
          }
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

static void alo_slice_sampler_start_voice(AloSliceSampler* s, const struct Alo* alo, uint32_t key,
                                   uint32_t start_delay_samples, uint32_t phase_samples,
                                   uint32_t length_samples, uint32_t fade_samples, float gain)
{
  if (!s)
    return;
  AloSliceVoice* target = NULL;

  for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
    if (s->voices[i].active && s->voices[i].key == key) {
      target = &s->voices[i];
      break;
    }
    if (!target && !s->voices[i].active) {
      target = &s->voices[i];
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
    /* initialise envelope state for simple fade-in/out using fade_samples */
    target->env.running = true;
    target->env.stage   = ENV_ATTACK;
    target->env.phase   = 0.0f;
    target->env.frames  = 0;
    target->env.value   = 0.0f;
    /* use c1 to store attack_frames, c0 to store release_frames */
    target->env.c1           = (float)fade_samples;
    target->env.c0           = (float)fade_samples;
    target->env.delta        = (fade_samples > 0) ? (1.0f / (float)fade_samples) : 0.0f;
    target->env.total_frames = length_samples;
  }
}

/* Envelope tick: simple linear attack-sustain-release over the voice's
 * lifetime.  This is a thin wrapper used by process_chunk to provide smoother
 * start/end ramps compared to the previous ad‑hoc fading helpers.
 */
static float alo_env_tick(AloEnvState* e)
{
  if (!e || !e->running)
    return 1.0f;

  float out;
  switch (e->stage) {
  case ENV_ATTACK:
    if (e->frames < (uint32_t)e->c1) {
      out = e->frames * e->delta;
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
    /* stay at full level until we are within release window */
    if (e->frames >= (uint32_t)(e->total_frames - (uint32_t)e->c0)) {
      e->stage  = ENV_RELEASE;
      e->frames = 0;
    }
    out = 1.0f;
    break;
  case ENV_RELEASE:
    if (e->frames < (uint32_t)e->c0) {
      out = (1.0f - ((float)e->frames * e->delta));
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
    if (s->slice_buffers[i].data) {
      free(s->slice_buffers[i].data);
      s->slice_buffers[i].data = NULL;
    }
    s->slice_buffers[i].length = 0;
    s->slice_buffers[i].valid  = false;
    if (s->slice_buffers_shadow[i].data) {
      free(s->slice_buffers_shadow[i].data);
      s->slice_buffers_shadow[i].data = NULL;
    }
    s->slice_buffers_shadow[i].length = 0;
    s->slice_buffers_shadow[i].valid  = false;
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
  /* allocate both primary and shadow sets */
  for (uint32_t i = 0; i < ALO_SLICE_INFO_MAX; ++i) {
    s->slice_buffers[i].data = (float*)calloc(max_len * channels, sizeof(float));
    if (!s->slice_buffers[i].data)
      return false;
    s->slice_buffers[i].length = max_len;
    s->slice_buffers[i].valid  = false;

    s->slice_buffers_shadow[i].data = (float*)calloc(max_len * channels, sizeof(float));
    if (!s->slice_buffers_shadow[i].data)
      return false;
    s->slice_buffers_shadow[i].length = max_len;
    s->slice_buffers_shadow[i].valid  = false;
  }
  s->slice_buffers_using_primary = true;
  s->clear_in_progress           = false;
  s->clear_slice_idx             = 0;
  s->clear_offset                = 0;
  return true;
}

