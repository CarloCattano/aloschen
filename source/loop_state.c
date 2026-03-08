#include "loop_state.h"
#include "alo_util.h"
#include "transport.h"

#include <math.h>

/* -------------------------------------------------------------------------
 * Track-audio helpers
 * ------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * Loop-state port helpers
 * ------------------------------------------------------------------------- */

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
    alo_port_write(self->ports.loop_state_out[t], loop_state_value(self, t, transport_stopped));
    alo_port_write(self->ports.undo_state_out[t], undo_state_value(self, t));
    alo_port_write(self->ports.has_audio_out[t],  has_audio_value(self, t));
  }
}

/* -------------------------------------------------------------------------
 * Timing helpers
 * ------------------------------------------------------------------------- */

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

/* reinitialize engine state to defaults
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
  alo_port_write(self->ports.bar_step_out,       0.0f);
  alo_port_write(self->ports.cycle_phase_out,    0.0f);
  alo_port_write(self->ports.host_bar_phase_out, 0.0f);

  update_loop_state_ports(self);
}

/* -------------------------------------------------------------------------
 * Quantized undo
 * ------------------------------------------------------------------------- */

bool apply_pending_undo_at_bar(Alo* self)
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