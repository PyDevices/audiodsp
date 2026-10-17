"""Deterministic audiosampler PCM, and the invariants the module exists for.

    sampler_probe.py audiosampler

Like modal_probe.py, this has no oracle: `audiosampler` is audiodsp's own
module, with no ancestor in CircuitPython. What the gate pins is that every
interpreter renders it identically, and that nothing moves it by accident.

Three of the sections are invariants rather than PCM:

- **ends** prints the block at which a `LOOP_NONE` voice stopped and the one
  at which a released `LOOP_UNTIL_RELEASE` voice reached the end of its
  buffer. A negative number means it never did, which is a failure.
- **fade** prints the block at which a released voice went silent and
  `playing` went False. Both numbers come from the release time, so a
  per-block envelope would move them.
- **held** prints how many frames of the buffer's tail were heard while a
  mode-3 voice was still held: zero is the claim.

Everything a probe prints has to be integer-exact on three interpreters, so
the buffer is an integer triangle wave, every pitch is an exact binary
fraction, and every envelope time is a whole number of samples at 8 kHz --
never `math.sin()`, which is three functions that nearly agree.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiosampler"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
sampler_module = __import__(MODULE)

SAMPLE_RATE = 8000
CYCLE = 64


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def triangle(frames=CYCLE, peak=16000):
    """One period of a triangle wave in integers, with a non-zero start so
    the loop seam is not hidden by a zero."""
    values = array("h")
    quarter = frames // 4
    for index in range(frames):
        phase = index % frames
        if phase < quarter:
            value = phase * peak // quarter
        elif phase < 3 * quarter:
            value = peak - (phase - quarter) * peak // quarter
        else:
            value = -peak + (phase - 3 * quarter) * peak // quarter
        values.append(value + 1000)
    return values


def tagged(frames=192, marker=12345):
    """A triangle whose tail past the loop region is a marker value."""
    values = triangle(128)
    for _ in range(frames - 128):
        values.append(marker)
    return values


def build(voices=2, channels=2):
    return sampler_module.Sampler(voice_count=voices, channel_count=channels,
                                  sample_rate=SAMPLE_RATE)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("sampler", tag, index, len(data), sum(data), checksum(data))


def words(data):
    out = []
    for position in range(0, len(data), 2):
        word = data[position] | (data[position + 1] << 8)
        out.append(word - 65536 if word >= 32768 else word)
    return out


def ends():
    sampler = build(2, 1)
    sampler.voice[0].play(triangle(), loop_mode=sampler_module.LOOP_NONE)
    stopped = -1
    for index in range(4):
        audiocore.get_buffer(sampler)
        if stopped < 0 and not sampler.voice[0].playing:
            stopped = index
    sampler.voice[1].play(tagged(), loop_start=32, loop_end=96,
                          loop_mode=sampler_module.LOOP_UNTIL_RELEASE,
                          pitch=0.75, release=1.0)
    heard = 0
    for _index in range(6):
        heard += sum(1 for w in words(bytes(audiocore.get_buffer(sampler)[1]))
                     if w == 12345)
    print("held", heard)
    sampler.voice[1].release()
    ended = -1
    for index in range(6):
        audiocore.get_buffer(sampler)
        if ended < 0 and not sampler.voice[1].playing:
            ended = index
    print("ends", stopped, ended)


def fade():
    sampler = build(1, 1)
    # 0.25 s at 8 kHz is 2000 samples: 7.8 blocks of 256.
    sampler.voice[0].play(triangle(), loop_mode=sampler_module.LOOP_FOREVER,
                          pitch=1.5, release=0.25)
    audiocore.get_buffer(sampler)
    sampler.voice[0].release()
    quiet = -1
    stopped = -1
    for index in range(12):
        data = bytes(audiocore.get_buffer(sampler)[1])
        if quiet < 0 and data == bytes(len(data)):
            quiet = index
        if stopped < 0 and not sampler.voice[0].playing:
            stopped = index
    print("fade", quiet, stopped)


def main():
    # The wire: pitch 1.0 on a whole-frame loop reads the buffer's own values.
    for channels in (1, 2):
        sampler = build(1, channels)
        sampler.voice[0].play(triangle(), loop_mode=sampler_module.LOOP_FOREVER)
        emit("wire%d" % channels, sampler, 3)

    # Two voices through the interpolator at binary-fraction pitches, panned
    # apart, one with a full DAHDSR whose stages are whole samples at 8 kHz.
    sampler = build(2, 2)
    sampler.voice[0].play(triangle(), pitch=1.5, pan=-0.5, level=0.75,
                          loop_mode=sampler_module.LOOP_FOREVER,
                          attack=0.0625, hold=0.03125, decay=0.125,
                          sustain=0.5)
    sampler.voice[1].play(triangle(), rate=4000, pitch=0.75, pan=0.25,
                          loop_start=16, loop_end=48,
                          loop_mode=sampler_module.LOOP_FOREVER, delay=0.0125)
    emit("pair", sampler, 10)
    # A live move of each fader, and of the pitch, landing as a ramp.
    sampler.voice[0].level = 0.25
    sampler.voice[1].pan = -1.0
    sampler.voice[1].pitch = 2.0
    emit("moved", sampler, 3)
    sampler.voice[0].release()
    emit("let_go", sampler, 2)

    ends()
    fade()


main()
