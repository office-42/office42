/* o42-image.h - decoding pictures, and handing them to cairo
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gdk-pixbuf does the decoding, which gives office42 every format it has a
 * loader for without office42 knowing anything about any of them.  The bytes
 * of the file are what the sheet keeps; the decoded picture is a cache.
 * gdk-pixbuf is not GTK, so this can sit below the interface.
 */

#pragma once

#include <cairo.h>
#include <gio/gio.h>

G_BEGIN_DECLS

gboolean o42_image_probe (GBytes      *data,
                          int         *width,
                          int         *height,
                          const char **format);

GBytes  *o42_image_load_file (GFile       *file,
                              int         *width,
                              int         *height,
                              const char **format,
                              GError     **error);

/* Premultiplied ARGB32, or NULL if the bytes are not a picture gdk-pixbuf
 * can draw (a metafile is not). */
cairo_surface_t *o42_image_surface (GBytes *data);

/* An EMF or WMF: kept as bytes, passed on to the file formats that hold
 * them, and drawn as a placeholder.  The size is the header's. */
gboolean o42_image_is_metafile (GBytes *data, int *width, int *height, const char **format);

/* The picture re-encoded as PNG, for a file format that holds nothing
 * else; NULL if gdk-pixbuf cannot decode it. */
GBytes  *o42_image_as_png (GBytes *data);

G_END_DECLS
