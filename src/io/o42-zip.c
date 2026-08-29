/* o42-zip.c - the zip container, as much of it as .xlsx needs
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-zip.h"

#include <string.h>

/* CRC-32 as zip uses it (IEEE polynomial, reflected). */
static guint32
crc32_bytes (const guchar *data, gsize n)
{
  static guint32 table[256];
  static gboolean ready = FALSE;
  guint32 crc = 0xffffffffu;

  if (!ready)
    {
      for (guint i = 0; i < 256; i++)
        {
          guint32 c = i;
          for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
          table[i] = c;
        }
      ready = TRUE;
    }
  for (gsize i = 0; i < n; i++)
    crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
  return crc ^ 0xffffffffu;
}

/* Runs `data` through a GConverter and returns the result. */
static GBytes *
convert (GConverter *conv, const guchar *data, gsize n)
{
  GOutputStream *mem = g_memory_output_stream_new_resizable ();
  GOutputStream *out = g_converter_output_stream_new (mem, conv);
  GBytes *result = NULL;

  if (g_output_stream_write_all (out, data, n, NULL, NULL, NULL)
      && g_output_stream_close (out, NULL, NULL))
    result = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (mem));
  g_object_unref (out);
  g_object_unref (mem);
  return result;
}

/* Little-endian readers over a bounds-checked buffer. */
static guint16
rd16 (const guchar *p)
{
  return p[0] | (p[1] << 8);
}

static guint32
rd32 (const guchar *p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((guint32) p[3] << 24);
}

GHashTable *
o42_zip_read (GBytes *archive, GError **error)
{
  gsize size;
  const guchar *buf = g_bytes_get_data (archive, &size);
  gsize eocd = 0;
  gboolean found = FALSE;

  /* The end-of-central-directory record sits at the end, possibly
   * followed by a comment of up to 64K. */
  for (gsize back = 22; back <= size && back <= 22 + 65535; back++)
    {
      gsize at = size - back;
      if (rd32 (buf + at) == 0x06054b50)
        {
          eocd = at;
          found = TRUE;
          break;
        }
    }
  if (!found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Not a zip archive");
      return NULL;
    }

  guint entries = rd16 (buf + eocd + 10);
  gsize cd = rd32 (buf + eocd + 16);
  GHashTable *table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify) g_bytes_unref);

  for (guint i = 0; i < entries; i++)
    {
      if (cd + 46 > size || rd32 (buf + cd) != 0x02014b50)
        break;
      guint method = rd16 (buf + cd + 10);
      gsize csize = rd32 (buf + cd + 20);
      gsize usize = rd32 (buf + cd + 24);
      guint nlen = rd16 (buf + cd + 28);
      guint xlen = rd16 (buf + cd + 30);
      guint clen = rd16 (buf + cd + 32);
      gsize local = rd32 (buf + cd + 42);
      if (cd + 46 + nlen > size)
        break;
      char *name = g_strndup ((const char *) buf + cd + 46, nlen);
      cd += 46 + nlen + xlen + clen;

      if (local + 30 > size || rd32 (buf + local) != 0x04034b50)
        {
          g_free (name);
          continue;
        }
      gsize data = local + 30 + rd16 (buf + local + 26) + rd16 (buf + local + 28);
      if (data + csize > size)
        {
          g_free (name);
          continue;
        }

      GBytes *content = NULL;
      if (method == 0)
        content = g_bytes_new (buf + data, csize);
      else if (method == 8)
        {
          GConverter *conv = G_CONVERTER (g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW));
          content = convert (conv, buf + data, csize);
          g_object_unref (conv);
        }
      if (content == NULL || (usize != 0 && g_bytes_get_size (content) != usize))
        {
          g_clear_pointer (&content, g_bytes_unref);
          g_free (name);
          continue;
        }
      g_hash_table_insert (table, name, content);
    }
  return table;
}

/* ---- writing ---- */

struct _O42ZipWriter
{
  GByteArray *body;      /* local headers and data so far */
  GByteArray *central;   /* central directory entries */
  guint       count;
};

static void
put16 (GByteArray *a, guint v)
{
  guchar b[2] = { v & 0xff, (v >> 8) & 0xff };
  g_byte_array_append (a, b, 2);
}

static void
put32 (GByteArray *a, guint32 v)
{
  guchar b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff };
  g_byte_array_append (a, b, 4);
}

O42ZipWriter *
o42_zip_writer_new (void)
{
  O42ZipWriter *zip = g_new0 (O42ZipWriter, 1);
  zip->body = g_byte_array_new ();
  zip->central = g_byte_array_new ();
  return zip;
}

static void
zip_writer_add_full (O42ZipWriter *zip, const char *name,
                     const char *data, gsize length, gboolean store)
{
  guint32 crc = crc32_bytes ((const guchar *) data, length);
  GConverter *conv = store ? NULL : G_CONVERTER (g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW, 6));
  GBytes *packed = conv != NULL ? convert (conv, (const guchar *) data, length) : NULL;
  if (conv != NULL)
    g_object_unref (conv);

  guint method = 8;
  gsize csize = packed ? g_bytes_get_size (packed) : length;
  const guchar *cdata = packed ? g_bytes_get_data (packed, NULL) : (const guchar *) data;
  if (packed == NULL || csize >= length)
    {
      method = 0;
      csize = length;
      cdata = (const guchar *) data;
    }
  /* A fixed DOS timestamp: 1980-01-01 00:00, so identical books give
   * identical files. */
  guint dos_time = 0, dos_date = (1 << 5) | 1;
  gsize nlen = strlen (name);
  guint32 offset = zip->body->len;

  put32 (zip->body, 0x04034b50);
  put16 (zip->body, 20);
  put16 (zip->body, 0);
  put16 (zip->body, method);
  put16 (zip->body, dos_time);
  put16 (zip->body, dos_date);
  put32 (zip->body, crc);
  put32 (zip->body, csize);
  put32 (zip->body, length);
  put16 (zip->body, nlen);
  put16 (zip->body, 0);
  g_byte_array_append (zip->body, (const guchar *) name, nlen);
  g_byte_array_append (zip->body, cdata, csize);

  put32 (zip->central, 0x02014b50);
  put16 (zip->central, 20);
  put16 (zip->central, 20);
  put16 (zip->central, 0);
  put16 (zip->central, method);
  put16 (zip->central, dos_time);
  put16 (zip->central, dos_date);
  put32 (zip->central, crc);
  put32 (zip->central, csize);
  put32 (zip->central, length);
  put16 (zip->central, nlen);
  put16 (zip->central, 0);
  put16 (zip->central, 0);
  put16 (zip->central, 0);
  put16 (zip->central, 0);
  put32 (zip->central, 0);
  put32 (zip->central, offset);
  g_byte_array_append (zip->central, (const guchar *) name, nlen);
  zip->count++;

  g_clear_pointer (&packed, g_bytes_unref);
}

void
o42_zip_writer_add (O42ZipWriter *zip, const char *name, const char *data, gsize length)
{
  zip_writer_add_full (zip, name, data, length, FALSE);
}

void
o42_zip_writer_add_stored (O42ZipWriter *zip, const char *name, const char *data, gsize length)
{
  zip_writer_add_full (zip, name, data, length, TRUE);
}

GBytes *
o42_zip_writer_finish (O42ZipWriter *zip)
{
  guint32 cd_offset = zip->body->len;
  guint32 cd_size = zip->central->len;

  g_byte_array_append (zip->body, zip->central->data, zip->central->len);
  put32 (zip->body, 0x06054b50);
  put16 (zip->body, 0);
  put16 (zip->body, 0);
  put16 (zip->body, zip->count);
  put16 (zip->body, zip->count);
  put32 (zip->body, cd_size);
  put32 (zip->body, cd_offset);
  put16 (zip->body, 0);

  GBytes *result = g_byte_array_free_to_bytes (zip->body);
  g_byte_array_unref (zip->central);
  g_free (zip);
  return result;
}
