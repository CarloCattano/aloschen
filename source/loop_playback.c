#include "alo_engine.h"
#include "alo_util.h"
#include "loop_state.h"
#include "transport.h"
#include "sampler_cache.h"

#include <math.h>
#include <string.h>

/* Anti-denormal bias value: added to the playback accumulator once per sample
 * to prevent subnormal FPU stalls on ARM Cortex-A where flush-to-zero is not
 * implicit.  At -300 dBFS this is far below audibility.
 */
#define ALO_ANTI_DENORM 1e-15f

/* Main loop playback engine: recording, overdub, and unified per-sample DSP. */

void run_loops(Alo* self, uint32_t n_samples)
{
  ALO_GUARD_VOID(self);

  // Discharge any queued arm now that we've recomputed transport state
  if (self->pending_arm_track >= 0) {
    bool other_busy = false;
    for (int u = 0; u < NUM_TRACKS; ++u) {
      if (u == self->pending_arm_track)
        continue;
      if (self->track_state[u] == TRACK_REC_BASE || self->track_state[u] == TRACK_REC_OVERDUB) {
        other_busy = true;
        break;
      }
    }
    if (!other_busy) {
      int t                          = self->pending_arm_track;
      self->pending_arm_track        = -1;
      self->track_state[t]           = self->pending_arm_type;
      self->rec_remaining_samples[t] = 0;
      update_loop_state_ports(self);
    }
  }

  /* incremental clearing of slice buffers */
  alo_slice_sampler_step_clear(&self->slice_sampler, n_samples * 2); /* two channels */

  const float* const input_l  = self->ports.input_l;
  const float* const input_r  = self->ports.input_r;
  float* const       output_l = self->ports.output_l;
  float* const       output_r = self->ports.output_r;

  if (!input_l || !input_r || !output_l || !output_r || !self->ports.mix) {
    return;
  }

  self->loopmix = fminf(1.0f, *self->ports.mix / 50.0f);
  self->inmix   = fminf(1.0f, (100.0f - *self->ports.mix) / 50.0f);

  float track_gain[NUM_TRACKS];
  for (int t = 0; t < NUM_TRACKS; ++t) {
    track_gain[t] = alo_read_gain_01(self->ports.loop_vol[t]);
  }

  /* Allow full range -2.5..2.5 for sampler gain. */
  const float sampler_gain = alo_read_gain_clamped(self->ports.sampler_vol, -2.5f, 2.5f);

  const bool transport_running =
      self->have_transport && self->have_last_transport_beats &&
      (self->transport_blocks_without_update <= 2u) && (!self->have_speed || self->speed != 0.0f) &&
      (self->transport_updated_this_cycle ? self->transport_moving : true);

  /* Normally loops stop/advance only while transport is running.  However, we
   * still want sampler slices and the cache rebuild to operate regardless of
   * transport state.  Remember when to suppress loop playback below.
   */
  bool skip_loops = false;
  if (!transport_running) {
    skip_loops = true;

    /* If the host reports a real transport stop, abort any RECORDING states
     * but keep ARM states so the user can arm before start.  Also clear any
     * in-flight slice voices to avoid stale audio while paused.
     */
    if (self->have_speed && self->speed == 0.0f) {
      for (int t = 0; t < NUM_TRACKS; ++t) {
        if (self->track_state[t] == TRACK_REC_BASE || self->track_state[t] == TRACK_REC_OVERDUB) {
          clear_track_state(self, t);
        }
      }
      alo_slice_sampler_reset(&self->slice_sampler);
    }
  }

  bool any_committed_audio = false;
  for (int t = 0; t < NUM_TRACKS; ++t) {
    if (self->have_loop[t]) {
      any_committed_audio = true;
      break;
    }
  }

  /* If committed content changed, we rebuild the sampler cache.  The new
   * mix is built into a shadow buffer so that ongoing slice voices may still
   * read from the previous buffer without dropout.  We no longer clear the
   * sampler when busy – voices continue until the swap at the end of rebuild.
   */
  /* (no action required here) */

  /* maintain incremental sampler cache with helper module */
  sampler_cache_process(self, n_samples, any_committed_audio);
  /* ensure slice buffers are kept in sync with whatever the cache just
     produced – this is what makes transient-split / threshold changes
     actually have an audible effect. */
  sampler_cache_update_slice_buffers(self);

  const bool sampler_busy = alo_slice_sampler_is_busy(&self->slice_sampler);

  if (!any_committed_audio && !sampler_busy) {
    alo_slice_sampler_reset(&self->slice_sampler);
  } else {
    alo_slice_sampler_begin_block(&self->slice_sampler, n_samples);
  }

  /* Align loop phase at start of block (transport-locked). */
  if (self->transport_loop_index_pending) {
    const uint32_t cycle = self->loop_samples ? self->loop_samples : 1u;
    uint32_t       phase = self->transport_loop_index;
    while (phase >= cycle) {
      phase -= cycle;
    }
    self->loop_phase                   = phase;
    self->loop_playhead                = (double)self->loop_start + (double)phase;
    self->transport_loop_index_pending = false;
  }

  /* Next-bar start scheduling for the very first loop origin.
   * Note: arming the first base recording forces a Bars-cycle/UI resync so
   * this next downbeat becomes step 0 (cycle start).
   */
  uint32_t first_base_start_offset[NUM_TRACKS];
  double   first_base_target_beats[NUM_TRACKS];
  for (int t = 0; t < NUM_TRACKS; ++t) {
    first_base_start_offset[t] = UINT32_MAX;
    first_base_target_beats[t] = 0.0;
  }

  if (!self->have_loop_origin && self->have_last_transport_beats) {
    const double bpm              = (self->bpm > 1e-6f) ? (double)self->bpm : (double)DEFAULT_BPM;
    const double samples_per_beat = (double)self->rate * 60.0 / bpm;
    const double global_beats0    = self->last_transport_beats;

    /* Quantize the very first base recording to the next Bars-cycle start. */
    const double target_beats = compute_next_cycle_start_beats(self, global_beats0);
    const double beats_until  = target_beats - global_beats0;
    uint64_t     offset_s     = 0;
    if (beats_until > 0.0) {
      /* normal quantization path */
      const double samples_until = beats_until * samples_per_beat;
      offset_s                   = (uint64_t)ceil(samples_until - 1e-9);
    }
    /* if we just resumed, keep offset_s=0 for immediate start */

    for (int t = 0; t < NUM_TRACKS; ++t) {
      if (self->track_state[t] == TRACK_ARM_BASE) {
        if (offset_s < (uint64_t)n_samples) {
          first_base_start_offset[t] = (uint32_t)offset_s;
          first_base_target_beats[t] = target_beats;

          /* if the UI isn't already synced, do so now using the computed
           * target.  This ensures the step indicator and audio origin match
           * even if handle_loop_press ran before transport info was known.
           */
          if (!self->ui_have_cycle_origin) {
            request_ui_cycle_resync(self);
            self->ui_cycle_origin_beats   = target_beats;
            self->ui_have_cycle_origin    = true;
            self->ui_cycle_resync_pending = false;
          }
        }
      }
    }
  }

  /* Compute bar length in samples for quantized undo (bar downbeats). */
  const uint32_t bar_len = alo_get_bar_len_samples(self);

  const float inmix   = self->inmix;
  const float loopmix = self->loopmix;

  /*
   * Optimization pass:
   * Pre-sum playback contribution for this block into preallocated scratch
   * arrays, so the per-sample hot loop doesn't iterate tracks × layers.
   *
   * Real-time: no heap allocation in run(); scratch is allocated at init.
   */
  const uint32_t cap       = self->rt_block_cap;
  float* const   slice_l_s = self->rt_slice_l;
  float* const   slice_r_s = self->rt_slice_r;

  uint32_t block_base = 0u;
  while (block_base < n_samples) {
    uint32_t blk = n_samples - block_base;
    if (cap > 0u && blk > cap) {
      blk = cap;
    }

    /* Determine whether any track has committed audio to play back. */
    bool any_play = false;
    if (!skip_loops) {
      for (int t = 0; t < NUM_TRACKS; ++t) {
        if (self->have_loop[t] && self->loop_buf[t]) {
          any_play = true;
          break;
        }
      }
    }

    memset(slice_l_s, 0, sizeof(float) * (size_t)blk);
    memset(slice_r_s, 0, sizeof(float) * (size_t)blk);

    /* Process slice sampler chunk into scratch buffers (once per sub-block). */
    if (sampler_gain > 0.0f && sampler_busy) {
      const uint32_t slices = alo_get_slices_per_bar_u(self);
      if (slices != self->cached_slices_per_bar) {
        self->cached_slices_per_bar   = slices;
        self->cached_slice_gain_scale = (slices > 1u) ? (1.0f / sqrtf((float)slices)) : 1.0f;
      }
      alo_slice_sampler_process_chunk(&self->slice_sampler, self, block_base, blk, slice_l_s,
                                      slice_r_s);
      for (uint32_t pos = 0; pos < blk; ++pos) {
        slice_l_s[pos] *= self->cached_slice_gain_scale;
        slice_r_s[pos] *= self->cached_slice_gain_scale;
      }
    }

    /* Single unified pass: compute playback, handle events, record, output.
     *
     * Playback is computed inline before the at-bar undo so that the
     * pre-undo od_count is used for this sample.  This preserves the same
     * clean bar-boundary undo behaviour as the previous two-pass approach
     * while eliminating the redundant phase-advance traversal.
     */
    uint32_t phase_loc    = self->loop_phase;
    double   playhead_loc = self->loop_playhead;
    uint32_t bar_phase    = phase_loc;
    /* normalise bar_phase into [0,bar_len) without modulo */
    if (bar_len > 0u) {
      while (bar_phase >= bar_len) {
        bar_phase -= bar_len;
      }
    }
    const uint32_t end_idx = self->loop_start + self->loop_samples;

    for (uint32_t pos = 0; pos < blk; ++pos) {
      const uint32_t pos_in_block = block_base + pos;

      uint32_t idx   = self->loop_start + phase_loc;
      uint32_t idx_r = idx + LOOP_SIZE;

      /* --- Playback pre-sum (inlined; uses pre-undo od_count) --- */
      float play_l = ALO_ANTI_DENORM;
      float play_r = ALO_ANTI_DENORM;
      if (any_play) {
        double   frac     = playhead_loc - (double)idx;
        uint32_t idx_next = idx + 1;
        if (idx_next >= end_idx) {
          idx_next = self->loop_start;
        }
        const uint32_t idx_next_r = idx_next + LOOP_SIZE;

        for (int t = 0; t < NUM_TRACKS; ++t) {
          if (!self->have_loop[t] || !self->loop_buf[t]) {
            continue;
          }
          const float g  = track_gain[t];
          const float l0 = self->loop_buf[t][idx];
          const float r0 = self->loop_buf[t][idx_r];
          const float l1 = self->loop_buf[t][idx_next];
          const float r1 = self->loop_buf[t][idx_next_r];

          const float il = alo_lerp_sample(frac, l0, l1);
          const float ir = alo_lerp_sample(frac, r0, r1);

          play_l += g * il;
          play_r += g * ir;

          const uint8_t n_layers = self->od_count[t];
          for (uint8_t l = 0; l < n_layers; ++l) {
            const float* const buf = self->od_buf[t][l];
            if (!buf) {
              continue;
            }
            const float bl0 = buf[idx];
            const float br0 = buf[idx_r];
            const float bl1 = buf[idx_next];
            const float br1 = buf[idx_next_r];

            const float ibl = alo_lerp_sample(frac, bl0, bl1);
            const float ibr = alo_lerp_sample(frac, br0, br1);

            play_l += g * ibl;
            play_r += g * ibr;
          }
        }
      }

      /* At-bar detection: apply pending undos AFTER reading playback so
       * the undo takes clean effect from the next sub-block onward.
       */
      const bool at_bar = (bar_len > 0u && bar_phase == 0u);
      if (at_bar) {
        if (apply_pending_undo_at_bar(self)) {
          self->sampler_src_dirty = true;
        }
      }

      const float in_l = input_l[pos_in_block];
      const float in_r = input_r[pos_in_block];

      float slice_l = 0.0f;
      float slice_r = 0.0f;
      if (sampler_gain > 0.0f && sampler_busy) {
        slice_l = slice_l_s[pos] * sampler_gain;
        slice_r = slice_r_s[pos] * sampler_gain;
      }

      /* Dry input + inlined playback + slice one-shots */
      float out_l_val        = inmix * in_l + play_l + slice_l;
      float out_r_val        = inmix * in_r + play_r + slice_r;
      output_l[pos_in_block] = out_l_val;
      output_r[pos_in_block] = out_r_val;

      /* Prevent clipping when summing multiple tracks / heavy overdubs / slice
       * polyphony. Only engage when the signal would actually clip, so normal
       * levels remain unity (keeps looper vs sampler gain staging consistent).
       */
      if (fabsf(output_l[pos_in_block]) > 1.0f) {
        output_l[pos_in_block] = alo_soft_clip_unit(output_l[pos_in_block]);
      }
      if (fabsf(output_r[pos_in_block]) > 1.0f) {
        output_r[pos_in_block] = alo_soft_clip_unit(output_r[pos_in_block]);
      }

      /* Start armed recordings. */
      for (int t = 0; t < NUM_TRACKS; ++t) {
        if (self->track_state[t] == TRACK_ARM_BASE) {
          if (!self->have_loop_origin) {
            if (first_base_start_offset[t] == pos_in_block) {
              /* Commit loop origin on first-ever base recording.  Also make sure
               * the UI cycle origin matches exactly so the step indicator shows
               * 0 at the same moment the audio begins.
               */
              self->loop_origin_beats            = first_base_target_beats[t];
              self->have_loop_origin             = true;
              self->transport_loop_index         = 0;
              self->transport_loop_index_pending = true;
              phase_loc                          = 0;
              playhead_loc                       = (double)self->loop_start;

              /* sync UI now that origin is definite */
              self->ui_cycle_origin_beats   = self->loop_origin_beats;
              self->ui_have_cycle_origin    = true;
              self->ui_cycle_resync_pending = false;
              /* force immediate step output so the UI doesn't lag behind audio */
              alo_port_write(self->ports.bar_step_out, 0.0f);
              self->ui_last_bar_step = 0;

              self->track_state[t]           = TRACK_REC_BASE;
              self->rec_remaining_samples[t] = self->loop_samples;
              self->have_loop[t]             = false;
              self->od_count[t]              = 0;
            }
          } else {
            /* Start base on next loop boundary. */
            if (phase_loc == 0) {
              /* Recording always starts on a loop boundary; make sure the UI
               * origin is still correct (bars may have changed during arm).
               */
              /* origin is already valid; just refresh UI fields */
              self->ui_cycle_origin_beats   = self->loop_origin_beats;
              self->ui_have_cycle_origin    = true;
              self->ui_cycle_resync_pending = false;
              alo_port_write(self->ports.bar_step_out, 0.0f);
              self->ui_last_bar_step = 0;

              self->track_state[t]           = TRACK_REC_BASE;
              self->rec_remaining_samples[t] = self->loop_samples;
              self->have_loop[t]             = false;
              self->od_count[t]              = 0;
            }
          }
        } else if (self->track_state[t] == TRACK_ARM_OVERDUB) {
          if (self->have_loop[t] && phase_loc == 0) {
            /* Only start an overdub if we have a free layer to record into.
             * Never overwrite an active layer in-place (aborts would corrupt
             * playback).
             */
            if (self->od_count[t] < ALO_MAX_UNDO_LAYERS) {
              self->track_state[t]           = TRACK_REC_OVERDUB;
              self->rec_remaining_samples[t] = self->loop_samples;
              self->rec_od_layer[t]          = self->od_count[t];
            } else {
              self->track_state[t]           = TRACK_IDLE;
              self->rec_remaining_samples[t] = 0;
            }
          }
        }
      }

      /* Recording. */
      for (int t = 0; t < NUM_TRACKS; ++t) {
        if (!self->loop_buf[t]) {
          continue;
        }

        if (self->track_state[t] == TRACK_REC_BASE) {
          self->loop_buf[t][idx]   = loopmix * in_l;
          self->loop_buf[t][idx_r] = loopmix * in_r;

          if (self->rec_remaining_samples[t] > 0) {
            self->rec_remaining_samples[t]--;
          }
          if (self->rec_remaining_samples[t] == 0) {
            self->track_state[t] = TRACK_IDLE;
            alo_apply_edge_fade_stereo(self->loop_buf[t], self->loop_start, self->loop_samples,
                                       alo_edge_fade_samples_u32(self));
            self->have_loop[t]      = true;
            self->sampler_src_dirty = true;
          }
        } else if (self->track_state[t] == TRACK_REC_OVERDUB) {
          const uint8_t rec_layer = self->rec_od_layer[t];
          if (rec_layer < ALO_MAX_UNDO_LAYERS && self->od_buf[t][rec_layer]) {
            float* const buf = self->od_buf[t][rec_layer];
            buf[idx]         = loopmix * in_l;
            buf[idx_r]       = loopmix * in_r;
          }

          if (self->rec_remaining_samples[t] > 0) {
            self->rec_remaining_samples[t]--;
          }
          if (self->rec_remaining_samples[t] == 0) {
            self->track_state[t] = TRACK_IDLE;
            self->have_loop[t]   = true;
            if (self->od_count[t] < ALO_MAX_UNDO_LAYERS) {
              /* Fade the newly committed layer edges to zero for click-free
               * wrap-around.
               */
              const uint8_t rec_layer2 = self->rec_od_layer[t];
              if (rec_layer2 < ALO_MAX_UNDO_LAYERS && self->od_buf[t][rec_layer2]) {
                alo_apply_edge_fade_stereo(self->od_buf[t][rec_layer2], self->loop_start,
                                           self->loop_samples, alo_edge_fade_samples_u32(self));
              }
              self->od_count[t]++;
            }
            self->sampler_src_dirty = true;
          }
        }
      }

      /* Advance phase & bar counters. */
      phase_loc++;
      if (phase_loc >= self->loop_samples) {
        phase_loc = 0;
      }
      playhead_loc += 1.0;
      if (playhead_loc >= (double)end_idx) {
        playhead_loc -= (double)self->loop_samples;
      }
      if (bar_len > 0u) {
        bar_phase++;
        if (bar_phase >= bar_len) {
          bar_phase -= bar_len;
        }
      }
    }
    /* store updated phase/playhead */
    self->loop_phase    = phase_loc;
    self->loop_playhead = playhead_loc;

    block_base += blk;
  }

  update_loop_state_ports(self);
}