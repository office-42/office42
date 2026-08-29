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

/* Premultiplied ARGB32, or NULL if the bytes are not a picture. */
cairo_surface_t *o42_image_surface (GBytes *data);

G_END_DECLS
