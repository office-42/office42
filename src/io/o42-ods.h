/* o42-ods.h - OpenDocument spreadsheets, .ods
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The format of LibreOffice Calc and of Gnumeric's second choice: a zip
 * of XML, with cell values typed, formulas in the OpenFormula notation
 * (of:=SUM([.A1:.B3])) and formats as automatic styles.  Written and
 * read: cells, formulas, formats and number styles, column widths and
 * row heights, hidden rows and columns, merges, notes, names and
 * frozen panes.  Not carried: charts, pictures, validation, filters,
 * conditional formats, pivots and scripts.
 */

#pragma once

#include "o42-book.h"
#include <gio/gio.h>

G_BEGIN_DECLS

gboolean o42_ods_save (O42Book *book, GFile *file, GError **error);
gboolean o42_ods_load (O42Book *book, GFile *file, GError **error);

G_END_DECLS
