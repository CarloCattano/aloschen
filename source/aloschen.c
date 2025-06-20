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
#include <sys/time.h>
#include <time.h>

#include "lv2/atom/atom.h"
#include "lv2/atom/util.h"
#include "lv2/time/time.h"
#include "lv2/urid/urid.h"
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>

#define ALO_URI "http://ktano-studio.com/aloschen"

typedef struct {
  LV2_URID atom_Blank;
  LV2_URID atom_Float;
  LV2_URID atom_Object;
  LV2_URID midi_MidiEvent;
  LV2_URID atom_Path;
  LV2_URID atom_Resource;
  LV2_URID atom_Sequence;
  LV2_URID time_Position;
  LV2_URID time_barBeat;
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
  ALO_LOOP3 = 6,
  ALO_LOOP4 = 7,
  ALO_LOOP5 = 8,
  ALO_LOOP6 = 9,
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
} PortIndex;

typedef enum {
  // NB: for all states, we are always recording in the background
  STATE_LOOP_OFF, // the loop is not playing
  STATE_LOOP_ON,  // the loop is playing
  STATE_RECORDING // no loop is set, we are only recording
} State;

typedef enum {
  STATE_OFF,    // No click
  STATE_ATTACK, // Envelope rising
  STATE_DECAY,  // Envelope lowering
  STATE_SILENT  // Silent
} ClickState;

static const size_t LOOP_SIZE = 2880000;
static const int NUM_LOOPS = 6;
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
    float *loops[NUM_LOOPS];
    float *bars;
    float *threshold;
    float *midi_base; // start note for midi control of loops
    float *pb_loops;  // number of loops in instant mode
    float *click;     // click volume
    float *mix;
    float *reset_mode;
    int *enabled;
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

  int32_t current_loop;

  uint32_t pb_loops; // number of loops in instant mode

  State state[NUM_LOOPS]; // we're recording, playing or not playing

  bool button_state[NUM_LOOPS];
  bool midi_control;
  uint32_t button_time[NUM_LOOPS]; // last time button was pressed

  float *loops[NUM_LOOPS];          // pointers to memory for playing loops
  uint32_t phrase_start[NUM_LOOPS]; // index into recording/loop
  float *recording;    // pointer to memory for recording - for all loops
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
  self->pb_loops = DEFAULT_INSTANT_LOOPS;

  self->current_loop = 0;

  self->midi_control = false;

  self->recording = (float *)calloc(LOOP_SIZE * 2, sizeof(float));

  for (int i = 0; i < NUM_LOOPS; i++) {
    self->loops[i] = (float *)calloc(LOOP_SIZE * 2, sizeof(float));
    self->phrase_start[i] = 0;
    self->state[i] = STATE_RECORDING;
  }
  self->loop_start = 0;
  self->loop_index = 0;
  self->threshold = 0.0;

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
  uris->atom_Object = map->map(map->handle, LV2_ATOM__Object);
  uris->atom_Path = map->map(map->handle, LV2_ATOM__Path);
  uris->atom_Resource = map->map(map->handle, LV2_ATOM__Resource);
  uris->atom_Sequence = map->map(map->handle, LV2_ATOM__Sequence);
  uris->time_Position = map->map(map->handle, LV2_TIME__Position);
  uris->time_barBeat = map->map(map->handle, LV2_TIME__barBeat);
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
  default:
    int loop = port - 4;
    self->ports.loops[loop] = (float *)data;
    log("Connect ALO_LOOP %d", loop);
  }
  log("Connect end");
}

static void reset(Alo *self) {
  log("Reset");
  self->pb_loops = (uint32_t)floorf(*(self->ports.pb_loops));
  self->loop_beats =
      (uint32_t)floorf(self->bpb) * (uint32_t)floorf(*(self->ports.bars));
  self->loop_samples = self->loop_beats * self->rate * 60.0f / self->bpm;

  if (self->loop_samples > LOOP_SIZE || self->speed == 0) {
    self->loop_samples = LOOP_SIZE;
  }
  self->loop_index = 0;
  self->loop_start = 0;
  log("Loop beats: %d", self->loop_beats);
  log("BPM: %G", self->bpm);
  log("Loop_samples: %d", self->loop_samples);
  for (int i = 0; i < NUM_LOOPS; i++) {
    self->button_state[i] = (*self->ports.loops[i]) > 0.0f ? true : false;
    self->state[i] = STATE_RECORDING;
    self->phrase_start[i] = 0;
    log("STATE: RECORDING (reset) [%d]", i);
  }
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
  LV2_Atom *beat = NULL, *bpm = NULL, *bpb = NULL, *speed = NULL;
  lv2_atom_object_get(obj, uris->time_barBeat, &beat, uris->time_beatsPerMinute,
                      &bpm, uris->time_speed, &speed, uris->time_beatsPerBar,
                      &bpb, NULL);

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
  if (beat && beat->type == uris->atom_Float) {
    // Received a beat position, synchronise
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
  }
}

/**
   WIP - button 0 records/overdubs up to NUM_LOOPS , button 1 undo/clears
*/

static void button_logic(LV2_Handle instance, bool btn_state, int i) {
  Alo *self = (Alo *)instance;

  struct timeval te;
  gettimeofday(&te, NULL);
  long long milliseconds = te.tv_sec * 1000LL + te.tv_usec / 1000;

  static bool last_record_state = false;
  static bool last_stop_state = false;

  const int stop_button_index = 1;
  const int record_button_index = 0;

  int difference = milliseconds - self->button_time[stop_button_index];

  // --- Record button logic ---
  if (i == record_button_index) {
    if (btn_state && !last_record_state) {
      self->button_state[self->current_loop] = true;
      self->button_time[self->current_loop] = milliseconds;
      last_record_state = true;
      log("[[ Recording into %d ]]", self->current_loop);
    } else if (!btn_state && last_record_state) {
      self->state[self->current_loop] = STATE_LOOP_ON;
      self->current_loop++;
      last_record_state = false;
      log(" -->>  Moving to loop %d -----", self->current_loop);
    }
  }

  // --- Stop/Undo button logic ---
  if (i == stop_button_index) {
    if (btn_state && !last_stop_state) {
      last_stop_state = true;

      // Only undo if not already at loop 0
      if (self->current_loop > 0) {
        self->phrase_start[self->current_loop] = self->loop_index;
        self->button_state[self->current_loop] = false;
        self->state[self->current_loop] = STATE_RECORDING;
        log("[[   UNDOING LOOP %d   ]]", self->current_loop);
        self->current_loop--;
      } else {
        self->current_loop = 0;
        self->phrase_start[0] = self->loop_index;
        self->button_state[0] = false;
        self->state[0] = STATE_RECORDING;
        log("Loop 0 rearmed for recording");
      }

      self->button_time[stop_button_index] = milliseconds;
    } else if (!btn_state && last_stop_state) {
      last_stop_state = false;

      // Only allow reset if button was released quickly after press
      if (difference < 500 && self->current_loop == 0) {
        reset(self);
        log("<<< RESET triggered >>>");
      }
    }
  }

  // --- Loop bounds enforcement ---
  if (self->current_loop < 0) {
    self->current_loop = 0;
  } else if (self->current_loop >= NUM_LOOPS) {
    self->current_loop = NUM_LOOPS - 1;
  }

  // --- Free running mode ---
  if (self->loop_samples == LOOP_SIZE) {
    for (int j = 0; j < NUM_LOOPS; j++) {
      if (self->phrase_start[j] != 0) {
        self->loop_samples =
            LOOP_SIZE + self->loop_index - self->phrase_start[j];
        self->loop_samples = self->loop_samples % LOOP_SIZE;
        self->loop_start = self->phrase_start[j];
      }
    }
  }
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

  for (uint32_t i = 0; i < NUM_LOOPS; i++) {
    if (self->state[i] == STATE_LOOP_ON) {
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
  const LV2_Atom_Sequence *midiin = self->ports.midiin;

  for (const LV2_Atom_Event *ev = lv2_atom_sequence_begin(&midiin->body);
       !lv2_atom_sequence_is_end(&midiin->body, midiin->atom.size, ev);

       ev = lv2_atom_sequence_next(ev)) {

    if (ev->body.type == self->uris.midi_MidiEvent) {
      const uint8_t *const msg = (const uint8_t *)(ev + 1);
      int i = msg[1] - (uint32_t)floorf(*(self->ports.midi_base));
      if (i >= 0 && i < NUM_LOOPS) {
        if (lv2_midi_message_type(msg) == LV2_MIDI_MSG_NOTE_ON) {
          button_logic(self, true, i);
        }
        if (lv2_midi_message_type(msg) == LV2_MIDI_MSG_NOTE_OFF) {
          button_logic(self, false, i);
        }
        self->midi_control = true;
      }
    }
  }

  if (self->midi_control == false) {
    for (int i = 0; i < NUM_LOOPS; i++) {
      bool new_button_state = (*self->ports.loops[i]) > 0.0f ? true : false;
      button_logic(self, new_button_state, i);
    }
  }

  const AloURIs *uris = &self->uris;

  // from metro.c
  // Work forwards in time frame by frame, handling events as we go
  const LV2_Atom_Sequence *in = self->ports.control;

  for (const LV2_Atom_Event *ev = lv2_atom_sequence_begin(&in->body);
       !lv2_atom_sequence_is_end(&in->body, in->atom.size, ev);

       ev = lv2_atom_sequence_next(ev)) {

    // Check if this event is an Object
    // (or deprecated Blank to tolerate old hosts)
    if (ev->body.type == uris->atom_Object ||
        ev->body.type == uris->atom_Blank) {
      const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
      if (obj->body.otype == uris->time_Position) {
        // Received position information, update
        update_position(self, obj);
      }
    }
  }
}

static void run_loops(Alo *self, uint32_t n_samples) {
  const float *const input_l = self->ports.input_l;
  const float *const input_r = self->ports.input_r;
  float *const output_l = self->ports.output_l;
  float *const output_r = self->ports.output_r;
  float *const recording = self->recording;
  self->threshold = dbToFloat(*self->ports.threshold);

  self->loopmix = fmin(1.0, *self->ports.mix / 50);
  self->inmix = fmin(1, (100 - *self->ports.mix) / 50);

  for (uint32_t pos = 0; pos < n_samples; ++pos) {
    float sample_l = input_l[pos];
    float sample_r = input_r[pos];
    output_l[pos] = self->inmix * sample_l;
    output_r[pos] = self->inmix * sample_r;
    recording[self->loop_index] = sample_l;
    recording[self->loop_index + LOOP_SIZE] = sample_r;

    for (uint32_t i = 0; i < NUM_LOOPS; ++i) {
      float *const loop = self->loops[i];
      if (self->state[i] == STATE_LOOP_ON) {
        output_l[pos] += loop[self->loop_index];
        output_r[pos] += loop[self->loop_index + LOOP_SIZE];
      }
      if (self->state[i] == STATE_RECORDING) {
        loop[self->loop_index] = self->loopmix * sample_l;
        loop[self->loop_index + LOOP_SIZE] = self->loopmix * sample_r;
        if (self->phrase_start[i] == 0 && (fabs(sample_l) > self->threshold ||
                                           fabs(sample_r) > self->threshold)) {
          self->phrase_start[i] = self->loop_index;
          log("[Looper %d] DETECTED PHRASE START [%d]", i, self->loop_index);
        }
      }
    }

    self->loop_index++;
    if (self->loop_index >= self->loop_start + self->loop_samples) {
      self->loop_index = self->loop_start;
    }
  }
}

/**
   The `run()` method is the main process function of the plugin.  It processes
   a block of audio in the audio context.  Since this plugin is
   `lv2:hardRTCapable`, `run()` must be real-time safe, so blocking (e.g. with
   a mutex) or memory allocation are not allowed.
*/
static void run(LV2_Handle instance, uint32_t n_samples) {
  Alo *self = (Alo *)instance;

  run_loops(self, n_samples);
  run_clicks(self, n_samples);
  run_events(self);

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

  for (int i = 0; i < NUM_LOOPS; i++) {
    free(self->loops[i]);
  }
  free(self->low_beat);
  free(self->high_beat);
  free(self->recording);
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
