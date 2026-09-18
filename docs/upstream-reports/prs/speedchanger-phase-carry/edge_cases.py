# Guards for the carry: the stream must still end, a carry larger than a whole
# source buffer must not read out of one, and a Resampler's bound rate must
# still be the ratio it was asked for.
import array
import audiocore
import audiofilters
import audiospeed


def drain(node, limit=400):
    frames = 0
    for _ in range(limit):
        result, buf = audiocore.get_buffer(node)
        frames += 0 if buf is None else len(buf)
        if result == 0:  # GET_BUFFER_DONE
            return frames, "done"
    return frames, "never finished"


def wire(block, frames):
    node = audiofilters.Filter(
        sample_rate=8000,
        channel_count=1,
        bits_per_sample=16,
        samples_signed=True,
        buffer_size=block * 2,
    )
    node.play(audiocore.RawSample(array.array("h", range(frames)), sample_rate=8000))
    return node


# A RawSample ends, so this exercises the end of the source. A Filter wire
# never does: it keeps handing out silence once its sample is finished.
for rate in (0.3, 1.0, 1.5, 6.0):
    source = audiocore.RawSample(array.array("h", range(512)), sample_rate=8000)
    frames, how = drain(audiospeed.SpeedChanger(source, rate))
    print(
        "rate %-4s RawSample(512) -> %4d frames, %s (512/%s = %d expected)"
        % (rate, frames, how, rate, 512 / rate)
    )

for rate, block in ((0.3, 100), (1.5, 4), (6.0, 4), (6.0, 100), (1.0, 128)):
    frames, how = drain(audiospeed.SpeedChanger(wire(block, 512), rate), limit=20)
    print(
        "rate %-4s %3d-frame wire -> %4d frames, %s (a wire never ends)"
        % (rate, block, frames, how)
    )

import audiomixer

sample = audiocore.RawSample(array.array("h", [0] * 64), sample_rate=48000)
resampler = audiospeed.Resampler(sample)
mixer = audiomixer.Mixer(
    voice_count=1, sample_rate=44100, channel_count=1, bits_per_sample=16, samples_signed=True
)
mixer.play(resampler)
print("Resampler bound 48000 -> 44100:", round(resampler.rate * 65536), "want 71332")
