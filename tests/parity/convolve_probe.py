"""Deterministic Convolver PCM.

    convolve_probe.py audioconvolve

No oracle, like multiply_probe.py and feedback_delay_probe.py: `audioconvolve`
is audiodsp's own module, with no ancestor in CircuitPython or in
micropython-vst3's engine. What the golden pins is that every interpreter
renders it identically.

This is the most float-dependent fixture in the suite. Every output sample is
a sum of hundreds of float products routed through two transforms, and the
transforms' twiddle factors come from a series rather than libm precisely so
that three interpreters can agree on them. If any of that were left to the
platform, this is where it would show first -- the whole point of the
arrangement in shared/audiodsp_trig.h is that it does not.

The synthesized impulse is here as well as the loaded one, because its noise
and its exponentials are generated in C for the same reason: a room that is
not bit-identical between builds is not a room, it is three rooms.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioconvolve"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
convolve = __import__(MODULE)

SAMPLE_RATE = 8000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames=1200, level=13000):
    """A burst and then silence. A convolution has to be driven with
    something that stops, so the tail arrives where the input is not."""
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            if frame < 90:
                shape = ((frame * (97 + channel * 18)) % 2001) - 1000
                values.append(shape * level // 1000)
            else:
                values.append(0)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def impulse(frames, channels, seed=12345):
    """A deterministic impulse: a leading spike, then decaying pseudo-noise.
    Deliberately not a smooth shape - a response with structure at every
    tap exercises every bin, where a few discrete echoes would leave most of
    the spectrum multiplied by nothing."""
    values = array("h")
    state = seed
    level = 32767
    for frame in range(frames):
        for channel in range(channels):
            if frame == 0:
                values.append(32767 if channel == 0 else -32767)
                continue
            state = (state * 1103515245 + 12345) & 0x7fffffff
            values.append(((state >> 8) % (2 * level + 1)) - level)
        level = level * 61 // 64
        if level < 1:
            level = 1
    return values


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("conv", tag, index, len(data), sum(data), checksum(data))


# One partition, several, and a length that does not divide evenly -- the
# last is where an off-by-one in the partition count would show.
for tag, taps, channels in (("one", 256, 1), ("many", 1024, 1),
                            ("ragged", 700, 1), ("stereo", 768, 2)):
    node = convolve.Convolver(impulse=impulse(taps, channels), max_taps=1024,
                              impulse_channels=channels,
                              sample_rate=SAMPLE_RATE, mix=0.5)
    node.play(source())
    emit(tag, node, 8)

# A mono impulse into a stereo convolver and a stereo one into a mono
# convolver: the two directions the channel counts are allowed to disagree.
node = convolve.Convolver(impulse=impulse(512, 1), max_taps=512,
                          impulse_channels=1, ir_channels=2,
                          sample_rate=SAMPLE_RATE, mix=0.5)
node.play(source())
emit("mono-to-stereo", node, 6)

node = convolve.Convolver(impulse=impulse(512, 2), max_taps=512,
                          impulse_channels=2, ir_channels=1,
                          sample_rate=SAMPLE_RATE, mix=0.5)
node.play(source())
emit("stereo-to-mono", node, 6)

# Mix at both ends and in the middle. Dry at unity until halfway is
# audiofreeverb's convention, not audiodelays' -- this is a reverb.
for tag, mix in (("dry", 0.0), ("half", 0.5), ("wet", 1.0)):
    node = convolve.Convolver(impulse=impulse(512, 1), max_taps=512,
                              sample_rate=SAMPLE_RATE, mix=mix)
    node.play(source())
    emit("mix-" + tag, node, 4)

# The synthesized room, which is where the C's noise and exponentials are.
for tag, settings in (
    ("synth-plain", {"decay": 0.4}),
    ("synth-damped", {"decay": 0.4, "damping_hz": 1200.0}),
    ("synth-delayed", {"decay": 0.4, "predelay_ms": 12.0,
                       "diffusion_ms": 6.0}),
    ("synth-seeded", {"decay": 0.4, "seed": 99}),
):
    node = convolve.Convolver(max_taps=1024, ir_channels=2,
                              sample_rate=SAMPLE_RATE, mix=1.0)
    node.synthesize(**settings)
    node.play(source())
    emit(tag, node, 8)

# Reloading mid-stream, and clearing. The impulse survives a clear; the
# history does not.
node = convolve.Convolver(impulse=impulse(512, 1), max_taps=1024,
                          sample_rate=SAMPLE_RATE, mix=1.0)
node.play(source())
emit("late-a", node, 3)
node.load(impulse(1024, 1, seed=777), 1, 0.5)
emit("late-b", node, 3)
node.clear()
emit("late-c", node, 3)

# Nothing loaded is a bypass, not silence, and it has no latency either.
node = convolve.Convolver(max_taps=512, sample_rate=SAMPLE_RATE)
node.play(source())
emit("bypass", node, 3)
print("conv taps-empty", node.taps, "latency", node.latency)

# No source at all: silence, never a short block, never finished.
node = convolve.Convolver(impulse=impulse(256, 1), max_taps=256,
                          sample_rate=SAMPLE_RATE)
emit("starved", node, 2)
print("conv taps-loaded", node.taps)
