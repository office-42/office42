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
#include "o42-pattern.h"

#include <string.h>
#include <glib/gstdio.h>

/* What the window does for a script; every call NULL until it says. */
static O42PythonHost host;

void
o42_python_set_host (const O42PythonHost *table)
{
  if (table != NULL)
    host = *table;
  else
    memset (&host, 0, sizeof host);
}

char *
o42_python_personal_folder (void)
{
  char *folder = g_build_filename (g_get_user_data_dir (), "office42", "scripts", NULL);
  g_mkdir_with_parents (folder, 0700);
  return folder;
}

char **
o42_python_personal_scripts (void)
{
  char *folder = o42_python_personal_folder ();
  GDir *dir = g_dir_open (folder, 0, NULL);
  GPtrArray *paths = g_ptr_array_new ();
  const char *name;

  while (dir != NULL && (name = g_dir_read_name (dir)) != NULL)
    if (g_str_has_suffix (name, ".py"))
      g_ptr_array_add (paths, g_build_filename (folder, name, NULL));
  if (dir != NULL)
    g_dir_close (dir);
  g_ptr_array_sort_values (paths, (GCompareFunc) g_strcmp0);
  g_ptr_array_add (paths, NULL);
  g_free (folder);
  return (char **) g_ptr_array_free (paths, FALSE);
}

#ifndef HAVE_PYTHON

gboolean    o42_python_available (void) { return FALSE; }
const char *o42_python_version   (void) { return NULL; }
void        o42_python_reset     (void) { }
void        o42_python_forget_book (O42Book *book) { (void) book; }
gboolean    o42_python_start     (void) { return FALSE; }

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

/* The book a script runs against, as a number scripts key their
 * functions by; 0 while the personal scripts load. */
static PyObject *
m_book_id (PyObject *self, PyObject *args)
{
  (void) self; (void) args;
  return PyLong_FromSize_t ((size_t) (guintptr) current_book);
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
  O42Sheet *gone;
  (void) self;
  if (!PyArg_ParseTuple (args, "i", &index) || (gone = sheet_arg (index)) == NULL)
    return NULL;
  if (!o42_book_remove_sheet (current_book, index))
    return PyErr_Format (PyExc_ValueError, "cannot remove the only sheet");
  sheets_touched = TRUE;
  /* Only a script that removed the sheet it was running on moves to
   * another; removing a scratch sheet leaves `sheet` where it was. */
  if (gone == current_sheet)
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
m_move_sheet (PyObject *self, PyObject *args)
{
  int from, to;
  (void) self;
  if (!PyArg_ParseTuple (args, "ii", &from, &to) || sheet_arg (from) == NULL)
    return NULL;
  if (!o42_book_move_sheet (current_book, from, to))
    return PyErr_Format (PyExc_IndexError, "no place %d for a sheet", to);
  sheets_touched = TRUE;
  Py_RETURN_NONE;
}

/* ---- Rows, columns and the shape of the sheet ---------------------- */

/* insert_rows(i, at, count), and its three siblings. */
static PyObject *
shift_band (PyObject *args, gboolean rows, gboolean insert)
{
  int index, at, count = 1;
  O42Sheet *sheet;
  int limit = rows ? O42_MAX_ROWS : O42_MAX_COLS;
  if (!PyArg_ParseTuple (args, "ii|i", &index, &at, &count) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  if (at < 0 || at >= limit || count < 0)
    return PyErr_Format (PyExc_IndexError, "%s %d is off the sheet", rows ? "row" : "column", at);
  if (rows)
    {
      if (insert) o42_sheet_insert_rows (sheet, at, count);
      else        o42_sheet_delete_rows (sheet, at, count);
    }
  else
    {
      if (insert) o42_sheet_insert_cols (sheet, at, count);
      else        o42_sheet_delete_cols (sheet, at, count);
    }
  book_touched = TRUE;
  Py_RETURN_NONE;
}

static PyObject *m_insert_rows (PyObject *self, PyObject *args) { (void) self; return shift_band (args, TRUE, TRUE); }
static PyObject *m_delete_rows (PyObject *self, PyObject *args) { (void) self; return shift_band (args, TRUE, FALSE); }
static PyObject *m_insert_cols (PyObject *self, PyObject *args) { (void) self; return shift_band (args, FALSE, TRUE); }
static PyObject *m_delete_cols (PyObject *self, PyObject *args) { (void) self; return shift_band (args, FALSE, FALSE); }

/* A range's corners, checked and put the right way round. */
static gboolean
range_ok (O42Range *r)
{
  if (!cell_ok (r->row0, r->col0) || !cell_ok (r->row1, r->col1))
    return FALSE;
  *r = o42_range_normalise (r->row0, r->col0, r->row1, r->col1);
  return TRUE;
}

/* The (i, row0, col0, row1, col1) most range calls begin with. */
#define RANGE_ARGS(args, index, r) \
  (PyArg_ParseTuple (args, "iiiii", &index, &(r).row0, &(r).col0, &(r).row1, &(r).col1) && range_ok (&r))

static PyObject *
m_shift_cells (PyObject *self, PyObject *args)
{
  int index, down, insert;
  O42Range r;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "iiiiipp", &index, &r.row0, &r.col0, &r.row1, &r.col1, &down, &insert) ||
      !range_ok (&r) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  o42_sheet_shift_cells (sheet, &r, down, insert);
  book_touched = TRUE;
  Py_RETURN_NONE;
}

static PyObject *
m_merge (PyObject *self, PyObject *args)
{
  int index;
  O42Range r;
  O42Sheet *sheet;
  (void) self;
  if (!RANGE_ARGS (args, index, r) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  o42_sheet_merge (sheet, &r);
  book_touched = TRUE;
  Py_RETURN_NONE;
}

static PyObject *
m_unmerge (PyObject *self, PyObject *args)
{
  int index;
  O42Range r;
  O42Sheet *sheet;
  (void) self;
  if (!RANGE_ARGS (args, index, r) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  o42_sheet_unmerge (sheet, &r);
  book_touched = TRUE;
  Py_RETURN_NONE;
}

static PyObject *
m_merged_at (PyObject *self, PyObject *args)
{
  int index, row, col;
  O42Sheet *sheet;
  O42Range r;
  (void) self;
  if (!PyArg_ParseTuple (args, "iii", &index, &row, &col) || (sheet = sheet_arg (index)) == NULL || !cell_ok (row, col))
    return NULL;
  if (!o42_sheet_merged_at (sheet, row, col, &r))
    Py_RETURN_NONE;
  return Py_BuildValue ("(iiii)", r.row0, r.col0, r.row1, r.col1);
}

/* row_height(i, row) and row_height(i, row, height): pixels at 96 dpi. */
static PyObject *
line_size (PyObject *args, gboolean rows)
{
  int index, at, size = -1;
  O42Sheet *sheet;
  if (!PyArg_ParseTuple (args, "ii|i", &index, &at, &size) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  if (at < 0 || at >= (rows ? O42_MAX_ROWS : O42_MAX_COLS))
    return PyErr_Format (PyExc_IndexError, "%s %d is off the sheet", rows ? "row" : "column", at);
  if (size >= 0)
    {
      if (rows) o42_sheet_set_row_height (sheet, at, size);
      else      o42_sheet_set_col_width (sheet, at, size);
      book_touched = TRUE;
    }
  return PyLong_FromLong (rows ? o42_sheet_row_height (sheet, at) : o42_sheet_col_width (sheet, at));
}

static PyObject *m_row_height (PyObject *self, PyObject *args) { (void) self; return line_size (args, TRUE); }
static PyObject *m_col_width  (PyObject *self, PyObject *args) { (void) self; return line_size (args, FALSE); }

/* set_hidden(i, rows, first, last, hidden) and hidden(i, rows, at). */
static PyObject *
m_set_hidden (PyObject *self, PyObject *args)
{
  int index, rows, first, last, hidden;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "ipiip", &index, &rows, &first, &last, &hidden) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  if (first < 0 || last >= (rows ? O42_MAX_ROWS : O42_MAX_COLS) || last < first)
    return PyErr_Format (PyExc_IndexError, "%s %d to %d are not on the sheet", rows ? "rows" : "columns", first, last);
  o42_sheet_begin_group (sheet);
  for (int at = first; at <= last; at++)
    {
      if (rows) o42_sheet_set_row_hidden (sheet, at, hidden);
      else      o42_sheet_set_col_hidden (sheet, at, hidden);
    }
  o42_sheet_end_group (sheet);
  book_touched = TRUE;
  Py_RETURN_NONE;
}

static PyObject *
m_hidden (PyObject *self, PyObject *args)
{
  int index, rows, at;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "ipi", &index, &rows, &at) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  if (at < 0 || at >= (rows ? O42_MAX_ROWS : O42_MAX_COLS))
    return PyErr_Format (PyExc_IndexError, "%s %d is off the sheet", rows ? "row" : "column", at);
  return PyBool_FromLong (rows ? o42_sheet_row_hidden (sheet, at) : o42_sheet_col_hidden (sheet, at));
}

/* frozen(i) -> (rows, cols); frozen(i, rows, cols) sets them. */
static PyObject *
m_frozen (PyObject *self, PyObject *args)
{
  int index, rows = -1, cols = -1;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "i|ii", &index, &rows, &cols) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  if (rows >= 0 && cols >= 0)
    {
      o42_sheet_set_frozen (sheet, rows, cols);
      book_touched = TRUE;
    }
  o42_sheet_get_frozen (sheet, &rows, &cols);
  return Py_BuildValue ("(ii)", rows, cols);
}

/* ---- Operations on a range ----------------------------------------- */

static PyObject *
m_clear_range (PyObject *self, PyObject *args)
{
  int index, formats;
  O42Range r;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "iiiiip", &index, &r.row0, &r.col0, &r.row1, &r.col1, &formats) ||
      !range_ok (&r) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  if (formats) o42_sheet_clear_formats (sheet, &r);
  else         o42_sheet_clear_range (sheet, &r);
  book_touched = TRUE;
  Py_RETURN_NONE;
}

static const char *PASTE_MODES[] = { "all", "values", "formats", "formulas" };

/* copy_range(i, r0, c0, r1, c1, row, col, mode, transpose) */
static PyObject *
m_copy_range (PyObject *self, PyObject *args)
{
  int index, row, col, transpose, mode;
  const char *mode_text;
  O42Range r;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "iiiiiiisp", &index, &r.row0, &r.col0, &r.row1, &r.col1, &row, &col, &mode_text, &transpose) ||
      !range_ok (&r) || (sheet = sheet_arg (index)) == NULL || !cell_ok (row, col))
    return NULL;
  mode = -1;
  for (guint i = 0; i < G_N_ELEMENTS (PASTE_MODES); i++)
    if (g_ascii_strcasecmp (mode_text, PASTE_MODES[i]) == 0)
      mode = (int) i;
  if (mode < 0)
    return PyErr_Format (PyExc_ValueError, "paste mode is all, values, formats or formulas, not %s", mode_text);
  o42_sheet_copy_range_special (sheet, &r, row, col, (O42PasteMode) mode, transpose);
  book_touched = TRUE;
  Py_RETURN_NONE;
}

static PyObject *
m_move_range (PyObject *self, PyObject *args)
{
  int index, row, col;
  O42Range r;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "iiiiiii", &index, &r.row0, &r.col0, &r.row1, &r.col1, &row, &col) ||
      !range_ok (&r) || (sheet = sheet_arg (index)) == NULL || !cell_ok (row, col))
    return NULL;
  o42_sheet_move_range (sheet, &r, row, col);
  book_touched = TRUE;
  Py_RETURN_NONE;
}

/* fill(i, r0, c0, r1, c1, down) and autofill(i, r0, c0, r1, c1, t0, u0, t1, u1) */
static PyObject *
m_fill (PyObject *self, PyObject *args)
{
  int index, down;
  O42Range r;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "iiiiip", &index, &r.row0, &r.col0, &r.row1, &r.col1, &down) ||
      !range_ok (&r) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  o42_sheet_fill (sheet, &r, down);
  book_touched = TRUE;
  Py_RETURN_NONE;
}

static PyObject *
m_autofill (PyObject *self, PyObject *args)
{
  int index;
  O42Range r, t;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "iiiiiiiii", &index, &r.row0, &r.col0, &r.row1, &r.col1, &t.row0, &t.col0, &t.row1, &t.col1) ||
      !range_ok (&r) || !range_ok (&t) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  o42_sheet_autofill (sheet, &r, &t);
  book_touched = TRUE;
  Py_RETURN_NONE;
}

/* A list of ints from Python, at most `max` of them. */
static int
int_list (PyObject *o, int *out, int max, const char *what)
{
  Py_ssize_t n;
  if (PyLong_Check (o))
    {
      out[0] = (int) PyLong_AsLong (o);
      return PyErr_Occurred () ? -1 : 1;
    }
  if (!PySequence_Check (o) || PyUnicode_Check (o))
    {
      PyErr_Format (PyExc_TypeError, "%s must be an int or a list of ints", what);
      return -1;
    }
  n = PySequence_Size (o);
  if (n > max)
    {
      PyErr_Format (PyExc_ValueError, "at most %d %s", max, what);
      return -1;
    }
  for (Py_ssize_t i = 0; i < n; i++)
    {
      PyObject *item = PySequence_GetItem (o, i);
      out[i] = item != NULL ? (int) PyLong_AsLong (item) : 0;
      Py_XDECREF (item);
      if (PyErr_Occurred ())
        return -1;
    }
  return (int) n;
}

/* sort(i, r0, c0, r1, c1, keys, ascending, header): keys are columns
 * counted from the range's left. */
static PyObject *
m_sort (PyObject *self, PyObject *args)
{
  int index, header;
  O42Range r;
  O42Sheet *sheet;
  PyObject *keys_o, *asc_o;
  int keys[3], n_keys, n_asc;
  int asc_i[3] = { 1, 1, 1 };
  gboolean asc[3];
  (void) self;
  if (!PyArg_ParseTuple (args, "iiiiiOOp", &index, &r.row0, &r.col0, &r.row1, &r.col1, &keys_o, &asc_o, &header) ||
      !range_ok (&r) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  n_keys = int_list (keys_o, keys, 3, "sort keys");
  if (n_keys < 0)
    return NULL;
  if (PyBool_Check (asc_o))
    {
      n_asc = 1;
      asc_i[0] = asc_o == Py_True;
    }
  else if ((n_asc = int_list (asc_o, asc_i, 3, "directions")) < 0)
    return NULL;
  for (int k = 0; k < n_keys; k++)
    {
      if (keys[k] < 0 || r.col0 + keys[k] > r.col1)
        return PyErr_Format (PyExc_IndexError, "sort key %d is outside the range", keys[k]);
      keys[k] += r.col0;
      asc[k] = k < n_asc ? asc_i[k] != 0 : (n_asc > 0 ? asc_i[n_asc - 1] != 0 : TRUE);
    }
  o42_sheet_sort_keys (sheet, &r, keys, asc, n_keys, header);
  book_touched = TRUE;
  Py_RETURN_NONE;
}

/* replace(i, r0, c0, r1, c1, needle, replacement, match_case) -> count;
 * r0 < 0 means the whole sheet. */
static PyObject *
m_replace (PyObject *self, PyObject *args)
{
  int index, match_case, count;
  O42Range r;
  O42Sheet *sheet;
  const char *needle, *replacement;
  (void) self;
  if (!PyArg_ParseTuple (args, "iiiiissp", &index, &r.row0, &r.col0, &r.row1, &r.col1, &needle, &replacement, &match_case) ||
      (sheet = sheet_arg (index)) == NULL)
    return NULL;
  if (r.row0 >= 0 && !range_ok (&r))
    return NULL;
  if (*needle == '\0')
    return PyErr_Format (PyExc_ValueError, "nothing to look for");
  count = o42_sheet_replace (sheet, r.row0 >= 0 ? &r : NULL, needle, replacement, match_case);
  if (count > 0)
    book_touched = TRUE;
  return PyLong_FromLong (count);
}

/* find(i, needle, match_case, whole_cell, row, col) -> (row, col) or None,
 * searching on from just after row, col. */
static PyObject *
m_find (PyObject *self, PyObject *args)
{
  int index, match_case, whole_cell, row, col;
  O42Sheet *sheet;
  const char *needle;
  (void) self;
  if (!PyArg_ParseTuple (args, "isppii", &index, &needle, &match_case, &whole_cell, &row, &col) ||
      (sheet = sheet_arg (index)) == NULL)
    return NULL;
  if (!o42_sheet_find (sheet, needle, match_case, whole_cell, &row, &col))
    Py_RETURN_NONE;
  return Py_BuildValue ("(ii)", row, col);
}

/* remove_duplicates(i, r0, c0, r1, c1, cols, header) -> how many rows went;
 * cols counted from the range's left. */
static PyObject *
m_remove_duplicates (PyObject *self, PyObject *args)
{
  int index, header, n_cols, removed;
  O42Range r;
  O42Sheet *sheet;
  PyObject *cols_o;
  int cols[256];
  (void) self;
  if (!PyArg_ParseTuple (args, "iiiiiOp", &index, &r.row0, &r.col0, &r.row1, &r.col1, &cols_o, &header) ||
      !range_ok (&r) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  if (cols_o == Py_None)
    {
      n_cols = MIN (r.col1 - r.col0 + 1, 256);
      for (int k = 0; k < n_cols; k++)
        cols[k] = k;
    }
  else if ((n_cols = int_list (cols_o, cols, 256, "columns")) < 0)
    return NULL;
  for (int k = 0; k < n_cols; k++)
    {
      if (cols[k] < 0 || r.col0 + cols[k] > r.col1)
        return PyErr_Format (PyExc_IndexError, "column %d is outside the range", cols[k]);
      cols[k] += r.col0;
    }
  if (n_cols == 0)
    return PyLong_FromLong (0);
  removed = o42_sheet_remove_duplicates (sheet, &r, cols, n_cols, header);
  if (removed > 0)
    book_touched = TRUE;
  return PyLong_FromLong (removed);
}

static PyObject *
m_set_autofilter (PyObject *self, PyObject *args)
{
  int index;
  O42Range r;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "iiiii", &index, &r.row0, &r.col0, &r.row1, &r.col1) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  if (r.row0 < 0)
    o42_sheet_clear_autofilter (sheet);
  else if (range_ok (&r))
    o42_sheet_set_autofilter (sheet, &r);
  else
    return NULL;
  book_touched = TRUE;
  Py_RETURN_NONE;
}

static PyObject *
m_get_autofilter (PyObject *self, PyObject *args)
{
  int index;
  O42Range r;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "i", &index) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  if (!o42_sheet_get_autofilter (sheet, &r))
    Py_RETURN_NONE;
  return Py_BuildValue ("(iiii)", r.row0, r.col0, r.row1, r.col1);
}

/* autofilter_choose(i, col, value or None); with one argument, the choice. */
static PyObject *
m_autofilter_choose (PyObject *self, PyObject *args)
{
  int index, col;
  PyObject *value = NULL;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "ii|O", &index, &col, &value) || (sheet = sheet_arg (index)) == NULL)
    return NULL;
  if (value == NULL)
    {
      const char *choice = o42_sheet_autofilter_choice (sheet, col);
      if (choice == NULL)
        Py_RETURN_NONE;
      return PyUnicode_FromString (choice);
    }
  if (value == Py_None)
    o42_sheet_autofilter_choose (sheet, col, NULL);
  else
    {
      PyObject *text = PyObject_Str (value);
      const char *s = text != NULL ? PyUnicode_AsUTF8 (text) : NULL;
      if (s == NULL)
        { Py_XDECREF (text); return NULL; }
      o42_sheet_autofilter_choose (sheet, col, s);
      Py_DECREF (text);
    }
  book_touched = TRUE;
  Py_RETURN_NONE;
}

/* ---- The window: selection, message boxes, files ------------------- */

static PyObject *
no_window (const char *what)
{
  return PyErr_Format (PyExc_RuntimeError, "no window to %s: the script is not running in one", what);
}

/* selection() -> (sheet index, row0, col0, row1, col1, active row, active col) */
static PyObject *
m_selection (PyObject *self, PyObject *args)
{
  O42Sheet *sheet = NULL;
  O42Range r;
  int arow = 0, acol = 0;
  (void) self; (void) args;
  if (host.get_selection == NULL || current_book == NULL)
    return no_window ("ask the selection of");
  if (!host.get_selection (host.user, current_book, &sheet, &r, &arow, &acol) || sheet == NULL)
    return no_window ("ask the selection of");
  return Py_BuildValue ("(iiiiiii)", o42_book_sheet_index (current_book, sheet),
                        r.row0, r.col0, r.row1, r.col1, arow, acol);
}

/* select(i, row0, col0, row1, col1, active row, active col) */
static PyObject *
m_select (PyObject *self, PyObject *args)
{
  int index, arow, acol;
  O42Range r;
  O42Sheet *sheet;
  (void) self;
  if (!PyArg_ParseTuple (args, "iiiiiii", &index, &r.row0, &r.col0, &r.row1, &r.col1, &arow, &acol) ||
      !range_ok (&r) || (sheet = sheet_arg (index)) == NULL || !cell_ok (arow, acol))
    return NULL;
  if (host.set_selection == NULL)
    return no_window ("select in");
  host.set_selection (host.user, current_book, sheet, &r, arow, acol);
  Py_RETURN_NONE;
}

static PyObject *
m_message (PyObject *self, PyObject *args)
{
  const char *text;
  (void) self;
  if (!PyArg_ParseTuple (args, "s", &text))
    return NULL;
  if (host.message == NULL)
    return no_window ("show a message in");
  host.message (host.user, current_book, text);
  Py_RETURN_NONE;
}

static PyObject *
m_input (PyObject *self, PyObject *args)
{
  const char *prompt, *initial = "";
  char *answer;
  PyObject *result;
  (void) self;
  if (!PyArg_ParseTuple (args, "s|s", &prompt, &initial))
    return NULL;
  if (host.input == NULL)
    return no_window ("ask a question in");
  answer = host.input (host.user, current_book, prompt, initial);
  if (answer == NULL)
    Py_RETURN_NONE;
  result = PyUnicode_FromString (answer);
  g_free (answer);
  return result;
}

static PyObject *
m_status (PyObject *self, PyObject *args)
{
  const char *text;
  (void) self;
  if (!PyArg_ParseTuple (args, "s", &text))
    return NULL;
  if (host.status != NULL)
    host.status (host.user, current_book, text);
  Py_RETURN_NONE;
}

static PyObject *
m_path (PyObject *self, PyObject *args)
{
  char *path;
  PyObject *result;
  (void) self; (void) args;
  if (host.path == NULL || current_book == NULL)
    Py_RETURN_NONE;
  path = host.path (host.user, current_book);
  if (path == NULL)
    Py_RETURN_NONE;
  result = PyUnicode_FromString (path);
  g_free (path);
  return result;
}

static PyObject *
m_save (PyObject *self, PyObject *args)
{
  const char *path = NULL;
  char *message = NULL;
  (void) self;
  if (!PyArg_ParseTuple (args, "|z", &path))
    return NULL;
  if (host.save == NULL || current_book == NULL)
    return no_window ("save from");
  if (!host.save (host.user, current_book, path, &message))
    {
      PyErr_SetString (PyExc_OSError, message != NULL ? message : "the book could not be saved");
      g_free (message);
      return NULL;
    }
  Py_RETURN_NONE;
}

static PyObject *
m_open (PyObject *self, PyObject *args)
{
  const char *path;
  char *message = NULL;
  (void) self;
  if (!PyArg_ParseTuple (args, "s", &path))
    return NULL;
  if (host.open == NULL)
    return no_window ("open a file beside");
  if (!host.open (host.user, path, &message))
    {
      PyErr_SetString (PyExc_OSError, message != NULL ? message : "the file could not be opened");
      g_free (message);
      return NULL;
    }
  Py_RETURN_NONE;
}

static PyObject *
m_personal_folder (PyObject *self, PyObject *args)
{
  char *folder = o42_python_personal_folder ();
  PyObject *result = PyUnicode_FromString (folder);
  (void) self; (void) args;
  g_free (folder);
  return result;
}

static PyObject *
m_close (PyObject *self, PyObject *args)
{
  (void) self; (void) args;
  if (host.close == NULL || current_book == NULL)
    return no_window ("close");
  host.close (host.user, current_book);
  Py_RETURN_NONE;
}

static PyObject *
m_calculate (PyObject *self, PyObject *args)
{
  (void) self; (void) args;
  if (current_book != NULL)
    {
      for (int i = 0; i < o42_book_n_sheets (current_book); i++)
        o42_sheet_recalculate (o42_book_sheet (current_book, i));
      book_touched = TRUE;
    }
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

/* relocate_formula(text, drow, dcol) */
static PyObject *
m_relocate_formula (PyObject *self, PyObject *args)
{
  const char *text;
  int drow, dcol;
  char *moved;
  PyObject *result;
  (void) self;
  if (!PyArg_ParseTuple (args, "sii", &text, &drow, &dcol))
    return NULL;
  moved = o42_sheet_relocate_formula (text, drow, dcol);
  result = PyUnicode_FromString (moved);
  g_free (moved);
  return result;
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
                                 "scientific", "text", "date", "time", "datetime",
                                 "accounting" };

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
      else if (strcmp (k, "shrink") == 0)    { fmt.shrink = PyObject_IsTrue (value); mask |= O42_FMT_WRAP; }
      else if (strcmp (k, "borders") == 0)
        {
          /* True, False, or a style name for all four sides. */
          O42BorderStyle style = O42_BORDER_NONE;
          if (PyUnicode_Check (value))
            {
              const char *s = PyUnicode_AsUTF8 (value);
              if (s == NULL || !o42_border_style_parse (s, &style))
                return PyErr_Format (PyExc_ValueError, "borders is True, False or a style: none, thin, medium, thick, double, dashed or dotted");
            }
          else if (PyObject_IsTrue (value))
            style = O42_BORDER_THIN;
          for (int i = 0; i < 4; i++)
            fmt.border_style[i] = style;
          o42_fmt_sync_borders (&fmt);
          mask |= O42_FMT_BORDERS;
        }
      else if (g_str_has_prefix (k, "border_") &&
               (g_str_has_prefix (k + 7, "top") || g_str_has_prefix (k + 7, "bottom") ||
                g_str_has_prefix (k + 7, "left") || g_str_has_prefix (k + 7, "right")))
        {
          /* One side: border_top="thin" (or None), border_top_colour="#rrggbb".
           * The other sides are kept as they are. */
          static const char *const SIDES[] = { "top", "bottom", "left", "right" };
          int side = -1;
          gboolean colour = g_str_has_suffix (k, "_colour") || g_str_has_suffix (k, "_color");
          for (int i = 0; i < 4; i++)
            if (g_str_has_prefix (k + 7, SIDES[i]) &&
                (k[7 + strlen (SIDES[i])] == '\0' || k[7 + strlen (SIDES[i])] == '_'))
              side = i;
          if (side < 0)
            return PyErr_Format (PyExc_TypeError, "no format property named %s", k);
          if (!(mask & O42_FMT_BORDERS))
            {
              const O42Fmt *have = o42_sheet_get_fmt (sheet, r.row0, r.col0);
              for (int i = 0; i < 4; i++)
                {
                  fmt.border_style[i] = have->border_style[i];
                  fmt.border_colour[i] = have->border_colour[i];
                }
            }
          if (colour)
            {
              if (!colour_arg (value, &fmt.border_colour[side])) return NULL;
            }
          else if (value == Py_None || value == Py_False)
            fmt.border_style[side] = O42_BORDER_NONE;
          else if (value == Py_True)
            fmt.border_style[side] = O42_BORDER_THIN;
          else
            {
              const char *s = PyUnicode_Check (value) ? PyUnicode_AsUTF8 (value) : NULL;
              O42BorderStyle style;
              if (s == NULL || !o42_border_style_parse (s, &style))
                return PyErr_Format (PyExc_ValueError, "%s is a style: none, thin, medium, thick, double, dashed or dotted", k);
              fmt.border_style[side] = style;
            }
          o42_fmt_sync_borders (&fmt);
          mask |= O42_FMT_BORDERS;
        }
      else if (strcmp (k, "pattern") == 0)
        {
          O42Pattern pattern = O42_PATTERN_NONE;
          if (value != Py_None)
            {
              const char *s = PyUnicode_Check (value) ? PyUnicode_AsUTF8 (value) : NULL;
              if (s == NULL || !o42_pattern_parse (s, &pattern))
                return PyErr_Format (PyExc_ValueError, "unknown pattern %S", value);
            }
          if (!(mask & O42_FMT_PATTERN))
            fmt.pattern_colour = o42_sheet_get_fmt (sheet, r.row0, r.col0)->pattern_colour;
          fmt.pattern = (guint8) pattern;
          mask |= O42_FMT_PATTERN;
        }
      else if (strcmp (k, "pattern_colour") == 0 || strcmp (k, "pattern_color") == 0)
        {
          if (!(mask & O42_FMT_PATTERN))
            fmt.pattern = o42_sheet_get_fmt (sheet, r.row0, r.col0)->pattern;
          if (!colour_arg (value, &fmt.pattern_colour)) return NULL;
          mask |= O42_FMT_PATTERN;
        }
      else if (strcmp (k, "locked") == 0 || strcmp (k, "hidden") == 0)
        {
          if (!(mask & O42_FMT_PROTECTION))
            {
              const O42Fmt *have = o42_sheet_get_fmt (sheet, r.row0, r.col0);
              fmt.locked = have->locked;
              fmt.hidden = have->hidden;
            }
          if (k[0] == 'l') fmt.locked = PyObject_IsTrue (value);
          else             fmt.hidden = PyObject_IsTrue (value);
          mask |= O42_FMT_PROTECTION;
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
  return Py_BuildValue ("{s:s,s:d,s:O,s:O,s:O,s:O,s:O,s:O,s:O,s:I,s:N,s:s,s:s,s:s,s:i,"
                        "s:s,s:s,s:s,s:s,s:I,s:I,s:I,s:I,s:s,s:I,s:O,s:O}",
                        "family", f->family, "size", f->size / 2.0,
                        "bold", f->bold ? Py_True : Py_False,
                        "italic", f->italic ? Py_True : Py_False,
                        "underline", f->underline ? Py_True : Py_False,
                        "strikeout", f->strikeout ? Py_True : Py_False,
                        "wrap", f->wrap ? Py_True : Py_False,
                        "shrink", f->shrink ? Py_True : Py_False,
                        "borders", (f->border_top && f->border_bottom && f->border_left && f->border_right) ? Py_True : Py_False,
                        "colour", (unsigned int) f->colour,
                        "fill", fill,
                        "halign", HALIGNS[f->halign], "valign", VALIGNS[f->valign],
                        "number", f->custom != NULL ? f->custom : NUMBERS[f->number],
                        "decimals", f->decimals,
                        "border_top", f->border_top ? o42_border_style_name (f->border_style[O42_SIDE_TOP]) : "none",
                        "border_bottom", f->border_bottom ? o42_border_style_name (f->border_style[O42_SIDE_BOTTOM]) : "none",
                        "border_left", f->border_left ? o42_border_style_name (f->border_style[O42_SIDE_LEFT]) : "none",
                        "border_right", f->border_right ? o42_border_style_name (f->border_style[O42_SIDE_RIGHT]) : "none",
                        "border_top_colour", (unsigned int) f->border_colour[O42_SIDE_TOP],
                        "border_bottom_colour", (unsigned int) f->border_colour[O42_SIDE_BOTTOM],
                        "border_left_colour", (unsigned int) f->border_colour[O42_SIDE_LEFT],
                        "border_right_colour", (unsigned int) f->border_colour[O42_SIDE_RIGHT],
                        "pattern", o42_pattern_name ((O42Pattern) f->pattern),
                        "pattern_colour", (unsigned int) f->pattern_colour,
                        "locked", f->locked ? Py_True : Py_False,
                        "hidden", f->hidden ? Py_True : Py_False);
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
  /* A formula evaluated on a sheet -- the current one unless another
   * is named -- as a cell there would. */
  const char *text;
  int index = -1;
  O42Sheet *sheet;
  O42Value v;
  PyObject *result;
  (void) self;
  if (!PyArg_ParseTuple (args, "s|i", &text, &index))
    return NULL;
  sheet = index >= 0 ? sheet_arg (index) : current_sheet;
  if (sheet == NULL)
    {
      if (index >= 0)
        return NULL;
      Py_RETURN_NONE;
    }
  v = o42_sheet_evaluate_formula (sheet, text);
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

/* script_options(name, shortcut letter or "", description) */
static PyObject *
m_script_options (PyObject *self, PyObject *args)
{
  const char *name, *key, *about;
  (void) self;
  if (!PyArg_ParseTuple (args, "sss", &name, &key, &about))
    return NULL;
  if (current_book != NULL)
    o42_book_set_script_options (current_book, name, key[0], about);
  Py_RETURN_NONE;
}

/* script_info(name) -> (shortcut letter or "", description) */
static PyObject *
m_script_info (PyObject *self, PyObject *args)
{
  const char *name;
  char key[2] = { 0, 0 };
  (void) self;
  if (!PyArg_ParseTuple (args, "s", &name))
    return NULL;
  if (current_book == NULL || o42_book_script_code (current_book, name) == NULL)
    return PyErr_Format (PyExc_KeyError, "no script named %s", name);
  key[0] = o42_book_script_shortcut (current_book, name);
  return Py_BuildValue ("(ss)", key, o42_book_script_description (current_book, name));
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
  { "script_options", m_script_options, METH_VARARGS, "Sets a script's shortcut letter and description." },
  { "script_info",    m_script_info,    METH_VARARGS, "A script's (shortcut letter, description)." },
  { "book_id",        m_book_id,        METH_NOARGS,  "A number for the book the script runs against." },
  { "n_sheets",       m_n_sheets,       METH_NOARGS,  "How many sheets the book has." },
  { "sheet_name",     m_sheet_name,     METH_VARARGS, "The name of sheet i." },
  { "sheet_index",    m_sheet_index,    METH_VARARGS, "The index of the sheet named so, or -1." },
  { "current",        m_current,        METH_NOARGS,  "The index of the sheet on show." },
  { "add_sheet",      m_add_sheet,      METH_VARARGS, "Adds a sheet; its index." },
  { "remove_sheet",   m_remove_sheet,   METH_VARARGS, "Removes sheet i." },
  { "rename_sheet",   m_rename_sheet,   METH_VARARGS, "Renames sheet i." },
  { "move_sheet",     m_move_sheet,     METH_VARARGS, "Moves sheet i to place j." },
  { "insert_rows",    m_insert_rows,    METH_VARARGS, "Inserts count rows before row at." },
  { "delete_rows",    m_delete_rows,    METH_VARARGS, "Deletes count rows from row at." },
  { "insert_cols",    m_insert_cols,    METH_VARARGS, "Inserts count columns before column at." },
  { "delete_cols",    m_delete_cols,    METH_VARARGS, "Deletes count columns from column at." },
  { "shift_cells",    m_shift_cells,    METH_VARARGS, "Inserts or deletes a range's cells, shifting the rest." },
  { "merge",          m_merge,          METH_VARARGS, "Merges a range into one cell." },
  { "unmerge",        m_unmerge,        METH_VARARGS, "Takes the merges touching a range apart." },
  { "merged_at",      m_merged_at,      METH_VARARGS, "The merged range a cell is in, or None." },
  { "row_height",     m_row_height,     METH_VARARGS, "A row's height in pixels; sets it with a third argument." },
  { "col_width",      m_col_width,      METH_VARARGS, "A column's width in pixels; sets it with a third argument." },
  { "set_hidden",     m_set_hidden,     METH_VARARGS, "Hides or shows rows (or columns) first..last." },
  { "hidden",         m_hidden,         METH_VARARGS, "Whether a row (or column) is hidden." },
  { "frozen",         m_frozen,         METH_VARARGS, "The frozen (rows, cols); sets them with two more arguments." },
  { "clear_range",    m_clear_range,    METH_VARARGS, "Empties a range's cells, or their formats." },
  { "copy_range",     m_copy_range,     METH_VARARGS, "Copies a range to a cell: all, values, formats or formulas." },
  { "move_range",     m_move_range,     METH_VARARGS, "Moves a range to a cell, formulas following." },
  { "fill",           m_fill,           METH_VARARGS, "Fill Down (or Right) over a range." },
  { "autofill",       m_autofill,       METH_VARARGS, "Continues a range's series over a target." },
  { "sort",           m_sort,           METH_VARARGS, "Sorts a range's rows by keys." },
  { "replace",        m_replace,        METH_VARARGS, "Replaces text in a range (or the sheet); how many cells." },
  { "find",           m_find,           METH_VARARGS, "The next cell holding a text, or None." },
  { "remove_duplicates", m_remove_duplicates, METH_VARARGS, "Removes a range's duplicate rows; how many." },
  { "set_autofilter", m_set_autofilter, METH_VARARGS, "Puts an AutoFilter on a range, or takes it off." },
  { "get_autofilter", m_get_autofilter, METH_VARARGS, "The AutoFilter's range, or None." },
  { "autofilter_choose", m_autofilter_choose, METH_VARARGS, "A filter column's choice; sets it with a value or None." },
  { "selection",      m_selection,      METH_NOARGS,  "The window's selection: (sheet, r0, c0, r1, c1, row, col)." },
  { "select",         m_select,         METH_VARARGS, "Selects a range in the window and makes a cell active." },
  { "message",        m_message,        METH_VARARGS, "A message box, waited for." },
  { "input",          m_input,          METH_VARARGS, "Asks the user for a line; None if cancelled." },
  { "status",         m_status,         METH_VARARGS, "Sets the status bar's text." },
  { "path",           m_path,           METH_NOARGS,  "The book's file, or None." },
  { "save",           m_save,           METH_VARARGS, "Saves the book, to a path if given." },
  { "open",           m_open,           METH_VARARGS, "Opens a file in a window of its own." },
  { "close",          m_close,          METH_NOARGS,  "Closes the window showing the book." },
  { "calculate",      m_calculate,      METH_NOARGS,  "Recalculates every sheet." },
  { "personal_folder", m_personal_folder, METH_NOARGS, "The personal scripts folder." },
  { "get_input",      m_get_input,      METH_VARARGS, "What was typed into a cell." },
  { "set_input",      m_set_input,      METH_VARARGS, "Types into a cell." },
  { "get_value",      m_get_value,      METH_VARARGS, "A cell's value." },
  { "relocate_formula", m_relocate_formula, METH_VARARGS, "A formula's text moved by rows and columns." },
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

  /* The personal scripts, with no book on show: what they define is
   * everyone's. */
  {
    char **paths = o42_python_personal_scripts ();
    PyObject *list = PyList_New (0);
    PyObject *r;

    for (int i = 0; paths[i] != NULL; i++)
      {
        PyObject *item = PyUnicode_FromString (paths[i]);
        PyList_Append (list, item);
        Py_DECREF (item);
      }
    r = PyObject_CallMethod (module, "_load_personal", "O", list);
    Py_XDECREF (r);
    Py_DECREF (list);
    PyErr_Clear ();
    g_strfreev (paths);
  }
  return TRUE;
}

gboolean
o42_python_available (void)
{
  return TRUE;
}

gboolean
o42_python_start (void)
{
  char **paths = o42_python_personal_scripts ();
  gboolean any = paths[0] != NULL;

  g_strfreev (paths);
  if (!any && module == NULL)
    return FALSE;
  return ensure_interpreter ();
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
  r = PyObject_CallMethod (module, "_reset", "n", (Py_ssize_t) (guintptr) current_book);
  Py_XDECREF (r);
  PyErr_Clear ();
}

void
o42_python_forget_book (O42Book *book)
{
  PyObject *r;
  if (module == NULL || book == NULL)
    return;
  r = PyObject_CallMethod (module, "_forget_book", "n", (Py_ssize_t) (guintptr) book);
  Py_XDECREF (r);
  PyErr_Clear ();
}

#endif /* HAVE_PYTHON */
