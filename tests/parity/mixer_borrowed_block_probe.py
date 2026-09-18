"""A mixer voice mixes the source's buffer as it stands at MIX time.

`common_hal_audiomixer_mixervoice_play` fetches one block from the source and
keeps what `audiosample_get_buffer` handed back -- and what it handed back is a
POINTER into the source node's own buffer (`int16_t buffer[...]` in
`src/audiobiquad/Biquad.h`, and in every other node audioif wrote). The
mix-down reads that pointer later (`uint32_t *src = voice->remaining_buffer` in
`src/audiomixer/Mixer.c`). So anything that pulls the source between the fetch
and the mix overwrites the buffer, and the voice mixes the LATER block. A node
is not double-buffered: it has one.

That is not a detail. `audioeffects.rebuilt.Saturation` settles its plate
coupling pole by pulling the wet chain 32 times with the shapers held on
silence, after the mixer voice behind it is already attached -- and it is this
overwrite that puts the settled block into the class's first render instead of
the bang the pole makes from cold. The CPython twin copied the block at
`play()` instead of borrowing it, so the bang survived there and only CPython
rendered it: shipped patches 4 and 6 came out `3b0e6b65f9c782d0` against
`1f9bcf50dc14a4ee` on desktop MicroPython, desktop CircuitPython and both
boards. audioif#89.

A probe of its own rather than a case appended to
`mixer_level_precision_probe.py`: one comparison covers a probe's whole output,
so a case added there would move the very numbers that say the 56 flipping
values still scale the way they did.

Agreement is the whole gate here for the usual reason and one more: the mixer's
voice bookkeeping is C on two targets and Python on the third, so there is no
shared kernel that could move all three together and hide a change.

    CASE  <name>  <what the direct pulls saw>  <first eight samples>  <sum>

`wire` is a `PEAKING_EQ` at `gain_db=0`, whose normalised coefficients are
`b = (1, a1, a2)` -- an exact wire -- so each block arrives at the voice
carrying the constant that says which block it is.
"""

from array import array

import audiobiquad
import audiocore
import audiomixer

SAMPLE_RATE = 48000
CHANNELS = 2
BLOCK = 256          # what every audioif node renders in
BLOCKS = 4
BUFFER_SIZE = 2048   # a Mixer hands back half of what it is given: 256 frames


def material():
    """Four blocks of square wave, +-1000, +-2000, +-3000, +-4000.

    Square rather than constant because a fresh voice starts at level 0 and
    takes its level at the first zero crossing (CircuitPython 10.3.0); a
    constant never crosses and the whole block would render silent, saying
    nothing about which block it was.
    """
    pcm = array("h", bytes(2 * BLOCKS * BLOCK * CHANNELS))
    for index in range(BLOCKS):
        for frame in range(BLOCK):
            value = 1000 * (index + 1)
            if frame % 2:
                value = -value
            for channel in range(CHANNELS):
                pcm[(index * BLOCK + frame) * CHANNELS + channel] = value
    return pcm


def sample(data, index):
    """One signed 16-bit little-endian sample. `array.frombytes` is CPython's
    alone -- MicroPython's `array` does not have it, and the probe has to read
    the same bytes on all three interpreters."""
    value = data[index * 2] | (data[index * 2 + 1] << 8)
    return value - 65536 if value & 0x8000 else value


def case(name, intervening):
    keep = []
    source = audiocore.RawSample(material(), sample_rate=SAMPLE_RATE,
                                 channel_count=CHANNELS)
    wire = audiobiquad.Biquad(mode=audiobiquad.PEAKING_EQ, frequency=1000.0,
                              Q=0.7071067811865475, gain_db=0.0,
                              sample_rate=SAMPLE_RATE,
                              channel_count=CHANNELS)
    wire.play(source)
    mixer = audiomixer.Mixer(
        voice_count=1, buffer_size=BUFFER_SIZE, sample_rate=SAMPLE_RATE,
        channel_count=CHANNELS, bits_per_sample=16, samples_signed=True)
    mixer.voice[0].level = 1.0
    mixer.voice[0].panning = 0.0
    mixer.voice[0].play(wire)
    keep.append((source, wire, mixer))

    seen = []
    for _pull in range(intervening):
        block = bytes(audiocore.get_buffer(wire)[1])
        seen.append(sample(block, 0))

    block = bytes(audiocore.get_buffer(mixer)[1])
    frames = len(block) // 2
    print("CASE", name, seen,
          [sample(block, index) for index in range(8)],
          sum(sample(block, index) for index in range(frames)))


# Nothing pulls the source, so the voice mixes the block it fetched: +-1000.
case("kept", 0)
# One pull overtakes it: the buffer now holds +-2000, and that is what mixes.
case("overtaken-once", 1)
# And it is the LAST render that mixes, not the next one along: +-3000.
case("overtaken-twice", 2)
