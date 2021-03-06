/*
 * freetype.c
 * Copyright (C) 2016 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "data-types.h"
#include <math.h>
#include <structmember.h>
#include <ft2build.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <hb.h>
#pragma GCC diagnostic pop
#include <hb-ft.h>

#if HB_VERSION_MAJOR > 1 || (HB_VERSION_MAJOR == 1 && (HB_VERSION_MINOR > 0 || (HB_VERSION_MINOR == 0 && HB_VERSION_MICRO >= 5)))
#define HARBUZZ_HAS_LOAD_FLAGS
#endif
#if HB_VERSION_MAJOR > 1 || (HB_VERSION_MAJOR == 1 && (HB_VERSION_MINOR > 6 || (HB_VERSION_MINOR == 6 && HB_VERSION_MICRO >= 3)))
#define HARFBUZZ_HAS_CHANGE_FONT
#endif

#include FT_FREETYPE_H
typedef struct {
    PyObject_HEAD

    FT_Face face;
    unsigned int units_per_EM;
    int ascender, descender, height, max_advance_width, max_advance_height, underline_position, underline_thickness;
    int hinting, hintstyle;
    bool is_scalable;
    FT_F26Dot6 char_width, char_height;
    FT_UInt xdpi, ydpi;
    PyObject *path;
    hb_buffer_t *harfbuzz_buffer;
    hb_font_t *harfbuzz_font;
} Face;

static PyObject* FreeType_Exception = NULL;

void 
set_freetype_error(const char* prefix, int err_code) {
    int i = 0;
#undef FTERRORS_H_
#undef __FTERRORS_H__
#define FT_ERRORDEF( e, v, s )  { e, s },
#define FT_ERROR_START_LIST     {
#define FT_ERROR_END_LIST       { 0, NULL } };

    static const struct {
        int          err_code;
        const char*  err_msg;
    } ft_errors[] =

#ifdef FT_ERRORS_H
#include FT_ERRORS_H
#else 
    FT_ERROR_START_LIST FT_ERROR_END_LIST
#endif

    while(ft_errors[i].err_msg != NULL) {
        if (ft_errors[i].err_code == err_code) {
            PyErr_Format(FreeType_Exception, "%s %s", prefix, ft_errors[i].err_msg);
            return;
        }
        i++;
    }
    PyErr_Format(FreeType_Exception, "%s (error code: %d)", prefix, err_code);
}

static FT_Library  library;

static inline bool
set_font_size(Face *self, FT_F26Dot6 char_width, FT_F26Dot6 char_height, FT_UInt xdpi, FT_UInt ydpi) {
    int error = FT_Set_Char_Size(self->face, char_width, char_height, xdpi, ydpi);
    if (!error) {
        self->char_width = char_width; self->char_height = char_height; self->xdpi = xdpi; self->ydpi = ydpi;
        if (self->harfbuzz_font != NULL) {
#ifdef HARFBUZZ_HAS_CHANGE_FONT
            hb_ft_font_changed(self->harfbuzz_font);
#else
            hb_font_set_scale(
                self->harfbuzz_font,
                (int) (((uint64_t) self->face->size->metrics.x_scale * (uint64_t) self->face->units_per_EM + (1u<<15)) >> 16),
                (int) (((uint64_t) self->face->size->metrics.y_scale * (uint64_t) self->face->units_per_EM + (1u<<15)) >> 16)
            );
#endif
        }
    } else {
        set_freetype_error("Failed to set char size, with error:", error); return false; 
    }
    return !error;
}


static PyObject*
new(PyTypeObject *type, PyObject *args, PyObject UNUSED *kwds) {
    Face *self;
    char *path;
    int error, hinting, hintstyle;
    long index;
    /* unsigned int columns=80, lines=24, scrollback=0; */
    if (!PyArg_ParseTuple(args, "slii", &path, &index, &hinting, &hintstyle)) return NULL;

    self = (Face *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->path = PyTuple_GET_ITEM(args, 0);
        Py_INCREF(self->path);
        error = FT_New_Face(library, path, index, &(self->face));
        if(error) { set_freetype_error("Failed to load face, with error:", error); Py_CLEAR(self); return NULL; }
#define CPY(n) self->n = self->face->n;
        CPY(units_per_EM); CPY(ascender); CPY(descender); CPY(height); CPY(max_advance_width); CPY(max_advance_height); CPY(underline_position); CPY(underline_thickness);
#undef CPY
        self->is_scalable = FT_IS_SCALABLE(self->face);
        self->harfbuzz_buffer = hb_buffer_create();
        self->hinting = hinting; self->hintstyle = hintstyle;
        if (self->harfbuzz_buffer == NULL || !hb_buffer_allocation_successful(self->harfbuzz_buffer) || !hb_buffer_pre_allocate(self->harfbuzz_buffer, 20)) { Py_CLEAR(self); return PyErr_NoMemory(); }
        if (!set_font_size(self, 10, 20, 96, 96)) { Py_CLEAR(self); return NULL; }
        self->harfbuzz_font = hb_ft_font_create(self->face, NULL);
        if (self->harfbuzz_font == NULL) { Py_CLEAR(self); return PyErr_NoMemory(); }
    }
    return (PyObject*)self;
}
 
static void
dealloc(Face* self) {
    if (self->harfbuzz_buffer) hb_buffer_destroy(self->harfbuzz_buffer);
    if (self->harfbuzz_font) hb_font_destroy(self->harfbuzz_font);
    if (self->face) FT_Done_Face(self->face);
    Py_CLEAR(self->path);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *
repr(Face *self) {
    return PyUnicode_FromFormat(
        "Face(path=%S, is_scalable=%S, units_per_EM=%u, ascender=%i, descender=%i, height=%i, max_advance_width=%i max_advance_height=%i, underline_position=%i, underline_thickness=%i)",
        self->path, self->is_scalable ? Py_True : Py_False, 
        self->ascender, self->descender, self->height, self->max_advance_width, self->max_advance_height, self->underline_position, self->underline_thickness
    );
}



static PyObject*
set_char_size(Face *self, PyObject *args) {
#define set_char_size_doc "set_char_size(width, height, xdpi, ydpi) -> set the character size. width, height is in 1/64th of a pt. dpi is in pixels per inch"
    long char_width, char_height;
    unsigned int xdpi, ydpi;
    if (!PyArg_ParseTuple(args, "llII", &char_width, &char_height, &xdpi, &ydpi)) return NULL;
    if (!set_font_size(self, char_width, char_height, xdpi, ydpi)) return NULL;
    Py_RETURN_NONE;
}

static inline int
get_load_flags(int hinting, int hintstyle, int base) {
    int flags = base;
    if (hinting) {
        if (hintstyle >= 3) flags |= FT_LOAD_TARGET_NORMAL;
        else if (0 < hintstyle  && hintstyle < 3) flags |= FT_LOAD_TARGET_LIGHT;
    } else flags |= FT_LOAD_NO_HINTING;
    return flags;
}

static inline bool 
load_glyph(Face *self, int glyph_index) {
    int flags = get_load_flags(self->hinting, self->hintstyle, FT_LOAD_RENDER);
    int error = FT_Load_Glyph(self->face, glyph_index, flags);
    if (error) { set_freetype_error("Failed to load glyph, with error:", error); Py_CLEAR(self); return false; }
    return true;
}

static PyObject*
get_char_index(Face *self, PyObject *args) {
#define get_char_index_doc ""
    int code;
    unsigned int ans;
    if (!PyArg_ParseTuple(args, "C", &code)) return NULL;
    ans = FT_Get_Char_Index(self->face, code);

    return Py_BuildValue("I", ans);
}
 
static PyObject*
calc_cell_width(Face *self) {
#define calc_cell_width_doc ""
    unsigned long ans = 0;
    for (char_type i = 32; i < 128; i++) {
        int glyph_index = FT_Get_Char_Index(self->face, i);
        if (!load_glyph(self, glyph_index)) return NULL;
        ans = MAX(ans, (unsigned long)ceilf((float)self->face->glyph->metrics.horiAdvance / 64.f));
    }
    return PyLong_FromUnsignedLong(ans);
}

static PyStructSequence_Field shape_fields[] = {
    {"glyph_id", NULL},
    {"cluster", NULL},
    {"mask", NULL},
    {"x_offset", NULL},
    {"y_offset", NULL},
    {"x_advance", NULL},
    {"y_advance", NULL},
    {NULL}
};
static PyStructSequence_Desc shape_fields_desc = {"Shape", NULL, shape_fields, 7};
static PyTypeObject ShapeFieldsType = {{{0}}};

static inline PyObject*
shape_to_py(unsigned int i, hb_glyph_info_t *info, hb_glyph_position_t *pos) {
    PyObject *ans = PyStructSequence_New(&ShapeFieldsType);
    if (ans == NULL) return NULL;
#define SI(num, src, attr, conv, func, div) PyStructSequence_SET_ITEM(ans, num, func(((conv)src[i].attr) / div)); if (PyStructSequence_GET_ITEM(ans, num) == NULL) { Py_CLEAR(ans); return PyErr_NoMemory(); }
#define INFO(num, attr) SI(num, info, attr, unsigned long, PyLong_FromUnsignedLong, 1)
#define POS(num, attr) SI(num + 3, pos, attr, double, PyFloat_FromDouble, 64.0)
    INFO(0, codepoint); INFO(1, cluster); INFO(2, mask);
    POS(0, x_offset); POS(1, y_offset); POS(2, x_advance); POS(3, y_advance);
#undef INFO
#undef POS
#undef SI
    return ans;
}

typedef struct {
    unsigned int length;
    hb_glyph_info_t *info;
    hb_glyph_position_t *positions;
} ShapeData;


static inline void
_shape(Face *self, const char *string, int len, ShapeData *ans) {
    hb_buffer_clear_contents(self->harfbuzz_buffer);
#ifdef HARBUZZ_HAS_LOAD_FLAGS
    hb_ft_font_set_load_flags(self->harfbuzz_font, get_load_flags(self->hinting, self->hintstyle, FT_LOAD_DEFAULT));
#endif
    hb_buffer_add_utf8(self->harfbuzz_buffer, string, len, 0, len);
    hb_buffer_guess_segment_properties(self->harfbuzz_buffer);
    hb_shape(self->harfbuzz_font, self->harfbuzz_buffer, NULL, 0);

    unsigned int info_length, positions_length;
    ans->info = hb_buffer_get_glyph_infos(self->harfbuzz_buffer, &info_length);
    ans->positions = hb_buffer_get_glyph_positions(self->harfbuzz_buffer, &positions_length);
    ans->length = MIN(info_length, positions_length);
}


static PyObject*
shape(Face *self, PyObject *args) {
#define shape_doc "shape(text)"
    const char *string;
    int len;
    if (!PyArg_ParseTuple(args, "s#", &string, &len)) return NULL;

    ShapeData sd;
    _shape(self, string, len, &sd);
    PyObject *ans = PyTuple_New(sd.length);
    if (ans == NULL) return NULL;
    for (unsigned int i = 0; i < sd.length; i++) {
        PyObject *s = shape_to_py(i, sd.info, sd.positions);
        if (s == NULL) { Py_CLEAR(ans); return NULL; }
        PyTuple_SET_ITEM(ans, i, s);
    }
    return ans;
}

typedef struct {
    unsigned char* buf;
    size_t start_x, width, stride;
    size_t rows;
} ProcessedBitmap;


static inline void
trim_borders(ProcessedBitmap *ans, size_t extra) {
    bool column_has_text = false;

    // Trim empty columns from the right side of the bitmap
    for (ssize_t x = ans->width - 1; !column_has_text && x > -1 && extra > 0; x--) {
        for (size_t y = 0; y < ans->rows && !column_has_text; y++) {
            if (ans->buf[x + y * ans->stride] > 200) column_has_text = true;
        }
        if (!column_has_text) { ans->width--; extra--; }
    }

    ans->start_x = extra;
    ans->width -= extra;
}


static inline bool
render_bitmap(Face *self, int glyph_id, ProcessedBitmap *ans, unsigned int cell_width, unsigned int num_cells, int bold, int italic, bool rescale) {
    if (!load_glyph(self, glyph_id)) return false;
    unsigned int max_width = cell_width * num_cells;
    FT_Bitmap *bitmap = &self->face->glyph->bitmap;
    ans->buf = bitmap->buffer;
    ans->start_x = 0; ans->width = bitmap->width;
    ans->stride = bitmap->pitch < 0 ? -bitmap->pitch : bitmap->pitch;
    ans->rows = bitmap->rows;
    if (ans->width > max_width) {
        size_t extra = bitmap->width - max_width;
        if (italic && extra < cell_width / 2) {
            trim_borders(ans, extra);
        } else if (rescale && self->is_scalable && extra > MAX(2, cell_width / 3)) {
            FT_F26Dot6 char_width = self->char_width, char_height = self->char_height;
            float ar = (float)max_width / (float)bitmap->width;
            if (set_font_size(self, (FT_F26Dot6)((float)self->char_width * ar), (FT_F26Dot6)((float)self->char_height * ar), self->xdpi, self->ydpi)) {
                if (!render_bitmap(self, glyph_id, ans, cell_width, num_cells, bold, italic, false)) return false;
                if (!set_font_size(self, char_width, char_height, self->xdpi, self->ydpi)) return false;
            }
        }
    }
    return true;
}

static inline void
place_bitmap_in_cell(unsigned char *cell, ProcessedBitmap *bm, size_t cell_width, size_t cell_height, float x_offset, float y_offset, FT_Glyph_Metrics *metrics, size_t baseline) {
    // We want the glyph to be positioned inside the cell based on the bearingX
    // and bearingY values, making sure that it does not overflow the cell.

    // Calculate column bounds
    ssize_t xoff = (ssize_t)(x_offset + (float)metrics->horiBearingX / 64.f);
    size_t src_start_column = bm->start_x, dest_start_column = 0, extra;
    if (xoff < 0) src_start_column += -xoff;
    else dest_start_column = xoff;
    // Move the dest start column back if the width overflows because of it
    if (dest_start_column > 0 && dest_start_column + bm->width > cell_width) {
        extra = dest_start_column + bm->width - cell_width;
        dest_start_column = extra > dest_start_column ? 0 : dest_start_column - extra;
    }

    // Calculate row bounds
    ssize_t yoff = (ssize_t)(y_offset + (float)metrics->horiBearingY / 64.f);
    size_t src_start_row, dest_start_row;
    if (yoff > 0 && (size_t)yoff > baseline) {
        src_start_row = 0;
        dest_start_row = 0;
    } else {
        src_start_row = 0;
        dest_start_row = baseline - yoff;
    }

    /* printf("src_start_row: %zu src_start_column: %zu dest_start_row: %zu dest_start_column: %zu\n", src_start_row, src_start_column, dest_start_row, dest_start_column); */

    for (size_t sr = src_start_row, dr = dest_start_row; sr < bm->rows && dr < cell_height; sr++, dr++) {
        for(size_t sc = src_start_column, dc = dest_start_column; sc < bm->width && dc < cell_width; sc++, dc++) {
            uint16_t val = cell[dr * cell_width + dc];
            val = (val + bm->buf[sr * bm->stride + sc]) % 256;
            cell[dr * cell_width + dc] = val;
        }
    }
}

static PyObject*
draw_single_glyph(Face *self, PyObject *args) {
#define draw_single_glyph_doc "draw_complex_glyph(codepoint, cell_width, cell_height, cell_buffer, num_cells, bold, italic, baseline)"
    int bold, italic;
    unsigned int cell_width, cell_height, num_cells, baseline, codepoint;
    PyObject *addr;
    if (!PyArg_ParseTuple(args, "IIIO!IppI", &codepoint, &cell_width, &cell_height, &PyLong_Type, &addr, &num_cells, &bold, &italic, &baseline)) return NULL;
    unsigned char *cell = PyLong_AsVoidPtr(addr);
    int glyph_id = FT_Get_Char_Index(self->face, codepoint);
    ProcessedBitmap bm;
    if (!render_bitmap(self, glyph_id, &bm, cell_width, num_cells, bold, italic, true)) return NULL;
    place_bitmap_in_cell(cell, &bm, cell_width * num_cells, cell_height, 0, 0, &self->face->glyph->metrics, baseline);
    Py_RETURN_NONE;
}

static PyObject*
draw_complex_glyph(Face *self, PyObject *args) {
#define draw_complex_glyph_doc "draw_complex_glyph(text, cell_width, cell_height, cell_buffer, num_cells, bold, italic, baseline)"
    const char *text;
    int text_len, bold, italic;
    unsigned int cell_width, cell_height, num_cells, baseline;
    PyObject *addr;
    float x = 0.f, y = 0.f;
    if (!PyArg_ParseTuple(args, "s#IIO!IppI", &text, &text_len, &cell_width, &cell_height, &PyLong_Type, &addr, &num_cells, &bold, &italic, &baseline)) return NULL;
    unsigned char *cell = PyLong_AsVoidPtr(addr);
    ShapeData sd;
    _shape(self, text, text_len, &sd);
    ProcessedBitmap bm;

    for (unsigned i = 0; i < sd.length; i++) {
        if (sd.info[i].codepoint == 0) continue;
        if (!render_bitmap(self, sd.info[i].codepoint, &bm, cell_width, num_cells, bold, italic, true)) return NULL;
        x += (float)sd.positions[i].x_offset / 64.0f;
        y = (float)sd.positions[i].y_offset / 64.0f;
        place_bitmap_in_cell(cell, &bm, cell_width * num_cells, cell_height, x, y, &self->face->glyph->metrics, baseline);
        x += (float)sd.positions[i].x_advance / 64.0f;

    }
    Py_RETURN_NONE;
}

static PyObject*
split_cells(Face UNUSED *self, PyObject *args) {
#define split_cells_doc "split_cells(cell_width, cell_height, src, *cells)"
    unsigned int cell_width, cell_height;
    unsigned char *cells[10], *src;
    size_t num_cells = PyTuple_GET_SIZE(args) - 3;
    if (num_cells > sizeof(cells)/sizeof(cells[0])) { PyErr_SetString(PyExc_ValueError, "Too many cells being split"); return NULL; }
    cell_width = PyLong_AsUnsignedLong(PyTuple_GET_ITEM(args, 0));
    cell_height = PyLong_AsUnsignedLong(PyTuple_GET_ITEM(args, 1));
    src = PyLong_AsVoidPtr(PyTuple_GET_ITEM(args, 2));
    for (size_t i = 3; i < num_cells + 3; i++) cells[i - 3] = PyLong_AsVoidPtr(PyTuple_GET_ITEM(args, i));

    size_t stride = num_cells * cell_width;
    for (size_t y = 0; y < cell_height; y++) {
        for (size_t i = 0; i < num_cells; i++) {
            unsigned char *dest = cells[i] + y * cell_width;
            for (size_t x = 0; x < cell_width; x++) {
                dest[x] = src[y * stride + i * cell_width + x];
            }
        }
    }
    Py_RETURN_NONE;
}


// Boilerplate {{{

static PyMemberDef members[] = {
#define MEM(name, type) {#name, type, offsetof(Face, name), READONLY, #name}
    MEM(units_per_EM, T_UINT),
    MEM(ascender, T_INT),
    MEM(descender, T_INT),
    MEM(height, T_INT),
    MEM(max_advance_width, T_INT),
    MEM(max_advance_height, T_INT),
    MEM(underline_position, T_INT),
    MEM(underline_thickness, T_INT),
    MEM(is_scalable, T_BOOL),
    MEM(path, T_OBJECT_EX),
    {NULL}  /* Sentinel */
};

static PyMethodDef methods[] = {
    METHOD(set_char_size, METH_VARARGS)
    METHOD(shape, METH_VARARGS)
    METHOD(draw_complex_glyph, METH_VARARGS)
    METHOD(draw_single_glyph, METH_VARARGS)
    METHOD(split_cells, METH_VARARGS)
    METHOD(get_char_index, METH_VARARGS)
    METHOD(calc_cell_width, METH_NOARGS)
    {NULL}  /* Sentinel */
};


PyTypeObject Face_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "fast_data_types.Face",
    .tp_basicsize = sizeof(Face),
    .tp_dealloc = (destructor)dealloc, 
    .tp_flags = Py_TPFLAGS_DEFAULT,        
    .tp_doc = "FreeType Font face",
    .tp_methods = methods,
    .tp_members = members,
    .tp_new = new,                
    .tp_repr = (reprfunc)repr,
};

INIT_TYPE(Face)

static void
free_freetype() {
    FT_Done_FreeType(library);
}

bool 
init_freetype_library(PyObject *m) {
    FreeType_Exception = PyErr_NewException("fast_data_types.FreeTypeError", NULL, NULL);
    if (FreeType_Exception == NULL) return false;
    if (PyModule_AddObject(m, "FreeTypeError", FreeType_Exception) != 0) return false;
    int error = FT_Init_FreeType(&library);
    if (error) {
        set_freetype_error("Failed to initialize FreeType library, with error:", error);
        return false;
    }
    if (Py_AtExit(free_freetype) != 0) {
        PyErr_SetString(FreeType_Exception, "Failed to register the freetype library at exit handler");
        return false;
    }
    if (PyStructSequence_InitType2(&ShapeFieldsType, &shape_fields_desc) != 0) return false;
    PyModule_AddObject(m, "ShapeFields", (PyObject*)&ShapeFieldsType);
    PyModule_AddIntMacro(m, FT_LOAD_RENDER);
    PyModule_AddIntMacro(m, FT_LOAD_TARGET_NORMAL);
    PyModule_AddIntMacro(m, FT_LOAD_TARGET_LIGHT);
    PyModule_AddIntMacro(m, FT_LOAD_NO_HINTING);
    PyModule_AddIntMacro(m, FT_PIXEL_MODE_GRAY);
    return true;
}

// }}}
