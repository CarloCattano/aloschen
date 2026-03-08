# Agents Reference

This repository uses the following helper agents for exploration:

- **Explore** – fast read-only codebase exploration and Q&A subagent. Use it for searches across the tree and general questions about code organization.

use /usr/bin/ls for ls
prefer ripgrep (rg) to grep

---

## Project Layout

```
aloschen/
├── Makefile          ← top-level build entry point (use this)
├── Makefile.mk       ← shared flags and static-analysis targets
├── source/           ← all DSP and UI C source files
├── aloschen.lv2/     ← compiled plugin bundle (built artefacts + TTL)
└── tests/            ← unit test sources and compiled test binaries
```

> **All `make` commands must be run from the project root (`aloschen/`), not from `source/`.**
>
> ### Transient-slicing support
> A lightweight `TransientDetector` class was added in
> `source/transient_detector.h,c`.  When the new `split_by_transient` control
> port is set true, the sampler will segment the loop buffer at detected onset
> positions instead of rigid bar subdivisions.  Sane limits are enforced by the
existing `slices_per_bar` value (and hard‑clamped to 32).  A companion
`transient_threshold` control lets the user raise/lower the detection ceiling
and thereby shrink/expand the number of slices produced.  The current count is
exposed via the `detected_slices` output port and surfaced in both the X11
UI and ModGUI.  A global `slice_env_frac` control (0..100) was added to let the user
specify the *release length* of sampler slice voices as a percentage of the
slice itself; 0 gives the shortest possible fade (1 sample) and 100 uses the
entire slice. Attack used to be fixed at ≈1 ms but is now exposed as a hidden
LV2 parameter (`slice_env_attack`) defaulting to 5 ms; hosts may automate it if
needed but the GUI does not expose a control.  The UI slider for decay now
updates active voices in real time, and the engine recalculates slice offsets
when the transient threshhold changes.  The parameter was previously a
fraction and then a millisecond-scale value; the constant symbol is still
`slice_env_frac` for compatibility.  Updated unit tests cover the mapping and
clamping behaviour (`tests/dsp_test.c`, `tests/engine_test.c`).  In addition,
 the transient‑threshold slider now extends up to 20 (was 10) to allow larger
values when loops sound too short.

> **STATUS (resolved)**: most issues have been addressed and the remaining
> items are now routine maintenance.
> 
> * previously the `detected_slices` output could appear to hang at zero. two
>   separate fixes were applied:
>   * the DSP helper stopped capping the slice count to the threshold control
>     (threshold now only affects the detector level). the port now reflects
>     the true number of offsets found, bounded only by the sampler voice
>     limit.
>   * the ModGUI script was not listening for `detected_slices` notifications,
>     so the knob never updated. the handler now updates the visible
>     `slices_per_bar` control when split mode is active.
>   testing exercises all of the above, including mid‑run threshold/split
>     adjustments.
> * transient slicing now restarts whenever the threshold, sensitivity or
>   split toggle changes, or when a new loop buffer is committed.  helper
>   code ensures the detector state is seeded with offset zero and that the
>   port is written immediately so the UI can stay in sync.
> * the sampler voice triggering has been improved: polyphonic mode allocates
>   unused voices first (avoiding unwanted reuse) and mono/round‑robin modes
>   behave correctly with the new play‑mode enum knob.  All modes are covered
>   by unit tests.
> * slice buffers are refreshed every block via `sampler_cache_update_slice_buffers`,
>   so changing UI controls now immediately affects audio playback.
> 
> Remaining TODOs are now minor (e.g. add diagnostic logging, tidy UI
> behaviour) and no longer block normal operation.
## Internal refactor notes

* slice_sampler.c received a heavyweight cleanup per recent bug report
  (‘one-note trigger’).  Key changes:
  * introduced `ALO_GUARD` macro and consolidated null checks.
  * factored repeated buffer reset logic into `alo_slice_buffers_reset()` and
    removed duplication across `clear_buffers`, `alloc_buffers` and
    `free_buffers`.
  * added `slice_idx` field to pending events so the expensive per‑sample
    division is computed once during scheduling.
  * `alo_slice_sampler_start_voice()` now returns the allocated voice and
    caller attaches slice buffer pointers directly, eliminating the O(N)
    search previously done on every trigger.  voice selection logic was also
    simplified and made poly‑friendly.
  * obsolete helpers removed (`alo_voice_is_active`), useless parameters
    dropped, and envelope pointer cached outside the sample loop.
  * minor cleanups: improved wrap logic, removed dead `step_clear` stub, and
    reduced per‑sample branching.
  * unit tests expanded to exercise the new play‑mode enum and ensure
    behaviour conforms.

  These changes shave dozens of lines, lower RTT complexity, and fix the
  “only first note” behaviour while also making the module easier to audit.

* **Wider DRY efforts**: added `ALO_GUARD_*` macros and a shared
  `alo_get_slice_count_u()` helper.  Several modules (loop_engine,
  sampler_cache, transport, loop_playback, button_logic) now use these
  abstractions, reducing repetitive null checks and arithmetic.  Additional
  macros (`ALO_GUARD_BOOL`, `ALO_GUARD_NULL`) make intent explicit and keep
  runtime logic minimal.

---

## VERIFICATION TRUTH COMMANDS

After every code change, run the following in order:

1. **Check editor diagnostics** – use the in-editor errors tool immediately after editing.
2. **Build the plugin:**
   ```sh
   cd ~/dev/audio/aloschen
   make
   ```
   Verify: zero errors, zero new warnings.

3. **Run unit tests:**
   ```sh
   cd ~/dev/audio/aloschen
   make clean && make tests
   ```
   Expected output ends with: `All tests completed successfully.`

4. **Confirm static analysis is clean:**
   ```sh
   cd ~/dev/audio/aloschen
   make scan
   ```
   Expected: `scan-build: No bugs found.`

Use `git --no-pager log/diff/show` freely to understand history and context.

---

## Test Workflow

Tests live in `tests/` and are compiled into separate binaries (one `main` per file).
### Build-and-load verification

A useful manual workflow to confirm the plugin runs in a real host:

1. `make` in project root to build the bundle.
2. Copy the bundle to `~/.lv2` (e.g., `cp -r aloschen.lv2 ~/.lv2/`).
3. Run `lv2lint -E -M -I ~/.lv2/aloschen.lv2 http://ktano-studio.com/aloschen`
   to validate the bundle.
4. launch with `jalv` using the URI.
   Run it with `ALO_LOG=1` to enable the internal debug logger.  While the
   plugin is running you can tail `/tmp/alo.log` to watch initialization and
   runtime messages.  Quit the host when done.

This sequence provides additional end‑to‑end assurance beyond unit tests.
| Binary                       | Source                      | Tests                                    |
|------------------------------|-----------------------------|------------------------------------------|
| `tests/run_transport_tests`  | `tests/transport_test.c`    | `compute_transport_phase_index`, `compute_next_cycle_start_beats` |
| `tests/run_dsp_tests`        | `tests/dsp_test.c`          | `alo_soft_clip_unit`, edge fade, bar-len helpers |

### Build and run tests

### Live Host Verification Skill

When needing extra confidence that the plugin can be loaded by a host:

1. Build the bundle with `make`.
2. Install it locally with `cp -r aloschen.lv2 ~/.lv2/`.
3. Run `lv2lint -E -M -I ~/.lv2/aloschen.lv2 <plugin_uri>` as a fast
   static/runtime sanity check.
4. If a GUI host such as **jalv** is available, launch it with
   `jalv http://ktano-studio.com/aloschen` or `jalv.gtk`.
5. Enable debug logging by setting `ALO_LOG=1` or otherwise tail
   `/tmp/alo.log` in another shell to watch real‑time messages.
6. Quit the host when the plugin is running; the log tail confirms the
   plugin was instantiated and any initialization messages appeared.

This “jalv + tail log” routine can be run from a child process in the
agent and serves as a lightweight end‑to‑end check when GUI access is
available.  It has been formally added to the agent’s toolset.

### Build and run tests

```sh
# First run (or after source changes): clean forces a full rebuild
make clean && make tests

# Incremental (tests rebuild only if sources changed):
make tests

# Force rebuild regardless of timestamps:
make -B tests
```

### Run individual test binary

```sh
./tests/run_transport_tests
./tests/run_dsp_tests
```

### Adding a new test file

1. Create `tests/my_feature_test.c` with its own `main()`.
2. Add a new target in the `Makefile` following the existing pattern:
   ```makefile
   tests/run_my_feature_tests: tests/my_feature_test.c source/relevant.c source/alo_util.c
   	$(CC) -Isource $^ $(BUILD_C_FLAGS) $(LINK_FLAGS) -lm -o $@
   	chmod +x $@
   ```
3. Add the binary to `TEST_BIN` and to the `clean` rule.

---

## Static Analysis Workflow

### 1. scan-build (Clang analyzer)

Run from the **project root**:

```sh
cd ~/dev/audio/aloschen
make scan
```

Reports dead stores, null dereferences, uninitialized values, etc. Address each
warning by fixing the root cause — do not silence warnings with pragmas.
Repeat until the report directory is empty.

### 2. clang-tidy

```sh
cd ~/dev/audio/aloschen
make tidy
```

Runs `clang-tidy` over all `.c`/`.cpp` files in `source/`. Uses
`compile_commands.json` if present (generate with `bear -- make`).

### 3. cppcheck

```sh
cd ~/dev/audio/aloschen
make cppcheck
```

Reports style issues and logic bugs. Informational messages about missing
system/LV2 headers are expected in a non-installed environment — treat only
`warning:` and `error:` lines as actionable.

---

## Build Targets Reference

| Target              | Description                                          |
|---------------------|------------------------------------------------------|
| `make` / `make all` | Build the plugin `.so` files and `manifest.ttl`      |
| `make tests`        | Build test binaries then run them; fail on error     |
| `make scan`         | Run `scan-build` static analyzer once                |
| `make tidy`         | Run `clang-tidy` on all source files                 |
| `make cppcheck`     | Run `cppcheck` on the whole tree                     |
| `make safe`         | Build with `-fno-fast-math` for precision debugging  |
| `make clean`        | Remove built artefacts including test binaries       |
| `make install`      | Install bundle to `$(PREFIX)/lib/lv2/`               |

---

## Real-Time Safety Rules (enforced at all times)

The `run()` callback and anything it calls transitively must **never**:

- Call `malloc`, `calloc`, `realloc`, `free`, `new`, `delete`
- Acquire a mutex, semaphore, or any blocking lock
- Call `printf`, `fprintf`, `fopen`, `fclose`, or any I/O
- Invoke OS system calls with unbounded runtime
- Use `std::vector::push_back` or any container that may allocate

All buffers are pre-allocated at `instantiate()` time.
Inter-thread state changes use flag-based handoff (no locks).
Spreading large work (e.g., buffer clearing) over multiple `run()` calls is
required for any task that exceeds a single block's time budget.

---

## Other Useful Tools

- **bear** — generate `compile_commands.json` for clang tooling:
  ```sh
  cd ~/dev/audio/aloschen
  bear -- make
  ```
- **lv2lint** — validate the plugin bundle against the LV2 spec:
  ```sh
  lv2lint -E -M -I aloschen.lv2 http://ktano-studio.com/aloschen
  ```
- **sord_validate** — validate TTL metadata syntax:
  ```sh
  sord_validate aloschen.lv2/*.ttl
  ```
- **jalv** — run plugin in a minimal JACK host for live testing:
  ```sh
  jalv.gtk http://ktano-studio.com/aloschen
  ```
- **Debug logging** — opt-in file logging to `/tmp/alo.log`:
  ```sh
  ALO_LOG=1 jalv.gtk http://ktano-studio.com/aloschen
  tail -f /tmp/alo.log
  ```
