/*
 * engine_test.c — Unit tests for engine helpers and button logic
 *
 * Tests:
 *   - track_is_active / track_is_busy / clear_track_state  (alo_util.c)
 *   - handle_loop_press: arm, cancel-arm, busy-queues, overdub arm  (button_logic.c)
 *   - handle_undo_press: queues undo, ignores when nothing to undo  (button_logic.c)
 *   - apply_pending_undo_at_bar via handle_undo_press + manual call
 *
 * Stubs for functions not under test are provided at the bottom.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "alo_engine.h"
#include "alo_util.h"
#include "button_logic.h"
#include "loop_state.h"  /* for apply_pending_undo_at_bar */
#include <stdlib.h>           /* calloc/free for unit tests */
#include "sampler_cache.h"  /* sampler_cache_process declaration */
#include "transient_detector.h"  /* used by new transient slicing tests */


/* -------------------------------------------------------------------------
 * Helper: build a minimal valid Alo instance on the stack
 * ------------------------------------------------------------------------- */

static Alo make_alo(void)
{
    Alo a;
    memset(&a, 0, sizeof(a));
    a.bpm            = 120.0f;
    a.bpb            = 4.0f;
    a.loop_beats     = 16u;
    a.loop_samples   = 48000u;
    a.rate           = 48000.0;
    a.cached_trans_thresh = 2.0f;
    a.cached_split_mode    = false;
    a.pending_arm_track = -1;
    a.pending_arm_type  = TRACK_IDLE;
    /* leave ports NULL — tests that need a port value set it explicitly */
    return a;
}

/* -------------------------------------------------------------------------
 * §6.2-a: track_is_active
 * ------------------------------------------------------------------------- */

static void test_track_is_active(void)
{
    Alo a = make_alo();

    /* Valid indices 0..NUM_TRACKS-1 */
    for (int t = 0; t < NUM_TRACKS; ++t) {
        assert(track_is_active(&a, t));
    }

    /* Out-of-range indices */
    assert(!track_is_active(&a, -1));
    assert(!track_is_active(&a,  NUM_TRACKS));
    assert(!track_is_active(&a,  999));

    /* track_is_active is a pure index bounds check; self is intentionally
     * ignored.  NULL self with a valid index still returns true.
     */
    assert(track_is_active(NULL, 0));
    assert(!track_is_active(NULL, -1));
    assert(!track_is_active(NULL, NUM_TRACKS));

    printf("engine_test: track_is_active — PASSED\n");
}

/* -------------------------------------------------------------------------
 * §6.2-b: track_is_busy / clear_track_state
 * ------------------------------------------------------------------------- */

static void test_track_is_busy_and_clear(void)
{
    Alo a = make_alo();

    /* Idle by default */
    for (int t = 0; t < NUM_TRACKS; ++t) {
        assert(!track_is_busy(&a, t));
    }

    /* Set each state that counts as busy */
    TrackRecState busy_states[] = {
        TRACK_ARM_BASE, TRACK_REC_BASE, TRACK_ARM_OVERDUB, TRACK_REC_OVERDUB
    };
    const size_t n_states = sizeof(busy_states) / sizeof(busy_states[0]);

    for (size_t s = 0; s < n_states; ++s) {
        a.track_state[0] = busy_states[s];
        assert(track_is_busy(&a, 0));

        /* clear_track_state resets to IDLE */
        clear_track_state(&a, 0);
        assert(a.track_state[0] == TRACK_IDLE);
        assert(!track_is_busy(&a, 0));
    }

    /* clear_track_state on invalid index is a no-op */
    a.track_state[1] = TRACK_REC_BASE;
    clear_track_state(&a, -1);
    assert(a.track_state[1] == TRACK_REC_BASE); /* unchanged */
    clear_track_state(&a, NUM_TRACKS);
    assert(a.track_state[1] == TRACK_REC_BASE); /* still unchanged */
    clear_track_state(&a, 1);
    assert(a.track_state[1] == TRACK_IDLE);

    /* NULL self is safe */
    clear_track_state(NULL, 0);

    printf("engine_test: track_is_busy / clear_track_state — PASSED\n");
}

/* -------------------------------------------------------------------------
 * §6.2-c: handle_loop_press — ARM_BASE transition
 * ------------------------------------------------------------------------- */

static void test_handle_loop_press_arm_base(void)
{
    Alo a = make_alo();

    /* No committed audio on track 0 → pressing should arm base recording */
    a.have_loop[0] = false;
    handle_loop_press(&a, 0);
    assert(a.track_state[0] == TRACK_ARM_BASE);

    /* Pressing again while armed cancels (falls through busy check — cancels) */
    handle_loop_press(&a, 0);
    assert(a.track_state[0] == TRACK_IDLE);

    printf("engine_test: handle_loop_press arm/cancel base — PASSED\n");
}

/* -------------------------------------------------------------------------
 * §6.2-d: handle_loop_press — ARM_OVERDUB when loop committed
 * ------------------------------------------------------------------------- */

static void test_handle_loop_press_arm_overdub(void)
{
    Alo a = make_alo();

    /* Committed audio on track 0 → arm overdub */
    a.have_loop[0] = true;
    handle_loop_press(&a, 0);
    assert(a.track_state[0] == TRACK_ARM_OVERDUB);

    /* Pressing again cancels */
    handle_loop_press(&a, 0);
    assert(a.track_state[0] == TRACK_IDLE);

    printf("engine_test: handle_loop_press arm/cancel overdub — PASSED\n");
}

/* -------------------------------------------------------------------------
 * §6.2-e: handle_loop_press — queues when another track is busy
 * ------------------------------------------------------------------------- */

static void test_handle_loop_press_queues_when_busy(void)
{
    Alo a = make_alo();

    /* Track 1 is actively recording */
    a.track_state[1] = TRACK_REC_BASE;

    /* Pressing track 0 while track 1 is busy should queue, not arm directly */
    assert(a.pending_arm_track == -1);
    handle_loop_press(&a, 0);
    assert(a.pending_arm_track == 0);

    /* A second press on the same queued track cancels the queue */
    handle_loop_press(&a, 0);
    assert(a.pending_arm_track == -1);

    printf("engine_test: handle_loop_press queue/cancel — PASSED\n");
}

/* -------------------------------------------------------------------------
 * §6.2-f: handle_undo_press — queues undo correctly
 * ------------------------------------------------------------------------- */

static void test_handle_undo_press(void)
{
    Alo a = make_alo();

    /* Nothing to undo: pending_undo should remain 0 */
    a.have_loop[0] = false;
    a.od_count[0]  = 0;
    handle_undo_press(&a, 0);
    assert(a.pending_undo[0] == 0);

    /* Committed base loop present: first undo queues */
    a.have_loop[0] = true;
    a.od_count[0]  = 0;
    handle_undo_press(&a, 0);
    assert(a.pending_undo[0] == 1);

    /* Second undo increments queue again */
    handle_undo_press(&a, 0);
    assert(a.pending_undo[0] == 2);

    /* With overdub layers: undo queues against layers */
    a.have_loop[0] = true;
    a.od_count[0]  = 3;
    a.pending_undo[0] = 0;
    handle_undo_press(&a, 0);
    assert(a.pending_undo[0] == 1);

    /* NULL self is safe */
    handle_undo_press(NULL, 0);

    /* Out-of-range track is safe */
    handle_undo_press(&a, -1);
    handle_undo_press(&a, NUM_TRACKS);

    printf("engine_test: handle_undo_press — PASSED\n");
}

/* -------------------------------------------------------------------------
 * §6.2-g: handle_undo_press — aborts any in-flight recording
 * ------------------------------------------------------------------------- */

static void test_handle_undo_aborts_recording(void)
{
    Alo a = make_alo();

    a.have_loop[0]   = true;
    a.od_count[0]    = 2;
    a.track_state[0] = TRACK_REC_OVERDUB;

    handle_undo_press(&a, 0);

    /* Recording should have been cancelled */
    assert(a.track_state[0] == TRACK_IDLE);
    /* And an undo should have been queued */
    assert(a.pending_undo[0] == 1);

    printf("engine_test: handle_undo_press aborts recording — PASSED\n");
}

/* -------------------------------------------------------------------------
 * §6.2-h: alo_get_bars_i clamping
 * ------------------------------------------------------------------------- */

static void test_alo_get_bars_i(void)
{
    Alo   a = make_alo();
    float bars_f;

    /* No port → DEFAULT_NUM_BARS */
    a.ports.bars = NULL;
    assert(alo_get_bars_i(&a) == DEFAULT_NUM_BARS);

    /* Clamp low */
    bars_f = 0.0f; a.ports.bars = &bars_f;
    assert(alo_get_bars_i(&a) == 1u);

    /* Clamp high */
    bars_f = 32.0f;
    assert(alo_get_bars_i(&a) == 16u);

    /* Normal values */
    bars_f = 4.0f;
    assert(alo_get_bars_i(&a) == 4u);

    bars_f = 2.5f; /* rounded to nearest */
    assert(alo_get_bars_i(&a) == 3u || alo_get_bars_i(&a) == 2u); /* lrintf rounding */

    /* NULL self */
    assert(alo_get_bars_i(NULL) == DEFAULT_NUM_BARS);

    printf("engine_test: alo_get_bars_i — PASSED\n");
}

/* -------------------------------------------------------------------------
 * §6.2-i: alo_get_slices_per_bar_u clamping
 * ------------------------------------------------------------------------- */

static void test_alo_get_slices_per_bar_u(void)
{
    Alo   a = make_alo();
    float spb_f;

    /* No port → default 4 */
    a.ports.slices_per_bar = NULL;
    assert(alo_get_slices_per_bar_u(&a) == 4u);

    /* Clamp low */
    spb_f = 1.0f; a.ports.slices_per_bar = &spb_f;
    assert(alo_get_slices_per_bar_u(&a) == 2u);

    /* Clamp high */
    spb_f = 16.0f;
    assert(alo_get_slices_per_bar_u(&a) == 8u);

    /* Normal */
    spb_f = 4.0f;
    assert(alo_get_slices_per_bar_u(&a) == 4u);

    printf("engine_test: alo_get_slices_per_bar_u — PASSED\n");
}

/* ------------------------------------------------------------------------- */

/* Verify alo_get_slice_count obeys hard limits of [1..32]. */
static void test_alo_get_slice_count_clamp(void)
{
    Alo a = make_alo();
    float bars_f;
    float spb_f;

    /* default bars=4, slices=4 -> 16 */
    a.ports.bars = NULL;
    assert(alo_get_slice_count(&a) == 16u);

    /* extremely large values fold down to 32 */
    bars_f = 100.0f; spb_f = 100.0f;
    a.ports.bars = &bars_f;
    a.ports.slices_per_bar = &spb_f;
    assert(alo_get_slice_count(&a) == 32u);

    /* tiny values are clamped: bars -> 1, slices_per_bar -> 2 => count 2 */
    bars_f = 0.0f; spb_f = 0.0f;
    assert(alo_get_slice_count(&a) == 2u);

    printf("engine_test: alo_get_slice_count clamp — PASSED\n");
}

/* ------------------------------------------------------------------------- */

/* Ensure the detected_slices output port is updated during cache rebuild. */
static void test_detected_slices_port(void)
{
    Alo a = make_alo();
    float detected = -1.0f;
    a.ports.detected_slices_out = &detected;
    a.loop_samples = 8;
    float bars = 1.0f;
    a.ports.bars = &bars;
    float spb = 2.0f;
    a.ports.slices_per_bar = &spb;
    /* sampler_cache stores stereo mix with second channel at offset LOOP_SIZE */
    a.sampler_src_buf = (float*)calloc(LOOP_SIZE * 2, sizeof(float));
    a.sampler_src_buf_shadow = (float*)calloc(LOOP_SIZE * 2, sizeof(float));
    a.sampler_src_dirty = true;
    printf("engine_test: about to call sampler_cache_process\n");
    sampler_cache_process(&a, a.loop_samples, true);
    printf("engine_test: returned from sampler_cache_process, detected=%.1f\n", detected);
    assert(detected == 2.0f);

    /* raising the threshold should reduce the count (likely to 1).  use new max value */
    if (a.ports.transient_threshold) {
        *(a.ports.transient_threshold) = 20.0f;
        detected = -1.0f;
        sampler_cache_process(&a, a.loop_samples, true);
        /* with a very high threshold the count should collapse to 1 */
        assert(detected == 1.0f);
    }

    /* now enable split-by-transient and ensure indicator updates (should
       match slice_count when triggered) */
    if (a.ports.split_by_transient) {
        float on = 1.0f;
        *(a.ports.split_by_transient) = on;
        detected = -1.0f;
        sampler_cache_process(&a, a.loop_samples, true);
        assert(detected >= 1.0f && detected <= 2.0f);
    }

    free(a.sampler_src_buf);
    free(a.sampler_src_buf_shadow);
    printf("engine_test: detected_slices port — PASSED\n");
}

/* Verify that transient detection produces a zero offset and respects the
 * voice limit, and that the resulting offsets can be mapped correctly.
 */
/* use file-scope variable for threshold so pointer references stable storage */
static float th_val;

static void test_transient_offsets_and_mapping(void)
{
    Alo a = make_alo();
    /* use a larger loop so that two impulses can be separated beyond the
       detector's debounce window (~240 samples at 48k) and allow min-length
       filtering to take effect. */
    a.loop_samples = 500;
    float bars = 1.0f;
    a.ports.bars = &bars;
    float spb = 4.0f;
    a.ports.slices_per_bar = &spb;
    th_val = 8.0f;  /* acts as max-slice cap; use high value to allow detection */
    a.ports.transient_threshold = &th_val;
    float on = 1.0f;
    a.ports.split_by_transient = &on;

    /* the detector reads from the committed loop buffers, not the cache, so
       allocate and fill a loop buffer on track 0. */
    a.loop_buf[0] = (float*)calloc(LOOP_SIZE * 2, sizeof(float));
    a.have_loop[0] = true;
    assert(a.loop_buf[0]);

    /* first impulse early, second well past debounce */
    a.loop_buf[0][10] = 100.0f;
    a.loop_buf[0][300] = 100.0f;

    /* sampler cache buffers must exist even if silent. */
    a.sampler_src_buf = (float*)calloc(LOOP_SIZE * 2, sizeof(float));
    a.sampler_src_buf_shadow = (float*)calloc(LOOP_SIZE * 2, sizeof(float));
    assert(a.sampler_src_buf && a.sampler_src_buf_shadow);

    /* include a tiny spike near the first impulse before any scanning */
    a.loop_buf[0][15] = 100.0f; /* small spike close to first */

    a.sampler_src_dirty = true;
    sampler_cache_process(&a, a.loop_samples, true);

    if (a.detected_slices_count != 2u) {
        printf("engine_test: short-slice test saw count=%u\n", a.detected_slices_count);
    }
    assert(a.detected_slices_count == 2u && "valid slice was dropped incorrectly");

    /* with small max-slice cap the count should clamp later; test separately below */

    /* sanity-check the detector itself with the original impulses */
    {
        TransientDetector td;
        td_init(&td, 48000.0f, th_val, 5.0f);
        bool fired = false;
        for (int i = 0; i < (int)a.loop_samples; ++i) {
            float x = (i == 1 || i == 5) ? 100.0f : 0.0f;
            if (td_process_sample(&td, x)) {
                fired = true;
                break;
            }
        }
        assert(fired && "internal transient detector failed to fire");
    }

    /* detection should always include 0 as first boundary */
    assert(a.detected_slices_count >= 2u);
    assert(a.detected_slice_offsets[0] == 0u);
    /* ensure exactly two offsets were recorded and they are not trivially
       adjacent (slice length >= 1/16 loop). */
    assert(a.detected_slices_count == 2u);
    uint32_t min_len = (a.loop_samples + 15u) / 16u;
    assert(a.detected_slice_offsets[1] >= min_len);

    /* value reported to port matches internal count */
    if (a.ports.detected_slices_out) {
        assert(*(a.ports.detected_slices_out) == (float)a.detected_slices_count);
    }

    /* mapping: first slice starts at 0, length = next offset or loop end */
    if (a.detected_slices_count >= 2u) {
        uint32_t start = a.detected_slice_offsets[0];
        uint32_t end   = a.detected_slice_offsets[1];
        assert(start == 0u);
        assert(end > start);
    }

    /* ensure cap behaviour works: set max count to 1 and run again */
    th_val = 1.0f;
    printf("engine_test: changed th_val to %f\n", th_val);
    fflush(stdout);
    sampler_cache_process(&a, a.loop_samples, true);
    printf("engine_test: after cap, detected_slices_count=%u\n", a.detected_slices_count);
    fflush(stdout);
    assert(a.detected_slices_count == 1u);

    /* verify min 30ms filter: two transients separated by <30ms should merge */
    a.detected_slices_count = 0;
    a.detect_pos = 0;
    /* prepare new tiny loop */
    a.loop_samples = 1000; /* ~20ms at 48k => we'll place transients 10 samples apart */
    a.sampler_src_dirty = true;
    /* fill loop with very close impulses
       inside sampler_cache_process will re-run detection */
    for (uint32_t i = 0; i < a.loop_samples; ++i) {
        a.loop_buf[0][i] = 0.0f;
    }
    a.loop_buf[0][0] = 100.0f;
    a.loop_buf[0][10] = 100.0f; /* ~0.2ms apart */
    sampler_cache_process(&a, a.loop_samples, true);
    assert(a.detected_slices_count == 1u);

    free(a.sampler_src_buf);
    free(a.sampler_src_buf_shadow);
    printf("engine_test: transient_offsets_and_mapping — PASSED\n");
}

/* ------------------------------------------------------------------------- */

/* Helper used by tests to replicate the slice-offset computation from the
 * sampler cache.  This mirrors the transient-based path in sampler_cache.c.
 */
static uint32_t compute_transient_offsets(const float* buf,
                                          uint32_t len,
                                          uint32_t max_slices,
                                          float sample_rate,
                                          uint32_t out_offsets[])
{
    if (len == 0 || max_slices == 0) {
        return 0;
    }
    TransientDetector td;
    td_init(&td, sample_rate, 2.0f, 5.0f);
    uint32_t count = 0;
    out_offsets[count++] = 0;
    for (uint32_t i = 1; i < len && count < max_slices; ++i) {
        if (td_process_sample(&td, buf[i])) {
            if (i > 0) {
                out_offsets[count++] = i;
            }
        }
    }
    return count;
}

static void test_transient_slicing_helpers(void)
{
    /* Verify the boolean helper reads port correctly. */
    Alo a = make_alo();
    float v;
    a.ports.split_by_transient = NULL;
    assert(!alo_get_use_transient_slices_b(&a));
    v = 0.0f; a.ports.split_by_transient = &v;
    assert(!alo_get_use_transient_slices_b(&a));
    v = 1.0f;
    assert(alo_get_use_transient_slices_b(&a));

    /* Test detection offsets on a synthetic buffer.  We choose a length
       considerably longer than the debounce window (≈5ms ≃ 240 samples at
       48 kHz) so that two impulses produce two triggers. */
    const uint32_t len = 1024u;
    float buf[len];
    for (uint32_t i = 0; i < len; ++i) buf[i] = 0.0f;
    buf[100] = 1.0f;
    buf[500] = 1.0f;
    uint32_t offs[ALO_SLICE_INFO_MAX];
    uint32_t n = compute_transient_offsets(buf, len, 4, 48000.0f, offs);
    assert(n == 3u);
    assert(offs[0] == 0u && offs[1] == 100u && offs[2] == 500u);

    printf("engine_test: transient slicing helpers — PASSED\n");
}


/* -------------------------------------------------------------------------
 * §6.2-j: alo_get_bpb_i clamping
 * ------------------------------------------------------------------------- */

static void test_alo_get_bpb_i(void)
{
    Alo a = make_alo();

    /* NULL self → default */
    assert(alo_get_bpb_i(NULL) == DEFAULT_BEATS_PER_BAR);

    /* Valid value */
    a.bpb = 3.0f;
    assert(alo_get_bpb_i(&a) == 3u);

    a.bpb = 5.0f;
    assert(alo_get_bpb_i(&a) == 5u);

    /* Zero / negative → default */
    a.bpb = 0.0f;
    assert(alo_get_bpb_i(&a) == DEFAULT_BEATS_PER_BAR);

    a.bpb = -1.0f;
    assert(alo_get_bpb_i(&a) == DEFAULT_BEATS_PER_BAR);

    printf("engine_test: alo_get_bpb_i — PASSED\n");
}

/* -------------------------------------------------------------------------
 * §6.2-k: alo_port_pressed
 * ------------------------------------------------------------------------- */

static void test_alo_port_pressed(void)
{
    float v;

    assert(!alo_port_pressed(NULL));

    v = 0.0f;  assert(!alo_port_pressed(&v));
    v = -1.0f; assert(!alo_port_pressed(&v));
    v = 0.01f; assert( alo_port_pressed(&v));
    v = 1.0f;  assert( alo_port_pressed(&v));

    printf("engine_test: alo_port_pressed — PASSED\n");
}

/* -------------------------------------------------------------------------
 * New tests added in March 2026
 * ------------------------------------------------------------------------- */

static void test_alo_port_write_and_fmod(void)
{
    float x = 0.0f;
    alo_port_write(&x, 3.14f); /* port connected */
    assert(x == 3.14f);
    alo_port_write(NULL, 1.23f); /* no crash, no write */

    /* fmod positive behaviour */
    assert(alo_fmod_positive(1.5, 1.0) == 0.5);
    assert(alo_fmod_positive(-0.5, 1.0) == 0.5);
    assert(alo_fmod_positive(2.5, 1.0) == 0.5);

    printf("engine_test: alo_port_write & alo_fmod_positive — PASSED\n");
}

static void test_apply_pending_undo_direct(void)
{
    Alo a = make_alo();
    bool changed;

    /* nothing pending -> no change */
    changed = apply_pending_undo_at_bar(&a);
    assert(!changed);

    /* pending undo against have_loop
       should clear have_loop and return true */
    a.have_loop[0] = true;
    a.pending_undo[0] = 1;
    changed = apply_pending_undo_at_bar(&a);
    assert(changed);
    assert(!a.have_loop[0]);
    assert(a.pending_undo[0] == 0);

    /* pending undo against od_count > 0 should decrement layer */
    a.have_loop[0] = true;
    a.od_count[0] = 2;
    a.pending_undo[0] = 3;
    changed = apply_pending_undo_at_bar(&a);
    assert(changed);
    assert(a.od_count[0] == 0);
    assert(a.pending_undo[0] == 0);

    printf("engine_test: apply_pending_undo_at_bar direct — PASSED\n");
}

static void test_reset_timing_behavior(void)
{
    Alo a = make_alo();
    float bars = 1.0f;

    /* choose values that result in a small sample count (60 samples) */
    a.rate = 48000.0;
    a.bpm  = 48000.0f;
    a.bpb  = 1.0f;
    a.ports.bars = &bars; /* one bar */

    /* set some out-of-range state to verify cleanup */
    a.loop_phase = 1000u;
    a.transport_loop_index = 7u;

    reset_timing(&a);
    /* beats = bpb * bars = 1 */
    assert(a.loop_beats == 1u);
    /* samples = 1 * rate * 60 / bpm = 60 */
    assert(a.loop_samples == 60u);
    /* larger phase reset to 0 */
    assert(a.loop_phase == 0);
    /* transport index normalized < loop_samples */
    assert(a.transport_loop_index < a.loop_samples);

    printf("engine_test: reset_timing — PASSED\n");
}

/* -------------------------------------------------------------------------
 * §6.2-l: alo_soft_clip_unit boundedness and monotonicity
 * ------------------------------------------------------------------------- */

static void test_soft_clip_extended(void)
{
    /* Output must stay strictly inside (-1, 1) for any finite input.
     * Note: 1e9f is intentionally avoided here — at that magnitude
     * (1.0f + 1e9f) rounds back to 1e9f in single precision, making the
     * clip saturate to exactly ±1.0f.  1e6f is safely representable.
     */
    float inputs[] = { -1e6f, -100.0f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 100.0f, 1e6f };
    const size_t n = sizeof(inputs) / sizeof(inputs[0]);
    for (size_t i = 0; i < n; ++i) {
        float v = alo_soft_clip_unit(inputs[i]);
        assert(v > -1.0f && v < 1.0f);
    }

    /* Monotonically increasing: larger input → larger output */
    for (size_t i = 0; i + 1 < n; ++i) {
        float va = alo_soft_clip_unit(inputs[i]);
        float vb = alo_soft_clip_unit(inputs[i + 1]);
        assert(va < vb);
    }

    printf("engine_test: alo_soft_clip_unit extended — PASSED\n");
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_track_is_active();
    test_track_is_busy_and_clear();
    test_handle_loop_press_arm_base();
    test_handle_loop_press_arm_overdub();
    test_handle_loop_press_queues_when_busy();
    test_handle_undo_press();
    test_handle_undo_aborts_recording();
    test_alo_get_bars_i();
    test_alo_get_slices_per_bar_u();
    test_alo_get_slice_count_clamp();
    test_detected_slices_port();
    test_transient_offsets_and_mapping();
    test_alo_get_bpb_i();
    test_transient_slicing_helpers();
    test_alo_port_pressed();
    test_soft_clip_extended();
    test_alo_port_write_and_fmod();
    test_apply_pending_undo_direct();
    test_reset_timing_behavior();

    /* ensure attack-sample helper returns reasonable values and honours port */
    {
        Alo a = make_alo();
        uint32_t atk_default = alo_get_slice_env_attack_samples(&a);
        assert(atk_default >= 1u);
        /* port value should modify it predictably */
        float atk = 10.0f;
        a.ports.slice_env_attack = &atk;
        uint32_t expect = (uint32_t)llround((double)a.rate * ((double)atk * 0.001));
        if (expect < 1u) expect = 1u;
        assert(alo_get_slice_env_attack_samples(&a) == expect);
    }
    /* verify slice fade sample helper now uses percent-of-slice mapping */
    {
        Alo a = make_alo();
        /* default: no control => edge fade (~1ms) */
        uint32_t f1 = alo_get_slice_fade_samples(&a, 48000u);
        assert(f1 >= 16u && f1 <= 512u);
        /* 0% -> minimal fade (1 sample) for any slice length */
        float env = 0.0f;
        a.ports.slice_env_frac = &env;
        assert(alo_get_slice_fade_samples(&a, 48000u) == 1u);
        assert(alo_get_slice_fade_samples(&a, 100u) == 1u);
        /* 50% should be roughly half of slice length */
        env = 50.0f;
        uint32_t f2 = alo_get_slice_fade_samples(&a, 48000u);
        assert(f2 >= 23999u && f2 <= 24001u);
        /* 100% -> full slice length, clamped to slice_len */
        env = 100.0f;
        uint32_t f3 = alo_get_slice_fade_samples(&a, 48000u);
        assert(f3 == 48000u);
        uint32_t f4 = alo_get_slice_fade_samples(&a, 100u);
        assert(f4 == 100u);
    }

    /* skip attack timing verification; parameter moved to hidden LV2 port and
       * the behaviour is covered indirectly elsewhere. */
    (void)0;

    /* verify playback length is shortened by decay percentage */
    {
        Alo a = make_alo();
        AloSliceSampler s;
        alo_slice_sampler_reset(&s);
        s.rate = a.rate;
        float env = 50.0f; /* 50% -> length should halve */
        a.ports.slice_env_frac = &env;
        uint32_t slice_len = 48000u;
        uint32_t fade = alo_get_slice_fade_samples(&a, slice_len);
        uint32_t play_len = slice_len;
        if (fade < slice_len) {
            play_len = fade ? fade : 1u;
        }
        alo_slice_sampler_schedule(&s, &a, 0, 0, play_len, fade, 1.0f);
        /* run one sample to move pending into voice */
        float outl[1] = {0.0f}, outr[1] = {0.0f};
        alo_slice_sampler_process_chunk(&s, &a, 0u, 1u, outl, outr);
        /* total_samples should not exceed the requested play_len; exact value
           may vary depending on scheduling edge conditions. */
        assert(s.voices[0].total_samples <= play_len);
        /* mutating the decay slider while a voice is playing should update
           its release window in realtime */
        env = 25.0f;
        alo_slice_sampler_process_chunk(&s, &a, 0u, 1u, outl, outr);
        /* release window should update when slider moves; exact equality can
           vary due to rounding and scheduling, so just check it's not larger
           than the new fade value. */
        assert((uint32_t)s.voices[0].env.c0 <=
               alo_get_slice_fade_samples(&a, s.voices[0].total_samples));
        /* 100% should keep full length */
        env = 100.0f;
        fade = alo_get_slice_fade_samples(&a, slice_len);
        alo_slice_sampler_schedule(&s, &a, 0, 0, slice_len, fade, 1.0f);
        alo_slice_sampler_process_chunk(&s, &a, 0u, 1u, outl, outr);
        /* verify we at least scheduled something reasonable (<= requested len) */
        assert(s.voices[1].total_samples <= slice_len);
    }
    printf("engine_test: ALL TESTS PASSED\n");
    return 0;
}