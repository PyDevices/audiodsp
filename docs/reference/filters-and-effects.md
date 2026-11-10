# Filters and effects

What the added filters and effects take and return. For a task-shaped
introduction to the biquad, see [Filter a sound](../guides/filter-a-sound.md).
The reasons these exist beside CircuitPython's own modules are in
[Additions beyond CircuitPython](../how-it-works/additions.md).

A level moved on a playing node walks to its new value in a straight line
over 20 ms instead of stepping, so turning it in one call doesn't click: the
`mix` of `Biquad`, `AllPass`, `Waveshaper`, `FeedbackDelay` and
`audiomath.Multiply`, the Waveshaper's `post_gain` and the Dynamics node's
`makeup_db`. A node that
hasn't played yet, or has just been reset, takes its levels at once.

## `audiobiquad`

What the two kinds of node take, side by side:

| | `Biquad` | `AllPass` |
|---|---|---|
| constructor | `mode`, `frequency`, `Q`, `gain_db`, `mix`, `sample_rate`, `channel_count` | `stages`, `frequency`, `feedback`, `mix`, `sample_rate`, `channel_count` |
| block inputs | `frequency`, `Q`, `gain_db`, `mix` | `frequency`, `feedback`, `mix` |
| fixed at construction | `sample_rate`, `channel_count` | those, plus `stages` (it sizes the state) |
| methods | `play`, `stop`, `clear` | `play`, `stop`, `clear` |
| read-only | `playing`, `coefficients` | `playing`, `stages`, `coefficient` |
| latency | 0 samples | 0 samples |

Generated from [`audiobiquad.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audiobiquad/audiobiquad.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audiobiquad
```

## `audiodynamics`

Generated from [`audiodynamics.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audiodynamics/audiodynamics.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audiodynamics
```

## `audioecho`

Generated from [`audioecho.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audioecho/audioecho.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audioecho
```

## `audioshaper`

Generated from [`audioshaper.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audioshaper/audioshaper.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audioshaper
```

## `audioladder`

Generated from [`audioladder.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audioladder/audioladder.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audioladder
```

## `audioverb`

Generated from [`audioverb.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audioverb/audioverb.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audioverb
```

## `audioroute.MidSide`

Generated from [`audioroute.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audioroute/audioroute.pyi); the file reads on its own.

```{eval-rst}
.. autoclass:: audioroute.MidSide
```

## `audiofilters`

Generated from [`audiofilters.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audiofilters/audiofilters.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audiofilters
```

## `audiodelays`

Generated from [`audiodelays.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audiodelays/audiodelays.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audiodelays
```

## `audiofreeverb`

Generated from [`audiofreeverb.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audiofreeverb/audiofreeverb.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audiofreeverb
```

## `audiospeed`

Generated from [`audiospeed.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audiospeed/audiospeed.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audiospeed
```

## `audioconvolve`

Generated from [`audioconvolve.pyi`](https://github.com/PyDevices/audiodsp/blob/main/src/audioconvolve/audioconvolve.pyi); the file reads on its own.

```{eval-rst}
.. automodule:: audioconvolve
```
