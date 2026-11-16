# Adafruit_MP3 patch queue

The patches audiodsp applies to the Adafruit_MP3 decoder. Applied by
[`scripts/fetch_deps.sh`](../../scripts/fetch_deps.sh) on top of the pinned Adafruit_MP3 commit
declared in `DEPENDENCIES.lock`.

- `0001-windows-msvc-inline-assembly.patch` — `src/assembly.h` guards its
  MSVC-only inline-asm branch on `defined _WIN32`, which mingw-w64 GCC
  (the Windows MicroPython target) also defines; the patch additionally
  excludes `__GNUC__` so mingw falls through to the portable C path.
  CircuitPython never hits this (no Windows port). Full rationale:
  [`docs/dev/upstream-record.md`](../../docs/dev/upstream-record.md#audiomp3-three-windows-only-local-fixes-no-unix-impact).
- `0002-msvc-x64-portable-path.patch` — 64-bit MSVC, which builds the
  Windows CPython wheel, has no inline assembly, so `src/assembly.h` also
  sends `_WIN64` to the portable C path, with an MSVC `CLZ`. No firmware
  build is compiled by MSVC, so none changes.

`setup.py` applies the same queue when it fetches the decoder for a wheel.
