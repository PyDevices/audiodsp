// Module skeleton for audiodsp: registers the top-level modules, each
// starting empty (just __name__). A module's globals table is filled in here
// as its ported classes and functions land, so `import <name>` succeeds from
// the start of the work on it.
//
// This is new code (not a CircuitPython port): CircuitPython doesn't need
// an explicit module-registration file since its module tables are wired
// through its own build-time manifest system instead of MP_REGISTER_MODULE.

#include "py/obj.h"
#include "py/runtime.h"

// audiocore has real content now -- see audiocore/module.c.
// synthio has real content now -- see synthio/__init__.c.
// audiomixer has real content now -- see audiomixer/module.c.

// audiofilters has real content now -- see audiofilters/module.c.

// audiodelays has real content now -- see audiodelays/module.c.

// audiofreeverb has real content now -- see audiofreeverb/module.c.

// audiospeed has real content now -- see audiospeed/module.c.

// audiomp3 has real content now -- see audiomp3/module.c.

// audioladder has real content now -- see audioladder/module.c.

// audiodynamics / audioroute are the first modules here that are not
// CircuitPython ports at all -- they come from micropython-vst3's `vstaudio`
// usermod. See audiodynamics/module.c, audioroute/module.c.

// audiomath, audioecho, audioshaper, audioconvolve, audiobiquad,
// audioverb, audiomodal, audiooscillator and audiosampler are audiodsp's own, with
// no ancestor anywhere.
// See each module.c.
