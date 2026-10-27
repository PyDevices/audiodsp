# Change what plays without a click


Swapping one graph for another mid-waveform is a seam you can hear, and a
change that holds the pump for longer than its ring lets the speaker run dry,
which cuts the sound off and clicks again when it comes back. Both calls that
change what plays can fade instead:

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
