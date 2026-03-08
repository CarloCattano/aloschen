#include "alo_engine.h"
#include "alo_util.h"
#include "loop_state.h"
#include "transport.h"
#include "button_logic.h"

#include "lv2/atom/util.h"

#include <math.h>

/* Click track, button-edge processing, and LV2 event dispatch for the looper. */

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
        phase = alo_fmod_positive(phase, (double)steps_u);
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
    alo_port_write(self->ports.bar_step_out, (float)step);
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
        phase_beats = alo_fmod_positive(phase_beats, (double)steps_u2);
        cycle_phase = (float)(phase_beats / (double)steps_u2);
        if (cycle_phase < 0.0f) {
          cycle_phase = 0.0f;
        } else if (cycle_phase > 1.0f) {
          cycle_phase = 1.0f;
        }
      }
    }

    alo_port_write(self->ports.host_bar_phase_out, host_bar_phase);
    alo_port_write(self->ports.cycle_phase_out,    cycle_phase);
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
        double       phase     = alo_fmod_positive(boundary_beats - self->ui_cycle_origin_beats, cycle_len);

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
  /* Consider the transport "stopped" when host speed is non-positive.  This
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

  /* 2) MIDI slice triggers (one-shot, sample-accurate start within the block). */
  const LV2_Atom_Sequence* midiin = self->ports.midiin;
  if (midiin) {
    /* Only accept MIDI slice triggers when we have committed loop audio. */

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
    /* choose slice count depending on mode */
    uint32_t slice_count;
    if (alo_get_use_transient_slices_b(self)) {
      slice_count = self->detected_slices_count ? self->detected_slices_count : 1u;
    } else {
      const uint32_t bars_i         = alo_get_bars_i(self);
      const uint32_t slices_per_bar = alo_get_slices_per_bar_u(self);
      slice_count = bars_i * slices_per_bar;
    }

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

      uint32_t phase_samples;
      uint32_t slice_len;
      if (alo_get_use_transient_slices_b(self) && self->detected_slices_count > 0) {
        /* map note index to detected offsets list */
        uint32_t idx = (uint32_t)slice_index;
        uint32_t start = self->detected_slice_offsets[idx];
        uint32_t end = (idx + 1 < self->detected_slices_count)
                          ? self->detected_slice_offsets[idx + 1]
                          : self->loop_samples;
        if (end <= start) continue;
        phase_samples = start;
        slice_len     = end - start;
      } else {
        const uint64_t loop_s = (uint64_t)self->loop_samples;
        const uint64_t s0     = ((uint64_t)slice_index * loop_s) / (uint64_t)slice_count;
        const uint64_t s1     = ((uint64_t)(slice_index + 1) * loop_s) / (uint64_t)slice_count;
        if (s1 <= s0) {
          continue;
        }
        phase_samples = (uint32_t)s0;
        slice_len     = (uint32_t)(s1 - s0);
      }

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
       *
       * fade_samples now comes from a percent-of-slice parameter.  A value of
       * 100 gives a release equal to the full slice length; an empty control
       * means we use the default edge fade (~1 ms).  Attack is handled
       * separately in the sampler.
       *
       * When the user supplies a percent less than 100, shorten the actual
       * playback length to match the fade duration so the slice behaves like a
       * one-shot whose duration is controlled by decay. */
      uint32_t fade_samples = alo_get_slice_fade_samples(self, slice_len);
      uint32_t play_len = slice_len;
      if (fade_samples < slice_len) {
        play_len = fade_samples;
        if (play_len == 0u) play_len = 1u;
      }

      alo_slice_sampler_schedule(&self->slice_sampler, self, start_offset_samples, phase_samples,
                                 play_len, fade_samples, 1.0f);
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