#ifndef BUTTON_LOGIC_H
#define BUTTON_LOGIC_H

#include "alo_engine.h"
#include "alo_util.h" 

/**
 * @brief Notify engine that UI cycle should resynchronize on next update.
 *
 * Also used by button logic so declaration lives here for convenience.
 *
 * @param self Engine instance (may be NULL).
 */
void request_ui_cycle_resync(Alo* self);

/**
 * @brief Process a press of the loop button on track @p t.
 *
 * Behavior depends on track state and other active tracks; may arm, record,
 * overdub, or cancel pending actions. Updates loop-state output ports.
 *
 * @param self Engine instance (may be NULL).
 * @param t    Track index.
 */
void handle_loop_press(Alo* self, int t);

/**
 * @brief Process a press of the undo button on track @p t.
 *
 * Stops any active track action and queues an undo if audio exists.
 *
 * @param self Engine instance (may be NULL).
 * @param t    Track index.
 */
void handle_undo_press(Alo* self, int t);

#endif // BUTTON_LOGIC_H
