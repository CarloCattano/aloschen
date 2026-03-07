#ifndef TRANSPORT_H
#define TRANSPORT_H

#include "alo_engine.h"

/* Helpers for transport-derived timing.  These used to live in
*/

/* Compute the current phase index (in samples) for the looper based on the
 * host's absolute beat position.  The result is quantized to [0, loop_samples).
 * Returns true if a valid phase could be computed.
 */
bool compute_transport_phase_index(const Alo* self, double global_beats,
                                   uint32_t* out_phase_samples);

uint32_t compute_loop_beats(const Alo* self);

double compute_next_cycle_start_beats(const Alo* self, double global_beats0);

void clear_inflight_actions_and_sync_controls(Alo* self);
void request_ui_cycle_resync(Alo* self);

/* Update the transport_loop_index pending field from a beat value.  This
 * schedules a phase alignment at the start of the next audio block.
 */
void update_transport_phase(Alo* self, double global_beats);

void update_position_from_atom(Alo* self, const LV2_Atom_Object* obj);

#endif // TRANSPORT_H
