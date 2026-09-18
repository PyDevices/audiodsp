# A rate is stored in 16.16, so `rate` should read back within half a step of
# what was asked for. Truncating instead of rounding costs a whole step.
#
# Run on the unix coverage build with CIRCUITPY_AUDIOSPEED=1.
import array
import audiocore
import audiospeed

source = audiocore.RawSample(array.array("h", [0] * 64), sample_rate=48000)

for asked, wanted in (
    (0.5 - 1e-6, 32768),
    (1.0 / 4.0000000000000036, 16384),
    (1.0 / 1.0000000000000004, 65536),
):
    stored = round(audiospeed.SpeedChanger(source, asked).rate * 65536)
    print(
        "asked %.17g -> %6d, nearest step %6d%s"
        % (asked, stored, wanted, "" if stored == wanted else "   <-- a whole LSB low")
    )
