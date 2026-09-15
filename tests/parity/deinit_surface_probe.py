"""Every node in the palette releases, and refuses to be used afterwards.

Runs unchanged on the CPython target, on desktop MicroPython and on
`circuitpython-effects`, because the thing it asserts is the same on all
three: a node has `deinit()`, calling it marks the node released, and every
way back into the audio path then raises instead of handing out stale audio
or reading freed memory.

It does not compare exception *types*, on purpose. The native builds raise
CircuitPython's `ValueError` and the CPython target raises `RuntimeError`
(audioif#73); this probe is not the place to settle that, so it records only
that the guard fired.

**The fault this exists to catch.** Twelve of the node types --
every one audioif wrote rather than ported from CircuitPython -- had no
`deinit()` at all, so no class built on them could release one and Tier 1's
"deinit() releases every node the class built" was unmeasurable on a board
(audioif#58, #60, #63). One of them, `audiomixer.Mixer`, had `deinit()` and
no guard on the C protocol entry point, and pulling a released one dumped
core (audioif#59).

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
import audiospeed
import audioverb
import synthio

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

    audioif's own builds **raise**: `audiosample_get_buffer` calls
    `audiosample_check_for_deinit`, which throws. Upstream CircuitPython
    **returns an error** instead - `shared-module/audiocore/__init__.c:37`
    checks `audiosample_deinited()` and hands back `GET_BUFFER_ERROR` with a
    NULL buffer, and its `reset_buffer` returns without doing anything. Both
    are guards; neither reads freed memory. The difference is a convention, not
    a defect, and this probe accepts both on purpose.

    An earlier version counted only exceptions, and so reported every node on
    the patched CircuitPython build as unguarded - which was wrong, and was
    written into audioif#59 before the measurement was taken. What a released
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


def main(argv):
    fault = "--fault" in argv
    deinit = not fault
    if fault:
        print("PLANTED FAULT: deinit() is not called, so every row must fail")
    print("%-26s %-7s %-9s %-7s %s"
          % ("node", "method", "state", "pull", "rewind"))
    print("-" * 66)

    failures = []
    for name, build in NODES:
        row, bad = check_node(name, build, deinit)
        print(row)
        failures.extend(bad)
    row, bad = check_splitter(deinit)
    print(row)
    failures.extend(bad)

    print("-" * 66)
    print("%d node types checked" % (len(NODES) + 1))
    if failures:
        print("FAIL: %d" % len(failures))
        for line in failures:
            print("  %s" % line)
        return 1
    print("PASS: every node released, and every way back in raises")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
