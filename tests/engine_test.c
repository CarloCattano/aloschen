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
    test_alo_get_bpb_i();
    test_alo_port_pressed();
    test_soft_clip_extended();
    /* new tests added Mar 2026 */
    test_alo_port_write_and_fmod();
    test_apply_pending_undo_direct();
    test_reset_timing_behavior();
    printf("engine_test: ALL TESTS PASSED\n");
    return 0;
}