#ifndef ALO_UI_UTIL_H
#define ALO_UI_UTIL_H

#include "alo_engine.h"
#include <math.h>

/**
 * Read the current Bars value from an LV2 port-value array (as used by the
 * native UI).  Mirrors alo_get_bars_i() but works on the raw float buffer.
 */
static inline int alo_ui_get_bars_i(const float* port_values)
{
  float bars_f =
      (port_values && ALO_BARS < ALO_PORT_COUNT) ? port_values[ALO_BARS] : (float)DEFAULT_NUM_BARS;
  if (bars_f < 1.0f) {
    bars_f = 1.0f;
  }
  if (bars_f > 16.0f) {
    bars_f = 16.0f;
  }
  int bars_i = (int)lrintf(bars_f);
  return (bars_i > 0) ? bars_i : 1;
}

/*
 * Colour roles used by the native X11 UI.  These are primarily for
 * documentation and future refactoring; the implementation currently
 * stores them as separate fields in AloUI.  Having the enum in a header
 * makes it easier to keep the naming consistent across the codebase.
 */

typedef enum
{
  ALO_UI_COL_FOREGROUND,
  ALO_UI_COL_BACKGROUND,
  ALO_UI_COL_GREY,
  ALO_UI_COL_CYCLE,
  ALO_UI_COL_CYCLE_PAST,
  ALO_UI_COL_HOST,
  ALO_UI_COL_ACTIVE,
  /* transport-ring state colours */
  ALO_UI_COL_RING_REC,
  ALO_UI_COL_RING_ARM,
  ALO_UI_COL_RING_PLAY,
  /* generic control colours */
  ALO_UI_COL_BTN_ON,
  ALO_UI_COL_BTN_OFF,
  ALO_UI_COL_SLIDER_FILL,
  ALO_UI_COL_COUNT
} AloUIColourRole;

#endif // ALO_UI_UTIL_H
