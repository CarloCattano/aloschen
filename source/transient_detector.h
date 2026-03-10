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
 *
 * In addition to a boolean trigger, the detector tracks lightweight onset
 * metrics so callers can refine slice boundaries:
 * - current absolute sample amplitude
 * - instantaneous onset ratio (fast_env / slow_env)
 * - peak amplitude and peak ratio seen within the active onset candidate
 * - onset start / peak / end sample positions for the current candidate
 */

/* default debounce interval used by callers who don't specify one */
#define TD_DEFAULT_DEBOUNCE_MS 5.0f
#define TD_DEFAULT_MIN_BURST_MS 20.0f
#define TD_DEFAULT_END_RATIO 1.1f

typedef struct {
    uint32_t start_sample;
    uint32_t peak_sample;
    uint32_t end_sample;
    float peak_amplitude;
    float peak_ratio;
    bool active;
    bool complete;
} TransientCandidate;

typedef struct {
    bool triggered;
    bool candidate_complete;
    float amplitude;
    float onset_ratio;
    uint32_t sample_index;
    TransientCandidate candidate;
} TransientDetectorEvent;

typedef struct {
    float fast_env;
    float slow_env;

    /* precomputed filter coefficients */
    float fast_coeff;
    float slow_coeff;

    /* when fast_env > slow_env * threshold_ratio ⇒ trigger */
    float threshold_ratio;
    float end_ratio;

    /* simple sample-based debounce */
    uint32_t debounce_counter;
    uint32_t debounce_samples;

    /* candidate tracking */
    uint32_t sample_index;
    uint32_t min_burst_samples;
    float current_amplitude;
    float current_ratio;
    TransientCandidate candidate;
    TransientCandidate last_completed_candidate;
} TransientDetector;

/*
 * Initialize the detector. "sample_rate" is used to compute the internal
 * envelope coefficients based on the attack/decay times chosen below.
 * "threshold_ratio" is typically > 1.0 (e.g. 2.0 means fast_env must be twice
 * the slow_env to fire). "debounce_time_ms" is the minimum allowed time
 * between triggers in milliseconds.
 */
void td_init(TransientDetector* td,
             float sample_rate,
             float threshold_ratio,
             float debounce_time_ms);

/*
 * Extended initializer that lets callers control candidate completion rules.
 * "min_burst_time_ms" rejects very short bursts/noise before they become
 * usable onset regions. "end_ratio" is the ratio below which an active onset
 * candidate is considered finished.
 */
void td_init_ex(TransientDetector* td,
                float sample_rate,
                float threshold_ratio,
                float debounce_time_ms,
                float min_burst_time_ms,
                float end_ratio);

/*
 * Process a single sample. Returns true if a transient was detected on this
 * sample. The detector maintains internal state (envelopes and debounce)
 * and must be reinitialized before reuse on a new stream.
 */
bool td_process_sample(TransientDetector* td, float sample);

/*
 * Extended processing API. Returns a snapshot of the detector state for the
 * current sample, including whether a trigger fired and whether an onset
 * candidate just completed.
 */
TransientDetectorEvent td_process_sample_ex(TransientDetector* td, float sample);

/* Accessors for the most recent completed onset candidate and live metrics. */
const TransientCandidate* td_last_completed_candidate(const TransientDetector* td);
const TransientCandidate* td_current_candidate(const TransientDetector* td);
float td_current_onset_ratio(const TransientDetector* td);
float td_current_amplitude(const TransientDetector* td);

#endif /* TRANSIENT_DETECTOR_H */