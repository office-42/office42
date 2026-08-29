/* o42-pattern.c - the shading patterns a cell can be filled with
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-pattern.h"

#include <string.h>

/* One row of bits per line of an eight by eight tile, the high bit at
 * the left.  The greys are dots at five densities; the stripes and
 * crosshatches come in a light weight and a dark one, which is Excel's
 * whole list. */
static const guint8 TILES[O42_PATTERN_LAST][8] = {
  [O42_PATTERN_SOLID]           = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
  [O42_PATTERN_GRAY75]          = { 0x55, 0xFF, 0xAA, 0xFF, 0x55, 0xFF, 0xAA, 0xFF },
  [O42_PATTERN_GRAY50]          = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 },
  [O42_PATTERN_GRAY25]          = { 0xAA, 0x00, 0x55, 0x00, 0xAA, 0x00, 0x55, 0x00 },
  [O42_PATTERN_GRAY125]         = { 0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00 },
  [O42_PATTERN_GRAY0625]        = { 0x88, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00 },
  [O42_PATTERN_HORIZONTAL]      = { 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00 },
  [O42_PATTERN_VERTICAL]        = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA },
  [O42_PATTERN_DOWN]            = { 0x33, 0x66, 0xCC, 0x99, 0x33, 0x66, 0xCC, 0x99 },
  [O42_PATTERN_UP]              = { 0x99, 0xCC, 0x66, 0x33, 0x99, 0xCC, 0x66, 0x33 },
  [O42_PATTERN_GRID]            = { 0xFF, 0xAA, 0xFF, 0xAA, 0xFF, 0xAA, 0xFF, 0xAA },
  [O42_PATTERN_TRELLIS]         = { 0xBB, 0xEE, 0xEE, 0xBB, 0xBB, 0xEE, 0xEE, 0xBB },
  [O42_PATTERN_THIN_HORIZONTAL] = { 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00 },
  [O42_PATTERN_THIN_VERTICAL]   = { 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
  [O42_PATTERN_THIN_DOWN]       = { 0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88 },
  [O42_PATTERN_THIN_UP]         = { 0x88, 0x44, 0x22, 0x11, 0x88, 0x44, 0x22, 0x11 },
  [O42_PATTERN_THIN_GRID]       = { 0xFF, 0x88, 0x88, 0x88, 0xFF, 0x88, 0x88, 0x88 },
  [O42_PATTERN_THIN_TRELLIS]    = { 0x99, 0x66, 0x66, 0x99, 0x99, 0x66, 0x66, 0x99 }
};

/* The names Excel's files call them by, in the enum's order. */
static const char *const NAMES[O42_PATTERN_LAST] = {
  "none", "solid", "darkGray", "mediumGray", "lightGray", "gray125", "gray0625",
  "darkHorizontal", "darkVertical", "darkDown", "darkUp", "darkGrid", "darkTrellis",
  "lightHorizontal", "lightVertical", "lightDown", "lightUp", "lightGrid", "lightTrellis"
};

/* Gnumeric's Shade numbers for the same patterns, by our enum. */
static const int SHADES[O42_PATTERN_LAST] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 9, 11, 12, 13, 14, 16, 15, 17, 18
};

const char *
o42_pattern_name (O42Pattern pattern)
{
  if (pattern < 0 || pattern >= O42_PATTERN_LAST)
    return "none";
  return NAMES[pattern];
}

gboolean
o42_pattern_parse (const char *name, O42Pattern *pattern)
{
  if (name == NULL)
    return FALSE;
  for (int i = 0; i < O42_PATTERN_LAST; i++)
    if (g_ascii_strcasecmp (name, NAMES[i]) == 0)
      {
        *pattern = (O42Pattern) i;
        return TRUE;
      }
  return FALSE;
}

int
o42_pattern_to_shade (O42Pattern pattern)
{
  if (pattern < 0 || pattern >= O42_PATTERN_LAST)
    return 0;
  return SHADES[pattern];
}

O42Pattern
o42_pattern_from_shade (int shade)
{
  for (int i = 0; i < O42_PATTERN_LAST; i++)
    if (SHADES[i] == shade)
      return (O42Pattern) i;
  return shade > 0 ? O42_PATTERN_SOLID : O42_PATTERN_NONE;
}

/* The tile as a surface, painted once and kept: eighteen small
 * bitmaps, and every cell that shares a pattern shares one. */
static cairo_pattern_t *
tile_for (O42Pattern pattern, guint32 colour)
{
  static cairo_pattern_t *cache[O42_PATTERN_LAST];
  static guint32 cached_colour[O42_PATTERN_LAST];
  cairo_surface_t *surface;
  cairo_t *cr;
  cairo_pattern_t *result;

  if (pattern <= O42_PATTERN_NONE || pattern >= O42_PATTERN_LAST)
    return NULL;
  if (cache[pattern] != NULL && cached_colour[pattern] == colour)
    return cache[pattern];

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 8, 8);
  cr = cairo_create (surface);
  cairo_set_source_rgba (cr, ((colour >> 16) & 0xFF) / 255.0,
                             ((colour >> 8) & 0xFF) / 255.0,
                             (colour & 0xFF) / 255.0, 1.0);
  for (int row = 0; row < 8; row++)
    for (int col = 0; col < 8; col++)
      if (TILES[pattern][row] & (0x80 >> col))
        cairo_rectangle (cr, col, row, 1, 1);
  cairo_fill (cr);
  cairo_destroy (cr);

  result = cairo_pattern_create_for_surface (surface);
  cairo_surface_destroy (surface);
  cairo_pattern_set_extend (result, CAIRO_EXTEND_REPEAT);
  cairo_pattern_set_filter (result, CAIRO_FILTER_NEAREST);

  if (cache[pattern] != NULL)
    cairo_pattern_destroy (cache[pattern]);
  cache[pattern] = result;
  cached_colour[pattern] = colour;
  return result;
}

void
o42_pattern_fill (const O42Fmt *fmt, cairo_t *cr, double x, double y, double w, double h)
{
  cairo_pattern_t *tile;

  g_return_if_fail (fmt != NULL && cr != NULL);

  if (fmt->fill != O42_FILL_NONE)
    {
      cairo_set_source_rgb (cr, ((fmt->fill >> 16) & 0xFF) / 255.0,
                                ((fmt->fill >> 8) & 0xFF) / 255.0,
                                (fmt->fill & 0xFF) / 255.0);
      cairo_rectangle (cr, x, y, w, h);
      cairo_fill (cr);
    }

  tile = tile_for ((O42Pattern) fmt->pattern, fmt->pattern_colour);
  if (tile == NULL)
    return;

  /* The tile is laid out in the sheet's own pixels, so it does not
   * crawl when a cell is scrolled or the view is zoomed. */
  cairo_save (cr);
  cairo_rectangle (cr, x, y, w, h);
  cairo_clip (cr);
  cairo_set_source (cr, tile);
  cairo_paint (cr);
  cairo_restore (cr);
}
