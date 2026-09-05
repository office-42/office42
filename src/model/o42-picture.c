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
  picture->lock_aspect = TRUE;

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

void
o42_picture_paint (O42Picture *picture, cairo_t *cr, double width, double height)
{
  cairo_surface_t *surface;
  double pw, ph, sx, sy, sw, sh;

  g_return_if_fail (picture != NULL && cr != NULL);
  surface = o42_picture_surface (picture);
  if (surface == NULL || width <= 0 || height <= 0)
    return;
  pw = cairo_image_surface_get_width (surface);
  ph = cairo_image_surface_get_height (surface);
  /* The part that is shown: the picture less the cropped margins. */
  sx = CLAMP (picture->crop_l, 0, 0.99) * pw;
  sy = CLAMP (picture->crop_t, 0, 0.99) * ph;
  sw = MAX (pw - sx - CLAMP (picture->crop_r, 0, 0.99) * pw, 1);
  sh = MAX (ph - sy - CLAMP (picture->crop_b, 0, 0.99) * ph, 1);

  cairo_save (cr);
  cairo_rectangle (cr, 0, 0, width, height);
  cairo_clip (cr);
  cairo_scale (cr, width / sw, height / sh);
  cairo_set_source_surface (cr, surface, -sx, -sy);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
  cairo_paint (cr);
  cairo_restore (cr);
}

cairo_surface_t *
o42_picture_surface (O42Picture *picture)
{
  g_return_val_if_fail (picture != NULL, NULL);

  if (picture->surface == NULL)
    picture->surface = o42_image_surface (picture->data);

  return picture->surface;
}
