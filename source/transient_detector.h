#ifndef TRANSIENT_DETECTOR_H
#define TRANSIENT_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

/*
 * A "fast and cheap" transient detector using two one-pole envelope followers.
 *
 * The algorithm is intended for real-time audio and therefore avoids any
 * allocations or expensive math inside the sample loop. All coefficients are
 * precomputed during initialization.
 *
 * fast_env  : short-time envelope (~1ms attack) used to catch sudden spikes.
 * slow_env  : long-time envelope (~50ms decay) to track background energy.
 * threshold_ratio : multiplier applied to slow_env to determine detection.
 * debounce_counter : prevents retriggering for a short period.
 */

typedef struct {
    float fast_env;
    float slow_env;

    /* precomputed filter coefficients */
    float fast_coeff;
    float slow_coeff;

    /* when fast_env > slow_env * threshold_ratio ⇒ trigger */
    float threshold_ratio;

    /* simple sample-based debounce */
    uint32_t debounce_counter;
    uint32_t debounce_samples;
} TransientDetector;

/*
 * Initialize the detector.  "sample_rate" is used to compute the internal
 * envelope coefficients based on the attack/decay times chosen below.
 * "threshold_ratio" is typically > 1.0 (e.g. 2.0 means fast_env must be twice
 * the slow_env to fire).  "debounce_time_ms" is the minimum allowed time
 * between triggers in milliseconds.
 */
void td_init(TransientDetector *td,
             float sample_rate,
             float threshold_ratio,
             float debounce_time_ms);

/*
 * Process a single sample.  Returns true if a transient was detected on this
 * sample.  The detector maintains internal state (envelopes and debounce)
 * and must be reinitialized before reuse on a new stream.
 */
bool td_process_sample(TransientDetector *td, float sample);

#endif /* TRANSIENT_DETECTOR_H */
