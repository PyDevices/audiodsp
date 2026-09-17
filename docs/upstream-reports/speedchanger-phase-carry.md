# Draft: `audiospeed` throws away its phase at every source buffer

**Note to the poster — strip everything above the `---`.**

The larger of the two `audiospeed` drafts, and the one with an audible symptom.
Independent of [speedchanger-rate-rounding.md](speedchanger-rate-rounding.md) —
different lines of the same file — but the repro reads better with that one
applied, because otherwise the rates in it are a hair off as well. Suggested
title:

> `audiospeed_fetch_source_buffer` resets the phase, so a `SpeedChanger` drifts
> and only renders correctly when the rate divides the buffer length

Verified present on `10.3.0` 2026-09-17 on a build of that tag with
`CIRCUITPY_AUDIOSPEED` on. The fix was verified by applying it to a port of the
same code: every measurement below goes to its ideal value, and the node's
output stops depending on the source's block size at all.

---

### `audiospeed_fetch_source_buffer` resets the phase, so a `SpeedChanger` drifts and only renders correctly when the rate divides the buffer length

`shared-module/audiospeed/__init__.c`:

```c
bool audiospeed_fetch_source_buffer(audiospeed_base_t *self) {
    ...
    self->src_sample_count = len / bytes_per_frame;
    self->source_done = (result == GET_BUFFER_DONE);
    // Reset phase to index within this new buffer
    audiospeed_reset_phase(&self->speed);
    return true;
}
```

and `audiospeed_get_buffer` assumes that reset a few lines later:

```c
            if (src_index >= self->src_sample_count) {
                ...
                if (!audiospeed_fetch_source_buffer(self)) {
                    break;
                }
                src_index = 0; // phase was reset by fetch
            }
```

The accumulator is in units of **source frames since the stream began**, not
since this buffer began, so the remainder it is holding when a buffer runs out
belongs to the next one. Zeroing it discards that remainder.

A buffer boundary is only ever the right place to resume from frame 0 when the
rate divides the buffer length exactly. At `rate` 1.8433 with a 256-frame
upstream, 0.2 of a frame is dropped every 139 output frames; at `rate` 6 with
the same upstream, four whole frames are dropped every 43.

#### What it sounds like

A `SpeedChanger` pair used as a sample-and-hold — decimate at N, restore at 1/N,
which is how a lo-fi rate reducer is built out of this module — is the clearest
case, because a hold has an exactly known answer.

Measured with a 256-frame source, everything else at defaults:

| | 10.3.0 | with the fix |
|---|---:|---:|
| the same hold over 64-, 100-, 256- and 1000-frame source buffers: frames differing from the 64-frame render, of 8192, N = 1.8433 | 8023, 8077, 8076 | 0, 0, 0 |
| frames differing from `source[(((n·up)>>16)·down)>>16]`, of 16384, at N = 1.8433 / 6.0 / 2.5 | 15657 / 16066 / 16090 | 0 / 0 / 0 |
| a 1 kHz image under a 7 kHz tone held at 8 kHz, where a zero-order hold puts it at −0.22 dB | −39.86 dB | −0.22 dB |
| worst miss against `20·log₁₀\|sinc(f·T)\|` from 500 Hz to 5 kHz, N = 1.8433 | 1.41 dB | 0.02 dB |
| a full-scale ramp of one code per frame: lag after 65536 frames at 48 / 44.1 kHz | 73 / 430 codes | 0 / 1 |

The first row is the property worth keeping: **what a `SpeedChanger` renders
should not depend on how the node above it happens to chunk its output**, and
today it depends on it almost entirely. The rest follow from it — the staircase
restarts 187 times a second at 48 kHz, so the held waveform is not the hold that
was asked for, its images are spread instead of placed, and the stream drifts
against everything playing beside it.

A rate that divides the buffer length is exempt, which is why this is easy to
miss: at N = 4 over 256-frame buffers, and at the 2.0 / 1.0 / 0.5 a `Resampler`
is usually bound to, the accumulator lands on the boundary every time and the
output is correct.

#### Fix

Subtract what the previous buffer held instead of zeroing, and let the caller
ask the accumulator where it is rather than assuming frame 0:

```diff
 bool audiospeed_fetch_source_buffer(audiospeed_base_t *self) {
     ...
-    self->src_sample_count = len / bytes_per_frame;
-    self->source_done = (result == GET_BUFFER_DONE);
-    // Reset phase to index within this new buffer
-    audiospeed_reset_phase(&self->speed);
+    // The frames just consumed are the ones the previous buffer held; the
+    // remainder belongs to this one.
+    audiospeed_consume_frames(&self->speed, self->src_sample_count);
+    self->src_sample_count = len / bytes_per_frame;
+    self->source_done = (result == GET_BUFFER_DONE);
     return true;
 }
```

with, in `__init__.h`:

```c
static inline void audiospeed_consume_frames(audiospeed_speed_t *self, uint32_t frames) {
    uint32_t consumed = frames << SPEED_SHIFT;
    self->phase = self->phase >= consumed ? self->phase - consumed : 0;
}
```

and, in both arms of `audiospeed_get_buffer`, pulling until the index lands
inside a buffer instead of assuming one pull is enough — at a rate above 1.0 the
carry can be several frames, and the next buffer is free to be shorter than
that:

```diff
-            uint32_t src_index = audiospeed_get_index(&self->speed);
-            if (src_index >= self->src_sample_count) {
+            while (audiospeed_get_index(&self->speed) >= self->src_sample_count) {
                 if (self->source_done) {
                     self->source_exhausted = true;
                     break;
                 }
                 if (!audiospeed_fetch_source_buffer(self)) {
                     break;
                 }
-                src_index = 0; // phase was reset by fetch
             }
+            if (audiospeed_get_index(&self->speed) >= self->src_sample_count) {
+                break;
+            }
+            uint32_t src_index = audiospeed_get_index(&self->speed);
```

One guard goes with it. `audiospeed_fetch_source_buffer` tests `len == 0`; with
the carry it needs `len < bytes_per_frame`, because a buffer holding no whole
frame can never advance the index. Today that case is an unconditional
`src_index = 0` against a zero-frame buffer, which reads off the end of it, so
the guard is worth having either way.

`audiospeed_reset_buffer` should keep zeroing the phase: it resets the source as
well, so the stream genuinely restarts there.

#### Repro

```python
# What a SpeedChanger renders should not depend on the block size of the node
# above it. Two identical holds, two different upstream buffer sizes.
import array, math
import audiocore, audiofilters, audiospeed

RATE, FRAMES, DOWN = 48000, 4096, 1.8433

values = array.array("h", bytes(FRAMES * 4))
for frame in range(FRAMES):
    value = int(round(16384 * math.sin(2 * math.pi * 100 * frame / RATE)))
    values[frame * 2] = values[frame * 2 + 1] = value


def hold(block):
    wire = audiofilters.Filter(sample_rate=RATE, channel_count=2,
                               buffer_size=block * 4)
    wire.play(audiocore.RawSample(values, sample_rate=RATE, channel_count=2))
    node = audiospeed.SpeedChanger(
        audiospeed.SpeedChanger(wire, DOWN), 1.0 / DOWN)
    out = array.array("h")
    while len(out) < FRAMES * 2:
        out.extend(array.array("h", bytes(audiocore.get_buffer(node)[1])))
    return [out[index * 2] for index in range(FRAMES // 2)]


small, large = hold(64), hold(256)
wrong = sum(1 for a, b in zip(small, large) if a != b)
print("%d of %d frames differ between a 64- and a 256-frame source"
      % (wrong, len(small)))
```

```
1947 of 2048 frames differ between a 64- and a 256-frame source
```

With the fix applied it prints `0 of 2048`.

`audiocore.get_buffer` is not upstream; on a board the same two graphs played
through an `AudioOut` differ audibly, since one of them is not the hold that was
asked for.
