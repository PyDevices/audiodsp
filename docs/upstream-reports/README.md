# Drafts for adafruit/circuitpython

Three contributions are **prepared and unfiled**, waiting on Brad. Each has a
directory under [prs/](prs/) holding a patch that applies to the `10.3.0` tag
and to the `main` tip (`268a168d`, 2026-09-17 — the files involved are
byte-identical between the two), a paste-ready title and body, the repro
scripts, and the publish commands.

| prepared | what | shape of the fix |
|---|---|---|
| [prs/speedchanger-rate-rounding/](prs/speedchanger-rate-rounding/PR.md) | the 16.16 rate truncates, so `1/1.0000000000000004` is 65535/65536 and a `SpeedChanger` pair asked for unity is not one | one line, twice |
| [prs/speedchanger-phase-carry/](prs/speedchanger-phase-carry/PR.md) | the phase accumulator is zeroed at every source buffer, so what the node renders depends on the block size above it | a few lines |
| [prs/flanger-int32-overflow/](prs/flanger-int32-overflow/PR.md) | the Flanger's wet tap interpolates in int32 and overflows at the rails, where the neighbouring product is already widened | one cast |

The two `audiospeed` ones go as **separate PRs**: different lines of the same
file, each patch applies on its own and in either order, and the rate one is a
read-and-merge where the phase one wants five minutes of thought. Their drafts
([speedchanger-rate-rounding.md](speedchanger-rate-rounding.md),
[speedchanger-phase-carry.md](speedchanger-phase-carry.md)) are the long-form
versions and are **not** what gets posted.

One thing a maintainer may ask, answered in the rate-rounding PR.md:
`audiospeed` is built by no test configuration (`CIRCUITPY_AUDIOSPEED ?= 0`,
only `ports/raspberrypi` turns it on) and does not compile under the unix
coverage variant's warnings, because `mp_arg_validate_obj_float_range` takes
`mp_int_t` bounds and the module passes it `0.001`. That is why neither PR
carries a `tests/circuitpython` regression test, and it is a third small
finding of its own.

Six bugs this port found in CircuitPython, written up as issue bodies.
**Filed 2026-08-28** (all re-verified by inspection of `10.3.0-rc.0` first):

- peaking-eq-sign -> [#11265](https://github.com/adafruit/circuitpython/issues/11265) -> **PR [#11275](https://github.com/adafruit/circuitpython/pull/11275) — MERGED** (filed 2026-08-31)
- dds-oscillator-off-by-one -> [#11266](https://github.com/adafruit/circuitpython/issues/11266) -> **PR [#11276](https://github.com/adafruit/circuitpython/pull/11276) — MERGED** (filed 2026-08-31)
- distortion-soft-clip-union -> [#11267](https://github.com/adafruit/circuitpython/issues/11267) -> **PR [#11277](https://github.com/adafruit/circuitpython/pull/11277) — MERGED** (filed 2026-08-31)
- biquad-reset -> [#11268](https://github.com/adafruit/circuitpython/issues/11268) -> **PR [#11278](https://github.com/adafruit/circuitpython/pull/11278) — MERGED** (filed 2026-08-31)
- biquad-band-edges -> [#11269](https://github.com/adafruit/circuitpython/issues/11269) — reframed concisely per maintainer feedback and posted 2026-09-01 (three-option ask: full fix / config-gated / trig-only); awaiting maintainer appetite before any PR
- synthio-note-repress-zero-level -> no issue -> **PR [#11289](https://github.com/adafruit/circuitpython/pull/11289)** — opened 2026-09-02, straight to a PR per tannewt's preference for small fixes
- distortion-overdrive-drive -> **not filed; resolved upstream**: 10.3.0-rc.0's
  OVERDRIVE docstring now states drive has no effect in that mode, which is
  what the draft asked for.

Three are in `shared-module/synthio/Biquad.c`:

| draft | what | shape of the fix |
|---|---|---|
| [peaking-eq-sign.md](peaking-eq-sign.md) | `PEAKING_EQ` computes `b2` with the wrong sign, so a +6 dB bell is a +21 dB bass shelf | one character |
| [biquad-reset.md](biquad-reset.md) | `synthio_biquad_filter_reset()` clears half its struct, so a reset filter plays 1.3 dB of the previous audio out of silence | one line |
| [biquad-band-edges.md](biquad-band-edges.md) | Q15 coefficients and a shared sin/cos polynomial: nothing below ~300 Hz or above ~16 kHz is the filter that was asked for | a design decision, with a real MCU cost |

Three are elsewhere:

| draft | what | shape of the fix |
|---|---|---|
| [dds-oscillator-off-by-one.md](dds-oscillator-off-by-one.md) | the oscillator wraps one sample late and reads past the end of the waveform, so renders are not reproducible | one character |
| [distortion-soft-clip-union.md](distortion-soft-clip-union.md) | `Distortion(soft_clip=False)` turns soft clipping **on** — a bool read through the wrong union member | one line |
| [distortion-overdrive-drive.md](distortion-overdrive-drive.md) | `drive` is ignored in OVERDRIVE mode, and nothing says so | wire it up, or document it |

Each file opens with a short **note to the poster** (strip it before posting),
then a `---` and the issue body itself.

## Suggested order

The one-character and one-line fixes first, separately, so none of them waits
behind a discussion: **peaking-eq-sign**, **dds-oscillator-off-by-one**,
**distortion-soft-clip-union**, **biquad-reset**. Then **biquad-band-edges**,
which is a limitation report with a proposed fix rather than a patch, because
the M0+ cost is the maintainers' call. **distortion-overdrive-drive** last and
lightly — it may well be intentional, and a docs fix would settle it.

## Provenance of the numbers

Everything was measured **on a build of upstream `main`** — the CircuitPython
checkout in the parent workspace, which this repo treats as a read-only
oracle and never patches.

That matters for the biquad ones: `docs/upstream-diff.md` carries a different
"before" table for the same bugs, measured on *this port's* copy, which by
then already differed from upstream in ways that move the numbers. Do not
paste this repo's figures into an upstream issue; the ones here are
upstream's own.

`audiocore.get_buffer` **is upstream's**, not ours — corrected 2026-09-18 by
reading the file rather than the note. It is gated on
`CIRCUITPY_AUDIOCORE_DEBUG`, which the unix coverage variant defines, and
upstream's own `tests/testlib/audiofilterhelper.py` imports it to render the
`audiofilter_*` tests. What this port changes is only the memoryview it hands
back: a byte view, where upstream's is typed to the sample width. So a repro
that reads `get_buffer(node)[1]` runs upstream unaltered as long as it does
not assume the item size. `audiocore.reset_buffer` is ours.

## Before posting

- Re-check that each bug is still on `main`. All six were verified there on
  **2026-08-27**.
- Two bugs this port also carries are **not** in here and must not be filed:
  the stereo `Filter` sharing one biquad state, and `Mixer.reset_buffer`
  stopping its voices. Upstream fixed both after 10.2.1. See
  `docs/upstream-diff.md`.
