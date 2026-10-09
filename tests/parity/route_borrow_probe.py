"""Routing nodes under a mixer voice: borrowed tap buffers, a looping voice
over a double-buffered sample, and a Port's pass-through.

    route_borrow_probe.py audioroute

Three of the items in audiodsp#178, each asserted as well as printed:

- **A mono tap's buffer is the tap's own.** The native mono SplitterTap copies
  its frames out of the stereo ring into one buffer of its own and hands that
  out, so a mixer voice primed from the tap, with the tap then pulled
  directly, mixes the block the last pull left there. The CPython twin used
  to hand out a fresh copy and mix the earlier block. A stereo tap hands out
  its stretch of the ring, which the next pull does not rewrite, so there the
  voice mixes the block it was primed with on every target.
- **A looping voice over a double-buffered RawSample** plays the sample round
  and round, the same as over a single-buffered one once its level is up:
  the two halves of the sample are two stretches, and neither loses or
  repeats a frame at the seam or at the wrap.
- **A Port moves no bytes.** What a voice mixes through a Port is what it
  mixes from the source itself. A re-point mid-stream is printed, so the
  interpreters are held to the same bytes there too.

Every block a voice mixes here is a level-1.0 wire, so the bytes are the
source's own.
"""

import sys
from array import array

import audiocore
import audiomixer

route = __import__(sys.argv[1] if len(sys.argv) > 1 else "audioroute")

RATE = 8000
BLOCK = 256


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def sample(data, index):
    value = data[index * 2] | (data[index * 2 + 1] << 8)
    return value - 65536 if value & 0x8000 else value


FAILED = []


def check(tag, ok):
    print("check", tag, "ok" if ok else "FAILED")
    if not ok:
        FAILED.append(tag)


def squares(blocks, frames, channels):
    """Block b is a square of +-1000 * (b + 1). It flips every two frames,
    so the packed 32-bit word a mixer's level gate reads crosses zero in
    mono as well as in stereo."""
    pcm = array("h", bytes(2 * blocks * frames * channels))
    for block in range(blocks):
        for frame in range(frames):
            value = 1000 * (block + 1)
            if (frame // 2) % 2:
                value = -value
            for channel in range(channels):
                pcm[(block * frames + frame) * channels + channel] = value
    return pcm


def mixer(channels, frames=BLOCK):
    node = audiomixer.Mixer(voice_count=1, buffer_size=frames * 2 * channels * 2,
                            sample_rate=RATE, channel_count=channels)
    node.voice[0].level = 1.0
    return node


# A tap borrowed by a voice, then pulled directly 0, 1 and 2 times.
for channels in (1, 2):
    for pulls in (0, 1, 2):
        tag = "tap-%dch-%d" % (channels, pulls)
        source = audiocore.RawSample(squares(8, BLOCK, channels),
                                     sample_rate=RATE, channel_count=channels)
        split = route.Splitter(source, taps=1)
        tap = split.tap(0)
        mix = mixer(channels)
        mix.voice[0].play(tap)
        seen = [sample(bytes(audiocore.get_buffer(tap)[1]), 0)
                for _ in range(pulls)]
        data = bytes(audiocore.get_buffer(mix)[1])
        # Past the first zero crossing, where the voice's level is up.
        level = abs(sample(data, 100 * channels))
        print("route-borrow", tag, seen, level, checksum(data))
        want = 1000 * (pulls + 1) if channels == 1 else 1000
        check(tag, level == want)
        split.deinit()

# A looping voice over a RawSample of three 100-frame stretches.
for channels in (1, 2):
    played = {}
    for single in (True, False):
        tag = "loop-%dch-%s" % (channels, "single" if single else "double")
        pcm = squares(3, 100, channels)
        source = audiocore.RawSample(pcm, sample_rate=RATE,
                                     channel_count=channels,
                                     single_buffer=single)
        mix = mixer(channels)
        mix.voice[0].play(source, loop=True)
        blocks = [bytes(audiocore.get_buffer(mix)[1]) for _ in range(6)]
        print("route-borrow", tag, [checksum(b) for b in blocks])
        # From the second block on the level is up, so every frame is the
        # source's, round and round: frame f of the mix is frame f % 300.
        period = 300 * channels
        ok = True
        for index in range(BLOCK * channels, 6 * BLOCK * channels):
            block, at = divmod(index, BLOCK * channels)
            if sample(blocks[block], at) != pcm[index % period]:
                ok = False
                break
        check(tag + " goes round", ok)
        played[single] = blocks[1:]
    check("loop-%dch single = double" % channels,
          played[True] == played[False])

# A Port in front of a voice, and re-pointed while it plays.
for channels in (1, 2):
    tag = "port-%dch" % channels
    first = squares(8, BLOCK, channels)
    second = squares(8, BLOCK, channels)
    for index in range(len(second)):
        second[index] = -(second[index] // 2)
    direct = mixer(channels)
    direct.voice[0].play(audiocore.RawSample(first, sample_rate=RATE,
                                             channel_count=channels))
    through = mixer(channels)
    port = route.Port(audiocore.RawSample(first, sample_rate=RATE,
                                          channel_count=channels))
    through.voice[0].play(port)
    a = [bytes(audiocore.get_buffer(direct)[1]) for _ in range(3)]
    b = [bytes(audiocore.get_buffer(through)[1]) for _ in range(3)]
    check(tag + " is a wire", a == b)
    other = audiocore.RawSample(second, sample_rate=RATE,
                                channel_count=channels)
    port.play(other)
    after = [bytes(audiocore.get_buffer(through)[1]) for _ in range(2)]
    print("route-borrow", tag, [checksum(x) for x in b + after])
    port.deinit()

print("done route-borrow")
if FAILED:
    raise SystemExit("failed: " + " ".join(FAILED))
