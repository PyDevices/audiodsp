"""The delay nodes' lifecycle matrix: the same events on every delay node.

    delay_lifecycle_probe.py audioecho
    delay_lifecycle_probe.py audiodelays

Every node in NODES for the module named goes through the same four events
(audiodsp#218), and each event asserts what it should do as well as printing
the PCM, so the interpreters are held to the same bytes and to the right ones:

- **feedback**: feedback taken to 0 while a tail rings. The repeat already
  written plays out, and one delay later the line is silent. Feedback back on
  brings nothing back, and a new note then rings exactly as it does on a node
  that never had a tail.
- **slew**: `delay_slew` taken to 0 in the middle of a glide (the nodes that
  have one). The read head lands on the new delay at once: from that block on
  the node plays exactly what a node built at that delay plays.
- **mix**: mix taken to 0 and brought back. While it is 0 the input passes
  through untouched. A node whose loop runs on at a mix of 0 then plays
  exactly what a node left at that mix plays. `Echo` is upstream's and holds
  its line still instead: what it plays after is what a node fed the same
  source with the muted stretch cut out plays.
- **settings**: several settings before one pull. Settings split over several
  calls land as one call does, two settings land the same in either order, and
  a value set and replaced before the pull leaves no trace.

A new delay node gets these checks by adding an entry to NODES.

Every level here is a power-of-two fraction. The CircuitPython-derived nodes
work out `mix` and `decay` in `mp_float_t`, which is single precision on a
float build, where the CPython twin uses a double: 0.4 is two different
numbers there, and 0.375 is one.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioecho"
module = __import__(MODULE)

RATE = 8000
BLOCK = 256          # frames a pull hands back, for every node below


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


def source(frames, impulses=(), tone=None):
    """Mono, 16-bit. Impulses at the frames named, and a quiet sawtooth over
    the stretch `tone` names, so a comparison has something in every block."""
    values = array("h", [0] * frames)
    for at in impulses:
        values[at] = 20000
    if tone is not None:
        for frame in range(tone[0], tone[1]):
            values[frame] = ((frame * 37) % 400 - 200) * 20
    return values


def raw(values):
    return audiocore.RawSample(values, sample_rate=RATE)


def pull(node):
    data = bytes(audiocore.get_buffer(node)[1])
    if len(data) != BLOCK * 2:
        raise SystemExit("a pull handed back %d bytes, not %d"
                         % (len(data), BLOCK * 2))
    return data


def pulls(node, count):
    return [pull(node) for _ in range(count)]


def silent(blocks):
    return all(not any(block) for block in blocks)


# --- the nodes ---------------------------------------------------------------
#
# Each entry builds a mono node from four plain settings and says how to
# change them. `mix` is CircuitPython's 0..1 everywhere here: 0 is dry alone,
# 0.5 dry and wet at unity, 1 wet alone. `delay` is the frames an impulse
# comes back after, which every event below is laid out around.


class FeedbackDelay:
    name = "FeedbackDelay"
    delay = 160                 # 20 ms at 8 kHz
    runs_on_at_mix_0 = True
    has_slew = True

    def make(self, feedback=0.5, mix=0.5, delay_ms=20.0, **extra):
        return module.FeedbackDelay(
            sample_rate=RATE, channel_count=1, max_delay_ms=200.0,
            delay_ms=delay_ms, feedback=feedback, mix=mix * 2.0, **extra)

    def set(self, node, **settings):
        if "mix" in settings:
            settings["mix"] *= 2.0
        node.set(**settings)

    def there_and_back(self):
        """(name, a value, the value it was built with), each set and put
        back before one pull."""
        return (("feedback", 0.9, 0.5), ("mix", 0.0, 0.5),
                ("delay_ms", 60.0, 20.0), ("wow_depth_ms", 5.0, 1.0))

    def busy(self):
        """Settings that give every option above something to move."""
        return dict(wow_hz=2.0, wow_depth_ms=1.0, loop_semitones=5.0)


class MultiTapDelay:
    name = "MultiTapDelay"
    delay = 320                 # 40 ms at 8 kHz, past the 256-frame floor
    runs_on_at_mix_0 = True
    has_slew = False

    def make(self, feedback=0.5, mix=0.5, delay_ms=40.0, **extra):
        return module.MultiTapDelay(
            max_delay_ms=200, delay_ms=delay_ms, decay=feedback, mix=mix,
            buffer_size=BLOCK * 2, sample_rate=RATE, channel_count=1, **extra)

    def set(self, node, **settings):
        for name, value in settings.items():
            setattr(node, "decay" if name == "feedback" else name, value)

    def there_and_back(self):
        return (("feedback", 0.9, 0.5), ("mix", 0.0, 0.5),
                ("delay_ms", 100.0, 40.0),
                ("taps", ((0.25, 1.0),), ((0.5, 0.8), (1.0, 1.0))))

    def busy(self):
        return dict(taps=((0.5, 0.8), (1.0, 1.0)))


class Echo:
    name = "Echo"
    delay = 320
    runs_on_at_mix_0 = False    # upstream holds the line still at a mix of 0
    has_slew = False

    def make(self, feedback=0.5, mix=0.5, delay_ms=40.0, **extra):
        return module.Echo(
            max_delay_ms=200, delay_ms=delay_ms, decay=feedback, mix=mix,
            buffer_size=BLOCK * 2, sample_rate=RATE, channel_count=1,
            freq_shift=False, **extra)

    def set(self, node, **settings):
        for name, value in settings.items():
            setattr(node, "decay" if name == "feedback" else name, value)

    def there_and_back(self):
        return (("feedback", 0.9, 0.5), ("mix", 0.0, 0.5),
                ("delay_ms", 100.0, 40.0))

    def busy(self):
        return {}


NODES = {
    "audioecho": (FeedbackDelay(),),
    "audiodelays": (MultiTapDelay(), Echo()),
}


# --- the events --------------------------------------------------------------

def feedback_event(kind):
    """Feedback to 0 with a tail ringing, and back."""
    tag = kind.name + " feedback"
    second = BLOCK * 14 + 40
    frames = BLOCK * 20
    node = kind.make()
    node.play(raw(source(frames, impulses=(10, second))))
    ringing = pulls(node, 3)
    kind.set(node, feedback=0.0)
    emptying = pulls(node, 3)
    kind.set(node, feedback=0.5)
    after = pulls(node, 14)
    print(tag, [checksum(b) for b in ringing + emptying + after])
    check(tag + " rings", not silent(ringing[1:]))
    # Nothing comes in after frame 10 until `second`, so one delay (plus the
    # two frames an interpolated read reaches) after feedback went to 0 the
    # line holds nothing but zeros.
    out = array("h", b"".join(emptying + after))
    quiet_from = kind.delay + 2
    quiet_to = second - BLOCK * 3
    check(tag + " empties in one delay",
          not any(out[quiet_from:quiet_to]))
    # A node that never had the first note: from the second note on, the two
    # must be the same bytes.
    fresh = kind.make()
    fresh.play(raw(source(frames, impulses=(second,))))
    reference = pulls(fresh, 20)
    start = second // BLOCK
    check(tag + " rings again as new",
          after[start - 6:] == reference[start:])
    node.deinit()
    fresh.deinit()


def slew_event(kind):
    """delay_slew to 0 in the middle of a glide."""
    tag = kind.name + " slew"
    frames = BLOCK * 16
    values = source(frames, impulses=(BLOCK * 8,), tone=(0, frames))
    # Feedback 0, so the line holds the input alone and a node built at the
    # target delay has the same line: the two can only differ by where they
    # read it.
    node = kind.make(feedback=0.0, mix=1.0, delay_slew=0.25)
    node.play(raw(values))
    head = pulls(node, 1)
    kind.set(node, delay_ms=60.0)       # 160 -> 480 frames, 1280 frames long
    gliding = pulls(node, 2)
    kind.set(node, delay_slew=0.0)
    landed = pulls(node, 13)
    print(tag, [checksum(b) for b in head + gliding + landed])
    target = kind.make(feedback=0.0, mix=1.0, delay_ms=60.0)
    target.play(raw(values))
    reference = pulls(target, 16)
    check(tag + " glides", gliding != reference[1:3])
    check(tag + " lands at once", landed == reference[3:])
    node.deinit()
    target.deinit()


def mix_event(kind):
    """Mix to 0 and back."""
    tag = kind.name + " mix"
    frames = BLOCK * 14
    values = source(frames, impulses=(10, 300, BLOCK * 6 + 5),
                    tone=(BLOCK * 2, BLOCK * 9))
    node = kind.make(feedback=0.75, mix=0.375)
    node.play(raw(values))
    before = pulls(node, 1)
    kind.set(node, mix=0.0)
    muted = pulls(node, 3)
    kind.set(node, mix=0.375)
    back = pulls(node, 8)
    print(tag, [checksum(b) for b in before + muted + back])
    check(tag + " passes the input at 0",
          b"".join(muted) == bytes(values[BLOCK:BLOCK * 4]))
    if kind.runs_on_at_mix_0:
        reference_values = values
        offset = 4
    else:
        # The line stood still for the three muted blocks, so the node is
        # three blocks behind its source: a node fed the source with those
        # blocks cut out plays the same from there.
        reference_values = values[:BLOCK] + values[BLOCK * 4:]
        offset = 1
    reference = kind.make(feedback=0.75, mix=0.375)
    reference.play(raw(reference_values))
    expected = pulls(reference, offset + 8)
    check(tag + " comes back where the loop is",
          back == expected[offset:])
    node.deinit()
    reference.deinit()


def settings_event(kind):
    """Several settings before one pull."""
    tag = kind.name + " settings"
    frames = BLOCK * 14
    values = source(frames, impulses=(10, 600, 1500), tone=(BLOCK * 3, BLOCK * 7))

    def render(calls):
        node = kind.make(**kind.busy())
        node.play(raw(values))
        out = pulls(node, 2)
        for settings in calls:
            kind.set(node, **settings)
        out += pulls(node, 12)
        node.deinit()
        return out

    untouched = render([])
    print(tag, "untouched", checksum(b"".join(untouched)))
    for name, value, built in kind.there_and_back():
        out = render([{name: value}, {name: built}])
        print(tag, name, checksum(b"".join(out)))
        check("%s %s there and back" % (tag, name), out == untouched)
    one = render([dict(feedback=0.25, mix=0.75)])
    split = render([dict(feedback=0.25), dict(mix=0.75)])
    swapped = render([dict(mix=0.75), dict(feedback=0.25)])
    print(tag, "one", checksum(b"".join(one)))
    check(tag + " split as one", split == one)
    check(tag + " either order", swapped == one)
    if kind.name == "FeedbackDelay":
        # A rate set and replaced before a pull never played, so it is not the
        # one the oscillator and the shifter coast to a stop at.
        stop = render([dict(wow_hz=0.0)])
        replaced = render([dict(wow_hz=7.0), dict(wow_hz=0.0)])
        print(tag, "wow stop", checksum(b"".join(stop)))
        check(tag + " wow rate replaced", replaced == stop)
        stop = render([dict(loop_semitones=0.0, loop_window_ms=60.0)])
        before = render([dict(loop_window_ms=60.0), dict(loop_semitones=0.0)])
        print(tag, "shift stop", checksum(b"".join(stop)))
        check(tag + " shift window order", before == stop)


for kind in NODES[MODULE]:
    feedback_event(kind)
    if kind.has_slew:
        slew_event(kind)
    mix_event(kind)
    settings_event(kind)
print("done delay-lifecycle")

if FAILED:
    raise SystemExit("failed: " + " ".join(FAILED))
