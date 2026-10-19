# Roadmap

audiodsp is heading toward more of the audio graph on more boards, with the
audio pump holding up under live use such as a stompbox or a stage rig.

## Later

- An RP2 pump driver: the pump on the second core with an I2S (PIO) sink, so
  RP2 boards get the same off-thread audio as ESP32. See
  [docs/pump-ports.md](docs/pump-ports.md) for how a port plugs in.
- An LC3 codec module beside `audiomp3`, frame by frame: encode PCM to one
  frame, decode a frame (or a lost one) to PCM. Leaves room for LE Audio later.
- A level ramp on `audiomixer` voices: a level move ramps over a set number of
  frames instead of stepping, so an effect can fade its wet path in or out
  without a click when you switch it in mid-note.
- More sampler voices: `audiosampler.Sampler` caps at 64 voices
  (`SAMPLER_MAX_VOICES`), while 64 voices render in under half a millisecond per
  21 ms period on a desktop. A dense General MIDI piece wants more.
- A short fade on a sampler voice's `stop()`, so stealing a voice to make room for
  a new note doesn't click.

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/audiodsp/issues).
