#ifndef ALO_SLICE_SAMPLER_MAX_PENDING
#define ALO_SLICE_SAMPLER_MAX_PENDING 8
#endif
#ifndef ALO_SLICE_SAMPLER_H
#define ALO_SLICE_SAMPLER_H

#include <stdbool.h> /* cppcheck-suppress missingIncludeSystem */
#include <stdint.h> /* cppcheck-suppress missingIncludeSystem */

#ifndef ALO_SLICE_INFO_MAX
#define ALO_SLICE_INFO_MAX 64
#endif

#define ALO_SLICE_MAX_CHANNELS 2
#define ALO_SLICE_MAX_SLICES ALO_SLICE_INFO_MAX
#define ALO_SLICE_MAX_SLICE_SAMPLES LOOP_SIZE

typedef struct
{
  uint32_t duration_samples;
  float    beat_position;
  uint32_t start_offset_samples;
  float*   audio_buf[ALO_SLICE_MAX_CHANNELS]; // [ch][sample]
  uint32_t audio_len;                         // samples per channel
  bool     audio_valid;
} AloSliceInfo;

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALO_SLICE_SAMPLER_MAX_VOICES
#define ALO_SLICE_SAMPLER_MAX_VOICES 8
#endif

typedef enum
{
  ENV_IDLE = 0,
  ENV_ATTACK,
  ENV_DECAY,
  ENV_SUSTAIN,
  ENV_RELEASE,
  ENV_END
} AloEnvStage;

typedef struct
{
  bool        running;
  AloEnvStage stage;
  float       phase;
  float       delta;
  float       value;
  float       c1, c0; /* attack / release length in frames */
  uint32_t    frames;
  uint32_t    total_frames; /* total duration of the voice (samples) */
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
  AloEnvState env;

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
} AloSlicePending;

typedef struct
{
  float*   data;   // Pointer to buffer (allocated at init)
  uint32_t length; // Length in samples (stereo: L/R interleaved or separate)
  bool     valid;  // True if buffer contains valid audio
} AloSliceBuffer;

typedef struct
{
  AloSliceVoice   voices[ALO_SLICE_SAMPLER_MAX_VOICES];
  AloSlicePending pending[ALO_SLICE_SAMPLER_MAX_PENDING];
  AloSliceInfo    slice_info[ALO_SLICE_INFO_MAX];
  uint32_t        slice_info_count;
  float           rate; // Sample rate for envelope generator

  /* Primary per-slice audio buffers that voices read from. */
  AloSliceBuffer slice_buffers[ALO_SLICE_INFO_MAX]; // Per-slice audio buffers
  /* Shadow copy used during a mix rebuild.  We write the new slice data here
   * and only swap it in when the rebuild finishes.  This keeps the active set
   * intact for playing voices.
   */
  AloSliceBuffer slice_buffers_shadow[ALO_SLICE_INFO_MAX];
  bool           slice_buffers_using_primary; /* true = slice_buffers active */

  uint32_t slice_buffer_channels; // 2 for stereo
  uint32_t slice_buffer_len;      // Max length per slice (samples)
  float*   slice_audio[ALO_SLICE_MAX_SLICES][ALO_SLICE_MAX_CHANNELS]; // [slice][ch][sample]
  float*   slice_audio_shadow[ALO_SLICE_MAX_SLICES][ALO_SLICE_MAX_CHANNELS];
  uint32_t slice_audio_len[ALO_SLICE_MAX_SLICES];
  uint32_t slice_audio_len_shadow[ALO_SLICE_MAX_SLICES];
  bool     slice_audio_valid[ALO_SLICE_MAX_SLICES];
  bool     slice_audio_valid_shadow[ALO_SLICE_MAX_SLICES];
  /* incremental-clear state used by clear_buffers() */
  bool     clear_in_progress;
  uint32_t clear_slice_idx;
  uint32_t clear_offset;
} AloSliceSampler;

struct Alo;

// Buffer management API

// Call at init/reset to allocate all slice buffers (outside RT)
bool alo_slice_sampler_alloc_buffers(AloSliceSampler* s, uint32_t max_len, uint32_t channels);
// Call to free all buffers (outside RT)
void alo_slice_sampler_free_buffers(AloSliceSampler* s);
// Mark all slice buffers invalid (starts an incremental clear)
void alo_slice_sampler_clear_buffers(AloSliceSampler* s);
// Progress an in-flight clear job by up to max_samples zero operations
void alo_slice_sampler_step_clear(AloSliceSampler* s, uint32_t max_samples);

void alo_slice_sampler_reset(AloSliceSampler* s);

// schedule from Alo context (allows buffer lookup)
void alo_slice_sampler_schedule(AloSliceSampler* s, const struct Alo* alo,
                                uint32_t start_offset_samples, uint32_t phase_samples,
                                uint32_t length_samples, uint32_t fade_samples, float gain);

void alo_slice_sampler_begin_block(AloSliceSampler* s, uint32_t n_samples);

bool alo_slice_sampler_is_busy(const AloSliceSampler* s);

/* Render a sub-range of the current host block.
 * block_offset_samples is the offset within the host block (0..block_len-1).
 * Pending trigger offsets remain relative to the full host block.
 */
void alo_slice_sampler_process_chunk(AloSliceSampler* s, const struct Alo* alo,
                                     const uint32_t block_offset_samples, const uint32_t n_samples,
                                     float* out_l, float* out_r);

void alo_slice_sampler_process_sample(AloSliceSampler* s, const struct Alo* alo,
                                      const float track_gain_3[3], float* out_l, float* out_r);


#ifdef __cplusplus
}
#endif

#endif /* ALO_SLICE_SAMPLER_H */
