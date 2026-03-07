#include "button_logic.h"
#include "transport.h"

static inline bool any_other_track_busy(const Alo* self, int exclude)
{
  for (int u = 0; u < NUM_TRACKS; ++u) {
    if (u == exclude)
      continue;
    if (self->track_state[u] == TRACK_REC_BASE || self->track_state[u] == TRACK_REC_OVERDUB) {
      return true;
    }
  }
  return false;
}

/* cppcheck-suppress unusedFunction - analyzed cross-file calls not detected */
void handle_loop_press(Alo* self, int t)
{
  // Cancel queued arm if pressing same track
  if (self && self->pending_arm_track == t) {
    self->pending_arm_track = -1;
    update_loop_state_ports(self);
    return;
  }
  // If another track is recording, queue this press instead of ignoring it
  if (self && any_other_track_busy(self, t)) {
    if (self->pending_arm_track < 0) {
      self->pending_arm_track = t;
      self->pending_arm_type  = self->have_loop[t] ? TRACK_ARM_OVERDUB : TRACK_ARM_BASE;
      update_loop_state_ports(self); // immediate UI feedback
    }
    return;
  }

  if (!self || !track_is_active(self, t))
    return;

  /* If the user presses while armed/recording, treat it as cancel/abort. */
  if (track_is_busy(self, t)) {
    clear_track_state(self, t);
    update_loop_state_ports(self);
    return;
  }

  if (!self->have_loop[t]) {
    /* First-ever base recording defines the loop origin. */
    if (!self->have_loop_origin) {
      /* If we already know the current transport position we can sync the UI
       * immediately; otherwise defer until run_loops quantizes the start.
       */
      if (self->have_last_transport_beats) {
        request_ui_cycle_resync(self);
        self->ui_cycle_origin_beats =
            compute_next_cycle_start_beats(self, self->last_transport_beats);
        self->ui_have_cycle_origin    = true;
        self->ui_cycle_resync_pending = false;
      }
    }
    self->track_state[t]           = TRACK_ARM_BASE;
    self->rec_remaining_samples[t] = 0;
  } else {
    self->track_state[t]           = TRACK_ARM_OVERDUB;
    self->rec_remaining_samples[t] = 0;
  }
}

/* cppcheck-suppress unusedFunction - analyzed cross-file calls not detected */
void handle_undo_press(Alo* self, int t)
{
  if (!self || !track_is_active(self, t))
    return;

  /* Stop any pending action/recording immediately (quantized audio change later). */
  clear_track_state(self, t);

  if (self->have_loop[t] || self->od_count[t] > 0) {
    self->pending_undo[t]++;
    if (self->pending_undo[t] > ALO_MAX_UNDO_LAYERS)
      self->pending_undo[t] = ALO_MAX_UNDO_LAYERS + 1;
  } else {
    /* nothing to undo; ignore */
    self->pending_undo[t] = 0;
  }
  update_loop_state_ports(self);
}
