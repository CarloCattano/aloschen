/*
 * DPF adapter: exposes every Aloschen LV2 control port as a plugin parameter
 * and maps host transport (bar/beat/tick, tempo, playing) onto the engine's
 * time:Position input via alo_bridge. No DSP lives here.
 *
 * Threads: constructor/destructor/activate on the host main thread;
 * run() on the audio thread (bridge contract: no alloc/locks/I/O).
 * State loss (honest): loop audio is not persisted, same as the LV2 build
 * (no LV2 state extension); parameter values are saved by the host.
 */

#include "DistrhoPlugin.hpp"
#include "alo_bridge.h"

START_NAMESPACE_DISTRHO

static constexpr uint32_t kMaxMidiEvents = 512;

class AloschenPlugin : public Plugin
{
public:
  AloschenPlugin() : Plugin(alo_bridge_param_count(), 0, 0)
  {
    bridge = alo_bridge_new(getSampleRate(), getBufferSize());
  }

  ~AloschenPlugin() override
  {
    alo_bridge_free(bridge);
  }

protected:
  const char* getLabel() const override
  {
    return "Aloschen";
  }
  const char* getDescription() const override
  {
    return "Transport-synced 3-slot looper with quantized undo and slice sampler";
  }
  const char* getMaker() const override
  {
    return "Ktano Studio";
  }
  const char* getHomePage() const override
  {
    return "http://ktano-studio.com/aloschen";
  }
  const char* getLicense() const override
  {
    return "ISC";
  }
  uint32_t getVersion() const override
  {
    return d_version(0, 1, 0);
  }

  void initParameter(uint32_t index, Parameter& parameter) override
  {
    const AloParamInfo* info = alo_bridge_param(index);
    parameter.symbol         = info->symbol;
    parameter.name           = info->name;
    parameter.ranges.min     = info->min;
    parameter.ranges.max     = info->max;
    parameter.ranges.def     = info->def;
    parameter.hints          = info->output ? kParameterIsOutput : kParameterIsAutomatable;
    if (info->kind == ALO_PARAM_INTEGER) {
      parameter.hints |= kParameterIsInteger;
    } else if (info->kind == ALO_PARAM_TOGGLE) {
      parameter.hints |= kParameterIsBoolean | kParameterIsInteger;
    }
  }

  float getParameterValue(uint32_t index) const override
  {
    return alo_bridge_get_param(bridge, index);
  }

  void setParameterValue(uint32_t index, float value) override
  {
    alo_bridge_set_param(bridge, index, value);
  }

  void activate() override
  {
    alo_bridge_activate(bridge);
  }

  /* Buffer-size changes need no action: the bridge splits larger blocks. */
  void sampleRateChanged(double newSampleRate) override
  {
    recreate(newSampleRate, getBufferSize());
  }

  void run(const float** inputs, float** outputs, uint32_t frames, const MidiEvent* midiEvents,
           uint32_t midiEventCount) override
  {
    if (!bridge) {
      for (uint32_t i = 0; i < frames; ++i) {
        outputs[0][i] = outputs[1][i] = 0.0f;
      }
      return;
    }
    uint32_t nmidi = 0;
    for (uint32_t i = 0; i < midiEventCount && nmidi < kMaxMidiEvents; ++i) {
      const MidiEvent& ev = midiEvents[i];
      if (ev.size == 0 || ev.size > 3) {
        continue;
      }
      AloMidiEvent& out = midi[nmidi++];
      out.frame         = ev.frame;
      out.size          = (uint8_t)ev.size;
      for (uint32_t b = 0; b < 3; ++b) {
        out.data[b] = b < ev.size ? ev.data[b] : 0;
      }
    }

    const AloTransport transport = hostTransport(getTimePosition(), getSampleRate());
    alo_bridge_run(bridge, &transport, inputs[0], inputs[1], outputs[0], outputs[1], frames, midi,
                   nmidi);
  }

private:
  /* DPF bar/beat are 1-based; absolute beats = whole bars + beat + tick fraction. */
  static AloTransport hostTransport(const TimePosition& pos, double rate)
  {
    AloTransport t{};
    t.playing = pos.playing;
    if (pos.bbt.valid) {
      const double tick_frac =
          pos.bbt.ticksPerBeat > 0.0 ? pos.bbt.tick / pos.bbt.ticksPerBeat : 0.0;
      t.valid         = true;
      t.bpm           = pos.bbt.beatsPerMinute;
      t.beats_per_bar = pos.bbt.beatsPerBar;
      t.beat          = (double)(pos.bbt.bar - 1) * (double)pos.bbt.beatsPerBar +
                        (double)(pos.bbt.beat - 1) + tick_frac;
    } else if (pos.playing && rate > 0.0) {
      /* Host runs but sends no musical time (e.g. Carla's VST3 host without
       * JACK): derive beats from the sample position at the 120 BPM, 4/4 default. */
      t.valid         = true;
      t.bpm           = 120.0;
      t.beats_per_bar = 4.0;
      t.beat          = (double)pos.frame * t.bpm / (60.0 * rate);
    } else {
      t.bpm           = 120.0;
      t.beats_per_bar = 4.0;
    }
    return t;
  }

  /* Host guarantees run() is not concurrent with these callbacks (DPF contract). */
  void recreate(double rate, uint32_t block)
  {
    float          saved[64];
    const uint32_t n = alo_bridge_param_count() < 64 ? alo_bridge_param_count() : 64;
    for (uint32_t i = 0; i < n; ++i) {
      saved[i] = alo_bridge_get_param(bridge, i);
    }
    alo_bridge_free(bridge);
    bridge = alo_bridge_new(rate, block);
    for (uint32_t i = 0; i < n; ++i) {
      alo_bridge_set_param(bridge, i, saved[i]);
    }
  }

  AloBridge*   bridge               = nullptr;
  AloMidiEvent midi[kMaxMidiEvents] = {};

  DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AloschenPlugin)
};

Plugin* createPlugin()
{
  return new AloschenPlugin();
}

END_NAMESPACE_DISTRHO
