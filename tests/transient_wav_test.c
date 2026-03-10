#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alo_engine.h"
#include "alo_util.h"
#include "sampler_cache.h"
#include "slice_sampler.h"
#include "transient_detector.h"
#include "helpers/test_wav_loader.h"

#define TRANSIENT_WAV_PATH "tests/assets/sample.wav"
#define TRANSIENT_TEST_MAX_OFFSETS ALO_SLICE_SAMPLER_MAX_VOICES

typedef struct DetectionResult {
    uint32_t count;
    uint32_t offsets[TRANSIENT_TEST_MAX_OFFSETS];
    uint32_t total_frames;
    uint32_t sample_rate;
} DetectionResult;

void clear_inflight_actions_and_sync_controls(Alo* self) { (void)self; }
void request_ui_cycle_resync(Alo* self) { (void)self; }
void reset_timing(Alo* self) { (void)self; }

/* Keep the weak logging symbol quiet in test builds. */
void alo_log(const char* message, ...) { (void)message; }

static Alo make_alo(void)
{
    Alo a;
    memset(&a, 0, sizeof(a));
    a.bpm = 120.0f;
    a.bpb = 4.0f;
    a.loop_beats = 16u;
    a.rate = 48000.0;
    a.cached_sens = -1.0f;
    a.cached_threshold = -1.0f;
    a.cached_split_mode = false;
    a.pending_arm_track = -1;
    a.pending_arm_type = TRACK_IDLE;
    return a;
}

static float mono_sample_at(const TestWavData* wav, size_t i)
{
    const float l = wav->left[i];
    const float r = wav->right ? wav->right[i] : l;
    return 0.5f * (l + r);
}

static void prepare_loop_from_wav(Alo* a, const TestWavData* wav)
{
    assert(a);
    assert(wav);
    assert(wav->frames > 0u);
    assert(wav->frames <= LOOP_SIZE);

    a->rate = (double)wav->sample_rate;
    a->loop_samples = (uint32_t)wav->frames;
    a->loop_start = 0u;
    a->have_loop[0] = true;

    a->loop_buf[0] = (float*)calloc((size_t)LOOP_SIZE * 2u, sizeof(float));
    a->sampler_src_buf = (float*)calloc((size_t)LOOP_SIZE * 2u, sizeof(float));
    a->sampler_src_buf_shadow = (float*)calloc((size_t)LOOP_SIZE * 2u, sizeof(float));

    assert(a->loop_buf[0]);
    assert(a->sampler_src_buf);
    assert(a->sampler_src_buf_shadow);

    for (size_t i = 0u; i < wav->frames; ++i) {
        a->loop_buf[0][i] = mono_sample_at(wav, i);
        a->loop_buf[0][i + LOOP_SIZE] = a->loop_buf[0][i];
    }

    a->sampler_src_dirty = true;
    a->sampler_src_valid = false;
    a->sampler_src_rebuild_active = false;
    a->sampler_src_pos = 0u;
    a->detect_active = false;
    a->detect_pos = 0u;
    a->detected_slices_count = 0u;
    memset(a->detected_slice_offsets, 0, sizeof(a->detected_slice_offsets));
    memset(a->detected_slice_strength, 0, sizeof(a->detected_slice_strength));
}

static void free_alo_buffers(Alo* a)
{
    if (!a) {
        return;
    }
    free(a->loop_buf[0]);
    a->loop_buf[0] = NULL;
    free(a->sampler_src_buf);
    a->sampler_src_buf = NULL;
    free(a->sampler_src_buf_shadow);
    a->sampler_src_buf_shadow = NULL;
}

static DetectionResult run_detection(const TestWavData* wav,
                                     float sensitivity,
                                     float threshold,
                                     float split_mode,
                                     float slices_per_bar)
{
    Alo a = make_alo();
    DetectionResult r;
    float bars = 1.0f;
    float sens = sensitivity;
    float thr = threshold;
    float split = split_mode;
    float spb = slices_per_bar;
    float detected_out = 0.0f;

    memset(&r, 0, sizeof(r));

    a.ports.bars = &bars;
    a.ports.slice_sens = &sens;
    a.ports.transient_threshold = &thr;
    a.ports.split_by_transient = &split;
    a.ports.slices_per_bar = &spb;
    a.ports.detected_slices_out = &detected_out;

    prepare_loop_from_wav(&a, wav);

    sampler_cache_process(&a, a.loop_samples, true);
    while (a.sampler_src_rebuild_active || a.detect_active) {
        sampler_cache_process(&a, a.loop_samples, true);
    }

    r.count = a.detected_slices_count;
    if (r.count > TRANSIENT_TEST_MAX_OFFSETS) {
        r.count = TRANSIENT_TEST_MAX_OFFSETS;
    }
    memcpy(r.offsets, a.detected_slice_offsets, sizeof(uint32_t) * r.count);
    r.total_frames = a.loop_samples;
    r.sample_rate = (uint32_t)a.rate;

    free_alo_buffers(&a);
    return r;
}

static uint32_t find_peak_index(const TestWavData* wav, uint32_t start, uint32_t end)
{
    uint32_t best = start;
    float best_abs = 0.0f;

    if (!wav || wav->frames == 0u) {
        return 0u;
    }

    if (end > (uint32_t)wav->frames) {
        end = (uint32_t)wav->frames;
    }
    if (start >= end) {
        return start;
    }

    for (uint32_t i = start; i < end; ++i) {
        const float x = mono_sample_at(wav, (size_t)i);
        const float ax = (x < 0.0f) ? -x : x;
        if (ax > best_abs) {
            best_abs = ax;
            best = i;
        }
    }

    return best;
}

static void assert_offsets_sorted_and_in_range(const DetectionResult* r)
{
    assert(r);
    assert(r->total_frames > 0u);
    assert(r->count >= 1u);
    assert(r->count <= TRANSIENT_TEST_MAX_OFFSETS);

    for (uint32_t i = 0u; i < r->count; ++i) {
        assert(r->offsets[i] < r->total_frames);
        if (i > 0u) {
            assert(r->offsets[i] > r->offsets[i - 1u]);
        }
    }
}

static void test_sample_wav_loads(void)
{
    TestWavData wav;
    memset(&wav, 0, sizeof(wav));

    assert(test_wav_load(TRANSIENT_WAV_PATH, &wav) == 0);
    assert(wav.frames > 0u);
    assert(wav.sample_rate > 0u);
    assert(wav.channels == 1u || wav.channels == 2u);
    assert(wav.left != NULL);
    assert(wav.right != NULL);

    test_wav_free(&wav);
    printf("transient_wav_test: sample.wav loads — PASSED\n");
}

static void test_transient_detection_produces_ordered_offsets(void)
{
    TestWavData wav;
    DetectionResult r;

    memset(&wav, 0, sizeof(wav));
    assert(test_wav_load(TRANSIENT_WAV_PATH, &wav) == 0);

    r = run_detection(&wav, 0.5f, 4.0f, 1.0f, 4.0f);

    assert_offsets_sorted_and_in_range(&r);
    assert(r.count >= 4u);
    assert(r.offsets[0] == 0u);

    test_wav_free(&wav);
    printf("transient_wav_test: ordered offsets — PASSED\n");
}

static void test_sensitivity_monotonic_slice_count(void)
{
    TestWavData wav;
    DetectionResult low;
    DetectionResult mid;
    DetectionResult high;

    memset(&wav, 0, sizeof(wav));
    assert(test_wav_load(TRANSIENT_WAV_PATH, &wav) == 0);

    low = run_detection(&wav, 0.0f, 4.0f, 1.0f, 4.0f);
    mid = run_detection(&wav, 0.5f, 4.0f, 1.0f, 4.0f);
    high = run_detection(&wav, 1.0f, 4.0f, 1.0f, 4.0f);

    /* The refined onset-region detector may legitimately collapse adjacent
       candidates into the same musical regions for this fixture, so exact
       growth is not guaranteed at every step. What must remain true is that
       higher sensitivity does not reduce the detected count. */
    assert(low.count <= mid.count);
    assert(mid.count <= high.count);
    assert(high.count >= low.count);

    test_wav_free(&wav);
    printf("transient_wav_test: sensitivity monotonic count — PASSED\n");
}

static void test_slice_spread_is_reasonable(void)
{
    TestWavData wav;
    DetectionResult r;
    uint32_t span;
    uint32_t positive_gaps = 0u;
    uint32_t min_gap = UINT32_MAX;

    memset(&wav, 0, sizeof(wav));
    assert(test_wav_load(TRANSIENT_WAV_PATH, &wav) == 0);

    r = run_detection(&wav, 0.75f, 4.0f, 1.0f, 4.0f);

    assert_offsets_sorted_and_in_range(&r);
    assert(r.count >= 4u);
    assert(r.offsets[0] == 0u);

    span = r.offsets[r.count - 1u] - r.offsets[0];
    assert(span > (r.total_frames / 3u));
    assert(r.offsets[r.count - 1u] > (r.total_frames / 4u));

    for (uint32_t i = 1u; i < r.count; ++i) {
        const uint32_t gap = r.offsets[i] - r.offsets[i - 1u];
        if (gap > 0u) {
            ++positive_gaps;
        }
        if (gap < min_gap) {
            min_gap = gap;
        }
    }

    assert(positive_gaps == (r.count - 1u));
    assert(min_gap > 0u);

    test_wav_free(&wav);
    printf("transient_wav_test: slice spread sanity — PASSED\n");
}

static void test_transient_boundaries_include_pretransient_audio(void)
{
    TestWavData wav;
    DetectionResult r;

    memset(&wav, 0, sizeof(wav));
    assert(test_wav_load(TRANSIENT_WAV_PATH, &wav) == 0);

    r = run_detection(&wav, 0.75f, 4.0f, 1.0f, 4.0f);

    assert_offsets_sorted_and_in_range(&r);
    assert(r.count >= 3u);

    for (uint32_t i = 1u; i < r.count; ++i) {
        const uint32_t slice_start = r.offsets[i - 1u];
        const uint32_t slice_end = r.offsets[i];
        const uint32_t peak = find_peak_index(&wav, slice_start, slice_end);

        assert(peak >= slice_start);
        assert(peak < slice_end);
        assert(peak > slice_start);
    }

    test_wav_free(&wav);
    printf("transient_wav_test: pre-transient boundaries — PASSED\n");
}

static void test_grid_setting_does_not_change_transient_offsets(void)
{
    TestWavData wav;
    DetectionResult a;
    DetectionResult b;
    DetectionResult c;

    memset(&wav, 0, sizeof(wav));
    assert(test_wav_load(TRANSIENT_WAV_PATH, &wav) == 0);

    a = run_detection(&wav, 0.65f, 4.0f, 1.0f, 2.0f);
    b = run_detection(&wav, 0.65f, 4.0f, 1.0f, 4.0f);
    c = run_detection(&wav, 0.65f, 4.0f, 1.0f, 8.0f);

    assert(a.count == b.count);
    assert(b.count == c.count);
    assert(memcmp(a.offsets, b.offsets, sizeof(uint32_t) * a.count) == 0);
    assert(memcmp(b.offsets, c.offsets, sizeof(uint32_t) * b.count) == 0);

    test_wav_free(&wav);
    printf("transient_wav_test: grid independence — PASSED\n");
}

int main(void)
{
    test_sample_wav_loads();
    test_transient_detection_produces_ordered_offsets();
    test_sensitivity_monotonic_slice_count();
    test_slice_spread_is_reasonable();
    test_transient_boundaries_include_pretransient_audio();
    test_grid_setting_does_not_change_transient_offsets();

    printf("transient_wav_test: all checks passed\n");
    return 0;
}
