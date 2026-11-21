import struct
import sys
import wave

rate = 48000
bar = 96000
n = bar * 6
w = wave.open(sys.argv[1], "wb")
w.setnchannels(2)
w.setsampwidth(2)
w.setframerate(rate)
fr = bytearray()
for i in range(n):
    v = 26000 if (i < bar * 3 and (i % bar) < 32) else 0
    fr += struct.pack("<hh", v, v)
w.writeframes(bytes(fr))
w.close()
