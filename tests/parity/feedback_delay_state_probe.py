"""Deterministic FeedbackDelay PCM for the node's own state: the damping
low-pass near the floor, the loop filters taken out and put back, and a wow
depth moved while playing.

    feedback_delay_state_probe.py audioecho

Its own fixture, like `feedback_delay_options_probe.py`, so the two older
probes keep rendering exactly what they rendered: none of their cases reaches
the floor, switches a filter out and back, or moves the wow depth, and their
output is byte-identical across these fixes. What this one pins is that every
interpreter renders the new arithmetic identically. No oracle: `audioecho` is
audiodsp's own.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioecho"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
echo = __import__(MODULE)

SAMPLE_RATE = 8000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames=1600, level=14000):
    """The burst-and-tail the other two probes use."""
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            if frame < 120:
                shape = ((frame * (97 + channel * 18)) % 2001) - 1000
                values.append(shape * level // 1000)
            else:
                values.append(0)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("fbds", tag, index, len(data), sum(data), checksum(data))


# audiodsp#157. A 2 LSB DC for 20 blocks, then silence, at 48 kHz with the
# damping low-pass in, at two of the feedbacks where 0.5 / (1 - feedback) is
# whole: the float32 state used to stop a few ulps above the line and hand it
# back for ever. The last four blocks of 270 are printed.
for damping_hz in (800.0, 3000.0):
    for feedback in (0.5, 0.9):
        node = echo.FeedbackDelay(sample_rate=48000, max_delay_ms=20.0,
                                  delay_ms=12.5, feedback=feedback, mix=2.0,
                                  damping_hz=damping_hz)
        values = array("h", [2] * (256 * 20 * 2))
        values.extend(array("h", bytes(256 * 250 * 2 * 2)))
        node.play(audiocore.RawSample(values, sample_rate=48000,
                                      channel_count=2))
        tag = "floor-%d-%s" % (int(damping_hz), feedback)
        # The whole approach to the floor in one number, then the floor.
        whole = 2166136261
        for _block in range(266):
            for byte in bytes(audiocore.get_buffer(node)[1]):
                whole = ((whole ^ byte) * 16777619) & 0xffffffff
        print("fbds", tag, "approach", whole)
        emit(tag, node, 4)

# audiodsp#158. The damping low-pass taken out while repeats circulate and
# put back in as the next one arrives. Its state follows the tap while out;
# frozen, it put back what it held when it went.
node = echo.FeedbackDelay(sample_rate=SAMPLE_RATE, max_delay_ms=120.0,
                          delay_ms=40.0, feedback=0.7, mix=1.0,
                          damping_hz=900.0)
node.play(source())
emit("damping-in", node, 3)
node.set(damping_hz=0.0)
emit("damping-out", node, 2)
node.set(damping_hz=900.0)
emit("damping-back", node, 3)

# audiodsp#159. The same for the cut high-pass. Its state rests at zero
# while out, where its output is its input; frozen, it subtracted what it
# held from whatever came next.
node = echo.FeedbackDelay(sample_rate=SAMPLE_RATE, max_delay_ms=120.0,
                          delay_ms=40.0, feedback=0.7, mix=1.0, cut_hz=220.0)
node.play(source())
emit("cut-in", node, 3)
node.set(cut_hz=0.0)
emit("cut-out", node, 2)
node.set(cut_hz=220.0)
emit("cut-back", node, 3)
