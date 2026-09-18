# What a SpeedChanger renders should not depend on how the node above it
# chunks its output. The phase accumulator counts source frames since the
# stream began, so a source buffer boundary is not frame zero.
#
# Source frame n holds the value n, and the rate is 1.5 (exact in 16.16), so
# output frame n must be source frame (n * 3) // 2.
#
# Run on the unix coverage build with CIRCUITPY_AUDIOSPEED=1.
import array
import audiocore
import audiofilters
import audiospeed

RATE = 1.5
FRAMES = 1024


def render(block, frames):
    """A Filter with nothing set is a wire; buffer_size is in bytes."""
    wire = audiofilters.Filter(
        sample_rate=8000,
        channel_count=1,
        bits_per_sample=16,
        samples_signed=True,
        buffer_size=block * 2,
    )
    wire.play(audiocore.RawSample(array.array("h", range(frames)), sample_rate=8000))
    node = audiospeed.SpeedChanger(wire, RATE)
    out = []
    while len(out) < frames // 2:
        out.extend(audiocore.get_buffer(node)[1])
    return out[: frames // 2]


ideal = [(n * 3) // 2 for n in range(FRAMES // 2)]
print("ideal          ", ideal[:16])
for block in (4, 100, 128):
    out = render(block, FRAMES)
    wrong = sum(1 for a, b in zip(out, ideal) if a != b)
    print(
        "%4d-frame src %s  %d of %d frames wrong"
        % (block, out[:16], wrong, len(ideal))
    )
