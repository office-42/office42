/* o42-sql.c - a SQLite database beside the sheet
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SQLite is the whole database: one file, no server, and it is already
 * on nearly every machine there is.  A book can point at such a file or
 * carry one inside itself; either way a query's answer comes back as a
 * rectangle of values, and a rectangle of values is what a sheet is.
 */

#include "o42-sql.h"
#include "o42-eval.h"

#include <string.h>

#ifdef HAVE_SQLITE
#include <sqlite3.h>
#endif

struct _O42Db {
  char *path;
#ifdef HAVE_SQLITE
  sqlite3 *handle;
#endif
};

gboolean
o42_db_available (void)
{
#ifdef HAVE_SQLITE
  return TRUE;
#else
  return FALSE;
#endif
}

#define O42_DB_ERROR (o42_db_error_quark ())

static GQuark
o42_db_error_quark (void)
{
  return g_quark_from_static_string ("o42-db-error");
}

O42Db *
o42_db_open (const char *path, GError **error)
{
#ifdef HAVE_SQLITE
  O42Db *db;
  sqlite3 *handle = NULL;

  g_return_val_if_fail (path != NULL, NULL);
  if (sqlite3_open (path, &handle) != SQLITE_OK)
    {
      g_set_error (error, O42_DB_ERROR, 1, "%s",
                   handle != NULL ? sqlite3_errmsg (handle) : "could not open the database");
      sqlite3_close (handle);
      return NULL;
    }

  db = g_new0 (O42Db, 1);
  db->path = g_strdup (path);
  db->handle = handle;
  return db;
#else
  (void) path;
  g_set_error (error, O42_DB_ERROR, 1,
               "this build of office42 has no SQLite in it");
  return NULL;
#endif
}

void
o42_db_close (O42Db *db)
{
  if (db == NULL)
    return;
#ifdef HAVE_SQLITE
  sqlite3_close (db->handle);
#endif
  g_free (db->path);
  g_free (db);
}

const char *
o42_db_path (O42Db *db)
{
  return db != NULL ? db->path : NULL;
}

void
o42_db_result_free (O42DbResult *result)
{
  if (result == NULL)
    return;
  for (int i = 0; i < result->rows * result->cols; i++)
    o42_value_clear (&result->cells[i]);
  g_free (result->cells);
  g_strfreev (result->names);
  g_free (result);
}

#ifdef HAVE_SQLITE

/* One cell of an answer.  SQLite's types are few and they line up with
 * a spreadsheet's: a number is a number, everything else is text, and
 * NULL is an empty cell. */
static O42Value
column_value (sqlite3_stmt *stmt, int col)
{
  switch (sqlite3_column_type (stmt, col))
    {
    case SQLITE_INTEGER:
    case SQLITE_FLOAT:
      return o42_value_number (sqlite3_column_double (stmt, col));
    case SQLITE_NULL:
      return o42_value_empty ();
    default:
      {
        const unsigned char *text = sqlite3_column_text (stmt, col);

        return o42_value_text (text != NULL ? (const char *) text : "");
      }
    }
}

#endif

O42DbResult *
o42_db_query (O42Db *db, const char *sql, GError **error)
{
#ifdef HAVE_SQLITE
  sqlite3_stmt *stmt = NULL;
  O42DbResult *result;
  GArray *cells;
  int cols, rows = 0, step;

  g_return_val_if_fail (db != NULL && sql != NULL, NULL);
  if (sqlite3_prepare_v2 (db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
      g_set_error (error, O42_DB_ERROR, 2, "%s", sqlite3_errmsg (db->handle));
      return NULL;
    }

  cols = sqlite3_column_count (stmt);
  cells = g_array_new (FALSE, FALSE, sizeof (O42Value));
  while ((step = sqlite3_step (stmt)) == SQLITE_ROW)
    {
      for (int c = 0; c < cols; c++)
        {
          O42Value v = column_value (stmt, c);

          g_array_append_val (cells, v);
        }
      rows++;
    }
  if (step != SQLITE_DONE)
    {
      g_set_error (error, O42_DB_ERROR, 2, "%s", sqlite3_errmsg (db->handle));
      for (guint i = 0; i < cells->len; i++)
        o42_value_clear (&g_array_index (cells, O42Value, i));
      g_array_free (cells, TRUE);
      sqlite3_finalize (stmt);
      return NULL;
    }

  result = g_new0 (O42DbResult, 1);
  result->rows = rows;
  result->cols = cols;
  result->names = g_new0 (char *, cols + 1);
  for (int c = 0; c < cols; c++)
    {
      const char *name = sqlite3_column_name (stmt, c);

      result->names[c] = g_strdup (name != NULL ? name : "");
    }
  result->cells = (O42Value *) g_array_free (cells, FALSE);
  sqlite3_finalize (stmt);
  return result;
#else
  (void) db; (void) sql;
  g_set_error (error, O42_DB_ERROR, 1, "this build of office42 has no SQLite in it");
  return NULL;
#endif
}

gboolean
o42_db_exec (O42Db *db, const char *sql, int *changes, GError **error)
{
#ifdef HAVE_SQLITE
  char *message = NULL;

  g_return_val_if_fail (db != NULL && sql != NULL, FALSE);
  if (sqlite3_exec (db->handle, sql, NULL, NULL, &message) != SQLITE_OK)
    {
      g_set_error (error, O42_DB_ERROR, 3, "%s",
                   message != NULL ? message : "the statement failed");
      sqlite3_free (message);
      return FALSE;
    }
  sqlite3_free (message);
  if (changes != NULL)
    *changes = sqlite3_changes (db->handle);
  return TRUE;
#else
  (void) db; (void) sql; (void) changes;
  g_set_error (error, O42_DB_ERROR, 1, "this build of office42 has no SQLite in it");
  return FALSE;
#endif
}

/* The one-column answer to a query, as a string vector. */
static char **
query_strings (O42Db *db, const char *sql)
{
  O42DbResult *result = o42_db_query (db, sql, NULL);
  GPtrArray *names;

  if (result == NULL)
    return NULL;
  names = g_ptr_array_new ();
  for (int r = 0; r < result->rows; r++)
    {
      O42Value *v = &result->cells[r * result->cols];

      g_ptr_array_add (names, o42_value_display (v));
    }
  g_ptr_array_add (names, NULL);
  o42_db_result_free (result);
  return (char **) g_ptr_array_free (names, FALSE);
}

char **
o42_db_tables (O42Db *db)
{
  g_return_val_if_fail (db != NULL, NULL);
  return query_strings (db, "SELECT name FROM sqlite_master WHERE type='table' "
                            "AND name NOT LIKE 'sqlite_%' ORDER BY name");
}

char **
o42_db_columns (O42Db *db, const char *table)
{
  char *sql, *quoted;
  char **names;

  g_return_val_if_fail (db != NULL && table != NULL, NULL);
  quoted = g_strdup (table);
  g_strdelimit (quoted, "'\"", ' ');
  sql = g_strdup_printf ("SELECT name FROM pragma_table_info('%s')", quoted);
  names = query_strings (db, sql);
  g_free (sql);
  g_free (quoted);
  return names;
}

O42Range
o42_db_put_result (O42Sheet *sheet, const O42DbResult *result,
                   int row, int col, gboolean headings)
{
  O42Range filled = { row, col, row, col };
  int at = row;

  g_return_val_if_fail (sheet != NULL && result != NULL, filled);

  if (headings)
    {
      for (int c = 0; c < result->cols; c++)
        o42_sheet_set_input (sheet, at, col + c, result->names[c]);
      at++;
    }
  for (int r = 0; r < result->rows; r++)
    {
      for (int c = 0; c < result->cols; c++)
        {
          const O42Value *v = &result->cells[r * result->cols + c];

          if (v->type == O42_VALUE_EMPTY)
            o42_sheet_set_input (sheet, at, col + c, "");
          else
            {
              /* Text goes in with an apostrophe before it, so that a
               * column of part numbers stays a column of part numbers
               * and does not turn into dates and sums. */
              char *text = o42_value_display (v);
              char *input = v->type == O42_VALUE_TEXT
                            ? g_strconcat ("'", text, NULL) : g_strdup (text);

              o42_sheet_set_input (sheet, at, col + c, input);
              g_free (input);
              g_free (text);
            }
        }
      at++;
    }

  filled.row1 = MAX (at - 1, row);
  filled.col1 = col + MAX (result->cols - 1, 0);
  return filled;
}

/* A name SQLite will take: quoted, with any quote inside it doubled. */
static char *
quote_name (const char *name)
{
  GString *out = g_string_new ("\"");

  for (const char *p = name; p != NULL && *p != '\0'; p++)
    {
      if (*p == '"')
        g_string_append_c (out, '"');
      g_string_append_c (out, *p);
    }
  g_string_append_c (out, '"');
  return g_string_free (out, FALSE);
}

/* A value SQLite will take: a number as itself, anything else quoted. */
static char *
quote_value (O42Sheet *sheet, int row, int col)
{
  O42Value v;
  char *text, *out;

  o42_sheet_get_value (sheet, row, col, &v);
  if (v.type == O42_VALUE_EMPTY)
    {
      o42_value_clear (&v);
      return g_strdup ("NULL");
    }
  if (v.type == O42_VALUE_NUMBER)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      g_ascii_dtostr (buf, sizeof buf, v.as.number);
      o42_value_clear (&v);
      return g_strdup (buf);
    }
  text = o42_value_display (&v);
  o42_value_clear (&v);
  {
    GString *quoted = g_string_new ("'");

    for (const char *p = text; *p != '\0'; p++)
      {
        if (*p == '\'')
          g_string_append_c (quoted, '\'');
        g_string_append_c (quoted, *p);
      }
    g_string_append_c (quoted, '\'');
    out = g_string_free (quoted, FALSE);
  }
  g_free (text);
  return out;
}

gboolean
o42_db_put_range (O42Db *db, O42Sheet *sheet, const O42Range *range,
                  const char *table, gboolean headings, GError **error)
{
  GString *sql;
  char *quoted_table;
  int first_row;
  gboolean ok = TRUE;

  g_return_val_if_fail (db != NULL && sheet != NULL && range != NULL && table != NULL, FALSE);
  quoted_table = quote_name (table);
  first_row = headings ? range->row0 + 1 : range->row0;

  /* The table is made if it is not there: the headings name its
   * columns, or C1, C2, C3 when there are none. */
  sql = g_string_new (NULL);
  g_string_append_printf (sql, "CREATE TABLE IF NOT EXISTS %s (", quoted_table);
  for (int c = range->col0; c <= range->col1; c++)
    {
      char *name;

      if (headings)
        {
          char *text = o42_sheet_get_display (sheet, range->row0, c);

          name = quote_name ((text != NULL && *text != '\0') ? text : "column");
          g_free (text);
        }
      else
        {
          char *plain = g_strdup_printf ("C%d", c - range->col0 + 1);

          name = quote_name (plain);
          g_free (plain);
        }
      g_string_append_printf (sql, "%s%s", c > range->col0 ? ", " : "", name);
      g_free (name);
    }
  g_string_append (sql, ")");
  ok = o42_db_exec (db, sql->str, NULL, error);
  g_string_free (sql, TRUE);

  for (int r = first_row; ok && r <= range->row1; r++)
    {
      sql = g_string_new (NULL);
      g_string_append_printf (sql, "INSERT INTO %s VALUES (", quoted_table);
      for (int c = range->col0; c <= range->col1; c++)
        {
          char *value = quote_value (sheet, r, c);

          g_string_append_printf (sql, "%s%s", c > range->col0 ? ", " : "", value);
          g_free (value);
        }
      g_string_append (sql, ")");
      ok = o42_db_exec (db, sql->str, NULL, error);
      g_string_free (sql, TRUE);
    }

  g_free (quoted_table);
  return ok;
}

O42Db *
o42_db_for_book (O42Book *book, GError **error)
{
  const char *path;

  g_return_val_if_fail (book != NULL, NULL);
  path = o42_book_database (book, NULL);
  if (path == NULL)
    {
      g_set_error (error, O42_DB_ERROR, 4, "this book has no database");
      return NULL;
    }
  return o42_db_open (path, error);
}

gboolean
o42_db_query_into (O42Db *db, O42Sheet *sheet, const char *sql,
                   int row, int col, gboolean headings,
                   O42Range *filled, GError **error)
{
  O42DbResult *result;
  O42Range where;

  g_return_val_if_fail (db != NULL && sheet != NULL && sql != NULL, FALSE);
  result = o42_db_query (db, sql, error);
  if (result == NULL)
    return FALSE;

  o42_sheet_begin_group (sheet);
  where = o42_db_put_result (sheet, result, row, col, headings);
  o42_sheet_add_query (sheet, sql, &where, headings);
  o42_sheet_end_group (sheet);
  o42_db_result_free (result);
  if (filled != NULL)
    *filled = where;
  return TRUE;
}

int
o42_db_refresh (O42Db *db, O42Sheet *sheet, GError **error)
{
  GArray *queries;
  GPtrArray *sql;
  GArray *at;
  GArray *headings;
  int done = 0;

  g_return_val_if_fail (db != NULL && sheet != NULL, -1);
  queries = o42_sheet_queries (sheet);

  /* The queries are copied out first: laying an answer down rewrites
   * the very array being walked. */
  sql = g_ptr_array_new_with_free_func (g_free);
  at = g_array_new (FALSE, FALSE, sizeof (O42Range));
  headings = g_array_new (FALSE, FALSE, sizeof (gboolean));
  for (guint i = 0; i < queries->len; i++)
    {
      const O42Query *q = &g_array_index (queries, O42Query, i);

      g_ptr_array_add (sql, g_strdup (q->sql));
      g_array_append_val (at, q->at);
      g_array_append_val (headings, q->headings);
    }

  for (guint i = 0; i < sql->len; i++)
    {
      O42Range where = g_array_index (at, O42Range, i);
      gboolean with_names = g_array_index (headings, gboolean, i);

      /* Whatever the last answer filled is cleared first, so that a
       * shorter one does not leave the tail of the old one behind. */
      o42_sheet_begin_group (sheet);
      for (int r = where.row0; r <= where.row1; r++)
        for (int c = where.col0; c <= where.col1; c++)
          o42_sheet_set_input (sheet, r, c, "");
      o42_sheet_end_group (sheet);

      if (!o42_db_query_into (db, sheet, g_ptr_array_index (sql, i),
                              where.row0, where.col0, with_names, NULL, error))
        {
          done = -1;
          break;
        }
      done++;
    }

  g_ptr_array_free (sql, TRUE);
  g_array_free (at, TRUE);
  g_array_free (headings, TRUE);

  /* SQLVALUE() in cells asked the database too, and its answer may
   * have changed under it; a refresh is a refresh. */
  o42_sheet_stale_formulas (sheet);
  return done;
}

/* ---- SQLVALUE(), a query in a cell ------------------------------------- */

/* The whole answer to a query does not fit in a cell, so the formula
 * answers with one cell of it -- the first by default, which is what a
 * SELECT SUM(...) or SELECT COUNT(*) has to say.  Whole tables come in
 * through Data > Get Data, which lays them out and can refresh them.
 *
 * The connection is kept between calls: a sheet full of SQLVALUE would
 * be unbearable if every one of them opened the file again. */

static O42Book *fn_book;
static O42Db   *fn_db;
static char    *fn_db_path;

static O42Db *
function_db (void)
{
  const char *path;

  if (fn_book == NULL)
    return NULL;
  path = o42_book_database (fn_book, NULL);
  if (path == NULL)
    return NULL;
  if (fn_db != NULL && g_strcmp0 (fn_db_path, path) == 0)
    return fn_db;

  o42_db_close (fn_db);
  g_free (fn_db_path);
  fn_db = o42_db_open (path, NULL);
  fn_db_path = g_strdup (path);
  return fn_db;
}

static O42Value
function_sqlvalue (O42EvalContext *ctx, const char *name,
                   O42Operand *args, int n_args, gpointer user)
{
  O42Db *db = function_db ();
  O42DbResult *result;
  O42Value query, out;
  char *sql;
  int col = 1, row = 1;

  (void) name; (void) user;
  if (db == NULL)
    return o42_value_error (O42_ERR_NA);
  if (n_args < 1)
    return o42_value_error (O42_ERR_VALUE);

  o42_operand_cell (ctx, &args[0], 0, 0, &query);
  sql = o42_value_display (&query);
  o42_value_clear (&query);

  for (int i = 1; i < n_args && i <= 2; i++)
    {
      O42Value v;
      double n = 0;
      O42ErrorCode err = O42_ERR_VALUE;

      o42_operand_cell (ctx, &args[i], 0, 0, &v);
      if (o42_value_to_number (&v, &n, &err))
        {
          if (i == 1)
            col = (int) n;
          else
            row = (int) n;
        }
      o42_value_clear (&v);
    }

  result = o42_db_query (db, sql, NULL);
  g_free (sql);
  if (result == NULL)
    return o42_value_error (O42_ERR_VALUE);
  if (row < 1 || row > result->rows || col < 1 || col > result->cols)
    out = o42_value_error (O42_ERR_NA);
  else
    out = o42_value_copy (&result->cells[(row - 1) * result->cols + (col - 1)]);
  o42_db_result_free (result);
  return out;
}

void
o42_db_register_function (O42Book *book)
{
  /* Without SQLite there is nothing to ask, and SQLVALUE should read
   * as the unknown name it is rather than answer #N/A forever. */
  if (!o42_db_available ())
    return;
  fn_book = book;
  if (book == NULL)
    {
      o42_db_close (fn_db);
      fn_db = NULL;
      g_clear_pointer (&fn_db_path, g_free);
      o42_function_unregister_external ("SQLVALUE");
      return;
    }
  o42_function_register_external ("SQLVALUE", 1, 3,
    "SQLVALUE(query, column, row)",
    "One cell of what a query asks of the book's database.",
    function_sqlvalue, NULL);
}
