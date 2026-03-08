/**
 * @file loop_state.h
 * @brief Internal engine-state helpers shared between loop_state.c,
 *        loop_playback.c, and loop_engine.c.
 *
 * These functions are engine-internal and are NOT part of the public
 * plugin API exposed in alo_engine.h.
 */

#ifndef LOOP_STATE_H
#define LOOP_STATE_H

#include "alo_engine.h"

/**
 * Cancel inflight track actions and prime UI button-edge detectors so that
 * buttons already held high at the start of a new transport run are ignored.
 */
void clear_inflight_actions_and_sync_controls(Alo* self);

/**
 * Request that the UI cycle-phase indicator resynchronises to the next bar
 * downbeat.  Call whenever the Bars value, transport state, or loop origin
 * changes in a way that would make the current cycle origin stale.
 */
void request_ui_cycle_resync(Alo* self);

/**
 * Apply any pending quantized undo/clear-all operations that are due at a
 * bar boundary.  Returns true if any track state changed (which implies the
 * sampler source cache needs to be rebuilt).
 *
 * Called once per sample from the hot loop in run_loops() whenever
 * bar_phase == 0.
 */
bool apply_pending_undo_at_bar(Alo* self);

#endif /* LOOP_STATE_H */