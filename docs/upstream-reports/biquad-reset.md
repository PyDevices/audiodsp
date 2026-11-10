# Draft: biquad filter reset clears half its state

**Note to the poster — strip everything above the `---`.**

Also a one-liner, also independent of the other two, and arguably the most
audible of the three on a real board: a filter that has been reset plays a
decaying burst of the *previous* audio out of silence, at −1.3 dBFS.
Suggested title:

> `synthio_biquad_filter_reset()` only clears half the filter state

Verified present on `main` 2026-08-27. The fix was verified by applying it to
this port's copy of the same function: 28072 → 0, with every one of our
parity fixtures still matching its recorded hash — so it is a small change in
practice as well as in diff.

---

## `synthio_biquad_filter_reset()` only clears half the filter state

`shared-module/synthio/Biquad.h`:

```c
typedef struct {
    int32_t x[2], y[2];
} biquad_filter_state;
```

`shared-module/synthio/Biquad.c`:

```c
void synthio_biquad_filter_reset(biquad_filter_state *st) {
    memset(&st->x, 0, 4 * sizeof(int16_t));
}
```

The struct is four `int32_t`, so 16 bytes. `4 * sizeof(int16_t)` is 8. The
memset clears `x[0]` and `x[1]` and stops; **`y[0]` and `y[1]` keep the
previous output history.**

The `int16_t` looks like a leftover from a time when the state was 16-bit —
`biquad_filter_sample()` reads all four as `int32_t` now.

Both callers want a full reset:

- `shared-module/audiofilters/Filter.c`, `audiofilters_filter_reset_buffer()`,
  which the audio output calls when playback starts.
- `shared-module/synthio/Note.c`, when a note's filter is (re)initialised.

A biquad's `y` history is its feedback memory, so what survives is not a
detail — the recursion keeps running on it. After a reset, feeding the filter
pure silence produces a decaying tail of the audio that was there before, and
for a low-pass with poles near the unit circle that tail starts at nearly full
scale.

Practically: start a loud sound through an `audiofilters.Filter`, stop it,
start something quiet, and the first few milliseconds carry a burst of the old
sound. On a `synthio.Note`, a re-pressed note briefly inherits the last one's
filter tail.

### Fix

```diff
 void synthio_biquad_filter_reset(biquad_filter_state *st) {
-    memset(&st->x, 0, 4 * sizeof(int16_t));
+    memset(st, 0, sizeof(*st));
 }
```

### Measured

800 Hz low-pass, fed a 200 Hz tone at 30000 for four blocks, then
`reset_buffer`, then pure silence. Peak of the first block of "silence":

| | peak out |
|---|---|
| before | **28072** (−1.3 dBFS) |
| after | 0 |

First eight samples before the fix: `28082, 28041, 27761, 26946, 25968,
24859, 23648, 22362` — a clean exponential decay, which is the filter ringing
down from a state that should have been zeroed.

### Repro

```python
# audiofilters.Filter.reset_buffer() should leave no audio behind.
import array, math
import audiocore, audiofilters, synthio

RATE = 48000
FRAMES = 1024


def s16(data):
    out = []
    for index in range(0, len(data) - 1, 2):
        value = data[index] | (data[index + 1] << 8)
        out.append(value - 65536 if value >= 32768 else value)
    return out


loud = array.array("h")
for frame in range(FRAMES):
    loud.append(int(30000 * math.sin(2 * math.pi * 200 * frame / RATE)))
silence = array.array("h", [0] * FRAMES)

biquad = synthio.Biquad(synthio.FilterMode.LOW_PASS, 800.0)
node = audiofilters.Filter(filter=biquad, sample_rate=RATE, channel_count=1,
                           bits_per_sample=16, samples_signed=True,
                           buffer_size=FRAMES * 2)

node.play(audiocore.RawSample(loud, sample_rate=RATE, channel_count=1),
          loop=True)
for _ in range(4):
    audiocore.get_buffer(node)          # fill the filter's memory

node.play(audiocore.RawSample(silence, sample_rate=RATE, channel_count=1),
          loop=True)
audiocore.reset_buffer(node)            # what an AudioOut does on play()
first = s16(bytes(audiocore.get_buffer(node)[1]))
print("silence in, after reset_buffer: first 8 samples", first[:8])
print("peak of the block:", max(abs(value) for value in first))
```

```
silence in, after reset_buffer: first 8 samples [28082, 28041, 27761, 26946, 25968, 24859, 23648, 22362]
peak of the block: 28082
```

`audiocore.get_buffer` / `audiocore.reset_buffer` are not in upstream; on a
board the same thing happens through an `AudioOut`, since starting playback is
what calls `reset_buffer` on the chain.

Note that `common_hal_audiofilters_filter_play()` does *not* call
`audiofilters_filter_reset_buffer()` — it resets the source only — so a plain
`filter.play(other_sample)` carries the filter memory over by design. The bug
is on the path that does mean to clear it.
