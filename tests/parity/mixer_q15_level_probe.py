"""A voice's level and pan are Q15 integers, and a sample is `(x * q) >> 15`.

CircuitPython 11.0.0-alpha.2 (adafruit/circuitpython#11617) scales a voice in
integers on every core: `src/audiomixer/Mixer.c`'s `mult16signed` is
`(ai * mul) >> 15`, with the product floored rather than truncated toward zero,
and a centred pan is `32768 - panning`, so a voice at level 1.0 and pan 0 is
exactly 32768 on both channels and a wire. Before that, the quotient was
`mul / 32767.0f`, and this port added a unity special case to make 1.0 a wire.

The material is chosen where those differ: the rails, the small values either
side of zero (a floor sends -1 at level 0.5 to -1; truncation sent it to 0), and
a spread of mid-range values. Each pair of values is scaled once by the left
multiplier and once by the right, at four levels and five pans, in stereo and
mono. The leading zero pair opens the level gate on the first word, so no
sample here renders at level zero.
"""

from array import array

import audiocore
import audiomixer

SAMPLE_RATE = 48000
BUFFER_SIZE = 2048

VALUES = (
    -32768, -32767, -30000, -22983, -10822, -3, -2, -1,
    1, 2, 3, 6227, 10822, 12103, 21760, 25429, 32766, 32767,
)
# Each level is printed by its name: a float32 build prints 100 / 127 with
# fewer digits than CPython, and the label would differ where the samples
# don't.
LEVELS = (("0.5", 0.5), ("100/127", 100 / 127), ("0.999", 0.999), ("1.0", 1.0))
PANS = (0.0, 0.5, -0.5, 1.0, -1.0)


def sample(data, index):
    """One signed 16-bit little-endian sample. `array.frombytes` is CPython's
    alone -- MicroPython's `array` does not have it, and the probe has to read
    the same bytes on all three interpreters."""
    value = data[index * 2] | (data[index * 2 + 1] << 8)
    return value - 65536 if value & 0x8000 else value


def render(channels, level, panning):
    material = array("h", [0] * channels)
    for value in VALUES:
        for _ in range(channels):
            material.append(value)
        for _ in range(channels):
            material.append(value)
    mixer = audiomixer.Mixer(
        voice_count=1, buffer_size=BUFFER_SIZE, sample_rate=SAMPLE_RATE,
        channel_count=channels, bits_per_sample=16, samples_signed=True)
    mixer.voice[0].level = level
    if channels == 2:
        mixer.voice[0].panning = panning
    mixer.voice[0].play(
        audiocore.RawSample(material, sample_rate=SAMPLE_RATE,
                            channel_count=channels), loop=True)
    block = bytes(audiocore.get_buffer(mixer)[1])
    return [sample(block, i) for i in range(len(block) // 2)]


for name, level in LEVELS:
    for panning in PANS:
        out = render(2, level, panning)
        print("stereo", name, panning, out[2:2 + len(VALUES) * 4])
    out = render(1, level, 0.0)
    print("mono", name, out[1:1 + len(VALUES) * 2])
