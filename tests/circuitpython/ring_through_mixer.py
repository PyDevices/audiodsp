# audiopump.Ring pulled through audiomixer, checked sample by sample.
#
# Run it with a CircuitPython unix build that has audiodsp in it:
#
#     circuitpython tests/circuitpython/ring_through_mixer.py
#
# It prints one line per check and "ALL PASS" last, and exits non-zero on the
# first failure. The same file runs on audiodsp's MicroPython unix build,
# whose Mixer and audiocore are CircuitPython's, so one producer is checked on
# both.
#
# What it would catch: channels swapped, a block dropped or repeated, an
# underrun that played silence without counting it (or counted it without
# playing silence), a short write that took a partial frame, a clear() that
# left old audio playing, and a position that jumps when the ring wraps.

import sys

import audiocore
import audiomixer
from array import array
from audiopump import Ring

checks = 0


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        print("FAIL", what)
        sys.exit(1)


def left(f):
    # Never zero, so silence can't pass for audio; never the right channel's
    # value, so a swap can't pass either.
    return 1000 + (f * 37) % 20000


def right(f):
    return -left(f) - 7


def frames_of(start, count, channels):
    a = array("h", bytes(count * channels * 2))
    for i in range(count):
        f = start + i
        if channels == 2:
            a[2 * i] = left(f)
            a[2 * i + 1] = right(f)
        else:
            a[i] = left(f)
    return a


def get(sample):
    """One pull, as signed 16-bit samples. CircuitPython's audiocore hands back
    a memoryview of samples and MicroPython's one of bytes, so go by bytes."""
    result, buf = audiocore.get_buffer(sample)
    check(result != 2, "get_buffer returned an error")
    return array("h", bytes(buf))


def pull(sample, frames, channels):
    """Pull at least `frames` frames out of `sample` and return them."""
    got = array("h")
    while len(got) < frames * channels:
        got.extend(get(sample))
    return got


def near(a, b):
    # A stereo Mixer at centre pan scales by 32767/32768, so a sample can come
    # out one below what went in. Nothing this file catches is that small.
    return a - b <= 1 and b - a <= 1


def split(got, channels, out_channels):
    """(non-silent frames as (l, r) tuples, number of silent frames)."""
    audio = []
    silent = 0
    for i in range(0, len(got), out_channels):
        frame = tuple(got[i:i + out_channels])
        if all(s == 0 for s in frame):
            silent += 1
        else:
            audio.append(frame)
    return audio, silent


def matches(audio, start, channels, out_channels):
    for i, frame in enumerate(audio):
        f = start + i
        want_l = left(f)
        want_r = right(f) if channels == 2 else want_l
        if not near(frame[0], want_l):
            return "frame %d: left %d, want %d" % (f, frame[0], want_l)
        if out_channels == 2 and not near(frame[1], want_r):
            return "frame %d: right %d, want %d" % (f, frame[1], want_r)
    return None


RATE = 22050


# --- 1. the ring by itself: bit-exact, digests agree ------------------------

ring = Ring(sample_rate=RATE, channel_count=2, frames=64, capacity=4)
check(ring.sample_rate == RATE and ring.channel_count == 2 and ring.bits_per_sample == 16,
      "audiosample fields")
check(ring.space() == 64 * 4 * 4 and ring.level() == 0, "empty ring's space and level")
data = frames_of(0, 64 * 3, 2)
check(ring.write(data) == len(data) * 2, "a write that fits takes it all")
check(ring.level() == 64 * 3 * 4, "level after write")
got = pull(ring, 64 * 3, 2)
check(got == data, "the ring hands out exactly what was written")
s = ring.stats()
check(s[0] == s[1] == 64 * 3 * 4 and s[6] == s[7], "wrote == read and the digests agree")
check(s[3] == 0 and s[5] == 3, "no underruns in three pulls")
print("ok  ring alone: bit-exact, digests agree")

# Underrun: a whole block of silence, counted, and the partial block kept.
ring.write(frames_of(1000, 40, 2))
buf = get(ring)
check(len(buf) == 64 * 2 and all(v == 0 for v in buf), "underrun plays a block of silence")
check(ring.stats()[3] == 1 and ring.stats()[2] == 64 * 4, "underrun is counted, and so is its silence")
check(ring.level() == 40 * 4, "underrun keeps the partial block")
ring.write(frames_of(1040, 24, 2))
check(pull(ring, 64, 2) == frames_of(1000, 64, 2), "the kept frames play once the block is whole")
print("ok  underrun: silence, counted, nothing lost")

# Overrun: whole frames only, short, counted.
ring = Ring(sample_rate=RATE, channel_count=2, frames=16, capacity=2)
took = ring.write(bytes(16 * 2 * 4 + 6))
check(took == 16 * 2 * 4, "a write past capacity takes what fits")
check(ring.stats()[4] == 1, "an overrun is counted")
ring.clear()
check(ring.space() == 16 * 2 * 4, "clear() frees the room at once")
check(ring.write(bytes(6)) == 4, "a write takes whole frames only")
print("ok  overrun and clear: short write, counted, whole frames")

# clear() drops what was queued: the next pull is silence, not old audio.
ring = Ring(sample_rate=RATE, channel_count=2, frames=16, capacity=4)
ring.write(frames_of(0, 48, 2))
ring.clear()
buf = get(ring)
check(all(v == 0 for v in buf), "clear() dropped the queued audio")
ring.write(frames_of(500, 16, 2))
check(pull(ring, 16, 2) == frames_of(500, 16, 2), "audio written after clear() plays")
print("ok  clear: old audio gone, new audio plays")

# Wrap: a capacity that does not divide the block counts, many times round.
ring = Ring(sample_rate=RATE, channel_count=2, frames=48, capacity=3)
f = 0
out = 0
for _ in range(200):
    n = ring.space() // 4
    if n:
        n = min(n, 37)
        ring.write(frames_of(f, n, 2))
        f += n
    if ring.level() >= 48 * 4:
        got = pull(ring, 48, 2)
        check(got == frames_of(out, 48, 2), "wrap: block at frame %d" % out)
        out += 48
check(ring.stats()[3] == 0 and out > 48 * 3 * 20, "wrap: round the ring many times, no underruns")
print("ok  wrap: %d frames round a %d-frame ring" % (out, 48 * 3))


# --- 2. through audiomixer ---------------------------------------------------

def through_mixer(channels, out_channels, block, mixer_frames, blocks, starve_at):
    """Feed `blocks` blocks through a Mixer, keeping the ring topped up, except
    that at block `starve_at` it is left to run dry. Returns the ring, what
    came out, the frames written, and the underruns before the tail."""
    ring = Ring(sample_rate=RATE, channel_count=channels, frames=block, capacity=4)
    mixer = audiomixer.Mixer(voice_count=1, sample_rate=RATE, channel_count=out_channels,
                             bits_per_sample=16, samples_signed=True,
                             buffer_size=mixer_frames * out_channels * 2)
    block_bytes = block * channels * 2
    # A voice starts at level 0 and comes up to its level at the first zero
    # crossing, or at the end of the Mixer's first buffer if there is none, so
    # a stream that never crosses zero would lose that buffer. Audio starts
    # from silence: one silent frame leads in, as a real signal's would.
    ring.write(bytes(channels * 2))
    f = 0
    ring.write(frames_of(f, block * 2 - 1, channels))
    f += block * 2 - 1
    written = 2
    mixer.voice[0].play(ring)
    got = array("h")
    while written < blocks:
        if written == starve_at:
            starve_at = -1
            while ring.level():
                got.extend(pull(mixer, mixer_frames, out_channels))
            got.extend(pull(mixer, block * 2, out_channels))
        while written < blocks and ring.space() >= block_bytes:
            ring.write(frames_of(f, block, channels))
            f += block
            written += 1
        got.extend(pull(mixer, mixer_frames, out_channels))
    underruns = ring.stats()[3]
    # The tail: everything still queued, and what the voice still holds.
    while ring.level():
        got.extend(pull(mixer, mixer_frames, out_channels))
    got.extend(pull(mixer, block * 2, out_channels))
    return ring, got, f, underruns


for channels, out_channels, block, mixer_frames in (
        (2, 2, 64, 64),     # the same block on both sides
        (2, 2, 96, 40),     # a mixer buffer that splits ring blocks
        (2, 2, 32, 100),    # a mixer buffer that spans several
        (1, 1, 64, 50),     # mono
        (1, 2, 64, 64)):    # mono ring into a stereo mixer
    label = "%dch ring, %dch mixer, block %d, mixer %d" % (channels, out_channels, block, mixer_frames)
    ring, got, written, underruns = through_mixer(channels, out_channels, block, mixer_frames, 40, 20)
    audio, silent = split(got, channels, out_channels)
    s = ring.stats()
    check(len(audio) == written, "%s: %d frames came out, %d went in" % (label, len(audio), written))
    bad = matches(audio, 0, channels, out_channels)
    check(bad is None, "%s: %s" % (label, bad))
    check(s[0] == s[1] and s[6] == s[7], "%s: every byte written was played, in order" % label)
    check(underruns >= 1, "%s: the starved stretch counted underruns" % label)
    check(s[2] == s[3] * block * channels * 2, "%s: silence counted as whole blocks" % label)
    # The voice never stops (a ring never says it is done), so every frame the
    # Mixer handed out came from the ring: the lead-in, the ring's silence, and
    # the audio. The ring's silence is all out but for what the voice still
    # holds of its last block, which is less than a block.
    ring_silent = s[2] // (channels * 2)
    check(ring_silent - block < silent - 1 <= ring_silent,
          "%s: %d silent frames out, the ring counted %d" % (label, silent - 1, ring_silent))
    print("ok  mixer: %s, %d frames, %d underruns while starved" % (label, written, underruns))

# An unbroken stream, kept ahead of the mixer, never underruns.
ring, got, written, underruns = through_mixer(2, 2, 64, 48, 60, -1)
check(underruns == 0, "a ring kept ahead of the mixer never underruns (%d)" % underruns)
audio, silent = split(got, 2, 2)
check(len(audio) == written and matches(audio, 0, 2, 2) is None, "unbroken stream plays in order")
print("ok  mixer: unbroken stream, 0 underruns")

# deinit(): the mixer's voice stops once it asks for another block.
ring = Ring(sample_rate=RATE, channel_count=2, frames=32, capacity=4)
mixer = audiomixer.Mixer(voice_count=1, sample_rate=RATE, channel_count=2,
                         bits_per_sample=16, samples_signed=True, buffer_size=32 * 4)
ring.write(frames_of(0, 96, 2))
mixer.voice[0].play(ring)
audiocore.get_buffer(mixer)
ring.deinit()
for _ in range(4):
    audiocore.get_buffer(mixer)
check(not mixer.voice[0].playing, "a deinitialised ring stops the voice")
try:
    ring.write(bytes(4))
    check(False, "write() after deinit() raises")
except ValueError:
    pass
print("ok  deinit: the voice stops within a few pulls, write() raises")

print("ALL PASS", checks, "checks on", sys.implementation.name)
