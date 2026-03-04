#ifndef ALO_ENGINE_H
#define ALO_ENGINE_H

#include <stdbool.h>
#include <stdint.h>
#include <lv2/atom/atom.h>
#include <lv2/urid/urid.h>
#include <lv2/midi/midi.h>
#include <lv2/core/lv2.h>

#define ALO_URI "http://ktano-studio.com/aloschen"

#define LOOP_SIZE 2880000
#define NUM_TRACKS 3

#ifndef ALO_MAX_UNDO_LAYERS
// Number of overdub layers per track (base layer is separate).
#define ALO_MAX_UNDO_LAYERS 7
#endif

#define DEFAULT_BEATS_PER_BAR 4
#define DEFAULT_NUM_BARS 4
#define DEFAULT_BPM 120
#define HIGH_BEAT_FREQ 880
#define LOW_BEAT_FREQ 440
#define START_BEAT_FREQ 1760

typedef enum {
  TRACK_IDLE = 0,
  TRACK_ARM_BASE,
  TRACK_REC_BASE,
  TRACK_ARM_OVERDUB,
  TRACK_REC_OVERDUB,
} TrackRecState;

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
  ALO_MIDIIN = 10,
  ALO_MIDI_BASE = 11,
  ALO_CLICK = 12,
  ALO_BARS = 13,
  ALO_CONTROL = 14,
  ALO_MIX = 15,
  ALO_ENABLED = 16,
  ALO_LOOP1_STATE = 17,
  ALO_LOOP2_STATE = 18,
  ALO_LOOP3_STATE = 19,
  ALO_BAR_STEP = 20,
  ALO_LOOP1_VOL = 21,
  ALO_LOOP2_VOL = 22,
  ALO_LOOP3_VOL = 23,
  ALO_UNDO1_STATE = 24,
  ALO_UNDO2_STATE = 25,
  ALO_UNDO3_STATE = 26,
  ALO_LOOP1_HAS_AUDIO = 27,
  ALO_LOOP2_HAS_AUDIO = 28,
  ALO_LOOP3_HAS_AUDIO = 29,
} PortIndex;

/* Keep in sync with the highest port index + 1. */
#define ALO_PORT_COUNT 30

typedef struct {
  LV2_URID atom_Blank;
  LV2_URID atom_Float;
  LV2_URID atom_Long;
  LV2_URID atom_Object;
  LV2_URID midi_MidiEvent;
  LV2_URID atom_Path;
  LV2_URID atom_Resource;
  LV2_URID atom_Sequence;
  LV2_URID time_Position;
  /** Sample rate (Hz). */
  LV2_URID time_beat;
  /** Transport tempo (beats per minute). */
  LV2_URID time_barBeat;
  /** Beats per bar (time signature numerator for LV2 time:beatsPerBar). */
  LV2_URID time_bar;
  /** Host transport speed (typically 0=stopped, 1=running). */
  LV2_URID time_beatsPerMinute;
  /** Whether we've ever received LV2 time:speed from the host. */
  LV2_URID time_beatsPerBar;
  /** Loop length in beats (bpb * bars). */
  LV2_URID time_speed;
  /** Loop length in samples (derived from loop_beats, bpm, sample rate). */
} AloURIs;

typedef struct {
  const float *input_l;
  const float *input_r;
  float *output_l;
  /** True when host beat position is advancing. */
  float *output_r;
  float *loop_btn[NUM_TRACKS];
  float *undo_btn[NUM_TRACKS];
  /** Per-track playback gain coefficient (0..1). */
  float *loop_vol[NUM_TRACKS];
  float *bars;
  float *midi_base;
  float *click;
  float *mix;
  float *enabled;
  /** Loop phase origin in host beats (set at first base start). */
  float *loop_state_out[NUM_TRACKS];
  /** Undo queue indicator for UI (0=off, 0.25=queued). */
  float *undo_state_out[NUM_TRACKS];
  /** Slot filled indicator for UI (0=empty, 1=has committed base loop). */
  float *has_audio_out[NUM_TRACKS];
  /** Current beat-step within the Bars-length cycle (0..Bars*4-1). */
  float *bar_step_out;
  /** Whether loop_origin_beats has been set. */
  LV2_Atom_Sequence *control;
  LV2_Atom_Sequence *midiin;
} AloPorts;

typedef struct {
  LV2_URID_Map *map;
  AloURIs uris;
  AloPorts ports;

  double rate;
  float bpm;
  float bpb;
  float speed;
  bool have_speed;
  uint32_t loop_beats;
  uint32_t loop_samples;
  float current_position;

  bool have_transport;
  bool transport_moving;
  uint32_t transport_loop_index;
  bool transport_loop_index_pending;
  float last_bar_beat;
  bool have_last_bar_beat;
  int64_t bar_counter_fallback;
  double last_transport_beats;
  bool have_last_transport_beats;

  // Loop phase origin in host beats (set when the first base recording starts).
  double loop_origin_beats;
  bool have_loop_origin;

  bool midi_control;

  /** Per-track loop audio buffer (stereo stored as [0..LOOP_SIZE) L, [LOOP_SIZE..2*LOOP_SIZE) R). */
  float *loop_buf[NUM_TRACKS];
  bool have_loop[NUM_TRACKS];

  /** Per-track overdub layers (each layer is a full loop-sized stereo buffer). */
  float *od_buf[NUM_TRACKS][ALO_MAX_UNDO_LAYERS];
  /** Number of active overdub layers. */
  uint8_t od_count[NUM_TRACKS];
  /** Target overdub layer index while recording an overdub. */
  uint8_t rec_od_layer[NUM_TRACKS];

  /** Pending quantized undo requests (applied at next bar downbeat). */
  uint8_t pending_undo[NUM_TRACKS];
  bool pending_clear_all[NUM_TRACKS];

  TrackRecState track_state[NUM_TRACKS];
  /** Remaining samples until auto-stop (base/overdub). */
  uint32_t rec_remaining_samples[NUM_TRACKS];

  bool last_loop_input[NUM_TRACKS];
  bool last_undo_input[NUM_TRACKS];

  /**
   * Number of consecutive run_events() calls where the loop button input has
   * been high. Used to support both momentary (pulsed) and latching toggle
   * controls without double-toggling.
   */
  uint32_t loop_btn_high_frames[NUM_TRACKS];

  uint32_t loop_start;
  uint32_t loop_index;

  float *high_beat;
  float *low_beat;
  float *start_beat;
  uint32_t beat_len;
  uint32_t high_beat_offset;
  uint32_t low_beat_offset;
  uint32_t start_beat_offset;

  /** Metronome bar index within the user-selected Bars cycle (0..bars-1). */
  uint32_t click_bar_in_cycle;
  /** Last bar-step value sent to the UI output port (throttle UI updates). */
  int8_t ui_last_bar_step;
  /** Last Bars value observed by the engine for UI/cycle resync. */
  uint32_t ui_last_bars_i;
  /** When set, force the UI step indicator to restart at step 0 on the next downbeat. */
  bool ui_cycle_resync_pending;
  /** Track transport stop/run transitions (when time:speed is available). */
  bool ui_transport_was_stopped;
  /** Host beat position of the Bars-cycle origin (set on the first downbeat after resync). */
  double ui_cycle_origin_beats;
  bool ui_have_cycle_origin;
  /** Track barBeat wrap to detect downbeats at block boundaries. */
  float ui_prev_bar_beat;
  bool ui_have_prev_bar_beat;
  float inmix;
  float loopmix;

  /** Track lv2:enabled state to avoid per-block resets when disabled. */
  bool have_last_enabled;
  bool last_enabled;
} Alo;

void alo_log(const char *message, ...);
void update_loop_state_ports(Alo *self);

void reset(Alo *self);
void reset_timing(Alo *self);
void run_events(Alo *self);
void run_loops(Alo *self, uint32_t n_samples);
void run_clicks(Alo *self, uint32_t n_samples);

#endif // ALO_ENGINE_H