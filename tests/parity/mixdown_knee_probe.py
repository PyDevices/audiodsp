"""Drive the synthio mix-down limiter across its knee and print digests.

WHY THIS PROBE EXISTS (audiodsp#27)
==================================

Every other parity probe in this repository renders material that stays
well inside the mix-down limiter's linear region, so all of them are
byte-identical at *any* value of ``CIRCUITPY_SYNTHIO_MAX_CHANNELS``. A
change to the voice ceiling therefore ships green through the whole gate
set. This probe is the exception: its material is loud enough and
polyphonic enough to push the summed signal past the +/-28000 knee, where
``SYNTHIO_MIX_DOWN_SCALE`` -- which is a function of the ceiling -- starts
to matter, so its output moves when the ceiling moves.

The ceiling site the CPython target actually reads is
``src/cpython/synthio.py``'s ``Synthesizer.max_polyphony``, handed to
``_audiodsp.mixdown_i32`` as the limiter's divisor. That is the one site
this probe can observe. The header, ``micropython.mk`` and
``micropython.cmake`` copies are read by no CI gate; ``tests/
test_voice_ceiling_consistency.py`` remains the only guard for those.

MATERIAL
========

A full-scale square wave (eight samples at +32767, eight at -32768), K
identical ``synthio.Note(220, waveform=square)`` objects pressed together
on one ``Synthesizer(sample_rate=22050, channel_count=1)``, two blocks
pulled per K. K=1 stays below the knee; K>=2 crosses it.

Measured 2026-09-06 on all three runtimes -- these are the numbers to
check by eye if the probe is ever edited:

    K=1   peak 16383, byte sums 68008 / 63296 -- byte-identical on the
                       CPython target, the anchor's bin/micropython and the pinned
                       CircuitPython oracle. Below the knee, so the ceiling
                       cannot reach it.
    K=2   peak 28010 (port, N=64) vs 28046 (oracle, N=14)
    K=3   peak 28042 vs 28202
    K=6   peak 28139 vs 28669
    K=10  peak 28268 vs 29292

The port/oracle divergence above the knee is the ceiling and nothing
else: SYNTHIO_MIX_DOWN_SCALE is 129 at N=64 and 623 at N=14, and it
multiplies every sample above the knee. No above-knee material can be
both above the knee and oracle-identical while those two numbers differ.
See ``src/synthio/__init__.h``'s ceiling comment and
``tests/parity/golden/mixdown_knee.json``.

PRINT FORMAT -- FIXED, because the golden is a function of it
=============================================================

One line per (K, block), fields space-separated in this order:

    section voices block length sum checksum peak

where ``section`` is ``below_knee`` for K=1 and ``above_knee`` for K>1,
``length`` is ``len(data)`` in bytes, ``sum`` is the sum of the raw
unsigned bytes, ``checksum`` is FNV-1a over those bytes (per-block sums
alone cancel opposite drift inside a block), and ``peak`` is the largest
absolute signed 16-bit sample in the block.

Plain Python only: this runs unchanged under CPython,
the anchor's ``bin/micropython`` and ``bin/circuitpython``.
"""

from array import array

import audiocore
import synthio


KNEE_VOICES = (1, 2, 3, 6, 10)
BLOCKS_PER_VOICE_COUNT = 2
SAMPLE_RATE = 22050
FREQUENCY = 220


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def peak(data):
    largest = 0
    for index in range(0, len(data) - 1, 2):
        value = data[index] | (data[index + 1] << 8)
        if value >= 32768:
            value -= 65536
        if value < 0:
            value = -value
        if value > largest:
            largest = value
    return largest


def square():
    values = array("h")
    for _ in range(8):
        values.append(32767)
    for _ in range(8):
        values.append(-32768)
    return values


waveform = square()

for voices in KNEE_VOICES:
    section = "below_knee" if voices == 1 else "above_knee"
    synth = synthio.Synthesizer(sample_rate=SAMPLE_RATE, channel_count=1)
    notes = tuple(
        synthio.Note(FREQUENCY, waveform=waveform) for _ in range(voices))
    synth.press(notes)
    for block in range(BLOCKS_PER_VOICE_COUNT):
        data = bytes(audiocore.get_buffer(synth)[1])
        print(section, voices, block, len(data), sum(data), checksum(data),
              peak(data))
