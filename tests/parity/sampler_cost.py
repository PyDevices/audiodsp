"""What a block of audiosampler costs, by voice count and by what each voice does.

    sampler_cost.py [voices ...]

Runs on CPython (with `_audiodsp` built) and MicroPython (desktop or board),
so one script times the desktop and a board alike. Every voice loops a
2,048-frame buffer at 48 kHz, stereo, 256-frame blocks, at a pitch of 1.06
so the interpolator works, and every voice is sounding. Three cases:

- plain: no filter, no LFO -- the path a voice took before any of these
  existed.
- filter: a 2 kHz low-pass with 6 dB of resonance, nothing moving it.
- full: the filter plus the modulation envelope on cutoff and pitch, the
  vibrato LFO and the modulation LFO on pitch, cutoff and volume.

It prints the best of five runs of 200 blocks, in microseconds per block,
beside the block's own length.
"""

import sys
from array import array

import audiocore
import audiosampler

try:
    from time import ticks_diff, ticks_us

    def now():
        return ticks_us()

    def elapsed(a, b):
        return ticks_diff(b, a)
except ImportError:
    from time import perf_counter

    def now():
        return perf_counter()

    def elapsed(a, b):
        return (b - a) * 1e6

RATE = 48000
BLOCKS = 200
LENGTH = 2048

CASES = {
    "plain": {},
    "filter": {"cutoff": 2000.0, "resonance": 6.0},
    "full": {"cutoff": 2000.0, "resonance": 6.0, "mod_attack": 0.01,
             "mod_decay": 0.5, "mod_sustain": 0.3, "mod_to_pitch": 50.0,
             "mod_to_cutoff": 2400.0, "vibrato_rate": 5.0,
             "vibrato_depth": 20.0, "lfo_rate": 3.0, "lfo_to_pitch": 10.0,
             "lfo_to_cutoff": 600.0, "lfo_to_volume": 3.0},
}


def tone():
    # A sum of three integer triangles, so it has partials for a filter to
    # take, and no math module is needed.
    data = array("h", [0] * LENGTH)
    for i in range(LENGTH):
        a = (i * 7) % 256
        b = (i * 31) % 128
        c = (i * 3) % 512
        data[i] = (abs(a - 128) - 64) * 60 + (abs(b - 64) - 32) * 50 + (
            abs(c - 256) - 128) * 20
    return data


def cost(voices, case, data):
    sampler = audiosampler.Sampler(voice_count=voices, channel_count=2,
                                   sample_rate=RATE)
    for v in range(voices):
        sampler.voice[v].play(data, pitch=1.06, loop_start=0,
                              loop_end=LENGTH,
                              loop_mode=audiosampler.LOOP_FOREVER,
                              level=0.02, **CASES[case])
    for _ in range(4):
        audiocore.get_buffer(sampler)
    best = None
    for _ in range(5):
        start = now()
        for _ in range(BLOCKS):
            audiocore.get_buffer(sampler)
        us = elapsed(start, now()) / BLOCKS
        if best is None or us < best:
            best = us
    sampler.deinit()
    return best


def main():
    counts = [int(a) for a in sys.argv[1:]] or [32, 48, 64]
    data = tone()
    print("block %d us (256 frames at %d Hz)" % (256 * 1000000 // RATE, RATE))
    for voices in counts:
        row = []
        for case in ("plain", "filter", "full"):
            try:
                row.append("%s %.0f" % (case, cost(voices, case, data)))
            except TypeError:  # an audiosampler from before the filter
                row.append("%s -" % case)
        print("%3d voices: %s us" % (voices, ", ".join(row)))


main()
