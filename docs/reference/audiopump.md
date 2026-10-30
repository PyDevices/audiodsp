# audiopump

What `spawn()` takes, what `status` holds, which builds carry the pump and
what can feed it. To start one, see
[Run audio off the interpreter thread](../guides/run-the-audio-pump.md).

## `retarget` and `loop=`

`retarget`'s `loop=` travels **with** the swap rather than being stored when
you call it: the old tail is pulled until the next block boundary, so setting
the flag early ends the pump one block before the new tail ever runs. Leave it
out and the flag stays as `spawn()` set it, which is what every caller before
the argument existed wanted. Get it wrong the other way and a looping client
left alone on a live pump stops at the end of its lap.

## `backpressure()`

```python
>>> audiopump.backpressure()
True
```

True where a full output ring makes the pump **wait** for room instead of
dropping the block; False where a free-running pump would lose audio. It is
the question a driver asks before it decides whether to park the pump between
ticks, and it is True in two different ways: on a threaded port whose driver
fills in `park_spin`, where a wake ends the wait, and in service mode, where
the loop hands the thread back on a full ring and the caller's next
`service()` is the wake. It is False only on a threaded build with no
`park_spin` — a driver that is not finished, which now says so rather than
quietly losing blocks.

## `DriftResampler`

```python
drift = audiopump.DriftResampler(ring, *, sample_rate=None, target=None,
                                 max_ppm=1000, bandwidth=0.01)
```

Plays `ring`, which something else fills on its own clock, and steers its
ratio so the Ring holds `target` frames. Play the DriftResampler, not the
Ring: it is the Ring's only reader. On every runtime, CircuitPython included.

| Argument | Meaning |
| --- | --- |
| `sample_rate` | The output's rate: the Ring's own (the default) up to twice it. |
| `target` | The fill to hold, in frames. Half the Ring by default; at least a block, and at most the Ring less 16 frames, the filter's look-ahead. Settable while playing; the fill moves there at the loop's pace. |
| `max_ppm` | How far the ratio may move from nominal, 0 to 10000. |
| `bandwidth` | How fast the loop follows the clocks once locked, 0.001 to 0.1 Hz. It starts eight times faster and narrows over about a minute. |

The Ring's block must be 16 to 1024 frames. `ppm` (the ratio's offset from
nominal), `fill` (the smoothed fill it is holding) and `rate` (Ring frames per
output frame) say what the loop is doing; `underruns` counts blocks of
silence played because the Ring ran dry, after which it waits for `target`
again. `reset()` starts over and forgets the rate. Why it is built this way,
and what it measured: [the drift resampler](../how-it-works/drift-resampler.md).

## What feeds the pump

Three kinds of source feed it and they are all the same kind of thing to it:
a graph; `audiopump.Ring`, an audiosample node Python writes PCM into, so a
pushed stream can sit behind a Mixer with effects on it like anything else;
and `audiopump.Events`, a queue of frame-stamped presses, plays and levels
the pump applies at block boundaries, which is what puts a sequenced bar on
the audio's clock instead of the interpreter's. `audiopump.Tap` reads what is
going out, for a meter or a scope, without being in the path.

**Which builds carry it.** Every MicroPython port: unix, Windows,
WebAssembly and esp32. **The CPython wheel does too**, in the shape a port
with no thread has: `driver()` is `'none'`, `threaded()` is False, `spawn()`
returns −2 and adopts the graph, and nothing is pulled until you call
`service()`, which fills the ring you passed to `spawn()` and returns when it
is full. The names, the status block, `Ring`, `Events` and `Tap` are the same,
and so are the bytes: drained from the ring, a graph is byte-identical to the
same graph on the MicroPython unix build.

```python
status = bytearray(audiopump.STATUS_BYTES)
ring = bytearray(16384)
audiopump.spawn(mixer, 0x7FFFFFFF, status, ring=ring)
buf = bytearray(4096)
while playing:                 # from your app's timer, say every 20 ms
    audiopump.service()
    n = audiopump.drain(buf)
    sink.write(memoryview(buf)[:n])
```

`audiodev` does this for you: on CPython its players drive the pump from
their own tick. What CPython does not have is `pull()` under a heap lock,
because it has no heap lock, and audiocore's pull-depth cap: a graph that
leads back into itself is caught at Python's recursion limit instead, and
reported as the same fault. **CircuitPython carries only `Ring` and
`DriftResampler`**: its playback layer already pulls natively from its own
audio thread, and a second pull loop would be a second owner of the same
graph, so there a board's audio output pulls the `Ring` itself. It still takes the lock and the
hook table, on default hooks, which cost a load and a branch.

The hardware — the channel a board writes into, and a live microphone as a
source — is not here. It arrives as a separate module, `_audioif`, from a
platform driver, and `audiopump.driver()` tells you which one bound:

```python
>>> audiopump.driver()
'esp32'          # or 'pthread', 'win32', 'none'
```

`'none'` is a real answer rather than a failure: the default hook table is
all NULLs, which means one thread and no hardware, and that is exactly how
the WebAssembly build runs. Writing a driver for a platform that has none yet
is [the pump-port guide](../how-it-works/pump-ports.md).
