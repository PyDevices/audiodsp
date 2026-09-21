"""Deterministic biquad PCM probes across every FilterMode.

The rest of the parity suite only ever builds LOW_PASS filters, which is how
the peaking-EQ sign error in `audiodsp_biquad.c` survived: no fixture reached
mode 4. This probe walks all seven modes in mono and stereo, and then all seven
again across the band, so the three deliberate deviations recorded in
docs/upstream-diff.md ("Peaking EQ computed b2 with the wrong sign", "A stereo
Filter shared one biquad state" and "The biquads are Q15, so they cannot go
low") are each pinned by something that fails when they regress.

Running this against `bin/circuitpython` is expected to differ. That is the
point of it; see docs/upstream-diff.md for the direction of each difference.

**The material is integer arithmetic, deliberately.** The square wave used to
be `(2*pi*hz*frame/RATE) % 2*pi < pi`, and a Python float is the interpreter's
`mp_float_t` -- so on a single-precision build the *input waveform* was a
different waveform (96000 against 120000 summed, 8 frames flipped in mono and
96 in stereo), and this fixture was comparing two DSP kernels on two different
signals. `(2 * hz * frame) % (2 * RATE) < RATE` is the same square wave stated
exactly, identical on every target. It is not byte-identical to what the float
expression produced on a double build -- the float form classified a handful of
exact-zero and exact-half crossings the other way -- so `golden/
biquad_component.json` was re-captured with it (audiodsp#80).

Q and A go through `audiodsp_util.float32` for the same reason one layer up:
0.4, 0.7079 and 1.4125 are not one number on a single-precision target.

**What is left, and why it is not fixed here.** Four lines -- `biquad_qa
high_pass` at Q 0.4 -- still diverge on a single-precision MicroPython, because
`src/synthio/Biquad.c:79` derives W0 in `mp_float_t` where the CPython
extension calls the shared `audiodsp_biquad_cp_w0()` in double. audiodsp#101 has
the measurement and the fix; it moves board digests, so it is its own change.
"""

from array import array

import audiocore
import audiofilters
import synthio
from audiodsp_util import float32


#: A and Q the shelf/peaking modes need. A is RBJ's amplitude parameter,
#: 10**(gain_db/40) - so 1.4125 is a +6 dB bell rather than a +1.4 dB one.
GAIN_A = float32(1.4125)
Q = 1.0
CENTER = 1200
RATE = 8000

#: Q and A were CONSTANT across all 182 fixtures, so three of the five
#: arguments to audiodsp_biquad_configure could be hardcoded to the probe's own
#: value without moving the hash - two of them silently untested. Frequency was
#: the one scalar that varied. These sweep them.
Q_VALUES = (float32(0.4), 1.0, 4.0)
A_VALUES = (float32(0.7079), float32(1.4125))


def checksum(data):
    """FNV-1a over the bytes.

    `sum(data)` is invariant under any permutation of a block and under any set
    of byte deltas that cancel, so 168 of this probe's 182 lines certified 4096
    samples with a statistic that could not see them reordered - demonstrated
    with 90.6% of samples wrong at an identical byte-sum. This is order- and
    magnitude-sensitive; audiocomponents' instrument probes use the same
    function for the same reason. Both are printed: the sum stays comparable
    with the older records, the checksum is what actually pins the PCM.
    """
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value

MODES = (
    ("low_pass", synthio.FilterMode.LOW_PASS),
    ("high_pass", synthio.FilterMode.HIGH_PASS),
    ("band_pass", synthio.FilterMode.BAND_PASS),
    ("notch", synthio.FilterMode.NOTCH),
    ("low_shelf", synthio.FilterMode.LOW_SHELF),
    ("high_shelf", synthio.FilterMode.HIGH_SHELF),
    ("peaking_eq", synthio.FilterMode.PEAKING_EQ),
)


def source(channel_count):
    """Two tones an octave apart, hard-panned when stereo.

    The channels have to differ, or a filter that shares one state across
    them is indistinguishable from one that does not.
    """
    values = array("h")
    for frame in range(768):
        for channel in range(channel_count):
            hz = 300 if channel == 0 else 2400
            # A cheap square-ish shape: deterministic, and rich enough above
            # and below CENTER that every mode has something to act on. In
            # integers, so it is the same shape on every interpreter -- see
            # the module docstring.
            values.append(12000 if (2 * hz * frame) % (2 * RATE) < RATE
                          else -12000)
    return audiocore.RawSample(values, sample_rate=RATE,
                               channel_count=channel_count)


for name, mode in MODES:
    for channels in (1, 2):
        biquad = synthio.Biquad(mode, CENTER, Q, A=GAIN_A)
        effect = audiofilters.Filter(
            filter=biquad, mix=1.0, buffer_size=512, sample_rate=RATE,
            bits_per_sample=16, samples_signed=True, channel_count=channels,
        )
        effect.play(source(channels))
        for index in range(4):
            data = bytes(audiocore.get_buffer(effect)[1])
            if index == 0:
                words = tuple(int.from_bytes(data[pos:pos + 2], "little")
                              for pos in range(0, 24, 2))
                decoded = tuple(word - 65536 if word >= 32768 else word
                                for word in words)
                print("biquad_samples", name, channels, decoded)
            print("biquad", name, channels, index, len(data),
                  sum(data), checksum(data))


#: Centers spanning the usable band at RATE (Nyquist 4000). The middle two are
#: unremarkable; the outer two are the whole point. 60 Hz is where Q15
#: coefficients used to collapse - a low-pass there returned silence - and
#: 3600 Hz is past pi/2 in W0, where the old sine/cosine fit was extrapolating
#: and a high-pass passed its own stopband. Neither end was pinned by anything
#: before the biquads were widened; see docs/upstream-diff.md.
EDGE_CENTERS = (60, 200, 1200, 3600)

for name, mode in MODES:
    for center in EDGE_CENTERS:
        biquad = synthio.Biquad(mode, center, Q, A=GAIN_A)
        effect = audiofilters.Filter(
            filter=biquad, mix=1.0, buffer_size=512, sample_rate=RATE,
            bits_per_sample=16, samples_signed=True, channel_count=2,
        )
        effect.play(source(2))
        for index in range(4):
            data = bytes(audiocore.get_buffer(effect)[1])
            print("biquad_edge", name, center, index, len(data),
                  sum(data), checksum(data))


# --- Q and A, which no fixture above varies -----------------------------------
# Three of the five arguments to the filter were pinned only at one value each.
# A resonance that is ignored, or a shelf gain wired to a constant, moved
# nothing in this gate before these lines existed.

for name, mode in MODES:
    for q_value in Q_VALUES:
        for a_value in A_VALUES:
            biquad = synthio.Biquad(mode, CENTER, q_value, A=a_value)
            effect = audiofilters.Filter(
                filter=biquad, mix=1.0, buffer_size=512, sample_rate=RATE,
                bits_per_sample=16, samples_signed=True, channel_count=2,
            )
            effect.play(source(2))
            for index in range(2):
                data = bytes(audiocore.get_buffer(effect)[1])
                print("biquad_qa", name, "%.4f" % q_value, "%.4f" % a_value,
                      index, len(data), sum(data), checksum(data))


# --- synthio.Note.filter, which this gate has never exercised at all ----------
# The probe only ever built audiofilters.Filter. `Note.filter` is a different
# path - it accepts a Biquad or a tuple of up to four, applied per voice inside
# the synthesizer - and it is audiodsp's own extension over the oracle. It could
# be reduced to a total no-op and nothing here would notice. The cascade is the
# part most worth pinning: a dropped fourth stage is exactly the defect a
# level-based check misses, and the only other coverage
# (test_cpython_press_semantics.py::FilterCascade) compares 1 stage against 2.

CASCADE_MODES = (synthio.FilterMode.LOW_PASS, synthio.FilterMode.HIGH_PASS,
                 synthio.FilterMode.BAND_PASS, synthio.FilterMode.PEAKING_EQ)

# A cascade -- `Note.filter` taking a tuple -- is this port's extension;
# CircuitPython 10.3.0 raises "filter must be of type Biquad, not tuple". The
# single-stage case is common ground and runs everywhere, so the cascade cases
# announce themselves and stop rather than killing the probe. That keeps this
# file runnable on all three interpreters, which is what lets verify_dsp
# compare them (audiodsp#77 -- graded against its own stored digest, this probe
# could not see the two targets disagreeing with each other).
for stages in (1, 2, 3, 4):
    synth = synthio.Synthesizer(sample_rate=RATE, channel_count=2)
    stack = tuple(
        synthio.Biquad(CASCADE_MODES[i], CENTER * (i + 1), Q, A=GAIN_A)
        for i in range(stages)
    )
    try:
        note = synthio.Note(220.0, amplitude=float32(0.6),
                            filter=stack[0] if stages == 1 else stack)
    except TypeError:
        print("note_filter_cascade unsupported", stages)
        continue
    synth.press(note)
    for index in range(4):
        data = bytes(audiocore.get_buffer(synth)[1])
        if index == 0:
            words = tuple(int.from_bytes(data[pos:pos + 2], "little")
                          for pos in range(0, 24, 2))
            print("note_filter_samples", stages,
                  tuple(w - 65536 if w >= 32768 else w for w in words))
        print("note_filter", stages, index, len(data), sum(data),
              checksum(data))
