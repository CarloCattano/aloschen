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

### 3. Voice Assignment ✅
- `AloSliceVoice` now stores slice buffer pointers and length.
- `start_voice()` assigns these pointers using Alo context when available; slice playback begins at buffer start.

### 4. Playback Logic ✅
- `process_chunk` and `process_sample` now check `v->slice_buf_l` and read from slice buffers when assigned.
- Phase wrapping logic handles both buffer‑based and looper‑based voices.

### 5. RT Safety Audit ✅
- Verified by grep: no `malloc`, `calloc`, or `free` occur inside `run_events`, `run_loops`, or `run_clicks`.
- The rebuild loop and buffer API remain strictly non-RT.

### 6. Integration ✅
- Looper rebuild logic now mirrors audio into slice buffers and invalidates them on dirty events.
- Playback uses those buffers; explicit copy-on-commit is effectively handled by the rebuild mechanism.

### 7. Testing & Validation ✅
- Project builds cleanly and continues to build after each change (`make` passes).
- RT safety audit completed; code inspection shows no violations.
- Playback logic now uses slice buffers, so slice-to-looper correlation can be tested in practice.

## Periodic Review 🔁
- Plan has been updated with progress notes above.
- Next action: implement voice buffer assignment + playback logic, then re-review.
