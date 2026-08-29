/* o42-escher.c - the Office Drawing records inside .xls
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-escher.h"

#include <string.h>

/* Record types */
enum {
  ESC_DGG_CONTAINER = 0xF000, ESC_BSTORE_CONTAINER = 0xF001, ESC_DG_CONTAINER = 0xF002,
  ESC_SPGR_CONTAINER = 0xF003, ESC_SP_CONTAINER = 0xF004, ESC_DGG = 0xF006, ESC_BSE = 0xF007,
  ESC_DG = 0xF008, ESC_SPGR = 0xF009, ESC_SP = 0xF00A, ESC_OPT = 0xF00B,
  ESC_CLIENT_ANCHOR = 0xF010, ESC_CLIENT_DATA = 0xF011, ESC_SPLIT_MENU = 0xF11E,
  ESC_BLIP_JPEG = 0xF01D, ESC_BLIP_PNG = 0xF01E, ESC_BLIP_DIB = 0xF01F
};

static guint16 rd16 (const guchar *p) { return p[0] | (p[1] << 8); }
static guint32 rd32 (const guchar *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((guint32) p[3] << 24); }
static void put16 (GByteArray *a, guint v) { guchar b[2] = { v & 0xff, (v >> 8) & 0xff }; g_byte_array_append (a, b, 2); }
static void put32 (GByteArray *a, guint32 v) { guchar b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff }; g_byte_array_append (a, b, 4); }

/* An Escher record header: version and instance in one word, the type,
 * the length of what follows. */
static void
header (GByteArray *a, guint ver, guint inst, guint type, guint32 len)
{
  put16 (a, (ver & 0x0F) | (inst << 4));
  put16 (a, type);
  put32 (a, len);
}

/* Patches a container's length once its children are in. */
static void
fix_length (GByteArray *a, gsize header_at)
{
  guint32 len = a->len - header_at - 8;
  a->data[header_at + 4] = len & 0xff;
  a->data[header_at + 5] = (len >> 8) & 0xff;
  a->data[header_at + 6] = (len >> 16) & 0xff;
  a->data[header_at + 7] = (len >> 24) & 0xff;
}

/* ====================================================================== */
/* Writing                                                                 */
/* ====================================================================== */

GBytes *
o42_escher_group (GPtrArray *images, GPtrArray *formats, GArray *shapes_per_drawing)
{
  GByteArray *a = g_byte_array_new ();
  gsize dgg_at = a->len, bstore_at;
  guint n_drawings = shapes_per_drawing->len;
  guint total_shapes = 0;

  for (guint i = 0; i < n_drawings; i++)
    total_shapes += g_array_index (shapes_per_drawing, int, i) + 1;   /* + the patriarch */

  header (a, 15, 0, ESC_DGG_CONTAINER, 0);

  /* Dgg: the shape id clusters, one of 1024 ids per drawing. */
  header (a, 0, 0, ESC_DGG, 16 + 8 * n_drawings);
  put32 (a, (n_drawings + 1) * 1024);          /* spidMax */
  put32 (a, n_drawings + 1);                    /* cidcl */
  put32 (a, total_shapes);                      /* cspSaved */
  put32 (a, n_drawings);                        /* cdgSaved */
  for (guint i = 0; i < n_drawings; i++)
    {
      put32 (a, i + 1);                                                    /* dgid */
      put32 (a, g_array_index (shapes_per_drawing, int, i) + 2);           /* cspidCur */
    }

  if (images->len > 0)
    {
      bstore_at = a->len;
      header (a, 15, images->len, ESC_BSTORE_CONTAINER, 0);
      for (guint i = 0; i < images->len; i++)
        {
          GBytes *img = g_ptr_array_index (images, i);
          const char *fmt = g_ptr_array_index (formats, i);
          gboolean jpeg = strcmp (fmt, "jpeg") == 0 || strcmp (fmt, "jpg") == 0;
          guint type = jpeg ? 5 : 6;   /* msoblipJPEG, msoblipPNG */
          gsize size = g_bytes_get_size (img);
          guchar uid[16];
          gsize bse_at = a->len;

          /* The blip's id is a digest of its bytes, as Office makes it. */
          {
            GChecksum *sum = g_checksum_new (G_CHECKSUM_MD5);
            gsize n = 16;
            g_checksum_update (sum, g_bytes_get_data (img, NULL), size);
            g_checksum_get_digest (sum, uid, &n);
            g_checksum_free (sum);
          }

          header (a, 2, type, ESC_BSE, 0);
          {
            guchar bt[2] = { type, type };
            g_byte_array_append (a, bt, 2);
          }
          g_byte_array_append (a, uid, 16);
          put16 (a, 0xFF);                       /* tag */
          put32 (a, 8 + 16 + 1 + size);          /* size of the blip record */
          put32 (a, 1);                          /* cRef */
          put32 (a, 0);                          /* foDelay */
          {
            guchar rest[4] = { 0, 0, 0, 0 };     /* usage, cbName, unused */
            g_byte_array_append (a, rest, 4);
          }
          header (a, 0, jpeg ? 0x46A : 0x6E0, jpeg ? ESC_BLIP_JPEG : ESC_BLIP_PNG, 16 + 1 + size);
          g_byte_array_append (a, uid, 16);
          {
            guchar tag = 0xFF;
            g_byte_array_append (a, &tag, 1);
          }
          g_byte_array_append (a, g_bytes_get_data (img, NULL), size);
          fix_length (a, bse_at);
        }
      fix_length (a, bstore_at);
    }

  /* The default drawing properties Excel writes. */
  header (a, 3, 3, ESC_OPT, 18);
  put16 (a, 0x00BF); put32 (a, 0x00080008);
  put16 (a, 0x0181); put32 (a, 0x08000041);
  put16 (a, 0x01C0); put32 (a, 0x08000040);
  header (a, 0, 0, ESC_SPLIT_MENU, 16);
  put32 (a, 0x0800000D); put32 (a, 0x0800000C); put32 (a, 0x08000017); put32 (a, 0x100000F7);

  fix_length (a, dgg_at);
  return g_byte_array_free_to_bytes (a);
}

/* Cell anchors count fractions in 1024ths of a column and 256ths of a
 * row. */
static void
put_anchor (GByteArray *a, const O42EscherShape *s)
{
  header (a, 0, 0, ESC_CLIENT_ANCHOR, 18);
  put16 (a, s->is_note ? 3 : 2);   /* 2: move with cells, size fixed; 3: notes as Excel writes them */
  put16 (a, s->col1); put16 (a, (guint) (CLAMP (s->dx1, 0, 1) * 1024));
  put16 (a, s->row1); put16 (a, (guint) (CLAMP (s->dy1, 0, 1) * 256));
  put16 (a, s->col2); put16 (a, (guint) (CLAMP (s->dx2, 0, 1) * 1024));
  put16 (a, s->row2); put16 (a, (guint) (CLAMP (s->dy2, 0, 1) * 256));
}

GPtrArray *
o42_escher_drawing (int drawing_id, GArray *shapes)
{
  GPtrArray *chunks = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  guint32 base = drawing_id * 1024;
  guint32 total = 0;

  for (guint i = 0; i < shapes->len; i++)
    {
      GByteArray *a = g_byte_array_new ();
      const O42EscherShape *s = &g_array_index (shapes, O42EscherShape, i);
      gsize sp_at;

      if (i == 0)
        {
          /* The drawing's headers, with lengths that cover every shape
           * to come: the containers are logically one record, cut by
           * the OBJ records between the shapes. */
          /* Lengths are patched below from the chunks themselves. */
          header (a, 15, 0, ESC_DG_CONTAINER, 0);
          header (a, 0, drawing_id, ESC_DG, 8);
          put32 (a, shapes->len + 1);     /* shapes, the patriarch included */
          put32 (a, base + shapes->len);  /* last spid used */
          header (a, 15, 0, ESC_SPGR_CONTAINER, 0);
          header (a, 15, 0, ESC_SP_CONTAINER, 8 + 16 + 8 + 8);
          header (a, 1, 0, ESC_SPGR, 16);
          put32 (a, 0); put32 (a, 0); put32 (a, 0); put32 (a, 0);
          header (a, 2, 0, ESC_SP, 8);
          put32 (a, base);
          put32 (a, 0x0005);              /* group, patriarch */
        }

      sp_at = a->len;
      header (a, 15, 0, ESC_SP_CONTAINER, 0);
      header (a, 2, s->is_note ? 202 : s->is_chart ? 201 : 75, ESC_SP, 8);   /* text box, host control, picture frame */
      put32 (a, base + 1 + i);
      put32 (a, 0x0A00);                  /* has anchor, has shape type */
      if (s->is_chart)
        {
          header (a, 3, 9, ESC_OPT, 9 * 6);
          put16 (a, 0x007F); put32 (a, 0x01040104);
          put16 (a, 0x00BF); put32 (a, 0x00080008);
          put16 (a, 0x0181); put32 (a, 0x0800004E);
          put16 (a, 0x0183); put32 (a, 0x0800004D);
          put16 (a, 0x01BF); put32 (a, 0x00110010);
          put16 (a, 0x01C0); put32 (a, 0x0800004D);
          put16 (a, 0x01FF); put32 (a, 0x00080008);
          put16 (a, 0x023F); put32 (a, 0x00020000);
          put16 (a, 0x03BF); put32 (a, 0x00080000);
        }
      else if (s->is_note)
        {
          header (a, 3, 7, ESC_OPT, 7 * 6);
          put16 (a, 0x0080); put32 (a, (i + 1) << 16);         /* lTxid */
          put16 (a, 0x0158); put32 (a, 0);                     /* fFitTextToShape etc. */
          put16 (a, 0x0181); put32 (a, 0x08000050);            /* fill: note yellow */
          put16 (a, 0x0183); put32 (a, 0x08000050);
          put16 (a, 0x01BF); put32 (a, 0x00110010);            /* fill on */
          put16 (a, 0x023F); put32 (a, 0x00030003);            /* shadow */
          put16 (a, 0x03BF); put32 (a, 0x000A0002);            /* hidden note, printable */
        }
      else
        {
          header (a, 3, 3, ESC_OPT, 3 * 6);
          put16 (a, 0x007F); put32 (a, 0x01000100);            /* lock aspect ratio */
          put16 (a, 0x4104); put32 (a, s->blip);               /* the picture */
          put16 (a, 0x01BF); put32 (a, 0x00110000);            /* no fill hit test */
        }
      put_anchor (a, s);
      header (a, 0, 0, ESC_CLIENT_DATA, 0);
      fix_length (a, sp_at);
      total += a->len - sp_at;

      g_ptr_array_add (chunks, g_byte_array_free_to_bytes (a));
    }

  /* Now the container lengths: SpgrContainer holds the patriarch and
   * every shape; DgContainer holds Dg and the SpgrContainer. */
  if (chunks->len > 0)
    {
      GBytes *first = g_ptr_array_index (chunks, 0);
      GByteArray *a = g_byte_array_new ();
      guint32 spgr_len, dg_len;
      const guint32 patriarch = 8 + (8 + 16 + 8 + 8);

      g_byte_array_append (a, g_bytes_get_data (first, NULL), g_bytes_get_size (first));
      spgr_len = patriarch + total;
      dg_len = (8 + 8) + 8 + spgr_len;
      a->data[4] = dg_len & 0xff; a->data[5] = (dg_len >> 8) & 0xff;
      a->data[6] = (dg_len >> 16) & 0xff; a->data[7] = (dg_len >> 24) & 0xff;
      {
        gsize spgr_at = 8 + 16;   /* after DgContainer header and Dg */
        a->data[spgr_at + 4] = spgr_len & 0xff; a->data[spgr_at + 5] = (spgr_len >> 8) & 0xff;
        a->data[spgr_at + 6] = (spgr_len >> 16) & 0xff; a->data[spgr_at + 7] = (spgr_len >> 24) & 0xff;
      }
      g_ptr_array_index (chunks, 0) = g_byte_array_free_to_bytes (a);
      g_bytes_unref (first);
    }
  return chunks;
}

/* ====================================================================== */
/* Reading                                                                 */
/* ====================================================================== */

void
o42_escher_parse_group (const guchar *data, gsize len, GPtrArray *images, GPtrArray *formats)
{
  /* Walk every record, descending into containers, and take each BSE's
   * embedded blip. */
  const guchar *p = data, *end = data + len;

  while (p + 8 <= end)
    {
      guint verinst = rd16 (p), type = rd16 (p + 2);
      guint32 rlen = rd32 (p + 4);
      guint ver = verinst & 0x0F;
      const guchar *body = p + 8;

      if (body + rlen > end)
        break;
      if (ver == 15)
        {
          p = body;   /* a container: its children follow directly */
          continue;
        }
      if (type == ESC_BSE && rlen >= 36)
        {
          /* The blip record sits after the 36-byte BSE header. */
          const guchar *b = body + 36;
          if (b + 8 <= body + rlen)
            {
              guint btype = rd16 (b + 2);
              guint binst = rd16 (b) >> 4;
              guint32 blen = rd32 (b + 4);
              const guchar *img = b + 8;
              gsize skip = 0;
              const char *fmt = NULL;

              if (btype == ESC_BLIP_PNG) { fmt = "png"; skip = (binst == 0x6E1 ? 32 : 16) + 1; }
              else if (btype == ESC_BLIP_JPEG) { fmt = "jpeg"; skip = (binst == 0x46B || binst == 0x6E3 ? 32 : 16) + 1; }
              else if (btype == ESC_BLIP_DIB) { fmt = "bmp"; skip = (binst == 0x7A9 ? 32 : 16) + 1; }
              if (fmt != NULL && img + skip <= body + rlen && blen >= skip && img + blen <= end)
                {
                  g_ptr_array_add (images, g_bytes_new (img + skip, blen - skip));
                  g_ptr_array_add (formats, (gpointer) g_intern_string (fmt));
                }
              else
                {
                  /* Keep the numbering: an image we cannot use. */
                  g_ptr_array_add (images, g_bytes_new (NULL, 0));
                  g_ptr_array_add (formats, (gpointer) g_intern_string ("unknown"));
                }
            }
        }
      p = body + rlen;
    }
}

void
o42_escher_parse_drawing (const guchar *data, gsize len, GArray *found)
{
  const guchar *p = data, *end = data + len;
  O42EscherFound cur;
  gboolean in_shape = FALSE;

  memset (&cur, 0, sizeof cur);
  while (p + 8 <= end)
    {
      guint verinst = rd16 (p), type = rd16 (p + 2);
      guint32 rlen = rd32 (p + 4);
      guint ver = verinst & 0x0F, inst = verinst >> 4;
      const guchar *body = p + 8;

      if (body + rlen > end)
        rlen = end - body;
      if (ver == 15)
        {
          if (type == ESC_SP_CONTAINER)
            {
              if (in_shape && cur.col2 >= 0)
                g_array_append_val (found, cur);
              memset (&cur, 0, sizeof cur);
              cur.col2 = -1;
              in_shape = TRUE;
            }
          p = body;
          continue;
        }
      if (type == ESC_SP && rlen >= 8)
        {
          cur.is_picture = inst == 75;
          cur.is_chart = inst == 201;
        }
      else if (type == ESC_OPT)
        {
          for (guint i = 0; i + 6 <= rlen; i += 6)
            {
              guint id = rd16 (body + i) & 0x3FFF;
              if (id == 0x0104)
                cur.blip = rd32 (body + i + 2);
            }
        }
      else if (type == ESC_CLIENT_ANCHOR && rlen >= 18)
        {
          cur.col1 = rd16 (body + 2); cur.dx1 = rd16 (body + 4) / 1024.0;
          cur.row1 = rd16 (body + 6); cur.dy1 = rd16 (body + 8) / 256.0;
          cur.col2 = rd16 (body + 10); cur.dx2 = rd16 (body + 12) / 1024.0;
          cur.row2 = rd16 (body + 14); cur.dy2 = rd16 (body + 16) / 256.0;
        }
      p = body + rlen;
    }
  if (in_shape && cur.col2 >= 0)
    g_array_append_val (found, cur);
}
