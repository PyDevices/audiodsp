# Adafruit_MP3 patch queue

Owned by audiodsp (Phase 2 of the modernization program moved ownership
here from the parent workspace's own patches/adafruit_mp3). Applied by
`scripts/fetch_deps.sh` on top of the pinned Adafruit_MP3 commit
declared in `DEPENDENCIES.lock`.

- `0001-windows-msvc-inline-assembly.patch` — `src/assembly.h` guards its
  MSVC-only inline-asm branch on `defined _WIN32`, which mingw-w64 GCC
  (the Windows MicroPython target) also defines; the patch additionally
  excludes `__GNUC__` so mingw falls through to the portable C path.
  CircuitPython never hits this (no Windows port). Full rationale:
  `docs/upstream-diff.md`, "Tier 5 audiomp3".
