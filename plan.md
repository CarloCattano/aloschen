# Plan for RT-safe Per-Slice Buffer Allocation in Sampler

## Objective
Implement a robust, real-time safe per-slice buffer system for the sampler, ensuring:
- All audio buffers are preallocated outside the audio thread.
- Each slice has its own independent buffer (no sharing, no copying in RT context).
- Voices only set pointers to these buffers at trigger time.
- All buffer allocation/copying is done outside the audio thread (e.g., when a new loop is committed).
- Playback is stable, 1:1, and matches the looper audio.

## Steps

### 1. Data Structure Design
- Add a per-slice buffer array to the sampler struct:
  - `float slice_audio[MAX_SLICES][SLICE_LEN * channels]` or pointers to preallocated buffers.
  - Track buffer length per slice.
- Add a flag/validity marker for each slice buffer.

### 2. Buffer Management API
- Add functions to:
  - Preallocate all slice buffers at sampler init/reset.
  - Copy audio from the looper into the correct slice buffer when a new loop is committed (outside RT context).
  - Mark buffers as valid/invalid as needed.

### 3. Voice Assignment
- Update voice struct to hold a pointer to the assigned slice buffer and its length.
- On trigger (in RT context), assign the pointer and length—never allocate or copy.

### 4. Playback Logic
- Update process_block/process_chunk to read from the assigned buffer for each voice.
- Remove any direct dependency on the looper buffer for sampler playback.

### 5. RT Safety Audit
- Ensure all buffer allocation/copying is outside the audio thread.
- Only pointer assignment and reading in RT context.

### 6. Integration
- Update looper code to call the buffer copy API when a new loop is committed.
- Ensure all state is reset/cleared on sampler reset.

### 7. Testing & Validation
- Build (`make`) and fix all errors.
- Test for correct, stable playback and no RT violations.
- Confirm that each slice plays back independently and matches the looper audio.

## Periodic Review
- Revisit this plan after each major step.
- Update plan.md to reflect progress, issues, and next actions.
