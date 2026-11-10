# Draft: the oscillator reads one sample past its waveform

**Note to the poster — strip everything above the `---`.**

One character, and the most consequential of the set: it makes renders
non-reproducible, because what gets read is whatever the allocator left after
the array. Suggested title:

> `synthio` oscillator wraps one sample late and reads past the end of the
> waveform

The repro below is deliberately built so the out-of-bounds read is *inside*
the buffer — a loop end short of the buffer end — so it is deterministic and
demonstrates the off-by-one without depending on undefined behaviour. The
paragraph after it explains why the common case (loop end == buffer end) is
the genuinely out-of-bounds one.

Verified present on `main` 2026-08-27 (four sites in
`shared-module/synthio/__init__.c`).

---

## `synthio` oscillator wraps one sample late and reads past the end of the waveform

`shared-module/synthio/__init__.c`, in `synthio_synth_synthesize()`:

```c
    uint32_t offset = waveform_start << SYNTHIO_FREQUENCY_SHIFT;
    uint32_t lim = waveform_length << SYNTHIO_FREQUENCY_SHIFT;
    ...
    for (uint16_t i = 0; i < dur; i++) {
        accum += dds_rate;
        // because dds_rate is low enough, the subtraction is guaranteed to go back into range, no expensive modulo needed
        if (accum > lim) {
            accum = accum - lim + offset;
        }
        int16_t idx = accum >> SYNTHIO_FREQUENCY_SHIFT;
        out_buffer32[i] = waveform[idx];
    }
```

`waveform_length` is an **exclusive** bound — the readable samples are
`[waveform_start, waveform_length)`. Wrapping on `>` rather than `>=` lets the
accumulator sit exactly on `lim`, and that iteration indexes
`waveform[waveform_length]`: one past the end of the loop, and for the common
case of a note looping a whole table, one past the end of the buffer itself.

The same two lines appear in the ring-modulator loop just below, with
`ring_waveform`.

This is not a rare edge. Any note whose `dds_rate` divides `lim` lands on the
boundary on a schedule — a table played at exactly one sample per output
sample hits it every `waveform_length` samples, and each voice hits it at its
own offset.

When the loop covers the whole buffer, the read is genuinely out of bounds and
returns whatever the allocator left there, so **the same script with the same
events does not render the same audio twice**. We found it downstream because
one drum-machine sequence rendered differently under `PYTHONMALLOC=default`,
`malloc` and `debug`, and changed again when unrelated objects were allocated
first.

### Fix

```diff
     for (uint16_t i = 0; i < dur; i++) {
         accum += dds_rate;
         // because dds_rate is low enough, the subtraction is guaranteed to go back into range, no expensive modulo needed
-        if (accum > lim) {
+        if (accum >= lim) {
             accum = accum - lim + offset;
         }
         int16_t idx = accum >> SYNTHIO_FREQUENCY_SHIFT;
         out_buffer32[i] = waveform[idx];
     }
```

and the same in the ring-modulator loop. The arithmetic in the body is already
right: `accum - lim + offset` is `accum - (lim - offset)`, which is the loop
span.

There is a smaller companion issue in the pre-loop reduction, in both places:

```c
    // can happen if note waveform gets set mid-note, but the expensive modulo is usually avoided
    if (accum > lim) {
        accum = accum % lim + offset;
    }
```

That reduces into `[0, lim)` and *then* adds `offset`, so with a non-zero
`waveform_loop_start` the result can land at or beyond `lim` and index out of
the loop on the very next sample. Reducing into `[offset, lim)` directly would
be the matching change.

### Repro

Deterministic, and stays inside the buffer, by making the loop end short of
the buffer end: the loop is 256 silent samples and sample 256 is a marker the
oscillator should never reach.

```python
import array, math
import audiocore, synthio

RATE = 8000
LOOP = 256

# 512 samples. The loop is [0, 256) and is silent; sample 256 is a marker the
# oscillator should never reach.
table = array.array("h", [0] * 512)
table[LOOP] = 30000

synth = synthio.Synthesizer(sample_rate=RATE, channel_count=1)
note = synthio.Note(frequency=RATE / LOOP,        # exactly one table step/sample
                    waveform=table,
                    waveform_loop_start=0,
                    waveform_loop_end=LOOP,
                    envelope=None)
synth.press(note)

out = []
for _ in range(4):
    data = bytes(audiocore.get_buffer(synth)[1])
    for index in range(0, len(data) - 1, 2):
        value = data[index] | (data[index + 1] << 8)
        out.append(value - 65536 if value >= 32768 else value)

hits = [i for i, v in enumerate(out) if abs(v) > 1000]
print("loop is [0, %d) and is all zeros; sample %d is the marker" % (LOOP, LOOP))
print("non-silent output samples:", hits[:8], "of", len(out))
print("values:", [out[i] for i in hits[:8]])
```

```
loop is [0, 256) and is all zeros; sample 256 is the marker
non-silent output samples: [255, 511, 767, 1023] of 1024
values: [14999, 14999, 14999, 14999]
```

The marker comes through once every 256 samples. With the one-character change
above:

```
non-silent output samples: [] of 1024
values: []
```

`audiocore.get_buffer` is not in upstream; on a board, play the synthesizer
into an `AudioOut` and capture instead — the marker is at −0.8 dBFS against
silence, so it survives any measurement.

Set `waveform_loop_end` to 512 instead of 256 and the same read walks off the
end of the array, which is the case that costs reproducibility.
