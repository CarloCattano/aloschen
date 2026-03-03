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

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alo_engine.h"
#include "lv2/time/time.h"
#include "lv2/urid/urid.h"

/* ------------------------------------------------------------------------
 * Logging
 * ------------------------------------------------------------------------ */

static bool log_enabled(void) {
  const char* v = getenv("ALO_LOG");
  return v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T');
}

void alo_log(const char *message, ...) {
  if (!log_enabled()) {
    return;
  }

  FILE *f = fopen("/tmp/alo.log", "a");
  if (!f) {
    return;
  }

  char buffer[2048];
  va_list argumentList;
  va_start(argumentList, message);
  vsnprintf(buffer, sizeof(buffer), message, argumentList);
  va_end(argumentList);

  fwrite(buffer, 1, strlen(buffer), f);
  fputc('\n', f);
  fclose(f);
}

/* ------------------------------------------------------------------------
 * Click waveform generation
 * ------------------------------------------------------------------------ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void sine_pulse(float *target, double frequency, double sample_rate,
                       uint32_t num_samples) {
  const uint32_t half_length = (uint32_t)(num_samples * 0.5f);
  const float amplitude_step = 1.0f / (float)half_length;
  const double sample_sin_step = 2.0 * M_PI * frequency / sample_rate;
  float amplitude = 0.0f;

  for (uint32_t i = 0; i < half_length; ++i) {
    amplitude = fminf(amplitude + amplitude_step, 1.0f);
    target[i] = 0.5f * amplitude * (float)sin(i * sample_sin_step);
  }

  for (uint32_t i = half_length; i < num_samples; ++i) {
    amplitude = fmaxf(amplitude - amplitude_step, 0.0f);
    target[i] = 0.5f * amplitude * (float)sin(i * sample_sin_step);
  }
}

/* ------------------------------------------------------------------------
 * LV2 instantiate
 * ------------------------------------------------------------------------ */

static LV2_Handle instantiate(const LV2_Descriptor *descriptor,
                              double rate,
                              const char *bundle_path,
                              const LV2_Feature *const *features) {
  (void)descriptor;
  (void)bundle_path;

  alo_log("Instantiate");

  Alo *self = (Alo *)calloc(1, sizeof(Alo));
  if (!self) {
    return NULL;
  }

  self->rate = rate;
  self->bpb = DEFAULT_BEATS_PER_BAR;
  self->loop_beats = DEFAULT_BEATS_PER_BAR * DEFAULT_NUM_BARS;
  self->bpm = DEFAULT_BPM;
  self->loop_samples =
      (uint32_t)(self->loop_beats * self->rate * 60.0f / self->bpm);
  self->current_position = 0.0f;

  self->have_transport = false;
  self->transport_loop_index = 0;
  self->transport_loop_index_pending = false;
  self->last_bar_beat = 0.0f;
  self->have_last_bar_beat = false;
  self->bar_counter_fallback = 0;
  self->last_transport_beats = 0.0;
  self->have_last_transport_beats = false;

  self->midi_control = false;

  for (int t = 0; t < NUM_TRACKS; ++t) {
    self->loop_buf[t] = (float *)calloc(LOOP_SIZE * 2, sizeof(float));
    if (!self->loop_buf[t]) {
      fprintf(stderr, "ALO: loop buffer allocation failed\n");
      goto fail;
    }

    for (int l = 0; l < ALO_MAX_UNDO_LAYERS; ++l) {
      self->od_buf[t][l] = (float *)calloc(LOOP_SIZE * 2, sizeof(float));
      if (!self->od_buf[t][l]) {
        fprintf(stderr, "ALO: overdub buffer allocation failed\n");
        goto fail;
      }
    }

    self->have_loop[t] = false;
    self->od_count[t] = 0;
    self->rec_od_layer[t] = 0;
    self->pending_undo[t] = 0;
    self->pending_clear_all[t] = false;
    self->track_state[t] = TRACK_IDLE;
    self->rec_remaining_samples[t] = 0;
    self->last_loop_input[t] = false;
    self->last_undo_input[t] = false;
    self->loop_btn_high_frames[t] = 0;
  }
  self->loop_start = 0;
  self->loop_index = 0;

  for (int t = 0; t < NUM_TRACKS; ++t) {
    self->ports.loop_state_out[t] = NULL;
  }
  self->ports.bar_step_out = NULL;

  LV2_URID_Map *map = NULL;
  for (int i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID_URI "#map")) {
      map = (LV2_URID_Map *)features[i]->data;
    }
  }

  if (!map) {
    fprintf(stderr, "Host does not support urid:map.\n");
    goto fail;
  }

  self->map = map;

  AloURIs *const uris = &self->uris;
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
  uris->time_beatsPerMinute =
      map->map(map->handle, LV2_TIME__beatsPerMinute);
  uris->time_speed = map->map(map->handle, LV2_TIME__speed);
  uris->time_beatsPerBar =
      map->map(map->handle, LV2_TIME__beatsPerBar);
  uris->midi_MidiEvent = map->map(map->handle, LV2_MIDI__MidiEvent);

  /* Generate pulses for the metronome */
  self->beat_len = (uint32_t)(0.02f * self->rate);
  self->high_beat = (float *)malloc(self->beat_len * sizeof(float));
  self->low_beat = (float *)malloc(self->beat_len * sizeof(float));
  self->start_beat = (float *)malloc(self->beat_len * sizeof(float));

  if (!self->high_beat || !self->low_beat || !self->start_beat) {
    fprintf(stderr, "ALO: click allocation failed\n");
    goto fail;
  }

  sine_pulse(self->high_beat, HIGH_BEAT_FREQ, self->rate, self->beat_len);
  sine_pulse(self->low_beat, LOW_BEAT_FREQ, self->rate, self->beat_len);
  sine_pulse(self->start_beat, START_BEAT_FREQ, self->rate, self->beat_len);
  self->high_beat_offset = self->beat_len;
  self->low_beat_offset = self->beat_len;
  self->start_beat_offset = self->beat_len;

  return (LV2_Handle)self;

fail:
  if (self) {
    for (int t = 0; t < NUM_TRACKS; ++t) {
      free(self->loop_buf[t]);
      for (int l = 0; l < ALO_MAX_UNDO_LAYERS; ++l) {
        free(self->od_buf[t][l]);
      }
    }
    free(self->high_beat);
    free(self->low_beat);
    free(self->start_beat);
    free(self);
  }
  return NULL;
}

/* ------------------------------------------------------------------------
 * LV2 connect_port
 * ------------------------------------------------------------------------ */

static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
  alo_log("Connect");
  Alo *self = (Alo *)instance;

  switch ((PortIndex)port) {
  case ALO_INPUT_L:
    self->ports.input_l = (const float *)data;
    alo_log("Connect ALO_INPUT_L %u", port);
    break;
  case ALO_INPUT_R:
    self->ports.input_r = (const float *)data;
    alo_log("Connect ALO_INPUT_R %u", port);
    break;
  case ALO_OUTPUT_L:
    self->ports.output_l = (float *)data;
    alo_log("Connect ALO_OUTPUT_L %u", port);
    break;
  case ALO_OUTPUT_R:
    self->ports.output_r = (float *)data;
    alo_log("Connect ALO_OUTPUT_R %u", port);
    break;

  case ALO_BARS:
    self->ports.bars = (float *)data;
    alo_log("Connect ALO_BARS %u", port);
    break;

  case ALO_CONTROL:
    self->ports.control = (LV2_Atom_Sequence *)data;
    alo_log("Connect ALO_CONTROL %u", port);
    break;

  case ALO_MIDIIN:
    self->ports.midiin = (LV2_Atom_Sequence *)data;
    alo_log("Connect ALO_MIDIIN %u", port);
    break;

  case ALO_MIDI_BASE:
    self->ports.midi_base = (float *)data;
    alo_log("Connect ALO_MIDI_BASE %u", port);
    break;

  case ALO_CLICK:
    self->ports.click = (float *)data;
    alo_log("Connect ALO_CLICK %u", port);
    break;

  case ALO_MIX:
    self->ports.mix = (float *)data;
    alo_log("Connect ALO_MIX %u", port);
    break;

  case ALO_ENABLED:
    self->ports.enabled = (int *)data;
    alo_log("Connect ALO_ENABLED %u", port);
    break;

  case ALO_LOOP1:
    self->ports.loop_btn[0] = (float *)data;
    alo_log("Connect ALO_LOOP1 %u", port);
    break;
  case ALO_UNDO1:
    self->ports.undo_btn[0] = (float *)data;
    alo_log("Connect ALO_UNDO1 %u", port);
    break;
  case ALO_LOOP2:
    self->ports.loop_btn[1] = (float *)data;
    alo_log("Connect ALO_LOOP2 %u", port);
    break;
  case ALO_UNDO2:
    self->ports.undo_btn[1] = (float *)data;
    alo_log("Connect ALO_UNDO2 %u", port);
    break;
  case ALO_LOOP3:
    self->ports.loop_btn[2] = (float *)data;
    alo_log("Connect ALO_LOOP3 %u", port);
    break;
  case ALO_UNDO3:
    self->ports.undo_btn[2] = (float *)data;
    alo_log("Connect ALO_UNDO3 %u", port);
    break;

  case ALO_LOOP1_STATE:
    self->ports.loop_state_out[0] = (float *)data;
    alo_log("Connect ALO_LOOP1_STATE %u", port);
    break;
  case ALO_LOOP2_STATE:
    self->ports.loop_state_out[1] = (float *)data;
    alo_log("Connect ALO_LOOP2_STATE %u", port);
    break;
  case ALO_LOOP3_STATE:
    self->ports.loop_state_out[2] = (float *)data;
    alo_log("Connect ALO_LOOP3_STATE %u", port);
    break;

  case ALO_BAR_STEP:
    self->ports.bar_step_out = (float *)data;
    alo_log("Connect ALO_BAR_STEP %u", port);
    break;

  default:
    alo_log("Connect unknown port %u", port);
    break;
  }

  alo_log("Connect end");
}

/* ------------------------------------------------------------------------
 * LV2 activate/deactivate
 * ------------------------------------------------------------------------ */

static void activate(LV2_Handle instance) {
  (void)instance;
  alo_log("Activate");
}

static void deactivate(LV2_Handle instance) {
  (void)instance;
  alo_log("Deactivate");
}

/* ------------------------------------------------------------------------
 * LV2 run
 * ------------------------------------------------------------------------ */

static void run(LV2_Handle instance, uint32_t n_samples) {
  Alo *self = (Alo *)instance;

  /* Handle control and MIDI events first so we quantize to the right boundary. */
  run_events(self);

  /* Process audio loops (record + playback). */
  run_loops(self, n_samples);

  /* Click/metronome. */
  run_clicks(self, n_samples);

  /* If plugin is disabled, reset engine state. */
  if (self->ports.enabled && !*(self->ports.enabled)) {
    reset(self);
  }
}

/* ------------------------------------------------------------------------
 * LV2 cleanup
 * ------------------------------------------------------------------------ */

static void cleanup(LV2_Handle instance) {
  alo_log("Cleanup");

  Alo *self = (Alo *)instance;

  for (int t = 0; t < NUM_TRACKS; ++t) {
    free(self->loop_buf[t]);
    for (int l = 0; l < ALO_MAX_UNDO_LAYERS; ++l) {
      free(self->od_buf[t][l]);
    }
  }

  free(self->low_beat);
  free(self->high_beat);
  free(self->start_beat);
  free(self);
}

/* ------------------------------------------------------------------------
 * LV2 extension_data
 * ------------------------------------------------------------------------ */

static const void *extension_data(const char *uri) {
  (void)uri;
  return NULL;
}

/* ------------------------------------------------------------------------
 * LV2 descriptor
 * ------------------------------------------------------------------------ */

static const LV2_Descriptor descriptor = {
  ALO_URI,      /* URI */
  instantiate,  /* instantiate */
  connect_port, /* connect_port */
  activate,     /* activate */
  run,          /* run */
  deactivate,   /* deactivate */
  cleanup,      /* cleanup */
  extension_data /* extension_data */
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor *lv2_descriptor(uint32_t index) {
  switch (index) {
  case 0:
    return &descriptor;
  default:
    return NULL;
  }
}