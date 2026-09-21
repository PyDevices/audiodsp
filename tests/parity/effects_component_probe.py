"""Deterministic effect PCM probes shared by MicroPython and CPython."""

from array import array

import audiocore
import audiodelays
import audiofilters
import audiofreeverb
try:
    import audiospeed
except ImportError:
    audiospeed = None


def checksum(data):
    """FNV-1a over the bytes.

    `sum(data)` is a sum over UNSIGNED BYTES, so it is invariant under any
    permutation of a block AND under any set of byte deltas that cancel - at
    any magnitude, with a sample's high and low byte weighted equally. A
    +256-LSB sample error (about -42 dBFS, plainly audible) paid for by a
    single -1-LSB error was invisible here. 138 of this probe's 139 lines
    carried only that statistic, so the gate checked 16 of 32,002 samples at
    PCM level - 0.05%. See issue #20 and audiodsp#15.

    This is order- and magnitude-sensitive. audiocomponents' instrument probes
    (`tests/parity/instruments_probe_new.py` there) use the same function for
    the same reason. The sum is printed alongside so the older
    per-architecture records stay comparable.
    """
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value

def source(channel_count=1):
    values = array("h")
    for frame in range(768):
        for channel in range(channel_count):
            values.append(((frame * (97 + channel * 18)) % 30001) - 15000)
    return audiocore.RawSample(values, sample_rate=8000, channel_count=channel_count)


for channels in (1, 2):
    for stages in (1, 6):
        effect = audiofilters.Phaser(
            frequency=1234, feedback=.63, mix=.71, stages=stages,
            buffer_size=512, sample_rate=8000, channel_count=channels,
        )
        effect.play(source(channels))
        for index in range(4):
            data = bytes(audiocore.get_buffer(effect)[1])
            if index == 0 and channels == 1 and stages == 1:
                words = tuple(int.from_bytes(data[pos:pos + 2], "little")
                              for pos in range(0, 32, 2))
                decoded = tuple(word - 65536 if word >= 32768 else word
                                for word in words)
                print("phaser_samples", decoded)
            print("phaser", channels, stages, index, len(data), sum(data),
                  checksum(data))

for channels in (1, 2):
    for voices in (1, 3):
        effect = audiodelays.Chorus(
            delay_ms=31.25, voices=voices, mix=.67, buffer_size=512,
            sample_rate=8000, channel_count=channels,
        )
        effect.play(source(channels))
        for index in range(4):
            data = bytes(audiocore.get_buffer(effect)[1])
            print("chorus", channels, voices, index, len(data), sum(data),
                  checksum(data))

for channels in (1, 2):
    for taps in (None, ((.25, .8), (.75, .4))):
        effect = audiodelays.MultiTapDelay(
            delay_ms=64, decay=.43, mix=.61, taps=taps, buffer_size=512,
            sample_rate=8000, channel_count=channels,
        )
        effect.play(source(channels))
        for index in range(6):
            data = bytes(audiocore.get_buffer(effect)[1])
            print("multitap", channels, 0 if taps is None else 2,
                  index, len(data), sum(data), checksum(data))

for channels in (1, 2):
    for semitones in (-5, 7):
        effect = audiodelays.PitchShift(
            semitones=semitones, mix=.72, window=1024, overlap=128,
            buffer_size=512, sample_rate=8000, channel_count=channels,
        )
        effect.play(source(channels))
        for index in range(5):
            data = bytes(audiocore.get_buffer(effect)[1])
            print("pitchshift", channels, semitones, index,
                  len(data), sum(data), checksum(data))

for channels in (1, 2):
    effect = audiofreeverb.Freeverb(
        roomsize=.73, damp=.36, mix=.62, buffer_size=512,
        sample_rate=8000, channel_count=channels,
    )
    effect.play(source(channels))
    for index in range(12):
        data = bytes(audiocore.get_buffer(effect)[1])
        print("freeverb", channels, index, len(data), sum(data),
              checksum(data))

if audiospeed is not None:
    for channels in (1, 2):
        for rate in (.5, 1, 2.25):
            changer = audiospeed.SpeedChanger(source(channels), rate)
            for index in range(8):
                result, view = audiocore.get_buffer(changer)
                data = bytes(view)
                print("speed", channels, rate, index,
                      result, len(data), sum(data), checksum(data))
                if not data: break
