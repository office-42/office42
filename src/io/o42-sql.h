/* o42-sql.h - a SQLite database beside the sheet
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef O42_SQL_H
#define O42_SQL_H

#include <glib.h>
#include "o42-value.h"
#include "o42-sheet.h"
#include "o42-book.h"

G_BEGIN_DECLS

/* A book can have one database: a SQLite file beside it, or one carried
 * inside the book itself.  Either way the sheet talks to it the same
 * way -- a query's answer arrives as a rectangle of values, which is
 * what a spreadsheet is made of. */

typedef struct _O42Db O42Db;

/* Opens a database file, making it if it is not there.  ":memory:" is
 * a database that lives only as long as the program does.  NULL and an
 * error if SQLite is not in this build. */
O42Db      *o42_db_open  (const char *path, GError **error);
void        o42_db_close (O42Db *db);
const char *o42_db_path  (O42Db *db);

/* Was office42 built with SQLite at all? */
gboolean    o42_db_available (void);

/* The answer to a query: `rows` by `cols` of values, with the column
 * names alongside.  The heading row is not among the values. */
typedef struct {
  int        rows;
  int        cols;
  char     **names;   /* cols of them, owned */
  O42Value  *cells;   /* rows * cols, row-major, owned */
} O42DbResult;

O42DbResult *o42_db_query       (O42Db *db, const char *sql, GError **error);
void         o42_db_result_free (O42DbResult *result);

/* For statements that answer with nothing: CREATE, INSERT, UPDATE and
 * the rest.  `changes` takes how many rows were touched, or NULL. */
gboolean     o42_db_exec (O42Db *db, const char *sql, int *changes, GError **error);

/* The tables in the database, in name order; free with g_strfreev. */
char       **o42_db_tables (O42Db *db);

/* The columns of one table, in the order they were declared. */
char       **o42_db_columns (O42Db *db, const char *table);

/* Puts a query's answer into the sheet, its top-left at (row, col), the
 * column names in the first row when `headings`.  Returns the range it
 * filled. */
O42Range     o42_db_put_result (O42Sheet *sheet, const O42DbResult *result,
                                int row, int col, gboolean headings);

/* Writes a range of the sheet into a table, making the table from the
 * first row's headings if it is not there already.  The remaining rows
 * become its rows.  TRUE if it worked. */
gboolean     o42_db_put_range (O42Db *db, O42Sheet *sheet, const O42Range *range,
                               const char *table, gboolean headings, GError **error);

/* Opens the database the book remembers -- a file beside it, or the one
 * carried inside it -- or NULL when the book has none. */
O42Db       *o42_db_for_book (O42Book *book, GError **error);

/* Runs every query this sheet remembers and lays each answer out again
 * where it was.  The number refreshed, or -1 on the first failure. */
int          o42_db_refresh (O42Db *db, O42Sheet *sheet, GError **error);

/* Runs a query and puts its answer at (row, col), remembering it on the
 * sheet so that a refresh can run it again.  FALSE and an error if the
 * query failed. */
gboolean     o42_db_query_into (O42Db *db, O42Sheet *sheet, const char *sql,
                                int row, int col, gboolean headings,
                                O42Range *filled, GError **error);

/* Makes SQLVALUE() work: a formula that asks the book's database a
 * question and answers with one cell of what it says.  The connection
 * is opened when it is first wanted and kept until the book changes.
 * Called with NULL to take the function away again. */
void         o42_db_register_function (O42Book *book);

G_END_DECLS

#endif /* O42_SQL_H */
