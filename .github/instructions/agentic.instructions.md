# Agentic Instructions for the aloschen Repository

## Description
These instructions are meant for the **agentic LV2 audio‑plugin coding agent** working in the `aloschen` project. They condense all relevant knowledge about real‑time audio, LV2 metadata, UI design, MOD device integration, and the repository's build and testing workflows. The agent should load these instructions whenever it is tasked with developing, reviewing, or debugging code in this workspace.

---

## Core Objectives
- Build, analyse, and validate a high‑performance LV2 plugin (DSP + UI) written in C for the `aloschen` codebase.
- Maintain real‑time safety, correct TTL/URID metadata, and modular separation between DSP and UI.
- Use the existing `Makefile`/`Makefile.mk` infrastructure for builds, tests, and static analysis.

---

## Principles and Practices
1. **Strict DSP/UI separation.**
   - DSP code lives under `source/` and compiles as the plugin; UI lives separately (e.g. under `source/` too but built into another target).
   - Communication only via host callbacks (`write_function` / `port_event`) and Atom messages.

2. **Deterministic, real‑time execution.**
   - `run()` must complete within the audio buffer (e.g. 64 samples).
   - **Forbidden in `run()`:** any dynamic memory alloc/free, locks/mutexes, I/O (`printf`), OS syscalls, or STL containers that may allocate.
   - Spread heavy work across multiple callbacks; pre‑allocate all buffers in `instantiate()`.

3. **Data‑centric designs with URIDs and Atoms.**
   - State, MIDI, automation, and parameter changes flow via LV2 Atoms and URIDs.
   - Follow the existing URID mapping conventions (`source/alo_engine.h`, etc.).

4. **LV2 metadata.**
   - `aloschen.lv2/manifest.ttl` is minimal; any port descriptions or features reside in the bundle's TTL files.
   - Ports must use contiguous indices, valid C symbols, and proper RDF classes.  Update `lv2:minorVersion` when modifying metadata.

5. **Real‑time communications.**
   - Use SPSC lock‑free ring buffers for audio↔UI event exchange.
   - Offload non‑RT work to a worker thread via the LV2 Worker extension.

6. **MOD device considerations.**
   - Optimize for ARM/aarch64; watch for thermal throttling, keep CPU load under ~70%.
   - Use `mod-host` and `/tmp/alo.log` for debugging on hardware.

---

## Build & Test Workflow
All make targets should be run from the project root (`/home/carlo/dev/audio/aloschen`):

```sh
# build plugin and UI
make

# build and run tests (unit tests located in tests/)
make clean && make tests

# static analysis
make scan      # scan-build
make tidy      # clang-tidy
make cppcheck  # cppcheck
```

### Individual test binaries
```sh
./tests/run_transport_tests
./tests/run_dsp_tests
./tests/run_engine_tests
```

When editing code, run these commands after changes to verify zero errors/warnings.  Fix any diagnostics before replying to the user.

---

## Validation Tools
- **LV2 TTL**: `sord_validate $(find aloschen.lv2 -name '*.ttl')`
- **LV2 runtime**: `lv2lint -E -M -I aloschen.lv2 <plugin_uri>`
- **Live testing**: use `jalv.gtk` or `lv2apply` as needed.

Aim for no lint warnings or analyzer reports.

---

## Agent Behaviour Checklist
Before presenting code or advice, ensure:

1. Plugin compiles cleanly via `make`.
2. Unit tests run and pass (`make tests`).
3. Static analysis (`make scan`, `make tidy`, `make cppcheck`) shows no new issues.
4. LV2 metadata validates (`sord_validate` + `lv2lint`).
5. `run()` contains no forbidden operations and loops are bounded.
6. UI/DSP communication is host-mediated with Atom messages.

If modifications are made to metadata, increment the `lv2:minorVersion` or `lv2:microVersion` accordingly.

---

## Real-Time Safety Quick Reference
- Bounded loops, pre‑allocated structures, no recursion, no blocking.
- Avoid mutexes and condition variables – use lock‑free SPSC queues.
- Prevent denormals by adding a tiny constant to processing.
- Use `mlockall()` if necessary to avoid paging.

---

## Technical Cheat-Sheet

### Real-Time / Audio-Thread Rules
- **Worst-case bound**: `run()` must finish inside one buffer (e.g. 64 samples).
- **Forbidden in `run()`**: `malloc`/`free`/`new`/`delete`, any lock/mutex/semaphore, `printf`/I-O, OS syscalls, GC, STL containers that may allocate.
- **Algorithmic hygiene**: bound loops by `n_samples`. Spread large work over multiple calls. Add ~1e-15 noise to avoid denormals.
- **Inter-thread comms**: SPSC lock-free ring buffers for audio↔UI; `LV2_Worker` for background tasks.
- **Hardware notes**: On ARM (MOD Duo) watch thermal throttling (<70% CPU), use `mlockall()` to prevent paging.

### LV2 Architecture & Metadata
- **Bundle layout**: minimal `manifest.ttl`; detailed `plugin.ttl` linked via `rdfs:seeAlso`.
- **Port rules**: contiguous indices, C-identifier symbol, correct RDF class (`lv2:AudioPort`, etc.).
- **Metadata fields**: `lv2:minimum`, `lv2:maximum`, `lv2:default`, `units:unit`, `lv2:scalePoint`, `lv2:designation`.
- **Versioning**: bump `lv2:minorVersion`/`microVersion` when altering TTL.
- **Prefixes**: `lv2`, `atom`, `urid`, `time`, `midi`, `doap`, `rdf`, `rdfs`, `units`.

### URID & Atom
- Map URIs to 32-bit integers for RT efficiency.
- Common URIDs: MIDI event, Time Position, Atom String/Sequence/Path.
- Handle sample-accurate events with `LV2_ATOM_SEQUENCE_FOREACH`.
- State and messages are URID-keyed Atoms.

### Lifecycle & Threading (C API)
1. `instantiate()` – calloc state, check features.
2. `connect_port()` – store host buffer pointers.
3. `activate()`/`deactivate()` – start/stop.
4. `run()` – audio thread (see RT rules above).
5. `save()`/`restore()` – use LV2_State, may run alongside `run()`.
6. `cleanup()` – free everything.

### UI Guidelines
- Build as separate `LV2UI_Descriptor` library; no global state.
- Communicate only via `write_function()` and `port_event()`.
- Preferred frameworks: Pugl or DPF abstractions (GTK, Qt5, etc.).
- Support HiDPI (`ui:scaleFactor`) and fallback to separate window if embedding fails.

### Build / Test / Validation Summary
- **Make workflow**:
  ```sh
  make
  make clean && make tests
  make scan
  make tidy
  make cppcheck
  ```
- Run tests individually (`./tests/run_*_tests`).
- **Static checks**: `sord_validate`, `lv2lint`.
- Live tests: `lv2apply`, `jalv.gtk`.
- MOD debug: `mod-host -p 1234 -i add <plugin_uri> 0`; check `/tmp/alo.log`.

### Quick Pre-Response Checklist
- [ ] Code compiles cleanly.
- [ ] Unit tests pass.
- [ ] Static analysis shows no new issues.
- [ ] TTL validates; `lv2lint` no warnings.
- [ ] `run()` free of banned ops; loops bounded.
- [ ] DSP/UI comms use host callbacks/atoms.
- [ ] Metadata version bumped if changed.

---

## Detailed Coding Guidelines

### LV2 Lifecycle & Threading
* `instantiate()` allocates private state (use `calloc`), checks required features.
* `connect_port()` stores pointers to host buffers; buffers may be reconnected anytime.
* `run()` is the real‑time audio thread and must never block or allocate.  All loops must be bounded by `sample_count` and avoid implicit allocations (no `std::vector`).
* `activate()/deactivate()` manage state transitions in the instantiation thread.
* `save()/restore()` may run concurrently with `run()`; use LV2 State extension correctly.
* `cleanup()` frees everything before host unloads plugin.

### TTL & Metadata Rules
* `manifest.ttl` must be minimal for fast scanning; link to a second TTL (`plugin.ttl`) using `rdfs:seeAlso`.
* Use these common prefixes:
  - `lv2`: http://lv2plug.in/ns/lv2core#
  - `atom`: http://lv2plug.in/ns/ext/atom#
  - `urid`: http://lv2plug.in/ns/ext/urid#
  - `time`: http://lv2plug.in/ns/ext/time#
  - `midi`: http://lv2plug.in/ns/ext/midi#
* Ports require contiguous indices, valid C symbols, and appropriate class (`lv2:AudioPort`, `lv2:ControlPort`, `atom:AtomPort`).
* Metadata fields: `lv2:minimum`, `lv2:maximum`, `lv2:default`, `units:unit`, `lv2:scalePoint`, and `lv2:designation` for host friendly names.
* Update `lv2:minorVersion`/`microVersion` when editing TTL.

### URID & Atom
* Map URIs to 32‑bit integers for speed. Access common URIDs in code via a lookup struct.
* Typical URIDs include:
  - MIDI event
  - Time position
  - Atom String/Sequence/Path
* Use `LV2_ATOM_SEQUENCE_FOREACH` for sample‑accurate event handling.

### Worker & State Examples
```
LV2_State_Status my_save(LV2_Handle instance,
                         LV2_State_Store_Function store,
                         LV2_State_Handle handle,
                         uint32_t flags,
                         const LV2_Feature *const *features) {
    MyPlugin* plugin = (MyPlugin*)instance;
    return store(handle,
                 plugin->uris.my_greeting_key,
                 plugin->state.greeting,
                 strlen(plugin->state.greeting) + 1,
                 plugin->uris.atom_String,
                 LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}
```
* Offload heavy or I/O tasks to `LV2_Worker_Interface`; signal back via atoms.

### UI Classes & Communication
* UI runs on its own thread; communicate using `write_function()` and `port_event()` only.
* Avoid globals; state flows through atoms or simple floats.
* Preferred UI frameworks: Pugl or DPF abstractions (GTK, Qt5, cairo, OpenGL, Vulkan).
* Support HiDPI scaling (`ui:scaleFactor`) and fallback to separate window if embedding fails.

### Algorithms & Parameter Handling
* Parameter smoothing formula:
  ```c
  y[n] = y[n-1] * (1 - alpha) + x[n] * alpha;
  ```
* Spread large buffer operations (memset) over several calls.
* Denormal prevention: add tiny noise (~1e-15) to recursive filters.

### Architectural Rules
* URI must match across code and TTL.
* Zero initialize all plugin data structures (`calloc`).
* Declare every required extension in `lv2:requiredFeature`.
* Always target sample‑accurate timing and avoid amortized complexity.

---

These consolidated instructions should be used for all agentic interactions with the `aloschen` codebase; they encompass the previous LV2‑, design‑, and real‑time‑focused guidance, with an emphasis on using the current `make`‑based workflow.