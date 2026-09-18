# The Flanger's wet tap interpolates between two int16 taps:
#
#   int32_t wet = s0 + (((s1 - s0) * (int32_t)delay_frac) >> 16);
#
# `s1 - s0` reaches +/-65535 and `delay_frac` reaches 65535, so the product
# reaches 4.29e9 and the int32 multiply overflows. Full-scale material that
# alternates sign frame to frame is enough to get there.
#
# Run on the unix coverage build.
import array
import audiocore
import audiodelays

# What the expression above computes for two taps at opposite rails, done in
# Python (no int32 to overflow), and what C's int32 does with the same values.
s0, s1, frac = -32768, 32767, 65535
exact = s0 + (((s1 - s0) * frac) >> 16)
wrapped = ((s1 - s0) * frac) & 0xFFFFFFFF
if wrapped >= 0x80000000:
    wrapped -= 0x100000000
print("taps %d..%d at frac %d: int32 wet %d, correct %d" % (s0, s1, frac, s0 + (wrapped >> 16), exact))

# And what it does to a render. Full-scale alternating input, no dry signal.
RAILS = array.array("h", [32767 if n % 2 == 0 else -32768 for n in range(2048)])
QUIET = array.array("h", [(n % 64) * 200 - 6400 for n in range(2048)])


def render(material):
    effect = audiodelays.Flanger(
        max_delay_ms=10,
        min_delay_ms=1.0,
        rate=2.0,
        depth=1.0,
        feedback=0.5,
        mix=1.0,
        buffer_size=512,
        sample_rate=8000,
        channel_count=1,
    )
    effect.play(audiocore.RawSample(material, sample_rate=8000), loop=True)
    out = []
    for _ in range(8):
        out.extend(audiocore.get_buffer(effect)[1])
    return out


for name, material in (("rails", RAILS), ("ordinary", QUIET)):
    out = render(material)
    print("%-8s first 8 output frames: %s" % (name, out[:8]))
    print("%-8s sum %d  min %d  max %d" % (name, sum(out), min(out), max(out)))
