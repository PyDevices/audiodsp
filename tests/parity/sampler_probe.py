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
- **choke** prints the last frame of the block after a `stop()` that still
  sounded: the fade is 5 ms, 40 frames at 8 kHz, so 39 is the claim and 255
  would be no fade at all, a cut a block late. Its PCM, and that of a
  `play()` over a sounding voice, is emitted beside it.
- **modulated** is PCM from voices with the filter, the modulation
  envelope and both LFOs, and from a filter and a vibrato set live.
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


def choke():
    sampler = build(1, 1)
    sampler.voice[0].play(triangle(), loop_mode=sampler_module.LOOP_FOREVER)
    audiocore.get_buffer(sampler)
    sampler.voice[0].stop()
    heard = words(bytes(audiocore.get_buffer(sampler)[1]))
    last = -1
    for index, value in enumerate(heard):
        if value:
            last = index
    print("choke", last)

    # A steal: a new note over one still sounding, and a stop() with a
    # play() straight after it, both fading the old note under the new.
    sampler = build(2, 2)
    sampler.voice[0].play(triangle(), pitch=1.5, pan=-0.5,
                          loop_mode=sampler_module.LOOP_FOREVER)
    sampler.voice[1].play(triangle(), pitch=0.75,
                          loop_mode=sampler_module.LOOP_FOREVER, release=0.25)
    audiocore.get_buffer(sampler)
    sampler.voice[1].release()
    audiocore.get_buffer(sampler)
    sampler.voice[0].play(triangle(), rate=4000, pan=0.25,
                          loop_mode=sampler_module.LOOP_FOREVER)
    sampler.voice[1].stop()
    sampler.voice[1].play(triangle(), pitch=2.0,
                          loop_mode=sampler_module.LOOP_FOREVER)
    emit("steal", sampler, 2)


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
    choke()
    modulated()


def modulated():
    # The filter, the modulation envelope and both LFOs on one voice, and a
    # filter moved live on another: the cutoff's tangent and every 2^x are
    # polynomials so that this is the same bits everywhere. Times are whole
    # samples at 8 kHz and depths whole cents.
    sampler = build(2, 2)
    sampler.voice[0].play(triangle(), pitch=1.5,
                          loop_mode=sampler_module.LOOP_FOREVER,
                          cutoff=1000.0, resonance=9.0, mod_attack=0.0625,
                          mod_decay=0.125, mod_sustain=0.25,
                          mod_to_pitch=200.0, mod_to_cutoff=1200.0,
                          vibrato_delay=0.03125, vibrato_rate=6.0,
                          vibrato_depth=30.0, lfo_rate=2.0, lfo_to_pitch=10.0,
                          lfo_to_cutoff=300.0, lfo_to_volume=3.0)
    sampler.voice[1].play(triangle(), pitch=0.75, pan=0.5,
                          loop_mode=sampler_module.LOOP_FOREVER)
    emit("modulated", sampler, 6)
    sampler.voice[1].cutoff = 500.0
    sampler.voice[1].vibrato_depth = 40.0
    emit("closed", sampler, 4)
    sampler.voice[0].release()
    sampler.voice[1].cutoff = None
    emit("opened", sampler, 3)


main()
