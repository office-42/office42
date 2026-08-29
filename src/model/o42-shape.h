/* o42-shape.h - a rectangle, an oval, a line or a text box over the grid
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The simplest of Excel's drawing objects, and the ones people
 * actually use on a sheet: a box to put a note in, an arrow to point
 * at a number, a line to separate two blocks.  A shape floats over the
 * grid anchored to a cell, exactly as a picture or a chart does, and
 * is drawn by the same code on screen, on paper and in a PDF.
 */

#pragma once

#include <cairo.h>

#include "o42-types.h"
#include "o42-fmt.h"

G_BEGIN_DECLS

typedef enum {
  O42_SHAPE_RECT = 0,
  O42_SHAPE_OVAL,
  O42_SHAPE_LINE,
  O42_SHAPE_ARROW,
  O42_SHAPE_TEXT,       /* a rectangle whose text is the point of it */

  /* The form controls, as on Excel's Forms toolbar: they sit on the
   * sheet and drive one cell between them, so a sheet can be worked
   * without typing into it.  O42_SHAPE_BUTTON is the first of them --
   * o42_shape_is_control says so. */
  O42_SHAPE_BUTTON,
  O42_SHAPE_CHECKBOX,
  O42_SHAPE_OPTION,     /* one of a set; the set shares a linked cell */
  O42_SHAPE_SPINNER,
  O42_SHAPE_SCROLLBAR,
  O42_SHAPE_LISTBOX,
  O42_SHAPE_COMBO,
  O42_SHAPE_LABEL,
  O42_SHAPE_GROUPBOX
} O42ShapeKind;

typedef struct {
  guint         id;         /* stable for the shape's lifetime */
  guint         group;      /* objects grouped together share one; 0 for none */
  O42ShapeKind  kind;
  int           row;        /* the anchor cell */
  int           col;
  double        dx;         /* offset inside the anchor cell, pixels */
  double        dy;
  double        width;      /* pixels */
  double        height;
  char         *text;       /* owned; may be empty */
  guint32       fill;       /* 0x00RRGGBB, or O42_FILL_NONE */
  guint32       line;       /* 0x00RRGGBB */
  double        line_width; /* pixels */

  /* Form controls only. */
  char         *link;       /* the cell it drives, "B2"; owned, may be NULL */
  char         *source;     /* a list's items come from this range; owned */
  char         *script;     /* a button runs this script; owned */
  double        value;      /* an option button's number in its set */
  double        min;        /* a spinner's and a scroll bar's bounds */
  double        max;
  double        step;       /* one click of an arrow */
  double        page;       /* one click beside a scroll bar's thumb */
} O42Shape;

O42Shape   *o42_shape_new  (O42ShapeKind kind);
void        o42_shape_free (O42Shape *shape);

/* Draws the shape into a box of `width` by `height`, at the origin.
 * A control drawn this way shows as if its linked cell were empty. */
void        o42_shape_draw (const O42Shape *shape, cairo_t *cr, double width, double height);

/* Draws a form control knowing what its linked cell says and what its
 * source range holds.  `items` may be NULL; so may `value`, for a
 * control whose cell is empty.  o42_sheet_draw_shape gathers both. */
void        o42_shape_draw_control (const O42Shape *shape, cairo_t *cr,
                                    double width, double height,
                                    gboolean has_value, double value,
                                    char **items);

gboolean    o42_shape_is_control (O42ShapeKind kind);

/* Where a control's parts are, for hit-testing and for drawing: the
 * answer is in the box's own coordinates. */
typedef enum {
  O42_CONTROL_NONE = 0,
  O42_CONTROL_BODY,       /* the button, the box, the caption */
  O42_CONTROL_UP,         /* a spinner's or scroll bar's near arrow */
  O42_CONTROL_DOWN,       /* ... and its far one */
  O42_CONTROL_PAGE_UP,    /* the track before the thumb */
  O42_CONTROL_PAGE_DOWN,  /* ... and after it */
  O42_CONTROL_THUMB,
  O42_CONTROL_ITEM        /* a row of a list box; the index comes back too */
} O42ControlPart;

O42ControlPart o42_shape_control_part (const O42Shape *shape,
                                       double width, double height,
                                       double x, double y,
                                       gboolean has_value, double value,
                                       int *item);

const char *o42_shape_kind_name  (O42ShapeKind kind);   /* "rectangle", "oval"... */
gboolean    o42_shape_kind_parse (const char *name, O42ShapeKind *kind);

G_END_DECLS
