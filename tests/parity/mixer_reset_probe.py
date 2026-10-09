"""A host reset of a Mixer keeps its voices where they are.

    mixer_reset_probe.py audiomixer

The rule every node follows on a host reset (audiodsp#181): clear what the
node holds of its own and keep the source frames it has already taken, because
the source was not reset. A Mixer holds nothing of its own that a reset should
clear, so after a reset it plays on exactly as a Mixer that was never reset
does (audiodsp#178). It used to rewind each voice's source and fetch again:
a voice over a RawSample, which hands out its whole buffer at once, played
the sample from its top again, and a voice over a source that can't rewind,
such as a SplitterTap, lost the frames it was holding.

Each case pulls a Mixer for one block, resets it, and pulls three more, beside
the same Mixer never reset. The blocks must match, and they are printed so the
interpreters are held to the same bytes. CircuitPython's own Mixer stops every
voice on a reset (docs/upstream-diff.md, "Resetting a Mixer silenced it"), so
`verify_dsp.py` doesn't run this there.
"""

import sys
from array import array

import audiocore
import audioroute

mixers = __import__(sys.argv[1] if len(sys.argv) > 1 else "audiomixer")

RATE = 8000
BLOCK = 256


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


FAILED = []


def check(tag, ok):
    print("check", tag, "ok" if ok else "FAILED")
    if not ok:
        FAILED.append(tag)


def ramp(frames, channels):
    """Every frame differs from its neighbours, so a frame lost or played
    twice changes the bytes."""
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            values.append(((frame * (97 + 34 * channel)) % 4001) - 2000)
    return values


def raw_voice(channels):
    return audiocore.RawSample(ramp(BLOCK * 8, channels), sample_rate=RATE,
                               channel_count=channels)


def tap_voice(channels):
    source = audiocore.RawSample(ramp(BLOCK * 8, channels), sample_rate=RATE,
                                 channel_count=channels)
    return audioroute.Splitter(source, taps=1).tap(0)


def render(channels, make_source, reset, frames_per_pull):
    node = mixers.Mixer(voice_count=1,
                        buffer_size=frames_per_pull * 2 * channels * 2,
                        sample_rate=RATE, channel_count=channels)
    node.voice[0].level = 1.0
    keep = make_source(channels)
    node.voice[0].play(keep)
    out = [bytes(audiocore.get_buffer(node)[1])]
    if reset:
        audiocore.reset_buffer(node)
    out += [bytes(audiocore.get_buffer(node)[1]) for _ in range(3)]
    node.deinit()
    return out


for channels in (1, 2):
    for name, make in (("raw", raw_voice), ("tap", tap_voice)):
        # 100 frames a pull, so the voice is holding part of what it took
        # when the reset comes.
        for frames in (BLOCK, 100):
            tag = "%s-%dch-%d" % (name, channels, frames)
            plain = render(channels, make, False, frames)
            reset = render(channels, make, True, frames)
            print("mixer-reset", tag, [checksum(b) for b in reset])
            check(tag, reset == plain)

print("done mixer-reset")
if FAILED:
    raise SystemExit("failed: " + " ".join(FAILED))
