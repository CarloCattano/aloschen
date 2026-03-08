#include "sampler_cache.h"
#include "alo_util.h"
#include "transient_detector.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>  /* for debug logging */

/* tests are built without the main plugin code; provide a simple stub so
   references to alo_log resolve.  The real implementation lives in
   aloschen.c and writes to /tmp/alo.log when ALO_LOG is enabled.  Declare
   the stub as weak so it can safely coexist with the real (strong) symbol. */
void alo_log(const char* message, ...) __attribute__((weak));
void alo_log(const char* message, ...) { (void)message; }

/* update full-loop stereo mix cache for current block */

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
    /* debug unconditional print to stdout for tests */
#ifdef UNIT_TESTS
    printf("DBG> count=%u thr=%f\n", count, thr);
#endif
    alo_log("DEBUG: update_detected_slices_port called with count=%u thr=%f", count, thr);
    /* apply user cap if present */
    if (self->ports.transient_threshold) {
        int max_slices = (int)floorf(thr);
        if (max_slices < 1) {
            max_slices = 1;
        } else if (max_slices > (int)ALO_SLICE_SAMPLER_MAX_VOICES) {
            max_slices = (int)ALO_SLICE_SAMPLER_MAX_VOICES;
        }
        if ((uint32_t)max_slices < count) {
            alo_log("DEBUG: cap %d applied, incoming count %u -> %u", max_slices, count, (uint32_t)max_slices);
            count = (uint32_t)max_slices;
        }
    }
    self->detected_slices_count = count;
    alo_port_write(self->ports.detected_slices_out, (float)count);
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
  bool threshold_changed = false;
  if (self->ports.transient_threshold) {
    float cur = *(self->ports.transient_threshold);
    if (cur != self->cached_trans_thresh) {
      self->cached_trans_thresh = cur;
      threshold_changed = true;
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
  } else if (threshold_changed || split_changed ||
             (self->sampler_src_rebuild_active && self->sampler_src_pos == 0u)) {
    /* restart the scan when in split mode (threshold or mode change or new
       loop data).  Reset the detection state but keep any existing offset
       seed so that we don't temporarily drop to zero slices. */
    self->detect_active = true;
    self->detect_pos    = 0u;
    /* start with offset 0 always present */
    self->detected_slices_count = 1u;
    self->detected_slice_offsets[0] = 0u;
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

      /* incrementally perform transient detection in lockstep with the
         cache rebuild.  This keeps work bounded per-block and avoids the
         huge scan that used to run when the rebuild completed. */
      if (self->detect_active) {
        if (self->detect_pos == 0u) {
          /* initialise detector with current threshold */
          float thresh = (self->ports.transient_threshold && *(self->ports.transient_threshold) > 0.0f)
                            ? *(self->ports.transient_threshold)
                            : 2.0f;
          td_init(&self->detect_td, (float)self->rate, thresh, 5.0f);
          /* first offset already set to zero during restart */
        }
        if (td_process_sample(&self->detect_td, ml)) {
          if (self->detect_pos > 0u) {
            /* enforce absolute & fractional minimum slice duration */
            uint32_t last_off = self->detected_slice_offsets[self->detected_slices_count - 1u];
            if (self->detect_pos - last_off < min_len) {
              /* too short, ignore this transient; restart detector to avoid
                 repeated firing on the same event. */
              float thresh2 = (self->ports.transient_threshold && *(self->ports.transient_threshold) > 0.0f)
                                ? *(self->ports.transient_threshold)
                                : 2.0f;
              td_init(&self->detect_td, (float)self->rate, thresh2, 5.0f);
            } else if (self->detected_slices_count < ALO_SLICE_SAMPLER_MAX_VOICES) {
              self->detected_slice_offsets[self->detected_slices_count] = self->detect_pos;
              self->detected_slices_count++;
              update_detected_slices_port(self, self->detected_slices_count);
            } else {
              /* reached voice limit; stop detection early to save cycles */
              self->detect_active = false;
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
        float thresh = (self->ports.transient_threshold &&
                        *(self->ports.transient_threshold) > 0.0f)
                           ? *(self->ports.transient_threshold)
                           : 2.0f;
        td_init(&self->detect_td, (float)self->rate, thresh, 5.0f);
        self->detected_slices_count = 0u;
      }
      float s = buf[cap_pos];
      if (td_process_sample(&self->detect_td, s)) {
        if (cap_pos > 0u && self->detected_slices_count < ALO_MAX_SLICES) {
          /* enforce user max-slices cap */
          int max_slices = (self->ports.transient_threshold)
                               ? (int)floorf(*(self->ports.transient_threshold))
                               : (int)ALO_SLICE_SAMPLER_MAX_VOICES;
          if (max_slices < 1) max_slices = 1;
          if (max_slices > (int)ALO_SLICE_SAMPLER_MAX_VOICES)
            max_slices = (int)ALO_SLICE_SAMPLER_MAX_VOICES;
          if ((int)self->detected_slices_count < max_slices) {
            self->detected_slice_offsets[self->detected_slices_count] = cap_pos;
            self->detected_slices_count++;
            update_detected_slices_port(self, self->detected_slices_count);
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

