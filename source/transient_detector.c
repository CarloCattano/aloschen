#include "transient_detector.h"
#include <math.h>

/* Typical time constants as described in the agentic instructions. */
#define TD_FAST_ATTACK_MS 1.0f   /* ~1ms for onset detection */
#define TD_SLOW_DECAY_MS 50.0f   /* ~50ms to follow background */

/* 5ms debounce default is used in multiple places. */
#define TD_DEFAULT_DEBOUNCE_MS 5.0f

void td_init(TransientDetector *td,
             float sample_rate,
             float threshold_ratio,
             float debounce_time_ms)
{
    if (!td) {
        return;
    }

    td->fast_env = 0.0f;
    td->slow_env = 0.0f;

    /* compute coefficients using standard one-pole formula: alpha = exp(-1/(T*fs)) */
    float fast_time_s = TD_FAST_ATTACK_MS * 0.001f;
    float slow_time_s = TD_SLOW_DECAY_MS  * 0.001f;

    td->fast_coeff = expf(-1.0f / (fast_time_s * sample_rate));
    td->slow_coeff = expf(-1.0f / (slow_time_s * sample_rate));

    td->threshold_ratio = threshold_ratio;

    td->debounce_counter = 0;
    td->debounce_samples = (uint32_t)((debounce_time_ms * 0.001f) * sample_rate + 0.5f);
}

bool td_process_sample(TransientDetector *td, float sample)
{
    if (!td) {
        return false;
    }

    float x = sample;
    if (x < 0.0f) x = -x;

    /* update envelopes */
    td->fast_env = td->fast_coeff * td->fast_env + (1.0f - td->fast_coeff) * x;
    td->slow_env = td->slow_coeff * td->slow_env + (1.0f - td->slow_coeff) * x;

    bool triggered = false;
    if (td->debounce_counter == 0) {
        if (td->fast_env > td->slow_env * td->threshold_ratio) {
            triggered = true;
            td->debounce_counter = td->debounce_samples;
        }
    }

    if (td->debounce_counter > 0) {
        td->debounce_counter--;
    }

    return triggered;
}
