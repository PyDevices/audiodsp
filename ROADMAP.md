# Roadmap

audiodsp is heading toward more of the audio graph on more boards, with the
audio pump holding up under live use such as a stompbox or a stage rig.

## Later

- An RP2 pump driver: the pump on the second core with an I2S (PIO) sink, so
  RP2 boards get the same off-thread audio as ESP32. See
  [docs/how-it-works/pump-ports.md](docs/how-it-works/pump-ports.md) for how a port plugs in.
- `audiograph`: `set()` at a frame inside a block, so a note lands on its own
  sample rather than at the top of the next block.
- An LC3 codec module beside `audiomp3`, frame by frame: encode PCM to one
  frame, decode a frame (or a lost one) to PCM. Leaves room for LE Audio later.
- A level ramp on `audiomixer` voices: a level move ramps over a set number of
  frames instead of stepping, so an effect can fade its wet path in or out
  without a click when you switch it in mid-note.

## Housekeeping

- `.pyi` stubs for every module, so your editor completes audiodsp's API and
  the Reference pages are generated from them rather than written by hand.
- The CircuitPython glue's folder takes a lasting name (`src/circuitpython_spike/`
  becomes `src/circuitpython/`), and `CIRCUITPYTHON_ORACLE` joins `DEPENDENCIES.lock`.

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/audiodsp/issues).
