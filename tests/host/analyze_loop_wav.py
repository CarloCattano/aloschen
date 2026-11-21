import struct
import sys
import wave

w = wave.open(sys.argv[1])
n = w.getnframes()
ch = w.getnchannels()
sw = w.getsampwidth()
rate = w.getframerate()
raw = w.readframes(n)
print("frames", n, "ch", ch, "sw", sw, "rate", rate)
bar = rate * 2
if sw == 3:

    def s(i):
        b = raw[i * 3 : (i + 1) * 3]
        v = int.from_bytes(b, "little", signed=True)
        return v / 8388608
elif sw == 2:

    def s(i):
        return struct.unpack_from("<h", raw, i * 2)[0] / 32768
else:

    def s(i):
        return struct.unpack_from("<f", raw, i * 4)[0]


for b in range(n // bar):
    peak = 0
    pos = []
    for f in range(b * bar, (b + 1) * bar):
        a = abs(s(f * ch))
        if a > peak:
            peak = a
        if a > 0.1:
            pos.append(f % bar)
    print(
        "bar",
        b,
        f"peakL {peak:.3f}",
        "loud",
        len(pos),
        "phase range",
        (min(pos), max(pos)) if pos else None,
    )
