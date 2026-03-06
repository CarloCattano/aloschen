#include "alo_engine.h"

#include "lv2/atom/util.h"

#include <math.h>
#include <string.h>

/*
 * loop_engine.c — core looper DSP/engine (simplified)
 *
 * Behavior (per track, 3 tracks total):
 * - First ever record (any track): arms on press, starts on next bar downbeat,
 *   records exactly one loop length, auto-stops, button auto-off.
 * - Subsequent presses on an empty track: arms base, starts on next loop boundary
 *   (phase-aligned to the first loop), records one loop, auto-stops.
 * - Press on a track that already has audio: arms overdub, starts on next loop
 *   boundary, overdubs for one loop, auto-stops.
 * - Undo: if an overdub happened, toggles between current and previous buffer
 *   (one-level undo/redo). If only a base exists, clears the track.
 *
 * Real-time: no malloc/free in run_events/run_loops/run_clicks.
 */

static inline bool track_is_active(const Alo* self, int t) {
  (void)self;
  return t >= 0 && t < NUM_TRACKS;
}

static inline bool track_is_busy(const Alo* self, int t) {
  return track_is_active(self, t) && self->track_state[t] != TRACK_IDLE;
}

static inline bool port_is_pressed(const float* p) {
  return p && (*p > 0.0f);
}

static inline float soft_clip_unit(float x) {
  /* Smoothly bounds signal to (-1, 1) without hard discontinuities. */
  return x / (1.0f + fabsf(x));
}

static void clear_track_state(Alo* self, int t) {
  self->track_state[t] = TRACK_IDLE;
  self->rec_remaining_samples[t] = 0;
}

static void clear_track_audio(Alo* self, int t) {
  if (!self || !track_is_active(self, t)) {
    return;
  }

  self->have_loop[t] = false;
  self->od_count[t] = 0;
  self->pending_undo[t] = 0;
  self->pending_clear_all[t] = false;
}

static void clear_inflight_actions_and_sync_controls(Alo* self) {
  if (!self) {
    return;
  }

  /* Cancel anything that could fire on the next downbeat/boundary. */
  for (int t = 0; t < NUM_TRACKS; ++t) {
    clear_track_state(self, t);
    self->pending_undo[t] = 0;
    self->pending_clear_all[t] = false;

    /* Prime edge detectors from current port values, so we don't treat a
     * latched/toggled "1" (or host glitches) as a new press after transport
     * stop/start.
     */
    const bool loop_btn = port_is_pressed(self->ports.loop_btn[t]);
    const bool undo_btn = port_is_pressed(self->ports.undo_btn[t]);
    self->last_loop_input[t] = loop_btn;
    self->last_undo_input[t] = undo_btn;
    self->loop_btn_high_frames[t] = loop_btn ? 1u : 0u;
  }
}

static inline void request_ui_cycle_resync(Alo* self) {
  if (!self) {
    return;
  }
  self->click_bar_in_cycle = 0;
  self->ui_cycle_resync_pending = true;
  self->ui_have_cycle_origin = false;
  self->ui_have_prev_bar_beat = false;
  /* Force a step-0 update to UIs on the next call. */
  self->ui_last_bar_step = -2;
}

static inline float loop_state_value(const Alo* self, const int t, const bool transport_stopped) {
  /* Encoded states (used by native + MOD UIs):
   * 0.0  = off/empty
   * 0.25 = armed (blink)
   * 0.5  = playing (solid)
   * 1.0  = recording (solid)
   */
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

static inline float undo_state_value(const Alo* self, const int t) {
  /* 0=off, 0.25=queued (blink in UI) */
  const bool queued = (self->pending_clear_all[t] || self->pending_undo[t] > 0);
  return queued ? 0.25f : 0.0f;
}

static inline float has_audio_value(const Alo* self, const int t) {
  return self->have_loop[t] ? 1.0f : 0.0f;
}

void update_loop_state_ports(Alo* self) {
  if (!self) {
    return;
  }

  const bool transport_stopped =
      self->have_transport && self->have_speed && (self->speed == 0.0f);

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

/* -------------------------------------------------------------------------
 * Timing helpers
 * ------------------------------------------------------------------------- */

static uint32_t get_bars_i(const Alo* self) {
  float bars_f = (self && self->ports.bars) ? *(self->ports.bars)
                                            : (float)DEFAULT_NUM_BARS;
  if (bars_f < 1.0f) {
    bars_f = 1.0f;
  }
  if (bars_f > 16.0f) {
    bars_f = 16.0f;
  }
  uint32_t bars_i = (uint32_t)lrintf(bars_f);
  if (!bars_i) {
    bars_i = 1u;
  }
  return bars_i;
}

static double compute_next_cycle_start_beats(const Alo* self, double global_beats0) {
  if (!self) {
    return global_beats0;
  }

  const double bpb = (self->bpb > 1e-6f) ? (double)self->bpb : (double)DEFAULT_BEATS_PER_BAR;
  if (!(bpb > 0.0)) {
    return global_beats0;
  }

  const uint32_t bars_i = get_bars_i(self);
  const double cycle_len_beats = (double)(bars_i ? bars_i : 1u) * bpb;
  if (!(cycle_len_beats > 0.0)) {
    return global_beats0;
  }

  /* Phase within the Bars-length cycle: [0, cycle_len_beats). */
  double phase = fmod(global_beats0, cycle_len_beats);
  if (phase < 0.0) {
    phase += cycle_len_beats;
  }

  /* If we're effectively on the boundary, start now (avoid drifting a cycle). */
  const double kCycleEpsBeats = 1e-3; /* ~0.5ms at 120 BPM */
  if (phase <= kCycleEpsBeats || (cycle_len_beats - phase) <= kCycleEpsBeats) {
    return global_beats0;
  }

  /* Next cycle start. */
  return global_beats0 + (cycle_len_beats - phase);
}

static uint32_t compute_loop_beats(const Alo* self) {
  if (!self) {
    return DEFAULT_BEATS_PER_BAR * DEFAULT_NUM_BARS;
  }

  const float bpb_f = (self->bpb > 0.0f) ? self->bpb : (float)DEFAULT_BEATS_PER_BAR;
  const uint32_t bpb_i = (uint32_t)lrintf(bpb_f);

  const uint32_t bars_i = get_bars_i(self);

  const uint32_t bpb_ok = bpb_i ? bpb_i : DEFAULT_BEATS_PER_BAR;
  const uint32_t bars_ok = bars_i ? bars_i : DEFAULT_NUM_BARS;
  return bpb_ok * bars_ok;
}

static uint32_t compute_loop_samples(const Alo* self, uint32_t loop_beats) {
  if (!self) {
    return LOOP_SIZE;
  }
  const float bpm = (self->bpm > 1e-6f) ? self->bpm : (float)DEFAULT_BPM;
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

static bool compute_transport_phase_index(const Alo* self, double global_beats, uint32_t* out_phase_samples) {
  if (!self || !out_phase_samples) {
    return false;
  }

  const uint32_t loop_beats = self->loop_beats;
  const uint32_t loop_samples = self->loop_samples;
  if (!(loop_beats > 0u && loop_samples > 0u)) {
    return false;
  }

  const double origin = self->have_loop_origin ? self->loop_origin_beats : 0.0;
  double phase_beats = fmod(global_beats - origin, (double)loop_beats);
  if (phase_beats < 0.0) {
    phase_beats += (double)loop_beats;
  }

  double phase_samples_d = phase_beats * (double)loop_samples / (double)loop_beats;
  if (phase_samples_d < 0.0) {
    phase_samples_d = 0.0;
  }
  uint32_t phase_samples = (uint32_t)floor(phase_samples_d);
  if (phase_samples >= loop_samples) {
    phase_samples = loop_samples - 1;
  }

  *out_phase_samples = phase_samples;
  return true;
}

void reset_timing(Alo* self) {
  if (!self) {
    return;
  }

  const uint32_t new_loop_beats = compute_loop_beats(self);
  const uint32_t new_loop_samples = compute_loop_samples(self, new_loop_beats);

  self->loop_beats = new_loop_beats;
  self->loop_samples = new_loop_samples ? new_loop_samples : 1u;

  if (self->loop_samples > LOOP_SIZE) {
    self->loop_samples = LOOP_SIZE;
  }

  if (self->loop_index >= self->loop_start + self->loop_samples) {
    self->loop_index = self->loop_start;
  }
  if (self->transport_loop_index >= self->loop_samples) {
    self->transport_loop_index %= self->loop_samples;
  }

  /*
   * Bars/tempo changes must keep playback phase aligned to host transport.
   * Recompute the transport phase immediately from the most recent position.
   */
  if (self->have_last_transport_beats) {
    uint32_t phase_samples = 0;
    if (compute_transport_phase_index(self, self->last_transport_beats, &phase_samples)) {
      self->transport_loop_index = phase_samples;
      self->transport_loop_index_pending = true;
      self->have_transport = true;
    }
  }

  /* Timing changes can shift downbeat/phase; force UI + cycle resync. */
  request_ui_cycle_resync(self);
  /* Drop any in-flight click envelope so we don't smear across tempo changes. */
  self->high_beat_offset = self->beat_len;
  self->low_beat_offset = self->beat_len;
  self->start_beat_offset = self->beat_len;

  update_loop_state_ports(self);
}

void reset(Alo* self) {
  if (!self) {
    return;
  }

  for (int t = 0; t < NUM_TRACKS; ++t) {
    self->have_loop[t] = false;
    self->od_count[t] = 0;
    self->rec_od_layer[t] = 0;
    self->pending_undo[t] = 0;
    self->pending_clear_all[t] = false;
    clear_track_state(self, t);

    self->last_loop_input[t] = false;
    self->last_undo_input[t] = false;
    self->loop_btn_high_frames[t] = 0;
  }

  self->loop_beats = compute_loop_beats(self);
  self->loop_samples = compute_loop_samples(self, self->loop_beats);
  if (!self->loop_samples) {
    self->loop_samples = 1u;
  }

  self->loop_start = 0;
  self->loop_index = 0;

  self->have_transport = false;
  self->transport_moving = false;
  self->transport_updated_this_cycle = false;
  self->transport_blocks_without_update = 0;
  self->have_speed = false;
  self->speed = 0.0f;
  self->transport_loop_index = 0;
  self->transport_loop_index_pending = false;

  self->have_last_bar_beat = false;
  self->last_bar_beat = 0.0f;
  self->bar_counter_fallback = 0;

  self->last_transport_beats = 0.0;
  self->have_last_transport_beats = false;

  self->loop_origin_beats = 0.0;
  self->have_loop_origin = false;

  /* Reset click envelopes. */
  self->high_beat_offset = self->beat_len;
  self->low_beat_offset = self->beat_len;
  self->start_beat_offset = self->beat_len;
  self->click_bar_in_cycle = 0;

  self->ui_last_bar_step = -2;
  self->ui_last_bars_i = get_bars_i(self);
  self->ui_cycle_resync_pending = true;
  self->ui_transport_was_stopped = true;
  self->ui_cycle_origin_beats = 0.0;
  self->ui_have_cycle_origin = false;
  self->ui_prev_bar_beat = 0.0f;
  self->ui_have_prev_bar_beat = false;

  /* Reset UI phase outputs immediately (they otherwise only update when
   * update_bar_step_out() runs).
   */
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

static void update_bar_step_out(Alo* self) {
  if (!self) {
    return;
  }

  const bool transport_stopped = (self->have_speed && self->speed == 0.0f);

  /* If Bars changed, restart the cycle on the next downbeat. */
  const uint32_t bars_i = get_bars_i(self);
  if (bars_i != self->ui_last_bars_i) {
    self->ui_last_bars_i = bars_i;
    request_ui_cycle_resync(self);
  }

  /* Default to showing the first position when stopped / not yet synced. */
  int step = 0;

  if (transport_stopped || !self->have_transport || !self->have_last_transport_beats) {
    /* Stopped/unknown: show step 0 and force next start to resync. */
    request_ui_cycle_resync(self);
    step = 0;
  } else {
    float bar_beat = self->current_position;
    if (!(bar_beat >= 0.0f)) {
      bar_beat = 0.0f;
    }

    bool downbeat_edge = false;
    if (self->ui_have_prev_bar_beat) {
      /* Detect wrap (e.g. 3.9 -> 0.1) at bar boundary. */
      if (bar_beat + 0.25f < self->ui_prev_bar_beat) {
        downbeat_edge = true;
      }
    }
    self->ui_prev_bar_beat = bar_beat;
    self->ui_have_prev_bar_beat = true;

    if (self->ui_cycle_resync_pending && !self->ui_have_cycle_origin) {
      /* Set cycle origin on the first downbeat after (re)sync. */
      const float kDownbeatGraceBeats = 0.25f;
      if (downbeat_edge || bar_beat <= kDownbeatGraceBeats) {
        self->ui_cycle_origin_beats = self->last_transport_beats - (double)bar_beat;
        self->ui_have_cycle_origin = true;
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
    float cycle_phase = 0.0f;

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
      const uint32_t bars_i2 = get_bars_i(self);
      const uint32_t steps_u = bars_i2 * (uint32_t)DEFAULT_BEATS_PER_BAR;
      if (steps_u > 0 && self->ui_have_cycle_origin && !self->ui_cycle_resync_pending) {
        double phase_beats = self->last_transport_beats - self->ui_cycle_origin_beats;
        if (phase_beats < 0.0) {
          phase_beats = 0.0;
        }
        phase_beats = fmod(phase_beats, (double)steps_u);
        if (phase_beats < 0.0) {
          phase_beats += (double)steps_u;
        }
        cycle_phase = (float)(phase_beats / (double)steps_u);
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
 * Transport update
 * ------------------------------------------------------------------------- */

static void update_transport_beats(Alo* self, double global_beats) {
  if (!self) {
    return;
  }

  /* Transport is considered present as soon as we receive beat position. */
  self->have_transport = true;

  if (self->have_last_transport_beats) {
    /* Primary signal: beat position advancing. */
    if (global_beats > self->last_transport_beats + 1e-6) {
      self->transport_moving = true;
    } else {
      /* If beat position is not advancing, treat transport as stopped even if
       * the host doesn't provide (or misreports) time:speed.
       */
      self->transport_moving = false;
    }
  } else {
    /* First ever position: only claim "moving" if the host explicitly
     * provides a non-zero speed.
     */
    self->transport_moving = (self->have_speed && self->speed != 0.0f);
  }

  /* Strong stop signal: speed==0 always means stopped. */
  if (self->have_speed && self->speed == 0.0f) {
    self->transport_moving = false;
  }

  if (self->have_last_transport_beats && global_beats < self->last_transport_beats - 0.5) {
    reset(self);
  }

  self->last_transport_beats = global_beats;
  self->have_last_transport_beats = true;
}

static void update_transport_phase(Alo* self, double global_beats) {
  if (!self) {
    return;
  }

  uint32_t phase_samples = 0;
  if (!compute_transport_phase_index(self, global_beats, &phase_samples)) {
    return;
  }

  self->transport_loop_index = phase_samples;
  self->transport_loop_index_pending = true;
}

static void update_position_from_atom(Alo* self, const LV2_Atom_Object* obj) {
  if (!self || !obj) {
    return;
  }

  /* Mark that we saw transport info this cycle (used for stop detection). */
  self->transport_updated_this_cycle = true;

  AloURIs* const uris = &self->uris;

  LV2_Atom *abs_beat = NULL, *beat = NULL, *bar = NULL;
  LV2_Atom *bpm = NULL, *bpb = NULL, *speed = NULL;

  lv2_atom_object_get(obj,
                      uris->time_beat, &abs_beat,
                      uris->time_barBeat, &beat,
                      uris->time_beatsPerMinute, &bpm,
                      uris->time_speed, &speed,
                      uris->time_beatsPerBar, &bpb,
                      uris->time_bar, &bar,
                      NULL);

  if (bpb && bpb->type == uris->atom_Float) {
    const float new_bpb = ((LV2_Atom_Float*)bpb)->body;
    if (fabsf(self->bpb - new_bpb) > 0.01f) {
      self->bpb = new_bpb;
      reset_timing(self);
    }
  }

  if (compute_loop_beats(self) != self->loop_beats) {
    reset_timing(self);
  }

  if (bpm && bpm->type == uris->atom_Float) {
    const float new_bpm = ((LV2_Atom_Float*)bpm)->body;
    if (fabsf(self->bpm - new_bpm) > 0.01f) {
      self->bpm = new_bpm;
      reset_timing(self);
    }
  }

  if (speed && speed->type == uris->atom_Float) {
    self->have_speed = true;
    self->speed = ((LV2_Atom_Float*)speed)->body;
  }

  if (abs_beat && abs_beat->type == uris->atom_Float) {
    const double global_beats = (double)((LV2_Atom_Float*)abs_beat)->body;

    update_transport_beats(self, global_beats);

    const float bar_beat =
        (self->bpb > 0.0f) ? fmodf((float)global_beats, self->bpb) : 0.0f;
    self->current_position = bar_beat;

    update_transport_phase(self, global_beats);

    return;
  }

  /* Fallback: barBeat + (optional) bar index. */
  if (beat && beat->type == uris->atom_Float) {
    self->current_position = ((LV2_Atom_Float*)beat)->body;

    int64_t bar_index = 0;
    if (bar) {
      if (bar->type == uris->atom_Float) {
        bar_index = (int64_t)floorf(((LV2_Atom_Float*)bar)->body);
      } else if (bar->type == uris->atom_Long) {
        bar_index = (int64_t)((const LV2_Atom_Long*)bar)->body;
      }
    }

    const float bar_beat2 = self->current_position;
    if (!bar) {
      if (self->have_last_bar_beat) {
        if (bar_beat2 + 0.25f < self->last_bar_beat) {
          self->bar_counter_fallback++;
        }
      }
      bar_index = self->bar_counter_fallback;
    }
    self->last_bar_beat = bar_beat2;
    self->have_last_bar_beat = true;

    if (self->loop_beats > 0 && self->loop_samples > 0) {
      const double global_beats =
          (double)bar_index * (double)self->bpb + (double)bar_beat2;

      update_transport_beats(self, global_beats);
      update_transport_phase(self, global_beats);
    }
  }
}

/* -------------------------------------------------------------------------
 * Button handling
 * ------------------------------------------------------------------------- */

static void handle_loop_press(Alo* self, int t) {
  if (!self || !track_is_active(self, t)) {
    return;
  }

  /* If the user presses while armed/recording, treat it as cancel/abort. */
  if (track_is_busy(self, t)) {
    clear_track_state(self, t);
    update_loop_state_ports(self);
    return;
  }

  if (!self->have_loop[t]) {
    /*
     * First-ever base recording defines the loop origin.
     * Force the Bars-cycle (UI stepbar + START click) to restart so the
     * quantized downbeat we start on is always step 0 / bar 1.
     */
    if (!self->have_loop_origin) {
      request_ui_cycle_resync(self);
      if (self->have_last_transport_beats) {
        /* Keep UI step origin aligned with the exact same target as audio. */
        self->ui_cycle_origin_beats = compute_next_cycle_start_beats(self, self->last_transport_beats);
        self->ui_have_cycle_origin = true;
        self->ui_cycle_resync_pending = false;
      }
    }
    self->track_state[t] = TRACK_ARM_BASE;
    self->rec_remaining_samples[t] = 0;
  } else {
    self->track_state[t] = TRACK_ARM_OVERDUB;
    self->rec_remaining_samples[t] = 0;
  }
}

static void handle_undo_press(Alo* self, int t) {
  if (!self || !track_is_active(self, t)) {
    return;
  }

  /* Stop any pending action/recording immediately (quantized audio change later). */
  clear_track_state(self, t);

  /*
   * Quantized undo:
   * - First press schedules one undo at the next bar downbeat.
   * - Additional presses before that downbeat clear the entire slot.
   * - Triple tap before the downbeat clears immediately (no waiting for sync).
   */
  if (self->pending_clear_all[t]) {
    clear_track_audio(self, t);
    return;
  }

  if (self->pending_undo[t] > 0) {
    self->pending_clear_all[t] = true;
    return;
  }

  if (self->pending_undo[t] < UINT8_MAX) {
    self->pending_undo[t]++;
  }
}

static void handle_button_edges(Alo* self, int t, bool loop_btn, bool undo_btn) {
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

static inline void apply_pending_undo_at_bar(Alo* self) {
  for (int t = 0; t < NUM_TRACKS; ++t) {
    if (self->pending_clear_all[t]) {
      clear_track_audio(self, t);
      continue;
    }

    while (self->pending_undo[t] > 0) {
      if (self->od_count[t] > 0) {
        self->od_count[t]--;
      } else if (self->have_loop[t]) {
        self->have_loop[t] = false;
      }
      self->pending_undo[t]--;
    }
  }
}

/* -------------------------------------------------------------------------
 * Click
 * ------------------------------------------------------------------------- */

static void click_mix(Alo* self, uint32_t begin, uint32_t end) {
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

void run_clicks(Alo* self, uint32_t n_samples) {
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

  const double bpm = (self->bpm > 1e-6f) ? (double)self->bpm : (double)DEFAULT_BPM;
  const double samples_per_beat = (double)self->rate * 60.0 / bpm;

  const float pos0 = self->current_position;
  const float delta_beats = (float)((samples_per_beat > 1e-9) ? ((double)n_samples / samples_per_beat) : 0.0);
  const float pos1 = pos0 + delta_beats;

  const int old_beat_i = (int)floorf(pos0);
  const int new_beat_i = (int)floorf(pos1);

  /* Keep current_position as a beat-in-bar phase for the next call. */
  self->current_position = fmodf(pos1, self->bpb);
  if (self->current_position < 0.0f) {
    self->current_position += self->bpb;
  }

  /* If a beat boundary lands exactly at the start of the block, the
   * "crossing" test won't catch it (old_beat_i==new_beat_i), which caused us
   * to miss the START click when the downbeat aligns to block boundaries.
   */
  const float frac0 = pos0 - floorf(pos0);
  const float kBeatBoundaryEps = 1e-3f; /* beats (~0.5ms at 120 BPM) */
  const bool boundary_at_block_start = (frac0 >= 0.0f && frac0 <= kBeatBoundaryEps);

  if (boundary_at_block_start || (new_beat_i != old_beat_i)) {
    uint32_t sample_offset = 0;
    double boundary_beats = self->last_transport_beats;

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
        uint64_t off = (uint64_t)ceil(samples_until - 1e-9);
        if (off > (uint64_t)n_samples) {
          off = (uint64_t)n_samples;
        }
        sample_offset = (uint32_t)off;
      }

      /* Absolute transport beat position exactly at the beat boundary. */
      boundary_beats = self->last_transport_beats + beats_to_boundary;
      boundary_beat_i = old_beat_i + 1;
    } else {
      /* Boundary at block start: snap boundary_beats to the integer beat. */
      boundary_beats = self->last_transport_beats - (double)frac0;
      if (boundary_beats < 0.0) {
        boundary_beats = 0.0;
      }
      sample_offset = 0;
    }

    const uint32_t bars_i = get_bars_i(self);
    bool is_cycle_start = false;

    /* Which beat-in-bar is this boundary? (0 = downbeat). */
    int bpb_i = (int)lrintf((self->bpb > 1e-6f) ? self->bpb : (float)DEFAULT_BEATS_PER_BAR);
    if (bpb_i < 1) {
      bpb_i = DEFAULT_BEATS_PER_BAR;
    }
    const int beat_in_bar = (bpb_i > 0) ? (boundary_beat_i % bpb_i) : 0;

    if (beat_in_bar == 0) {
      /* If resync is pending and the downbeat occurs within this block, lock
       * the Bars-cycle origin to this exact boundary.
       */
      if (self->ui_cycle_resync_pending && !self->ui_have_cycle_origin) {
        self->ui_cycle_origin_beats = boundary_beats;
        self->ui_have_cycle_origin = true;
        self->ui_cycle_resync_pending = false;
      }

      if (self->ui_have_cycle_origin && !self->ui_cycle_resync_pending) {
        const double bpb = (self->bpb > 1e-6f) ? (double)self->bpb : (double)DEFAULT_BEATS_PER_BAR;
        const double cycle_len = (double)(bars_i ? bars_i : 1u) * bpb;
        double phase = fmod(boundary_beats - self->ui_cycle_origin_beats, cycle_len);
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
        self->high_beat_offset = is_cycle_start ? self->beat_len : 0;
        self->low_beat_offset = self->beat_len;
        self->start_beat_offset = is_cycle_start ? 0 : self->beat_len;
      } else {
        /* Other beats: low click only. */
        self->low_beat_offset = 0;
        self->high_beat_offset = self->beat_len;
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

void run_events(Alo* self) {
  if (!self) {
    return;
  }

  const AloURIs* uris = &self->uris;

  /* 1) Transport first. */
  self->transport_updated_this_cycle = false;
  const LV2_Atom_Sequence* in = self->ports.control;
  if (in) {
    LV2_ATOM_SEQUENCE_FOREACH (in, ev) {
      if (ev->body.type == uris->atom_Object || ev->body.type == uris->atom_Blank) {
        const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
        if (obj->body.otype == uris->time_Position) {
          update_position_from_atom(self, obj);
        }
      }
    }
  }

  /* If the host stops sending time:Position updates, treat that as stopped
   * after a short grace period.
   */
  if (self->transport_updated_this_cycle) {
    self->transport_blocks_without_update = 0;
  } else if (self->have_transport) {
    if (self->transport_blocks_without_update < UINT32_MAX) {
      self->transport_blocks_without_update++;
    }
  }

  /* Transport stop/start resync: make the next downbeat be cycle start. */
    const bool stopped_now =
      (self->have_speed && self->speed == 0.0f) ||
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

    if (!stopped_now) {
      /* Transport resumed: restart playback from loop beginning.
       *
       * We only do this when there's existing recorded audio, otherwise we'd
       * interfere with the special "first loop" quantization which depends
       * on have_loop_origin being false.
       */
      bool any_audio = false;
      for (int t = 0; t < NUM_TRACKS; ++t) {
        if (self->have_loop[t]) {
          any_audio = true;
          break;
        }
      }

      if (any_audio && self->have_last_transport_beats) {
        self->loop_origin_beats = self->last_transport_beats;
        self->have_loop_origin = true;
        self->transport_loop_index = 0;
        self->transport_loop_index_pending = true;
        self->loop_index = self->loop_start;
      }
    }
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
    const uint32_t bars_i = get_bars_i(self);
    if (bars_i != self->ui_last_bars_i) {
      const bool have_pos = self->have_last_transport_beats;
      const double global_beats = self->last_transport_beats;
      const bool have_speed = self->have_speed;
      const float speed = self->speed;
      const bool moving = self->transport_moving;
      const uint32_t blocks_wo = self->transport_blocks_without_update;
      const bool transport_updated = self->transport_updated_this_cycle;

      reset(self);

      self->have_speed = have_speed;
      self->speed = speed;
      self->transport_moving = moving;
      self->transport_blocks_without_update = blocks_wo;
      self->transport_updated_this_cycle = transport_updated;

      if (have_pos) {
        self->have_transport = true;
        self->have_last_transport_beats = true;
        self->last_transport_beats = global_beats;

        const float bpb = (self->bpb > 1e-6f) ? self->bpb : (float)DEFAULT_BEATS_PER_BAR;
        self->current_position = (bpb > 0.0f) ? fmodf((float)global_beats, bpb) : 0.0f;
        update_transport_phase(self, global_beats);
      }
    }
  }

  /* 2) MIDI. */
  self->midi_control = false;
  const LV2_Atom_Sequence* midiin = self->ports.midiin;
  if (midiin && self->ports.midi_base) {
    LV2_ATOM_SEQUENCE_FOREACH (midiin, ev) {
      if (ev->body.type == self->uris.midi_MidiEvent) {
        const uint8_t* const msg = (const uint8_t*)(ev + 1);
        const int note = (int)msg[1];
        const int base = (int)floorf(*(self->ports.midi_base));
        const int rel = note - base;

        if (rel >= 0 && rel < 6) {
          const int track = rel % 3;
          const bool is_undo = (rel >= 3);

          if (lv2_midi_message_type(msg) == LV2_MIDI_MSG_NOTE_ON) {
            if (is_undo) {
              handle_undo_press(self, track);
            } else {
              handle_loop_press(self, track);
            }
          }

          self->midi_control = true;
        }
      }
    }
  }

  /* 3) UI edges (unless MIDI is driving). */
  if (!self->midi_control) {
    for (int t = 0; t < NUM_TRACKS; ++t) {
      const bool loop_btn = port_is_pressed(self->ports.loop_btn[t]);
      const bool undo_btn = port_is_pressed(self->ports.undo_btn[t]);
      handle_button_edges(self, t, loop_btn, undo_btn);
    }
  }

  update_bar_step_out(self);

  update_loop_state_ports(self);
}

/* -------------------------------------------------------------------------
 * Main loop engine
 * ------------------------------------------------------------------------- */

void run_loops(Alo* self, uint32_t n_samples) {
  if (!self) {
    return;
  }

  const float* const input_l = self->ports.input_l;
  const float* const input_r = self->ports.input_r;
  float* const output_l = self->ports.output_l;
  float* const output_r = self->ports.output_r;

  if (!input_l || !input_r || !output_l || !output_r || !self->ports.mix) {
    return;
  }

  self->loopmix = fminf(1.0f, *self->ports.mix / 50.0f);
  self->inmix = fminf(1.0f, (100.0f - *self->ports.mix) / 50.0f);

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

      const bool transport_running =
        self->have_transport && self->have_last_transport_beats &&
        (self->transport_blocks_without_update <= 2u) &&
        (!self->have_speed || self->speed != 0.0f) &&
        (self->transport_updated_this_cycle ? self->transport_moving : true);

  /* If transport stops, do not advance or record; keep arms latched. */
  if (!transport_running) {
    /* If the host reports a real transport stop, abort any in-flight
     * arming/recording so nothing triggers on restart.
     */
    if (self->have_speed && self->speed == 0.0f) {
      for (int t = 0; t < NUM_TRACKS; ++t) {
        if (self->track_state[t] != TRACK_IDLE) {
          clear_track_state(self, t);
        }
      }
    }
    for (uint32_t i = 0; i < n_samples; ++i) {
      const float l = input_l[i];
      const float r = input_r[i];
      output_l[i] = self->inmix * l;
      output_r[i] = self->inmix * r;
    }
    update_loop_state_ports(self);
    return;
  }

  /* Align loop phase at start of block (transport-locked). */
  if (self->transport_loop_index_pending) {
    const uint32_t cycle = self->loop_samples ? self->loop_samples : 1u;
    self->loop_index = self->loop_start + (self->transport_loop_index % cycle);
    self->transport_loop_index_pending = false;
  }

  /* Next-bar start scheduling for the very first loop origin.
   * Note: arming the first base recording forces a Bars-cycle/UI resync so
   * this next downbeat becomes step 0 (cycle start).
   */
  uint32_t first_base_start_offset[NUM_TRACKS];
  double first_base_target_beats[NUM_TRACKS];
  for (int t = 0; t < NUM_TRACKS; ++t) {
    first_base_start_offset[t] = UINT32_MAX;
    first_base_target_beats[t] = 0.0;
  }

  if (!self->have_loop_origin && self->have_last_transport_beats) {
    const double bpm = (self->bpm > 1e-6f) ? (double)self->bpm : (double)DEFAULT_BPM;
    const double samples_per_beat = (double)self->rate * 60.0 / bpm;
    const double global_beats0 = self->last_transport_beats;

    /* Quantize the very first base recording to the next Bars-cycle start. */
    const double target_beats = compute_next_cycle_start_beats(self, global_beats0);
    const double beats_until = target_beats - global_beats0;
    uint64_t offset_s = 0;
    if (beats_until > 0.0) {
      /* Never start early: ceil to the next sample at/after the boundary. */
      const double samples_until = beats_until * samples_per_beat;
      offset_s = (uint64_t)ceil(samples_until - 1e-9);
    }

    for (int t = 0; t < NUM_TRACKS; ++t) {
      if (self->track_state[t] == TRACK_ARM_BASE) {
        if (offset_s < (uint64_t)n_samples) {
          first_base_start_offset[t] = (uint32_t)offset_s;
          first_base_target_beats[t] = target_beats;
        }
      }
    }
  }

  /* Compute bar length in samples for quantized undo (bar downbeats). */
  const uint32_t bars_i = get_bars_i(self);
  uint32_t bar_len = (bars_i > 0 && self->loop_samples > 0) ? (self->loop_samples / bars_i)
                                                           : self->loop_samples;
  if (!bar_len) {
    bar_len = self->loop_samples ? self->loop_samples : 1u;
  }

  const float inmix = self->inmix;
  const float loopmix = self->loopmix;

  /*
   * Optimization pass:
   * Pre-sum playback contribution for this block into scratch arrays, so the
   * per-sample hot loop doesn't iterate tracks × layers.
   *
   * Note: This uses C99 VLA stack allocation; it's RT-safe (no malloc) and
   * bounded by the host block size.
   */
  float play_l[n_samples];
  float play_r[n_samples];
  memset(play_l, 0, sizeof(play_l));
  memset(play_r, 0, sizeof(play_r));

  /* Only compute playback if we have at least one active slot. */
  bool any_play = false;
  for (int t = 0; t < NUM_TRACKS; ++t) {
    if (self->have_loop[t] && self->loop_buf[t]) {
      any_play = true;
      break;
    }
  }

  if (any_play) {
    uint32_t idx = self->loop_index;
    for (uint32_t pos = 0; pos < n_samples; ++pos) {
      const uint32_t idx_r = idx + LOOP_SIZE;

      for (int t = 0; t < NUM_TRACKS; ++t) {
        if (!self->have_loop[t] || !self->loop_buf[t]) {
          continue;
        }

        const float g = track_gain[t];
        play_l[pos] += g * self->loop_buf[t][idx];
        play_r[pos] += g * self->loop_buf[t][idx_r];

        const uint8_t n_layers = self->od_count[t];
        for (uint8_t l = 0; l < n_layers; ++l) {
          float* const buf = self->od_buf[t][l];
          if (!buf) {
            continue;
          }
          play_l[pos] += g * buf[idx];
          play_r[pos] += g * buf[idx_r];
        }
      }

      idx++;
      if (idx >= self->loop_start + self->loop_samples) {
        idx = self->loop_start;
      }
    }
  }

  for (uint32_t pos = 0; pos < n_samples; ++pos) {
    /* Apply pending undo at bar downbeat. */
    const uint32_t phase = (self->loop_index - self->loop_start);
    const bool at_bar = ((phase % bar_len) == 0u);
    if (at_bar) {
      apply_pending_undo_at_bar(self);
    }

    const uint32_t idx = self->loop_index;
    const uint32_t idx_r = idx + LOOP_SIZE;

    const float in_l = input_l[pos];
    const float in_r = input_r[pos];

    /* Dry input + precomputed playback */
    output_l[pos] = inmix * in_l + play_l[pos];
    output_r[pos] = inmix * in_r + play_r[pos];

    /* Prevent clipping when summing multiple tracks / heavy overdubs. */
    output_l[pos] = soft_clip_unit(output_l[pos]);
    output_r[pos] = soft_clip_unit(output_r[pos]);

    /* Start armed recordings. */
    for (int t = 0; t < NUM_TRACKS; ++t) {
      if (self->track_state[t] == TRACK_ARM_BASE) {
        if (!self->have_loop_origin) {
          if (first_base_start_offset[t] == pos) {
            self->loop_origin_beats = first_base_target_beats[t];
            self->have_loop_origin = true;
            self->transport_loop_index = 0;
            self->transport_loop_index_pending = true;
            self->loop_index = self->loop_start;

            self->track_state[t] = TRACK_REC_BASE;
            self->rec_remaining_samples[t] = self->loop_samples;
            self->have_loop[t] = false;
            self->od_count[t] = 0;
          }
        } else {
          /* Start base on next loop boundary. */
          if (self->loop_index == self->loop_start) {
            self->track_state[t] = TRACK_REC_BASE;
            self->rec_remaining_samples[t] = self->loop_samples;
            self->have_loop[t] = false;
            self->od_count[t] = 0;
          }
        }
      } else if (self->track_state[t] == TRACK_ARM_OVERDUB) {
        if (self->have_loop[t] && self->loop_index == self->loop_start) {
          /* Only start an overdub if we have a free layer to record into.
           * Never overwrite an active layer in-place (aborts would corrupt
           * playback).
           */
          if (self->od_count[t] < ALO_MAX_UNDO_LAYERS) {
            self->track_state[t] = TRACK_REC_OVERDUB;
            self->rec_remaining_samples[t] = self->loop_samples;
            self->rec_od_layer[t] = self->od_count[t];
          } else {
            self->track_state[t] = TRACK_IDLE;
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
        self->loop_buf[t][idx] = loopmix * in_l;
        self->loop_buf[t][idx_r] = loopmix * in_r;

        if (self->rec_remaining_samples[t] > 0) {
          self->rec_remaining_samples[t]--;
        }
        if (self->rec_remaining_samples[t] == 0) {
          self->track_state[t] = TRACK_IDLE;
          self->have_loop[t] = true;
        }
      } else if (self->track_state[t] == TRACK_REC_OVERDUB) {
        const uint8_t layer = self->rec_od_layer[t];
        if (layer < ALO_MAX_UNDO_LAYERS && self->od_buf[t][layer]) {
          float* const buf = self->od_buf[t][layer];
          buf[idx] = loopmix * in_l;
          buf[idx_r] = loopmix * in_r;
        }

        if (self->rec_remaining_samples[t] > 0) {
          self->rec_remaining_samples[t]--;
        }
        if (self->rec_remaining_samples[t] == 0) {
          self->track_state[t] = TRACK_IDLE;
          self->have_loop[t] = true;
          if (self->od_count[t] < ALO_MAX_UNDO_LAYERS) {
            self->od_count[t]++;
          }
        }
      }
    }

    /* Advance phase. */
    self->loop_index++;
    if (self->loop_index >= self->loop_start + self->loop_samples) {
      self->loop_index = self->loop_start;
    }
  }

  update_loop_state_ports(self);
}
