# The consequence of the lost LSB: a SpeedChanger pair asked to cancel --
# decimate at N, restore at 1/N -- is not an identity when 1/N truncates.
# N = 1.0000000000000004 is what fs / rate_hz returns for a log-mapped rate
# knob at the top of its travel.
import array
import math
import audiocore
import audiofilters
import audiospeed

RATE = 44100
FRAMES = 8192
N = 1.0000000000000004

material = array.array(
    "h", [int(round(32000 * math.sin(2 * math.pi * 441 * n / RATE))) for n in range(FRAMES)]
)

wire = audiofilters.Filter(
    sample_rate=RATE, channel_count=1, bits_per_sample=16, samples_signed=True, buffer_size=512
)
wire.play(audiocore.RawSample(material, sample_rate=RATE), loop=True)
down = audiospeed.SpeedChanger(wire, N)
pair = audiospeed.SpeedChanger(down, 1.0 / N)

out = []
while len(out) < FRAMES:
    out.extend(audiocore.get_buffer(pair)[1])

worst = max(abs(a - b) for a, b in zip(out[:FRAMES], material))
print("N = %.17g" % N)
print(
    "  leg rates as stored, in 16.16: %d and %d"
    % (round(down.rate * 65536), round(pair.rate * 65536))
)
print("  worst |out - in| over %d frames: %d codes (a wire reads 0)" % (FRAMES, worst))
