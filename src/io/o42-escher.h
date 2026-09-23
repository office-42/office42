/* o42-escher.h - the Office Drawing records inside .xls
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pictures and notes in a BIFF8 workbook are shapes in "Escher", the
 * drawing layer Office 97 shared: a group container in the workbook
 * globals holding the images, and a drawing container per sheet with a
 * shape per object, each anchored to cells.  The bytes ride in
 * MSODRAWINGGROUP and MSODRAWING records, the latter interleaved with
 * OBJ (and, for notes, TXO) records.  This module builds and parses
 * the Escher bytes; the BIFF records around them are o42-xls.c's.
 */

#pragma once

#include <gio/gio.h>

#include "o42-shape.h"

G_BEGIN_DECLS

/* ---- writing ---- */

/* A shape to write: a picture (index into the blip store, from 1) or a
 * note (with its text), anchored by cell and fraction of a cell. */
typedef struct {
  gboolean is_note;
  gboolean is_chart;               /* a host control the chart substream fills */
  gboolean is_control;             /* a form control; its OBJ says which */
  const O42Shape *drawing;         /* a drawn shape: its outline, fill, line and text */
  double   rotation;               /* a picture: degrees, and mirrored */
  gboolean flip_h, flip_v;
  int      blip;                   /* pictures: 1-based index in the group's store */
  int      col1, row1, col2, row2; /* the anchor cells */
  double   dx1, dy1, dx2, dy2;     /* fractions of those cells, 0..1 */
  O42AnchorMode anchor_mode;       /* how a drawing, picture or chart follows the cells */
  char    *note;                   /* notes: the text */
  int      note_row, note_col;     /* notes: the cell they belong to */
} O42EscherShape;

/* The MSODRAWINGGROUP body: a store of the images, and the shape
 * counts per drawing (one drawing per sheet that has shapes). */
GBytes *o42_escher_group (GPtrArray *images, GPtrArray *formats,
                          GArray *shapes_per_drawing);

/* A sheet's drawing, cut where Excel cuts it: one Escher chunk per
 * shape, the first chunk also carrying the drawing's own headers.
 * Returns chunks (GBytes) in shape order. */
GPtrArray *o42_escher_drawing (int drawing_id, GArray *shapes);

/* ---- reading ---- */

typedef struct {
  gboolean is_picture;
  gboolean is_chart;
  int      blip;
  int      col1, row1, col2, row2;
  double   dx1, dy1, dx2, dy2;

  /* A drawn shape: what its Sp and Opt records said.  `spt` is the
   * shape type, 0 when the record had none. */
  int      spt;
  gboolean flip_h, flip_v;
  double   rotation;               /* degrees */
  gboolean filled;                 /* fFilled, on unless the Opt says otherwise */
  guint32  fill;                   /* 0x00RRGGBB */
  gboolean lined;
  guint32  line;
  double   line_width;             /* pixels */
  O42Dash  dash;
  O42Head  head_start, head_end;
  O42HeadSize head_start_size, head_end_size;
  gboolean has_text;               /* an lTxid: a TXO follows the OBJ */
  double   text_inset;             /* px, from dxTextLeft; -1 when unsaid */
  int      text_wrap;              /* WrapText: 0 square, 2 none; -1 when unsaid */
  int      fill_type;              /* fillType: 0 solid, 4-7 a shade (gradient) */
  guint32  fill_back;              /* fillBackColor: the gradient's second colour */
  double   fill_angle;             /* fillAngle, degrees as office42 counts them */
  gboolean shadow;
  guint32  shadow_colour;
  double   shadow_dx, shadow_dy;   /* px */
  O42AnchorMode anchor_mode;       /* the ClientAnchor's flags: 0 two-cell, 2 one-cell, 3 absolute */
  GArray  *path;                   /* O42PathPoint: a freeform's outline, or NULL; the caller frees */
  gboolean closed;
  gboolean path_raw;               /* the path's numbers are EMU of the shape's box, not fractions */
} O42EscherFound;

/* The images in a group container, in store order: GBytes with the
 * format name stored beside in `formats` (interned). */
void o42_escher_parse_group (const guchar *data, gsize len,
                             GPtrArray *images, GPtrArray *formats);

/* The anchored shapes in a drawing (the concatenated MSODRAWING bodies
 * of a sheet), in order. */
void o42_escher_parse_drawing (const guchar *data, gsize len, GArray *found);

G_END_DECLS
