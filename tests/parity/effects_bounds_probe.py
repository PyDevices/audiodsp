"""Deterministic PCM for the effect bounds and silence fills upstream fixed.

    effects_bounds_probe.py audiodelays audiofilters

Upstream CircuitPython fixed a batch of these after 10.3.0, in 6dddbda87
("audiodelays, audiofilters, audiofreeverb: buffer lengths and silence
fills", released in 11.0.0-alpha.1), and this port has the same fixes
(audiodsp#201). Each case here is one of them:

- a Chorus asked for a delay longer than the line it allocated;
- a MultiTapDelay whose maximum is shorter than one audio buffer (the floor
  used to come after the ceiling, so the line ran past its memory);
- a PitchShift window too small to hold one frame, which is refused now;
- unsigned 16-bit silence from PitchShift, GranularPitchShift and Distortion
  with nothing playing, which is the midpoint 0x8000 and not the zeros a
  byte-wise memset() wrote;
- a hard-clipped Distortion at full scale, whose top is 32767 and no longer
  32768 wrapped to -32768.

Freeverb's stereo bank switch is covered by effects_component_probe.py.
"""

import sys
from array import array

import audiocore

DELAYS = __import__(sys.argv[1] if len(sys.argv) > 1 else "audiodelays")
FILTERS = __import__(sys.argv[2] if len(sys.argv) > 2 else "audiofilters")


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames, channels, scale=9):
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            values.append((((frame * (131 + channel * 7)) % 2001) - 1000)
                          * scale)
    return audiocore.RawSample(values, sample_rate=8000,
                               channel_count=channels)


def words(data, count=4):
    return tuple(data[i] | (data[i + 1] << 8) for i in range(0, 2 * count, 2))


def blocks(tag, effect, src, count):
    if src is not None:
        effect.play(src, loop=True)
    for index in range(count):
        data = bytes(audiocore.get_buffer(effect)[1])
        print(tag, index, len(data), sum(data), checksum(data))


# A delay asked for far past the 5 ms line Chorus allocated.
for channels in (1, 2):
    blocks("chorus_long", DELAYS.Chorus(
        max_delay_ms=5, delay_ms=200, voices=2, mix=0.5, buffer_size=256,
        sample_rate=8000, channel_count=channels), source(700, channels), 4)

# A 10 ms maximum is 80 frames, under the 256-frame buffer.
for channels in (1, 2):
    blocks("multitap_short_max", DELAYS.MultiTapDelay(
        max_delay_ms=10, delay_ms=1, decay=0.5, mix=0.5, buffer_size=512,
        sample_rate=8000, channel_count=channels), source(700, channels), 4)

for channels in (1, 2):
    try:
        DELAYS.PitchShift(window=channels, buffer_size=512, sample_rate=8000,
                          channel_count=channels)
        print("pitchshift_window", channels, "accepted")
    except ValueError:
        print("pitchshift_window", channels, "ValueError")
    DELAYS.PitchShift(window=2 * channels, buffer_size=512,
                      sample_rate=8000, channel_count=channels)
    print("pitchshift_window", channels * 2, "accepted")

# Unsigned 16-bit silence with nothing playing.
for name, effect in (
        ("pitchshift", DELAYS.PitchShift(
            buffer_size=64, sample_rate=8000, samples_signed=False)),
        ("granular", DELAYS.GranularPitchShift(
            buffer_size=64, sample_rate=8000, samples_signed=False)),
        ("distortion", FILTERS.Distortion(
            buffer_size=64, sample_rate=8000, samples_signed=False))):
    data = bytes(audiocore.get_buffer(effect)[1])
    print("unsigned_silence", name, len(data), words(data), checksum(data))

# Full scale, hard clipped, all wet: nothing wraps to the bottom.
for channels in (1, 2):
    effect = FILTERS.Distortion(
        drive=0.9, pre_gain=24, post_gain=0, mode=FILTERS.DistortionMode.CLIP,
        soft_clip=False, mix=1.0, buffer_size=256, sample_rate=8000,
        channel_count=channels)
    effect.play(source(700, channels, scale=32), loop=True)
    for index in range(3):
        data = bytes(audiocore.get_buffer(effect)[1])
        samples = [data[i] | (data[i + 1] << 8) for i in range(0, len(data), 2)]
        samples = [s - 65536 if s >= 32768 else s for s in samples]
        print("distortion_clip", channels, index, len(data), min(samples),
              max(samples), checksum(data))
