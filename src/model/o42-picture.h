/* o42-picture.h - a picture floating over the grid
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A picture in a spreadsheet is not in a cell; it floats above the grid,
 * anchored to a cell so that it moves with the rows and columns around it.
 * That is how Excel has always done it and it is the only arrangement that
 * survives a row being inserted above.
 */

#pragma once

#include <cairo.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  guint            id;         /* stable for the picture's lifetime */
  guint            group;      /* objects grouped together share one; 0 for none */
  GBytes          *data;       /* the file's bytes, as loaded */
  const char      *format;     /* interned: "png", "jpeg", ... */
  int              pixel_w;
  int              pixel_h;
  int              row;        /* the anchor cell */
  int              col;
  double           dx;         /* offset inside the anchor cell, pixels */
  double           dy;
  double           width;      /* shown size, pixels */
  double           height;
  cairo_surface_t *surface;    /* decoded on first draw */
} O42Picture;

O42Picture      *o42_picture_new     (GBytes *data, const char *format,
                                      int pixel_w, int pixel_h);
void             o42_picture_free    (O42Picture *picture);
cairo_surface_t *o42_picture_surface (O42Picture *picture);

G_END_DECLS
