"""Deterministic FeedbackDelay PCM for two controls taken to their stop.

    feedback_delay_park_probe.py audioecho

From audiodsp#177, each asserted here as well as printed:

- `wow_hz` taken to 0 parks the read head at centre at the oscillator's next
  zero crossing: an impulse played after that repeats exactly `delay_ms`
  later. It used to freeze mid-swing and leave the echo off pitch.
- `loop_semitones` taken to 0 keeps reading where the shifter was: the
  crossfade turns on to a phase where one tap carries all the gain and stays
  there, so every repeat after that lands half a window late, and turning it
  on again carries on from that phase. It used to step back to the unshifted
  read, and resume from a stale phase.
"""

import sys
from array import array

import audiocore

echo = __import__(sys.argv[1] if len(sys.argv) > 1 else "audioecho")

RATE = 8000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def samples(data):
    out = [data[i] | (data[i + 1] << 8) for i in range(0, len(data), 2)]
    return [s - 65536 if s >= 32768 else s for s in out]


def pull(node):
    return bytes(audiocore.get_buffer(node)[1])


FAILED = []


def check(tag, ok):
    print("check", tag, "ok" if ok else "FAILED")
    if not ok:
        FAILED.append(tag)


def impulse_source(frames, at):
    values = array("h")
    for frame in range(frames):
        values.append(20000 if frame in at else 0)
    return audiocore.RawSample(values, sample_rate=RATE)


def first_peak(values, start):
    best, where = 0, -1
    for index in range(start, len(values)):
        if abs(values[index]) > best:
            best, where = abs(values[index]), index
    return where


def run(node, blocks):
    values = []
    data_all = b""
    for _ in range(blocks):
        data = pull(node)
        data_all += data
        values += samples(data)
    return values, data_all


# wow_hz to 0. A 20 ms echo is 160 frames; the issue measured 148 with the
# oscillator frozen mid-swing.
node = echo.FeedbackDelay(sample_rate=RATE, channel_count=1, max_delay_ms=60.0,
                          delay_ms=20.0, feedback=0.0, mix=2.0, wow_hz=3.0,
                          wow_depth_ms=2.0)
node.play(impulse_source(256 * 40, at=(256 * 30,)))
_, head = run(node, 3)
node.set(wow_hz=0.0)
values, tail = run(node, 37)
print("wow_stop", len(head), checksum(head), len(tail), checksum(tail))
impulse = 256 * 30 - 256 * 3
echo_at = first_peak(values, impulse + 1)
print("wow_stop echo", echo_at - impulse)
check("wow_stop", echo_at - impulse == 160)
node.set(wow_hz=3.0)
values, again = run(node, 4)
print("wow_again", len(again), checksum(again))

# loop_semitones on, off and on. The window is 25 ms, 200 frames, so the
# repeat of a shifted or parked loop lands 100 frames after the delay.
node = echo.FeedbackDelay(sample_rate=RATE, channel_count=1, max_delay_ms=200.0,
                          delay_ms=40.0, feedback=0.0, mix=2.0,
                          loop_semitones=7.0)
node.play(impulse_source(256 * 60, at=(256 * 20, 256 * 40)))
_, shifted = run(node, 7)
node.set(loop_semitones=0.0)
values, parked = run(node, 45)
print("shift_stop", len(shifted), checksum(shifted), len(parked),
      checksum(parked))
lags = []
for at in (256 * 20, 256 * 40):
    start = at - 256 * 7
    lags.append(first_peak(values[:start + 600], start + 1) - start)
print("shift_stop lags", lags[0], lags[1])
check("shift_stop", lags[0] == lags[1] == 320 + 100)
node.set(loop_semitones=7.0)
_, again = run(node, 4)
print("shift_again", len(again), checksum(again))

if FAILED:
    raise SystemExit("failed: " + " ".join(FAILED))
