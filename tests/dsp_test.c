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
    assert(feq(alo_soft_clip_unit(0.0f), 0.0f, 1e-9f));

    /* expected 0.5 / (1 + 0.5) = 1/3 */
    assert(feq(alo_soft_clip_unit(0.5f), 0.33333334f, 1e-7f));

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

    /* Null/invalid check -> default 64 */
    assert(alo_edge_fade_samples_u32(NULL) == 64u);

    /* Very low rate -> clamp to minimum 16 */
    alo.rate = 1000.0; /* 1kHz -> 1 sample per ms rounded -> 1 -> clamped to 16 */
    assert(alo_edge_fade_samples_u32(&alo) == 16u);

    /* Reasonable rate 48kHz -> ~48 samples */
    alo.rate = 48000.0;
    assert(alo_edge_fade_samples_u32(&alo) == 48u);

    /* Very high rate -> saturate at 512 */
    alo.rate = 1920000.0; /* 1.92MHz -> 1920 -> clamped to 512 */
    assert(alo_edge_fade_samples_u32(&alo) == 512u);
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
        assert(feq(buf[loop_start + i], 1.0f, 1e-9f));
        assert(feq(buf[loop_start + i + LOOP_SIZE], 1.0f, 1e-9f));
    }

    /* Apply fade */
    alo_apply_edge_fade_stereo(buf, loop_start, loop_samples, fade_samples);

    /* For fade_samples == 3, inv = 1/(3-1) = 0.5
     * Start fade-in (i = 0..2):
     *  i=0 -> g = 0 * inv = 0.0
     *  i=1 -> g = 1 * inv = 0.5
     *  i=2 -> g = 2 * inv = 1.0
     *
     * End fade-out is mirrored at the end of the loop.
     */
    const float inv = 1.0f / (float)(fade_samples - 1u);

    /* Start fade checks */
    for (uint32_t i = 0; i < fade_samples; ++i) {
        const float expected_g = (float)i * inv;
        const uint32_t idx = loop_start + i;
        assert(feq(buf[idx], 1.0f * expected_g, 1e-6f));
        assert(feq(buf[idx + LOOP_SIZE], 1.0f * expected_g, 1e-6f));
    }

    /* End fade checks: positions s1 - fade_samples + i */
    const uint32_t s1 = loop_start + loop_samples;
    for (uint32_t i = 0; i < fade_samples; ++i) {
        const float expected_g = (float)(fade_samples - 1u - i) * inv;
        const uint32_t idx = s1 - fade_samples + i;
        assert(feq(buf[idx], 1.0f * expected_g, 1e-6f));
        assert(feq(buf[idx + LOOP_SIZE], 1.0f * expected_g, 1e-6f));
    }

    free(buf);
}


/* Main entry: run all tests */
int main(void)
{
    test_soft_clip_unit();
    test_edge_fade_samples();
    test_get_bar_len_samples();
    test_apply_edge_fade_stereo();


    /* If we reach here all assertions passed. */
    printf("dsp_test: all tests passed\n");
    return 0;
}