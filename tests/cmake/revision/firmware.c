// The two strings audiodsp_build.c would put in a firmware. See CMakeLists.txt.
#include <stdio.h>

#ifdef AUDIODSP_REVISION_HEADER
#include "audiodsp_revision.h"
#endif
#ifndef AUDIODSP_VERSION
#define AUDIODSP_VERSION "0.0.0+unknown"
#endif
#ifndef AUDIODSP_REVISION
#define AUDIODSP_REVISION "unknown"
#endif

int main(void) {
    printf("%s %s\n", AUDIODSP_VERSION, AUDIODSP_REVISION);
    return 0;
}
