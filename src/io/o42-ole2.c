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
 * sector numbers in order.
 *
 * A sector cannot honestly be in a chain twice, and a file that says it
 * is would have the chain go round its loop until it had as many
 * sectors as the FAT has entries -- which a small file can make very
 * many of, by naming the same FAT sector in every one of its DIFAT
 * slots.  Ten megabytes of such a file asked for seven of stream and a
 * hundred would have asked for gigabytes, so each sector is taken once
 * and the loop ends where it closes. */
static GArray *
chain (const guint32 *fat, gsize n_fat, guint32 start)
{
  GArray *sectors = g_array_new (FALSE, FALSE, sizeof (guint32));
  guchar *seen = g_malloc0 (n_fat / 8 + 1);
  guint32 s = start;

  while (s < n_fat && !(seen[s / 8] & (1 << (s % 8))))
    {
      seen[s / 8] |= 1 << (s % 8);
      g_array_append_val (sectors, s);
      s = fat[s];
    }
  g_free (seen);
  return sectors;
}

/* Whether `take` bytes of sector `s` lie within `len` bytes that begin
 * with a header of `header` bytes, and if so where they start.  Found
 * by division: a sector number from the file times the sector size can
 * wrap round where gsize is 32 bits, and a wrapped offset passes any
 * comparison made by adding. */
static gboolean
sector_fits (gsize len, gsize header, guint32 s, gsize sector_size,
             gsize take, gsize *at)
{
  if (len < header || s > (len - header) / sector_size)
    return FALSE;
  *at = header + (gsize) s * sector_size;
  return take <= len - *at;
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
  /* `size` comes from the file and can say anything; the chain cannot
   * hold more than the file does, so that is all that is reserved. */
  GByteArray *out = g_byte_array_sized_new ((guint) MIN (size, base_len));

  for (guint i = 0; i < sectors->len && out->len < size; i++)
    {
      gsize take = MIN (sector_size, size - out->len);
      gsize at;
      if (!sector_fits (base_len, header, g_array_index (sectors, guint32, i),
                        sector_size, take, &at))
        break;
      g_byte_array_append (out, base + at, take);
    }
  g_array_unref (sectors);
  return g_byte_array_free_to_bytes (out);
}

/* A compound file opened for reading: its FATs, directory and mini
 * stream, found once and asked of for as many streams as are wanted. */
typedef struct {
  const guchar *buf;
  gsize         size;
  gsize         sector_size, mini_size;
  GArray       *fat;          /* guint32 */
  GArray       *minifat;      /* guint32, or NULL */
  GBytes       *dir;
  GBytes       *mini_stream;  /* or NULL */
  const guchar *d;            /* the directory's bytes */
  gsize         n_entries;
} Ole2;

static void
ole2_close (Ole2 *o)
{
  g_clear_pointer (&o->minifat, g_array_unref);
  g_clear_pointer (&o->mini_stream, g_bytes_unref);
  g_clear_pointer (&o->dir, g_bytes_unref);
  g_clear_pointer (&o->fat, g_array_unref);
}

static gboolean
ole2_open (Ole2 *o, GBytes *file, GError **error)
{
  gsize size;
  const guchar *buf = g_bytes_get_data (file, &size);
  guint sector_shift, mini_shift;
  gsize sector_size, dir_len;
  guint32 n_fat_sectors, first_dir, first_minifat, n_minifat, first_difat, n_difat;

  memset (o, 0, sizeof *o);
  if (!o42_ole2_is_compound (file))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Not an OLE2 compound file");
      return FALSE;
    }
  sector_shift = rd16 (buf + 0x1E);
  mini_shift = rd16 (buf + 0x20);
  if (sector_shift < 7 || sector_shift > 12 || mini_shift > sector_shift)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Bad compound file header");
      return FALSE;
    }
  o->buf = buf;
  o->size = size;
  o->sector_size = sector_size = (gsize) 1 << sector_shift;
  o->mini_size = (gsize) 1 << mini_shift;
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
    /* Both counts come from the header; a file that lies about them
     * would have the loop spin for as long as it liked, so the file's
     * own size bounds them too. */
    if (n_fat_sectors > size / sector_size)
      n_fat_sectors = (guint32) (size / sector_size);
    if (n_difat > size / sector_size)
      n_difat = (guint32) (size / sector_size);
    for (guint32 ds = first_difat, k = 0; ds < FREESECT - 1 && k < n_difat; k++)
      {
        gsize at;
        if (!sector_fits (size, 512, ds, sector_size, sector_size, &at)) break;
        for (guint i = 0; i + 1 < per_sector && fat_sectors->len < n_fat_sectors; i++)
          {
            guint32 s = rd32 (buf + at + i * 4);
            if (s >= FREESECT - 1) break;
            g_array_append_val (fat_sectors, s);
          }
        ds = rd32 (buf + at + (per_sector - 1) * 4);
      }

    o->fat = g_array_new (FALSE, FALSE, sizeof (guint32));
    for (guint i = 0; i < fat_sectors->len; i++)
      {
        gsize at;
        if (!sector_fits (size, 512, g_array_index (fat_sectors, guint32, i),
                          sector_size, sector_size, &at))
          break;
        for (guint j = 0; j < per_sector; j++)
          {
            guint32 e = rd32 (buf + at + j * 4);
            g_array_append_val (o->fat, e);
          }
      }
    g_array_unref (fat_sectors);
  }

  o->dir = read_chain (buf, size, sector_size, 512, (const guint32 *) o->fat->data, o->fat->len,
                       first_dir, G_MAXSIZE / 2);
  o->d = g_bytes_get_data (o->dir, &dir_len);
  o->n_entries = dir_len / 128;

  /* The root entry's stream holds the mini sectors. */
  if (o->n_entries > 0 && n_minifat > 0)
    {
      guint32 root_start = rd32 (o->d + 116);
      gsize root_size = rd32 (o->d + 120);
      GBytes *mf;
      gsize mf_len;
      const guchar *m;

      o->mini_stream = read_chain (buf, size, sector_size, 512, (const guint32 *) o->fat->data,
                                   o->fat->len, root_start, root_size);
      mf = read_chain (buf, size, sector_size, 512, (const guint32 *) o->fat->data, o->fat->len,
                       first_minifat, (gsize) n_minifat * sector_size);
      m = g_bytes_get_data (mf, &mf_len);
      o->minifat = g_array_new (FALSE, FALSE, sizeof (guint32));
      for (gsize i = 0; i + 4 <= mf_len; i += 4)
        {
          guint32 e = rd32 (m + i);
          g_array_append_val (o->minifat, e);
        }
      g_bytes_unref (mf);
    }
  return TRUE;
}

/* The name of directory entry `i`, as UTF-8 (caller frees), or NULL. */
static char *
ole2_entry_name (const Ole2 *o, guint32 i)
{
  const guchar *e;
  guint name_len;
  gunichar2 units[32];

  if (i >= o->n_entries)
    return NULL;
  e = o->d + (gsize) i * 128;
  name_len = rd16 (e + 64);
  if (name_len < 2 || name_len > 64)
    return NULL;
  /* Read unit by unit: the directory's bytes need not be aligned for
   * gunichar2, and the file is little-endian whatever the host is. */
  for (guint k = 0; k < name_len / 2 - 1; k++)
    units[k] = rd16 (e + k * 2);
  return g_utf16_to_utf8 (units, name_len / 2 - 1, NULL, NULL, NULL);
}

static guint
ole2_entry_type (const Ole2 *o, guint32 i)
{
  return i < o->n_entries ? o->d[(gsize) i * 128 + 66] : 0;
}

static GBytes *
ole2_entry_data (const Ole2 *o, guint32 i)
{
  const guchar *e = o->d + (gsize) i * 128;
  guint32 start = rd32 (e + 116);
  gsize stream_size = rd32 (e + 120);

  if (stream_size < MINI_CUTOFF && o->mini_stream != NULL && o->minifat != NULL)
    return read_chain (g_bytes_get_data (o->mini_stream, NULL), g_bytes_get_size (o->mini_stream),
                       o->mini_size, 0, (const guint32 *) o->minifat->data, o->minifat->len,
                       start, stream_size);
  return read_chain (o->buf, o->size, o->sector_size, 512, (const guint32 *) o->fat->data,
                     o->fat->len, start, stream_size);
}

/* The entries that are children of storage `parent`, found by walking
 * the tree its child pointer roots; each entry is taken once, so that a
 * directory whose pointers go round in a circle still ends. */
static void
ole2_collect (const Ole2 *o, guint32 node, guchar *seen, GArray *out, int depth)
{
  const guchar *e;

  if (node >= o->n_entries || seen[node] || depth > 4096)
    return;
  seen[node] = 1;
  e = o->d + (gsize) node * 128;
  ole2_collect (o, rd32 (e + 68), seen, out, depth + 1);
  g_array_append_val (out, node);
  ole2_collect (o, rd32 (e + 72), seen, out, depth + 1);
}

static GArray *
ole2_children (const Ole2 *o, guint32 parent, guchar *seen)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (guint32));

  if (parent < o->n_entries)
    ole2_collect (o, rd32 (o->d + (gsize) parent * 128 + 76), seen, out, 0);
  return out;
}

GBytes *
o42_ole2_read_stream (GBytes *file, const char *name, GError **error)
{
  Ole2 o;
  GBytes *result = NULL;

  if (!ole2_open (&o, file, error))
    return NULL;
  for (guint32 i = 0; i < o.n_entries && result == NULL; i++)
    {
      char *ename;

      if (ole2_entry_type (&o, i) != 2)
        continue;
      ename = ole2_entry_name (&o, i);
      if (ename != NULL && g_ascii_strcasecmp (ename, name) == 0)
        result = ole2_entry_data (&o, i);
      g_free (ename);
    }
  if (result == NULL)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No \"%s\" stream in the file", name);
  ole2_close (&o);
  return result;
}

/* The entry of storage `parent` called `name`, of type `type` (1 a
 * storage, 2 a stream), or NOSTREAM. */
static guint32
ole2_find_child (const Ole2 *o, guint32 parent, const char *name, guint type)
{
  guchar *seen = g_malloc0 (o->n_entries + 1);
  GArray *kids = ole2_children (o, parent, seen);
  guint32 found = NOSTREAM;
  char *want = g_utf8_casefold (name, -1);

  /* Names are compared as the format compares them, without regard to
   * case: Excel writes "VBA" and "dir", and finds "vba" and "DIR". */
  for (guint k = 0; k < kids->len && found == NOSTREAM; k++)
    {
      guint32 i = g_array_index (kids, guint32, k);
      char *ename = ole2_entry_type (o, i) == type ? ole2_entry_name (o, i) : NULL;
      char *have = ename != NULL ? g_utf8_casefold (ename, -1) : NULL;

      if (have != NULL && strcmp (have, want) == 0)
        found = i;
      g_free (have);
      g_free (ename);
    }
  g_free (want);
  g_array_unref (kids);
  g_free (seen);
  return found;
}

GBytes *
o42_ole2_read_path (GBytes *file, const char *path, GError **error)
{
  Ole2 o;
  char **parts;
  guint32 at = 0;
  GBytes *result = NULL;

  if (!ole2_open (&o, file, error))
    return NULL;
  parts = g_strsplit (path, "/", -1);
  for (int k = 0; parts[k] != NULL && at != NOSTREAM; k++)
    {
      gboolean last = parts[k + 1] == NULL;
      if (*parts[k] == '\0')
        continue;
      at = ole2_find_child (&o, at, parts[k], last ? 2 : 1);
      if (last && at != NOSTREAM)
        result = ole2_entry_data (&o, at);
    }
  if (result == NULL)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No \"%s\" stream in the file", path);
  g_strfreev (parts);
  ole2_close (&o);
  return result;
}

static void
ole2_list_under (const Ole2 *o, guint32 storage, const char *prefix, guchar *seen, GPtrArray *out)
{
  GArray *kids = ole2_children (o, storage, seen);

  for (guint k = 0; k < kids->len; k++)
    {
      guint32 i = g_array_index (kids, guint32, k);
      char *ename = ole2_entry_name (o, i);
      char *path;

      if (ename == NULL)
        continue;
      path = *prefix != '\0' ? g_strconcat (prefix, "/", ename, NULL) : g_strdup (ename);
      if (ole2_entry_type (o, i) == 2)
        g_ptr_array_add (out, g_strdup (path));
      else if (ole2_entry_type (o, i) == 1)
        ole2_list_under (o, i, path, seen, out);
      g_free (path);
      g_free (ename);
    }
  g_array_unref (kids);
}

GPtrArray *
o42_ole2_list (GBytes *file, const char *storage)
{
  Ole2 o;
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  guint32 at = 0;
  guchar *seen;

  if (!ole2_open (&o, file, NULL))
    return out;
  seen = g_malloc0 (o.n_entries + 1);
  if (storage != NULL && *storage != '\0')
    {
      char **parts = g_strsplit (storage, "/", -1);
      for (int k = 0; parts[k] != NULL && at != NOSTREAM; k++)
        if (*parts[k] != '\0')
          at = ole2_find_child (&o, at, parts[k], 1);
      g_strfreev (parts);
    }
  if (at != NOSTREAM)
    ole2_list_under (&o, at, "", seen, out);
  g_free (seen);
  ole2_close (&o);
  return out;
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

/* A directory entry being laid out: the root, a storage or a stream. */
typedef struct {
  char      *name;
  guint      type;        /* 5 the root, 1 a storage, 2 a stream */
  int        stream;      /* which of the streams, for a stream */
  GPtrArray *children;    /* DirNode *, for the root and a storage */
  guint32    id, left, right, child;
} DirNode;

static DirNode *
dir_node_new (const char *name, guint type, int stream)
{
  DirNode *node = g_new0 (DirNode, 1);

  node->name = g_strdup (name);
  node->type = type;
  node->stream = stream;
  node->children = g_ptr_array_new ();
  node->left = node->right = node->child = NOSTREAM;
  return node;
}

static void
dir_node_free (DirNode *node)
{
  for (guint i = 0; i < node->children->len; i++)
    dir_node_free (g_ptr_array_index (node->children, i));
  g_ptr_array_unref (node->children);
  g_free (node->name);
  g_free (node);
}

static DirNode *
dir_node_storage (DirNode *parent, const char *name)
{
  for (guint i = 0; i < parent->children->len; i++)
    {
      DirNode *child = g_ptr_array_index (parent->children, i);
      if (child->type == 1 && strcmp (child->name, name) == 0)
        return child;
    }
  g_ptr_array_add (parent->children, dir_node_new (name, 1, -1));
  return g_ptr_array_index (parent->children, parent->children->len - 1);
}

/* How many storages the paths name, each counted once. */
static guint
count_storages (const char **names, int n)
{
  GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  guint count;

  for (int i = 0; i < n; i++)
    {
      const char *slash = names[i];
      while ((slash = strchr (slash, '/')) != NULL)
        {
          g_hash_table_add (seen, g_strndup (names[i], slash - names[i]));
          slash++;
        }
    }
  count = g_hash_table_size (seen);
  g_hash_table_unref (seen);
  return count;
}

/* The order the format keeps siblings in: a shorter name first, and
 * names of one length by their letters upper-cased. */
static int
dir_node_compare (gconstpointer pa, gconstpointer pb)
{
  const DirNode *a = *(DirNode * const *) pa, *b = *(DirNode * const *) pb;
  glong la = 0, lb = 0;
  gunichar2 *ua = g_utf8_to_utf16 (a->name, -1, NULL, &la, NULL);
  gunichar2 *ub = g_utf8_to_utf16 (b->name, -1, NULL, &lb, NULL);
  int result = la < lb ? -1 : la > lb ? 1 : 0;

  for (glong i = 0; result == 0 && i < la; i++)
    {
      gunichar ca = g_unichar_toupper (ua[i]), cb = g_unichar_toupper (ub[i]);
      result = ca < cb ? -1 : ca > cb ? 1 : 0;
    }
  g_free (ua);
  g_free (ub);
  return result;
}

static void
dir_node_number (DirNode *node, guint32 *next, GPtrArray *order)
{
  node->id = (*next)++;
  g_ptr_array_add (order, node);
  for (guint i = 0; i < node->children->len; i++)
    dir_node_number (g_ptr_array_index (node->children, i), next, order);
}

/* Siblings from `lo` to `hi` as a balanced binary tree; its root. */
static guint32
dir_node_tree (GPtrArray *sorted, int lo, int hi)
{
  int mid;
  DirNode *node;

  if (lo > hi)
    return NOSTREAM;
  mid = (lo + hi) / 2;
  node = g_ptr_array_index (sorted, mid);
  node->left = dir_node_tree (sorted, lo, mid - 1);
  node->right = dir_node_tree (sorted, mid + 1, hi);
  return node->id;
}

static void
dir_node_link (DirNode *node)
{
  if (node->children->len == 0)
    return;
  {
    GPtrArray *sorted = g_ptr_array_copy (node->children, NULL, NULL);

    g_ptr_array_sort (sorted, dir_node_compare);
    node->child = dir_node_tree (sorted, 0, (int) sorted->len - 1);
    g_ptr_array_unref (sorted);
  }
  for (guint i = 0; i < node->children->len; i++)
    dir_node_link (g_ptr_array_index (node->children, i));
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
  guint n_dir_entries = 1 + n + count_storages (names, n), n_dir_sectors, n_data_sectors, n_fat_sectors, n_mini_sectors, n_minifat_sectors;
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

  /* The directory: the root, the storages the names' paths ask for,
   * and the streams, each storage's children a binary tree in the
   * order the format sorts names in. */
  {
    DirNode *root = dir_node_new ("Root Entry", 5, -1);
    GPtrArray *order = g_ptr_array_new ();
    guint32 next = 0;

    for (int i = 0; i < n; i++)
      {
        char **parts = g_strsplit (names[i], "/", -1);
        DirNode *at = root;
        int k;

        for (k = 0; parts[k] != NULL && parts[k + 1] != NULL; k++)
          if (*parts[k] != '\0')
            at = dir_node_storage (at, parts[k]);
        g_ptr_array_add (at->children, dir_node_new (parts[k] != NULL ? parts[k] : "", 2, i));
        g_strfreev (parts);
      }
    dir_node_number (root, &next, order);
    dir_node_link (root);
    for (guint i = 0; i < order->len; i++)
      {
        DirNode *node = g_ptr_array_index (order, i);

        if (node->type == 5)
          put_dir_entry (dir, node->name, 5, NOSTREAM, NOSTREAM, node->child,
                         n_mini_sectors > 0 ? mini_start : ENDOFCHAIN, mini->len);
        else if (node->type == 1)
          put_dir_entry (dir, node->name, 1, node->left, node->right, node->child, 0, 0);
        else
          put_dir_entry (dir, node->name, 2, node->left, node->right, NOSTREAM,
                         stream_start[node->stream], stream_size[node->stream]);
      }
    g_ptr_array_unref (order);
    dir_node_free (root);
  }
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
