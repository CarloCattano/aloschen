/*
 * Aloschen VST3 editor (DPF + NanoVG). Draws three loop slots as animated
 * progress rings coloured by engine state, a host-bar playhead, a Bars-cycle
 * step strip, peak meters and one knob per engine control.
 *
 * Contract: reads engine outputs only through DPF parameterChanged() (output
 * parameters); writes inputs only via setParameterValue() wrapped in
 * editParameter(). Animation is UI-thread only (uiIdle -> repaint); the ring
 * phase is predicted from host tempo between DSP updates and corrected on each
 * update so motion stays smooth at low parameter-update rates.
 *
 * Button semantics follow the engine (source/loop_engine.c): loop ports are
 * flipped per click (edge = press); undo ports need a rising edge, so the UI
 * raises them and lowers them again after kUndoPulseMs.
 */

#include "DistrhoUI.hpp"
#include "alo_bridge.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

START_NAMESPACE_DISTRHO

namespace {

constexpr int    kSlots       = 3;
constexpr double kUndoPulseMs = 80.0;
constexpr float  kTau         = 6.28318530718f;

struct Rect
{
  float x, y, w, h;
  bool  contains(double px, double py) const
  {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

struct KnobSpec
{
  const char* symbol;
  const char* label;
};

/* Every engine input that is not a slot control gets a knob or toggle. */
constexpr KnobSpec kKnobs[] = {
    {"bars", "BARS"},
    {"click", "CLICK"},
    {"mix", "MIX"},
    {"sampler_vol", "SMP VOL"},
    {"slices_per_bar", "SLICES"},
    {"slice_root", "ROOT"},
    {"transient_threshold", "THRESH"},
    {"slice_env_frac", "DECAY"},
    {"slice_sens", "SENS"},
    {"slice_env_attack", "ATTACK"},
    {"transient_debounce", "DEBNCE"},
    {"transient_burst", "BURST"},
    {"transient_end_ratio", "END"},
    {"transient_pre_ms", "PRE MS"},
};
constexpr int kNumKnobs = sizeof(kKnobs) / sizeof(kKnobs[0]);

uint32_t paramIndex(const char* symbol)
{
  for (uint32_t i = 0; i < alo_bridge_param_count(); ++i) {
    if (std::strcmp(alo_bridge_param(i)->symbol, symbol) == 0) {
      return i;
    }
  }
  return UINT32_MAX;
}

double nowMs()
{
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

class AloschenUI : public UI
{
public:
  AloschenUI() : UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT)
  {
    loadSharedResources();
    const uint32_t n = alo_bridge_param_count();
    for (uint32_t i = 0; i < n && i < kMaxParams; ++i) {
      values[i] = alo_bridge_param(i)->def;
    }
    for (int s = 0; s < kSlots; ++s) {
      char sym[32];
      std::snprintf(sym, sizeof sym, "loop%d", s + 1);
      slot[s].loop = paramIndex(sym);
      std::snprintf(sym, sizeof sym, "Undo%d", s + 1);
      slot[s].undo = paramIndex(sym);
      std::snprintf(sym, sizeof sym, "loop%d_vol", s + 1);
      slot[s].vol = paramIndex(sym);
      std::snprintf(sym, sizeof sym, "loop%d_state", s + 1);
      slot[s].state = paramIndex(sym);
      std::snprintf(sym, sizeof sym, "undo%d_state", s + 1);
      slot[s].undoState = paramIndex(sym);
      std::snprintf(sym, sizeof sym, "loop%d_has_audio", s + 1);
      slot[s].hasAudio = paramIndex(sym);
    }
    for (int k = 0; k < kNumKnobs; ++k) {
      knobParam[k] = paramIndex(kKnobs[k].symbol);
    }
    pCycle     = paramIndex("cycle_phase");
    pBarPhase  = paramIndex("host_bar_phase");
    pBarStep   = paramIndex("bar_step");
    pBars      = paramIndex("bars");
    pBpm       = paramIndex("host_bpm");
    pPlaying   = paramIndex("host_playing");
    pEnabled   = paramIndex("ENABLED");
    pSplit     = paramIndex("split_by_transient");
    pDetected  = paramIndex("detected_slices");
    pMeters[0] = paramIndex("in_peak_l");
    pMeters[1] = paramIndex("in_peak_r");
    pMeters[2] = paramIndex("out_peak_l");
    pMeters[3] = paramIndex("out_peak_r");
    lastIdle   = nowMs();
  }

protected:
  void parameterChanged(uint32_t index, float value) override
  {
    if (index >= kMaxParams) {
      return;
    }
    values[index] = value;
    if (index == pCycle) {
      cycleMeasured = value;
    } else if (index == pBarPhase) {
      barMeasured = value;
    }
  }

  void uiIdle() override
  {
    const double t  = nowMs();
    const double dt = (t - lastIdle) * 0.001;
    lastIdle        = t;
    animTime += dt;

    /* Predict ring and bar phases from tempo; pull toward DSP truth. */
    const float bpm     = value(pBpm) > 1.0f ? value(pBpm) : 120.0f;
    const float bars    = value(pBars) >= 1.0f ? value(pBars) : 1.0f;
    const bool  playing = value(pPlaying) > 0.5f;
    if (playing) {
      cycleShown = wrap01(cycleShown + (float)(dt * bpm / 60.0 / (4.0 * bars)));
      barShown   = wrap01(barShown + (float)(dt * bpm / 60.0 / 4.0));
    }
    cycleShown = approachWrapped(cycleShown, cycleMeasured, 0.35f);
    barShown   = approachWrapped(barShown, barMeasured, 0.35f);

    for (int m = 0; m < 4; ++m) {
      const float target = value(pMeters[m]);
      meterShown[m]      = target > meterShown[m] ? target : meterShown[m] * 0.88f;
      if (meterShown[m] >= meterHold[m]) {
        meterHold[m] = meterShown[m];
        holdTime[m]  = t;
      } else if (t - holdTime[m] > 900.0) {
        meterHold[m] *= 0.95f;
      }
    }

    for (int s = 0; s < kSlots; ++s) {
      if (undoReleaseAt[s] > 0.0 && t >= undoReleaseAt[s]) {
        writeParam(slot[s].undo, 0.0f);
        undoReleaseAt[s] = 0.0;
      }
      const float target = stateIntensity(s);
      slotGlow[s] += (target - slotGlow[s]) * 0.2f;
    }
    repaint();
  }

  void onNanoDisplay() override
  {
    const float W = (float)getWidth(), H = (float)getHeight();
    drawBackground(W, H);
    drawHeader(W);
    for (int s = 0; s < kSlots; ++s) {
      drawSlot(s);
    }
    drawStepStrip();
    for (int k = 0; k < kNumKnobs; ++k) {
      drawKnob(k);
    }
  }

  bool onMouse(const MouseEvent& ev) override
  {
    if (ev.button != 1 && ev.button != 3) {
      return false;
    }
    const double x = ev.pos.getX(), y = ev.pos.getY();
    if (!ev.press) {
      if (dragParam != UINT32_MAX) {
        editParameter(dragParam, false);
        dragParam = UINT32_MAX;
      }
      return true;
    }
    for (int s = 0; s < kSlots; ++s) {
      if (ringRect(s).contains(x, y)) {
        writeParam(slot[s].loop, value(slot[s].loop) > 0.5f ? 0.0f : 1.0f);
        return true;
      }
      if (undoRect(s).contains(x, y)) {
        writeParam(slot[s].undo, 1.0f);
        undoReleaseAt[s] = nowMs() + kUndoPulseMs;
        return true;
      }
      if (volRect(s).contains(x, y)) {
        beginDrag(slot[s].vol, y, ev.button == 3);
        return true;
      }
    }
    if (enabledRect().contains(x, y)) {
      writeParam(pEnabled, value(pEnabled) > 0.5f ? 0.0f : 1.0f);
      return true;
    }
    if (splitRect().contains(x, y)) {
      writeParam(pSplit, value(pSplit) > 0.5f ? 0.0f : 1.0f);
      return true;
    }
    for (int k = 0; k < kNumKnobs; ++k) {
      if (knobRect(k).contains(x, y)) {
        beginDrag(knobParam[k], y, ev.button == 3);
        return true;
      }
    }
    return false;
  }

  bool onMotion(const MotionEvent& ev) override
  {
    if (dragParam == UINT32_MAX) {
      return false;
    }
    const AloParamInfo* p     = alo_bridge_param(dragParam);
    const float         range = p->max - p->min;
    const float         delta = (float)(dragStartY - ev.pos.getY()) / 180.0f * range;
    float               v     = dragStartValue + delta;
    v                         = v < p->min ? p->min : (v > p->max ? p->max : v);
    if (p->kind != ALO_PARAM_FLOAT) {
      v = std::round(v);
    }
    values[dragParam] = v;
    setParameterValue(dragParam, v);
    repaint();
    return true;
  }

  bool onScroll(const ScrollEvent& ev) override
  {
    const double x = ev.pos.getX(), y = ev.pos.getY();
    for (int k = 0; k < kNumKnobs; ++k) {
      if (knobRect(k).contains(x, y)) {
        const AloParamInfo* p    = alo_bridge_param(knobParam[k]);
        const float         step = p->kind == ALO_PARAM_FLOAT ? (p->max - p->min) / 100.0f : 1.0f;
        float               v    = value(knobParam[k]) + (float)ev.delta.getY() * step;
        v                        = v < p->min ? p->min : (v > p->max ? p->max : v);
        writeParam(knobParam[k], v);
        return true;
      }
    }
    return false;
  }

private:
  static constexpr uint32_t kMaxParams = 64;

  struct SlotParams
  {
    uint32_t loop, undo, vol, state, undoState, hasAudio;
  };

  float value(uint32_t index) const
  {
    return index < kMaxParams ? values[index] : 0.0f;
  }

  void writeParam(uint32_t index, float v)
  {
    if (index >= kMaxParams) {
      return;
    }
    values[index] = v;
    editParameter(index, true);
    setParameterValue(index, v);
    editParameter(index, false);
    repaint();
  }

  /* Right-click resets to the TTL default; left-click starts a vertical drag. */
  void beginDrag(uint32_t index, double y, bool reset)
  {
    if (reset) {
      writeParam(index, alo_bridge_param(index)->def);
      return;
    }
    dragParam      = index;
    dragStartY     = y;
    dragStartValue = value(index);
    editParameter(index, true);
  }

  static float wrap01(float v)
  {
    return v - std::floor(v);
  }

  static float approachWrapped(float shown, float target, float k)
  {
    float d = target - shown;
    if (d > 0.5f) {
      d -= 1.0f;
    } else if (d < -0.5f) {
      d += 1.0f;
    }
    return wrap01(shown + d * k);
  }

  /* Engine loop_state encoding (source/loop_state.c): 0 idle/stopped,
   * 0.25 armed or queued, 0.5 playing, 1 recording. */
  enum class SlotMode
  {
    Empty,
    Stopped,
    Armed,
    Playing,
    Recording
  };

  SlotMode slotMode(int s) const
  {
    const float st = value(slot[s].state);
    if (st > 0.75f) {
      return SlotMode::Recording;
    }
    if (st > 0.4f) {
      return SlotMode::Playing;
    }
    if (st > 0.1f) {
      return SlotMode::Armed;
    }
    return value(slot[s].hasAudio) > 0.5f ? SlotMode::Stopped : SlotMode::Empty;
  }

  float stateIntensity(int s) const
  {
    switch (slotMode(s)) {
    case SlotMode::Recording:
      return 1.0f;
    case SlotMode::Playing:
      return 0.7f;
    case SlotMode::Armed:
      return 0.5f;
    case SlotMode::Stopped:
      return 0.25f;
    default:
      return 0.0f;
    }
  }

  Color modeColor(SlotMode m, float alpha = 1.0f) const
  {
    switch (m) {
    case SlotMode::Recording:
      return Color(255, 59, 92, alpha);
    case SlotMode::Playing:
      return Color(0, 230, 180, alpha);
    case SlotMode::Armed:
      return Color(255, 184, 0, alpha);
    case SlotMode::Stopped:
      return Color(90, 140, 255, alpha);
    default:
      return Color(90, 100, 120, alpha);
    }
  }

  static const char* modeLabel(SlotMode m)
  {
    switch (m) {
    case SlotMode::Recording:
      return "REC";
    case SlotMode::Playing:
      return "PLAY";
    case SlotMode::Armed:
      return "ARMED";
    case SlotMode::Stopped:
      return "STOPPED";
    default:
      return "EMPTY";
    }
  }

  /* Layout */
  static constexpr float kSlotW = 300.0f, kSlotX0 = 30.0f, kSlotY = 86.0f, kRingR = 78.0f;

  float slotCx(int s) const
  {
    return kSlotX0 + kSlotW * (float)s + kSlotW * 0.5f;
  }
  float ringCy() const
  {
    return kSlotY + 112.0f;
  }
  Rect ringRect(int s) const
  {
    return {slotCx(s) - kRingR - 10, ringCy() - kRingR - 10, 2 * kRingR + 20, 2 * kRingR + 20};
  }
  Rect undoRect(int s) const
  {
    return {slotCx(s) - 120, kSlotY + 220, 70, 30};
  }
  Rect volRect(int s) const
  {
    return {slotCx(s) - 40, kSlotY + 220, 160, 30};
  }
  Rect enabledRect() const
  {
    return {730, 360, 100, 30};
  }
  Rect splitRect() const
  {
    return {840, 360, 100, 30};
  }
  Rect knobRect(int k) const
  {
    return {20.0f + 66.0f * (float)k, 420, 60, 120};
  }

  void drawBackground(float W, float H)
  {
    beginPath();
    rect(0, 0, W, H);
    fillPaint(linearGradient(0, 0, 0, H, Color(10, 12, 20), Color(20, 24, 38)));
    fill();
    /* subtle animated scanlines */
    strokeWidth(1.0f);
    strokeColor(Color(255, 255, 255, 0.025f));
    const float off = std::fmod((float)animTime * 12.0f, 24.0f);
    for (float y = off; y < H; y += 24.0f) {
      beginPath();
      moveTo(0, y);
      lineTo(W, y);
      stroke();
    }
  }

  void text(float x, float y, float size, Color c, const char* s, int align)
  {
    fontSize(size);
    fillColor(c);
    textAlign(align);
    NanoVG::text(x, y, s, nullptr);
  }

  void drawHeader(float W)
  {
    text(30, 40, 30, Color(0, 230, 255), "ALOSCHEN", ALIGN_LEFT | ALIGN_MIDDLE);
    text(30, 64, 12, Color(140, 150, 180), "transport-synced looper", ALIGN_LEFT | ALIGN_MIDDLE);

    const bool  playing = value(pPlaying) > 0.5f;
    const float pulse   = playing ? 0.6f + 0.4f * std::sin((float)animTime * 6.0f) : 0.3f;
    beginPath();
    circle(260, 40, 7);
    fillColor(playing ? Color(0, 230, 120, pulse) : Color(200, 60, 60, 0.8f));
    fill();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s  %.1f BPM", playing ? "PLAY" : "STOP", value(pBpm));
    text(275, 40, 15, Color(220, 225, 240), buf, ALIGN_LEFT | ALIGN_MIDDLE);

    /* host bar playhead */
    const float bx = 430, bw = 320, by = 32, bh = 16;
    beginPath();
    roundedRect(bx, by, bw, bh, 4);
    fillColor(Color(30, 36, 54));
    fill();
    for (int b = 1; b < 4; ++b) {
      beginPath();
      moveTo(bx + bw * (float)b / 4.0f, by);
      lineTo(bx + bw * (float)b / 4.0f, by + bh);
      strokeColor(Color(80, 90, 120, 0.8f));
      stroke();
    }
    const float px = bx + bw * barShown;
    beginPath();
    roundedRect(bx, by, px - bx, bh, 4);
    fillPaint(linearGradient(bx, 0, px, 0, Color(0, 120, 255, 0.2f), Color(0, 230, 255, 0.8f)));
    fill();
    beginPath();
    rect(px - 1.5f, by - 4, 3, bh + 8);
    fillColor(Color(255, 255, 255));
    fill();

    /* meters: IN L/R, OUT L/R */
    static const char* labels[4] = {"IN", "", "OUT", ""};
    for (int m = 0; m < 4; ++m) {
      const float mx = W - 150 + (float)m * 22.0f + (m >= 2 ? 14.0f : 0.0f), my = 12, mh = 56;
      beginPath();
      rect(mx, my, 14, mh);
      fillColor(Color(30, 36, 54));
      fill();
      const float lvl = meterNorm(meterShown[m]);
      beginPath();
      rect(mx, my + mh * (1.0f - lvl), 14, mh * lvl);
      fillPaint(linearGradient(0, my + mh, 0, my, Color(0, 230, 120), Color(255, 60, 60)));
      fill();
      const float hold = meterNorm(meterHold[m]);
      beginPath();
      rect(mx, my + mh * (1.0f - hold) - 1, 14, 2);
      fillColor(Color(255, 255, 255, 0.9f));
      fill();
      if (labels[m][0]) {
        text(mx + 18, my + mh + 12, 10, Color(140, 150, 180), labels[m],
             ALIGN_CENTER | ALIGN_MIDDLE);
      }
    }
  }

  /* -60..0 dBFS mapped to 0..1 */
  static float meterNorm(float peak)
  {
    if (peak <= 1e-6f) {
      return 0.0f;
    }
    const float db = 20.0f * std::log10(peak);
    const float n  = (db + 60.0f) / 60.0f;
    return n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
  }

  void drawSlot(int s)
  {
    const SlotMode m  = slotMode(s);
    const float    cx = slotCx(s), cy = ringCy();
    const float    glow = slotGlow[s];

    beginPath();
    roundedRect(kSlotX0 + kSlotW * (float)s + 8, kSlotY, kSlotW - 16, 270, 14);
    fillColor(Color(22, 27, 42, 0.95f));
    fill();
    strokeWidth(1.5f);
    strokeColor(modeColor(m, 0.25f + 0.5f * glow));
    stroke();

    /* glow halo */
    float alpha = 0.35f * glow;
    if (m == SlotMode::Armed) {
      alpha *= 0.5f + 0.5f * std::sin((float)animTime * 9.0f);
    }
    beginPath();
    circle(cx, cy, kRingR + 22);
    fillPaint(
        radialGradient(cx, cy, kRingR - 10, kRingR + 22, modeColor(m, alpha), modeColor(m, 0.0f)));
    fill();

    /* base ring */
    beginPath();
    arc(cx, cy, kRingR, 0, kTau, NanoVG::CW);
    strokeWidth(10.0f);
    strokeColor(Color(40, 46, 66));
    stroke();

    /* progress arc */
    const float start = -kTau / 4.0f;
    const float prog  = (m == SlotMode::Empty) ? 0.0f : cycleShown;
    if (prog > 0.001f) {
      beginPath();
      arc(cx, cy, kRingR, start, start + kTau * prog, NanoVG::CW);
      strokeWidth(10.0f);
      lineCap(ROUND);
      strokeColor(modeColor(m));
      stroke();
      const float a = start + kTau * prog;
      beginPath();
      circle(cx + kRingR * std::cos(a), cy + kRingR * std::sin(a), 7.0f);
      fillColor(Color(255, 255, 255));
      fill();
    }

    char buf[16];
    std::snprintf(buf, sizeof buf, "%d", s + 1);
    text(cx, cy - 14, 40, Color(230, 235, 250), buf, ALIGN_CENTER | ALIGN_MIDDLE);
    text(cx, cy + 24, 14, modeColor(m), modeLabel(m), ALIGN_CENTER | ALIGN_MIDDLE);

    /* undo pill */
    const Rect  u      = undoRect(s);
    const bool  queued = value(slot[s].undoState) > 0.1f;
    const float blink  = queued ? 0.5f + 0.5f * std::sin((float)animTime * 10.0f) : 0.0f;
    beginPath();
    roundedRect(u.x, u.y, u.w, u.h, 15);
    fillColor(queued ? Color(170, 80, 255, 0.4f + 0.5f * blink) : Color(40, 46, 66));
    fill();
    text(u.x + u.w / 2, u.y + u.h / 2, 13, Color(230, 230, 250), "UNDO",
         ALIGN_CENTER | ALIGN_MIDDLE);

    /* volume slider */
    const Rect  v   = volRect(s);
    const float vol = value(slot[s].vol);
    beginPath();
    roundedRect(v.x, v.y + 11, v.w, 8, 4);
    fillColor(Color(40, 46, 66));
    fill();
    beginPath();
    roundedRect(v.x, v.y + 11, v.w * vol, 8, 4);
    fillColor(modeColor(m == SlotMode::Empty ? SlotMode::Stopped : m, 0.9f));
    fill();
    beginPath();
    circle(v.x + v.w * vol, v.y + 15, 8);
    fillColor(Color(240, 240, 250));
    fill();
  }

  void drawStepStrip()
  {
    const int   bars  = (int)(value(pBars) >= 1.0f ? value(pBars) : 1.0f);
    const int   steps = bars * 4;
    const int   cur   = (int)value(pBarStep);
    const float x0 = 30, y0 = 366, w = 680, h = 18;
    const float cw = w / (float)steps;
    for (int i = 0; i < steps; ++i) {
      beginPath();
      roundedRect(x0 + cw * (float)i + 1, y0, cw - 2, h, 3);
      const bool down = (i % 4) == 0;
      if (i == cur) {
        fillColor(Color(0, 230, 255, 0.95f));
      } else {
        fillColor(down ? Color(60, 70, 100) : Color(36, 42, 62));
      }
      fill();
    }
    text(x0, y0 - 10, 11, Color(140, 150, 180), "CYCLE", ALIGN_LEFT | ALIGN_MIDDLE);

    drawToggle(enabledRect(), "ENABLED", value(pEnabled) > 0.5f);
    drawToggle(splitRect(), "TRANSIENT", value(pSplit) > 0.5f);
    char buf[32];
    std::snprintf(buf, sizeof buf, "SLICES %d", (int)value(pDetected));
    text(940, 406, 11, Color(140, 150, 180), buf, ALIGN_RIGHT | ALIGN_MIDDLE);
  }

  void drawToggle(const Rect& r, const char* label, bool on)
  {
    beginPath();
    roundedRect(r.x, r.y, r.w, r.h, 15);
    fillColor(on ? Color(0, 200, 160, 0.85f) : Color(40, 46, 66));
    fill();
    text(r.x + r.w / 2, r.y + r.h / 2, 12, Color(235, 240, 250), label,
         ALIGN_CENTER | ALIGN_MIDDLE);
  }

  void drawKnob(int k)
  {
    const Rect          r    = knobRect(k);
    const uint32_t      idx  = knobParam[k];
    const AloParamInfo* p    = alo_bridge_param(idx);
    const float         norm = (value(idx) - p->min) / (p->max - p->min);
    const float         cx = r.x + r.w / 2, cy = r.y + 34, rad = 22;
    const float         a0 = kTau * 0.375f, a1 = a0 + kTau * 0.75f;

    beginPath();
    arc(cx, cy, rad, a0, a1, NanoVG::CW);
    strokeWidth(5.0f);
    lineCap(ROUND);
    strokeColor(Color(40, 46, 66));
    stroke();
    if (norm > 0.001f) {
      beginPath();
      arc(cx, cy, rad, a0, a0 + (a1 - a0) * norm, NanoVG::CW);
      strokeColor(dragParam == idx ? Color(255, 255, 255) : Color(0, 200, 255));
      stroke();
    }
    beginPath();
    circle(cx, cy, rad - 8);
    fillColor(Color(28, 34, 52));
    fill();
    const float a = a0 + (a1 - a0) * norm;
    beginPath();
    moveTo(cx + (rad - 14) * std::cos(a), cy + (rad - 14) * std::sin(a));
    lineTo(cx + (rad - 6) * std::cos(a), cy + (rad - 6) * std::sin(a));
    strokeWidth(2.5f);
    strokeColor(Color(230, 235, 250));
    stroke();

    char buf[32];
    if (p->kind == ALO_PARAM_FLOAT) {
      std::snprintf(buf, sizeof buf, "%.2f", value(idx));
    } else {
      std::snprintf(buf, sizeof buf, "%d", (int)value(idx));
    }
    text(cx, cy + 40, 12, Color(220, 225, 240), buf, ALIGN_CENTER | ALIGN_MIDDLE);
    text(cx, cy + 58, 10, Color(140, 150, 180), kKnobs[k].label, ALIGN_CENTER | ALIGN_MIDDLE);
  }

  float      values[kMaxParams]   = {};
  SlotParams slot[kSlots]         = {};
  uint32_t   knobParam[kNumKnobs] = {};
  uint32_t   pCycle = 0, pBarPhase = 0, pBarStep = 0, pBars = 0, pBpm = 0, pPlaying = 0;
  uint32_t   pEnabled = 0, pSplit = 0, pDetected = 0;
  uint32_t   pMeters[4] = {};

  float  cycleShown = 0, cycleMeasured = 0, barShown = 0, barMeasured = 0;
  float  meterShown[4] = {}, meterHold[4] = {};
  double holdTime[4]           = {};
  float  slotGlow[kSlots]      = {};
  double undoReleaseAt[kSlots] = {};
  double lastIdle = 0, animTime = 0;

  uint32_t dragParam      = UINT32_MAX;
  double   dragStartY     = 0;
  float    dragStartValue = 0;

  DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AloschenUI)
};

UI* createUI()
{
  return new AloschenUI();
}

END_NAMESPACE_DISTRHO
