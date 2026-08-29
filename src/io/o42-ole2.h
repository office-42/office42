/* o42-ole2.h - the OLE2 compound file, as much of it as .xls needs
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Excel 5 through 2003 wrote a "compound file": a little file system
 * of 512-byte sectors with a FAT, a directory and a mini-FAT for the
 * small streams, holding the workbook as one stream called "Workbook"
 * (or "Book" in Excel 5's day).  word42's .doc lives in the same
 * container.  Reading walks the FAT chains; writing lays out one or
 * more streams with nothing fancy: version 3, 512-byte sectors, the
 * FAT listed in the header.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* TRUE if the bytes begin with the compound-file signature. */
gboolean o42_ole2_is_compound (GBytes *file);

/* The named stream's content, searched without regard to case, or NULL
 * if there is no such stream. */
GBytes *o42_ole2_read_stream (GBytes *file, const char *name, GError **error);

/* A compound file holding the given streams.  `names` and `contents`
 * run in parallel; `n` of each. */
GBytes *o42_ole2_build (const char **names, GBytes **contents, int n);

G_END_DECLS
