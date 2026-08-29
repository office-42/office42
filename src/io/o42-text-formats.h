/* o42-text-formats.h - DIF, SYLK and LaTeX
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Three interchange formats older than the spreadsheets that read them
 * now, and the one a paper is set in.
 *
 * DIF is VisiCalc's, from 1981: a header of triples, then a vector per
 * row, each cell three lines long.  SYLK is Multiplan's, from the same
 * years and still what Excel offers as "SYLK (Symbolic Link)": one
 * line per record, semicolon-separated fields, formulas in R1C1.  Both
 * are read and written here.
 *
 * LaTeX is written only, as a tabular a paper can \input: what the
 * cells show, escaped, with the alignment each column asks for.
 */

#pragma once

#include <gio/gio.h>

#include "o42-sheet.h"

G_BEGIN_DECLS

/* DIF: one sheet, the values as they are shown and as numbers where
 * they are numbers. */
gboolean o42_dif_save (O42Sheet *sheet, GFile *file, GError **error);
gboolean o42_dif_load (O42Sheet *sheet, GFile *file, GError **error);

/* SYLK: one sheet, with formulas kept as formulas and column widths
 * and number formats carried along. */
gboolean o42_sylk_save (O42Sheet *sheet, GFile *file, GError **error);
gboolean o42_sylk_load (O42Sheet *sheet, GFile *file, GError **error);

/* LaTeX: a tabular of what the cells show. */
gboolean o42_latex_save (O42Sheet *sheet, GFile *file, GError **error);

G_END_DECLS
