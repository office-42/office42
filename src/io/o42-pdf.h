/* o42-pdf.h - a sheet out to PDF, and a PDF's tables back into a sheet
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Export paginates the used range onto landscape Letter pages -- across
 * first, then down, as Excel's default print order has it -- through cairo's
 * PDF surface, with the cells' own fonts and formats and the pictures where
 * they float.
 *
 * Import goes through poppler when it was available at build time.  A PDF
 * has no cells; what it has is characters with positions.  Those are grouped
 * into lines by their height on the page, lines are split into cells where
 * the gaps between characters are wider than characters, and cells are put
 * into columns by clustering where they start.  It recovers a printed table
 * well and a paragraph of prose as one long cell, which is what there is to
 * recover.
 */

#pragma once

#include <cairo.h>
#include <gio/gio.h>

#include "o42-sheet.h"
#include "o42-book.h"

G_BEGIN_DECLS

gboolean o42_pdf_export (O42Sheet *sheet, GFile *file, GError **error);

/* Every sheet of a book, one after another in tab order, into one PDF:
 * Excel's "entire workbook".  A chart sheet contributes its one page. */
gboolean o42_pdf_export_book (O42Book *book, GFile *file, GError **error);

/* ---- Pagination, shared with printing --------------------------------- */

/* The used range cut into pages of a given printable size, across then
 * down.  Sizes are in points; each page is drawn with its top-left corner
 * at the cairo origin, in points. */
typedef struct _O42Pages O42Pages;

O42Pages *o42_pages_new   (O42Sheet *sheet, double width_pt, double height_pt);

/* The file's name, for &F in a header or footer. */
void      o42_pages_set_document (O42Pages *pages, const char *name);
int       o42_pages_count (O42Pages *pages);
void      o42_pages_draw  (O42Pages *pages, int page, cairo_t *cr);
void      o42_pages_free  (O42Pages *pages);

/* Where the printed pages divide: the first row of every row band
 * after the first, and the first column of every column band after
 * the first.  The caller frees the array with g_free. */
int       o42_pages_row_breaks (O42Pages *pages, int **rows);
int       o42_pages_col_breaks (O42Pages *pages, int **cols);

gboolean o42_pdf_import_available (void);

/* Appends the PDF's pages below whatever the sheet already holds, one blank
 * row between pages, as a single undo step. */
gboolean o42_pdf_import (O42Sheet *sheet, GFile *file, GError **error);

G_END_DECLS
