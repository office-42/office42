/* o42-file.h - which format a file is, and writing one whole
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* The formats office42 reads and writes. */
typedef enum {
  O42_FILE_GNUMERIC,   /* .gnumeric, and a name office42 does not know */
  O42_FILE_XLSX,       /* .xlsx and .xlsm */
  O42_FILE_XLS,
  O42_FILE_ODS,        /* .ods, and .fods, the same in one XML document */
  O42_FILE_HTML,       /* .html and .htm */
  O42_FILE_CSV,        /* .csv; .txt, .tsv and .tab have tabs */
  O42_FILE_DIF,
  O42_FILE_SYLK,       /* .slk and .sylk */
  O42_FILE_LATEX,      /* .tex, written and never read */
  O42_FILE_LOTUS       /* .wk1 and .wks */
} O42FileFormat;

/* The format a name's ending says, whatever its case: BOOK.XLSX is an
 * Excel book as much as book.xlsx is. */
O42FileFormat o42_file_format_for_name (const char *name);
O42FileFormat o42_file_format (GFile *file);

/* Whether the name ends in one of those endings at all. */
gboolean      o42_file_name_has_format (const char *name);

/* The format to read a file as: what its first bytes say when they say
 * it plainly -- a zip, an OLE2 compound file, gzip, XML, HTML, SYLK, DIF
 * or 1-2-3 -- and what its name says when they do not.  An Excel book
 * called .xls that is really HTML, as the web exports them, or a book
 * with no ending at all, opens as what it is. */
O42FileFormat o42_file_format_to_read (GFile *file);

/* Whether the format holds every sheet of a book, or only the one on
 * show. */
gboolean o42_file_format_is_book (O42FileFormat format);

/* g_file_replace_contents, except that a write that fails -- the disk
 * full, a quota reached -- leaves the file that was there as it was,
 * where GLib would put what it had written so far in its place. */
gboolean o42_file_replace (GFile *file, const void *data, gsize length,
                           GError **error);

/* Closes a stream from g_file_replace, throwing away what was written
 * to it, so that the file it was to replace stays as it was. */
void     o42_file_abandon (GOutputStream *stream);

G_END_DECLS
