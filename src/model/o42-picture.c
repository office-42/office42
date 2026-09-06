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
  g_clear_pointer (&picture->adjusted, cairo_surface_destroy);
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

void
o42_picture_paint (O42Picture *picture, cairo_t *cr, double width, double height)
{
  cairo_surface_t *surface;
  double pw, ph, sx, sy, sw, sh;

  g_return_if_fail (picture != NULL && cr != NULL);
  surface = o42_picture_shown (picture);
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
  if (picture->surface == NULL)
    picture->surface = placeholder_surface (picture);

  return picture->surface;
}

cairo_surface_t *
o42_picture_shown (O42Picture *picture)
{
  cairo_surface_t *base = o42_picture_surface (picture);
  int w, h, stride;
  unsigned char *src, *dst;
  double b, c;

  if (base == NULL || (picture->brightness == 0 && picture->contrast == 0))
    return base;
  if (picture->adjusted != NULL && picture->adjusted_b == picture->brightness &&
      picture->adjusted_c == picture->contrast)
    return picture->adjusted;
  g_clear_pointer (&picture->adjusted, cairo_surface_destroy);
  if (cairo_image_surface_get_format (base) != CAIRO_FORMAT_ARGB32 &&
      cairo_image_surface_get_format (base) != CAIRO_FORMAT_RGB24)
    return base;

  /* Each channel: contrast about the middle, then brightness added,
   * on the colour with its alpha taken out and put back. */
  w = cairo_image_surface_get_width (base);
  h = cairo_image_surface_get_height (base);
  picture->adjusted = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  cairo_surface_flush (base);
  stride = cairo_image_surface_get_stride (base);
  src = cairo_image_surface_get_data (base);
  dst = cairo_image_surface_get_data (picture->adjusted);
  b = CLAMP (picture->brightness, -1, 1) * 255;
  c = CLAMP (picture->contrast, -1, 1);
  c = c >= 0 ? 1 + 3 * c : 1 + c;   /* a gain: up to four times, down to nothing */
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        const guint32 *sp = (const guint32 *) (src + y * stride + x * 4);
        guint32 *dp = (guint32 *) (dst + y * cairo_image_surface_get_stride (picture->adjusted) + x * 4);
        guint32 p = *sp;
        double a = cairo_image_surface_get_format (base) == CAIRO_FORMAT_ARGB32 ? ((p >> 24) & 0xFF) / 255.0 : 1.0;
        guint32 out = ((guint32) (a * 255)) << 24;

        for (int shift = 0; shift <= 16; shift += 8)
          {
            double v = ((p >> shift) & 0xFF);
            if (a > 0) v /= a;
            v = (v - 128) * c + 128 + b;
            v = CLAMP (v, 0, 255) * a;
            out |= ((guint32) (v + 0.5)) << shift;
          }
        *dp = out;
      }
  cairo_surface_mark_dirty (picture->adjusted);
  picture->adjusted_b = picture->brightness;
  picture->adjusted_c = picture->contrast;
  return picture->adjusted;
}

