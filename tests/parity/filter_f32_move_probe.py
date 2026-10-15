"""audiobiquad's coefficient moves, rendered for every interpreter to agree on.

    filter_f32_move_probe.py audiobiquad

A Biquad whose coefficients change while it is playing walks to the new ones
over about 20 ms through a state-variable filter, then hands its memory back
to the direct-form recursion (audiocomponents#162). None of that path runs in
`filter_f32_probe.py`: its one mid-stream move lands on a tail that has
already decayed below an LSB, so it renders the same bytes either way. This
probe moves the coefficients under a live tone, in each of the ways the walk
branches -- a plain jump, a move during a move, a mode change, a gain move on
a shelf, a 0 dB peak whose memory the output cannot see, a jump to 0 Hz and
back, and `clear()` part-way through a walk.

Its own file rather than cases appended there, so that probe keeps rendering
what it rendered. Integer material only, for the reason that one gives.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiobiquad"
biquad = __import__(MODULE)

#: 48 kHz, where a walk is 1024 frames: four of the node's blocks, so a set
#: one block later lands inside it.
SAMPLE_RATE = 48000

#: filter_f32_probe.py's table: one period of a sine at 28000 peak.
SINE64 = (
    0, 2744, 5463, 8128, 10715, 13199, 15556, 17763,
    19799, 21644, 23281, 24694, 25869, 26794, 27462, 27865,
    28000, 27865, 27462, 26794, 25869, 24694, 23281, 21644,
    19799, 17763, 15556, 13199, 10715, 8128, 5463, 2744,
    0, -2744, -5463, -8128, -10715, -13199, -15556, -17763,
    -19799, -21644, -23281, -24694, -25869, -26794, -27462, -27865,
    -28000, -27865, -27462, -26794, -25869, -24694, -23281, -21644,
    -19799, -17763, -15556, -13199, -10715, -8128, -5463, -2744,
)


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def material(frames=16384, channels=2):
    """Two partials on each channel, different on the two, so a move has
    something below and above the corner to work on."""
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            low = SINE64[(frame * (2 + channel)) % 64]
            high = SINE64[(frame * (13 + channel)) % 64]
            values.append((low + high) // 2)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("move", tag, index, len(data), sum(data), checksum(data))


def node_for(channels, **options):
    node = biquad.Biquad(sample_rate=SAMPLE_RATE, channel_count=channels,
                         **options)
    node.play(material(channels=channels))
    return node


for channels in (1, 2):
    suffix = "-" + str(channels)

    # The jump audiocomponents#162 is about, down and back up.
    node = node_for(channels, mode=biquad.LOW_PASS, frequency=20000.0)
    emit("jump-a" + suffix, node, 2)
    node.frequency = 20.0
    emit("jump-b" + suffix, node, 6)
    node.frequency = 20000.0
    emit("jump-c" + suffix, node, 6)

    # A move during a move: the second starts from where the first had got.
    node = node_for(channels, mode=biquad.BAND_PASS, frequency=2000.0, Q=4.0)
    emit("again-a" + suffix, node, 1)
    node.frequency = 100.0
    node.Q = 0.7
    emit("again-b" + suffix, node, 1)
    node.frequency = 1000.0
    emit("again-c" + suffix, node, 6)

    # A mode change walks the output mix as well as the corner.
    node = node_for(channels, mode=biquad.LOW_PASS, frequency=600.0, Q=2.0)
    emit("mode-a" + suffix, node, 1)
    node.mode = biquad.NOTCH
    emit("mode-b" + suffix, node, 5)

    # Gain alone on a shelf.
    node = node_for(channels, mode=biquad.HIGH_SHELF, frequency=1500.0,
                    gain_db=-12.0)
    emit("shelf-a" + suffix, node, 1)
    node.gain_db = 12.0
    emit("shelf-b" + suffix, node, 5)

    # From a 0 dB peak -- a wire, whose memory the output cannot see.
    node = node_for(channels, mode=biquad.PEAKING_EQ, frequency=700.0,
                    Q=1.5, gain_db=0.0)
    emit("flat-a" + suffix, node, 1)
    node.gain_db = 9.0
    emit("flat-b" + suffix, node, 5)

    # To 0 Hz, where the section has no corner to walk to, and back.
    node = node_for(channels, mode=biquad.HIGH_PASS, frequency=900.0, Q=3.0)
    emit("zero-a" + suffix, node, 1)
    node.frequency = 0.0
    emit("zero-b" + suffix, node, 1)
    node.frequency = 900.0
    emit("zero-c" + suffix, node, 5)

    # clear() part-way through a walk: the next block starts fresh.
    node = node_for(channels, mode=biquad.LOW_PASS, frequency=3000.0, Q=8.0)
    emit("clear-a" + suffix, node, 1)
    node.frequency = 200.0
    emit("clear-b" + suffix, node, 1)
    node.clear()
    emit("clear-c" + suffix, node, 5)
