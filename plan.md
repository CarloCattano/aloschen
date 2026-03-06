# Plan for RT-safe Per-Slice Buffer Allocation in Sampler

## Objective
Implement a robust, real-time safe per-slice buffer system for the sampler, ensuring:
- All audio buffers are preallocated outside the audio thread.
- Each slice has its own independent buffer (no sharing, no copying in RT context).
- Voices only set pointers to these buffers at trigger time.
- All buffer allocation/copying is done outside the audio thread (e.g., when a new loop is committed).
- Playback is stable, 1:1, and matches the looper audio.

## Steps

### 1. Data Structure Design ✅
- Added `slice_audio` pointer array, `slice_audio_len`, and `slice_audio_valid` fields to `AloSliceSampler`.
- Also introduced `AloSliceBuffer` struct and `slice_buffers` array with per-buffer metadata.
- Validity markers present.

### 2. Buffer Management API ✅
- Implemented `alo_slice_sampler_alloc_buffers`/`free_buffers`, `fill_slice`, `clear_buffers` plus a thin `prealloc_buffers` alias.
- API is non-RT; callers must invoke outside audio thread.
- Looper now mirrors audio into these buffers during `sampler_src` rebuild (part of integration).

### 3. Voice Assignment ⚠️ (partial)
- `AloSliceVoice` already has placeholder fields `slice_buf_l/r` and `slice_buf_len` but they are not yet used.
- RT logic currently mixes from looper cache; next step is to assign the slice buffer pointers during scheduling.

### 4. Playback Logic 🔜
- Future work: modify `alo_slice_sampler_process_chunk` to use `v->slice_buf_l/r` instead of `alo_mix_looper_at_phase` when a voice has an assigned buffer.

### 5. RT Safety Audit ✅
- All allocation/copy code resides in non-RT API and in the looper rebuild loop (bounded per-block).
- No heap ops or large loops remain in the audio thread.

### 6. Integration ⚠️
- Partial integration: sampler rebuild loop now mirrors cache to slice buffers incrementally and invalidates them on dirty.
- Still need explicit copy-on-commit or optimization for when loop content changes drastically (could reuse current rebuild).

### 7. Testing & Validation ✅
- Project builds cleanly; `make` reports no errors.
- RT safety confirmed via code inspection; manual testing not yet performed.
- Slice‑to‑looper audio correlation will need to be tested once playback logic is updated.

## Periodic Review 🔁
- Plan has been updated with progress notes above.
- Next action: implement voice buffer assignment + playback logic, then re-review.
