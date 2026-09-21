// audioroute.Port for CircuitPython. See Port.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audioroute/Port.h"

// Zero copy, and that is the point: the source's own pointer, its own length
// and its own result, handed straight back. A port that copied a block would
// cost 1 KB of memmove per block on every class in the palette, and would
// have to own a buffer big enough for the biggest source anyone ever points
// it at.
audioio_get_buffer_result_t audioroute_port_get_buffer(
    audioroute_port_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    // One read of one word. Whatever `play()` does to it, this sees one of
    // the two values and never half of either.
    mp_obj_t source = self->source;
    if (source == mp_const_none) {
        *buffer = NULL;
        *buffer_length = 0;
        return GET_BUFFER_ERROR;
    }
    // Through CircuitPython's own funnel, not through the protocol directly,
    // for the same reason the MicroPython twin uses audiodsp's: the funnel
    // carries the deinit guard, so a class that releases the node behind its
    // port gets GET_BUFFER_ERROR instead of a read of freed buffers. On this
    // build the funnel is stock `shared-module/audiocore/__init__.c` and it
    // still raises exactly where CircuitPython raises -- there is no fault
    // register here, because there is no thread that cannot raise.
    return audiosample_get_buffer(source, single_channel_output, channel,
        buffer, buffer_length);
}

// Forwarded, and it has to be.
//
// Before the port, a consumer held the tail node itself and `MixerVoice.play()`
// rewound THAT. If the port swallowed the rewind, a delay line or a filter
// would start a playback with the last take still in it, and the bytes would
// move -- which is the one thing a pass-through is not allowed to do.
//
// What this does NOT do is walk into a component's borrowed source on its own
// account: the port forwards one step, to whatever it is playing, and if that
// is the borrowed source then the consumer asked for exactly what it would
// have got by holding the source directly.
void audioroute_port_reset_buffer(audioroute_port_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    mp_obj_t source = self->source;
    if (source == mp_const_none) {
        return;
    }
    audiosample_reset_buffer(source, single_channel_output, channel);
}
