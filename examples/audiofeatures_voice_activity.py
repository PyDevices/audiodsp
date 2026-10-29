# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Find where someone is speaking in a WAV file.

An audiofeatures Frontend flags every 10 ms slice it makes as speech or not,
beside the features a wake word model reads. The program feeds it a WAV file
a block at a time, reads the flags, and prints each stretch of speech with
its start and end time, so a file of a few phrases prints a line for each.

Needs: audiodsp, built into MicroPython or installed on CPython with
pip install pydevices-audiodsp, and a 16-bit PCM WAV file at 16 kHz or
higher, mono or stereo, that starts with a moment of quiet.

Run it:
    micropython audiofeatures_voice_activity.py

Tested on: the unix port of MicroPython and CPython 3.12 on Linux.
"""

import struct

import audiofeatures

# Change these to match your setup.
FILENAME = "speech.wav"
BLOCK = 480  # frames read and fed at a time

# Walk the WAV's chunks to its format and the start of its samples.
f = open(FILENAME, "rb")
assert f.read(12)[8:] == b"WAVE", "not a WAV file"
while True:
    tag, size = struct.unpack("<4sI", f.read(8))
    if tag == b"data":
        break
    body = f.read(size + (size & 1))
    if tag == b"fmt ":
        kind, channels, rate = struct.unpack("<HHI", body[:8])
        assert kind == 1 and body[14] == 16, "16-bit PCM only"

# The features go to a buffer this program ignores; the flags, one byte a
# slice, go to their own.
frontend = audiofeatures.Frontend()
features = bytearray(audiofeatures.FEATURE_SIZE * 10)
voice = bytearray(10)

# Feed the samples and watch the flags change. Slice k starts k * 10 ms in.
left, slices, start, voiced = size, 0, None, 0
while left > 0:
    block = f.read(min(left, BLOCK * 2 * channels))
    left -= len(block)
    frontend.feed(block, rate, channels)
    count = frontend.readinto(features, voice)
    for i in range(count):
        voiced += voice[i]
        if voice[i] and start is None:
            start = slices + i
        elif not voice[i] and start is not None:
            print("speech from %.2f s to %.2f s" % (start / 100, (slices + i) / 100))
            start = None
    slices += count
f.close()

# Speech still going at the end of the file ends with it.
if start is not None:
    print("speech from %.2f s to the end" % (start / 100))
print("%.2f s of speech in %.2f s of audio" % (voiced / 100, slices / 100))
