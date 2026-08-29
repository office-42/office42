/* o42-image.c - see o42-image.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-image.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

static GdkPixbuf *
decode (GBytes *data, const char **format)
{
  GdkPixbufLoader *loader;
  GdkPixbuf *pixbuf = NULL;
  gsize len = 0;
  const guint8 *bytes;

  if (data == NULL)
    return NULL;

  bytes = g_bytes_get_data (data, &len);
  loader = gdk_pixbuf_loader_new ();

  if (gdk_pixbuf_loader_write (loader, bytes, len, NULL) &&
      gdk_pixbuf_loader_close (loader, NULL))
    {
      pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
      if (pixbuf != NULL)
        {
          g_object_ref (pixbuf);

          if (format != NULL)
            {
              GdkPixbufFormat *f = gdk_pixbuf_loader_get_format (loader);
              char *name = f != NULL ? gdk_pixbuf_format_get_name (f) : NULL;
              *format = g_intern_string (name != NULL ? name : "unknown");
              g_free (name);
            }
        }
    }
  else
    {
      gdk_pixbuf_loader_close (loader, NULL);
    }

  g_object_unref (loader);
  return pixbuf;
}

gboolean
o42_image_probe (GBytes *data, int *width, int *height, const char **format)
{
  GdkPixbuf *pixbuf = decode (data, format);

  if (pixbuf == NULL)
    return FALSE;

  if (width)  *width  = gdk_pixbuf_get_width (pixbuf);
  if (height) *height = gdk_pixbuf_get_height (pixbuf);

  g_object_unref (pixbuf);
  return TRUE;
}

GBytes *
o42_image_load_file (GFile       *file,
                     int         *width,
                     int         *height,
                     const char **format,
                     GError     **error)
{
  char *contents = NULL;
  gsize length = 0;
  GBytes *data;

  g_return_val_if_fail (G_IS_FILE (file), NULL);

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return NULL;

  data = g_bytes_new_take (contents, length);

  if (!o42_image_probe (data, width, height, format))
    {
      g_bytes_unref (data);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "That file is not a picture in any format office42 "
                   "can read.");
      return NULL;
    }

  return data;
}

/* Straight RGB(A) bytes to premultiplied native-order BGRA: the conversion
 * GTK does inside gdk_cairo_set_source_pixbuf(), done here without GTK. */
cairo_surface_t *
o42_image_surface (GBytes *data)
{
  GdkPixbuf *pixbuf = decode (data, NULL);
  cairo_surface_t *surface;
  int width, height, src_stride, dst_stride, channels;
  const guint8 *src;
  guint8 *dst;
  gboolean alpha;

  if (pixbuf == NULL)
    return NULL;

  width  = gdk_pixbuf_get_width (pixbuf);
  height = gdk_pixbuf_get_height (pixbuf);
  src_stride = gdk_pixbuf_get_rowstride (pixbuf);
  channels = gdk_pixbuf_get_n_channels (pixbuf);
  alpha = gdk_pixbuf_get_has_alpha (pixbuf);
  src = gdk_pixbuf_read_pixels (pixbuf);

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy (surface);
      g_object_unref (pixbuf);
      return NULL;
    }

  cairo_surface_flush (surface);
  dst = cairo_image_surface_get_data (surface);
  dst_stride = cairo_image_surface_get_stride (surface);

  for (int y = 0; y < height; y++)
    {
      const guint8 *s = src + y * src_stride;
      guint32 *d = (guint32 *) (dst + y * dst_stride);

      for (int x = 0; x < width; x++)
        {
          guint32 r = s[0], g = s[1], b = s[2];
          guint32 a = alpha ? s[3] : 255;

          if (a != 255)
            {
              r = (r * a + 127) / 255;
              g = (g * a + 127) / 255;
              b = (b * a + 127) / 255;
            }

          d[x] = (a << 24) | (r << 16) | (g << 8) | b;
          s += channels;
        }
    }

  cairo_surface_mark_dirty (surface);
  g_object_unref (pixbuf);

  return surface;
}
