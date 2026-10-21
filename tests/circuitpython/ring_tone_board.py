# A 441 Hz tone written from Python into audiopump.Ring and played by the
# board's audiobusio.I2SOut for 30 s, then the ring's counts. Copy it to a
# CircuitPython board with an I2S amplifier as code.py, or paste it at the
# REPL. A pass is `underruns=0`.
#
# Written for the LilyGO T-Embed (ESP32-S3, MAX98357A): its amplifier's rail
# is behind IO46. On another board, change the pins and drop the rail.

import array
import math
import time

import audiobusio
import board
import digitalio
from audiopump import Ring

rail = digitalio.DigitalInOut(board.IO46)
rail.switch_to_output(value=True)

RATE = 22050
FRAMES = 256
PERIOD = 50                                  # 22050 / 441
table = array.array("h", [int(9830 * math.sin(2 * math.pi * i / PERIOD)) for i in range(PERIOD * 16)])
table.extend(table[:FRAMES])                 # so any block-long slice is contiguous
mv = memoryview(table)
pos = 0

# ESP32's I2SOut takes about 3,000 frames when it starts and about 1,000 at a
# time after: 16 blocks of 256, filled before play(), keep ahead of it.
ring = Ring(sample_rate=RATE, channel_count=1, frames=FRAMES, capacity=16)
while ring.space() >= FRAMES * 2:
    ring.write(mv[pos:pos + FRAMES])
    pos = (pos + FRAMES) % (PERIOD * 16)

out = audiobusio.I2SOut(board.I2S_BCLK, board.I2S_WCLK, board.I2S_DOUT)
out.play(ring)
start = time.monotonic()
while time.monotonic() - start < 30:
    while ring.space() >= FRAMES * 2:
        ring.write(mv[pos:pos + FRAMES])
        pos = (pos + FRAMES) % (PERIOD * 16)
    time.sleep(0.002)
s = ring.stats()
out.stop()
out.deinit()
rail.deinit()
print("wrote=%d read=%d starved=%d underruns=%d overruns=%d pulls=%d" % s[:6])
