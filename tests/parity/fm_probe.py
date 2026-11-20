# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Deterministic audiofm PCM and bank bytes, for every interpreter to agree on.

    fm_probe.py audiofm

`audiofm` is audiodsp's own module, so there is no oracle: what the gate pins
is that every interpreter renders it identically. The kernel is fixed point
throughout, and the bindings hand it only integers (the pitch bend is turned
into whole 14-bit steps before it reaches the kernel), so the output has no
reason to differ by a single bit.

Sections:

- **alg** renders every algorithm with all six operators on and the feedback
  at 7, and prints each block's checksum and peak. A peak of 0 is a failure
  as well as a difference.
- **voice** plays the same chord through press, release, sustain, pitch bend
  and the mod wheel.
- **bank** packs 32 voices, unpacks them, and prints the bank's checksum and
  whether every voice came back unchanged.
"""

import sys

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiofm"
fm = __import__(MODULE)

RATE = 32000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def loudest(data):
    top = 0
    for position in range(0, len(data), 2):
        word = data[position] | (data[position + 1] << 8)
        if word >= 32768:
            word = 65536 - word
        if word > top:
            top = word
    return top


def emit(tag, synth, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(synth)[1])
        print("fm", tag, index, len(data), checksum(data), loudest(data))


def everything_on(algorithm):
    patch = fm.Patch()
    patch.algorithm = algorithm
    patch.feedback = 7
    for number in range(1, 7):
        op = patch.operators[number - 1]
        op.level = 88
        op.coarse = number
        op.rates = (99, 40, 40, 60)
        op.levels = (99, 80, 60, 0)
    return patch


for algorithm in range(1, 33):
    synth = fm.Synth(sample_rate=RATE, patch=everything_on(algorithm))
    synth.press(57, 110)
    emit("alg%d" % algorithm, synth, 3)

patch = everything_on(5)
patch.lfo_speed = 70
patch.lfo_pitch_depth = 40
patch.lfo_amp_depth = 60
patch.lfo_pitch_sens = 5
for op in patch.operators:
    op.amp_mod = 3
    op.velocity = 4
synth = fm.Synth(voices=4, sample_rate=RATE, channel_count=2, patch=patch)
for note in (48, 55, 60, 64, 67):
    synth.press(note, note)
emit("chord", synth, 4)
synth.sustain = True
synth.release(48)
synth.release(55)
synth.pitch_bend = 0.5
synth.mod_wheel = 1.0
emit("held", synth, 4)
synth.sustain = False
emit("lifted", synth, 8)
print("active", synth.active)

patches = []
for index in range(32):
    voice = everything_on(index + 1)
    voice.transpose = index
    voice.name = "PROBE %d" % index
    patches.append(voice)
bank = fm.Patch.pack_bank(patches)
back = fm.Patch.bank(bank)
same = 0
for original, copy in zip(patches, back):
    same += 1 if original.pack() == copy.pack() else 0
print("bank", len(bank), checksum(bank), same, back[7].name)
