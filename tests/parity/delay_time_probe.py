"""A delay's Time shortened and lengthened again: the line comes back empty.

    delay_time_probe.py audiodelays
    delay_time_probe.py audioecho

The line a shorter Time leaves unused has to be silent when the Time comes
back, or the longer line replays audio from before it. Each node is taken two
ways into that, and each asserts that nothing plays out of the silence after:

- **in silence**: the input stops, then the Time is shortened, the output
  falls silent, and the Time comes back. The CPython `Echo` used to keep the
  old repeats past the shorter line and play them here.
- **while playing**: the Time is shortened while the input still plays, with
  the write head past the end of the shorter line. Upstream's `Echo` and
  `MultiTapDelay` wrote one more sample there before starting the line again,
  where nothing read it until the Time grew back.

It sits apart from `delay_lifecycle_probe.py` because CircuitPython's own
`Echo` and `MultiTapDelay` still do the second, so this probe is skipped
there, and that probe's other events keep their three-way comparison.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiodelays"
module = __import__(MODULE)

RATE = 8000
BLOCK = 256


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


FAILED = []


def check(tag, ok):
    print("check", tag, "ok" if ok else "FAILED")
    if not ok:
        FAILED.append(tag)


def tone(frames):
    """Mono, 16-bit, a quiet sawtooth in every frame."""
    values = array("h", [0] * frames)
    for frame in range(frames):
        values[frame] = ((frame * 37) % 400 - 200) * 20
    return values


def pull(node):
    data = bytes(audiocore.get_buffer(node)[1])
    if len(data) != BLOCK * 2:
        raise SystemExit("a pull handed back %d bytes, not %d"
                         % (len(data), BLOCK * 2))
    return data


def quiet_after(node, limit=200):
    """Pull until eight blocks in a row are silent; the pulls it took."""
    run = 0
    for count in range(1, limit + 1):
        run = run + 1 if not any(pull(node)) else 0
        if run == 8:
            return count
    return None


def make(name, delay_ms):
    """A mono node, wet alone, decay a half."""
    if name == "FeedbackDelay":
        return module.FeedbackDelay(
            sample_rate=RATE, channel_count=1, max_delay_ms=200.0,
            delay_ms=delay_ms, feedback=0.5, mix=2.0)
    if name == "MultiTapDelay":
        return module.MultiTapDelay(
            max_delay_ms=200, delay_ms=delay_ms, decay=0.5, mix=1.0,
            buffer_size=BLOCK * 2, sample_rate=RATE, channel_count=1)
    return module.Echo(
        max_delay_ms=200, delay_ms=delay_ms, decay=0.5, mix=1.0,
        buffer_size=BLOCK * 2, sample_rate=RATE, channel_count=1,
        freq_shift=False)


def set_time(node, name, delay_ms):
    if name == "FeedbackDelay":
        node.set(delay_ms=delay_ms)
    else:
        node.delay_ms = delay_ms


NODES = {
    "audiodelays": ("MultiTapDelay", "Echo"),
    "audioecho": ("FeedbackDelay",),
}

# 100 ms is 800 frames, and 5 ms is raised to the 256-frame block floor on the
# nodes that have one: two pulls in, the write head is at 512, past it.
for name in NODES[MODULE]:
    for case, shorten_at in (("in silence", 8), ("while playing", 2)):
        tag = "%s %s" % (name, case)
        node = make(name, 100.0)
        node.play(audiocore.RawSample(tone(BLOCK * 6), sample_rate=RATE))
        for _ in range(shorten_at):
            pull(node)
        set_time(node, name, 5.0)
        waited = quiet_after(node)
        check(tag + " falls silent", waited is not None)
        set_time(node, name, 100.0)
        back = [pull(node) for _ in range(8)]
        print(tag, waited, [checksum(block) for block in back])
        check(tag + " comes back silent",
              all(not any(block) for block in back))
        node.deinit()
print("done delay-time")

if FAILED:
    raise SystemExit("failed: " + " ".join(FAILED))
