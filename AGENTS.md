# Agents Reference
## Project Layout
aloschen/
├── Makefile          ← top-level build entry point (use this)
├── Makefile.mk       ← shared flags and static-analysis targets
├── source/           ← all DSP and UI C source files
├── aloschen.lv2/     ← compiled plugin bundle (built artefacts + TTL)
└── tests/            ← unit test sources and compiled test binaries

### Key Make targets added for agent workflows
- `make analyze` — runs all static analysis in one go: `scan` + `tidy` + `cppcheck`
- `make fullcheck` — runs the full quality gate in one go: `tests` + `analyze`

> **All `make` commands must be run from the project root (`aloschen/`), not from `source/`.**

## VERIFICATION TRUTH COMMANDS
After every code change, run the following in order:
1. **Check editor diagnostics** – use the in-editor errors tool immediately after editing.
2. **Build the plugin:**
   cd ~/dev/audio/aloschen
   make
   Verify: zero errors, zero new warnings.
3. **Run unit tests:**
   make clean && make tests
   Expected output ends with: `All tests completed successfully.`
4. **Confirm static analysis is clean:**
   make scan
   Expected: `scan-build: No bugs found.`

### Single-command verification (agent-friendly)
If you want one command that runs the full quality gate (tests + all static analysis) in one go:
- `make fullcheck`

This runs:
- `make tests`
- `make analyze` (see below)

Use `git --no-pager log/diff/show/status` freely to understand history and context.
---

## Test Workflow
Tests live in `tests/` and are compiled into separate binaries (one `main` per file).

### Build-and-load verification

A useful manual workflow to confirm the plugin runs in a real host:
1. `make` in project root to build the bundle.
2. Copy the bundle to `~/.lv2` (e.g., `cp -r aloschen.lv2 ~/.lv2/`).
3. Run `lv2lint -E -M -I ~/.lv2/aloschen.lv2 http://ktano-studio.com/aloschen`
   to validate the bundle.
4. Launch with `jalv` using the URI.
   Run it with `ALO_LOG=1` to enable the internal debug logger. While the
   plugin is running you can tail `/tmp/alo.log` to watch initialization and
   runtime messages. Quit the host when done.

### Transient slicing debug (known `sample.wav`)
The transient WAV test binary includes an opt-in debug report for tuning transient slicing
against `tests/assets/sample.wav` (intended for manual investigation, not CI baselines).

- Run normal (quiet) tests:
  - `./tests/run_transient_wav_tests`

- Run with debug output enabled:
  - `TRANSIENT_WAV_TEST_DEBUG=1 ./tests/run_transient_wav_tests`

The debug report prints additional cases (Sens / threshold multiplier / slices-per-bar),
the resulting detected split counts, and a note→slice assignment summary so you can
cross-check behavior when testing the same asset in a host.

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
make clean && make tests

### Run individual test binary
./tests/run_transport_tests
./tests/run_dsp_tests
./tests/run_engine_tests
./tests/run_transient_wav_tests

### Adding a new test file
1. Create `tests/my_feature_test.c` with its own `main()`.
2. Add a new target in the `Makefile` following the existing pattern:
   tests/run_my_feature_tests: tests/my_feature_test.c source/relevant.c source/alo_util.c
   	$(CC) -Isource $^ $(BUILD_C_FLAGS) $(LINK_FLAGS) -lm -o $@
   	chmod +x $@
3. Add the binary to `TEST_BIN` and to the `clean` rule.
---
## Static Analysis Workflow
### Quick path (recommended)
Run everything in one go from the **project root**:
- `make analyze`

This runs:
1. `make scan` (scan-build / Clang analyzer)
2. `make tidy` (clang-tidy)
3. `make cppcheck` (cppcheck)

### 1. scan-build (Clang analyzer)
Run from the **project root**:
make scan
Reports dead stores, null dereferences, uninitialized values, etc. Address each warning by fixing the root cause — do not silence warnings with pragmas.
Repeat until the report directory is empty.

### 2. clang-tidy
cd ~/dev/audio/aloschen
make tidy

Runs `clang-tidy` over all `.c`/`.cpp` files in `source/`. Uses
`compile_commands.json` if present (generate with `bear -- make`).

### 3. cppcheck
make cppcheck

Reports style issues and logic bugs. Informational messages about missing
system/LV2 headers are expected in a non-installed environment — treat only
`warning:` and `error:` lines as actionable.
---

## Build Targets Reference
| Target                | Description                                          |
|-----------------------|------------------------------------------------------|
| `make` / `make all`   | Build the plugin `.so` files and `manifest.ttl`      |
| `make tests`          | Build test binaries then run them; fail on error     |
| `make scan`           | Run `scan-build` static analyzer once                |
| `make tidy`           | Run `clang-tidy` on all source files                 |
| `make cppcheck`       | Run `cppcheck` on the whole tree                     |
| `make analyze`        | Run `scan` + `tidy` + `cppcheck` in one go           |
| `make fullcheck`      | Run `tests` + `analyze` in one go                    |
| `make safe`           | Build with `-fno-fast-math` for precision debugging  |
| `make clean`          | Remove built artefacts including test binaries       |
| `make install`        | Install bundle to `$(PREFIX)/lib/lv2/`               |
---

## Real-Time Safety Programming Rules
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
  cd ~/dev/audio/aloschen
  bear -- make
- **lv2lint** — validate the plugin bundle against the LV2 spec:
  lv2lint -E -M -I aloschen.lv2 http://ktano-studio.com/aloschen
- **sord_validate** — validate TTL metadata syntax:
  sord_validate aloschen.lv2/*.ttl
- **jalv** — run plugin in a minimal JACK host for live testing:
  jalv.gtk http://ktano-studio.com/aloschen
- **Debug logging** — opt-in file logging to `/tmp/alo.log`:
  ALO_LOG=1 jalv.gtk http://ktano-studio.com/aloschen
  tail -f /tmp/alo.log

## LV2 / TTL / C Reference Guidance (x42 Knowledge Base)

Use the local `x42-plugins/` tree as a practical reference when assumptions about
LV2 descriptors, Atom/MIDI event handling, TTL metadata, or UI wiring are unclear.

### Purpose
- Anchor implementation decisions to real-world, working LV2 plugins.
- Reduce guesswork for host-facing behavior and metadata conventions.
- Cross-check DSP/control-port design patterns before refactors.

### What to use as reference
- `x42-plugins/*/*.ttl` for LV2 metadata patterns:
  - `lv2:port` structure and ordering
  - control/audio/atom port declaration style
  - defaults, ranges, scale points, units
  - optional vs required features
- `x42-plugins` C sources for runtime behavior:
  - `instantiate`, `connect_port`, `run`, `cleanup`
  - URID mapping and Atom sequence parsing
  - MIDI note-on/note-off handling in RT-safe loops
  - UI/worker separation patterns and host communication
- Existing `aloschen.lv2/*.ttl` as the canonical target format for this project.

### How to apply it
1. Identify the feature being implemented (e.g., slice MIDI mapping, new control port).
2. Find 1–2 comparable implementations in `x42-plugins`.
3. Mirror LV2/TTL structure and host contract behavior where applicable.
4. Adapt naming/ranges to `aloschen` conventions without breaking existing URIs.
5. Verify end-to-end with build, tests, and analyzer steps in this document.

### Rules when borrowing patterns
- Never copy URIs, brand names, or plugin identity from references.
- Keep `aloschen` ABI/port-index stability unless explicitly doing a breaking change.
- Preserve RT-safety constraints (no allocation/locks/I/O in `run()` path).
- Treat x42 as behavioral reference, not a drop-in dependency.

### LV2 Assumption Checklist (before merging)
- Port indices in C enums match TTL declaration order exactly.
- Every exposed control has sensible min/max/default and, when relevant, units.
- Atom sequence parsing handles malformed/short events safely.
- MIDI mapping logic is deterministic and bounded (valid note range, max slices).
- Feature requirements (`urid:map`, options, etc.) are validated at instantiate time.
- TTL validates cleanly and bundle remains host-loadable.

### Suggested verification for LV2-facing changes
1. `make`
2. `make clean && make tests`
3. `make scan`
4. Optional metadata/runtime checks:
   - `sord_validate aloschen.lv2/*.ttl`
   - `lv2lint -E -M -I aloschen.lv2 http://ktano-studio.com/aloschen`
