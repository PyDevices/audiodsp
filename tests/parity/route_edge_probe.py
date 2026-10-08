"""Routing and mixer events no other probe covers (audiodsp#178).

    EDGE  <case>  <what was seen>

- `split-part-frame`: a Splitter whose source hands back a block that is not
  whole frames. The native node drops the block; the CPython twin used to keep
  the bytes and raise on that pull and every one after.
- `midside-surface`: MidSide's public names. The twin had `stop()` and
  `playing`, which the native node has never had.
- `midside-set-twice`: two `set(width=...)` calls before one pull: the pull
  renders the second.
- `mixer-two-moves`: two level moves on a voice before one pull, and a level
  taken to 0 and back.
"""

from array import array

import audiocore
import audiomixer
import audioroute

RATE = 48000


def checksum(data):
    value = 2166136261
    for byte in bytes(data):
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def ramp(frames, channels=2):
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            values.append(((frame * (37 + channel * 13)) % 801) * 20 - 8000)
    return values


def pull(node):
    try:
        result, data = audiocore.get_buffer(node)
    except Exception as error:
        return type(error).__name__
    return "%d:%d:%d" % (result, len(bytes(data)), checksum(data))


# split-part-frame
source = audiocore.RawSample(array("h", [1000, 2000, 3000]),
                             sample_rate=RATE, channel_count=2)
splitter = audioroute.Splitter(source, taps=1)
tap = splitter.tap(0)
print("EDGE split-part-frame", " ".join(pull(tap) for _ in range(3)))

# midside-surface
node = audioroute.MidSide(width=1.0)
print("EDGE midside-surface",
      " ".join(name for name in ("deinit", "play", "playing", "set", "stop")
               if hasattr(node, name)))

# midside-set-twice
node = audioroute.MidSide(width=1.0, sample_rate=RATE)
node.play(audiocore.RawSample(ramp(2048), sample_rate=RATE, channel_count=2))
node.set(width=0.25)
node.set(width=1.75)
print("EDGE midside-set-twice", pull(node), pull(node))

# mixer-two-moves
mixer = audiomixer.Mixer(voice_count=1, sample_rate=RATE, channel_count=2,
                         buffer_size=2048)
mixer.voice[0].play(audiocore.RawSample(ramp(300), sample_rate=RATE,
                                        channel_count=2), loop=True)
seen = [pull(mixer)]
mixer.voice[0].level = 0.3
mixer.voice[0].level = 0.7
seen.append(pull(mixer))
mixer.voice[0].level = 0.0
seen.append(pull(mixer))
mixer.voice[0].level = 1.0
seen.append(pull(mixer))
seen.append(pull(mixer))
print("EDGE mixer-two-moves", " ".join(seen))
