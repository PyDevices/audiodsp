"""A block bigger than the Splitter's ring must arrive whole.

    splitter_overflow_probe.py

Runs unchanged on CPython, on desktop MicroPython and on a board.

audiodsp#87. `audiocore.get_buffer` takes no length, so a source hands back what
it has: a `RawSample` over a 9600-frame table returns all 9600 in one call. The
ring holds 8192, and writing the lot lapped every cursor including the cursor
of the tap about to read -- the first 1408 frames were destroyed before anyone
saw them, and the stream had a seam at frame 8192. Found by the effects board
runner, on a class that fed a whole table through a `Splitter`; re-blocking the
source hid it, which is why it survived every probe here (they all feed blocks
smaller than the ring).

The material is a per-frame ramp on purpose. A dropped or repeated frame then
reads as an index, so this reports *which* frame arrived where rather than
"the audio changed".

It is not in `verify_dsp.py`'s PROBES. That gate compares interpreters against
each other, and this is a property of one: every target must render the whole
block, and each of them says so on its own.
"""

import sys
from array import array

import audiocore
import audioroute

SAMPLE_RATE = 48000
# shared/audiodsp_splitter.h. If the ring's depth moves, this moves with it --
# what the probe needs is a block bigger than the ring, not the number 8192.
RING_FRAMES = 8192
FRAMES = RING_FRAMES + 1408

table = array("h")
for frame in range(FRAMES):
    value = frame - 15000
    table.append(value)
    table.append(value)

splitter = audioroute.Splitter(
    audiocore.RawSample(table, sample_rate=SAMPLE_RATE, channel_count=2),
    taps=2)
tap = splitter.tap(0)

out = array("h")
# Bounded by construction: a tap hands back at least one frame per call, so
# this is many times what is needed and a tap that stopped producing fails
# here rather than running for ever.
for _ in range(FRAMES):
    if len(out) >= FRAMES * 2:
        break
    out.extend(array("h", bytes(audiocore.get_buffer(tap)[1])))

print("ring %d frames, block %d frames" % (RING_FRAMES, FRAMES))
print("collected %d frames of %d" % (len(out) // 2, FRAMES))

if len(out) < FRAMES * 2:
    print("FAIL the tap stopped early")
    sys.exit(1)

first_bad = -1
for index in range(FRAMES):
    if out[index * 2] != index - 15000:
        first_bad = index
        break

if first_bad < 0:
    print("ok every frame arrived in order")
    sys.exit(0)

got = out[first_bad * 2] + 15000
print("FAIL frame %d is source frame %d, not %d -- %d frames lost"
      % (first_bad, got, first_bad, got - first_bad))
sys.exit(1)
