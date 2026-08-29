/* o42-xlsx.h - Excel's file format, the Office Open XML one
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An .xlsx is a zip of XML parts: a workbook listing the sheets, one
 * part per sheet with rows of cells, a table of shared strings, and a
 * styles part the cells index into.  Formulas are written in the same
 * A1 notation office42 speaks, so they travel unchanged.
 *
 * The writer puts out what the model holds -- values, formulas, formats,
 * column widths and row heights, hidden rows and columns, merged cells,
 * frozen panes, the AutoFilter range and defined names.  The reader
 * takes the same from anything Excel, LibreOffice or Gnumeric wrote,
 * including shared formulas, and passes over the rest.  Pictures,
 * charts, notes and conditional formats stay in .gnumeric for now.
 */

#pragma once

#include <gio/gio.h>

#include "o42-book.h"

G_BEGIN_DECLS

gboolean o42_xlsx_save (O42Book *book, GFile *file, GError **error);

/* Shared with the .xls reader: the format code Excel's built-in number
 * format ids stand for (NULL if unknown), and a code applied to a
 * format -- preset, date/time kind, or custom.  Returns TRUE if the
 * code shows a date or time. */
const char *o42_xlsx_builtin_number_format (int id);
gboolean    o42_xlsx_apply_format_code    (O42Fmt *fmt, const char *code);

/* Replaces the book's sheets with the file's, like o42_gnumeric_load. */
gboolean o42_xlsx_load (O42Book *book, GFile *file, GError **error);

G_END_DECLS
