"""CircuitPython 11.0.0-alpha.1's WaveFile and MP3Decoder fixes.

    cp11_files_probe.py audiocore

The file-backed half of what this port took when it resynced with
11.0.0-alpha.1 (audiodsp#220). Each case prints something different on a
build without its fix:

- an 8-bit WaveFile whose data ends part-way through a 32-bit word is padded
  up to the next word with silence (0x80). 10.3.0 padded by the remainder
  instead, so a 5-byte tail came out 6 bytes long (34441b1af5);
- a WaveFile buffer must be a multiple of 8 bytes, so each half holds whole
  words and the pad cannot run past it (bd9b603c9c);
- an MP3 cut off mid-frame ends with GET_BUFFER_DONE, the way a file ends,
  and not GET_BUFFER_ERROR, so a looping player starts it again instead of
  stopping (b34aa34c19).

MicroPython and CircuitPython only. The CPython package reads WAV files
through the standard library in its own block sizes and has no audiomp3, so
the release job's run of every probe under CPython finds this one standing
aside.
"""

import os
import sys

import audiocore

try:
    import audiomp3
except ImportError:
    print("CP11 FILES SKIPPED: no audiomp3 here")
    sys.exit(0)

HERE = __file__.replace("\\", "/").rsplit("/", 1)[0]
FIXTURE = HERE + "/fixtures/rewind.mp3"
WAV = "cp11_files_probe.wav"
MP3 = "cp11_files_probe.mp3"


def checksum(value, data):
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def le(value, size):
    return bytes((value >> (8 * index)) & 0xff for index in range(size))


def write_wav(data, bits=8, channels=1, rate=8000):
    block = channels * bits // 8
    fmt = (le(1, 2) + le(channels, 2) + le(rate, 4) + le(rate * block, 4)
           + le(block, 2) + le(bits, 2))
    body = (b"WAVEfmt " + le(len(fmt), 4) + fmt
            + b"data" + le(len(data), 4) + data)
    with open(WAV, "wb") as handle:
        handle.write(b"RIFF" + le(len(body), 4) + body)


def drain(tag, sample, limit=64):
    # A player resets a sample before its first pull; WaveFile counts what is
    # left from there.
    audiocore.reset_buffer(sample)
    for index in range(limit):
        result, view = audiocore.get_buffer(sample)
        data = bytes(view)
        print(tag, index, result, len(data), " ".join("%02x" % b for b in data[-8:]))
        if result != 1:
            return


def refused(tag, action):
    try:
        action()
    except Exception as error:  # noqa: BLE001 - the type is what is printed
        print(tag, "refused", type(error).__name__)
    else:
        print(tag, "accepted")


try:
    for length in (5, 6, 7):
        write_wav(bytes(range(0x10, 0x10 + length)))
        with open(WAV, "rb") as handle:
            drain("wav8_%d" % length, audiocore.WaveFile(handle))

    write_wav(bytes(range(0x20, 0x20 + 21)))
    with open(WAV, "rb") as handle:
        drain("wav8_buffer16", audiocore.WaveFile(handle, bytearray(16)))
        refused("wav_buffer_12",
                lambda: audiocore.WaveFile(handle, bytearray(12)))
        refused("wav_buffer_8",
                lambda: audiocore.WaveFile(handle, bytearray(8)))

    with open(FIXTURE, "rb") as handle:
        whole = handle.read()
    # Two thirds of the file and then some, so the cut falls inside a frame.
    with open(MP3, "wb") as handle:
        handle.write(whole[:len(whole) * 2 // 3 + 77])
    with open(MP3, "rb") as handle:
        decoder = audiomp3.MP3Decoder(handle)
        for lap in (1, 2):
            value, buffers = 2166136261, 0
            while True:
                result, view = audiocore.get_buffer(decoder)
                value = checksum(value, bytes(view))
                buffers += 1
                if result != 1:
                    break
            # The second lap's bytes are not compared: this port clears the
            # decoder's state on a rewind and 11.0.0-alpha.1 does not yet
            # (docs/upstream-sync.md), so only its ending is.
            print("mp3_cut lap", lap, "result", result, buffers,
                  value if lap == 1 else "")
            audiocore.reset_buffer(decoder)
finally:
    for name in (WAV, MP3):
        try:
            os.remove(name)
        except OSError:
            pass
