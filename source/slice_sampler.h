/**
 * @file slice_sampler.h
 * @brief Slice sampler API and data structures.
 */

#ifndef ALO_SLICE_SAMPLER_MAX_PENDING
#define ALO_SLICE_SAMPLER_MAX_PENDING 32
#endif
#ifndef ALO_SLICE_SAMPLER_H
#define ALO_SLICE_SAMPLER_H

#include <stdbool.h> /* cppcheck-suppress missingIncludeSystem */
#include <stdint.h>  /* cppcheck-suppress missingIncludeSystem */

#ifndef ALO_SLICE_INFO_MAX
#define ALO_SLICE_INFO_MAX 64
#endif

#define ALO_SLICE_MAX_CHANNELS 2

typedef struct
{
  const float* data;     // Borrowed pointer into sampler source storage
  uint32_t     length;   // Samples per channel
  bool         valid;    // True if this slice currently maps to valid audio
  bool         borrowed; // True when data must not be freed
} AloSliceBuffer;

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALO_SLICE_SAMPLER_MAX_VOICES
#define ALO_SLICE_SAMPLER_MAX_VOICES 32
#endif

typedef struct
{
  bool     running;
  uint32_t total_frames; /* retained only for compatibility with existing code/tests */
} AloEnvState;

typedef struct
{
  bool        active;
  uint32_t    key;
  uint32_t    start_delay_samples;
  uint32_t    phase_samples;
  uint32_t    remaining_samples;
  uint32_t    total_samples;
  uint32_t    elapsed_samples;
  uint32_t    fade_samples;
  float       fade_inv;
  float       gain;
  uint32_t    attack_samples;
  float       attack_inv;
  uint32_t    release_samples;
  uint32_t    sustain_samples;
  float       release_inv;
  AloEnvState env;

  /* MIDI identity is retained only for trigger provenance/debugging.
   * Slice playback is intentionally one-shot and does not depend on note-off.
   */
  uint8_t midi_note;

  // Per-slice buffer assignment
  const float* slice_buf_l;   // Pointer to left channel buffer
  const float* slice_buf_r;   // Pointer to right channel buffer
  uint32_t     slice_buf_len; // Length in samples (per channel)
} AloSliceVoice;

typedef struct
{
  bool     active;
  uint32_t key;
  uint32_t offset_samples;
  uint32_t phase_samples;
  uint32_t length_samples;
  uint32_t fade_samples;
  float    gain;
  uint32_t slice_idx; /* precomputed index into slice_buffers to avoid div */

  /* MIDI identity is carried through pending triggers only so the eventual
   * voice can retain information about which note created it.
   */
  uint8_t midi_note;
} AloSlicePending;

typedef struct
{
  AloSliceVoice   voices[ALO_SLICE_SAMPLER_MAX_VOICES];
  AloSlicePending pending[ALO_SLICE_SAMPLER_MAX_PENDING];
  float           rate;       // Sample rate for envelope generator
  uint32_t        next_voice; /* index for round‑robin voice selection */

  /* Active and shadow slice maps. Each entry borrows pointers into the current
   * stereo sampler source buffers instead of owning copied per-slice audio.
   * Channel 0 starts at `data`; channel 1 starts at `data + length`.
   */
  AloSliceBuffer slice_buffers[ALO_SLICE_INFO_MAX];
  AloSliceBuffer slice_buffers_shadow[ALO_SLICE_INFO_MAX];
  bool           slice_buffers_using_primary; /* true = slice_buffers active */

  /* In zero-copy mode this stores slice metadata only: channel count and the
   * nominal max slice span used for phase-to-slice lookup.
   */
  uint32_t slice_buffer_channels; // 2 for stereo
  uint32_t slice_buffer_len;      // Max length per slice (samples)

  /* incremental-clear state used by clear_buffers() */
  bool     clear_in_progress;
  uint32_t clear_slice_idx;
  uint32_t clear_offset;
} AloSliceSampler;

struct Alo;

// Buffer management API

/**
 * @brief Allocate scratch buffers for all slices (call outside realtime).
 */
bool alo_slice_sampler_alloc_buffers(AloSliceSampler* s, uint32_t max_len, uint32_t channels);

/**
 * @brief Free previously allocated slice buffers (outside realtime).
 */
void alo_slice_sampler_free_buffers(AloSliceSampler* s);

/**
 * @brief Mark all slice buffers invalid and reset clear-state bookkeeping.
 */
void alo_slice_sampler_clear_buffers(AloSliceSampler* s);

/**
 * @brief Compatibility no-op for legacy incremental clear call sites.
 *
 * The sampler now clears buffer metadata immediately in
 * `alo_slice_sampler_clear_buffers()`, so this helper intentionally performs
 * no work and only preserves the existing API surface.
 */
void alo_slice_sampler_step_clear(AloSliceSampler* s, uint32_t max_samples);

void alo_slice_sampler_reset(AloSliceSampler* s);

// schedule from Alo context (allows buffer lookup)
void alo_slice_sampler_schedule(AloSliceSampler* s, const struct Alo* alo,
                                uint32_t start_offset_samples, uint32_t phase_samples,
                                uint32_t length_samples, uint32_t fade_samples, float gain,
                                uint8_t midi_note);

void alo_slice_sampler_begin_block(AloSliceSampler* s, uint32_t n_samples);

bool alo_slice_sampler_is_busy(const AloSliceSampler* s);

/* Render a sub-range of the current host block.
 * block_offset_samples is the offset within the host block (0..block_len-1).
 * Pending trigger offsets remain relative to the full host block.
 */
void alo_slice_sampler_process_chunk(AloSliceSampler* s, const struct Alo* alo,
                                     const uint32_t block_offset_samples, const uint32_t n_samples,
                                     float* out_l, float* out_r);

#ifdef __cplusplus
}
#endif

#endif /* ALO_SLICE_SAMPLER_H */
