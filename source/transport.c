/* transport and timing helpers for the looper engine */

#include "transport.h"
#include "alo_util.h"
/* cppcheck-suppress missingIncludeSystem */
#include <math.h>
/* cppcheck-suppress missingIncludeSystem */
#include <stddef.h>
/* cppcheck-suppress missingInclude */
#include "lv2/atom/util.h"

/* compute phase index from host beat position */
bool compute_transport_phase_index(const Alo* self, double global_beats,
                                   uint32_t* out_phase_samples)
{
  if (!self || !out_phase_samples)
    return false;

  const uint32_t loop_beats   = self->loop_beats;
  const uint32_t loop_samples = self->loop_samples;
  if (!(loop_beats > 0u && loop_samples > 0u))
    return false;

  const double origin      = self->have_loop_origin ? self->loop_origin_beats : 0.0;
  double       phase_beats = fmod(global_beats - origin, (double)loop_beats);
  if (phase_beats < 0.0)
    phase_beats += (double)loop_beats;

  double phase_samples_d = phase_beats * (double)loop_samples / (double)loop_beats;
  if (phase_samples_d < 0.0)
    phase_samples_d = 0.0;
  uint32_t phase_samples = (uint32_t)floor(phase_samples_d);
  if (phase_samples >= loop_samples)
    phase_samples = loop_samples - 1;

  *out_phase_samples = phase_samples;
  return true;
}

/* Timing helpers */

/* return number of beats in current loop */
uint32_t compute_loop_beats(const Alo* self)
{
  if (!self) {
    return DEFAULT_BEATS_PER_BAR * DEFAULT_NUM_BARS;
  }

  const uint32_t bpb_ok  = alo_get_bpb_i(self);
  const uint32_t bars_i  = alo_get_bars_i(self);
  const uint32_t bars_ok = bars_i ? bars_i : DEFAULT_NUM_BARS;
  return bpb_ok * bars_ok;
}

/* compute beat position of next cycle start */
double compute_next_cycle_start_beats(const Alo* self, double global_beats0)
{
  if (!self) {
    return global_beats0;
  }

  const double bpb = (self->bpb > 1e-6f) ? (double)self->bpb : (double)DEFAULT_BEATS_PER_BAR;
  if (!(bpb > 0.0)) {
    return global_beats0;
  }

  const uint32_t bars_i          = alo_get_bars_i(self);
  const double   cycle_len_beats = (double)(bars_i ? bars_i : 1u) * bpb;
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

/* update transport phase index from beat value */
void update_transport_phase(Alo* self, double global_beats)
{
  if (!self)
    return;
  uint32_t phase_samples = 0;
  if (!compute_transport_phase_index(self, global_beats, &phase_samples))
    return;
  while (phase_samples >= self->loop_samples)
    phase_samples -= self->loop_samples;
  self->transport_loop_index         = phase_samples;
  self->transport_loop_index_pending = true;
}

/* Transport beats update */

/* helper for new transport beat notifications */
static void update_transport_beats(Alo* self, double global_beats)
{
  if (!self)
    return;

  /* True if any slot has committed loop audio. */
  bool any_audio = false;
  for (int t = 0; t < NUM_TRACKS; ++t) {
    if (self->have_loop[t]) {
      any_audio = true;
      break;
    }
  }

  self->have_transport = true;

  if (self->have_last_transport_beats) {
    if (global_beats > self->last_transport_beats + 1e-6) {
      self->transport_moving = true;
    } else {
      self->transport_moving = false;
    }
  } else {
    self->transport_moving = (self->have_speed && self->speed != 0.0f);
  }

  if (self->have_speed && self->speed == 0.0f) {
    self->transport_moving = false;
  }

  if (self->have_last_transport_beats && global_beats < self->last_transport_beats - 0.5) {
    clear_inflight_actions_and_sync_controls(self);
    request_ui_cycle_resync(self);
    alo_slice_sampler_reset(&self->slice_sampler);
    if (any_audio) {
      self->loop_origin_beats            = global_beats;
      self->have_loop_origin             = true;
      self->transport_loop_index         = 0;
      self->transport_loop_index_pending = true;
      self->loop_phase                   = 0;
      self->loop_playhead                = (double)self->loop_start;
    } else {
      self->have_loop_origin  = false;
      self->loop_origin_beats = 0.0;
    }
  }

  self->last_transport_beats      = global_beats;
  self->have_last_transport_beats = true;
}

/* parse LV2 time atom and update transport fields */
void update_position_from_atom(Alo* self, const LV2_Atom_Object* obj)
{
  if (!self || !obj)
    return;

  /* Mark that we saw transport info this cycle (used for stop detection). */
  self->transport_updated_this_cycle = true;

  AloURIs* const uris = &self->uris;

  LV2_Atom *abs_beat = NULL, *beat = NULL, *bar = NULL;
  LV2_Atom *bpm = NULL, *bpb = NULL, *speed = NULL;

  lv2_atom_object_get(obj, uris->time_beat, &abs_beat, uris->time_barBeat, &beat,
                      uris->time_beatsPerMinute, &bpm, uris->time_speed, &speed,
                      uris->time_beatsPerBar, &bpb, uris->time_bar, &bar, NULL);

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
    self->speed      = ((LV2_Atom_Float*)speed)->body;
  }

  if (abs_beat && abs_beat->type == uris->atom_Float) {
    const double global_beats = (double)((LV2_Atom_Float*)abs_beat)->body;

    update_transport_beats(self, global_beats);

    const float bar_beat   = (self->bpb > 0.0f) ? fmodf((float)global_beats, self->bpb) : 0.0f;
    self->current_position = bar_beat;

    update_transport_phase(self, global_beats);

    return;
  }

  /* Fallback: attempt to derive a global beat value from bar+beat info. */
  if (beat && beat->type == uris->atom_Float) {
    const float beat_val   = ((LV2_Atom_Float*)beat)->body;
    self->current_position = beat_val;

    /* Compute rough absolute position if we know the bar index and beats per bar. */
    double global_beats = (double)beat_val;
    if (bar && bar->type == uris->atom_Long && bpb && bpb->type == uris->atom_Float) {
      double bar_val = (double)((LV2_Atom_Long*)bar)->body;
      double bpb_val = (double)((LV2_Atom_Float*)bpb)->body;
      global_beats   = bar_val * bpb_val + (double)beat_val;
    }
    /* Update transport state even when we don't have an absolute beat atom. */
    update_transport_beats(self, global_beats);
    update_transport_phase(self, global_beats);
  }
}
