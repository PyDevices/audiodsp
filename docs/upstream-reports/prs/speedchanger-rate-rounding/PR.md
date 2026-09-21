# PR for adafruit/circuitpython: audiospeed rate rounding

Branch: `fix-speedchanger-rate-rounding` (one commit, the patch beside this
document). From audiodsp#92 and the draft in
[speedchanger-rate-rounding.md](../../speedchanger-rate-rounding.md).

Independent of the [phase-carry PR](../speedchanger-phase-carry/PR.md): they
touch different lines of the same file, each patch applies to a clean tree on
its own, and either can go first. Verified against the `10.3.0` tag
(`d897c15f`) and the `main` tip (`268a168d`, 2026-09-17) — the three files
involved are byte-identical between the two.

## Title

```
audiospeed: round the 16.16 rate instead of truncating
```

## Body

```markdown
`audiospeed_rate_to_fp` casts to `uint32_t`, which truncates, so a rate that
lands a hair under a step loses a whole LSB: `1/1.0000000000000004` stores
65535/65536 rather than unity, and a pair of SpeedChangers meant to cancel
doesn't. `Resampler`'s `calculate_rate` truncates the same way — 48000 into
44100 stores 71331 where 71331.9 rounds to 71332. Rounding halves the
conversion's worst-case error; a rate already on a step is unaffected.
```

## Verification transcript (2026-09-18) — not for posting

Bug still present on the `main` tip, cited from the file upstream serves
today (`gh api .../contents/shared-module/audiospeed/__init__.c?ref=main`,
`268a168d`, identical to `10.3.0`):

```
shared-module_audiospeed___init__.c:20:    return (uint32_t)(rate * (1 << SPEED_SHIFT));
shared-module_audiospeed_Resampler.c:11:        self->speed.rate_fp = (uint32_t)((mp_float_t)self->base.sample_rate / sample_rate * (1 << SPEED_SHIFT));
```

Built the `10.3.0` tree's unix coverage port (see "Building audiospeed on the
coverage port" below) and ran `rate_rounding.py` beside this file.

Before (stock `10.3.0`):

```
$ ports/unix/build-coverage/micropython rate_rounding.py
asked 0.499999 ->  32767, nearest step  32768   <-- a whole LSB low
asked 0.24999999999999978 ->  16383, nearest step  16384   <-- a whole LSB low
asked 0.9999999999999996 ->  65535, nearest step  65536   <-- a whole LSB low
```

After (same build, this patch applied):

```
$ ports/unix/build-coverage/micropython rate_rounding.py
asked 0.499999 ->  32768, nearest step  32768
asked 0.24999999999999978 ->  16384, nearest step  16384
asked 0.9999999999999996 ->  65536, nearest step  65536
```

`Resampler`, bound 48000 into 44100 through an `audiomixer.Mixer`
(`edge_cases.py` in the phase-carry directory): 71331 before, 71332 after.

What the lost LSB does to audio, from `identity_pair.py` beside this file — a
441 Hz tone at 44.1 kHz through a SpeedChanger pair asked to cancel, N =
1.0000000000000004, which is what `fs / rate_hz` returns for a log-mapped
knob at the top of its travel:

```
stock 10.3.0        leg rates as stored: 65536 and 65535   worst |out - in|: 64000 codes
this patch only     leg rates as stored: 65536 and 65536   worst |out - in|:     0 codes
```

The same build with only this patch leaves `phase_carry.py` exactly as stock
reads it (507 / 411 / 426 frames wrong), so the two fixes are independent in
measurement as well as in text.

Upstream's own `tests/circuitpython` suite: 20 pass, 3 skip, 34 fail, the same
34 with and without the patch (synthio and traceback tests that fail on stock
`10.3.0` in this environment for unrelated reasons).

`tools/codeformat.py` leaves the changed files untouched; `codespell` with
upstream's `.codespellrc` is clean.

## Building audiospeed on the coverage port — not for posting

`CIRCUITPY_AUDIOSPEED ?= 0` and only `ports/raspberrypi` turns it on, so no
test configuration builds this module. To reproduce, add to
`ports/unix/variants/coverage/mpconfigvariant.mk` the six
`shared-bindings/audiospeed/*.c` and `shared-module/audiospeed/*.c` files and
`-DCIRCUITPY_AUDIOSPEED=1`, then build with `CFLAGS_EXTRA=-Wno-float-conversion`
— `shared-module/audiospeed/__init__.c:19` passes `0.001` to
`mp_arg_validate_obj_float_range`, whose bounds are `mp_int_t`, so the
coverage variant's warning set rejects it:

```
../../shared-module/audiospeed/__init__.c:19:65: error: conversion from 'double' to 'mp_int_t' {aka 'long int'} changes value from '1.0e-3' to '0' [-Werror=float-conversion]
```

That is a third (small) upstream finding: the documented lower bound of 0.001
is compiled as 0, and the module cannot be added to a test build until it is
resolved. It is why this PR carries no `tests/circuitpython` regression test.

## Publish commands (Brad runs these; nothing has been pushed)

```sh
git clone https://github.com/adafruit/circuitpython.git
cd circuitpython
git checkout -b fix-speedchanger-rate-rounding
git am /home/brad/gh/pydevices/audiodsp/docs/upstream-reports/prs/speedchanger-rate-rounding/0001-audiospeed-round-the-16.16-rate-instead-of-truncatin.patch

gh repo fork adafruit/circuitpython --remote --remote-name fork
git push fork fix-speedchanger-rate-rounding
gh pr create --repo adafruit/circuitpython \
  --title "audiospeed: round the 16.16 rate instead of truncating" \
  --body-file <the Body section above, fences stripped>
```
