"""Deterministic Echo.filter PCM.

    echo_filter_probe.py audiodelays

The first case sets no filter. Its bytes have to be the bytes Echo produced
before the property existed: an empty chain returns the word unchanged.

`decay` goes through `audioif_util.float32` because the node keeps it as an
`mp_float_t`: 0.7 written plainly is a different number on a single-precision
build, and the un-filtered case diverged on that alone (audioif#80).

**What that does not fix, and cannot from here.** The three cases that attach
a `synthio.Biquad` still diverge on a single-precision MicroPython, and the
cause is below this file: `src/audiodelays/Echo.c:389` computes its per-sample
`echo * decay + sample_word` in `mp_float_t` while the CPython twin's filtered
path (`src/cpython/audiodelays.py`) computes it in Python double. Rounding
those two expressions to float32 in the twin lands it on the float build's
bytes exactly, which is the measurement. audioif#102.
"""

import sys
from array import array

import audiocore
import synthio
from audioif_util import float32

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiodelays"
delays = __import__(MODULE)

SAMPLE_RATE = 8000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames=1600, level=14000, channels=2):
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            if frame < 120:
                shape = ((frame * (97 + channel * 18)) % 2001) - 1000
                values.append(shape * level // 1000)
            else:
                values.append(0)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("ecf", tag, index, len(data), sum(data), checksum(data))


echo = delays.Echo(
    max_delay_ms=80, delay_ms=40, decay=float32(0.7), mix=1.0, freq_shift=False,
    sample_rate=SAMPLE_RATE, channel_count=2, buffer_size=512)
echo.play(source())
emit("bare", echo, 8)

low = synthio.Biquad(synthio.FilterMode.LOW_PASS, 800, 0.7)
echo = delays.Echo(
    max_delay_ms=80, delay_ms=40, decay=float32(0.7), mix=1.0, freq_shift=False,
    filter=low, sample_rate=SAMPLE_RATE, channel_count=2, buffer_size=512)
echo.play(source())
emit("low", echo, 8)

high = synthio.Biquad(synthio.FilterMode.HIGH_PASS, 1200, 0.9)
echo = delays.Echo(
    max_delay_ms=80, delay_ms=40, decay=float32(0.85), mix=1.0, freq_shift=False,
    filter=high, sample_rate=SAMPLE_RATE, channel_count=2, buffer_size=512)
echo.play(source())
emit("high", echo, 6)

echo = delays.Echo(
    max_delay_ms=80, delay_ms=40, decay=float32(0.7), mix=1.0, freq_shift=True,
    filter=synthio.Biquad(synthio.FilterMode.LOW_PASS, 600, 0.8),
    sample_rate=SAMPLE_RATE, channel_count=2, buffer_size=512)
echo.play(source())
emit("shift", echo, 6)

echo = delays.Echo(
    max_delay_ms=80, delay_ms=40, decay=float32(0.7), mix=1.0, freq_shift=False,
    sample_rate=SAMPLE_RATE, channel_count=2, buffer_size=512)
echo.play(source())
emit("late-a", echo, 3)
echo.filter = synthio.Biquad(synthio.FilterMode.LOW_PASS, 500, 0.7)
emit("late-b", echo, 3)
