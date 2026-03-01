/*
  Minimal native X11 UI for the ALO LV2 plugin.

  This UI is a separate shared object (aloschen_ui.so) and communicates with the
  DSP only via LV2 UI callbacks (write_function / port_event).
*/

#include <lv2/ui/ui.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALO_URI "http://ktano-studio.com/aloschen"
#define ALO_UI_URI "http://ktano-studio.com/aloschen#ui"

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
  ALO_INPUT_L = 0,
  ALO_INPUT_R = 1,
  ALO_OUTPUT_L = 2,
  ALO_OUTPUT_R = 3,
  ALO_LOOP1 = 4,
  ALO_UNDO1 = 5,
  ALO_LOOP2 = 6,
  ALO_UNDO2 = 7,
  ALO_LOOP3 = 8,
  ALO_UNDO3 = 9,
  ALO_THRESHOLD = 10,
  ALO_MIDIIN = 11,
  ALO_MIDI_BASE = 12,
  ALO_INSTANT_LOOPS = 13,
  ALO_CLICK = 14,
  ALO_BARS = 15,
  ALO_CONTROL = 16,
  ALO_MIX = 17,
  ALO_RESET_MODE = 18,
  ALO_ENABLED = 19,
  ALO_LOOP1_STATE = 20,
  ALO_LOOP2_STATE = 21,
  ALO_LOOP3_STATE = 22,
  ALO_PORT_COUNT = 23
} PortIndex;

typedef enum {
  CTL_TOGGLE,
  CTL_TRIGGER,
  CTL_SLIDER_INT
} ControlType;

typedef struct {
  uint32_t port_index;
  uint32_t display_port_index;
  const char* label;
  ControlType type;
  float min;
  float max;
} Control;

static const Control kControls[] = {
  {ALO_LOOP1, ALO_LOOP1_STATE, "Loop1", CTL_TOGGLE, 0.0f, 1.0f},
  {ALO_UNDO1, ALO_UNDO1, "Undo1", CTL_TRIGGER, 0.0f, 1.0f},
  {ALO_LOOP2, ALO_LOOP2_STATE, "Loop2", CTL_TOGGLE, 0.0f, 1.0f},
  {ALO_UNDO2, ALO_UNDO2, "Undo2", CTL_TRIGGER, 0.0f, 1.0f},
  {ALO_LOOP3, ALO_LOOP3_STATE, "Loop3", CTL_TOGGLE, 0.0f, 1.0f},
  {ALO_UNDO3, ALO_UNDO3, "Undo3", CTL_TRIGGER, 0.0f, 1.0f},

    {ALO_BARS, ALO_BARS, "Bars", CTL_SLIDER_INT, 1.0f, 32.0f},
    {ALO_CLICK, ALO_CLICK, "Click", CTL_SLIDER_INT, 0.0f, 10.0f},
    {ALO_THRESHOLD, ALO_THRESHOLD, "Threshold", CTL_SLIDER_INT, -90.0f, 24.0f},
    {ALO_MIX, ALO_MIX, "Mix", CTL_SLIDER_INT, 0.0f, 100.0f},
    {ALO_INSTANT_LOOPS, ALO_INSTANT_LOOPS, "Instant", CTL_SLIDER_INT, 0.0f, 6.0f},
    {ALO_RESET_MODE, ALO_RESET_MODE, "Reset", CTL_SLIDER_INT, 0.0f, 3.0f},

    {ALO_MIDI_BASE, ALO_MIDI_BASE, "MIDI Base", CTL_SLIDER_INT, 1.0f, 120.0f},
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

  unsigned int width;
  unsigned int height;

  LV2UI_Write_Function write;
  LV2UI_Controller controller;

  float port_values[ALO_PORT_COUNT];
  bool needs_redraw;

  int active_control; /* index into kControls */
  HitType active_hit;
} AloUI;

static float clampf(const float v, const float lo, const float hi) {
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

static float round_int_value(const float v) {
  return (float)((v >= 0.0f) ? (int)(v + 0.5f) : (int)(v - 0.5f));
}

static void ui_send_port(AloUI* ui, const uint32_t port_index, float value) {
  if (!ui || !ui->write) {
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

static void ui_redraw(AloUI* ui) {
  if (!ui || !ui->dpy) {
    return;
  }

  XClearWindow(ui->dpy, ui->win);

  const int pad = UI_SI(10);
  int x = pad;
  int y = pad;

  /* Header */
  draw_string(ui, pad, UI_SI(18), "ALO (native UI)");

  y += UI_SI(22);

  /* Trigger buttons row */
  const int btn_w = UI_SI(96);
  const int btn_h = UI_SI(32);
  const int btn_gap = UI_SI(10);

  for (int i = 0; i < (int)(sizeof(kControls) / sizeof(kControls[0])); ++i) {
    const Control* c = &kControls[i];
    if (c->type != CTL_TOGGLE && c->type != CTL_TRIGGER) {
      continue;
    }

    const int bx = x;
    const int by = y;

    XDrawRectangle(ui->dpy, ui->win, ui->gc, bx, by, btn_w, btn_h);

    const float state_v = ui->port_values[c->display_port_index];
    const float in_v = ui->port_values[c->port_index];
    const bool on = (state_v >= 0.5f) || (in_v >= 0.5f);

    if (on) {
      XFillRectangle(ui->dpy, ui->win, ui->gc, bx + 1, by + 1, btn_w - 1, btn_h - 1);
      XSetForeground(ui->dpy, ui->gc, WhitePixel(ui->dpy, ui->screen));
      draw_string(ui, bx + UI_SI(10), by + UI_SI(20), c->label);
      XSetForeground(ui->dpy, ui->gc, BlackPixel(ui->dpy, ui->screen));
    } else {
      draw_string(ui, bx + UI_SI(10), by + UI_SI(20), c->label);
    }

    x += btn_w + btn_gap;
  }

  x = pad;
  y += btn_h + UI_SI(16);

  /* Sliders */
  const int slider_w = (int)ui->width - 2 * pad;
  const int slider_h = UI_SI(18);
  const int row_h = UI_SI(40);

  int slider_index = 0;
  for (int i = 0; i < (int)(sizeof(kControls) / sizeof(kControls[0])); ++i) {
    const Control* c = &kControls[i];
    if (c->type != CTL_SLIDER_INT) {
      continue;
    }

    const int sy = y + slider_index * row_h;

    char label[128];
    const float v = ui->port_values[c->port_index];
    snprintf(label, sizeof(label), "%s: %.0f", c->label, v);
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

  XFlush(ui->dpy);
  ui->needs_redraw = false;
}

static bool point_in_rect(const int px, const int py, const int x, const int y, const int w,
                          const int h) {
  return px >= x && px < (x + w) && py >= y && py < (y + h);
}

static int hit_test(AloUI* ui, const int px, const int py, HitType* out_type) {
  const int pad = UI_SI(10);
  int x = pad;
  int y = pad + UI_SI(22);

  const int btn_w = UI_SI(96);
  const int btn_h = UI_SI(32);
  const int btn_gap = UI_SI(10);

  /* Toggles */
  for (int i = 0; i < (int)(sizeof(kControls) / sizeof(kControls[0])); ++i) {
    const Control* c = &kControls[i];
    if (c->type != CTL_TOGGLE && c->type != CTL_TRIGGER) {
      continue;
    }

    if (point_in_rect(px, py, x, y, btn_w, btn_h)) {
      *out_type = HIT_BUTTON;
      return i;
    }

    x += btn_w + btn_gap;
  }

  /* Sliders */
  x = pad;
  y = pad + UI_SI(22) + btn_h + UI_SI(16);

  const int slider_w = (int)ui->width - 2 * pad;
  const int slider_h = UI_SI(18);
  const int row_h = UI_SI(40);

  int slider_index = 0;
  for (int i = 0; i < (int)(sizeof(kControls) / sizeof(kControls[0])); ++i) {
    const Control* c = &kControls[i];
    if (c->type != CTL_SLIDER_INT) {
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
  const int pad = UI_SI(10);
  const int slider_w = (int)ui->width - 2 * pad;

  const float t = clampf(((float)(px - pad) / (float)slider_w), 0.0f, 1.0f);
  float v = c->min + t * (c->max - c->min);
  v = round_int_value(v);
  v = clampf(v, c->min, c->max);

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

static int ui_idle(LV2UI_Handle handle) {
  AloUI* ui = (AloUI*)handle;
  if (!ui || !ui->dpy) {
    return 0;
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
  ui->port_values[port_index] = v;

  // If the DSP reports a loop state change, keep the corresponding loop input
  // parameter in sync. This matters for auto-stop: the DSP can turn the loop
  // off, but it cannot write to the *input* control port, so we do it here.
  if (port_index == ALO_LOOP1_STATE || port_index == ALO_LOOP2_STATE || port_index == ALO_LOOP3_STATE) {
    uint32_t in_port = ALO_LOOP1;
    if (port_index == ALO_LOOP2_STATE) {
      in_port = ALO_LOOP2;
    } else if (port_index == ALO_LOOP3_STATE) {
      in_port = ALO_LOOP3;
    }

    // When DSP turns state off (auto-stop, undo), ensure the host parameter is also set to 0.
    if (v < 0.5f) {
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
  ui->height = UI_SUI(380);
  ui->active_control = -1;
  ui->active_hit = HIT_NONE;

  ui->dpy = XOpenDisplay(NULL);
  if (!ui->dpy) {
    free(ui);
    return NULL;
  }

  ui->screen = DefaultScreen(ui->dpy);

  ui->win = XCreateSimpleWindow(ui->dpy, ui->parent, 0, 0, ui->width, ui->height, 0,
                                BlackPixel(ui->dpy, ui->screen),
                                WhitePixel(ui->dpy, ui->screen));

  XSelectInput(ui->dpy, ui->win,
               ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                   StructureNotifyMask);

  ui->gc = XCreateGC(ui->dpy, ui->win, 0, NULL);
  XSetForeground(ui->dpy, ui->gc, BlackPixel(ui->dpy, ui->screen));
  XSetLineAttributes(ui->dpy, ui->gc, (unsigned int)UI_SI(1), LineSolid, CapButt, JoinMiter);

  XMapWindow(ui->dpy, ui->win);
  XFlush(ui->dpy);

  *widget = (LV2UI_Widget)(uintptr_t)ui->win;

  /* Sensible defaults in case host doesn't send initial values */
  ui->port_values[ALO_BARS] = 2.0f;
  ui->port_values[ALO_CLICK] = 1.0f;
  ui->port_values[ALO_THRESHOLD] = -40.0f;
  ui->port_values[ALO_MIX] = 50.0f;
  ui->port_values[ALO_INSTANT_LOOPS] = 0.0f;
  ui->port_values[ALO_RESET_MODE] = 3.0f;
  ui->port_values[ALO_MIDI_BASE] = 60.0f;

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
