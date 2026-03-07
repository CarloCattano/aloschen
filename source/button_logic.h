#ifndef BUTTON_LOGIC_H
#define BUTTON_LOGIC_H

#include "alo_engine.h"
#include "alo_util.h" /* track-state helpers, etc. */

/* resync helpers from loop_engine */
void request_ui_cycle_resync(Alo* self);

/* Button/track interaction helpers moved out of loop_engine.c for clarity */
void handle_loop_press(Alo* self, int t);
void handle_undo_press(Alo* self, int t);

#endif // BUTTON_LOGIC_H
