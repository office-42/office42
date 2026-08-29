/* o42-book.c - see o42-book.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-book.h"

#include "o42-pyquote.h"
#include <string.h>

/* o42-types.h for the reference check in name_is_legal comes with
 * o42-sheet.h. */

typedef struct {
  O42Sheet *sheet;
  O42Range  range;
} NamedRange;

typedef struct {
  O42BookWatcher watcher;
  gpointer       user;
} Watcher;

struct _O42Book {
  GPtrArray *views;        /* O42BookView *, the named window states */
  GString *recording;      /* the macro being recorded, or NULL */
  char    *recorded_sheet; /* the sheet its last line was about */
  GPtrArray    *sheets;   /* O42Sheet*, owned, in tab order */
  O42UndoStack *stack;    /* shared by every sheet */
  GHashTable   *names;    /* upper-case name -> NamedRange*, owned */
  int           refs;
  GArray       *watchers; /* Watcher */
  GPtrArray    *scripts;  /* Script, in the order added */
  GArray       *styles;   /* Style, in the order defined */
  gboolean      scripts_modified;
  gboolean      iterative;    /* go round a circular reference rather than refuse */
  int           max_iterations;
  double        tolerance;
  gboolean      manual;       /* nothing is worked out until F9 */
  char         *db_path;      /* the database beside the book, or NULL */
  gboolean      db_embedded;  /* ...and whether it lives inside it */
};

typedef struct {
  char *name;
  char *code;
} Script;

typedef struct {
  char      *name;    /* owned */
  O42Fmt     fmt;
  O42FmtMask mask;
} Style;

static void
script_free (gpointer data)
{
  Script *s = data;
  g_free (s->name);
  g_free (s->code);
  g_free (s);
}

static Script *
find_script (O42Book *book, const char *name)
{
  for (guint i = 0; name != NULL && i < book->scripts->len; i++)
    {
      Script *s = g_ptr_array_index (book->scripts, i);
      if (strcmp (s->name, name) == 0)
        return s;
    }
  return NULL;
}

int
o42_book_n_scripts (O42Book *book)
{
  g_return_val_if_fail (book != NULL, 0);
  return (int) book->scripts->len;
}

const char *
o42_book_script_name (O42Book *book, int index)
{
  g_return_val_if_fail (book != NULL, NULL);
  if (index < 0 || (guint) index >= book->scripts->len)
    return NULL;
  return ((Script *) g_ptr_array_index (book->scripts, index))->name;
}

const char *
o42_book_script_code (O42Book *book, const char *name)
{
  Script *s;
  g_return_val_if_fail (book != NULL, NULL);
  s = find_script (book, name);
  return s != NULL ? s->code : NULL;
}

void
o42_book_set_script (O42Book *book, const char *name, const char *code)
{
  Script *s;
  g_return_if_fail (book != NULL && name != NULL && *name != '\0' && code != NULL);
  s = find_script (book, name);
  if (s == NULL)
    {
      s = g_new0 (Script, 1);
      s->name = g_strdup (name);
      g_ptr_array_add (book->scripts, s);
    }
  else if (strcmp (s->code, code) == 0)
    return;
  g_free (s->code);
  s->code = g_strdup (code);
  book->scripts_modified = TRUE;
  o42_book_changed (book, "scripts");
}

gboolean
o42_book_remove_script (O42Book *book, const char *name)
{
  Script *s;
  g_return_val_if_fail (book != NULL, FALSE);
  s = find_script (book, name);
  if (s == NULL)
    return FALSE;
  g_ptr_array_remove (book->scripts, s);
  book->scripts_modified = TRUE;
  o42_book_changed (book, "scripts");
  return TRUE;
}

/* The styles a new book starts with, as Excel's are named. */
static void
add_builtin_styles (O42Book *book)
{
  static const struct {
    const char *name;
    O42FmtMask  mask;
    guint       bold : 1;
    guint       italic : 1;
    int         size;        /* half-points, 0 to leave alone */
    guint32     colour;
    guint32     fill;
    O42NumberFormat number;
    int         decimals;
  } BUILTIN[] = {
    { "Normal",     O42_FMT_BOLD | O42_FMT_ITALIC | O42_FMT_SIZE | O42_FMT_COLOUR | O42_FMT_FILL | O42_FMT_NUMBER,
      0, 0, 20, 0x000000, O42_FILL_NONE, O42_NUM_GENERAL, 0 },
    { "Title",      O42_FMT_BOLD | O42_FMT_SIZE | O42_FMT_COLOUR, 1, 0, 36, 0x1F497D, O42_FILL_NONE, O42_NUM_GENERAL, 0 },
    { "Heading 1",  O42_FMT_BOLD | O42_FMT_SIZE | O42_FMT_COLOUR, 1, 0, 30, 0x1F497D, O42_FILL_NONE, O42_NUM_GENERAL, 0 },
    { "Heading 2",  O42_FMT_BOLD | O42_FMT_SIZE | O42_FMT_COLOUR, 1, 0, 26, 0x1F497D, O42_FILL_NONE, O42_NUM_GENERAL, 0 },
    { "Good",       O42_FMT_COLOUR | O42_FMT_FILL, 0, 0, 0, 0x006100, 0xC6EFCE, O42_NUM_GENERAL, 0 },
    { "Bad",        O42_FMT_COLOUR | O42_FMT_FILL, 0, 0, 0, 0x9C0006, 0xFFC7CE, O42_NUM_GENERAL, 0 },
    { "Neutral",    O42_FMT_COLOUR | O42_FMT_FILL, 0, 0, 0, 0x9C6500, 0xFFEB9C, O42_NUM_GENERAL, 0 },
    { "Note",       O42_FMT_FILL, 0, 0, 0, 0x000000, 0xFFFFCC, O42_NUM_GENERAL, 0 },
    { "Comma",      O42_FMT_NUMBER | O42_FMT_DECIMALS, 0, 0, 0, 0x000000, O42_FILL_NONE, O42_NUM_COMMA, 2 },
    { "Currency",   O42_FMT_NUMBER | O42_FMT_DECIMALS, 0, 0, 0, 0x000000, O42_FILL_NONE, O42_NUM_CURRENCY, 2 },
    { "Percent",    O42_FMT_NUMBER | O42_FMT_DECIMALS, 0, 0, 0, 0x000000, O42_FILL_NONE, O42_NUM_PERCENT, 0 },
  };

  for (guint i = 0; i < G_N_ELEMENTS (BUILTIN); i++)
    {
      O42Fmt fmt;
      Style fresh;

      o42_fmt_init_default (&fmt);
      fmt.bold = BUILTIN[i].bold;
      fmt.italic = BUILTIN[i].italic;
      if (BUILTIN[i].size > 0) fmt.size = BUILTIN[i].size;
      fmt.colour = BUILTIN[i].colour;
      fmt.fill = BUILTIN[i].fill;
      fmt.number = BUILTIN[i].number;
      fmt.decimals = BUILTIN[i].decimals;
      fresh.name = g_strdup (BUILTIN[i].name);
      fresh.fmt = fmt;
      fresh.mask = BUILTIN[i].mask;
      g_array_append_val (book->styles, fresh);
    }
}

O42Book *
o42_book_new (void)
{
  O42Book *book = g_new0 (O42Book, 1);

  book->sheets = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_sheet_free);
  book->stack = o42_undo_stack_new ();
  book->names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  book->refs = 1;
  book->watchers = g_array_new (FALSE, FALSE, sizeof (Watcher));
  book->scripts = g_ptr_array_new_with_free_func (script_free);
  book->styles = g_array_new (FALSE, FALSE, sizeof (Style));
  add_builtin_styles (book);
  o42_book_add_sheet (book, "Sheet1", -1);

  return book;
}

void
o42_book_free (O42Book *book)
{
  if (book == NULL)
    return;

  /* The history first: it may hold sheets taken out of the book, and
   * their snapshots look at the book while they are cleared. */
  o42_undo_stack_free (book->stack);
  g_ptr_array_free (book->sheets, TRUE);
  g_hash_table_destroy (book->names);
  g_array_free (book->watchers, TRUE);
  g_ptr_array_free (book->scripts, TRUE);
  if (book->views != NULL)
    g_ptr_array_unref (book->views);
  if (book->recording != NULL)
    g_string_free (book->recording, TRUE);
  g_free (book->recorded_sheet);
  g_free (book->db_path);
  for (guint i = 0; i < book->styles->len; i++)
    g_free (g_array_index (book->styles, Style, i).name);
  g_array_free (book->styles, TRUE);
  g_free (book);
}

O42Book *
o42_book_ref (O42Book *book)
{
  g_return_val_if_fail (book != NULL, NULL);
  book->refs++;
  return book;
}

void
o42_book_unref (O42Book *book)
{
  if (book == NULL)
    return;
  if (--book->refs <= 0)
    o42_book_free (book);
}

void
o42_book_watch (O42Book *book, O42BookWatcher watcher, gpointer user)
{
  Watcher w = { watcher, user };

  g_return_if_fail (book != NULL);
  g_array_append_val (book->watchers, w);
}

void
o42_book_unwatch (O42Book *book, O42BookWatcher watcher, gpointer user)
{
  g_return_if_fail (book != NULL);

  for (guint i = 0; i < book->watchers->len; i++)
    {
      const Watcher *w = &g_array_index (book->watchers, Watcher, i);
      if (w->watcher == watcher && w->user == user)
        {
          g_array_remove_index (book->watchers, i);
          return;
        }
    }
}

void
o42_book_changed (O42Book *book, const char *what)
{
  GArray *copy;

  g_return_if_fail (book != NULL);

  /* A watcher may unwatch while being told, so tell a copy of the list. */
  copy = g_array_copy (book->watchers);
  for (guint i = 0; i < copy->len; i++)
    {
      const Watcher *w = &g_array_index (copy, Watcher, i);
      w->watcher (book, what, w->user);
    }
  g_array_free (copy, TRUE);
}

int
o42_book_n_sheets (O42Book *book)
{
  g_return_val_if_fail (book != NULL, 0);
  return (int) book->sheets->len;
}

O42Sheet *
o42_book_sheet (O42Book *book, int index)
{
  g_return_val_if_fail (book != NULL, NULL);

  if (index < 0 || index >= (int) book->sheets->len)
    return NULL;
  return g_ptr_array_index (book->sheets, index);
}

/* Sheet names compare without regard to case, as Excel's do. */
O42Sheet *
o42_book_find_sheet (O42Book *book, const char *name)
{
  g_return_val_if_fail (book != NULL, NULL);

  if (name == NULL)
    return NULL;

  for (guint i = 0; i < book->sheets->len; i++)
    {
      O42Sheet *sheet = g_ptr_array_index (book->sheets, i);

      if (g_ascii_strcasecmp (o42_sheet_get_name (sheet), name) == 0)
        return sheet;
    }

  return NULL;
}

int
o42_book_sheet_index (O42Book *book, O42Sheet *sheet)
{
  g_return_val_if_fail (book != NULL, -1);

  for (guint i = 0; i < book->sheets->len; i++)
    if (g_ptr_array_index (book->sheets, i) == sheet)
      return (int) i;

  return -1;
}

O42Sheet *
o42_book_add_sheet (O42Book *book, const char *name, int index)
{
  O42Sheet *sheet;
  char *fresh = NULL;

  g_return_val_if_fail (book != NULL, NULL);

  if (name == NULL || *name == '\0' || o42_book_find_sheet (book, name) != NULL)
    {
      /* The first SheetN not in use, counting from one past what there is. */
      for (int n = (int) book->sheets->len + 1; ; n++)
        {
          g_free (fresh);
          fresh = g_strdup_printf ("Sheet%d", n);
          if (o42_book_find_sheet (book, fresh) == NULL)
            break;
        }
      name = fresh;
    }

  sheet = o42_sheet_new (name);
  o42_sheet_set_book (sheet, book);

  /* One undo step: the sheet was not there. */
  o42_sheet_begin_group (sheet);
  o42_sheet_undo_capture_sheet (sheet, FALSE);
  if (index < 0 || index >= (int) book->sheets->len)
    g_ptr_array_add (book->sheets, sheet);
  else
    g_ptr_array_insert (book->sheets, index, sheet);
  o42_sheet_end_group (sheet);

  g_free (fresh);
  return sheet;
}

gboolean
o42_book_detach_sheet (O42Book *book, int index)
{
  O42Sheet *gone;

  g_return_val_if_fail (book != NULL, FALSE);
  if (index < 0 || index >= (int) book->sheets->len || book->sheets->len < 2)
    return FALSE;
  gone = g_ptr_array_index (book->sheets, index);
  {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, book->names);
    while (g_hash_table_iter_next (&iter, &key, &value))
      if (((NamedRange *) value)->sheet == gone)
        g_hash_table_iter_remove (&iter);
  }
  g_ptr_array_steal_index (book->sheets, index);
  return TRUE;
}

void
o42_book_attach_sheet (O42Book *book, O42Sheet *sheet, int index)
{
  g_return_if_fail (book != NULL && sheet != NULL);
  if (o42_book_sheet_index (book, sheet) >= 0)
    return;
  o42_sheet_set_book (sheet, book);
  if (index < 0 || index >= (int) book->sheets->len)
    g_ptr_array_add (book->sheets, sheet);
  else
    g_ptr_array_insert (book->sheets, index, sheet);
}

gboolean
o42_book_remove_sheet (O42Book *book, int index)
{
  O42Sheet *gone;
  O42Sheet *survivor;

  g_return_val_if_fail (book != NULL, FALSE);

  if (index < 0 || index >= (int) book->sheets->len || book->sheets->len < 2)
    return FALSE;

  gone = g_ptr_array_index (book->sheets, index);
  survivor = g_ptr_array_index (book->sheets, index == 0 ? 1 : 0);

  /* The sheet is taken out of the book rather than destroyed, and the
   * undo record keeps it: Undo puts it back where it was, with
   * everything on it.  Formulas that read it give #REF! while it is
   * away and come right again when it returns, which is why they are
   * not rewritten.  Excel cannot undo this at all. */
  o42_sheet_begin_group (survivor);
  o42_sheet_undo_capture_sheet (gone, FALSE);
  o42_book_detach_sheet (book, index);
  o42_sheet_end_group (survivor);

  for (guint i = 0; i < book->sheets->len; i++)
    o42_sheet_stale_formulas (g_ptr_array_index (book->sheets, i));

  o42_book_changed (book, "sheets");
  o42_book_set_modified (book, TRUE);
  return TRUE;
}

static gboolean book_rename (O42Book *book, int index, const char *name, gboolean record);

gboolean
o42_book_rename_sheet (O42Book *book, int index, const char *name)
{
  return book_rename (book, index, name, TRUE);
}

/* Undo's rename: the same, but not itself an undo step. */
gboolean
o42_book_rename_sheet_unrecorded (O42Book *book, int index, const char *name)
{
  return book_rename (book, index, name, FALSE);
}

static gboolean
book_rename (O42Book *book, int index, const char *name, gboolean record)
{
  O42Sheet *sheet;
  char *old;

  g_return_val_if_fail (book != NULL, FALSE);

  sheet = o42_book_sheet (book, index);
  if (sheet == NULL || name == NULL || *name == '\0')
    return FALSE;
  if (strchr (name, '!') != NULL || strchr (name, ':') != NULL)
    return FALSE;

  {
    O42Sheet *taken = o42_book_find_sheet (book, name);
    if (taken != NULL && taken != sheet)
      return FALSE;
  }

  old = g_strdup (o42_sheet_get_name (sheet));
  if (strcmp (old, name) == 0)
    { g_free (old); return TRUE; }

  if (record)
    {
      /* One undo step: the name and every formula that spelled it. */
      o42_sheet_begin_group (sheet);
      o42_sheet_undo_capture_sheet (sheet, TRUE);
      o42_sheet_set_name (sheet, name);
      for (guint i = 0; i < book->sheets->len; i++)
        o42_sheet_rename_references (g_ptr_array_index (book->sheets, i), old, name);
      o42_sheet_end_group (sheet);
    }
  else
    {
      /* Undo restores the formulas itself from its cell snapshots. */
      o42_sheet_set_name (sheet, name);
    }

  g_free (old);
  return TRUE;
}

void
o42_book_set_iteration (O42Book *book, gboolean on, int max, double tolerance)
{
  g_return_if_fail (book != NULL);
  book->iterative = on;
  book->max_iterations = max > 0 ? max : 100;
  book->tolerance = tolerance > 0 ? tolerance : 0.001;
}

gboolean
o42_book_iteration (O42Book *book, int *max, double *tolerance)
{
  g_return_val_if_fail (book != NULL, FALSE);
  if (max != NULL)
    *max = book->max_iterations > 0 ? book->max_iterations : 100;
  if (tolerance != NULL)
    *tolerance = book->tolerance > 0 ? book->tolerance : 0.001;
  return book->iterative;
}

void
o42_book_set_manual (O42Book *book, gboolean manual)
{
  g_return_if_fail (book != NULL);
  book->manual = manual;
}

gboolean
o42_book_manual (O42Book *book)
{
  return book != NULL && book->manual;
}

void
o42_book_set_database (O42Book *book, const char *path, gboolean embedded)
{
  g_return_if_fail (book != NULL);
  g_free (book->db_path);
  book->db_path = (path != NULL && *path != '\0') ? g_strdup (path) : NULL;
  book->db_embedded = book->db_path != NULL && embedded;
  book->scripts_modified = TRUE;   /* the book, not a sheet, has changed */
}

const char *
o42_book_database (O42Book *book, gboolean *embedded)
{
  g_return_val_if_fail (book != NULL, NULL);
  if (embedded != NULL)
    *embedded = book->db_embedded;
  return book->db_path;
}

gboolean
o42_book_is_modified (O42Book *book)
{
  g_return_val_if_fail (book != NULL, FALSE);

  for (guint i = 0; i < book->sheets->len; i++)
    if (o42_sheet_is_modified (g_ptr_array_index (book->sheets, i)))
      return TRUE;

  return book->scripts_modified;
}

void
o42_book_set_modified (O42Book *book, gboolean modified)
{
  g_return_if_fail (book != NULL);

  for (guint i = 0; i < book->sheets->len; i++)
    o42_sheet_set_modified (g_ptr_array_index (book->sheets, i), modified);
  book->scripts_modified = modified;
}

static gboolean
name_is_legal (const char *name)
{
  int row, col;

  if (name == NULL || *name == '\0' || g_ascii_isdigit (*name))
    return FALSE;
  for (const char *p = name; *p != '\0'; p++)
    if (!g_ascii_isalnum (*p) && *p != '_' && *p != '.')
      return FALSE;
  /* A name that reads as a cell reference could never be reached. */
  if (o42_ref_parse (name, &row, &col, NULL))
    return FALSE;
  return TRUE;
}

static void
stale_users_of_name (O42Book *book, const char *upper)
{
  for (guint i = 0; i < book->sheets->len; i++)
    o42_sheet_name_changed (g_ptr_array_index (book->sheets, i), upper);
}

gboolean
o42_book_define_name (O42Book *book, const char *name, O42Sheet *sheet,
                      const O42Range *range)
{
  NamedRange *nr;
  char *upper;

  g_return_val_if_fail (book != NULL, FALSE);
  g_return_val_if_fail (sheet != NULL && range != NULL, FALSE);

  if (!name_is_legal (name))
    return FALSE;

  upper = g_ascii_strup (name, -1);
  nr = g_new0 (NamedRange, 1);
  nr->sheet = sheet;
  nr->range = *range;
  g_hash_table_insert (book->names, upper, nr);
  stale_users_of_name (book, g_intern_string (upper));
  o42_sheet_set_modified (sheet, TRUE);
  return TRUE;
}

gboolean
o42_book_undefine_name (O42Book *book, const char *name)
{
  char *upper;
  gboolean had;

  g_return_val_if_fail (book != NULL, FALSE);
  if (name == NULL)
    return FALSE;

  upper = g_ascii_strup (name, -1);
  had = g_hash_table_remove (book->names, upper);
  if (had)
    {
      stale_users_of_name (book, g_intern_string (upper));
      o42_book_set_modified (book, TRUE);
    }
  g_free (upper);
  return had;
}

gboolean
o42_book_lookup_name (O42Book *book, const char *name, O42Sheet **sheet,
                      O42Range *range)
{
  char *upper;
  NamedRange *nr;

  g_return_val_if_fail (book != NULL, FALSE);
  if (name == NULL)
    return FALSE;

  upper = g_ascii_strup (name, -1);
  nr = g_hash_table_lookup (book->names, upper);
  g_free (upper);
  if (nr == NULL)
    return FALSE;

  if (sheet) *sheet = nr->sheet;
  if (range) *range = nr->range;
  return TRUE;
}

GList *
o42_book_names (O42Book *book)
{
  GList *keys;

  g_return_val_if_fail (book != NULL, NULL);
  keys = g_hash_table_get_keys (book->names);
  return g_list_sort (keys, (GCompareFunc) g_strcmp0);
}

O42UndoStack *
o42_book_undo_stack (O42Book *book)
{
  g_return_val_if_fail (book != NULL, NULL);
  return book->stack;
}

void
o42_book_cell_changed (O42Book *book, O42Sheet *sheet, int row, int col)
{
  const char *name;

  g_return_if_fail (book != NULL);

  name = o42_sheet_get_name (sheet);
  for (guint i = 0; i < book->sheets->len; i++)
    {
      O42Sheet *other = g_ptr_array_index (book->sheets, i);

      if (other != sheet)
        o42_sheet_invalidate_from (other, name, row, col);
    }
}

void
o42_book_sheet_shifted (O42Book *book, O42Sheet *sheet, gboolean rows,
                        int at, int count)
{
  const char *name;

  g_return_if_fail (book != NULL);

  name = o42_sheet_get_name (sheet);
  for (guint i = 0; i < book->sheets->len; i++)
    {
      O42Sheet *other = g_ptr_array_index (book->sheets, i);

      if (other != sheet)
        o42_sheet_shift_references (other, name, rows, at, count);
    }
}

void
o42_book_clear (O42Book *book)
{
  const O42Range everything = { 0, 0, O42_MAX_ROWS - 1, O42_MAX_COLS - 1 };
  O42Sheet *first;
  GPtrArray *pictures;
  GList *names;

  while (o42_book_n_sheets (book) > 1)
    o42_book_remove_sheet (book, o42_book_n_sheets (book) - 1);
  g_ptr_array_set_size (book->scripts, 0);
  for (guint i = 0; i < book->styles->len; i++)
    g_free (g_array_index (book->styles, Style, i).name);
  g_array_set_size (book->styles, 0);
  add_builtin_styles (book);
  book->scripts_modified = FALSE;

  first = o42_book_sheet (book, 0);
  pictures = o42_sheet_pictures (first);
  o42_sheet_clear_range (first, &everything);
  o42_sheet_clear_formats (first, &everything);
  while (pictures->len > 0)
    o42_sheet_remove_picture (first, ((O42Picture *) g_ptr_array_index (pictures, 0))->id);
  while (o42_sheet_charts (first)->len > 0)
    o42_sheet_remove_chart (first, ((O42Chart *) g_ptr_array_index (o42_sheet_charts (first), 0))->id);
  g_array_set_size (o42_sheet_merges (first), 0);
  g_hash_table_remove_all (o42_sheet_notes (first));
  o42_sheet_set_frozen (first, 0, 0);
  o42_sheet_clear_conditions (first, NULL);
  o42_sheet_clear_validations (first, NULL);
  while (o42_sheet_pivots (first)->len > 0)
    o42_sheet_remove_pivot (first, &g_array_index (o42_sheet_pivots (first), O42Pivot, 0));
  o42_sheet_clear_autofilter (first);
  for (int i = 0; i < O42_MAX_COLS; i++)
    {
      o42_sheet_set_col_hidden (first, i, FALSE);
      o42_sheet_set_col_width (first, i, o42_sheet_col_width (first, O42_MAX_COLS - 1));
    }
  o42_book_rename_sheet (book, 0, "Sheet1");

  names = o42_book_names (book);
  for (GList *l = names; l != NULL; l = l->next)
    o42_book_undefine_name (book, l->data);
  g_list_free (names);
  o42_sheet_clear_undo (first);
}

O42Sheet *
o42_book_find_table (O42Book *book, const char *name)
{
  g_return_val_if_fail (book != NULL && name != NULL, NULL);
  for (guint i = 0; i < book->sheets->len; i++)
    {
      O42Sheet *sheet = g_ptr_array_index (book->sheets, i);
      if (o42_sheet_find_table (sheet, name) != NULL)
        return sheet;
    }
  return NULL;
}

/* ---- Cell styles ---------------------------------------------------- */

static Style *
find_style (O42Book *book, const char *name)
{
  for (guint i = 0; name != NULL && i < book->styles->len; i++)
    {
      Style *s = &g_array_index (book->styles, Style, i);
      if (g_ascii_strcasecmp (s->name, name) == 0)
        return s;
    }
  return NULL;
}

int
o42_book_n_styles (O42Book *book)
{
  g_return_val_if_fail (book != NULL, 0);
  return (int) book->styles->len;
}

const char *
o42_book_style_name (O42Book *book, int index)
{
  g_return_val_if_fail (book != NULL, NULL);
  if (index < 0 || (guint) index >= book->styles->len)
    return NULL;
  return g_array_index (book->styles, Style, index).name;
}

gboolean
o42_book_style (O42Book *book, const char *name, O42Fmt *fmt, O42FmtMask *mask)
{
  Style *s;

  g_return_val_if_fail (book != NULL, FALSE);
  s = find_style (book, name);
  if (s == NULL)
    return FALSE;
  if (fmt != NULL) *fmt = s->fmt;
  if (mask != NULL) *mask = s->mask;
  return TRUE;
}

void
o42_book_set_style (O42Book *book, const char *name, const O42Fmt *fmt, O42FmtMask mask)
{
  Style *s;

  g_return_if_fail (book != NULL && name != NULL && *name != '\0' && fmt != NULL);
  s = find_style (book, name);
  if (s == NULL)
    {
      Style fresh;
      fresh.name = g_strdup (name);
      fresh.fmt = *fmt;
      fresh.mask = mask;
      g_array_append_val (book->styles, fresh);
    }
  else
    {
      s->fmt = *fmt;
      s->mask = mask;
    }
  for (guint i = 0; i < book->sheets->len; i++)
    o42_sheet_restyle (g_ptr_array_index (book->sheets, i), name);
  o42_book_changed (book, "styles");
}

gboolean
o42_book_remove_style (O42Book *book, const char *name)
{
  Style *s = find_style (book, name);

  g_return_val_if_fail (book != NULL, FALSE);
  if (s == NULL)
    return FALSE;
  g_free (s->name);
  g_array_remove_index (book->styles, (guint) (s - &g_array_index (book->styles, Style, 0)));
  o42_book_changed (book, "styles");
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Recording a macro                                                       */
/* ---------------------------------------------------------------------- */

void
o42_book_record_start (O42Book *book)
{
  g_return_if_fail (book != NULL);
  if (book->recording != NULL)
    g_string_free (book->recording, TRUE);
  g_clear_pointer (&book->recorded_sheet, g_free);
  book->recording = g_string_new ("# Recorded by office42.\n"
                                  "import office42\n"
                                  "book = office42.book\n");
}

gboolean
o42_book_recording (O42Book *book)
{
  return book != NULL && book->recording != NULL;
}

char *
o42_book_record_stop (O42Book *book)
{
  char *text;

  g_return_val_if_fail (book != NULL, NULL);
  if (book->recording == NULL)
    return NULL;
  text = g_string_free (book->recording, FALSE);
  book->recording = NULL;
  g_clear_pointer (&book->recorded_sheet, g_free);
  return text;
}

void
o42_book_record_line (O42Book *book, const char *line)
{
  if (book == NULL || book->recording == NULL || line == NULL)
    return;
  g_string_append (book->recording, line);
  g_string_append_c (book->recording, '\n');
}

gboolean
o42_book_record_sheet (O42Book *book, const char *sheet_name)
{
  if (book == NULL || book->recording == NULL)
    return FALSE;
  if (sheet_name != NULL && g_strcmp0 (book->recorded_sheet, sheet_name) != 0)
    {
      char *escaped = o42_python_quote (sheet_name);

      g_string_append_printf (book->recording, "sheet = book[%s]\n", escaped);
      g_free (escaped);
      g_free (book->recorded_sheet);
      book->recorded_sheet = g_strdup (sheet_name);
    }
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Custom views                                                            */
/* ---------------------------------------------------------------------- */

static void
book_view_free (gpointer data)
{
  O42BookView *view = data;

  g_free (view->name);
  g_free (view->sheet);
  g_free (view);
}

static GPtrArray *
book_views (O42Book *book)
{
  if (book->views == NULL)
    book->views = g_ptr_array_new_with_free_func (book_view_free);
  return book->views;
}

int
o42_book_n_views (O42Book *book)
{
  g_return_val_if_fail (book != NULL, 0);
  return book->views != NULL ? (int) book->views->len : 0;
}

const O42BookView *
o42_book_view (O42Book *book, int index)
{
  g_return_val_if_fail (book != NULL, NULL);
  if (book->views == NULL || index < 0 || index >= (int) book->views->len)
    return NULL;
  return g_ptr_array_index (book->views, index);
}

const O42BookView *
o42_book_find_view (O42Book *book, const char *name)
{
  g_return_val_if_fail (book != NULL, NULL);
  if (book->views == NULL || name == NULL)
    return NULL;
  for (guint i = 0; i < book->views->len; i++)
    {
      O42BookView *view = g_ptr_array_index (book->views, i);

      if (g_ascii_strcasecmp (view->name, name) == 0)
        return view;
    }
  return NULL;
}

void
o42_book_set_view (O42Book *book, const O42BookView *value)
{
  GPtrArray *views;
  O42BookView *view;

  g_return_if_fail (book != NULL && value != NULL && value->name != NULL);
  views = book_views (book);
  view = (O42BookView *) o42_book_find_view (book, value->name);
  if (view == NULL)
    {
      view = g_new0 (O42BookView, 1);
      view->name = g_strdup (value->name);
      g_ptr_array_add (views, view);
    }
  g_free (view->sheet);
  view->sheet = g_strdup (value->sheet != NULL ? value->sheet : "");
  view->selection = value->selection;
  view->active_row = value->active_row;
  view->active_col = value->active_col;
  view->zoom = value->zoom > 0 ? value->zoom : 1.0;
  view->frozen_rows = value->frozen_rows;
  view->frozen_cols = value->frozen_cols;
  view->split = value->split;
  o42_book_set_modified (book, TRUE);
}

gboolean
o42_book_remove_view (O42Book *book, const char *name)
{
  g_return_val_if_fail (book != NULL, FALSE);
  if (book->views == NULL || name == NULL)
    return FALSE;
  for (guint i = 0; i < book->views->len; i++)
    {
      O42BookView *view = g_ptr_array_index (book->views, i);

      if (g_ascii_strcasecmp (view->name, name) == 0)
        {
          g_ptr_array_remove_index (book->views, i);
          o42_book_set_modified (book, TRUE);
          return TRUE;
        }
    }
  return FALSE;
}
