"""Deterministic Convolver PCM across a re-synthesis on a playing node.

    convolve_state_probe.py audioconvolve

`convolve_probe.py` synthesizes every room on a fresh node, before anything
plays. This one moves the room while audio is in flight, which is where
`synthesize()` keeps the history and crossfades the block being played out
(audiodsp#163): the two extra convolutions of that block, the fade's
division and the mix the block was computed at. Its own file, so the probe
above keeps rendering what it rendered.

No oracle: what `verify_dsp.py` checks is that every interpreter renders
it identically.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioconvolve"
convolve = __import__(MODULE)

SAMPLE_RATE = 8000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames, channels, level=9000, seed=2468):
    values = array("h")
    state = seed
    span = 2 * level + 1
    for _index in range(frames * channels):
        state = (state * 1103515245 + 12345) & 0x7fffffff
        values.append(((state >> 8) % span) - level)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


ROOM = {"decay": 0.1, "damping_hz": 2000.0, "predelay_ms": 0.0,
        "diffusion_ms": 10.0, "seed": 1}


def moved(**change):
    room = dict(ROOM)
    room.update(change)
    return room


def run(tag, channels, mix, calls, blocks=10):
    node = convolve.Convolver(max_taps=1024, ir_channels=channels,
                              sample_rate=SAMPLE_RATE,
                              channel_count=channels, mix=mix)
    node.synthesize(**ROOM)
    node.play(source(blocks * 256, channels))
    for index in range(blocks):
        if index in calls:
            calls[index](node)
        data = bytes(audiocore.get_buffer(node)[1])
        print("state", tag, index, len(data), sum(data), checksum(data))


for channels in (2, 1):
    for mix in (0.0, 0.5, 1.0):
        for name, room in (("same", moved()), ("decay", moved(decay=0.06)),
                           ("damping", moved(damping_hz=500.0)),
                           ("predelay", moved(predelay_ms=15.0)),
                           ("diffusion", moved(diffusion_ms=0.0)),
                           ("seed", moved(seed=36))):
            run("ch%d-mix%.1f-%s" % (channels, mix, name), channels, mix,
                {4: lambda node, room=room: node.synthesize(**room)})

# A Mix move and then a room move before the next pull: the block in flight
# keeps the mix it was computed at.
for channels in (2, 1):
    def mix_then_room(node):
        node.set(mix=1.0)
        node.synthesize(**moved(seed=36))
    run("ch%d-mix-then-room" % channels, channels, 0.0, {4: mix_then_room})

# Two moves in a row, and a clear after a move.
run("twice", 2, 0.5, {4: lambda node: node.synthesize(**moved(seed=9)),
                      5: lambda node: node.synthesize(**moved(decay=0.05))})
run("clear-after", 2, 0.5, {4: lambda node: node.synthesize(**moved(seed=9)),
                            6: lambda node: node.clear()})
