/*
  Copyright 2006-2012 David Robillard <d@drobilla.net>
  Copyright 2006 Steve Harris <steve@plugin.org.uk>
  Copyright 2018 Stevie <modplugins@radig.com>
  Copyright 2018 Paul Sherwood <devcurmudgeon@gmail.com>
  Copyright 2025-2026 Carlo Cattano <contact@ktano-studio.com>

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

/* Main LV2 plugin implementation and audio callback glue. */

/* cppcheck-suppress missingIncludeSystem */
#include <math.h>
/* cppcheck-suppress missingIncludeSystem */
#include <stdarg.h>
/* cppcheck-suppress missingIncludeSystem */
#include <stdio.h>
/* cppcheck-suppress missingIncludeSystem */
#include <stdlib.h>
/* cppcheck-suppress missingIncludeSystem */
#include <string.h>

#include "alo_engine.h"
#include "lv2/buf-size/buf-size.h"
#include "lv2/options/options.h"
#include "lv2/time/time.h"
#include "lv2/urid/urid.h"

/* Logging */

/* check environment variable to see if logging is enabled */
static bool log_enabled(void)
{
  const char* v = getenv("ALO_LOG");
  return v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T');
}

/* append formatted message to log file if enabled */
void alo_log(const char* message, ...)
{
  if (!log_enabled()) {
    return;
  }

  FILE* f = fopen("/tmp/alo.log", "a");
  if (!f) {
    return;
  }

  char    buffer[2048];
  va_list argumentList;
  va_start(argumentList, message);
  vsnprintf(buffer, sizeof(buffer), message, argumentList);
  va_end(argumentList);

  fwrite(buffer, 1, strlen(buffer), f);
  fputc('\n', f);
  fclose(f);
}

/* Click waveform generation */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* generate sine-based click pulse into target */
static void sine_pulse(float* target, double frequency, double sample_rate, uint32_t num_samples)
{
  const uint32_t half_length     = (uint32_t)(num_samples * 0.5f);
  const float    amplitude_step  = 1.0f / (float)half_length;
  const double   sample_sin_step = 2.0 * M_PI * frequency / sample_rate;
  float          amplitude       = 0.0f;

  for (uint32_t i = 0; i < half_length; ++i) {
    amplitude = fminf(amplitude + amplitude_step, 1.0f);
    target[i] = 0.5f * amplitude * (float)sin(i * sample_sin_step);
  }

  for (uint32_t i = half_length; i < num_samples; ++i) {
    amplitude = fmaxf(amplitude - amplitude_step, 0.0f);
    target[i] = 0.5f * amplitude * (float)sin(i * sample_sin_step);
  }
}

/* free all resources associated with an Alo instance */
static void free_instance(Alo* self)
{
  if (!self) {
    return;
  }

  for (int t = 0; t < NUM_TRACKS; ++t) {
    free(self->loop_buf[t]);
    for (int l = 0; l < ALO_MAX_UNDO_LAYERS; ++l) {
      free(self->od_buf[t][l]);
    }
  }

  free(self->high_beat);
  free(self->low_beat);
  free(self->start_beat);
  free(self->sampler_src_buf);
  free(self->sampler_src_buf_shadow);
  /* release slice sampler buffers as well */
  alo_slice_sampler_free_buffers(&self->slice_sampler);
  free(self->rt_play_l);
  free(self->rt_play_r);
  free(self->rt_slice_l);
  free(self->rt_slice_r);
  free(self);
}

/* allocate loop/overdub and scratch buffers for a new instance */
static bool alloc_track_buffers(Alo* self)
{
  if (!self) {
    return false;
  }

  self->sampler_src_buf = (float*)calloc(LOOP_SIZE * 2, sizeof(float));
  if (!self->sampler_src_buf) {
    fprintf(stderr, "ALO: sampler source buffer allocation failed\n");
    return false;
  }
  self->sampler_src_buf_shadow = (float*)calloc(LOOP_SIZE * 2, sizeof(float));
  if (!self->sampler_src_buf_shadow) {
    fprintf(stderr, "ALO: sampler shadow buffer allocation failed\n");
    return false;
  }

  if (self->rt_block_cap == 0u) {
    self->rt_block_cap = ALO_RT_BLOCK_CAP;
  }
  self->rt_play_l  = (float*)calloc((size_t)self->rt_block_cap, sizeof(float));
  self->rt_play_r  = (float*)calloc((size_t)self->rt_block_cap, sizeof(float));
  self->rt_slice_l = (float*)calloc((size_t)self->rt_block_cap, sizeof(float));
  self->rt_slice_r = (float*)calloc((size_t)self->rt_block_cap, sizeof(float));
  if (!self->rt_play_l || !self->rt_play_r || !self->rt_slice_l || !self->rt_slice_r) {
    fprintf(stderr, "ALO: RT scratch buffer allocation failed\n");
    return false;
  }

  for (int t = 0; t < NUM_TRACKS; ++t) {
    self->loop_buf[t] = (float*)calloc(LOOP_SIZE * 2, sizeof(float));
    if (!self->loop_buf[t]) {
      fprintf(stderr, "ALO: loop buffer allocation failed\n");
      return false;
    }

    for (int l = 0; l < ALO_MAX_UNDO_LAYERS; ++l) {
      self->od_buf[t][l] = (float*)calloc(LOOP_SIZE * 2, sizeof(float));
      if (!self->od_buf[t][l]) {
        fprintf(stderr, "ALO: overdub buffer allocation failed\n");
        return false;
      }
    }
  }

  return true;
}

/* LV2 instantiate */

/* LV2 instantiate callback; allocate and initialize engine state */
static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate,
                              const char* bundle_path, const LV2_Feature* const* features)
{
  (void)descriptor;
  (void)bundle_path;

  alo_log("Instantiate");

  Alo* self = (Alo*)calloc(1, sizeof(Alo));
  if (!self) {
    return NULL;
  }

  self->rate                     = rate;
  self->slice_sampler.rate       = (float)rate;
  self->sampler_src_shadow_valid = false;

  /* Preallocate per-slice audio buffers so the sampler can copy data
   * without any heap allocation in the audio thread. LOOP_SIZE is the
   * worst-case slice length (one full loop) and we always use stereo.
   */
  if (!alo_slice_sampler_alloc_buffers(&self->slice_sampler, LOOP_SIZE, 2)) {
    fprintf(stderr, "ALO: slice sampler buffer allocation failed\n");
    goto fail;
  }

  self->bpb          = DEFAULT_BEATS_PER_BAR;
  self->loop_beats   = DEFAULT_BEATS_PER_BAR * DEFAULT_NUM_BARS;
  self->bpm          = DEFAULT_BPM;
  self->loop_samples = (uint32_t)(self->loop_beats * self->rate * 60.0f / self->bpm);

  self->have_last_enabled = false;
  self->last_enabled      = true;

  LV2_URID_Map* map = NULL;
  for (int i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID_URI "#map")) {
      map = (LV2_URID_Map*)features[i]->data;
    }
  }

  if (!map) {
    fprintf(stderr, "Host does not support urid:map.\n");
    goto fail;
  }

  self->map = map;

  AloURIs* const uris       = &self->uris;
  uris->atom_Blank          = map->map(map->handle, LV2_ATOM__Blank);
  uris->atom_Float          = map->map(map->handle, LV2_ATOM__Float);
  uris->atom_Int            = map->map(map->handle, LV2_ATOM__Int);
  uris->atom_Long           = map->map(map->handle, LV2_ATOM__Long);
  uris->atom_Object         = map->map(map->handle, LV2_ATOM__Object);
  uris->atom_Path           = map->map(map->handle, LV2_ATOM__Path);
  uris->atom_Resource       = map->map(map->handle, LV2_ATOM__Resource);
  uris->atom_Sequence       = map->map(map->handle, LV2_ATOM__Sequence);
  uris->time_Position       = map->map(map->handle, LV2_TIME__Position);
  uris->time_beat           = map->map(map->handle, LV2_TIME__beat);
  uris->time_barBeat        = map->map(map->handle, LV2_TIME__barBeat);
  uris->time_bar            = map->map(map->handle, LV2_TIME__bar);
  uris->time_beatsPerMinute = map->map(map->handle, LV2_TIME__beatsPerMinute);
  uris->time_speed          = map->map(map->handle, LV2_TIME__speed);
  uris->time_beatsPerBar    = map->map(map->handle, LV2_TIME__beatsPerBar);
  uris->midi_MidiEvent      = map->map(map->handle, LV2_MIDI__MidiEvent);

  uris->bufsz_maxBlockLength     = map->map(map->handle, LV2_BUF_SIZE__maxBlockLength);
  uris->bufsz_nominalBlockLength = map->map(map->handle, LV2_BUF_SIZE__nominalBlockLength);

  /* Determine RT scratch capacity from LV2 options (instantiate-time only). */
  uint32_t                  opt_nominal = 0u;
  uint32_t                  opt_max     = 0u;
  const LV2_Options_Option* options     = NULL;
  for (int i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_OPTIONS__options)) {
      options = (const LV2_Options_Option*)features[i]->data;
      break;
    }
  }
  if (options) {
    for (const LV2_Options_Option* o = options; o->key; ++o) {
      uint32_t v = 0u;
      if (o->type == uris->atom_Int && o->value) {
        const int32_t vi = *(const int32_t*)o->value;
        if (vi > 0) {
          v = (uint32_t)vi;
        }
      } else if (o->type == uris->atom_Long && o->value) {
        const int64_t vl = *(const int64_t*)o->value;
        if (vl > 0 && vl <= (int64_t)UINT32_MAX) {
          v = (uint32_t)vl;
        }
      }

      if (v == 0u) {
        continue;
      }

      if (o->key == uris->bufsz_nominalBlockLength) {
        opt_nominal = v;
      } else if (o->key == uris->bufsz_maxBlockLength) {
        opt_max = v;
      }
    }
  }

  // TODO: Comment this logic
  uint32_t cap = ALO_RT_BLOCK_CAP;
  if (opt_max != 0u) {
    cap = opt_max;
  } else if (opt_nominal != 0u) {
    cap = opt_nominal;
  }
  if (cap == 0u) {
    cap = ALO_RT_BLOCK_CAP;
  }
  if (cap > ALO_RT_BLOCK_CAP) {
    cap = ALO_RT_BLOCK_CAP;
  }
  self->rt_block_cap = cap;

  if (!alloc_track_buffers(self)) {
    goto fail;
  }

  /* Generate pulses for the metronome */
  self->beat_len   = (uint32_t)(0.02f * self->rate);
  self->high_beat  = (float*)malloc(self->beat_len * sizeof(float));
  self->low_beat   = (float*)malloc(self->beat_len * sizeof(float));
  self->start_beat = (float*)malloc(self->beat_len * sizeof(float));

  if (!self->high_beat || !self->low_beat || !self->start_beat) {
    fprintf(stderr, "ALO: click allocation failed\n");
    goto fail;
  }

  sine_pulse(self->high_beat, HIGH_BEAT_FREQ, self->rate, self->beat_len);
  sine_pulse(self->low_beat, LOW_BEAT_FREQ, self->rate, self->beat_len);
  sine_pulse(self->start_beat, START_BEAT_FREQ, self->rate, self->beat_len);
  self->high_beat_offset  = self->beat_len;
  self->low_beat_offset   = self->beat_len;
  self->start_beat_offset = self->beat_len;

  return (LV2_Handle)self;

fail:
  free_instance(self);
  return NULL;
}

/* ------------------------------------------------------------------------
 * LV2 connect_port
 * ------------------------------------------------------------------------ */

static void connect_port(LV2_Handle instance, uint32_t port, void* data)
{
  Alo* self = (Alo*)instance;

  switch ((PortIndex)port) {
  case ALO_INPUT_L:
    self->ports.input_l = (const float*)data;
    break;
  case ALO_INPUT_R:
    self->ports.input_r = (const float*)data;
    break;
  case ALO_OUTPUT_L:
    self->ports.output_l = (float*)data;
    break;
  case ALO_OUTPUT_R:
    self->ports.output_r = (float*)data;
    break;

  case ALO_BARS:
    self->ports.bars = (float*)data;
    break;

  case ALO_CONTROL:
    self->ports.control = (LV2_Atom_Sequence*)data;
    break;

  case ALO_MIDIIN:
    self->ports.midiin = (LV2_Atom_Sequence*)data;
    break;

  case ALO_SLICE_ROOT:
    self->ports.slice_root = (float*)data;
    break;

  case ALO_CLICK:
    self->ports.click = (float*)data;
    break;

  case ALO_MIX:
    self->ports.mix = (float*)data;
    break;

  case ALO_ENABLED:
    self->ports.enabled = (float*)data;
    break;

  case ALO_LOOP1:
    self->ports.loop_btn[0] = (float*)data;
    break;
  case ALO_UNDO1:
    self->ports.undo_btn[0] = (float*)data;
    break;
  case ALO_LOOP2:
    self->ports.loop_btn[1] = (float*)data;
    break;
  case ALO_UNDO2:
    self->ports.undo_btn[1] = (float*)data;
    break;
  case ALO_LOOP3:
    self->ports.loop_btn[2] = (float*)data;
    break;
  case ALO_UNDO3:
    self->ports.undo_btn[2] = (float*)data;
    break;

  case ALO_LOOP1_VOL:
    self->ports.loop_vol[0] = (float*)data;
    break;
  case ALO_LOOP2_VOL:
    self->ports.loop_vol[1] = (float*)data;
    break;
  case ALO_LOOP3_VOL:
    self->ports.loop_vol[2] = (float*)data;
    break;

  case ALO_SAMPLER_VOL:
    self->ports.sampler_vol = (float*)data;
    break;

  case ALO_SLICES_PER_BAR:
    self->ports.slices_per_bar = (float*)data;
    break;

  case ALO_SPLIT_TRANSIENTS:
    self->ports.split_by_transient = (float*)data;
    break;

  case ALO_DETECTED_SLICES:
    self->ports.detected_slices_out = (float*)data;
    break;

  case ALO_TRANSIENT_THRESH:
    self->ports.transient_threshold = (float*)data;
    break;
  case ALO_SLICE_ENV_FRAC:
    self->ports.slice_env_frac = (float*)data;
    break;
  case ALO_SLICE_ENV_ATTACK:
    /* not exposed in the UI; host may automate if desired */
    self->ports.slice_env_attack = (float*)data;
    break;

  case ALO_LOOP1_STATE:
    self->ports.loop_state_out[0] = (float*)data;
    break;
  case ALO_LOOP2_STATE:
    self->ports.loop_state_out[1] = (float*)data;
    break;
  case ALO_LOOP3_STATE:
    self->ports.loop_state_out[2] = (float*)data;
    break;

  case ALO_UNDO1_STATE:
    self->ports.undo_state_out[0] = (float*)data;
    break;
  case ALO_UNDO2_STATE:
    self->ports.undo_state_out[1] = (float*)data;
    break;
  case ALO_UNDO3_STATE:
    self->ports.undo_state_out[2] = (float*)data;
    break;

  case ALO_LOOP1_HAS_AUDIO:
    self->ports.has_audio_out[0] = (float*)data;
    break;
  case ALO_LOOP2_HAS_AUDIO:
    self->ports.has_audio_out[1] = (float*)data;
    break;
  case ALO_LOOP3_HAS_AUDIO:
    self->ports.has_audio_out[2] = (float*)data;
    break;

  case ALO_BAR_STEP:
    self->ports.bar_step_out = (float*)data;
    break;

  case ALO_CYCLE_PHASE:
    self->ports.cycle_phase_out = (float*)data;
    break;

  case ALO_HOST_BAR_PHASE:
    self->ports.host_bar_phase_out = (float*)data;
    break;

  default:
    break;
  }
}

/* ------------------------------------------------------------------------
 * LV2 activate/deactivate
 * ------------------------------------------------------------------------ */

static void activate(LV2_Handle instance)
{
  Alo* self = (Alo*)instance;
  alo_log("Activate");
  reset(self);
}

static void deactivate(LV2_Handle instance)
{
  (void)instance;
  alo_log("Deactivate");
}

/* ------------------------------------------------------------------------
 * LV2 run
 * ------------------------------------------------------------------------ */

static void run(LV2_Handle instance, uint32_t n_samples)
{
  Alo* self = (Alo*)instance;

  /* lv2:enabled behaves like a bypass: when disabled, stop looper/click
   * processing and reset counters/state.
   */
  bool enabled_now = true;
  if (self->ports.enabled) {
    enabled_now = (*(self->ports.enabled) >= 0.5f);
  }

  if (!self->have_last_enabled) {
    self->have_last_enabled = true;
    self->last_enabled      = enabled_now;
    if (!enabled_now) {
      reset(self);
    }
  } else if (self->last_enabled != enabled_now) {
    /* Reset on both edges so re-enabling picks up any changed timing params
     * while the plugin was disabled.
     */
    reset(self);
    self->last_enabled = enabled_now;
  }

  if (!enabled_now) {
    /* Dry passthrough when bypassed/disabled. */
    if (self->ports.output_l) {
      if (self->ports.input_l) {
        memcpy(self->ports.output_l, self->ports.input_l, n_samples * sizeof(float));
      } else {
        memset(self->ports.output_l, 0, n_samples * sizeof(float));
      }
    }
    if (self->ports.output_r) {
      if (self->ports.input_r) {
        memcpy(self->ports.output_r, self->ports.input_r, n_samples * sizeof(float));
      } else {
        memset(self->ports.output_r, 0, n_samples * sizeof(float));
      }
    }
    return;
  }

  /* Handle control and MIDI events first so we quantize to the right boundary. */
  run_events(self, n_samples);

  /* Process audio loops (record + playback). */
  run_loops(self, n_samples);

  /* Click/metronome. */
  run_clicks(self, n_samples);
}

/* ------------------------------------------------------------------------
 * LV2 cleanup
 * ------------------------------------------------------------------------ */

static void cleanup(LV2_Handle instance)
{
  alo_log("Cleanup");
}

/* ------------------------------------------------------------------------
 * LV2 extension_data
 * ------------------------------------------------------------------------ */

static const void* extension_data(const char* uri)
{
  (void)uri;
  return NULL;
}

/* ------------------------------------------------------------------------
 * LV2 descriptor
 * ------------------------------------------------------------------------ */

static const LV2_Descriptor descriptor = {
    ALO_URI,       /* URI */
    instantiate,   /* instantiate */
    connect_port,  /* connect_port */
    activate,      /* activate */
    run,           /* run */
    deactivate,    /* deactivate */
    cleanup,       /* cleanup */
    extension_data /* extension_data */
};

LV2_SYMBOL_EXPORT
/* cppcheck-suppress unusedFunction - required by LV2 host loader */
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
  switch (index) {
  case 0:
    return &descriptor;
  default:
    return NULL;
  }
}