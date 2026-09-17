"""Every value an `audiomixer` voice scales differently in float32 and float64.

The native kernel forms a voice's scaled sample in **single** precision
(`src/audiomixer/Mixer.c`, `mult16signed`: `float mod_mul = (float)level /
(float)((1 << 15) - 1); int32_t intermediate = (int32_t)(ai * mod_mul);`).
Both precisions truncate toward zero, so they part company exactly where the
true product sits within a float32 rounding of an integer -- and then they
differ by a whole LSB, not by a rounding.

The material is the 56 `int16` values that flip at level `100/127`, enumerated
rather than sampled: 28 against the left channel's Q15 scale of 25800 and 28
against the right's 25801. Each appears twice in a row, so each is scaled once
by the left scale and once by the right. The leading zero pair is what opens
the level gate on the first word, so no sample here renders at level zero.

A float64 twin and a float32 native disagree on all 56. audioif#84.
"""

from array import array

import audiocore
import audiomixer

SAMPLE_RATE = 48000
BUFFER_SIZE = 2048
LEVEL = 100 / 127

# Enumerated, not chosen: every int16 whose truncated product differs between
# the two precisions at this level. Left (Q15 25800) and right (25801) have 28
# each and share none.
FLIPPING = (
    -32466, -31544, -31135, -30321, -29503, -29098, -27875, -27871,
    -26652, -26239, -25429, -24908, -24607, -24206, -23276, -22983,
    -21760, -21644, -18681, -17049, -15772, -14549, -13326, -12454,
    -12103, -10880, -10822, -6227, 6227, 10822, 10880, 12103,
    12454, 13326, 14549, 15772, 17049, 18681, 21644, 21760,
    22983, 23276, 24206, 24607, 24908, 25429, 26239, 26652,
    27871, 27875, 29098, 29503, 30321, 31135, 31544, 32466,
)


def material():
    values = array("h", [0, 0])
    for value in FLIPPING:
        values.append(value)
        values.append(value)
    return values


mixer = audiomixer.Mixer(
    voice_count=1, buffer_size=BUFFER_SIZE, sample_rate=SAMPLE_RATE,
    channel_count=2, bits_per_sample=16, samples_signed=True)
mixer.voice[0].level = LEVEL
mixer.voice[0].panning = 0.0
mixer.voice[0].play(
    audiocore.RawSample(material(), sample_rate=SAMPLE_RATE, channel_count=2),
    loop=True)

block = bytes(audiocore.get_buffer(mixer)[1])


def sample(data, index):
    """One signed 16-bit little-endian sample. `array.frombytes` is CPython's
    alone -- MicroPython's `array` does not have it, and the probe has to read
    the same bytes on all three interpreters."""
    value = data[index * 2] | (data[index * 2 + 1] << 8)
    return value - 65536 if value & 0x8000 else value


# One line per input value: what the left scale and the right scale make of it.
for index in range(len(FLIPPING)):
    print("flip", FLIPPING[index],
          sample(block, 2 + index * 2), sample(block, 3 + index * 2))
print("block", len(block) // 2,
      sum(sample(block, i) for i in range(len(block) // 2)))
