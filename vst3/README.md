# Aloschen VST3 for Linux

A DPF VST3 wrapper around the shared Aloschen looper/slice engine.

## Build and install on Marvin

Install a C/C++ toolchain, Git, Make, LV2 headers, and X11/OpenGL development
libraries. From the repository root:

```sh
git clone https://github.com/DISTRHO/DPF.git deps/DPF
git -C deps/DPF checkout 4238e1c7f0351bbe488d79f0899c540543ac7583
git -C deps/DPF submodule update --init --recursive
make vst3-package
```

The build applies the tracked DPF transport patch to that checkout and refuses
an unexpected DPF revision. Keep `deps/DPF` dedicated to this checkout.

`build/vst3/` contains the `aloschen.vst3` bundle, the
`aloschen-linux-$(uname -m).tar.gz` archive, and a SHA-256 file. Check the
archive by running `sha256sum -c aloschen-linux-$(uname -m).tar.gz.sha256`
from the directory containing it. Copy or extract the bundle into `~/.vst3/`
and rescan in your host. If another build of Aloschen is already installed,
replace it rather than installing a second copy with the same plugin ID.

## Timing contract

1. Prefer the host's musical position and tempo. DPF converts VST3 quarter
   notes to denominator-note beats without rounding odd meters to whole
   quarter notes. The bridge preserves double precision through to the DSP.
2. A beat boundary belongs to the first sample at or after it. A boundary
   at the end of a host buffer belongs to the next buffer. Clicks, loop wraps,
   and quantized edits use that same rule, including fractional beat lengths.
3. At a fixed meter, locating preserves the recorded loop origin. Existing
   loops follow tempo changes by resampling (pitch follows tempo). A tempo
   change cancels in-flight recording; return to the original tempo to add
   layers or new slots alongside committed loops.
4. When the host omits musical time, the sample-clock fallback assumes
   constant tempo and 4/4. It cannot reconstruct tempo maps. Meter changes
   within a project are not supported for committed loops; changing the meter
   clears the slots. Very slow/long loops beyond the fixed audio storage
   capacity cannot span their full host cycle.
5. **Click is a count-in:** it is intentionally silent while any loop slot
   contains audio, and when the host is stopped. A sample-rate change
   recreates the engine. Loop audio is not stored in the DAW project.

References: [VST3 ProcessContext][vst-context], [LV2 Time][lv2-time].
The adapter keeps host clock policy in `AloschenPlugin.cpp` and port/Atom
translation in `alo_bridge.c`; the engine owns audio scheduling. No plugin ID
or parameter ID changes are needed for this fix.

## Verify

- `make tests`: bridge renders exercise varying buffer sizes, split blocks,
  late PPQ, fractional tempos, stopping, seeking, and loop replay. Loop
  markers and click onsets are checked at exact samples.
- Steinberg validator: `validator -e build/vst3/aloschen.vst3`
- pluginval: `pluginval --strictness-level 10 --validate build/vst3/aloschen.vst3`
- REAPER: with the bundle installed, run
  `tests/host/run_reaper.sh build/reaper-check click-7-8`. Once
  `build/reaper-check/reaper-log.txt` says `render done`, run
  `python3 tests/host/analyze_loop_wav.py build/reaper-check/render.wav click-7-8`.
  Repeat with modes `loop`, `click`, and `click-tempo` in separate output
  directories. The script leaves a private REAPER window open with the VST3
  editor visible and the master muted. Close it before the next run.

On Marvin, also try start/stop, mid-bar seeks, and low buffers in your normal
DAW/audio setup. Offline render tests do not establish physical latency or
real-time performance on different hardware.

[vst-context]: https://steinbergmedia.github.io/vst3_doc/vstinterfaces/structSteinberg_1_1Vst_1_1ProcessContext.html
[lv2-time]: https://lv2plug.in/ns/ext/time
