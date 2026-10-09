"""CircuitPython 11.0.0-alpha.1's WaveFile and MP3Decoder fixes.

    cp11_files_probe.py            # PASS, or FAIL and exit 1
    cp11_files_probe.py --fault    # alters one observed line: must FAIL

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

Every line is compared with what CircuitPython 11.0.0-alpha.1 prints for the
same script, which is EXPECTED below, and the oracle passes it too.
MicroPython and CircuitPython only. The CPython package reads WAV files
through the standard library in its own block sizes and has no audiomp3, so
the release job's run of every probe under CPython finds this one standing
aside, and CI runs it on the MicroPython build and requires the PASS line.
"""

import os
import sys

import audiocore

try:
    import audiomp3
except ImportError:
    print("CP11 FILES SKIPPED: no audiomp3 here")
    sys.exit(0)

EXPECTED = (
    "wav8_5 0 0 8 10 11 12 13 14 80 80 80",
    "wav8_6 0 0 8 10 11 12 13 14 15 80 80",
    "wav8_7 0 0 8 10 11 12 13 14 15 16 80",
    "wav8_buffer16 0 1 8 20 21 22 23 24 25 26 27",
    "wav8_buffer16 1 1 8 28 29 2a 2b 2c 2d 2e 2f",
    "wav8_buffer16 2 0 8 30 31 32 33 34 80 80 80",
    "wav_buffer_12 refused ValueError",
    "wav_buffer_8 accepted",
    "mp3_cut lap 1 result 0 buffers 79",
    "mp3_cut lap 2 result 0 buffers 79",
)
seen = []


def emit(*fields):
    line = " ".join(str(field) for field in fields)
    print(line)
    seen.append(line)


HERE = __file__.replace("\\", "/").rsplit("/", 1)[0]
FIXTURE = HERE + "/fixtures/rewind.mp3"
WAV = "cp11_files_probe.wav"
MP3 = "cp11_files_probe.mp3"


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
        emit(tag, index, result, len(data),
             " ".join("%02x" % b for b in data[-8:]))
        if result != 1:
            return


def refused(tag, action):
    try:
        action()
    except Exception as error:  # noqa: BLE001 - the type is what is printed
        emit(tag, "refused", type(error).__name__)
    else:
        emit(tag, "accepted")


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
        # Only how each lap ends is compared, not its bytes: this port clears
        # the decoder's state on a rewind and 11.0.0-alpha.1 does not yet
        # (docs/upstream-sync.md), so the second laps differ by design.
        for lap in (1, 2):
            buffers = 0
            while True:
                result, view = audiocore.get_buffer(decoder)
                buffers += 1
                if result != 1:
                    break
            emit("mp3_cut lap", lap, "result", result, "buffers", buffers)
            audiocore.reset_buffer(decoder)
finally:
    for name in (WAV, MP3):
        try:
            os.remove(name)
        except OSError:
            pass

if "--fault" in sys.argv:
    seen[0] = seen[0][:-3]
if tuple(seen) == EXPECTED:
    print("CP11 FILES PASS")
else:
    for want, got in zip(EXPECTED, seen):
        if want != got:
            print("expected:", want)
            print("got:     ", got)
            break
    print("CP11 FILES FAIL")
    sys.exit(1)
