/*
  Copyright 2006-2012 David Robillard <d@drobilla.net>
  Copyright 2006 Steve Harris <steve@plugin.org.uk>
  Copyright 2018 Stevie <modplugins@radig.com>
  Copyright 2018 Paul Sherwood <devcurmudgeon@gmail.com>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

/** Include standard C headers */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>

#include "lv2/atom/atom.h"
#include "lv2/atom/util.h"
#include "lv2/time/time.h"
#include "lv2/urid/urid.h"
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>

#define ALO_URI "http://ktano-studio.com/aloschen"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
  LV2_URID time_beat;
  LV2_URID time_barBeat;
  LV2_URID time_bar;
  LV2_URID time_beatsPerMinute;
  LV2_URID time_beatsPerBar;
  LV2_URID time_speed;
} AloURIs;

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
} PortIndex;

typedef enum {
  STATE_OFF,    // No click
  STATE_ATTACK, // Envelope rising
  STATE_DECAY,  // Envelope lowering
  STATE_SILENT  // Silent
} ClickState;

static const size_t LOOP_SIZE = 2880000;

#define NUM_TRACKS 3

#ifndef ALO_MAX_UNDO_LAYERS
// Number of overdub layers per track (base layer is separate).
// Memory use is roughly: NUM_TRACKS * (1 + ALO_MAX_UNDO_LAYERS) * LOOP_SIZE * 2 * sizeof(float)
#define ALO_MAX_UNDO_LAYERS 8
#endif

static const int NUM_LAYERS = 1 + ALO_MAX_UNDO_LAYERS;
static const bool LOG_ENABLED = true;

#define DEFAULT_BEATS_PER_BAR 4
#define DEFAULT_NUM_BARS 4
#define DEFAULT_BPM 120
#define DEFAULT_INSTANT_LOOPS 0

#define HIGH_BEAT_FREQ 880
#define LOW_BEAT_FREQ 440

void log(const char *message, ...) {
  if (!LOG_ENABLED) {
    return;
  }

  FILE *f;
  f = fopen("/tmp/alo.log", "a+");

  char buffer[2048];
  va_list argumentList;
  va_start(argumentList, message);
  vsnprintf(&buffer[0], sizeof(buffer), message, argumentList);
  va_end(argumentList);
  fwrite(buffer, 1, strlen(buffer), f);
  fprintf(f, "\n");
  fclose(f);
}

///
/// Convert an input parameter expressed as db into a linear float value
///
static float dbToFloat(float db) {
  if (db <= -90.0f)
    return 0.0f;
  return powf(10.0f, db * 0.05f);
}

/**
   Every plugin defines a private structure for the plugin instance.  All data
   associated with a plugin instance is stored here, and is available to
   every instance method.
*/
typedef struct {

  LV2_URID_Map *map; // URID map feature
  AloURIs uris;      // Cache of mapped URIDs

  // Port buffers
  struct {
    const float *input_l;
    const float *input_r;
    float *output_l;
    float *output_r;
    float *loop_btn[NUM_TRACKS];
    float *undo_btn[NUM_TRACKS];
    float *bars;
    float *threshold;
    float *midi_base; // start note for midi control of loops
    float *pb_loops;  // number of loops in instant mode
    float *click;     // click volume
    float *mix;
    float *reset_mode;
    int *enabled;
    float *loop_state_out[NUM_TRACKS];
    LV2_Atom_Sequence *control;
    LV2_Atom_Sequence *midiin; // midi input
  } ports;

  // Variables to keep track of the tempo information sent by the host
  double rate;           // Sample rate
  float bpm;             // Beats per minute (tempo)
  float bpb;             // Beats per bar
  float speed;           // Transport speed (usually 0=stop, 1=play)
  float threshold;       // minimum level to trigger loop start
  uint32_t loop_beats;   // loop length in beats
  uint32_t loop_samples; // loop length in samples
  uint32_t current_bb;   // which beat of the bar we are on (1, 2, 3, 0)
  uint32_t current_lb;   // which beat of the loop we are on (1, 2, ...)
  float current_position;

  // Transport-aligned loop phase (derived from host time:Position).
  bool have_transport;
  uint32_t transport_loop_index;      // 0..loop_samples-1
  bool transport_loop_index_pending;  // apply at start of next audio processing
  float last_bar_beat;
  bool have_last_bar_beat;
  int64_t bar_counter_fallback; // used if host doesn't provide time:bar
  double last_transport_beats;
  bool have_last_transport_beats;

  uint32_t pb_loops; // number of loops in instant mode
  bool midi_control;

  // Layered looper engine: 3 independent tracks.
  // Each track has a base layer plus N overdub layers.
  float *layers[NUM_TRACKS][/*NUM_LAYERS*/ 1 + ALO_MAX_UNDO_LAYERS];
  uint32_t layer_count[NUM_TRACKS]; // active layers (0 = empty, 1 = base only, ...)

  // "Tape" capture: continuously record input into this ring buffer.
  // Stereo is stored in two halves: [0..LOOP_SIZE) for L and [+LOOP_SIZE..+2*LOOP_SIZE) for R.
  float *preroll;
  uint32_t tape_index;
  uint32_t tape_filled;

  // Loop button is a toggle: 1 means "capture enabled"; 0 means "capture disabled".
  // When capture transitions 1->0, we commit the last loop (N bars) at the next loop boundary.
  bool capture_on[NUM_TRACKS];
  bool commit_pending[NUM_TRACKS];
  bool undo_pending[NUM_TRACKS];

  // Auto-stop for the first loop: if track is empty and Loop is turned on,
  // start at the next boundary and auto-commit at the following boundary.
  bool base_arm[NUM_TRACKS];
  bool base_recording[NUM_TRACKS];

  bool last_loop_input[NUM_TRACKS];
  bool last_undo_input[NUM_TRACKS];

  uint32_t loop_start; // non-zero for free-running loops
  uint32_t loop_index; // index into loop for current play point

  ClickState clickstate;

  uint32_t elapsed_len; // Frames since the start of the last click
  uint32_t wave_offset; // Current play offset in the wave

  // Click beats
  float *high_beat;
  float *low_beat;
  uint32_t beat_len;
  uint32_t high_beat_offset;
  uint32_t low_beat_offset;
  float inmix;
  float loopmix;
} Alo;

static inline float* ensure_layer(Alo* self, const int track, const int layer) {
  if (track < 0 || track >= NUM_TRACKS || layer < 0 || layer >= NUM_LAYERS) {
    return NULL;
  }
  if (!self->layers[track][layer]) {
    self->layers[track][layer] = (float*)calloc(LOOP_SIZE * 2, sizeof(float));
  }
  return self->layers[track][layer];
}

static inline void clear_layer(float* layer) {
  if (!layer) {
    return;
  }
  memset(layer, 0, LOOP_SIZE * 2 * sizeof(float));
}

static inline void tape_write(Alo* self, const float l, const float r) {
  if (!self->preroll) {
    return;
  }

  const uint32_t idx = self->tape_index;
  self->preroll[idx] = l;
  self->preroll[idx + LOOP_SIZE] = r;
  self->tape_index = (idx + 1u) % LOOP_SIZE;
  if (self->tape_filled < LOOP_SIZE) {
    self->tape_filled++;
  }
}

static inline void commit_from_tape(Alo* self, const int track, const int layer_index) {
  if (!self->preroll) {
    return;
  }

  float* const layer = ensure_layer(self, track, layer_index);
  if (!layer) {
    return;
  }

  const uint32_t n = self->loop_samples;
  if (n == 0 || n > LOOP_SIZE) {
    return;
  }

  if (self->tape_filled < n) {
    // Not enough audio captured since last reset/instantiate.
    return;
  }

  const uint32_t end = self->tape_index; // next write position (segment ends here)
  const uint32_t start = (end + LOOP_SIZE - n) % LOOP_SIZE;

  const uint32_t base = self->loop_start;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t ti = (start + i) % LOOP_SIZE;
    const uint32_t di = base + i;
    layer[di] = self->loopmix * self->preroll[ti];
    layer[di + LOOP_SIZE] = self->loopmix * self->preroll[ti + LOOP_SIZE];
  }
}

static inline void update_loop_state_ports(Alo* self) {
  for (int t = 0; t < NUM_TRACKS; ++t) {
    if (self->ports.loop_state_out[t]) {
      *(self->ports.loop_state_out[t]) = self->capture_on[t] ? 1.0f : 0.0f;
    }
  }
}

void sine_pulse(float *target, double frequency, double sample_rate,
                uint32_t num_samples) {
  const uint32_t half_length = (uint32_t)(num_samples * 0.5f);
  const float amplitude_step = 1.0f / (float)half_length;
  const double sample_sin_step = 2 * M_PI * frequency / sample_rate;
  float amplitude = 0.0f;

  for (uint32_t i = 0; i < half_length; ++i) {
    amplitude = fmin(amplitude + amplitude_step, 1.0f);
    target[i] = 0.5f * amplitude * sin(i * sample_sin_step);
  }

  for (uint32_t i = half_length; i < num_samples; ++i) {
    amplitude = fmax(amplitude - amplitude_step, 0.0f);
    target[i] = 0.5f * amplitude * sin(i * sample_sin_step);
  }
}

/**
   The `instantiate()` function is called by the host to create a new plugin
   instance.  The host passes the plugin descriptor, sample rate, and bundle
   path for plugins that need to load additional resources (e.g. waveforms).
   The features parameter contains host-provided features defined in LV2
   extensions, but this simple plugin does not use any.

   This function is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/
static LV2_Handle instantiate(const LV2_Descriptor *descriptor, double rate,
                              const char *bundle_path,
                              const LV2_Feature *const *features) {
  log("Instantiate");

  Alo *self = (Alo *)calloc(1, sizeof(Alo));
  self->rate = rate;
  self->bpb = DEFAULT_BEATS_PER_BAR;
  self->loop_beats = DEFAULT_BEATS_PER_BAR * DEFAULT_NUM_BARS;
  self->bpm = DEFAULT_BPM;
  self->loop_samples = self->loop_beats * self->rate * 60.0f / self->bpm;
  self->current_bb = 0;
  self->current_lb = 0;
  self->current_position = 0.0f;

  self->have_transport = false;
  self->transport_loop_index = 0;
  self->transport_loop_index_pending = false;
  self->last_bar_beat = 0.0f;
  self->have_last_bar_beat = false;
  self->bar_counter_fallback = 0;
  self->last_transport_beats = 0.0;
  self->have_last_transport_beats = false;
  self->pb_loops = DEFAULT_INSTANT_LOOPS;

  self->midi_control = false;

  for (int t = 0; t < NUM_TRACKS; ++t) {
    self->layer_count[t] = 0;
    self->capture_on[t] = false;
    self->commit_pending[t] = false;
    self->undo_pending[t] = false;
    self->base_arm[t] = false;
    self->base_recording[t] = false;
    self->last_loop_input[t] = false;
    self->last_undo_input[t] = false;

    for (int l = 0; l < NUM_LAYERS; ++l) {
      self->layers[t][l] = NULL; // lazy
    }
  }

  self->preroll = (float*)calloc(LOOP_SIZE * 2, sizeof(float));
  if (!self->preroll) {
    fprintf(stderr, "ALO: preroll allocation failed\n");
    free(self);
    return NULL;
  }
  self->tape_index = 0;
  self->tape_filled = 0;
  self->loop_start = 0;
  self->loop_index = 0;
  self->threshold = 0.0;

  for (int t = 0; t < NUM_TRACKS; ++t) {
    self->ports.loop_state_out[t] = NULL;
  }

  LV2_URID_Map *map = NULL;
  for (int i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID_URI "#map")) {
      map = (LV2_URID_Map *)features[i]->data;
    }
  }
  if (!map) {
    fprintf(stderr, "Host does not support urid:map.\n");
    free(self);
    return NULL;
  }

  // Map URIS
  AloURIs *const uris = &self->uris;
  self->map = map;
  uris->atom_Blank = map->map(map->handle, LV2_ATOM__Blank);
  uris->atom_Float = map->map(map->handle, LV2_ATOM__Float);
  uris->atom_Long = map->map(map->handle, LV2_ATOM__Long);
  uris->atom_Object = map->map(map->handle, LV2_ATOM__Object);
  uris->atom_Path = map->map(map->handle, LV2_ATOM__Path);
  uris->atom_Resource = map->map(map->handle, LV2_ATOM__Resource);
  uris->atom_Sequence = map->map(map->handle, LV2_ATOM__Sequence);
  uris->time_Position = map->map(map->handle, LV2_TIME__Position);
  uris->time_beat = map->map(map->handle, LV2_TIME__beat);
  uris->time_barBeat = map->map(map->handle, LV2_TIME__barBeat);
  uris->time_bar = map->map(map->handle, LV2_TIME__bar);
  uris->time_beatsPerMinute = map->map(map->handle, LV2_TIME__beatsPerMinute);
  uris->time_speed = map->map(map->handle, LV2_TIME__speed);
  uris->time_beatsPerBar = map->map(map->handle, LV2_TIME__beatsPerBar);
  uris->midi_MidiEvent = map->map(map->handle, LV2_MIDI__MidiEvent);

  // Generate pulses for the metronome
  self->beat_len = (uint32_t)(0.02f * self->rate);
  self->high_beat = (float *)malloc(self->beat_len * sizeof(float));
  self->low_beat = (float *)malloc(self->beat_len * sizeof(float));
  sine_pulse(self->high_beat, HIGH_BEAT_FREQ, self->rate, self->beat_len);
  sine_pulse(self->low_beat, LOW_BEAT_FREQ, self->rate, self->beat_len);
  self->high_beat_offset = self->beat_len;
  self->low_beat_offset = self->beat_len;

  return (LV2_Handle)self;
}

/**
   The `connect_port()` method is called by the host to connect a particular
   port to a buffer.  The plugin must store the data location, but data may not
   be accessed except in run().

   This method is in the ``audio'' threading class, and is called in the same
   context as run().
*/
static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
  log("Connect");
  Alo *self = (Alo *)instance;

  switch ((PortIndex)port) {
  case ALO_INPUT_L:
    self->ports.input_l = (const float *)data;
    log("Connect ALO_INPUT %d", port);
    break;
  case ALO_OUTPUT_L:
    self->ports.output_l = (float *)data;
    log("Connect ALO_OUTPUT %d", port);
    break;
  case ALO_INPUT_R:
    self->ports.input_r = (const float *)data;
    log("Connect ALO_INPUT %d", port);
    break;
  case ALO_OUTPUT_R:
    self->ports.output_r = (float *)data;
    log("Connect ALO_OUTPUT %d", port);
    break;
  case ALO_BARS:
    self->ports.bars = (float *)data;
    log("Connect ALO_BEATS %d %d", port);
    break;
  case ALO_CONTROL:
    self->ports.control = (LV2_Atom_Sequence *)data;
    log("Connect ALO_CONTROL %d", port);
    break;
  case ALO_THRESHOLD:
    self->ports.threshold = (float *)data;
    log("Connect ALO_THRESHOLD %d %d", port);
    break;
  case ALO_MIDIIN:
    self->ports.midiin = (LV2_Atom_Sequence *)data;
    log("Connect ALO_MIDIIN %d %d", port);
    break;
  case ALO_MIDI_BASE:
    self->ports.midi_base = (float *)data;
    log("Connect ALO_MIDI_BASE %d %d", port);
    break;
  case ALO_INSTANT_LOOPS:
    self->ports.pb_loops = (float *)data;
    log("Connect ALO_INSTANT_LOOPS %d %d", port);
    break;
  case ALO_CLICK:
    self->ports.click = (float *)data;
    log("Connect ALO_CLICK %d %d", port);
    break;
  case ALO_MIX:
    self->ports.mix = (float *)data;
    log("Connect ALO_MIX %d", port);
    break;
  case ALO_RESET_MODE:
    self->ports.reset_mode = (float *)data;
    log("Connect ALO_RESET_MODE %d", port);
    break;
  case ALO_ENABLED:
    self->ports.enabled = (int *)data;
    log("Connect ALO_ENABLED %d", port);
    break;
  case ALO_LOOP1:
    self->ports.loop_btn[0] = (float *)data;
    break;
  case ALO_UNDO1:
    self->ports.undo_btn[0] = (float *)data;
    break;
  case ALO_LOOP2:
    self->ports.loop_btn[1] = (float *)data;
    break;
  case ALO_UNDO2:
    self->ports.undo_btn[1] = (float *)data;
    break;
  case ALO_LOOP3:
    self->ports.loop_btn[2] = (float *)data;
    break;
  case ALO_UNDO3:
    self->ports.undo_btn[2] = (float *)data;
    break;
  case ALO_LOOP1_STATE:
    self->ports.loop_state_out[0] = (float *)data;
    break;
  case ALO_LOOP2_STATE:
    self->ports.loop_state_out[1] = (float *)data;
    break;
  case ALO_LOOP3_STATE:
    self->ports.loop_state_out[2] = (float *)data;
    break;
  default:
    break;
  }
  log("Connect end");
}

static void reset(Alo *self) {
  log("Reset");
  for (int t = 0; t < NUM_TRACKS; ++t) {
    self->layer_count[t] = 0;
    self->capture_on[t] = false;
    self->commit_pending[t] = false;
    self->undo_pending[t] = false;
    self->base_arm[t] = false;
    self->base_recording[t] = false;
    self->last_loop_input[t] = false;
    self->last_undo_input[t] = false;
  }

  self->pb_loops = (uint32_t)floorf(*(self->ports.pb_loops));
  self->loop_beats =
      (uint32_t)floorf(self->bpb) * (uint32_t)floorf(*(self->ports.bars));
  self->loop_samples = self->loop_beats * self->rate * 60.0f / self->bpm;

  if (self->loop_samples > LOOP_SIZE || self->speed == 0) {
    self->loop_samples = LOOP_SIZE;
  }
  self->loop_index = 0;
  self->loop_start = 0;
  self->tape_index = 0;
  self->tape_filled = 0;

  self->have_transport = false;
  self->transport_loop_index = 0;
  self->transport_loop_index_pending = false;
  self->have_last_bar_beat = false;
  self->last_bar_beat = 0.0f;
  self->bar_counter_fallback = 0;
  self->last_transport_beats = 0.0;
  self->have_last_transport_beats = false;
  update_loop_state_ports(self);
  log("Loop beats: %d", self->loop_beats);
  log("BPM: %G", self->bpm);
  log("Loop_samples: %d", self->loop_samples);
  log("Reset end");
}

/**
   The `activate()` method is called by the host to initialise and prepare the
   plugin instance for running.	 The plugin must reset all internal state
   except for buffer locations set by `connect_port()`.	 Since this plugin has
   no other internal state, this method does nothing.

   This method is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/
static void activate(LV2_Handle instance) { log("Activate"); }

/**
   Update the current (midi) position based on a host message.	This is called
   by run() when a time:Position is received.
*/
static void update_position(Alo *self, const LV2_Atom_Object *obj) {
  AloURIs *const uris = &self->uris;

  // Received new transport position/speed
  LV2_Atom *abs_beat = NULL, *beat = NULL, *bar = NULL, *bpm = NULL, *bpb = NULL, *speed = NULL;
  lv2_atom_object_get(obj, uris->time_beat, &abs_beat, uris->time_barBeat, &beat,
                      uris->time_beatsPerMinute,
                      &bpm, uris->time_speed, &speed, uris->time_beatsPerBar,
                      &bpb, uris->time_bar, &bar, NULL);

  if (bpb && bpb->type == uris->atom_Float) {
    if (self->bpb != ((LV2_Atom_Float *)bpb)->body) {
      self->bpb = ((LV2_Atom_Float *)bpb)->body;
      reset(self);
    }
  }

  if ((uint32_t)floorf(self->bpb) * (uint32_t)floorf(*(self->ports.bars)) !=
      self->loop_beats) {
    reset(self);
  }

  if (bpm && bpm->type == uris->atom_Float) {
    if (round(self->bpm) != round(((LV2_Atom_Float *)bpm)->body)) {
      // Tempo changed, update BPM
      self->bpm = ((LV2_Atom_Float *)bpm)->body;
      reset(self);
    }
  }

  if ((uint32_t)floorf(self->bpb) * (uint32_t)floorf(*(self->ports.bars)) !=
      self->loop_beats) {
    reset(self);
  }

  if (speed && speed->type == uris->atom_Float) {
    if (self->speed != ((LV2_Atom_Float *)speed)->body) {
      // Speed changed, e.g. 0 (stop) to 1 (play)
      // reset the loop start
      self->speed = ((LV2_Atom_Float *)speed)->body;
      reset(self);
      log("Speed change: %G", self->speed);
      log("Loop: [%d][%d]", self->loop_beats, self->loop_samples);
    };
  }
  // Prefer absolute time:beat when available; it's the best way to align loops
  // across bars without relying on time:bar.
  if (abs_beat && abs_beat->type == uris->atom_Float) {
    const double global_beats = (double)((LV2_Atom_Float *)abs_beat)->body;
    const float bar_beat = (self->bpb > 0.0f) ? fmodf((float)global_beats, self->bpb) : 0.0f;
    self->current_position = bar_beat;

    if (self->loop_beats > 0 && self->loop_samples > 0) {
      if (self->have_last_transport_beats && global_beats < self->last_transport_beats - 0.5) {
        log("Transport seek backwards (%.3f -> %.3f), resetting", self->last_transport_beats,
            global_beats);
        reset(self);
      }
      self->last_transport_beats = global_beats;
      self->have_last_transport_beats = true;

      const double phase_beats = fmod(global_beats, (double)self->loop_beats);
      double phase_samples_d = phase_beats * (double)self->loop_samples / (double)self->loop_beats;
      if (phase_samples_d < 0.0) {
        phase_samples_d = 0.0;
      }
      uint32_t phase_samples = (uint32_t)floor(phase_samples_d);
      if (phase_samples >= self->loop_samples) {
        phase_samples = self->loop_samples - 1;
      }
      self->transport_loop_index = phase_samples;
      self->transport_loop_index_pending = true;
      self->have_transport = true;
    }
  } else if (beat && beat->type == uris->atom_Float) {
    // Received a beat position within the bar, synchronise
    self->current_position = ((LV2_Atom_Float *)beat)->body;
    if (self->current_bb != (uint32_t)self->current_position) {
      // we are onto the next beat
      self->current_bb = (uint32_t)self->current_position;
      if (self->current_lb == self->loop_beats) {
        self->current_lb = 0;
      }
      // log("Beat:[%d][%d] index[%d] beat[%G]\n", self->current_bb,
      // self->current_lb, self->loop_index, self->current_position);
      self->current_lb += 1;
    }

    // Host-aligned looping: compute current loop phase from transport.
    // Prefer time:bar when available, else maintain a fallback bar counter.
    int64_t bar_index = 0;
    if (bar) {
      if (bar->type == uris->atom_Float) {
        bar_index = (int64_t)floorf(((LV2_Atom_Float *)bar)->body);
      } else if (bar->type == uris->atom_Long) {
        bar_index = (int64_t)((const LV2_Atom_Long*)bar)->body;
      }
    }

    const float bar_beat = self->current_position;
    if (!bar) {
      if (self->have_last_bar_beat) {
        // Detect bar wrap (e.g. 3.9 -> 0.0)
        if (bar_beat + 0.25f < self->last_bar_beat) {
          self->bar_counter_fallback++;
        }
      }
      bar_index = self->bar_counter_fallback;
    }
    self->last_bar_beat = bar_beat;
    self->have_last_bar_beat = true;

    if (self->loop_beats > 0 && self->loop_samples > 0) {
      const double global_beats = (double)bar_index * (double)self->bpb + (double)bar_beat;

      if (self->have_last_transport_beats && global_beats < self->last_transport_beats - 0.5) {
        log("Transport seek backwards (%.3f -> %.3f), resetting", self->last_transport_beats,
            global_beats);
        reset(self);
      }
      self->last_transport_beats = global_beats;
      self->have_last_transport_beats = true;

      const double phase_beats = fmod(global_beats, (double)self->loop_beats);
      double phase_samples_d = phase_beats * (double)self->loop_samples / (double)self->loop_beats;
      if (phase_samples_d < 0.0) {
        phase_samples_d = 0.0;
      }
      uint32_t phase_samples = (uint32_t)floor(phase_samples_d);
      if (phase_samples >= self->loop_samples) {
        phase_samples = self->loop_samples - 1;
      }
      self->transport_loop_index = phase_samples;
      self->transport_loop_index_pending = true;
      self->have_transport = true;
    }
  }
}

static void handle_loop_toggle(Alo* self, const int t) {
  if (t < 0 || t >= NUM_TRACKS) {
    return;
  }

  if (!self->capture_on[t]) {
    self->capture_on[t] = true;
    self->commit_pending[t] = false;
    if (self->layer_count[t] == 0) {
      self->base_arm[t] = true;
      self->base_recording[t] = false;
      log("[[ Track %d: base arm (auto-stop enabled) ]]", t + 1);
    }
    log("[[ Track %d: capture ON ]]", t + 1);
  } else {
    self->capture_on[t] = false;
    self->commit_pending[t] = true;
    self->base_arm[t] = false;
    self->base_recording[t] = false;
    log("[[ Track %d: capture OFF -> commit pending @ next boundary ]]", t + 1);
  }
}

static void handle_loop_edge(Alo* self, const int t, const bool loop_btn) {
  if (t < 0 || t >= NUM_TRACKS) {
    return;
  }

  if (loop_btn) {
    self->capture_on[t] = true;
    self->commit_pending[t] = false;
    if (self->layer_count[t] == 0) {
      self->base_arm[t] = true;
      self->base_recording[t] = false;
      log("[[ Track %d: base arm (auto-stop enabled) ]]", t + 1);
    }
    log("[[ Track %d: capture ON ]]", t + 1);
  } else {
    if (self->capture_on[t]) {
      self->capture_on[t] = false;
      self->commit_pending[t] = true;
      self->base_arm[t] = false;
      self->base_recording[t] = false;
      log("[[ Track %d: capture OFF -> commit pending @ next boundary ]]", t + 1);
    }
  }
}

static void handle_undo_press(Alo* self, const int t) {
  if (t < 0 || t >= NUM_TRACKS) {
    return;
  }

  // Instant undo: stop affecting output immediately (no boundary wait).
  self->commit_pending[t] = false;
  self->base_arm[t] = false;
  self->base_recording[t] = false;
  self->capture_on[t] = false;

  if (self->layer_count[t] > 0) {
    self->layer_count[t]--;
    log("[[ Track %d: UNDO NOW -> layers=%d ]]", t + 1, self->layer_count[t]);
  }
}

static void handle_button_edges(Alo* self, const int t, const bool loop_btn, const bool undo_btn) {
  if (loop_btn != self->last_loop_input[t]) {
    handle_loop_edge(self, t, loop_btn);
  }
  if (undo_btn && !self->last_undo_input[t]) {
    handle_undo_press(self, t);
  }
  self->last_loop_input[t] = loop_btn;
  self->last_undo_input[t] = undo_btn;
}

/**
   ** Taken directly from metro.c **
   Play back audio for the range [begin..end) relative to this cycle.  This is
   called by run() in-between events to output audio up until the current time.
*/
static void click(Alo *self, uint32_t begin, uint32_t end) {
  float *const output_l = self->ports.output_l;
  float *const output_r = self->ports.output_r;

  float amplitude = (uint32_t)floorf(*(self->ports.click));

  for (uint32_t idx = begin; idx < end; idx++) {
    if (self->high_beat_offset < self->beat_len) {
      output_l[idx] +=
          0.1 * amplitude * self->high_beat[self->high_beat_offset];
      output_r[idx] +=
          0.1 * amplitude * self->high_beat[self->high_beat_offset];
      self->high_beat_offset++;
    }

    if (self->low_beat_offset < self->beat_len) {
      output_l[idx] += 0.1 * amplitude * self->low_beat[self->low_beat_offset];
      output_r[idx] += 0.1 * amplitude * self->low_beat[self->low_beat_offset];
      self->low_beat_offset++;
    }
  }
}

static void run_clicks(Alo *self, uint32_t n_samples) {
  bool play_click = true;

  const float old_beat = floorf(self->current_position);
  self->current_position += n_samples / self->rate / 60.0f * self->bpm;
  const float new_beat = floorf(self->current_position);
  self->current_position = fmodf(self->current_position, self->bpb);
  const float beat = floorf(self->current_position);

  for (uint32_t t = 0; t < NUM_TRACKS; t++) {
    // Preserve old behavior: click plays while recording, and stops once any loop is playing.
    if (self->layer_count[t] > 0) {
      play_click = false;
    }
  }

  if (play_click && *self->ports.click && self->speed) {
    if (new_beat != old_beat) {
      const uint32_t sample_offset =
          (uint32_t)((self->current_position - beat) * self->rate);

      click(self, 0, sample_offset);

      if (beat == 0.0f) {
        self->high_beat_offset = 0;
      } else {
        self->low_beat_offset = 0;
      }

      click(self, sample_offset, n_samples);
    } else {
      click(self, 0, n_samples);
    }
  }
}

static void run_events(Alo *self) {
  // 1) Transport/time first (keeps loop phase aligned)
  const AloURIs *uris = &self->uris;
  const LV2_Atom_Sequence *in = self->ports.control;
  for (const LV2_Atom_Event *ev = lv2_atom_sequence_begin(&in->body);
       !lv2_atom_sequence_is_end(&in->body, in->atom.size, ev);
       ev = lv2_atom_sequence_next(ev)) {

    if (ev->body.type == uris->atom_Object || ev->body.type == uris->atom_Blank) {
      const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
      if (obj->body.otype == uris->time_Position) {
        update_position(self, obj);
      }
    }
  }

  // 2) MIDI (if present)
  const LV2_Atom_Sequence *midiin = self->ports.midiin;
  self->midi_control = false;
  for (const LV2_Atom_Event *ev = lv2_atom_sequence_begin(&midiin->body);
       !lv2_atom_sequence_is_end(&midiin->body, midiin->atom.size, ev);
       ev = lv2_atom_sequence_next(ev)) {

    if (ev->body.type == self->uris.midi_MidiEvent) {
      const uint8_t *const msg = (const uint8_t *)(ev + 1);
      const int note = (int)msg[1];
      const int base = (int)floorf(*(self->ports.midi_base));
      const int rel = note - base;

      if (rel >= 0 && rel < 6) {
        const int track = rel % 3;
        const bool is_undo = (rel >= 3);

        if (lv2_midi_message_type(msg) == LV2_MIDI_MSG_NOTE_ON) {
          if (is_undo) {
            handle_undo_press(self, track);
          } else {
            handle_loop_toggle(self, track);
          }
        }

        self->midi_control = true;
      }
    }
  }

  // 3) UI button edges (unless MIDI is driving)
  if (self->midi_control == false) {
    for (int t = 0; t < NUM_TRACKS; ++t) {
      const bool loop_btn = self->ports.loop_btn[t] && (*self->ports.loop_btn[t] > 0.0f);
      const bool undo_btn = self->ports.undo_btn[t] && (*self->ports.undo_btn[t] > 0.0f);
      handle_button_edges(self, t, loop_btn, undo_btn);
    }
  }
}

static void run_loops(Alo *self, uint32_t n_samples) {
  const float *const input_l = self->ports.input_l;
  const float *const input_r = self->ports.input_r;
  float *const output_l = self->ports.output_l;
  float *const output_r = self->ports.output_r;
  self->threshold = dbToFloat(*self->ports.threshold);

  self->loopmix = fmin(1.0, *self->ports.mix / 50);
  self->inmix = fmin(1, (100 - *self->ports.mix) / 50);

  // Align loop phase to host transport (if available).
  if (self->transport_loop_index_pending) {
    self->loop_index = self->loop_start + (self->transport_loop_index % (self->loop_samples ? self->loop_samples : 1u));
    self->transport_loop_index_pending = false;
  }

  for (uint32_t pos = 0; pos < n_samples; ++pos) {
    float sample_l = input_l[pos];
    float sample_r = input_r[pos];

    // Continuously record into the tape ring buffer.
    tape_write(self, sample_l, sample_r);
    output_l[pos] = self->inmix * sample_l;
    output_r[pos] = self->inmix * sample_r;

    // Playback: sum all active layers across tracks
    for (uint32_t t = 0; t < NUM_TRACKS; ++t) {
      const uint32_t n_layers = self->layer_count[t];
      for (uint32_t l = 0; l < n_layers; ++l) {
        float *const layer = self->layers[t][l];
        if (!layer) {
          continue;
        }
        output_l[pos] += layer[self->loop_index];
        output_r[pos] += layer[self->loop_index + LOOP_SIZE];
      }
    }

    self->loop_index++;
    if (self->loop_index >= self->loop_start + self->loop_samples) {
      self->loop_index = self->loop_start;

      // Loop boundary reached (quantization point)
      for (int t = 0; t < NUM_TRACKS; ++t) {
        // Auto-stop base recording: arm at first boundary, commit at next.
        if (self->base_arm[t]) {
          self->base_arm[t] = false;
          self->base_recording[t] = true;
          log("[[ Track %d: base recording START @ boundary ]]", t + 1);
        } else if (self->base_recording[t]) {
          self->base_recording[t] = false;
          self->capture_on[t] = false;
          self->commit_pending[t] = true;
          log("[[ Track %d: base recording AUTO-STOP -> commit ]]", t + 1);
        }

        // Commit "last N bars" (base or overdub)
        if (self->commit_pending[t]) {
          self->commit_pending[t] = false;

          if (self->layer_count[t] == 0) {
            commit_from_tape(self, t, 0);
            self->layer_count[t] = 1;
            log("[[ Track %d: COMMIT base (layers=%d) ]]", t + 1, self->layer_count[t]);
          } else if (self->layer_count[t] < (uint32_t)NUM_LAYERS) {
            const int li = (int)self->layer_count[t];
            commit_from_tape(self, t, li);
            self->layer_count[t]++;
            log("[[ Track %d: COMMIT overdub (layers=%d) ]]", t + 1, self->layer_count[t]);
          } else {
            log("[[ Track %d: COMMIT ignored (max layers reached) ]]", t + 1);
          }
        }
      }
    }
  }

  update_loop_state_ports(self);
}

/**
   The `run()` method is the main process function of the plugin.  It processes
   a block of audio in the audio context.  Since this plugin is
   `lv2:hardRTCapable`, `run()` must be real-time safe, so blocking (e.g. with
   a mutex) or memory allocation are not allowed.
*/
static void run(LV2_Handle instance, uint32_t n_samples) {
  Alo *self = (Alo *)instance;

  // Handle controls first so loop commits/undo can occur on the correct boundary.
  run_events(self);
  run_loops(self, n_samples);
  run_clicks(self, n_samples);

  if (!*(self->ports.enabled)) {
    reset(self);
  }
}

/**
   The `deactivate()` method is the counterpart to `activate()`, and is called
   by the host after running the plugin.  It indicates that the host will not
   call `run()` again until another call to `activate()` and is mainly useful
   for more advanced plugins with ``live'' characteristics such as those with
   auxiliary processing threads.	As with `activate()`, this plugin has no
   use for this information so this method does nothing.

   This method is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/

static void deactivate(LV2_Handle instance) { log("Deactivate"); }

/**
   Destroy a plugin instance (counterpart to `instantiate()`).

   This method is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/
static void cleanup(LV2_Handle instance) {
  log("Cleanup");

  Alo *self = (Alo *)instance;

  for (int t = 0; t < NUM_TRACKS; ++t) {
    for (int l = 0; l < NUM_LAYERS; ++l) {
      free(self->layers[t][l]);
    }
  }
  free(self->low_beat);
  free(self->high_beat);
  free(self->preroll);
  free(self);
}

/**
   The `extension_data()` function returns any extension data supported by the
   plugin.  Note that this is not an instance method, but a function on the
   plugin descriptor.  It is usually used by plugins to implement additional
   interfaces.	This plugin does not have any extension data, so this function
   returns NULL.

   This method is in the ``discovery'' threading class, so no other functions
   or methods in this plugin library will be called concurrently with it.
*/
static const void *extension_data(const char *uri) { return NULL; }

/**
   Every plugin must define an `LV2_Descriptor`.  It is best to define
   descriptors statically to avoid leaking memory and non-portable shared
   library constructors and destructors to clean up properly.
*/
static const LV2_Descriptor descriptor = {ALO_URI,  instantiate,   connect_port,
                                          activate, run,           deactivate,
                                          cleanup,  extension_data};

/**
   The `lv2_descriptor()` function is the entry point to the plugin library. The
   host will load the library and call this function repeatedly with increasing
   indices to find all the plugins defined in the library.  The index is not an
   indentifier, the URI of the returned descriptor is used to determine the
   identify of the plugin.

   This method is in the ``discovery'' threading class, so no other functions
   or methods in this plugin library will be called concurrently with it.
*/
LV2_SYMBOL_EXPORT
const LV2_Descriptor *lv2_descriptor(uint32_t index) {
  switch (index) {
  case 0:
    return &descriptor;
  default:
    return NULL;
  }
}
