# PR for adafruit/circuitpython: the Flanger's int32 interpolation

Branch: `fix-flanger-int32-overflow` (one commit, the patch beside this
document). From audioif#76 and the `audiodelays.Flanger` section of
[upstream-diff.md](../../../upstream-diff.md). Verified against the `10.3.0` tag
(`d897c15f`) and the `main` tip (`268a168d`, 2026-09-17) —
`shared-module/audiodelays/Flanger.c` is byte-identical between the two.

**A PR, not an issue.** The judgement upstream would have to make — pay for a
64-bit multiply in the per-sample loop, or give up a bit of `delay_frac` — is
one they already made eighteen lines above, where `delay_span_q16 * tri` is
widened with exactly this cast. If a maintainer raises the Cortex-M0+ cost
(`__aeabi_lmul` per sample), the cheaper answer is
`s0 + ((s1 - s0) * (int32_t)(delay_frac >> 1) >> 15)`: 65535 × 32767 fits
int32 with 98 302 to spare, at the price of the bottom bit of the fractional
delay. Do not offer that unprompted; it costs a round trip to explain.

## Title

```
audiodelays: widen the Flanger's wet interpolation
```

## Body

```markdown
The wet tap's linear interpolation multiplies a tap difference of up to 65535
by a fractional delay of up to 65535 in `int32_t`, which overflows on
full-scale material that alternates sign: interpolating between two taps at
opposite rails returns -32770 where it should return 32766. On a rails render
1717 of 2048 samples differ from the widened arithmetic, worst case 32087
codes, while ordinary material is identical. The `delay_span_q16 * tri`
product just above already uses the same cast.
```

## Verification transcript (2026-09-18) — not for posting

Still present on the `main` tip, cited from the file upstream serves today
(`gh api .../contents/shared-module/audiodelays/Flanger.c?ref=main`):

```
shared-module_audiodelays_Flanger.c:347:                uint32_t delay_q16 = delay_min_q16 + (uint32_t)(((uint64_t)delay_span_q16 * tri) >> 16);
shared-module_audiodelays_Flanger.c:365:                int32_t wet = s0 + (((s1 - s0) * (int32_t)delay_frac) >> 16);
```

`flanger_overflow.py` beside this file, on the `10.3.0` unix coverage build.
The first line is the expression's own arithmetic, done once in Python (which
has no int32 to overflow) and once as C's int32 wraps it:

```
$ ports/unix/build-coverage/micropython flanger_overflow.py
taps -32768..32767 at frac 65535: int32 wet -32770, correct 32766
rails    first 8 output frames: [28520, -28521, 28520, -28521, 28520, -28521, 28520, -28521]
rails    sum 86409  min -32096  max 32095
ordinary first 8 output frames: [-6400, -6200, -6000, -5800, -5600, -5400, -5200, -5000]
ordinary sum -775787  min -17643  max 15725
```

The `rails` line moves with the patch (`sum -7305  min -32088  max 32072`);
the `ordinary` line does not.

Sample-by-sample, from `flanger_dump.py` run on both builds — 2048 frames of
each material, `max_delay_ms=10`, `min_delay_ms=1.0`, `rate=2.0`, `depth=1.0`,
`feedback=0.5`, `mix=1.0`, 8 kHz mono:

```
rails: 1717 of 2048 samples differ, worst 32087 codes
ordinary: 0 of 2048 samples differ, worst 0 codes
```

That is the signature the three-way `verify_dsp` run of 2026-09-09 saw from
the other side: 18 of 54 probe lines differed and every one of them was a
`rails` case.

`tools/codeformat.py` leaves the file untouched; `codespell` with upstream's
`.codespellrc` is clean. Upstream's `tests/circuitpython` suite is unchanged
by the patch (same 20 pass / 3 skip / 34 pre-existing failures).

## If Brad would rather ask than patch — not for posting as well as the body

Two sentences, as an issue instead:

```markdown
`Flanger.c:365` computes the wet tap as `s0 + (((s1 - s0) * (int32_t)delay_frac) >> 16)`, and both factors reach 65535, so the product overflows int32 on full-scale material that alternates sign — taps at opposite rails interpolate to -32770 instead of 32766, and 1717 of 2048 samples of a rails render differ from the same render with the multiply widened.

Happy to send the one-line `(int64_t)` cast, matching `delay_span_q16 * tri` eighteen lines above, or a `delay_frac >> 1` version that stays in int32 if the M0+ cost of a 64-bit multiply per sample matters more than the bottom bit of the fractional delay.
```

## Publish commands (Brad runs these; nothing has been pushed)

```sh
git clone https://github.com/adafruit/circuitpython.git
cd circuitpython
git checkout -b fix-flanger-int32-overflow
git am /home/brad/gh/pydevices/audioif/docs/upstream-reports/prs/flanger-int32-overflow/0001-audiodelays-widen-the-Flanger-s-wet-interpolation.patch

gh repo fork adafruit/circuitpython --remote --remote-name fork
git push fork fix-flanger-int32-overflow
gh pr create --repo adafruit/circuitpython \
  --title "audiodelays: widen the Flanger's wet interpolation" \
  --body-file <the Body section above, fences stripped>
```
