# Aloschen VST3 (Linux)

A VST3 build of the unchanged Aloschen LV2 engine (`source/`), wrapped with
[DPF](https://github.com/DISTRHO/DPF) and a NanoVG editor.

## Why DPF
The iPlug3 marketplace installed here ships validator skills only; no public
iPlug3 framework release exists. DPF builds VST3 natively on Linux with no
Steinberg SDK at build time, has an OpenGL/NanoVG UI and a small C++ surface.

## Build

```sh
git clone --depth 1 --recursive https://github.com/DISTRHO/DPF deps/DPF
make vst3            # -> build/vst3/aloschen.vst3
```
Install: link or copy `build/vst3/aloschen.vst3` into `~/.vst3/`.

## Layers
- `alo_bridge.{h,c}`: in-process host for the LV2 descriptor (URID map, buf-size
  options, one `time:Position` atom per block from host bar/beat/tempo/playing,
  MIDI note atoms, peak meters). Single parameter table; `tests/vst3_bridge_test.c`
  checks it against `aloschen.lv2/aloschen.ttl` and proves bar-synced looping.
- `AloschenPlugin.cpp`: DPF adapter (parameters, MIDI in, `TimePosition`).
- `AloschenUI.cpp`: editor (rings per slot, state colours, playhead, step strip,
  meters, one control per engine input).

## Contracts and known limits
- Loop buttons are `lv2:toggled`: one press = one flip; flips closer than
  `ALO_LOOP_BTN_HOLD_FRAMES` (8) process calls apart merge. Undo needs a rising edge;
  the UI pulses it for 80 ms.
- `ENABLED` is a 0/1 toggle (the TTL's 0.15..2.5 range is a MOD-host quirk).
- Loop audio is not saved with the project (same as the LV2 build: no state
  extension). A sample-rate change re-creates the engine and clears loops.
- DPF exposes 2048 MIDI-CC proxy parameters plus buffer-size/sample-rate
  parameters; the engine ignores CC.
- `time:beat` is sent as Float (the engine's parsed type): precision drops for
  very long sessions.

## Checks
- `make tests` (includes the bridge test)
- `deps/vst3sdk/build/bin/Release/validator -e build/vst3/aloschen.vst3`
- `deps/pluginval/pluginval --strictness-level 10 --validate build/vst3/aloschen.vst3`
- `tests/host/run_reaper.sh OUTDIR`: Reaper renders with transport running; prints
  per-bar peaks of the rendered output.
