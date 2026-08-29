/* o42-fmt.c - see o42-fmt.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-fmt.h"

#include <string.h>

struct _O42FmtTable {
  GPtrArray  *records;   /* O42Fmt*, index is the O42FmtIdx */
  GHashTable *index;     /* O42Fmt* -> index + 1 */
  O42FmtIdx   fallback;
};

void
o42_fmt_init_default (O42Fmt *fmt)
{
  g_return_if_fail (fmt != NULL);

  memset (fmt, 0, sizeof *fmt);

  /* Excel 5 opened in 10pt Arial. */
  fmt->family   = g_intern_static_string ("Arial");
  fmt->locked   = 1;   /* Excel locks every cell until the sheet is protected */
  fmt->size     = 20;
  fmt->colour   = 0x000000;
  fmt->fill     = O42_FILL_NONE;
  fmt->halign   = O42_HALIGN_GENERAL;
  fmt->valign   = O42_VALIGN_BOTTOM;
  fmt->number   = O42_NUM_GENERAL;
  fmt->decimals = 2;
}

void
o42_fmt_apply_mask (O42Fmt *fmt, O42FmtMask mask, const O42Fmt *value)
{
  if (mask & O42_FMT_FAMILY)    fmt->family    = value->family;
  if (mask & O42_FMT_SIZE)      fmt->size      = value->size;
  if (mask & O42_FMT_BOLD)      fmt->bold      = value->bold;
  if (mask & O42_FMT_ITALIC)    fmt->italic    = value->italic;
  if (mask & O42_FMT_UNDERLINE) fmt->underline = value->underline;
  if (mask & O42_FMT_STRIKEOUT) fmt->strikeout = value->strikeout;
  if (mask & O42_FMT_COLOUR)    fmt->colour    = value->colour;
  if (mask & O42_FMT_FILL)      fmt->fill      = value->fill;
  if (mask & O42_FMT_HALIGN)    fmt->halign    = value->halign;
  if (mask & O42_FMT_VALIGN)    fmt->valign    = value->valign;
  if (mask & O42_FMT_NUMBER)    { fmt->number = value->number; fmt->custom = value->custom; }
  if (mask & O42_FMT_DECIMALS)  fmt->decimals  = value->decimals;
  if (mask & O42_FMT_WRAP)      fmt->wrap      = value->wrap;

  if (mask & O42_FMT_BORDERS)
    {
      for (int i = 0; i < 4; i++)
        {
          fmt->border_style[i] = value->border_style[i];
          fmt->border_colour[i] = value->border_colour[i];
        }
      o42_fmt_sync_borders (fmt);
    }
  if (mask & O42_FMT_PROTECTION) { fmt->locked = value->locked; fmt->hidden = value->hidden; }
  if (mask & O42_FMT_PATTERN)   { fmt->pattern = value->pattern;
                                  fmt->pattern_colour = value->pattern_colour; }
  if (mask & O42_FMT_INDENT)    fmt->indent = value->indent;
  if (mask & O42_FMT_ROTATION)  fmt->rotation = value->rotation;
}

void
o42_fmt_sync_borders (O42Fmt *fmt)
{
  fmt->border_top = fmt->border_style[O42_SIDE_TOP] != O42_BORDER_NONE;
  fmt->border_bottom = fmt->border_style[O42_SIDE_BOTTOM] != O42_BORDER_NONE;
  fmt->border_left = fmt->border_style[O42_SIDE_LEFT] != O42_BORDER_NONE;
  fmt->border_right = fmt->border_style[O42_SIDE_RIGHT] != O42_BORDER_NONE;
}

static const char *BORDER_NAMES[] = { "none", "thin", "medium", "thick", "double", "dashed", "dotted" };

const char *
o42_border_style_name (O42BorderStyle style)
{
  return (guint) style < G_N_ELEMENTS (BORDER_NAMES) ? BORDER_NAMES[style] : "none";
}

gboolean
o42_border_style_parse (const char *name, O42BorderStyle *style)
{
  for (guint i = 0; name != NULL && i < G_N_ELEMENTS (BORDER_NAMES); i++)
    if (g_ascii_strcasecmp (name, BORDER_NAMES[i]) == 0)
      { *style = (O42BorderStyle) i; return TRUE; }
  return FALSE;
}

static guint
fmt_hash (gconstpointer key)
{
  const guint8 *bytes = key;
  guint hash = 5381;

  for (gsize i = 0; i < sizeof (O42Fmt); i++)
    hash = (hash << 5) + hash + bytes[i];

  return hash;
}

static gboolean
fmt_equal (gconstpointer a, gconstpointer b)
{
  return memcmp (a, b, sizeof (O42Fmt)) == 0;
}

O42FmtTable *
o42_fmt_table_new (void)
{
  O42FmtTable *table = g_new0 (O42FmtTable, 1);
  O42Fmt fallback;

  table->records = g_ptr_array_new_with_free_func (g_free);
  table->index   = g_hash_table_new (fmt_hash, fmt_equal);

  o42_fmt_init_default (&fallback);
  table->fallback = o42_fmt_table_intern (table, &fallback);

  return table;
}

void
o42_fmt_table_free (O42FmtTable *table)
{
  if (table == NULL)
    return;

  g_hash_table_destroy (table->index);
  g_ptr_array_free (table->records, TRUE);
  g_free (table);
}

O42FmtIdx
o42_fmt_table_intern (O42FmtTable *table, const O42Fmt *fmt)
{
  gpointer found;
  O42Fmt *copy;

  g_return_val_if_fail (table != NULL, 0);
  g_return_val_if_fail (fmt != NULL, 0);

  found = g_hash_table_lookup (table->index, fmt);
  if (found != NULL)
    return (O42FmtIdx) (GPOINTER_TO_UINT (found) - 1);

  copy = g_memdup2 (fmt, sizeof *fmt);
  g_ptr_array_add (table->records, copy);

  /* The key is the stored record, so it lives as long as the table does. */
  g_hash_table_insert (table->index, copy,
                       GUINT_TO_POINTER (table->records->len));

  return (O42FmtIdx) (table->records->len - 1);
}

const O42Fmt *
o42_fmt_table_get (O42FmtTable *table, O42FmtIdx idx)
{
  g_return_val_if_fail (table != NULL, NULL);

  if (idx >= table->records->len)
    idx = table->fallback;

  return g_ptr_array_index (table->records, idx);
}

O42FmtIdx
o42_fmt_table_default (O42FmtTable *table)
{
  g_return_val_if_fail (table != NULL, 0);
  return table->fallback;
}

/* ---------------------------------------------------------------------- */
/* Writing a value out                                                     */
/* ---------------------------------------------------------------------- */

char *
o42_fmt_display (const O42Fmt *fmt, const O42Value *value)
{
  g_return_val_if_fail (value != NULL, g_strdup (""));

  /* A format string applies to numbers and, through its fourth section,
   * to text. */
  if (fmt != NULL && fmt->custom != NULL)
    {
      if (value->type == O42_VALUE_NUMBER)
        return o42_format_string (fmt->custom, value->as.number, NULL);
      if (value->type == O42_VALUE_TEXT)
        return o42_format_string (fmt->custom, 0, value->as.text);
    }

  /* Only numbers have a number format.  Everything else shows as itself. */
  if (value->type != O42_VALUE_NUMBER || fmt == NULL ||
      fmt->number == O42_NUM_GENERAL || fmt->number == O42_NUM_TEXT)
    return o42_value_display (value);

  return o42_number_format (value->as.number, fmt->number, fmt->decimals);
}

gboolean
o42_fmt_display_colour (const O42Fmt *fmt, const O42Value *value, guint32 *colour)
{
  if (fmt == NULL || fmt->custom == NULL || value->type != O42_VALUE_NUMBER)
    return FALSE;
  return o42_format_string_colour (fmt->custom, value->as.number, colour);
}

char *
o42_fmt_format_string (const O42Fmt *fmt)
{
  g_return_val_if_fail (fmt != NULL, g_strdup ("General"));

  if (fmt->custom != NULL)
    return g_strdup (fmt->custom);
  return o42_number_format_to_string (fmt->number, fmt->decimals);
}

O42HAlign
o42_fmt_effective_halign (const O42Fmt *fmt, const O42Value *value)
{
  if (fmt != NULL && fmt->halign != O42_HALIGN_GENERAL)
    return fmt->halign;

  switch (value->type)
    {
    case O42_VALUE_NUMBER: return O42_HALIGN_RIGHT;
    case O42_VALUE_BOOL:
    case O42_VALUE_ERROR:  return O42_HALIGN_CENTRE;
    default:               return O42_HALIGN_LEFT;
    }
}

void
o42_draw_border_line (cairo_t *cr, O42BorderStyle style, guint32 colour,
                      double x0, double y0, double x1, double y1)
{
  static const double dashes[] = { 4, 3 };
  static const double dots[] = { 1, 2 };
  gboolean vertical = x0 == x1;

  if (style == O42_BORDER_NONE)
    return;
  cairo_save (cr);
  cairo_set_source_rgb (cr, ((colour >> 16) & 0xFF) / 255.0, ((colour >> 8) & 0xFF) / 255.0, (colour & 0xFF) / 255.0);
  cairo_set_line_width (cr, style == O42_BORDER_MEDIUM ? 2 : style == O42_BORDER_THICK ? 3 : 1);
  if (style == O42_BORDER_DASHED) cairo_set_dash (cr, dashes, 2, 0);
  if (style == O42_BORDER_DOTTED) cairo_set_dash (cr, dots, 2, 0);
  if (style == O42_BORDER_DOUBLE)
    {
      double dx = vertical ? 1 : 0, dy = vertical ? 0 : 1;
      cairo_move_to (cr, x0 - dx, y0 - dy); cairo_line_to (cr, x1 - dx, y1 - dy);
      cairo_move_to (cr, x0 + dx, y0 + dy); cairo_line_to (cr, x1 + dx, y1 + dy);
    }
  else
    {
      cairo_move_to (cr, x0, y0);
      cairo_line_to (cr, x1, y1);
    }
  cairo_stroke (cr);
  cairo_restore (cr);
}
