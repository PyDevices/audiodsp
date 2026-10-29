"""Deterministic audiofeatures slices.

    features_probe.py audiofeatures

`audiofeatures` is audiodsp's own, with no oracle: what this pins is that every
interpreter makes the same bytes for the same samples. The frontend and the
resampler are shared/audiodsp_features.c and the microfrontend it includes,
the same C everywhere, so a disagreement is the finding.

The signal is made with integers only (tone and noise bursts, as in
tests/test_cpython_audiofeatures.py), then held at 48 kHz stereo and 44.1 kHz
mono by repeating samples, which is integer too. Each case is fed in two block
sizes, and both must print the same.
"""

import sys
from array import array

ARGV = getattr(sys, "argv", [])     # a board has none
MODULE = ARGV[1] if len(ARGV) > 1 else "audiofeatures"
features_module = __import__(MODULE)

SIZE = 40


def signal(quarters=6):
    out = array("h")
    s = 2024
    seg = 4000
    plan = [(0, 0, 300), (7721, 9000, 300), (3861, 12000, 2000), (0, 0, 9000),
            (25736, 6000, 600), (38604, 3000, 4000)]
    for i in range(quarters):
        step, level, noise = plan[i % len(plan)]
        x, y = level << 8, 0
        for n in range(seg):
            env = n * 4 if n < seg // 4 else seg - (n - seg // 4) * 4 // 3
            env = max(0, min(seg, env))
            x -= (y * step) >> 16
            y += (x * step) >> 16
            s = (s * 1103515245 + 12345) & 0x7FFFFFFF
            r = ((s >> 15) & 0xFFFF) - 32768
            v = ((x >> 8) * env) // seg + r * noise // 32768
            out.append(max(-32768, min(32767, v)))
    return out


def hold(pcm, rate, channels):
    """pcm (16 kHz mono) at `rate`, each output sample the input one before it."""
    out = array("h")
    for i in range(len(pcm) * rate // 16000):
        v = pcm[i * 16000 // rate]
        for _ in range(channels):
            out.append(v)
    return out


def run(pcm, rate, channels, block):
    fe = features_module.Frontend(slices=64)
    raw = array("H", bytes(2 * SIZE * 64))
    got = array("H")
    mv = memoryview(pcm)
    step = block * channels
    for at in range(0, len(pcm), step):
        fe.feed(mv[at:at + step], rate, channels)
        n = fe.readinto_raw(raw)
        got.extend(raw[:n * SIZE])
    dropped = fe.stats()["dropped"]
    fe.deinit()
    return got, dropped


def main():
    base = signal()
    cases = (
        ("16k_mono", base, 16000, 1),
        ("48k_stereo", hold(base, 48000, 2), 48000, 2),
        ("44k1_mono", hold(base, 44100, 1), 44100, 1),
    )
    for name, pcm, rate, channels in cases:
        a, dropped = run(pcm, rate, channels, 480)
        b, _ = run(pcm, rate, channels, 37)
        h = 2166136261
        for v in a:
            h = ((h ^ v) * 16777619) & 0xFFFFFFFF
        slices = len(a) // SIZE
        print("%s slices %d dropped %d fnv %08x" % (name, slices, dropped, h))
        for k in range(0, slices, 25):
            print("%s %d %s" % (name, k, " ".join("%d" % v for v in a[k * SIZE:(k + 1) * SIZE])))
        print("%s blocks %s" % (name, "same" if a == b else "DIFFERENT"))
    print("quantize", " ".join("%d" % features_module.quantize(v) for v in (0, 1, 332, 333, 334, 665, 666, 670, 65535)))


main()
