/* o42-richtext.c - a cell's text set in more than one font
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-richtext.h"

#include <string.h>

/* One run's worth of attributes over [start, end). */
static void
add_run (PangoAttrList *list, const O42Fmt *fmt, const O42Fmt *base,
         guint start, guint end)
{
  PangoAttribute *attr;

  if (end <= start)
    return;

  if (fmt->family != base->family && fmt->family != NULL)
    {
      attr = pango_attr_family_new (fmt->family);
      attr->start_index = start; attr->end_index = end;
      pango_attr_list_insert (list, attr);
    }
  if (fmt->size != base->size)
    {
      attr = pango_attr_size_new ((fmt->size / 2) * PANGO_SCALE);
      attr->start_index = start; attr->end_index = end;
      pango_attr_list_insert (list, attr);
    }
  if (fmt->bold != base->bold)
    {
      attr = pango_attr_weight_new (fmt->bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
      attr->start_index = start; attr->end_index = end;
      pango_attr_list_insert (list, attr);
    }
  if (fmt->italic != base->italic)
    {
      attr = pango_attr_style_new (fmt->italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
      attr->start_index = start; attr->end_index = end;
      pango_attr_list_insert (list, attr);
    }
  if (fmt->underline != base->underline)
    {
      attr = pango_attr_underline_new (fmt->underline ? PANGO_UNDERLINE_SINGLE
                                                      : PANGO_UNDERLINE_NONE);
      attr->start_index = start; attr->end_index = end;
      pango_attr_list_insert (list, attr);
    }
  if (fmt->strikeout != base->strikeout)
    {
      attr = pango_attr_strikethrough_new (fmt->strikeout);
      attr->start_index = start; attr->end_index = end;
      pango_attr_list_insert (list, attr);
    }
  if (fmt->colour != base->colour)
    {
      attr = pango_attr_foreground_new ((fmt->colour >> 16 & 0xFF) * 257,
                                        (fmt->colour >> 8 & 0xFF) * 257,
                                        (fmt->colour & 0xFF) * 257);
      attr->start_index = start; attr->end_index = end;
      pango_attr_list_insert (list, attr);
    }
}

PangoAttrList *
o42_runs_attributes (const O42TextRun *runs, int n_runs, const O42Fmt *base,
                     const char *text)
{
  PangoAttrList *list;
  guint length;

  if (runs == NULL || n_runs <= 0 || text == NULL || base == NULL)
    return NULL;
  length = (guint) strlen (text);
  list = pango_attr_list_new ();

  for (int i = 0; i < n_runs; i++)
    {
      guint start = (guint) MAX (runs[i].start, 0);
      guint end = (i + 1 < n_runs) ? (guint) MAX (runs[i + 1].start, 0) : length;

      add_run (list, &runs[i].fmt, base, MIN (start, length), MIN (end, length));
    }
  return list;
}

void
o42_format_pad_attributes (PangoLayout *layout, const O42FormatLayout *pads,
                           PangoAttrList **attrs)
{
  o42_format_pad_attributes_between (layout, pads, 0, G_MAXINT, attrs);
}

void
o42_format_pad_attributes_between (PangoLayout *layout, const O42FormatLayout *pads,
                                   int from, int to, PangoAttrList **attrs)
{
  g_return_if_fail (layout != NULL && pads != NULL && attrs != NULL);

  for (int i = 0; i < pads->n_pads; i++)
    {
      char glyph[8];
      int width, height;
      PangoRectangle ink, logical;
      PangoAttribute *attr;

      if (pads->pads[i].at < from || pads->pads[i].at >= to)
        continue;
      glyph[g_unichar_to_utf8 (pads->pads[i].glyph, glyph)] = '\0';
      pango_layout_set_attributes (layout, NULL);
      pango_layout_set_text (layout, glyph, -1);
      pango_layout_get_size (layout, &width, &height);
      /* The space stands in for the glyph: it is laid out with the
       * glyph's advance and is not drawn.  The rectangle sits on the
       * baseline as the glyph would, so the line stays its own height. */
      ink.x = 0; ink.y = 0; ink.width = 0; ink.height = 0;
      logical.x = 0; logical.y = -pango_layout_get_baseline (layout);
      logical.width = width; logical.height = height;
      attr = pango_attr_shape_new (&ink, &logical);
      attr->start_index = (guint) (pads->pads[i].at - from);
      attr->end_index = attr->start_index + 1;
      if (*attrs == NULL)
        *attrs = pango_attr_list_new ();
      pango_attr_list_insert (*attrs, attr);
    }
  pango_layout_set_text (layout, "", -1);
}

void
o42_format_draw_filled (cairo_t *cr, PangoLayout *layout, const char *text,
                        const O42FormatLayout *fill, gboolean underline, gboolean strikeout,
                        double lx, double rx, double gap, double ty)
{
  const char *right = text + fill->fill_at;
  char *left = g_strndup (text, (gsize) fill->fill_at);
  PangoAttrList *attrs;

  g_return_if_fail (cr != NULL && layout != NULL && text != NULL && fill != NULL);

  /* Each half laid out on its own, so that neither's ink can stray
   * into the other's place. */
  for (int half = 0; half < 2; half++)
    {
      const char *part = half == 0 ? left : right;
      int from = half == 0 ? 0 : fill->fill_at;
      int to = half == 0 ? fill->fill_at : G_MAXINT;

      attrs = NULL;
      o42_format_pad_attributes_between (layout, fill, from, to, &attrs);
      if (underline || strikeout)
        {
          if (attrs == NULL)
            attrs = pango_attr_list_new ();
          if (underline)
            pango_attr_list_insert (attrs, pango_attr_underline_new (PANGO_UNDERLINE_SINGLE));
          if (strikeout)
            pango_attr_list_insert (attrs, pango_attr_strikethrough_new (TRUE));
        }
      pango_layout_set_text (layout, part, -1);
      pango_layout_set_attributes (layout, attrs);
      if (attrs != NULL)
        pango_attr_list_unref (attrs);
      cairo_move_to (cr, half == 0 ? lx : rx, ty);
      pango_cairo_show_layout (cr, layout);
    }
  g_free (left);

  /* The fill character across the gap, as many as fit. */
  if (fill->fill_char != ' ' && gap > 0)
    {
      char glyph[8];
      int gw, gh;
      GString *run = g_string_new (NULL);
      int left_w, left_h;

      pango_layout_set_attributes (layout, NULL);
      glyph[g_unichar_to_utf8 (fill->fill_char, glyph)] = '\0';
      pango_layout_set_text (layout, glyph, -1);
      pango_layout_get_pixel_size (layout, &gw, &gh);
      for (int i = 0; gw > 0 && (i + 1) * gw <= gap; i++)
        g_string_append (run, glyph);
      pango_layout_set_text (layout, run->str, -1);
      pango_layout_get_pixel_size (layout, &left_w, &left_h);
      cairo_move_to (cr, rx - left_w, ty);
      pango_cairo_show_layout (cr, layout);
      g_string_free (run, TRUE);
    }
  pango_layout_set_attributes (layout, NULL);
}

gboolean
o42_format_fill_split (PangoLayout *layout, const O42FormatLayout *fill,
                       double width, double pad,
                       double *left_w, double *right_w, double *gap)
{
  PangoRectangle pos;
  int tw, th;

  g_return_val_if_fail (layout != NULL && fill != NULL, FALSE);

  if (fill->fill_at < 0)
    return FALSE;
  pango_layout_get_pixel_size (layout, &tw, &th);
  if (tw + 2 * pad > width)
    return FALSE;
  pango_layout_index_to_pos (layout, fill->fill_at, &pos);
  *left_w = pos.x / (double) PANGO_SCALE;
  *right_w = tw - *left_w;
  *gap = width - 2 * pad - tw;
  return *gap > 0;
}
