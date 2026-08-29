/* o42-html.h - a book out as HTML tables, and tables back in
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * How a spreadsheet most often reaches a browser: one table per sheet,
 * the values as they are shown, with the fonts, fills, borders and
 * alignments as inline style, merged cells as colspan and rowspan, and
 * hyperlinks as links.  Reading goes the other way, taking the tables
 * out of a page whether or not it is well-formed XML -- most pages are
 * not -- so a table copied from the web lands in cells.
 */

#pragma once

#include "o42-book.h"

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean o42_html_save (O42Book *book, GFile *file, GError **error);

/* Every table in the file, one under the other on `sheet`, a blank row
 * between them.  Numbers and dates are parsed as typing them would be. */
gboolean o42_html_load (O42Sheet *sheet, GFile *file, GError **error);

G_END_DECLS
