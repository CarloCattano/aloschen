#include "sampler_cache.h"
#include "alo_util.h"
#include "transient_detector.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* Local constants                                                            */
/* ------------------------------------------------------------------------- */

#define SC_MAX_PER_RUN 4096u
#define SC_SCALE_FACTOR 8u

/* tests are built without the main plugin code; provide a simple stub so
   references to alo_log resolve. The real implementation lives in
   aloschen.c and writes to /tmp/alo.log when ALO_LOG is enabled. */
void alo_log(const char* message, ...) __attribute__((weak));
void alo_log(const char* message, ...) { (void)message; }

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static int
cmp_u32(const void* a, const void* b)
{
    const uint32_t va = *(const uint32_t*)a;
    const uint32_t vb = *(const uint32_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static float
sc_absf(float x)
{
    return (x < 0.0f) ? -x : x;
}

static uint32_t
sc_compute_min_slice_len(const Alo* self)
{
    uint32_t min_len30 = UINT32_MAX;
    uint32_t frac_len  = UINT32_MAX;

    if (self && self->rate > 1e-6) {
        min_len30 = (uint32_t)((double)self->rate * 0.030);
    }
    if (self && self->loop_samples > 0u) {
        frac_len = (self->loop_samples + 15u) / 16u;
    }

    if (min_len30 == UINT32_MAX && frac_len == UINT32_MAX) {
        return 1u;
    }
    if (min_len30 == UINT32_MAX) {
        return frac_len ? frac_len : 1u;
    }
    if (frac_len == UINT32_MAX) {
        return min_len30 ? min_len30 : 1u;
    }

    return (min_len30 < frac_len ? min_len30 : frac_len) ? (min_len30 < frac_len ? min_len30 : frac_len) : 1u;
}

static void
sc_seed_detected_offsets(Alo* self)
{
    if (!self) {
        return;
    }

    self->detected_slices_count      = 1u;
    self->detected_slice_offsets[0]  = 0u;
    self->detected_slice_strength[0] = 0.0f;
}

static uint32_t
sc_last_detected_offset(const Alo* self)
{
    if (!self || self->detected_slices_count == 0u) {
        return 0u;
    }
    return self->detected_slice_offsets[self->detected_slices_count - 1u];
}

static void
update_detected_slices_port(Alo* self, uint32_t count)
{
    if (!self) {
        return;
    }

    if (count > ALO_SLICE_INFO_MAX) {
        count = ALO_SLICE_INFO_MAX;
    }

    /* Restore the old "musical floor" in transient mode so sparse detector
       output still yields useful slice playback like the earlier sweet-spot
       implementation. */
    if (alo_get_use_transient_slices_b(self)) {
        uint32_t bars = alo_get_bars_i(self);
        uint32_t minreq = bars * ALO_MIN_SLICES_PER_BAR;
        if (minreq < ALO_MIN_SLICES_PER_BAR) {
            minreq = ALO_MIN_SLICES_PER_BAR;
        }

        if (count < minreq) {
            for (uint32_t i = 0u; i < minreq && i < ALO_SLICE_INFO_MAX; ++i) {
                self->detected_slice_offsets[i] =
                    (uint32_t)(((uint64_t)i * (uint64_t)self->loop_samples) / (uint64_t)minreq);
                self->detected_slice_strength[i] = 0.0f;
            }
            count = minreq;
        }
    }

    self->detected_slices_count = count;
    alo_port_write(self->ports.detected_slices_out, (float)count);
}

static void
update_slice_buffers_internal(Alo* self)
{
    const bool use_transient = alo_get_use_transient_slices_b(self);
    uint32_t slice_count = use_transient ? self->detected_slices_count
                                         : alo_get_slice_count_u(self);

    if (!self) {
        return;
    }

    if (slice_count > ALO_SLICE_INFO_MAX) {
        slice_count = ALO_SLICE_INFO_MAX;
    }

    for (uint32_t i = 0; i < ALO_SLICE_INFO_MAX; ++i) {
        AloSliceBuffer* p = &self->slice_sampler.slice_buffers[i];
        AloSliceBuffer* s = &self->slice_sampler.slice_buffers_shadow[i];
        p->valid    = false;
        s->valid    = false;
        p->data     = NULL;
        s->data     = NULL;
        p->length   = 0u;
        s->length   = 0u;
        p->borrowed = true;
        s->borrowed = true;
    }

    if (use_transient) {
        for (uint32_t i = 0; i < slice_count; ++i) {
            const uint32_t start = self->detected_slice_offsets[i];
            const uint32_t end   = (i + 1u < slice_count)
                                 ? self->detected_slice_offsets[i + 1u]
                                 : self->loop_samples;

            if (end <= start) {
                continue;
            }

            {
                const AloSliceBuffer buf = {
                    .valid    = true,
                    .data     = &self->sampler_src_buf[start],
                    .length   = end - start,
                    .borrowed = true
                };
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
                const AloSliceBuffer buf = {
                    .valid    = true,
                    .data     = &self->sampler_src_buf[(uint32_t)s0],
                    .length   = (uint32_t)(s1 - s0),
                    .borrowed = true
                };
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

void
sampler_cache_update_slice_buffers(Alo* self)
{
    if (!self) {
        return;
    }
    if (!self->sampler_src_valid || self->loop_samples == 0u) {
        return;
    }
    update_slice_buffers_internal(self);
}

void
sampler_cache_process(Alo* self, uint32_t n_samples, bool any_committed_audio)
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
        bool restart_transient   = false;

        if (self->ports.slice_sens) {
            const float cur = *(self->ports.slice_sens);
            if (cur != self->cached_sens) {
                self->cached_sens = cur;
                sensitivity_changed = true;
            }
        }

        if (self->ports.transient_threshold) {
            const float cur = *(self->ports.transient_threshold);
            if (cur != self->cached_threshold) {
                self->cached_threshold = cur;
                threshold_changed = true;
            }
        }

        if (self->ports.split_by_transient) {
            const bool mode = (*(self->ports.split_by_transient) > 0.5f);
            if (mode != self->cached_split_mode) {
                self->cached_split_mode = mode;
                split_changed = true;
            }
        }

        transient_mode = alo_get_use_transient_slices_b(self);

        if (!transient_mode) {
            const uint32_t slice_count = alo_get_slice_count(self);
            update_detected_slices_port(self, slice_count);
            self->detect_active = false;
        } else {
            restart_transient = sensitivity_changed ||
                                threshold_changed ||
                                split_changed ||
                                (self->sampler_src_rebuild_active && self->sampler_src_pos == 0u);

            if (restart_transient) {
                self->detect_active = true;
                self->detect_pos    = 0u;
                sc_seed_detected_offsets(self);
                update_detected_slices_port(self, self->detected_slices_count);
            }
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
            const uint32_t slice_count = alo_get_slice_count(self);
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
        uint32_t cap_pos = self->sampler_src_pos;
        const uint32_t cap_end = self->loop_samples;
        uint32_t cap_max = n_samples;

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
                const float a = (a_l > a_r) ? a_l : a_r;
                if (a > self->sampler_src_peak_abs) {
                    self->sampler_src_peak_abs = a;
                }
            }

            /* Restore the older, simpler trigger-based transient slicing:
               initialize a detector once, accept trigger positions directly,
               and enforce only minimum distance + max voice count. */
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
                        const uint32_t min_len = sc_compute_min_slice_len(self);
                        const uint32_t last_off = sc_last_detected_offset(self);

                        if (self->detect_pos - last_off >= min_len) {
                            const float strength = sc_absf(ml);

                            if (self->detected_slices_count < ALO_SLICE_SAMPLER_MAX_VOICES) {
                                const uint32_t ins = self->detected_slices_count;
                                self->detected_slice_offsets[ins]  = self->detect_pos;
                                self->detected_slice_strength[ins] = strength;
                                self->detected_slices_count++;
                                qsort(self->detected_slice_offsets,
                                      self->detected_slices_count,
                                      sizeof(uint32_t),
                                      cmp_u32);
                                update_detected_slices_port(self, self->detected_slices_count);
                            } else {
                                int weakest = 0;
                                float weakest_strength = self->detected_slice_strength[0];
                                for (uint32_t wi = 1u; wi < self->detected_slices_count; ++wi) {
                                    if (self->detected_slice_strength[wi] < weakest_strength) {
                                        weakest_strength = self->detected_slice_strength[wi];
                                        weakest = (int)wi;
                                    }
                                }

                                if (strength > weakest_strength) {
                                    self->detected_slice_offsets[weakest]  = self->detect_pos;
                                    self->detected_slice_strength[weakest] = strength;
                                    qsort(self->detected_slice_offsets,
                                          self->detected_slices_count,
                                          sizeof(uint32_t),
                                          cmp_u32);
                                    update_detected_slices_port(self, self->detected_slices_count);
                                }
                            }
                        } else {
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
                    const uint32_t min_len2 = sc_compute_min_slice_len(self);

                    if (self->detected_slices_count > 1u) {
                        const uint32_t last_off = sc_last_detected_offset(self);
                        if (self->loop_samples > last_off &&
                            (self->loop_samples - last_off) < min_len2) {
                            self->detected_slices_count--;
                        }
                    }

                    if (self->detected_slices_count == 0u) {
                        sc_seed_detected_offsets(self);
                    }

                    self->detect_active = false;
                    update_detected_slices_port(self, self->detected_slices_count);
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
            self->sampler_src_norm_gain = (self->sampler_src_peak_abs > 1.0f)
                ? (1.0f / self->sampler_src_peak_abs)
                : 1.0f;

            if (self->detect_active && self->detect_pos < self->loop_samples) {
                const uint32_t remaining = self->loop_samples - self->detect_pos;
                sampler_cache_process(self, remaining, any_committed_audio);
            }
        }
    }

    /* Continue scanning the already-built buffer when only controls changed. */
    if (!self->sampler_src_rebuild_active &&
        self->detect_active &&
        self->sampler_src_valid &&
        self->loop_samples > 0u) {

        uint32_t cap_end = self->loop_samples;
        uint32_t cap_pos = self->detect_pos;
        uint32_t cap_max = n_samples * 8u;
        const float* buf = self->sampler_src_buf;

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
                        const uint32_t min_len = sc_compute_min_slice_len(self);
                        const uint32_t last_off = sc_last_detected_offset(self);

                        if (cap_pos - last_off >= min_len) {
                            const float strength = sc_absf(s);

                            if (self->detected_slices_count < ALO_SLICE_SAMPLER_MAX_VOICES) {
                                const uint32_t ins = self->detected_slices_count;
                                self->detected_slice_offsets[ins]  = cap_pos;
                                self->detected_slice_strength[ins] = strength;
                                self->detected_slices_count++;
                                qsort(self->detected_slice_offsets,
                                      self->detected_slices_count,
                                      sizeof(uint32_t),
                                      cmp_u32);
                                update_detected_slices_port(self, self->detected_slices_count);
                            } else {
                                int weakest = 0;
                                float weakest_strength = self->detected_slice_strength[0];
                                for (uint32_t wi = 1u; wi < self->detected_slices_count; ++wi) {
                                    if (self->detected_slice_strength[wi] < weakest_strength) {
                                        weakest_strength = self->detected_slice_strength[wi];
                                        weakest = (int)wi;
                                    }
                                }

                                if (strength > weakest_strength) {
                                    self->detected_slice_offsets[weakest]  = cap_pos;
                                    self->detected_slice_strength[weakest] = strength;
                                    qsort(self->detected_slice_offsets,
                                          self->detected_slices_count,
                                          sizeof(uint32_t),
                                          cmp_u32);
                                    update_detected_slices_port(self, self->detected_slices_count);
                                }
                            }
                        }
                    }
                }
            }

            cap_pos++;
            self->detect_pos = cap_pos;
        }

        if (cap_pos >= cap_end) {
            const uint32_t min_len2 = sc_compute_min_slice_len(self);

            if (self->detected_slices_count > 1u) {
                const uint32_t last_off = sc_last_detected_offset(self);
                if (self->loop_samples > last_off &&
                    (self->loop_samples - last_off) < min_len2) {
                    self->detected_slices_count--;
                }
            }

            if (self->detected_slices_count == 0u) {
                sc_seed_detected_offsets(self);
            }

            self->detect_active = false;
            update_detected_slices_port(self, self->detected_slices_count);
        }
    }
}