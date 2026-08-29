/* o42-zip.h - the zip container, as much of it as .xlsx needs
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An .xlsx file is a zip archive of XML parts.  Reading one needs the
 * central directory and inflate; writing one needs deflate and the
 * headers around it.  GLib supplies deflate and inflate in their raw
 * form, so the container itself is a few hundred lines and no new
 * dependency.  Nothing beyond plain deflated or stored entries is
 * understood: no encryption, no zip64, no spanning.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* The archive's entries, name -> GBytes of the uncompressed content.
 * NULL with `error` set if the bytes are not a zip file. */
GHashTable *o42_zip_read (GBytes *archive, GError **error);

/* Builds an archive.  Add entries in the order they should appear;
 * finish gives the bytes and frees the builder. */
typedef struct _O42ZipWriter O42ZipWriter;

O42ZipWriter *o42_zip_writer_new    (void);
void          o42_zip_writer_add    (O42ZipWriter *zip, const char *name,
                                     const char *data, gsize length);
/* The same, uncompressed: what OpenDocument wants for its mimetype
 * entry, which must be first and readable without inflating. */
void          o42_zip_writer_add_stored (O42ZipWriter *zip, const char *name,
                                         const char *data, gsize length);
GBytes       *o42_zip_writer_finish (O42ZipWriter *zip);

G_END_DECLS
