# Decode Ogg Vorbis


Shaped like `audiomp3.MP3Decoder`, so it plays wherever an MP3 does: through
`audiocore.get_buffer()`, a `Mixer` voice, or a board's audio output.

```python
import audiovorbis
song = audiovorbis.VorbisDecoder("song.ogg")   # or an open binary file
mixer.voice[0].play(song, loop=True)
```

It takes the same arguments (a file or a path, and an optional buffer) and
has the same `file`, `open()`, `rms_level`, `samples_decoded`, `deinit()`
and `with` as `MP3Decoder`. Mono and stereo files play; `sample_rate` and
`channel_count` come from the file. Like an MP3, it reads its file while it
plays, so `audiopump` refuses it: feed the pump from the interpreter instead.

The decoder is Xiph's Tremor, integer-only, carried in
[third_party/tremor](../../third_party/tremor) under its own BSD-3-Clause licence.
It decodes to the same samples on every build, and within two steps of
ffmpeg's float decoder. Other C modules in the same firmware can decode a
stream through it too. [The C interface page](../reference/vorbis.md) has the C interface,
what it needs of memory on a board, and how it's tested.

