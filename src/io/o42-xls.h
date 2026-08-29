/* o42-xls.h - Excel's binary file format, BIFF in a compound file
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The .xls of Excel 5 through 2003: a "Workbook" stream of BIFF
 * records -- fonts, formats, XFs, bound sheets, a shared string table,
 * then per sheet the rows, cells and formulas as token streams.  The
 * writer produces BIFF8 (Excel 97-2003), which every later Excel, Calc
 * and Gnumeric read.  The reader takes BIFF8 and, for cells and
 * formulas, Excel 5's BIFF5 too.
 *
 * Formulas are compiled to Excel's parsed tokens and back, with the
 * function table Excel numbered; functions Excel 97 did not have go
 * out the way Excel writes them, as add-in names.
 */

#pragma once

#include <gio/gio.h>

#include "o42-book.h"

G_BEGIN_DECLS

gboolean o42_xls_save (O42Book *book, GFile *file, GError **error);

/* How many cells the last o42_xls_save left out because they were
 * outside Excel 97's grid of 65,536 rows by 256 columns.  Zero when
 * everything fitted, which is nearly always. */
extern int o42_xls_dropped_cells;
gboolean o42_xls_load (O42Book *book, GFile *file, GError **error);

G_END_DECLS
