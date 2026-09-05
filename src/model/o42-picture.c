/* o42-picture.c - see o42-picture.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-picture.h"

#include "o42-image.h"

#include <pango/pangocairo.h>

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

/* What stands in for a picture that cannot be drawn -- a Windows
 * metafile, which gdk-pixbuf does not decode: a grey box with the
 * format's name in it, so the sheet shows where the picture is and
 * the bytes go on to the next save unharmed. */
static cairo_surface_t *
placeholder_surface (O42Picture *picture)
{
  int w = MAX (picture->pixel_w, 16), h = MAX (picture->pixel_h, 16);
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create (surface);
  PangoLayout *layout;
  char *label = g_ascii_strup (picture->format != NULL ? picture->format : "?", -1);
  int tw, th;

  cairo_set_source_rgb (cr, 0.93, 0.93, 0.93);
  cairo_paint (cr);
  cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
  cairo_set_line_width (cr, 1.0);
  cairo_rectangle (cr, 0.5, 0.5, w - 1, h - 1);
  cairo_stroke (cr);
  layout = pango_cairo_create_layout (cr);
  {
    PangoFontDescription *desc = pango_font_description_from_string ("Sans 10");
    pango_layout_set_font_description (layout, desc);
    pango_font_description_free (desc);
  }
  pango_layout_set_text (layout, label, -1);
  pango_layout_get_pixel_size (layout, &tw, &th);
  cairo_set_source_rgb (cr, 0.4, 0.4, 0.4);
  cairo_move_to (cr, (w - tw) / 2.0, (h - th) / 2.0);
  pango_cairo_show_layout (cr, layout);
  g_object_unref (layout);
  cairo_destroy (cr);
  g_free (label);
  return surface;
}

cairo_surface_t *
o42_picture_surface (O42Picture *picture)
{
  g_return_val_if_fail (picture != NULL, NULL);

  if (picture->surface == NULL)
    picture->surface = o42_image_surface (picture->data);
  if (picture->surface == NULL)
    picture->surface = placeholder_surface (picture);

  return picture->surface;
}
