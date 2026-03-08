#ifndef SAMPLER_CACHE_H
#define SAMPLER_CACHE_H

#include "alo_engine.h"

/* Helpers to manage the "sampler_src" full‑loop stereo mix cache.  The
 * cache is rebuilt incrementally on the audio thread; a shadow buffer is used
 * so MIDI slice voices remain uninterrupted while the new mix is being prepared.
 */

/* Perform rebuild/update actions for this block.  "any_committed_audio" should
 * be true if at least one loop slot currently contains audio.
 * `n_samples` is the host block size; this function will copy at most a bounded
 * amount of data to avoid overruns.
 */
void sampler_cache_process(Alo* self, uint32_t n_samples, bool any_committed_audio);

/* Update slice-sampler buffers so that playback voices will read from the
 * correct regions of the loop according to the current slicing mode and
 * transient-detection results.  This may be called on either the audio
 * thread or from unit tests; the cache must be valid (sampler_src_valid true)
 * otherwise the call is a no-op.  Both the primary and shadow buffer sets are
 * populated so the sampler can swap safely during a cache rebuild.
 */
void sampler_cache_update_slice_buffers(Alo* self);

#endif // SAMPLER_CACHE_H
