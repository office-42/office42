/* o42-escher.c - the Office Drawing records inside .xls
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-escher.h"

#include <math.h>
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

/* Escher turns a shape and then mirrors it, where office42 (and Office
 * Open XML) mirror first: with one flip on, the angle changes sign
 * between the two.  The angle goes in and out as 16.16 degrees. */
static guint32
escher_rotation (double degrees, gboolean flip_h, gboolean flip_v)
{
  if (flip_h != flip_v)
    degrees = -degrees;
  return (guint32) (gint32) (fmod (fmod (degrees, 360) + 360, 360) * 65536);
}

static double
rotation_from_escher (guint32 v, gboolean flip_h, gboolean flip_v)
{
  double degrees = (gint32) v / 65536.0;

  if (flip_h != flip_v)
    degrees = -degrees;
  return fmod (fmod (degrees, 360) + 360, 360);
}

/* Escher keeps a colour as 0x00BBGGRR. */
static guint32
escher_colour (guint32 rgb)
{
  return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}

/* The dash and head numbers Escher uses, from office42's. */
static const guint32 DASH_TO_ESCHER[O42_N_DASHES] = { 0, 6, 5, 8, 7, 1, 2 };

static O42Dash
dash_from_escher (guint32 v)
{
  switch (v)
    {
    case 1: return O42_DASH_SYS_DASH;
    case 2: return O42_DASH_SYS_DOT;
    case 3: case 4: return O42_DASH_DASH_DOT;
    case 5: return O42_DASH_DOT;
    case 6: return O42_DASH_DASH;
    case 7: return O42_DASH_LONG_DASH;
    case 8: case 9: case 10: return O42_DASH_DASH_DOT;
    default: return O42_DASH_SOLID;
    }
}

/* Escher numbers the heads as office42 does: none, triangle, stealth,
 * diamond, oval, open. */
static O42Head
head_from_escher (guint32 v)
{
  return v < O42_N_HEADS ? (O42Head) v : O42_HEAD_TRIANGLE;
}

/* The Opt record of a drawn shape: the properties in ascending order,
 * as the format wants them.  `txid` numbers the text, when there is
 * any, for the TXO that follows the OBJ. */
static void
put_drawing_opt (GByteArray *a, const O42Shape *sh, guint txid)
{
  gsize opt_at = a->len;
  guint n = 0;
  gboolean line_kind = sh->kind == O42_SHAPE_LINE || sh->kind == O42_SHAPE_ARROW;
  gboolean has_text = !line_kind && sh->text != NULL && *sh->text != '\0';

  header (a, 3, 0, ESC_OPT, 0);
  if (sh->rotation != 0)
    { put16 (a, 0x0004); put32 (a, escher_rotation (sh->rotation, sh->flip_h, sh->flip_v)); n++; }
  if (has_text)
    {
      put16 (a, 0x0080); put32 (a, txid << 16); n++;                      /* lTxid */
      put16 (a, 0x00BF); put32 (a, 0x00080008); n++;                      /* fFitTextToShape off, text on */
    }
  if (!line_kind && sh->fill != O42_FILL_NONE)
    { put16 (a, 0x0181); put32 (a, escher_colour (sh->fill)); n++; }     /* fillColor */
  put16 (a, 0x01BF); put32 (a, (!line_kind && sh->fill != O42_FILL_NONE) ? 0x00100010 : 0x00100000); n++;  /* fFilled */
  put16 (a, 0x01C0); put32 (a, escher_colour (sh->line)); n++;           /* lineColor */
  put16 (a, 0x01CB); put32 (a, (guint32) (sh->line_width * 9525)); n++;  /* lineWidth, EMU */
  if (sh->dash != O42_DASH_SOLID)
    { put16 (a, 0x01CE); put32 (a, DASH_TO_ESCHER[sh->dash]); n++; }     /* lineDashing */
  if (line_kind && sh->head_start != O42_HEAD_NONE)
    { put16 (a, 0x01D0); put32 (a, sh->head_start); n++; }                /* lineStartArrowhead */
  if (line_kind && sh->head_end != O42_HEAD_NONE)
    { put16 (a, 0x01D1); put32 (a, sh->head_end); n++; }                  /* lineEndArrowhead */
  if (line_kind && sh->head_start != O42_HEAD_NONE)
    {
      put16 (a, 0x01D2); put32 (a, sh->head_start_size); n++;             /* width: narrow, medium, wide */
      put16 (a, 0x01D3); put32 (a, sh->head_start_size); n++;             /* length: short, medium, long */
    }
  if (line_kind && sh->head_end != O42_HEAD_NONE)
    {
      put16 (a, 0x01D4); put32 (a, sh->head_end_size); n++;
      put16 (a, 0x01D5); put32 (a, sh->head_end_size); n++;
    }
  put16 (a, 0x01FF); put32 (a, 0x00080008); n++;                         /* fLine on */
  put16 (a, 0x03BF); put32 (a, 0x00080000); n++;                         /* not hidden, printable */
  /* The header's instance is the property count. */
  a->data[opt_at] = (3 & 0x0F) | ((n & 0x0F) << 4);
  a->data[opt_at + 1] = (n >> 4) & 0xFF;
  fix_length (a, opt_at);
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
      header (a, 2, s->drawing != NULL ? o42_shape_spt (s->drawing)
                    : s->is_note ? 202 : s->is_chart || s->is_control ? 201 : 75, ESC_SP, 8);   /* text box, host control, picture frame */
      put32 (a, base + 1 + i);
      put32 (a, 0x0A00 |                  /* has anchor, has shape type */
              ((s->drawing != NULL ? s->drawing->flip_h : s->flip_h) ? 0x40 : 0) |
              ((s->drawing != NULL ? s->drawing->flip_v : s->flip_v) ? 0x80 : 0));
      if (s->drawing != NULL)
        put_drawing_opt (a, s->drawing, i + 1);
      else if (s->is_control)
        {
          /* What Excel puts on a form control: no fill of its own, no
           * line, and the text laid out inside the shape. */
          header (a, 3, 7, ESC_OPT, 7 * 6);
          put16 (a, 0x007F); put32 (a, 0x01000100);
          put16 (a, 0x0080); put32 (a, 0x00000000);
          put16 (a, 0x0085); put32 (a, 0x00000001);
          put16 (a, 0x00BF); put32 (a, 0x001A0008);
          put16 (a, 0x01BF); put32 (a, 0x00100000);
          put16 (a, 0x01FF); put32 (a, 0x00080000);
          put16 (a, 0x03BF); put32 (a, 0x00080000);
        }
      else if (s->is_chart)
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
          gboolean turned = s->rotation != 0;

          header (a, 3, turned ? 4 : 3, ESC_OPT, (turned ? 4 : 3) * 6);
          if (turned)
            { put16 (a, 0x0004); put32 (a, escher_rotation (s->rotation, s->flip_h, s->flip_v)); }
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
              /* What a shape has unless its Opt says otherwise. */
              cur.filled = TRUE;
              cur.fill = 0xFFFFFF;
              cur.lined = TRUE;
              cur.line = 0x000000;
              cur.line_width = 1;
              cur.head_start_size = cur.head_end_size = O42_HEAD_MEDIUM;
              in_shape = TRUE;
            }
          p = body;
          continue;
        }
      if (type == ESC_SP && rlen >= 8)
        {
          guint32 flags = rd32 (body + 4);

          cur.spt = inst;
          cur.is_picture = inst == 75;
          cur.is_chart = inst == 201;
          cur.flip_h = (flags & 0x40) != 0;
          cur.flip_v = (flags & 0x80) != 0;
        }
      else if (type == ESC_OPT)
        {
          /* The instance counts the properties; a complex one's bytes
           * follow the table and are not properties themselves. */
          for (guint i = 0, n = 0; i + 6 <= rlen && n < inst; i += 6, n++)
            {
              guint id = rd16 (body + i) & 0x3FFF;
              guint32 v = rd32 (body + i + 2);

              switch (id)
                {
                case 0x0004: cur.rotation = (gint32) v / 65536.0; break;   /* the flips are known by the end: see below */
                case 0x0080: cur.has_text = TRUE; break;
                case 0x0104: cur.blip = v; break;
                /* A high byte marks a palette or system colour, which is left
                 * at the default rather than misread as an RGB. */
                case 0x0181: if ((v >> 24) == 0) cur.fill = escher_colour (v); break;
                case 0x01BF: if (v & 0x00100000) cur.filled = (v & 0x10) != 0; break;
                case 0x01C0: if ((v >> 24) == 0) cur.line = escher_colour (v); break;
                case 0x01CB: cur.line_width = floor (v / 95.25 + 0.5) / 100; break;   /* EMU, to the hundredth */
                case 0x01CE: cur.dash = dash_from_escher (v); break;
                case 0x01D0: cur.head_start = head_from_escher (v); break;
                case 0x01D1: cur.head_end = head_from_escher (v); break;
                case 0x01D3: cur.head_start_size = (O42HeadSize) MIN (v, 2); break;
                case 0x01D5: cur.head_end_size = (O42HeadSize) MIN (v, 2); break;
                case 0x01FF: if (v & 0x00080000) cur.lined = (v & 0x08) != 0; break;
                default: break;
                }
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

  /* The angle's sign depends on the flips, which the Sp record gave
   * before the Opt: settle it now that both are known. */
  for (guint i = 0; i < found->len; i++)
    {
      O42EscherFound *f = &g_array_index (found, O42EscherFound, i);

      if (f->rotation != 0)
        f->rotation = rotation_from_escher ((guint32) (gint32) (f->rotation * 65536), f->flip_h, f->flip_v);
    }
}
