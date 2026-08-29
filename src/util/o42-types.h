/* o42-types.h - cell addresses and the shapes everything else is built from
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.  See the LICENSE file for the full text.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Excel 5 offered 16384 rows and 256 columns, and so does office42.  The
 * limits are not a storage decision -- the sheet is sparse and an empty cell
 * costs nothing -- they are what a reference like IV16384 can name. */
#define O42_MAX_ROWS 1048576
#define O42_MAX_COLS 16384

/* A cell address.  Rows and columns are zero-based inside the program and
 * one-based-and-lettered on screen, which is the only place the two
 * conventions are allowed to meet. */
typedef struct {
  int row;
  int col;
} O42Ref;

/* A rectangle of cells, both corners inclusive. */
typedef struct {
  int row0;
  int col0;
  int row1;
  int col1;
} O42Range;

/* Cells are keyed by one integer so the sparse store can be an ordinary hash
 * table rather than anything clever. */
static inline guint64
o42_key (int row, int col)
{
  return ((guint64) (guint32) row << 32) | (guint32) col;
}

static inline int o42_key_row (guint64 key) { return (int) (key >> 32); }
static inline int o42_key_col (guint64 key) { return (int) (key & 0xffffffffu); }

static inline gboolean
o42_range_contains (const O42Range *r, int row, int col)
{
  return row >= r->row0 && row <= r->row1 && col >= r->col0 && col <= r->col1;
}

static inline O42Range
o42_range_normalise (int row0, int col0, int row1, int col1)
{
  O42Range r;

  r.row0 = MIN (row0, row1);
  r.row1 = MAX (row0, row1);
  r.col0 = MIN (col0, col1);
  r.col1 = MAX (col0, col1);

  return r;
}

/* ---- A1-style names --------------------------------------------------- */

/* Writes the column letters for `col` (0 is A, 26 is AA) into `buffer`,
 * which needs room for four characters and a terminator. */
void  o42_col_name (int col, char *buffer, gsize size);

/* "B12" for row 11, column 1.  The caller frees the result. */
char *o42_ref_name (int row, int col);

/* Parses "B12", "$B$12" and the mixed forms.  The dollar signs mark a row
 * or column as absolute -- one that stays put when the formula is copied
 * somewhere else -- and the full form reports which were present. */
gboolean o42_ref_parse (const char *text, int *row, int *col, gsize *length);
gboolean o42_ref_parse_full (const char *text, int *row, int *col,
                             gboolean *row_abs, gboolean *col_abs,
                             gsize *length);

/* "$B$12" and the rest, the way a formula writes them. */
char *o42_ref_name_full (int row, int col, gboolean row_abs, gboolean col_abs);

G_END_DECLS
