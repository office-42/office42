/* o42-shape.c - a rectangle, an oval, a line or a text box over the grid
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-shape.h"

#include <pango/pangocairo.h>
#include <string.h>
#include <math.h>

O42Shape *
o42_shape_new (O42ShapeKind kind)
{
  O42Shape *shape = g_new0 (O42Shape, 1);

  static const struct { double width, height; } CONTROL_SIZE[] = {
    { 100, 26 },   /* button */
    { 130, 20 },   /* check box */
    { 130, 20 },   /* option button */
    {  20, 40 },   /* spinner */
    { 160, 20 },   /* scroll bar */
    { 130, 80 },   /* list box */
    { 130, 22 },   /* combo box */
    { 110, 20 },   /* label */
    { 160, 100 }   /* group box */
  };

  shape->kind = kind;
  shape->text = g_strdup ("");
  shape->fill = (kind == O42_SHAPE_LINE || kind == O42_SHAPE_ARROW) ? O42_FILL_NONE
              : (kind == O42_SHAPE_TEXT) ? 0xFFFFCC : 0xDCE6F1;
  shape->line = 0x1F497D;
  shape->line_width = 1.5;
  if (kind == O42_SHAPE_ARROW)
    shape->head_end = O42_HEAD_TRIANGLE;
  shape->head_start_size = shape->head_end_size = O42_HEAD_MEDIUM;
  shape->width = (kind == O42_SHAPE_LINE || kind == O42_SHAPE_ARROW) ? 120 : 140;
  shape->height = (kind == O42_SHAPE_LINE || kind == O42_SHAPE_ARROW) ? 0 : 60;
  if (o42_shape_is_control (kind))
    {
      shape->width = CONTROL_SIZE[kind - O42_SHAPE_BUTTON].width;
      shape->height = CONTROL_SIZE[kind - O42_SHAPE_BUTTON].height;
      shape->max = 100;
      shape->step = 1;
      shape->page = 10;
    }
  return shape;
}

void
o42_shape_free (O42Shape *shape)
{
  if (shape == NULL)
    return;
  g_free (shape->text);
  g_free (shape->link);
  g_free (shape->source);
  g_free (shape->script);
  g_free (shape);
}

static const char *KIND_NAMES[] = {
  "rectangle", "oval", "line", "arrow", "textbox",
  "button", "checkbox", "option", "spinner", "scrollbar",
  "listbox", "combo", "label", "groupbox"
};

const char *
o42_shape_kind_name (O42ShapeKind kind)
{
  return (guint) kind < G_N_ELEMENTS (KIND_NAMES) ? KIND_NAMES[kind] : "rectangle";
}

gboolean
o42_shape_kind_parse (const char *name, O42ShapeKind *kind)
{
  for (guint i = 0; name != NULL && i < G_N_ELEMENTS (KIND_NAMES); i++)
    if (g_ascii_strcasecmp (name, KIND_NAMES[i]) == 0)
      { *kind = (O42ShapeKind) i; return TRUE; }
  return FALSE;
}

O42Shape *
o42_shape_copy (const O42Shape *shape)
{
  O42Shape *copy;

  g_return_val_if_fail (shape != NULL, NULL);
  copy = g_new (O42Shape, 1);
  *copy = *shape;
  copy->id = 0;
  copy->group = 0;
  copy->text = g_strdup (shape->text != NULL ? shape->text : "");
  copy->link = g_strdup (shape->link);
  copy->source = g_strdup (shape->source);
  copy->script = g_strdup (shape->script);
  return copy;
}

/* ---- The outlines ------------------------------------------------------ */

/* One row per geometry, in enum order.  `spt` is Escher's shape type
 * number, the one .xls files carry. */
static const struct {
  const char *name;
  const char *label;
  const char *prst;
  const char *ods;
  int         spt;
} GEOMS[O42_N_GEOMS] = {
  { "rect",            "Rectangle",           "rect",                "rectangle",            1   },
  { "roundrect",       "Rounded Rectangle",   "roundRect",           "round-rectangle",      2   },
  { "triangle",        "Triangle",            "triangle",            "isosceles-triangle",   5   },
  { "rttriangle",      "Right Triangle",      "rtTriangle",          "right-triangle",       6   },
  { "diamond",         "Diamond",             "diamond",             "diamond",              4   },
  { "pentagon",        "Pentagon",            "pentagon",            "pentagon",             56  },
  { "hexagon",         "Hexagon",             "hexagon",             "hexagon",              9   },
  { "octagon",         "Octagon",             "octagon",             "octagon",              10  },
  { "plus",            "Cross",               "plus",                "cross",                11  },
  { "star4",           "4-Point Star",        "star4",               "star4",                187 },
  { "star5",           "5-Point Star",        "star5",               "star5",                12  },
  { "star8",           "8-Point Star",        "star8",               "star8",                58  },
  { "rightarrow",      "Right Arrow",         "rightArrow",          "right-arrow",          13  },
  { "leftarrow",       "Left Arrow",          "leftArrow",           "left-arrow",           66  },
  { "uparrow",         "Up Arrow",            "upArrow",             "up-arrow",             68  },
  { "downarrow",       "Down Arrow",          "downArrow",           "down-arrow",           67  },
  { "leftrightarrow",  "Left-Right Arrow",    "leftRightArrow",      "left-right-arrow",     69  },
  { "rectcallout",     "Rectangular Callout", "wedgeRectCallout",    "rectangular-callout",  61  },
  { "ellipsecallout",  "Oval Callout",        "wedgeEllipseCallout", "round-callout",        63  },
  { "flowprocess",     "Flowchart: Process",  "flowChartProcess",    "flowchart-process",    109 },
  { "flowdecision",    "Flowchart: Decision", "flowChartDecision",   "flowchart-decision",   110 },
  { "flowterminator",  "Flowchart: Terminator", "flowChartTerminator", "flowchart-terminator", 116 },
};

const char *
o42_shape_geom_name (O42ShapeGeom geom)
{
  return (guint) geom < O42_N_GEOMS ? GEOMS[geom].name : GEOMS[0].name;
}

const char *
o42_shape_geom_label (O42ShapeGeom geom)
{
  return (guint) geom < O42_N_GEOMS ? GEOMS[geom].label : GEOMS[0].label;
}

gboolean
o42_shape_geom_parse (const char *name, O42ShapeGeom *geom)
{
  for (guint i = 0; name != NULL && i < O42_N_GEOMS; i++)
    if (g_ascii_strcasecmp (name, GEOMS[i].name) == 0)
      { *geom = (O42ShapeGeom) i; return TRUE; }
  return FALSE;
}

/* Whether the shape wears an outline from the table at all. */
static gboolean
has_geom (const O42Shape *shape)
{
  return shape->kind == O42_SHAPE_RECT || shape->kind == O42_SHAPE_TEXT;
}

const char *
o42_shape_prst (const O42Shape *shape)
{
  g_return_val_if_fail (shape != NULL, "rect");
  switch (shape->kind)
    {
    case O42_SHAPE_OVAL:  return "ellipse";
    case O42_SHAPE_LINE:
    case O42_SHAPE_ARROW: return "line";
    default:              return has_geom (shape) && (guint) shape->geom < O42_N_GEOMS
                                 ? GEOMS[shape->geom].prst : "rect";
    }
}

void
o42_shape_apply_prst (O42Shape *shape, const char *prst)
{
  g_return_if_fail (shape != NULL);
  shape->geom = O42_GEOM_RECT;
  if (prst == NULL)
    return;
  if (g_ascii_strcasecmp (prst, "ellipse") == 0)
    { shape->kind = O42_SHAPE_OVAL; return; }
  if (g_ascii_strcasecmp (prst, "line") == 0 || g_str_has_prefix (prst, "straightConnector"))
    { if (shape->kind != O42_SHAPE_ARROW) shape->kind = O42_SHAPE_LINE; return; }
  for (guint i = 0; i < O42_N_GEOMS; i++)
    if (g_ascii_strcasecmp (prst, GEOMS[i].prst) == 0)
      {
        shape->geom = (O42ShapeGeom) i;
        if (!has_geom (shape))
          shape->kind = O42_SHAPE_RECT;
        return;
      }
  if (!has_geom (shape))
    shape->kind = O42_SHAPE_RECT;
}

const char *
o42_shape_ods_type (const O42Shape *shape)
{
  g_return_val_if_fail (shape != NULL, NULL);
  if (!has_geom (shape) || shape->geom == O42_GEOM_RECT || (guint) shape->geom >= O42_N_GEOMS)
    return NULL;
  return GEOMS[shape->geom].ods;
}

gboolean
o42_shape_apply_ods_type (O42Shape *shape, const char *type)
{
  g_return_val_if_fail (shape != NULL, FALSE);
  shape->geom = O42_GEOM_RECT;
  if (type == NULL)
    return FALSE;
  /* LibreOffice keeps a shape it read from Excel under Excel's name. */
  if (g_str_has_prefix (type, "ooxml-"))
    { o42_shape_apply_prst (shape, type + 6); return TRUE; }
  if (g_ascii_strcasecmp (type, "ellipse") == 0)
    { shape->kind = O42_SHAPE_OVAL; return TRUE; }
  for (guint i = 0; i < O42_N_GEOMS; i++)
    if (g_ascii_strcasecmp (type, GEOMS[i].ods) == 0)
      {
        shape->geom = (O42ShapeGeom) i;
        if (!has_geom (shape))
          shape->kind = O42_SHAPE_RECT;
        return TRUE;
      }
  return FALSE;
}

int
o42_shape_spt (const O42Shape *shape)
{
  g_return_val_if_fail (shape != NULL, 1);
  switch (shape->kind)
    {
    case O42_SHAPE_OVAL:  return 3;
    case O42_SHAPE_LINE:
    case O42_SHAPE_ARROW: return 20;
    case O42_SHAPE_TEXT:  return shape->geom == O42_GEOM_RECT ? 202 : GEOMS[shape->geom].spt;
    default:              return has_geom (shape) && (guint) shape->geom < O42_N_GEOMS
                                 ? GEOMS[shape->geom].spt : 1;
    }
}

gboolean
o42_shape_apply_spt (O42Shape *shape, int spt)
{
  g_return_val_if_fail (shape != NULL, FALSE);
  shape->geom = O42_GEOM_RECT;
  switch (spt)
    {
    case 3:   shape->kind = O42_SHAPE_OVAL; return TRUE;
    case 20:  if (shape->kind != O42_SHAPE_ARROW) shape->kind = O42_SHAPE_LINE; return TRUE;
    case 202: shape->kind = O42_SHAPE_TEXT; return TRUE;
    default:  break;
    }
  for (guint i = 0; i < O42_N_GEOMS; i++)
    if (GEOMS[i].spt == spt)
      {
        shape->geom = (O42ShapeGeom) i;
        if (!has_geom (shape))
          shape->kind = O42_SHAPE_RECT;
        return TRUE;
      }
  return FALSE;
}

/* A closed polygon from a list of points. */
static void
polygon (cairo_t *cr, const double *pts, int n)
{
  cairo_move_to (cr, pts[0], pts[1]);
  for (int i = 1; i < n; i++)
    cairo_line_to (cr, pts[2 * i], pts[2 * i + 1]);
  cairo_close_path (cr);
}

/* A rectangle with its corners rounded to `r`. */
static void
rounded (cairo_t *cr, double x, double y, double w, double h, double r)
{
  r = MIN (r, MIN (w, h) / 2);
  cairo_new_sub_path (cr);
  cairo_arc (cr, x + w - r, y + r, r, -G_PI / 2, 0);
  cairo_arc (cr, x + w - r, y + h - r, r, 0, G_PI / 2);
  cairo_arc (cr, x + r, y + h - r, r, G_PI / 2, G_PI);
  cairo_arc (cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
  cairo_close_path (cr);
}

/* A star of `n` points in the box, the inner radius `inner` of the outer. */
static void
star (cairo_t *cr, double x, double y, double w, double h, int n, double inner)
{
  double cx = x + w / 2, cy = y + h / 2;

  for (int i = 0; i < 2 * n; i++)
    {
      double a = -G_PI / 2 + i * G_PI / n;
      double r = (i % 2 == 0) ? 1.0 : inner;
      double px = cx + cos (a) * r * w / 2, py = cy + sin (a) * r * h / 2;

      if (i == 0)
        cairo_move_to (cr, px, py);
      else
        cairo_line_to (cr, px, py);
    }
  cairo_close_path (cr);
}

/* A block arrow along an axis: `len` long, `thick` across, the head
 * `head` long and the shaft half the thickness, as Excel's defaults have
 * it.  The points come out as (along, across) pairs, seven of them. */
static void
block_arrow (double len, double thick, double head, double *pts)
{
  const double a[7] = { 0, len - head, len - head, len, len - head, len - head, 0 };
  const double c[7] = { thick / 4, thick / 4, 0, thick / 2, thick, 3 * thick / 4, 3 * thick / 4 };

  for (int i = 0; i < 7; i++)
    { pts[2 * i] = a[i]; pts[2 * i + 1] = c[i]; }
}

void
o42_shape_geom_path (O42ShapeGeom geom, cairo_t *cr, double width, double height, double inset)
{
  double x = inset, y = inset;
  double w = MAX (width - 2 * inset, 1), h = MAX (height - 2 * inset, 1);
  double m = MIN (w, h);
  double pts[24];

  g_return_if_fail (cr != NULL);

  switch (geom)
    {
    case O42_GEOM_ROUND_RECT:
      rounded (cr, x, y, w, h, m / 6);
      break;

    case O42_GEOM_FLOW_TERMINATOR:
      rounded (cr, x, y, w, h, h / 2);
      break;

    case O42_GEOM_TRIANGLE:
      { double p[] = { x + w / 2, y, x + w, y + h, x, y + h }; polygon (cr, p, 3); }
      break;

    case O42_GEOM_RT_TRIANGLE:
      { double p[] = { x, y, x + w, y + h, x, y + h }; polygon (cr, p, 3); }
      break;

    case O42_GEOM_DIAMOND:
    case O42_GEOM_FLOW_DECISION:
      { double p[] = { x + w / 2, y, x + w, y + h / 2, x + w / 2, y + h, x, y + h / 2 }; polygon (cr, p, 4); }
      break;

    case O42_GEOM_PENTAGON:
      for (int i = 0; i < 5; i++)
        {
          double a = -G_PI / 2 + i * 2 * G_PI / 5;
          pts[2 * i] = x + w / 2 + cos (a) * w / 2;
          pts[2 * i + 1] = y + h / 2 + sin (a) * h / 2;
        }
      polygon (cr, pts, 5);
      break;

    case O42_GEOM_HEXAGON:
      { double p[] = { x + w / 4, y, x + 3 * w / 4, y, x + w, y + h / 2,
                       x + 3 * w / 4, y + h, x + w / 4, y + h, x, y + h / 2 };
        polygon (cr, p, 6); }
      break;

    case O42_GEOM_OCTAGON:
      {
        double c = m * 0.29289;
        double p[] = { x + c, y, x + w - c, y, x + w, y + c, x + w, y + h - c,
                       x + w - c, y + h, x + c, y + h, x, y + h - c, x, y + c };
        polygon (cr, p, 8);
      }
      break;

    case O42_GEOM_PLUS:
      {
        double c = m / 4;
        double p[] = { x + c, y, x + w - c, y, x + w - c, y + c, x + w, y + c,
                       x + w, y + h - c, x + w - c, y + h - c, x + w - c, y + h,
                       x + c, y + h, x + c, y + h - c, x, y + h - c, x, y + c, x + c, y + c };
        polygon (cr, p, 12);
      }
      break;

    case O42_GEOM_STAR4: star (cr, x, y, w, h, 4, 0.45); break;
    case O42_GEOM_STAR5: star (cr, x, y, w, h, 5, 0.382); break;
    case O42_GEOM_STAR8: star (cr, x, y, w, h, 8, 0.72); break;

    case O42_GEOM_RIGHT_ARROW:
    case O42_GEOM_LEFT_ARROW:
      block_arrow (w, h, MIN (m / 2, w), pts);
      for (int i = 0; i < 7; i++)
        {
          double a = pts[2 * i], c = pts[2 * i + 1];
          pts[2 * i] = x + (geom == O42_GEOM_LEFT_ARROW ? w - a : a);
          pts[2 * i + 1] = y + c;
        }
      polygon (cr, pts, 7);
      break;

    case O42_GEOM_DOWN_ARROW:
    case O42_GEOM_UP_ARROW:
      block_arrow (h, w, MIN (m / 2, h), pts);
      for (int i = 0; i < 7; i++)
        {
          double a = pts[2 * i], c = pts[2 * i + 1];
          pts[2 * i] = x + c;
          pts[2 * i + 1] = y + (geom == O42_GEOM_UP_ARROW ? h - a : a);
        }
      polygon (cr, pts, 7);
      break;

    case O42_GEOM_LEFT_RIGHT_ARROW:
      {
        double hd = MIN (m / 2, w / 3);
        double p[] = { x, y + h / 2, x + hd, y, x + hd, y + h / 4, x + w - hd, y + h / 4,
                       x + w - hd, y, x + w, y + h / 2, x + w - hd, y + h, x + w - hd, y + 3 * h / 4,
                       x + hd, y + 3 * h / 4, x + hd, y + h };
        polygon (cr, p, 10);
      }
      break;

    case O42_GEOM_RECT_CALLOUT:
      {
        /* The box takes the top three quarters; the wedge points down
         * and to the left from its bottom edge. */
        double b = y + 3 * h / 4;
        double p[] = { x, y, x + w, y, x + w, b, x + w / 2, b, x + w / 4, y + h,
                       x + w / 3, b, x, b };
        polygon (cr, p, 7);
      }
      break;

    case O42_GEOM_ELLIPSE_CALLOUT:
      {
        double rx = w / 2, ry = 3 * h / 8;
        double cx = x + rx, cy = y + ry;

        cairo_new_sub_path (cr);
        cairo_save (cr);
        cairo_translate (cr, cx, cy);
        cairo_scale (cr, rx, ry);
        cairo_arc (cr, 0, 0, 1, 125 * G_PI / 180, 460 * G_PI / 180);
        cairo_restore (cr);
        cairo_line_to (cr, x + w / 4, y + h);
        cairo_close_path (cr);
      }
      break;

    case O42_GEOM_RECT:
    case O42_GEOM_FLOW_PROCESS:
    default:
      cairo_rectangle (cr, x, y, w, h);
      break;
    }
}

static const char *DASH_NAMES[O42_N_DASHES] = {
  "solid", "dash", "dot", "dashDot", "lgDash", "sysDash", "sysDot"
};
static const char *HEAD_NAMES[O42_N_HEADS] = {
  "none", "triangle", "stealth", "diamond", "oval", "arrow"
};

const char *
o42_dash_name (O42Dash dash)
{
  return (guint) dash < O42_N_DASHES ? DASH_NAMES[dash] : DASH_NAMES[0];
}

gboolean
o42_dash_parse (const char *name, O42Dash *dash)
{
  for (guint i = 0; name != NULL && i < O42_N_DASHES; i++)
    if (g_ascii_strcasecmp (name, DASH_NAMES[i]) == 0)
      { *dash = (O42Dash) i; return TRUE; }
  return FALSE;
}

const char *
o42_head_name (O42Head head)
{
  return (guint) head < O42_N_HEADS ? HEAD_NAMES[head] : HEAD_NAMES[0];
}

gboolean
o42_head_parse (const char *name, O42Head *head)
{
  for (guint i = 0; name != NULL && i < O42_N_HEADS; i++)
    if (g_ascii_strcasecmp (name, HEAD_NAMES[i]) == 0)
      { *head = (O42Head) i; return TRUE; }
  return FALSE;
}

/* The dash pattern, in units of the line's width, as Office draws
 * each of them. */
static void
set_dash (cairo_t *cr, O42Dash dash, double line_width)
{
  static const double PATTERNS[O42_N_DASHES][4] = {
    { 0 },
    { 4, 3 },
    { 1, 3 },
    { 4, 3, 1, 3 },
    { 8, 3 },
    { 3, 1 },
    { 1, 1 }
  };
  static const int COUNTS[O42_N_DASHES] = { 0, 2, 2, 4, 2, 2, 2 };
  double unit = MAX (line_width, 1);
  double scaled[4];

  if ((guint) dash >= O42_N_DASHES || COUNTS[dash] == 0)
    {
      cairo_set_dash (cr, NULL, 0, 0);
      return;
    }
  for (int i = 0; i < COUNTS[dash]; i++)
    scaled[i] = PATTERNS[dash][i] * unit;
  cairo_set_dash (cr, scaled, COUNTS[dash], 0);
}

/* A head at (x, y), pointing along `angle`, of a line `line_width`
 * wide: filled for the closed kinds, two strokes for the open arrow.
 * Returns how far back from the point the line should stop, so that a
 * dashed line does not show through a filled head. */
static double
draw_head (cairo_t *cr, double x, double y, double angle, O42Head head,
           O42HeadSize size, double line_width)
{
  static const double SCALE[3] = { 0.7, 1.0, 1.5 };
  double len = MAX (7, line_width * 3.5) * SCALE[CLAMP (size, 0, 2)];
  double c = cos (angle), s = sin (angle);
  double back = 0;

  cairo_save (cr);
  cairo_set_dash (cr, NULL, 0, 0);
  switch (head)
    {
    case O42_HEAD_TRIANGLE:
      cairo_move_to (cr, x, y);
      cairo_line_to (cr, x - len * cos (angle - G_PI / 7), y - len * sin (angle - G_PI / 7));
      cairo_line_to (cr, x - len * cos (angle + G_PI / 7), y - len * sin (angle + G_PI / 7));
      cairo_close_path (cr);
      cairo_fill (cr);
      back = len * 0.8;
      break;

    case O42_HEAD_STEALTH:
      cairo_move_to (cr, x, y);
      cairo_line_to (cr, x - len * cos (angle - G_PI / 6), y - len * sin (angle - G_PI / 6));
      cairo_line_to (cr, x - len * 0.6 * c, y - len * 0.6 * s);
      cairo_line_to (cr, x - len * cos (angle + G_PI / 6), y - len * sin (angle + G_PI / 6));
      cairo_close_path (cr);
      cairo_fill (cr);
      back = len * 0.5;
      break;

    case O42_HEAD_DIAMOND:
      {
        double h = len * 0.5;

        cairo_move_to (cr, x, y);
        cairo_line_to (cr, x - h * c - h * s, y - h * s + h * c);
        cairo_line_to (cr, x - len * c, y - len * s);
        cairo_line_to (cr, x - h * c + h * s, y - h * s - h * c);
        cairo_close_path (cr);
        cairo_fill (cr);
        back = len;
      }
      break;

    case O42_HEAD_OVAL:
      cairo_new_sub_path (cr);
      cairo_arc (cr, x - len * 0.5 * c, y - len * 0.5 * s, len * 0.5, 0, 2 * G_PI);
      cairo_fill (cr);
      back = len;
      break;

    case O42_HEAD_ARROW:
      cairo_set_line_width (cr, line_width);
      cairo_move_to (cr, x - len * cos (angle - G_PI / 6), y - len * sin (angle - G_PI / 6));
      cairo_line_to (cr, x, y);
      cairo_line_to (cr, x - len * cos (angle + G_PI / 6), y - len * sin (angle + G_PI / 6));
      cairo_stroke (cr);
      break;

    default:
      break;
    }
  cairo_restore (cr);
  return back;
}

static void
set_rgb (cairo_t *cr, guint32 colour)
{
  cairo_set_source_rgb (cr, ((colour >> 16) & 0xFF) / 255.0,
                        ((colour >> 8) & 0xFF) / 255.0, (colour & 0xFF) / 255.0);
}

void
o42_shape_draw (const O42Shape *shape, cairo_t *cr, double width, double height)
{
  double inset;

  g_return_if_fail (shape != NULL && cr != NULL);
  if (o42_shape_is_control (shape->kind))
    {
      o42_shape_draw_control (shape, cr, width, height, FALSE, 0, NULL);
      return;
    }
  inset = shape->line_width / 2;

  cairo_save (cr);
  cairo_new_path (cr);   /* cairo_save does not keep the path out of our way */
  cairo_set_line_width (cr, shape->line_width);
  set_dash (cr, shape->dash, shape->line_width);

  switch (shape->kind)
    {
    case O42_SHAPE_LINE:
    case O42_SHAPE_ARROW:
      {
        /* The heads first, each along the line and pointing out of it;
         * the line then runs between what they leave of its ends. */
        double angle = atan2 (height, width);
        double length = hypot (width, height);
        double from = 0, to = length;

        set_rgb (cr, shape->line);
        if (shape->head_start != O42_HEAD_NONE)
          from = draw_head (cr, 0, 0, angle + G_PI, shape->head_start,
                            shape->head_start_size, shape->line_width);
        if (shape->head_end != O42_HEAD_NONE)
          to = length - draw_head (cr, width, height, angle, shape->head_end,
                                   shape->head_end_size, shape->line_width);
        if (to > from)
          {
            cairo_move_to (cr, from * cos (angle), from * sin (angle));
            cairo_line_to (cr, to * cos (angle), to * sin (angle));
            cairo_stroke (cr);
          }
      }
      break;

    case O42_SHAPE_OVAL:
      cairo_save (cr);
      cairo_translate (cr, width / 2, height / 2);
      cairo_scale (cr, MAX (width / 2 - inset, 1), MAX (height / 2 - inset, 1));
      cairo_new_sub_path (cr);   /* or the arc is joined to wherever the pen was */
      cairo_arc (cr, 0, 0, 1, 0, 2 * G_PI);
      cairo_restore (cr);
      if (shape->fill != O42_FILL_NONE)
        {
          set_rgb (cr, shape->fill);
          cairo_fill_preserve (cr);
        }
      set_rgb (cr, shape->line);
      cairo_stroke (cr);
      break;

    default:
      o42_shape_geom_path (shape->geom, cr, width, height, inset);
      if (shape->fill != O42_FILL_NONE)
        {
          set_rgb (cr, shape->fill);
          cairo_fill_preserve (cr);
        }
      set_rgb (cr, shape->line);
      cairo_stroke (cr);
      break;
    }

  /* The text, wrapped and centred, except on a line where it sits at
   * the start. */
  if (shape->text != NULL && *shape->text != '\0')
    {
      PangoLayout *layout = pango_cairo_create_layout (cr);
      PangoFontDescription *desc = pango_font_description_from_string ("Arial 10");
      int tw, th;

      pango_layout_set_font_description (layout, desc);
      pango_layout_set_text (layout, shape->text, -1);
      if (shape->kind != O42_SHAPE_LINE && shape->kind != O42_SHAPE_ARROW)
        {
          pango_layout_set_width (layout, (int) MAX (width - 8, 8) * PANGO_SCALE);
          pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
          pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
        }
      pango_layout_get_pixel_size (layout, &tw, &th);
      cairo_set_source_rgb (cr, 0, 0, 0);
      if (shape->kind == O42_SHAPE_LINE || shape->kind == O42_SHAPE_ARROW)
        cairo_move_to (cr, 2, -th - 2);
      else
        cairo_move_to (cr, 4, MAX ((height - th) / 2, 2));
      pango_cairo_show_layout (cr, layout);
      pango_font_description_free (desc);
      g_object_unref (layout);
    }

  cairo_restore (cr);
}

/* ---- Form controls ---------------------------------------------------- */

gboolean
o42_shape_is_control (O42ShapeKind kind)
{
  return kind >= O42_SHAPE_BUTTON && kind <= O42_SHAPE_GROUPBOX;
}

#define CONTROL_BOX   13.0   /* the tick box and the option's circle */
#define CONTROL_ROW   16.0   /* one row of a list box */

/* A scroll bar lies the long way; a spinner always stands up. */
static gboolean
control_horizontal (const O42Shape *shape, double width, double height)
{
  return shape->kind == O42_SHAPE_SCROLLBAR && width >= height;
}

/* The arrow button at either end, and the track between them. */
static void
scroll_geometry (const O42Shape *shape, double width, double height,
                 double *arrow, double *track_start, double *track_length)
{
  gboolean flat = control_horizontal (shape, width, height);
  double length = flat ? width : height;
  double thick = flat ? height : width;
  double a = MIN (thick, length / 3);

  *arrow = MAX (a, 1);
  *track_start = *arrow;
  *track_length = MAX (length - 2 * *arrow, 1);
}

/* How far along its track the thumb sits, and how big it is. */
static void
thumb_geometry (const O42Shape *shape, double width, double height,
                gboolean has_value, double value, double *pos, double *size)
{
  double arrow, start, track, span, where;

  scroll_geometry (shape, width, height, &arrow, &start, &track);
  span = shape->max - shape->min;
  where = has_value ? CLAMP ((value - shape->min) / (span > 0 ? span : 1), 0, 1) : 0;
  *size = CLAMP (track * (shape->page > 0 ? shape->page / (span + shape->page) : 0.2),
                 MIN (12, track), track);
  *pos = start + where * (track - *size);
}

O42ControlPart
o42_shape_control_part (const O42Shape *shape, double width, double height,
                        double x, double y, gboolean has_value, double value,
                        int *item)
{
  g_return_val_if_fail (shape != NULL, O42_CONTROL_NONE);
  if (item != NULL)
    *item = 0;
  if (x < 0 || y < 0 || x > width || y > height)
    return O42_CONTROL_NONE;

  switch (shape->kind)
    {
    case O42_SHAPE_SPINNER:
      return y < height / 2 ? O42_CONTROL_UP : O42_CONTROL_DOWN;

    case O42_SHAPE_SCROLLBAR:
      {
        gboolean flat = control_horizontal (shape, width, height);
        double along = flat ? x : y;
        double arrow, start, track, pos, size;

        scroll_geometry (shape, width, height, &arrow, &start, &track);
        thumb_geometry (shape, width, height, has_value, value, &pos, &size);
        if (along < start)
          return O42_CONTROL_UP;
        if (along > start + track)
          return O42_CONTROL_DOWN;
        if (along < pos)
          return O42_CONTROL_PAGE_UP;
        if (along > pos + size)
          return O42_CONTROL_PAGE_DOWN;
        return O42_CONTROL_THUMB;
      }

    case O42_SHAPE_LISTBOX:
      if (item != NULL)
        *item = (int) ((y - 2) / CONTROL_ROW) + 1;
      return O42_CONTROL_ITEM;

    case O42_SHAPE_LABEL:
    case O42_SHAPE_GROUPBOX:
      return O42_CONTROL_NONE;   /* they do nothing when clicked */

    default:
      return O42_CONTROL_BODY;
    }
}

/* The face of a button or a scroll bar's arrow: grey, with a light edge
 * on the top and left and a dark one on the bottom and right, the way
 * every control looked in 1993. */
static void
draw_bevel (cairo_t *cr, double x, double y, double w, double h, gboolean sunken)
{
  cairo_set_source_rgb (cr, 0.847, 0.847, 0.847);
  cairo_rectangle (cr, x, y, w, h);
  cairo_fill (cr);

  cairo_set_line_width (cr, 1);
  cairo_set_source_rgb (cr, sunken ? 0.5 : 1, sunken ? 0.5 : 1, sunken ? 0.5 : 1);
  cairo_move_to (cr, x + 0.5, y + h - 0.5);
  cairo_line_to (cr, x + 0.5, y + 0.5);
  cairo_line_to (cr, x + w - 0.5, y + 0.5);
  cairo_stroke (cr);
  cairo_set_source_rgb (cr, sunken ? 1 : 0.5, sunken ? 1 : 0.5, sunken ? 1 : 0.5);
  cairo_move_to (cr, x + w - 0.5, y + 0.5);
  cairo_line_to (cr, x + w - 0.5, y + h - 0.5);
  cairo_line_to (cr, x + 0.5, y + h - 0.5);
  cairo_stroke (cr);
}

/* A triangle pointing up, down, left or right, inside a box. */
static void
draw_arrow (cairo_t *cr, double x, double y, double w, double h, int dx, int dy)
{
  double cx = x + w / 2, cy = y + h / 2;
  double r = MAX (MIN (w, h) / 4, 2);

  cairo_set_source_rgb (cr, 0, 0, 0);
  if (dy != 0)
    {
      cairo_move_to (cr, cx, cy + dy * r);
      cairo_line_to (cr, cx - r, cy - dy * r);
      cairo_line_to (cr, cx + r, cy - dy * r);
    }
  else
    {
      cairo_move_to (cr, cx + dx * r, cy);
      cairo_line_to (cr, cx - dx * r, cy - r);
      cairo_line_to (cr, cx - dx * r, cy + r);
    }
  cairo_close_path (cr);
  cairo_fill (cr);
}

/* Arial 10, the face every control's caption is set in. */
static PangoLayout *
control_layout (cairo_t *cr, const char *text)
{
  PangoLayout *layout = pango_cairo_create_layout (cr);
  PangoFontDescription *desc = pango_font_description_from_string ("Arial 10");

  pango_layout_set_font_description (layout, desc);
  pango_layout_set_text (layout, text != NULL ? text : "", -1);
  pango_font_description_free (desc);
  return layout;
}

static void
draw_caption (cairo_t *cr, const char *text, double x, double y, double w, double h,
              gboolean centred)
{
  PangoLayout *layout;
  int tw, th;

  if (text == NULL || *text == '\0')
    return;
  layout = control_layout (cr, text);
  if (centred)
    {
      pango_layout_set_width (layout, (int) MAX (w, 8) * PANGO_SCALE);
      pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
    }
  pango_layout_get_pixel_size (layout, &tw, &th);
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_move_to (cr, x, y + MAX ((h - th) / 2, 0));
  pango_cairo_show_layout (cr, layout);
  g_object_unref (layout);
}

void
o42_shape_draw_control (const O42Shape *shape, cairo_t *cr,
                        double width, double height,
                        gboolean has_value, double value, char **items)
{
  g_return_if_fail (shape != NULL && cr != NULL);

  cairo_save (cr);
  cairo_new_path (cr);
  cairo_set_line_width (cr, 1);

  switch (shape->kind)
    {
    case O42_SHAPE_BUTTON:
      draw_bevel (cr, 0, 0, width, height, FALSE);
      draw_caption (cr, shape->text, 0, 0, width, height, TRUE);
      break;

    case O42_SHAPE_CHECKBOX:
    case O42_SHAPE_OPTION:
      {
        gboolean on = shape->kind == O42_SHAPE_CHECKBOX
                      ? (has_value && value != 0)
                      : (has_value && value == (shape->value != 0 ? shape->value : 1));
        double box = MIN (CONTROL_BOX, height - 2);
        double top = (height - box) / 2;

        cairo_set_source_rgb (cr, 1, 1, 1);
        if (shape->kind == O42_SHAPE_OPTION)
          {
            cairo_arc (cr, 1 + box / 2, top + box / 2, box / 2, 0, 2 * G_PI);
            cairo_fill_preserve (cr);
            cairo_set_source_rgb (cr, 0.4, 0.4, 0.4);
            cairo_stroke (cr);
            if (on)
              {
                cairo_set_source_rgb (cr, 0, 0, 0);
                cairo_arc (cr, 1 + box / 2, top + box / 2, box / 5, 0, 2 * G_PI);
                cairo_fill (cr);
              }
          }
        else
          {
            cairo_rectangle (cr, 1.5, top + 0.5, box, box);
            cairo_fill_preserve (cr);
            cairo_set_source_rgb (cr, 0.4, 0.4, 0.4);
            cairo_stroke (cr);
            if (on)
              {
                /* A tick, drawn as two strokes of the pen. */
                cairo_set_line_width (cr, 2);
                cairo_set_source_rgb (cr, 0, 0, 0);
                cairo_move_to (cr, 4, top + box / 2);
                cairo_line_to (cr, 1.5 + box / 2.4, top + box - 3);
                cairo_line_to (cr, box, top + 2);
                cairo_stroke (cr);
                cairo_set_line_width (cr, 1);
              }
          }
        draw_caption (cr, shape->text, box + 6, 0, width - box - 6, height, FALSE);
      }
      break;

    case O42_SHAPE_SPINNER:
      draw_bevel (cr, 0, 0, width, height / 2, FALSE);
      draw_bevel (cr, 0, height / 2, width, height / 2, FALSE);
      draw_arrow (cr, 0, 0, width, height / 2, 0, -1);
      draw_arrow (cr, 0, height / 2, width, height / 2, 0, 1);
      break;

    case O42_SHAPE_SCROLLBAR:
      {
        gboolean flat = control_horizontal (shape, width, height);
        double arrow, start, track, pos, size;

        scroll_geometry (shape, width, height, &arrow, &start, &track);
        thumb_geometry (shape, width, height, has_value, value, &pos, &size);

        cairo_set_source_rgb (cr, 0.91, 0.91, 0.91);
        cairo_rectangle (cr, 0, 0, width, height);
        cairo_fill (cr);
        if (flat)
          {
            draw_bevel (cr, 0, 0, arrow, height, FALSE);
            draw_bevel (cr, width - arrow, 0, arrow, height, FALSE);
            draw_arrow (cr, 0, 0, arrow, height, -1, 0);
            draw_arrow (cr, width - arrow, 0, arrow, height, 1, 0);
            draw_bevel (cr, pos, 0, size, height, FALSE);
          }
        else
          {
            draw_bevel (cr, 0, 0, width, arrow, FALSE);
            draw_bevel (cr, 0, height - arrow, width, arrow, FALSE);
            draw_arrow (cr, 0, 0, width, arrow, 0, -1);
            draw_arrow (cr, 0, height - arrow, width, arrow, 0, 1);
            draw_bevel (cr, 0, pos, width, size, FALSE);
          }
      }
      break;

    case O42_SHAPE_LISTBOX:
    case O42_SHAPE_COMBO:
      {
        int chosen = has_value ? (int) value : 0;
        double text_width = width - 4;

        cairo_set_source_rgb (cr, 1, 1, 1);
        cairo_rectangle (cr, 0, 0, width, height);
        cairo_fill (cr);
        cairo_set_source_rgb (cr, 0.4, 0.4, 0.4);
        cairo_rectangle (cr, 0.5, 0.5, width - 1, height - 1);
        cairo_stroke (cr);

        if (shape->kind == O42_SHAPE_COMBO)
          {
            double arrow = MIN (16, width / 3);

            draw_bevel (cr, width - arrow - 1, 1, arrow, height - 2, FALSE);
            draw_arrow (cr, width - arrow - 1, 1, arrow, height - 2, 0, 1);
            text_width -= arrow;
            if (items != NULL && chosen >= 1 && chosen <= (int) g_strv_length (items))
              draw_caption (cr, items[chosen - 1], 3, 0, text_width, height, FALSE);
          }
        else
          {
            int n = items != NULL ? (int) g_strv_length (items) : 0;

            for (int i = 0; i < n; i++)
              {
                double top = 2 + i * CONTROL_ROW;

                if (top + CONTROL_ROW > height)
                  break;
                if (i + 1 == chosen)
                  {
                    cairo_set_source_rgb (cr, 0.12, 0.29, 0.49);
                    cairo_rectangle (cr, 1, top, width - 2, CONTROL_ROW);
                    cairo_fill (cr);
                  }
                {
                  PangoLayout *layout = control_layout (cr, items[i]);

                  if (i + 1 == chosen)
                    cairo_set_source_rgb (cr, 1, 1, 1);
                  else
                    cairo_set_source_rgb (cr, 0, 0, 0);
                  cairo_move_to (cr, 3, top);
                  pango_cairo_show_layout (cr, layout);
                  g_object_unref (layout);
                }
              }
          }
      }
      break;

    case O42_SHAPE_GROUPBOX:
      {
        PangoLayout *layout = control_layout (cr, shape->text);
        int tw, th;

        pango_layout_get_pixel_size (layout, &tw, &th);
        cairo_set_source_rgb (cr, 0.5, 0.5, 0.5);
        /* The frame, with a gap at the top left for the caption. */
        cairo_move_to (cr, 8.5 + (tw > 0 ? tw + 4 : 0), th / 2.0 + 0.5);
        cairo_line_to (cr, width - 0.5, th / 2.0 + 0.5);
        cairo_line_to (cr, width - 0.5, height - 0.5);
        cairo_line_to (cr, 0.5, height - 0.5);
        cairo_line_to (cr, 0.5, th / 2.0 + 0.5);
        if (tw > 0)
          cairo_line_to (cr, 6.5, th / 2.0 + 0.5);
        cairo_stroke (cr);
        cairo_set_source_rgb (cr, 0, 0, 0);
        cairo_move_to (cr, 8, 0);
        pango_cairo_show_layout (cr, layout);
        g_object_unref (layout);
      }
      break;

    default:   /* a label: its text and nothing else */
      draw_caption (cr, shape->text, 0, 0, width, height, FALSE);
      break;
    }

  cairo_restore (cr);
}

