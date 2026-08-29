/* o42-gnumeric.h - Gnumeric's own file format
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * office42's native format is Gnumeric's: gzipped XML, documented by its
 * source, and able to carry everything office42's model holds -- values,
 * formulas, formats, column widths, pictures.  Choosing it over a format of
 * our own means a file saved here opens in Gnumeric, and Gnumeric's files
 * open here, which is the shortest route to a corpus of real spreadsheets.
 *
 * The writer puts out the subset office42 has; the reader takes what it
 * understands from anything Gnumeric wrote and passes over the rest.
 */

#pragma once

#include <gio/gio.h>

#include "o42-book.h"

G_BEGIN_DECLS

gboolean o42_gnumeric_save (O42Book *book, GFile *file, GError **error);

/* Replaces the book's sheets with the file's.  Not an undo step: opening
 * a file is where a history starts. */
gboolean o42_gnumeric_load (O42Book *book, GFile *file, GError **error);

G_END_DECLS
