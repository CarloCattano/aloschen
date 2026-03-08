#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include "transport.h"
#include "alo_util.h"  /* bring in shared constants such as ALO_BEAT_BOUNDARY_EPS */

/* small tolerances used in assertions */
#define TEST_EPS       1e-6
#define TEST_EPS_SMALL 1e-9

/* Expanded tests for compute_transport_phase_index and
 * compute_next_cycle_start_beats covering:
 *  - wrap-around
 *  - zero and negative beats
 *  - fractional beats
 *  - single-beat loop
 *  - large loop_samples handling
 *  - multiple bpb/bars combinations
 *
 * Stubs for functions referenced by transport.c but not under test.
 */
void clear_inflight_actions_and_sync_controls(Alo* self) { (void)self; }
void request_ui_cycle_resync(Alo* self) { (void)self; }
void alo_slice_sampler_reset(AloSliceSampler* s) { (void)s; }
void reset_timing(Alo* self) { (void)self; }

/* Helper: compute expected phase samples using the same algorithm as
 * compute_transport_phase_index, so tests verify arithmetic precisely.
 */
static uint32_t expected_phase_index(const Alo* self, double global_beats)
{
    if (!self)
        return 0u;
    const uint32_t loop_beats   = self->loop_beats;
    const uint32_t loop_samples = self->loop_samples;
    if (!(loop_beats > 0u && loop_samples > 0u))
        return 0u;

    const double origin      = self->have_loop_origin ? self->loop_origin_beats : 0.0;
    double       phase_beats = fmod(global_beats - origin, (double)loop_beats);
    if (phase_beats < 0.0)
        phase_beats += (double)loop_beats;

    double phase_samples_d = phase_beats * (double)loop_samples / (double)loop_beats;
    if (phase_samples_d < 0.0)
        phase_samples_d = 0.0;
    uint32_t phase_samples = (uint32_t)floor(phase_samples_d);
    if (phase_samples >= loop_samples)
        phase_samples = loop_samples - 1;

    return phase_samples;
}

static void test_compute_transport_phase_index_variety(void)
{
    printf("transport_test: compute_transport_phase_index - start\n");

    Alo alo = {0};
    /* Basic wrap-around and fractional tests */
    alo.loop_beats = 4u;
    alo.loop_samples = 48000u; /* 1 beat == 48000 samples for these tests */

    double test_beats[] = {0.0, 1.0, 1.5, 2.75, 3.9999, 4.0, 5.0, -1.0, -4.0, 9.25};
    const size_t n = sizeof(test_beats) / sizeof(test_beats[0]);

    for (size_t i = 0; i < n; ++i) {
        double b = test_beats[i];
        uint32_t idx = 0u;
        assert(compute_transport_phase_index(&alo, b, &idx));
        /* Check equality */
        assert(idx == expected_phase_index(&alo, b));
    }

    /* Single-beat loop: loop_beats = 1 */
    alo.loop_beats = 1u;
    alo.loop_samples = 48000u;
    {
        double bvals[] = {0.0, 0.5, 1.0, 1.25, -0.1};
        const size_t m = sizeof(bvals) / sizeof(bvals[0]);
        for (size_t i = 0; i < m; ++i) {
            uint32_t idx = 0u;
            assert(compute_transport_phase_index(&alo, bvals[i], &idx));
            assert(idx == expected_phase_index(&alo, bvals[i]));
        }
    }

    /* Very large loop_samples: ensure we don't overflow and result is < loop_samples */
    alo.loop_beats = 4u;
    alo.loop_samples = 0xFFFFFFFFu; /* max uint32_t */
    {
        double b = 5.5; /* phase_beats = 1.5 -> expect approx (1.5/4)*UINT32_MAX */
        uint32_t idx = 0u;
        assert(compute_transport_phase_index(&alo, b, &idx));
        assert(idx == expected_phase_index(&alo, b));
        assert(idx < alo.loop_samples);
    }

    printf("transport_test: compute_transport_phase_index - all checks passed\n");
}

/* Tests for compute_next_cycle_start_beats per the user's checklist */
static void test_compute_next_cycle_start_beats_variety(void)
{
    printf("transport_test: compute_next_cycle_start_beats - start\n");

    Alo alo = {0};

    /* Helper to set bars via port pointer */
    float bars_val;

    /* Case: current beat exactly at cycle start -> should return same beat */
    alo.bpb = 4.0f;
    bars_val = 2.0f; /* 2 bars -> cycle length = 8 beats */
    alo.ports.bars = &bars_val;
    {
        double cur = 8.0;
        assert(fabs(compute_next_cycle_start_beats(&alo, cur) - cur) < 1e-9);
    }

    /* Fractional beat positions */
    {
        double cur = 0.1;
        /* cycle_len = bars * bpb = 2*4 = 8, phase = 0.1 -> next = cur + (8 - 0.1) */
        const double expected = cur + (8.0 - fmod(cur, 8.0));
        /* But when on boundary we return cur; this is not boundary, so expect > cur */
        assert(compute_next_cycle_start_beats(&alo, cur) > cur);
        /* numeric tolerance */
        assert(fabs(compute_next_cycle_start_beats(&alo, cur) - expected) < TEST_EPS);
    }

    {
        double cur = 7.5;
        /* Expect next cycle start at 8.0 */
        assert(fabs(compute_next_cycle_start_beats(&alo, cur) - 8.0) < TEST_EPS);
    }

    /* Different beats-per-bar (bpb = 3, 5) and multiple bars (2,3,4) */
    {
        float bpb_vals[] = {3.0f, 5.0f};
        uint32_t bars_opts[] = {2u, 3u, 4u};
        for (size_t i = 0; i < sizeof(bpb_vals) / sizeof(bpb_vals[0]); ++i) {
            alo.bpb = bpb_vals[i];
            for (size_t j = 0; j < sizeof(bars_opts) / sizeof(bars_opts[0]); ++j) {
                float bv = (float)bars_opts[j];
                alo.ports.bars = &bv;
                double cycle_len = (double)bv * (double)alo.bpb;
                /* test multiple current beats across a couple cycles */
                for (double cur = 0.0; cur < cycle_len * 2.0; cur += (cycle_len / 4.0)) {
                    const double next = compute_next_cycle_start_beats(&alo, cur);
                    /* if we're effectively on the boundary, next==cur */
                    double phase = fmod(cur, cycle_len);
                    if (phase < 0.0)
                        phase += cycle_len;
                    const double kCycleEpsBeats = (double)ALO_BEAT_BOUNDARY_EPS;
                    if (phase <= kCycleEpsBeats || (cycle_len - phase) <= kCycleEpsBeats) {
                        assert(fabs(next - cur) < TEST_EPS);
                    } else {
                        assert(fabs(next - (cur + (cycle_len - phase))) < TEST_EPS);
                    }
                }
            }
        }
    }

    /* Edge cases: zero bpb should return the input beat (safe handling) */
    alo.bpb = 0.0f;
    bars_val = 2.0f;
    alo.ports.bars = &bars_val;
    {
        double cur = 3.14;
        assert(fabs(compute_next_cycle_start_beats(&alo, cur) - cur) < TEST_EPS_SMALL);
    }

    printf("transport_test: compute_next_cycle_start_beats - all checks passed\n");
}

int main(void) {
    test_compute_transport_phase_index_variety();
    test_compute_next_cycle_start_beats_variety();
    printf("transport_test: ALL TRANSPORT TESTS PASSED\n");
    return 0;
}
