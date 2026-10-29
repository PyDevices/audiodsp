# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Print the speech features a wake word model sees in a WAV file.

An audiofeatures Frontend turns every 10 ms of audio into a slice of 40
values, one per mel band from 125 Hz to 7.5 kHz. The program feeds it a WAV
file a block at a time and prints each slice as a row of characters, low
bands on the left, darker for more energy, so speech shows up as dense rows
and silence as blank ones.

Needs: audiodsp, built into MicroPython or installed on CPython with
pip install pydevices-audiodsp, and a 16-bit PCM WAV file at 16 kHz or
higher, mono or stereo.

Run it:
    micropython audiofeatures_simpletest.py

Tested on: the unix port of MicroPython and CPython 3.12 on Linux.
"""

import struct

import audiofeatures

# Change these to match your setup.
FILENAME = "speech.wav"
BLOCK = 480  # frames read and fed at a time
SHADES = " .:-=+*#%@"  # from no energy to the most


def main():
    with open(FILENAME, "rb") as f:
        # Walk the WAV's chunks to its format and the start of its samples.
        assert f.read(12)[8:] == b"WAVE", "not a WAV file"
        while True:
            tag, size = struct.unpack("<4sI", f.read(8))
            if tag == b"data":
                break
            body = f.read(size + (size & 1))
            if tag == b"fmt ":
                kind, channels, rate = struct.unpack("<HHI", body[:8])
                assert kind == 1 and body[14] == 16, "16-bit PCM only"

        # Feed the samples to the frontend and print each slice it makes.
        frontend = audiofeatures.Frontend()
        features = bytearray(audiofeatures.FEATURE_SIZE * 10)
        left, rows = size, 0
        while left > 0:
            block = f.read(min(left, BLOCK * 2 * channels))
            left -= len(block)
            frontend.feed(block, rate, channels)
            count = frontend.readinto(features)
            for i in range(count * audiofeatures.FEATURE_SIZE):
                # Each value is a signed byte, -128 to 127, held unsigned here.
                value = (features[i] + 128) & 0xFF
                print(SHADES[value * len(SHADES) // 256], end="")
                if i % audiofeatures.FEATURE_SIZE == audiofeatures.FEATURE_SIZE - 1:
                    print()
            rows += count

    # Every 10 ms after the first 30 ms makes a slice.
    print("%d slices from %d Hz audio, %d dropped" % (rows, rate, frontend.stats()["dropped"]))


if __name__ == "__main__":
    main()
