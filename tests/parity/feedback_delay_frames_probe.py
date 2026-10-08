"""FeedbackDelay: `delay_frames` reads the line exactly that many frames back.

    feedback_delay_frames_probe.py audioecho

At 44.1 kHz no float32 `delay_ms` lands on frame 16457: the nearest is 1/512
of a frame short, so a click comes back split 19961 / 39 across frames 16456
and 16457 (audiodsp#179). `delay_frames=16457` lands on it whole. Printed for
each case: every non-zero output frame of a single click, so a split read
shows as two lines and an exact one as one. Then a slew to a new frame count,
the clamps at both ends, and the refusal of `delay_ms` and `delay_frames`
together.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioecho"
echo = __import__(MODULE)

BLOCK = 256


def click(rate, frames):
    values = array("h", [0] * (frames * 2))
    values[0] = values[1] = 20000
    return audiocore.RawSample(values, sample_rate=rate, channel_count=2)


def nonzero(tag, node, blocks):
    frame = 0
    for _ in range(blocks):
        data = array("h", bytes(audiocore.get_buffer(node)[1]))
        for index in range(0, len(data), 2):
            if data[index] or data[index + 1]:
                print("fbf", tag, frame, data[index], data[index + 1])
            frame += 1


def run(tag, rate, frames_back, **delay):
    node = echo.FeedbackDelay(sample_rate=rate, max_delay_ms=600.0,
                              feedback=0.0, mix=2.0, **delay)
    node.play(click(rate, frames_back + BLOCK * 2))
    nonzero(tag, node, (frames_back + BLOCK) // BLOCK + 1)
    node.deinit()


run("ms-44k", 44100, 16457, delay_ms=373.175)
run("frames-44k", 44100, 16457, delay_frames=16457)
run("frames-22k", 22050, 8231, delay_frames=8231)
run("frames-48k", 48000, 17913, delay_frames=17913)
run("frames-half", 44100, 1000, delay_frames=1000.5)

# A slew glides to a new delay_frames the way it glides to a new delay_ms.
node = echo.FeedbackDelay(sample_rate=44100, max_delay_ms=200.0,
                          delay_frames=2000, feedback=0.6, mix=2.0,
                          delay_slew=0.5)
node.play(click(44100, BLOCK * 40))
nonzero("slew-a", node, 4)
node.set(delay_frames=3000)
nonzero("slew-b", node, 30)
node.deinit()

# Clamped at both ends, as delay_ms is: one frame, and two short of the line.
for value in (0, -5, 10 ** 9):
    node = echo.FeedbackDelay(sample_rate=8000, max_delay_ms=40.0,
                              delay_frames=value, feedback=0.0, mix=2.0)
    node.play(click(8000, BLOCK * 2))
    nonzero("clamp-%d" % value, node, 2)
    node.deinit()

for options in ({"delay_ms": 10.0, "delay_frames": 80},):
    try:
        echo.FeedbackDelay(sample_rate=8000, max_delay_ms=40.0, **options)
        print("fbf both refused at construction: no")
    except TypeError:
        print("fbf both refused at construction: yes")
    node = echo.FeedbackDelay(sample_rate=8000, max_delay_ms=40.0)
    try:
        node.set(**options)
        print("fbf both refused by set: no")
    except TypeError:
        print("fbf both refused by set: yes")
print("done feedback-delay-frames")
