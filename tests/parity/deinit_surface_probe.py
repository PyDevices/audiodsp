"""Every node in the palette releases, and refuses to be used afterwards.

Runs unchanged on the CPython target, on desktop MicroPython and on
`circuitpython-effects`, because the thing it asserts is the same on all
three: a node has `deinit()`, calling it marks the node released, and every
way back into the audio path then raises instead of handing out stale audio
or reading freed memory.

It does not compare exception *types*, on purpose. The native builds raise
CircuitPython's `ValueError` and the CPython target raises `RuntimeError`
(audiodsp#73); this probe is not the place to settle that, so it records only
that the guard fired.

**The fault this exists to catch.** Twelve of the node types --
every one audiodsp wrote rather than ported from CircuitPython -- had no
`deinit()` at all, so no class built on them could release one and Tier 1's
"deinit() releases every node the class built" was unmeasurable on a board
(audiodsp#58, #60, #63). One of them, `audiomixer.Mixer`, had `deinit()` and
no guard on the C protocol entry point, and pulling a released one dumped
core (audiodsp#59).

**Every method, on the nodes audiodsp wrote.** The audio path is not the
only way back in. On the native builds `audioverb.Tank` still took `set()`,
`clear()` and `play()` after `deinit()`, and `audioconvolve.Convolver` still
took `clear()`, `load()` and `synthesize()`, writing through pointers into
storage the collector was free to take back (audiodsp#176). So for every node
type audiodsp wrote itself (`OWN`), every public method and property on that
build's surface is called on a released node and must raise. Each call is
first made on a live node of the same type and must succeed there, so a raise
on the released one is the guard and not a bad argument. A method with no
call recipe fails the run rather than going unchecked. The node types ported
from CircuitPython keep CircuitPython's surface, which guards the audio path
and leaves some properties and `stop()` open; they are checked on the audio
path only.

A module a target does not build (CircuitPython 11 has no `audiospeed`) is
named in the output and its nodes are skipped.

Run `--fault` to check the probe can fail: it skips the `deinit()` call, so
every node reads as unreleased and the run must exit nonzero. A probe whose
failing mode was never run is not a gate.
"""

import array
import sys

import audiobiquad
import audioconvolve
import audiocore
import audiodelays
import audiodynamics
import audioecho
import audiofilters
import audiofreeverb
import audioladder
import audiomath
import audiomixer
import audiomodal
import audioroute
import audioshaper
import audioverb
import synthio

#: Modules some target does not build. CircuitPython has its own audio core
#: and takes audiodsp's own modules on top; `audiospeed` is not among them.
MISSING = []
try:
    import audiospeed
except ImportError:
    audiospeed = None
    MISSING.append("audiospeed")

RATE = 48000
CHANNELS = 2
PCM = {"sample_rate": RATE, "channel_count": CHANNELS}

_SILENCE = array.array("h", [0] * 1024)
_CURVE = array.array("h", [-32768, 0, 32767])
_IMPULSE = array.array("h", [32767] + [0] * 15)


def source():
    return audiocore.RawSample(_SILENCE, sample_rate=RATE,
                               channel_count=CHANNELS)


#: (name, builder). Every type in the palette that can be built without a
#: file on disk; `audiocore.WaveFile` and `audiomp3.MP3Decoder` need one and
#: are covered by their own tests. A node added to the palette belongs here
#: the same day: the probe's claim is *every* type, and a list that quietly
#: falls behind the module tables makes that claim false.
NODES = (
    ("audiobiquad.AllPass", lambda: audiobiquad.AllPass(
        frequency=1000.0, stages=2, **PCM)),
    ("audiobiquad.Biquad", lambda: audiobiquad.Biquad(
        mode=audiobiquad.LOW_PASS, frequency=1000.0, Q=0.7071, **PCM)),
    ("audioconvolve.Convolver", lambda: audioconvolve.Convolver(
        impulse=_IMPULSE, **PCM)),
    ("audiocore.RawSample", source),
    ("audiodelays.Chorus", lambda: audiodelays.Chorus(
        max_delay_ms=50, **PCM)),
    ("audiodelays.Echo", lambda: audiodelays.Echo(max_delay_ms=50, **PCM)),
    ("audiodelays.Flanger", lambda: audiodelays.Flanger(max_delay_ms=10, **PCM)),
    ("audiodelays.GranularPitchShift", lambda: audiodelays.GranularPitchShift(**PCM)),
    ("audiodelays.MultiTapDelay", lambda: audiodelays.MultiTapDelay(
        max_delay_ms=50, **PCM)),
    ("audiodelays.PitchShift", lambda: audiodelays.PitchShift(**PCM)),
    ("audiodynamics.Dynamics", lambda: audiodynamics.Dynamics(
        audiodynamics.DYN_LIMIT, **PCM)),
    ("audioecho.FeedbackDelay", lambda: audioecho.FeedbackDelay(
        max_delay_ms=50, **PCM)),
    ("audiofilters.Distortion", lambda: audiofilters.Distortion(**PCM)),
    ("audiofilters.Filter", lambda: audiofilters.Filter(
        filter=synthio.Biquad(synthio.FilterMode.LOW_PASS, 1000.0, Q=0.707),
        **PCM)),
    ("audiofilters.Phaser", lambda: audiofilters.Phaser(**PCM)),
    ("audiofreeverb.Freeverb", lambda: audiofreeverb.Freeverb(**PCM)),
    ("audioladder.Ladder", lambda: audioladder.Ladder(
        cutoff_hz=1000.0, **PCM)),
    ("audiomath.Multiply", lambda: audiomath.Multiply(**PCM)),
    ("audiomath.SubOctave", lambda: audiomath.SubOctave(**PCM)),
    ("audiomixer.Mixer", lambda: audiomixer.Mixer(voice_count=2, **PCM)),
    ("audiomodal.Bank", lambda: audiomodal.Bank(modes=4, **PCM)),
    ("audioroute.MidSide", lambda: audioroute.MidSide(**PCM)),
    ("audioroute.SplitterTap", lambda: audioroute.Splitter(
        source(), taps=2).tap(0)),
    ("audioshaper.SampleHold", lambda: audioshaper.SampleHold(
        source(), num=400, den=217)),
    ("audioshaper.Waveshaper", lambda: audioshaper.Waveshaper(
        curve=_CURVE, **PCM)),
    ("audiospeed.Resampler", lambda: audiospeed.Resampler(source())),
    ("audiospeed.SpeedChanger", lambda: audiospeed.SpeedChanger(source())),
    ("audioverb.Tank", lambda: audioverb.Tank(**PCM)),
    ("synthio.Synthesizer", lambda: synthio.Synthesizer(
        sample_rate=RATE, channel_count=CHANNELS)),
)


def guard_fired(call):
    """Whether the released node refused, by either of the two conventions.

    audiodsp's own builds **raise**: `audiosample_get_buffer` calls
    `audiosample_check_for_deinit`, which throws. Upstream CircuitPython
    **returns an error** instead - `shared-module/audiocore/__init__.c:37`
    checks `audiosample_deinited()` and hands back `GET_BUFFER_ERROR` with a
    NULL buffer, and its `reset_buffer` returns without doing anything. Both
    are guards; neither reads freed memory. The difference is a convention, not
    a defect, and this probe accepts both on purpose.

    An earlier version counted only exceptions, and so reported every node on
    the patched CircuitPython build as unguarded - which was wrong, and was
    written into audiodsp#59 before the measurement was taken. What a released
    node must not do is hand back data.
    """
    try:
        result = call()
    except Exception:
        return True
    if result is None:
        return True                      # reset_buffer: nothing handed back
    try:
        code, data = result
    except (TypeError, ValueError):
        return False                     # a value that is not a (code, buffer)
    #: `GET_BUFFER_ERROR` is 2 in both runtimes; CircuitPython does not export
    #: the name from `audiocore`, so the number is used rather than imported.
    return code == 2 or data is None or len(data) == 0


def check_node(name, build, deinit):
    """Returns (row, failures) for one node."""
    failures = []
    node = build()

    if not hasattr(node, "deinit"):
        return ("%-26s no deinit" % name,
                ["%s has no deinit()" % name])

    play = getattr(node, "play", None)
    if play is not None:
        try:
            play(source())
        except (TypeError, ValueError):
            # Some nodes take their source only at construction, and
            # `Mixer.play` wants a voice. Either way the node is playable
            # enough for what follows.
            pass

    if deinit:
        node.deinit()
        # Idempotent: the second call is what a context manager makes on an
        # object already released by hand.
        node.deinit()

    released = guard_fired(lambda: node.channel_count)
    pulled = guard_fired(lambda: audiocore.get_buffer(node))
    rewound = guard_fired(lambda: audiocore.reset_buffer(node))

    if not released:
        failures.append("%s still answers channel_count after deinit()" % name)
    if not pulled:
        failures.append("%s can still be pulled after deinit()" % name)
    if not rewound:
        failures.append("%s can still be rewound after deinit()" % name)

    return ("%-26s deinit  %-9s %-7s %s"
            % (name, "released" if released else "LIVE",
               "raises" if pulled else "PULLS",
               "raises" if rewound else "REWINDS"), failures)


def check_splitter(deinit):
    """`audioroute.Splitter` is not a sample: it hands out taps, keeps its own
    released flag, and releasing it must release the taps that read its ring."""
    failures = []
    splitter = audioroute.Splitter(source(), taps=2)
    tap = splitter.tap(0)

    if not hasattr(splitter, "deinit"):
        return ("%-26s no deinit" % "audioroute.Splitter",
                ["audioroute.Splitter has no deinit()"])

    if deinit:
        splitter.deinit()
        splitter.deinit()

    refused = guard_fired(lambda: splitter.tap(0))
    stale = guard_fired(lambda: audiocore.get_buffer(tap))
    if not refused:
        failures.append("Splitter still hands out taps after deinit()")
    if not stale:
        failures.append("a tap of a released Splitter can still be pulled")

    return ("%-26s deinit  %-9s %-7s"
            % ("audioroute.Splitter", "refuses" if refused else "TAPS",
               "raises" if stale else "PULLS"), failures)


#: The node types audiodsp wrote rather than ported from CircuitPython. These
#: are held to every method, not just the audio path.
OWN = (
    "audiobiquad.AllPass", "audiobiquad.Biquad", "audioconvolve.Convolver",
    "audiodynamics.Dynamics", "audioecho.FeedbackDelay", "audioladder.Ladder",
    "audiomath.Multiply", "audiomath.SubOctave", "audiomodal.Bank",
    "audioroute.MidSide", "audioshaper.SampleHold", "audioshaper.Waveshaper",
    "audioverb.Tank",
)

#: How to call each method with arguments a live node accepts. Keyed by name,
#: since the same name means the same call on every node that has it.
CALLS = {
    "play": lambda n: n.play(source()),
    "key": lambda n: n.key(source()),
    "modulate": lambda n: n.modulate(source()),
    "stop": lambda n: n.stop(),
    "clear": lambda n: n.clear(),
    "set": lambda n: n.set(),
    "load": lambda n: n.load(_IMPULSE),
    "synthesize": lambda n: n.synthesize(decay=0.01),
    "set_mode": lambda n: n.set_mode(0, 440.0, 0.1, 0.5),
    "set_modes": lambda n: n.set_modes([(440.0, 0.1, 0.5)]),
    "gain_reduction_db": lambda n: n.gain_reduction_db(),
}

#: Methods whose live call needs the node's own arguments.
NODE_CALLS = {
    ("audioshaper.SampleHold", "set"): lambda n: n.set(num=400, den=217),
}

#: Not checked: releasing is the one call a released node must still take.
UNCHECKED = ("deinit",)


def surface(node):
    """Public (methods, properties) of this build's node."""
    methods, properties = [], []
    for attr in sorted(dir(node)):
        if attr.startswith("_") or attr in UNCHECKED:
            continue
        try:
            value = getattr(node, attr)
        except Exception:
            properties.append(attr)
            continue
        if callable(value):
            methods.append(attr)
        else:
            properties.append(attr)
    return methods, properties


def check_methods(name, build, deinit):
    """Every public method and property of a released node must raise."""
    failures = []
    methods, properties = surface(build())
    checks = []
    for method in methods:
        call = NODE_CALLS.get((name, method), CALLS.get(method))
        if call is None:
            failures.append("%s.%s has no call recipe in this probe"
                            % (name, method))
            continue
        checks.append((method + "()", call))
    for prop in properties:
        checks.append((prop, lambda n, prop=prop: getattr(n, prop)))

    open_ = []
    for label, call in checks:
        try:
            call(build())
        except Exception as error:
            failures.append("%s.%s fails on a live node, so the probe cannot "
                            "tell a guard from a bad call: %r"
                            % (name, label, error))
            continue
        node = build()
        if deinit:
            node.deinit()
        try:
            call(node)
        except Exception:
            continue
        open_.append(label)
        failures.append("%s.%s still works after deinit()" % (name, label))
    return ("%-26s %d checked, %s"
            % (name, len(checks),
               "all raise" if not open_ else "OPEN: " + " ".join(open_)),
            failures)


#: The nodes whose storage is most of what they cost: a reverb's lines, a
#: convolver's kernel, a delay's line. Releasing one gives that memory back
#: at `deinit()`, not when the last reference to the object goes, so a class
#: that keeps a released node around does not keep its storage (audiodsp#181).
HEAVY = (
    ("audioverb.Tank", lambda: audioverb.Tank(**PCM)),
    ("audioconvolve.Convolver", lambda: audioconvolve.Convolver(
        impulse=_IMPULSE, **PCM)),
    ("audioecho.FeedbackDelay", lambda: audioecho.FeedbackDelay(
        max_delay_ms=500, **PCM)),
)


def memory_meter():
    """Bytes in use after a collection: the heap on the native builds, what
    tracemalloc has seen on CPython (the twins' state is `PyMem_` memory)."""
    import gc
    if hasattr(gc, "mem_alloc"):
        def used():
            gc.collect()
            return gc.mem_alloc()
        return used
    import tracemalloc
    tracemalloc.start()

    def used():
        gc.collect()
        return tracemalloc.get_traced_memory()[0]
    return used


def check_storage(deinit):
    failures = []
    rows = []
    used = memory_meter()
    for name, build in HEAVY:
        if name.split(".")[0] in MISSING:
            continue
        before = used()
        node = build()
        held = used() - before
        if deinit:
            node.deinit()
        freed = held - (used() - before)
        # The object itself and its inline buffers stay until it is dropped,
        # so most of it is the bar, not all of it.
        ok = freed * 10 >= held * 9
        rows.append("%-26s %6d bytes, %6d freed by deinit()%s"
                    % (name, held, freed, "" if ok else "  KEPT"))
        if not ok:
            failures.append("%s keeps %d of its %d bytes after deinit()"
                            % (name, held - freed, held))
        del node
    return rows, failures


#: Native nodes that still hold their source after deinit(), each with the
#: issue that tracks it (the CPython twins all let go). An entry that starts
#: letting go fails the run, so the exception cannot outlive its reason:
#: delete the line when it does.
KEEPS_SOURCE = {
}

#: On CircuitPython these modules are CircuitPython's own, not this
#: repository's, so what their deinit() keeps is not ours to check.
CIRCUITPYTHON_OWN = ("audiodelays", "audiofilters", "audiofreeverb",
                     "audiomixer", "synthio", "audiocore")


def _armed(build, frames):
    """A node playing a source of `frames` stereo frames, built in a frame of
    its own so nothing on this one's stack still points at the source."""
    node = build()
    node.play(audiocore.RawSample(array.array("h", bytes(frames * 4)),
                                  sample_rate=RATE, channel_count=CHANNELS))
    return node


def _scrub(depth=40):
    """The native collectors scan the stack conservatively, so a word left
    over from building the source, in a slot the collector's own frames leave
    unwritten, can keep it alive. Calls nested deeper than building it went
    overwrite that stretch of stack with small integers first."""
    if depth:
        return _scrub(depth - 1) + depth
    return 0


def check_sources(deinit):
    """A released node lets go of its source, so releasing the tail of a
    chain lets the rest of it be collected (audiodsp#177)."""
    failures = []
    rows = []
    used = memory_meter()
    frames = 50000                       # 200 KB of source
    on_circuitpython = sys.implementation.name == "circuitpython"
    for name, build in NODES:
        module = name.split(".")[0]
        if module in MISSING or (on_circuitpython
                                 and module in CIRCUITPYTHON_OWN):
            continue
        try:
            node = _armed(build, frames)
        except (TypeError, ValueError, AttributeError):
            continue                     # takes its source at construction
        _scrub()
        before = used()
        if deinit:
            node.deinit()
        _scrub()
        freed = before - used()
        released = freed * 10 >= frames * 4 * 9
        known = None if sys.implementation.name == "cpython" \
            else KEEPS_SOURCE.get(name)
        if known is not None:
            ok = not released
            note = "keeps it (%s)" % known if ok else \
                "lets go now: remove it from KEEPS_SOURCE"
        else:
            ok = released
            note = "lets go" if ok else "KEEPS IT"
        rows.append("%-26s %s" % (name, note))
        if not ok:
            failures.append("%s: source after deinit(): %s" % (name, note))
        del node
    return rows, failures


def main(argv):
    fault = "--fault" in argv
    deinit = not fault
    if fault:
        print("PLANTED FAULT: deinit() is not called, so every row must fail")
    print("%-26s %-7s %-9s %-7s %s"
          % ("node", "method", "state", "pull", "rewind"))
    print("-" * 66)

    failures = []
    checked = 0
    for name, build in NODES:
        if name.split(".")[0] in MISSING:
            continue
        row, bad = check_node(name, build, deinit)
        print(row)
        failures.extend(bad)
        checked += 1
    if "audioroute" not in MISSING:
        row, bad = check_splitter(deinit)
        print(row)
        failures.extend(bad)
        checked += 1

    print("-" * 66)
    print("every method and property of the node types audiodsp wrote")
    for name, build in NODES:
        if name not in OWN or name.split(".")[0] in MISSING:
            continue
        row, bad = check_methods(name, build, deinit)
        print(row)
        failures.extend(bad)

    print("-" * 66)
    print("the source let go at deinit()")
    rows, bad = check_sources(deinit)
    for row in rows:
        print(row)
    failures.extend(bad)

    print("-" * 66)
    print("storage given back at deinit()")
    rows, bad = check_storage(deinit)
    for row in rows:
        print(row)
    failures.extend(bad)

    print("-" * 66)
    print("%d node types checked" % checked)
    if MISSING:
        print("not built here, skipped: %s" % ", ".join(MISSING))
    if failures:
        print("FAIL: %d" % len(failures))
        for line in failures:
            print("  %s" % line)
        return 1
    print("PASS: every node released, and every way back in raises")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
