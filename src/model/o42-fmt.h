/* o42-fmt.h - how a cell looks, and how a number is written out
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Formats are interned.  A sheet where a thousand cells are bold and
 * right-aligned holds one format record, and a cell holds an index into the
 * table rather than a copy, so comparing two cells' formatting is an integer
 * compare.  It is the same trick word42 uses for character formatting, and
 * it matters more here: a spreadsheet has far more cells than a document has
 * runs of text.
 */

#pragma once

#include "o42-value.h"
#include "o42-numfmt.h"
#include <cairo.h>

G_BEGIN_DECLS

typedef guint32 O42FmtIdx;

typedef enum {
  O42_HALIGN_GENERAL = 0,   /* numbers right, text left: the default */
  O42_HALIGN_LEFT,
  O42_HALIGN_CENTRE,
  O42_HALIGN_RIGHT
} O42HAlign;

typedef enum {
  O42_VALIGN_BOTTOM = 0,
  O42_VALIGN_MIDDLE,
  O42_VALIGN_TOP
} O42VAlign;

typedef enum {
  O42_BORDER_NONE = 0,
  O42_BORDER_THIN,
  O42_BORDER_MEDIUM,
  O42_BORDER_THICK,
  O42_BORDER_DOUBLE,
  O42_BORDER_DASHED,
  O42_BORDER_DOTTED
} O42BorderStyle;

/* The sides, in the order the arrays below keep them. */
enum { O42_SIDE_TOP = 0, O42_SIDE_BOTTOM, O42_SIDE_LEFT, O42_SIDE_RIGHT };

typedef struct {
  const char      *family;      /* interned, never freed */
  int              size;        /* half-points, so 20 is 10pt */
  guint            bold      : 1;
  guint            italic    : 1;
  guint            underline : 1;
  guint            strikeout : 1;
  guint            wrap      : 1;
  guint            border_top : 1;      /* these four say whether a side is
                                          * drawn; border_style says how */
  guint            border_bottom : 1;
  guint            border_left : 1;
  guint            border_right : 1;
  guint8           border_style[4];     /* O42BorderStyle, by O42_SIDE_* */
  guint32          border_colour[4];    /* 0x00RRGGBB, by O42_SIDE_* */
  guint            locked : 1;          /* on a protected sheet, cannot be
                                         * typed into; on by default, as in
                                         * Excel, so protecting locks the lot */
  guint            hidden : 1;          /* its formula is not shown either */
  guint8           indent;              /* steps in from the aligned edge */
  gint16           rotation;            /* degrees anticlockwise, -90..90 */
  guint32          colour;      /* 0x00RRGGBB */
  guint32          fill;        /* the shading behind the pattern:
                                 * 0x00RRGGBB, or O42_FILL_NONE */
  guint8           pattern;     /* O42Pattern, over the shading */
  guint32          pattern_colour;  /* the pattern's own colour */
  O42HAlign        halign;
  O42VAlign        valign;
  O42NumberFormat  number;
  int              decimals;
  const char      *custom;      /* interned format string, or NULL to use
                                 * `number` and `decimals` */
} O42Fmt;

#define O42_FILL_NONE 0xFFFFFFFFu

/* The eighteen patterns Excel shades a cell with, and no pattern.
 * o42-pattern.c draws them and knows their names. */
typedef enum {
  O42_PATTERN_NONE = 0,
  O42_PATTERN_SOLID,            /* the pattern colour, edge to edge */
  O42_PATTERN_GRAY75,
  O42_PATTERN_GRAY50,
  O42_PATTERN_GRAY25,
  O42_PATTERN_GRAY125,
  O42_PATTERN_GRAY0625,
  O42_PATTERN_HORIZONTAL,
  O42_PATTERN_VERTICAL,
  O42_PATTERN_DOWN,
  O42_PATTERN_UP,
  O42_PATTERN_GRID,
  O42_PATTERN_TRELLIS,
  O42_PATTERN_THIN_HORIZONTAL,
  O42_PATTERN_THIN_VERTICAL,
  O42_PATTERN_THIN_DOWN,
  O42_PATTERN_THIN_UP,
  O42_PATTERN_THIN_GRID,
  O42_PATTERN_THIN_TRELLIS,
  O42_PATTERN_LAST
} O42Pattern;

typedef enum {
  O42_FMT_FAMILY     = 1 << 0,
  O42_FMT_SIZE       = 1 << 1,
  O42_FMT_BOLD       = 1 << 2,
  O42_FMT_ITALIC     = 1 << 3,
  O42_FMT_UNDERLINE  = 1 << 4,
  O42_FMT_STRIKEOUT  = 1 << 5,
  O42_FMT_COLOUR     = 1 << 6,
  O42_FMT_FILL       = 1 << 7,
  O42_FMT_HALIGN     = 1 << 8,
  O42_FMT_VALIGN     = 1 << 9,
  O42_FMT_NUMBER     = 1 << 10,
  O42_FMT_DECIMALS   = 1 << 11,
  O42_FMT_WRAP       = 1 << 12,
  O42_FMT_BORDERS    = 1 << 13,      /* the four sides: drawn, style and colour */
  O42_FMT_INDENT     = 1 << 14,
  O42_FMT_ROTATION   = 1 << 15,
  O42_FMT_PROTECTION = 1 << 16,      /* locked and hidden */
  O42_FMT_PATTERN    = 1 << 17,      /* the pattern and its colour */
  O42_FMT_ALL        = 0x3FFFF
} O42FmtMask;

/* Sets the four drawn bits from the styles: a side is drawn when its
 * style is not none.  Call after setting border_style. */
void o42_fmt_sync_borders (O42Fmt *fmt);
const char *o42_border_style_name (O42BorderStyle style);   /* "thin", "double"... */
gboolean    o42_border_style_parse (const char *name, O42BorderStyle *style);

/* Interning compares bytes, which is only sound because every O42Fmt is
 * zeroed here before its fields are set -- that defines the padding.  Always
 * start from this function, never from an uninitialised struct. */
void o42_fmt_init_default (O42Fmt *fmt);

void o42_fmt_apply_mask (O42Fmt *fmt, O42FmtMask mask, const O42Fmt *value);

typedef struct _O42FmtTable O42FmtTable;

O42FmtTable   *o42_fmt_table_new     (void);
void           o42_fmt_table_free    (O42FmtTable *table);
O42FmtIdx      o42_fmt_table_intern  (O42FmtTable *table, const O42Fmt *fmt);
const O42Fmt  *o42_fmt_table_get     (O42FmtTable *table, O42FmtIdx idx);
O42FmtIdx      o42_fmt_table_default (O42FmtTable *table);

/* ---- Writing a value out ---------------------------------------------- */

/* The text a cell shows, which is the format applied to the value.  Text and
 * errors ignore the number format entirely: there is no sense in which
 * "hello" has two decimal places. */
char *o42_fmt_display (const O42Fmt *fmt, const O42Value *value);

/* Which way a value leans when the format says "General": numbers to the
 * right, text to the left, errors and booleans centred, as Excel does. */
O42HAlign o42_fmt_effective_halign (const O42Fmt *fmt, const O42Value *value);

/* The colour the format gives this value, if its format string names one
 * ([Red] and the rest); FALSE otherwise. */
gboolean o42_fmt_display_colour (const O42Fmt *fmt, const O42Value *value,
                                 guint32 *colour);

/* The format as a format string, custom or the preset's. Caller frees. */
char *o42_fmt_format_string (const O42Fmt *fmt);

/* Draws one border side in its style and colour, from (x0,y0) to
 * (x1,y1), for the grid, the printer and the PDF alike. */
void o42_draw_border_line (cairo_t *cr, O42BorderStyle style, guint32 colour,
                           double x0, double y0, double x1, double y1);

G_END_DECLS
