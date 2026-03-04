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

void update_loop_state_ports(Alo* self) {
  if (!self) {
    return;
  }
  for (int t = 0; t < NUM_TRACKS; ++t) {
    if (self->ports.loop_state_out[t]) {
      float v = 0.0f;
      switch (self->track_state[t]) {
        case TRACK_ARM_BASE:
        case TRACK_ARM_OVERDUB:
          v = 0.25f; /* armed, waiting for quantized start */
          break;
        case TRACK_REC_BASE:
        case TRACK_REC_OVERDUB:
          v = 1.0f; /* actively recording */
          break;
        case TRACK_IDLE:
        default:
          /* Idle: show playback state if this track has a loop. */
          v = self->have_loop[t] ? 0.5f : 0.0f;
          break;
      }
      *(self->ports.loop_state_out[t]) = v;
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
  if (bars_f > 32.0f) {
    bars_f = 32.0f;
  }
  uint32_t bars_i = (uint32_t)lrintf(bars_f);
  if (!bars_i) {
    bars_i = 1u;
  }
  return bars_i;
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

  update_loop_state_ports(self);
}

static void update_bar_step_out(Alo* self) {
  if (!self || !self->ports.bar_step_out) {
    return;
  }

  const bool transport_stopped = (self->have_speed && self->speed == 0.0f);

  /* If Bars changed, restart the cycle on the next downbeat. */
  const uint32_t bars_i = get_bars_i(self);
  if (bars_i != self->ui_last_bars_i) {
    self->ui_last_bars_i = bars_i;
    self->click_bar_in_cycle = 0;
    self->ui_cycle_resync_pending = true;
    self->ui_have_cycle_origin = false;
    self->ui_have_prev_bar_beat = false;
  }

  /* Default to showing the first position when stopped / not yet synced. */
  int step = 0;

  if (transport_stopped || !self->have_transport || !self->have_last_transport_beats) {
    /* Stopped/unknown: show step 0 and force next start to resync. */
    self->click_bar_in_cycle = 0;
    self->ui_cycle_resync_pending = true;
    self->ui_have_cycle_origin = false;
    self->ui_have_prev_bar_beat = false;
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
      if (downbeat_edge || bar_beat < 1e-3f) {
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
    *(self->ports.bar_step_out) = (float)step;
    self->ui_last_bar_step = (int8_t)step;
  }
}

/* -------------------------------------------------------------------------
 * Transport update
 * ------------------------------------------------------------------------- */

static void update_position_from_atom(Alo* self, const LV2_Atom_Object* obj) {
  if (!self || !obj) {
    return;
  }

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

    if (self->have_last_transport_beats) {
      if (global_beats > self->last_transport_beats + 1e-6) {
        self->transport_moving = true;
      } else if (self->have_speed && self->speed == 0.0f) {
        self->transport_moving = false;
      }
    }

    if (self->have_last_transport_beats &&
        global_beats < self->last_transport_beats - 0.5) {
      reset(self);
    }

    self->last_transport_beats = global_beats;
    self->have_last_transport_beats = true;

    const float bar_beat =
        (self->bpb > 0.0f) ? fmodf((float)global_beats, self->bpb) : 0.0f;
    self->current_position = bar_beat;

    if (self->loop_beats > 0 && self->loop_samples > 0) {
      const double origin = self->have_loop_origin ? self->loop_origin_beats : 0.0;
      double phase_beats = fmod(global_beats - origin, (double)self->loop_beats);
      if (phase_beats < 0.0) {
        phase_beats += (double)self->loop_beats;
      }

      double phase_samples_d =
          phase_beats * (double)self->loop_samples / (double)self->loop_beats;
      if (phase_samples_d < 0.0) {
        phase_samples_d = 0.0;
      }
      uint32_t phase_samples = (uint32_t)floor(phase_samples_d);
      if (phase_samples >= self->loop_samples) {
        phase_samples = self->loop_samples - 1;
      }

      self->transport_loop_index = phase_samples;
      self->transport_loop_index_pending = true;
      self->have_transport = true;
    }

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

      if (self->have_last_transport_beats) {
        if (global_beats > self->last_transport_beats + 1e-6) {
          self->transport_moving = true;
        } else if (self->have_speed && self->speed == 0.0f) {
          self->transport_moving = false;
        }
      }

      if (self->have_last_transport_beats &&
          global_beats < self->last_transport_beats - 0.5) {
        reset(self);
      }

      self->last_transport_beats = global_beats;
      self->have_last_transport_beats = true;

      const double origin = self->have_loop_origin ? self->loop_origin_beats : 0.0;
      double phase_beats = fmod(global_beats - origin, (double)self->loop_beats);
      if (phase_beats < 0.0) {
        phase_beats += (double)self->loop_beats;
      }

      double phase_samples_d =
          phase_beats * (double)self->loop_samples / (double)self->loop_beats;
      if (phase_samples_d < 0.0) {
        phase_samples_d = 0.0;
      }
      uint32_t phase_samples = (uint32_t)floor(phase_samples_d);
      if (phase_samples >= self->loop_samples) {
        phase_samples = self->loop_samples - 1;
      }

      self->transport_loop_index = phase_samples;
      self->transport_loop_index_pending = true;
      self->have_transport = true;
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

  /* One-shot: ignore presses while busy. */
  if (track_is_busy(self, t)) {
    return;
  }

  if (!self->have_loop[t]) {
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
   */
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

  const float old_beat = floorf(self->current_position);
  self->current_position += n_samples / self->rate / 60.0f * self->bpm;
  const float new_beat = floorf(self->current_position);
  self->current_position = fmodf(self->current_position, self->bpb);
  const float beat = floorf(self->current_position);

  if (new_beat != old_beat) {
    const uint32_t sample_offset =
        (uint32_t)((self->current_position - beat) * self->rate);

    /*
     * Advance the Bars-cycle phase on every bar downbeat, even when the click
     * is muted or suppressed (e.g. loops present). The UI step indicator and
     * the START click both reference this cycle.
     */
    const uint32_t bars_i = get_bars_i(self);
    if (self->click_bar_in_cycle >= bars_i) {
      self->click_bar_in_cycle = 0;
    }
    const bool is_cycle_start = (beat == 0.0f) && (self->click_bar_in_cycle == 0u);

    if (play_click && *self->ports.click && self->speed) {
      click_mix(self, 0, sample_offset);

      if (beat == 0.0f) {
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

    if (beat == 0.0f) {
      /* Advance bar within cycle on the downbeat. */
      self->click_bar_in_cycle++;
      if (self->click_bar_in_cycle >= bars_i) {
        self->click_bar_in_cycle = 0;
      }
    }
  } else {
    if (play_click && *self->ports.click && self->speed) {
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

  /* Transport stop/start resync: make the next downbeat be cycle start. */
  if (self->have_speed) {
    const bool stopped = (self->speed == 0.0f);
    if (stopped != self->ui_transport_was_stopped) {
      self->ui_transport_was_stopped = stopped;
      self->click_bar_in_cycle = 0;
      self->ui_cycle_resync_pending = true;
      self->ui_have_cycle_origin = false;
      self->ui_have_prev_bar_beat = false;
      self->ui_last_bar_step = -2;
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
      const bool loop_btn = self->ports.loop_btn[t] && (*self->ports.loop_btn[t] > 0.0f);
      const bool undo_btn = self->ports.undo_btn[t] && (*self->ports.undo_btn[t] > 0.0f);
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

  self->loopmix = fminf(1.0f, *self->ports.mix / 50.0f);
  self->inmix = fminf(1.0f, (100.0f - *self->ports.mix) / 50.0f);

  const bool transport_running = self->have_transport
                                    ? (self->transport_moving || !self->have_speed ||
                                       self->speed != 0.0f)
                                    : true;

  /* If transport stops, do not advance or record; keep arms latched. */
  if (!transport_running) {
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

  /* Next-bar start scheduling for the very first loop origin. */
  uint32_t first_base_start_offset[NUM_TRACKS];
  double first_base_target_beats[NUM_TRACKS];
  for (int t = 0; t < NUM_TRACKS; ++t) {
    first_base_start_offset[t] = UINT32_MAX;
    first_base_target_beats[t] = 0.0;
  }

  if (!self->have_loop_origin && self->have_last_transport_beats) {
    const double bpm = (self->bpm > 1e-6f) ? (double)self->bpm : (double)DEFAULT_BPM;
    const double samples_per_beat = (double)self->rate * 60.0 / bpm;
    const double bpb = (self->bpb > 1e-6f) ? (double)self->bpb : (double)DEFAULT_BEATS_PER_BAR;
    const double global_beats0 = self->last_transport_beats;

    /*
     * Quantize the very first base recording to the next bar downbeat.
     *
     * Control-port changes (UI button presses) are typically applied at the
     * next audio block boundary. If the user presses right before a downbeat,
     * the host may deliver the armed state in the *following* block, when the
     * transport position is already slightly past the downbeat. Without a
     * guard, we can miss that downbeat and schedule one full bar later.
     *
     * Grace window: if we are very near the downbeat at the start of this
     * block, treat it as the downbeat and start immediately (offset 0).
     */
    const float bar_beat0_f = self->current_position;
    double bar_beat0 = (bar_beat0_f >= 0.0f) ? (double)bar_beat0_f : 0.0;
    if (bar_beat0 < 0.0) {
      bar_beat0 = 0.0;
    }
    const double kDownbeatGraceBeats = 0.25; /* quarter-beat */

    const double k = floor(global_beats0 / bpb);
    double target_beats = k * bpb;

    if (bar_beat0 <= kDownbeatGraceBeats) {
      /* Start at the current bar downbeat (block boundary). */
      target_beats = global_beats0 - bar_beat0;
    } else {
      /* Start at the next bar downbeat. */
      if (global_beats0 - target_beats > 1e-6) {
        target_beats += bpb;
      }
    }

    const double beats_until = fmax(0.0, target_beats - global_beats0);
    const uint64_t offset_s = (uint64_t)llround(beats_until * samples_per_beat);

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

  for (uint32_t pos = 0; pos < n_samples; ++pos) {
    /* Apply pending undo at bar downbeat. */
    const uint32_t phase = (self->loop_index - self->loop_start);
    const bool at_bar = ((phase % bar_len) == 0u);
    if (at_bar) {
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

    const float in_l = input_l[pos];
    const float in_r = input_r[pos];

    /* Dry input */
    output_l[pos] = self->inmix * in_l;
    output_r[pos] = self->inmix * in_r;

    /* Playback (sum tracks). */
    for (int t = 0; t < NUM_TRACKS; ++t) {
      if (!self->have_loop[t] || !self->loop_buf[t]) {
        continue;
      }
      output_l[pos] += self->loop_buf[t][self->loop_index];
      output_r[pos] += self->loop_buf[t][self->loop_index + LOOP_SIZE];

      const uint8_t n_layers = self->od_count[t];
      for (uint8_t l = 0; l < n_layers; ++l) {
        float* const buf = self->od_buf[t][l];
        if (!buf) {
          continue;
        }
        output_l[pos] += buf[self->loop_index];
        output_r[pos] += buf[self->loop_index + LOOP_SIZE];
      }
    }

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
          self->track_state[t] = TRACK_REC_OVERDUB;
          self->rec_remaining_samples[t] = self->loop_samples;

          /* Append if possible, else overwrite most recent layer. */
          if (self->od_count[t] < ALO_MAX_UNDO_LAYERS) {
            self->rec_od_layer[t] = self->od_count[t];
          } else {
            self->rec_od_layer[t] = (ALO_MAX_UNDO_LAYERS > 0) ? (ALO_MAX_UNDO_LAYERS - 1) : 0;
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
        self->loop_buf[t][self->loop_index] = self->loopmix * in_l;
        self->loop_buf[t][self->loop_index + LOOP_SIZE] = self->loopmix * in_r;

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
          buf[self->loop_index] = self->loopmix * in_l;
          buf[self->loop_index + LOOP_SIZE] = self->loopmix * in_r;
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
