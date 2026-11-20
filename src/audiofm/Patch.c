// audiofm.Patch and audiofm.Operator. See Patch.h.
// SPDX-License-Identifier: MIT

#include "audiofm/Patch.h"

#include <string.h>

#include "cp_compat/objproperty.h"
#include "py/objstr.h"
#include "py/runtime.h"

// How a field is read and written: one byte, a flag, or four bytes as a
// tuple.
enum { FIELD_BYTE, FIELD_FLAG, FIELD_FOUR };

// A field is a constant object in the type's locals dict, so its name shows
// in dir() like any attribute, and the attr handler below reads and writes
// the byte it points at. One small ROM object per field, where a property
// would cost a getter and a setter each.
typedef struct {
    mp_obj_base_t base;
    uint8_t offset;
    uint8_t kind;
} audiofm_field_obj_t;

static MP_DEFINE_CONST_OBJ_TYPE(audiofm_field_type, MP_QSTR_field,
    MP_TYPE_FLAG_NONE);

#define FIELD(name, offset, kind) \
    static const audiofm_field_obj_t audiofm_field_##name = \
    { { &audiofm_field_type }, (offset), (kind) }

FIELD(rates, AUDIODSP_FM_OP_RATES, FIELD_FOUR);
FIELD(levels, AUDIODSP_FM_OP_LEVELS, FIELD_FOUR);
FIELD(breakpoint, AUDIODSP_FM_OP_BREAKPOINT, FIELD_BYTE);
FIELD(left_depth, AUDIODSP_FM_OP_LEFT_DEPTH, FIELD_BYTE);
FIELD(right_depth, AUDIODSP_FM_OP_RIGHT_DEPTH, FIELD_BYTE);
FIELD(left_curve, AUDIODSP_FM_OP_LEFT_CURVE, FIELD_BYTE);
FIELD(right_curve, AUDIODSP_FM_OP_RIGHT_CURVE, FIELD_BYTE);
FIELD(rate_scaling, AUDIODSP_FM_OP_RATE_SCALING, FIELD_BYTE);
FIELD(amp_mod, AUDIODSP_FM_OP_AMP_MOD, FIELD_BYTE);
FIELD(velocity, AUDIODSP_FM_OP_VELOCITY, FIELD_BYTE);
FIELD(level, AUDIODSP_FM_OP_LEVEL, FIELD_BYTE);
FIELD(fixed, AUDIODSP_FM_OP_FIXED, FIELD_FLAG);
FIELD(coarse, AUDIODSP_FM_OP_COARSE, FIELD_BYTE);
FIELD(fine, AUDIODSP_FM_OP_FINE, FIELD_BYTE);
FIELD(detune, AUDIODSP_FM_OP_DETUNE, FIELD_BYTE);

FIELD(pitch_rates, AUDIODSP_FM_PITCH_RATES, FIELD_FOUR);
FIELD(pitch_levels, AUDIODSP_FM_PITCH_LEVELS, FIELD_FOUR);
FIELD(feedback, AUDIODSP_FM_FEEDBACK, FIELD_BYTE);
FIELD(key_sync, AUDIODSP_FM_KEY_SYNC, FIELD_FLAG);
FIELD(lfo_speed, AUDIODSP_FM_LFO_SPEED, FIELD_BYTE);
FIELD(lfo_delay, AUDIODSP_FM_LFO_DELAY, FIELD_BYTE);
FIELD(lfo_pitch_depth, AUDIODSP_FM_LFO_PITCH_DEPTH, FIELD_BYTE);
FIELD(lfo_amp_depth, AUDIODSP_FM_LFO_AMP_DEPTH, FIELD_BYTE);
FIELD(lfo_key_sync, AUDIODSP_FM_LFO_KEY_SYNC, FIELD_FLAG);
FIELD(lfo_wave, AUDIODSP_FM_LFO_WAVE, FIELD_BYTE);
FIELD(lfo_pitch_sens, AUDIODSP_FM_LFO_PITCH_SENS, FIELD_BYTE);
FIELD(transpose, AUDIODSP_FM_TRANSPOSE, FIELD_BYTE);

#define FIELD_ENTRY(name) \
    { MP_ROM_QSTR(MP_QSTR_##name), MP_ROM_PTR(&audiofm_field_##name) }

static mp_obj_t audiofm_field_load(const uint8_t *data, size_t offset,
    uint8_t kind) {
    if (kind == FIELD_FOUR) {
        mp_obj_t items[4];
        for (int i = 0; i < 4; i++) {
            items[i] = MP_OBJ_NEW_SMALL_INT(data[offset + i]);
        }
        return mp_obj_new_tuple(4, items);
    }
    if (kind == FIELD_FLAG) {
        return mp_obj_new_bool(data[offset] != 0);
    }
    return MP_OBJ_NEW_SMALL_INT(data[offset]);
}

// Checks every value before writing any, so a bad fourth rate leaves the
// first three as they were.
static void audiofm_field_store(uint8_t *data, size_t offset, uint8_t kind,
    qstr name, mp_obj_t value) {
    uint8_t values[4];
    size_t count = 1;
    if (kind == FIELD_FOUR) {
        size_t len = 0;
        mp_obj_t *items = NULL;
        mp_obj_get_array(value, &len, &items);
        if (len != 4) {
            mp_raise_msg_varg(&mp_type_ValueError,
                MP_ERROR_TEXT("%q takes four values"), name);
        }
        count = 4;
        for (size_t i = 0; i < 4; i++) {
            mp_int_t v = mp_obj_get_int(items[i]);
            if (v < 0 || v > audiodsp_fm_patch_max[offset + i]) {
                mp_raise_msg_varg(&mp_type_ValueError,
                    MP_ERROR_TEXT("%q must be 0 to %d"), name,
                    (int)audiodsp_fm_patch_max[offset + i]);
            }
            values[i] = (uint8_t)v;
        }
    } else if (kind == FIELD_FLAG) {
        values[0] = mp_obj_is_true(value) ? 1 : 0;
    } else {
        mp_int_t v = mp_obj_get_int(value);
        if (v < 0 || v > audiodsp_fm_patch_max[offset]) {
            mp_raise_msg_varg(&mp_type_ValueError,
                MP_ERROR_TEXT("%q must be 0 to %d"), name,
                (int)audiodsp_fm_patch_max[offset]);
        }
        values[0] = (uint8_t)v;
    }
    memcpy(data + offset, values, count);
}

// Loads and stores a field; anything else goes on to the properties and
// methods in the locals dict.
static void audiofm_field_attr(mp_obj_t self_in, uint8_t *data, size_t base,
    qstr attr, mp_obj_t *dest) {
    const mp_obj_type_t *type = mp_obj_get_type(self_in);
    mp_map_t *locals = &MP_OBJ_TYPE_GET_SLOT(type, locals_dict)->map;
    mp_map_elem_t *elem = mp_map_lookup(locals, MP_OBJ_NEW_QSTR(attr),
        MP_MAP_LOOKUP);
    if (elem == NULL || !mp_obj_is_type(elem->value, &audiofm_field_type)) {
        cp_compat_attr(self_in, attr, dest);
        return;
    }
    const audiofm_field_obj_t *field = MP_OBJ_TO_PTR(elem->value);
    if (dest[0] == MP_OBJ_NULL) {
        dest[0] = audiofm_field_load(data, base + field->offset, field->kind);
    } else if (dest[1] != MP_OBJ_NULL) {
        audiofm_field_store(data, base + field->offset, field->kind, attr,
            dest[1]);
        dest[0] = MP_OBJ_NULL;
    }
}

// --- Operator ------------------------------------------------------------------

static size_t audiofm_operator_base(const audiofm_operator_obj_t *self) {
    return (size_t)(6 - self->number) * AUDIODSP_FM_OP_BYTES;
}

static void audiofm_operator_attr(mp_obj_t self_in, qstr attr,
    mp_obj_t *dest) {
    audiofm_operator_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiofm_field_attr(self_in, self->patch->data,
        audiofm_operator_base(self), attr, dest);
}

static mp_obj_t audiofm_operator_get_number(mp_obj_t self_in) {
    audiofm_operator_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->number);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_operator_get_number_obj,
    audiofm_operator_get_number);
MP_PROPERTY_GETTER(audiofm_operator_number_obj,
    (mp_obj_t)&audiofm_operator_get_number_obj);

static mp_obj_t audiofm_operator_get_enabled(mp_obj_t self_in) {
    audiofm_operator_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint8_t bit = (uint8_t)(1u << (6 - self->number));
    return mp_obj_new_bool(self->patch->data[AUDIODSP_FM_OP_ENABLE] & bit);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_operator_get_enabled_obj,
    audiofm_operator_get_enabled);

static mp_obj_t audiofm_operator_set_enabled(mp_obj_t self_in,
    mp_obj_t value) {
    audiofm_operator_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint8_t bit = (uint8_t)(1u << (6 - self->number));
    if (mp_obj_is_true(value)) {
        self->patch->data[AUDIODSP_FM_OP_ENABLE] |= bit;
    } else {
        self->patch->data[AUDIODSP_FM_OP_ENABLE] &= (uint8_t)(~bit & 0x3f);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiofm_operator_set_enabled_obj,
    audiofm_operator_set_enabled);
MP_PROPERTY_GETSET(audiofm_operator_enabled_obj,
    (mp_obj_t)&audiofm_operator_get_enabled_obj,
    (mp_obj_t)&audiofm_operator_set_enabled_obj);

static const mp_rom_map_elem_t audiofm_operator_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_number), MP_ROM_PTR(&audiofm_operator_number_obj) },
    { MP_ROM_QSTR(MP_QSTR_enabled),
      MP_ROM_PTR(&audiofm_operator_enabled_obj) },
    FIELD_ENTRY(rates), FIELD_ENTRY(levels), FIELD_ENTRY(breakpoint),
    FIELD_ENTRY(left_depth), FIELD_ENTRY(right_depth),
    FIELD_ENTRY(left_curve), FIELD_ENTRY(right_curve),
    FIELD_ENTRY(rate_scaling), FIELD_ENTRY(amp_mod), FIELD_ENTRY(velocity),
    FIELD_ENTRY(level), FIELD_ENTRY(fixed), FIELD_ENTRY(coarse),
    FIELD_ENTRY(fine), FIELD_ENTRY(detune),
};
static MP_DEFINE_CONST_DICT(audiofm_operator_locals,
    audiofm_operator_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiofm_operator_type,
    MP_QSTR_Operator,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    attr, audiofm_operator_attr,
    locals_dict, &audiofm_operator_locals
    );

// --- Patch ---------------------------------------------------------------------

audiofm_patch_obj_t *audiofm_patch_new(void) {
    audiofm_patch_obj_t *self = mp_obj_malloc(audiofm_patch_obj_t,
        &audiofm_patch_type);
    audiodsp_fm_init_voice(self->data);
    mp_obj_t items[6];
    for (int i = 0; i < 6; i++) {
        audiofm_operator_obj_t *op = mp_obj_malloc(audiofm_operator_obj_t,
            &audiofm_operator_type);
        op->patch = self;
        op->number = (uint8_t)(i + 1);
        items[i] = MP_OBJ_FROM_PTR(op);
    }
    self->operators = mp_obj_new_tuple(6, items);
    return self;
}

static mp_obj_t audiofm_patch_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    (void)type;
    (void)all_args;
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    return MP_OBJ_FROM_PTR(audiofm_patch_new());
}

static void audiofm_patch_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    audiofm_patch_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiofm_field_attr(self_in, self->data, 0, attr, dest);
}

static mp_obj_t audiofm_patch_get_operators(mp_obj_t self_in) {
    audiofm_patch_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return self->operators;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_patch_get_operators_obj,
    audiofm_patch_get_operators);
MP_PROPERTY_GETTER(audiofm_patch_operators_obj,
    (mp_obj_t)&audiofm_patch_get_operators_obj);

static mp_obj_t audiofm_patch_get_algorithm(mp_obj_t self_in) {
    audiofm_patch_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->data[AUDIODSP_FM_ALGORITHM] + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_patch_get_algorithm_obj,
    audiofm_patch_get_algorithm);

static mp_obj_t audiofm_patch_set_algorithm(mp_obj_t self_in, mp_obj_t value) {
    audiofm_patch_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t algorithm = mp_obj_get_int(value);
    if (algorithm < 1 || algorithm > 32) {
        mp_raise_ValueError(MP_ERROR_TEXT("algorithm must be 1 to 32"));
    }
    self->data[AUDIODSP_FM_ALGORITHM] = (uint8_t)(algorithm - 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiofm_patch_set_algorithm_obj,
    audiofm_patch_set_algorithm);
MP_PROPERTY_GETSET(audiofm_patch_algorithm_obj,
    (mp_obj_t)&audiofm_patch_get_algorithm_obj,
    (mp_obj_t)&audiofm_patch_set_algorithm_obj);

static mp_obj_t audiofm_patch_get_name(mp_obj_t self_in) {
    audiofm_patch_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char name[AUDIODSP_FM_NAME_BYTES];
    size_t len = AUDIODSP_FM_NAME_BYTES;
    for (size_t i = 0; i < AUDIODSP_FM_NAME_BYTES; i++) {
        uint8_t c = self->data[AUDIODSP_FM_NAME + i];
        name[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    while (len > 0 && name[len - 1] == ' ') {
        len--;
    }
    return mp_obj_new_str(name, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_patch_get_name_obj,
    audiofm_patch_get_name);

static mp_obj_t audiofm_patch_set_name(mp_obj_t self_in, mp_obj_t value) {
    audiofm_patch_obj_t *self = MP_OBJ_TO_PTR(self_in);
    size_t len = 0;
    const char *text = mp_obj_str_get_data(value, &len);
    if (len > AUDIODSP_FM_NAME_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("a name is at most 10 characters"));
    }
    for (size_t i = 0; i < len; i++) {
        if ((uint8_t)text[i] > 127) {
            mp_raise_ValueError(MP_ERROR_TEXT("a name is ASCII"));
        }
    }
    memset(self->data + AUDIODSP_FM_NAME, ' ', AUDIODSP_FM_NAME_BYTES);
    memcpy(self->data + AUDIODSP_FM_NAME, text, len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiofm_patch_set_name_obj,
    audiofm_patch_set_name);
MP_PROPERTY_GETSET(audiofm_patch_name_obj,
    (mp_obj_t)&audiofm_patch_get_name_obj,
    (mp_obj_t)&audiofm_patch_set_name_obj);

static mp_obj_t audiofm_patch_pack(mp_obj_t self_in) {
    audiofm_patch_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bytes(self->data, AUDIODSP_FM_VOICE_BYTES);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_patch_pack_obj, audiofm_patch_pack);

static const uint8_t audiofm_voice_header[6] = {
    0xf0, 0x43, 0x00, 0x00, 0x01, 0x1b
};
static const uint8_t audiofm_bank_header[6] = {
    0xf0, 0x43, 0x00, 0x09, 0x20, 0x00
};

// True if `data` starts with `header`, the channel nibble in byte 2 aside.
static bool audiofm_header_matches(const uint8_t *data,
    const uint8_t *header) {
    return data[0] == header[0] && data[1] == header[1] &&
           (data[2] & 0xf0) == 0 && memcmp(data + 3, header + 3, 3) == 0;
}

static mp_obj_t audiofm_patch_unpack(mp_obj_t data_in) {
    mp_buffer_info_t info;
    mp_get_buffer_raise(data_in, &info, MP_BUFFER_READ);
    const uint8_t *data = info.buf;
    if (info.len == AUDIODSP_FM_VOICE_SYSEX_BYTES) {
        if (!audiofm_header_matches(data, audiofm_voice_header) ||
            data[info.len - 1] != 0xf7) {
            mp_raise_ValueError(MP_ERROR_TEXT("not a single-voice message"));
        }
        if (audiodsp_fm_checksum(data + 6, AUDIODSP_FM_VOICE_BYTES) !=
            data[6 + AUDIODSP_FM_VOICE_BYTES]) {
            mp_raise_ValueError(MP_ERROR_TEXT("checksum does not match"));
        }
        data += 6;
    } else if (info.len != AUDIODSP_FM_VOICE_BYTES) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("a voice is 155 bytes, or 163 as a message"));
    }
    audiofm_patch_obj_t *patch = audiofm_patch_new();
    memcpy(patch->data, data, AUDIODSP_FM_VOICE_BYTES);
    patch->data[AUDIODSP_FM_OP_ENABLE] = 0x3f;
    audiodsp_fm_clamp(patch->data);
    return MP_OBJ_FROM_PTR(patch);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_patch_unpack_fun_obj,
    audiofm_patch_unpack);
static MP_DEFINE_CONST_STATICMETHOD_OBJ(audiofm_patch_unpack_obj,
    MP_ROM_PTR(&audiofm_patch_unpack_fun_obj));

static mp_obj_t audiofm_patch_bank(mp_obj_t data_in) {
    mp_buffer_info_t info;
    mp_get_buffer_raise(data_in, &info, MP_BUFFER_READ);
    const uint8_t *data = info.buf;
    if (info.len == AUDIODSP_FM_BANK_SYSEX_BYTES) {
        if (!audiofm_header_matches(data, audiofm_bank_header) ||
            data[info.len - 1] != 0xf7) {
            mp_raise_ValueError(MP_ERROR_TEXT("not a 32-voice bank message"));
        }
        if (audiodsp_fm_checksum(data + 6, AUDIODSP_FM_BANK_DATA_BYTES) !=
            data[6 + AUDIODSP_FM_BANK_DATA_BYTES]) {
            mp_raise_ValueError(MP_ERROR_TEXT("checksum does not match"));
        }
        data += 6;
    } else if (info.len != AUDIODSP_FM_BANK_DATA_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "a bank is 4104 bytes, or 4096 without its header"));
    }
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < AUDIODSP_FM_BANK_VOICES; i++) {
        audiofm_patch_obj_t *patch = audiofm_patch_new();
        audiodsp_fm_unpack_voice(data + i * AUDIODSP_FM_PACKED_BYTES,
            patch->data);
        mp_obj_list_append(list, MP_OBJ_FROM_PTR(patch));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_patch_bank_fun_obj,
    audiofm_patch_bank);
static MP_DEFINE_CONST_STATICMETHOD_OBJ(audiofm_patch_bank_obj,
    MP_ROM_PTR(&audiofm_patch_bank_fun_obj));

static mp_obj_t audiofm_patch_pack_bank(mp_obj_t patches_in) {
    size_t count = 0;
    mp_obj_t *patches = NULL;
    mp_obj_get_array(patches_in, &count, &patches);
    if (count > AUDIODSP_FM_BANK_VOICES) {
        mp_raise_ValueError(MP_ERROR_TEXT("a bank holds 32 voices"));
    }
    for (size_t i = 0; i < count; i++) {
        if (!mp_obj_is_type(patches[i], &audiofm_patch_type)) {
            mp_raise_TypeError(MP_ERROR_TEXT("a bank holds Patch objects"));
        }
    }
    vstr_t out;
    vstr_init_len(&out, AUDIODSP_FM_BANK_SYSEX_BYTES);
    uint8_t *bytes = (uint8_t *)out.buf;
    memcpy(bytes, audiofm_bank_header, 6);
    uint8_t blank[AUDIODSP_FM_PATCH_BYTES];
    audiodsp_fm_init_voice(blank);
    for (size_t i = 0; i < AUDIODSP_FM_BANK_VOICES; i++) {
        const uint8_t *voice = blank;
        if (i < count) {
            voice = ((audiofm_patch_obj_t *)MP_OBJ_TO_PTR(patches[i]))->data;
        }
        audiodsp_fm_pack_voice(voice,
            bytes + 6 + i * AUDIODSP_FM_PACKED_BYTES);
    }
    bytes[6 + AUDIODSP_FM_BANK_DATA_BYTES] = audiodsp_fm_checksum(bytes + 6,
        AUDIODSP_FM_BANK_DATA_BYTES);
    bytes[AUDIODSP_FM_BANK_SYSEX_BYTES - 1] = 0xf7;
    return mp_obj_new_bytes_from_vstr(&out);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_patch_pack_bank_fun_obj,
    audiofm_patch_pack_bank);
static MP_DEFINE_CONST_STATICMETHOD_OBJ(audiofm_patch_pack_bank_obj,
    MP_ROM_PTR(&audiofm_patch_pack_bank_fun_obj));

static const mp_rom_map_elem_t audiofm_patch_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_operators),
      MP_ROM_PTR(&audiofm_patch_operators_obj) },
    { MP_ROM_QSTR(MP_QSTR_algorithm),
      MP_ROM_PTR(&audiofm_patch_algorithm_obj) },
    { MP_ROM_QSTR(MP_QSTR_name), MP_ROM_PTR(&audiofm_patch_name_obj) },
    { MP_ROM_QSTR(MP_QSTR_pack), MP_ROM_PTR(&audiofm_patch_pack_obj) },
    { MP_ROM_QSTR(MP_QSTR_unpack), MP_ROM_PTR(&audiofm_patch_unpack_obj) },
    { MP_ROM_QSTR(MP_QSTR_bank), MP_ROM_PTR(&audiofm_patch_bank_obj) },
    { MP_ROM_QSTR(MP_QSTR_pack_bank),
      MP_ROM_PTR(&audiofm_patch_pack_bank_obj) },
    FIELD_ENTRY(pitch_rates), FIELD_ENTRY(pitch_levels),
    FIELD_ENTRY(feedback), FIELD_ENTRY(key_sync), FIELD_ENTRY(lfo_speed),
    FIELD_ENTRY(lfo_delay), FIELD_ENTRY(lfo_pitch_depth),
    FIELD_ENTRY(lfo_amp_depth), FIELD_ENTRY(lfo_key_sync),
    FIELD_ENTRY(lfo_wave), FIELD_ENTRY(lfo_pitch_sens),
    FIELD_ENTRY(transpose),
};
static MP_DEFINE_CONST_DICT(audiofm_patch_locals, audiofm_patch_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiofm_patch_type,
    MP_QSTR_Patch,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiofm_patch_make_new,
    attr, audiofm_patch_attr,
    locals_dict, &audiofm_patch_locals
    );
