#include "sampler_cache.h"
#include "alo_util.h"
#include "transient_detector.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h> /* qsort */
#include <stdio.h>  /* for debug logging */

/* comparator for qsort when sorting 32-bit unsigned integers */
static int cmp_u32(const void *a, const void *b)
{
    uint32_t va = *(const uint32_t*)a;
    uint32_t vb = *(const uint32_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

/* local constants */
#define SC_MAX_PER_RUN 4096u          /* throttle amount for rebuild work */
#define SC_SCALE_FACTOR 8u            /* multiplier used when scaling cap_max */

/* tests are built without the main plugin code; provide a simple stub so
   references to alo_log resolve.  The real implementation lives in
   aloschen.c and writes to /tmp/alo.log when ALO_LOG is enabled.  Declare
   the stub as weak so it can safely coexist with the real (strong) symbol. */
void alo_log(const char* message, ...) __attribute__((weak));
void alo_log(const char* message, ...) { (void)message; }

/* update full-loop stereo mix cache for current block */

/* Forward declare helper used by public wrapper. */
static void update_slice_buffers_internal(Alo* self);

/* Public wrapper exposed in sampler_cache.h.  See header comment for details. */
void sampler_cache_update_slice_buffers(Alo* self)
{
    if (!self)
        return;
    /* Only update when we have valid audio to borrow from. */
    if (!self->sampler_src_valid || self->loop_samples == 0u)
        return;
    update_slice_buffers_internal(self);
}

/* Internal implementation that actually walks the current slice definition
 * and writes borrowed pointers/lengths into both the primary and shadow
 * slice-buffer arrays.  Populating both sets makes it safe to call the
 * helper regardless of which buffer set is currently active; the sampler
 * will always read from the active set chosen in process_chunk.
 */
static void update_slice_buffers_internal(Alo* self)
{
    /* compute number of slices under the current mode */
    uint32_t slice_count = alo_get_slice_count_u(self);
    if (slice_count > ALO_SLICE_INFO_MAX)
        slice_count = ALO_SLICE_INFO_MAX;

    /* helper to clear both sets */
    for (uint32_t i = 0; i < ALO_SLICE_INFO_MAX; ++i) {
        AloSliceBuffer *p = &self->slice_sampler.slice_buffers[i];
        AloSliceBuffer *s = &self->slice_sampler.slice_buffers_shadow[i];
        p->valid = s->valid = false;
        p->data  = s->data  = NULL;
        p->length = s->length = 0u;
        p->borrowed = s->borrowed = true;
    }

    /* populate according to mode */
    if (alo_get_use_transient_slices_b(self)) {
        for (uint32_t i = 0; i < slice_count; ++i) {
            uint32_t start = self->detected_slice_offsets[i];
            uint32_t end = (i + 1 < slice_count)
                               ? self->detected_slice_offsets[i + 1]
                               : self->loop_samples;
            if (end <= start)
                continue;
            uint32_t len = end - start;
            AloSliceBuffer buf = {
                .valid = true,
                .data = &self->sampler_src_buf[start],
                .length = len,
                .borrowed = true
            };
            self->slice_sampler.slice_buffers[i] = buf;
            self->slice_sampler.slice_buffers_shadow[i] = buf;
        }
    } else {
        uint64_t loop_s = (uint64_t)self->loop_samples;
        for (uint32_t i = 0; i < slice_count; ++i) {
            uint64_t s0 = ((uint64_t)i * loop_s) / slice_count;
            uint64_t s1 = ((uint64_t)(i + 1) * loop_s) / slice_count;
            if (s1 <= s0)
                continue;
            uint32_t len = (uint32_t)(s1 - s0);
            AloSliceBuffer buf = {
                .valid = true,
                .data = &self->sampler_src_buf[(uint32_t)s0],
                .length = len,
                .borrowed = true
            };
            self->slice_sampler.slice_buffers[i] = buf;
            self->slice_sampler.slice_buffers_shadow[i] = buf;
        }
    }

    /* make sure the primary set is marked active so that immediately
       scheduled voices see the new mapping */
    self->slice_sampler.slice_buffers_using_primary = true;
}

/* Helper to update the slice count port, applying any user-specified cap.
   The DSP writes to the port via alo_port_write so that hosts which
   propagate output-control values will generate port_event notifications
   for the UI. */
static void update_detected_slices_port(Alo* self, uint32_t count)
{
    float thr = NAN;
    if (self->ports.transient_threshold) {
        thr = *(self->ports.transient_threshold);
    }
    alo_log("DEBUG: update_detected_slices_port called with count=%u thr=%f", count, thr);
    /* enforce minimum 4 slices per bar when transient mode is active */
    if (alo_get_use_transient_slices_b(self)) {
        uint32_t bars = alo_get_bars_i(self);
        uint32_t minreq = bars * ALO_MIN_SLICES_PER_BAR;
        if (minreq < ALO_MIN_SLICES_PER_BAR) minreq = ALO_MIN_SLICES_PER_BAR; /* always at least floor */
        if (count < minreq) {
            /* override offsets with uniform grid of size minreq */
            for (uint32_t i = 0; i < minreq && i < ALO_SLICE_INFO_MAX; ++i) {
                self->detected_slice_offsets[i] =
                    (uint32_t)(((uint64_t)i * self->loop_samples) / minreq);
            }
            count = minreq;
        }
    }
    self->detected_slices_count = count;
    alo_port_write(self->ports.detected_slices_out, (float)count);
    /* also emit normalized offsets for UI drawing */
    if (self->loop_samples > 0) {
        for (uint32_t i = 0; i < ALO_SLICE_SAMPLER_MAX_VOICES; ++i) {
            float v = 0.0f;
            if (i < count) {
                v = (float)self->detected_slice_offsets[i] /
                    (float)self->loop_samples;
            }
            if (self->ports.slice_offset[i]) {
                alo_port_write(self->ports.slice_offset[i], v);
            }
        }
    }
}

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

  /* propagate threshold/split changes into the incremental detector state
     but do not mark the sampler dirty – the audio output itself does not
     depend on these controls.  A rebuild is only required when the committed
     loop content changes. */
  bool sensitivity_changed = false;
  if (self->ports.slice_sens) {
    float cur = *(self->ports.slice_sens);
    if (cur != self->cached_sens) {
      self->cached_sens = cur;
      sensitivity_changed = true;
    }
  }
  bool threshold_changed = false;
  if (self->ports.transient_threshold) {
    float cur = *(self->ports.transient_threshold);
    if (cur != self->cached_threshold) {
      self->cached_threshold = cur;
      threshold_changed = true;
      /* report current count immediately so the UI doesn't lag */
      update_detected_slices_port(self, self->detected_slices_count);
    }
  }
  bool split_changed = false;
  if (self->ports.split_by_transient) {
    bool mode = (*(self->ports.split_by_transient) > 0.5f);
    if (mode != self->cached_split_mode) {
      self->cached_split_mode = mode;
      split_changed = true;
    }
  }

  /* compute absolute minimum slice length: 30 ms or 1/16 loop, whichever is
     **smaller**.  Intervals shorter than this are assumed to be spurious and
     skipped. */
  uint32_t min_len30 = UINT32_MAX;
  uint32_t frac_len  = UINT32_MAX;
  if (self->rate > 1e-6) {
    min_len30 = (uint32_t)((double)self->rate * 0.030);
  }
  if (self->loop_samples > 0) {
    frac_len = (self->loop_samples + 15u) / 16u;
  }
  uint32_t min_len = min_len30 < frac_len ? min_len30 : frac_len;
  if (min_len == UINT32_MAX) {
    min_len = 1u;
  }

  /* If transient slicing is *disabled* we can short-circuit the detector and
     simply report the slice count; cancel any in-progress scan. */
  if (!alo_get_use_transient_slices_b(self)) {
    uint32_t slice_count = alo_get_slice_count(self);
    update_detected_slices_port(self, slice_count);
    self->detect_active = false;
  } else if (sensitivity_changed || threshold_changed || split_changed ||
             (self->sampler_src_rebuild_active && self->sampler_src_pos == 0u)) {
    /* restart the scan when in split mode (threshold or mode change or new
       loop data).  Reset the detection state but keep any existing offset
       seed so that we don't temporarily drop to zero slices. */
    self->detect_active = true;
    self->detect_pos    = 0u;
    /* start with offset 0 always present */
    self->detected_slices_count = 1u;
    self->detected_slice_offsets[0] = 0u;
    self->detected_slice_strength[0] = 0.0f;
    /* report the current count immediately (this may be 1 until detection
       completes) */
    update_detected_slices_port(self, self->detected_slices_count);
  }

  if (self->sampler_src_dirty) {
    /* start rebuild into shadow */
    self->sampler_src_shadow_valid   = false;
    self->sampler_src_rebuild_active = true;
    self->sampler_src_pos            = 0u;
    self->sampler_src_peak_abs       = 0.0f;
    self->sampler_src_norm_gain      = 1.0f;
    self->sampler_src_loop_samples   = self->loop_samples;

    /* detection should also restart now that the underlying buffer will
       change, but only when transient splitting is enabled.  Do not wipe any
       offset seed that may already have been prepared above (e.g. when the
       threshold or split mode changed). */
    if (alo_get_use_transient_slices_b(self)) {
      self->detect_active = true;
      self->detect_pos    = 0u;
      if (self->detected_slices_count == 0u) {
        /* ensure offset zero exists */
        self->detected_slices_count = 1u;
        self->detected_slice_offsets[0] = 0u;
        self->detected_slice_strength[0] = 0.0f;
      }
    } else {
      uint32_t slice_count = alo_get_slice_count(self);
      self->detected_slices_count = slice_count;
      if (self->ports.detected_slices_out) {
        *(self->ports.detected_slices_out) = (float)slice_count;
      }
      self->detect_active = false;
    }

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
    /* slice_count not needed here */
    uint32_t       cap_pos = self->sampler_src_pos;
    const uint32_t cap_end = self->loop_samples;
    uint32_t       cap_max = n_samples;
    {
      const uint32_t kMaxPerRun = SC_MAX_PER_RUN; /* cap work per call */
      uint64_t       scaled     = (uint64_t)n_samples * SC_SCALE_FACTOR;
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

      /* incrementally perform transient detection in lockstep with the
         cache rebuild.  This keeps work bounded per-block and avoids the
         huge scan that used to run when the rebuild completed. */
      if (self->detect_active) {
        if (self->detect_pos == 0u) {
          /* initialise detector: if user provided an explicit threshold use
             that value directly, otherwise fall back to the legacy
             sensitivity mapping helper. */
          float thr;
          if (self->ports.transient_threshold) {
            thr = *(self->ports.transient_threshold);
          } else {
            thr = alo_sensitivity_to_threshold(self);
          }
          td_init(&self->detect_td, (float)self->rate, thr, TD_DEFAULT_DEBOUNCE_MS);
          /* first offset already set to zero during restart */
        }
        if (td_process_sample(&self->detect_td, ml)) {
          if (self->detect_pos > 0u) {
            /* enforce absolute & fractional minimum slice duration */
            uint32_t last_off = self->detected_slice_offsets[self->detected_slices_count - 1u];
            if (self->detect_pos - last_off < min_len) {
              /* too short, ignore this transient; restart detector to avoid
                 repeated firing on the same event. */
              float thr2 = alo_sensitivity_to_threshold(self);
              td_init(&self->detect_td, (float)self->rate, thr2, TD_DEFAULT_DEBOUNCE_MS);
            } else {
              /* candidate qualifies; apply cap and ranking logic */
              float strength = (ml < 0.0f) ? -ml : ml;
              /* no explicit cap any more; we only limit to the maximum
                 number of voices enforced by the sampler.  stronger transients
                 can bump weaker ones when we run out of slots. */
              if ((int)self->detected_slices_count < (int)ALO_SLICE_SAMPLER_MAX_VOICES) {
                uint32_t idx = self->detected_slices_count;
                self->detected_slice_offsets[idx] = self->detect_pos;
                self->detected_slice_strength[idx] = strength;
                self->detected_slices_count++;
                qsort(self->detected_slice_offsets, self->detected_slices_count,
                      sizeof(uint32_t), cmp_u32);
                update_detected_slices_port(self, self->detected_slices_count);
              } else {
                /* replace weakest if stronger */
                int weakest = 0;
                float weakest_strength = self->detected_slice_strength[0];
                for (int wi = 1; wi < (int)self->detected_slices_count; ++wi) {
                  if (self->detected_slice_strength[wi] < weakest_strength) {
                    weakest_strength = self->detected_slice_strength[wi];
                    weakest = wi;
                  }
                }
                if (strength > weakest_strength) {
                  self->detected_slice_offsets[weakest] = self->detect_pos;
                  self->detected_slice_strength[weakest] = strength;
                  qsort(self->detected_slice_offsets, self->detected_slices_count,
                        sizeof(uint32_t), cmp_u32);
                  update_detected_slices_port(self, self->detected_slices_count);
                }
              }
            }
          }
        }
        self->detect_pos++;
        if (self->detect_pos >= self->loop_samples) {
          /* drop trailing slice if it would be too short using same min_len */
          uint32_t min_len2 = UINT32_MAX;
          if (self->rate > 1e-6) {
            uint32_t m30 = (uint32_t)((double)self->rate * 0.030);
            min_len2 = m30;
          }
          if (self->loop_samples > 0) {
            uint32_t frac = (self->loop_samples + 15u) / 16u;
            if (frac < min_len2) min_len2 = frac;
          }
          if (min_len2 == UINT32_MAX) min_len2 = 1u;
          if (self->detected_slices_count > 1u) {
            uint32_t last_off = self->detected_slice_offsets[self->detected_slices_count - 1u];
            if (self->loop_samples - last_off < min_len2) {
              self->detected_slices_count--;
            }
          }
          if (self->detected_slices_count == 0u) {
            self->detected_slices_count = 1u;
            self->detected_slice_offsets[0] = 0u;
          }
          self->detect_active = false;
          update_detected_slices_port(self, self->detected_slices_count);
        } else {
          /* update output with partial count so UI stays responsive */
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
      if (self->sampler_src_peak_abs > 1.0f)
        self->sampler_src_norm_gain = 1.0f / self->sampler_src_peak_abs;
      else
        self->sampler_src_norm_gain = 1.0f;

      /* start a detection run on the freshly swapped buffer if it didn't
         already complete during the copy phase above.  (In practice it will
         always have been driven to completion while rebuilding, but this
         guard ensures correct behaviour when rebuild is skipped.) */
      if (self->detect_active && self->detect_pos < self->loop_samples) {
        /* continue scanning the remainder of the buffer now that the new
           audio is accessible in sampler_src_buf */
        uint32_t remaining = self->loop_samples - self->detect_pos;
        /* call ourselves recursively with n_samples large enough to finish
           the scan; recursion depth is low because we immediately clear
           detect_active inside. */
        sampler_cache_process(self, remaining, any_committed_audio);
      }
    }
  }

  /* If detection is active but we are not currently rebuilding the cache,
     continue scanning the already-written buffer in small chunks. This
     handles threshold/split-mode tweaks when the sampler is otherwise idle. */
  if (!self->sampler_src_rebuild_active && self->detect_active &&
      self->sampler_src_valid && self->loop_samples > 0) {
    uint32_t cap_end = self->loop_samples;
    uint32_t cap_pos = self->detect_pos;
    uint32_t cap_max = n_samples * 8u;
    if (cap_max < 1u)
      cap_max = 1u;
    const float* buf = self->sampler_src_buf;
    while (cap_pos < cap_end && cap_max--) {
      if (cap_pos == 0u) {
        /* initialise detector with explicit threshold if available, else
           fall back to legacy sensitivity mapping. */
        float thr;
        if (self->ports.transient_threshold) {
          thr = *(self->ports.transient_threshold);
        } else {
          thr = alo_sensitivity_to_threshold(self);
        }
        td_init(&self->detect_td, (float)self->rate, thr, 5.0f);
        self->detected_slices_count = 0u;
      }
      float s = buf[cap_pos];
      if (td_process_sample(&self->detect_td, s)) {
        if (cap_pos > 0u) {
          /* compute strength as absolute sample amplitude */
          float strength = s < 0.0f ? -s : s;
          /* regular insertion with a hard voice limit; no cap port any
             more. */
          if ((int)self->detected_slices_count < (int)ALO_SLICE_SAMPLER_MAX_VOICES) {
            uint32_t idx = self->detected_slices_count;
            self->detected_slice_offsets[idx] = cap_pos;
            self->detected_slice_strength[idx] = strength;
            self->detected_slices_count++;
            /* sort by offset so playback order remains increasing */
            qsort(self->detected_slice_offsets, self->detected_slices_count,
                  sizeof(uint32_t), cmp_u32);
            update_detected_slices_port(self, self->detected_slices_count);
          } else {
            /* already at maximum voices: replace weakest if this one is stronger */
            int weakest = 0;
            float weakest_strength = self->detected_slice_strength[0];
            for (int wi = 1; wi < (int)self->detected_slices_count; ++wi) {
              if (self->detected_slice_strength[wi] < weakest_strength) {
                weakest_strength = self->detected_slice_strength[wi];
                weakest = wi;
              }
            }
            if (strength > weakest_strength) {
              self->detected_slice_offsets[weakest] = cap_pos;
              self->detected_slice_strength[weakest] = strength;
              /* maintain sorted order after replacement */
              qsort(self->detected_slice_offsets, self->detected_slices_count,
                    sizeof(uint32_t), cmp_u32);
              update_detected_slices_port(self, self->detected_slices_count);
            }
          }
        }
      }
      cap_pos++;
      self->detect_pos = cap_pos;
      if (self->ports.detected_slices_out) {
        alo_port_write(self->ports.detected_slices_out, (float)self->detected_slices_count);
      }
    }
    if (cap_pos >= cap_end) {
      if (self->detected_slices_count == 0u) {
        self->detected_slices_count = 1u;
        self->detected_slice_offsets[0] = 0u;
      }
      self->detect_active = false;
    }
  }
}

