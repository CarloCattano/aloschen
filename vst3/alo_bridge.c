/* Host bridge: drives the unchanged LV2 engine descriptor. Contract in alo_bridge.h. */

#include "alo_bridge.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/atom/forge.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include "alo_engine.h"

const LV2_Descriptor* lv2_descriptor(uint32_t index);

/* Mirrors aloschen.lv2/aloschen.ttl (checked by tests/vst3_bridge_test.c).
 * Deviation: ENABLED is exposed as a 0/1 toggle; the engine treats >= 0.5 as
 * enabled, the TTL's 0.15..2.5 range is a MOD-host quirk. */
static const AloParamInfo ALO_BRIDGE_PARAMS[] = {
    {ALO_LOOP1, "loop1", "Loop1", 0, 1, 0, ALO_PARAM_TOGGLE, false},
    {ALO_UNDO1, "Undo1", "Undo1", 0, 1, 0, ALO_PARAM_TOGGLE, false},
    {ALO_LOOP2, "loop2", "Loop2", 0, 1, 0, ALO_PARAM_TOGGLE, false},
    {ALO_UNDO2, "Undo2", "Undo2", 0, 1, 0, ALO_PARAM_TOGGLE, false},
    {ALO_LOOP3, "loop3", "Loop3", 0, 1, 0, ALO_PARAM_TOGGLE, false},
    {ALO_UNDO3, "Undo3", "Undo3", 0, 1, 0, ALO_PARAM_TOGGLE, false},
    {ALO_SLICE_ROOT, "slice_root", "Slice Root", 0, 127, 36, ALO_PARAM_INTEGER, false},
    {ALO_CLICK, "click", "Click", 0, 10, 1, ALO_PARAM_INTEGER, false},
    {ALO_BARS, "bars", "Bars", 1, 16, 2, ALO_PARAM_INTEGER, false},
    {ALO_MIX, "mix", "Mix", 0, 100, 50, ALO_PARAM_INTEGER, false},
    {ALO_ENABLED, "ENABLED", "ENABLED", 0, 1, 1, ALO_PARAM_TOGGLE, false},
    {ALO_LOOP1_VOL, "loop1_vol", "Loop1 Volume", 0, 1, 1, ALO_PARAM_FLOAT, false},
    {ALO_LOOP2_VOL, "loop2_vol", "Loop2 Volume", 0, 1, 1, ALO_PARAM_FLOAT, false},
    {ALO_LOOP3_VOL, "loop3_vol", "Loop3 Volume", 0, 1, 1, ALO_PARAM_FLOAT, false},
    {ALO_SAMPLER_VOL, "sampler_vol", "Sampler Vol", 0, 2.5f, 1, ALO_PARAM_FLOAT, false},
    {ALO_SLICES_PER_BAR, "slices_per_bar", "Slices Per Bar", 2, 8, 4, ALO_PARAM_INTEGER, false},
    {ALO_SPLIT_TRANSIENTS, "split_by_transient", "Transients", 0, 1, 0, ALO_PARAM_TOGGLE, false},
    {ALO_TRANSIENT_THRESH, "transient_threshold", "Threshold", 0, 15, 4, ALO_PARAM_FLOAT, false},
    {ALO_SLICE_ENV_FRAC, "slice_env_frac", "Decay", 0, 100, 0, ALO_PARAM_FLOAT, false},
    {ALO_SLICE_ENV_ATTACK, "slice_env_attack", "Attack", 0, 100, 5, ALO_PARAM_FLOAT, false},
    {ALO_SLICE_SENS, "slice_sens", "Sensitivity", 0, 10, 5, ALO_PARAM_FLOAT, false},
    {ALO_TRANSIENT_DEBOUNCE, "transient_debounce", "Transient Debounce", 0, 40, 2, ALO_PARAM_FLOAT,
     false},
    {ALO_TRANSIENT_BURST, "transient_burst", "Transient Burst", 0, 40, 3, ALO_PARAM_FLOAT, false},
    {ALO_TRANSIENT_END, "transient_end_ratio", "Transient End Ratio", 1.001f, 1.5f, 1.02f,
     ALO_PARAM_FLOAT, false},
    {ALO_TRANSIENT_PRE_MS, "transient_pre_ms", "Transient Pre-ms", 0, 20, 2, ALO_PARAM_FLOAT,
     false},
    {ALO_LOOP1_STATE, "loop1_state", "Loop1 State", 0, 1, 0, ALO_PARAM_FLOAT, true},
    {ALO_LOOP2_STATE, "loop2_state", "Loop2 State", 0, 1, 0, ALO_PARAM_FLOAT, true},
    {ALO_LOOP3_STATE, "loop3_state", "Loop3 State", 0, 1, 0, ALO_PARAM_FLOAT, true},
    {ALO_BAR_STEP, "bar_step", "Bar Step", 0, 63, 0, ALO_PARAM_INTEGER, true},
    {ALO_UNDO1_STATE, "undo1_state", "Undo1 State", 0, 1, 0, ALO_PARAM_FLOAT, true},
    {ALO_UNDO2_STATE, "undo2_state", "Undo2 State", 0, 1, 0, ALO_PARAM_FLOAT, true},
    {ALO_UNDO3_STATE, "undo3_state", "Undo3 State", 0, 1, 0, ALO_PARAM_FLOAT, true},
    {ALO_LOOP1_HAS_AUDIO, "loop1_has_audio", "Loop1 Has Audio", 0, 1, 0, ALO_PARAM_INTEGER, true},
    {ALO_LOOP2_HAS_AUDIO, "loop2_has_audio", "Loop2 Has Audio", 0, 1, 0, ALO_PARAM_INTEGER, true},
    {ALO_LOOP3_HAS_AUDIO, "loop3_has_audio", "Loop3 Has Audio", 0, 1, 0, ALO_PARAM_INTEGER, true},
    {ALO_CYCLE_PHASE, "cycle_phase", "Cycle Phase", 0, 1, 0, ALO_PARAM_FLOAT, true},
    {ALO_HOST_BAR_PHASE, "host_bar_phase", "Host Bar Phase", 0, 1, 0, ALO_PARAM_FLOAT, true},
    {ALO_DETECTED_SLICES, "detected_slices", "Slices", 0, 32, 0, ALO_PARAM_INTEGER, true},
    {ALO_BRIDGE_IN_PEAK_L, "in_peak_l", "In Peak L", 0, 1, 0, ALO_PARAM_FLOAT, true},
    {ALO_BRIDGE_IN_PEAK_R, "in_peak_r", "In Peak R", 0, 1, 0, ALO_PARAM_FLOAT, true},
    {ALO_BRIDGE_OUT_PEAK_L, "out_peak_l", "Out Peak L", 0, 1, 0, ALO_PARAM_FLOAT, true},
    {ALO_BRIDGE_OUT_PEAK_R, "out_peak_r", "Out Peak R", 0, 1, 0, ALO_PARAM_FLOAT, true},
    {ALO_BRIDGE_HOST_BPM, "host_bpm", "Host BPM", 0, 999, 120, ALO_PARAM_FLOAT, true},
    {ALO_BRIDGE_HOST_PLAYING, "host_playing", "Host Playing", 0, 1, 0, ALO_PARAM_TOGGLE, true},
};

#define ALO_BRIDGE_NPARAMS (sizeof(ALO_BRIDGE_PARAMS) / sizeof(ALO_BRIDGE_PARAMS[0]))
#define ALO_URID_CAP 64u
#define ALO_URI_LEN 160u
#define ALO_CONTROL_SEQ_BYTES 1024u
#define ALO_MIDI_SEQ_BYTES 8192u

typedef struct
{
  char     uris[ALO_URID_CAP][ALO_URI_LEN];
  uint32_t count;
} AloUridTable;

struct AloBridge
{
  const LV2_Descriptor* desc;
  LV2_Handle            engine;
  AloUridTable          urids;
  LV2_URID_Map          map;
  LV2_Atom_Forge        forge;
  LV2_URID              u_position, u_beat, u_bpm, u_bpb, u_speed, u_midi;

  float    values[ALO_BRIDGE_NPARAMS];
  float    port_values[ALO_PORT_COUNT];
  uint32_t max_block;
  double   rate;
  float*   in_l;
  float*   in_r;
  uint32_t dropped_midi;

  _Alignas(8) uint8_t control_seq[ALO_CONTROL_SEQ_BYTES];
  _Alignas(8) uint8_t midi_seq[ALO_MIDI_SEQ_BYTES];
};

/* Called only during instantiate/creation (non-RT). Unknown URIs past the
 * capacity map to 0, which the LV2 URID spec reserves for "not mapped". */
static LV2_URID urid_map(LV2_URID_Map_Handle handle, const char* uri)
{
  AloUridTable* t = (AloUridTable*)handle;
  for (uint32_t i = 0; i < t->count; ++i) {
    if (!strcmp(t->uris[i], uri)) {
      return i + 1u;
    }
  }
  if (t->count >= ALO_URID_CAP || strlen(uri) >= ALO_URI_LEN) {
    return 0;
  }
  memcpy(t->uris[t->count], uri, strlen(uri) + 1u);
  return ++t->count;
}

uint32_t alo_bridge_param_count(void)
{
  return (uint32_t)ALO_BRIDGE_NPARAMS;
}

const AloParamInfo* alo_bridge_param(uint32_t index)
{
  return index < ALO_BRIDGE_NPARAMS ? &ALO_BRIDGE_PARAMS[index] : NULL;
}

static float* port_slot(AloBridge* b, uint32_t port)
{
  return port < ALO_PORT_COUNT ? &b->port_values[port] : NULL;
}

AloBridge* alo_bridge_new(double sample_rate, uint32_t max_block)
{
  if (max_block == 0u) {
    max_block = 4096u;
  }
  AloBridge* b = (AloBridge*)calloc(1, sizeof(AloBridge));
  if (!b) {
    return NULL;
  }
  b->max_block = max_block;
  b->rate      = sample_rate;
  b->in_l      = (float*)calloc(max_block, sizeof(float));
  b->in_r      = (float*)calloc(max_block, sizeof(float));
  b->desc      = lv2_descriptor(0);
  if (!b->in_l || !b->in_r || !b->desc) {
    alo_bridge_free(b);
    return NULL;
  }

  b->map.handle = &b->urids;
  b->map.map    = urid_map;
  lv2_atom_forge_init(&b->forge, &b->map);
  b->u_position = urid_map(&b->urids, LV2_TIME__Position);
  b->u_beat     = urid_map(&b->urids, LV2_TIME__beat);
  b->u_bpm      = urid_map(&b->urids, LV2_TIME__beatsPerMinute);
  b->u_bpb      = urid_map(&b->urids, LV2_TIME__beatsPerBar);
  b->u_speed    = urid_map(&b->urids, LV2_TIME__speed);
  b->u_midi     = urid_map(&b->urids, LV2_MIDI__MidiEvent);

  int32_t                  max_len   = (int32_t)max_block;
  const LV2_Options_Option options[] = {
      {LV2_OPTIONS_INSTANCE, 0, urid_map(&b->urids, LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t),
       urid_map(&b->urids, LV2_ATOM__Int), &max_len},
      {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL},
  };
  const LV2_Feature  map_feature  = {LV2_URID__map, &b->map};
  const LV2_Feature  opts_feature = {LV2_OPTIONS__options, (void*)options};
  const LV2_Feature* features[]   = {&map_feature, &opts_feature, NULL};

  b->engine = b->desc->instantiate(b->desc, sample_rate, "", features);
  if (!b->engine) {
    alo_bridge_free(b);
    return NULL;
  }

  for (uint32_t i = 0; i < ALO_BRIDGE_NPARAMS; ++i) {
    b->values[i] = ALO_BRIDGE_PARAMS[i].def;
    float* slot  = port_slot(b, ALO_BRIDGE_PARAMS[i].port);
    if (slot) {
      *slot = ALO_BRIDGE_PARAMS[i].def;
      b->desc->connect_port(b->engine, ALO_BRIDGE_PARAMS[i].port, slot);
    }
  }
  b->desc->connect_port(b->engine, ALO_CONTROL, b->control_seq);
  b->desc->connect_port(b->engine, ALO_MIDIIN, b->midi_seq);
  alo_bridge_activate(b);
  return b;
}

void alo_bridge_free(AloBridge* b)
{
  if (!b) {
    return;
  }
  if (b->engine) {
    b->desc->cleanup(b->engine);
  }
  free(b->in_l);
  free(b->in_r);
  free(b);
}

void alo_bridge_activate(AloBridge* b)
{
  if (b && b->engine && b->desc->activate) {
    b->desc->activate(b->engine);
  }
}

void alo_bridge_set_param(AloBridge* b, uint32_t index, float value)
{
  if (!b || index >= ALO_BRIDGE_NPARAMS || ALO_BRIDGE_PARAMS[index].output) {
    return;
  }
  const AloParamInfo* p = &ALO_BRIDGE_PARAMS[index];
  if (!(value >= p->min)) {
    value = p->min; /* also catches NaN */
  }
  if (value > p->max) {
    value = p->max;
  }
  b->values[index]        = value;
  b->port_values[p->port] = value;
}

float alo_bridge_get_param(const AloBridge* b, uint32_t index)
{
  if (!b || index >= ALO_BRIDGE_NPARAMS) {
    return 0.0f;
  }
  const uint32_t port = ALO_BRIDGE_PARAMS[index].port;
  return port < ALO_PORT_COUNT ? b->port_values[port] : b->values[index];
}

uint32_t alo_bridge_dropped_midi(const AloBridge* b)
{
  return b ? b->dropped_midi : 0u;
}

static void set_extra(AloBridge* b, uint32_t id, float v)
{
  b->values[ALO_BRIDGE_NPARAMS - (ALO_BRIDGE_EXTRA_END - id)] = v;
}

static float block_peak(const float* x, uint32_t n)
{
  float peak = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    const float a = fabsf(x[i]);
    peak          = a > peak ? a : peak;
  }
  return peak;
}

/* Preserve VST3 clock precision. The shared engine also accepts the
 * legacy LV2 float tempo/beat atoms. */
static void forge_transport(AloBridge* b, const AloTransport* t)
{
  LV2_Atom_Forge*      f = &b->forge;
  LV2_Atom_Forge_Frame seq_frame, obj_frame;
  lv2_atom_forge_set_buffer(f, b->control_seq, sizeof(b->control_seq));
  lv2_atom_forge_sequence_head(f, &seq_frame, 0);
  if (t) {
    lv2_atom_forge_frame_time(f, 0);
    lv2_atom_forge_object(f, &obj_frame, 0, b->u_position);
    lv2_atom_forge_key(f, b->u_bpm);
    lv2_atom_forge_double(f, t->bpm);
    lv2_atom_forge_key(f, b->u_bpb);
    lv2_atom_forge_float(f, (float)t->beats_per_bar);
    lv2_atom_forge_key(f, b->u_speed);
    lv2_atom_forge_float(f, (t->valid && t->playing) ? 1.0f : 0.0f);
    if (t->valid) {
      lv2_atom_forge_key(f, b->u_beat);
      lv2_atom_forge_double(f, t->beat);
    }
    lv2_atom_forge_pop(f, &obj_frame);
  }
  lv2_atom_forge_pop(f, &seq_frame);
}

static void forge_midi(AloBridge* b, const AloMidiEvent* midi, uint32_t n_midi, uint32_t begin,
                       uint32_t end)
{
  LV2_Atom_Forge*      f = &b->forge;
  LV2_Atom_Forge_Frame seq_frame;
  lv2_atom_forge_set_buffer(f, b->midi_seq, sizeof(b->midi_seq));
  lv2_atom_forge_sequence_head(f, &seq_frame, 0);
  for (uint32_t i = 0; i < n_midi; ++i) {
    const AloMidiEvent* ev = &midi[i];
    if (ev->frame < begin || ev->frame >= end || ev->size == 0u || ev->size > 3u) {
      continue;
    }
    /* Reserve a whole event before writing anything: a partial event would
     * leave a sequence header with a dangling frame or payload. */
    const uint32_t event_bytes = sizeof(LV2_Atom_Event) + lv2_atom_pad_size(ev->size);
    if (event_bytes > f->size - f->offset) {
      b->dropped_midi++;
      continue;
    }
    lv2_atom_forge_frame_time(f, ev->frame - begin);
    lv2_atom_forge_atom(f, ev->size, b->u_midi);
    lv2_atom_forge_write(f, ev->data, ev->size);
  }
  lv2_atom_forge_pop(f, &seq_frame);
}

void alo_bridge_run(AloBridge* b, const AloTransport* transport, const float* in_l,
                    const float* in_r, float* out_l, float* out_r, uint32_t n_frames,
                    const AloMidiEvent* midi, uint32_t n_midi)
{
  if (!b || !out_l || !out_r) {
    return;
  }
  float        in_peak[2] = {0.0f, 0.0f}, out_peak[2] = {0.0f, 0.0f};
  AloTransport t = transport ? *transport : (AloTransport){false, false, 120.0, 4.0, 0.0};

  if (!(t.bpm >= 1.0 && t.bpm <= 10000.0)) {
    t.bpm = 120.0;
  }
  if (!(t.beats_per_bar >= 1.0 && t.beats_per_bar <= 64.0)) {
    t.beats_per_bar = 4.0;
  }
  t.valid                      = t.valid && t.beat >= -2e8 && t.beat <= 2e8;
  const double beat0           = t.valid ? t.beat : 0.0;
  const double beats_per_frame = t.valid && t.playing ? t.bpm / (60.0 * b->rate) : 0.0;

  for (uint32_t begin = 0; begin < n_frames;) {
    const uint32_t n = (n_frames - begin) < b->max_block ? (n_frames - begin) : b->max_block;
    /* Copy inputs: hosts may process in place and the engine writes outputs
     * before it has finished reading inputs. */
    if (in_l) {
      memcpy(b->in_l, in_l + begin, n * sizeof(float));
    } else {
      memset(b->in_l, 0, n * sizeof(float));
    }
    if (in_r) {
      memcpy(b->in_r, in_r + begin, n * sizeof(float));
    } else {
      memset(b->in_r, 0, n * sizeof(float));
    }
    b->desc->connect_port(b->engine, ALO_INPUT_L, b->in_l);
    b->desc->connect_port(b->engine, ALO_INPUT_R, b->in_r);
    b->desc->connect_port(b->engine, ALO_OUTPUT_L, out_l + begin);
    b->desc->connect_port(b->engine, ALO_OUTPUT_R, out_r + begin);

    t.beat = beat0 + begin * beats_per_frame;
    forge_transport(b, &t);
    forge_midi(b, midi, n_midi, begin, begin + n);
    b->desc->run(b->engine, n);

    in_peak[0]  = fmaxf(in_peak[0], block_peak(b->in_l, n));
    in_peak[1]  = fmaxf(in_peak[1], block_peak(b->in_r, n));
    out_peak[0] = fmaxf(out_peak[0], block_peak(out_l + begin, n));
    out_peak[1] = fmaxf(out_peak[1], block_peak(out_r + begin, n));

    begin += n;
  }

  set_extra(b, ALO_BRIDGE_IN_PEAK_L, in_peak[0]);
  set_extra(b, ALO_BRIDGE_IN_PEAK_R, in_peak[1]);
  set_extra(b, ALO_BRIDGE_OUT_PEAK_L, out_peak[0]);
  set_extra(b, ALO_BRIDGE_OUT_PEAK_R, out_peak[1]);
  set_extra(b, ALO_BRIDGE_HOST_BPM, (float)t.bpm);
  set_extra(b, ALO_BRIDGE_HOST_PLAYING, (t.valid && t.playing) ? 1.0f : 0.0f);
}
