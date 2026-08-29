/* o42-picture.c - see o42-picture.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-picture.h"

#include "o42-image.h"

O42Picture *
o42_picture_new (GBytes *data, const char *format, int pixel_w, int pixel_h)
{
  O42Picture *picture;

  g_return_val_if_fail (data != NULL, NULL);

  picture = g_new0 (O42Picture, 1);
  picture->data    = g_bytes_ref (data);
  picture->format  = g_intern_string (format != NULL ? format : "unknown");
  picture->pixel_w = pixel_w;
  picture->pixel_h = pixel_h;
  picture->width   = pixel_w;
  picture->height  = pixel_h;

  return picture;
}

void
o42_picture_free (O42Picture *picture)
{
  if (picture == NULL)
    return;

  g_clear_pointer (&picture->data, g_bytes_unref);
  g_clear_pointer (&picture->surface, cairo_surface_destroy);
  g_free (picture);
}

cairo_surface_t *
o42_picture_surface (O42Picture *picture)
{
  g_return_val_if_fail (picture != NULL, NULL);

  if (picture->surface == NULL)
    picture->surface = o42_image_surface (picture->data);

  return picture->surface;
}
