# Ogg Vorbis: `audiovorbis`

`audiovorbis.VorbisDecoder` plays an Ogg Vorbis file the way
`audiomp3.MP3Decoder` plays an MP3. CircuitPython has no Vorbis module, so this
one is PyDevices' own; it builds on MicroPython (unix, Windows, ESP32 and the
other ports audiodsp builds for), into the CPython wheel, and into
CircuitPython as a user C module.

```python
import audiocore, audiomixer, audiovorbis

song = audiovorbis.VorbisDecoder("song.ogg")      # a path, or a file opened "rb"
print(song.sample_rate, song.channel_count)       # from the file
mixer.voice[0].play(song, loop=True)
```

## What it has, and how it differs from `MP3Decoder`

The constructor takes a file or a path and an optional buffer, and the object
has `file` (settable), `open(path)`, `rms_level`, `samples_decoded`,
`sample_rate`, `channel_count`, `bits_per_sample`, `deinit()` and `with`,
exactly as `MP3Decoder` does. It always hands out signed 16-bit samples.

Where it behaves differently, on purpose:

- **Mono and stereo only.** A file with more channels is refused with
  `ValueError`, because nothing downstream of an audio sample takes more
  than two.
- **The buffer** holds decoded audio only (Tremor reads the file itself, so
  there is no input buffer). Each pull hands out half of it, in whole stereo
  frames; a buffer whose halves are under 256 bytes isn't used. Without one,
  each pull is 1024 frames.
- **A rewind always rewinds.** `reset_buffer()` goes back to the start
  whenever anything has played. `MP3Decoder` rewinds only once it has reached
  the end, so an MP3 played half way and played again carries on.
- **A stream that fails mid-play** ends the playback with `GET_BUFFER_ERROR`
  rather than raising inside the pull. Opening a file still raises the
  stream's own exception.
- **Errors opening a file** are `RuntimeError("Failed to parse Vorbis file:
  ...")`, naming what was wrong: not Ogg Vorbis, damaged headers, a version
  it can't read.

Like an MP3, it reads its file while it plays, so `audiopump` refuses it at
the door wherever it sits in a graph. Decode into a ring from the interpreter
instead.

A chained file (several Ogg streams back to back) plays through as long as
every link has the first one's rate and channel count, and ends at the first
link that doesn't.

## The decoder, and decoding from C

The decoding is Xiph's Tremor, the lowmem branch: integer-only, BSD-3-Clause,
carried in [third_party/tremor](../third_party/tremor) with a few small changes
with its [ORIGIN.txt](../third_party/tremor/ORIGIN.txt). It is compiled as one
translation unit, [src/shared/audiodsp_vorbis.c](../src/shared/audiodsp_vorbis.c),
which:

- renames every symbol Tremor defines to `audiodsp_tremor_*`
  ([audiodsp_tremor_names.h](../src/shared/audiodsp_tremor_names.h)), so a
  firmware can link another copy of Tremor beside it;
- keeps Tremor's warnings to Tremor's code, so a port that builds with
  `-Werror` still checks ours;
- routes Tremor's allocations through the bound driver (below).

Any C module in the same firmware can decode through it with
[src/shared/audiodsp_vorbis.h](../src/shared/audiodsp_vorbis.h): give it a
read callback (and seek and tell, for a stream that can), then pull
interleaved int16 frames. It never touches an interpreter, so it can run on
a thread of its own; one decoder belongs to one thread at a time.

```c
static long my_read(void *s, void *buf, size_t len);   // bytes, 0 at the end, -1 on error
static const audiodsp_vorbis_io_t io = { my_read, NULL, NULL };

int err;
audiodsp_vorbis_t *v = audiodsp_vorbis_open(&io, my_stream, &err);
long n;
int16_t pcm[1024 * 2];
while ((n = audiodsp_vorbis_read(v, pcm, 1024)) > 0) { /* n frames */ }
audiodsp_vorbis_close(v);
```

## Memory

Nothing comes from the MicroPython or CPython heap. Tremor allocates through
the bound driver's memory hooks (`mem_alloc`, `mem_realloc` and `mem_free` in
[audiodsp_port.h](../src/shared/audiodsp_port.h)), and through the C
library's `malloc` when there are none. A driver for a board with PSRAM puts
the decoder there: it is tens of kilobytes in many small blocks that no DMA
touches, and internal RAM is what Wi-Fi and DMA can't do without. Because it
isn't on the MicroPython heap, `gc.mem_free()` doesn't see it; `deinit()`, or
the finaliser at a soft reset, frees it.

What a stream needs, measured on a 64-bit desktop with the decoder's own
counter (`AUDIODSP_VORBIS_HEAP_STATS`); a 32-bit board needs somewhat less,
since many of the blocks are tables of pointers:

| File | Heap after opening | Heap at most |
|---|---|---|
| mono 44.1 kHz, 48 kbit/s | 24 KB | 30 KB |
| stereo 48 kHz, 500 kbit/s | 83 KB | 96 KB |

On a Waveshare ESP32-P4 DEV-KIT, a 44.1 kHz stereo file at 112 kbit/s took
31 KB to open and 47 KB once playing, every byte of it in PSRAM and none in
internal RAM; five open, decode and `deinit()` cycles left both exactly where
they started.

Most of it is the stream's codebooks, which a higher bitrate makes larger,
and the rest is two blocks of decode state per channel. On top of that the
binding keeps two output buffers on the interpreter's heap: 8 KB without a
`buffer`.

Stack: Tremor's call chains are about 1 KB deep on RISC-V, and a packet
puts up to about 2 KB more of scratch on the stack while it decodes; under
that sits the stream's own read. Unpacking a stream's codebooks used to take
scratch on the stack sized by the codebook, which on the ESP32's 16 KB
Python stack crashed a board when an ordinary file was opened from deep in
a call chain, so that scratch comes from the heap now (and a codebook too
large to be real is refused). Tremor doesn't check its own depth, so the
binding checks before every call into it that 6 KB are left: a program that
deep gets `RuntimeError("not enough stack to open a Vorbis file here")`, or
a pull that returns `GET_BUFFER_ERROR`, rather than a crashed board.

Flash: the decoder is about 90 KB of code and tables on an ESP32-P4
(`riscv32-esp-elf-size` on `audiodsp_vorbis.c`'s object), most of it the
MDCT and window lookup tables.

Tremor doesn't check for a failed allocation. A board that runs out of
memory while opening or decoding a file fails the way it would in any other
C library that doesn't, so leave it the room above.

## Tests

- [tests/parity/vorbis_probe.py](../tests/parity/vorbis_probe.py) runs on
  MicroPython, CircuitPython and CPython from two small fixtures: the
  attributes, a rewound lap against the first, a `Mixer` voice playing the
  file unchanged and looping it, a file cut off in its headers and in its
  audio, things that aren't Vorbis, `deinit()` twice, and the pump's refusal.
  `--fault` skips the rewind and must fail.
- [tests/parity/verify_vorbis.py](../tests/parity/verify_vorbis.py) makes
  files with ffmpeg (mono and stereo, 44.1 and 48 kHz, 4 s and 3 minutes, from
  the lowest libvorbis quality to the highest), decodes them with ffmpeg, and
  compares every sample. Tremor and ffmpeg land on 16 bits differently:
  Tremor's shift rounds down, ffmpeg rounds to nearest, so half the samples
  are one step apart (RMS 0.71); none has been more than 2 apart, and the
  gate is 3. Every interpreter must decode the same bytes. `--fault swap`
  (left and right exchanged) and `--fault drop` (a block lost) must fail.
- [tests/parity/vorbis_symbols_check.py](../tests/parity/vorbis_symbols_check.py)
  compiles the decoder alone and fails on any global symbol outside
  `audiodsp_`; `--fault` plants one.
