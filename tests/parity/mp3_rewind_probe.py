"""An MP3 rewound for a loop decodes its second lap exactly like its first.

    mp3_rewind_probe.py            # PASS, or FAIL and exit 1
    mp3_rewind_probe.py --fault    # skips the rewind: must FAIL

The fixture is a synthetic 3 s stereo MP3 (noise over a 55 Hz tone, made with
ffmpeg, 128 kbps) that starts with an ID3v2 tag. Two bugs made a rewound lap
differ. Skipping the tag on a rewind called stream_lseek with its offset and
whence swapped, which seeked the file back to byte 1, so the decoder read the
file's start twice; at the end of a long song that failed a few frames in and
a looping player stopped. And the rewind kept the decoder's frame-to-frame
state, so the second lap's first frames were decoded against the first lap's
last ones. MicroPython only: the CPython package has no audiomp3.
"""

import sys

import audiocore

try:
    import audiomp3
except ImportError:
    # The CPython package has no audiomp3. The release job runs every probe
    # under CPython, so this one stands aside there; the MicroPython job runs
    # it and requires the PASS line, so a skip can't pass for a pass.
    print("MP3 REWIND SKIPPED: no audiomp3 here")
    sys.exit(0)

GET_BUFFER_MORE_DATA = 1
FIXTURE = __file__.replace("\\", "/").rsplit("/", 1)[0] + "/fixtures/rewind.mp3"


def checksum(value, data):
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def lap(decoder):
    """(result, buffers, checksum) for one pass through the file."""
    value, buffers = 2166136261, 0
    while True:
        result, buf = audiocore.get_buffer(decoder)
        if buf:
            value = checksum(value, bytes(buf))
        buffers += 1
        if result != GET_BUFFER_MORE_DATA:
            return result, buffers, value


decoder = audiomp3.MP3Decoder(open(FIXTURE, "rb"))
first = lap(decoder)
if "--fault" not in sys.argv:
    audiocore.reset_buffer(decoder)
second = lap(decoder)
print("lap 1: result %d, %d buffers, %08x" % first)
print("lap 2: result %d, %d buffers, %08x" % second)
if first == second:
    print("MP3 REWIND PASS")
else:
    print("MP3 REWIND FAIL")
    sys.exit(1)
