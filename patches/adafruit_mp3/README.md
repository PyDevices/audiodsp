# Adafruit_MP3 patch queue

Owned by audiodsp (Phase 2 of the modernization program moved ownership
here from the parent workspace's own patches/adafruit_mp3). Applied by
[`scripts/fetch_deps.sh`](../../scripts/fetch_deps.sh) on top of the pinned Adafruit_MP3 commit
declared in `DEPENDENCIES.lock`.

- `0001-windows-msvc-inline-assembly.patch` — `src/assembly.h` guards its
  MSVC-only inline-asm branch on `defined _WIN32`, which mingw-w64 GCC
  (the Windows MicroPython target) also defines; the patch additionally
  excludes `__GNUC__` so mingw falls through to the portable C path.
  CircuitPython never hits this (no Windows port). Full rationale:
  [`docs/how-it-works/upstream-diff.md`](../../docs/how-it-works/upstream-diff.md), "Tier 5 audiomp3".
- `0002-msvc-x64-portable-path.patch` — 64-bit MSVC, which builds the
  Windows CPython wheel, has no inline assembly, so `src/assembly.h` also
  sends `_WIN64` to the portable C path, with an MSVC `CLZ`. No firmware
  build is compiled by MSVC, so none changes.

`setup.py` applies the same queue when it fetches the decoder for a wheel.
