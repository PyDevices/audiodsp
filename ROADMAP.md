# Roadmap

audiodsp is heading toward more of the audio graph on more boards, with the
audio pump holding up under live use such as a stompbox or a stage rig.

## Later

- An RP2 pump driver: the pump on the second core with an I2S (PIO) sink, so
  RP2 boards get the same off-thread audio as ESP32. See
  [docs/pump-ports.md](docs/pump-ports.md) for how a port plugs in.
- An LC3 codec module beside `audiomp3`, frame by frame: encode PCM to one
  frame, decode a frame (or a lost one) to PCM. Leaves room for LE Audio later.
- `audiograph` on CPython and CircuitPython: the graph runner's core is
  runtime-neutral C, so each is a shim over the same calls, and parity tests
  across the three can then pin a graph's render on the desktop to the board's.
- A level ramp on `audiomixer` voices: a level move ramps over a set number of
  frames instead of stepping, so an effect can fade its wet path in or out
  without a click when you switch it in mid-note.

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/audiodsp/issues).
