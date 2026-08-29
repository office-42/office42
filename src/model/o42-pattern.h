/* o42-pattern.h - the shading patterns a cell can be filled with
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Excel fills a cell with a background colour and, over it, one of
 * eighteen patterns in a second colour: greys of five densities,
 * stripes four ways, and the two crosshatches, each in a light and a
 * dark weight.  Every one is an eight by eight bitmap laid down over
 * and over, which is what this draws.
 */

#pragma once

#include <cairo.h>
#include <glib.h>

#include "o42-fmt.h"

G_BEGIN_DECLS

/* Fills the rectangle the way the format asks: the shading colour
 * first, then the pattern over it in its own colour.  Nothing at all
 * for a cell with neither. */
void o42_pattern_fill (const O42Fmt *fmt, cairo_t *cr,
                       double x, double y, double w, double h);

/* The name Excel's files use: "solid", "darkGray", "lightUp"... */
const char *o42_pattern_name  (O42Pattern pattern);
gboolean    o42_pattern_parse (const char *name, O42Pattern *pattern);

/* Gnumeric's Shade numbers, which are its own ordering of the same
 * patterns; 1 is a solid fill. */
int         o42_pattern_to_shade   (O42Pattern pattern);
O42Pattern  o42_pattern_from_shade (int shade);

G_END_DECLS
