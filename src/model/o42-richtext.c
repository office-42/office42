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
