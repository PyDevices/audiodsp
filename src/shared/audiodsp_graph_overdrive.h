// The graph's OVERDRIVE node. See audiodsp_graph.h.
// SPDX-License-Identifier: MIT
//
// It lives apart from the rest of the runner because it needs
// double-precision pow() and log10(), and through audiodsp_distortion.c and
// audiodsp_biquad.c more of libm, which CircuitPython's RP2040 build cannot
// link. A build that leaves this file out defines
// AUDIODSP_GRAPH_HAS_OVERDRIVE to 0, and the graph then refuses the kind.

#ifndef AUDIODSP_GRAPH_OVERDRIVE_H
#define AUDIODSP_GRAPH_OVERDRIVE_H

#include <stddef.h>
#include <stdint.h>

#ifndef AUDIODSP_GRAPH_HAS_OVERDRIVE
#define AUDIODSP_GRAPH_HAS_OVERDRIVE (1)
#endif

size_t audiodsp_graph_overdrive_size(void);
void audiodsp_graph_overdrive_reset(void *data);
// `param` is the node's AUDIODSP_GRAPH_OVERDRIVE_* parameters.
void audiodsp_graph_overdrive_configure(void *data, const float *param,
    uint32_t sample_rate);
// `in` is NULL for an unconnected input, which reads as silence.
void audiodsp_graph_overdrive_run(void *data, const int16_t *in, int16_t *out,
    uint32_t frames);

#endif
