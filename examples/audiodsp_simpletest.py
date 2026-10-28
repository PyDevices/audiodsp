# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Play a chord on synthio, filter it, and save it as a WAV file.

A synthio Synthesizer plays three notes, an audiofilters low-pass softens
them, and the program pulls the result one block at a time and writes it
to a WAV file you can open in any audio player. No speaker is needed.

Needs: audiodsp, built into MicroPython or installed on CPython with
pip install pydevices-audiodsp. CircuitPython has these modules built in.

Run it:
    micropython audiodsp_simpletest.py

Tested on: the unix port of MicroPython, the unix port of CircuitPython,
and CPython 3.12 on Linux.
"""

import struct

import audiocore
import audiofilters
import synthio

# Change these to match your setup.
FILENAME = "chord.wav"
NOTES = (60, 64, 67)  # MIDI note numbers: C, E and G
SAMPLE_RATE = 22050
SECONDS = 2
CUTOFF = 1200  # Hz; lower it for a darker sound

# A synthesizer with a gentle attack and release.
envelope = synthio.Envelope(attack_time=0.05, release_time=0.5)
synth = synthio.Synthesizer(sample_rate=SAMPLE_RATE, envelope=envelope)

# A low-pass filter that takes the synthesizer as its input.
lowpass = synthio.Biquad(synthio.FilterMode.LOW_PASS, CUTOFF)
effect = audiofilters.Filter(filter=lowpass, sample_rate=SAMPLE_RATE)
effect.play(synth)

# Write the WAV header now; the data size is filled in at the end.
out = open(FILENAME, "wb")
out.write(b"RIFF\0\0\0\0WAVEfmt ")
out.write(struct.pack("<IHHIIHH", 16, 1, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16))
out.write(b"data\0\0\0\0")

# Hold the chord, then let it go for the last half second.
synth.press(NOTES)
size = 0
while size < SECONDS * SAMPLE_RATE * 2:
    if size >= (SECONDS - 0.5) * SAMPLE_RATE * 2:
        synth.release_all()
    status, block = audiocore.get_buffer(effect)
    size += out.write(block)

# Go back and fill in the two sizes.
out.seek(4)
out.write(struct.pack("<I", 36 + size))
out.seek(40)
out.write(struct.pack("<I", size))
out.close()
print("Wrote %d samples to %s" % (size // 2, FILENAME))
