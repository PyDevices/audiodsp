// No fused multiply-add in this translation unit. See below for why.
// SPDX-License-Identifier: MIT
#ifndef AUDIOIF_FP_CONTRACT_H
#define AUDIOIF_FP_CONTRACT_H

// `a * b + c` may be emitted as multiply-round-add-round, or as one fused
// instruction that rounds once. Both are legal C, they differ in the last bit,
// and which one a compiler picks depends on the target. The same source
// therefore computes different floats on the P4's RISC-V and the S3's Xtensa --
// and the two ESP toolchains do not even fuse at the same number of sites:
//
//     xtensa-esp32s3-elf-gcc -O2   audioif_feedback_delay.c   17 madd.s/msub.s
//     riscv32-esp-elf-gcc    -O2   audioif_feedback_delay.c   20 fmadd.s/...
//     xtensa-esp32s3-elf-gcc -O2   audioif_ladder.c           13
//     riscv32-esp-elf-gcc    -O2   audioif_ladder.c           15
//
// That was the whole of the P4-vs-S3 split on `audiodynamics`' transient attack
// path (audioif#66, 29 against 30 sites there), which this file generalises to
// the rest of the kernel (audioif#79). `docs/correctness-standard.md` holds our
// nodes to every target rendering them identically; contraction is one of the
// few ways the same C can fail that without anybody writing a bug.
//
// TWO THINGS THAT DO NOT WORK, both measured rather than assumed:
//
//   * `#pragma STDC FP_CONTRACT OFF`, the standard spelling and the one to
//     reach for first, is silently IGNORED by GCC. It still emits vfmadd.
//   * `-ffp-contract=off` as a build flag. On the CMake ports audioif's flags
//     ride on `usermod_mpaudio`, an INTERFACE library, so the flag would land
//     on everything that links it -- all of MicroPython -- to fix two dozen
//     files of ours. Per-file is the point. (`setup.py` does pass the flag on
//     macOS, where clang's default is `on` and AArch64 fuses by baseline; that
//     predates this header and is belt-and-braces now rather than the
//     mechanism.)
//
// INCLUDE THIS FIRST, AND ONLY FROM A `.c`. `#pragma GCC optimize` applies from
// where it appears to the end of the translation unit, so it has to come before
// the code it governs; and putting it in a leaf header keeps it from riding
// into a translation unit that never asked for it. No `audioif_*.h` includes
// this one.
//
// `#pragma GCC optimize` is known to reset a file's other optimisation settings
// on some GCC versions. It does not here: checked per file on both ESP
// toolchains, instruction counts unchanged except for the fused ops
// disappearing. Check it again after a toolchain bump rather than trusting this
// paragraph.
//
// The four `src/shared/` files that do not include this -- `audioif_sample.c`,
// `audioif_splitter.c`, `audioif_remix.c`, `audioif_synth_dsp.c` -- have no
// floating-point arithmetic at all. Every other one does, whether or not it
// contracts today, because a file that starts contracting after a code change
// should not also need somebody to remember this.

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("fp-contract=off")
#elif defined(__clang__)
#pragma clang fp contract(off)
#endif

#endif  // AUDIOIF_FP_CONTRACT_H
