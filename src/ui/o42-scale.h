/* o42-scale.h - how much larger the desktop wants its text
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Windows set to 125 or 150 per cent asks every program for text and
 * controls that much larger.  GTK scales a window by whole numbers
 * only, so there it draws at 100 per cent and passes the rest on in one
 * place: the text resolution, 120 or 144 DPI where 96 is normal.  Font
 * sizes given in points follow it; everything given in pixels -- the
 * sizes in style.css, the cells of a sheet -- does not.  GNOME's "large
 * text" arrives the same way.
 *
 * This is that remainder as a factor, never below 1: macOS reports 72
 * DPI as its convention for 1 point to the pixel, which is not a wish
 * for anything smaller.  style.css's pixel sizes and the grid's zoom
 * are both multiplied by it, so the chrome and the sheet grow together
 * and a cell's text stays in proportion to the cell.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The factor for `display`'s settings: 1.0 at 96 DPI, 1.25 at 120. */
double o42_text_scale (GdkDisplay *display);

G_END_DECLS
