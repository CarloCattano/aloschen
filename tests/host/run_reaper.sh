#!/usr/bin/env bash
# Launch a private REAPER test instance. Analyze render.wav once "render done"
# appears in reaper-log.txt (the test project/editor remain open).
set -euo pipefail
out=${1:?usage: run_reaper.sh OUTDIR [loop|click|click-7-8|click-tempo]}
mode=${2:-loop}
case "$mode" in loop | click | click-7-8 | click-tempo) ;; *)
  echo "Unknown test: $mode" >&2
  exit 2
  ;;
esac
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
rm -f "$out/render.wav"
if [[ $mode == loop ]]; then
  python3 -I "$here/make_impulse_wav.py" "$out/input.wav"
fi
printf '[reaper]\nlinux_audio_mode=%s\n' "${ALO_REAPER_AUDIO_MODE:-0}" >"$out/reaper.ini"
exec env ALO_HOST_DIR="$out" ALO_HOST_MODE="$mode" reaper -newinst -nosplash -cfgfile "$out/reaper.ini" "$here/reaper_vst3_loop.lua"
