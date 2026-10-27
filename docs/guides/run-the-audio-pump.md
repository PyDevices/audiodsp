# Run audio off the interpreter thread

`audiopump` runs the same pull a player runs, in C, on a thread the
interpreter is not on: a task pinned to the other core on esp32, a native
thread on unix and Windows, and the calling thread on a port that has no
threads at all. A graph pulled that way keeps an exact clock while a screen
redraws, a USB stack runs and Python does whatever it likes.

The thread and the output it writes to come from a platform driver, which
this repository doesn't carry; the end of this section says where it comes
from. The example below is for a board whose driver has opened its output. On
a build without one, such as unix built from this repository alone,
`sink=True` raises `ValueError: no i2s sink open`, and the `service()` form
under **Which builds carry it** is the one to use.

```python
import audiopump, audiomixer, synthio

synth = synthio.Synthesizer(sample_rate=48000, channel_count=2)
mixer = audiomixer.Mixer(voice_count=1, sample_rate=48000,
                         channel_count=2, buffer_size=2048)
mixer.voice[0].play(synth)

status = bytearray(audiopump.STATUS_BYTES)
audiopump.spawn(mixer, 0x7FFFFFFF, status, sink=True)

synth.press(60)                # while it plays; no parking, no ceremony
mixer.voice[0].level = 0.6
audiopump.shutdown()           # and a soft reset does this for you
```

`spawn(sample, blocks, status, …)` starts it; `pull()` runs the same loop on
the calling thread, so `micropython.heap_lock()` around it is a real gate on
"the pull does not allocate"; `service()` advances it where there is no
thread; `retarget(sample, loop=…)` points it at a different tail;
`shutdown()` stops it. `status` is a `bytearray` of counters — blocks, bytes,
a digest of everything pulled, the worst block, what the sink clocked, the
error and the fault — read back with `struct.unpack`. Everything that can
refuse happens on the calling thread. The pull itself never raises, because
there is no interpreter on that thread to raise on, so it publishes a fault
code and stops.

What `status` holds, which builds carry the pump, and what each source can be are in the [`audiopump` reference](../reference/audiopump.md). To change what plays while the pump runs, see [Change what plays without a click](change-without-a-click.md).
