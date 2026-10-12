"""audiovorbis.VorbisDecoder: the surface, the edges, and a loop.

    vorbis_probe.py            # PASS, or FAIL and exit 1
    vorbis_probe.py --fault    # skips the rewind before the second lap: must FAIL

Runs unchanged on MicroPython and CPython (and on a board, from anywhere the
two fixtures sit beside it in fixtures/). The decoded samples themselves are
compared with ffmpeg's in verify_vorbis.py; this file is about behaviour:

- the attributes MP3Decoder has, with the values the file says;
- a rewound file plays its second lap exactly like its first, from the end
  and from part-way through;
- a Mixer voice plays the file unchanged, and looping it plays lap after lap;
- an optional buffer sets the block size;
- a file cut off in its headers is refused, and one cut off in its audio
  plays what is there and ends with DONE;
- things that aren't Vorbis (WAV, MP3, Ogg with a damaged header, noise, an
  empty file) are refused with RuntimeError, never a crash;
- deinit() twice is fine, and afterwards the decoder raises ValueError;
- the pump refuses it, as it does an MP3.

The fixtures are 2 s of 440 Hz left and 660 Hz right at 44.1 kHz, and 1 s of
a mono chirp at 48 kHz, both made by ffmpeg's libvorbis at -q:a 0.
"""

import io
import sys

import audiocore
import audiomixer

try:
    import audiovorbis
except ImportError:
    print("VORBIS SKIPPED: no audiovorbis here")
    sys.exit(0)

FAULT = "--fault" in sys.argv
HERE = __file__.replace("\\", "/").rsplit("/", 1)[0]
STEREO = HERE + "/fixtures/vorbis_stereo.ogg"
MONO = HERE + "/fixtures/vorbis_mono.ogg"
MP3 = HERE + "/fixtures/rewind.mp3"

DONE, MORE, ERROR = 0, 1, 2
failures = []


def check(name, ok, detail=""):
    print("  %-26s %-4s %s" % (name, "ok" if ok else "FAIL", detail))
    if not ok:
        failures.append(name)


def fnv(value, data):
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def lap(sample, limit=100000):
    """(last result, bytes, checksum) for one pass."""
    value, total = 2166136261, 0
    for _ in range(limit):
        result, buf = audiocore.get_buffer(sample)
        # bytes(): CircuitPython's get_buffer hands out a view of int16, whose
        # len() counts samples; MicroPython's and CPython's count bytes.
        data = bytes(buf) if buf else b""
        value = fnv(value, data)
        total += len(data)
        if result != MORE:
            return result, total, value
    return -1, total, value


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def refused(make):
    try:
        decoder = make()
    except RuntimeError as error:
        return "Vorbis" in str(error), type(error).__name__
    except Exception as error:  # noqa: BLE001 - which exception is the question
        return False, type(error).__name__
    decoder.deinit()
    return False, "accepted"


# --- the surface -------------------------------------------------------------

print("surface")
decoder = audiovorbis.VorbisDecoder(STEREO)
check("sample_rate", decoder.sample_rate == 44100, decoder.sample_rate)
check("channel_count", decoder.channel_count == 2, decoder.channel_count)
check("bits_per_sample", decoder.bits_per_sample == 16, decoder.bits_per_sample)
check("file", decoder.file is not None)
check("samples_decoded at 0", decoder.samples_decoded == 0, decoder.samples_decoded)
# MP3Decoder's own names, every one of them.
for name in ("open", "deinit", "file", "rms_level", "samples_decoded",
             "sample_rate", "bits_per_sample", "channel_count"):
    check("has " + name, hasattr(decoder, name))
for name in ("__enter__", "__exit__"):
    check("has " + name, hasattr(type(decoder), name))

# --- laps --------------------------------------------------------------------

print("laps")
first = lap(decoder)
check("first lap ends DONE", first[0] == DONE, first)
check("first lap is 2 s of stereo", first[1] == 2 * 44100 * 2 * 2, first[1])
check("samples_decoded", decoder.samples_decoded == 2 * 44100 * 2, decoder.samples_decoded)
check("rms_level is a level", 0 <= decoder.rms_level < 32768, decoder.rms_level)
if not FAULT:
    audiocore.reset_buffer(decoder)
second = lap(decoder)
check("rewound lap matches", second == first, "%r %r" % (first, second))

# A rewind part-way through starts again from the top, too.
audiocore.reset_buffer(decoder)
for _ in range(10):
    audiocore.get_buffer(decoder)
audiocore.reset_buffer(decoder)
third = lap(decoder)
check("mid-file rewind matches", third == first, third)
decoder.deinit()

mono = audiovorbis.VorbisDecoder(open(MONO, "rb"))
check("mono rate and channels", (mono.sample_rate, mono.channel_count) == (48000, 1),
      (mono.sample_rate, mono.channel_count))
mono_lap = lap(mono)
check("mono lap is 1 s", mono_lap[1] == 48000 * 2 and mono_lap[0] == DONE, mono_lap)
mono.deinit()

# --- the file property and open() ---------------------------------------------

print("files")
with audiovorbis.VorbisDecoder(STEREO) as switched:
    switched.file = open(MONO, "rb")
    check("file= switches format", (switched.sample_rate, switched.channel_count) == (48000, 1))
    check("file= plays the new file", lap(switched) == mono_lap)
    switched.open(STEREO)
    check("open() plays the new file", lap(switched) == first)
check("with: deinited on exit", refused(lambda: switched.sample_rate) == (False, "ValueError"))

# --- buffer ------------------------------------------------------------------

print("buffer")
sized = audiovorbis.VorbisDecoder(STEREO, bytearray(2048))
result, buf = audiocore.get_buffer(sized)
check("a 2048-byte buffer gives 1024-byte blocks", len(bytes(buf)) == 1024, len(bytes(buf)))
sized.deinit()
small = audiovorbis.VorbisDecoder(STEREO, bytearray(16))
result, buf = audiocore.get_buffer(small)
check("a too-small buffer is not used", len(bytes(buf)) == 1024 * 4, len(bytes(buf)))
small.deinit()

# --- the mixer ---------------------------------------------------------------

print("mixer")
BLOCK = 1024
mixer = audiomixer.Mixer(voice_count=1, sample_rate=44100, channel_count=2,
                         bits_per_sample=16, samples_signed=True,
                         buffer_size=BLOCK * 4)
played = audiovorbis.VorbisDecoder(STEREO)
mixer.voice[0].play(played)
value, total = 2166136261, 0
for _ in range(400):
    if not mixer.voice[0].playing:
        break
    result, buf = audiocore.get_buffer(mixer)
    data = bytes(buf)[:first[1] - total]
    if total == 0:
        # A new Mixer's level starts at zero for its very first frame, whatever
        # it plays: compare from the second frame on.
        data = data[4:]
        value = fnv(value, bytes(4))
    value = fnv(value, data)
    total += len(data) + (4 if total == 0 else 0)
head = audiovorbis.VorbisDecoder(STEREO)
expect = fnv(2166136261, bytes(4))
skip = 4
while True:
    result, buf = audiocore.get_buffer(head)
    expect = fnv(expect, bytes(buf)[skip:])
    skip = 0
    if result != MORE:
        break
head.deinit()
check("one voice plays the file unchanged", (total, value) == (first[1], expect), (total, value))

looped = audiovorbis.VorbisDecoder(STEREO)
mixer.voice[0].play(looped, loop=True)
value, total = 2166136261, 0
while total < 3 * first[1]:
    result, buf = audiocore.get_buffer(mixer)
    data = bytes(buf)
    if total < first[1] and total + len(data) >= first[1]:
        cut = first[1] - total
        lap_one, value = fnv(value, data[:cut]), 2166136261
        value = fnv(value, data[cut:])
    else:
        value = fnv(value, data)
    total += len(data)
check("still playing after two laps", mixer.voice[0].playing)
mixer.voice[0].stop()
check("looped lap 1 matches", lap_one == first[2], hex(lap_one))
looped.deinit()
played.deinit()
mixer.deinit()

# --- truncated files ---------------------------------------------------------

print("truncated")
whole = read_bytes(STEREO)
ok, kind = refused(lambda: audiovorbis.VorbisDecoder(io.BytesIO(whole[:200])))
check("cut in the headers is refused", ok, kind)
# Half way into the last page, so the first audio page is whole.
last_page = whole.rfind(b"OggS")
cut = audiovorbis.VorbisDecoder(io.BytesIO(whole[:(last_page + len(whole)) // 2]))
result, total, value = lap(cut)
check("cut in the audio ends DONE", result == DONE, result)
check("cut in the audio plays part", 0 < total < first[1], total)
# What it plays is the start of the whole file, sample for sample.
full = audiovorbis.VorbisDecoder(STEREO)
prefix_value, prefix_total = 2166136261, 0
while prefix_total < total:
    r, buf = audiocore.get_buffer(full)
    data = bytes(buf)[:total - prefix_total]
    prefix_value = fnv(prefix_value, data)
    prefix_total += len(data)
check("cut plays the file's start", prefix_value == value)
full.deinit()
audiocore.reset_buffer(cut)
check("cut rewinds", lap(cut) == (result, total, value))
cut.deinit()

# --- not Vorbis --------------------------------------------------------------

print("not vorbis")
wav = (b"RIFF" + (36 + 400).to_bytes(4, "little") + b"WAVEfmt "
       + (16).to_bytes(4, "little") + (1).to_bytes(2, "little")
       + (1).to_bytes(2, "little") + (8000).to_bytes(4, "little")
       + (16000).to_bytes(4, "little") + (2).to_bytes(2, "little")
       + (16).to_bytes(2, "little") + b"data" + (400).to_bytes(4, "little")
       + bytes(400))
damaged = bytearray(whole)
at = damaged.find(b"vorbis")
damaged[at:at + 6] = b"VORBIS"
noise = bytes((i * 73 + 41) & 0xFF for i in range(20000))


def ogg_crc(page):
    crc = 0
    for byte in page:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7 if crc & 0x80000000 else crc << 1) & 0xFFFFFFFF
    return crc


def huge_codebook(data):
    """The fixture with its first codebook claiming 65535 dimensions and
    16 million entries, its page checksum made right again so the claim is
    read. Tremor sized stack buffers from those two numbers."""
    data = bytearray(data)
    book = data.find(b"BCV")
    page = data.rfind(b"OggS", 0, book)
    segments = data[page + 26]
    length = 27 + segments + sum(data[page + 27:page + 27 + segments])
    data[book + 3:book + 5] = b"\xff\xff"          # dimensions
    data[book + 5:book + 8] = b"\xff\xff\xff"      # entries
    data[page + 22:page + 26] = bytes(4)
    data[page + 22:page + 26] = ogg_crc(data[page:page + length]).to_bytes(4, "little")
    return bytes(data)


for name, data in (("a WAV file", wav), ("an MP3 file", read_bytes(MP3)),
                   ("damaged Vorbis header", bytes(damaged)),
                   ("noise", noise), ("an empty file", b""),
                   ("an absurd codebook", huge_codebook(whole))):
    ok, kind = refused(lambda: audiovorbis.VorbisDecoder(io.BytesIO(data)))
    check(name, ok, kind)

# --- deinit ------------------------------------------------------------------

print("deinit")
twice = audiovorbis.VorbisDecoder(STEREO)
audiocore.get_buffer(twice)
twice.deinit()
try:
    twice.deinit()
    again = True
except Exception:  # noqa: BLE001
    again = False
check("deinit twice", again)
check("released raises ValueError", refused(lambda: twice.channel_count) == (False, "ValueError"))
try:
    # CircuitPython's get_buffer hands a released sample's error back as a
    # result rather than raising; either is a refusal.
    result, buf = audiocore.get_buffer(twice)
    pulled = "ERROR" if result == ERROR else "result %d" % result
except ValueError:
    pulled = "ValueError"
check("released pull is refused", pulled in ("ValueError", "ERROR"), pulled)

# --- the pump ----------------------------------------------------------------

print("pump")
try:
    import audiopump
except ImportError:
    audiopump = None
if audiopump is None:
    print("  no audiopump here")
else:
    graph = audiomixer.Mixer(voice_count=2, sample_rate=44100, channel_count=2,
                             bits_per_sample=16, samples_signed=True,
                             buffer_size=1024)
    source = audiovorbis.VorbisDecoder(STEREO)
    graph.voice[1].play(source)
    try:
        audiopump.spawn(graph, 4, bytearray(audiopump.STATUS_BYTES))
        audiopump.shutdown()
        said = "accepted"
    except ValueError as error:
        said = "VorbisDecoder" in str(error) and "refused" or str(error)
    check("refused behind a Mixer", said == "refused", said)
    graph.voice[1].stop()
    source.deinit()
    graph.deinit()

if failures:
    print("VORBIS FAIL:", ", ".join(failures))
    sys.exit(1)
print("VORBIS PASS")
