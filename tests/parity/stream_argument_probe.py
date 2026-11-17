# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""A decoder or file reader refuses an audio object as its file.

    stream_argument_probe.py            # PASS, or FAIL and exit 1
    stream_argument_probe.py --fault    # alters one observed line: must FAIL

audiocore.WaveFile, audiomp3.MP3Decoder, audiovorbis.VorbisDecoder and
synthio.from_file take a file. An audio object such as a RawSample, a Mixer or
a Synthesizer is not one, and each of them must be refused the way any other
argument that is not a file is, with OSError. On MicroPython the type slot an
audio object keeps its audiosample protocol in is the one the stream check
reads, so the check once took the protocol for a stream and called through it:
the interpreter died with a segmentation fault (audiodsp#428).

Every line is what CircuitPython prints for the same script, and the oracle
passes it. MicroPython and CircuitPython only: the CPython package raises
AttributeError for the same calls, because it reads files through the
standard library.
"""

import sys
from array import array

import audiocore
import audiomixer
import synthio

if sys.implementation.name == "cpython":
    print("STREAM ARGUMENT SKIPPED: CPython reads files its own way")
    sys.exit(0)

# The folder this script is in, where the fixtures are.
FIXTURES = sys.argv[0].rsplit("/", 1)[0] + "/fixtures/"
seen = []


def refused(tag, action):
    try:
        action()
    except Exception as error:  # noqa: BLE001 - the type is what is printed
        seen.append("%s refused %s" % (tag, type(error).__name__))
    else:
        seen.append("%s accepted" % tag)


sample = audiocore.RawSample(array("h", [0] * 64), sample_rate=22050)
mixer = audiomixer.Mixer(sample_rate=22050)
synth = synthio.Synthesizer(sample_rate=22050)

refused("wave_bytes", lambda: audiocore.WaveFile(b"abc"))
refused("wave_int", lambda: audiocore.WaveFile(5))
refused("wave_rawsample", lambda: audiocore.WaveFile(sample))
refused("wave_mixer", lambda: audiocore.WaveFile(mixer))
refused("wave_synth", lambda: audiocore.WaveFile(synth))
refused("synthio_rawsample", lambda: synthio.from_file(sample))

try:
    import audiomp3
except ImportError:
    audiomp3 = None
if audiomp3 is not None:
    refused("mp3_rawsample", lambda: audiomp3.MP3Decoder(sample))
    decoder = audiomp3.MP3Decoder(open(FIXTURES + "rewind.mp3", "rb"))
    refused("mp3_file_rawsample", lambda: setattr(decoder, "file", sample))

try:
    import audiovorbis
except ImportError:
    audiovorbis = None
if audiovorbis is not None:
    refused("vorbis_rawsample", lambda: audiovorbis.VorbisDecoder(sample))
    ogg = audiovorbis.VorbisDecoder(open(FIXTURES + "vorbis_mono.ogg", "rb"))
    refused("vorbis_file_rawsample", lambda: setattr(ogg, "file", sample))

EXPECTED = (
    "wave_bytes refused OSError",
    "wave_int refused OSError",
    "wave_rawsample refused OSError",
    "wave_mixer refused OSError",
    "wave_synth refused OSError",
    "synthio_rawsample refused OSError",
    "mp3_rawsample refused OSError",
    "mp3_file_rawsample refused OSError",
    "vorbis_rawsample refused OSError",
    "vorbis_file_rawsample refused OSError",
)

if "--fault" in sys.argv:
    seen[2] = seen[2].replace("OSError", "TypeError")
if tuple(seen) == EXPECTED:
    print("STREAM ARGUMENT PASS")
else:
    for want, got in zip(EXPECTED, seen):
        if want != got:
            print("expected:", want)
            print("     got:", got)
    if len(seen) != len(EXPECTED):
        print("expected %d lines, got %d" % (len(EXPECTED), len(seen)))
    print("STREAM ARGUMENT FAIL")
    sys.exit(1)
