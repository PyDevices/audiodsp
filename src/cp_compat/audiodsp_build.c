// The two build strings, defined once. See audiodsp_build.h.
//
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "cp_compat/audiodsp_build.h"

#include "py/objstr.h"

// Passed by micropython.mk / micropython.cmake. A build that reaches here
// without them says so rather than claiming a revision it does not know --
// "unknown" is the honest answer and the one that made this file necessary.
#ifndef AUDIODSP_VERSION
#define AUDIODSP_VERSION "0.0.0+unknown"
#endif
#ifndef AUDIODSP_REVISION
#define AUDIODSP_REVISION "unknown"
#endif

const MP_DEFINE_STR_OBJ(audiodsp_version_obj, AUDIODSP_VERSION);
const MP_DEFINE_STR_OBJ(audiodsp_revision_obj, AUDIODSP_REVISION);
