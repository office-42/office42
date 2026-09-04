/* o42-csv.h - comma-separated values, both ways
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The oldest exchange format there is, and the one every program reads.
 * Writing puts out what the cells show, as Excel and Gnumeric do, since a
 * formula means nothing to whoever reads a CSV.  Reading follows RFC 4180:
 * fields are separated by commas, a field holding a comma, a quote or a
 * newline is wrapped in quotes, and a quote inside is doubled.  Each field
 * then goes through the same number-or-text decision a typed cell gets.
 */

#pragma once

#include <gio/gio.h>

#include "o42-sheet.h"

G_BEGIN_DECLS

/* Text from a file that may not be UTF-8 -- a Lotus, SYLK, DIF or HTML
 * file from a Windows of thirty years ago -- made into UTF-8: kept as it
 * is when it already is, else read as the machine's code page, else
 * Windows-1252, else with the bad bytes replaced.  Never NULL; the model
 * must never hold text it cannot write out again.  Caller frees. */
char *o42_text_to_utf8 (const char *text, gssize length);

gboolean o42_csv_save (O42Sheet *sheet, GFile *file, GError **error);

/* Replaces the sheet's cells with the file's, as one undo step. */
gboolean o42_csv_load (O42Sheet *sheet, GFile *file, GError **error);

G_END_DECLS
