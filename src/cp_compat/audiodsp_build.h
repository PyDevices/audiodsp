// Which audiodsp a firmware was built from, readable on the device.
//
// audiodsp#55 asked for this: a board digest moved between two firmwares and
// neither recorded the audiodsp revision it came from, so what moved was not
// attributable. `os.uname().version` answers for MicroPython and says nothing
// about us. It bit again on 2026-09-09: the boards' firmware had been built
// from a throwaway source tree during the pin move, so the .bin files on disk
// were not the flashed ones and the commit was unrecoverable from the
// filesystem entirely.
//
// The two strings are defined ONCE (audiodsp_build.c) and every module points at
// them, so the cost is two `mp_obj_str_t` plus two table entries per module
// rather than two objects per module.
//
// NOT in src/shared/: that is deliberately runtime-neutral C so the CPython
// extension can compile it, and `mp_obj_str_t` is a MicroPython type. The
// CPython target answers the same question through `_audiodsp.__version__` and
// `_audiodsp.__revision__`, which the Python twins re-export.
//
// Scope: the NINE modules that are ours. The ported CircuitPython modules
// (audiocore, audiodelays, audiofilters, audiofreeverb, audiomixer, audiomp3,
// audiospeed, synthio) are left alone -- adding a dunder to upstream's
// namespace would be a deviation from its surface, and any of our nine answers
// the question, since they are always built together (Brad, 2026-09-09).

#pragma once

#include "py/obj.h"
#include "py/objstr.h"  // mp_obj_str_t

extern const mp_obj_str_t audiodsp_version_obj;
extern const mp_obj_str_t audiodsp_revision_obj;

// One line inside a module's existing globals table.
#define AUDIODSP_BUILD_GLOBALS \
    { MP_ROM_QSTR(MP_QSTR___version__), MP_ROM_PTR(&audiodsp_version_obj) }, \
    { MP_ROM_QSTR(MP_QSTR___revision__), MP_ROM_PTR(&audiodsp_revision_obj) }
