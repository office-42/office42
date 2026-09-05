/* o42-richtext.h - a cell's text set in more than one font
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Turning a cell's runs into the Pango attributes that draw them, so
 * that the grid, the printer and the PDF all show the same thing.
 */

#pragma once

#include <pango/pangocairo.h>

#include "o42-fmt.h"
#include "o42-sheet.h"

G_BEGIN_DECLS

/* The attributes for `text` under `base`, with each run laid over it.
 * NULL when there are no runs.  The caller unrefs what comes back. */
PangoAttrList *o42_runs_attributes (const O42TextRun *runs, int n_runs,
                                    const O42Fmt *base, const char *text);

/* The "_x" gaps a format asked for, as shape attributes that make each
 * stand-in space as wide as the glyph x is in the layout's font, added
 * to `attrs` (made if NULL).  `layout` is used to measure and is left
 * with its text unset. */
void o42_format_pad_attributes (PangoLayout *layout, const O42FormatLayout *pads,
                                PangoAttrList **attrs);
/* The same for the pads in [from, to) of the text, with their offsets
 * taken from `from`: for laying out a part of the text on its own. */
void o42_format_pad_attributes_between (PangoLayout *layout, const O42FormatLayout *pads,
                                        int from, int to, PangoAttrList **attrs);

/* Draws a filled text in two halves: what is before the fill at lx,
 * what is after it at rx, and the fill character (when it is not a
 * space) across the `gap` between them, all at `ty`.  The layout's
 * font is used and its text and attributes are left unset. */
void o42_format_draw_filled (cairo_t *cr, PangoLayout *layout, const char *text,
                             const O42FormatLayout *fill, gboolean underline, gboolean strikeout,
                             double lx, double rx, double gap, double ty);

/* Where the two halves of a filled text go in a cell `width` wide with
 * `pad` at each side: the left half at its left edge, the right half
 * flush right, and how wide the gap between them is.  FALSE when the
 * text has no fill or does not fit, and then it is drawn as one. */
gboolean o42_format_fill_split (PangoLayout *layout, const O42FormatLayout *fill,
                                double width, double pad,
                                double *left_w, double *right_w, double *gap);

G_END_DECLS
