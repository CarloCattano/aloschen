/* Tests for vst3/alo_bridge.c: parameter table vs TTL, and transport-synced
 * record -> loop playback through the unchanged engine. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alo_bridge.h"

#define TTL_PATH "aloschen.lv2/aloschen.ttl"
#define RATE 48000.0
#define BLOCK 256u

static int failures = 0;
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                         \
      fprintf(stderr, __VA_ARGS__);                                                                \
      fprintf(stderr, "\n");                                                                       \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static char* read_file(const char* path)
{
  FILE* f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  const long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* buf = (char*)malloc((size_t)len + 1u);
  if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) {
    free(buf);
    buf = NULL;
  }
  if (buf) {
    buf[len] = '\0';
  }
  fclose(f);
  return buf;
}

/* Returns the TTL value after key inside [begin,end), or NAN. */
static double ttl_number(const char* begin, const char* end, const char* key)
{
  const size_t klen = strlen(key);
  for (const char* p = begin; p + klen < end; ++p) {
    if (!strncmp(p, key, klen)) {
      return strtod(p + klen, NULL);
    }
  }
  return NAN;
}

static void test_param_table_matches_ttl(void)
{
  char* ttl = read_file(TTL_PATH);
  CHECK(ttl != NULL, "cannot read %s (run from repo root)", TTL_PATH);
  if (!ttl) {
    return;
  }
  uint32_t lv2_params = 0;
  for (uint32_t i = 0; i < alo_bridge_param_count(); ++i) {
    const AloParamInfo* p = alo_bridge_param(i);
    if (p->port >= ALO_BRIDGE_EXTRA_BASE) {
      CHECK(p->output, "extra %s must be an output", p->symbol);
      continue;
    }
    lv2_params++;
    char needle[64];
    snprintf(needle, sizeof needle, "lv2:index %u;", p->port);
    const char* at = strstr(ttl, needle);
    CHECK(at != NULL, "port %u (%s) missing in TTL", p->port, p->symbol);
    if (!at) {
      continue;
    }
    const char* begin = at;
    while (begin > ttl && *begin != '[') {
      --begin;
    }
    const char* end = strchr(at, ']');
    char        sym[96];
    snprintf(sym, sizeof sym, "lv2:symbol \"%s\"", p->symbol);
    const char* sp = strstr(begin, sym);
    CHECK(sp && sp < end, "port %u symbol mismatch, expected %s", p->port, p->symbol);
    const char* dir = p->output ? "lv2:OutputPort" : "lv2:InputPort";
    const char* dp  = strstr(begin, dir);
    CHECK(dp && dp < end, "port %u (%s) direction mismatch", p->port, p->symbol);
    if (strcmp(p->symbol, "ENABLED") == 0) {
      continue; /* documented deviation in alo_bridge.c */
    }
    CHECK(fabs(ttl_number(begin, end, "lv2:default ") - p->def) < 1e-4, "%s default", p->symbol);
    CHECK(fabs(ttl_number(begin, end, "lv2:minimum ") - p->min) < 1e-4, "%s minimum", p->symbol);
    CHECK(fabs(ttl_number(begin, end, "lv2:maximum ") - p->max) < 1e-4, "%s maximum", p->symbol);
  }
  uint32_t ttl_controls = 0;
  for (const char* p = ttl; (p = strstr(p, "lv2:ControlPort")); ++p) {
    ttl_controls++;
  }
  CHECK(ttl_controls == lv2_params, "TTL has %u control ports, bridge maps %u", ttl_controls,
        lv2_params);
  free(ttl);
}

static uint32_t param_index(const char* symbol)
{
  for (uint32_t i = 0; i < alo_bridge_param_count(); ++i) {
    if (!strcmp(alo_bridge_param(i)->symbol, symbol)) {
      return i;
    }
  }
  fprintf(stderr, "unknown symbol %s\n", symbol);
  exit(2);
}

/* Records one bar of impulses on bar starts with the host playing, then
 * checks playback after the input goes silent: peaks land on bar starts. */
static void test_records_and_loops_synced_to_transport(void)
{
  AloBridge* b = alo_bridge_new(RATE, BLOCK);
  CHECK(b != NULL, "bridge creation failed");
  if (!b) {
    return;
  }
  alo_bridge_set_param(b, param_index("click"), 0.0f);
  alo_bridge_set_param(b, param_index("bars"), 1.0f);

  const uint32_t bar_frames = (uint32_t)(RATE * 60.0 / 120.0 * 4.0); /* 96000 */
  float          in[BLOCK], out_l[BLOCK], out_r[BLOCK];
  AloTransport   t     = {true, true, 120.0, 4.0, 0.0};
  const uint32_t loop1 = param_index("loop1"), state1 = param_index("loop1_state");
  float          max_state = 0.0f, play_peak = 0.0f;
  uint32_t       peaks_on_grid = 0, peaks_total = 0;
  bool           saw_playing = false;

  const uint32_t total_blocks = (bar_frames * 6u) / BLOCK;
  for (uint32_t blk = 0; blk < total_blocks; ++blk) {
    const uint64_t frame0           = (uint64_t)blk * BLOCK;
    const bool     recording_window = frame0 < (uint64_t)bar_frames * 3u;
    for (uint32_t i = 0; i < BLOCK; ++i) {
      const uint64_t f = frame0 + i;
      in[i]            = (recording_window && (f % bar_frames) < 32u) ? 0.8f : 0.0f;
    }
    /* The LV2 port is lv2:toggled: one press = one flip (rising edge, or a
     * falling edge after >= ALO_LOOP_BTN_HOLD_FRAMES runs). Flip on in bar 0
     * to arm base recording for the next cycle start. */
    alo_bridge_set_param(b, loop1, blk >= 10u ? 1.0f : 0.0f);
    t.beat = (double)frame0 * 120.0 / (60.0 * RATE);
    alo_bridge_run(b, &t, in, in, out_l, out_r, BLOCK, NULL, 0);

    const float st = alo_bridge_get_param(b, state1);
    max_state      = st > max_state ? st : max_state;
    if (frame0 >= (uint64_t)bar_frames * 4u) {
      saw_playing = saw_playing || fabsf(st - 0.5f) < 1e-3f;
      for (uint32_t i = 0; i < BLOCK; ++i) {
        const float a = fabsf(out_l[i]);
        play_peak     = a > play_peak ? a : play_peak;
        if (a > 0.1f) {
          peaks_total++;
          const uint32_t ph = (uint32_t)((frame0 + i) % bar_frames);
          if (ph < 64u || ph > bar_frames - 64u) {
            peaks_on_grid++;
          }
        }
      }
    }
  }
  CHECK(max_state >= 1.0f, "loop1 never reported recording (max state %.2f)", max_state);
  CHECK(saw_playing, "loop1 not reported playing after recording");
  CHECK(play_peak > 0.1f, "loop playback silent after input stopped (peak %.4f)", play_peak);
  CHECK(peaks_total > 0 && peaks_on_grid == peaks_total,
        "playback not bar-synced: %u of %u loud samples near bar starts", peaks_on_grid,
        peaks_total);
  CHECK(alo_bridge_get_param(b, param_index("out_peak_l")) >= 0.0f, "meter readable");
  printf("loop playback peak %.3f, %u/%u loud samples on bar grid\n", play_peak, peaks_on_grid,
         peaks_total);
  alo_bridge_free(b);
}

/* The pulse starts at zero, so its first nonzero sample is boundary + 1.
 * Large PPQ exposes precision loss; the small chunks straddle a fractional
 * grid line and must not retrigger the pulse at their next block start. */
static void test_click_sample_grid(double bpm, uint32_t max_block)
{
  AloBridge* b = alo_bridge_new(RATE, max_block);
  CHECK(b != NULL, "bridge creation failed");
  if (!b) {
    return;
  }
  const double   spb     = RATE * 60.0 / bpm;
  const double   origin  = 65536.0;
  const uint32_t sizes[] = {1, 7, 257, 4096, 13, 256};
  float          left[4096], right[4096];
  uint32_t       rendered = 0, step = 0, beats = 0, mismatches = 0;
  AloTransport   t = {true, true, bpm, 4.0, origin};
  while (rendered < 4.0 * spb + 1000.0) {
    const uint32_t n = sizes[step++ % (sizeof sizes / sizeof sizes[0])];
    t.beat           = origin + (double)rendered / spb;
    alo_bridge_run(b, &t, NULL, NULL, left, right, n, NULL, 0);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t f           = rendered + i;
      const uint32_t pulse_start = (uint32_t)ceil((double)beats * spb);
      /* Check onset against the independent grid, and enforce silence
       * outside the 20 ms pulse (the rest of the signal is denormal bias). */
      if (f == pulse_start + 1u && fabsf(left[i]) <= 1e-7f) {
        ++mismatches;
      }
      if ((f <= pulse_start || f >= pulse_start + (uint32_t)(0.02f * RATE)) &&
          fabsf(left[i]) > 1e-7f) {
        ++mismatches;
      }
      if (f == pulse_start + (uint32_t)(0.02f * RATE)) {
        ++beats;
      }
    }
    rendered += n;
  }
  CHECK(beats >= 5 && mismatches == 0, "%.6f BPM, max block %u: %u off-grid click samples", bpm,
        max_block, mismatches);
  /* Stop during a pulse, then seek backward and resume halfway to a beat:
   * the old click tail must not leak into either render. */
  t.beat = origin + 64.0 / spb;
  alo_bridge_run(b, &t, NULL, NULL, left, right, 32, NULL, 0);
  t.playing = false;
  alo_bridge_run(b, &t, NULL, NULL, left, right, 32, NULL, 0);
  CHECK(fabsf(left[4]) < 1e-7f, "click audible while stopped");
  t.beat    = origin + 0.5;
  t.playing = true;
  alo_bridge_run(b, &t, NULL, NULL, left, right, 32, NULL, 0);
  CHECK(fabsf(left[4]) < 1e-7f, "stale click tail on resume");
  alo_bridge_free(b);
}

/* A single-sample stereo marker, beyond the edge fade, must return at
 * exactly the same offset from each quantized cycle boundary. */
static void test_loop_sample_grid(double bpm)
{
  AloBridge* b = alo_bridge_new(RATE, BLOCK);
  CHECK(b != NULL, "bridge creation failed");
  if (!b) {
    return;
  }
  const double   spb         = RATE * 60.0 / bpm;
  const double   start_beat  = 65536.0;
  const double   bar         = 4.0 * spb;
  const uint32_t marker      = ((uint32_t)bar - 513u) & ~1u;
  const uint32_t recorded_at = (uint32_t)ceil(bar) + marker;
  const uint32_t total       = (uint32_t)ceil(7.0 * bar);
  const uint32_t sizes[]     = {257, 71, 19, 512, 1, 243};
  const uint32_t loop1       = param_index("loop1");
  float          in_l[512], in_r[512], out_l[512], out_r[512];
  alo_bridge_set_param(b, param_index("click"), 0);
  alo_bridge_set_param(b, param_index("bars"), 1);
  alo_bridge_set_param(b, param_index("mix"), 100);

  AloTransport t  = {true, true, bpm, 4.0, start_beat};
  uint32_t     at = 0, block = 0, expected = 2, actual = 0, mismatches = 0;
  while (at < total) {
    uint32_t       n      = sizes[block++ % (sizeof sizes / sizeof sizes[0])];
    const uint32_t window = (uint32_t)ceil(expected * bar) + marker;
    if (at + n > window - 8u && at <= window + 8u) {
      n = at < window - 8u ? window - 8u - at : 1u;
    }
    const uint32_t begin_playback = (uint32_t)ceil(2.0 * bar);
    t.beat                        = start_beat + at / spb;
    alo_bridge_set_param(b, loop1, at > 4096u ? 1.0f : 0.0f);
    for (uint32_t i = 0; i < n; i++) {
      in_l[i] = at + i == recorded_at ? 0.8f : 0.0f;
      in_r[i] = -in_l[i];
    }
    alo_bridge_run(b, &t, in_l, in_r, out_l, out_r, n, NULL, 0);
    for (uint32_t i = 0; i < n && at + i < total; i++) {
      const uint32_t frame = at + i;
      if (frame >= begin_playback) {
        const bool on_grid = frame == (uint32_t)ceil(expected * bar) + marker;
        if (on_grid) {
          ++expected;
        }
        const bool has_marker = out_l[i] > 0.7f && out_r[i] < -0.7f;
        actual += has_marker;
        mismatches += has_marker != on_grid;
      }
    }
    at += n;
  }
  CHECK(actual == 5 && mismatches == 0, "%.6f BPM: %u loop markers, %u sample mismatches", bpm,
        actual, mismatches);
  if (bpm == 120.0) {
    /* A backward seek to a non-boundary at double tempo must retain the
     * recorded buffer and rephase it against the original origin. */
    t.bpm                 = 240.0;
    t.beat                = start_beat - 3.75;
    const uint32_t target = marker / 2u - 3000u;
    t.beat += (target - 23u) / 12000.0;
    alo_bridge_run(b, &t, NULL, NULL, out_l, out_r, 64, NULL, 0);
    for (uint32_t i = 0; i < 64u; ++i) {
      CHECK((out_l[i] > 0.7f && out_r[i] < -0.7f) == (i == 23u),
            "retimed loop seek: unexpected audio at frame %u", i);
    }
  }
  alo_bridge_free(b);
}

static void test_param_clamping(void)
{
  AloBridge* b = alo_bridge_new(RATE, BLOCK);
  if (!b) {
    failures++;
    return;
  }
  const uint32_t bars = param_index("bars");
  alo_bridge_set_param(b, bars, 99.0f);
  CHECK(alo_bridge_get_param(b, bars) == 16.0f, "bars clamps to max");
  alo_bridge_set_param(b, bars, NAN);
  CHECK(alo_bridge_get_param(b, bars) == 1.0f, "NaN clamps to min");
  alo_bridge_free(b);
}

int main(void)
{
  test_param_table_matches_ttl();
  test_param_clamping();
  test_records_and_loops_synced_to_transport();
  test_click_sample_grid(120.0, 1);
  test_click_sample_grid(137.125987, 113);
  test_loop_sample_grid(120.0);
  test_loop_sample_grid(137.125987);
  test_loop_sample_grid(131.0);
  if (failures) {
    fprintf(stderr, "vst3 bridge tests: %d failure(s)\n", failures);
    return 1;
  }
  printf("vst3 bridge tests passed\n");
  return 0;
}
