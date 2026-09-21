// Ported from CircuitPython's shared-module/audiofilters/__init__.c
// (upstream repo: https://github.com/adafruit/circuitpython, MIT).
// The assign path keeps Filter.c's iterable-to-tuple conversion (upstream
// only accepts a tuple). process is the per-stage loop over
// synthio_biquad_filter_sample; states are sized objs_len * channel_count
// and indexed [stage * channel_count + channel]. An empty chain returns
// the word unchanged.
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Cooper Dalrymple
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiofilters/__init__.h"

#include "cp_compat/argcheck.h"

#include "py/objtuple.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

void audiofilters_assign_filter_chain(audiofilters_filter_chain_t *self, mp_obj_t filter_in, uint8_t channel_count) {
    size_t n_items;
    mp_obj_t *items;

    if (filter_in == mp_const_none) {
        n_items = 0;
        items = NULL;
    } else if (MP_OBJ_TYPE_HAS_SLOT(mp_obj_get_type(filter_in), iter)) {
        filter_in = MP_OBJ_TYPE_GET_SLOT(&mp_type_tuple, make_new)(
            &mp_type_tuple, 1, 0, &filter_in);
        mp_obj_tuple_get(filter_in, &n_items, &items);
        for (size_t i = 0; i < n_items; i++) {
            if (!mp_obj_is_type(items[i], &synthio_biquad_type_obj)) {
                mp_raise_TypeError_varg(
                    MP_ERROR_TEXT("%q in %q must be of type %q, not %q"),
                    MP_QSTR_object,
                    MP_QSTR_filter,
                    MP_QSTR_Biquad,
                    mp_obj_get_type(items[i])->name);
            }
        }
    } else {
        n_items = 1;
        if (!mp_obj_is_type(filter_in, &synthio_biquad_type_obj)) {
            mp_raise_TypeError_varg(
                MP_ERROR_TEXT("%q must be of type %q or %q, not %q"),
                MP_QSTR_filter, MP_QSTR_Biquad, MP_QSTR_iterable, mp_obj_get_type(filter_in)->name);
        }
        items = &self->obj;
    }

    // The allocation happens first and lands in a local, because m_renew can
    // collect and the lock may not be held across a collection. Then one
    // locked store publishes {states, objs, objs_len} together: the pull
    // indexes states[j * channel_count + channel] with objs_len as its bound,
    // so a pointer published ahead of its length is a read off the end of a
    // shrunken array. Eight control-path entries reach this one function
    // (Filter.filter, Echo.filter, Freeverb.pre_filter and .post_filter, and
    // the four deinits below), so it is fixed once here rather than in each.
    biquad_filter_state *states = m_renew(biquad_filter_state,
        self->states,
        self->objs_len * channel_count,
        n_items * channel_count);
    if (items == &self->obj) {
        // The single-filter case borrows the address of self->obj itself, so
        // the pointer has to be taken after the store, not before.
        audiodsp_pump_lock_acquire();
        self->obj = filter_in;
        self->states = states;
        self->objs = &self->obj;
        self->objs_len = n_items;
        audiodsp_pump_lock_release();
        return;
    }
    audiodsp_pump_lock_acquire();
    self->obj = filter_in;
    self->states = states;
    self->objs = items;
    self->objs_len = n_items;
    audiodsp_pump_lock_release();
}

void audiofilters_reset_filter_chain(audiofilters_filter_chain_t *self, uint8_t channel_count) {
    if (self->states) {
        size_t total = self->objs_len * channel_count;
        for (size_t i = 0; i < total; i++) {
            synthio_biquad_filter_reset(&self->states[i]);
        }
    }
}

void audiofilters_tick_filter_chain(audiofilters_filter_chain_t *self) {
    for (uint8_t j = 0; j < self->objs_len; j++) {
        common_hal_synthio_biquad_tick(self->objs[j]);
    }
}

int32_t audiofilters_process_filter_chain(audiofilters_filter_chain_t *self, uint8_t channel_count, uint8_t channel, int32_t word) {
    for (uint8_t j = 0; j < self->objs_len; j++) {
        word = synthio_biquad_filter_sample(self->objs[j], &self->states[j * channel_count + channel], word);
    }
    return word;
}

void audiofilters_deinit_filter_chain(audiofilters_filter_chain_t *self) {
    // objs_len first would leave the pull looping over a NULL objs; objs
    // first would leave it looping the old count over NULL. Neither, under
    // the lock.
    audiodsp_pump_lock_acquire();
    self->obj = mp_const_none;
    self->objs = NULL;
    self->objs_len = 0;
    self->states = NULL;
    audiodsp_pump_lock_release();
}
