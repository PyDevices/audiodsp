# Change what plays without a click

Swapping one graph for another mid-waveform is a seam you can hear, and a
change that holds the pump for longer than its ring lets the speaker run dry,
which cuts the sound off and clicks again when it comes back. You can swap the
whole graph the pump plays, with a fade, or swap one part of it through an
`audioroute.Port` without touching the rest.

## Swap the whole graph with a fade

Both `audiopump` calls that change what plays can fade instead:

```python
rack = build_the_next_patch()              # off the lock, as long as it takes
audiopump.retarget(rack.output, fade=True)  # old fades out, new fades in
old_rack.deinit()                           # safe: True means it is let go

audiopump.park(fade=True)   # hold the graph while you rewire it
rewire()                    # however long this takes, the speaker is fed
audiopump.unpark()
```

`retarget(tail, fade=True)` sends the current tail's next block out faded to
silence, swaps, and fades the new tail's first block in: one block each way,
5.3 ms at 256 frames and 48 kHz. A freshly built graph's first block is often
its slowest, and with the fade that lands in the gap rather than in a cut. It
returns True once the pump has stopped pulling the old tail, so you can
release it; False if it stopped waiting (the pump is parked, asleep on a full
ring, or `timeout_us`, 100 ms by default, ran out). The swap still happens on
the next block, and the old tail is kept until then.

`park(fade=True)` is a park with a deadline. The pump pulls one block ahead
and keeps it. If you `unpark()` before the speaker's queue is down to that
block, it goes out untouched and nothing is audible. If the queue gets there
first, the block goes out faded, the pump feeds the speaker silence for as
long as the park lasts, and the first block after `unpark()` fades in. It
needs a pump on a thread of its own and a ring, or a sink whose driver counts
what it clocked; where nothing can say how full the queue is, it fades at once.

`fades()` says what happened since `spawn()`: `(fades, clean holds, silence
bytes)`. The status block's digest is still the graph's own audio; only what
goes to the speaker, the ring and the tap is faded. Without `fade=`, both calls
behave exactly as before.

## Swap one part of the graph through a port

`audioroute.Port` is a zero-copy pass-through you can re-point while it is
playing. Its `get_buffer` hands back its source's own pointer, length and result; its
`play(source)` swaps that source atomically against a pull in flight. Hold a
port, wire it into a mixer voice or a rack, and change what is behind it as
often as you like — the object the consumer took never changes.

```python
import audioroute

port = audioroute.Port(overdrive)
mixer.voice[0].play(port)       # take it once

port.play(bitcrusher)           # heard on the next block; nothing rewires
port.source                     # what it is playing right now
```

To switch once the old source has had time to finish something, such as a
level walking down over 20 ms, give `after` a number of frames. The port
keeps playing what it plays for that many more frames and switches at the
start of the next pull. Until then `source` is still the old one, and a
`play()` replaces the waiting switch. A port nothing has pulled since it was
pointed at what it plays now switches at once, since that hasn't started.
Pointing it at the same source again doesn't count as a new start.

```python
mixer.voice[0].level = 0.0      # walks down over 960 frames
port.play(dry, after=960 + 256) # switches once the walk has landed
```

`Port` has no `stop()`, deliberately — a port always has a source. A port
pulled while it is already inside itself, which is what a component wrapping
its own output builds, publishes a loop fault and hands back an error
instead of recursing until the stack is gone.

Why it exists, and what its pull costs: [Additions beyond CircuitPython](../how-it-works/additions.md#why-audiorouteport-exists).
