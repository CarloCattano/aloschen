/**
 * @file alo_bridge.h
 * @brief Format-neutral host bridge around the unchanged Aloschen LV2 engine.
 *
 * Purpose: let a non-LV2 wrapper (the DPF VST3 adapter) drive the exact LV2
 * descriptor from source/aloschen.c in-process, without touching the DSP.
 *
 * I/O: control-port values in, audio + MIDI + host transport per block in;
 * audio and control-port outputs (plus adapter-side peak meters) out.
 *
 * Invariants:
 *  - alo_bridge_new / alo_bridge_free run on a non-realtime thread; they own
 *    every allocation (engine buffers, atom buffers, input scratch).
 *  - alo_bridge_run, alo_bridge_set_control and alo_bridge_get_value are
 *    realtime safe: no allocation, locks, I/O or unbounded work.
 *  - Host transport is converted to one LV2 time:Position object per block,
 *    using the atom types the engine parses (source/transport.c).
 *  - n_frames per run must be <= max_block given at creation; larger blocks
 *    are split internally.
 *
 * Extension points: ALO_BRIDGE_PARAMS (alo_bridge.c) is the single parameter
 * table; adding a port means adding one row (tests/vst3_bridge_test.c checks
 * every row against aloschen.lv2/aloschen.ttl).
 */
#ifndef ALO_BRIDGE_H
#define ALO_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Port indices >= this are adapter-only outputs (not LV2 ports). */
#define ALO_BRIDGE_EXTRA_BASE 100u
enum
{
  ALO_BRIDGE_IN_PEAK_L = ALO_BRIDGE_EXTRA_BASE,
  ALO_BRIDGE_IN_PEAK_R,
  ALO_BRIDGE_OUT_PEAK_L,
  ALO_BRIDGE_OUT_PEAK_R,
  ALO_BRIDGE_HOST_BPM,
  ALO_BRIDGE_HOST_PLAYING,
  ALO_BRIDGE_EXTRA_END
};

typedef enum
{
  ALO_PARAM_FLOAT   = 0,
  ALO_PARAM_INTEGER = 1,
  ALO_PARAM_TOGGLE  = 2,
} AloParamKind;

typedef struct
{
  uint32_t     port;   /**< LV2 port index, or ALO_BRIDGE_* extra id */
  const char*  symbol; /**< LV2 symbol (stable id for state/automation) */
  const char*  name;
  float        min, max, def;
  AloParamKind kind;
  bool         output;
} AloParamInfo;

typedef struct
{
  bool   valid; /**< host supplied musical position */
  bool   playing;
  double bpm;
  double beats_per_bar;
  double beat; /**< absolute position in beats since song start */
} AloTransport;

typedef struct
{
  uint32_t frame;
  uint8_t  size; /**< 1..3 */
  uint8_t  data[3];
} AloMidiEvent;

typedef struct AloBridge AloBridge;

uint32_t            alo_bridge_param_count(void);
const AloParamInfo* alo_bridge_param(uint32_t index);

/** NULL on allocation/instantiate failure. */
AloBridge* alo_bridge_new(double sample_rate, uint32_t max_block);
void       alo_bridge_free(AloBridge* bridge);
/** Resets engine state (LV2 activate). Non-RT. */
void alo_bridge_activate(AloBridge* bridge);

void  alo_bridge_set_param(AloBridge* bridge, uint32_t index, float value);
float alo_bridge_get_param(const AloBridge* bridge, uint32_t index);

/** MIDI events beyond the internal capacity are dropped and counted. */
void alo_bridge_run(AloBridge* bridge, const AloTransport* transport, const float* in_l,
                    const float* in_r, float* out_l, float* out_r, uint32_t n_frames,
                    const AloMidiEvent* midi, uint32_t n_midi);

uint32_t alo_bridge_dropped_midi(const AloBridge* bridge);

#ifdef __cplusplus
}
#endif
#endif /* ALO_BRIDGE_H */
