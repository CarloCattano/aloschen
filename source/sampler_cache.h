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

#endif // SAMPLER_CACHE_H
