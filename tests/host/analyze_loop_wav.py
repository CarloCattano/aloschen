"""Check a REAPER render against the grid, not against callback timestamps."""

import argparse
import math
import wave
from fractions import Fraction

RATE = 48000


def verify(path, mode):
    with wave.open(path, "rb") as wav:
        channels, width = wav.getnchannels(), wav.getsampwidth()
        if wav.getframerate() != RATE or channels != 2:
            raise ValueError("expected a 48 kHz stereo render")
        raw = wav.readframes(wav.getnframes())
    stride = channels * width
    if len(raw) < RATE * 12 * stride:
        raise ValueError("render is shorter than the requested 12 seconds")

    def sample(frame, channel):
        start = frame * stride + channel * width
        return int.from_bytes(raw[start : start + width], "little", signed=True) / (
            2 ** (8 * width - 1)
        )

    if mode == "loop":
        expected = [b * RATE * 2 + 513 for b in range(2, 6)]
        actual = [
            f for f in range(RATE * 4, RATE * 12) if sample(f, 0) > 0.7 and sample(f, 1) < -0.7
        ]
        if expected != actual:
            raise ValueError(f"loop marker frames: expected {expected}, got {actual[:20]}")
        print(f"Loop: {len(actual)} stereo markers, all at exact sample positions")
        return

    if mode == "click-tempo":
        grid = [i * 24000 for i in range(4)]
        grid += list(range(96000, RATE * 12, 30000))
    else:
        step = Fraction(RATE * 60, 131 * 2) if mode == "click-7-8" else Fraction(24000)
        grid = [math.ceil(i * step) for i in range(math.ceil(RATE * 12 / step))]
    for boundary in grid:
        if abs(sample(boundary, 0)) > 5e-5 or abs(sample(boundary + 1, 0)) < 5e-5:
            raise ValueError(f"missing or shifted click at frame {boundary}")
    pulse = RATE // 50
    for start, end in zip(grid, grid[1:] + [RATE * 12]):
        for f in range(start + pulse, end):
            if abs(sample(f, 0)) > 5e-5 or abs(sample(f, 1)) > 5e-5:
                raise ValueError(f"off-grid click at frame {f}")
    print(f"Click: {len(grid)} onsets at exact sample positions; silent between pulses")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav")
    parser.add_argument("mode", choices=("loop", "click", "click-7-8", "click-tempo"))
    args = parser.parse_args()
    try:
        verify(args.wav, args.mode)
    except (ValueError, OSError, wave.Error) as exc:
        parser.exit(1, f"FAIL: {exc}\n")


if __name__ == "__main__":
    main()
