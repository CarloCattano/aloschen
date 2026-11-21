#!/usr/bin/env bash
# Usage: tests/host/run_reaper.sh OUTDIR
# Needs: reaper, python3, ~/.vst3/aloschen.vst3 (link to build/vst3/aloschen.vst3).
# Renders OUTDIR/render.wav and prints per-bar peaks; bars 3..5 (input silent)
# must be non-silent with loud samples at the same in-bar phase as the input.
# Reaper stays open playing with the editor shown; quit it when done.
set -euo pipefail
out=${1:?usage: run_reaper.sh OUTDIR}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
rm -f "$out/render.wav"
python3 -I "$here/make_impulse_wav.py" "$out/input.wav"
printf '[reaper]\nlinux_audio_mode=1\n' >"$out/reaper.ini"
ALO_HOST_DIR="$out" reaper -nosplash -new -cfgfile "$out/reaper.ini" "$here/reaper_vst3_loop.lua" >"$out/reaper-stdout.log" 2>&1 &
for _ in $(seq 1 60); do
  grep -q "render done" "$out/reaper-log.txt" 2>/dev/null && break
  sleep 1
done
python3 -I "$here/analyze_loop_wav.py" "$out/render.wav"
