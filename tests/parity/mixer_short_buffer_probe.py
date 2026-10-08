"""A level move on a mixer voice whose source hands out short buffers.

A voice's level and pan move wait for a zero crossing, so they cannot click,
and are forced through if none comes. The native mixer forces a pending move
at the end of each stretch it mixes from one source buffer, and a stretch is
capped at 256 frames; the CPython twin used to force it once per block. So
with source buffers shorter than the block, the native builds moved the level
partway through a block and the twin a block late: from a fresh voice on DC,
MicroPython and CircuitPython went non-zero at frame 100 of a 100-frame
source, and CPython played nothing in block 0 (audiodsp#178). Every other
mixer probe feeds sources at least as long as the block, which is why none
saw it.

    CASE  <name>  <block>  <first non-zero sample index or -1>  <sum>  <checksum>

DC never crosses zero, so every move in the DC cases is a forced one. The
`ramp` cases cross zero, so their moves happen at a crossing where there is
one and are forced where there is not.
"""

from array import array

import audiocore
import audiomixer

RATE = 8000


def checksum(values):
    value = 2166136261
    for word in values:
        value = ((value ^ (word & 0xffff)) * 16777619) & 0xffffffff
    return value


def dc(frames, channels, level=4000):
    return array("h", [level] * (frames * channels))


def ramp(frames, channels):
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            values.append(((frame * (53 + channel * 11)) % 1201) * 9 - 5400)
    return values


def run(name, channels, frames, source, moves, blocks=6):
    sample = audiocore.RawSample(source(frames, channels), sample_rate=RATE,
                                 channel_count=channels)
    mixer = audiomixer.Mixer(voice_count=1, sample_rate=RATE,
                             channel_count=channels,
                             buffer_size=512 * channels)
    mixer.voice[0].play(sample, loop=True)
    for block in range(blocks):
        if block in moves:
            level, panning = moves[block]
            mixer.voice[0].level = level
            if channels == 2:
                mixer.voice[0].panning = panning
        words = array("h", bytes(audiocore.get_buffer(mixer)[1]))
        first = -1
        for index in range(len(words)):
            if words[index]:
                first = index
                break
        print("CASE", name, block, first, sum(words), checksum(words))


MOVES = {2: (0.5, 0.0), 3: (0.0, 0.0), 4: (0.8, 0.6)}

for channels in (1, 2):
    for frames in (100, 37, 3, 1000):
        run("dc-%dch-%d" % (channels, frames), channels, frames, dc, MOVES)
        run("ramp-%dch-%d" % (channels, frames), channels, frames, ramp, MOVES)
