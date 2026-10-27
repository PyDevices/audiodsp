# Filter a sound


`audiofilters.Filter` (over `synthio.Biquad`) and `audiofilters.Phaser` are
ported CircuitPython and their recursions are integer. Both have states that
reproduce themselves: fed silence after a burst, they hold a constant for ever.
`audiobiquad` is the same two filters with `float` state and a flush of anything
below 1e-20, so a tail decays to zero and stays there — and its all-pass
feedback is not clamped to `0.1..0.9`, so zero is zero and a feedback-free
phaser's notches are true nulls. It is also the one to reach for below about
100 Hz, where the integer coefficients run out of resolution —
[which filter, and why](../how-it-works/upstream-diff.md#which-filter-to-reach-for).

```python
import audiobiquad, synthio

eq = audiobiquad.Biquad(mode=audiobiquad.PEAKING_EQ, frequency=3200,
                        Q=1.2, gain_db=-4.0, sample_rate=48000)
eq.play(source)

sweep = synthio.LFO(rate=0.4, scale=400.0, offset=900.0)
phase = audiobiquad.AllPass(stages=4, frequency=sweep, feedback=0.0,
                            mix=0.5, sample_rate=48000)
phase.play(eq)
```

Every constructor argument, what can change mid-stream, and the limits are in the [filters and effects reference](../reference/filters-and-effects.md#audiobiquad).
