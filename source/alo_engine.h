/**
 * @file alo_engine.h
 * @brief Core engine definitions, port indices, and public types.
 */

#ifndef ALO_ENGINE_H
#define ALO_ENGINE_H

#include <stdbool.h> /* cppcheck-suppress missingIncludeSystem */
#include <stdint.h> /* cppcheck-suppress missingIncludeSystem */
#include <lv2/atom/atom.h> /* cppcheck-suppress missingIncludeSystem */
#include <lv2/urid/urid.h> /* cppcheck-suppress missingIncludeSystem */
#include <lv2/midi/midi.h> /* cppcheck-suppress missingIncludeSystem */
#include <lv2/core/lv2.h> /* cppcheck-suppress missingIncludeSystem */

#include "slice_sampler.h"
#include "transient_detector.h"  /* needed for incremental detection state */

#define ALO_URI "http://ktano-studio.com/aloschen"

#ifndef LOOP_SIZE_MAX_SECONDS
/* Target-dependent maximum loop length. The default 32 s covers the documented
 * maximum of 16 bars at 120 BPM while reducing per-instance memory use.
 */
#define LOOP_SIZE_MAX_SECONDS 32u
#endif

/* Worst-case loop buffer size in samples, derived from the configured time
 * horizon at 48 kHz. All long-lived loop and sampler source buffers use this
 * upper bound.
 */
#define LOOP_SIZE (48000u * LOOP_SIZE_MAX_SECONDS)
#define NUM_TRACKS 3

#ifndef ALO_MAX_UNDO_LAYERS
/* Number of overdub layers per track (base layer is separate). Kept
 * overridable for lower-memory targets.
 */
#define ALO_MAX_UNDO_LAYERS 4
#endif

#define DEFAULT_BEATS_PER_BAR 4
#define DEFAULT_NUM_BARS 4
#define DEFAULT_BPM 120
#define HIGH_BEAT_FREQ 880
#define LOW_BEAT_FREQ 440
#define START_BEAT_FREQ 1760

/* Maximum number of slices the engine must handle simultaneously.  This is
 * derived from the UI limits: up to 16 bars and 2..8 slices per bar.  It is
 * used for fixed-size arrays stored in the Alo struct so that the audio
 * thread can index safely without dynamic allocation.
 */
#define ALO_MAX_SLICES (16u * 8u)

/* Upper bound for preallocated per-block scratch buffers used in the audio
 * thread. If a host provides blocks larger than this, the engine will process
 * them in bounded chunks (still no heap allocation in run()).
 */
#ifndef ALO_RT_BLOCK_CAP
#define ALO_RT_BLOCK_CAP 65536u
#endif

typedef enum
{
  TRACK_IDLE = 0,
  TRACK_ARM_BASE,
  TRACK_REC_BASE,
  TRACK_ARM_OVERDUB,
  TRACK_REC_OVERDUB,
} TrackRecState;

typedef enum
{
  ALO_INPUT_L         = 0,
  ALO_INPUT_R         = 1,
  ALO_OUTPUT_L        = 2,
  ALO_OUTPUT_R        = 3,
  ALO_LOOP1           = 4,
  ALO_UNDO1           = 5,
  ALO_LOOP2           = 6,
  ALO_UNDO2           = 7,
  ALO_LOOP3           = 8,
  ALO_UNDO3           = 9,
  ALO_MIDIIN          = 10,
  ALO_SLICE_ROOT      = 11,
  ALO_CLICK           = 12,
  ALO_BARS            = 13,
  ALO_CONTROL         = 14,
  ALO_MIX             = 15,
  ALO_ENABLED         = 16,
  ALO_LOOP1_STATE     = 17,
  ALO_LOOP2_STATE     = 18,
  ALO_LOOP3_STATE     = 19,
  ALO_BAR_STEP        = 20,
  ALO_LOOP1_VOL       = 21,
  ALO_LOOP2_VOL       = 22,
  ALO_LOOP3_VOL       = 23,
  ALO_UNDO1_STATE     = 24,
  ALO_UNDO2_STATE     = 25,
  ALO_UNDO3_STATE     = 26,
  ALO_LOOP1_HAS_AUDIO = 27,
  ALO_LOOP2_HAS_AUDIO = 28,
  ALO_LOOP3_HAS_AUDIO = 29,
  /* Debug/visualization outputs (MOD GUI rings). */
  ALO_CYCLE_PHASE    = 30,
  ALO_HOST_BAR_PHASE = 31,
  ALO_SAMPLER_VOL    = 32,
  ALO_SLICES_PER_BAR = 33,
  ALO_SPLIT_TRANSIENTS   = 34, /* boolean toggle: split slices at detected transients */
  ALO_DETECTED_SLICES    = 35, /* output: number of slices currently active/detected */
  ALO_TRANSIENT_THRESH   = 36, /* control: detection threshold ratio */
  ALO_SLICE_ENV_FRAC     = 37, /* control: release percent 0..100 of slice */
  ALO_SLICE_ENV_ATTACK   = 38, /* control: attack length in milliseconds */
  ALO_SLICE_SENS         = 39, /* control: detection sensitivity 0..1 */
  ALO_TRANSIENT_DEBOUNCE = 40, /* control: detector retrigger gap in ms */
  ALO_TRANSIENT_BURST    = 41, /* control: minimum onset burst length in ms */
  ALO_TRANSIENT_END      = 42, /* control: ratio below which a transient ends */
  ALO_TRANSIENT_PRE_MS   = 43, /* control: pre-roll before transient peak in ms */
} PortIndex;

/* Keep in sync with the highest port index + 1. */
#define ALO_PORT_COUNT 44

typedef struct
{
  LV2_URID atom_Blank;
  LV2_URID atom_Float;
  LV2_URID atom_Int;
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

  /* LV2 options / buf-size (instantiate-time only). */
  LV2_URID bufsz_maxBlockLength;
  LV2_URID bufsz_nominalBlockLength;
} AloURIs;

typedef struct
{
  const float* input_l;
  const float* input_r;
  float*       output_l;
  /** True when host beat position is advancing. */
  float* output_r;
  float* loop_btn[NUM_TRACKS];
  float* undo_btn[NUM_TRACKS];
  /** Per-track playback gain coefficient (0..1). */
  float* loop_vol[NUM_TRACKS];
  /** Sampler (slice one-shot) output gain coefficient (0..1). */
  float* sampler_vol;
  float* bars;
  /** Number of slices per bar for MIDI one-shots (integer 2..8). */
  float* slices_per_bar;
  float* split_by_transient;   /* 0 = uniform slices, >0 = transient-based */
  float* transient_threshold;  /* threshold multiplier for transient detection */
  float* slice_env_frac;       /* release length percent: 0..100 of slice length */
  float* slice_env_attack;     /* attack length in ms (hidden, not exposed via UI) */
  float* slice_sens;           /* normalized sensitivity 0..1 */
  float* transient_debounce;   /* detector retrigger gap in milliseconds */
  float* transient_burst;      /* minimum burst duration in milliseconds */
  float* transient_end_ratio;  /* candidate completion ratio */
  float* transient_pre_ms;     /* move slice start before peak by this many ms */
  float* detected_slices_out;  /* output count for UI */
  float* slice_root;
  float* click;
  float* mix;
  float* enabled;
  /** Loop phase origin in host beats (set at first base start). */
  float* loop_state_out[NUM_TRACKS];
  /** Undo queue indicator for UI (0=off, 0.25=queued). */
  float* undo_state_out[NUM_TRACKS];
  /** Slot filled indicator for UI (0=empty, 1=has committed base loop). */
  float* has_audio_out[NUM_TRACKS];
  /** Current beat-step within the Bars-length cycle (0..Bars*4-1). */
  float* bar_step_out;
  /** Continuous phase through Bars-length cycle (0..1). */
  float* cycle_phase_out;
  /** Continuous phase through the current host bar (0..1). */
  float* host_bar_phase_out;
  /** Whether loop_origin_beats has been set. */
  LV2_Atom_Sequence* control;
  LV2_Atom_Sequence* midiin;
} AloPorts;

typedef struct Alo
{
  LV2_URID_Map* map;
  AloURIs       uris;
  AloPorts      ports;

  double   rate;
  float    bpm;
  float    bpb;
  float    speed;
  bool     have_speed;
  uint32_t loop_beats;
  uint32_t loop_samples;
  float    current_position;

  bool have_transport;
  bool transport_moving;
  /** True when run_events() observed a time:Position update this cycle. */
  bool transport_updated_this_cycle;
  /** Number of consecutive cycles without time:Position updates. */
  uint32_t transport_blocks_without_update;
  uint32_t transport_loop_index;
  bool     transport_loop_index_pending;
  float    last_bar_beat;
  bool     have_last_bar_beat;
  int64_t  bar_counter_fallback;
  double   last_transport_beats;
  bool     have_last_transport_beats;

  // Loop phase origin in host beats (set when the first base recording starts).
  double loop_origin_beats;
  bool   have_loop_origin;

  AloSliceSampler slice_sampler;

  /* Preallocated per-block scratch buffers (RT-safe; allocated once at init). */
  uint32_t rt_block_cap;
  float*   rt_play_l;
  float*   rt_play_r;
  float*   rt_slice_l;
  float*   rt_slice_r;

  /* Sampler source cache: committed full-loop stereo mix buffer (L then R).
   * Rebuilt automatically (RT-safe, bounded work per run call) whenever the
   * committed loop content changes.
   */
  float* sampler_src_buf;
  /* Secondary buffer used while rebuilding the mix so that existing MIDI
   * slice voices can continue reading from the old audio without interruption.
   * When a rebuild completes the two pointers are swapped atomically.
   */
  float* sampler_src_buf_shadow;
  bool   sampler_src_shadow_valid;

  bool     sampler_src_valid;
  bool     sampler_src_dirty;
  bool     sampler_src_rebuild_active;
  uint32_t sampler_src_pos;
  float    sampler_src_peak_abs;
  float    sampler_src_norm_gain;
  uint32_t sampler_src_loop_samples;

  /** Per-track loop audio buffer (stereo stored as [0..LOOP_SIZE) L, [LOOP_SIZE..2*LOOP_SIZE) R).
   */
  float* loop_buf[NUM_TRACKS];
  bool   have_loop[NUM_TRACKS];

  /** Per-track overdub layers (each layer is a full loop-sized stereo buffer). */
  float* od_buf[NUM_TRACKS][ALO_MAX_UNDO_LAYERS];
  /** Number of active overdub layers. */
  uint8_t od_count[NUM_TRACKS];
  /** Target overdub layer index while recording an overdub. */
  uint8_t rec_od_layer[NUM_TRACKS];

  /** Pending quantized undo requests (applied at next bar downbeat). */
  uint8_t pending_undo[NUM_TRACKS];
  bool    pending_clear_all[NUM_TRACKS];

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
  /* Playhead uses double precision to support sub-sample positioning.  The
   * fractional part is used for linear interpolation during playback to
   * eliminate jitter when the loop length does not divide evenly into the
   * sample rate.  `loop_phase` is the corresponding integer offset (0..len-1)
   * used for quantized events and wrap detection without modulo ops.
   */
  double   loop_playhead;
  uint32_t loop_phase;

  float*   high_beat;
  float*   low_beat;
  float*   start_beat;
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
  bool   ui_have_cycle_origin;
  /** Track barBeat wrap to detect downbeats at block boundaries. */
  float ui_prev_bar_beat;
  bool  ui_have_prev_bar_beat;
  float inmix;
  float loopmix;

  /* Cached sampler gain normalisation derived from slices-per-bar. Recomputed
   * only when the control value changes so the audio thread avoids repeated
   * sqrtf work.
   */
  uint32_t cached_slices_per_bar;
  float    cached_sens;        /* last-used slice sensitivity for dirty detection */
  float    cached_threshold;   /* last-used transient threshold control */
  float    cached_debounce_ms; /* last-used detector debounce in ms */
  float    cached_burst_ms;    /* last-used minimum burst time in ms */
  float    cached_end_ratio;   /* last-used candidate end ratio */
  float    cached_pre_ms;      /* last-used transient pre-roll in ms */
  bool     cached_split_mode;  /* previous state of split_by_transient */
  uint32_t detected_slices_count; /* current number of slices after detection */
  uint32_t detected_slice_offsets[ALO_SLICE_SAMPLER_MAX_VOICES]; /* start offsets of each slice in samples */
  float    detected_slice_strength[ALO_SLICE_SAMPLER_MAX_VOICES]; /* strength for ranked selection */

  /* state used by the incremental transient detector so that scanning the
   * loop buffer can be spread across many audio blocks and avoid large
   * one-shot loops that could cause xruns.  Detection is restarted whenever
   * the loop content changes, or when the threshold/split-mode values change.
   */
  TransientDetector detect_td; /* working detector instance */
  uint32_t          detect_pos; /* next sample index to inspect */
  bool              detect_active; /* true while a scan is in progress */

  float    cached_slice_gain_scale;

  /** Track lv2:enabled state to avoid per-block resets when disabled. */
  bool have_last_enabled;
  bool last_enabled;

  int           pending_arm_track; // -1 if none
  TrackRecState pending_arm_type;
} Alo;

void alo_log(const char* message, ...);

void update_loop_state_ports(Alo* self);

void reset(Alo* self);
void reset_timing(Alo* self);
void run_events(Alo* self, uint32_t n_samples);
void run_loops(Alo* self, uint32_t n_samples);
void run_clicks(Alo* self, uint32_t n_samples);

#endif // ALO_ENGINE_H