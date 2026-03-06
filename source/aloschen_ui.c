/*
  Minimal native X11 UI for the ALOSCHEN LV2 plugin.

  This UI is a separate shared object (aloschen_ui.so) and communicates with the
  DSP only via LV2 UI callbacks (write_function / port_event).
*/

/* For clock_gettime / CLOCK_MONOTONIC on glibc. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <lv2/ui/ui.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

#define ALO_UI_URI "http://ktano-studio.com/aloschen#ui"

/* Reuse the DSP's canonical port indices + plugin URI. */
#include "alo_engine.h"

/*
 * UI scaling (integer geometry only).
 *
 * Set UI_SCALE to taste. 1.0 = original size.
 */
#ifndef UI_SCALE
#define UI_SCALE 1.25f
#endif

#define UI_SI(v) ((int)((float)(v) * (UI_SCALE)))
#define UI_SUI(v) ((unsigned int)UI_SI(v))

typedef enum {
  CTL_TOGGLE,
  CTL_TRIGGER,
  CTL_SLIDER_INT,
  CTL_SLIDER_FLOAT
} ControlType;

typedef struct {
  uint32_t port_index;
  uint32_t display_port_index;
  const char* label;
  ControlType type;
  float min;
  float max;
} Control;

#define CTL_TRIGGER_(port_, label_) {port_, port_, label_, CTL_TRIGGER, 0.0f, 1.0f}
#define CTL_LOOP_(port_, state_port_, label_) {port_, state_port_, label_, CTL_TRIGGER, 0.0f, 1.0f}
#define CTL_TOGGLE_(port_, label_) {port_, port_, label_, CTL_TOGGLE, 0.0f, 1.0f}
#define CTL_SLIDER_INT_(port_, label_, min_, max_) {port_, port_, label_, CTL_SLIDER_INT, min_, max_}
#define CTL_SLIDER_FLOAT_(port_, label_, min_, max_) {port_, port_, label_, CTL_SLIDER_FLOAT, min_, max_}

typedef enum {
  ALO_T1 = 0,
  ALO_T2 = 1,
  ALO_T3 = 2,
} AloTrack;

/* UI-only actions (not LV2 ports). */
#define ALO_UI_ACTION_MUTE_ALL (UINT32_MAX - 1u)

static inline bool ui_is_undo_port(const uint32_t port_index) {
  return port_index == ALO_UNDO1 || port_index == ALO_UNDO2 || port_index == ALO_UNDO3;
}

static inline bool ui_is_loop_state_port(const uint32_t port_index) {
  return port_index == ALO_LOOP1_STATE || port_index == ALO_LOOP2_STATE || port_index == ALO_LOOP3_STATE;
}

static inline bool ui_is_armed_waiting(const float state_v) { return (state_v >= 0.20f) && (state_v < 0.45f); }
static inline bool ui_is_playing(const float state_v) { return (state_v >= 0.45f) && (state_v < 0.75f); }
static inline bool ui_is_recording(const float state_v) { return (state_v >= 0.75f); }

static inline int ui_track_from_ui_port(const uint32_t port_index) {
  switch (port_index) {
    case ALO_LOOP1:
    case ALO_UNDO1:
    case ALO_LOOP1_STATE:
    case ALO_UNDO1_STATE:
    case ALO_LOOP1_HAS_AUDIO:
      return 0;
    case ALO_LOOP2:
    case ALO_UNDO2:
    case ALO_LOOP2_STATE:
    case ALO_UNDO2_STATE:
    case ALO_LOOP2_HAS_AUDIO:
      return 1;
    case ALO_LOOP3:
    case ALO_UNDO3:
    case ALO_LOOP3_STATE:
    case ALO_UNDO3_STATE:
    case ALO_LOOP3_HAS_AUDIO:
      return 2;
    default:
      return -1;
  }
}

static inline uint32_t ui_loop_has_audio_port_for_track(const int t) {
  switch (t) {
    case 0:
      return ALO_LOOP1_HAS_AUDIO;
    case 1:
      return ALO_LOOP2_HAS_AUDIO;
    case 2:
      return ALO_LOOP3_HAS_AUDIO;
    default:
      return ALO_LOOP1_HAS_AUDIO;
  }
}

static inline uint32_t ui_loop_input_port_for_state_port(const uint32_t state_port) {
  switch (state_port) {
    case ALO_LOOP1_STATE:
      return ALO_LOOP1;
    case ALO_LOOP2_STATE:
      return ALO_LOOP2;
    case ALO_LOOP3_STATE:
      return ALO_LOOP3;
    default:
      return UINT32_MAX;
  }
}

static const Control kControls[] = {
  CTL_LOOP_(ALO_LOOP1, ALO_LOOP1_STATE, "Loop1"),
  CTL_LOOP_(ALO_UNDO1, ALO_UNDO1_STATE, "Undo1"),
  CTL_LOOP_(ALO_LOOP2, ALO_LOOP2_STATE, "Loop2"),
  CTL_LOOP_(ALO_UNDO2, ALO_UNDO2_STATE, "Undo2"),
  CTL_LOOP_(ALO_LOOP3, ALO_LOOP3_STATE, "Loop3"),
  CTL_LOOP_(ALO_UNDO3, ALO_UNDO3_STATE, "Undo3"),

  /* Isolation helpers */
  {ALO_UI_ACTION_MUTE_ALL, ALO_UI_ACTION_MUTE_ALL, "Mute All", CTL_TRIGGER, 0.0f, 1.0f},
  CTL_TOGGLE_(ALO_FREEZE_MODE, "Dump"),

  CTL_SLIDER_FLOAT_(ALO_LOOP1_VOL, "Loop1 Vol", 0.0f, 1.0f),
  CTL_SLIDER_FLOAT_(ALO_LOOP2_VOL, "Loop2 Vol", 0.0f, 1.0f),
  CTL_SLIDER_FLOAT_(ALO_LOOP3_VOL, "Loop3 Vol", 0.0f, 1.0f),
  CTL_SLIDER_FLOAT_(ALO_SAMPLER_VOL, "Sampler Vol", 0.0f, 1.0f),

  CTL_SLIDER_INT_(ALO_BARS, "Bars", 1.0f, 16.0f),
  CTL_SLIDER_INT_(ALO_CLICK, "Click", 0.0f, 10.0f),
  CTL_SLIDER_INT_(ALO_MIX, "Mix", 0.0f, 100.0f),
  CTL_SLIDER_INT_(ALO_SLICE_ROOT, "Slice Root", 0.0f, 127.0f),
};

typedef enum {
  HIT_NONE = -1,
  HIT_BUTTON = 0,
  HIT_SLIDER = 1,
} HitType;

typedef struct {
  Display* dpy;
  int screen;
  Window parent;
  Window win;
  GC gc;

  Colormap cmap;
  unsigned long col_fg;
  unsigned long col_bg;
  unsigned long col_grey;
  unsigned long col_cycle;
  unsigned long col_cycle_past;
  unsigned long col_host;
  unsigned long col_active;

  unsigned int width;
  unsigned int height;

  LV2UI_Write_Function write;
  LV2UI_Controller controller;

  float port_values[ALO_PORT_COUNT];
  bool needs_redraw;

  bool last_blink_on;

  int active_control; /* index into kControls */
  HitType active_hit;

  bool mute_all_on;
  bool have_saved_vols;
  float saved_loop_vol[NUM_TRACKS];
} AloUI;

static inline bool undo_is_enabled(const AloUI* ui, uint32_t undo_port_index) {
  if (!ui) {
    return true;
  }

  const int t = ui_track_from_ui_port(undo_port_index);
  if (t < 0) {
    return true;
  }

  return ui->port_values[ui_loop_has_audio_port_for_track(t)] > 0.5f;
}

typedef struct {
  int pad;
  int header_h;

  int rings_x0;
  int rings_y0;
  int rings_d;

  int buttons_x0;
  int buttons_y0;
  int btn_w;
  int btn_h;
  int btn_gap;
  int slider_h;
  int row_h;
  int slider_y0;
} UILayout;

static UILayout ui_layout(const AloUI* ui) {
  (void)ui;
  UILayout l;
  l.pad = UI_SI(10);
  l.header_h = UI_SI(22);

  l.rings_x0 = l.pad;
  l.rings_y0 = l.pad + l.header_h + UI_SI(6);
  l.rings_d = UI_SI(120);

  l.buttons_x0 = l.rings_x0 + l.rings_d + l.pad;
  l.buttons_y0 = l.rings_y0;

  l.btn_w = UI_SI(104);
  l.btn_h = UI_SI(34);
  l.btn_gap = UI_SI(10);
  l.slider_h = UI_SI(18);
  l.row_h = UI_SI(44);

  /* Place sliders below the lower of: rings box, or button grid (2 rows). */
  const int buttons_h = 2 * l.btn_h + l.btn_gap;
  int content_h = l.rings_y0 + l.rings_d;
  if (l.buttons_y0 + buttons_h > content_h) {
    content_h = l.buttons_y0 + buttons_h;
  }
  l.slider_y0 = content_h + UI_SI(18);
  return l;
}

static int ui_get_bars_i(const AloUI* ui) {
  float bars_f = (ui && ALO_BARS < ALO_PORT_COUNT) ? ui->port_values[ALO_BARS]
                                                   : (float)DEFAULT_NUM_BARS;
  if (bars_f < 1.0f) {
    bars_f = 1.0f;
  }
  if (bars_f > 16.0f) {
    bars_f = 16.0f;
  }
  int bars_i = (int)lrintf(bars_f);
  return (bars_i > 0) ? bars_i : 1;
}

static void ui_draw_bar_steps(AloUI* ui, int x, int y) {
  if (!ui) {
    return;
  }

  /* A lightweight loop-position indicator: one step per beat (4 per bar). */
  const int bars_i = ui_get_bars_i(ui);
  const int steps = bars_i * DEFAULT_BEATS_PER_BAR;
  if (steps <= 0) {
    return;
  }

  const int avail_w = (int)ui->width - 2 * x;
  const int box_max = UI_SI(14);
  const int box_min = 1;
  int gap = UI_SI(4);
  if (gap < 1) {
    gap = 1;
  }

  int box = box_max;
  if (avail_w > 0) {
    const int denom = steps;
    const int numer = avail_w - (steps - 1) * gap;
    int fit = (denom > 0) ? (numer / denom) : box_max;
    if (fit > box_max) {
      fit = box_max;
    }
    if (fit < box_min) {
      gap = 1;
      const int numer2 = avail_w - (steps - 1) * gap;
      fit = (denom > 0) ? (numer2 / denom) : box_min;
      if (fit < box_min) {
        fit = box_min;
      }
    }
    box = fit;
  }

  int cur = -1;
  if (ALO_BAR_STEP < ALO_PORT_COUNT) {
    const float v = ui->port_values[ALO_BAR_STEP];
    if (v >= -0.5f) {
      cur = (int)lrintf(v);
    }
  }

  if (cur < 0 || cur >= steps) {
    cur = -1;
  }

  for (int i = 0; i < steps; ++i) {
    const int bx = x + i * (box + gap);
    XDrawRectangle(ui->dpy, ui->win, ui->gc, bx, y, (unsigned int)box, (unsigned int)box);
    if (i == cur) {
      XFillRectangle(ui->dpy, ui->win, ui->gc, bx + 1, y + 1, (unsigned int)(box - 1),
                     (unsigned int)(box - 1));
    }
  }
}

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static bool ui_blink_on(void) {
  return ((monotonic_ms() / 250ull) % 2ull) == 0ull;
}

static float clampf(const float v, const float lo, const float hi) {
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

static float round_int_value(const float v) {
  return (float)((v >= 0.0f) ? (int)(v + 0.5f) : (int)(v - 0.5f));
}

static void draw_string(AloUI* ui, int x, int y, const char* text);

static float clamp01f(const float v) {
  if (!(v >= 0.0f)) {
    return 0.0f;
  }
  if (v > 1.0f) {
    return 1.0f;
  }
  return v;
}

static unsigned long ui_alloc_named_color(AloUI* ui, const char* name, unsigned long fallback) {
  if (!ui || !ui->dpy || !name) {
    return fallback;
  }

  XColor scr;
  XColor exact;
  if (XAllocNamedColor(ui->dpy, ui->cmap, name, &scr, &exact)) {
    return scr.pixel;
  }
  return fallback;
}

static void ui_set_fg(AloUI* ui, unsigned long pixel) {
  if (!ui || !ui->dpy) {
    return;
  }
  XSetForeground(ui->dpy, ui->gc, pixel);
}

static void ui_draw_arc_deg(AloUI* ui, int cx, int cy, int r, int start_deg, int extent_deg) {
  if (!ui || !ui->dpy || r <= 0) {
    return;
  }

  const int d = 2 * r;
  const int x = cx - r;
  const int y = cy - r;
  XDrawArc(ui->dpy, ui->win, ui->gc, x, y, (unsigned int)d, (unsigned int)d,
           start_deg * 64, extent_deg * 64);
}

static void ui_draw_transport_rings(AloUI* ui, const UILayout* l) {
  if (!ui || !l) {
    return;
  }

  const int x0 = l->rings_x0;
  const int y0 = l->rings_y0;
  const int d0 = l->rings_d;
  if (d0 < UI_SI(40)) {
    return;
  }

  /* Map the MOD SVG radii (46/38/26/14) into pixels. */
  const int stroke_outer = UI_SI(3);
  const int outer_r = (d0 / 2) - stroke_outer - UI_SI(2);
  if (outer_r < UI_SI(10)) {
    return;
  }
  const float s = (float)outer_r / 46.0f;

  const int r_bars = outer_r;
  const int r_steps = (int)lrintf(38.0f * s);
  const int r_cycle = (int)lrintf(26.0f * s);
  const int r_host = (int)lrintf(14.0f * s);

  const int cx = x0 + d0 / 2;
  const int cy = y0 + d0 / 2;

  const int bars = ui_get_bars_i(ui);
  int steps = bars * DEFAULT_BEATS_PER_BAR;
  if (steps < 1) {
    steps = 1;
  }
  if (steps > 128) {
    steps = 128;
  }

  int bar_step = -1;
  if (ALO_BAR_STEP < ALO_PORT_COUNT) {
    const float v = ui->port_values[ALO_BAR_STEP];
    if (v >= -0.5f) {
      bar_step = (int)lrintf(v);
    }
  }

  float cycle_phase = 0.0f;
  float host_phase = 0.0f;
  if (ALO_CYCLE_PHASE < ALO_PORT_COUNT) {
    cycle_phase = clamp01f(ui->port_values[ALO_CYCLE_PHASE]);
  }
  if (ALO_HOST_BAR_PHASE < ALO_PORT_COUNT) {
    host_phase = clamp01f(ui->port_values[ALO_HOST_BAR_PHASE]);
  }

  /* Angles: X11 uses 0deg at 3 o'clock, CCW positive.
   * We want 0 at 12 o'clock and clockwise progression.
   */
  const int start0 = 90;

  /* Background circle (subtle). */
  ui_set_fg(ui, ui->col_grey);
  XSetLineAttributes(ui->dpy, ui->gc, (unsigned int)UI_SI(3), LineSolid, CapRound, JoinRound);
  ui_draw_arc_deg(ui, cx, cy, r_cycle, 0, 360);

  /* Bars ring: dashed segments + active bar segment. */
  const float seg_bars = 360.0f / (float)bars;
  const float on_bars = seg_bars * 0.78f;
  const int w_bars = UI_SI(3);
  XSetLineAttributes(ui->dpy, ui->gc, (unsigned int)w_bars, LineSolid, CapButt, JoinMiter);
  ui_set_fg(ui, ui->col_grey);
  for (int i = 0; i < bars; ++i) {
    const int a0 = (int)lrintf((float)start0 - (float)i * seg_bars);
    ui_draw_arc_deg(ui, cx, cy, r_bars, a0, (int)lrintf(-on_bars));
  }

  int bar_index = 0;
  if (bar_step >= 0) {
    bar_index = bar_step / 4;
    if (bar_index < 0) {
      bar_index = 0;
    } else if (bar_index >= bars) {
      bar_index = bars - 1;
    }
  }
  ui_set_fg(ui, ui->col_active);
  {
    const int a0 = (int)lrintf((float)start0 - (float)bar_index * seg_bars);
    ui_draw_arc_deg(ui, cx, cy, r_bars, a0, (int)lrintf(-on_bars));
  }

  /* Steps ring: dashed segments + active step segment. */
  const float seg_steps = 360.0f / (float)steps;
  const float on_steps = seg_steps * 0.55f;
  const int w_steps = UI_SI(3);
  XSetLineAttributes(ui->dpy, ui->gc, (unsigned int)w_steps, LineSolid, CapButt, JoinMiter);
  ui_set_fg(ui, ui->col_cycle);
  for (int i = 0; i < steps; ++i) {
    const int a0 = (int)lrintf((float)start0 - (float)i * seg_steps);
    ui_draw_arc_deg(ui, cx, cy, r_steps, a0, (int)lrintf(-on_steps));
  }

  int step_index = bar_step;
  if (!(step_index >= 0 && step_index < steps)) {
    step_index = -1;
  }

  /* Steps: future grey, past dark green, current bright green. */
  for (int i = 0; i < steps; ++i) {
    if (step_index < 0) {
      ui_set_fg(ui, ui->col_grey);
    } else if (i < step_index) {
      ui_set_fg(ui, ui->col_cycle_past);
    } else if (i == step_index) {
      ui_set_fg(ui, ui->col_cycle);
    } else {
      ui_set_fg(ui, ui->col_grey);
    }
    const int a0 = (int)lrintf((float)start0 - (float)i * seg_steps);
    ui_draw_arc_deg(ui, cx, cy, r_steps, a0, (int)lrintf(-on_steps));
  }

  /* Step dots (subtle), with active dot filled. */
  const int dot_r = UI_SI(2);
  const int dot_r_active = UI_SI(3);
  const double kPi = 3.14159265358979323846;
  for (int i = 0; i < steps; ++i) {
    const double a = ((double)i / (double)steps) * (kPi * 2.0) - (kPi / 2.0);
    const int dx = cx + (int)lrint(cos(a) * (double)r_steps);
    const int dy = cy + (int)lrint(sin(a) * (double)r_steps);
    const bool is_cur = (step_index >= 0 && i == step_index);
    const int rr = is_cur ? dot_r_active : dot_r;

    if (is_cur) {
      ui_set_fg(ui, ui->col_cycle);
      XFillArc(ui->dpy, ui->win, ui->gc, dx - rr, dy - rr, (unsigned int)(2 * rr),
               (unsigned int)(2 * rr), 0, 360 * 64);
    } else {
      if (step_index < 0) {
        ui_set_fg(ui, ui->col_grey);
      } else if (i < step_index) {
        ui_set_fg(ui, ui->col_cycle_past);
      } else {
        ui_set_fg(ui, ui->col_grey);
      }
      XDrawArc(ui->dpy, ui->win, ui->gc, dx - rr, dy - rr, (unsigned int)(2 * rr),
               (unsigned int)(2 * rr), 0, 360 * 64);
    }
  }

  /* Smooth rings: cycle phase (outer) + host bar phase (inner). */
  const int w_cycle = UI_SI(6);
  const int w_host = UI_SI(6);
  XSetLineAttributes(ui->dpy, ui->gc, (unsigned int)w_cycle, LineSolid, CapRound, JoinRound);
  ui_set_fg(ui, ui->col_cycle);
  ui_draw_arc_deg(ui, cx, cy, r_cycle, start0, (int)lrintf(-360.0f * cycle_phase));

  XSetLineAttributes(ui->dpy, ui->gc, (unsigned int)w_host, LineSolid, CapRound, JoinRound);
  ui_set_fg(ui, ui->col_host);
  ui_draw_arc_deg(ui, cx, cy, r_host, start0, (int)lrintf(-360.0f * host_phase));

  /* Restore defaults for rest of UI. */
  XSetLineAttributes(ui->dpy, ui->gc, (unsigned int)UI_SI(1), LineSolid, CapButt, JoinMiter);
  ui_set_fg(ui, ui->col_fg);

  draw_string(ui, x0, y0 - UI_SI(4), "SEQ");
}

static bool ui_trigger_grid_pos(const Control* c, int* out_col, int* out_row) {
  if (!c || !out_col || !out_row) {
    return false;
  }

  /* Arrange loop logic as:
   * Row 0: Loop1 Loop2 Loop3
   * Row 1: Undo1 Undo2 Undo3
   */
  const int t = ui_track_from_ui_port(c->port_index);
  if (t < 0 || t >= NUM_TRACKS) {
    return false;
  }

  const bool is_undo = ui_is_undo_port(c->port_index);
  *out_col = t;
  *out_row = is_undo ? 1 : 0;
  return true;
}

static void ui_send_port(AloUI* ui, const uint32_t port_index, float value) {
  if (!ui || !ui->write) {
    return;
  }

  if (port_index >= ALO_PORT_COUNT) {
    return;
  }

  ui->port_values[port_index] = value;
  ui->write(ui->controller, port_index, sizeof(float), 0, &value);
}

static void draw_string(AloUI* ui, int x, int y, const char* text) {
  if (!text) {
    return;
  }
  XDrawString(ui->dpy, ui->win, ui->gc, x, y, text, (int)strlen(text));
}

static bool ui_button_is_on(const AloUI* ui, const Control* c, const bool blink_on) {
  if (!ui || !c) {
    return false;
  }

  if (c->port_index >= ALO_PORT_COUNT || c->display_port_index >= ALO_PORT_COUNT) {
    return false;
  }

  const float state_v = ui->port_values[c->display_port_index];
  const float in_v = ui->port_values[c->port_index];

  const bool is_loop_button = (c->display_port_index != c->port_index);
  if (is_loop_button) {
    return ui_is_recording(state_v) || ui_is_playing(state_v) || (ui_is_armed_waiting(state_v) && blink_on);
  }

  if (ui_is_undo_port(c->port_index)) {
    /* Undo buttons: show queued undo (blink) from undo*_state output */
    const bool queued = (state_v >= 0.20f);
    return queued && blink_on;
  }

  return (in_v >= 0.5f);
}

static void ui_draw_button(AloUI* ui, const int bx, const int by, const int bw, const int bh,
                           const Control* c, const bool blink_on) {
  if (!ui || !c) {
    return;
  }

  XDrawRectangle(ui->dpy, ui->win, ui->gc, bx, by, bw, bh);

  const bool on = ui_button_is_on(ui, c, blink_on);
  if (on) {
    XFillRectangle(ui->dpy, ui->win, ui->gc, bx + 1, by + 1, bw - 1, bh - 1);
    XSetForeground(ui->dpy, ui->gc, WhitePixel(ui->dpy, ui->screen));
    draw_string(ui, bx + UI_SI(10), by + UI_SI(20), c->label);
    XSetForeground(ui->dpy, ui->gc, BlackPixel(ui->dpy, ui->screen));
  } else {
    draw_string(ui, bx + UI_SI(10), by + UI_SI(20), c->label);
  }
}

static void ui_redraw(AloUI* ui) {
  if (!ui || !ui->dpy) {
    return;
  }

  const bool blink_on = ui_blink_on();

  XClearWindow(ui->dpy, ui->win);

  const UILayout l = ui_layout(ui);

  int x = l.pad;
  int y = l.pad;

  /* Header */
  draw_string(ui, l.pad, UI_SI(18), "ALOSCHEN — native UI");
  draw_string(ui, (int)ui->width - UI_SI(180), UI_SI(18), "Carlo Cattano");
  y += l.header_h;

  /* Circular timing UI (transport rings). */
  ui_draw_transport_rings(ui, &l);

  /* Trigger buttons row */
  const int btn_w = l.btn_w;
  const int btn_h = l.btn_h;
  const int btn_gap = l.btn_gap;

  x = l.buttons_x0;
  y = l.buttons_y0;

  int fallback_i = 0;
  for (int i = 0; i < ARRAY_LEN(kControls); ++i) {
    const Control* c = &kControls[i];
    if (c->type != CTL_TOGGLE && c->type != CTL_TRIGGER) {
      continue;
    }

    int col = 0;
    int row = 0;
    if (!ui_trigger_grid_pos(c, &col, &row)) {
      /* Fallback: place sequentially below the 2-row grid if new controls are added later. */
      col = fallback_i % 3;
      row = 2 + (fallback_i / 3);
      fallback_i++;
    }

    const int bx = l.buttons_x0 + col * (btn_w + btn_gap);
    const int by = l.buttons_y0 + row * (btn_h + btn_gap);
    ui_draw_button(ui, bx, by, btn_w, btn_h, c, blink_on);
  }

  x = l.pad;
  y = l.slider_y0;

  /* Sliders */
  const int slider_w = (int)ui->width - 2 * l.pad;
  const int slider_h = l.slider_h;
  const int row_h = l.row_h;

  int slider_index = 0;
  for (int i = 0; i < ARRAY_LEN(kControls); ++i) {
    const Control* c = &kControls[i];
    if (c->type != CTL_SLIDER_INT && c->type != CTL_SLIDER_FLOAT) {
      continue;
    }

    const int sy = y + slider_index * row_h;

    char label[128];
    const float v = ui->port_values[c->port_index];
    if (c->type == CTL_SLIDER_FLOAT) {
      snprintf(label, sizeof(label), "%s: %.2f", c->label, v);
    } else {
      snprintf(label, sizeof(label), "%s: %.0f", c->label, v);
    }
    draw_string(ui, x, sy + UI_SI(12), label);

    const int bar_x = x;
    const int bar_y = sy + UI_SI(18);
    XDrawRectangle(ui->dpy, ui->win, ui->gc, bar_x, bar_y, slider_w, slider_h);

    const float norm = (c->max > c->min) ? ((v - c->min) / (c->max - c->min)) : 0.0f;
    const int fill_w = (int)(clampf(norm, 0.0f, 1.0f) * (float)(slider_w - 2));
    XFillRectangle(ui->dpy, ui->win, ui->gc, bar_x + 1, bar_y + 1, (unsigned int)fill_w,
                   (unsigned int)(slider_h - 2));

    slider_index++;
  }

  /* Bar step indicator row (bottom). */
  {
    const int box = UI_SI(14);
    const int steps_y = (int)ui->height - l.pad - box;
    draw_string(ui, l.pad, steps_y - UI_SI(6), "Cycle");
    ui_draw_bar_steps(ui, l.pad, steps_y);
  }

  XFlush(ui->dpy);
  ui->needs_redraw = false;
}

static bool point_in_rect(const int px, const int py, const int x, const int y, const int w,
                          const int h) {
  return px >= x && px < (x + w) && py >= y && py < (y + h);
}

static int hit_test(AloUI* ui, const int px, const int py, HitType* out_type) {
  const UILayout l = ui_layout(ui);
  int x = l.buttons_x0;
  int y = l.buttons_y0;

  const int btn_w = l.btn_w;
  const int btn_h = l.btn_h;
  const int btn_gap = l.btn_gap;

  /* Toggles */
  int fallback_i = 0;
  for (int i = 0; i < ARRAY_LEN(kControls); ++i) {
    const Control* c = &kControls[i];
    if (c->type != CTL_TOGGLE && c->type != CTL_TRIGGER) {
      continue;
    }

    int col = 0;
    int row = 0;
    if (!ui_trigger_grid_pos(c, &col, &row)) {
      col = fallback_i % 3;
      row = 2 + (fallback_i / 3);
      fallback_i++;
    }

    const int bx = l.buttons_x0 + col * (btn_w + btn_gap);
    const int by = l.buttons_y0 + row * (btn_h + btn_gap);

    if (point_in_rect(px, py, bx, by, btn_w, btn_h)) {
      *out_type = HIT_BUTTON;
      return i;
    }
  }

  /* Sliders */
  x = l.pad;
  y = l.slider_y0;

  const int slider_w = (int)ui->width - 2 * l.pad;
  const int slider_h = l.slider_h;
  const int row_h = l.row_h;

  int slider_index = 0;
  for (int i = 0; i < ARRAY_LEN(kControls); ++i) {
    const Control* c = &kControls[i];
    if (c->type != CTL_SLIDER_INT && c->type != CTL_SLIDER_FLOAT) {
      continue;
    }

    const int sy = y + slider_index * row_h;
    const int bar_x = x;
    const int bar_y = sy + UI_SI(18);

    if (point_in_rect(px, py, bar_x, bar_y, slider_w, slider_h)) {
      *out_type = HIT_SLIDER;
      return i;
    }

    slider_index++;
  }

  *out_type = HIT_NONE;
  return -1;
}

static void update_slider_from_x(AloUI* ui, const int control_index, const int px) {
  const Control* c = &kControls[control_index];
  const UILayout l = ui_layout(ui);
  const int slider_w = (int)ui->width - 2 * l.pad;

  const float t = clampf(((float)(px - l.pad) / (float)slider_w), 0.0f, 1.0f);
  float v = c->min + t * (c->max - c->min);
  if (c->type == CTL_SLIDER_INT) {
    v = round_int_value(v);
    v = clampf(v, c->min, c->max);
  } else {
    /* Keep float sliders reasonably stable (2 decimal places). */
    v = floorf(v * 100.0f + 0.5f) * 0.01f;
    v = clampf(v, c->min, c->max);
  }

  ui_send_port(ui, c->port_index, v);
  ui->needs_redraw = true;
}

static void handle_button_press(AloUI* ui, const XButtonEvent* e) {
  HitType hit_type = HIT_NONE;
  const int index = hit_test(ui, e->x, e->y, &hit_type);
  if (index < 0) {
    return;
  }

  ui->active_control = index;
  ui->active_hit = hit_type;

  const Control* c = &kControls[index];
  if (hit_type == HIT_BUTTON) {
    if (c->port_index == ALO_UI_ACTION_MUTE_ALL) {
      const float v1 = (ALO_LOOP1_VOL < ALO_PORT_COUNT) ? ui->port_values[ALO_LOOP1_VOL] : 1.0f;
      const float v2 = (ALO_LOOP2_VOL < ALO_PORT_COUNT) ? ui->port_values[ALO_LOOP2_VOL] : 1.0f;
      const float v3 = (ALO_LOOP3_VOL < ALO_PORT_COUNT) ? ui->port_values[ALO_LOOP3_VOL] : 1.0f;

      const bool any_on = (v1 > 1e-6f) || (v2 > 1e-6f) || (v3 > 1e-6f);
      if (any_on) {
        ui->saved_loop_vol[0] = v1;
        ui->saved_loop_vol[1] = v2;
        ui->saved_loop_vol[2] = v3;
        ui->have_saved_vols = true;
        ui->mute_all_on = true;
        ui_send_port(ui, ALO_LOOP1_VOL, 0.0f);
        ui_send_port(ui, ALO_LOOP2_VOL, 0.0f);
        ui_send_port(ui, ALO_LOOP3_VOL, 0.0f);
      } else {
        const float r1 = ui->have_saved_vols ? ui->saved_loop_vol[0] : 1.0f;
        const float r2 = ui->have_saved_vols ? ui->saved_loop_vol[1] : 1.0f;
        const float r3 = ui->have_saved_vols ? ui->saved_loop_vol[2] : 1.0f;
        ui->mute_all_on = false;
        ui_send_port(ui, ALO_LOOP1_VOL, r1);
        ui_send_port(ui, ALO_LOOP2_VOL, r2);
        ui_send_port(ui, ALO_LOOP3_VOL, r3);
      }
      ui->needs_redraw = true;
      ui->active_control = -1;
      ui->active_hit = HIT_NONE;
      return;
    }

    if ((c->port_index == ALO_UNDO1 || c->port_index == ALO_UNDO2 || c->port_index == ALO_UNDO3) &&
        !undo_is_enabled(ui, c->port_index)) {
      ui->active_control = -1;
      ui->active_hit = HIT_NONE;
      return;
    }

    if (c->type == CTL_TRIGGER) {
      /* Momentary trigger: press sends 1, release sends 0 */
      ui_send_port(ui, c->port_index, 1.0f);
      ui->needs_redraw = true;
    } else if (c->type == CTL_TOGGLE) {
      /* Latching toggle: each press flips state */
      const float state_v = ui->port_values[c->display_port_index];
      const float in_v = ui->port_values[c->port_index];
      const float cur = ((state_v >= 0.5f) || (in_v >= 0.5f)) ? 1.0f : 0.0f;
      const float next = (cur >= 0.5f) ? 0.0f : 1.0f;
      ui_send_port(ui, c->port_index, next);
      ui->needs_redraw = true;
    }
  } else if (hit_type == HIT_SLIDER) {
    update_slider_from_x(ui, index, e->x);
  }
}

static void handle_motion(AloUI* ui, const XMotionEvent* e) {
  if (ui->active_hit != HIT_SLIDER || ui->active_control < 0) {
    return;
  }
  update_slider_from_x(ui, ui->active_control, e->x);
}

static void handle_button_release(AloUI* ui, const XButtonEvent* e) {
  (void)e;
  if (ui->active_hit == HIT_BUTTON && ui->active_control >= 0) {
    const Control* c = &kControls[ui->active_control];
    if (c->type == CTL_TRIGGER) {
      ui_send_port(ui, c->port_index, 0.0f);
      ui->needs_redraw = true;
    }
  }
  ui->active_control = -1;
  ui->active_hit = HIT_NONE;
}

static void handle_configure(AloUI* ui, const XConfigureEvent* e) {
  if (!ui) {
    return;
  }
  if (ui->width != (unsigned int)e->width || ui->height != (unsigned int)e->height) {
    ui->width = (unsigned int)e->width;
    ui->height = (unsigned int)e->height;
    ui->needs_redraw = true;
  }
}

static bool ui_any_armed_waiting(const AloUI* ui) {
  if (!ui) {
    return false;
  }

  const uint32_t ports[] = {ALO_LOOP1_STATE, ALO_LOOP2_STATE, ALO_LOOP3_STATE};
  for (int i = 0; i < (int)ARRAY_LEN(ports); ++i) {
    const uint32_t p = ports[i];
    if (p < ALO_PORT_COUNT && ui_is_armed_waiting(ui->port_values[p])) {
      return true;
    }
  }

  return false;
}

static int ui_idle(LV2UI_Handle handle) {
  AloUI* ui = (AloUI*)handle;
  if (!ui || !ui->dpy) {
    return 0;
  }

  /* Force periodic redraw while any loop is armed (waiting-to-record) to blink. */
  if (ui_any_armed_waiting(ui)) {
    const bool blink_on = ui_blink_on();
    if (blink_on != ui->last_blink_on) {
      ui->last_blink_on = blink_on;
      ui->needs_redraw = true;
    }
  }

  while (XPending(ui->dpy) > 0) {
    XEvent ev;
    XNextEvent(ui->dpy, &ev);

    switch (ev.type) {
      case Expose:
        ui->needs_redraw = true;
        break;
      case ConfigureNotify:
        handle_configure(ui, &ev.xconfigure);
        break;
      case ButtonPress:
        handle_button_press(ui, &ev.xbutton);
        break;
      case ButtonRelease:
        handle_button_release(ui, &ev.xbutton);
        break;
      case MotionNotify:
        handle_motion(ui, &ev.xmotion);
        break;
      default:
        break;
    }
  }

  if (ui->needs_redraw) {
    ui_redraw(ui);
  }

  return 0;
}

static int ui_show(LV2UI_Handle handle) {
  AloUI* ui = (AloUI*)handle;
  if (!ui || !ui->dpy) {
    return 1;
  }
  XMapRaised(ui->dpy, ui->win);
  ui->needs_redraw = true;
  return 0;
}

static int ui_hide(LV2UI_Handle handle) {
  AloUI* ui = (AloUI*)handle;
  if (!ui || !ui->dpy) {
    return 1;
  }
  XUnmapWindow(ui->dpy, ui->win);
  return 0;
}

static void ui_cleanup(LV2UI_Handle handle) {
  AloUI* ui = (AloUI*)handle;
  if (!ui) {
    return;
  }

  if (ui->dpy && ui->win) {
    XDestroyWindow(ui->dpy, ui->win);
  }
  if (ui->dpy) {
    XCloseDisplay(ui->dpy);
  }

  free(ui);
}

static void ui_port_event(LV2UI_Handle handle, uint32_t port_index, uint32_t buffer_size,
                          uint32_t format, const void* buffer) {
  AloUI* ui = (AloUI*)handle;
  (void)format;

  if (!ui || !buffer) {
    return;
  }

  if (port_index >= ALO_PORT_COUNT) {
    return;
  }

  if (buffer_size < sizeof(float)) {
    return;
  }

  const float v = *(const float*)buffer;

  /* Throttle redraw for high-rate phase outputs (keeps CPU reasonable). */
  const float old = ui->port_values[port_index];
  if (port_index == ALO_CYCLE_PHASE || port_index == ALO_HOST_BAR_PHASE) {
    if (fabsf(v - old) < 0.0025f) {
      return;
    }
  }

  ui->port_values[port_index] = v;

  /*
   * If the DSP reports a loop state change, keep the corresponding loop input
   * parameter in sync. This matters for auto-stop: the DSP can turn the loop
   * off, but it cannot write to the *input* control port, so we do it here.
   */
  if (ui_is_loop_state_port(port_index)) {
    const uint32_t in_port = ui_loop_input_port_for_state_port(port_index);

    /* When DSP turns state off (auto-stop, undo), ensure the host parameter is also set to 0. */
    if (in_port != UINT32_MAX && v < 0.5f && ui->write) {
      ui->port_values[in_port] = 0.0f;
      const float zero = 0.0f;
      ui->write(ui->controller, in_port, sizeof(float), 0, &zero);
    }
  }

  ui->needs_redraw = true;
}

static LV2UI_Handle ui_instantiate(const LV2UI_Descriptor* descriptor, const char* plugin_uri,
                                  const char* bundle_path, LV2UI_Write_Function write_function,
                                  LV2UI_Controller controller, LV2UI_Widget* widget,
                                  const LV2_Feature* const* features) {
  (void)descriptor;
  (void)bundle_path;

  if (!plugin_uri || strcmp(plugin_uri, ALO_URI)) {
    return NULL;
  }

  Window parent = 0;
  for (int i = 0; features && features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_UI__parent)) {
      parent = (Window)(uintptr_t)features[i]->data;
    }
  }

  if (!parent) {
    return NULL;
  }

  AloUI* ui = (AloUI*)calloc(1, sizeof(AloUI));
  if (!ui) {
    return NULL;
  }

  ui->write = write_function;
  ui->controller = controller;
  ui->parent = parent;
  ui->width = UI_SUI(640);
  ui->height = UI_SUI(520);
  ui->active_control = -1;
  ui->active_hit = HIT_NONE;

  ui->dpy = XOpenDisplay(NULL);
  if (!ui->dpy) {
    free(ui);
    return NULL;
  }

  ui->screen = DefaultScreen(ui->dpy);

  ui->cmap = DefaultColormap(ui->dpy, ui->screen);
  ui->col_fg = BlackPixel(ui->dpy, ui->screen);
  ui->col_bg = WhitePixel(ui->dpy, ui->screen);
  ui->col_grey = ui->col_fg;
  ui->col_cycle = ui->col_fg;
  ui->col_cycle_past = ui->col_fg;
  ui->col_host = ui->col_fg;
  ui->col_active = ui->col_fg;

  ui->win = XCreateSimpleWindow(ui->dpy, ui->parent, 0, 0, ui->width, ui->height, 0,
                                BlackPixel(ui->dpy, ui->screen),
                                WhitePixel(ui->dpy, ui->screen));

  XSelectInput(ui->dpy, ui->win,
               ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                   StructureNotifyMask);

  ui->gc = XCreateGC(ui->dpy, ui->win, 0, NULL);
  XSetForeground(ui->dpy, ui->gc, ui->col_fg);
  XSetLineAttributes(ui->dpy, ui->gc, (unsigned int)UI_SI(1), LineSolid, CapButt, JoinMiter);

  /* Best-effort colors (fall back to black if unavailable). */
  ui->col_grey = ui_alloc_named_color(ui, "gray55", ui->col_fg);
  ui->col_cycle = ui_alloc_named_color(ui, "#00ff00", ui->col_fg);
  ui->col_cycle_past = ui_alloc_named_color(ui, "#008800", ui->col_fg);
  ui->col_host = ui_alloc_named_color(ui, "#0066cc", ui->col_fg);
  ui->col_active = ui_alloc_named_color(ui, "black", ui->col_fg);

  XMapWindow(ui->dpy, ui->win);
  XFlush(ui->dpy);

  *widget = (LV2UI_Widget)(uintptr_t)ui->win;

  /* Sensible defaults in case host doesn't send initial values */
  ui->port_values[ALO_LOOP1_VOL] = 1.0f;
  ui->port_values[ALO_LOOP2_VOL] = 1.0f;
  ui->port_values[ALO_LOOP3_VOL] = 1.0f;
  ui->port_values[ALO_SAMPLER_VOL] = 1.0f;
  ui->port_values[ALO_BARS] = 2.0f;
  ui->port_values[ALO_CLICK] = 1.0f;
  ui->port_values[ALO_MIX] = 50.0f;
  ui->port_values[ALO_SLICE_ROOT] = 36.0f;
  ui->port_values[ALO_BAR_STEP] = 0.0f;
  ui->port_values[ALO_CYCLE_PHASE] = 0.0f;
  ui->port_values[ALO_HOST_BAR_PHASE] = 0.0f;

  ui->needs_redraw = true;

  return (LV2UI_Handle)ui;
}

static const LV2UI_Idle_Interface kIdleInterface = {ui_idle};

static const LV2UI_Show_Interface kShowInterface = {
    ui_show,
    ui_hide,
};

static const void* ui_extension_data(const char* uri) {
  if (!uri) {
    return NULL;
  }

  if (!strcmp(uri, LV2_UI__idleInterface)) {
    return &kIdleInterface;
  }
  if (!strcmp(uri, LV2_UI__showInterface)) {
    return &kShowInterface;
  }

  return NULL;
}

static const LV2UI_Descriptor kUIDescriptor = {
    ALO_UI_URI,
    ui_instantiate,
    ui_cleanup,
    ui_port_event,
    ui_extension_data,
};

LV2_SYMBOL_EXPORT const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index) {
  switch (index) {
    case 0:
      return &kUIDescriptor;
    default:
      return NULL;
  }
}
