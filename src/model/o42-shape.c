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

  switch (shape->kind)
    {
    case O42_SHAPE_LINE:
    case O42_SHAPE_ARROW:
      set_rgb (cr, shape->line);
      cairo_move_to (cr, 0, 0);
      cairo_line_to (cr, width, height);
      cairo_stroke (cr);
      if (shape->kind == O42_SHAPE_ARROW)
        {
          /* A head at the far end, along the line. */
          double angle = atan2 (height, width);
          double size = MAX (8, shape->line_width * 4);

          cairo_move_to (cr, width, height);
          cairo_line_to (cr, width - size * cos (angle - G_PI / 7), height - size * sin (angle - G_PI / 7));
          cairo_line_to (cr, width - size * cos (angle + G_PI / 7), height - size * sin (angle + G_PI / 7));
          cairo_close_path (cr);
          cairo_fill (cr);
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
      cairo_rectangle (cr, inset, inset, MAX (width - shape->line_width, 1),
                       MAX (height - shape->line_width, 1));
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

