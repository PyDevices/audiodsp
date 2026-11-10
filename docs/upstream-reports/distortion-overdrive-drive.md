# Draft: `drive` does nothing in OVERDRIVE mode

**Note to the poster — strip everything above the `---`.**

The softest of the set, and the only one that might be intentional: the
OVERDRIVE curve is a fixed shape, so ignoring `drive` may be deliberate. But
nothing says so, `drive` is documented as the parameter that controls the
amount of distortion, and three of the four modes use it. So file it as
"either wire it up or document it", and be content with a docs fix.
Suggested title:

> `audiofilters.Distortion`: `drive` has no effect in OVERDRIVE mode

Do not lead with this one. It is worth having on record, but it is a
different kind of thing from the other reports and shouldn't set the tone.

Verified present on `main` 2026-08-27.

---

## `audiofilters.Distortion`: `drive` has no effect in OVERDRIVE mode

`shared-module/audiofilters/Distortion.c`:

```c
                            case DISTORTION_MODE_OVERDRIVE: {
                                wordf *= MICROPY_FLOAT_CONST(0.686306);
                                mp_float_t z = MICROPY_FLOAT_CONST(1.0) + MICROPY_FLOAT_C_FUN(exp)(MICROPY_FLOAT_C_FUN(sqrt)(MICROPY_FLOAT_C_FUN(fabs)(wordf)) * MICROPY_FLOAT_CONST(-0.75));
                                mp_float_t word_exp = MICROPY_FLOAT_C_FUN(exp)(wordf);
                                wordf *= MICROPY_FLOAT_CONST(-1.0);
                                wordf = (word_exp - MICROPY_FLOAT_C_FUN(exp)(wordf * z)) / (word_exp + MICROPY_FLOAT_C_FUN(exp)(wordf));
                            } break;
                            case DISTORTION_MODE_WAVESHAPE: {
                                wordf = (MICROPY_FLOAT_CONST(1.0) + drive) * wordf / (MICROPY_FLOAT_CONST(1.0) + drive * MICROPY_FLOAT_C_FUN(fabs)(wordf));
                            } break;
```

`drive` is never read in the OVERDRIVE branch. The curve is a fixed shape, so
every value of `drive` renders identically — the only way to push harder into
it is `pre_gain`.

The docstring for `drive` says it is "the amount of distortion" and does not
mention that one of the four modes ignores it, so as written the argument
looks connected and is not. CLIP and WAVESHAPE both use it, and LOFI's effect
is the bit masking rather than the curve.

Two ways to close this, and the choice is yours:

- **Wire it up**, e.g. by folding `drive` into the pre-gain applied before the
  curve, which is what a user reaching for that knob is trying to do. This
  changes existing renders for anyone who set `drive` in OVERDRIVE mode and
  heard nothing.
- **Document it** — say in the `drive` docstring and in `DistortionMode` that
  OVERDRIVE has a fixed curve and responds to `pre_gain` instead. No render
  moves.

We took the first option downstream (as pre-gain, with the level put back so
the historical default stays bit-identical), but a docs fix would have saved
us the measurement, and either is fine from out here.

### Repro

```python
import array, math
import audiocore, audiofilters

RATE = 8000
FRAMES = 512
tone = array.array("h", [int(20000 * math.sin(2 * math.pi * 100 * f / RATE))
                         for f in range(FRAMES)])


def render(mode, drive):
    node = audiofilters.Distortion(mode=mode, drive=drive, sample_rate=RATE,
                                   channel_count=1, bits_per_sample=16,
                                   samples_signed=True, buffer_size=FRAMES * 2)
    node.play(audiocore.RawSample(tone, sample_rate=RATE, channel_count=1),
              loop=True)
    audiocore.get_buffer(node)
    return bytes(audiocore.get_buffer(node)[1])


DM = audiofilters.DistortionMode
for name, mode in (("OVERDRIVE", DM.OVERDRIVE), ("WAVESHAPE", DM.WAVESHAPE)):
    outputs = [render(mode, d) for d in (0.0, 0.2, 0.5, 0.9)]
    same = all(o == outputs[0] for o in outputs)
    print("%-10s drive 0.0/0.2/0.5/0.9 -> %s"
          % (name, "identical output" if same else "output differs"))
```

```
OVERDRIVE  drive 0.0/0.2/0.5/0.9 -> identical output
WAVESHAPE  drive 0.0/0.2/0.5/0.9 -> output differs
```

`audiocore.get_buffer` is not in upstream; on a board, capture through an
`AudioOut` instead — the point is only that the four renders are the same
bytes.
