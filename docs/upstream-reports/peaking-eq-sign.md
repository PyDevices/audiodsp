# Draft: peaking EQ sign

**Note to the poster — strip everything above the `---`.**

Smallest of the three and the easiest to accept: one character, no
performance question, no design argument. File it first and on its own, so it
is not held up behind the band-edge discussion. Suggested title:

> `synthio.Biquad` PEAKING_EQ has the wrong sign on `b2`

Verified present on `main` 2026-08-27. Measured on a unix build of `main`;
the repro also runs on a board with the `get_buffer` loop replaced by an
`AudioOut`.

---

## `synthio.Biquad` PEAKING_EQ has the wrong sign on `b2`

`shared-module/synthio/Biquad.c`, in `common_hal_synthio_biquad_tick()`:

```c
        case SYNTHIO_PEAKING_EQ:
            b0 = 1 + alpha * A;
            b1 = -2 * sc.c;
            b2 = 1 + alpha * A;      // <-- should be 1 - alpha * A
            a0 = 1 + alpha / A;
            a1 = -2 * sc.c;
            a2 = 1 - alpha / A;
            break;
```

`b2` should be `1 - alpha * A`. The RBJ cookbook's peaking section is

```
b0 = 1 + alpha*A    b1 = -2*cos(w0)    b2 = 1 - alpha*A
a0 = 1 + alpha/A    a1 = -2*cos(w0)    a2 = 1 - alpha/A
```

and the minus is what makes numerator and denominator sum to the same value at
DC, and again at Nyquist. That is the whole premise of a peaking filter: unity
everywhere except the band around `f0`.

With the plus, the numerator picks up `2*alpha*A` at DC that the denominator
does not. Since `1 - cos(W0)` is very small at low frequencies, that term
dominates and the DC gain becomes

```
1 + alpha*A / (1 - cos(W0))
```

So the filter is not a bell with a blemish; it is a bass shelf with a bell
buried in it, and it gets worse as `f0` drops. A +6 dB bell at 1 kHz, Q 1,
48 kHz should be flat at DC and is **+21.4 dB**; at 200 Hz it is **+34.8 dB**.
The centre gain is wrong too — the bell that was asked for is sitting on top
of the shelf.

The other six modes are correct, which is why this survived: `PEAKING_EQ` is
the one mode a synthesis library rarely reaches for.

It reaches `synthio.Note.filter` as well as `audiofilters.Filter` — the
coefficients are shared.

### Fix

```diff
         case SYNTHIO_PEAKING_EQ:
             b0 = 1 + alpha * A;
             b1 = -2 * sc.c;
-            b2 = 1 + alpha * A;
+            b2 = 1 - alpha * A;
             a0 = 1 + alpha / A;
             a1 = -2 * sc.c;
             a2 = 1 - alpha / A;
             break;
```

### Measured

On a build of `main`, a +6 dB bell at 1 kHz / Q 1 / 48 kHz:

| | before | after | should be |
|---|---|---|---|
| DC | **+21.39 dB** | +0.00 | 0.00 |
| centre (1 kHz) | **+23.42 dB** | +6.00 | +6.00 |
| 12 kHz | −0.01 dB | −0.01 | 0.00 |

and a −6 dB cut reads −6.00 dB at centre after the fix. The +21.39 agrees with
the closed form `1 + alpha*A/(1 - cos W0)` = +21.40 dB to within the Q15
coefficient rounding.

### Repro

```python
# A peaking bell should be unity at DC and at Nyquist, and hit its own gain
# at the centre. On current main it is none of the three.
import array, math
import audiocore, audiofilters, synthio

RATE = 48000
FRAMES = 4096
LEVEL = 2000
GAIN_DB = 6.0
CENTRE = 1000.0


def bell():
    # A is 10 ** (dBgain / 40), per the Biquad docs.
    return synthio.Biquad(synthio.FilterMode.PEAKING_EQ, CENTRE, Q=1.0,
                          A=10 ** (GAIN_DB / 40))


def through(values):
    source = audiocore.RawSample(values, sample_rate=RATE, channel_count=1)
    node = audiofilters.Filter(filter=bell(), sample_rate=RATE,
                               channel_count=1, bits_per_sample=16,
                               samples_signed=True, buffer_size=FRAMES * 2)
    node.play(source, loop=True)
    out = []
    for block in range(3):
        data = bytes(audiocore.get_buffer(node)[1])
        if block < 2:                       # let the filter settle
            continue
        for index in range(0, len(data) - 1, 2):
            sample = data[index] | (data[index + 1] << 8)
            out.append(sample - 65536 if sample >= 32768 else sample)
    return out


def tone(hz):
    return array.array("h", [int(LEVEL * math.sin(2 * math.pi * hz * frame
                                                  / RATE))
                             for frame in range(FRAMES)])


def rms_db(values):
    total = sum(value * value for value in values)
    return 20 * math.log10(math.sqrt(total / len(values))
                           / (LEVEL / math.sqrt(2)))


print("a %+g dB bell at %g Hz, Q 1, %d Hz" % (GAIN_DB, CENTRE, RATE))
dc = through(array.array("h", [LEVEL] * FRAMES))
print("  DC      %+7.2f dB   (should be 0.00)"
      % (20 * math.log10(abs(dc[-1]) / LEVEL)))
print("  centre  %+7.2f dB   (should be %+.2f)"
      % (rms_db(through(tone(CENTRE))), GAIN_DB))
print("  12 kHz  %+7.2f dB   (should be 0.00)"
      % rms_db(through(tone(12000.0))))
```

```
a +6 dB bell at 1000 Hz, Q 1, 48000 Hz
  DC       +21.39 dB   (should be 0.00)
  centre   +23.42 dB   (should be +6.00)
  12 kHz    -0.01 dB   (should be 0.00)
```

`audiocore.get_buffer` is not in upstream; on a board, play the filter into an
`AudioOut` instead and capture. The effect is 21 dB, so it is not subtle
however it is measured.
