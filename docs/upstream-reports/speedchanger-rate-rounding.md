# Draft: `audiospeed`'s Q16 rate truncates, so it can lose a whole LSB

**Note to the poster — strip everything above the `---`.**

A one-liner, in a module that is new in 10.3.0, and independent of the phase
report beside it ([speedchanger-phase-carry.md](speedchanger-phase-carry.md)) —
they touch different lines of the same file and either can go first. Suggested
title:

> `audiospeed_rate_to_fp` truncates to 16.16, so a rate just under a step
> loses a whole LSB

Verified present on `10.3.0` 2026-09-17 by running the repro below on a build
of that tag with `CIRCUITPY_AUDIOSPEED` on. The reader-visible symptom needs
only the `rate` property, so nothing in the repro is specific to how the audio
was captured.

---

### `audiospeed_rate_to_fp` truncates to 16.16, so a rate just under a step loses a whole LSB

`shared-module/audiospeed/__init__.c`:

```c
uint32_t audiospeed_rate_to_fp(mp_obj_t rate_obj) {
    mp_float_t rate = mp_arg_validate_obj_float_range(rate_obj, 0.001, 1000.0, MP_QSTR_rate);
    return (uint32_t)(rate * (1 << SPEED_SHIFT));
}
```

A C cast to an integer type truncates toward zero, so this is a floor, not a
round. The conversion's worst-case error is a full 16.16 step rather than half
a step, and a float that lands a hair *below* a step falls to the step beneath
it instead of arriving at the one it is next to:

| `rate` | `rate_fp` | as a float | wanted |
|---|---:|---|---:|
| `0.5 - 1e-6` | 32767 | 0.4999847412109375 | 32768 |
| `1.0/4.0000000000000036` | 16383 | 0.2499847412109375 | 16384 |
| `1.0/1.0000000000000004` | 65535 | 0.9999847412109375 | 65536 |

`shared-module/audiospeed/Resampler.c` reaches the same arithmetic by another
road and truncates the same way:

```c
static void calculate_rate(audiospeed_base_t *self, uint32_t sample_rate) {
    if (self->source != NULL && sample_rate) {
        self->speed.rate_fp = (uint32_t)((mp_float_t)self->base.sample_rate / sample_rate * (1 << SPEED_SHIFT));
```

so a `Resampler` binding 48000 into 44100 stores 71331 where 71331.918… wants
71332.

#### Why it is worth a character

A caller that computes a rate rather than typing one is the common case —
`fs / some_hz`, an interval as `2 ** (semitones / 12)`, or the reciprocal of a
rate it used elsewhere — and floating point lands a hair either side of the
exact value about half the time. Landing *below* costs a whole LSB.

The case that surfaced this: a sample-and-hold built from two `SpeedChanger`s,
one decimating at `N` and one restoring at `1/N`. At `N = 1.0000000000000004`,
which is what `fs / rate_hz` returns for a log-mapped rate knob at the top of
its travel, the pair should be an identity and is not: the second leg is
65535/65536, and a full-scale 441 Hz tone at 44.1 kHz comes out **27666 codes**
away from what went in rather than 0. At 48 kHz and 22.05 kHz the same knob
happens to land exactly on 1.0, so the same code is an identity there — which
is what makes it look like a sample-rate bug rather than a rounding one.

#### Fix

```diff
 uint32_t audiospeed_rate_to_fp(mp_obj_t rate_obj) {
     mp_float_t rate = mp_arg_validate_obj_float_range(rate_obj, 0.001, 1000.0, MP_QSTR_rate);
-    return (uint32_t)(rate * (1 << SPEED_SHIFT));
+    return (uint32_t)(rate * (1 << SPEED_SHIFT) + (mp_float_t)0.5);
 }
```

and, in `Resampler.c`:

```diff
-        self->speed.rate_fp = (uint32_t)((mp_float_t)self->base.sample_rate / sample_rate * (1 << SPEED_SHIFT));
+        self->speed.rate_fp = (uint32_t)((mp_float_t)self->base.sample_rate / sample_rate * (1 << SPEED_SHIFT) + (mp_float_t)0.5);
```

`rate` is validated non-negative before either line, so the unsigned `+ 0.5`
needs no sign test. The range check is unaffected: 1000.0 rounds to 65536000,
which is what it truncated to as well.

#### Repro

```python
# audiospeed.SpeedChanger.rate should come back as close to what was asked
# for as 16.16 allows, which is half a step, not a whole one.
import array
import audiocore, audiospeed

source = audiocore.RawSample(array.array("h", [0] * 512),
                             sample_rate=48000, channel_count=2)

for asked, wanted in ((0.5 - 1e-6, 32768),
                      (1.0 / 4.0000000000000036, 16384),
                      (1.0 / 1.0000000000000004, 65536)):
    stored = round(audiospeed.SpeedChanger(source, asked).rate * 65536)
    print("asked %.17g -> %6d, wanted %6d %s"
          % (asked, stored, wanted, "" if stored == wanted else "<-- an LSB"))
```

```
asked 0.499999 ->  32767, wanted  32768 <-- an LSB
asked 0.24999999999999978 ->  16383, wanted  16384 <-- an LSB
asked 0.9999999999999996 ->  65535, wanted  65536 <-- an LSB
```

A build with `MICROPY_FLOAT_IMPL_FLOAT` rounds the last two arguments to 1.0
and 0.25 before they arrive, so use the first row there; it fails in both
float widths.
