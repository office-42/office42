/* o42-ole2.c - the OLE2 compound file, as much of it as .xls needs
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-ole2.h"

#include <string.h>

#define ENDOFCHAIN 0xFFFFFFFEu
#define FREESECT   0xFFFFFFFFu
#define NOSTREAM   0xFFFFFFFFu
#define MINI_CUTOFF 4096
#define MINI_SECTOR 64

static const guchar SIGNATURE[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };

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

gboolean
o42_ole2_is_compound (GBytes *file)
{
  gsize size;
  const guchar *buf = g_bytes_get_data (file, &size);
  return size >= 512 && memcmp (buf, SIGNATURE, 8) == 0;
}

/* Follows a chain through a FAT (an array of guint32) and returns the
 * sector numbers in order.  Bounded, so a looping FAT ends rather
 * than hangs. */
static GArray *
chain (const guint32 *fat, gsize n_fat, guint32 start)
{
  GArray *sectors = g_array_new (FALSE, FALSE, sizeof (guint32));
  guint32 s = start;

  while (s < n_fat && sectors->len < n_fat)
    {
      g_array_append_val (sectors, s);
      s = fat[s];
    }
  return sectors;
}

/* Reads a chain of sectors of `sector_size` from `base` (the file for
 * regular sectors, the mini stream for mini sectors) into one buffer,
 * clipped to `size` bytes. */
static GBytes *
read_chain (const guchar *base, gsize base_len, gsize sector_size,
            gsize header, const guint32 *fat, gsize n_fat,
            guint32 start, gsize size)
{
  GArray *sectors = chain (fat, n_fat, start);
  GByteArray *out = g_byte_array_sized_new (size);

  for (guint i = 0; i < sectors->len && out->len < size; i++)
    {
      gsize at = header + (gsize) g_array_index (sectors, guint32, i) * sector_size;
      gsize take = MIN (sector_size, size - out->len);
      if (at + take > base_len)
        break;
      g_byte_array_append (out, base + at, take);
    }
  g_array_unref (sectors);
  return g_byte_array_free_to_bytes (out);
}

GBytes *
o42_ole2_read_stream (GBytes *file, const char *name, GError **error)
{
  gsize size;
  const guchar *buf = g_bytes_get_data (file, &size);
  guint sector_shift, mini_shift;
  gsize sector_size, mini_size;
  guint32 n_fat_sectors, first_dir, first_minifat, n_minifat, first_difat, n_difat;
  GArray *fat, *minifat = NULL;
  GBytes *dir, *mini_stream = NULL, *result = NULL;
  gsize dir_len;
  const guchar *d;

  if (!o42_ole2_is_compound (file))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Not an OLE2 compound file");
      return NULL;
    }
  sector_shift = rd16 (buf + 0x1E);
  mini_shift = rd16 (buf + 0x20);
  if (sector_shift < 7 || sector_shift > 12 || mini_shift > sector_shift)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Bad compound file header");
      return NULL;
    }
  sector_size = (gsize) 1 << sector_shift;
  mini_size = (gsize) 1 << mini_shift;
  n_fat_sectors = rd32 (buf + 0x2C);
  first_dir = rd32 (buf + 0x30);
  first_minifat = rd32 (buf + 0x3C);
  n_minifat = rd32 (buf + 0x40);
  first_difat = rd32 (buf + 0x44);
  n_difat = rd32 (buf + 0x48);

  /* The FAT: its sectors are listed in the header's DIFAT, then in
   * DIFAT sectors chained through their last entry. */
  {
    GArray *fat_sectors = g_array_new (FALSE, FALSE, sizeof (guint32));
    guint per_sector = sector_size / 4;

    for (guint i = 0; i < 109 && fat_sectors->len < n_fat_sectors; i++)
      {
        guint32 s = rd32 (buf + 0x4C + i * 4);
        if (s >= FREESECT - 1) break;
        g_array_append_val (fat_sectors, s);
      }
    for (guint32 ds = first_difat, k = 0; ds < FREESECT - 1 && k < n_difat; k++)
      {
        gsize at = 512 + (gsize) ds * sector_size;
        if (at + sector_size > size) break;
        for (guint i = 0; i + 1 < per_sector && fat_sectors->len < n_fat_sectors; i++)
          {
            guint32 s = rd32 (buf + at + i * 4);
            if (s >= FREESECT - 1) break;
            g_array_append_val (fat_sectors, s);
          }
        ds = rd32 (buf + at + (per_sector - 1) * 4);
      }

    fat = g_array_new (FALSE, FALSE, sizeof (guint32));
    for (guint i = 0; i < fat_sectors->len; i++)
      {
        gsize at = 512 + (gsize) g_array_index (fat_sectors, guint32, i) * sector_size;
        if (at + sector_size > size) break;
        for (guint j = 0; j < per_sector; j++)
          {
            guint32 e = rd32 (buf + at + j * 4);
            g_array_append_val (fat, e);
          }
      }
    g_array_unref (fat_sectors);
  }

  dir = read_chain (buf, size, sector_size, 512, (const guint32 *) fat->data, fat->len,
                    first_dir, G_MAXSIZE / 2);
  d = g_bytes_get_data (dir, &dir_len);

  /* The root entry's stream holds the mini sectors. */
  if (dir_len >= 128 && n_minifat > 0)
    {
      guint32 root_start = rd32 (d + 116);
      gsize root_size = rd32 (d + 120);
      mini_stream = read_chain (buf, size, sector_size, 512, (const guint32 *) fat->data, fat->len,
                                root_start, root_size);
      {
        GBytes *mf = read_chain (buf, size, sector_size, 512, (const guint32 *) fat->data, fat->len,
                                 first_minifat, (gsize) n_minifat * sector_size);
        gsize mf_len;
        const guchar *m = g_bytes_get_data (mf, &mf_len);
        minifat = g_array_new (FALSE, FALSE, sizeof (guint32));
        for (gsize i = 0; i + 4 <= mf_len; i += 4)
          {
            guint32 e = rd32 (m + i);
            g_array_append_val (minifat, e);
          }
        g_bytes_unref (mf);
      }
    }

  for (gsize at = 0; at + 128 <= dir_len; at += 128)
    {
      const guchar *e = d + at;
      guint type = e[66];
      guint name_len = rd16 (e + 64);
      char *ename;
      gboolean match;

      if (type != 2 || name_len < 2 || name_len > 64)
        continue;
      ename = g_utf16_to_utf8 ((const gunichar2 *) e, name_len / 2 - 1, NULL, NULL, NULL);
      match = ename != NULL && g_ascii_strcasecmp (ename, name) == 0;
      g_free (ename);
      if (!match)
        continue;

      {
        guint32 start = rd32 (e + 116);
        gsize stream_size = rd32 (e + 120);

        if (stream_size < MINI_CUTOFF && mini_stream != NULL && minifat != NULL)
          result = read_chain (g_bytes_get_data (mini_stream, NULL), g_bytes_get_size (mini_stream),
                               mini_size, 0, (const guint32 *) minifat->data, minifat->len,
                               start, stream_size);
        else
          result = read_chain (buf, size, sector_size, 512, (const guint32 *) fat->data, fat->len,
                               start, stream_size);
      }
      break;
    }

  if (result == NULL)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No \"%s\" stream in the file", name);
  g_clear_pointer (&minifat, g_array_unref);
  g_clear_pointer (&mini_stream, g_bytes_unref);
  g_bytes_unref (dir);
  g_array_unref (fat);
  return result;
}

/* ---- writing ---- */

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

static void
pad_to (GByteArray *a, gsize unit)
{
  while (a->len % unit != 0)
    {
      guchar zero = 0;
      g_byte_array_append (a, &zero, 1);
    }
}

/* A directory entry: name as UTF-16 in 64 bytes, then the fields. */
static void
put_dir_entry (GByteArray *a, const char *name, guint type, guint32 left, guint32 right,
               guint32 child, guint32 start, guint32 size)
{
  glong n = 0;
  gunichar2 *u = g_utf8_to_utf16 (name, -1, NULL, &n, NULL);
  guchar buf[64] = { 0 };

  if (n > 31) n = 31;
  for (glong i = 0; i < n; i++)
    {
      buf[i * 2] = u[i] & 0xff;
      buf[i * 2 + 1] = u[i] >> 8;
    }
  g_free (u);
  g_byte_array_append (a, buf, 64);
  put16 (a, (n + 1) * 2);
  {
    guchar tc[2] = { type, 1 };   /* colour: black */
    g_byte_array_append (a, tc, 2);
  }
  put32 (a, left);
  put32 (a, right);
  put32 (a, child);
  {
    guchar zeros[36] = { 0 };     /* CLSID, state bits, times */
    g_byte_array_append (a, zeros, 36);
  }
  put32 (a, start);
  put32 (a, size);
  put32 (a, 0);
}

GBytes *
o42_ole2_build (const char **names, GBytes **contents, int n)
{
  const gsize S = 512;
  GByteArray *mini = g_byte_array_new ();       /* the mini stream */
  GArray *minifat = g_array_new (FALSE, FALSE, sizeof (guint32));
  GByteArray *big = g_byte_array_new ();        /* regular-sector data, after FAT and dir */
  GArray *fat = g_array_new (FALSE, FALSE, sizeof (guint32));
  guint32 *stream_start = g_new0 (guint32, n);
  gsize *stream_size = g_new0 (gsize, n);
  GByteArray *dir = g_byte_array_new ();
  GByteArray *file;
  guint n_dir_entries = n + 1, n_dir_sectors, n_data_sectors, n_fat_sectors, n_mini_sectors, n_minifat_sectors;
  guint32 mini_start, minifat_start, dir_start, data_start;

  /* Small streams go to the mini stream, large ones to the big data
   * area.  Chains are laid out contiguously, so building the FATs is a
   * matter of counting. */
  for (int i = 0; i < n; i++)
    {
      gsize size = g_bytes_get_size (contents[i]);
      const guchar *data = g_bytes_get_data (contents[i], NULL);
      stream_size[i] = size;
      if (size < MINI_CUTOFF)
        {
          guint first = mini->len / MINI_SECTOR;
          guint count = (size + MINI_SECTOR - 1) / MINI_SECTOR;
          stream_start[i] = first;
          g_byte_array_append (mini, data, size);
          pad_to (mini, MINI_SECTOR);
          for (guint k = 0; k < count; k++)
            {
              guint32 next = k + 1 < count ? first + k + 1 : ENDOFCHAIN;
              g_array_append_val (minifat, next);
            }
        }
      else
        {
          stream_start[i] = big->len / S;   /* relative to data_start for now */
          g_byte_array_append (big, data, size);
          pad_to (big, S);
        }
    }

  n_mini_sectors = mini->len / S + (mini->len % S ? 1 : 0);
  pad_to (mini, S);
  n_minifat_sectors = (minifat->len * 4 + S - 1) / S;
  n_dir_sectors = (n_dir_entries * 128 + S - 1) / S;
  n_data_sectors = big->len / S;

  /* Sector layout: FAT sectors, directory, mini-FAT, mini stream, data.
   * The FAT must describe itself, so its size is solved by iteration. */
  n_fat_sectors = 1;
  for (;;)
    {
      guint total = n_fat_sectors + n_dir_sectors + n_minifat_sectors + n_mini_sectors + n_data_sectors;
      guint need = (total * 4 + S - 1) / S;
      if (need <= n_fat_sectors) break;
      n_fat_sectors = need;
    }
  dir_start = n_fat_sectors;
  minifat_start = dir_start + n_dir_sectors;
  mini_start = minifat_start + n_minifat_sectors;
  data_start = mini_start + n_mini_sectors;

  {
    guint32 v;
    for (guint i = 0; i < n_fat_sectors; i++) { v = 0xFFFFFFFD; g_array_append_val (fat, v); }   /* FATSECT */
    for (guint i = 0; i < n_dir_sectors; i++) { v = i + 1 < n_dir_sectors ? dir_start + i + 1 : ENDOFCHAIN; g_array_append_val (fat, v); }
    for (guint i = 0; i < n_minifat_sectors; i++) { v = i + 1 < n_minifat_sectors ? minifat_start + i + 1 : ENDOFCHAIN; g_array_append_val (fat, v); }
    for (guint i = 0; i < n_mini_sectors; i++) { v = i + 1 < n_mini_sectors ? mini_start + i + 1 : ENDOFCHAIN; g_array_append_val (fat, v); }
    for (int i = 0; i < n; i++)
      if (stream_size[i] >= MINI_CUTOFF)
        {
          guint count = (stream_size[i] + S - 1) / S;
          guint first = data_start + stream_start[i];
          stream_start[i] = first;
          for (guint k = 0; k < count; k++)
            { v = k + 1 < count ? first + k + 1 : ENDOFCHAIN; g_array_append_val (fat, v); }
        }
    while (fat->len < n_fat_sectors * (S / 4))
      { v = FREESECT; g_array_append_val (fat, v); }
  }

  /* The directory: root, then the streams as a chain of siblings. */
  put_dir_entry (dir, "Root Entry", 5, NOSTREAM, NOSTREAM, n > 0 ? 1 : NOSTREAM,
                 n_mini_sectors > 0 ? mini_start : ENDOFCHAIN, mini->len);
  for (int i = 0; i < n; i++)
    put_dir_entry (dir, names[i], 2, NOSTREAM, i + 1 < n ? (guint32) (i + 2) : NOSTREAM, NOSTREAM,
                   stream_start[i], stream_size[i]);
  while (dir->len < n_dir_sectors * S)
    put_dir_entry (dir, "", 0, NOSTREAM, NOSTREAM, NOSTREAM, 0, 0);

  file = g_byte_array_new ();
  g_byte_array_append (file, SIGNATURE, 8);
  { guchar clsid[16] = { 0 }; g_byte_array_append (file, clsid, 16); }
  put16 (file, 0x003E);          /* minor version */
  put16 (file, 0x0003);          /* major version */
  put16 (file, 0xFFFE);          /* byte order */
  put16 (file, 9);               /* sector shift */
  put16 (file, 6);               /* mini sector shift */
  { guchar z[6] = { 0 }; g_byte_array_append (file, z, 6); }
  put32 (file, 0);               /* directory sectors (v4 only) */
  put32 (file, n_fat_sectors);
  put32 (file, dir_start);
  put32 (file, 0);               /* transaction signature */
  put32 (file, MINI_CUTOFF);
  put32 (file, n_minifat_sectors > 0 ? minifat_start : ENDOFCHAIN);
  put32 (file, n_minifat_sectors);
  put32 (file, ENDOFCHAIN);      /* first DIFAT sector: none */
  put32 (file, 0);
  for (guint i = 0; i < 109; i++)
    put32 (file, i < n_fat_sectors ? i : FREESECT);

  for (guint i = 0; i < fat->len; i++)
    put32 (file, g_array_index (fat, guint32, i));
  g_byte_array_append (file, dir->data, dir->len);
  for (guint i = 0; i < minifat->len; i++)
    put32 (file, g_array_index (minifat, guint32, i));
  pad_to (file, S);
  g_byte_array_append (file, mini->data, mini->len);
  g_byte_array_append (file, big->data, big->len);

  g_byte_array_unref (dir);
  g_byte_array_unref (mini);
  g_byte_array_unref (big);
  g_array_unref (minifat);
  g_array_unref (fat);
  g_free (stream_start);
  g_free (stream_size);
  return g_byte_array_free_to_bytes (file);
}
