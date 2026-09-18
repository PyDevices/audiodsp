# Dump a Flanger render, one sample a line, for comparing two builds.
#   micropython flanger_dump.py rails > rails.txt
import sys
import array
import audiocore
import audiodelays

which = sys.argv[1] if len(sys.argv) > 1 else "rails"
if which == "rails":
    material = array.array("h", [32767 if n % 2 == 0 else -32768 for n in range(2048)])
else:
    material = array.array("h", [(n % 64) * 200 - 6400 for n in range(2048)])

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
n = 0
for _ in range(8):
    for value in audiocore.get_buffer(effect)[1]:
        print(n, value)
        n += 1
