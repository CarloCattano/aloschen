#ifndef ALO_UI_UTIL_H
#define ALO_UI_UTIL_H

#include "alo_engine.h" /* for ALO_BARS, ALO_PORT_COUNT, DEFAULT_* macros */
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

#endif // ALO_UI_UTIL_H
