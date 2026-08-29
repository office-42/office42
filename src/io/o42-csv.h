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

gboolean o42_csv_save (O42Sheet *sheet, GFile *file, GError **error);

/* Replaces the sheet's cells with the file's, as one undo step. */
gboolean o42_csv_load (O42Sheet *sheet, GFile *file, GError **error);

G_END_DECLS
