#ifndef ALO_SLICE_SAMPLER_H
#define ALO_SLICE_SAMPLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALO_SLICE_SAMPLER_MAX_VOICES
#define ALO_SLICE_SAMPLER_MAX_VOICES 8
#endif

#ifndef ALO_SLICE_SAMPLER_MAX_PENDING
#define ALO_SLICE_SAMPLER_MAX_PENDING 32
#endif


typedef struct {
  bool active;
  uint32_t key;
  uint32_t start_delay_samples;
  uint32_t phase_samples;
  uint32_t remaining_samples;
  uint32_t total_samples;
  uint32_t elapsed_samples;
  uint32_t fade_samples;
  float fade_inv;
  float gain;
} AloSliceVoice;

typedef struct {
  bool active;
  uint32_t key;
  uint32_t offset_samples;
  uint32_t phase_samples;
  uint32_t length_samples;
  uint32_t fade_samples;
  float gain;
} AloSlicePending;

typedef struct {
  AloSliceVoice voices[ALO_SLICE_SAMPLER_MAX_VOICES];
  AloSlicePending pending[ALO_SLICE_SAMPLER_MAX_PENDING];
} AloSliceSampler;

struct Alo;

void alo_slice_sampler_reset(AloSliceSampler* s);

void alo_slice_sampler_schedule(AloSliceSampler* s,
                               uint32_t start_offset_samples,
                               uint32_t phase_samples,
                               uint32_t length_samples,
                               uint32_t fade_samples,
                               float gain);

void alo_slice_sampler_begin_block(AloSliceSampler* s, uint32_t n_samples);

bool alo_slice_sampler_is_busy(const AloSliceSampler* s);

void alo_slice_sampler_process_block(AloSliceSampler* s,
                                    const struct Alo* alo,
                                    const float track_gain_3[3],
                                    uint32_t n_samples,
                                    float* out_l,
                                    float* out_r);

/* Render a sub-range of the current host block.
 * block_offset_samples is the offset within the host block (0..block_len-1).
 * Pending trigger offsets remain relative to the full host block.
 */
void alo_slice_sampler_process_chunk(AloSliceSampler* s,
                                    const struct Alo* alo,
                                    const float track_gain_3[3],
                                    const uint32_t block_offset_samples,
                                    const uint32_t n_samples,
                                    float* out_l,
                                    float* out_r);

void alo_slice_sampler_process_sample(AloSliceSampler* s,
                                     const struct Alo* alo,
                                     const float track_gain_3[3],
                                     float* out_l,
                                     float* out_r);

#ifdef __cplusplus
}
#endif

#endif /* ALO_SLICE_SAMPLER_H */
