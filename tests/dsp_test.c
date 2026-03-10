/*
 * DSP unit tests for aloschen
 *
 * This file contains small, deterministic tests for utility DSP helpers:
 * - alo_soft_clip_unit (inline in alo_util.h)
 * - alo_edge_fade_samples_u32 (alo_util.c)
 * - alo_get_bar_len_samples (inline helper)
 * - alo_apply_edge_fade_stereo (alo_util.c)
 *
 * Build note:
 * The repository's test harness may need to be updated to compile this file.
 * The tests rely only on headers and implementations available in source/.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alo_util.h"
#include "alo_engine.h"

/* tolerances for floating-point assertions in DSP tests */
#define DSP_TEST_EPS_SMALL 1e-9f
#define DSP_TEST_EPS_MED   1e-6f
#define DSP_TEST_EPS_LARGE 1e-7f
#define DSP_TEST_WAV_PATH "tests/assets/sample.wav"

void clear_inflight_actions_and_sync_controls(Alo* self) { (void)self; }
void request_ui_cycle_resync(Alo* self) { (void)self; }
void reset_timing(Alo* self) { (void)self; }
void alo_log(const char* message, ...) { (void)message; }

static float td_test_absf(float x)
{
    return (x < 0.0f) ? -x : x;
}

/* Simple floating-point approximate equality */
static int feq(float a, float b, float eps)
{
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d <= eps;
}

/* Test alo_soft_clip_unit inline helper */
static void test_soft_clip_unit(void)
{
    /* A few representative inputs */
    assert(feq(alo_soft_clip_unit(0.0f), 0.0f, DSP_TEST_EPS_SMALL));

    /* expected 0.5 / (1 + 0.5) = 1/3 */
    assert(feq(alo_soft_clip_unit(0.5f), 0.33333334f, DSP_TEST_EPS_LARGE));

    /* expected -2 / (1 + 2) = -2/3 */
    assert(feq(alo_soft_clip_unit(-2.0f), -0.6666667f, 1e-7f));

    /* Large magnitude should approach sign(x) */
    assert(alo_soft_clip_unit(1e6f) < 1.0f && alo_soft_clip_unit(1e6f) > 0.999f);

    /* Symmetry */
    assert(alo_soft_clip_unit(-1e6f) > -1.0f && alo_soft_clip_unit(-1e6f) < -0.999f);
}

/* Test alo_edge_fade_samples_u32 behavior for various sample rates */
static void test_edge_fade_samples(void)
{
    Alo alo = {0};

    /* Null/invalid check -> default value */
    assert(alo_edge_fade_samples_u32(NULL) == ALO_EDGE_FADE_DEFAULT_SAMPLES);

    /* Very low rate -> clamp to minimum */
    alo.rate = 1000.0; /* 1kHz -> 1 sample per ms rounded -> 1 -> clamped to min */
    assert(alo_edge_fade_samples_u32(&alo) == ALO_EDGE_FADE_SAMPLES_MIN);

    /* Reasonable rate 48kHz -> ~48 samples */
    alo.rate = 48000.0;
    assert(alo_edge_fade_samples_u32(&alo) == (uint32_t)lrintf((double)alo.rate * 0.001));

    /* Very high rate -> saturate at maximum */
    alo.rate = 1920000.0; /* 1.92MHz -> 1920 -> clamped to max */
    assert(alo_edge_fade_samples_u32(&alo) == ALO_EDGE_FADE_SAMPLES_MAX);
}

/* Test alo_get_bar_len_samples inline helper */
static void test_get_bar_len_samples(void)
{
    Alo alo = {0};

    /* If ports.bars is missing, alo_get_bars_i will return DEFAULT_NUM_BARS (4) */
    alo.loop_samples = 48000u;
    alo.ports.bars = NULL;
    /* loop_samples / 4 -> 12000 */
    assert(alo_get_bar_len_samples(&alo) == 12000u);

    /* Provide bars=2 -> bar length = loop_samples/2 */
    float bars_f = 2.0f;
    alo.ports.bars = &bars_f;
    assert(alo_get_bar_len_samples(&alo) == 24000u);

    /* Edge: loop_samples may not divide evenly; ensure non-zero */
    alo.loop_samples = 3u;
    bars_f = 2.0f;
    alo.ports.bars = &bars_f;
    /* 3/2 -> 1 (integer division), should be >=1 */
    assert(alo_get_bar_len_samples(&alo) == 1u);
}

/* Test sensitivity-to-threshold helper */
static void test_sensitivity_mapping(void)
{
    Alo alo = {0};
    float sens;
    alo.ports.slice_sens = &sens;

    sens = 0.0f;
    assert(feq(alo_sensitivity_to_threshold(&alo), ALO_SENS_THRESH_MAX_RATIO, 1e-6f));
    sens = 1.0f;
    assert(feq(alo_sensitivity_to_threshold(&alo), ALO_SENS_THRESH_MIN_RATIO, 1e-6f));

    printf("dsp_test: sensitivity mapping — PASSED\n");
}

/* Test alo_apply_edge_fade_stereo:
 * We allocate a stereo loop buffer (L then R each of size LOOP_SIZE) and set
 * a small loop region within it to known values. We then apply a small fade
 * and check that start/end samples are multiplied by the expected factors.
 */
static void test_apply_edge_fade_stereo(void)
{
    /* Allocate the large pre-sized loop buffer required by the implementation.
     * Note: LOOP_SIZE is defined in alo_engine.h and is the expected layout size.
     */
    size_t total_samples = (size_t)LOOP_SIZE * 2u;
    float* buf = (float*)calloc(total_samples, sizeof(float));
    if (!buf) {
        /* If allocation fails, skip the test with a fatal assertion. */
        fprintf(stderr, "dsp_test: failed to allocate loop buffer of %zu floats\n", total_samples);
        assert(0 && "allocation failed");
    }

    /* We'll mark a short loop inside the buffer and fill it with 1.0f values. */
    const uint32_t loop_start = 10u;
    const uint32_t loop_samples = 8u; /* small test loop */
    const uint32_t fade_samples = 3u; /* small fade length */

    /* Fill left channel region [loop_start .. loop_start+loop_samples) with 1.0 */
    for (uint32_t i = 0; i < loop_samples; ++i) {
        buf[loop_start + i] = 1.0f;
        /* Corresponding right channel is offset by LOOP_SIZE */
        buf[loop_start + i + LOOP_SIZE] = 1.0f;
    }

    /* Sanity: ensure preconditions are met */
    for (uint32_t i = 0; i < loop_samples; ++i) {
        assert(feq(buf[loop_start + i], 1.0f, DSP_TEST_EPS_SMALL));
        assert(feq(buf[loop_start + i + LOOP_SIZE], 1.0f, DSP_TEST_EPS_SMALL));
    }

    /* Apply fade */
    alo_apply_edge_fade_stereo(buf, loop_start, loop_samples, fade_samples);

    /* With the raised‑cosine fade implemented below the shape is smoother
     * (zero slope at both ends).  For fade_samples == 3 the gains are:
     *
     *   i=0 -> 0.0
     *   i=1 -> 0.5 * (1 - cos(pi*0.5)) = 0.5
     *   i=2 -> 1.0
     *
     * i.e. the same numeric values as the linear case for this tiny fade, but
     * the general formula is different and we test it accordingly. */
    const float pi = 3.14159265358979323846f;
    /* Start fade checks */
    for (uint32_t i = 0; i < fade_samples; ++i) {
        float t = (float)i / (float)(fade_samples - 1u);
        float expected_g = 0.5f * (1.0f - cosf(pi * t));
        const uint32_t idx = loop_start + i;
        assert(feq(buf[idx], 1.0f * expected_g, 1e-6f));
        assert(feq(buf[idx + LOOP_SIZE], 1.0f * expected_g, 1e-6f));
    }

    /* End fade checks: positions s1 - fade_samples + i */
    const uint32_t s1 = loop_start + loop_samples;
    for (uint32_t i = 0; i < fade_samples; ++i) {
        float t = (float)(fade_samples - 1u - i) / (float)(fade_samples - 1u);
        float expected_g = 0.5f * (1.0f - cosf(pi * t));
        const uint32_t idx = s1 - fade_samples + i;
        assert(feq(buf[idx], 1.0f * expected_g, 1e-6f));
        assert(feq(buf[idx + LOOP_SIZE], 1.0f * expected_g, 1e-6f));
    }

    free(buf);
}

/* ------------------------------------------------------------------------- */

/* Unit test for transient detector logic described in the assignment. */
static void test_transient_detector(void)
{
    TransientDetector td;
    td_init(&td, 48000.0f, 2.0f /*threshold*/, 5.0f /*debounce ms*/);

    /* feed a buffer of silence then a single impulse */
    bool fired = false;
    for (int i = 0; i < 100; ++i) {
        if (td_process_sample(&td, (i == 50) ? 1.0f : 0.0f)) {
            fired = true;
            /* ensure it only fires once */
            break;
        }
    }
    assert(fired && "detector failed to fire on synthetic pulse");

    /* if we feed another strong sample immediately, debounce should prevent
       a second trigger. */
    fired = false;
    td_init(&td, 48000.0f, 2.0f, 5.0f);
    /* first sample triggers */
    assert(td_process_sample(&td, 1.0f));
    /* next sample within debounce window should not trigger */
    for (int i = 0; i < (int)td.debounce_samples - 1; ++i) {
        if (td_process_sample(&td, 1.0f)) {
            fired = true;
            break;
        }
    }
    assert(!fired && "debounce window failed");
}

static void test_transient_candidate_pretransient_boundary(void)
{
    TransientDetector td;
    const float sr = 48000.0f;
    const uint32_t lead_start = 100u;
    const uint32_t peak_at = 140u;
    const uint32_t tail_end = 260u;
    const uint32_t pre_roll = (uint32_t)((8.0f * 0.001f) * sr + 0.5f);
    uint32_t first_trigger_sample = UINT32_MAX;
    float best_amp = 0.0f;
    float best_ratio = 0.0f;
    uint32_t best_sample = 0u;

    td_init_ex(&td, sr, 1.2f, 5.0f, 1.0f, 1.05f);

    for (uint32_t i = 0u; i < tail_end; ++i) {
        float x = 0.0f;

        if (i >= lead_start && i < 110u) {
            x = 0.05f;
        } else if (i >= 110u && i < 120u) {
            x = 0.10f;
        } else if (i >= 120u && i < 130u) {
            x = 0.25f;
        } else if (i >= 130u && i < peak_at) {
            x = 0.55f;
        } else if (i == peak_at) {
            x = 1.0f;
        } else if (i > peak_at && i < 150u) {
            x = 0.45f;
        } else if (i >= 150u && i < 170u) {
            x = 0.08f;
        }

        {
            TransientDetectorEvent ev = td_process_sample_ex(&td, x);
            if (ev.triggered && first_trigger_sample == UINT32_MAX) {
                first_trigger_sample = ev.sample_index;
            }
            if (ev.amplitude > best_amp) {
                best_amp = ev.amplitude;
            }
            if (ev.onset_ratio > best_ratio) {
                best_ratio = ev.onset_ratio;
                best_sample = ev.sample_index;
            }
        }
    }

    assert(first_trigger_sample != UINT32_MAX);
    assert(best_amp > 0.9f);
    assert(best_ratio > 1.2f);

    /* Validate the musical contract instead of the previous detector's exact
       candidate lifecycle: the onset must trigger before the transient peak,
       and the chosen boundary should still fall shortly before the peak. */
    assert(first_trigger_sample >= lead_start);
    assert(first_trigger_sample < peak_at);
    assert(best_sample >= first_trigger_sample);
    assert(best_sample <= peak_at);

    {
        uint32_t boundary = first_trigger_sample;
        uint32_t pre_peak = (peak_at > pre_roll) ? (peak_at - pre_roll) : 0u;
        if (pre_peak > boundary) {
            boundary = pre_peak;
        }

        assert(boundary < peak_at);
        assert(boundary >= first_trigger_sample);
        assert(boundary >= lead_start);
        assert(boundary < tail_end);
    }

    printf("dsp_test: pre-transient boundary tracking — PASSED\n");
}

static void test_transient_candidate_rejects_short_burst(void)
{
    TransientDetector td;
    const float sr = 48000.0f;
    const uint32_t short_len = (uint32_t)((5.0f * 0.001f) * sr + 0.5f);
    bool saw_trigger = false;
    bool completed_any = false;

    td_init_ex(&td, sr, 1.2f, 5.0f, 20.0f, 1.05f);

    for (uint32_t i = 0u; i < 4000u; ++i) {
        float x = 0.0f;
        if (i >= 1000u && i < 1000u + short_len) {
            x = (i == 1000u + short_len / 2u) ? 1.0f : 0.45f;
        }

        {
            TransientDetectorEvent ev = td_process_sample_ex(&td, x);
            if (ev.triggered) {
                saw_trigger = true;
            }
            if (ev.candidate_complete) {
                completed_any = true;
            }
        }
    }

    /* A very short burst may still cross the trigger threshold, but it must
       not become a musically valid completed onset region. */
    assert(saw_trigger);
    assert(!completed_any);
    {
        const TransientCandidate* last = td_last_completed_candidate(&td);
        assert(!last || !last->complete || td_test_absf(last->peak_amplitude) < 1e-9f);
    }

    printf("dsp_test: short burst rejection — PASSED\n");
}

static Alo make_detection_alo(void)
{
    Alo a;
    memset(&a, 0, sizeof(a));
    a.bpm = 120.0f;
    a.bpb = 4.0f;
    a.loop_beats = 16u;
    a.rate = 48000.0;
    a.cached_sens = -1.0f;
    a.cached_threshold = -1.0f;
    a.cached_debounce_ms = -1.0f;
    a.cached_burst_ms = -1.0f;
    a.cached_end_ratio = -1.0f;
    a.cached_pre_ms = -1.0f;
    a.cached_split_mode = false;
    a.pending_arm_track = -1;
    a.pending_arm_type = TRACK_IDLE;
    return a;
}

static float dsp_test_mono_sample_at(const TestWavData* wav, size_t i)
{
    const float l = wav->left[i];
    const float r = wav->right ? wav->right[i] : l;
    return 0.5f * (l + r);
}

static void prepare_detection_loop_from_wav(Alo* a, const TestWavData* wav)
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
        a->loop_buf[0][i] = dsp_test_mono_sample_at(wav, i);
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

static void free_detection_alo(Alo* a)
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

static void test_sample_wav_transient_slice_guardrails(void)
{
    TestWavData wav;
    Alo a;
    float bars = 1.0f;
    float sens = 0.95f;
    float thr = 2.0f;
    float split = 1.0f;
    float spb = 4.0f;
    float debounce = 0.5f;
    float burst = 1.0f;
    float end_ratio = 1.01f;
    float pre_ms = 1.0f;
    float detected_out = 0.0f;

    memset(&wav, 0, sizeof(wav));
    assert(test_wav_load(DSP_TEST_WAV_PATH, &wav) == 0);

    a = make_detection_alo();
    a.ports.bars = &bars;
    a.ports.slice_sens = &sens;
    a.ports.transient_threshold = &thr;
    a.ports.split_by_transient = &split;
    a.ports.slices_per_bar = &spb;
    a.ports.transient_debounce = &debounce;
    a.ports.transient_burst = &burst;
    a.ports.transient_end_ratio = &end_ratio;
    a.ports.transient_pre_ms = &pre_ms;
    a.ports.detected_slices_out = &detected_out;

    prepare_detection_loop_from_wav(&a, &wav);

    sampler_cache_process(&a, a.loop_samples, true);
    while (a.sampler_src_rebuild_active || a.detect_active) {
        sampler_cache_process(&a, a.loop_samples, true);
    }

    assert(a.detected_slices_count >= 5u);
    assert(a.detected_slices_count <= 30u);
    assert(detected_out == (float)a.detected_slices_count);
    assert(a.detected_slice_offsets[0] == 0u);

    for (uint32_t i = 1u; i < a.detected_slices_count; ++i) {
        assert(a.detected_slice_offsets[i] > a.detected_slice_offsets[i - 1u]);
        assert(a.detected_slice_offsets[i] < a.loop_samples);
    }

    free_detection_alo(&a);
    test_wav_free(&wav);
    printf("dsp_test: sample.wav slice guardrails — PASSED\n");
}


int main(void)
{
    test_soft_clip_unit();
    test_edge_fade_samples();
    test_get_bar_len_samples();
    test_sensitivity_mapping();
    test_apply_edge_fade_stereo();

    /* run transient detector tests last */
    test_transient_detector();
    test_transient_candidate_pretransient_boundary();
    test_transient_candidate_rejects_short_burst();
    test_sample_wav_transient_slice_guardrails();

    return 0;
}
