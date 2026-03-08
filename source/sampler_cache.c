#include "sampler_cache.h"
#include "alo_util.h"
#include <math.h>
#include <stddef.h>

/* update full-loop stereo mix cache for current block */
void sampler_cache_process(Alo* self, uint32_t n_samples, bool any_committed_audio)
{
  if (!self)
    return;

  if (!any_committed_audio) {
    self->sampler_src_valid          = false;
    self->sampler_src_rebuild_active = false;
    self->sampler_src_pos            = 0u;
    self->sampler_src_peak_abs       = 0.0f;
    self->sampler_src_norm_gain      = 1.0f;
    self->sampler_src_loop_samples   = 0u;
    return;
  }

  if (self->sampler_src_dirty) {
    /* start rebuild into shadow */
    self->sampler_src_shadow_valid   = false;
    self->sampler_src_rebuild_active = true;
    self->sampler_src_pos            = 0u;
    self->sampler_src_peak_abs       = 0.0f;
    self->sampler_src_norm_gain      = 1.0f;
    self->sampler_src_loop_samples   = self->loop_samples;

    /* invalidate shadow slice descriptors; they will be rebound to borrowed
     * regions inside sampler_src_buf_shadow once the rebuild completes.
     */
    const uint32_t slice_count = alo_get_slice_count(self);
    for (uint32_t si = 0; si < slice_count && si < ALO_SLICE_INFO_MAX; ++si) {
      self->slice_sampler.slice_buffers_shadow[si].valid    = false;
      self->slice_sampler.slice_buffers_shadow[si].data     = NULL;
      self->slice_sampler.slice_buffers_shadow[si].length   = 0u;
      self->slice_sampler.slice_buffers_shadow[si].borrowed = true;
    }
    self->sampler_src_dirty = false;
  }

  if (self->sampler_src_rebuild_active && self->sampler_src_buf_shadow && self->loop_samples) {
    const uint32_t slice_count = alo_get_slice_count(self);
    const uint32_t slice_len   = (slice_count > 0) ? (self->loop_samples / slice_count) : 0u;

    uint32_t       cap_pos = self->sampler_src_pos;
    const uint32_t cap_end = self->loop_samples;
    uint32_t       cap_max = n_samples;
    {
      const uint32_t kMaxPerRun = 4096u; /* cap work per call */
      uint64_t       scaled     = (uint64_t)n_samples * 8u;
      if (scaled < (uint64_t)cap_max)
        scaled = (uint64_t)cap_max;
      if (scaled > (uint64_t)kMaxPerRun)
        scaled = (uint64_t)kMaxPerRun;
      cap_max = (uint32_t)scaled;
      if (cap_max == 0u)
        cap_max = 1u;
    }
    while (cap_pos < cap_end && cap_max--) {
      const uint32_t idx   = self->loop_start + cap_pos;
      const uint32_t idx_r = idx + LOOP_SIZE;

      float ml = 0.0f;
      float mr = 0.0f;
      for (int t = 0; t < NUM_TRACKS; ++t) {
        if (!self->have_loop[t] || !self->loop_buf[t])
          continue;
        if (self->track_state[t] == TRACK_REC_BASE)
          continue;
        ml += self->loop_buf[t][idx];
        mr += self->loop_buf[t][idx_r];
        uint8_t n_layers = self->od_count[t];
        if (self->track_state[t] == TRACK_REC_OVERDUB) {
          const uint8_t rec_layer = self->rec_od_layer[t];
          if (rec_layer < n_layers)
            n_layers = rec_layer;
        }
        for (uint8_t l = 0; l < n_layers; ++l) {
          const float* const buf = self->od_buf[t][l];
          if (!buf)
            continue;
          ml += buf[idx];
          mr += buf[idx_r];
        }
      }

      self->sampler_src_buf_shadow[cap_pos]             = ml;
      self->sampler_src_buf_shadow[cap_pos + LOOP_SIZE] = mr;
      const float a_l = fabsf(ml);
      const float a_r = fabsf(mr);
      const float a   = (a_l > a_r) ? a_l : a_r;
      if (a > self->sampler_src_peak_abs)
        self->sampler_src_peak_abs = a;

      cap_pos++;
    }
    self->sampler_src_pos = cap_pos;
    if (self->sampler_src_pos >= self->loop_samples) {
      float* tmp_buf               = self->sampler_src_buf;
      self->sampler_src_buf        = self->sampler_src_buf_shadow;
      self->sampler_src_buf_shadow = tmp_buf;

      self->sampler_src_rebuild_active = false;
      self->sampler_src_valid          = true;
      self->sampler_src_shadow_valid   = true;
      self->sampler_src_loop_samples   = self->loop_samples;
      if (self->sampler_src_peak_abs > 1.0f)
        self->sampler_src_norm_gain = 1.0f / self->sampler_src_peak_abs;
      else
        self->sampler_src_norm_gain = 1.0f;

      if (slice_count > 0 && slice_len > 0) {
        for (uint32_t si = 0; si < slice_count && si < ALO_SLICE_INFO_MAX; ++si) {
          const uint32_t s0 = si * slice_len;
          uint32_t       s1 = s0 + slice_len;
          if (si + 1u == slice_count || s1 > self->loop_samples) {
            s1 = self->loop_samples;
          }

          AloSliceBuffer* sb = &self->slice_sampler.slice_buffers[si];
          sb->data           = self->sampler_src_buf + s0;
          sb->length         = (s1 > s0) ? (s1 - s0) : 0u;
          sb->valid          = (sb->length > 0u);
          sb->borrowed       = true;
        }
        for (uint32_t si = slice_count; si < ALO_SLICE_INFO_MAX; ++si) {
          self->slice_sampler.slice_buffers[si].data     = NULL;
          self->slice_sampler.slice_buffers[si].length   = 0u;
          self->slice_sampler.slice_buffers[si].valid    = false;
          self->slice_sampler.slice_buffers[si].borrowed = true;
        }
        self->slice_sampler.slice_buffers_using_primary = true;
      }
    }
  }
}
