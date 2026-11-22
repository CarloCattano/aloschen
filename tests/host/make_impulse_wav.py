"""Generate a stereo marker in bar two for the REAPER loop test."""

import struct
import sys
import wave

rate = 48000
bar = rate * 2
marker = 513
with wave.open(sys.argv[1], "wb") as wav:
    wav.setnchannels(2)
    wav.setsampwidth(2)
    wav.setframerate(rate)
    audio = bytearray(bar * 6 * 4)
    struct.pack_into("<hh", audio, (bar + marker) * 4, 26000, -26000)
    wav.writeframes(audio)
