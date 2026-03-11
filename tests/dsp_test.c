/*
 * dsp_test.c — Small deterministic unit tests for DSP/utility helpers.
 *
 * Keep this file focused on pure helpers that can be tested without
 * allocating large buffers or depending on higher-level subsystems.
 */

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "alo_util.h"

/* Stubs for weak/extern symbols referenced by some headers in unit-test builds. */
void clear_inflight_actions_and_sync_controls(Alo* self) { (void)self; }
void request_ui_cycle_resync(Alo* self) { (void)self; }
void reset_timing(Alo* self) { (void)self; }
void alo_log(const char* message, ...) { (void)message; }

/* tolerances for floating-point assertions in DSP tests */
#define DSP_TEST_EPS_SMALL 1e-9f
#define DSP_TEST_EPS_LARGE 1e-6f

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

    /* Current contract: UI exposes a wider 0..10 sensitivity range.
     * Mapping is linear:
     *   s=0  -> 20.0
     *   s=10 -> 1.0
     * Lower returned values produce more transient triggers.
     */
    sens = 0.0f;
    assert(feq(alo_sensitivity_to_threshold(&alo), 20.0f, 1e-6f));
    sens = 10.0f;
    assert(feq(alo_sensitivity_to_threshold(&alo), 1.0f, 1e-6f));

    /* Monotonicity: increasing sensitivity must not increase the threshold. */
    sens = 2.0f;
    const float t2 = alo_sensitivity_to_threshold(&alo);
    sens = 8.0f;
    const float t8 = alo_sensitivity_to_threshold(&alo);
    assert(t8 <= t2);
}

int main(void)
{
    test_soft_clip_unit();
    test_edge_fade_samples();
    test_get_bar_len_samples();
    test_sensitivity_mapping();
    return 0;
}
