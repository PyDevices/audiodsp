# Play computed audio


CircuitPython's audio outputs play samples, and a sample is filled from C, so
CircuitPython has no built-in way to play a stream your Python code or a graph of
nodes renders as it goes. A `Ring` is a sample you write into. The output
pulls it in C, block by block, while your loop keeps it topped up:

```python
import audiobusio, board
from array import array
from audiopump import Ring

ring = Ring(sample_rate=22050, channel_count=1, frames=256, capacity=16)
block = array("h", bytes(512))           # 256 mono frames
while ring.space() >= len(block) * 2:    # fill it before play()
    ring.write(render(block))
out = audiobusio.I2SOut(board.I2S_BCLK, board.I2S_WCLK, board.I2S_DOUT)
out.play(ring)
while True:
    while ring.space() >= len(block) * 2:
        ring.write(render(block))
```

It plays through `audiobusio.I2SOut`, `audioio.AudioOut`,
`audiopwmio.PWMAudioOut` and an `audiomixer` voice like any other sample.
When your code falls behind, the output plays a block of silence rather than
stopping, and `ring.stats()[3]` counts those underruns; a write that doesn't
fit is short and returns the bytes it took. `capacity` is how many blocks the
ring holds, which is also its latency.

Size the ring for what your output takes at once, and fill it before
`play()`. An output that takes a large first gulp underruns at the start
however fast you write, so leave room beyond one block. Blocks of 256 frames
with `capacity=16` hold 186 ms at 22,050 Hz.

It is the same `audiopump.Ring` MicroPython's pump pulls, with the same
methods (`write`, `space`, `level`, `stats`, `clear`, `deinit`), so one
producer runs on both. On CircuitPython the module carries only `Ring`: the
board's output is the pump.

