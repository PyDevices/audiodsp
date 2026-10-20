"""Deterministic Oscillator PCM.

    oscillator_probe.py audiooscillator

`audiooscillator` is audiodsp's own module, so this has no oracle: what the
comparison pins is that every interpreter renders it identically. Every input
here is integer arithmetic on array("h"), so the material is the same bytes on
every interpreter before the oscillator sees it, and the oscillator's own
exponential and sine are polynomials rather than libm for the same reason.

Each case drives one input the node exists for -- sync from another
oscillator, linear FM, pulse width and pitch that move inside a block -- plus
the waveforms on their own, a stereo pair from one phase, and a small
buffer_size. A per-block sum and an FNV-1a checksum are printed, because a sum
alone cancels two samples that drift in opposite directions.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiooscillator"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
osc = __import__(MODULE)

RATE = 48000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def table(values):
    return audiocore.RawSample(array("h", values), sample_rate=RATE,
                               channel_count=1)


def triangle(frames, period, level):
    values = []
    half = period // 2
    for frame in range(frames):
        step = frame % period
        rise = step if step < half else period - step
        values.append(rise * 2 * level // half - level)
    return values


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("osc", tag, index, len(data), sum(data), checksum(data))


for name, wave in (("saw", osc.SAW), ("pulse", osc.PULSE),
                   ("triangle", osc.TRIANGLE), ("sine", osc.SINE),
                   ("all", osc.SAW + osc.PULSE + osc.TRIANGLE + osc.SINE)):
    for hz in (110.0, 2345.6, 9876.5):
        node = osc.Oscillator(sample_rate=RATE, frequency=hz, waveform=wave,
                              pulse_width=0.3, level=0.4)
        emit("%s-%d" % (name, int(hz)), node, 3)

pair = osc.Oscillator(sample_rate=RATE, channel_count=2, frequency=777.0,
                      waveform=osc.SAW, waveform_right=osc.PULSE)
emit("stereo", pair, 3)

master = osc.Oscillator(sample_rate=RATE, frequency=123.4,
                        waveform=osc.PULSE)
slave = osc.Oscillator(sample_rate=RATE, frequency=531.7,
                       waveform=osc.SAW + osc.TRIANGLE + osc.SINE,
                       level=0.3, sync=master)
emit("sync", slave, 6)

fm = osc.Oscillator(sample_rate=RATE, frequency=440.0, waveform=osc.SINE,
                    fm_depth=1500.0, fm=table(triangle(96, 96, 30000)))
emit("fm", fm, 4)

pwm = osc.Oscillator(sample_rate=RATE, frequency=1000.0, waveform=osc.PULSE,
                     pwm_depth=0.9, pwm=table(triangle(480, 480, 32000)))
emit("pwm", pwm, 4)

steps = [0] * 100 + [3277] * 100 + [-6554] * 100 + list(range(-8000, 8000, 160))
pitch = osc.Oscillator(sample_rate=RATE, frequency=330.0, waveform=osc.SAW,
                       pitch=table(steps))
emit("pitch", pitch, 4)

small = osc.Oscillator(sample_rate=RATE, frequency=1234.0, waveform=osc.PULSE,
                       buffer_size=64 * 2, pwm=table(triangle(64, 64, 20000)))
emit("small", small, 8)

moved = osc.Oscillator(sample_rate=RATE, frequency=200.0, waveform=osc.SAW)
emit("moved-before", moved, 1)
moved.set(frequency=3000.0, waveform=osc.TRIANGLE)
emit("moved-after", moved, 1)
moved.clear()
emit("moved-clear", moved, 1)
