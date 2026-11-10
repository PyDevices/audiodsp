# Filters and effects

What the added filters and effects take and return. For a task-shaped
introduction to the biquad, see [Filter a sound](../guides/filter-a-sound.md).
The reasons these exist beside CircuitPython's own modules are in
[Additions beyond CircuitPython](../how-it-works/additions.md).

A level moved on a playing node walks to its new value in a straight line
over 20 ms instead of stepping, so turning it in one call doesn't click: the
`mix` of `Biquad`, `AllPass`, `Waveshaper` and `FeedbackDelay`, the
Waveshaper's `post_gain` and the Dynamics node's `makeup_db`. A node that
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

```{eval-rst}
.. automodule:: audiobiquad
```

## `audiodynamics`

```{eval-rst}
.. automodule:: audiodynamics
```

## `audioecho`

```{eval-rst}
.. automodule:: audioecho
```

## `audioshaper`

```{eval-rst}
.. automodule:: audioshaper
```

## `audioladder`

```{eval-rst}
.. automodule:: audioladder
```

## `audioverb`

```{eval-rst}
.. automodule:: audioverb
```

## `audioroute.MidSide`

```{eval-rst}
.. autoclass:: audioroute.MidSide
```

## `audiofilters`

```{eval-rst}
.. automodule:: audiofilters
```

## `audiodelays`

```{eval-rst}
.. automodule:: audiodelays
```

## `audiofreeverb`

```{eval-rst}
.. automodule:: audiofreeverb
```

## `audiospeed`

```{eval-rst}
.. automodule:: audiospeed
```

## `audioconvolve`

```{eval-rst}
.. automodule:: audioconvolve
```
