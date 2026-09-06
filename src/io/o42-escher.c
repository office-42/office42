/* o42-escher.c - the Office Drawing records inside .xls
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-escher.h"
#include "o42-image.h"

#include <math.h>
#include <string.h>

/* Record types */
enum {
  ESC_DGG_CONTAINER = 0xF000, ESC_BSTORE_CONTAINER = 0xF001, ESC_DG_CONTAINER = 0xF002,
  ESC_SPGR_CONTAINER = 0xF003, ESC_SP_CONTAINER = 0xF004, ESC_DGG = 0xF006, ESC_BSE = 0xF007,
  ESC_DG = 0xF008, ESC_SPGR = 0xF009, ESC_SP = 0xF00A, ESC_OPT = 0xF00B,
  ESC_CLIENT_ANCHOR = 0xF010, ESC_CLIENT_DATA = 0xF011, ESC_SPLIT_MENU = 0xF11E,
  ESC_BLIP_EMF = 0xF01A, ESC_BLIP_WMF = 0xF01B, ESC_BLIP_PICT = 0xF01C,
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

/* A BSE's own record length is set by fix_length; its `size` field,
 * 20 bytes into the body, is the length of the blip record inside it,
 * which is the BSE's length less its 36-byte header. */
static void
set_blip_size (GByteArray *a, gsize bse_at)
{
  guint32 len = a->data[bse_at + 4] | (a->data[bse_at + 5] << 8) |
                (a->data[bse_at + 6] << 16) | ((guint32) a->data[bse_at + 7] << 24);
  guint32 size = len - 36;
  a->data[bse_at + 8 + 20] = size & 0xff;
  a->data[bse_at + 8 + 21] = (size >> 8) & 0xff;
  a->data[bse_at + 8 + 22] = (size >> 16) & 0xff;
  a->data[bse_at + 8 + 23] = (size >> 24) & 0xff;
}

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
          gboolean emf = strcmp (fmt, "emf") == 0, wmf = strcmp (fmt, "wmf") == 0;
          /* msoblipEMF, msoblipWMF, msoblipJPEG, msoblipPNG */
          guint type = emf ? 2 : wmf ? 3 : jpeg ? 5 : 6;
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
            guchar bt[2] = { type, emf || wmf ? 4 : type };   /* on Windows; on a Mac, a PICT */
            g_byte_array_append (a, bt, 2);
          }
          g_byte_array_append (a, uid, 16);
          put16 (a, 0xFF);                       /* tag */
          put32 (a, 0);                          /* size of the blip record, set below */
          put32 (a, 1);                          /* cRef */
          put32 (a, 0);                          /* foDelay */
          {
            guchar rest[4] = { 0, 0, 0, 0 };     /* usage, cbName, unused */
            g_byte_array_append (a, rest, 4);
          }
          if (emf || wmf)
            {
              /* A metafile blip: the uid, then a header giving the
               * size in bytes, the bounds in hundredths of a
               * millimetre, the size in EMUs, and the deflated bytes,
               * which is how Office stores them. */
              int pw = 1, ph = 1;
              const char *f;
              GBytes *packed;

              o42_image_is_metafile (img, &pw, &ph, &f);
              {
                GConverter *conv = G_CONVERTER (g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_ZLIB, 6));
                GInputStream *mem = g_memory_input_stream_new_from_bytes (img);
                GInputStream *in = g_converter_input_stream_new (mem, conv);
                GByteArray *out = g_byte_array_new ();
                guchar buffer[8192];
                gssize got;
                while ((got = g_input_stream_read (in, buffer, sizeof buffer, NULL, NULL)) > 0)
                  g_byte_array_append (out, buffer, got);
                g_object_unref (in);
                g_object_unref (mem);
                g_object_unref (conv);
                packed = g_byte_array_free_to_bytes (out);
              }
              header (a, 0, emf ? 0x3D4 : 0x216, emf ? ESC_BLIP_EMF : ESC_BLIP_WMF,
                      16 + 34 + g_bytes_get_size (packed));
              g_byte_array_append (a, uid, 16);
              put32 (a, size);                     /* cb, the bytes unpacked */
              put32 (a, 0); put32 (a, 0);          /* rcBounds */
              put32 (a, (guint32) (pw * 2540.0 / 96.0 + 0.5)); put32 (a, (guint32) (ph * 2540.0 / 96.0 + 0.5));
              put32 (a, pw * 9525); put32 (a, ph * 9525);   /* ptSize, EMUs */
              put32 (a, g_bytes_get_size (packed));         /* cbSave */
              {
                guchar cf[2] = { 0x00, 0xFE };     /* compression deflate, filter none */
                g_byte_array_append (a, cf, 2);
              }
              g_byte_array_append (a, g_bytes_get_data (packed, NULL), g_bytes_get_size (packed));
              g_bytes_unref (packed);
              fix_length (a, bse_at);
              set_blip_size (a, bse_at);
              continue;
            }
          else
            {
              header (a, 0, jpeg ? 0x46A : 0x6E0, jpeg ? ESC_BLIP_JPEG : ESC_BLIP_PNG, 16 + 1 + size);
              g_byte_array_append (a, uid, 16);
              {
                guchar tag = 0xFF;
                g_byte_array_append (a, &tag, 1);
              }
            }
          g_byte_array_append (a, g_bytes_get_data (img, NULL), size);
          fix_length (a, bse_at);
          set_blip_size (a, bse_at);
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
  const O42Shape *complex_from = NULL;

  header (a, 3, 0, ESC_OPT, 0);
  if (sh->rotation != 0)
    { put16 (a, 0x0004); put32 (a, escher_rotation (sh->rotation, sh->flip_h, sh->flip_v)); n++; }
  if (has_text)
    {
      guint32 inset = (guint32) (MAX (sh->text_inset, 0) * 9525);

      put16 (a, 0x0080); put32 (a, txid << 16); n++;                      /* lTxid */
      put16 (a, 0x0081); put32 (a, inset); n++;                           /* dxTextLeft, EMU */
      put16 (a, 0x0082); put32 (a, inset); n++;                           /* dyTextTop */
      put16 (a, 0x0083); put32 (a, inset); n++;                           /* dxTextRight */
      put16 (a, 0x0084); put32 (a, inset); n++;                           /* dyTextBottom */
      put16 (a, 0x0086); put32 (a, sh->text_nowrap ? 2 : 0); n++;        /* WrapText: none, or square */
      put16 (a, 0x00BF); put32 (a, 0x00080008); n++;                      /* fFitTextToShape off, text on */
    }
  if (sh->kind == O42_SHAPE_FREEFORM && sh->path != NULL)
    {
      /* A freeform: its outline in a 21600-square, as vertices and the
       * segments that join them.  The two arrays are complex properties:
       * their sizes stand in the table, their bytes follow it. */
      guint n_pts = 0, n_seg = 3;   /* the moveTo, and the close and end */

      for (guint i = 0; i < sh->path->len; i++)
        {
          const O42PathPoint *pt = &g_array_index (sh->path, O42PathPoint, i);
          n_pts += pt->op == 'C' ? 3 : 1;
          if (i > 0) n_seg++;
        }
      put16 (a, 0x0140); put32 (a, 0); n++;                              /* geoLeft */
      put16 (a, 0x0141); put32 (a, 0); n++;                              /* geoTop */
      put16 (a, 0x0142); put32 (a, 21600); n++;                          /* geoRight */
      put16 (a, 0x0143); put32 (a, 21600); n++;                          /* geoBottom */
      put16 (a, 0x0144); put32 (a, 4); n++;                              /* shapePath: complex */
      put16 (a, 0x8145); put32 (a, 6 + 8 * n_pts); n++;                  /* pVertices, complex */
      put16 (a, 0x8146); put32 (a, 6 + 2 * n_seg); n++;                  /* pSegmentInfo, complex */
      complex_from = sh;
    }
  if (!line_kind && sh->fill != O42_FILL_NONE && sh->fill_kind == O42_SHAPE_FILL_GRADIENT)
    { put16 (a, 0x0180); put32 (a, 4); n++; }                            /* fillType: shade */
  if (!line_kind && sh->fill != O42_FILL_NONE)
    { put16 (a, 0x0181); put32 (a, escher_colour (sh->fill)); n++; }     /* fillColor */
  if (!line_kind && sh->fill != O42_FILL_NONE && sh->fill_kind == O42_SHAPE_FILL_GRADIENT)
    {
      /* Escher's angle runs the other way from ours and starts at the
       * top; 16.16 fixed point. */
      double angle = fmod (fmod (270 - sh->gradient_angle, 360) + 360, 360);

      put16 (a, 0x0183); put32 (a, escher_colour (sh->fill2)); n++;      /* fillBackColor */
      put16 (a, 0x0186); put32 (a, 0); n++;                              /* fillFocus */
      put16 (a, 0x018B); put32 (a, (guint32) (gint32) (angle * 65536)); n++;   /* fillAngle */
    }
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
  if (sh->shadow)
    {
      put16 (a, 0x0200); put32 (a, 0); n++;                              /* shadowType: offset */
      put16 (a, 0x0201); put32 (a, escher_colour (sh->shadow_colour)); n++;
      put16 (a, 0x0205); put32 (a, (guint32) (gint32) (sh->shadow_dx * 9525)); n++;   /* shadowOffsetX, EMU */
      put16 (a, 0x0206); put32 (a, (guint32) (gint32) (sh->shadow_dy * 9525)); n++;
      put16 (a, 0x023F); put32 (a, 0x00020002); n++;                     /* fShadow */
    }
  put16 (a, 0x03BF); put32 (a, 0x00080000); n++;                         /* not hidden, printable */
  if (complex_from != NULL)
    {
      /* The complex properties' bytes, in the order of the table. */
      GArray *path = complex_from->path;
      guint n_pts = 0, n_seg = 3;

      for (guint i = 0; i < path->len; i++)
        {
          const O42PathPoint *pt = &g_array_index (path, O42PathPoint, i);
          n_pts += pt->op == 'C' ? 3 : 1;
          if (i > 0) n_seg++;
        }
      put16 (a, n_pts); put16 (a, n_pts); put16 (a, 8);                  /* IMsoArray: count, allocated, element size */
      for (guint i = 0; i < path->len; i++)
        {
          const O42PathPoint *pt = &g_array_index (path, O42PathPoint, i);
          if (pt->op == 'C')
            {
              put32 (a, (guint32) (gint32) (pt->x1 * 21600)); put32 (a, (guint32) (gint32) (pt->y1 * 21600));
              put32 (a, (guint32) (gint32) (pt->x2 * 21600)); put32 (a, (guint32) (gint32) (pt->y2 * 21600));
            }
          put32 (a, (guint32) (gint32) (pt->x * 21600)); put32 (a, (guint32) (gint32) (pt->y * 21600));
        }
      put16 (a, n_seg); put16 (a, n_seg); put16 (a, 2);
      put16 (a, 0x4000);                                                  /* moveTo */
      for (guint i = 1; i < path->len; i++)
        put16 (a, g_array_index (path, O42PathPoint, i).op == 'C' ? 0x2001 : 0x0001);
      put16 (a, complex_from->closed ? 0x6001 : 0x0000);                  /* close, or a lineTo of nothing */
      put16 (a, 0x8000);                                                  /* end */
    }
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
  /* 0: moved and sized with the cells; 2: moved, size its own; 3: neither
   * -- which is how Excel writes notes too. */
  put16 (a, s->is_note ? 3 : s->anchor_mode == O42_ANCHOR_ABSOLUTE ? 3 : s->anchor_mode == O42_ANCHOR_ONE_CELL ? 2 : 0);
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
              else if (btype == ESC_BLIP_EMF || btype == ESC_BLIP_WMF || btype == ESC_BLIP_PICT)
                {
                  /* A metafile: one or two uids, then a 34-byte header
                   * whose last two bytes say whether the bytes are
                   * deflated (0) or stored as they are (0xFE). */
                  gboolean two = binst == 0x3D5 || binst == 0x217 || binst == 0x543;
                  const guchar *h = img + (two ? 32 : 16);
                  fmt = btype == ESC_BLIP_EMF ? "emf" : btype == ESC_BLIP_WMF ? "wmf" : "pict";
                  skip = (two ? 32 : 16) + 34;
                  if (h + 34 <= body + rlen && blen >= skip && img + blen <= end)
                    {
                      guint32 cb = rd32 (h);
                      GBytes *raw = g_bytes_new (img + skip, blen - skip);
                      if (h[32] == 0)
                        {
                          /* zlib-deflated: what GLib undoes. */
                          GConverter *conv = G_CONVERTER (g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_ZLIB));
                          GInputStream *mem = g_memory_input_stream_new_from_bytes (raw);
                          GInputStream *in = g_converter_input_stream_new (mem, conv);
                          GByteArray *out = g_byte_array_new ();
                          guchar buffer[8192];
                          gssize got;
                          while ((got = g_input_stream_read (in, buffer, sizeof buffer, NULL, NULL)) > 0 &&
                                 out->len < cb + sizeof buffer)
                            g_byte_array_append (out, buffer, got);
                          g_object_unref (in);
                          g_object_unref (mem);
                          g_object_unref (conv);
                          g_bytes_unref (raw);
                          raw = g_byte_array_free_to_bytes (out);
                        }
                      g_ptr_array_add (images, raw);
                      g_ptr_array_add (formats, (gpointer) g_intern_string (fmt));
                      p = body + rlen;
                      continue;
                    }
                  fmt = NULL;
                }
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

/* A freeform's vertices and segments into a path of fractions of the
 * shape's own geometry box.  Elements are 4 or 8 bytes (two 16-bit or
 * two 32-bit numbers; 0xFFF0 in the size field means the 16-bit kind).
 * Segments say how the vertices are joined: a move, so many lines, so
 * many curves, a close; without them every vertex is a line. */
static void
escher_read_path (O42EscherFound *cur, const guchar *vertices, gsize vlen,
                  const guchar *segments, gsize slen, const gint32 *geo)
{
  guint n = rd16 (vertices), cb = rd16 (vertices + 4);
  gsize each = (cb == 8) ? 8 : 4;
  /* A box of nothing (LibreOffice's) means the numbers are EMU of the
   * shape's own size, which the caller knows. */
  gboolean raw = geo[2] - geo[0] == 0 && geo[3] - geo[1] == 0;
  double gw = raw ? 1 : MAX (geo[2] - geo[0], 1), gh = raw ? 1 : MAX (geo[3] - geo[1], 1);
  const guchar *v = vertices + 6;
  guint used = 0;
  GArray *path = g_array_new (FALSE, FALSE, sizeof (O42PathPoint));
  gboolean closed = FALSE;

#define VERTEX(i, px, py) G_STMT_START {                                            \
    const guchar *q = v + (gsize) (i) * each;                                       \
    if (each == 8) { px = ((gint32) rd32 (q) - geo[0]) / gw; py = ((gint32) rd32 (q + 4) - geo[1]) / gh; } \
    else { px = ((gint16) rd16 (q) - geo[0]) / gw; py = ((gint16) rd16 (q + 2) - geo[1]) / gh; } \
  } G_STMT_END

  if (6 + (gsize) n * each > vlen)
    n = (guint) ((vlen - 6) / each);
  /* A vertex that names a guide (bit 31 set) is an AutoShape's formula,
   * which office42 does not evaluate: no path, and the caller draws
   * what it can. */
  if (each == 8)
    for (guint i = 0; i < n; i++)
      if ((rd32 (v + (gsize) i * 8) & 0x80000000u) || (rd32 (v + (gsize) i * 8 + 4) & 0x80000000u))
        { g_array_unref (path); return; }
  if (segments != NULL && slen >= 6)
    {
      guint ns = rd16 (segments);
      const guchar *s = segments + 6;

      if (6 + (gsize) ns * 2 > slen)
        ns = (guint) ((slen - 6) / 2);
      for (guint k = 0; k < ns; k++)
        {
          guint code = rd16 (s + 2 * k);
          guint kind = code >> 13, count = code & 0x1FFF;

          if (kind <= 2 && used >= n)
            break;                /* a step wanting a vertex there is not */
          if (kind == 2)          /* moveTo */
            {
              O42PathPoint p = { 'M', 0, 0, 0, 0, 0, 0 };
              VERTEX (used, p.x, p.y); used++;
              g_array_append_val (path, p);
            }
          else if (kind == 0)     /* lineTo, count of them */
            for (guint c = 0; c < count && used < n; c++)
              {
                O42PathPoint p = { 'L', 0, 0, 0, 0, 0, 0 };
                VERTEX (used, p.x, p.y); used++;
                g_array_append_val (path, p);
              }
          else if (kind == 1)     /* curveTo, three vertices each */
            for (guint c = 0; c < count && used + 2 < n; c++)
              {
                O42PathPoint p = { 'C', 0, 0, 0, 0, 0, 0 };
                VERTEX (used, p.x1, p.y1); VERTEX (used + 1, p.x2, p.y2); VERTEX (used + 2, p.x, p.y);
                used += 3;
                g_array_append_val (path, p);
              }
          else if (kind == 3)     /* close */
            closed = TRUE;
          else if (kind == 4)     /* end */
            break;
        }
    }
  else
    {
      for (guint i = 0; i < n; i++)
        {
          O42PathPoint p = { i == 0 ? 'M' : 'L', 0, 0, 0, 0, 0, 0 };
          VERTEX (i, p.x, p.y);
          g_array_append_val (path, p);
        }
      closed = n >= 3;
    }
#undef VERTEX
  if (path->len >= 2)
    {
      /* Whatever the segments said, an outline starts with a move. */
      g_array_index (path, O42PathPoint, 0).op = 'M';
      if (cur->path != NULL) g_array_unref (cur->path);
      cur->path = path;
      cur->closed = closed;
      cur->path_raw = raw;
    }
  else
    g_array_unref (path);
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
              cur.text_inset = -1;
              cur.text_wrap = -1;
              cur.fill_back = 0xFFFFFF;
              cur.shadow_colour = 0x808080;
              cur.shadow_dx = cur.shadow_dy = 3;
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
          const guchar *complex = body + 6 * MIN (inst, rlen / 6);
          const guchar *vertices = NULL, *segments = NULL;
          gsize n_vertex_bytes = 0, n_segment_bytes = 0;
          gint32 geo[4] = { 0, 0, 21600, 21600 };

          for (guint i = 0, n = 0; i + 6 <= rlen && n < inst; i += 6, n++)
            {
              guint id = rd16 (body + i) & 0x3FFF;
              gboolean is_complex = (rd16 (body + i) & 0x8000) != 0;
              guint32 v = rd32 (body + i + 2);

              if (is_complex)
                {
                  /* Its bytes are the next v of the data after the table.
                   * An array's v counts its six-byte header in Excel's
                   * files and leaves it out in LibreOffice's; the header
                   * itself says how long the array really is. */
                  gsize actual = v;

                  if (id >= 0x0145 && id <= 0x0159 && complex + 6 <= body + rlen)
                    {
                      guint ne = rd16 (complex), cb = rd16 (complex + 4);
                      gsize each = cb == 0xFFF0 ? 4 : cb;
                      gsize with_header = 6 + (gsize) ne * each;

                      if (with_header == v + 6 || (with_header != v && with_header <= (gsize) (body + rlen - complex)))
                        actual = with_header;
                    }
                  if (complex + actual <= body + rlen)
                    {
                      if (id == 0x0145) { vertices = complex; n_vertex_bytes = actual; }
                      else if (id == 0x0146) { segments = complex; n_segment_bytes = actual; }
                    }
                  complex += actual;
                  continue;
                }
              switch (id)
                {
                case 0x0140: geo[0] = (gint32) v; break;
                case 0x0141: geo[1] = (gint32) v; break;
                case 0x0142: geo[2] = (gint32) v; break;
                case 0x0143: geo[3] = (gint32) v; break;
                case 0x0004: cur.rotation = (gint32) v / 65536.0; break;   /* the flips are known by the end: see below */
                case 0x0080: cur.has_text = TRUE; break;
                case 0x0081: cur.text_inset = floor (v / 9525.0 + 0.5); break;
                case 0x0086: cur.text_wrap = (int) v; break;
                case 0x0104: cur.blip = v; break;
                /* A high byte marks a palette or system colour, which is left
                 * at the default rather than misread as an RGB. */
                case 0x0180: cur.fill_type = (int) v; break;
                case 0x0181: if ((v >> 24) == 0) cur.fill = escher_colour (v); break;
                case 0x0183: if ((v >> 24) == 0) cur.fill_back = escher_colour (v); break;
                case 0x018B: cur.fill_angle = fmod (fmod (270 - (gint32) v / 65536.0, 360) + 360, 360); break;
                case 0x0201: if ((v >> 24) == 0) cur.shadow_colour = escher_colour (v); break;
                case 0x0205: cur.shadow_dx = (gint32) v / 9525.0; break;
                case 0x0206: cur.shadow_dy = (gint32) v / 9525.0; break;
                case 0x023F: if (v & 0x00020000) cur.shadow = (v & 0x02) != 0; break;
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
          if (vertices != NULL && n_vertex_bytes >= 6)
            escher_read_path (&cur, vertices, n_vertex_bytes, segments, n_segment_bytes, geo);
        }
      else if (type == ESC_CLIENT_ANCHOR && rlen >= 18)
        {
          guint flags = rd16 (body);

          cur.anchor_mode = flags == 3 ? O42_ANCHOR_ABSOLUTE : flags == 2 ? O42_ANCHOR_ONE_CELL : O42_ANCHOR_TWO_CELL;
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
