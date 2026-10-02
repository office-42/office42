/* o42-xlsx-draw.h - pictures and charts in .xlsx: the drawing parts
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A worksheet's floating objects live in a drawing part, anchored to
 * cells in EMUs, with pictures as blips into xl/media and charts as
 * graphic frames pointing at chart parts in DrawingML's chart schema.
 * This is the half of .xlsx that is about objects rather than cells.
 */

#pragma once

#include <gio/gio.h>

#include "o42-book.h"
#include "o42-zip.h"

G_BEGIN_DECLS

/* Writes the drawing, media and chart parts for `sheet` (number
 * `index`, from 1) into the archive, appends the content-type entries
 * they need to `content_types` and the drawing relationship to
 * `sheet_rels` (taking an id from `next_rid`).  Returns the id used,
 * or 0 if the sheet has nothing floating over it. */
int      o42_xlsx_draw_write (O42ZipWriter *zip, O42Sheet *sheet, int index,
                              GString *content_types, GHashTable *extensions_seen,
                              GString *sheet_rels, int *next_rid);

/* Package plumbing shared with the cell reader: a part's relationships
 * (Id -> Target; empty if it has none), and a target resolved against
 * the directory of the part that names it. */
GHashTable *o42_xlsx_read_rels (GHashTable *parts, const char *part);
char       *o42_xlsx_resolve   (const char *part, const char *target);

/* Reads the drawing that `sheet_part` (the worksheet's part name)
 * refers to by relationship id `rid`, adding its pictures and charts
 * to `sheet`.  `theme` is the book's twelve theme colours, lt1, dk1,
 * lt2, dk2, the six accents and the two link colours, which a chart
 * takes its colours from; `ours` says the book is one office42 wrote,
 * whose charts are in Excel 97's look unless they say otherwise. */
void     o42_xlsx_draw_read  (GHashTable *parts, const char *sheet_part,
                              const char *rid, O42Sheet *sheet,
                              const guint32 *theme, gboolean ours);

G_END_DECLS
