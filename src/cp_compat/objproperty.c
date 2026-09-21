// Compat shim for audiodsp: makes MP_PROPERTY_GETTER/GETSET
// objects (see objproperty.h) actually get invoked on native types.
//
// This is the missing half of the shim. CircuitPython's declarative
// property macros only work there because CircuitPython patches its own
// py/runtime.c (`mp_convert_member_lookup`, "CIRCUITPY-CHANGE" tagged) to
// recognize a `mp_type_property` value found in a *native* type's
// locals_dict and invoke its getter/setter -- mainline's own
// mp_convert_member_lookup has no such case (checked directly: it only
// special-cases bound/static/class methods). We can't carry that core
// patch (no edits to micropython/, and it would only help this usermod
// when the rest of the interpreter never needs it). Instead, every ported
// type with properties wires this function as its own `attr` type slot
// (mainline's supported, documented extension point for exactly this --
// see the AdvancedTimer example in micropython/examples/usercmodule/
// cexample/examplemodule.c), and it reproduces CP's core-patch behavior
// from usermod code: property lookup first, then fall through to ordinary
// locals_dict lookup (methods, plain values) via mp_convert_member_lookup
// for everything else.
//
// SPDX-License-Identifier: MIT

#include "cp_compat/objproperty.h"
#include "py/obj.h"
#include "py/runtime.h"

void cp_compat_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    const mp_obj_type_t *type = mp_obj_get_type(self_in);
    if (!MP_OBJ_TYPE_HAS_SLOT(type, locals_dict)) {
        dest[1] = MP_OBJ_SENTINEL;
        return;
    }
    mp_map_t *locals_map = &MP_OBJ_TYPE_GET_SLOT(type, locals_dict)->map;
    mp_map_elem_t *elem = mp_map_lookup(locals_map, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
    if (elem == NULL) {
        // Not found here -- let the VM's generic locals_dict lookup run,
        // matching what happens for any other unhandled attribute name.
        dest[1] = MP_OBJ_SENTINEL;
        return;
    }

    mp_obj_t member = elem->value;
    if (mp_obj_is_obj(member) && ((mp_obj_base_t *)MP_OBJ_TO_PTR(member))->type == &mp_type_property) {
        const cp_compat_property_t *prop = MP_OBJ_TO_PTR(member);
        if (dest[0] == MP_OBJ_NULL) {
            // Load.
            mp_obj_t getter = prop->proxy[0];
            if (getter == MP_ROM_NONE) {
                mp_raise_msg(&mp_type_AttributeError, MP_ERROR_TEXT("unreadable attribute"));
            }
            dest[0] = mp_call_function_1(getter, self_in);
        } else {
            // Store (dest[1] holds the value) or delete (dest[1] == MP_OBJ_NULL).
            if (dest[1] == MP_OBJ_NULL) {
                mp_raise_msg(&mp_type_AttributeError, MP_ERROR_TEXT("can't delete attribute"));
            }
            mp_obj_t setter = prop->proxy[1];
            if (setter == MP_ROM_NONE) {
                mp_raise_msg(&mp_type_AttributeError, MP_ERROR_TEXT("can't set attribute"));
            }
            mp_call_function_2(setter, self_in, dest[1]);
            dest[0] = MP_OBJ_NULL;
        }
        return;
    }

    // Not a property.
    if (dest[0] == MP_OBJ_NULL) {
        // Load: same handling as the VM's own default locals_dict lookup
        // (bound methods, plain values).
        mp_convert_member_lookup(self_in, type, member, dest);
        return;
    }
    // Store/delete of a non-property locals_dict entry (e.g. assigning to a
    // method name) isn't meaningful for a native type with no instance
    // dict -- deny it outright rather than relaying a SENTINEL for a
    // fallback path that has nowhere else to store it either.
    mp_raise_msg(&mp_type_AttributeError, MP_ERROR_TEXT("can't set attribute"));
}
