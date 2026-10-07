# Roadmap

audiodsp is heading toward more of the audio graph on more boards, with the
audio pump holding up under live use such as a stompbox or a stage rig.

## Next

- Patch changes that take longer than the audio ring fade out and back in,
  rather than holding the audio while a new effect or rack is built.

## Later

- An RP2 pump driver: the pump on the second core with an I2S (PIO) sink, so
  RP2 boards get the same off-thread audio as ESP32. See
  [docs/pump-ports.md](docs/pump-ports.md) for how a port plugs in.
- An LC3 codec module beside `audiomp3`, frame by frame: encode PCM to one
  frame, decode a frame (or a lost one) to PCM. Leaves room for LE Audio later.

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/audiodsp/issues).
