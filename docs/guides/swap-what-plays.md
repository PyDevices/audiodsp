# Change what plays without rewiring


A zero-copy pass-through you can re-point while it is playing. Its
`get_buffer` hands back its source's own pointer, length and result; its
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
