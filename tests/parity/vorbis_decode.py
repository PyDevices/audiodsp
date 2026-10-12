"""Decode one .ogg to raw PCM through audiovorbis, for verify_vorbis.py.

    vorbis_decode.py IN.ogg OUT.raw [--fault swap|drop]

Runs on MicroPython and CPython alike. The output is the interleaved,
little-endian int16 that audiocore.get_buffer() hands out, block after block,
and the last line printed is "rate channels frames".

The faults are what verify_vorbis.py --fault plants, here where the decoder's
output is written, so that the comparison is what gets tested:

- swap: left and right exchanged in every stereo frame (a wrong channel order);
- drop: one block in the middle of the file never written (a lost packet).
"""

import sys

import audiocore
import audiovorbis

source, target = sys.argv[1], sys.argv[2]
fault = sys.argv[sys.argv.index("--fault") + 1] if "--fault" in sys.argv else None

decoder = audiovorbis.VorbisDecoder(open(source, "rb"))
channels = decoder.channel_count
blocks = []
with open(target, "wb") as out:
    index = frames = 0
    while True:
        result, buf = audiocore.get_buffer(decoder)
        data = bytes(buf)
        if fault == "swap" and channels == 2:
            swapped = bytearray(len(data))
            for at in range(0, len(data), 4):
                swapped[at:at + 2] = data[at + 2:at + 4]
                swapped[at + 2:at + 4] = data[at:at + 2]
            data = bytes(swapped)
        if not (fault == "drop" and index == 20):
            out.write(data)
            frames += len(data) // (2 * channels)
        index += 1
        if result != 1:
            break
print(decoder.sample_rate, channels, frames)
decoder.deinit()
