/* o42-python.c - Python inside the spreadsheet
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The C half of the `office42` module: a dozen plain functions over
 * the book, sheets and cells, gathered in a private module `_office42`.
 * The Python half, office42.py, builds Book, Sheet and Range on top of
 * them and is compiled in as a string.  A script that defines a
 * function with @office42.function registers it with the evaluator,
 * so =NAME(...) in a cell calls back into Python.
 */

#include "o42-python.h"

#include "o42-sheet.h"
#include "o42-eval.h"
#include "o42-formula.h"

#include <string.h>

#ifndef HAVE_PYTHON

gboolean    o42_python_available (void) { return FALSE; }
const char *o42_python_version   (void) { return NULL; }
void        o42_python_reset     (void) { }

gboolean
o42_python_run (O42Book *book, O42Sheet *sheet, const char *code, const char *filename, char **output)
{
  (void) book; (void) sheet; (void) code; (void) filename;
  if (output != NULL)
    *output = g_strdup ("This build of Office42 Spreadsheet has no Python in it.\n");
  return FALSE;
}

gboolean
o42_python_run_file (O42Book *book, O42Sheet *sheet, GFile *file, char **output)
{
  (void) file;
  return o42_python_run (book, sheet, "", NULL, output);
}

#else /* HAVE_PYTHON */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#ifdef G_OS_WIN32
#include <windows.h>
#endif

#include "office42-py.h"   /* generated from office42.py: OFFICE42_PY */

static O42Book  *current_book  = NULL;
static O42Sheet *current_sheet = NULL;
static PyObject *module        = NULL;   /* the office42 module */
static PyObject *error_class   = NULL;   /* office42.Error */
static char     *init_failure  = NULL;
static gboolean  book_touched  = FALSE;  /* cells changed since the run began */
static gboolean  sheets_touched = FALSE; /* sheets added, removed or renamed */

/* ---- Between the two value systems --------------------------------- */

static PyObject *
value_to_py (const O42Value *v)
{
  switch (v->type)
    {
    case O42_VALUE_NUMBER: return PyFloat_FromDouble (v->as.number);
    case O42_VALUE_TEXT:   return PyUnicode_FromString (v->as.text != NULL ? v->as.text : "");
    case O42_VALUE_BOOL:   return PyBool_FromLong (v->as.boolean);
    case O42_VALUE_ERROR:  return PyObject_CallFunction (error_class, "s", o42_error_name (v->as.error));
    case O42_VALUE_EMPTY:
    default:               Py_RETURN_NONE;
    }
}

static O42Value
py_to_value (PyObject *o)
{
  if (o == NULL || o == Py_None)
    return o42_value_empty ();
  if (PyBool_Check (o))
    return o42_value_bool (o == Py_True);
  if (PyLong_Check (o) || PyFloat_Check (o))
    {
      double d = PyFloat_AsDouble (o);
      if (PyErr_Occurred ())
        {
          PyErr_Clear ();
          return o42_value_error (O42_ERR_NUM);
        }
      return o42_value_number (d);
    }
  if (PyUnicode_Check (o))
    {
      const char *s = PyUnicode_AsUTF8 (o);
      return o42_value_text (s != NULL ? s : "");
    }
  if (error_class != NULL && PyObject_IsInstance (o, error_class) == 1)
    {
      PyObject *text = PyObject_Str (o);
      const char *s = text != NULL ? PyUnicode_AsUTF8 (text) : NULL;
      O42ErrorCode code = O42_ERR_VALUE;
      for (int e = O42_ERR_NULL; e <= O42_ERR_SPILL; e++)
        if (s != NULL && strcmp (s, o42_error_name ((O42ErrorCode) e)) == 0)
          code = (O42ErrorCode) e;
      Py_XDECREF (text);
      return o42_value_error (code);
    }
  {
    /* Anything else shows as its text, which is what a console would
     * do with it. */
    PyObject *text = PyObject_Str (o);
    const char *s = text != NULL ? PyUnicode_AsUTF8 (text) : NULL;
    O42Value v = o42_value_text (s != NULL ? s : "");
    Py_XDECREF (text);
    PyErr_Clear ();
    return v;
  }
}

/* ---- The _office42 module ------------------------------------------ */

static O42Sheet *
sheet_arg (int index)
{
  O42Sheet *sheet = current_book != NULL ? o42_book_sheet (current_book, index) : NULL;
  if (sheet == NULL)
    PyErr_Format (PyExc_IndexError, "no sheet %d", index);
  return sheet;
}

static gboolean
cell_ok (int row, int col)
{
  if (row < 0 || col < 0 || row >= O42_MAX_ROWS || col >= O42_MAX_COLS)
    {
      PyErr_Format (PyExc_IndexError, "cell (%d, %d) is off the sheet", row, col);
      return FALSE;
    }
  return TRUE;
}

static PyObject *
m_n_sheets (PyObject *self, PyObject *args)
{
  (void) self; (void) args;
  return PyLong_FromLong (current_book != NULL ? o42_book_n_sheets (current_book) : 0);
}

static PyObject *
m_sheet_name (PyObject *self, PyObject *args)
{
  int index;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "i", &index) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  return PyUnicode_FromString (o42_sheet_get_name (sheet));
}

static PyObject *
m_sheet_index (PyObject *self, PyObject *args)
{
  const char *name;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "s", &name))
    return NULL;
  sheet = current_book != NULL ? o42_book_find_sheet (current_book, name) : NULL;
  return PyLong_FromLong (sheet != NULL ? o42_book_sheet_index (current_book, sheet) : -1);
}

static PyObject *
m_current (PyObject *self, PyObject *args)
{
  (void) self; (void) args;
  return PyLong_FromLong (current_book != NULL && current_sheet != NULL
                          ? o42_book_sheet_index (current_book, current_sheet) : 0);
}

static PyObject *
m_add_sheet (PyObject *self, PyObject *args)
{
  const char *name;
  int index = -1;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "s|i", &name, &index))
    return NULL;
  if (current_book == NULL)
    Py_RETURN_NONE;
  sheet = o42_book_add_sheet (current_book, name, index);
  if (sheet == NULL)
    return PyErr_Format (PyExc_ValueError, "cannot add a sheet named %s", name);
  sheets_touched = TRUE;
  return PyLong_FromLong (o42_book_sheet_index (current_book, sheet));
}

static PyObject *
m_remove_sheet (PyObject *self, PyObject *args)
{
  int index;
  (void) self;
  if (!PyArg_ParseTuple (args, "i", &index) || sheet_arg (index) == NULL)
    return NULL;
  if (!o42_book_remove_sheet (current_book, index))
    return PyErr_Format (PyExc_ValueError, "cannot remove the only sheet");
  sheets_touched = TRUE;
  current_sheet = o42_book_sheet (current_book, 0);
  Py_RETURN_NONE;
}

static PyObject *
m_rename_sheet (PyObject *self, PyObject *args)
{
  int index;
  const char *name;
  (void) self;
  if (!PyArg_ParseTuple (args, "is", &index, &name) || sheet_arg (index) == NULL)
    return NULL;
  if (!o42_book_rename_sheet (current_book, index, name))
    return PyErr_Format (PyExc_ValueError, "cannot rename a sheet to %s", name);
  sheets_touched = TRUE;
  Py_RETURN_NONE;
}

static PyObject *
m_get_input (PyObject *self, PyObject *args)
{
  int index, row, col;
  O42Sheet *sheet;
  char *input;
  PyObject *result;
  (void) self;
  if (!PyArg_ParseTuple (args, "iii", &index, &row, &col) || (sheet = sheet_arg (index)) == NULL || !cell_ok (row, col))
    return NULL;
  input = o42_sheet_get_input (sheet, row, col);
  result = PyUnicode_FromString (input != NULL ? input : "");
  g_free (input);
  return result;
}

static PyObject *
m_set_input (PyObject *self, PyObject *args)
{
  int index, row, col;
  const char *text;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "iiis", &index, &row, &col, &text) || (sheet = sheet_arg (index)) == NULL || !cell_ok (row, col))
    return NULL;
  o42_sheet_set_input (sheet, row, col, text);
  book_touched = TRUE;
  Py_RETURN_NONE;
}

static PyObject *
m_get_value (PyObject *self, PyObject *args)
{
  int index, row, col;
  O42Sheet *sheet;
  O42Value v;
  PyObject *result;
  (void) self;
  if (!PyArg_ParseTuple (args, "iii", &index, &row, &col) || (sheet = sheet_arg (index)) == NULL || !cell_ok (row, col))
    return NULL;
  o42_sheet_get_value (sheet, row, col, &v);
  result = value_to_py (&v);
  o42_value_clear (&v);
  return result;
}

static PyObject *
m_get_display (PyObject *self, PyObject *args)
{
  int index, row, col;
  O42Sheet *sheet;
  O42Value v;
  char *text;
  PyObject *result;
  (void) self;
  if (!PyArg_ParseTuple (args, "iii", &index, &row, &col) || (sheet = sheet_arg (index)) == NULL || !cell_ok (row, col))
    return NULL;
  o42_sheet_get_value (sheet, row, col, &v);
  text = o42_fmt_display (o42_sheet_get_fmt (sheet, row, col), &v);
  result = PyUnicode_FromString (text != NULL ? text : "");
  g_free (text);
  o42_value_clear (&v);
  return result;
}

static PyObject *
m_used_range (PyObject *self, PyObject *args)
{
  int index;
  O42Sheet *sheet;
  O42Range r;
  (void) self;
  if (!PyArg_ParseTuple (args, "i", &index) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  o42_sheet_used_range (sheet, &r);
  if (r.row1 < r.row0 || r.col1 < r.col0)
    Py_RETURN_NONE;
  return Py_BuildValue ("(iiii)", r.row0, r.col0, r.row1, r.col1);
}

static PyObject *
m_begin (PyObject *self, PyObject *args)
{
  int index;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "i", &index) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  o42_sheet_begin_group (sheet);
  Py_RETURN_NONE;
}

static PyObject *
m_end (PyObject *self, PyObject *args)
{
  int index;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "i", &index) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  o42_sheet_end_group (sheet);
  Py_RETURN_NONE;
}

static PyObject *
m_ref_parse (PyObject *self, PyObject *args)
{
  const char *text;
  int row, col;
  gsize used = 0;
  (void) self;
  if (!PyArg_ParseTuple (args, "s", &text))
    return NULL;
  if (!o42_ref_parse (text, &row, &col, &used))
    Py_RETURN_NONE;
  return Py_BuildValue ("(iin)", row, col, (Py_ssize_t) used);
}

static PyObject *
m_ref_name (PyObject *self, PyObject *args)
{
  int row, col;
  char *name;
  PyObject *result;
  (void) self;
  if (!PyArg_ParseTuple (args, "ii", &row, &col))
    return NULL;
  name = o42_ref_name (row, col);
  result = PyUnicode_FromString (name);
  g_free (name);
  return result;
}

static const char *HALIGNS[] = { "general", "left", "centre", "right" };
static const char *VALIGNS[] = { "bottom", "middle", "top" };
static const char *NUMBERS[] = { "general", "fixed", "comma", "currency", "percent",
                                 "scientific", "text", "date", "time", "datetime" };

static gboolean
colour_arg (PyObject *o, guint32 *out)
{
  if (PyLong_Check (o))
    {
      *out = (guint32) PyLong_AsUnsignedLong (o);
      return !PyErr_Occurred ();
    }
  if (PyUnicode_Check (o))
    {
      const char *s = PyUnicode_AsUTF8 (o);
      if (s != NULL && s[0] == '#' && strlen (s) == 7)
        {
          *out = (guint32) g_ascii_strtoull (s + 1, NULL, 16);
          return TRUE;
        }
    }
  PyErr_SetString (PyExc_ValueError, "a colour is an int 0xRRGGBB or a text '#rrggbb'");
  return FALSE;
}

static int
choice_arg (PyObject *o, const char **choices, int n, const char *what)
{
  const char *s = PyUnicode_Check (o) ? PyUnicode_AsUTF8 (o) : NULL;
  if (s != NULL)
    for (int i = 0; i < n; i++)
      if (g_ascii_strcasecmp (s, choices[i]) == 0 ||
          (strcmp (choices[i], "centre") == 0 && g_ascii_strcasecmp (s, "center") == 0))
        return i;
  PyErr_Format (PyExc_ValueError, "unknown %s %S", what, o);
  return -1;
}

static PyObject *
m_set_format (PyObject *self, PyObject *args, PyObject *kwargs)
{
  int index;
  O42Range r;
  O42Sheet *sheet;
  O42Fmt fmt;
  O42FmtMask mask = 0;
  PyObject *key, *value;
  Py_ssize_t pos = 0;
  (void) self;

  if (!PyArg_ParseTuple (args, "iiiii", &index, &r.row0, &r.col0, &r.row1, &r.col1) ||
      (sheet = sheet_arg (index)) == NULL || !cell_ok (r.row0, r.col0) || !cell_ok (r.row1, r.col1))
    return NULL;
  o42_fmt_init_default (&fmt);
  while (kwargs != NULL && PyDict_Next (kwargs, &pos, &key, &value))
    {
      const char *k = PyUnicode_AsUTF8 (key);
      int choice;
      if (k == NULL)
        return NULL;
      if (strcmp (k, "bold") == 0)           { fmt.bold = PyObject_IsTrue (value); mask |= O42_FMT_BOLD; }
      else if (strcmp (k, "italic") == 0)    { fmt.italic = PyObject_IsTrue (value); mask |= O42_FMT_ITALIC; }
      else if (strcmp (k, "underline") == 0) { fmt.underline = PyObject_IsTrue (value); mask |= O42_FMT_UNDERLINE; }
      else if (strcmp (k, "strikeout") == 0) { fmt.strikeout = PyObject_IsTrue (value); mask |= O42_FMT_STRIKEOUT; }
      else if (strcmp (k, "wrap") == 0)      { fmt.wrap = PyObject_IsTrue (value); mask |= O42_FMT_WRAP; }
      else if (strcmp (k, "borders") == 0)
        {
          int on = PyObject_IsTrue (value);
          for (int i = 0; i < 4; i++)
            fmt.border_style[i] = on ? O42_BORDER_THIN : O42_BORDER_NONE;
          o42_fmt_sync_borders (&fmt);
          mask |= O42_FMT_BORDERS;
        }
      else if (strcmp (k, "border_style") == 0)
        {
          const char *s = PyUnicode_Check (value) ? PyUnicode_AsUTF8 (value) : NULL;
          O42BorderStyle style;
          if (s == NULL || !o42_border_style_parse (s, &style))
            return PyErr_Format (PyExc_ValueError, "border_style is none, thin, medium, thick, double, dashed or dotted");
          for (int i = 0; i < 4; i++)
            fmt.border_style[i] = style;
          o42_fmt_sync_borders (&fmt);
          mask |= O42_FMT_BORDERS;
        }
      else if (strcmp (k, "border_colour") == 0 || strcmp (k, "border_color") == 0)
        {
          guint32 bc;
          if (!colour_arg (value, &bc)) return NULL;
          for (int i = 0; i < 4; i++)
            fmt.border_colour[i] = bc;
          if (!(mask & O42_FMT_BORDERS))
            {
              const O42Fmt *have = o42_sheet_get_fmt (sheet, r.row0, r.col0);
              for (int i = 0; i < 4; i++)
                fmt.border_style[i] = have->border_style[i];
              o42_fmt_sync_borders (&fmt);
            }
          mask |= O42_FMT_BORDERS;
        }
      else if (strcmp (k, "indent") == 0)
        {
          fmt.indent = (guint8) CLAMP (PyLong_AsLong (value), 0, 15);
          if (PyErr_Occurred ()) return NULL;
          mask |= O42_FMT_INDENT;
        }
      else if (strcmp (k, "rotation") == 0)
        {
          fmt.rotation = (gint16) CLAMP (PyLong_AsLong (value), -90, 90);
          if (PyErr_Occurred ()) return NULL;
          mask |= O42_FMT_ROTATION;
        }
      else if (strcmp (k, "size") == 0)
        {
          double pt = PyFloat_AsDouble (value);
          if (PyErr_Occurred ()) return NULL;
          fmt.size = (int) (pt * 2 + 0.5);
          mask |= O42_FMT_SIZE;
        }
      else if (strcmp (k, "family") == 0 || strcmp (k, "font") == 0)
        {
          const char *s = PyUnicode_Check (value) ? PyUnicode_AsUTF8 (value) : NULL;
          if (s == NULL) return PyErr_Format (PyExc_TypeError, "%s must be a text", k);
          fmt.family = g_intern_string (s);
          mask |= O42_FMT_FAMILY;
        }
      else if (strcmp (k, "colour") == 0 || strcmp (k, "color") == 0)
        {
          if (!colour_arg (value, &fmt.colour)) return NULL;
          mask |= O42_FMT_COLOUR;
        }
      else if (strcmp (k, "fill") == 0)
        {
          if (value == Py_None) fmt.fill = O42_FILL_NONE;
          else if (!colour_arg (value, &fmt.fill)) return NULL;
          mask |= O42_FMT_FILL;
        }
      else if (strcmp (k, "halign") == 0 || strcmp (k, "align") == 0)
        {
          if ((choice = choice_arg (value, HALIGNS, G_N_ELEMENTS (HALIGNS), "alignment")) < 0) return NULL;
          fmt.halign = (O42HAlign) choice;
          mask |= O42_FMT_HALIGN;
        }
      else if (strcmp (k, "valign") == 0)
        {
          if ((choice = choice_arg (value, VALIGNS, G_N_ELEMENTS (VALIGNS), "vertical alignment")) < 0) return NULL;
          fmt.valign = (O42VAlign) choice;
          mask |= O42_FMT_VALIGN;
        }
      else if (strcmp (k, "number") == 0 || strcmp (k, "number_format") == 0)
        {
          const char *s = PyUnicode_Check (value) ? PyUnicode_AsUTF8 (value) : NULL;
          int found = -1;
          if (s == NULL) return PyErr_Format (PyExc_TypeError, "%s must be a text", k);
          for (guint i = 0; i < G_N_ELEMENTS (NUMBERS); i++)
            if (g_ascii_strcasecmp (s, NUMBERS[i]) == 0) found = (int) i;
          if (found >= 0)
            {
              fmt.number = (O42NumberFormat) found;
              fmt.custom = NULL;
            }
          else
            fmt.custom = g_intern_string (s);   /* a format code, "#,##0.00" */
          mask |= O42_FMT_NUMBER;
        }
      else if (strcmp (k, "decimals") == 0)
        {
          fmt.decimals = (int) PyLong_AsLong (value);
          if (PyErr_Occurred ()) return NULL;
          mask |= O42_FMT_DECIMALS;
        }
      else
        return PyErr_Format (PyExc_TypeError, "no format property named %s", k);
    }
  if (mask != 0)
    {
      r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
      o42_sheet_apply_fmt (sheet, &r, mask, &fmt);
      book_touched = TRUE;
    }
  Py_RETURN_NONE;
}

static PyObject *
m_get_format (PyObject *self, PyObject *args)
{
  int index, row, col;
  O42Sheet *sheet;
  const O42Fmt *f;
  PyObject *fill;
  (void) self;
  if (!PyArg_ParseTuple (args, "iii", &index, &row, &col) || (sheet = sheet_arg (index)) == NULL || !cell_ok (row, col))
    return NULL;
  f = o42_sheet_get_fmt (sheet, row, col);
  /* The fill is a new object, handed over with N so that it is not
   * leaked; None is borrowed and gets its own reference first. */
  fill = f->fill == O42_FILL_NONE ? Py_None : PyLong_FromUnsignedLong (f->fill);
  if (fill == Py_None)
    Py_INCREF (fill);
  return Py_BuildValue ("{s:s,s:d,s:O,s:O,s:O,s:O,s:O,s:O,s:I,s:N,s:s,s:s,s:s,s:i}",
                        "family", f->family, "size", f->size / 2.0,
                        "bold", f->bold ? Py_True : Py_False,
                        "italic", f->italic ? Py_True : Py_False,
                        "underline", f->underline ? Py_True : Py_False,
                        "strikeout", f->strikeout ? Py_True : Py_False,
                        "wrap", f->wrap ? Py_True : Py_False,
                        "borders", (f->border_top && f->border_bottom && f->border_left && f->border_right) ? Py_True : Py_False,
                        "colour", (unsigned int) f->colour,
                        "fill", fill,
                        "halign", HALIGNS[f->halign], "valign", VALIGNS[f->valign],
                        "number", f->custom != NULL ? f->custom : NUMBERS[f->number],
                        "decimals", f->decimals);
}

/* A cell formula calling a function a script defined. */
static O42Value
python_function (O42EvalContext *ctx, const char *name, O42Operand *args, int n_args, gpointer user)
{
  O42Sheet *saved_sheet = current_sheet;
  O42Book *saved_book = current_book;
  PyObject *list, *result;
  O42Value value;
  (void) user;

  /* The formula's sheet is the script's sheet while it runs. */
  current_sheet = ctx->user_data;
  current_book = current_sheet != NULL ? o42_sheet_get_book (current_sheet) : current_book;

  /* =PY() in a cell is code from the file, and it runs only once the
   * user has said the book's Python may: until then it is no function
   * at all, as it is where Python is off. */
  if (strcmp (name, "PY") == 0 && !o42_book_scripts_trusted (current_book))
    {
      current_sheet = saved_sheet;
      current_book = saved_book;
      return o42_value_error (O42_ERR_NAME);
    }

  list = PyList_New (n_args);
  for (int i = 0; i < n_args; i++)
    {
      PyObject *item;
      if (args[i].is_range)
        {
          int rows, cols;
          o42_operand_dims (&args[i], &rows, &cols);
          item = PyList_New (rows);
          for (int r = 0; r < rows; r++)
            {
              PyObject *line = PyList_New (cols);
              for (int c = 0; c < cols; c++)
                {
                  O42Value v;
                  o42_operand_cell (ctx, &args[i], r, c, &v);
                  PyList_SET_ITEM (line, c, value_to_py (&v));
                  o42_value_clear (&v);
                }
              PyList_SET_ITEM (item, r, line);
            }
        }
      else
        item = value_to_py (&args[i].value);
      PyList_SET_ITEM (list, i, item);
    }

  result = PyObject_CallMethod (module, "_call", "sO", name, list);
  Py_DECREF (list);
  if (result == NULL)
    {
      PyErr_Clear ();
      value = o42_value_error (O42_ERR_VALUE);
    }
  else
    {
      value = py_to_value (result);
      Py_DECREF (result);
    }
  current_sheet = saved_sheet;
  current_book = saved_book;
  return value;
}

static PyObject *
m_define (PyObject *self, PyObject *args)
{
  const char *name, *signature, *summary;
  int min_args, max_args;
  (void) self;
  if (!PyArg_ParseTuple (args, "siiss", &name, &min_args, &max_args, &signature, &summary))
    return NULL;
  o42_function_register_external (name, min_args, max_args, signature, summary, python_function, NULL);
  Py_RETURN_NONE;
}

static PyObject *
m_undefine (PyObject *self, PyObject *args)
{
  const char *name;
  (void) self;
  if (!PyArg_ParseTuple (args, "s", &name))
    return NULL;
  return PyBool_FromLong (o42_function_unregister_external (name));
}

static PyObject *
m_is_builtin (PyObject *self, PyObject *args)
{
  const char *name;
  (void) self;
  if (!PyArg_ParseTuple (args, "s", &name))
    return NULL;
  return PyBool_FromLong (o42_function_exists (name) && !o42_function_is_external (name));
}

static PyObject *
m_function_names (PyObject *self, PyObject *args)
{
  guint n = 0;
  const char * const *names = o42_function_names (&n);
  PyObject *list = PyList_New (n);
  (void) self; (void) args;
  for (guint i = 0; i < n; i++)
    PyList_SET_ITEM (list, i, PyUnicode_FromString (names[i]));
  return list;
}

static PyObject *
m_evaluate (PyObject *self, PyObject *args)
{
  /* A formula evaluated on the current sheet, as a cell would. */
  const char *text;
  O42Value v;
  PyObject *result;
  (void) self;
  if (!PyArg_ParseTuple (args, "s", &text))
    return NULL;
  if (current_sheet == NULL)
    Py_RETURN_NONE;
  v = o42_sheet_evaluate_formula (current_sheet, text);
  result = value_to_py (&v);
  o42_value_clear (&v);
  return result;
}

static PyObject *
m_scripts (PyObject *self, PyObject *args)
{
  int n = current_book != NULL ? o42_book_n_scripts (current_book) : 0;
  PyObject *list = PyList_New (n);
  (void) self; (void) args;
  for (int i = 0; i < n; i++)
    PyList_SET_ITEM (list, i, PyUnicode_FromString (o42_book_script_name (current_book, i)));
  return list;
}

static PyObject *
m_get_script (PyObject *self, PyObject *args)
{
  const char *name, *code;
  (void) self;
  if (!PyArg_ParseTuple (args, "s", &name))
    return NULL;
  code = current_book != NULL ? o42_book_script_code (current_book, name) : NULL;
  if (code == NULL)
    return PyErr_Format (PyExc_KeyError, "no script named %s", name);
  return PyUnicode_FromString (code);
}

static PyObject *
m_set_script (PyObject *self, PyObject *args)
{
  const char *name, *code;
  (void) self;
  if (!PyArg_ParseTuple (args, "ss", &name, &code))
    return NULL;
  if (current_book != NULL)
    o42_book_set_script (current_book, name, code);
  Py_RETURN_NONE;
}

static PyObject *
m_remove_script (PyObject *self, PyObject *args)
{
  const char *name;
  (void) self;
  if (!PyArg_ParseTuple (args, "s", &name))
    return NULL;
  return PyBool_FromLong (current_book != NULL && o42_book_remove_script (current_book, name));
}

static PyMethodDef METHODS[] = {
  { "scripts",        m_scripts,        METH_NOARGS,  "The names of the book's scripts." },
  { "get_script",     m_get_script,     METH_VARARGS, "A script's code." },
  { "set_script",     m_set_script,     METH_VARARGS, "Stores a script in the book." },
  { "remove_script",  m_remove_script,  METH_VARARGS, "Removes a script from the book." },
  { "n_sheets",       m_n_sheets,       METH_NOARGS,  "How many sheets the book has." },
  { "sheet_name",     m_sheet_name,     METH_VARARGS, "The name of sheet i." },
  { "sheet_index",    m_sheet_index,    METH_VARARGS, "The index of the sheet named so, or -1." },
  { "current",        m_current,        METH_NOARGS,  "The index of the sheet on show." },
  { "add_sheet",      m_add_sheet,      METH_VARARGS, "Adds a sheet; its index." },
  { "remove_sheet",   m_remove_sheet,   METH_VARARGS, "Removes sheet i." },
  { "rename_sheet",   m_rename_sheet,   METH_VARARGS, "Renames sheet i." },
  { "get_input",      m_get_input,      METH_VARARGS, "What was typed into a cell." },
  { "set_input",      m_set_input,      METH_VARARGS, "Types into a cell." },
  { "get_value",      m_get_value,      METH_VARARGS, "A cell's value." },
  { "get_display",    m_get_display,    METH_VARARGS, "A cell's value as shown." },
  { "used_range",     m_used_range,     METH_VARARGS, "The rectangle with something in it, or None." },
  { "begin",          m_begin,          METH_VARARGS, "Opens an undo group." },
  { "end",            m_end,            METH_VARARGS, "Closes an undo group." },
  { "ref_parse",      m_ref_parse,      METH_VARARGS, "A1 -> (row, col, length)." },
  { "ref_name",       m_ref_name,       METH_VARARGS, "(row, col) -> A1." },
  { "set_format",     (PyCFunction) (void (*) (void)) m_set_format, METH_VARARGS | METH_KEYWORDS, "Formats a range." },
  { "get_format",     m_get_format,     METH_VARARGS, "A cell's format as a dict." },
  { "define",         m_define,         METH_VARARGS, "Registers a function with the evaluator." },
  { "undefine",       m_undefine,       METH_VARARGS, "Forgets a registered function." },
  { "is_builtin",     m_is_builtin,     METH_VARARGS, "Whether a name is a built-in function." },
  { "function_names", m_function_names, METH_NOARGS,  "Every function the evaluator knows." },
  { "evaluate",       m_evaluate,       METH_VARARGS, "Evaluates a formula on the current sheet." },
  { NULL, NULL, 0, NULL }
};

static struct PyModuleDef MODULE_DEF = {
  PyModuleDef_HEAD_INIT, "_office42", "The C half of the office42 module.", -1, METHODS,
  NULL, NULL, NULL, NULL
};

static PyObject *
init_private_module (void)
{
  return PyModule_Create (&MODULE_DEF);
}

/* ---- The interpreter ----------------------------------------------- */

#ifdef G_OS_WIN32
/* Python finds its standard library from its home directory; an
 * embedding program has to say where that is.  The DLL knows: it lives
 * in HOME/bin (MSYS2) or in HOME itself (python.org). */
static void
set_home_from_dll (PyConfig *config)
{
  const char *names[] = { "libpython" G_STRINGIFY (PY_MAJOR_VERSION) "." G_STRINGIFY (PY_MINOR_VERSION) ".dll",
                          "python" G_STRINGIFY (PY_MAJOR_VERSION) G_STRINGIFY (PY_MINOR_VERSION) ".dll",
                          "python3.dll" };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    {
      HMODULE dll = GetModuleHandleA (names[i]);
      wchar_t path[MAX_PATH];
      if (dll != NULL && GetModuleFileNameW (dll, path, MAX_PATH) > 0)
        {
          char *utf8 = g_utf16_to_utf8 ((gunichar2 *) path, -1, NULL, NULL, NULL);
          char *dir = g_path_get_dirname (utf8);
          char *lib = g_build_filename (dir, "lib", NULL);
          char *home;
          if (g_file_test (lib, G_FILE_TEST_IS_DIR))
            home = g_strdup (dir);
          else
            home = g_path_get_dirname (dir);     /* .../bin/libpython3.14.dll */
          {
            wchar_t *whome = (wchar_t *) g_utf8_to_utf16 (home, -1, NULL, NULL, NULL);
            PyConfig_SetString (config, &config->home, whome);
            g_free (whome);
          }
          g_free (home);
          g_free (lib);
          g_free (dir);
          g_free (utf8);
          return;
        }
    }
}
#endif

static gboolean
ensure_interpreter (void)
{
  PyConfig config;
  PyStatus status;
  PyObject *code, *loaded;

  if (module != NULL)
    return TRUE;
  if (init_failure != NULL)
    return FALSE;

  PyImport_AppendInittab ("_office42", init_private_module);
  PyConfig_InitPythonConfig (&config);
  config.install_signal_handlers = 0;
  config.parse_argv = 0;
  config.site_import = 1;
  PyConfig_SetString (&config, &config.program_name, L"office42");
#ifdef G_OS_WIN32
  set_home_from_dll (&config);
#endif
  status = Py_InitializeFromConfig (&config);
  PyConfig_Clear (&config);
  if (PyStatus_Exception (status))
    {
      init_failure = g_strdup_printf ("Python could not start: %s", status.err_msg != NULL ? status.err_msg : "unknown reason");
      return FALSE;
    }

  code = Py_CompileString (OFFICE42_PY, "office42.py", Py_file_input);
  loaded = code != NULL ? PyImport_ExecCodeModule ("office42", code) : NULL;
  Py_XDECREF (code);
  if (loaded == NULL)
    {
      PyObject *type, *value, *trace, *text;
      PyErr_Fetch (&type, &value, &trace);
      text = value != NULL ? PyObject_Str (value) : NULL;
      init_failure = g_strdup_printf ("The office42 module did not load: %s",
                                      text != NULL ? PyUnicode_AsUTF8 (text) : "unknown reason");
      Py_XDECREF (text); Py_XDECREF (type); Py_XDECREF (value); Py_XDECREF (trace);
      return FALSE;
    }
  module = loaded;
  error_class = PyObject_GetAttrString (module, "Error");
  PyModule_AddStringConstant (module, "__version__", O42_VERSION);
  return TRUE;
}

gboolean
o42_python_available (void)
{
  return TRUE;
}

const char *
o42_python_version (void)
{
  return PY_VERSION;
}

gboolean
o42_python_run (O42Book *book, O42Sheet *sheet, const char *code, const char *filename, char **output)
{
  O42Sheet *saved_sheet = current_sheet;
  O42Book *saved_book = current_book;
  gboolean saved_touched = book_touched, saved_sheets = sheets_touched;
  gboolean was_trusted;
  PyObject *result;
  gboolean ok = FALSE;
  const char *text = "";

  g_return_val_if_fail (book != NULL && code != NULL, FALSE);
  was_trusted = o42_book_scripts_trusted (book);
  if (!ensure_interpreter ())
    {
      if (output != NULL)
        *output = g_strdup_printf ("%s\n", init_failure);
      return FALSE;
    }

  current_book = book;
  current_sheet = sheet != NULL ? sheet : o42_book_sheet (book, 0);
  book_touched = sheets_touched = FALSE;
  /* Running Python against the book is the user saying its Python may
   * run, =PY() cells included. */
  o42_book_set_scripts_trusted (book, TRUE);
  if (current_sheet != NULL)
    o42_sheet_begin_group (current_sheet);
  result = PyObject_CallMethod (module, "_run", "ss", code, filename != NULL ? filename : "<console>");
  /* The group is the book's, so it is ended on whichever sheet is still
   * there: a script that removed its own sheet once left it open, and
   * every edit after that fell into it. */
  if (current_sheet != NULL)
    {
      O42Sheet *still = o42_book_sheet_index (book, current_sheet) >= 0
                        ? current_sheet : o42_book_sheet (book, 0);
      if (still != NULL)
        o42_sheet_end_group (still);
    }
  if (result != NULL && PyTuple_Check (result) && PyTuple_Size (result) == 2)
    {
      ok = PyObject_IsTrue (PyTuple_GetItem (result, 0));
      text = PyUnicode_AsUTF8 (PyTuple_GetItem (result, 1));
    }
  else
    {
      PyErr_Clear ();
      text = "The script runner itself failed.\n";
    }
  if (output != NULL)
    *output = g_strdup (text != NULL ? text : "");
  Py_XDECREF (result);
  /* The book's Python became runnable just now: the =PY() cells that
   * were #NAME? are worked out again. */
  if (!was_trusted)
    {
      for (int i = 0; i < o42_book_n_sheets (book); i++)
        o42_sheet_touch_volatiles (o42_book_sheet (book, i));
      book_touched = TRUE;
    }
  if (sheets_touched)
    o42_book_changed (book, "sheets");
  else if (book_touched)
    o42_book_changed (book, "cells");
  current_sheet = saved_sheet;
  current_book = saved_book;
  book_touched = saved_touched;
  sheets_touched = saved_sheets;
  return ok;
}

gboolean
o42_python_run_file (O42Book *book, O42Sheet *sheet, GFile *file, char **output)
{
  char *code = NULL;
  char *name;
  GError *error = NULL;
  gboolean ok;

  g_return_val_if_fail (file != NULL, FALSE);
  if (!g_file_load_contents (file, NULL, &code, NULL, NULL, &error))
    {
      if (output != NULL)
        *output = g_strdup_printf ("%s\n", error != NULL ? error->message : "cannot read the file");
      g_clear_error (&error);
      return FALSE;
    }
  name = g_file_get_path (file);
  ok = o42_python_run (book, sheet, code, name != NULL ? name : "<file>", output);
  g_free (name);
  g_free (code);
  return ok;
}

void
o42_python_reset (void)
{
  PyObject *r;
  if (module == NULL)
    return;
  r = PyObject_CallMethod (module, "_reset", NULL);
  Py_XDECREF (r);
  PyErr_Clear ();
}

#endif /* HAVE_PYTHON */
