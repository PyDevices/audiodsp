"""Deterministic audiometer levels.

    meter_probe.py audiometer

`audiometer` is audiodsp's own, with no oracle: what this pins is that every
interpreter reads the same bytes for the same samples. The arithmetic is all in
shared/audiodsp_meter.c, the same C everywhere, and its window, filter, band
edges and dB rounding avoid libm on purpose, so a disagreement is the finding.

The signals are made with integers only. A sine from math.sin would differ
between a double-precision desktop and a single-precision board before the
meter saw it; a Minsky circle oscillator in fixed point is the same everywhere.
Each case is fed in two block sizes, and both must print the same.
"""

import sys
from array import array

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiometer"
meter_module = __import__(MODULE)

RATE = 48000


def tone(step, frames, level=12000):
    """A near-sine from the Minsky circle algorithm: step is 2*pi*f/rate in Q16."""
    x, y = level << 8, 0
    out = array("h")
    for _ in range(frames):
        x -= (y * step) >> 16
        y += (x * step) >> 16
        v = x >> 8
        out.append(v)
        out.append(v)
    return out


def white(frames, seed=12345, level=8000):
    out = array("h")
    s = seed
    for _ in range(frames):
        s = (s * 1103515245 + 12345) & 0x7FFFFFFF
        v = ((s >> 15) & 0xFFFF) - 32768
        v = v * level // 32768
        out.append(v)
        out.append(v)
    return out


def pink(frames, seed=99, rows=16, level=1500):
    """Voss-McCartney: row k changes every 2**k samples, the sum is pink."""
    out = array("h")
    s = seed
    vals = [0] * rows
    total = 0
    for i in range(frames):
        n = i + 1
        k = 0
        while k < rows - 1 and not (n >> k) & 1:
            k += 1
        s = (s * 1103515245 + 12345) & 0x7FFFFFFF
        r = ((s >> 16) & 0x7FFF) - 16384
        total += r - vals[k]
        vals[k] = r
        v = total * level // (16384 * 4)
        if v > 32767:
            v = 32767
        elif v < -32768:
            v = -32768
        out.append(v)
        out.append(v)
    return out


def run(name, pcm, bands, block):
    m = meter_module.Meter(bands, low_hz=35, high_hz=20000)
    frames = len(pcm) // 2
    mv = memoryview(pcm)
    seen = []
    at = 0
    while at < frames:
        n = min(block, frames - at)
        m.feed(mv[2 * at:2 * (at + n)], RATE, 2)
        at += n
        seq, lv, pk, rms = m.levels()
        if seq and (not seen or seen[-1][0] != seq):
            seen.append((seq, bytes(lv), pk, rms))
    m.deinit()
    # every 15th analysis, and the last: enough to catch a drift, short to read
    lines = []
    for seq, lv, pk, rms in seen:
        if seq % 15 == 0 or seq == seen[-1][0]:
            lines.append("%s %d %s %d %d" % (name, seq, " ".join("%d" % b for b in lv), pk, rms))
    return lines


def main():
    frames = RATE  # one second of each
    cases = (
        ("silence", array("h", bytes(4 * frames)), 24),
        ("tone_60", tone(514, frames), 32),       # 60 Hz, the decimated FFT
        ("tone_1k", tone(8579, frames), 32),      # 1 kHz
        ("tone_7k6", tone(65000, frames), 32),    # 7.6 kHz
        ("white", white(frames), 32),
        ("pink", pink(frames), 48),
    )
    for name, pcm, bands in cases:
        a = run(name, pcm, bands, 480)
        b = run(name, pcm, bands, 37)
        for line in a:
            print(line)
        print("%s blocks %s" % (name, "same" if a == b else "DIFFERENT"))
    print("db", " ".join("%d" % meter_module.db_byte(p) for p in (1.0, 0.5, 0.25, 1e-3, 1e-6, 1e-11)))


main()
