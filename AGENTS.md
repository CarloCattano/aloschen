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

| Binary                       | Source                      | Tests                                    |
|------------------------------|-----------------------------|------------------------------------------|
| `tests/run_transport_tests`  | `tests/transport_test.c`    | `compute_transport_phase_index`, `compute_next_cycle_start_beats` |
| `tests/run_dsp_tests`        | `tests/dsp_test.c`          | `alo_soft_clip_unit`, edge fade, bar-len helpers |

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
