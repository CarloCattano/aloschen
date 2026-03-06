---
description: Describe when these instructions should be loaded by the agent based on task context
# applyTo: 'Describe when these instructions should be loaded by the agent based on task context' # when provided, instructions will automatically be added to the request context when the pattern matches an attached file
---
# EXPERT RECOMMENDATIONS: REAL-TIME AUDIO DESIGN (BENCINA-LV2)

## 1. THE CARDINAL RULE
**"If you don't know how long it will take, don't do it."**
Real-time audio is not about *average* performance; it is about *worst-case* performance. A single buffer period (e.g., 1.3ms for 64 samples @ 48kHz) is your absolute deadline.

## 2. ELIMINATING UNBOUNDED OPERATIONS
The following operations have unpredictable execution times and must be strictly excluded from the `run()` callback:

* **Priority Inversion (Mutexes/Locks):** Never use standard mutexes. If a low-priority UI thread holds a lock needed by the high-priority audio thread, the audio thread is effectively demoted to UI priority.
* **Memory Paging & I/O:** * Avoid file I/O (disk seeks average ~8ms, far exceeding buffer deadlines).
    * Avoid network sockets.
    * Consider `mlockall()` on Linux to prevent the OS from paging your audio memory to disk.
* **Language Hazards:** * **Objective-C/Swift/C++:** Avoid calling into high-level objects that may implicitly allocate memory or use locks (e.g., `NSLog`, `std::map` insertions, or anything using a Global Interpreter Lock).
    * **Garbage Collection:** Never use languages/routines with non-deterministic GC cycles unless they provide a "Real-Time" mode.

## 3. ALGORITHMIC DESIGN FOR LATENCY
* **Worst-Case vs. Throughput:** Do not optimize for "average speed." An algorithm that is fast 99% of the time but has a periodic "cleanup" spike will cause glitches.
* **Spreading the Load:** If a large task is required (e.g., zeroing a massive 10-second delay line), do not `memset` it in one cycle. Spread the work over multiple `run()` calls (e.g., clear 256 samples per callback).
* **Avoid "Amortized" Complexity:** Standard library containers (like `std::vector::push_back`) that occasionally reallocate are strictly forbidden.

## 4. INTER-THREAD COMMUNICATION (SPSC)
To communicate between the `run()` thread and the rest of the world (UI/Disk/Network):
* **Single-Producer Single-Consumer (SPSC) Ring Buffers:** Use lock-free, wait-free circular buffers.
* **The Pattern:** * **Audio -> UI:** Audio thread writes to ring buffer; UI thread polls/reads.
    * **UI -> Audio:** UI thread writes parameter changes/events; Audio thread reads.
* **Worker Extension:** In LV2, use the `LV2_Worker_Interface` to offload non-deterministic tasks (like loading a file) to a background thread, then signal completion back to the audio thread via an Atom.

## 5. HARDWARE-SPECIFIC AUDIT (MOD DEVICES)
* **Thermal Throttling:** High CPU usage leads to heat. If the ARM CPU throttles, your "worst-case" execution time doubles instantly. Aim for a "Safety Margin" (keep DSP load below 70% of the reporting limit).
* **Denormal Protection:** Always apply a "noise floor" (approx $10^{-15}$) to recursive filters to prevent ARM CPUs from entering a slow-path processing mode when signals approach zero.

## 6. FINAL ARCHITECTURAL CHECKLIST
- [ ] Is every loop bound constant or tied directly to `sample_count`?
- [ ] Are all data structures used in `run()` fixed-size and pre-allocated?
- [ ] Is communication handled via lock-free primitives?
- [ ] Is the "Worst Case" execution time (WCET) verified as being lower than the buffer duration?