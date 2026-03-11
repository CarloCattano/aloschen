#include "sampler_cache.h"
#include "alo_util.h"
#include "transient_detector.h"


#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* Local constants                                                            */
/* ------------------------------------------------------------------------- */

#define SC_MAX_PER_RUN 4096u
#define SC_SCALE_FACTOR 8u
#define SC_MIN_SLICE_MS 80.0
#define SC_MIN_SLICE_PEAK_RATIO 0.035f
#define SC_MIN_SLICE_AVG_RATIO 0.012f

/* tests are built without the main plugin code; provide a simple stub so
   references to alo_log resolve. The real implementation lives in
   aloschen.c and writes to /tmp/alo.log when ALO_LOG is enabled. */
void alo_log(const char* message, ...) __attribute__((weak));
void alo_log(const char* message, ...)
{
  (void)message;
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct
{
  uint32_t offset;
  float    strength;
} ScSliceMarker;

static int cmp_marker_offset(const void* a, const void* b)
{
  const ScSliceMarker* const ma = (const ScSliceMarker*)a;
  const ScSliceMarker* const mb = (const ScSliceMarker*)b;
  if (ma->offset < mb->offset)
    return -1;
  if (ma->offset > mb->offset)
    return 1;
  return 0;
}

static float sc_absf(float x)
{
  return (x < 0.0f) ? -x : x;
}

static uint32_t sc_compute_min_slice_len(const Alo* self)
{
  uint32_t min_len_ms = UINT32_MAX;
  uint32_t frac_len   = UINT32_MAX;

  if (self && self->rate > 1e-6) {
    min_len_ms = (uint32_t)((double)self->rate * (SC_MIN_SLICE_MS * 0.001));
  }
  if (self && self->loop_samples > 0u) {
    frac_len = (self->loop_samples + 15u) / 16u;
  }

  if (min_len_ms == UINT32_MAX && frac_len == UINT32_MAX) {
    return 1u;
  }
  if (min_len_ms == UINT32_MAX) {
    return frac_len ? frac_len : 1u;
  }
  if (frac_len == UINT32_MAX) {
    return min_len_ms ? min_len_ms : 1u;
  }

  return (min_len_ms < frac_len ? min_len_ms : frac_len)
             ? (min_len_ms < frac_len ? min_len_ms : frac_len)
             : 1u;
}

static void sc_seed_detected_offsets(Alo* self)
{
  if (!self) {
    return;
  }

  self->detected_slices_count      = 1u;
  self->detected_slice_offsets[0]  = 0u;
  self->detected_slice_strength[0] = 0.0f;
}

static uint32_t sc_last_detected_offset(const Alo* self)
{
  if (!self || self->detected_slices_count == 0u) {
    return 0u;
  }
  return self->detected_slice_offsets[self->detected_slices_count - 1u];
}

static void update_detected_slices_port(Alo* self, uint32_t count);

static void sc_sort_detected_markers(Alo* self)
{
  if (!self || self->detected_slices_count < 2u) {
    return;
  }

  ScSliceMarker markers[ALO_SLICE_SAMPLER_MAX_VOICES];
  uint32_t      count = self->detected_slices_count;
  if (count > ALO_SLICE_SAMPLER_MAX_VOICES) {
    count = ALO_SLICE_SAMPLER_MAX_VOICES;
  }

  for (uint32_t i = 0u; i < count; ++i) {
    markers[i].offset   = self->detected_slice_offsets[i];
    markers[i].strength = self->detected_slice_strength[i];
  }

  qsort(markers, count, sizeof(markers[0]), cmp_marker_offset);

  for (uint32_t i = 0u; i < count; ++i) {
    self->detected_slice_offsets[i]  = markers[i].offset;
    self->detected_slice_strength[i] = markers[i].strength;
  }
}

static bool sc_slice_region_has_content(const Alo* self, const float* buf_l, const float* buf_r,
                                        uint32_t start, uint32_t end)
{
  if (!self || !buf_l || start >= end || end > self->loop_samples) {
    return false;
  }

  const uint32_t len = end - start;
  if (len == 0u) {
    return false;
  }

  float peak_abs = 0.0f;
  float sum_abs  = 0.0f;

  for (uint32_t i = start; i < end; ++i) {
    const float l = sc_absf(buf_l[i]);
    const float r = buf_r ? sc_absf(buf_r[i]) : l;
    const float a = (l > r) ? l : r;
    if (a > peak_abs) {
      peak_abs = a;
    }
    sum_abs += a;
  }

  const float avg_abs   = sum_abs / (float)len;
  const float loop_peak = (self->sampler_src_peak_abs > 1.0e-9f) ? self->sampler_src_peak_abs : 1.0f;

  if (peak_abs < (loop_peak * SC_MIN_SLICE_PEAK_RATIO)) {
    return false;
  }
  if (avg_abs < (loop_peak * SC_MIN_SLICE_AVG_RATIO)) {
    return false;
  }

  return true;
}

static bool sc_try_accept_detected_offset(Alo* self, uint32_t candidate_offset, float strength,
                                          const float* buf_l, const float* buf_r)
{
  if (!self) {
    return false;
  }

  const uint32_t min_len  = sc_compute_min_slice_len(self);
  const uint32_t last_off = sc_last_detected_offset(self);

  if (candidate_offset <= last_off) {
    return false;
  }
  if ((candidate_offset - last_off) < min_len) {
    return false;
  }
  if (!sc_slice_region_has_content(self, buf_l, buf_r, last_off, candidate_offset)) {
    return false;
  }

  if (self->detected_slices_count < ALO_SLICE_SAMPLER_MAX_VOICES) {
    const uint32_t ins                 = self->detected_slices_count;
    self->detected_slice_offsets[ins]  = candidate_offset;
    self->detected_slice_strength[ins] = strength;
    self->detected_slices_count++;
    sc_sort_detected_markers(self);
    update_detected_slices_port(self, self->detected_slices_count);
    return true;
  }

  {
    int   weakest          = 0;
    float weakest_strength = self->detected_slice_strength[0];
    for (uint32_t wi = 1u; wi < self->detected_slices_count; ++wi) {
      if (self->detected_slice_strength[wi] < weakest_strength) {
        weakest_strength = self->detected_slice_strength[wi];
        weakest          = (int)wi;
      }
    }

    if (strength > weakest_strength) {
      self->detected_slice_offsets[weakest]  = candidate_offset;
      self->detected_slice_strength[weakest] = strength;
      sc_sort_detected_markers(self);
      update_detected_slices_port(self, self->detected_slices_count);
      return true;
    }
  }

  return false;
}

static void sc_finalize_detected_slices(Alo* self, const float* buf_l, const float* buf_r)
{
  if (!self) {
    return;
  }

  const uint32_t min_len = sc_compute_min_slice_len(self);

  sc_sort_detected_markers(self);

  if (self->detected_slices_count > 1u) {
    const uint32_t last_off = sc_last_detected_offset(self);
    if (self->loop_samples > last_off &&
        ((self->loop_samples - last_off) < min_len ||
         !sc_slice_region_has_content(self, buf_l, buf_r, last_off, self->loop_samples))) {
      self->detected_slices_count--;
    }
  }

  if (self->detected_slices_count == 0u) {
    sc_seed_detected_offsets(self);
  }

  update_detected_slices_port(self, self->detected_slices_count);
}

static void update_detected_slices_port(Alo* self, uint32_t count)
{
  if (!self) {
    return;
  }

  if (count > ALO_SLICE_SAMPLER_MAX_VOICES) {
    count = ALO_SLICE_SAMPLER_MAX_VOICES;
  }

  self->detected_slices_count = count;
  alo_port_write(self->ports.detected_slices_out, (float)count);
}

static void update_slice_buffers_internal(Alo* self)
{
  if (!self) {
    return;
  }

  const bool use_transient = alo_get_use_transient_slices_b(self);
  uint32_t slice_count =
      use_transient ? self->detected_slices_count : alo_get_slice_count_u(self);

  if (slice_count > ALO_SLICE_INFO_MAX) {
    slice_count = ALO_SLICE_INFO_MAX;
  }

  for (uint32_t i = 0; i < ALO_SLICE_INFO_MAX; ++i) {
    AloSliceBuffer* p = &self->slice_sampler.slice_buffers[i];
    AloSliceBuffer* s = &self->slice_sampler.slice_buffers_shadow[i];
    p->valid          = false;
    s->valid          = false;
    p->data           = NULL;
    s->data           = NULL;
    p->length         = 0u;
    s->length         = 0u;
    p->borrowed       = true;
    s->borrowed       = true;
  }

  if (use_transient) {
    for (uint32_t i = 0; i < slice_count; ++i) {
      const uint32_t start = self->detected_slice_offsets[i];
      const uint32_t end =
          (i + 1u < slice_count) ? self->detected_slice_offsets[i + 1u] : self->loop_samples;

      if (end <= start) {
        continue;
      }

      {
        const AloSliceBuffer buf                    = {.valid    = true,
                                                       .data     = &self->sampler_src_buf[start],
                                                       .length   = end - start,
                                                       .borrowed = true};
        self->slice_sampler.slice_buffers[i]        = buf;
        self->slice_sampler.slice_buffers_shadow[i] = buf;
      }
    }
  } else {
    const uint64_t loop_s = (uint64_t)self->loop_samples;
    for (uint32_t i = 0; i < slice_count; ++i) {
      const uint64_t s0 = ((uint64_t)i * loop_s) / slice_count;
      const uint64_t s1 = ((uint64_t)(i + 1u) * loop_s) / slice_count;

      if (s1 <= s0) {
        continue;
      }

      {
        const AloSliceBuffer buf                    = {.valid    = true,
                                                       .data     = &self->sampler_src_buf[(uint32_t)s0],
                                                       .length   = (uint32_t)(s1 - s0),
                                                       .borrowed = true};
        self->slice_sampler.slice_buffers[i]        = buf;
        self->slice_sampler.slice_buffers_shadow[i] = buf;
      }
    }
  }

  self->slice_sampler.slice_buffers_using_primary = true;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

void sampler_cache_update_slice_buffers(Alo* self)
{
  if (!self) {
    return;
  }
  if (!self->sampler_src_valid || self->loop_samples == 0u) {
    return;
  }
  update_slice_buffers_internal(self);
}

void sampler_cache_process(Alo* self, uint32_t n_samples, bool any_committed_audio)
{
  if (!self) {
    return;
  }

  if (!any_committed_audio) {
    self->sampler_src_valid          = false;
    self->sampler_src_rebuild_active = false;
    self->sampler_src_pos            = 0u;
    self->sampler_src_peak_abs       = 0.0f;
    self->sampler_src_norm_gain      = 1.0f;
    self->sampler_src_loop_samples   = 0u;
    self->detect_active              = false;
    return;
  }

  /* propagate threshold/split/sensitivity changes into the detector state
     without forcing an audio-cache rebuild. */
  {
    bool sensitivity_changed = false;
    bool threshold_changed   = false;
    bool split_changed       = false;
    bool transient_mode      = false;

    if (self->ports.slice_sens) {
      const float cur = *(self->ports.slice_sens);
      if (cur != self->cached_sens) {
        self->cached_sens   = cur;
        sensitivity_changed = true;
      }
    }

    if (self->ports.transient_threshold) {
      const float cur = *(self->ports.transient_threshold);
      if (cur != self->cached_threshold) {
        self->cached_threshold = cur;
        threshold_changed      = true;
      }
    }

    if (self->ports.split_by_transient) {
      const bool mode = (*(self->ports.split_by_transient) > 0.5f);
      if (mode != self->cached_split_mode) {
        self->cached_split_mode = mode;
        split_changed           = true;
      }
    }

    transient_mode = alo_get_use_transient_slices_b(self);

    if (!transient_mode) {
      const uint32_t slice_count = alo_get_slice_count(self);
      update_detected_slices_port(self, slice_count);
      self->detect_active = false;
    } else if (sensitivity_changed || threshold_changed || split_changed ||
               (self->sampler_src_rebuild_active && self->sampler_src_pos == 0u)) {
      self->detect_active = true;
      self->detect_pos    = 0u;
      sc_seed_detected_offsets(self);
      update_detected_slices_port(self, self->detected_slices_count);
    }
  }

  if (self->sampler_src_dirty) {
    self->sampler_src_shadow_valid   = false;
    self->sampler_src_rebuild_active = true;
    self->sampler_src_pos            = 0u;
    self->sampler_src_peak_abs       = 0.0f;
    self->sampler_src_norm_gain      = 1.0f;
    self->sampler_src_loop_samples   = self->loop_samples;

    if (alo_get_use_transient_slices_b(self)) {
      self->detect_active = true;
      self->detect_pos    = 0u;
      if (self->detected_slices_count == 0u) {
        sc_seed_detected_offsets(self);
      }
    } else {
      const uint32_t slice_count  = alo_get_slice_count(self);
      self->detected_slices_count = slice_count;
      alo_port_write(self->ports.detected_slices_out, (float)slice_count);
      self->detect_active = false;
    }

    {
      const uint32_t slice_count = alo_get_slice_count(self);
      for (uint32_t si = 0; si < slice_count && si < ALO_SLICE_INFO_MAX; ++si) {
        self->slice_sampler.slice_buffers_shadow[si].valid    = false;
        self->slice_sampler.slice_buffers_shadow[si].data     = NULL;
        self->slice_sampler.slice_buffers_shadow[si].length   = 0u;
        self->slice_sampler.slice_buffers_shadow[si].borrowed = true;
      }
    }

    self->sampler_src_dirty = false;
  }

  if (self->sampler_src_rebuild_active && self->sampler_src_buf_shadow && self->loop_samples) {
    uint32_t       cap_pos = self->sampler_src_pos;
    const uint32_t cap_end = self->loop_samples;
    uint32_t       cap_max = n_samples;

    {
      uint64_t scaled = (uint64_t)n_samples * SC_SCALE_FACTOR;
      if (scaled < (uint64_t)cap_max) {
        scaled = (uint64_t)cap_max;
      }
      if (scaled > (uint64_t)SC_MAX_PER_RUN) {
        scaled = (uint64_t)SC_MAX_PER_RUN;
      }
      cap_max = (uint32_t)scaled;
      if (cap_max == 0u) {
        cap_max = 1u;
      }
    }

    while (cap_pos < cap_end && cap_max--) {
      const uint32_t idx   = self->loop_start + cap_pos;
      const uint32_t idx_r = idx + LOOP_SIZE;

      float ml = 0.0f;
      float mr = 0.0f;

      for (int t = 0; t < NUM_TRACKS; ++t) {
        if (!self->have_loop[t] || !self->loop_buf[t]) {
          continue;
        }
        if (self->track_state[t] == TRACK_REC_BASE) {
          continue;
        }

        ml += self->loop_buf[t][idx];
        mr += self->loop_buf[t][idx_r];

        {
          uint8_t n_layers = self->od_count[t];
          if (self->track_state[t] == TRACK_REC_OVERDUB) {
            const uint8_t rec_layer = self->rec_od_layer[t];
            if (rec_layer < n_layers) {
              n_layers = rec_layer;
            }
          }

          for (uint8_t l = 0; l < n_layers; ++l) {
            const float* const buf = self->od_buf[t][l];
            if (!buf) {
              continue;
            }
            ml += buf[idx];
            mr += buf[idx_r];
          }
        }
      }

      self->sampler_src_buf_shadow[cap_pos]             = ml;
      self->sampler_src_buf_shadow[cap_pos + LOOP_SIZE] = mr;

      {
        const float a_l = sc_absf(ml);
        const float a_r = sc_absf(mr);
        const float a   = (a_l > a_r) ? a_l : a_r;
        if (a > self->sampler_src_peak_abs) {
          self->sampler_src_peak_abs = a;
        }
      }

      /* Build transient slices chronologically, but only accept boundaries that
         create musically useful regions with enough duration and content. */
      if (self->detect_active) {
        if (self->detect_pos == 0u) {
          float thr;
          if (self->ports.transient_threshold) {
            thr = *(self->ports.transient_threshold);
          } else {
            thr = alo_sensitivity_to_threshold(self);
          }
          td_init(&self->detect_td, (float)self->rate, thr, TD_DEFAULT_DEBOUNCE_MS);
        }

        if (td_process_sample(&self->detect_td, ml)) {
          if (self->detect_pos > 0u) {
            const float strength = sc_absf(ml);
            if (!sc_try_accept_detected_offset(self, self->detect_pos, strength,
                                               self->sampler_src_buf_shadow,
                                               self->sampler_src_buf_shadow + LOOP_SIZE)) {
              /* restart detector so the same onset cluster
                 doesn't keep refiring. */
              float thr2;
              if (self->ports.transient_threshold) {
                thr2 = *(self->ports.transient_threshold);
              } else {
                thr2 = alo_sensitivity_to_threshold(self);
              }
              td_init(&self->detect_td, (float)self->rate, thr2, TD_DEFAULT_DEBOUNCE_MS);
            }
          }
        }

        self->detect_pos++;

        if (self->detect_pos >= self->loop_samples) {
          sc_finalize_detected_slices(self, self->sampler_src_buf_shadow,
                                      self->sampler_src_buf_shadow + LOOP_SIZE);
          self->detect_active = false;
        } else {
          update_detected_slices_port(self, self->detected_slices_count);
        }
      }

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
      self->sampler_src_norm_gain =
          (self->sampler_src_peak_abs > 1.0f) ? (1.0f / self->sampler_src_peak_abs) : 1.0f;

      if (self->detect_active && self->detect_pos < self->loop_samples) {
        const uint32_t remaining = self->loop_samples - self->detect_pos;
        sampler_cache_process(self, remaining, any_committed_audio);
      }
    }
  }

  /* Continue scanning the already-built buffer when only controls changed. */
  if (!self->sampler_src_rebuild_active && self->detect_active && self->sampler_src_valid &&
      self->loop_samples > 0u) {

    uint32_t     cap_end = self->loop_samples;
    uint32_t     cap_pos = self->detect_pos;
    uint32_t     cap_max = n_samples * 8u;
    const float* buf     = self->sampler_src_buf;

    if (cap_max < 1u) {
      cap_max = 1u;
    }

    while (cap_pos < cap_end && cap_max--) {
      if (cap_pos == 0u) {
        float thr;
        if (self->ports.transient_threshold) {
          thr = *(self->ports.transient_threshold);
        } else {
          thr = alo_sensitivity_to_threshold(self);
        }
        td_init(&self->detect_td, (float)self->rate, thr, TD_DEFAULT_DEBOUNCE_MS);
        sc_seed_detected_offsets(self);
      }

      {
        const float s = buf[cap_pos];
        if (td_process_sample(&self->detect_td, s)) {
          if (cap_pos > 0u) {
            const float strength = sc_absf(s);
            sc_try_accept_detected_offset(self, cap_pos, strength, buf, buf + LOOP_SIZE);
          }
        }
      }

      cap_pos++;
      self->detect_pos = cap_pos;
    }

    if (cap_pos >= cap_end) {
      sc_finalize_detected_slices(self, buf, buf + LOOP_SIZE);
      self->detect_active = false;
    }
  }
}