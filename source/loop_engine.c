#include "alo_engine.h"
#include "alo_util.h"
#include "transport.h"
#include "sampler_cache.h"
#include "button_logic.h"

#include "lv2/atom/util.h"

#include <math.h>
#include <string.h>
/* Core looper DSP/engine implementation */

/* clear all audio state for a track */
static void clear_track_audio(Alo* self, int t)
{
  if (!self || !track_is_active(self, t)) {
    return;
  }

  self->have_loop[t]         = false;
  self->od_count[t]          = 0;
  self->pending_undo[t]      = 0;
  self->pending_clear_all[t] = false;

  /* Committed audio changed; sampler source cache must be rebuilt. */
  self->sampler_src_dirty        = true;
  self->sampler_src_valid        = false;
  self->sampler_src_shadow_valid = false;
}

/* cancel inflight track actions and prime UI controls */
void clear_inflight_actions_and_sync_controls(Alo* self)
{
  if (!self) {
    return;
  }

  /* Reset pending actions. */
  for (int t = 0; t < NUM_TRACKS; ++t) {
    clear_track_state(self, t);
    self->pending_undo[t]      = 0;
    self->pending_clear_all[t] = false;

    /* Prime edge detectors to ignore held buttons. */
    const bool loop_btn           = alo_port_pressed(self->ports.loop_btn[t]);
    const bool undo_btn           = alo_port_pressed(self->ports.undo_btn[t]);
    self->last_loop_input[t]      = loop_btn;
    self->last_undo_input[t]      = undo_btn;
    self->loop_btn_high_frames[t] = loop_btn ? 1u : 0u;
  }
}

/* force UI cycle phase resync */
void request_ui_cycle_resync(Alo* self)
{
  if (!self) {
    return;
  }
  self->click_bar_in_cycle      = 0;
  self->ui_cycle_resync_pending = true;
  self->ui_have_cycle_origin    = false;
  self->ui_have_prev_bar_beat   = false;
  /* Force UI step-0 refresh. */
  self->ui_last_bar_step = -2;
}

static inline float loop_state_value(const Alo* self, const int t, const bool transport_stopped)
{
  if (!self) {
    return 0.0f;
  }

  /* Loop state encoding for UIs. */
  /* queued arm overrides idle state. */
  if (self->pending_arm_track == t) {
    return 0.25f;
  }

  switch (self->track_state[t]) {
  case TRACK_ARM_BASE:
  case TRACK_ARM_OVERDUB:
    return 0.25f;
  case TRACK_REC_BASE:
  case TRACK_REC_OVERDUB:
    return 1.0f;
  case TRACK_IDLE:
  default:
    return (self->have_loop[t] && !transport_stopped) ? 0.5f : 0.0f;
  }
}

static inline float undo_state_value(const Alo* self, const int t)
{
  /* Undo output values. */
  const bool queued = (self->pending_clear_all[t] || self->pending_undo[t] > 0);
  return queued ? 0.25f : 0.0f;
}

static inline float has_audio_value(const Alo* self, const int t)
{
  return self->have_loop[t] ? 1.0f : 0.0f;
}

/* update LV2 output ports for loop/undo/audio states */
void update_loop_state_ports(Alo* self)
{
  if (!self) {
    return;
  }

  const bool transport_stopped = self->have_transport && self->have_speed && (self->speed == 0.0f);

  for (int t = 0; t < NUM_TRACKS; ++t) {
    if (self->ports.loop_state_out[t]) {
      *(self->ports.loop_state_out[t]) = loop_state_value(self, t, transport_stopped);
    }
    if (self->ports.undo_state_out[t]) {
      *(self->ports.undo_state_out[t]) = undo_state_value(self, t);
    }
    if (self->ports.has_audio_out[t]) {
      *(self->ports.has_audio_out[t]) = has_audio_value(self, t);
    }
  }
}

/** @name Timing helpers
 * @{ */


/* compute number of samples in a loop given beat length */
static uint32_t compute_loop_samples(const Alo* self, uint32_t loop_beats)
{
  if (!self) {
    return LOOP_SIZE;
  }
  const float  bpm       = (self->bpm > 1e-6f) ? self->bpm : (float)DEFAULT_BPM;
  const double samples_d = (double)loop_beats * (double)self->rate * 60.0 / (double)bpm;
  if (!(samples_d > 0.0)) {
    return LOOP_SIZE;
  }
  uint64_t s = (uint64_t)llround(samples_d);
  if (s < 1u) {
    s = 1u;
  }
  if (s > (uint64_t)LOOP_SIZE) {
    s = (uint64_t)LOOP_SIZE;
  }
  return (uint32_t)s;
}

/* recompute loop timing parameters after change */
void reset_timing(Alo* self)
{
  if (!self) {
    return;
  }

  const uint32_t new_loop_beats   = compute_loop_beats(self);
  const uint32_t new_loop_samples = compute_loop_samples(self, new_loop_beats);

  self->loop_beats   = new_loop_beats;
  self->loop_samples = new_loop_samples ? new_loop_samples : 1u;

  if (self->loop_samples > LOOP_SIZE) {
    self->loop_samples = LOOP_SIZE;
  }

  /* Clamp playhead/phase within new loop length. */
  if (self->loop_phase >= self->loop_samples) {
    self->loop_phase    = 0;
    self->loop_playhead = (double)self->loop_start;
  }
  /* Normalize transport_loop_index without modulo. */
  while (self->transport_loop_index >= self->loop_samples) {
    self->transport_loop_index -= self->loop_samples;
  }

  /* Keep phase aligned after tempo change. Recompute transport phase. */
  if (self->have_last_transport_beats) {
    uint32_t phase_samples = 0;
    if (compute_transport_phase_index(self, self->last_transport_beats, &phase_samples)) {
      self->transport_loop_index         = phase_samples;
      self->transport_loop_index_pending = true;
      self->have_transport               = true;
    }
  }

  /* Flag UI cycle resync after timing change. */
  request_ui_cycle_resync(self);
  /* Reset click envelope. */
  self->high_beat_offset  = self->beat_len;
  self->low_beat_offset   = self->beat_len;
  self->start_beat_offset = self->beat_len;

  update_loop_state_ports(self);
}

/* reinitialize engine state to defaults */
 * flags.  Intended to be called when the plugin is instantiated or when the
 * host sends a full reset.
 *
 * @param self Engine instance (may be NULL).
 */
void reset(Alo* self)
{
  if (!self) {
    return;
  }
  self->pending_arm_track = -1;
  self->pending_arm_type  = TRACK_IDLE;

  alo_slice_sampler_reset(&self->slice_sampler);
  /* Invalidate preallocated slice buffers. */
  alo_slice_sampler_clear_buffers(&self->slice_sampler);
  self->slice_sampler.rate = (float)self->rate;

  self->sampler_src_valid          = false;
  self->sampler_src_shadow_valid   = false;
  self->sampler_src_dirty          = true;
  self->sampler_src_rebuild_active = false;
  self->sampler_src_pos            = 0u;
  self->sampler_src_peak_abs       = 0.0f;
  self->sampler_src_norm_gain      = 1.0f;
  self->sampler_src_loop_samples   = 0u;

  for (int t = 0; t < NUM_TRACKS; ++t) {
    self->have_loop[t]         = false;
    self->od_count[t]          = 0;
    self->rec_od_layer[t]      = 0;
    self->pending_undo[t]      = 0;
    self->pending_clear_all[t] = false;
    clear_track_state(self, t);

    self->last_loop_input[t]      = false;
    self->last_undo_input[t]      = false;
    self->loop_btn_high_frames[t] = 0;
  }

  self->loop_beats   = compute_loop_beats(self);
  self->loop_samples = compute_loop_samples(self, self->loop_beats);
  if (!self->loop_samples) {
    self->loop_samples = 1u;
  }

  self->loop_start    = 0;
  self->loop_phase    = 0;
  self->loop_playhead = (double)self->loop_start;

  self->have_transport                  = false;
  self->transport_moving                = false;
  self->transport_updated_this_cycle    = false;
  self->transport_blocks_without_update = 0;
  self->have_speed                      = false;
  self->speed                           = 0.0f;
  self->transport_loop_index            = 0;
  self->transport_loop_index_pending    = false;

  self->have_last_bar_beat   = false;
  self->last_bar_beat        = 0.0f;
  self->bar_counter_fallback = 0;

  self->last_transport_beats      = 0.0;
  self->have_last_transport_beats = false;

  self->loop_origin_beats = 0.0;
  self->have_loop_origin  = false;

  /* Reset click. */
  self->high_beat_offset   = self->beat_len;
  self->low_beat_offset    = self->beat_len;
  self->start_beat_offset  = self->beat_len;
  self->click_bar_in_cycle = 0;

  self->ui_last_bar_step         = -2;
  self->ui_last_bars_i           = alo_get_bars_i(self);
  self->ui_cycle_resync_pending  = true;
  self->ui_transport_was_stopped = true;
  self->ui_cycle_origin_beats    = 0.0;
  self->ui_have_cycle_origin     = false;
  self->ui_prev_bar_beat         = 0.0f;
  self->ui_have_prev_bar_beat    = false;

  /* Immediately clear UI phase outputs. */
  if (self->ports.bar_step_out) {
    *(self->ports.bar_step_out) = 0.0f;
  }
  if (self->ports.cycle_phase_out) {
    *(self->ports.cycle_phase_out) = 0.0f;
  }
  if (self->ports.host_bar_phase_out) {
    *(self->ports.host_bar_phase_out) = 0.0f;
  }

  update_loop_state_ports(self);
}

/* return current slice root MIDI note (0..127) */
static inline int get_slice_root_note(const Alo* self)
{
  if (!self) {
    return 36;
  }
  if (!self->ports.slice_root) {
    return 36;
  }
  const int v = (int)floorf(*(self->ports.slice_root));
  if (v < 0) {
    return 0;
  }
  if (v > 127) {
    return 127;
  }
  return v;
}

/* compute and emit current bar-step/cycle-phase for UI outputs */
static void update_bar_step_out(Alo* self)
{
  if (!self) {
    return;
  }

  const bool transport_stopped = (self->have_speed && self->speed == 0.0f);

  /* Bars change -> resync cycle. */
  const uint32_t bars_i = alo_get_bars_i(self);
  if (bars_i != self->ui_last_bars_i) {
    self->ui_last_bars_i = bars_i;
    request_ui_cycle_resync(self);
  }

  /* Default step when stopped/unsynced. */
  int step = 0;

  if (transport_stopped || !self->have_transport || !self->have_last_transport_beats) {
    /* Stopped/unknown: force resync. */
    request_ui_cycle_resync(self);
    step = 0;
  } else {
    float bar_beat = self->current_position;
    if (!(bar_beat >= 0.0f)) {
      bar_beat = 0.0f;
    }

    bool downbeat_edge = false;
    if (self->ui_have_prev_bar_beat) {
      /* Detect bar wrap. */
      if (bar_beat + 0.25f < self->ui_prev_bar_beat) {
        downbeat_edge = true;
      }
    }
    self->ui_prev_bar_beat      = bar_beat;
    self->ui_have_prev_bar_beat = true;

    if (self->ui_cycle_resync_pending && !self->ui_have_cycle_origin) {
      /* Set cycle origin on first downbeat. */
      const float kDownbeatGraceBeats = 0.25f;
      if (downbeat_edge || bar_beat <= kDownbeatGraceBeats) {
        self->ui_cycle_origin_beats   = self->last_transport_beats - (double)bar_beat;
        self->ui_have_cycle_origin    = true;
        self->ui_cycle_resync_pending = false;
      }
    }

    if (self->ui_cycle_resync_pending || !self->ui_have_cycle_origin) {
      step = 0;
    } else {
      const uint32_t steps_u = bars_i * (uint32_t)DEFAULT_BEATS_PER_BAR;
      if (steps_u > 0) {
        double phase = self->last_transport_beats - self->ui_cycle_origin_beats;
        if (phase < 0.0) {
          phase = 0.0;
        }
        phase = fmod(phase, (double)steps_u);
        if (phase < 0.0) {
          phase += (double)steps_u;
        }
        int s = (int)floor(phase);
        if (s < 0) {
          s = 0;
        } else if (s >= (int)steps_u) {
          s = (int)steps_u - 1;
        }
        step = s;
      } else {
        step = 0;
      }
    }
  }

  if (step != (int)self->ui_last_bar_step) {
    if (self->ports.bar_step_out) {
      *(self->ports.bar_step_out) = (float)step;
    }
    self->ui_last_bar_step = (int8_t)step;
  }

  /* Continuous debug phases (used by MOD GUI transport rings). */
  if (self->ports.host_bar_phase_out || self->ports.cycle_phase_out) {
    const bool transport_stopped2 = (self->have_speed && self->speed == 0.0f);

    float host_bar_phase = 0.0f;
    float cycle_phase    = 0.0f;

    if (!transport_stopped2 && self->have_transport && self->have_last_transport_beats) {
      /* Host bar phase: current beat within bar / beatsPerBar. */
      if (self->bpb > 0.0f) {
        float bb = self->current_position;
        if (!(bb >= 0.0f)) {
          bb = 0.0f;
        }
        host_bar_phase = bb / self->bpb;
        if (host_bar_phase < 0.0f) {
          host_bar_phase = 0.0f;
        } else if (host_bar_phase > 1.0f) {
          host_bar_phase = 1.0f;
        }
      }

      /* Bars-cycle phase: based on the same origin used for bar_step. */
      const uint32_t bars_i2  = alo_get_bars_i(self);
      const uint32_t steps_u2 = bars_i2 * (uint32_t)DEFAULT_BEATS_PER_BAR;
      if (steps_u2 > 0 && self->ui_have_cycle_origin && !self->ui_cycle_resync_pending) {
        double phase_beats = self->last_transport_beats - self->ui_cycle_origin_beats;
        if (phase_beats < 0.0) {
          phase_beats = 0.0;
        }
        phase_beats = fmod(phase_beats, (double)steps_u2);
        if (phase_beats < 0.0) {
          phase_beats += (double)steps_u2;
        }
        cycle_phase = (float)(phase_beats / (double)steps_u2);
        if (cycle_phase < 0.0f) {
          cycle_phase = 0.0f;
        } else if (cycle_phase > 1.0f) {
          cycle_phase = 1.0f;
        }
      }
    }

    if (self->ports.host_bar_phase_out) {
      *(self->ports.host_bar_phase_out) = host_bar_phase;
    }
    if (self->ports.cycle_phase_out) {
      *(self->ports.cycle_phase_out) = cycle_phase;
    }
  }
}

/* -------------------------------------------------------------------------
 * Button handling
 * ------------------------------------------------------------------------- */

static void handle_button_edges(Alo* self, int t, bool loop_btn, bool undo_btn)
{
  if (!self || !track_is_active(self, t)) {
    return;
  }

  /*
   * Loop buttons are toggles in many hosts/UIs.
   * - Trigger on rising edge always.
   * - Also trigger on falling edge only if it was held high long enough
   *   (supports latching toggles, avoids double-trigger for momentary pulses).
   */
  if (loop_btn) {
    if (!self->last_loop_input[t]) {
      self->loop_btn_high_frames[t] = 1;
      handle_loop_press(self, t);
    } else {
      if (self->loop_btn_high_frames[t] < UINT32_MAX) {
        self->loop_btn_high_frames[t]++;
      }
    }
  } else {
    if (self->last_loop_input[t]) {
      if (self->loop_btn_high_frames[t] >= 8) {
        handle_loop_press(self, t);
      }
    }
    self->loop_btn_high_frames[t] = 0;
  }

  if (undo_btn && !self->last_undo_input[t]) {
    handle_undo_press(self, t);
  }

  self->last_loop_input[t] = loop_btn;
  self->last_undo_input[t] = undo_btn;
}

static inline bool apply_pending_undo_at_bar(Alo* self)
{
  bool changed = false;
  for (int t = 0; t < NUM_TRACKS; ++t) {
    if (self->pending_clear_all[t]) {
      clear_track_audio(self, t);
      changed = true;
      continue;
    }

    while (self->pending_undo[t] > 0) {
      if (self->od_count[t] > 0) {
        self->od_count[t]--;
        changed = true;
      } else if (self->have_loop[t]) {
        self->have_loop[t] = false;
        changed            = true;
      }
      self->pending_undo[t]--;
    }
  }

  return changed;
}

/* -------------------------------------------------------------------------
 * Click
 * ------------------------------------------------------------------------- */

static void click_mix(Alo* self, uint32_t begin, uint32_t end)
{
  float* const output_l = self->ports.output_l;
  float* const output_r = self->ports.output_r;

  const float amplitude = (uint32_t)floorf(*(self->ports.click));

  for (uint32_t idx = begin; idx < end; idx++) {
    if (self->start_beat_offset < self->beat_len) {
      output_l[idx] += 0.1f * amplitude * self->start_beat[self->start_beat_offset];
      output_r[idx] += 0.1f * amplitude * self->start_beat[self->start_beat_offset];
      self->start_beat_offset++;
    }

    if (self->high_beat_offset < self->beat_len) {
      output_l[idx] += 0.1f * amplitude * self->high_beat[self->high_beat_offset];
      output_r[idx] += 0.1f * amplitude * self->high_beat[self->high_beat_offset];
      self->high_beat_offset++;
    }

    if (self->low_beat_offset < self->beat_len) {
      output_l[idx] += 0.1f * amplitude * self->low_beat[self->low_beat_offset];
      output_r[idx] += 0.1f * amplitude * self->low_beat[self->low_beat_offset];
      self->low_beat_offset++;
    }
  }
}

void run_clicks(Alo* self, uint32_t n_samples)
{
  if (!self) {
    return;
  }

  if (!self->ports.click || !self->ports.output_l || !self->ports.output_r) {
    return;
  }

  /* This plugin is transport-synced: do not free-run without host position. */
  if (!self->have_transport || !self->have_last_transport_beats) {
    return;
  }

  /* With host transport: do not advance click/beat state while stopped. */
  if (self->have_speed && self->speed == 0.0f) {
    return;
  }

  bool play_click = true;
  for (uint32_t t = 0; t < NUM_TRACKS; ++t) {
    if (self->have_loop[t]) {
      play_click = false;
      break;
    }
  }

  const bool can_click = play_click && (*(self->ports.click) > 0.0f) && self->speed;

  const double bpm              = (self->bpm > 1e-6f) ? (double)self->bpm : (double)DEFAULT_BPM;
  const double samples_per_beat = (double)self->rate * 60.0 / bpm;

  /* compute absolute beat positions at block start/end from host transport
   * information rather than relying on a drift-prone running counter.
   */
  const double beat_start = self->have_last_transport_beats ? self->last_transport_beats : 0.0;
  const double beat_end =
      beat_start + ((samples_per_beat > 1e-9) ? ((double)n_samples / samples_per_beat) : 0.0);

  const float pos0 = (float)fmod(beat_start, (double)self->bpb);
  const float pos1 = (float)fmod(beat_end, (double)self->bpb);

  const int old_beat_i = (int)floorf(pos0);
  const int new_beat_i = (int)floorf(pos1);

  if (self->have_last_transport_beats) {
    self->current_position = pos1;
  }

  /* If a beat boundary lands exactly at the start of the block, the
   * "crossing" test won't catch it (old_beat_i==new_beat_i), which caused us
   * to miss the START click when the downbeat aligns to block boundaries.
   */
  const float frac0                   = pos0 - floorf(pos0);
  const float kBeatBoundaryEps        = 1e-3f; /* beats (~0.5ms at 120 BPM) */
  const bool  boundary_at_block_start = (frac0 >= 0.0f && frac0 <= kBeatBoundaryEps);

  if (boundary_at_block_start || (new_beat_i != old_beat_i)) {
    uint32_t sample_offset  = 0;
    double   boundary_beats;

    int boundary_beat_i = old_beat_i;
    if (!boundary_at_block_start) {
      /* Compute the beat boundary inside this block (first integer beat crossed). */
      double beats_to_boundary = (double)(old_beat_i + 1) - (double)pos0;
      if (beats_to_boundary < 0.0) {
        beats_to_boundary = 0.0;
      }

      if (samples_per_beat > 1e-9) {
        /* Never fire early: ceil to the first sample at/after the boundary. */
        const double samples_until = beats_to_boundary * samples_per_beat;
        uint64_t     off           = (uint64_t)ceil(samples_until - 1e-9);
        if (off > (uint64_t)n_samples) {
          off = (uint64_t)n_samples;
        }
        sample_offset = (uint32_t)off;
      }

      /* Absolute transport beat position exactly at the beat boundary. */
      boundary_beats  = self->last_transport_beats + beats_to_boundary;
      boundary_beat_i = old_beat_i + 1;
    } else {
      /* Boundary at block start: snap boundary_beats to the integer beat. */
      boundary_beats = self->last_transport_beats - (double)frac0;
      if (boundary_beats < 0.0) {
        boundary_beats = 0.0;
      }
      sample_offset = 0;
    }

    const uint32_t bars_i         = alo_get_bars_i(self);
    bool           is_cycle_start = false;

    /* Which beat-in-bar is this boundary? (0 = downbeat). */
    const int beat_in_bar = boundary_beat_i % (int)alo_get_bpb_i(self);

    if (beat_in_bar == 0) {
      /* If resync is pending and the downbeat occurs within this block, lock
       * the Bars-cycle origin to this exact boundary.
       */
      if (self->ui_cycle_resync_pending && !self->ui_have_cycle_origin) {
        self->ui_cycle_origin_beats   = boundary_beats;
        self->ui_have_cycle_origin    = true;
        self->ui_cycle_resync_pending = false;
      }

      if (self->ui_have_cycle_origin && !self->ui_cycle_resync_pending) {
        const double bpb = (self->bpb > 1e-6f) ? (double)self->bpb : (double)DEFAULT_BEATS_PER_BAR;
        const double cycle_len = (double)(bars_i ? bars_i : 1u) * bpb;
        double       phase     = fmod(boundary_beats - self->ui_cycle_origin_beats, cycle_len);
        if (phase < 0.0) {
          phase += cycle_len;
        }

        const double kCycleEpsBeats = 1e-3;
        is_cycle_start = (phase <= kCycleEpsBeats) || ((cycle_len - phase) <= kCycleEpsBeats);

        /* Keep click_bar_in_cycle consistent with the same origin. */
        uint32_t bar_in_cycle = 0;
        if (bpb > 1e-9) {
          const double bar_phase = floor(phase / bpb);
          if (bar_phase >= 0.0) {
            bar_in_cycle = (uint32_t)bar_phase;
          }
        }
        if (bar_in_cycle >= bars_i) {
          bar_in_cycle = 0;
        }
        self->click_bar_in_cycle = bar_in_cycle;
      } else {
        /* Fallback before origin is established. */
        if (self->click_bar_in_cycle >= bars_i) {
          self->click_bar_in_cycle = 0;
        }
        is_cycle_start = (self->click_bar_in_cycle == 0u);
      }
    }

    if (can_click) {
      click_mix(self, 0, sample_offset);

      if (beat_in_bar == 0) {
        /* Downbeat: start click only on cycle start, otherwise normal accent. */
        self->high_beat_offset  = is_cycle_start ? self->beat_len : 0;
        self->low_beat_offset   = self->beat_len;
        self->start_beat_offset = is_cycle_start ? 0 : self->beat_len;
      } else {
        /* Other beats: low click only. */
        self->low_beat_offset   = 0;
        self->high_beat_offset  = self->beat_len;
        self->start_beat_offset = self->beat_len;
      }

      click_mix(self, sample_offset, n_samples);
    }

    if (beat_in_bar == 0) {
      /* When the origin isn't locked yet, advance the fallback counter on
       * downbeats.
       */
      if (!self->ui_have_cycle_origin || self->ui_cycle_resync_pending) {
        self->click_bar_in_cycle++;
        if (self->click_bar_in_cycle >= bars_i) {
          self->click_bar_in_cycle = 0;
        }
      }
    }
  } else {
    if (can_click) {
      click_mix(self, 0, n_samples);
    }
  }
}

/* -------------------------------------------------------------------------
 * Events (transport + MIDI + UI)
 * ------------------------------------------------------------------------- */

void run_events(Alo* self, const uint32_t n_samples)
{
  if (!self) {
    return;
  }

  const AloURIs* uris = &self->uris;

  /* 1) Transport first. */
  self->transport_updated_this_cycle = false;
  const LV2_Atom_Sequence* in        = self->ports.control;
  if (in) {
    LV2_ATOM_SEQUENCE_FOREACH(in, ev)
    {
      if (ev->body.type == uris->atom_Object || ev->body.type == uris->atom_Blank) {
        const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
        if (obj->body.otype == uris->time_Position) {
          update_position_from_atom(self, obj);
        }
      }
    }
  }

  /* If the host stops sending time:Position updates, treat that as stopped
   * after a short grace period.  However, if we know the host is running we
   * advance our internal beat counter anyway (predictive integration) so the
   * UI and click logic continue to move even when updates are sparse.  This
   * mirrors the strategy used by the stepseq example.
   */
  if (self->transport_updated_this_cycle) {
    self->transport_blocks_without_update = 0;
  } else if (self->have_transport) {
    if (self->have_speed && self->speed != 0.0f) {
      /* pretend we saw an update so we don't transition to "stopped"; then
       * increment last_transport_beats by the number of beats that should
       * have elapsed in this block.  Compose formula carefully to avoid
       * division by zero.
       */
      double samples_per_beat = (double)self->rate * 60.0 /
                                ((self->bpm > 1e-6f) ? (double)self->bpm : (double)DEFAULT_BPM);
      if (samples_per_beat > 0.0) {
        self->last_transport_beats += ((double)n_samples * (double)self->speed) / samples_per_beat;
      }
      self->transport_blocks_without_update = 0;
    } else {
      if (self->transport_blocks_without_update < UINT32_MAX) {
        self->transport_blocks_without_update++;
      }
    }
  }

  /* Transport stop/start resync: make the next downbeat be cycle start. */
  /* Consider the transport "stopped" when host speed is non‑positive.  This
   * matches the stepseq example and avoids treating a reverse/zero speed as a
   * continued running state.  Other conditions cover jack-derived stop/start or
   * long gaps without updates. */
  const bool stopped_now = (self->have_speed && self->speed <= 0.0f) ||
                           (self->have_transport && self->have_last_transport_beats &&
                            self->transport_updated_this_cycle && !self->transport_moving) ||
                           (self->have_transport && self->transport_blocks_without_update > 2u);

  if (stopped_now != self->ui_transport_was_stopped) {
    self->ui_transport_was_stopped = stopped_now;
    request_ui_cycle_resync(self);

    /* Always clear pending actions and sync button edge tracking on transport
     * edges to prevent accidental triggers.
     */
    clear_inflight_actions_and_sync_controls(self);

    /* Transport resumed: do not reset loop origin.
     * If the host keeps beat position stable during stop, this allows a true
     * pause/resume (phase-continuous). If the host actually rewinds/locates,
     * update_transport_beats() handles that jump without deleting audio.
     */
  }

  /* Hard rule: while transport is stopped, do not accept UI/MIDI actions.
   * Many hosts leave toggled controls high or resend control values on
   * transport changes; we don't want that to arm/undo/record.
   */
  if (stopped_now) {
    clear_inflight_actions_and_sync_controls(self);
    update_bar_step_out(self);
    update_loop_state_ports(self);
    return;
  }

  /*
   * Bars changes are a blocking operation: treat them like a disable/enable
   * to guarantee the engine is fully resynchronized (no drifted loop origin
   * or UI cycle offsets). Preserve transport position so we stay phase-locked.
   */
  {
    const uint32_t bars_i = alo_get_bars_i(self);
    if (bars_i != self->ui_last_bars_i) {
      const bool     have_pos          = self->have_last_transport_beats;
      const double   global_beats      = self->last_transport_beats;
      const bool     have_speed        = self->have_speed;
      const float    speed             = self->speed;
      const bool     moving            = self->transport_moving;
      const uint32_t blocks_wo         = self->transport_blocks_without_update;
      const bool     transport_updated = self->transport_updated_this_cycle;

      reset(self);

      self->have_speed                      = have_speed;
      self->speed                           = speed;
      self->transport_moving                = moving;
      self->transport_blocks_without_update = blocks_wo;
      self->transport_updated_this_cycle    = transport_updated;

      if (have_pos) {
        self->have_transport            = true;
        self->have_last_transport_beats = true;
        self->last_transport_beats      = global_beats;

        const float bpb        = (self->bpb > 1e-6f) ? self->bpb : (float)DEFAULT_BEATS_PER_BAR;
        self->current_position = (bpb > 0.0f) ? fmodf((float)global_beats, bpb) : 0.0f;
        update_transport_phase(self, global_beats);
      }
    }
  }

  const bool sampler_enabled = true;
  /* 2) MIDI slice triggers (one-shot, sample-accurate start within the block). */
  const LV2_Atom_Sequence* midiin = self->ports.midiin;
  if (midiin) {
    /* Only accept MIDI slice triggers when sampler output is enabled and we
     * have committed loop audio.
     */
    if (!sampler_enabled) {
      goto midi_done;
    }

    /* Do not accept triggers while the sampler source cache is invalid.
     * We intentionally allow triggers during a background rebuild so that
     * existing voices continue using the old mix and new notes still fire.
     */
    if (!self->sampler_src_valid || !self->sampler_src_buf) {
      goto midi_done;
    }

    bool any_committed_audio = false;
    for (int t = 0; t < NUM_TRACKS; ++t) {
      if (self->have_loop[t]) {
        any_committed_audio = true;
        break;
      }
    }
    if (!any_committed_audio) {
      goto midi_done;
    }

    const int      root           = get_slice_root_note(self);
    const uint32_t bars_i         = alo_get_bars_i(self);
    const uint32_t slices_per_bar = alo_get_slices_per_bar_u(self);
    const uint32_t slice_count    = bars_i * slices_per_bar;

    LV2_ATOM_SEQUENCE_FOREACH(midiin, ev)
    {
      if (ev->body.type != self->uris.midi_MidiEvent) {
        continue;
      }

      if (ev->body.size < 3u) {
        /* Ignore truncated/invalid MIDI events. */
        continue;
      }

      const uint8_t* const msg = (const uint8_t*)(ev + 1);
      const uint8_t        typ = lv2_midi_message_type(msg);
      if (typ != LV2_MIDI_MSG_NOTE_ON) {
        continue;
      }

      const int note = (int)msg[1];
      const int vel  = (int)msg[2];
      if (vel <= 0) {
        continue;
      }

      const int slice_index = note - root;
      if (slice_index < 0 || slice_count == 0 || (uint32_t)slice_index >= slice_count) {
        continue;
      }

      if (self->loop_samples == 0) {
        continue;
      }

      const uint64_t loop_s = (uint64_t)self->loop_samples;
      const uint64_t s0     = ((uint64_t)slice_index * loop_s) / (uint64_t)slice_count;
      const uint64_t s1     = ((uint64_t)(slice_index + 1) * loop_s) / (uint64_t)slice_count;
      if (s1 <= s0) {
        continue;
      }

      const uint32_t phase_samples = (uint32_t)s0;
      const uint32_t slice_len     = (uint32_t)(s1 - s0);

      uint32_t start_offset_samples = ev->time.frames;
      if (n_samples > 0u && start_offset_samples >= n_samples) {
        /* Some hosts/plugins may emit out-of-range timestamps.
         * Clamp into this block so dense retriggers don't get deferred into
         * the pending queue (which can cause dropouts/silence).
         */
        start_offset_samples = n_samples - 1u;
      }

      /* Short fade-in/out to avoid clicks at slice edges (RT-safe).
       * Clamp to a sensible range so very high sample rates don't over-fade.
       */
      uint32_t fade_samples = 0u;
      if (self->rate > 1e-6) {
        const double fade_s = 0.001; /* 1ms */
        uint64_t     fs     = (uint64_t)llround((double)self->rate * fade_s);
        if (fs < 16u) {
          fs = 16u;
        } else if (fs > 512u) {
          fs = 512u;
        }
        fade_samples = (uint32_t)fs;
      }

      alo_slice_sampler_schedule(&self->slice_sampler, self, start_offset_samples, phase_samples,
                                 slice_len, fade_samples, 1.0f);
    }
  }

midi_done:

  /* 3) UI edges (always active). */
  for (int t = 0; t < NUM_TRACKS; ++t) {
    const bool loop_btn = alo_port_pressed(self->ports.loop_btn[t]);
    const bool undo_btn = alo_port_pressed(self->ports.undo_btn[t]);
    handle_button_edges(self, t, loop_btn, undo_btn);
  }

  update_bar_step_out(self);

  update_loop_state_ports(self);
}

/* -------------------------------------------------------------------------
 * Main loop engine
 * ------------------------------------------------------------------------- */

void run_loops(Alo* self, uint32_t n_samples)
{
  if (!self) {
    return;
  }

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

  const bool sampler_enabled = true;
  // const float duck_amt = (self->ports.sidechain_amt) ? fmaxf(0.0f,
  // fminf(*(self->ports.sidechain_amt), 1.0f)) : 0.0f;

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
    float v = 1.0f;
    if (self->ports.loop_vol[t]) {
      v = *(self->ports.loop_vol[t]);
    }
    if (v < 0.0f) {
      v = 0.0f;
    } else if (v > 1.0f) {
      v = 1.0f;
    }
    track_gain[t] = v;
  }

  float sampler_gain = 1.0f;
  if (self->ports.sampler_vol) {
    sampler_gain = *(self->ports.sampler_vol);
  }
  // Allow full range -2.5..2.5 for sampler gain
  if (sampler_gain < -2.5f)
    sampler_gain = -2.5f;
  if (sampler_gain > 2.5f)
    sampler_gain = 2.5f;

  const bool transport_running =
      self->have_transport && self->have_last_transport_beats &&
      (self->transport_blocks_without_update <= 2u) && (!self->have_speed || self->speed != 0.0f) &&
      (self->transport_updated_this_cycle ? self->transport_moving : true);
  // const bool just_resumed = transport_running && !self->transport_prev_running;
  // self->transport_prev_running = transport_running;

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
  float* const   play_l_s  = self->rt_play_l;
  float* const   play_r_s  = self->rt_play_r;
  float* const   slice_l_s = self->rt_slice_l;
  float* const   slice_r_s = self->rt_slice_r;

  uint32_t block_base = 0u;
  while (block_base < n_samples) {
    uint32_t blk = n_samples - block_base;
    if (cap > 0u && blk > cap) {
      blk = cap;
    }

    /* Only compute loop playback if transport allows and we have any
     * committed slot.  When skip_loops is true the scratch buffers should
     * remain zero so that the output becomes dry+slice only.
     */
    bool any_play = false;
    if (!skip_loops) {
      for (int t = 0; t < NUM_TRACKS; ++t) {
        if (self->have_loop[t] && self->loop_buf[t]) {
          any_play = true;
          break;
        }
      }
    }

    memset(play_l_s, 0, sizeof(float) * (size_t)blk);
    memset(play_r_s, 0, sizeof(float) * (size_t)blk);
    memset(slice_l_s, 0, sizeof(float) * (size_t)blk);
    memset(slice_r_s, 0, sizeof(float) * (size_t)blk);

    if (any_play) {
      /* local copies of phase & playhead so the advance logic is shared with
       * the later event/recording loop; we do not update the struct until the
       * second pass so that badge values remain unchanged for external
       * observers during the first pass.
       */
      uint32_t       phase_loc    = self->loop_phase;
      double         playhead_loc = self->loop_playhead;
      const uint32_t end_idx      = self->loop_start + self->loop_samples;

      for (uint32_t pos = 0; pos < blk; ++pos) {
        uint32_t idx   = self->loop_start + phase_loc;
        uint32_t idx_r = idx + LOOP_SIZE;
        /* fractional interp */
        double   frac     = playhead_loc - (double)idx;
        uint32_t idx_next = idx + 1;
        if (idx_next >= end_idx) {
          idx_next = self->loop_start;
        }
        uint32_t idx_next_r = idx_next + LOOP_SIZE;

        for (int t = 0; t < NUM_TRACKS; ++t) {
          if (!self->have_loop[t] || !self->loop_buf[t]) {
            continue;
          }

          const float g = track_gain[t];
          /* interpolate left/right samples */
          float l0 = self->loop_buf[t][idx];
          float r0 = self->loop_buf[t][idx_r];
          float l1 = self->loop_buf[t][idx_next];
          float r1 = self->loop_buf[t][idx_next_r];
          float il = (float)((1.0 - frac) * (double)l0 + frac * (double)l1);
          float ir = (float)((1.0 - frac) * (double)r0 + frac * (double)r1);
          play_l_s[pos] += g * il;
          play_r_s[pos] += g * ir;

          const uint8_t n_layers = self->od_count[t];
          for (uint8_t l = 0; l < n_layers; ++l) {
            const float* const buf = self->od_buf[t][l];
            if (!buf) {
              continue;
            }
            float bl0 = buf[idx];
            float br0 = buf[idx_r];
            float bl1 = buf[idx_next];
            float br1 = buf[idx_next_r];
            float ibl = (float)((1.0 - frac) * (double)bl0 + frac * (double)bl1);
            float ibr = (float)((1.0 - frac) * (double)br0 + frac * (double)br1);
            play_l_s[pos] += g * ibl;
            play_r_s[pos] += g * ibr;
          }
        }

        /* advance local playhead/phase without modulo */
        playhead_loc += 1.0;
        phase_loc++;
        if (phase_loc >= self->loop_samples) {
          phase_loc = 0;
        }
        if (playhead_loc >= (double)end_idx) {
          playhead_loc -= (double)self->loop_samples;
        }
      }
      /* struct values remain unchanged until after the second loop */
    }

    const bool sampler_busy2 = alo_slice_sampler_is_busy(&self->slice_sampler);
    if (sampler_enabled && sampler_gain > 0.0f && sampler_busy2) {
      float slice_gain_scale = 1.0f;
      {
        const uint32_t slices = alo_get_slices_per_bar_u(self);
        if (slices > 1u) {
          slice_gain_scale = 1.0f / sqrtf((float)slices);
        }
      }
      alo_slice_sampler_process_chunk(&self->slice_sampler, self, block_base, blk, slice_l_s,
                                      slice_r_s);
      for (uint32_t pos = 0; pos < blk; ++pos) {
        slice_l_s[pos] *= slice_gain_scale;
        slice_r_s[pos] *= slice_gain_scale;
      }
    }

    /* carry our current phase/playhead into locals so we can increment
     * them exactly once per sample and then store back to the struct at the end
     * of the block.
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

      /* At-bar detection using an incremental counter. */
      const bool at_bar = (bar_len > 0u && bar_phase == 0u);
      if (at_bar) {
        if (apply_pending_undo_at_bar(self)) {
          self->sampler_src_dirty = true;
        }
      }

      uint32_t idx   = self->loop_start + phase_loc;
      uint32_t idx_r = idx + LOOP_SIZE;

      const float in_l = input_l[pos_in_block];
      const float in_r = input_r[pos_in_block];

      float slice_l = 0.0f;
      float slice_r = 0.0f;
      if (sampler_enabled && sampler_gain > 0.0f && sampler_busy2) {
        slice_l = slice_l_s[pos];
        slice_r = slice_r_s[pos];
      }
      slice_l *= sampler_gain;
      slice_r *= sampler_gain;

      /* Dry input + precomputed playback + slice one-shots */
      float out_l_val        = inmix * in_l + play_l_s[pos] + slice_l;
      float out_r_val        = inmix * in_r + play_r_s[pos] + slice_r;
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
              if (self->ports.bar_step_out) {
                *(self->ports.bar_step_out) = 0.0f;
              }
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
              if (self->ports.bar_step_out) {
                *(self->ports.bar_step_out) = 0.0f;
              }
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
