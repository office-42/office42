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

/* The outline a rectangle-kind shape is drawn with: Excel's AutoShapes,
 * by the names Office Open XML gives them.  A shape of kind
 * O42_SHAPE_RECT or O42_SHAPE_TEXT wears one; the other kinds have
 * their own outline and ignore it. */
typedef enum {
  O42_GEOM_RECT = 0,
  O42_GEOM_ROUND_RECT,
  O42_GEOM_TRIANGLE,
  O42_GEOM_RT_TRIANGLE,
  O42_GEOM_DIAMOND,
  O42_GEOM_PENTAGON,
  O42_GEOM_HEXAGON,
  O42_GEOM_OCTAGON,
  O42_GEOM_PLUS,
  O42_GEOM_STAR4,
  O42_GEOM_STAR5,
  O42_GEOM_STAR8,
  O42_GEOM_RIGHT_ARROW,
  O42_GEOM_LEFT_ARROW,
  O42_GEOM_UP_ARROW,
  O42_GEOM_DOWN_ARROW,
  O42_GEOM_LEFT_RIGHT_ARROW,
  O42_GEOM_RECT_CALLOUT,
  O42_GEOM_ELLIPSE_CALLOUT,
  O42_GEOM_FLOW_PROCESS,
  O42_GEOM_FLOW_DECISION,
  O42_GEOM_FLOW_TERMINATOR,
  O42_N_GEOMS
} O42ShapeGeom;

/* How an outline is dashed, by Office Open XML's names. */
typedef enum {
  O42_DASH_SOLID = 0,
  O42_DASH_DASH,
  O42_DASH_DOT,
  O42_DASH_DASH_DOT,
  O42_DASH_LONG_DASH,
  O42_DASH_SYS_DASH,
  O42_DASH_SYS_DOT,
  O42_N_DASHES
} O42Dash;

/* The head at either end of a line, likewise. */
typedef enum {
  O42_HEAD_NONE = 0,
  O42_HEAD_TRIANGLE,
  O42_HEAD_STEALTH,
  O42_HEAD_DIAMOND,
  O42_HEAD_OVAL,
  O42_HEAD_ARROW,       /* two open strokes */
  O42_N_HEADS
} O42Head;

typedef enum {
  O42_HEAD_SMALL = 0,
  O42_HEAD_MEDIUM,
  O42_HEAD_LARGE
} O42HeadSize;

typedef struct {
  guint         id;         /* stable for the shape's lifetime */
  guint         group;      /* objects grouped together share one; 0 for none */
  guint         z;          /* the painting order: higher is nearer the front */
  O42ShapeKind  kind;
  O42ShapeGeom  geom;       /* the outline of a rectangle kind */
  O42Dash       dash;       /* how the outline is dashed */
  O42Head       head_start; /* a line's heads: at its first point ... */
  O42Head       head_end;   /* ... and at its last; an arrow is a line with one here */
  O42HeadSize   head_start_size;
  O42HeadSize   head_end_size;
  int           row;        /* the anchor cell */
  int           col;
  double        dx;         /* offset inside the anchor cell, pixels */
  double        dy;
  double        width;      /* pixels */
  double        height;
  double        rotation;   /* degrees clockwise about the box's centre */
  gboolean      flip_h;     /* mirrored left to right, before turning */
  gboolean      flip_v;
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

/* A copy with a fresh identity: everything but the id and the group. */
O42Shape   *o42_shape_copy (const O42Shape *shape);

/* The outlines, by four names each: office42's own ("roundrect"), the
 * label a menu shows ("Rounded Rectangle"), Office Open XML's
 * ("roundRect") and OpenDocument's ("round-rectangle"). */
const char *o42_shape_geom_name  (O42ShapeGeom geom);
const char *o42_shape_geom_label (O42ShapeGeom geom);
gboolean    o42_shape_geom_parse (const char *name, O42ShapeGeom *geom);

/* What a file calls this shape's outline, and the reverse: the kind and
 * the geometry from the name Office Open XML ("ellipse", "star5"),
 * OpenDocument ("round-callout") or Escher (a shape type number) uses.
 * A name none of them knows comes back as a rectangle. */
const char *o42_shape_prst        (const O42Shape *shape);
void        o42_shape_apply_prst  (O42Shape *shape, const char *prst);
const char *o42_shape_ods_type    (const O42Shape *shape);   /* NULL for a plain rect, ellipse or line */
gboolean    o42_shape_apply_ods_type (O42Shape *shape, const char *type);
int         o42_shape_spt         (const O42Shape *shape);
gboolean    o42_shape_apply_spt   (O42Shape *shape, int spt);

/* The dashes and heads by Office Open XML's names ("dashDot",
 * "stealth"), which .gnumeric uses as well. */
const char *o42_dash_name  (O42Dash dash);
gboolean    o42_dash_parse (const char *name, O42Dash *dash);
const char *o42_head_name  (O42Head head);
gboolean    o42_head_parse (const char *name, O42Head *head);

/* Adds the outline of a rectangle-kind shape to the current path, in
 * the box (0, 0, width, height) less `inset` all round. */
void        o42_shape_geom_path   (O42ShapeGeom geom, cairo_t *cr,
                                   double width, double height, double inset);

G_END_DECLS
