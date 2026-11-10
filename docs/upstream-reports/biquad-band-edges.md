# Draft: the biquads cannot reach either end of the audio band

**Note to the poster — strip everything above the `---`.**

The big one, and the only one of the three that is a judgement call rather
than a mistake: the fix costs real cycles on Cortex-M0+, and the maintainers
may reasonably want it opt-in, per-port, or not at all. So this is written as
a **limitation report with a proposed fix and its measured cost**, not as a
patch to merge. Offer the measurements and let them decide the shape.

File it *after* the peaking-sign and reset one-liners — those are unarguable
and should not wait behind this discussion. Suggested title:

> `synthio.Biquad` is inaccurate below ~300 Hz and above ~16 kHz (two
> independent causes)

Two things worth being ready for in the thread:

- The two causes must be fixed **together**. Fixing the fixed-point format
  alone leaves ~1 dB of error at 50 Hz, and fixing the polynomial alone leaves
  everything below 300 Hz where it already is. The isolation table below is
  the evidence for that, and is the main reason this is one report.
- The sine/cosine half is nearly free on every core and could land on its own
  as a partial improvement — it is the whole story above 16 kHz. If the thread
  stalls on the M0+ cost, that is the fallback worth proposing.

Verified present on `main` 2026-08-27 (`BIQUAD_SHIFT` is still 15,
`fast_sincos` unchanged). Everything below measured on a unix build of `main`.

---

## `synthio.Biquad` is inaccurate below ~300 Hz and above ~16 kHz

A biquad asked for a corner near either end of the audio band is not the
filter that was asked for, and it fails silently — a `LOW_PASS` at 50 Hz
returns silence, a `HIGH_PASS` at 30 Hz *boosts*, and a low shelf does
nothing.

There are **two independent causes** and neither can be fixed alone, which is
why they are in one report.

### Cause 1: Q15 coefficients in an int32 accumulator

`shared-module/synthio/Biquad.h`:

```c
#define BIQUAD_SHIFT (15)
```

`shared-module/synthio/Biquad.c`:

```c
static int32_t biquad_scale_arg_float(mp_float_t arg) {
    return (int32_t)MICROPY_FLOAT_C_FUN(round)(MICROPY_FLOAT_C_FUN(ldexp)(arg, BIQUAD_SHIFT));
}
...
static inline int32_t biquad_filter_sample(int32_t input, int32_t a1, int32_t a2, int32_t b0, int32_t b1, int32_t b2, int32_t x0, int32_t x1, int32_t y0, int32_t y1) {
    return synthio_sat16((b0 * input + b1 * x0 + b2 * x1 - a1 * y0 - a2 * y1 + (1 << (BIQUAD_SHIFT - 1))), BIQUAD_SHIFT);
}
```

Both halves are the problem, for different reasons.

**The coefficients are what quantize.** A low-pass has `b0 = (1 - cos W0)/2`,
which goes to zero with `W0`. At 48 kHz:

| corner | `b0` (normalised) | in Q15 | stored |
|---|---|---|---|
| 1000 Hz | 3.916e-3 | 128.32 | 128 |
| 400 Hz | 6.608e-4 | 21.65 | 22 |
| 200 Hz | 1.682e-4 | 5.51 | 6 |
| 100 Hz | 4.244e-5 | **1.39** | **1** |
| 50 Hz | 1.066e-5 | **0.35** | **0** |

By 100 Hz the numerator is carrying one and a half significant *bits*, and by
50 Hz it has rounded away entirely.

**The accumulator is what overflows.** `a1` approaches −2 as `W0` goes to
zero: at 100 Hz it is −1.9815, which is −64929 in Q15. Multiplied by a
full-scale sample that is 2.13e9, against an `INT32_MAX` of 2.147e9 — so one
of the five terms already fills the accumulator, with four still to add.

**And the feedback state is rounded to whole samples.** `y0`/`y1` hold the
saturated `int16` output, and a biquad low down has both poles close to the
unit circle: `1/A(z)` at DC is about 4000 for a 100 Hz low-pass and 43000 for
a 30 Hz high-pass. That is the gain the loop applies to whatever error is fed
back into it, so half an LSB of rounding is not half an LSB by the time it
comes round again. This is what takes a 50 Hz low-pass from "wrong" to
"silent", and what makes a 30 Hz high-pass *gain* rather than merely mis-sit.

### Cause 2: `fast_sincos()` fits one polynomial to two functions

```c
#define FOUR_OVER_PI (4 / M_PI)
static void fast_sincos(mp_float_t theta, sincos_result_t *result) {
    mp_float_t x = (theta * FOUR_OVER_PI) - 1;
    ...
    mp_float_t evens = c4x4 + c2x2 + c0, odds = c5x5 + c3x3 + c1x;
    result->c = evens + odds;
    result->s = evens - odds;
}
```

One 5th-order fit to both sine and cosine over `[0, pi/2]`, worth about
**2.3e-5** of absolute error at worst. That is a fine approximation in
general, and wrong at both ends of the audio band for two different reasons.

**At the bottom**, every low-pass, notch and shelf coefficient is built from
`1 - cos W0`, which is a difference of two nearly-equal numbers:

| corner (48 kHz) | `1 - cos W0` | cosine error | error as a fraction |
|---|---|---|---|
| 1000 Hz | 8.56e-3 | 2.3e-5 | 0.3 % |
| 300 Hz | 7.71e-4 | 1.4e-5 | 1.8 % |
| 100 Hz | 8.57e-5 | 5.5e-6 | **6.4 %** |
| 50 Hz | 2.14e-5 | 2.8e-6 | **13.2 %** |
| 20 Hz | 3.43e-6 | 1.2e-6 | **33.7 %** |

**At the top**, `pi/2` in `W0` is `SR/4` — only 12 kHz at 48 kHz — and above
that the fit is *extrapolating*:

| tone (48 kHz) | sine | error | `1 + cos W0` | error |
|---|---|---|---|---|
| 16 kHz | +0.86914 vs +0.86603 | +0.4 % | 0.50221 vs 0.50000 | +0.4 % |
| 20 kHz | +0.53305 vs +0.50000 | **+6.6 %** | 0.15369 vs 0.13397 | **+14.7 %** |
| 22 kHz | +0.33865 vs +0.25882 | **+30.8 %** | 0.07766 vs 0.03407 | **+128 %** |

This half appears not to have been reported before. It is also the half that
is cheap to fix.

Worth noting for a board rather than a desktop: `mp_float_t` is single
precision by default (`py/circuitpy_mpconfig.h`,
`MICROPY_FLOAT_IMPL_FLOAT`), so the cancellation in `1 - cos W0` is worse
there than in the double-precision figures above — float epsilon alone costs
about 3.5 % at 100 Hz before the polynomial's error is counted.

### What it measures

One biquad, 48 kHz, Q 0.707 unless stated, on a build of `main`. A corner
should read −3.01 dB and a passband 0.00:

| asked for | reads | should be |
|---|---|---|
| `LOW_PASS` 50 Hz, at cutoff | **silence** | −3.01 |
| `LOW_PASS` 100 Hz, at cutoff | −3.94 | −3.01 |
| `LOW_PASS` 200 Hz, at 50 Hz | −0.03 | −0.02 |
| `HIGH_PASS` 30 Hz, at cutoff | **+9.03** | −3.01 |
| `HIGH_PASS` 100 Hz, at cutoff | −4.36 | −3.01 |
| `LOW_SHELF` 80 Hz +1.5 dB, at 20 Hz | **−0.28** | +1.49 |
| `LOW_PASS` 16 kHz, at cutoff | −3.06 | −3.01 |
| `LOW_PASS` 20 kHz, at cutoff | −3.71 | −3.01 |
| `LOW_PASS` 22 kHz, at cutoff | **−7.30** | −3.01 |
| `HIGH_PASS` 22 kHz, at 1 kHz (stopband) | **−52.41** | −82.56 |

The usable range is roughly 300 Hz to 16 kHz, and about 400 Hz to 12 kHz for
anything under half a decibel of error.

**"Silence" in the first row is too kind — it latches.** Measured on this port
2026-09-09 (identical on the CPython extension, desktop MicroPython and a
patched CircuitPython 10.3.0 build, so it is the kernel and not a binding).
Feed a `LOW_PASS` at 40 Hz / Q 0.707 one 512-frame block and then silence, and
its output settles on a constant and stays there:

| material | settles at |
|---|---|
| DC burst | 16143 |
| kick — 60 Hz decaying sine | 16143 |
| bass note, 55 Hz | 16143 |
| square, 80 Hz | 16143 |
| noise burst | 16143 |
| asymmetric saw, 45 Hz | 0 |

Five of six, and at 100 Hz all six settle at 2631. Feeding fresh audio
afterwards does not recover it: `min == max == 16143`, so the section has
stopped passing signal rather than merely attenuating it, and
`audiofilters.Filter` exposes no way to clear the state.

The reason is the `b0` column above, read at its logical conclusion. At 40 Hz
the input coefficient rounds to **zero** — the section has no input gain at all
— leaving a bare feedback recursion whose poles sit at radius 0.9963 and which
therefore has an integer fixed point to fall into. The `b0` figures here are
computed with an exact cosine; through `fast_sincos`, which is what the code
actually runs, they are 4.51e-05 at 100 Hz and 7.96e-06 at 40 Hz, so the
picture is the same or slightly worse.

**Proportion, so the thread does not start on the wrong foot.** This is a corner
most callers never visit: at 200 Hz and above the coefficients are fine, and
synthio's own note filters sit well clear of it. It matters to a project trying
for exact replication and for instrument-grade fidelity below 100 Hz. It is
worth reporting as a limitation with a fix and a cost, which is what this
document is — not as an alarm.

### Why both causes have to go together

Turning each cause off in the closed form, at 48 kHz:

| | as shipped | Q15 only | polynomial only | ideal |
|---|---|---|---|---|
| `LOW_PASS` 50 Hz @ cutoff | −6.09 | −6.09 | **−1.96** | −3.01 |
| `LOW_PASS` 100 Hz @ cutoff | −3.95 | −3.95 | **−2.48** | −3.01 |
| `LOW_SHELF` 80 Hz +1.5 @ 20 Hz | +0.08 | +0.08 | **+1.50** | +1.49 |
| `HIGH_PASS` 30 Hz @ cutoff | −4.71 | −4.71 | **−3.12** | −3.01 |
| `LOW_PASS` 20 kHz @ cutoff | −3.71 | **−3.01** | −3.71 | −3.01 |
| `LOW_PASS` 22 kHz @ cutoff | −7.29 | **−3.01** | −7.29 | −3.01 |

Read the columns:

- **Below 300 Hz**, quantization swamps everything — "as shipped" and "Q15
  only" are identical, so the polynomial's error is currently *invisible*
  there. Widen the coefficients and it appears: about 1 dB at 50 Hz.
- **Above 16 kHz**, the polynomial is the whole story — "Q15 only" is exact
  and "polynomial only" reproduces the shipped error to the last digit.

So a fix to either half alone leaves half the band still wrong. (The closed
form is also optimistic at the very bottom: it says −6.09 dB at 50 Hz where
the build gives silence, and −4.71 at 30 Hz where the build gives +9.03. The
difference is the whole-sample feedback state described above, which a
transfer function cannot express.)

### A fix, and what it costs

This is what we did in a downstream port of these files. Offering it as a
starting point rather than a patch to take, because the cost below is real
and it is your call whether to pay it.

1. **Per-filter coefficient format instead of a fixed Q15.** Take the largest
   of the five normalised coefficients and give them all as many fractional
   bits as that one has room for in an `int32_t`, capped at 30. A plain
   low-pass tops out near 2 (that is `a1`) and gets 29 bits; a 20 dB shelf
   reaches ~200 and gets 23. One fixed format for every filter means giving
   them all the shelf's. The chosen shift has to travel with the coefficients
   — they are meaningless apart — so it is cached in `synthio_biquad_t`
   alongside `a1..b2`.
2. **`int64_t` accumulator, and a feedback state carrying ~12 bits below the
   sample grid.** The second half is not optional, for the `1/A(z)` reason
   above: rounding the feedback to whole samples hands a gain of thousands
   half an LSB to amplify.
3. **Replace `fast_sincos()`**: reflect into `[0, pi/2]`
   (`sin(pi - t) = sin t`, `cos(pi - t) = -cos t`) and evaluate the two Taylor
   series separately, seven terms each. Worst error across 20 Hz – 24 kHz
   falls from 2.3e-5 to 6.3e-9, and it stays sane past Nyquist, so a frequency
   asked for above Nyquist is merely wrong rather than absurd. About ten extra
   multiplies, paid once per coefficient update at block rate, not per sample.

With all three, every mode lands within **0.03 dB** of the double-precision
closed form from 50 Hz to 22 kHz.

**The cost, measured** by cross-compiling the filter kernel at `-Os` and
counting instructions in the per-sample function:

| core | before | after | note |
|---|---|---|---|
| Cortex-M4 / M7 | 37 | 76 | no library calls — `SMULL`/`SMLAL` |
| Cortex-M0+ | 55 | 154 | 5 `__aeabi_lmul` + 1 `__aeabi_lasr` |

Roughly 2x on anything with a long multiply, closer to 4x without one. One
stereo biquad at 22050 Hz goes from about 5 % of a 48 MHz M0+ to about 17 %.
That is the trade Q15 was making, and on an RP2040 it is not a small one. If
it matters, the products are 32x32 into 64 and could be hand-written for M0
rather than going through `__aeabi_lmul`.

Step 3 on its own is nearly free on every core, and it is the entire fix above
16 kHz — so it is worth considering separately even if steps 1 and 2 are not
wanted.

### Repro

```python
# What a biquad does at the ends of the audio band.
import array, math
import audiocore, audiofilters, synthio

RATE = 48000
FRAMES = 8192


def tone(hz, level=6000, frames=FRAMES):
    values = array.array("h")
    for frame in range(frames):
        values.append(int(level * math.sin(2 * math.pi * hz * frame / RATE)))
    return values


def through(mode, corner, hz, Q=0.7071067811865475, A=None, level=6000):
    values = tone(hz, level)
    source = audiocore.RawSample(values, sample_rate=RATE, channel_count=1)
    biquad = (synthio.Biquad(mode, corner, Q=Q) if A is None
              else synthio.Biquad(mode, corner, Q=Q, A=A))
    node = audiofilters.Filter(filter=biquad, sample_rate=RATE,
                               channel_count=1, bits_per_sample=16,
                               samples_signed=True, buffer_size=FRAMES * 2)
    node.play(source, loop=True)
    total = 0
    count = 0
    for block in range(4):
        data = bytes(audiocore.get_buffer(node)[1])
        if block < 2:                      # let the filter settle
            continue
        for index in range(0, len(data) - 1, 2):
            sample = data[index] | (data[index + 1] << 8)
            if sample >= 32768:
                sample -= 65536
            total += sample * sample
            count += 1
    out = math.sqrt(total / count)
    reference = level / math.sqrt(2)
    return 20 * math.log10(out / reference) if out > 0 else float("-inf")


FM = synthio.FilterMode
print("A corner should read -3.01 dB, and a passband 0.00.")
print()
for label, mode, corner, hz, A in (
        ("LOW_PASS   50 Hz, at cutoff", FM.LOW_PASS, 50.0, 50.0, None),
        ("LOW_PASS  100 Hz, at cutoff", FM.LOW_PASS, 100.0, 100.0, None),
        ("LOW_PASS  200 Hz, at 50 Hz ", FM.LOW_PASS, 200.0, 50.0, None),
        ("HIGH_PASS  30 Hz, at cutoff", FM.HIGH_PASS, 30.0, 30.0, None),
        ("HIGH_PASS 100 Hz, at cutoff", FM.HIGH_PASS, 100.0, 100.0, None),
        ("LOW_SHELF  80 Hz +1.5 dB, at 20 Hz",
         FM.LOW_SHELF, 80.0, 20.0, 10 ** (1.5 / 40)),
        ("LOW_PASS   16 kHz, at cutoff", FM.LOW_PASS, 16000.0, 16000.0, None),
        ("LOW_PASS   20 kHz, at cutoff", FM.LOW_PASS, 20000.0, 20000.0, None),
        ("LOW_PASS   22 kHz, at cutoff", FM.LOW_PASS, 22000.0, 22000.0, None),
        ("HIGH_PASS  22 kHz, at 1 kHz (stopband)",
         FM.HIGH_PASS, 22000.0, 1000.0, None)):
    print("%-40s %+8.2f dB" % (label, through(mode, corner, hz, A=A)))
```

```
A corner should read -3.01 dB, and a passband 0.00.

LOW_PASS   50 Hz, at cutoff                  -inf dB
LOW_PASS  100 Hz, at cutoff                 -3.94 dB
LOW_PASS  200 Hz, at 50 Hz                  -0.03 dB
HIGH_PASS  30 Hz, at cutoff                 +9.03 dB
HIGH_PASS 100 Hz, at cutoff                 -4.36 dB
LOW_SHELF  80 Hz +1.5 dB, at 20 Hz          -0.28 dB
LOW_PASS   16 kHz, at cutoff                -3.06 dB
LOW_PASS   20 kHz, at cutoff                -3.71 dB
LOW_PASS   22 kHz, at cutoff                -7.30 dB
HIGH_PASS  22 kHz, at 1 kHz (stopband)     -52.41 dB
```

`audiocore.get_buffer` is not in upstream; on a board, play each filter into
an `AudioOut` and capture instead. The effects run from 1 dB to total silence,
so they survive any reasonable measurement.
