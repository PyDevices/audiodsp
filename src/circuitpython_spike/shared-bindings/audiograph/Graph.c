// audiograph.Graph bindings for CircuitPython.
//
// The same calls, arguments and refusals as the MicroPython binding
// (src/audiograph/Graph.c), so a graph written for one runs on the other and
// renders the same samples.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "shared-bindings/audiograph/Graph.h"

#include "py/binary.h"
#include "py/runtime.h"

//| class Graph:
//|     """A whole patch rendered by one C call per block.
//|
//|     A graph owns ``buffers`` blocks of ``frames`` signed 16-bit samples
//|     and a table of up to ``nodes`` nodes. Every node reads some buffers,
//|     writes one, belongs to a group, and holds one kernel's settings
//|     behind up to sixteen float parameters. Nodes run in the order they
//|     were added. 16384 counts (`UNIT`) is 1.0 on every buffer."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         sample_rate: int = 48000,
//|         frames: int = 128,
//|         channel_count: int = 2,
//|         buffers: int = 64,
//|         nodes: int = 64,
//|     ) -> None:
//|         """Create an empty graph. ``frames`` is the block, 1 to 256."""
//|         ...
//|
//|     def buffer(self) -> int:
//|         """A fresh block buffer's id."""
//|         ...
//|
//|     def add(self, kind: int, group: int, inputs: Sequence[Optional[int]],
//|             output: Optional[int], arg: float = 0.0) -> int:
//|         """Append a node and return its index. ``group`` is 0 to 31 or
//|         `ALWAYS`; an input left ``None`` is open."""
//|         ...
//|
//|     def set(self, node: int, index: int, value: float) -> None:
//|         """Store a parameter; the node derives its settings before the next
//|         run."""
//|         ...
//|
//|     def get(self, node: int, index: int) -> float:
//|         """Read a parameter back."""
//|         ...
//|
//|     def table(self, node: int, table: array.array) -> None:
//|         """A feedback delay's wow shape, an int16 array, borrowed."""
//|         ...
//|
//|     def reset(self, node: int) -> None:
//|         """Clear a node's running state."""
//|         ...
//|
//|     def run(self, out: Optional[WriteableBuffer] = None, block: int = 0,
//|             enable: int = 0xFFFFFFFF) -> int:
//|         """Render one block into ``out`` at block ``block`` and return the
//|         mask of groups whose envelopes are still sounding."""
//|         ...
//|
//|     def peak(self, buffer: int) -> int:
//|         """A block's peak."""
//|         ...
//|
//|     def read(self, buffer: int, dst: WriteableBuffer) -> None:
//|         """Copy a block out into an int16 buffer of ``frames``."""
//|         ...
//|
//|     def write(self, buffer: int, src: ReadableBuffer) -> None:
//|         """Fill a block from an int16 buffer of ``frames``."""
//|         ...
//|

static audiograph_graph_obj_t *graph_self(mp_obj_t self_in) {
    return MP_OBJ_TO_PTR(self_in);
}

static mp_obj_t audiograph_graph_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_sample_rate, ARG_frames, ARG_channel_count, ARG_buffers,
           ARG_nodes };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 48000} },
        { MP_QSTR_frames, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 128} },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 2} },
        { MP_QSTR_buffers, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 64} },
        { MP_QSTR_nodes, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 64} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t frames = args[ARG_frames].u_int;
    if (frames < 1 || frames > (mp_int_t)AUDIODSP_GRAPH_MAX_FRAMES) {
        mp_raise_ValueError(MP_ERROR_TEXT("frames must be from 1 to 256"));
    }
    mp_int_t channels = args[ARG_channel_count].u_int;
    if (channels < 1 || channels > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }
    mp_int_t buffers = args[ARG_buffers].u_int;
    mp_int_t nodes = args[ARG_nodes].u_int;
    if (buffers < 1 || buffers > 4096 || nodes < 1 || nodes > 4096) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffers and nodes must be from 1 to 4096"));
    }
    if (args[ARG_sample_rate].u_int < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate must be at least 1"));
    }
    audiograph_graph_obj_t *self = mp_obj_malloc(audiograph_graph_obj_t, type);
    common_hal_audiograph_graph_construct(self,
        (uint32_t)args[ARG_sample_rate].u_int, (uint32_t)frames,
        (uint32_t)channels, (uint32_t)buffers, (uint32_t)nodes);
    return MP_OBJ_FROM_PTR(self);
}

// buffer(): a fresh block buffer's id.
static mp_obj_t audiograph_graph_buffer(mp_obj_t self_in) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    if (self->next_buffer >= self->graph.nbufs) {
        mp_raise_ValueError(MP_ERROR_TEXT("no buffers left"));
    }
    return MP_OBJ_NEW_SMALL_INT(self->next_buffer++);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiograph_graph_buffer_obj,
    audiograph_graph_buffer);

static int16_t buffer_id(mp_obj_t obj) {
    if (obj == mp_const_none) {
        return AUDIODSP_GRAPH_NONE;
    }
    mp_int_t v = mp_obj_get_int(obj);
    if (v < 0 || v > 32767) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer id"));
    }
    return (int16_t)v;
}

// add(kind, group, inputs, output, arg=0.0): the node's index. `inputs` is
// a sequence of buffer ids (None for an open input), `output` a buffer id
// or None.
static mp_obj_t audiograph_graph_add(size_t n_args, const mp_obj_t *args) {
    audiograph_graph_obj_t *self = graph_self(args[0]);
    mp_int_t kind = mp_obj_get_int(args[1]);
    if (kind < 0 || kind >= (mp_int_t)AUDIODSP_GRAPH_KINDS) {
        mp_raise_ValueError(MP_ERROR_TEXT("unknown node kind"));
    }
    mp_int_t group = mp_obj_get_int(args[2]);
    if (group < 0 || (group > 31 && group != AUDIODSP_GRAPH_ALWAYS)) {
        mp_raise_ValueError(MP_ERROR_TEXT("group must be 0..31 or ALWAYS"));
    }
    size_t len = 0;
    mp_obj_t *items = NULL;
    mp_obj_get_array(args[3], &len, &items);
    if (len > AUDIODSP_GRAPH_MAX_INPUTS) {
        mp_raise_ValueError(MP_ERROR_TEXT("too many inputs"));
    }
    int16_t inputs[AUDIODSP_GRAPH_MAX_INPUTS];
    for (size_t i = 0; i < len; ++i) {
        inputs[i] = buffer_id(items[i]);
    }
    int16_t output = buffer_id(args[4]);
    float arg = n_args > 5 ? (float)mp_obj_get_float(args[5]) : 0.0f;
    int index = common_hal_audiograph_graph_add(self,
        (audiodsp_graph_kind_t)kind, (uint32_t)group, inputs, (uint32_t)len,
        output, arg);
    if (index < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("graph is full, a buffer id is out of range, or the node has no output"));
    }
    return MP_OBJ_NEW_SMALL_INT(index);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiograph_graph_add_obj, 5, 6,
    audiograph_graph_add);

static mp_obj_t audiograph_graph_set(size_t n_args, const mp_obj_t *args) {
    audiograph_graph_obj_t *self = graph_self(args[0]);
    (void)n_args;
    if (!audiodsp_graph_set(&self->graph, (uint32_t)mp_obj_get_int(args[1]),
            (uint32_t)mp_obj_get_int(args[2]), (float)mp_obj_get_float(args[3]))) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad node or parameter index"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiograph_graph_set_obj, 4, 4,
    audiograph_graph_set);

static mp_obj_t audiograph_graph_get(mp_obj_t self_in, mp_obj_t node,
    mp_obj_t index) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    float value = 0.0f;
    if (!audiodsp_graph_get(&self->graph, (uint32_t)mp_obj_get_int(node),
            (uint32_t)mp_obj_get_int(index), &value)) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad node or parameter index"));
    }
    return mp_obj_new_float(value);
}
static MP_DEFINE_CONST_FUN_OBJ_3(audiograph_graph_get_obj,
    audiograph_graph_get);

// table(node, int16_array): a feedback delay's wow shape, borrowed.
static mp_obj_t audiograph_graph_table(mp_obj_t self_in, mp_obj_t node,
    mp_obj_t table) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    mp_buffer_info_t info;
    mp_get_buffer_raise(table, &info, MP_BUFFER_READ);
    if (info.typecode != 'h' || info.len % 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("table must be an int16 array"));
    }
    if (!audiodsp_graph_set_table(&self->graph, (uint32_t)mp_obj_get_int(node),
            (const int16_t *)info.buf, (uint32_t)(info.len / 2))) {
        mp_raise_ValueError(MP_ERROR_TEXT("not a delay node, or a bad table length"));
    }
    mp_obj_list_append(self->keep, table);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(audiograph_graph_table_obj,
    audiograph_graph_table);

static mp_obj_t audiograph_graph_reset(mp_obj_t self_in, mp_obj_t node) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    if (!audiodsp_graph_reset(&self->graph, (uint32_t)mp_obj_get_int(node))) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad node"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiograph_graph_reset_obj,
    audiograph_graph_reset);

// run(out=None, block=0, enable=0xFFFFFFFF): renders one block into `out`
// at block `block` (frames * channel_count samples each), and returns the
// mask of groups whose envelopes are still sounding.
static mp_obj_t audiograph_graph_run(size_t n_args, const mp_obj_t *args) {
    audiograph_graph_obj_t *self = graph_self(args[0]);
    int16_t *out = NULL;
    if (n_args > 1 && args[1] != mp_const_none) {
        mp_buffer_info_t info;
        mp_get_buffer_raise(args[1], &info, MP_BUFFER_WRITE);
        mp_int_t block = n_args > 2 ? mp_obj_get_int(args[2]) : 0;
        size_t block_bytes = (size_t)self->graph.frames * self->graph.channel_count * 2u;
        if (block < 0 || (size_t)(block + 1) * block_bytes > info.len) {
            mp_raise_ValueError(MP_ERROR_TEXT("out is too small for that block"));
        }
        out = (int16_t *)(void *)((uint8_t *)info.buf + (size_t)block * block_bytes);
    }
    uint32_t enable = 0xFFFFFFFFu;
    if (n_args > 3) {
        enable = (uint32_t)mp_obj_get_int_truncated(args[3]);
    }
    uint32_t sounding = audiodsp_graph_run(&self->graph, out, enable);
    return mp_obj_new_int_from_uint(sounding);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiograph_graph_run_obj, 1, 4,
    audiograph_graph_run);

static mp_obj_t audiograph_graph_peak(mp_obj_t self_in, mp_obj_t buffer) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    int32_t peak = audiodsp_graph_peak(&self->graph, (uint32_t)mp_obj_get_int(buffer));
    if (peak < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer id"));
    }
    return MP_OBJ_NEW_SMALL_INT(peak);
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiograph_graph_peak_obj,
    audiograph_graph_peak);

static int16_t *block_of(audiograph_graph_obj_t *self, mp_obj_t buffer) {
    mp_int_t id = mp_obj_get_int(buffer);
    if (id < 0 || (uint32_t)id >= self->graph.nbufs) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer id"));
    }
    return self->graph.bufs + (size_t)id * self->graph.frames;
}

// read(buffer, dst): copy a block out into an int16 buffer of `frames`.
static mp_obj_t audiograph_graph_read(mp_obj_t self_in, mp_obj_t buffer,
    mp_obj_t dst) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    const int16_t *src = block_of(self, buffer);
    mp_buffer_info_t info;
    mp_get_buffer_raise(dst, &info, MP_BUFFER_WRITE);
    if (info.len != (size_t)self->graph.frames * 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("dst must hold one block of int16"));
    }
    memcpy(info.buf, src, info.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(audiograph_graph_read_obj,
    audiograph_graph_read);

// write(buffer, src): fill a block from an int16 buffer of `frames`.
static mp_obj_t audiograph_graph_write(mp_obj_t self_in, mp_obj_t buffer,
    mp_obj_t src) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    int16_t *dst = block_of(self, buffer);
    mp_buffer_info_t info;
    mp_get_buffer_raise(src, &info, MP_BUFFER_READ);
    if (info.len != (size_t)self->graph.frames * 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("src must hold one block of int16"));
    }
    memcpy(dst, info.buf, info.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(audiograph_graph_write_obj,
    audiograph_graph_write);

static const mp_rom_map_elem_t audiograph_graph_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_buffer), MP_ROM_PTR(&audiograph_graph_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_add), MP_ROM_PTR(&audiograph_graph_add_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audiograph_graph_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_get), MP_ROM_PTR(&audiograph_graph_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_table), MP_ROM_PTR(&audiograph_graph_table_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&audiograph_graph_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&audiograph_graph_run_obj) },
    { MP_ROM_QSTR(MP_QSTR_peak), MP_ROM_PTR(&audiograph_graph_peak_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&audiograph_graph_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&audiograph_graph_write_obj) },
};
static MP_DEFINE_CONST_DICT(audiograph_graph_locals,
    audiograph_graph_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiograph_graph_type,
    MP_QSTR_Graph,
    MP_TYPE_FLAG_NONE,
    make_new, audiograph_graph_make_new,
    locals_dict, &audiograph_graph_locals
    );
