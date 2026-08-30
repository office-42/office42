/* o42-grid.c - see o42-grid.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The grid is one widget that paints everything -- headers, cells, gridlines,
 * selection -- through cairo, and reports a size to its scrolled window that
 * is the whole sheet.  That keeps scrolling to GtkScrolledWindow's own
 * machinery and keeps the painting code to one place.  It does mean the
 * widget is nominally very tall; GTK copes, because only the visible part is
 * ever painted.
 *
 * In-cell editing is a GtkEntry laid over the active cell, which is how every
 * spreadsheet has done it: it gets the text editing, the input method and
 * the clipboard for free, and it goes away when editing ends.
 */

#include "o42-grid.h"
#include "o42-shape.h"
#include "o42-pattern.h"
#include "o42-richtext.h"
#include "o42-pdf.h"
#include "o42-formula.h"
#include "o42-eval.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

static void     end_edit (O42Grid *self);
static gboolean entry_allowed (O42Grid *self, const char *text);
static void     show_custom_filter (O42Grid *self, int col);
static gboolean outline_click (O42Grid *self, double x, double y);

/* The row-number and column-letter margins, widened by the outline
 * margin when the sheet has grouped rows or columns; every function
 * that uses them has `self` in scope. */
/* The row headings are as wide as the numbers in them: a sheet a
 * million rows deep writes seven digits, and 42 pixels held five. */
#define HEADER_W  (self->header_w + self->outline_w)
#define HEADER_H  (20 + self->outline_h)
#define OUTLINE_STEP 13
#define INDENT_PX 9   /* one step of indent */
#define CELL_PAD   3
#define DEFAULT_COL_STEP 80
#define DEFAULT_ROW_STEP 20
#define GRIP       4        /* pixels either side of a header boundary that
                             * grab it for resizing */
#define FILTER_BTN 14       /* the AutoFilter dropdown square in a heading */

/* An auditing arrow: from a rectangle a formula reads to the cell that
 * reads it. */
typedef struct {
  O42Range from;
  int      to_row, to_col;
} AuditArrow;

struct _O42Grid {
  GtkWidget      parent_instance;

  O42Sheet      *sheet;
  int            active_row, active_col;
  int            anchor_row, anchor_col;   /* where a drag started */
  gboolean       dragging;

  /* Tab, Tab, Tab, Enter goes back to the column the row of entries began
   * in, not the column Enter was pressed in.  Excel has done this since
   * version 4 and every touch typist depends on it. */
  int            tab_origin_col;           /* -1 when no Tab run is active */

  GtkWidget     *editor;                   /* a GtkEntry while editing */
  gboolean       editing;

  /* Copying puts the range's inputs here as tab-separated rows; pasting
   * reads the same format back, which is also what every other spreadsheet
   * puts on the clipboard and so what they read from ours. */
  char          *clip_text;
  O42Range       clip_range;

  gboolean       hide_gridlines;
  gboolean       hide_zeros;
  double         zoom;                     /* 1.0 is 100% */
  int            frozen_rows, frozen_cols; /* View > Freeze Panes */
  gboolean       show_breaks;              /* View > Page Breaks */
  gboolean       split;                    /* Window > Split: the bands
                                            * scroll on their own rather
                                            * than staying pinned */
  int            split_top_row;            /* the first row the top band shows */
  int            split_left_col;           /* the first column the left band shows */

  guint          blink_id;
  gboolean       blink_on;

  PangoLayout   *layout;                   /* reused across every cell */

  /* The fill handle being dragged: the source is the selection at the
   * grab, and the target grows from it in one direction. */
  gboolean       fill_drag;
  O42Range       fill_source;
  O42Range       fill_target;

  /* A column or row boundary being dragged in the header.  -1 when none. */
  int            resize_col, resize_row;
  double         resize_start;             /* pointer position at grab */
  int            resize_size;              /* the size at grab */

  /* A picture, when one is selected instead of cells, and the drag that
   * moves it. */
  guint          selected_picture;         /* 0 for none; a chart's or shape's id counts too */
  gboolean       selected_is_chart;
  gboolean       selected_is_shape;
  gboolean       picture_drag;
  gboolean       move_drag;               /* dragging the selection to move it */
  gboolean       move_copy;               /* ...with Ctrl held: copy instead */
  O42Range       move_source;
  int            move_row, move_col;      /* where its top-left would land */
  gboolean       completing;              /* inside an AutoComplete change */
  GArray        *edit_fmt;                /* O42Fmt per byte of the text being
                                           * edited, once part of it has been
                                           * given a look of its own */
  char          *edit_text;               /* the text those go with */
  int            resize_handle;            /* -1, or 0..7 while a handle is dragged */
  int            outline_w, outline_h;     /* the outline margins, 0 without groups */
  double         resize_x0, resize_y0, resize_w0, resize_h0;  /* the object at grab */
  double         drag_mouse_x, drag_mouse_y;
  double         pointer_x, pointer_y;    /* where the mouse last was */
  double         drag_pic_x, drag_pic_y;
  guint          thumb_shape;             /* the scroll bar whose thumb is held */
  GHashTable    *wrap_fit;                /* row -> height, while rows are fitted */
  int            split_drag;              /* 0 none, 1 the horizontal bar, 2 the vertical */
  /* The grid scrolls itself.  It used to be a widget as big as the
   * sheet inside a scrolled window, which is simple and works until the
   * sheet is a million rows deep: a render tree keeps its coordinates in
   * floats and cairo rasterises in 24.8 fixed point, so past about eight
   * million pixels nothing is drawn at all.  Now the widget is the size
   * of the window, it carries the adjustments itself, and everything is
   * drawn relative to the top left of what is on screen. */
  GtkAdjustment *hadj, *vadj;
  guint          hscroll_policy : 1;
  guint          vscroll_policy : 1;
  gulong         hadj_changed, vadj_changed;
  /* Point mode: while a formula is being typed, the pointer and the
   * arrows write a reference into it rather than moving the cursor. */
  GArray        *refs;                    /* RefSpan: the references in what is typed */

  /* The function names offered while one is being typed. */
  GtkWidget     *complete_box;            /* the frame around the list */
  GtkWidget     *complete_list;
  GPtrArray     *complete_names;          /* interned names, not owned */
  int            complete_start;          /* where the partial name begins */
  int            complete_at;             /* which of them is highlighted */

  /* The signature of the call the caret is inside. */
  GtkWidget     *tip_box;
  GtkWidget     *tip_label;
  gboolean       pointing;
  gboolean       point_dragging;          /* the button is down over the grid */
  int            point_start, point_end;  /* the reference's place in the text */
  int            point_row, point_col;    /* its anchor */
  int            point_row2, point_col2;  /* and its far corner */

  GArray        *arrows;                  /* AuditArrow: what Trace Precedents drew */
  GArray        *extra_sel;               /* O42Range: the selections Ctrl+click added
                                           * before the one being made now */
  int            header_w;                /* the row headings' width, by the digits in them */
  gboolean       painting;                /* the format painter is loaded */
  O42Fmt         paint_fmt;               /* ...with this look */
  int            break_drag;              /* 0 none, 1 a row break, 2 a column break */
  int            break_from;              /* the row or column it was grabbed at */
};

G_DEFINE_FINAL_TYPE_WITH_CODE (O42Grid, o42_grid, GTK_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, NULL))

enum {
  PROP_0,
  PROP_HADJUSTMENT,
  PROP_VADJUSTMENT,
  PROP_HSCROLL_POLICY,
  PROP_VSCROLL_POLICY
};

enum {
  SIGNAL_SELECTION_CHANGED,
  SIGNAL_SHEET_CHANGED,
  SIGNAL_RUN_SCRIPT,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ---------------------------------------------------------------------- */
/* Geometry                                                                */
/* ---------------------------------------------------------------------- */

/* Pixel x of the left edge of `col`, and the same for rows.  Linear in the
 * index, which is fine: a screen shows a few dozen columns and the loop is
 * over those, not over 256 every paint. */
static double
col_x (O42Grid *self, int col)
{
  return HEADER_W + o42_sheet_col_offset (self->sheet, col);
}

static double
row_y (O42Grid *self, int row)
{
  return HEADER_H + o42_sheet_row_offset (self->sheet, row);
}

static int
col_at_x (O42Grid *self, double x)
{
  if (x < HEADER_W)
    return -1;
  return o42_sheet_col_at (self->sheet, x - HEADER_W);
}

static int
row_at_y (O42Grid *self, double y)
{
  if (y < HEADER_H)
    return -1;
  return o42_sheet_row_at (self->sheet, y - HEADER_H);
}

/* Where a picture sits, in widget pixels. */
static void
picture_rect (O42Grid *self, const O42Picture *pic,
              double *x, double *y, double *w, double *h)
{
  *x = col_x (self, pic->col) + pic->dx;
  *y = row_y (self, pic->row) + pic->dy;
  *w = pic->width;
  *h = pic->height;
}

static void selection_range (O42Grid *self, O42Range *out);
static void selection_ranges (O42Grid *self, GArray *out);
static void pin_point (O42Grid *self, double *x, double *y);
static void point_end_mode (O42Grid *self);
static void colour_editor_refs (O42Grid *self);
static void complete_offer (O42Grid *self);
static void complete_hide (O42Grid *self);
static void complete_take_down (O42Grid *self);
static gboolean complete_accept (O42Grid *self);
static gboolean complete_move (O42Grid *self, int delta);
static gboolean complete_key (O42Grid *self, guint keyval);
static void editor_rect (O42Grid *self, GdkRectangle *at);
static void place_editor_extras (O42Grid *self);
static void tip_offer (O42Grid *self);
static void tip_hide (O42Grid *self);
static gboolean cycle_dollars (O42Grid *self);
static gboolean point_arrow (O42Grid *self, int drow, int dcol, gboolean extend, gboolean to_edge);
static gboolean point_at (O42Grid *self, int row, int col, gboolean extend);
static void sheet_changed (O42Grid *self);

/* ---- The format painter ------------------------------------------------ */

void
o42_grid_selection_ranges (O42Grid *self, GArray *out)
{
  g_return_if_fail (O42_IS_GRID (self) && out != NULL);
  selection_ranges (self, out);
}

void
o42_grid_pick_up_format (O42Grid *self)
{
  const O42Fmt *fmt;

  g_return_if_fail (O42_IS_GRID (self));
  if (self->sheet == NULL)
    return;
  fmt = o42_sheet_get_fmt (self->sheet, self->active_row, self->active_col);
  if (fmt == NULL)
    return;
  self->paint_fmt = *fmt;
  self->painting = TRUE;
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "copy");
}

gboolean
o42_grid_is_painting (O42Grid *self)
{
  return O42_IS_GRID (self) && self->painting;
}

/* The click that puts the look down.  Everything a cell can wear is
 * given at once -- the font, the colours, the borders, the alignment
 * and the number format -- which is what a painter is for. */
static void
paint_onto (O42Grid *self, int row, int col)
{
  O42Range where = { row, col, row, col };
  O42Range selected;

  selection_range (self, &selected);
  if (o42_range_contains (&selected, row, col))
    where = selected;

  o42_sheet_apply_fmt (self->sheet, &where, O42_FMT_ALL, &self->paint_fmt);
  self->painting = FALSE;
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "cell");
  sheet_changed (self);
}

/* Where a shape sits, in widget pixels. */
static void
shape_rect (O42Grid *self, const O42Shape *shape,
            double *x, double *y, double *w, double *h)
{
  *x = col_x (self, shape->col) + shape->dx;
  *y = row_y (self, shape->row) + shape->dy;
  *w = shape->width;
  *h = shape->height;
}

static void sheet_changed (O42Grid *self);

/* A combo box drops its items down in a popover, and picking one writes
 * the row's number into the linked cell. */
static void
on_combo_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
  O42Grid *self = data;
  GtkWidget *popover = gtk_widget_get_ancestor (GTK_WIDGET (box), GTK_TYPE_POPOVER);
  guint id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (box), "o42-shape"));
  O42Shape *shape = o42_sheet_find_shape (self->sheet, id);

  if (shape != NULL)
    {
      o42_sheet_control_set (self->sheet, shape,
                             gtk_list_box_row_get_index (row) + 1);
      sheet_changed (self);
    }
  if (popover != NULL)
    gtk_popover_popdown (GTK_POPOVER (popover));
}

static void
show_combo_list (O42Grid *self, O42Shape *shape)
{
  GtkWidget *popover = gtk_popover_new ();
  GtkWidget *scrolled = gtk_scrolled_window_new ();
  GtkWidget *box = gtk_list_box_new ();
  char **items = o42_sheet_control_items (self->sheet, shape);
  double sx, sy, sw, sh;
  GdkRectangle at;

  shape_rect (self, shape, &sx, &sy, &sw, &sh);
  pin_point (self, &sx, &sy);
  at.x = (int) (sx * self->zoom);
  at.y = (int) (sy * self->zoom);
  at.width = (int) (sw * self->zoom);
  at.height = (int) (sh * self->zoom);

  g_object_set_data (G_OBJECT (box), "o42-shape", GUINT_TO_POINTER (shape->id));
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box), GTK_SELECTION_NONE);
  g_signal_connect (box, "row-activated", G_CALLBACK (on_combo_row_activated), self);

  for (int i = 0; items != NULL && items[i] != NULL; i++)
    {
      GtkWidget *label = gtk_label_new (items[i]);
      GtkWidget *row = gtk_list_box_row_new ();

      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
      gtk_list_box_append (GTK_LIST_BOX (box), row);
    }
  g_strfreev (items);

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled), 260);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), box);
  gtk_widget_set_size_request (scrolled, MAX (at.width, 80), -1);

  gtk_popover_set_child (GTK_POPOVER (popover), scrolled);
  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &at);
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  gtk_widget_set_parent (popover, GTK_WIDGET (self));
  g_signal_connect (popover, "closed", G_CALLBACK (gtk_widget_unparent), NULL);
  gtk_popover_popup (GTK_POPOVER (popover));
}

/* Works the control under the pointer: the click writes its linked
 * cell, or starts a drag of a scroll bar's thumb, or asks the window to
 * run a button's script.  TRUE if the click was the control's. */
static gboolean
control_pressed (O42Grid *self, O42Shape *shape, double x, double y)
{
  double sx, sy, sw, sh, value = 0;
  gboolean has_value;
  O42ControlPart part;
  int item;

  if (!o42_shape_is_control (shape->kind))
    return FALSE;
  shape_rect (self, shape, &sx, &sy, &sw, &sh);

  if (shape->kind == O42_SHAPE_BUTTON)
    {
      if (shape->script != NULL && *shape->script != '\0')
        g_signal_emit (self, signals[SIGNAL_RUN_SCRIPT], 0, shape->script);
      return TRUE;
    }

  if (shape->kind == O42_SHAPE_COMBO)
    {
      show_combo_list (self, shape);
      return TRUE;
    }

  has_value = o42_sheet_control_value (self->sheet, shape, &value);
  part = o42_shape_control_part (shape, sw, sh, x - sx, y - sy, has_value, value, &item);

  /* A scroll bar's thumb follows the pointer until it is let go. */
  if (part == O42_CONTROL_THUMB)
    {
      self->thumb_shape = shape->id;
      return TRUE;
    }

  if (o42_sheet_control_click (self->sheet, shape, x - sx, y - sy, sw, sh))
    sheet_changed (self);
  return TRUE;
}

/* Where the pointer is along a scroll bar's track, as a value. */
static void
thumb_drag_to (O42Grid *self, double x, double y)
{
  O42Shape *shape = o42_sheet_find_shape (self->sheet, self->thumb_shape);
  double sx, sy, sw, sh, along, span;

  if (shape == NULL)
    return;
  shape_rect (self, shape, &sx, &sy, &sw, &sh);
  span = shape->max - shape->min;
  if (span <= 0)
    return;
  if (sw >= sh)
    along = sw > 0 ? (x - sx) / sw : 0;
  else
    along = sh > 0 ? (y - sy) / sh : 0;
  o42_sheet_control_set (self->sheet, shape,
                         shape->min + CLAMP (along, 0, 1) * span);
  sheet_changed (self);
}

/* The page break within a few pixels of a point, when the breaks are
 * being shown: 1 and the row for one across, 2 and the column for one
 * down, 0 for neither.  The breaks are asked of the paginator, so an
 * automatic break can be taken hold of as readily as one set by hand --
 * which is what dragging one in Excel does: it becomes a manual break
 * where it is dropped. */
static int
page_break_at (O42Grid *self, double x, double y, int *at)
{
  double margin;
  O42Pages *pages;
  int *rows = NULL, *cols = NULL;
  int n_rows, n_cols, found = 0;

  if (!self->show_breaks || self->sheet == NULL)
    return 0;
  margin = o42_sheet_print_setup (self->sheet)->margin;
  pages = o42_pages_new (self->sheet, 595 - 2 * margin, 842 - 2 * margin);
  n_rows = o42_pages_row_breaks (pages, &rows);
  n_cols = o42_pages_col_breaks (pages, &cols);

  for (int i = 0; i < n_rows && found == 0; i++)
    if (fabs (y - row_y (self, rows[i])) <= GRIP && x > HEADER_W)
      { found = 1; *at = rows[i]; }
  for (int i = 0; i < n_cols && found == 0; i++)
    if (fabs (x - col_x (self, cols[i])) <= GRIP && y > HEADER_H)
      { found = 2; *at = cols[i]; }

  g_free (rows);
  g_free (cols);
  o42_pages_free (pages);
  return found;
}

/* Which split bar is within a few pixels of a point: 1 for the one
 * across, 2 for the one down, 0 for neither.  Only a split has bars a
 * pointer can take hold of; a frozen pane is set from the menu. */
static int
split_bar_at (O42Grid *self, double x, double y)
{
  double edge;

  if (!self->split || self->sheet == NULL)
    return 0;
  if (self->frozen_rows > 0)
    {
      edge = row_y (self, self->frozen_rows);
      if (fabs (y - edge) <= GRIP && x > HEADER_W)
        return 1;
    }
  if (self->frozen_cols > 0)
    {
      edge = col_x (self, self->frozen_cols);
      if (fabs (x - edge) <= GRIP && y > HEADER_H)
        return 2;
    }
  return 0;
}

/* The topmost shape under a point, or NULL.  A line has no width to
 * speak of, so its box is grown a little to be clickable. */
static O42Shape *
shape_at (O42Grid *self, double x, double y)
{
  GPtrArray *shapes;

  if (self->sheet == NULL)
    return NULL;
  shapes = o42_sheet_shapes (self->sheet);
  for (guint i = shapes->len; i > 0; i--)
    {
      O42Shape *shape = g_ptr_array_index (shapes, i - 1);
      double sx, sy, sw, sh;

      shape_rect (self, shape, &sx, &sy, &sw, &sh);
      if (sw < 0) { sx += sw; sw = -sw; }
      if (sh < 0) { sy += sh; sh = -sh; }
      if (x >= sx - 3 && x < sx + sw + 3 && y >= sy - 3 && y < sy + sh + 3)
        return shape;
    }
  return NULL;
}

/* Where a chart sits, in widget pixels. */
static void
chart_rect (O42Grid *self, const O42Chart *chart,
            double *x, double *y, double *w, double *h)
{
  *x = col_x (self, chart->col) + chart->dx;
  *y = row_y (self, chart->row) + chart->dy;
  *w = chart->width;
  *h = chart->height;
}

static O42Chart *
chart_at (O42Grid *self, double x, double y)
{
  GPtrArray *charts;

  if (self->sheet == NULL)
    return NULL;

  charts = o42_sheet_charts (self->sheet);
  for (guint i = charts->len; i > 0; i--)
    {
      O42Chart *chart = g_ptr_array_index (charts, i - 1);
      double cx, cy, cw, ch;

      chart_rect (self, chart, &cx, &cy, &cw, &ch);
      if (x >= cx && x < cx + cw && y >= cy && y < cy + ch)
        return chart;
    }

  return NULL;
}

static void anchor_place (O42Grid *self, int *arow, int *acol, double *adx,
                          double *ady, double x, double y);
static void picture_place (O42Grid *self, O42Picture *pic, double x, double y);

/* The eight handles of a selected object, in the order they are drawn:
 * the corners and the middles of the sides. */
static const double HANDLE_X[8] = { 0, 0.5, 1, 0, 1, 0, 0.5, 1 };
static const double HANDLE_Y[8] = { 0, 0, 0, 0.5, 0.5, 1, 1, 1 };

/* The rectangle of whatever object is selected, or FALSE. */
static gboolean
selected_object_rect (O42Grid *self, double *x, double *y, double *w, double *h)
{
  if (self->sheet == NULL || self->selected_picture == 0)
    return FALSE;

  if (self->selected_is_shape)
    {
      O42Shape *shape = o42_sheet_find_shape (self->sheet, self->selected_picture);
      if (shape == NULL)
        return FALSE;
      shape_rect (self, shape, x, y, w, h);
    }
  else if (self->selected_is_chart)
    {
      O42Chart *chart = o42_sheet_find_chart (self->sheet, self->selected_picture);
      if (chart == NULL)
        return FALSE;
      chart_rect (self, chart, x, y, w, h);
    }
  else
    {
      O42Picture *pic = o42_sheet_find_picture (self->sheet, self->selected_picture);
      if (pic == NULL)
        return FALSE;
      picture_rect (self, pic, x, y, w, h);
    }

  return TRUE;
}

/* Which handle of the selected object is under the point, or -1. */
static int
handle_at (O42Grid *self, double x, double y)
{
  double ox, oy, ow, oh;

  if (!selected_object_rect (self, &ox, &oy, &ow, &oh))
    return -1;

  for (int k = 0; k < 8; k++)
    {
      double hx = ox + HANDLE_X[k] * ow, hy = oy + HANDLE_Y[k] * oh;

      if (fabs (x - hx) <= GRIP && fabs (y - hy) <= GRIP)
        return k;
    }

  return -1;
}

/* Puts the selected object at a rectangle, re-anchoring its corner. */
static void
selected_object_set_rect (O42Grid *self, double x, double y, double w, double h)
{
  w = MAX (w, 16);
  h = MAX (h, 16);

  if (self->selected_is_shape)
    {
      O42Shape *shape = o42_sheet_find_shape (self->sheet, self->selected_picture);
      if (shape == NULL)
        return;
      anchor_place (self, &shape->row, &shape->col, &shape->dx, &shape->dy, x, y);
      shape->width = w;
      shape->height = h;
    }
  else if (self->selected_is_chart)
    {
      O42Chart *chart = o42_sheet_find_chart (self->sheet, self->selected_picture);
      if (chart == NULL)
        return;
      anchor_place (self, &chart->row, &chart->col, &chart->dx, &chart->dy, x, y);
      chart->width = w;
      chart->height = h;
    }
  else
    {
      O42Picture *pic = o42_sheet_find_picture (self->sheet, self->selected_picture);
      if (pic == NULL)
        return;
      picture_place (self, pic, x, y);
      pic->width = w;
      pic->height = h;
    }
}

/* The topmost picture under a point, or NULL. */
static O42Picture *
picture_at (O42Grid *self, double x, double y)
{
  GPtrArray *pictures;

  if (self->sheet == NULL)
    return NULL;

  pictures = o42_sheet_pictures (self->sheet);

  for (guint i = pictures->len; i > 0; i--)
    {
      O42Picture *pic = g_ptr_array_index (pictures, i - 1);
      double px, py, pw, ph;

      picture_rect (self, pic, &px, &py, &pw, &ph);
      if (x >= px && x < px + pw && y >= py && y < py + ph)
        return pic;
    }

  return NULL;
}

/* Re-anchors a picture to whatever cell its top-left corner is over, so
 * that it keeps its place when rows and columns around it change size. */
static void
anchor_place (O42Grid *self, int *arow, int *acol, double *adx, double *ady,
              double x, double y)
{
  int col = col_at_x (self, MAX (x, HEADER_W));
  int row = row_at_y (self, MAX (y, HEADER_H));

  col = MAX (col, 0);
  row = MAX (row, 0);

  *acol = col;
  *arow = row;
  *adx = MAX (0.0, x - col_x (self, col));
  *ady = MAX (0.0, y - row_y (self, row));
}

static void
picture_place (O42Grid *self, O42Picture *pic, double x, double y)
{
  anchor_place (self, &pic->row, &pic->col, &pic->dx, &pic->dy, x, y);
}

static void
selection_range (O42Grid *self, O42Range *out)
{
  *out = o42_range_normalise (self->anchor_row, self->anchor_col,
                              self->active_row, self->active_col);
}

/* Every rectangle of the selection: the one being made, and the ones
 * Ctrl+click put behind it.  Excel calls the lot one selection and so
 * does everything here that formats or clears. */
static void
selection_ranges (O42Grid *self, GArray *out)
{
  O42Range current;

  g_array_set_size (out, 0);
  for (guint i = 0; self->extra_sel != NULL && i < self->extra_sel->len; i++)
    g_array_append_val (out, g_array_index (self->extra_sel, O42Range, i));
  selection_range (self, &current);
  g_array_append_val (out, current);
}

/* A plain click, or a move of the active cell, starts again. */
static void
selection_forget_extras (O42Grid *self)
{
  if (self->extra_sel != NULL && self->extra_sel->len > 0)
    {
      g_array_set_size (self->extra_sel, 0);
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

/* The column whose right edge is within GRIP pixels of x, if x is in the
 * column header; -1 otherwise.  Rows likewise. */
static int
col_boundary_at (O42Grid *self, double x, double y)
{
  double edge = HEADER_W;

  if (y >= HEADER_H || x < HEADER_W)
    return -1;

  for (int c = 0; c < O42_MAX_COLS; c++)
    {
      edge += o42_sheet_col_width (self->sheet, c);
      if (fabs (x - edge) <= GRIP)
        return c;
      if (x < edge)
        return -1;
    }

  return -1;
}

static int
row_boundary_at (O42Grid *self, double x, double y)
{
  double edge = HEADER_H;

  if (x >= HEADER_W || y < HEADER_H)
    return -1;

  for (int r = 0; r < O42_MAX_ROWS; r++)
    {
      edge += o42_sheet_row_height (self->sheet, r);
      if (fabs (y - edge) <= GRIP)
        return r;
      if (y < edge)
        return -1;
    }

  return -1;
}

/* The scroll offsets, in sheet pixels. */
static void
grid_scroll (O42Grid *self, double *sx, double *sy)
{
  *sx = self->hadj != NULL ? gtk_adjustment_get_value (self->hadj) / self->zoom : 0;
  *sy = self->vadj != NULL ? gtk_adjustment_get_value (self->vadj) / self->zoom : 0;
}

/* The other way about: a point in the sheet, in widget coordinates, for
 * placing something over the grid -- a popover, the cell editor.  A
 * pinned row or column stays where it is painted. */
static void
pin_point (O42Grid *self, double *x, double *y)
{
  double sx, sy;
  double frozen_w, frozen_h;

  if (self->sheet == NULL)
    return;
  grid_scroll (self, &sx, &sy);
  frozen_w = col_x (self, self->frozen_cols) - HEADER_W;
  frozen_h = row_y (self, self->frozen_rows) - HEADER_H;

  if (*y >= HEADER_H + frozen_h)
    *y -= sy;
  if (*x >= HEADER_W + frozen_w)
    *x -= sx;
}

/* A point in widget coordinates (already divided by the zoom) into
 * sheet coordinates: the scroll is added, and then the headers and the
 * frozen panes -- which are painted pinned to the viewport -- have it
 * taken off again, since a point in them means the row or column that
 * is pinned there rather than the one that scrolled under it. */
static void
unpin_point (O42Grid *self, double *x, double *y)
{
  double sx, sy;
  double frozen_w, frozen_h;

  if (self->sheet == NULL)
    return;

  grid_scroll (self, &sx, &sy);
  *x += sx;
  *y += sy;
  frozen_w = col_x (self, self->frozen_cols) - HEADER_W;
  frozen_h = row_y (self, self->frozen_rows) - HEADER_H;

  if (*y < sy + HEADER_H + frozen_h)
    *y -= sy;
  if (*x < sx + HEADER_W + frozen_w)
    *x -= sx;
}

/* Is the point on the fill handle, the little square at the bottom right
 * of the selection? */
static gboolean
on_fill_handle (O42Grid *self, double x, double y)
{
  O42Range sel;
  double hx, hy;

  selection_range (self, &sel);
  hx = col_x (self, sel.col1) + o42_sheet_col_width (self->sheet, sel.col1);
  hy = row_y (self, sel.row1) + o42_sheet_row_height (self->sheet, sel.row1);

  return fabs (x - hx) <= GRIP && fabs (y - hy) <= GRIP;
}

/* Is the point on the outline of the selection, where a drag moves the
 * cells rather than choosing them?  The fill handle wins where they
 * meet, since it is inside this band. */
static gboolean
on_selection_edge (O42Grid *self, double x, double y)
{
  O42Range sel;
  double x0, y0, x1, y1;

  if (self->sheet == NULL || on_fill_handle (self, x, y))
    return FALSE;
  selection_range (self, &sel);
  x0 = col_x (self, sel.col0);
  y0 = row_y (self, sel.row0);
  x1 = col_x (self, sel.col1) + o42_sheet_col_width (self->sheet, sel.col1);
  y1 = row_y (self, sel.row1) + o42_sheet_row_height (self->sheet, sel.row1);
  if (x < x0 - 3 || x > x1 + 3 || y < y0 - 3 || y > y1 + 3)
    return FALSE;
  return fabs (x - x0) <= 3 || fabs (x - x1) <= 3 || fabs (y - y0) <= 3 || fabs (y - y1) <= 3;
}

/* The width a column needs to show every cell in it without clipping. */
static int
column_fit_width (O42Grid *self, int col)
{
  O42Range used;
  int widest = 0;

  o42_sheet_used_range (self->sheet, &used);

  for (int row = used.row0; row <= used.row1; row++)
    {
      const O42Fmt *fmt;
      PangoFontDescription *desc;
      char *text;
      int tw, th;

      if (o42_sheet_is_empty (self->sheet, row, col))
        continue;

      fmt = o42_sheet_get_fmt (self->sheet, row, col);
      text = o42_sheet_get_display (self->sheet, row, col);

      desc = pango_font_description_new ();
      pango_font_description_set_family (desc, fmt->family ? fmt->family : "Sans");
      pango_font_description_set_size (desc, (fmt->size / 2) * PANGO_SCALE);
      pango_font_description_set_weight (desc, fmt->bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
      pango_font_description_set_style (desc, fmt->italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
      pango_layout_set_font_description (self->layout, desc);
      pango_font_description_free (desc);
      pango_layout_set_attributes (self->layout, NULL);
      pango_layout_set_text (self->layout, text, -1);
      pango_layout_set_width (self->layout, -1);
      pango_layout_get_pixel_size (self->layout, &tw, &th);
      g_free (text);

      widest = MAX (widest, tw);
    }

  /* A column with nothing in it goes back to the default. */
  return (widest > 0) ? widest + 2 * CELL_PAD + 2 : 80;
}

/* ---------------------------------------------------------------------- */
/* The adjustments: the grid scrolls itself                                */
/* ---------------------------------------------------------------------- */

/* How far the sheet reaches, in widget pixels: the used range and a
 * screenful past it, which is what Excel's scrollbars describe.  A cell
 * beyond is still reached by name, and typing in one brings it inside
 * this the moment it becomes the active cell. */
static void
grid_extent (O42Grid *self, double *width, double *height)
{
  O42Range used;
  int last_col, last_row;

  *width = *height = 100;
  if (self->sheet == NULL)
    return;
  o42_sheet_used_range (self->sheet, &used);
  last_col = MIN (MAX (used.col1 + 20, MAX (self->active_col + 20, 40)), O42_MAX_COLS);
  last_row = MIN (MAX (used.row1 + 100, MAX (self->active_row + 100, 200)), O42_MAX_ROWS);
  *width = col_x (self, last_col) * self->zoom;
  *height = row_y (self, last_row) * self->zoom;
}

static void
grid_configure_adjustment (O42Grid *self, GtkAdjustment *adj, gboolean horizontal)
{
  double extent_w, extent_h, extent, page;

  if (adj == NULL)
    return;
  grid_extent (self, &extent_w, &extent_h);
  extent = horizontal ? extent_w : extent_h;
  page = horizontal ? gtk_widget_get_width (GTK_WIDGET (self))
                    : gtk_widget_get_height (GTK_WIDGET (self));
  if (page <= 0)
    page = 1;

  gtk_adjustment_configure (adj,
                            MIN (gtk_adjustment_get_value (adj), MAX (extent - page, 0)),
                            0, MAX (extent, page),
                            horizontal ? DEFAULT_COL_STEP : DEFAULT_ROW_STEP,
                            page * 0.9, page);
}

static void
grid_configure_adjustments (O42Grid *self)
{
  grid_configure_adjustment (self, self->hadj, TRUE);
  grid_configure_adjustment (self, self->vadj, FALSE);
}

static void
on_adjustment_value_changed (GtkAdjustment *adj, gpointer data)
{
  (void) adj;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

static void
grid_set_adjustment (O42Grid *self, gboolean horizontal, GtkAdjustment *adj)
{
  GtkAdjustment **slot = horizontal ? &self->hadj : &self->vadj;
  gulong *handler = horizontal ? &self->hadj_changed : &self->vadj_changed;

  if (*slot == adj)
    return;
  if (*slot != NULL)
    {
      g_signal_handler_disconnect (*slot, *handler);
      g_object_unref (*slot);
    }
  if (adj == NULL)
    adj = gtk_adjustment_new (0, 0, 0, 0, 0, 0);
  *slot = g_object_ref_sink (adj);
  *handler = g_signal_connect (adj, "value-changed",
                              G_CALLBACK (on_adjustment_value_changed), self);
  grid_configure_adjustment (self, adj, horizontal);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
o42_grid_get_property (GObject *object, guint id, GValue *value, GParamSpec *spec)
{
  O42Grid *self = O42_GRID (object);

  switch (id)
    {
    case PROP_HADJUSTMENT: g_value_set_object (value, self->hadj); break;
    case PROP_VADJUSTMENT: g_value_set_object (value, self->vadj); break;
    case PROP_HSCROLL_POLICY: g_value_set_enum (value, self->hscroll_policy); break;
    case PROP_VSCROLL_POLICY: g_value_set_enum (value, self->vscroll_policy); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, spec); break;
    }
}

static void
o42_grid_set_property (GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
  O42Grid *self = O42_GRID (object);

  switch (id)
    {
    case PROP_HADJUSTMENT: grid_set_adjustment (self, TRUE, g_value_get_object (value)); break;
    case PROP_VADJUSTMENT: grid_set_adjustment (self, FALSE, g_value_get_object (value)); break;
    case PROP_HSCROLL_POLICY: self->hscroll_policy = g_value_get_enum (value); break;
    case PROP_VSCROLL_POLICY: self->vscroll_policy = g_value_get_enum (value); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, spec); break;
    }
}

/* ---------------------------------------------------------------------- */
/* Scrolling                                                               */
/* ---------------------------------------------------------------------- */

static void
scroll_to_active (O42Grid *self)
{
  GtkAdjustment *h, *v;
  double x0, y0, x1, y1, value, page;

  if (self->sheet == NULL)
    return;

  if (self->hadj == NULL || self->vadj == NULL)
    return;

  x0 = col_x (self, self->active_col) * self->zoom;
  x1 = x0 + o42_sheet_col_width (self->sheet, self->active_col) * self->zoom;
  y0 = row_y (self, self->active_row) * self->zoom;
  y1 = y0 + o42_sheet_row_height (self->sheet, self->active_row) * self->zoom;

  h = self->hadj;
  v = self->vadj;

  /* The headers are painted at the widget's origin, so they scroll away;
   * keeping the active cell clear of them means treating their size as part
   * of the viewport's top-left inset. */
  {
    double inset_x = col_x (self, self->frozen_cols) * self->zoom;
    double inset_y = row_y (self, self->frozen_rows) * self->zoom;

    /* A cell inside a frozen band is always on show. */
    if (self->active_col >= self->frozen_cols)
      {
        value = gtk_adjustment_get_value (h);
        page = gtk_adjustment_get_page_size (h);
        if (x0 - inset_x < value)
          gtk_adjustment_set_value (h, MAX (0, x0 - inset_x));
        else if (x1 > value + page)
          gtk_adjustment_set_value (h, x1 - page);
      }

    if (self->active_row >= self->frozen_rows)
      {
        value = gtk_adjustment_get_value (v);
        page = gtk_adjustment_get_page_size (v);
        if (y0 - inset_y < value)
          gtk_adjustment_set_value (v, MAX (0, y0 - inset_y));
        else if (y1 > value + page)
          gtk_adjustment_set_value (v, y1 - page);
      }
  }
}

/* ---------------------------------------------------------------------- */
/* Selection                                                               */
/* ---------------------------------------------------------------------- */

static void
selection_changed (O42Grid *self)
{
  self->blink_on = TRUE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
}

/* The outline margins follow the sheet's deepest groups, and the row
 * headings the number of digits the deepest row number needs. */
static void
outline_sync (O42Grid *self)
{
  int rl = self->sheet ? o42_sheet_max_row_level (self->sheet) : 0;
  int cl = self->sheet ? o42_sheet_max_col_level (self->sheet) : 0;
  int w = rl > 0 ? rl * OUTLINE_STEP + 6 : 0;
  int h = cl > 0 ? cl * OUTLINE_STEP + 6 : 0;
  int digits = 3;
  int header_w;

  if (self->sheet != NULL)
    {
      O42Range used;
      int deepest;

      o42_sheet_used_range (self->sheet, &used);
      deepest = MAX (used.row1, self->active_row) + 1;
      while (deepest >= 1000)
        { digits++; deepest /= 10; }
      digits = MAX (digits, 3);
    }
  header_w = 12 + digits * 8;

  if (w != self->outline_w || h != self->outline_h || header_w != self->header_w)
    {
      self->outline_w = w;
      self->outline_h = h;
      self->header_w = header_w;
      gtk_widget_queue_resize (GTK_WIDGET (self));
    }
}

static void
sheet_changed (O42Grid *self)
{
  outline_sync (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_SHEET_CHANGED], 0);
}

static void
move_active (O42Grid *self, int row, int col, gboolean extend)
{
  O42Range merged;

  /* Moving the cell, or making a new selection, forgets the rectangles
   * Ctrl+click had put behind this one. */
  if (!extend)
    selection_forget_extras (self);

  row = CLAMP (row, 0, O42_MAX_ROWS - 1);
  col = CLAMP (col, 0, O42_MAX_COLS - 1);

  /* A merged range is one cell: landing anywhere in it lands on its
   * top-left, stepping past it from there. */
  if (self->sheet != NULL && !extend &&
      o42_sheet_merged_at (self->sheet, row, col, &merged) &&
      !(row == merged.row0 && col == merged.col0))
    {
      if (row > self->active_row && merged.row1 + 1 < O42_MAX_ROWS &&
          o42_range_contains (&merged, self->active_row, self->active_col))
        row = merged.row1 + 1;
      else if (col > self->active_col && merged.col1 + 1 < O42_MAX_COLS &&
               o42_range_contains (&merged, self->active_row, self->active_col))
        col = merged.col1 + 1;
      else
        {
          row = merged.row0;
          col = merged.col0;
        }
    }

  /* A hidden row or column is skipped in the direction of travel. */
  if (self->sheet != NULL)
    {
      int drow = (row > self->active_row) ? 1 : -1;
      int dcol = (col > self->active_col) ? 1 : -1;

      while (o42_sheet_row_height (self->sheet, row) == 0 &&
             row + drow >= 0 && row + drow < O42_MAX_ROWS)
        row += drow;
      while (o42_sheet_col_width (self->sheet, col) == 0 &&
             col + dcol >= 0 && col + dcol < O42_MAX_COLS)
        col += dcol;
    }

  /* Any move that is not a Tab ends the Tab run.  Tab itself re-arms it
   * just after calling this.  It also drops any picture that was selected:
   * the keyboard is back on the cells. */
  self->tab_origin_col = -1;
  self->selected_picture = 0;

  self->active_row = row;
  self->active_col = col;

  if (!extend)
    {
      self->anchor_row = row;
      self->anchor_col = col;
    }

  scroll_to_active (self);
  selection_changed (self);
}

void
o42_grid_get_active (O42Grid *self, int *row, int *col)
{
  g_return_if_fail (O42_IS_GRID (self));

  if (row) *row = self->active_row;
  if (col) *col = self->active_col;
}

void
o42_grid_set_active (O42Grid *self, int row, int col)
{
  g_return_if_fail (O42_IS_GRID (self));

  if (self->editing)
    o42_grid_commit_edit (self);

  move_active (self, row, col, FALSE);
}

void
o42_grid_get_selection (O42Grid *self, O42Range *range)
{
  g_return_if_fail (O42_IS_GRID (self));
  g_return_if_fail (range != NULL);

  selection_range (self, range);
}

void
o42_grid_select_range (O42Grid *self, const O42Range *range)
{
  g_return_if_fail (O42_IS_GRID (self));
  g_return_if_fail (range != NULL);

  self->anchor_row = range->row0;
  self->anchor_col = range->col0;
  self->active_row = range->row1;
  self->active_col = range->col1;

  selection_changed (self);
}

void
o42_grid_select_all (O42Grid *self)
{
  O42Range used;

  g_return_if_fail (O42_IS_GRID (self));

  if (self->sheet == NULL)
    return;

  /* Selecting the used range rather than all four million cells, so that a
   * copy afterwards does not try to serialise the whole sheet. */
  o42_sheet_used_range (self->sheet, &used);
  used.row0 = 0;
  used.col0 = 0;
  o42_grid_select_range (self, &used);
}

/* ---------------------------------------------------------------------- */
/* Editing                                                                 */
/* ---------------------------------------------------------------------- */

static void place_editor (O42Grid *self);

static void
on_editor_activate (GtkEntry *entry, gpointer data)
{
  O42Grid *self = data;
  int col = (self->tab_origin_col >= 0) ? self->tab_origin_col
                                        : self->active_col;

  (void) entry;

  /* Enter commits and moves down, as it has since VisiCalc -- back to the
   * column the Tab run started in, if there was one. */
  o42_grid_commit_edit (self);
  move_active (self, self->active_row + 1, col, FALSE);
}

static gboolean
on_editor_key (GtkEventControllerKey *controller,
               guint                  keyval,
               guint                  keycode,
               GdkModifierType        state,
               gpointer               data)
{
  O42Grid *self = data;

  (void) controller; (void) keycode;

  /* Anything but an arrow or a modifier leaves the reference where it
   * is: what follows belongs to the formula, not to it. */
  switch (keyval)
    {
    case GDK_KEY_Left:  case GDK_KEY_KP_Left:
    case GDK_KEY_Right: case GDK_KEY_KP_Right:
    case GDK_KEY_Up:    case GDK_KEY_KP_Up:
    case GDK_KEY_Down:  case GDK_KEY_KP_Down:
    case GDK_KEY_Shift_L: case GDK_KEY_Shift_R:
    case GDK_KEY_Control_L: case GDK_KEY_Control_R:
    case GDK_KEY_F4:
      break;
    default:
      point_end_mode (self);
      break;
    }

  /* The name list, while it is up, has the arrows, Tab and Enter. */
  if (complete_key (self, keyval))
    return GDK_EVENT_STOP;

  switch (keyval)
    {
    case GDK_KEY_F4:
      /* The dollars on the reference at the caret, round the four ways
       * a reference can be written. */
      if (cycle_dollars (self))
        return GDK_EVENT_STOP;
      return GDK_EVENT_PROPAGATE;

    case GDK_KEY_Escape:
      o42_grid_cancel_edit (self);
      return GDK_EVENT_STOP;

    case GDK_KEY_Return: case GDK_KEY_KP_Enter:
      /* Ctrl+Shift+Enter: an array formula over the selection. */
      if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK))
        {
          const char *text = gtk_editable_get_text (GTK_EDITABLE (self->editor));
          O42Range sel;

          o42_grid_get_selection (self, &sel);
          if (text[0] == '=' && entry_allowed (self, text))
            o42_sheet_set_array_formula (self->sheet, &sel, text);
          end_edit (self);
          sheet_changed (self);
          return GDK_EVENT_STOP;
        }
      return GDK_EVENT_PROPAGATE;

    case GDK_KEY_Tab:
      {
        int origin = (self->tab_origin_col >= 0) ? self->tab_origin_col
                                                 : self->active_col;
        o42_grid_commit_edit (self);
        move_active (self, self->active_row, self->active_col + 1, FALSE);
        self->tab_origin_col = origin;
        return GDK_EVENT_STOP;
      }

    case GDK_KEY_ISO_Left_Tab:
      {
        int origin = (self->tab_origin_col >= 0) ? self->tab_origin_col
                                                 : self->active_col;
        o42_grid_commit_edit (self);
        move_active (self, self->active_row, self->active_col - 1, FALSE);
        self->tab_origin_col = origin;
        return GDK_EVENT_STOP;
      }

    case GDK_KEY_Left:  case GDK_KEY_KP_Left:
    case GDK_KEY_Right: case GDK_KEY_KP_Right:
      /* Left and right move the caret through the text, unless a
       * reference may stand where it is: then they point. */
      if (point_arrow (self, 0, keyval == GDK_KEY_Left || keyval == GDK_KEY_KP_Left ? -1 : 1,
                       (state & GDK_SHIFT_MASK) != 0, (state & GDK_CONTROL_MASK) != 0))
        return GDK_EVENT_STOP;
      return GDK_EVENT_PROPAGATE;

    case GDK_KEY_Up:
    case GDK_KEY_Down:
      if (point_arrow (self, keyval == GDK_KEY_Up ? -1 : 1, 0,
                       (state & GDK_SHIFT_MASK) != 0, (state & GDK_CONTROL_MASK) != 0))
        return GDK_EVENT_STOP;
      /* Arrows leave the cell in a spreadsheet, unlike in a text field;
       * within the entry they would only move a caret in one line. */
      if (!(state & GDK_CONTROL_MASK))
        {
          int delta = (keyval == GDK_KEY_Up) ? -1 : 1;
          o42_grid_commit_edit (self);
          move_active (self, self->active_row + delta, self->active_col, FALSE);
          return GDK_EVENT_STOP;
        }
      break;

    default:
      break;
    }

  return GDK_EVENT_PROPAGATE;
}

gboolean
o42_grid_is_editing (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), FALSE);
  return self->editing;
}

/* AutoComplete: while a word is being typed into a cell, the first
 * text already in the column that begins with it is offered, with the
 * part not yet typed selected so that typing on replaces it. */
static void
on_editor_changed (GtkEditable *editable, gpointer data)
{
  O42Grid *self = data;

  colour_editor_refs (self);
  complete_offer (self);
  tip_offer (self);
  const char *typed = gtk_editable_get_text (editable);
  int caret = gtk_editable_get_position (editable);
  O42Range used;
  char *found = NULL;
  gsize length;

  if (self->completing || self->sheet == NULL || typed == NULL)
    return;
  length = strlen (typed);
  if (length == 0 || typed[0] == '=' || caret != (int) length)
    return;
  if (g_ascii_isdigit (typed[0]))
    return;

  o42_sheet_used_range (self->sheet, &used);
  for (int row = used.row0; row <= used.row1 && found == NULL; row++)
    {
      char *shown;

      if (row == self->active_row)
        continue;
      shown = o42_sheet_get_display (self->sheet, row, self->active_col);
      if (strlen (shown) > length && g_ascii_strncasecmp (shown, typed, length) == 0)
        {
          O42Value v;
          o42_sheet_get_value (self->sheet, row, self->active_col, &v);
          if (v.type == O42_VALUE_TEXT)
            found = g_strdup (shown);
          o42_value_clear (&v);
        }
      g_free (shown);
    }

  if (found != NULL)
    {
      self->completing = TRUE;
      gtk_editable_set_text (editable, found);
      gtk_editable_select_region (editable, (int) length, -1);
      self->completing = FALSE;
      g_free (found);
    }
}

/* ---- The references in a formula, coloured ----------------------------- */

/* While a formula is being typed, each reference in it is written in a
 * colour of its own and the cells it names are outlined in the same
 * one.  It is the quickest way to see that a formula is reading what
 * you meant it to read, and every spreadsheet since Lotus has done it.
 *
 * The colours go round in the order Excel uses them. */
static const guint32 REF_COLOURS[] = {
  0x1F49BF, 0xC0392B, 0x1E8449, 0x8E44AD, 0xB9770E, 0x117A8B
};

typedef struct {
  int      start, end;    /* where the reference stands in the text */
  O42Range range;
  int      colour;        /* which of REF_COLOURS */
} RefSpan;

/* Every reference a formula's text holds, in the order they are
 * written, skipping anything inside quotes and anything that names
 * another sheet -- an outline can only be drawn on this one. */
static GArray *
formula_refs (const char *text)
{
  GArray *found = g_array_new (FALSE, FALSE, sizeof (RefSpan));
  int n = (int) strlen (text);
  int i = 0, colour = 0;

  while (i < n)
    {
      int row, col, row2, col2, start;
      gsize used = 0;

      if (text[i] == '"')
        {
          for (i++; i < n && text[i] != '"'; i++)
            ;
          i++;
          continue;
        }
      /* A letter that follows one is part of a name, not the start of
       * a reference: SUM is not S, U, M. */
      if (!(g_ascii_isalpha (text[i]) || text[i] == '$') ||
          (i > 0 && (g_ascii_isalnum (text[i - 1]) || text[i - 1] == '_' ||
                     text[i - 1] == '!' || text[i - 1] == '$')))
        { i++; continue; }

      start = i;
      if (!o42_ref_parse (text + i, &row, &col, &used) || used == 0)
        { i++; continue; }
      i += (int) used;
      row2 = row;
      col2 = col;
      if (text[i] == ':' && o42_ref_parse (text + i + 1, &row2, &col2, &used) && used > 0)
        i += 1 + (int) used;
      /* Not a reference after all if a letter or a bracket follows: SUM(
       * begins a call, and A1B is a name. */
      if (g_ascii_isalpha (text[i]) || text[i] == '(' || text[i] == '_')
        continue;

      {
        RefSpan span;

        span.start = start;
        span.end = i;
        span.range = o42_range_normalise (row, col, row2, col2);
        span.colour = colour % (int) G_N_ELEMENTS (REF_COLOURS);
        g_array_append_val (found, span);
        colour++;
      }
    }
  return found;
}

/* The colours, put on the text of the editor. */
static void
colour_editor_refs (O42Grid *self)
{
  const char *text;
  PangoAttrList *attrs;
  GArray *refs;

  if (!self->editing || self->editor == NULL)
    return;
  text = gtk_editable_get_text (GTK_EDITABLE (self->editor));
  if (text == NULL || text[0] != '=')
    {
      gtk_entry_set_attributes (GTK_ENTRY (self->editor), NULL);
      if (self->refs != NULL)
        g_array_set_size (self->refs, 0);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

  refs = formula_refs (text);
  attrs = pango_attr_list_new ();
  for (guint i = 0; i < refs->len; i++)
    {
      const RefSpan *span = &g_array_index (refs, RefSpan, i);
      guint32 rgb = REF_COLOURS[span->colour];
      PangoAttribute *a = pango_attr_foreground_new (((rgb >> 16) & 0xFF) * 257,
                                                     ((rgb >> 8) & 0xFF) * 257,
                                                     (rgb & 0xFF) * 257);

      a->start_index = (guint) span->start;
      a->end_index = (guint) span->end;
      pango_attr_list_insert (attrs, a);
    }
  gtk_entry_set_attributes (GTK_ENTRY (self->editor), attrs);
  pango_attr_list_unref (attrs);

  if (self->refs != NULL)
    g_array_unref (self->refs);
  self->refs = refs;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- Naming a function while it is being typed ------------------------- */

/* office42 knows 610 functions, and until now the only way to reach one
 * was to remember its name exactly or to leave the formula and open the
 * Function Wizard.  Two letters into a name, the ones that begin that
 * way are offered under the caret; Tab or Enter takes the highlighted
 * one and opens its bracket, as Excel and Gnumeric both do.
 *
 * The list never takes the keyboard: it is an ordinary child of the
 * grid, placed under the cell and painted over it, and nothing in it
 * can be focused, so what is typed goes on reaching the editor and the
 * list follows it.  A popover would have done neither: it takes a grab,
 * and it lives in a surface the grid cannot place or paint. */

#define COMPLETE_MOST 12

/* Where the name being typed begins, or -1: a run of letters and dots
 * that follows something a function may follow. */
static int
name_being_typed (const char *text, int caret)
{
  int start;

  if (text == NULL || text[0] != '=' || caret < 2)
    return -1;
  if (caret > (int) strlen (text))
    return -1;
  /* A name that is already opened is finished with. */
  if (text[caret] == '(')
    return -1;

  for (start = caret; start > 0; start--)
    {
      char c = text[start - 1];

      if (!g_ascii_isalpha (c) && !g_ascii_isdigit (c) && c != '.' && c != '_')
        break;
    }
  if (caret - start < 2)
    return -1;
  if (g_ascii_isdigit (text[start]))
    return -1;
  switch (text[start - 1])
    {
    case '=': case '+': case '-': case '*': case '/': case '^': case '&':
    case '<': case '>': case '(': case ',': case ';': case ':': case '%':
      return start;
    default:
      return -1;
    }
}

/* The list goes off the screen but the names it was made from stay:
 * offering a new list starts by taking the old one down. */
static void
complete_take_down (O42Grid *self)
{
  if (self->complete_box != NULL)
    {
      gtk_widget_unparent (self->complete_box);
      self->complete_box = NULL;
      self->complete_list = NULL;
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

static void
complete_hide (O42Grid *self)
{
  complete_take_down (self);
  if (self->complete_names != NULL)
    g_ptr_array_set_size (self->complete_names, 0);
  self->complete_start = -1;
  self->complete_at = 0;
}

/* Puts the highlight on the nth row, and nothing on the others. */
static void
complete_highlight (O42Grid *self)
{
  GtkWidget *row;
  int i = 0;

  if (self->complete_list == NULL)
    return;
  for (row = gtk_widget_get_first_child (self->complete_list);
       row != NULL;
       row = gtk_widget_get_next_sibling (row), i++)
    {
      if (i == self->complete_at)
        gtk_list_box_select_row (GTK_LIST_BOX (self->complete_list),
                                 GTK_LIST_BOX_ROW (row));
    }
}

/* Takes the highlighted name: it replaces what has been typed of it,
 * and its bracket is opened, which is what everybody expects and what
 * saves the second keystroke. */
static gboolean
complete_accept (O42Grid *self)
{
  const char *text, *name;
  char *before, *after, *whole;
  int caret;

  if (self->complete_box == NULL || self->complete_names == NULL ||
      self->complete_names->len == 0 || self->complete_start < 0)
    return FALSE;

  name = g_ptr_array_index (self->complete_names,
                            CLAMP (self->complete_at, 0,
                                   (int) self->complete_names->len - 1));
  text = gtk_editable_get_text (GTK_EDITABLE (self->editor));
  caret = gtk_editable_get_position (GTK_EDITABLE (self->editor));
  if (caret < 0 || caret > (int) strlen (text))
    caret = (int) strlen (text);

  before = g_strndup (text, (gsize) self->complete_start);
  after = g_strdup (text + caret);
  whole = g_strconcat (before, name, "(", after, NULL);
  caret = self->complete_start + (int) strlen (name) + 1;

  complete_hide (self);
  gtk_editable_set_text (GTK_EDITABLE (self->editor), whole);
  gtk_editable_set_position (GTK_EDITABLE (self->editor), caret);

  g_free (whole);
  g_free (after);
  g_free (before);
  return TRUE;
}

static void
on_complete_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
  O42Grid *self = data;

  (void) box;
  self->complete_at = gtk_list_box_row_get_index (row);
  complete_accept (self);
  gtk_widget_grab_focus (self->editor);
}

/* The names that begin the way the one being typed does, offered under
 * the caret. */
static void
complete_offer (O42Grid *self)
{
  const char *text;
  int caret, start;
  gsize length;
  guint n_names = 0;
  const char * const *names;

  if (!self->editing || self->editor == NULL)
    { complete_hide (self); return; }

  text = gtk_editable_get_text (GTK_EDITABLE (self->editor));
  caret = gtk_editable_get_position (GTK_EDITABLE (self->editor));
  if (caret < 0)
    caret = (int) strlen (text);
  start = name_being_typed (text, caret);
  if (start < 0)
    { complete_hide (self); return; }

  if (self->complete_names == NULL)
    self->complete_names = g_ptr_array_new ();
  g_ptr_array_set_size (self->complete_names, 0);

  length = (gsize) (caret - start);
  names = o42_function_names (&n_names);
  for (guint i = 0; i < n_names && self->complete_names->len < COMPLETE_MOST; i++)
    if (g_ascii_strncasecmp (names[i], text + start, length) == 0)
      g_ptr_array_add (self->complete_names, (gpointer) names[i]);

  if (self->complete_names->len == 0 ||
      (self->complete_names->len == 1 &&
       strlen (g_ptr_array_index (self->complete_names, 0)) == length))
    { complete_hide (self); return; }

  /* The list is made again each time: a dozen rows is nothing, and it
   * keeps the highlight and the names in step by construction. */
  complete_take_down (self);
  self->complete_start = start;
  self->complete_at = 0;

  {
    GtkWidget *frame = gtk_frame_new (NULL);
    GtkWidget *box = gtk_list_box_new ();

    for (guint i = 0; i < self->complete_names->len; i++)
      {
        const char *name = g_ptr_array_index (self->complete_names, i);
        const char *signature = NULL, *summary = NULL;
        GtkWidget *row = gtk_list_box_row_new ();
        GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new (name);

        /* The name in a column of its own so the eye can run down it,
         * and what the function does beside it, as Gnumeric shows it. */
        gtk_label_set_xalign (GTK_LABEL (label), 0.0);
        gtk_label_set_width_chars (GTK_LABEL (label), 12);
        gtk_box_append (GTK_BOX (line), label);

        if (o42_function_help (name, &signature, &summary) && summary != NULL)
          {
            GtkWidget *note = gtk_label_new (summary);

            gtk_label_set_xalign (GTK_LABEL (note), 0.0);
            gtk_label_set_ellipsize (GTK_LABEL (note), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars (GTK_LABEL (note), 40);
            gtk_widget_add_css_class (note, "o42-complete-note");
            gtk_box_append (GTK_BOX (line), note);
          }

        gtk_widget_set_can_focus (row, FALSE);
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), line);
        gtk_list_box_append (GTK_LIST_BOX (box), row);
      }
    g_signal_connect (box, "row-activated", G_CALLBACK (on_complete_row_activated), self);

    /* A child of the grid rather than a popover: a popover would take
     * the keyboard away from the cell being typed into, and it would
     * float in a surface of its own where the grid cannot place it. */
    gtk_frame_set_child (GTK_FRAME (frame), box);
    gtk_widget_add_css_class (frame, "o42-complete");
    gtk_widget_set_can_focus (frame, FALSE);
    gtk_widget_set_can_focus (box, FALSE);
    gtk_widget_set_parent (frame, GTK_WIDGET (self));

    self->complete_box = frame;
    self->complete_list = box;
    place_editor_extras (self);
    complete_highlight (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}

/* What the list makes of a key, or FALSE for a key it has no use for
 * -- which the editor then gets, so what is typed goes on reaching the
 * cell and the list follows it. */
static gboolean
complete_key (O42Grid *self, guint keyval)
{
  if (self->complete_box == NULL)
    return FALSE;

  switch (keyval)
    {
    case GDK_KEY_Down: case GDK_KEY_KP_Down:
      return complete_move (self, 1);
    case GDK_KEY_Up: case GDK_KEY_KP_Up:
      return complete_move (self, -1);
    case GDK_KEY_Tab:
    case GDK_KEY_Return: case GDK_KEY_KP_Enter:
      return complete_accept (self);
    case GDK_KEY_Escape:
      /* The first Escape takes the list away and leaves the formula. */
      complete_hide (self);
      return TRUE;
    default:
      return FALSE;
    }
}

/* Up and down walk the list while it is up. */
static gboolean
complete_move (O42Grid *self, int delta)
{
  if (self->complete_box == NULL || self->complete_names == NULL ||
      self->complete_names->len == 0)
    return FALSE;
  self->complete_at = CLAMP (self->complete_at + delta, 0,
                             (int) self->complete_names->len - 1);
  complete_highlight (self);
  return TRUE;
}

/* ---- What the call being typed wants next ------------------------------ */

/* Once the brackets are open, knowing the name is not enough: VLOOKUP
 * takes four arguments and nobody remembers which is which.  While the
 * caret stands inside a call's brackets, the call's signature is shown
 * under the cell with the argument the caret is on in bold, and it
 * follows the commas and the nesting -- inside SUM(A1,IF( it is IF's
 * signature that is wanted, not SUM's.
 *
 * Like the name list, it is a child of the grid rather than a popover,
 * for the same reasons. */

#define CALLS_DEEP 32

/* The innermost call whose brackets hold the caret: where its name
 * stands, and how many commas the caret is past. */
static gboolean
enclosing_call (const char *text, int caret,
                int *out_start, int *out_end, int *out_arg)
{
  struct { int start, end, arg; } open[CALLS_DEEP];
  int depth = 0;

  if (text == NULL || text[0] != '=')
    return FALSE;
  if (caret > (int) strlen (text))
    caret = (int) strlen (text);

  for (int i = 0; i < caret; i++)
    {
      char c = text[i];

      if (c == '"')
        {
          for (i++; i < caret && text[i] != '"'; i++)
            ;
          continue;
        }
      if (c == '(')
        {
          int s = i;

          while (s > 0 && (g_ascii_isalnum (text[s - 1]) ||
                           text[s - 1] == '.' || text[s - 1] == '_'))
            s--;
          if (depth < CALLS_DEEP)
            {
              open[depth].start = s;
              open[depth].end = i;
              open[depth].arg = 0;
            }
          depth++;
          continue;
        }
      if (c == ')')
        {
          if (depth > 0)
            depth--;
          continue;
        }
      if ((c == ',' || c == ';') && depth > 0 && depth <= CALLS_DEEP)
        open[depth - 1].arg++;
    }

  if (depth <= 0 || depth > CALLS_DEEP)
    return FALSE;
  /* A bracket with nothing in front of it groups, it does not call. */
  if (open[depth - 1].end == open[depth - 1].start)
    return FALSE;

  *out_start = open[depth - 1].start;
  *out_end = open[depth - 1].end;
  *out_arg = open[depth - 1].arg;
  return TRUE;
}

/* Where the nth argument stands in a signature of the form
 * NAME(a, b, ...), clamped to the last one so that SUM's third number
 * still lands on the "...". */
static gboolean
argument_span (const char *signature, int nth, int *start, int *end)
{
  int i = 0, depth = 0, seen = 0, from = -1;

  while (signature[i] != '\0' && signature[i] != '(')
    i++;
  if (signature[i] != '(')
    return FALSE;
  i++;
  from = i;

  for (; signature[i] != '\0'; i++)
    {
      if (signature[i] == '(')
        depth++;
      else if (signature[i] == ')' && depth > 0)
        depth--;
      else if (depth == 0 && (signature[i] == ')' || signature[i] == ','))
        {
          if (seen == nth || signature[i] == ')')
            {
              *start = from;
              *end = i;
              while (*start < *end && signature[*start] == ' ')
                (*start)++;
              return *end > *start;
            }
          seen++;
          from = i + 1;
        }
    }
  return FALSE;
}

static void
tip_hide (O42Grid *self)
{
  if (self->tip_box != NULL)
    {
      gtk_widget_unparent (self->tip_box);
      self->tip_box = NULL;
      self->tip_label = NULL;
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

/* The signature of the call the caret is in, or nothing. */
static void
tip_offer (O42Grid *self)
{
  const char *text, *signature = NULL, *summary = NULL;
  int caret, start, end, arg, from = 0, to = 0;
  char *name;
  PangoAttrList *attrs;

  if (!self->editing || self->editor == NULL)
    { tip_hide (self); return; }

  text = gtk_editable_get_text (GTK_EDITABLE (self->editor));
  caret = gtk_editable_get_position (GTK_EDITABLE (self->editor));
  if (caret < 0)
    caret = (int) strlen (text);
  if (!enclosing_call (text, caret, &start, &end, &arg))
    { tip_hide (self); return; }

  name = g_strndup (text + start, (gsize) (end - start));
  if (!o42_function_help (name, &signature, &summary) || signature == NULL)
    { g_free (name); tip_hide (self); return; }
  g_free (name);

  if (self->tip_box == NULL)
    {
      GtkWidget *frame = gtk_frame_new (NULL);
      GtkWidget *label = gtk_label_new (NULL);

      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_widget_add_css_class (frame, "o42-argtip");
      gtk_widget_set_can_focus (frame, FALSE);
      gtk_frame_set_child (GTK_FRAME (frame), label);
      gtk_widget_set_parent (frame, GTK_WIDGET (self));
      self->tip_box = frame;
      self->tip_label = label;
    }

  gtk_label_set_text (GTK_LABEL (self->tip_label), signature);
  attrs = pango_attr_list_new ();
  if (argument_span (signature, arg, &from, &to))
    {
      PangoAttribute *a = pango_attr_weight_new (PANGO_WEIGHT_BOLD);

      a->start_index = (guint) from;
      a->end_index = (guint) to;
      pango_attr_list_insert (attrs, a);
    }
  gtk_label_set_attributes (GTK_LABEL (self->tip_label), attrs);
  pango_attr_list_unref (attrs);

  place_editor_extras (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- Point mode -------------------------------------------------------- */

/* Half of writing a formula is not typing it: you type "=SUM(" and then
 * click the cells you mean, and the reference appears where the caret
 * is.  Excel calls it point mode, and every spreadsheet since VisiCalc
 * has had it.
 *
 * It begins where a reference could begin -- after the leading "=",
 * after an operator, after a bracket or a comma -- and nowhere else, so
 * that an arrow key after "=A1" still means what it always did and
 * leaves the cell.  While it lasts, the arrows and the pointer move the
 * reference rather than the cursor, and the text between point_start
 * and point_end is rewritten every time it moves. */

/* Whether a reference may be written at `caret`: what stands before it
 * has to be something a reference can follow. */
static gboolean
ref_may_follow (const char *text, int caret)
{
  int i;

  if (text == NULL || text[0] != '=')
    return FALSE;
  if (caret <= 0 || caret > (int) strlen (text))
    return FALSE;

  for (i = caret - 1; i > 0 && text[i] == ' '; i--)
    ;
  switch (text[i])
    {
    case '=': case '+': case '-': case '*': case '/': case '^': case '&':
    case '<': case '>': case '(': case ',': case ';': case ':': case '%':
      return TRUE;
    default:
      return FALSE;
    }
}

/* The reference the pointed rectangle comes to, written into the text
 * between point_start and point_end. */
static void
point_write (O42Grid *self)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->editor));
  O42Range r = o42_range_normalise (self->point_row, self->point_col,
                                    self->point_row2, self->point_col2);
  char *first = o42_ref_name (r.row0, r.col0);
  char *last = o42_ref_name (r.row1, r.col1);
  char *ref = (r.row0 == r.row1 && r.col0 == r.col1)
                ? g_strdup (first) : g_strdup_printf ("%s:%s", first, last);
  char *before = g_strndup (text, (gsize) self->point_start);
  char *after = g_strdup (text + self->point_end);
  char *whole = g_strconcat (before, ref, after, NULL);

  self->pointing = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (self->editor), whole);
  self->point_end = self->point_start + (int) strlen (ref);
  gtk_editable_set_position (GTK_EDITABLE (self->editor), self->point_end);

  g_free (whole);
  g_free (after);
  g_free (before);
  g_free (ref);
  g_free (last);
  g_free (first);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* Starts pointing at a cell, replacing nothing: the reference goes in
 * at the caret. */
static void
point_begin (O42Grid *self, int row, int col)
{
  int caret = gtk_editable_get_position (GTK_EDITABLE (self->editor));

  self->point_start = caret;
  self->point_end = caret;
  self->point_row = self->point_row2 = CLAMP (row, 0, O42_MAX_ROWS - 1);
  self->point_col = self->point_col2 = CLAMP (col, 0, O42_MAX_COLS - 1);
  point_write (self);
}

/* Leaves the reference where it is: the next thing typed is not part
 * of it. */
static void
point_end_mode (O42Grid *self)
{
  if (!self->pointing)
    return;
  self->pointing = FALSE;
  self->point_dragging = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* A click or a drag over the grid while a formula is being typed. */
static gboolean
point_at (O42Grid *self, int row, int col, gboolean extend)
{
  const char *text;
  int caret;

  if (!self->editing || self->editor == NULL || row < 0 || col < 0)
    return FALSE;

  text = gtk_editable_get_text (GTK_EDITABLE (self->editor));
  caret = gtk_editable_get_position (GTK_EDITABLE (self->editor));
  if (caret < 0)
    caret = (int) strlen (text);

  if (self->pointing && extend)
    {
      self->point_row2 = CLAMP (row, 0, O42_MAX_ROWS - 1);
      self->point_col2 = CLAMP (col, 0, O42_MAX_COLS - 1);
      point_write (self);
      return TRUE;
    }

  if (!self->pointing && !ref_may_follow (text, caret))
    return FALSE;

  if (!self->pointing)
    point_begin (self, row, col);
  else
    {
      self->point_row = self->point_row2 = CLAMP (row, 0, O42_MAX_ROWS - 1);
      self->point_col = self->point_col2 = CLAMP (col, 0, O42_MAX_COLS - 1);
      point_write (self);
    }
  return TRUE;
}

/* An arrow key while a formula is being typed: it moves the reference
 * when one may stand where the caret is, and otherwise means what it
 * always did. */
static gboolean
point_arrow (O42Grid *self, int drow, int dcol, gboolean extend, gboolean to_edge)
{
  const char *text;
  int caret, row, col;

  if (!self->editing || self->editor == NULL)
    return FALSE;

  text = gtk_editable_get_text (GTK_EDITABLE (self->editor));
  caret = gtk_editable_get_position (GTK_EDITABLE (self->editor));

  if (!self->pointing)
    {
      if (!ref_may_follow (text, caret))
        return FALSE;
      /* The first step is taken from the cell the formula is in. */
      row = self->active_row + drow;
      col = self->active_col + dcol;
      if (to_edge)
        {
          row = self->active_row;
          col = self->active_col;
          o42_sheet_edge (self->sheet, &row, &col, drow, dcol);
        }
      point_begin (self, row, col);
      return TRUE;
    }

  row = extend ? self->point_row2 : self->point_row;
  col = extend ? self->point_col2 : self->point_col;
  if (to_edge)
    o42_sheet_edge (self->sheet, &row, &col, drow, dcol);
  else
    {
      row += drow;
      col += dcol;
    }
  row = CLAMP (row, 0, O42_MAX_ROWS - 1);
  col = CLAMP (col, 0, O42_MAX_COLS - 1);

  self->point_row2 = row;
  self->point_col2 = col;
  if (!extend)
    {
      self->point_row = row;
      self->point_col = col;
    }
  point_write (self);
  return TRUE;
}

/* ---- F4: the dollars on a reference ------------------------------------ */

/* Where the reference at (or just before) `caret` begins and ends, and
 * what it says.  A reference is a run of an optional dollar, one to
 * three letters, an optional dollar and the digits: the caret may be
 * anywhere in it, or just past its end, which is where it sits after
 * one has been typed or pointed at. */
static gboolean
ref_at_caret (const char *text, int caret, int *start, int *end,
              int *row, int *col, gboolean *row_abs, gboolean *col_abs)
{
  int n = (int) strlen (text);
  int from, to;

  for (from = MIN (caret, n); from > 0; from--)
    {
      char c = text[from - 1];

      if (!g_ascii_isalnum (c) && c != '$')
        break;
    }
  for (to = from; to < n; to++)
    {
      char c = text[to];

      if (!g_ascii_isalnum (c) && c != '$')
        break;
    }
  if (to < caret || from > caret)
    return FALSE;

  {
    char *token = g_strndup (text + from, (gsize) (to - from));
    gboolean ok = o42_ref_parse_full (token, row, col, row_abs, col_abs, NULL);

    g_free (token);
    if (!ok)
      return FALSE;
  }
  *start = from;
  *end = to;
  return TRUE;
}

/* A1 -> $A$1 -> A$1 -> $A1 -> A1, which is the order Excel goes round
 * in and the one everybody's fingers know.  A range takes its dollars
 * on both halves at once, which is what makes F4 worth pressing on the
 * range a formula was pointed at. */
static gboolean
cycle_dollars (O42Grid *self)
{
  const char *text;
  int caret, start, end, row, col;
  int other_start = -1, other_end = -1, other_row = 0, other_col = 0;
  gboolean row_abs, col_abs, other_row_abs = FALSE, other_col_abs = FALSE;
  gboolean second_half = FALSE;
  char *before, *after, *ref, *whole;

  if (!self->editing || self->editor == NULL)
    return FALSE;

  text = gtk_editable_get_text (GTK_EDITABLE (self->editor));
  caret = gtk_editable_get_position (GTK_EDITABLE (self->editor));
  if (caret < 0)
    caret = (int) strlen (text);
  if (text[0] != '=')
    return FALSE;
  if (!ref_at_caret (text, caret, &start, &end, &row, &col, &row_abs, &col_abs))
    return FALSE;

  /* The other half, when the reference is one end of a range. */
  if (start > 0 && text[start - 1] == ':' &&
      ref_at_caret (text, start - 1, &other_start, &other_end,
                    &other_row, &other_col, &other_row_abs, &other_col_abs))
    second_half = TRUE;
  else if (text[end] == ':' &&
           ref_at_caret (text, end + 2, &other_start, &other_end,
                         &other_row, &other_col, &other_row_abs, &other_col_abs))
    second_half = FALSE;
  else
    other_start = -1;

  /* The first half decides which way round the pair goes next. */
  if (other_start >= 0 && second_half)
    { row_abs = other_row_abs; col_abs = other_col_abs; }

  if (!row_abs && !col_abs)       { row_abs = TRUE;  col_abs = TRUE;  }
  else if (row_abs && col_abs)    { row_abs = TRUE;  col_abs = FALSE; }
  else if (row_abs && !col_abs)   { row_abs = FALSE; col_abs = TRUE;  }
  else                            { row_abs = FALSE; col_abs = FALSE; }

  if (other_start >= 0)
    {
      char *a, *b;
      int from = second_half ? other_start : start;
      int to = second_half ? end : other_end;

      a = o42_ref_name_full (second_half ? other_row : row,
                             second_half ? other_col : col, row_abs, col_abs);
      b = o42_ref_name_full (second_half ? row : other_row,
                             second_half ? col : other_col, row_abs, col_abs);
      ref = g_strconcat (a, ":", b, NULL);
      g_free (a);
      g_free (b);
      start = from;
      end = to;
    }
  else
    ref = o42_ref_name_full (row, col, row_abs, col_abs);

  before = g_strndup (text, (gsize) start);
  after = g_strdup (text + end);
  whole = g_strconcat (before, ref, after, NULL);

  gtk_editable_set_text (GTK_EDITABLE (self->editor), whole);
  gtk_editable_set_position (GTK_EDITABLE (self->editor), start + (int) strlen (ref));

  /* A reference that has been given its dollars is finished with: the
   * next arrow starts a new one rather than moving this. */
  if (self->pointing)
    self->point_end = start + (int) strlen (ref);

  g_free (whole);
  g_free (after);
  g_free (before);
  g_free (ref);
  return TRUE;
}

gboolean
o42_grid_point_step (O42Grid *self, const char *direction)
{
  gboolean extend = FALSE, edge = FALSE;
  int drow = 0, dcol = 0;

  g_return_val_if_fail (O42_IS_GRID (self), FALSE);
  if (direction == NULL)
    return FALSE;
  while (TRUE)
    {
      if (g_str_has_prefix (direction, "shift-")) { extend = TRUE; direction += 6; }
      else if (g_str_has_prefix (direction, "ctrl-")) { edge = TRUE; direction += 5; }
      else break;
    }
  if (g_ascii_strcasecmp (direction, "f4") == 0)
    return cycle_dollars (self);

  /* The name list answers first, exactly as it does under a real
   * keyboard. */
  {
    guint keyval = 0;

    if (g_ascii_strcasecmp (direction, "tab") == 0)         keyval = GDK_KEY_Tab;
    else if (g_ascii_strcasecmp (direction, "enter") == 0)  keyval = GDK_KEY_Return;
    else if (g_ascii_strcasecmp (direction, "escape") == 0) keyval = GDK_KEY_Escape;
    else if (g_ascii_strcasecmp (direction, "up") == 0)     keyval = GDK_KEY_Up;
    else if (g_ascii_strcasecmp (direction, "down") == 0)   keyval = GDK_KEY_Down;

    if (keyval != 0 && complete_key (self, keyval))
      return TRUE;

    /* With no list up they finish the entry, as they do at a keyboard. */
    if (keyval == GDK_KEY_Return && self->editing)
      { o42_grid_commit_edit (self); return TRUE; }
    if (keyval == GDK_KEY_Escape && self->editing)
      { o42_grid_cancel_edit (self); return TRUE; }
    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_Return ||
        keyval == GDK_KEY_Escape)
      return FALSE;
  }

  if (g_ascii_strcasecmp (direction, "left") == 0)       dcol = -1;
  else if (g_ascii_strcasecmp (direction, "right") == 0) dcol = 1;
  else if (g_ascii_strcasecmp (direction, "up") == 0)    drow = -1;
  else if (g_ascii_strcasecmp (direction, "down") == 0)  drow = 1;
  else
    {
      /* Anything that reads as a reference is a click on it, so that a
       * script can say what it does in the order it does it. */
      O42Range r;
      gsize used = 0;

      if (!o42_ref_parse (direction, &r.row0, &r.col0, &used) || used == 0)
        return FALSE;
      if (direction[used] != ':' ||
          !o42_ref_parse (direction + used + 1, &r.row1, &r.col1, NULL))
        { r.row1 = r.row0; r.col1 = r.col0; }
      o42_grid_click_range (self, &r);
      return TRUE;
    }

  return point_arrow (self, drow, dcol, extend, edge);
}

void
o42_grid_click_range (O42Grid *self, const O42Range *range)
{
  g_return_if_fail (O42_IS_GRID (self));
  if (range == NULL)
    return;

  /* The rule a click follows: point at it while a formula is being
   * typed and a reference may stand where the caret is, and otherwise
   * commit what is there and select. */
  if (point_at (self, range->row0, range->col0, FALSE))
    {
      if (range->row1 != range->row0 || range->col1 != range->col0)
        point_at (self, range->row1, range->col1, TRUE);
      return;
    }
  if (self->editing)
    o42_grid_commit_edit (self);
  move_active (self, range->row0, range->col0, FALSE);
  if (range->row1 != range->row0 || range->col1 != range->col0)
    move_active (self, range->row1, range->col1, TRUE);
}

void
o42_grid_begin_edit (O42Grid *self, const char *initial)
{
  GtkEventController *key;

  g_return_if_fail (O42_IS_GRID (self));

  if (self->sheet == NULL || self->editing)
    return;

  if (o42_sheet_is_chart_sheet (self->sheet))
    return;

  /* A protected sheet refuses the cells its formats lock. */
  if (!o42_sheet_cell_editable (self->sheet, self->active_row, self->active_col))
    {
      gtk_widget_error_bell (GTK_WIDGET (self));
      return;
    }

  self->editor = gtk_entry_new ();
  gtk_widget_add_css_class (self->editor, "o42-cell-editor");
  gtk_entry_set_has_frame (GTK_ENTRY (self->editor), FALSE);
  gtk_widget_set_parent (self->editor, GTK_WIDGET (self));

  g_signal_connect (self->editor, "changed", G_CALLBACK (on_editor_changed), self);
  g_signal_connect_swapped (self->editor, "notify::cursor-position",
                            G_CALLBACK (tip_offer), self);

  if (initial != NULL)
    {
      /* The character that opened the editor is delivered to the grid, not
       * to an entry that did not exist when the key went down, so it has to
       * be put in by hand.  It also has to be put in *before* the entry is
       * focused, or focus-in selects the whole text and the next character
       * replaces it. */
      gtk_editable_set_text (GTK_EDITABLE (self->editor), initial);
    }
  else
    {
      char *current = o42_sheet_get_input (self->sheet,
                                           self->active_row, self->active_col);
      gtk_editable_set_text (GTK_EDITABLE (self->editor), current);
      gtk_editable_set_position (GTK_EDITABLE (self->editor), -1);
      g_free (current);
    }

  g_signal_connect (self->editor, "activate",
                    G_CALLBACK (on_editor_activate), self);

  key = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (key, GTK_PHASE_CAPTURE);
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_editor_key), self);
  gtk_widget_add_controller (self->editor, key);

  self->editing = TRUE;
  colour_editor_refs (self);
  place_editor (self);
  gtk_widget_grab_focus (self->editor);

  /* Focusing a GtkEntry selects its text; the caret belongs at the end. */
  gtk_editable_select_region (GTK_EDITABLE (self->editor), -1, -1);
  gtk_editable_set_position (GTK_EDITABLE (self->editor), -1);

  /* The text put in by hand did not go through the editor's own
   * changed signal with the caret where it now is. */
  complete_offer (self);
  tip_offer (self);
}

static void
end_edit (O42Grid *self)
{
  if (self->editor != NULL)
    {
      gtk_widget_unparent (self->editor);
      self->editor = NULL;
    }

  complete_hide (self);
  tip_hide (self);
  self->editing = FALSE;
  if (self->refs != NULL)
    g_array_set_size (self->refs, 0);
  self->pointing = FALSE;
  self->point_dragging = FALSE;
  gtk_widget_grab_focus (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* Data > Validation: an entry the active cell's rules refuse is
 * announced and dropped, as Excel's "stop" style does. */
static gboolean
entry_allowed (O42Grid *self, const char *text)
{
  char *message = NULL;

  if (o42_sheet_validate (self->sheet, self->active_row, self->active_col, text, &message))
    return TRUE;
  {
    GtkAlertDialog *alert = gtk_alert_dialog_new ("%s", message);
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

    gtk_alert_dialog_set_detail (alert, _("Data > Validation limits what this cell may hold."));
    gtk_alert_dialog_show (alert, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL);
    g_object_unref (alert);
  }
  g_free (message);
  return FALSE;
}

static void commit_editor_runs (O42Grid *self, const char *text);

void
o42_grid_commit_edit (O42Grid *self)
{
  const char *text;

  g_return_if_fail (O42_IS_GRID (self));

  if (!self->editing || self->editor == NULL)
    return;

  text = gtk_editable_get_text (GTK_EDITABLE (self->editor));
  if (!entry_allowed (self, text))
    {
      end_edit (self);
      return;
    }
  o42_sheet_set_input (self->sheet, self->active_row, self->active_col, text);
  commit_editor_runs (self, text);

  end_edit (self);
  sheet_changed (self);
}

void
o42_grid_cancel_edit (O42Grid *self)
{
  g_return_if_fail (O42_IS_GRID (self));

  g_clear_pointer (&self->edit_fmt, g_array_unref);
  g_clear_pointer (&self->edit_text, g_free);

  if (!self->editing)
    return;

  end_edit (self);
}

void
o42_grid_set_active_input (O42Grid *self, const char *text)
{
  g_return_if_fail (O42_IS_GRID (self));

  if (self->sheet == NULL)
    return;

  if (self->editing)
    o42_grid_cancel_edit (self);

  if (!entry_allowed (self, text))
    return;
  o42_sheet_set_input (self->sheet, self->active_row, self->active_col, text);
  sheet_changed (self);
}

void
o42_grid_delete_selection (O42Grid *self)
{
  O42Range range;

  g_return_if_fail (O42_IS_GRID (self));

  if (self->sheet == NULL)
    return;

  {
    GArray *ranges = g_array_new (FALSE, FALSE, sizeof (O42Range));

    selection_ranges (self, ranges);
    o42_sheet_begin_group (self->sheet);
    for (guint i = 0; i < ranges->len; i++)
      o42_sheet_clear_range (self->sheet, &g_array_index (ranges, O42Range, i));
    o42_sheet_end_group (self->sheet);
    g_array_unref (ranges);
  }
  (void) range;
  sheet_changed (self);
}

void
o42_grid_undo (O42Grid *self)
{
  O42Range touched;

  g_return_if_fail (O42_IS_GRID (self));

  if (self->sheet == NULL)
    return;

  if (self->editing)
    o42_grid_cancel_edit (self);

  if (o42_sheet_undo (self->sheet, &touched))
    {
      move_active (self, touched.row0, touched.col0, FALSE);
      sheet_changed (self);
    }
}

void
o42_grid_redo (O42Grid *self)
{
  O42Range touched;

  g_return_if_fail (O42_IS_GRID (self));

  if (self->sheet == NULL)
    return;

  if (self->editing)
    o42_grid_cancel_edit (self);

  if (o42_sheet_redo (self->sheet, &touched))
    {
      move_active (self, touched.row0, touched.col0, FALSE);
      sheet_changed (self);
    }
}

/* ---------------------------------------------------------------------- */
/* Clipboard                                                               */
/* ---------------------------------------------------------------------- */

/* Tab-separated rows of the cells' *inputs*, so a formula copied and pasted
 * is still a formula.  That is also what Excel, Calc and Gnumeric put on the
 * clipboard as text, so it exchanges both ways. */
void
o42_grid_copy (O42Grid *self)
{
  O42Range range;
  GString *out;

  g_return_if_fail (O42_IS_GRID (self));

  if (self->sheet == NULL)
    return;

  selection_range (self, &range);
  out = g_string_new (NULL);

  for (int row = range.row0; row <= range.row1; row++)
    {
      for (int col = range.col0; col <= range.col1; col++)
        {
          char *input = o42_sheet_get_input (self->sheet, row, col);

          if (col > range.col0)
            g_string_append_c (out, '\t');
          g_string_append (out, input);
          g_free (input);
        }
      g_string_append_c (out, '\n');
    }

  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)),
                          out->str);

  /* Remembering what was put there, and where it came from, is what lets a
   * paste of our own copy relocate its formulas.  If the clipboard has
   * changed hands since, the text will not match and the paste is plain. */
  g_free (self->clip_text);
  self->clip_text = g_string_free (out, FALSE);
  self->clip_range = range;
}

gboolean
o42_grid_has_own_copy (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), FALSE);
  return self->clip_text != NULL;
}

void
o42_grid_paste_special (O42Grid *self, O42PasteMode mode, gboolean transpose)
{
  g_return_if_fail (O42_IS_GRID (self));

  if (self->sheet == NULL || self->clip_text == NULL)
    return;

  if (self->editing)
    o42_grid_commit_edit (self);

  o42_sheet_copy_range_special (self->sheet, &self->clip_range,
                                self->active_row, self->active_col, mode, transpose);
  sheet_changed (self);
}

void
o42_grid_cut (O42Grid *self)
{
  g_return_if_fail (O42_IS_GRID (self));

  o42_grid_copy (self);
  o42_grid_delete_selection (self);
}

static void
on_paste_text (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Grid *self = data;
  char *text;
  char **lines;

  text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (source), result, NULL);

  if (text != NULL && self->sheet != NULL && self->clip_text != NULL &&
      strcmp (text, self->clip_text) == 0)
    {
      /* Our own copy, coming back: paste it with its references moved, and
       * tile it across the selection if the selection is a whole number of
       * copies, as Excel does. */
      O42Range sel;
      int rows = self->clip_range.row1 - self->clip_range.row0 + 1;
      int cols = self->clip_range.col1 - self->clip_range.col0 + 1;
      int across = 1, down = 1;

      selection_range (self, &sel);
      if ((sel.row1 - sel.row0 + 1) % rows == 0 &&
          (sel.col1 - sel.col0 + 1) % cols == 0)
        {
          down = (sel.row1 - sel.row0 + 1) / rows;
          across = (sel.col1 - sel.col0 + 1) / cols;
        }
      else
        {
          sel.row0 = self->active_row;
          sel.col0 = self->active_col;
        }

      o42_sheet_begin_group (self->sheet);
      for (int r = 0; r < down; r++)
        for (int c = 0; c < across; c++)
          o42_sheet_copy_range (self->sheet, &self->clip_range,
                                sel.row0 + r * rows, sel.col0 + c * cols);
      o42_sheet_end_group (self->sheet);
      sheet_changed (self);
    }
  else if (text != NULL && self->sheet != NULL)
    {
      lines = g_strsplit (text, "\n", -1);

      o42_sheet_begin_group (self->sheet);

      for (guint r = 0; lines[r] != NULL; r++)
        {
          char **cells;
          char *line = lines[r];
          gsize len = strlen (line);

          /* A trailing empty line is the newline after the last row, not a
           * row of its own. */
          if (len == 0 && lines[r + 1] == NULL)
            break;

          if (len > 0 && line[len - 1] == '\r')
            line[len - 1] = '\0';

          cells = g_strsplit (line, "\t", -1);

          for (guint c = 0; cells[c] != NULL; c++)
            o42_sheet_set_input (self->sheet,
                                 self->active_row + (int) r,
                                 self->active_col + (int) c,
                                 cells[c]);

          g_strfreev (cells);
        }

      o42_sheet_end_group (self->sheet);
      g_strfreev (lines);
      sheet_changed (self);
    }

  g_free (text);
  g_object_unref (self);
}

void
o42_grid_paste (O42Grid *self)
{
  g_return_if_fail (O42_IS_GRID (self));

  if (self->editing)
    o42_grid_commit_edit (self);

  gdk_clipboard_read_text_async (gtk_widget_get_clipboard (GTK_WIDGET (self)),
                                 NULL, on_paste_text, g_object_ref (self));
}

/* ---------------------------------------------------------------------- */
/* Fill, and rows and columns                                              */
/* ---------------------------------------------------------------------- */

void
o42_grid_fill (O42Grid *self, gboolean down)
{
  O42Range range;

  g_return_if_fail (O42_IS_GRID (self));

  if (self->sheet == NULL)
    return;

  if (self->editing)
    o42_grid_commit_edit (self);

  selection_range (self, &range);
  o42_sheet_fill (self->sheet, &range, down);
  sheet_changed (self);
}

static void
grid_band_op (O42Grid *self, gboolean rows, gboolean insert)
{
  O42Range range;
  int at, count;

  if (self->sheet == NULL)
    return;

  if (self->editing)
    o42_grid_commit_edit (self);

  selection_range (self, &range);
  at = rows ? range.row0 : range.col0;
  count = rows ? range.row1 - range.row0 + 1 : range.col1 - range.col0 + 1;

  if (rows && insert)        o42_sheet_insert_rows (self->sheet, at, count);
  else if (rows)             o42_sheet_delete_rows (self->sheet, at, count);
  else if (insert)           o42_sheet_insert_cols (self->sheet, at, count);
  else                       o42_sheet_delete_cols (self->sheet, at, count);

  gtk_widget_queue_resize (GTK_WIDGET (self));
  sheet_changed (self);
}

void o42_grid_insert_rows    (O42Grid *self) { g_return_if_fail (O42_IS_GRID (self)); grid_band_op (self, TRUE,  TRUE);  }
void o42_grid_insert_columns (O42Grid *self) { g_return_if_fail (O42_IS_GRID (self)); grid_band_op (self, FALSE, TRUE);  }
void o42_grid_delete_rows    (O42Grid *self) { g_return_if_fail (O42_IS_GRID (self)); grid_band_op (self, TRUE,  FALSE); }
void o42_grid_delete_columns (O42Grid *self) { g_return_if_fail (O42_IS_GRID (self)); grid_band_op (self, FALSE, FALSE); }

void
o42_grid_set_column_width (O42Grid *self, int width)
{
  O42Range range;

  g_return_if_fail (O42_IS_GRID (self));
  if (self->sheet == NULL)
    return;

  selection_range (self, &range);
  for (int col = range.col0; col <= range.col1; col++)
    o42_sheet_set_col_width (self->sheet, col, width);

  gtk_widget_queue_resize (GTK_WIDGET (self));
  sheet_changed (self);
}

void
o42_grid_set_row_height (O42Grid *self, int height)
{
  O42Range range;

  g_return_if_fail (O42_IS_GRID (self));
  if (self->sheet == NULL)
    return;

  selection_range (self, &range);
  for (int row = range.row0; row <= range.row1; row++)
    o42_sheet_set_row_height (self->sheet, row, height);

  gtk_widget_queue_resize (GTK_WIDGET (self));
  sheet_changed (self);
}

void
o42_grid_autofit_columns (O42Grid *self)
{
  O42Range range;

  g_return_if_fail (O42_IS_GRID (self));
  if (self->sheet == NULL)
    return;

  selection_range (self, &range);
  for (int col = range.col0; col <= range.col1; col++)
    o42_sheet_set_col_width (self->sheet, col, column_fit_width (self, col));

  gtk_widget_queue_resize (GTK_WIDGET (self));
  sheet_changed (self);
}

/* ---------------------------------------------------------------------- */
/* Formatting                                                              */
/* ---------------------------------------------------------------------- */

void
o42_grid_insert_picture (O42Grid    *self,
                         GBytes     *data,
                         const char *format,
                         int         pixel_w,
                         int         pixel_h)
{
  O42Picture *pic;
  const double max_w = 480.0;

  g_return_if_fail (O42_IS_GRID (self));
  g_return_if_fail (data != NULL);

  if (self->sheet == NULL || pixel_w <= 0 || pixel_h <= 0)
    return;

  if (self->editing)
    o42_grid_commit_edit (self);

  pic = o42_sheet_add_picture (self->sheet, data, format, pixel_w, pixel_h,
                               self->active_row, self->active_col);
  if (pic == NULL)
    return;

  if (pic->width > max_w)
    {
      pic->height = pic->height * (max_w / pic->width);
      pic->width = max_w;
    }

  self->selected_picture = pic->id;
  sheet_changed (self);
}

void
o42_grid_guess_chart_labels (O42Grid *self, gboolean *first_row, gboolean *first_col)
{
  O42Range range;
  O42Chart *probe;

  g_return_if_fail (O42_IS_GRID (self));

  *first_row = *first_col = FALSE;
  if (self->sheet == NULL)
    return;

  selection_range (self, &range);
  probe = o42_sheet_add_chart (self->sheet, O42_CHART_COLUMN, &range, 0, 0);
  if (probe != NULL)
    {
      *first_row = probe->first_row_labels;
      *first_col = probe->first_col_labels;
      o42_sheet_remove_chart (self->sheet, probe->id);
    }
}

void
o42_grid_insert_chart (O42Grid *self, O42ChartKind kind, const char *title,
                       gboolean series_in_rows,
                       gboolean first_row_labels, gboolean first_col_labels)
{
  O42Range range;
  O42Chart *chart;
  int row, col;

  g_return_if_fail (O42_IS_GRID (self));

  if (self->sheet == NULL)
    return;

  if (self->editing)
    o42_grid_commit_edit (self);

  selection_range (self, &range);
  row = MIN (range.row1 + 2, O42_MAX_ROWS - 1);
  col = MIN (range.col1 + 2, O42_MAX_COLS - 1);
  chart = o42_sheet_add_chart (self->sheet, kind, &range, row, col);
  if (chart == NULL)
    return;

  if (title != NULL)
    {
      g_free (chart->title);
      chart->title = g_strdup (title);
    }
  chart->series_in_rows = series_in_rows;
  chart->first_row_labels = first_row_labels;
  chart->first_col_labels = first_col_labels;

  self->selected_picture = chart->id;
  self->selected_is_chart = TRUE;
  sheet_changed (self);
}

/* Formatting part of the text in a cell rather than the whole cell:
 * while a cell is being edited and part of its text is selected, the
 * font buttons work on that part.  A look is kept for every byte of
 * the text; when the edit is committed those become the cell's runs. */
static gboolean
apply_to_editor_selection (O42Grid *self, O42FmtMask mask, const O42Fmt *value)
{
  int start = 0, end = 0;
  const char *text;
  O42Fmt base;

  if (!self->editing || self->editor == NULL)
    return FALSE;
  if (!gtk_editable_get_selection_bounds (GTK_EDITABLE (self->editor), &start, &end))
    return FALSE;

  text = gtk_editable_get_text (GTK_EDITABLE (self->editor));
  if (text == NULL || *text == '\0' || text[0] == '=')
    return FALSE;

  /* The offsets come back in characters and the runs are in bytes. */
  {
    const char *from = g_utf8_offset_to_pointer (text, start);
    const char *to = g_utf8_offset_to_pointer (text, end);

    start = (int) (from - text);
    end = (int) (to - text);
  }
  if (end <= start)
    return FALSE;

  base = *o42_sheet_get_fmt (self->sheet, self->active_row, self->active_col);
  if (self->edit_fmt == NULL || g_strcmp0 (self->edit_text, text) != 0)
    {
      gsize length = strlen (text);

      g_clear_pointer (&self->edit_fmt, g_array_unref);
      self->edit_fmt = g_array_sized_new (FALSE, FALSE, sizeof (O42Fmt), length);
      for (gsize i = 0; i < length; i++)
        g_array_append_val (self->edit_fmt, base);
      g_free (self->edit_text);
      self->edit_text = g_strdup (text);
    }

  for (int i = start; i < end && i < (int) self->edit_fmt->len; i++)
    o42_fmt_apply_mask (&g_array_index (self->edit_fmt, O42Fmt, i), mask, value);

  /* Show it in the editor as it will look in the cell. */
  {
    GArray *runs = g_array_new (FALSE, FALSE, sizeof (O42TextRun));
    PangoAttrList *attrs;

    for (guint i = 0; i < self->edit_fmt->len; i++)
      {
        const O42Fmt *fmt = &g_array_index (self->edit_fmt, O42Fmt, i);

        if (i == 0 || memcmp (fmt, &g_array_index (self->edit_fmt, O42Fmt, i - 1),
                              sizeof *fmt) != 0)
          {
            O42TextRun run = { (int) i, *fmt };

            g_array_append_val (runs, run);
          }
      }
    attrs = o42_runs_attributes (&g_array_index (runs, O42TextRun, 0),
                                 (int) runs->len, &base, text);
    gtk_entry_set_attributes (GTK_ENTRY (self->editor), attrs);
    if (attrs != NULL)
      pango_attr_list_unref (attrs);
    g_array_unref (runs);
  }
  return TRUE;
}

/* The runs the finished edit leaves on the cell, if any. */
static void
commit_editor_runs (O42Grid *self, const char *text)
{
  GArray *runs;

  if (self->edit_fmt == NULL)
    return;
  if (text == NULL || g_strcmp0 (self->edit_text, text) != 0)
    {
      /* The text was typed on after the formatting: the offsets no
       * longer mean anything, so the cell goes back to one font. */
      g_clear_pointer (&self->edit_fmt, g_array_unref);
      g_clear_pointer (&self->edit_text, g_free);
      return;
    }

  runs = g_array_new (FALSE, FALSE, sizeof (O42TextRun));
  for (guint i = 0; i < self->edit_fmt->len; i++)
    {
      const O42Fmt *fmt = &g_array_index (self->edit_fmt, O42Fmt, i);

      if (i == 0 || memcmp (fmt, &g_array_index (self->edit_fmt, O42Fmt, i - 1),
                            sizeof *fmt) != 0)
        {
          O42TextRun run = { (int) i, *fmt };

          g_array_append_val (runs, run);
        }
    }
  if (runs->len > 1)
    o42_sheet_set_runs (self->sheet, self->active_row, self->active_col,
                        &g_array_index (runs, O42TextRun, 0), (int) runs->len);
  g_array_unref (runs);
  g_clear_pointer (&self->edit_fmt, g_array_unref);
  g_clear_pointer (&self->edit_text, g_free);
}

void
o42_grid_apply_fmt (O42Grid *self, O42FmtMask mask, const O42Fmt *value)
{
  O42Range range;

  g_return_if_fail (O42_IS_GRID (self));
  g_return_if_fail (value != NULL);

  if (self->sheet == NULL)
    return;

  /* Part of one cell's text, when that is what is selected. */
  if (apply_to_editor_selection (self, mask, value))
    return;

  {
    GArray *ranges = g_array_new (FALSE, FALSE, sizeof (O42Range));

    selection_ranges (self, ranges);
    o42_sheet_begin_group (self->sheet);
    for (guint i = 0; i < ranges->len; i++)
      o42_sheet_apply_fmt (self->sheet, &g_array_index (ranges, O42Range, i), mask, value);
    o42_sheet_end_group (self->sheet);
    g_array_unref (ranges);
  }
  (void) range;
  sheet_changed (self);
}

const O42Fmt *
o42_grid_active_fmt (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), NULL);

  if (self->sheet == NULL)
    return NULL;

  return o42_sheet_get_fmt (self->sheet, self->active_row, self->active_col);
}

void
o42_grid_refresh (O42Grid *self)
{
  g_return_if_fail (O42_IS_GRID (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
o42_grid_hide_rows (O42Grid *self, gboolean hide)
{
  O42Range range;

  g_return_if_fail (O42_IS_GRID (self));
  if (self->sheet == NULL)
    return;

  selection_range (self, &range);
  for (int row = range.row0; row <= range.row1; row++)
    o42_sheet_set_row_hidden (self->sheet, row, hide);

  gtk_widget_queue_resize (GTK_WIDGET (self));
  sheet_changed (self);
}

void
o42_grid_hide_columns (O42Grid *self, gboolean hide)
{
  O42Range range;

  g_return_if_fail (O42_IS_GRID (self));
  if (self->sheet == NULL)
    return;

  selection_range (self, &range);
  for (int col = range.col0; col <= range.col1; col++)
    o42_sheet_set_col_hidden (self->sheet, col, hide);

  gtk_widget_queue_resize (GTK_WIDGET (self));
  sheet_changed (self);
}

void
o42_grid_toggle_autofilter (O42Grid *self)
{
  O42Range range;

  g_return_if_fail (O42_IS_GRID (self));
  if (self->sheet == NULL)
    return;

  if (o42_sheet_get_autofilter (self->sheet, NULL))
    {
      o42_sheet_clear_autofilter (self->sheet);
    }
  else
    {
      selection_range (self, &range);

      /* A single cell means the block of data around it: the used range,
       * which is what Excel guesses too. */
      if (range.row0 == range.row1 && range.col0 == range.col1)
        {
          O42Range used;
          o42_sheet_used_range (self->sheet, &used);
          range = used;
        }
      o42_sheet_set_autofilter (self->sheet, &range);
    }

  gtk_widget_queue_resize (GTK_WIDGET (self));
  sheet_changed (self);
}

/* The dropdown of one AutoFilter column: "(All)" and every distinct
 * value, in a popover over the heading. */
static void
on_filter_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
  O42Grid *self = data;
  GtkWidget *popover = gtk_widget_get_ancestor (GTK_WIDGET (box), GTK_TYPE_POPOVER);
  int col = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (box), "o42-col"));
  const char *value = g_object_get_data (G_OBJECT (row), "o42-value");

  if (g_object_get_data (G_OBJECT (row), "o42-custom") != NULL)
    {
      if (popover != NULL)
        gtk_popover_popdown (GTK_POPOVER (popover));
      show_custom_filter (self, col);
      return;
    }

  if (self->sheet != NULL)
    o42_sheet_autofilter_choose (self->sheet, col, value);

  if (popover != NULL)
    gtk_popover_popdown (GTK_POPOVER (popover));
  gtk_widget_queue_resize (GTK_WIDGET (self));
  sheet_changed (self);
}

/* Custom AutoFilter: an operator and a value, composed into a criterion
 * the sheet reads as COUNTIF would. */
typedef struct {
  O42Grid   *grid;
  int        col;
  GtkWidget *window, *op, *value;
} CustomFilter;

static const char *CUSTOM_OPS[] = {
  "equals", "does not equal", "is greater than", "is greater than or equal to",
  "is less than", "is less than or equal to", "begins with", "ends with", "contains", NULL
};

static void
on_custom_filter_ok (GtkWidget *button, gpointer data)
{
  CustomFilter *cf = data;
  const char *value = gtk_editable_get_text (GTK_EDITABLE (cf->value));
  guint op = gtk_drop_down_get_selected (GTK_DROP_DOWN (cf->op));
  static const char *prefix[] = { "=", "<>", ">", ">=", "<", "<=", "", "*", "*" };
  static const char *suffix[] = { "", "", "", "", "", "", "*", "", "*" };
  char *criterion;

  (void) button;
  if (op >= G_N_ELEMENTS (prefix)) op = 0;
  criterion = g_strconcat (prefix[op], value, suffix[op], NULL);
  if (cf->grid->sheet != NULL)
    o42_sheet_autofilter_choose (cf->grid->sheet, cf->col, value[0] ? criterion : NULL);
  g_free (criterion);
  gtk_widget_queue_resize (GTK_WIDGET (cf->grid));
  sheet_changed (cf->grid);
  gtk_window_destroy (GTK_WINDOW (cf->window));
}

static void
show_custom_filter (O42Grid *self, int col)
{
  CustomFilter *cf = g_new0 (CustomFilter, 1);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *ok = gtk_button_new_with_mnemonic (_("_OK"));
  GtkWidget *cancel = gtk_button_new_with_mnemonic (_("_Cancel"));
  const char *current = self->sheet ? o42_sheet_autofilter_choice (self->sheet, col) : NULL;

  cf->grid = self;
  cf->col = col;
  cf->window = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (cf->window), _("Custom AutoFilter"));
  gtk_window_set_modal (GTK_WINDOW (cf->window), TRUE);
  gtk_window_set_resizable (GTK_WINDOW (cf->window), FALSE);
  if (GTK_IS_WINDOW (root))
    gtk_window_set_transient_for (GTK_WINDOW (cf->window), GTK_WINDOW (root));

  gtk_widget_set_margin_start (box, 12); gtk_widget_set_margin_end (box, 12);
  gtk_widget_set_margin_top (box, 12); gtk_widget_set_margin_bottom (box, 12);
  gtk_box_append (GTK_BOX (box), gtk_label_new (_("Show rows where the column:")));
  cf->op = gtk_drop_down_new_from_strings (CUSTOM_OPS);
  cf->value = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (cf->value), 14);
  if (current != NULL)
    {
      /* Take the current criterion apart, so the dialog shows it. */
      static const char *prefix[] = { "<>", ">=", "<=", "=", ">", "<" };
      static const guint which[] = { 1, 3, 5, 0, 2, 4 };
      gboolean done = FALSE;
      for (guint i = 0; i < G_N_ELEMENTS (prefix) && !done; i++)
        if (g_str_has_prefix (current, prefix[i]))
          {
            gtk_drop_down_set_selected (GTK_DROP_DOWN (cf->op), which[i]);
            gtk_editable_set_text (GTK_EDITABLE (cf->value), current + strlen (prefix[i]));
            done = TRUE;
          }
      if (!done)
        {
          gsize len = strlen (current);
          gboolean star0 = current[0] == '*', star1 = len > 0 && current[len - 1] == '*';
          char *inner = g_strndup (current + (star0 ? 1 : 0), len - (star0 ? 1 : 0) - (star1 && len > 1 ? 1 : 0));
          gtk_drop_down_set_selected (GTK_DROP_DOWN (cf->op), star0 && star1 ? 8 : star0 ? 7 : star1 ? 6 : 0);
          gtk_editable_set_text (GTK_EDITABLE (cf->value), inner);
          g_free (inner);
        }
    }
  gtk_box_append (GTK_BOX (row), cf->op);
  gtk_box_append (GTK_BOX (row), cf->value);
  gtk_box_append (GTK_BOX (box), row);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (buttons), ok);
  gtk_box_append (GTK_BOX (buttons), cancel);
  gtk_box_append (GTK_BOX (box), buttons);
  gtk_window_set_child (GTK_WINDOW (cf->window), box);
  gtk_window_set_default_widget (GTK_WINDOW (cf->window), ok);
  gtk_entry_set_activates_default (GTK_ENTRY (cf->value), TRUE);
  g_signal_connect (ok, "clicked", G_CALLBACK (on_custom_filter_ok), cf);
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (gtk_window_destroy), cf->window);
  g_signal_connect_swapped (cf->window, "destroy", G_CALLBACK (g_free), cf);
  gtk_window_present (GTK_WINDOW (cf->window));
  gtk_widget_grab_focus (cf->value);
}

static void
show_filter_menu (O42Grid *self, int col, double x, double y)
{
  GtkWidget *popover = gtk_popover_new ();
  GtkWidget *scrolled = gtk_scrolled_window_new ();
  GtkWidget *box = gtk_list_box_new ();
  GtkWidget *row;
  char **values = o42_sheet_autofilter_values (self->sheet, col);
  double px = x - FILTER_BTN, py = y - FILTER_BTN;
  GdkRectangle at;

  pin_point (self, &px, &py);
  at.x = (int) (px * self->zoom);
  at.y = (int) (py * self->zoom);
  at.width = at.height = (int) (FILTER_BTN * self->zoom);

  g_object_set_data (G_OBJECT (box), "o42-col", GINT_TO_POINTER (col));
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box), GTK_SELECTION_NONE);
  g_signal_connect (box, "row-activated", G_CALLBACK (on_filter_row_activated), self);

  row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), gtk_label_new (_("(All)")));
  gtk_list_box_append (GTK_LIST_BOX (box), row);

  row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), gtk_label_new (_("(Custom...)")));
  g_object_set_data (G_OBJECT (row), "o42-custom", GINT_TO_POINTER (1));
  gtk_list_box_append (GTK_LIST_BOX (box), row);

  for (int i = 0; values != NULL && values[i] != NULL; i++)
    {
      GtkWidget *label = gtk_label_new (values[i]);
      row = gtk_list_box_row_new ();
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
      g_object_set_data_full (G_OBJECT (row), "o42-value", g_strdup (values[i]), g_free);
      gtk_list_box_append (GTK_LIST_BOX (box), row);
    }
  g_strfreev (values);

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled), 260);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), box);
  gtk_widget_set_size_request (scrolled, 140, -1);

  gtk_popover_set_child (GTK_POPOVER (popover), scrolled);
  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &at);
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  gtk_widget_set_parent (popover, GTK_WIDGET (self));
  g_signal_connect (popover, "closed", G_CALLBACK (gtk_widget_unparent), NULL);
  gtk_popover_popup (GTK_POPOVER (popover));
}

/* Is the point on the AutoFilter button of a heading cell?  Returns the
 * column, or -1. */
static int
filter_button_at (O42Grid *self, double x, double y)
{
  O42Range filter;

  if (self->sheet == NULL || !o42_sheet_get_autofilter (self->sheet, &filter))
    return -1;

  for (int col = filter.col0; col <= filter.col1; col++)
    {
      double cx = col_x (self, col) + o42_sheet_col_width (self->sheet, col);
      double cy = row_y (self, filter.row0) + o42_sheet_row_height (self->sheet, filter.row0);

      if (o42_sheet_col_width (self->sheet, col) == 0)
        continue;
      if (x >= cx - FILTER_BTN && x < cx && y >= cy - FILTER_BTN && y < cy)
        return col;
    }

  return -1;
}

void
o42_grid_merge_cells (O42Grid *self, gboolean merge)
{
  O42Range range;

  g_return_if_fail (O42_IS_GRID (self));
  if (self->sheet == NULL)
    return;

  if (self->editing)
    o42_grid_commit_edit (self);

  selection_range (self, &range);
  if (merge)
    {
      o42_sheet_merge (self->sheet, &range);
      move_active (self, range.row0, range.col0, FALSE);
    }
  else
    o42_sheet_unmerge (self->sheet, &range);

  sheet_changed (self);
}

void
o42_grid_freeze_panes (O42Grid *self)
{
  g_return_if_fail (O42_IS_GRID (self));

  if (self->frozen_rows > 0 || self->frozen_cols > 0)
    {
      self->frozen_rows = self->frozen_cols = 0;
    }
  else
    {
      /* Rows above and columns left of the active cell, as Excel freezes
       * them; at A1, Excel freezes at the middle of the window, which is
       * rarely what anyone meant, so here it does nothing. */
      self->frozen_rows = self->active_row;
      self->frozen_cols = self->active_col;
    }

  if (self->sheet != NULL)
    o42_sheet_set_frozen (self->sheet, self->frozen_rows, self->frozen_cols);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* Window > Split: two or four views of the sheet, divided above and
 * left of the active cell, each showing whatever it is scrolled to.
 * Splitting again takes the split away, as Excel's menu item does. */
/* Tools > Auditing: an arrow from each rectangle the cell's formula
 * reads, or from the cell to each formula that reads it. */
void
o42_grid_trace (O42Grid *self, gboolean precedents)
{
  GArray *found;
  O42Range sel = { 0, 0, 0, 0 };

  g_return_if_fail (O42_IS_GRID (self));
  if (self->sheet == NULL)
    return;
  if (self->arrows == NULL)
    self->arrows = g_array_new (FALSE, FALSE, sizeof (AuditArrow));

  o42_grid_get_selection (self, &sel);
  found = precedents ? o42_sheet_precedents (self->sheet, sel.row0, sel.col0)
                     : o42_sheet_dependents (self->sheet, sel.row0, sel.col0);
  for (guint i = 0; i < found->len; i++)
    {
      const O42Range *r = &g_array_index (found, O42Range, i);
      AuditArrow arrow;

      if (precedents)
        {
          arrow.from = *r;
          arrow.to_row = sel.row0;
          arrow.to_col = sel.col0;
        }
      else
        {
          arrow.from.row0 = arrow.from.row1 = sel.row0;
          arrow.from.col0 = arrow.from.col1 = sel.col0;
          arrow.to_row = r->row0;
          arrow.to_col = r->col0;
        }
      g_array_append_val (self->arrows, arrow);
    }
  g_array_unref (found);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
o42_grid_clear_arrows (O42Grid *self)
{
  g_return_if_fail (O42_IS_GRID (self));
  if (self->arrows != NULL)
    g_array_set_size (self->arrows, 0);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
o42_grid_has_arrows (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), FALSE);
  return self->arrows != NULL && self->arrows->len > 0;
}

/* View > Page Breaks: dashed lines where the printed pages divide,
 * which is Excel's page break preview without the resizing. */
void
o42_grid_show_page_breaks (O42Grid *self, gboolean show)
{
  g_return_if_fail (O42_IS_GRID (self));
  self->show_breaks = show;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
o42_grid_page_breaks_shown (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), FALSE);
  return self->show_breaks;
}

/* Moving an object that belongs to a group moves the rest of it with
 * the same step, which is what grouping is for. */
static void
move_group_with (O42Grid *self, guint id, int drow, int dcol, double ddx, double ddy)
{
  guint group = o42_sheet_object_group (self->sheet, id);
  GPtrArray *lists[3];

  if (group == 0 || (drow == 0 && dcol == 0 && ddx == 0 && ddy == 0))
    return;
  lists[0] = o42_sheet_pictures (self->sheet);
  lists[1] = o42_sheet_shapes (self->sheet);
  lists[2] = o42_sheet_charts (self->sheet);

  for (guint i = 0; i < lists[0]->len; i++)
    {
      O42Picture *p = g_ptr_array_index (lists[0], i);

      if (p->group == group && p->id != id)
        { p->row = CLAMP (p->row + drow, 0, O42_MAX_ROWS - 1);
          p->col = CLAMP (p->col + dcol, 0, O42_MAX_COLS - 1);
          p->dx += ddx; p->dy += ddy; }
    }
  for (guint i = 0; i < lists[1]->len; i++)
    {
      O42Shape *shape = g_ptr_array_index (lists[1], i);

      if (shape->group == group && shape->id != id)
        { shape->row = CLAMP (shape->row + drow, 0, O42_MAX_ROWS - 1);
          shape->col = CLAMP (shape->col + dcol, 0, O42_MAX_COLS - 1);
          shape->dx += ddx; shape->dy += ddy; }
    }
  for (guint i = 0; i < lists[2]->len; i++)
    {
      O42Chart *chart = g_ptr_array_index (lists[2], i);

      if (chart->group == group && chart->id != id)
        { chart->row = CLAMP (chart->row + drow, 0, O42_MAX_ROWS - 1);
          chart->col = CLAMP (chart->col + dcol, 0, O42_MAX_COLS - 1);
          chart->dx += ddx; chart->dy += ddy; }
    }
}

/* Format > Group and Ungroup, over the objects anchored in the
 * selection. */
void
o42_grid_group_objects (O42Grid *self, gboolean group)
{
  O42Range range;

  g_return_if_fail (O42_IS_GRID (self));
  if (self->sheet == NULL)
    return;
  selection_range (self, &range);
  if (group)
    o42_sheet_group_objects (self->sheet, &range);
  else
    o42_sheet_ungroup_objects (self->sheet, &range);
  sheet_changed (self);
}

void
o42_grid_split_panes (O42Grid *self)
{
  g_return_if_fail (O42_IS_GRID (self));

  if (self->split)
    {
      self->split = FALSE;
      self->frozen_rows = self->frozen_cols = 0;
    }
  else
    {
      self->split = TRUE;
      self->frozen_rows = self->active_row;
      self->frozen_cols = self->active_col;
      self->split_top_row = 0;
      self->split_left_col = 0;
      if (self->frozen_rows == 0 && self->frozen_cols == 0)
        self->split = FALSE;   /* at A1 there is nothing to divide */
    }
  if (self->sheet != NULL)
    o42_sheet_set_frozen (self->sheet, self->split ? 0 : self->frozen_rows,
                          self->split ? 0 : self->frozen_cols);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
o42_grid_is_split (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), FALSE);
  return self->split;
}

/* Scrolls the pane the pointer is over.  Returns FALSE when it is over
 * the main pane, which the scrolled window handles itself. */
gboolean
o42_grid_scroll_pane (O42Grid *self, double x, double y, double dy)
{
  double band_w, band_h;
  int step;

  g_return_val_if_fail (O42_IS_GRID (self), FALSE);
  if (!self->split || self->sheet == NULL)
    return FALSE;

  band_w = col_x (self, self->frozen_cols);
  band_h = row_y (self, self->frozen_rows);
  step = dy > 0 ? 3 : -3;

  if (y < band_h && self->frozen_rows > 0)
    {
      self->split_top_row = CLAMP (self->split_top_row + step, 0, O42_MAX_ROWS - 1);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return TRUE;
    }
  if (x < band_w && self->frozen_cols > 0)
    {
      self->split_left_col = CLAMP (self->split_left_col + step, 0, O42_MAX_COLS - 1);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return TRUE;
    }
  return FALSE;
}

gboolean
o42_grid_has_frozen_panes (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), FALSE);
  return self->frozen_rows > 0 || self->frozen_cols > 0;
}

void
o42_grid_set_zoom (O42Grid *self, double zoom)
{
  g_return_if_fail (O42_IS_GRID (self));

  self->zoom = CLAMP (zoom, 0.25, 4.0);
  if (self->editing)
    place_editor (self);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

double
o42_grid_get_zoom (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), 1.0);
  return self->zoom;
}

void
o42_grid_set_show_gridlines (O42Grid *self, gboolean show)
{
  g_return_if_fail (O42_IS_GRID (self));
  self->hide_gridlines = !show;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
o42_grid_get_show_gridlines (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), TRUE);
  return !self->hide_gridlines;
}

void
o42_grid_set_show_zeros (O42Grid *self, gboolean show)
{
  g_return_if_fail (O42_IS_GRID (self));
  self->hide_zeros = !show;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
o42_grid_get_show_zeros (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), TRUE);
  return !self->hide_zeros;
}

/* ---------------------------------------------------------------------- */
/* Input                                                                   */
/* ---------------------------------------------------------------------- */

static gboolean
on_key_pressed (GtkEventControllerKey *controller,
                guint                  keyval,
                guint                  keycode,
                GdkModifierType        state,
                gpointer               data)
{
  O42Grid *self = data;
  gboolean shift = (state & GDK_SHIFT_MASK) != 0;
  gboolean ctrl  = (state & GDK_CONTROL_MASK) != 0;
  int row = self->active_row;
  int col = self->active_col;

  (void) controller; (void) keycode;

  if (self->sheet == NULL || self->editing)
    return GDK_EVENT_PROPAGATE;

  switch (keyval)
    {
    case GDK_KEY_Left:  case GDK_KEY_KP_Left:
    case GDK_KEY_Right: case GDK_KEY_KP_Right:
    case GDK_KEY_Up:    case GDK_KEY_KP_Up:
    case GDK_KEY_Down:  case GDK_KEY_KP_Down:
      {
        int drow = (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) ? -1
                 : (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) ? 1 : 0;
        int dcol = (keyval == GDK_KEY_Left || keyval == GDK_KEY_KP_Left) ? -1
                 : (keyval == GDK_KEY_Right || keyval == GDK_KEY_KP_Right) ? 1 : 0;

        /* Ctrl+arrow jumps to the edge of the data, as it has since Lotus. */
        if (ctrl)
          o42_sheet_edge (self->sheet, &row, &col, drow, dcol);
        else
          {
            row += drow;
            col += dcol;
          }
        move_active (self, row, col, shift);
        return GDK_EVENT_STOP;
      }

    case GDK_KEY_End:   case GDK_KEY_KP_End:
      {
        /* Ctrl+End is the last used cell; End alone the end of the row. */
        O42Range used;
        o42_sheet_used_range (self->sheet, &used);
        if (ctrl)
          move_active (self, used.row1, used.col1, shift);
        else
          {
            int c = col;
            o42_sheet_edge (self->sheet, &row, &c, 0, 1);
            move_active (self, row, c, shift);
          }
        return GDK_EVENT_STOP;
      }

    case GDK_KEY_space:
      /* Shift+Space selects the row, Ctrl+Space the column. */
      if (shift && !ctrl)
        {
          O42Range r = { row, 0, row, O42_MAX_COLS - 1 };
          o42_grid_select_range (self, &r);
          self->active_col = col;
          return GDK_EVENT_STOP;
        }
      if (ctrl && !shift)
        {
          O42Range r = { 0, col, O42_MAX_ROWS - 1, col };
          o42_grid_select_range (self, &r);
          self->active_row = row;
          return GDK_EVENT_STOP;
        }
      break;

    case GDK_KEY_Page_Up:
      move_active (self, row - 20, col, shift);
      return GDK_EVENT_STOP;

    case GDK_KEY_Page_Down:
      move_active (self, row + 20, col, shift);
      return GDK_EVENT_STOP;

    case GDK_KEY_Home:  case GDK_KEY_KP_Home:
      move_active (self, ctrl ? 0 : row, 0, shift);
      return GDK_EVENT_STOP;

    case GDK_KEY_Tab:
      {
        int origin = (self->tab_origin_col >= 0) ? self->tab_origin_col : col;
        move_active (self, row, col + 1, FALSE);
        self->tab_origin_col = origin;
        return GDK_EVENT_STOP;
      }

    case GDK_KEY_ISO_Left_Tab:
      {
        int origin = (self->tab_origin_col >= 0) ? self->tab_origin_col : col;
        move_active (self, row, col - 1, FALSE);
        self->tab_origin_col = origin;
        return GDK_EVENT_STOP;
      }

    case GDK_KEY_Return: case GDK_KEY_KP_Enter:
      {
        int back = (self->tab_origin_col >= 0) ? self->tab_origin_col : col;
        move_active (self, shift ? row - 1 : row + 1, back, FALSE);
        return GDK_EVENT_STOP;
      }

    case GDK_KEY_F2:
      o42_grid_begin_edit (self, NULL);
      return GDK_EVENT_STOP;

    case GDK_KEY_Delete: case GDK_KEY_KP_Delete:
      if (self->selected_picture != 0)
        {
          if (self->selected_is_shape)
            o42_sheet_remove_shape (self->sheet, self->selected_picture);
          else if (self->selected_is_chart)
            o42_sheet_remove_chart (self->sheet, self->selected_picture);
          else
            o42_sheet_remove_picture (self->sheet, self->selected_picture);
          self->selected_picture = 0;
          sheet_changed (self);
          return GDK_EVENT_STOP;
        }
      o42_grid_delete_selection (self);
      return GDK_EVENT_STOP;

    case GDK_KEY_BackSpace:
      /* Backspace on a cell empties it and opens it for typing, which is
       * how Excel 5 behaved and is still the fastest way to retype one. */
      o42_grid_begin_edit (self, "");
      return GDK_EVENT_STOP;

    case GDK_KEY_Escape:
      move_active (self, row, col, FALSE);
      return GDK_EVENT_STOP;

    default:
      break;
    }

  /* Any printable character starts editing with that character, replacing
   * what was there.  Typing into a spreadsheet has always worked this way. */
  if (!ctrl)
    {
      gunichar c = gdk_keyval_to_unicode (keyval);

      if (c != 0 && g_unichar_isprint (c))
        {
          char utf8[8];
          int n = g_unichar_to_utf8 (c, utf8);

          utf8[n] = '\0';
          o42_grid_begin_edit (self, utf8);
          return GDK_EVENT_STOP;
        }
    }

  return GDK_EVENT_PROPAGATE;
}

static void
on_click_pressed (GtkGestureClick *gesture,
                  int              n_press,
                  double           x,
                  double           y,
                  gpointer         data)
{
  O42Grid *self = data;
  int row, col;
  GdkModifierType state;

  if (self->sheet == NULL)
    return;

  /* The pointer arrives in widget pixels; the grid thinks in sheet pixels. */
  x /= self->zoom;
  y /= self->zoom;
  unpin_point (self, &x, &y);

  state = gtk_event_controller_get_current_event_state (
            GTK_EVENT_CONTROLLER (gesture));

  row = row_at_y (self, y);
  col = col_at_x (self, x);

  /* A click while a formula is being typed writes the cell into it,
   * and the button stays down to drag out a range. */
  if (self->editing && point_at (self, row, col, (state & GDK_SHIFT_MASK) != 0))
    {
      self->point_dragging = TRUE;
      return;
    }

  if (self->editing)
    o42_grid_commit_edit (self);

  gtk_widget_grab_focus (GTK_WIDGET (self));

  /* Ctrl+click on a cell adds a rectangle to the selection, as it does
   * everywhere else: what was selected is put behind, and a new one
   * starts where the pointer is.  A plain click forgets them all. */
  if ((state & GDK_CONTROL_MASK) && row >= 0 && col >= 0 &&
      o42_sheet_get_link (self->sheet, row, col) == NULL)
    {
      O42Range current;

      if (self->extra_sel == NULL)
        self->extra_sel = g_array_new (FALSE, FALSE, sizeof (O42Range));
      selection_range (self, &current);
      g_array_append_val (self->extra_sel, current);
      self->anchor_row = self->active_row = row;
      self->anchor_col = self->active_col = col;
      self->dragging = TRUE;
      selection_changed (self);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

  /* Ctrl+click on a hyperlink follows it: a place in the sheet is
   * selected, anything else goes to the system. */
  if ((state & GDK_CONTROL_MASK) && row >= 0 && col >= 0 &&
      o42_sheet_get_link (self->sheet, row, col) != NULL)
    {
      const char *target = o42_sheet_get_link (self->sheet, row, col);
      if (target[0] == '#')
        {
          O42Node *tree = o42_formula_parse (target + 1);
          if (tree->type == O42_NODE_REF && (tree->sheet == NULL || g_ascii_strcasecmp (tree->sheet, o42_sheet_get_name (self->sheet)) == 0))
            {
              O42Range one = { tree->as.ref.row, tree->as.ref.col, tree->as.ref.row, tree->as.ref.col };
              o42_grid_select_range (self, &one);
            }
          else if (tree->type == O42_NODE_RANGE && (tree->sheet == NULL || g_ascii_strcasecmp (tree->sheet, o42_sheet_get_name (self->sheet)) == 0))
            o42_grid_select_range (self, &tree->as.range);
          o42_node_free (tree);
        }
      else
        {
          GtkUriLauncher *launcher = gtk_uri_launcher_new (target);
          GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
          gtk_uri_launcher_launch (launcher, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, NULL, NULL);
          g_object_unref (launcher);
        }
      return;
    }

  /* A box in the outline margin folds or unfolds its group. */
  if (outline_click (self, x, y))
    return;

  /* The AutoFilter button in a heading opens its dropdown. */
  {
    int fcol = filter_button_at (self, x, y);

    if (fcol >= 0)
      {
        show_filter_menu (self, fcol, x, y);
        return;
      }
  }

  /* With the painter loaded, the next click puts the look down and
   * nothing else happens. */
  if (self->painting)
    {
      int paint_row = row_at_y (self, y), paint_col = col_at_x (self, x);

      if (paint_row >= 0 && paint_col >= 0)
        {
          paint_onto (self, paint_row, paint_col);
          return;
        }
      self->painting = FALSE;
      gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "cell");
    }

  /* A split bar can be taken hold of and moved, as in Excel. */
  {
    int bar = split_bar_at (self, x, y);
    int at = 0;

    if (bar != 0)
      {
        self->split_drag = bar;
        return;
      }
    bar = page_break_at (self, x, y, &at);
    if (bar != 0)
      {
        self->break_drag = bar;
        self->break_from = at;
        return;
      }
  }

  /* A handle of the selected object starts a resize. */
  {
    int handle = handle_at (self, x, y);

    if (handle >= 0)
      {
        o42_sheet_begin_group (self->sheet);
        o42_sheet_capture_object (self->sheet, self->selected_picture);
        self->resize_handle = handle;
        self->drag_mouse_x = x;
        self->drag_mouse_y = y;
        selected_object_rect (self, &self->resize_x0, &self->resize_y0,
                              &self->resize_w0, &self->resize_h0);
        return;
      }
  }

  /* A picture or chart under the pointer takes the click before any cell
   * does. */
  {
    O42Picture *pic = picture_at (self, x, y);
    O42Chart *chart = chart_at (self, x, y);
    O42Shape *shape = shape_at (self, x, y);

    /* A form control is worked by a plain click, as it would be in
     * Excel; Ctrl+click takes hold of it instead, to move it or to
     * give it a linked cell. */
    if (shape != NULL && !(state & GDK_CONTROL_MASK) &&
        o42_shape_is_control (shape->kind) && control_pressed (self, shape, x, y))
      return;

    if (shape != NULL)
      {
        double px, py, pw, ph;

        shape_rect (self, shape, &px, &py, &pw, &ph);
        self->selected_picture = shape->id;
        self->selected_is_chart = FALSE;
        self->selected_is_shape = TRUE;
        o42_sheet_begin_group (self->sheet);
        o42_sheet_capture_object (self->sheet, shape->id);
        self->picture_drag = TRUE;
        self->drag_mouse_x = x;
        self->drag_mouse_y = y;
        self->drag_pic_x = px;
        self->drag_pic_y = py;
        gtk_widget_queue_draw (GTK_WIDGET (self));
        return;
      }

    if (chart != NULL)
      {
        double px, py, pw, ph;

        chart_rect (self, chart, &px, &py, &pw, &ph);
        self->selected_picture = chart->id;
        self->selected_is_chart = TRUE;
        self->selected_is_shape = FALSE;
        o42_sheet_begin_group (self->sheet);
        o42_sheet_capture_object (self->sheet, chart->id);
        self->picture_drag = TRUE;
        self->drag_mouse_x = x;
        self->drag_mouse_y = y;
        self->drag_pic_x = px;
        self->drag_pic_y = py;
        gtk_widget_queue_draw (GTK_WIDGET (self));
        return;
      }

    if (pic != NULL)
      {
        double px, py, pw, ph;

        picture_rect (self, pic, &px, &py, &pw, &ph);
        self->selected_picture = pic->id;
        self->selected_is_chart = FALSE;
        self->selected_is_shape = FALSE;
        o42_sheet_begin_group (self->sheet);
        o42_sheet_capture_object (self->sheet, pic->id);
        self->picture_drag = TRUE;
        self->drag_mouse_x = x;
        self->drag_mouse_y = y;
        self->drag_pic_x = px;
        self->drag_pic_y = py;
        gtk_widget_queue_draw (GTK_WIDGET (self));
        return;
      }

    if (self->selected_picture != 0)
      {
        self->selected_picture = 0;
        gtk_widget_queue_draw (GTK_WIDGET (self));
      }
  }

  /* The selection's own outline starts a move of the cells, or a copy
   * of them when Ctrl is held, as Excel has always had it. */
  if (row >= 0 && col >= 0 && on_selection_edge (self, x, y))
    {
      selection_range (self, &self->move_source);
      self->move_row = self->move_source.row0;
      self->move_col = self->move_source.col0;
      self->move_drag = TRUE;
      self->move_copy = (state & GDK_CONTROL_MASK) != 0;
      self->drag_mouse_x = x;
      self->drag_mouse_y = y;
      return;
    }

  /* The fill handle starts an autofill drag. */
  if (row >= 0 && col >= 0 && on_fill_handle (self, x, y))
    {
      selection_range (self, &self->fill_source);
      self->fill_target = self->fill_source;
      self->fill_drag = TRUE;
      return;
    }

  /* A boundary in a header is grabbed for resizing; double-clicked, it
   * fits the column to its contents. */
  {
    int bc = col_boundary_at (self, x, y);
    int br = row_boundary_at (self, x, y);

    if (bc >= 0)
      {
        if (n_press == 2)
          {
            o42_sheet_set_col_width (self->sheet, bc, column_fit_width (self, bc));
            gtk_widget_queue_resize (GTK_WIDGET (self));
            sheet_changed (self);
            return;
          }
        self->resize_col = bc;
        self->resize_start = x;
        self->resize_size = o42_sheet_col_width (self->sheet, bc);
        return;
      }

    if (br >= 0)
      {
        self->resize_row = br;
        self->resize_start = y;
        self->resize_size = o42_sheet_row_height (self->sheet, br);
        return;
      }
  }

  /* A click in the column header selects the column; in the row header,
   * the row; in the corner, everything. */
  if (row < 0 && col < 0)
    {
      o42_grid_select_all (self);
      return;
    }

  if (row < 0)
    {
      O42Range r = { 0, col, O42_MAX_ROWS - 1, col };
      o42_grid_select_range (self, &r);
      self->active_row = 0;
      return;
    }

  if (col < 0)
    {
      O42Range r = { row, 0, row, O42_MAX_COLS - 1 };
      o42_grid_select_range (self, &r);
      self->active_col = 0;
      return;
    }

  if (n_press == 2)
    {
      move_active (self, row, col, FALSE);
      o42_grid_begin_edit (self, NULL);
      return;
    }

  move_active (self, row, col, (state & GDK_SHIFT_MASK) != 0);
  self->dragging = TRUE;
}

static void
on_click_released (GtkGestureClick *gesture, int n_press,
                   double x, double y, gpointer data)
{
  O42Grid *self = data;

  (void) gesture; (void) n_press; (void) x; (void) y;

  self->dragging = FALSE;
  self->point_dragging = FALSE;
  self->thumb_shape = 0;
  self->split_drag = 0;

  /* A page break dropped somewhere else becomes a manual break there,
   * and the one it came from stops being one if it was. */
  if (self->break_drag != 0 && self->sheet != NULL)
    {
      double px = x / self->zoom, py = y / self->zoom;
      gboolean rows = self->break_drag == 1;
      int to;

      unpin_point (self, &px, &py);
      to = rows ? row_at_y (self, py) : col_at_x (self, px);
      if (to > 0 && to != self->break_from)
        {
          if (o42_sheet_page_break (self->sheet, rows, self->break_from))
            o42_sheet_toggle_page_break (self->sheet, rows, self->break_from);
          if (!o42_sheet_page_break (self->sheet, rows, to))
            o42_sheet_toggle_page_break (self->sheet, rows, to);
          sheet_changed (self);
        }
      self->break_drag = 0;
    }

  if (self->move_drag)
    {
      self->move_drag = FALSE;
      if (self->sheet != NULL &&
          (self->move_row != self->move_source.row0 || self->move_col != self->move_source.col0))
        {
          O42Range landed = { self->move_row, self->move_col,
                              self->move_row + (self->move_source.row1 - self->move_source.row0),
                              self->move_col + (self->move_source.col1 - self->move_source.col0) };

          if (self->move_copy)
            o42_sheet_copy_range (self->sheet, &self->move_source,
                                  self->move_row, self->move_col);
          else
            o42_sheet_move_range (self->sheet, &self->move_source,
                                  self->move_row, self->move_col);
          o42_grid_select_range (self, &landed);
          sheet_changed (self);
        }
      else
        gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

  if (self->fill_drag)
    {
      self->fill_drag = FALSE;
      if (self->sheet != NULL &&
          (self->fill_target.row1 != self->fill_source.row1 ||
           self->fill_target.row0 != self->fill_source.row0 ||
           self->fill_target.col1 != self->fill_source.col1 ||
           self->fill_target.col0 != self->fill_source.col0))
        {
          o42_sheet_autofill (self->sheet, &self->fill_source, &self->fill_target);
          o42_grid_select_range (self, &self->fill_target);
          sheet_changed (self);
        }
      else
        gtk_widget_queue_draw (GTK_WIDGET (self));
    }

  if (self->resize_col >= 0 || self->resize_row >= 0)
    {
      self->resize_col = self->resize_row = -1;
      sheet_changed (self);
    }

  if (self->picture_drag)
    {
      self->picture_drag = FALSE;
      if (self->sheet != NULL)
        o42_sheet_end_group (self->sheet);
      sheet_changed (self);
    }

  if (self->resize_handle >= 0)
    {
      self->resize_handle = -1;
      if (self->sheet != NULL)
        {
          o42_sheet_set_modified (self->sheet, TRUE);
          o42_sheet_end_group (self->sheet);
        }
      sheet_changed (self);
    }
}

static void
on_motion (GtkEventControllerMotion *controller,
           double x, double y, gpointer data)
{
  O42Grid *self = data;
  int row, col;

  (void) controller;

  self->pointer_x = x;
  self->pointer_y = y;
  if (self->sheet == NULL)
    return;

  x /= self->zoom;
  y /= self->zoom;
  unpin_point (self, &x, &y);

  if (self->point_dragging)
    {
      point_at (self, row_at_y (self, y), col_at_x (self, x), TRUE);
      return;
    }

  if (self->thumb_shape != 0)
    {
      thumb_drag_to (self, x, y);
      return;
    }

  if (self->split_drag != 0)
    {
      /* The bar follows the pointer, and the pane above or left of it
       * grows or shrinks by whole rows and columns. */
      if (self->split_drag == 1)
        self->frozen_rows = CLAMP (row_at_y (self, y), 0, O42_MAX_ROWS - 1);
      else
        self->frozen_cols = CLAMP (col_at_x (self, x), 0, O42_MAX_COLS - 1);
      if (self->frozen_rows == 0 && self->frozen_cols == 0)
        self->split = FALSE;   /* dragged back to the corner: no division left */
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

  if (self->resize_handle >= 0)
    {
      double dx = x - self->drag_mouse_x, dy = y - self->drag_mouse_y;
      double nx = self->resize_x0, ny = self->resize_y0;
      double nw = self->resize_w0, nh = self->resize_h0;
      double hx = HANDLE_X[self->resize_handle], hy = HANDLE_Y[self->resize_handle];

      /* A left-side handle moves the left edge; a right-side one the
       * right edge; the middles leave the other axis alone. */
      if (hx == 0)      { nx += dx; nw -= dx; }
      else if (hx == 1) { nw += dx; }
      if (hy == 0)      { ny += dy; nh -= dy; }
      else if (hy == 1) { nh += dy; }

      if (nw < 16) { if (hx == 0) nx = self->resize_x0 + self->resize_w0 - 16; nw = 16; }
      if (nh < 16) { if (hy == 0) ny = self->resize_y0 + self->resize_h0 - 16; nh = 16; }

      selected_object_set_rect (self, nx, ny, nw, nh);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

  if (self->move_drag)
    {
      /* The block keeps the grip the pointer took it by. */
      int dr = row_at_y (self, y) - row_at_y (self, self->drag_mouse_y);
      int dc = col_at_x (self, x) - col_at_x (self, self->drag_mouse_x);
      int rows = self->move_source.row1 - self->move_source.row0;
      int cols = self->move_source.col1 - self->move_source.col0;
      int want_row = CLAMP (self->move_source.row0 + dr, 0, O42_MAX_ROWS - 1 - rows);
      int want_col = CLAMP (self->move_source.col0 + dc, 0, O42_MAX_COLS - 1 - cols);

      if (want_row != self->move_row || want_col != self->move_col)
        {
          self->move_row = want_row;
          self->move_col = want_col;
          gtk_widget_queue_draw (GTK_WIDGET (self));
        }
      return;
    }

  if (self->fill_drag)
    {
      int r = CLAMP (row_at_y (self, y), 0, O42_MAX_ROWS - 1);
      int c = CLAMP (col_at_x (self, x), 0, O42_MAX_COLS - 1);
      const O42Range *src = &self->fill_source;
      O42Range target = *src;
      int below = r - src->row1, above = src->row0 - r;
      int right = c - src->col1, left = src->col0 - c;
      int best = MAX (MAX (below, above), MAX (right, left));

      /* The target grows in whichever direction the pointer is furthest
       * from the source, never in two at once. */
      if (best > 0)
        {
          if (best == below)      target.row1 = r;
          else if (best == above) target.row0 = r;
          else if (best == right) target.col1 = c;
          else                    target.col0 = c;
        }

      if (memcmp (&target, &self->fill_target, sizeof target) != 0)
        {
          self->fill_target = target;
          gtk_widget_queue_draw (GTK_WIDGET (self));
        }
      return;
    }

  if (self->resize_col >= 0)
    {
      int width = self->resize_size + (int) (x - self->resize_start);
      o42_sheet_set_col_width (self->sheet, self->resize_col, MAX (width, 8));
      gtk_widget_queue_resize (GTK_WIDGET (self));
      return;
    }

  if (self->resize_row >= 0)
    {
      int height = self->resize_size + (int) (y - self->resize_start);
      o42_sheet_set_row_height (self->sheet, self->resize_row, MAX (height, 6));
      gtk_widget_queue_resize (GTK_WIDGET (self));
      return;
    }

  /* The pointer shape says what a press would do. */
  if (!self->dragging && !self->picture_drag)
    {
      const char *cursor = "cell";

      int handle = handle_at (self, x, y);
      int break_at = 0;
      static const char *const HANDLE_CURSORS[8] = {
        "nw-resize", "n-resize", "ne-resize", "w-resize", "e-resize",
        "sw-resize", "s-resize", "se-resize"
      };

      if (handle >= 0)
        cursor = HANDLE_CURSORS[handle];
      else if (x >= HEADER_W && y >= HEADER_H && on_selection_edge (self, x, y))
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self),
                                         (gtk_event_controller_get_current_event_state (
                                            GTK_EVENT_CONTROLLER (controller)) & GDK_CONTROL_MASK)
                                         ? "copy" : "move");
      else if (x >= HEADER_W && y >= HEADER_H && on_fill_handle (self, x, y))
        cursor = "crosshair";
      else if (split_bar_at (self, x, y) == 1)     cursor = "row-resize";
      else if (split_bar_at (self, x, y) == 2)     cursor = "col-resize";
      else if (page_break_at (self, x, y, &break_at) == 1) cursor = "row-resize";
      else if (page_break_at (self, x, y, &break_at) == 2) cursor = "col-resize";
      else if (col_boundary_at (self, x, y) >= 0) cursor = "col-resize";
      else if (row_boundary_at (self, x, y) >= 0) cursor = "row-resize";
      else if (y < HEADER_H || x < HEADER_W)      cursor = "default";

      gtk_widget_set_cursor_from_name (GTK_WIDGET (self), cursor);
    }

  if (self->picture_drag && self->sheet != NULL)
    {
      double nx = self->drag_pic_x + (x - self->drag_mouse_x);
      double ny = self->drag_pic_y + (y - self->drag_mouse_y);
      int was_row = 0, was_col = 0;
      double was_dx = 0, was_dy = 0;
      O42Chart *chart = self->selected_is_chart
                        ? o42_sheet_find_chart (self->sheet, self->selected_picture) : NULL;
      O42Picture *pic = self->selected_is_chart
                        ? NULL : o42_sheet_find_picture (self->sheet, self->selected_picture);

      if (chart != NULL)
        { was_row = chart->row; was_col = chart->col; was_dx = chart->dx; was_dy = chart->dy; }
      else if (pic != NULL)
        { was_row = pic->row; was_col = pic->col; was_dx = pic->dx; was_dy = pic->dy; }

      if (chart != NULL)
        anchor_place (self, &chart->row, &chart->col, &chart->dx, &chart->dy, nx, ny);
      else if (pic != NULL)
        picture_place (self, pic, nx, ny);

      /* Whatever is grouped with it comes along by the same step. */
      if (chart != NULL)
        move_group_with (self, chart->id, chart->row - was_row, chart->col - was_col,
                         chart->dx - was_dx, chart->dy - was_dy);
      else if (pic != NULL)
        move_group_with (self, pic->id, pic->row - was_row, pic->col - was_col,
                         pic->dx - was_dx, pic->dy - was_dy);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

  if (!self->dragging || self->sheet == NULL)
    return;

  row = row_at_y (self, y);
  col = col_at_x (self, x);

  if (row < 0 || col < 0)
    return;

  if (row != self->active_row || col != self->active_col)
    {
      self->active_row = row;
      self->active_col = col;
      selection_changed (self);
    }
}

static gboolean
on_blink (gpointer data)
{
  O42Grid *self = data;

  /* Only the marching-ants copy border would need this; the active cell's
   * heavy border does not blink.  Kept as a timer so that adding the ants
   * later needs no plumbing. */
  self->blink_on = !self->blink_on;
  return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------------- */
/* Painting                                                                */
/* ---------------------------------------------------------------------- */

static void
set_rgb (cairo_t *cr, guint32 colour)
{
  cairo_set_source_rgb (cr,
                        ((colour >> 16) & 0xff) / 255.0,
                        ((colour >> 8) & 0xff) / 255.0,
                        (colour & 0xff) / 255.0);
}

/* How far a label may run past its cell: across the empty cells to its
 * right (or left, for right-aligned text), stopping at the first that
 * holds anything.  A number never spills; a number that does not fit is
 * a number that must not be misread, and it is clipped instead. */
static double
spill_extent (O42Grid *self, int row, int col, gboolean rightward)
{
  double extra = 0;

  if (rightward)
    {
      for (int c = col + 1; c < O42_MAX_COLS && extra < 4000; c++)
        {
          if (!o42_sheet_is_empty (self->sheet, row, c))
            break;
          extra += o42_sheet_col_width (self->sheet, c);
        }
    }
  else
    {
      for (int c = col - 1; c >= 0 && extra < 4000; c--)
        {
          if (!o42_sheet_is_empty (self->sheet, row, c))
            break;
          extra += o42_sheet_col_width (self->sheet, c);
        }
    }

  return extra;
}

/* The height wrapped text needs in a cell of this width, or 0 when the
 * cell does not wrap or has nothing in it. */
static int
wrapped_height (O42Grid *self, int row, int col)
{
  const O42Fmt *fmt = o42_sheet_get_fmt (self->sheet, row, col);
  O42Value value;
  char *text;
  PangoFontDescription *desc;
  int width = o42_sheet_col_width (self->sheet, col);
  int tw, th;

  if (fmt == NULL || !fmt->wrap)
    return 0;
  o42_sheet_get_value (self->sheet, row, col, &value);
  if (value.type != O42_VALUE_TEXT)
    { o42_value_clear (&value); return 0; }
  text = o42_fmt_display (fmt, &value);
  o42_value_clear (&value);

  desc = pango_font_description_new ();
  pango_font_description_set_family (desc, fmt->family ? fmt->family : "Sans");
  pango_font_description_set_size (desc, (fmt->size / 2) * PANGO_SCALE);
  pango_font_description_set_weight (desc, fmt->bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_style (desc, fmt->italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
  pango_layout_set_font_description (self->layout, desc);
  pango_font_description_free (desc);

  pango_layout_set_attributes (self->layout, NULL);
  pango_layout_set_text (self->layout, text, -1);
  pango_layout_set_width (self->layout, (int) MAX (width - 2 * CELL_PAD, 1) * PANGO_SCALE);
  pango_layout_set_wrap (self->layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_get_pixel_size (self->layout, &tw, &th);
  pango_layout_set_width (self->layout, -1);
  g_free (text);
  return th + 2;
}

/* One stored cell, on the way past: the tallest wrap in each row is
 * remembered and the rows are grown afterwards.  Walking the cells that
 * exist rather than the used rectangle is what keeps this from taking a
 * million steps on a sheet with one deep cell in it. */
static void
note_wrapped_cell (O42Sheet *sheet, int row, int col, gpointer user)
{
  O42Grid *self = user;
  int wanted;

  if (o42_sheet_row_height_set (sheet, row))
    return;   /* the file, or a person, said how tall it is */
  wanted = wrapped_height (self, row, col);
  if (wanted > 0)
    {
      gpointer had = g_hash_table_lookup (self->wrap_fit, GINT_TO_POINTER (row));

      if (wanted > GPOINTER_TO_INT (had))
        g_hash_table_insert (self->wrap_fit, GINT_TO_POINTER (row),
                             GINT_TO_POINTER (wanted));
    }
}

void
o42_grid_fit_wrapped_rows (O42Grid *self)
{
  GHashTableIter iter;
  gpointer key, value;

  g_return_if_fail (O42_IS_GRID (self));
  if (self->sheet == NULL)
    return;

  self->wrap_fit = g_hash_table_new (g_direct_hash, g_direct_equal);
  o42_sheet_foreach_cell (self->sheet, note_wrapped_cell, self);
  g_hash_table_iter_init (&iter, self->wrap_fit);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      int row = GPOINTER_TO_INT (key);
      int wanted = GPOINTER_TO_INT (value);

      if (wanted > o42_sheet_row_height (self->sheet, row))
        o42_sheet_set_row_height (self->sheet, row, wanted);
    }
  g_clear_pointer (&self->wrap_fit, g_hash_table_destroy);
}

static void
draw_cell_text (O42Grid      *self,
                cairo_t      *cr,
                int           row,
                int           col,
                double        x,
                double        y,
                double        w,
                double        h)
{
  gboolean is_text;
  guint32 colour;
  const O42Fmt *fmt = o42_sheet_get_fmt (self->sheet, row, col);
  O42Fmt conditional;
  O42Value value;
  char *text;
  PangoFontDescription *desc;
  int tw, th;
  double tx, ty;
  O42HAlign halign;

  if (o42_sheet_conditional_fmt (self->sheet, row, col, &conditional))
    fmt = &conditional;

  o42_sheet_get_value (self->sheet, row, col, &value);
  if (value.type == O42_VALUE_EMPTY ||
      (self->hide_zeros && value.type == O42_VALUE_NUMBER && value.as.number == 0))
    {
      o42_value_clear (&value);
      return;
    }

  text = o42_fmt_display (fmt, &value);
  halign = o42_fmt_effective_halign (fmt, &value);
  is_text = (value.type == O42_VALUE_TEXT);
  colour = fmt->colour;
  o42_fmt_display_colour (fmt, &value, &colour);
  o42_value_clear (&value);
  if (o42_sheet_get_link (self->sheet, row, col) != NULL)
    colour = 0x0000EE;   /* a hyperlink: blue, and underlined below */

  desc = pango_font_description_new ();
  pango_font_description_set_family (desc, fmt->family ? fmt->family : "Sans");
  pango_font_description_set_size (desc, (fmt->size / 2) * PANGO_SCALE);
  pango_font_description_set_weight (desc, fmt->bold ? PANGO_WEIGHT_BOLD
                                                     : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_style (desc, fmt->italic ? PANGO_STYLE_ITALIC
                                                      : PANGO_STYLE_NORMAL);
  pango_layout_set_font_description (self->layout, desc);
  pango_font_description_free (desc);

  pango_layout_set_text (self->layout, text, -1);
  /* Wrapped text is laid out to the width of its cell and stays inside
   * it; everything else is measured on one line and may run on. */
  if (fmt->wrap)
    {
      pango_layout_set_width (self->layout,
                              (int) MAX (w - 2 * CELL_PAD, 1) * PANGO_SCALE);
      pango_layout_set_wrap (self->layout, PANGO_WRAP_WORD_CHAR);
      pango_layout_set_alignment (self->layout,
                                  halign == O42_HALIGN_RIGHT ? PANGO_ALIGN_RIGHT :
                                  halign == O42_HALIGN_CENTRE ? PANGO_ALIGN_CENTER
                                                              : PANGO_ALIGN_LEFT);
    }
  else
    {
      pango_layout_set_width (self->layout, -1);
      pango_layout_set_alignment (self->layout, PANGO_ALIGN_LEFT);
    }
  /* One attribute list for the cell: the runs of text set in their own
   * fonts, and the underline or strikeout the whole cell wears. */
  {
    int n_runs = 0;
    const O42TextRun *runs = is_text ? o42_sheet_runs (self->sheet, row, col, &n_runs) : NULL;
    PangoAttrList *attrs = o42_runs_attributes (runs, n_runs, fmt, text);
    gboolean linked = o42_sheet_get_link (self->sheet, row, col) != NULL;

    if (fmt->underline || fmt->strikeout || linked)
      {
        if (attrs == NULL)
          attrs = pango_attr_list_new ();
        if (fmt->underline || linked)
          pango_attr_list_insert (attrs, pango_attr_underline_new (PANGO_UNDERLINE_SINGLE));
        if (fmt->strikeout)
          pango_attr_list_insert (attrs, pango_attr_strikethrough_new (TRUE));
      }
    pango_layout_set_attributes (self->layout, attrs);
    if (attrs != NULL)
      pango_attr_list_unref (attrs);
  }
  pango_layout_get_pixel_size (self->layout, &tw, &th);

  /* A number that does not fit must not be shown clipped: 1234 with its
   * first digit cut off reads as 234.  A General-format number is shown
   * with fewer digits while that helps; anything else becomes a row of
   * hashes, which is what Excel has always drawn there. */
  if (!is_text && tw + 2 * CELL_PAD > w)
    {
      O42Value v;
      gboolean fits = FALSE;

      o42_sheet_get_value (self->sheet, row, col, &v);
      if (v.type == O42_VALUE_NUMBER && fmt->number == O42_NUM_GENERAL)
        {
          for (int digits = 9; digits >= 1 && !fits; digits--)
            {
              char spec[8], buffer[G_ASCII_DTOSTR_BUF_SIZE];

              g_snprintf (spec, sizeof spec, "%%.%dg", digits);
              g_ascii_formatd (buffer, sizeof buffer, spec, v.as.number);
              pango_layout_set_text (self->layout, buffer, -1);
              pango_layout_get_pixel_size (self->layout, &tw, &th);
              if (tw + 2 * CELL_PAD <= w)
                {
                  g_free (text);
                  text = g_strdup (buffer);
                  fits = TRUE;
                }
            }
        }
      o42_value_clear (&v);

      if (!fits)
        {
          GString *hashes = g_string_new ("#");

          pango_layout_set_text (self->layout, "#", -1);
          pango_layout_get_pixel_size (self->layout, &tw, &th);
          while (tw > 0 && (hashes->len + 1) * tw + 2 * CELL_PAD <= w)
            g_string_append_c (hashes, '#');

          g_free (text);
          text = g_string_free (hashes, FALSE);
          pango_layout_set_text (self->layout, text, -1);
          pango_layout_get_pixel_size (self->layout, &tw, &th);
        }
    }

  if (fmt->wrap)
    tx = x + CELL_PAD;
  else
    switch (halign)
      {
      case O42_HALIGN_RIGHT:  tx = x + w - CELL_PAD - tw - fmt->indent * INDENT_PX; break;
      case O42_HALIGN_CENTRE: tx = x + (w - tw) / 2.0;    break;
      default:                tx = x + CELL_PAD + fmt->indent * INDENT_PX; break;
      }

  switch (fmt->valign)
    {
    case O42_VALIGN_TOP:    ty = y + 1;              break;
    case O42_VALIGN_MIDDLE: ty = y + (h - th) / 2.0; break;
    default:                ty = y + h - th - 1;     break;
    }

  /* A label wider than its cell runs on over empty neighbours, as Excel
   * allows; anything else is clipped to its cell, which is what keeps a
   * long number from painting over the next column's value. */
  {
    double clip_x = x, clip_w = w;

    if (is_text && !fmt->wrap && tw + 2 * CELL_PAD > w)
      {
        if (halign == O42_HALIGN_RIGHT)
          {
            double extra = spill_extent (self, row, col, FALSE);
            clip_x -= extra;
            clip_w += extra;
          }
        else if (halign == O42_HALIGN_CENTRE)
          {
            double left = spill_extent (self, row, col, FALSE);
            double right = spill_extent (self, row, col, TRUE);
            clip_x -= left;
            clip_w += left + right;
          }
        else
          clip_w += spill_extent (self, row, col, TRUE);
      }

    cairo_save (cr);
    cairo_rectangle (cr, clip_x, y, clip_w, h);
    cairo_clip (cr);
  }

  set_rgb (cr, colour);
  if (fmt->rotation != 0)
    {
      /* Turned about the cell's centre. */
      cairo_translate (cr, x + w / 2.0, y + h / 2.0);
      cairo_rotate (cr, -fmt->rotation * G_PI / 180.0);
      cairo_move_to (cr, -tw / 2.0, -th / 2.0);
    }
  else
    cairo_move_to (cr, tx, ty);
  pango_cairo_show_layout (cr, self->layout);
  cairo_restore (cr);
  pango_layout_set_attributes (self->layout, NULL);

  g_free (text);
}

/* The cells of a rectangle of rows and columns: fills first, then the
 * selection wash, then gridlines, text and borders.  Called once for the
 * scrolled area and once more for each frozen band, translated. */
/* Paints one merged range as a single cell: its fill, the top-left cell's
 * text over the whole, and its border. */
static void
paint_merge (O42Grid *self, cairo_t *cr, const O42Range *sel, const O42Range *m)
{
  double x = col_x (self, m->col0), y = row_y (self, m->row0);
  double w = col_x (self, m->col1) + o42_sheet_col_width (self->sheet, m->col1) - x;
  double h = row_y (self, m->row1) + o42_sheet_row_height (self->sheet, m->row1) - y;
  const O42Fmt *fmt = o42_sheet_get_fmt (self->sheet, m->row0, m->col0);

  if (w <= 0 || h <= 0)
    return;

  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_rectangle (cr, x + 1, y + 1, w - 1, h - 1);
  cairo_fill (cr);

  o42_pattern_fill (fmt, cr, x, y, w, h);

  if (o42_range_contains (sel, m->row0, m->col0) &&
      !(m->row0 == self->active_row && m->col0 == self->active_col))
    {
      cairo_set_source_rgba (cr, 0.0, 0.0, 0.5, 0.15);
      cairo_rectangle (cr, x, y, w, h);
      cairo_fill (cr);
    }

  if (!(self->editing && m->row0 == self->active_row && m->col0 == self->active_col))
    draw_cell_text (self, cr, m->row0, m->col0, x, y, w, h);
}

static void
paint_cells (O42Grid *self, cairo_t *cr, const O42Range *sel,
             int first_row, int last_row, int first_col, int last_col)
{
  double x, y;

  /* Cells: fills first, then the selection wash, then text, then lines.
   * That order is what keeps a fill from hiding the gridlines around it. */
  y = row_y (self, first_row);
  for (int row = first_row; row <= last_row; row++)
    {
      double h = o42_sheet_row_height (self->sheet, row);

      x = col_x (self, first_col);
      for (int col = first_col; col <= last_col; col++)
        {
          double w = o42_sheet_col_width (self->sheet, col);
          const O42Fmt *fmt = o42_sheet_get_fmt (self->sheet, row, col);
          O42Fmt conditional;

          if (o42_sheet_conditional_fmt (self->sheet, row, col, &conditional))
            fmt = &conditional;

          o42_pattern_fill (fmt, cr, x, y, w, h);

          if (o42_range_contains (sel, row, col) &&
              !(row == self->active_row && col == self->active_col))
            {
              cairo_set_source_rgba (cr, 0.0, 0.0, 0.5, 0.15);
              cairo_rectangle (cr, x, y, w, h);
              cairo_fill (cr);
            }

          x += w;
        }
      y += h;
    }

  /* Gridlines, on half-pixels so they are one pixel wide and grey. */
  cairo_set_source_rgb (cr, 0.80, 0.80, 0.80);
  cairo_set_line_width (cr, 1.0);
  if (self->hide_gridlines)
    cairo_set_source_rgba (cr, 0, 0, 0, 0);

  x = col_x (self, first_col);
  for (int col = first_col; col <= last_col + 1 && col <= O42_MAX_COLS; col++)
    {
      cairo_move_to (cr, floor (x) + 0.5, row_y (self, first_row));
      cairo_line_to (cr, floor (x) + 0.5, row_y (self, last_row + 1));
      if (col < O42_MAX_COLS)
        x += o42_sheet_col_width (self->sheet, col);
    }

  y = row_y (self, first_row);
  for (int row = first_row; row <= last_row + 1 && row <= O42_MAX_ROWS; row++)
    {
      cairo_move_to (cr, col_x (self, first_col), floor (y) + 0.5);
      cairo_line_to (cr, col_x (self, last_col + 1), floor (y) + 0.5);
      if (row < O42_MAX_ROWS)
        y += o42_sheet_row_height (self->sheet, row);
    }
  cairo_stroke (cr);

  /* Text. */
  y = row_y (self, first_row);
  for (int row = first_row; row <= last_row; row++)
    {
      double h = o42_sheet_row_height (self->sheet, row);

      x = col_x (self, first_col);
      for (int col = first_col; col <= last_col; col++)
        {
          double w = o42_sheet_col_width (self->sheet, col);

          if (!(self->editing && row == self->active_row &&
                col == self->active_col) &&
              !o42_sheet_merged_at (self->sheet, row, col, NULL))
            draw_cell_text (self, cr, row, col, x, y, w, h);

          x += w;
        }
      y += h;
    }

  /* Borders the format asks for, each side in its style and colour. */
  y = row_y (self, first_row);
  for (int row = first_row; row <= last_row; row++)
    {
      double h = o42_sheet_row_height (self->sheet, row);

      x = col_x (self, first_col);
      for (int col = first_col; col <= last_col; col++)
        {
          double w = o42_sheet_col_width (self->sheet, col);
          const O42Fmt *fmt = o42_sheet_get_fmt (self->sheet, row, col);

          if (fmt->border_top)
            o42_draw_border_line (cr, fmt->border_style[O42_SIDE_TOP], fmt->border_colour[O42_SIDE_TOP],
                                  x, floor (y) + 0.5, x + w, floor (y) + 0.5);
          if (fmt->border_bottom)
            o42_draw_border_line (cr, fmt->border_style[O42_SIDE_BOTTOM], fmt->border_colour[O42_SIDE_BOTTOM],
                                  x, floor (y + h) + 0.5, x + w, floor (y + h) + 0.5);
          if (fmt->border_left)
            o42_draw_border_line (cr, fmt->border_style[O42_SIDE_LEFT], fmt->border_colour[O42_SIDE_LEFT],
                                  floor (x) + 0.5, y, floor (x) + 0.5, y + h);
          if (fmt->border_right)
            o42_draw_border_line (cr, fmt->border_style[O42_SIDE_RIGHT], fmt->border_colour[O42_SIDE_RIGHT],
                                  floor (x + w) + 0.5, y, floor (x + w) + 0.5, y + h);

          x += w;
        }
      y += h;
    }


  /* Merged ranges last, painted as one cell over the gridlines. */
  {
    GArray *merges = o42_sheet_merges (self->sheet);

    for (guint i = 0; i < merges->len; i++)
      {
        const O42Range *m = &g_array_index (merges, O42Range, i);

        if (m->row1 < first_row || m->row0 > last_row ||
            m->col1 < first_col || m->col0 > last_col)
          continue;
        paint_merge (self, cr, sel, m);
      }
  }
}

/* The selection's outline and the active cell's heavy border, with the
 * fill handle. */
/* One of the rectangles behind the current selection: the same wash and
 * outline, and no active cell. */
static void
paint_extra_selection (O42Grid *self, cairo_t *cr, const O42Range *sel)
{
  double sx0 = col_x (self, sel->col0);
  double sy0 = row_y (self, sel->row0);
  double sx1 = col_x (self, sel->col1) + o42_sheet_col_width (self->sheet, sel->col1);
  double sy1 = row_y (self, sel->row1) + o42_sheet_row_height (self->sheet, sel->row1);

  cairo_save (cr);
  cairo_set_source_rgba (cr, 0.16, 0.30, 0.50, 0.16);
  cairo_rectangle (cr, sx0, sy0, sx1 - sx0, sy1 - sy0);
  cairo_fill (cr);
  cairo_set_source_rgb (cr, 0.16, 0.30, 0.50);
  cairo_set_line_width (cr, 1);
  cairo_rectangle (cr, floor (sx0) + 0.5, floor (sy0) + 0.5, sx1 - sx0, sy1 - sy0);
  cairo_stroke (cr);
  cairo_restore (cr);
}

static void
paint_selection (O42Grid *self, cairo_t *cr, const O42Range *sel)
{
  /* The selection's outline and the active cell's heavy border. */
  {
    double sx0 = col_x (self, sel->col0);
    double sy0 = row_y (self, sel->row0);
    double sx1 = col_x (self, sel->col1) + o42_sheet_col_width (self->sheet, sel->col1);
    double sy1 = row_y (self, sel->row1) + o42_sheet_row_height (self->sheet, sel->row1);
    double ax = col_x (self, self->active_col);
    double ay = row_y (self, self->active_row);
    double aw = o42_sheet_col_width (self->sheet, self->active_col);
    double ah = o42_sheet_row_height (self->sheet, self->active_row);
    O42Range merged;

    if (o42_sheet_merged_at (self->sheet, self->active_row, self->active_col, &merged))
      {
        ax = col_x (self, merged.col0);
        ay = row_y (self, merged.row0);
        aw = col_x (self, merged.col1) + o42_sheet_col_width (self->sheet, merged.col1) - ax;
        ah = row_y (self, merged.row1) + o42_sheet_row_height (self->sheet, merged.row1) - ay;
        /* A selection that is just the merged cell reaches to its corner. */
        sx1 = MAX (sx1, ax + aw);
        sy1 = MAX (sy1, ay + ah);
      }

    cairo_set_source_rgb (cr, 0, 0, 0);

    if (sel->row0 != sel->row1 || sel->col0 != sel->col1)
      {
        cairo_set_line_width (cr, 1.0);
        cairo_rectangle (cr, floor (sx0) + 0.5, floor (sy0) + 0.5,
                         floor (sx1 - sx0), floor (sy1 - sy0));
        cairo_stroke (cr);
      }

    cairo_set_line_width (cr, 2.0);
    cairo_rectangle (cr, floor (ax) + 1, floor (ay) + 1, floor (aw) - 1, floor (ah) - 1);
    cairo_stroke (cr);

    /* The fill handle at the bottom right of the selection. */
    cairo_rectangle (cr, floor (sx1) - 3, floor (sy1) - 3, 5, 5);
    cairo_fill (cr);
  }

}

/* A run of rows (columns) at or above `level` beginning at `start`:
 * its last index, or -1 if `start` is not in one. */
static int
outline_run_end (O42Sheet *sheet, gboolean rows, int start, int level)
{
  int limit = rows ? O42_MAX_ROWS : O42_MAX_COLS;
  int end = start;

  if ((rows ? o42_sheet_row_level (sheet, start) : o42_sheet_col_level (sheet, start)) < level)
    return -1;
  while (end + 1 < limit &&
         (rows ? o42_sheet_row_level (sheet, end + 1) : o42_sheet_col_level (sheet, end + 1)) >= level)
    end++;
  return end;
}

static gboolean
outline_run_hidden (O42Sheet *sheet, gboolean rows, int start, int end)
{
  for (int i = start; i <= end; i++)
    if (!(rows ? o42_sheet_row_hidden_by_hand (sheet, i) : o42_sheet_col_hidden (sheet, i)))
      return FALSE;
  return TRUE;
}

/* The outline margin: a bar along each run of grouped rows (columns)
 * at every level, and a minus or plus box against the row (column)
 * after the run, where Excel puts its summary. */
static void
paint_outline (O42Grid *self, cairo_t *cr, gboolean rows, double hx, double hy,
               int first, int last, double extent)
{
  int levels = rows ? o42_sheet_max_row_level (self->sheet) : o42_sheet_max_col_level (self->sheet);
  int limit = rows ? O42_MAX_ROWS : O42_MAX_COLS;

  cairo_save (cr);
  if (rows)
    cairo_rectangle (cr, hx, hy + HEADER_H, self->outline_w, extent);
  else
    cairo_rectangle (cr, hx + HEADER_W, hy, extent, self->outline_h);
  cairo_clip (cr);
  cairo_set_line_width (cr, 1.0);

  for (int level = 1; level <= levels; level++)
    {
      double centre = (rows ? hx : hy) + 3 + (level - 1) * OUTLINE_STEP + OUTLINE_STEP / 2.0;
      int i = 0;

      /* Runs are walked from the top so a run that starts before the
       * first visible row still shows its bar. */
      while (i < limit && i <= last + 1)
        {
          int end = outline_run_end (self->sheet, rows, i, level);
          double a, b, box;
          gboolean collapsed;

          if (end < 0) { i++; continue; }
          if (end < first - 1) { i = end + 1; continue; }
          /* The headers are painted at sheet positions offset by the
           * scroll, which hx and hy carry. */
          a = rows ? hy + row_y (self, i) : hx + col_x (self, i);
          b = rows ? hy + row_y (self, end + 1) : hx + col_x (self, end + 1);
          box = b;   /* the summary row's top */
          collapsed = outline_run_hidden (self->sheet, rows, i, end);

          cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
          if (!collapsed && b > a)
            {
              if (rows)
                { cairo_move_to (cr, centre + 0.5, a + 2); cairo_line_to (cr, centre + 0.5, b - 1); }
              else
                { cairo_move_to (cr, a + 2, centre + 0.5); cairo_line_to (cr, b - 1, centre + 0.5); }
              cairo_stroke (cr);
            }
          /* The box, 9 by 9, against the summary row or column. */
          {
            double bx = rows ? centre - 4.5 : box + 2, by = rows ? box + 2 : centre - 4.5;
            double size = 9;
            cairo_set_source_rgb (cr, 1, 1, 1);
            cairo_rectangle (cr, bx, by, size, size);
            cairo_fill (cr);
            cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
            cairo_rectangle (cr, floor (bx) + 0.5, floor (by) + 0.5, size, size);
            cairo_stroke (cr);
            cairo_move_to (cr, bx + 2, floor (by + size / 2) + 0.5);
            cairo_line_to (cr, bx + size - 2, floor (by + size / 2) + 0.5);
            if (collapsed)
              {
                cairo_move_to (cr, floor (bx + size / 2) + 0.5, by + 2);
                cairo_line_to (cr, floor (bx + size / 2) + 0.5, by + size - 2);
              }
            cairo_stroke (cr);
          }
          i = end + 1;
        }
    }
  cairo_restore (cr);
}

/* A click in the outline margin: which run's box, if any, and toggle it. */
static gboolean
outline_click (O42Grid *self, double x, double y)
{
  gboolean rows;
  int levels, level;
  double along, across;

  if (self->sheet == NULL)
    return FALSE;
  if (self->outline_w > 0 && x < self->outline_w && y >= HEADER_H)
    { rows = TRUE; along = y; across = x; }
  else if (self->outline_h > 0 && y < self->outline_h && x >= HEADER_W)
    { rows = FALSE; along = x; across = y; }
  else
    return FALSE;

  levels = rows ? o42_sheet_max_row_level (self->sheet) : o42_sheet_max_col_level (self->sheet);
  level = (int) ((across - 3) / OUTLINE_STEP) + 1;
  if (level < 1 || level > levels)
    return TRUE;

  /* The box sits at the top of the row after a run: find the run whose
   * summary row the click is in. */
  {
    int at = rows ? row_at_y (self, along) : col_at_x (self, along);
    int end = at - 1, start;
    if (end < 0 || (rows ? o42_sheet_row_level (self->sheet, end) : o42_sheet_col_level (self->sheet, end)) < level)
      return TRUE;
    start = end;
    while (start > 0 && (rows ? o42_sheet_row_level (self->sheet, start - 1) : o42_sheet_col_level (self->sheet, start - 1)) >= level)
      start--;
    {
      gboolean hide = !outline_run_hidden (self->sheet, rows, start, end);
      o42_sheet_begin_group (self->sheet);
      for (int i = start; i <= end; i++)
        {
          if (rows) o42_sheet_set_row_hidden (self->sheet, i, hide);
          else o42_sheet_set_col_hidden (self->sheet, i, hide);
        }
      o42_sheet_end_group (self->sheet);
    }
  }
  gtk_widget_queue_resize (GTK_WIDGET (self));
  sheet_changed (self);
  return TRUE;
}

static void
paint_col_headers (O42Grid *self, cairo_t *cr, const O42Range *sel, double hy,
                   int first, int last, double x_start)
{
  double x;

    x = x_start;
    for (int col = first; col <= last; col++)
      {
        double w = o42_sheet_col_width (self->sheet, col);
        char name[8];
        int tw, th;
        gboolean lit = (col >= sel->col0 && col <= sel->col1);

        if (w <= 0)
          continue;     /* hidden */

        if (lit)
          {
            cairo_set_source_rgb (cr, 0.60, 0.60, 0.60);
            cairo_rectangle (cr, x, hy, w, HEADER_H);
            cairo_fill (cr);
          }

        o42_col_name (col, name, sizeof name);
        pango_layout_set_text (self->layout, name, -1);
        pango_layout_get_pixel_size (self->layout, &tw, &th);

        cairo_set_source_rgb (cr, 0, 0, 0);
        cairo_move_to (cr, x + (w - tw) / 2.0, hy + (HEADER_H - th) / 2.0);
        pango_cairo_show_layout (cr, self->layout);

        cairo_set_source_rgb (cr, 0.50, 0.50, 0.50);
        cairo_move_to (cr, floor (x + w) + 0.5, hy);
        cairo_line_to (cr, floor (x + w) + 0.5, hy + HEADER_H);
        cairo_stroke (cr);

        x += w;
      }
}

static void
paint_row_headers (O42Grid *self, cairo_t *cr, const O42Range *sel, double hx,
                   int first, int last, double y_start)
{
  double y;

    y = y_start;
    for (int row = first; row <= last; row++)
      {
        double h = o42_sheet_row_height (self->sheet, row);
        char name[16];
        int tw, th;
        gboolean lit = (row >= sel->row0 && row <= sel->row1);

        if (h <= 0)
          continue;     /* hidden */

        if (lit)
          {
            cairo_set_source_rgb (cr, 0.60, 0.60, 0.60);
            cairo_rectangle (cr, hx, y, HEADER_W, h);
            cairo_fill (cr);
          }

        g_snprintf (name, sizeof name, "%d", row + 1);
        pango_layout_set_text (self->layout, name, -1);
        pango_layout_get_pixel_size (self->layout, &tw, &th);

        cairo_set_source_rgb (cr, 0, 0, 0);
        cairo_move_to (cr, hx + (HEADER_W - tw) / 2.0, y + (h - th) / 2.0);
        pango_cairo_show_layout (cr, self->layout);

        cairo_set_source_rgb (cr, 0.50, 0.50, 0.50);
        cairo_move_to (cr, hx, floor (y + h) + 0.5);
        cairo_line_to (cr, hx + HEADER_W, floor (y + h) + 0.5);
        cairo_stroke (cr);

        y += h;
      }
}

static void
o42_grid_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  O42Grid *self = O42_GRID (widget);
  int width = gtk_widget_get_width (widget);
  int height = gtk_widget_get_height (widget);
  cairo_t *cr;
  double scroll_x = 0, scroll_y = 0, view_w = width, view_h = height;
  O42Range sel;
  int first_row, last_row, first_col, last_col;

  if (self->sheet == NULL || width <= 0 || height <= 0)
    return;

  /* Only what is on screen is painted, and it is painted where the
   * scroll says -- the widget itself is the size of the window. */
  if (self->hadj != NULL && self->vadj != NULL)
    {
      scroll_x = gtk_adjustment_get_value (self->hadj);
      scroll_y = gtk_adjustment_get_value (self->vadj);
      view_w = width;
      view_h = height;
    }

  /* The cairo node covers the widget, which is the window's size; the
   * drawing below is in sheet pixels, so the origin is moved to the top
   * left of what is on screen.  Everything cairo sees is then within a
   * screenful of zero, which is what keeps a sheet a million rows deep
   * drawable at all. */
  cr = gtk_snapshot_append_cairo (snapshot,
                                  &GRAPHENE_RECT_INIT (0, 0, width, height));

  /* From here on everything is in sheet pixels; the zoom is one scale. */
  cairo_scale (cr, self->zoom, self->zoom);
  scroll_x /= self->zoom;
  scroll_y /= self->zoom;
  cairo_translate (cr, -scroll_x, -scroll_y);
  view_w /= self->zoom;
  view_h /= self->zoom;
  pango_cairo_update_context (cr, pango_layout_get_context (self->layout));

  selection_range (self, &sel);

  {
    double frozen_w = col_x (self, self->frozen_cols) - HEADER_W;
    double frozen_h = row_y (self, self->frozen_rows) - HEADER_H;

    first_col = col_at_x (self, scroll_x + HEADER_W + frozen_w);
    last_col  = MIN (O42_MAX_COLS - 1, col_at_x (self, scroll_x + view_w));
    first_row = row_at_y (self, scroll_y + HEADER_H + frozen_h);
    last_row  = MIN (O42_MAX_ROWS - 1, row_at_y (self, scroll_y + view_h));
    /* Frozen panes hold the first rows and columns still; split ones
     * scroll, so the pane below one may show any row at all. */
    if (!self->split)
      {
        first_col = MAX (self->frozen_cols, first_col);
        first_row = MAX (self->frozen_rows, first_row);
      }
  }

  /* Paper. */
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_rectangle (cr, scroll_x, scroll_y, view_w, view_h);
  cairo_fill (cr);

  if (o42_sheet_is_chart_sheet (self->sheet))
    {
      /* One chart, filling the window with a margin around it. */
      O42Chart *chart = o42_sheet_the_chart (self->sheet);
      double margin = 12;

      if (chart != NULL && view_w > 4 * margin && view_h > 4 * margin)
        {
          cairo_save (cr);
          cairo_translate (cr, scroll_x + margin, scroll_y + margin);
          o42_sheet_draw_chart (self->sheet, chart, cr,
                                view_w - 2 * margin, view_h - 2 * margin);
          cairo_restore (cr);
        }
      cairo_destroy (cr);
      return;
    }

  paint_cells (self, cr, &sel, first_row, last_row, first_col, last_col);

  /* Notes: a small red triangle in the top-right corner of the cell, as
   * Excel 5 marked them. */
  {
    GHashTable *notes = o42_sheet_notes (self->sheet);
    GHashTableIter iter;
    gpointer key_ptr;

    cairo_set_source_rgb (cr, 0.8, 0, 0);
    g_hash_table_iter_init (&iter, notes);
    while (g_hash_table_iter_next (&iter, &key_ptr, NULL))
      {
        guint64 key = *(guint64 *) key_ptr;
        int row = o42_key_row (key), col = o42_key_col (key);
        double nx, ny;

        if (row < first_row || row > last_row || col < first_col || col > last_col)
          continue;
        nx = col_x (self, col) + o42_sheet_col_width (self->sheet, col);
        ny = row_y (self, row);
        cairo_move_to (cr, nx - 6, ny + 1);
        cairo_line_to (cr, nx - 1, ny + 1);
        cairo_line_to (cr, nx - 1, ny + 6);
        cairo_close_path (cr);
        cairo_fill (cr);
      }
  }

  /* AutoFilter buttons: a small square with a triangle at the right of
   * each heading, as Excel 5 drew them. */
  {
    O42Range filter;

    if (o42_sheet_get_autofilter (self->sheet, &filter) &&
        filter.row0 >= first_row && filter.row0 <= last_row)
      {
        double fy = row_y (self, filter.row0) + o42_sheet_row_height (self->sheet, filter.row0);

        for (int col = MAX (filter.col0, first_col); col <= MIN (filter.col1, last_col); col++)
          {
            double fx = col_x (self, col) + o42_sheet_col_width (self->sheet, col);
            double bx = fx - FILTER_BTN, by = fy - FILTER_BTN;
            gboolean chosen = o42_sheet_autofilter_choice (self->sheet, col) != NULL;

            if (o42_sheet_col_width (self->sheet, col) == 0)
              continue;

            cairo_set_source_rgb (cr, 0.753, 0.753, 0.753);
            cairo_rectangle (cr, bx, by, FILTER_BTN - 1, FILTER_BTN - 1);
            cairo_fill (cr);
            cairo_set_source_rgb (cr, 0.5, 0.5, 0.5);
            cairo_set_line_width (cr, 1.0);
            cairo_rectangle (cr, floor (bx) + 0.5, floor (by) + 0.5, FILTER_BTN - 2, FILTER_BTN - 2);
            cairo_stroke (cr);

            if (chosen) cairo_set_source_rgb (cr, 0, 0, 0.6);
            else        cairo_set_source_rgb (cr, 0, 0, 0);
            cairo_move_to (cr, bx + 3, by + 5);
            cairo_line_to (cr, bx + FILTER_BTN - 4, by + 5);
            cairo_line_to (cr, bx + FILTER_BTN / 2.0 - 0.5, by + FILTER_BTN - 4);
            cairo_close_path (cr);
            cairo_fill (cr);
          }
      }
  }

  /* Pictures float above the cells. */
  {
    GPtrArray *pictures = o42_sheet_pictures (self->sheet);

    for (guint i = 0; i < pictures->len; i++)
      {
        O42Picture *pic = g_ptr_array_index (pictures, i);
        cairo_surface_t *surface;
        double px, py, pw, ph;

        picture_rect (self, pic, &px, &py, &pw, &ph);

        if (px > scroll_x + view_w || py > scroll_y + view_h ||
            px + pw < scroll_x || py + ph < scroll_y)
          continue;

        surface = o42_picture_surface (pic);
        if (surface == NULL)
          continue;

        cairo_save (cr);
        cairo_translate (cr, px, py);
        cairo_scale (cr, pw / cairo_image_surface_get_width (surface),
                         ph / cairo_image_surface_get_height (surface));
        cairo_set_source_surface (cr, surface, 0, 0);
        cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
        cairo_paint (cr);
        cairo_restore (cr);

        if (pic->id == self->selected_picture && !self->selected_is_chart)
          {
            /* Eight handles, as Excel drew them; each drags its edge. */
            cairo_set_source_rgb (cr, 0, 0, 0);
            cairo_set_line_width (cr, 1.0);
            cairo_rectangle (cr, floor (px) + 0.5, floor (py) + 0.5,
                             floor (pw), floor (ph));
            cairo_stroke (cr);

            for (int k = 0; k < 8; k++)
              {
                cairo_rectangle (cr, px + HANDLE_X[k] * pw - 3, py + HANDLE_Y[k] * ph - 3,
                                 6, 6);
                cairo_fill (cr);
              }
          }
      }
  }

  /* Shapes: rectangles, ovals, lines, arrows and text boxes. */
  {
    GPtrArray *shapes = o42_sheet_shapes (self->sheet);

    for (guint i = 0; i < shapes->len; i++)
      {
        O42Shape *shape = g_ptr_array_index (shapes, i);
        double sx, sy, shape_w, shape_h;

        shape_rect (self, shape, &sx, &sy, &shape_w, &shape_h);
        if (sx > scroll_x + view_w || sy > scroll_y + view_h ||
            sx + MAX (shape_w, 1) < scroll_x || sy + MAX (shape_h, 1) < scroll_y)
          continue;

        cairo_save (cr);
        cairo_translate (cr, sx, sy);
        o42_sheet_draw_shape (self->sheet, shape, cr, shape_w, shape_h);
        cairo_restore (cr);

        if (shape->id == self->selected_picture && self->selected_is_shape)
          {
            cairo_set_source_rgb (cr, 0, 0, 0);
            cairo_set_line_width (cr, 1.0);
            cairo_rectangle (cr, floor (sx) + 0.5, floor (sy) + 0.5, floor (shape_w), floor (shape_h));
            cairo_stroke (cr);
            for (int k = 0; k < 8; k++)
              {
                cairo_rectangle (cr, sx + HANDLE_X[k] * shape_w - 3, sy + HANDLE_Y[k] * shape_h - 3, 6, 6);
                cairo_fill (cr);
              }
          }
      }
  }

  /* Charts, drawn from the cells as they stand. */
  {
    GPtrArray *charts = o42_sheet_charts (self->sheet);

    for (guint i = 0; i < charts->len; i++)
      {
        O42Chart *chart = g_ptr_array_index (charts, i);
        double cx, cy, cw, ch;

        chart_rect (self, chart, &cx, &cy, &cw, &ch);
        if (cx > scroll_x + view_w || cy > scroll_y + view_h ||
            cx + cw < scroll_x || cy + ch < scroll_y)
          continue;

        cairo_save (cr);
        cairo_translate (cr, cx, cy);
        o42_sheet_draw_chart (self->sheet, chart, cr, cw, ch);
        cairo_restore (cr);

        if (chart->id == self->selected_picture && self->selected_is_chart)
          {
            cairo_set_source_rgb (cr, 0, 0, 0);
            cairo_set_line_width (cr, 1.0);
            cairo_rectangle (cr, floor (cx) + 0.5, floor (cy) + 0.5, floor (cw), floor (ch));
            cairo_stroke (cr);
            for (int k = 0; k < 8; k++)
              {
                cairo_rectangle (cr, cx + HANDLE_X[k] * cw - 3, cy + HANDLE_Y[k] * ch - 3, 6, 6);
                cairo_fill (cr);
              }
          }
      }
  }

  /* The range an autofill drag will fill, as a grey outline. */
  if (self->move_drag)
    {
      /* An outline where the block would land. */
      double mx = col_x (self, self->move_col);
      double my = row_y (self, self->move_row);
      double mw = 0, mh = 0;

      for (int c = self->move_source.col0; c <= self->move_source.col1; c++)
        mw += o42_sheet_col_width (self->sheet, c);
      for (int r = self->move_source.row0; r <= self->move_source.row1; r++)
        mh += o42_sheet_row_height (self->sheet, r);
      cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
      cairo_set_line_width (cr, 2);
      cairo_rectangle (cr, floor (mx) + 1, floor (my) + 1, mw - 2, mh - 2);
      cairo_stroke (cr);
    }

  if (self->fill_drag)
    {
      const O42Range *t = &self->fill_target;
      double tx0 = col_x (self, t->col0), ty0 = row_y (self, t->row0);
      double tx1 = col_x (self, t->col1) + o42_sheet_col_width (self->sheet, t->col1);
      double ty1 = row_y (self, t->row1) + o42_sheet_row_height (self->sheet, t->row1);

      cairo_set_source_rgb (cr, 0.4, 0.4, 0.4);
      cairo_set_line_width (cr, 1.0);
      cairo_rectangle (cr, floor (tx0) + 0.5, floor (ty0) + 0.5,
                       floor (tx1 - tx0), floor (ty1 - ty0));
      cairo_stroke (cr);
    }

  /* The rectangles Ctrl+click put behind this one are washed the same
   * way, without the heavy border, which belongs to the one being
   * made. */
  for (guint i = 0; self->extra_sel != NULL && i < self->extra_sel->len; i++)
    paint_extra_selection (self, cr, &g_array_index (self->extra_sel, O42Range, i));
  paint_selection (self, cr, &sel);

  /* Every reference the formula holds, outlined in the colour it is
   * written in. */
  for (guint i = 0; self->editing && self->refs != NULL && i < self->refs->len; i++)
    {
      const RefSpan *span = &g_array_index (self->refs, RefSpan, i);
      guint32 rgb = REF_COLOURS[span->colour];
      double x0 = col_x (self, span->range.col0), y0 = row_y (self, span->range.row0);
      double x1 = col_x (self, span->range.col1 + 1), y1 = row_y (self, span->range.row1 + 1);

      cairo_save (cr);
      set_rgb (cr, rgb);
      cairo_set_line_width (cr, 2);
      cairo_rectangle (cr, floor (x0) + 1, floor (y0) + 1,
                       floor (x1 - x0) - 2, floor (y1 - y0) - 2);
      cairo_stroke (cr);
      cairo_restore (cr);
    }

  /* The rectangle being pointed at, in the colour a reference is
   * written in and with the marching border Excel gives it. */
  if (self->pointing)
    {
      O42Range r = o42_range_normalise (self->point_row, self->point_col,
                                        self->point_row2, self->point_col2);
      double x0 = col_x (self, r.col0), y0 = row_y (self, r.row0);
      double x1 = col_x (self, r.col1 + 1), y1 = row_y (self, r.row1 + 1);
      static const double dashes[] = { 4, 3 };

      cairo_save (cr);
      cairo_set_source_rgb (cr, 0.13, 0.35, 0.75);
      cairo_set_line_width (cr, 2);
      cairo_set_dash (cr, dashes, 2, 0);
      cairo_rectangle (cr, floor (x0) + 1, floor (y0) + 1,
                       floor (x1 - x0) - 2, floor (y1 - y0) - 2);
      cairo_stroke (cr);
      cairo_restore (cr);
    }

  /* The auditing arrows, from what a formula reads to the formula, or
   * the other way for its dependents. */
  for (guint i = 0; self->arrows != NULL && i < self->arrows->len; i++)
    {
      const AuditArrow *a = &g_array_index (self->arrows, AuditArrow, i);
      double fx = col_x (self, a->from.col0), fy = row_y (self, a->from.row0);
      double fx1 = col_x (self, a->from.col1 + 1), fy1 = row_y (self, a->from.row1 + 1);
      double tx = (col_x (self, a->to_col) + col_x (self, a->to_col + 1)) / 2;
      double ty = (row_y (self, a->to_row) + row_y (self, a->to_row + 1)) / 2;
      double sx = (fx + fx1) / 2, sy = (fy + fy1) / 2;
      double angle;

      cairo_save (cr);
      cairo_set_source_rgb (cr, 0.1, 0.25, 0.7);
      cairo_set_line_width (cr, 1.5);

      /* A ring round the range the arrow comes from, then the arrow. */
      cairo_rectangle (cr, floor (fx) + 0.5, floor (fy) + 0.5,
                       floor (fx1 - fx) - 1, floor (fy1 - fy) - 1);
      cairo_stroke (cr);

      cairo_move_to (cr, sx, sy);
      cairo_line_to (cr, tx, ty);
      cairo_stroke (cr);

      angle = atan2 (ty - sy, tx - sx);
      cairo_move_to (cr, tx, ty);
      cairo_line_to (cr, tx - 9 * cos (angle - 0.4), ty - 9 * sin (angle - 0.4));
      cairo_line_to (cr, tx - 9 * cos (angle + 0.4), ty - 9 * sin (angle + 0.4));
      cairo_close_path (cr);
      cairo_fill (cr);
      cairo_restore (cr);
    }

  /* Page breaks, where the printed pages would divide. */
  if (self->show_breaks && self->sheet != NULL)
    {
      double margin = o42_sheet_print_setup (self->sheet)->margin;
      O42Pages *pages = o42_pages_new (self->sheet, 595 - 2 * margin, 842 - 2 * margin);
      int *rows = NULL, *cols = NULL;
      int n_rows = o42_pages_row_breaks (pages, &rows);
      int n_cols = o42_pages_col_breaks (pages, &cols);
      static const double dashes[] = { 6, 4 };

      cairo_save (cr);
      cairo_set_source_rgb (cr, 0.1, 0.3, 0.8);
      cairo_set_line_width (cr, 2);
      cairo_set_dash (cr, dashes, 2, 0);
      for (int i = 0; i < n_rows; i++)
        {
          double y = row_y (self, rows[i]);

          cairo_move_to (cr, HEADER_W, floor (y) + 0.5);
          cairo_line_to (cr, scroll_x + view_w, floor (y) + 0.5);
        }
      for (int i = 0; i < n_cols; i++)
        {
          double x = col_x (self, cols[i]);

          cairo_move_to (cr, floor (x) + 0.5, HEADER_H);
          cairo_line_to (cr, floor (x) + 0.5, scroll_y + view_h);
        }
      cairo_stroke (cr);
      cairo_restore (cr);
      g_free (rows);
      g_free (cols);
      o42_pages_free (pages);
    }

  /* Frozen panes: the top rows and left columns painted again where they
   * are pinned, over whatever scrolled under them, with a line along the
   * edge as Excel draws it.  A split pane is the same band, except that
   * it shows whatever row or column it has been scrolled to rather than
   * the first. */
  if (self->frozen_rows > 0 || self->frozen_cols > 0)
    {
      double frozen_w = col_x (self, self->frozen_cols) - HEADER_W;
      double frozen_h = row_y (self, self->frozen_rows) - HEADER_H;
      int top_row = self->split ? self->split_top_row : 0;
      int left_col = self->split ? self->split_left_col : 0;
      /* Where the band's own first row and column have to land. */
      double dy = HEADER_H + scroll_y - row_y (self, top_row);
      double dx = HEADER_W + scroll_x - col_x (self, left_col);
      int band_last_row = MIN (O42_MAX_ROWS - 1, row_at_y (self, row_y (self, top_row) + frozen_h));
      int band_last_col = MIN (O42_MAX_COLS - 1, col_at_x (self, col_x (self, left_col) + frozen_w));

      if (!self->split)
        {
          dy = scroll_y;
          dx = scroll_x;
          band_last_row = self->frozen_rows - 1;
          band_last_col = self->frozen_cols - 1;
        }

      if (self->frozen_rows > 0)
        {
          cairo_save (cr);
          cairo_rectangle (cr, scroll_x + HEADER_W + frozen_w, scroll_y + HEADER_H,
                           view_w, frozen_h);
          cairo_clip (cr);
          cairo_set_source_rgb (cr, 1, 1, 1);
          cairo_paint (cr);
          cairo_translate (cr, 0, dy);
          paint_cells (self, cr, &sel, top_row, band_last_row, first_col, last_col);
          paint_selection (self, cr, &sel);
          cairo_restore (cr);
        }

      if (self->frozen_cols > 0)
        {
          cairo_save (cr);
          cairo_rectangle (cr, scroll_x + HEADER_W, scroll_y + HEADER_H + frozen_h,
                           frozen_w, view_h);
          cairo_clip (cr);
          cairo_set_source_rgb (cr, 1, 1, 1);
          cairo_paint (cr);
          cairo_translate (cr, dx, 0);
          paint_cells (self, cr, &sel, first_row, last_row, left_col, band_last_col);
          paint_selection (self, cr, &sel);
          cairo_restore (cr);
        }

      if (self->frozen_rows > 0 && self->frozen_cols > 0)
        {
          cairo_save (cr);
          cairo_rectangle (cr, scroll_x + HEADER_W, scroll_y + HEADER_H, frozen_w, frozen_h);
          cairo_clip (cr);
          cairo_set_source_rgb (cr, 1, 1, 1);
          cairo_paint (cr);
          cairo_translate (cr, dx, dy);
          paint_cells (self, cr, &sel, top_row, band_last_row, left_col, band_last_col);
          paint_selection (self, cr, &sel);
          cairo_restore (cr);
        }

      /* The edge: a line for a frozen pane, a bar for a split one. */
      if (self->frozen_rows > 0)
        {
          double ly = floor (scroll_y + HEADER_H + frozen_h) + 0.5;

          if (self->split)
            {
              cairo_set_source_rgb (cr, 0.55, 0.55, 0.55);
              cairo_rectangle (cr, scroll_x + HEADER_W, ly - 2, view_w, 4);
              cairo_fill (cr);
            }
          cairo_set_source_rgb (cr, 0, 0, 0);
          cairo_set_line_width (cr, 1.0);
          cairo_move_to (cr, scroll_x + HEADER_W, ly);
          cairo_line_to (cr, scroll_x + view_w, ly);
          cairo_stroke (cr);
        }
      if (self->frozen_cols > 0)
        {
          double lx = floor (scroll_x + HEADER_W + frozen_w) + 0.5;

          if (self->split)
            {
              cairo_set_source_rgb (cr, 0.55, 0.55, 0.55);
              cairo_rectangle (cr, lx - 2, scroll_y + HEADER_H, 4, view_h);
              cairo_fill (cr);
            }
          cairo_set_source_rgb (cr, 0, 0, 0);
          cairo_set_line_width (cr, 1.0);
          cairo_move_to (cr, lx, scroll_y + HEADER_H);
          cairo_line_to (cr, lx, scroll_y + view_h);
          cairo_stroke (cr);
        }
    }

  /* Headers, painted last so that they sit on top of whatever has scrolled
   * under them.  Their position tracks the scroll so they stay pinned. */
  {
    double hx = scroll_x;
    double hy = scroll_y;

    cairo_set_source_rgb (cr, 0.753, 0.753, 0.753);
    cairo_rectangle (cr, hx, hy, view_w, HEADER_H);
    cairo_fill (cr);
    cairo_rectangle (cr, hx, hy, HEADER_W, view_h);
    cairo_fill (cr);

    {
      PangoFontDescription *desc = pango_font_description_from_string ("Sans 8");
      pango_layout_set_font_description (self->layout, desc);
      pango_layout_set_attributes (self->layout, NULL);
      pango_font_description_free (desc);
    }

    cairo_set_line_width (cr, 1.0);

    /* The scrolled columns' headers are clipped to the area beyond the
     * frozen band, whose own headers are painted pinned after them. */
    cairo_save (cr);
    cairo_rectangle (cr, hx + col_x (self, self->frozen_cols), hy, view_w, HEADER_H);
    cairo_clip (cr);
    paint_col_headers (self, cr, &sel, hy, first_col, last_col, col_x (self, first_col));
    cairo_restore (cr);
    if (self->frozen_cols > 0)
      paint_col_headers (self, cr, &sel, hy, 0, self->frozen_cols - 1, hx + HEADER_W);

    cairo_save (cr);
    cairo_rectangle (cr, hx, hy + row_y (self, self->frozen_rows), HEADER_W, view_h);
    cairo_clip (cr);
    paint_row_headers (self, cr, &sel, hx, first_row, last_row, row_y (self, first_row));
    cairo_restore (cr);
    if (self->frozen_rows > 0)
      paint_row_headers (self, cr, &sel, hx, 0, self->frozen_rows - 1, hy + HEADER_H);

    if (self->outline_w > 0)
      paint_outline (self, cr, TRUE, hx, hy, first_row, last_row, view_h);
    if (self->outline_h > 0)
      paint_outline (self, cr, FALSE, hx, hy, first_col, last_col, view_w);

    /* The corner, and the lines that edge the two headers. */
    cairo_set_source_rgb (cr, 0.753, 0.753, 0.753);
    cairo_rectangle (cr, hx, hy, HEADER_W, HEADER_H);
    cairo_fill (cr);

    cairo_set_source_rgb (cr, 0.50, 0.50, 0.50);
    cairo_move_to (cr, hx, hy + HEADER_H - 0.5);
    cairo_line_to (cr, hx + view_w, hy + HEADER_H - 0.5);
    cairo_move_to (cr, hx + HEADER_W - 0.5, hy);
    cairo_line_to (cr, hx + HEADER_W - 0.5, hy + view_h);
    cairo_stroke (cr);
  }

  cairo_destroy (cr);

  /* The editor is a child widget; let GTK paint it over everything. */
  if (self->editor != NULL)
    gtk_widget_snapshot_child (widget, self->editor, snapshot);
  if (self->tip_box != NULL)
    gtk_widget_snapshot_child (widget, self->tip_box, snapshot);
  if (self->complete_box != NULL)
    gtk_widget_snapshot_child (widget, self->complete_box, snapshot);
}

/* Where the cell being edited is on screen: what the name list and the
 * argument tip point at. */
static void
editor_rect (O42Grid *self, GdkRectangle *at)
{
  double sx, sy;
  double w = 80, h = 20;

  at->x = at->y = 0;
  at->width = (int) w;
  at->height = (int) h;
  if (self->sheet == NULL)
    return;

  w = o42_sheet_col_width (self->sheet, self->active_col);
  h = o42_sheet_row_height (self->sheet, self->active_row);
  grid_scroll (self, &sx, &sy);
  at->x = (int) ((col_x (self, self->active_col) -
                  (self->active_col < self->frozen_cols ? 0 : sx)) * self->zoom);
  at->y = (int) ((row_y (self, self->active_row) -
                  (self->active_row < self->frozen_rows ? 0 : sy)) * self->zoom);
  at->width = (int) (w * self->zoom);
  at->height = (int) (h * self->zoom);
}

/* One of them, put where it goes and kept off the right-hand edge. */
static void
place_one (O42Grid *self, GtkWidget *child, int x, int y, int w, int h,
           int view_w)
{
  (void) self;
  if (view_w > 0 && x + w > view_w)
    x = MAX (0, view_w - w);
  gtk_widget_allocate (child, w, h, -1,
                       gsk_transform_translate (NULL,
                                                &GRAPHENE_POINT_INIT (x, y)));
}

/* The tip and the list stack under the cell, the tip nearest it, and
 * go above it together when there is no room below.  Neither ever runs
 * off the right-hand edge. */
static void
place_editor_extras (O42Grid *self)
{
  GdkRectangle at;
  int view_w = gtk_widget_get_width (GTK_WIDGET (self));
  int view_h = gtk_widget_get_height (GTK_WIDGET (self));
  int tip_w = 0, tip_h = 0, list_w = 0, list_h = 0, ignored, y;
  gboolean above = FALSE;

  if (self->tip_box == NULL && self->complete_box == NULL)
    return;

  if (self->tip_box != NULL)
    {
      gtk_widget_measure (self->tip_box, GTK_ORIENTATION_HORIZONTAL, -1,
                          &ignored, &tip_w, NULL, NULL);
      gtk_widget_measure (self->tip_box, GTK_ORIENTATION_VERTICAL, tip_w,
                          &ignored, &tip_h, NULL, NULL);
    }
  if (self->complete_box != NULL)
    {
      gtk_widget_measure (self->complete_box, GTK_ORIENTATION_HORIZONTAL, -1,
                          &ignored, &list_w, NULL, NULL);
      gtk_widget_measure (self->complete_box, GTK_ORIENTATION_VERTICAL, list_w,
                          &ignored, &list_h, NULL, NULL);
    }

  editor_rect (self, &at);
  y = at.y + at.height;
  if (view_h > 0 && y + tip_h + list_h > view_h && at.y - tip_h - list_h >= 0)
    {
      y = at.y - tip_h - list_h;
      above = TRUE;
    }

  /* Going down the tip comes first, going up the list does, so that
   * whichever way they stack the tip is the one against the cell. */
  if (above && self->complete_box != NULL)
    {
      place_one (self, self->complete_box, at.x, y, list_w, list_h, view_w);
      y += list_h;
    }
  if (self->tip_box != NULL)
    {
      place_one (self, self->tip_box, at.x, y, tip_w, tip_h, view_w);
      y += tip_h;
    }
  if (!above && self->complete_box != NULL)
    place_one (self, self->complete_box, at.x, y, list_w, list_h, view_w);
}

static void
place_editor (O42Grid *self)
{
  GtkAllocation alloc;
  graphene_rect_t bounds;
  GskTransform *transform;

  if (self->editor == NULL || self->sheet == NULL)
    return;

  {
    double sx, sy;

    O42Range merged;
    double w = o42_sheet_col_width (self->sheet, self->active_col);
    double h = o42_sheet_row_height (self->sheet, self->active_row);

    if (o42_sheet_merged_at (self->sheet, self->active_row, self->active_col, &merged))
      {
        w = col_x (self, merged.col1) + o42_sheet_col_width (self->sheet, merged.col1) - col_x (self, merged.col0);
        h = row_y (self, merged.row1) + o42_sheet_row_height (self->sheet, merged.row1) - row_y (self, merged.row0);
      }

    grid_scroll (self, &sx, &sy);
    /* The widget is the window's size, so a cell's place in it is its
     * place in the sheet less the scroll -- except in a frozen band,
     * which is painted where it is pinned. */
    alloc.x = (int) ((col_x (self, self->active_col) -
                      (self->active_col < self->frozen_cols ? 0 : sx)) * self->zoom) + 1;
    alloc.y = (int) ((row_y (self, self->active_row) -
                      (self->active_row < self->frozen_rows ? 0 : sy)) * self->zoom) + 1;
    alloc.width = (int) (w * self->zoom) - 1;
    alloc.height = (int) (h * self->zoom) - 1;
  }

  (void) bounds;
  transform = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (alloc.x, alloc.y));
  gtk_widget_allocate (self->editor, alloc.width, alloc.height, -1, transform);
}

static void
o42_grid_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  O42Grid *self = O42_GRID (widget);

  (void) width; (void) height; (void) baseline;

  /* The page size is the window's, so the scrollbars have to be told
   * whenever it changes. */
  grid_configure_adjustments (self);
  if (self->editor != NULL)
    place_editor (self);
  place_editor_extras (self);
}

static void
o42_grid_measure (GtkWidget      *widget,
                  GtkOrientation  orientation,
                  int             for_size,
                  int            *minimum,
                  int            *natural,
                  int            *minimum_baseline,
                  int            *natural_baseline)
{
  double size;

  (void) widget; (void) for_size;

  /* The widget is the size of the window, not the size of the sheet:
   * it scrolls itself, through the adjustments a GtkScrollable carries,
   * and paints what is on screen.  Anything else stops working when the
   * sheet is a million rows deep -- a render tree keeps its coordinates
   * in floats and cairo rasterises in 24.8 fixed point, so past about
   * eight million pixels nothing is drawn at all.
   *
   * What is asked for here is a decent window, and no more. */
  (void) orientation;
  size = orientation == GTK_ORIENTATION_HORIZONTAL ? 320 : 200;

  *minimum = *natural = (int) ceil (size);
  *minimum_baseline = *natural_baseline = -1;
}

/* ---------------------------------------------------------------------- */
/* The menus the right button opens                                        */
/* ---------------------------------------------------------------------- */

/* Excel keeps half its commands here: one menu over the cells, one over
 * each set of headings, and one on the sheet tabs.  Every item is an
 * action the menu bar already has, so nothing new is wired up -- what
 * is new is the pointing at it. */

static void
menu_add (GMenu *menu, const char *label, const char *action)
{
  GMenuItem *item = g_menu_item_new (label, action);

  g_menu_append_item (menu, item);
  g_object_unref (item);
}

static void
show_context_menu (O42Grid *self, GMenu *menu, double x, double y)
{
  GtkWidget *popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
  GdkRectangle at;

  pin_point (self, &x, &y);
  at.x = (int) (x * self->zoom);
  at.y = (int) (y * self->zoom);
  at.width = at.height = 1;

  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &at);
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  gtk_widget_set_halign (popover, GTK_ALIGN_START);
  gtk_widget_set_parent (popover, GTK_WIDGET (self));
  g_signal_connect (popover, "closed", G_CALLBACK (gtk_widget_unparent), NULL);
  g_object_unref (menu);
  gtk_popover_popup (GTK_POPOVER (popover));
}

/* The menu over a cell: what Excel's has, in Excel's order. */
static GMenu *
cell_menu (O42Grid *self)
{
  GMenu *menu = g_menu_new ();
  GMenu *edit = g_menu_new ();
  GMenu *cells = g_menu_new ();
  GMenu *look = g_menu_new ();

  menu_add (edit, "Cu_t", "win.cut");
  menu_add (edit, "_Copy", "win.copy");
  menu_add (edit, "_Paste", "win.paste");
  menu_add (edit, "Paste _Special...", "win.paste-special");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (edit));

  menu_add (cells, "_Insert Cells...", "win.insert-cells");
  menu_add (cells, "_Delete Cells...", "win.delete-cells");
  menu_add (cells, "Clear Co_ntents", "win.clear");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (cells));

  menu_add (look, "_Format Cells...", "win.format-cells");
  menu_add (look, "Insert N_ote...", "win.insert-note");
  menu_add (look, "_Hyperlink...", "win.insert-link");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (look));

  g_object_unref (edit);
  g_object_unref (cells);
  g_object_unref (look);
  (void) self;
  return menu;
}

static GMenu *
row_menu (void)
{
  GMenu *menu = g_menu_new ();
  GMenu *edit = g_menu_new ();
  GMenu *rows = g_menu_new ();

  menu_add (edit, "Cu_t", "win.cut");
  menu_add (edit, "_Copy", "win.copy");
  menu_add (edit, "_Paste", "win.paste");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (edit));

  menu_add (rows, "_Insert Rows", "win.insert-rows");
  menu_add (rows, "_Delete Rows", "win.delete-rows");
  menu_add (rows, "Clear Co_ntents", "win.clear");
  menu_add (rows, "Row _Height...", "win.row-height");
  menu_add (rows, "H_ide", "win.hide-rows");
  menu_add (rows, "_Unhide", "win.unhide-rows");
  menu_add (rows, "_Format Cells...", "win.format-cells");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (rows));

  g_object_unref (edit);
  g_object_unref (rows);
  return menu;
}

static GMenu *
column_menu (void)
{
  GMenu *menu = g_menu_new ();
  GMenu *edit = g_menu_new ();
  GMenu *cols = g_menu_new ();

  menu_add (edit, "Cu_t", "win.cut");
  menu_add (edit, "_Copy", "win.copy");
  menu_add (edit, "_Paste", "win.paste");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (edit));

  menu_add (cols, "_Insert Columns", "win.insert-columns");
  menu_add (cols, "_Delete Columns", "win.delete-columns");
  menu_add (cols, "Clear Co_ntents", "win.clear");
  menu_add (cols, "Column _Width...", "win.column-width");
  menu_add (cols, "_AutoFit", "win.autofit");
  menu_add (cols, "H_ide", "win.hide-columns");
  menu_add (cols, "_Unhide", "win.unhide-columns");
  menu_add (cols, "_Format Cells...", "win.format-cells");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (cols));

  g_object_unref (edit);
  g_object_unref (cols);
  return menu;
}

/* An object -- a picture, a chart, a shape or a control -- has its own
 * short menu. */
static GMenu *
object_menu (O42Grid *self)
{
  GMenu *menu = g_menu_new ();

  if (self->selected_is_chart)
    menu_add (menu, "Format C_hart...", "win.format-chart");
  else if (self->selected_is_shape)
    {
      menu_add (menu, "Format Sha_pe...", "win.format-shape");
      menu_add (menu, "Format Contro_l...", "win.format-control");
    }
  menu_add (menu, "_Group Objects", "win.group-objects");
  menu_add (menu, "_Ungroup Objects", "win.ungroup-objects");
  return menu;
}

/* The right button: what is under it is selected first, so that the
 * command the menu offers acts on what was pointed at. */
static void
on_secondary_pressed (GtkGestureClick *gesture, int n_press,
                      double x, double y, gpointer data)
{
  O42Grid *self = data;
  O42Range selected;
  int row, col;

  (void) gesture; (void) n_press;
  if (self->sheet == NULL)
    return;

  x /= self->zoom;
  y /= self->zoom;
  unpin_point (self, &x, &y);

  if (self->editing)
    o42_grid_commit_edit (self);
  gtk_widget_grab_focus (GTK_WIDGET (self));

  /* An object under the pointer takes the click, as it does with the
   * left button. */
  {
    O42Shape *shape = shape_at (self, x, y);
    O42Chart *chart = shape == NULL ? chart_at (self, x, y) : NULL;
    O42Picture *pic = (shape == NULL && chart == NULL) ? picture_at (self, x, y) : NULL;

    if (shape != NULL || chart != NULL || pic != NULL)
      {
        self->selected_picture = shape != NULL ? shape->id
                               : chart != NULL ? chart->id : pic->id;
        self->selected_is_shape = shape != NULL;
        self->selected_is_chart = chart != NULL;
        gtk_widget_queue_draw (GTK_WIDGET (self));
        show_context_menu (self, object_menu (self), x, y);
        return;
      }
  }

  row = row_at_y (self, y);
  col = col_at_x (self, x);
  selection_range (self, &selected);

  if (row < 0 && col >= 0)          /* the column headings */
    {
      O42Range whole = { 0, col, O42_MAX_ROWS - 1, col };

      if (!(col >= selected.col0 && col <= selected.col1 &&
            selected.row0 == 0 && selected.row1 == O42_MAX_ROWS - 1))
        o42_grid_select_range (self, &whole);
      show_context_menu (self, column_menu (), x, y);
      return;
    }
  if (col < 0 && row >= 0)          /* the row headings */
    {
      O42Range whole = { row, 0, row, O42_MAX_COLS - 1 };

      if (!(row >= selected.row0 && row <= selected.row1 &&
            selected.col0 == 0 && selected.col1 == O42_MAX_COLS - 1))
        o42_grid_select_range (self, &whole);
      show_context_menu (self, row_menu (), x, y);
      return;
    }
  if (row < 0 || col < 0)
    return;                          /* the corner box */

  /* A cell inside the selection keeps it; one outside becomes the
   * selection, which is what every spreadsheet does. */
  if (!o42_range_contains (&selected, row, col))
    move_active (self, row, col, FALSE);
  show_context_menu (self, cell_menu (self), x, y);
}

/* ---------------------------------------------------------------------- */
/* Object plumbing                                                         */
/* ---------------------------------------------------------------------- */

static void
o42_grid_dispose (GObject *object)
{
  O42Grid *self = O42_GRID (object);

  if (self->blink_id != 0)
    {
      g_source_remove (self->blink_id);
      self->blink_id = 0;
    }

  if (self->complete_box != NULL)
    {
      gtk_widget_unparent (self->complete_box);
      self->complete_box = NULL;
      self->complete_list = NULL;
    }

  if (self->tip_box != NULL)
    {
      gtk_widget_unparent (self->tip_box);
      self->tip_box = NULL;
      self->tip_label = NULL;
    }

  if (self->editor != NULL)
    {
      gtk_widget_unparent (self->editor);
      self->editor = NULL;
    }

  g_clear_pointer (&self->complete_names, g_ptr_array_unref);
  g_clear_pointer (&self->extra_sel, g_array_unref);
  g_clear_pointer (&self->refs, g_array_unref);
  g_clear_pointer (&self->arrows, g_array_unref);
  g_clear_object (&self->layout);
  g_clear_pointer (&self->clip_text, g_free);
  self->sheet = NULL;     /* not owned */

  G_OBJECT_CLASS (o42_grid_parent_class)->dispose (object);
}

static void
o42_grid_class_init (O42GridClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = o42_grid_dispose;
  object_class->get_property = o42_grid_get_property;
  object_class->set_property = o42_grid_set_property;

  /* The four a GtkScrollable must have; the scrolled window round the
   * grid finds them by name and hands over its adjustments. */
  g_object_class_override_property (object_class, PROP_HADJUSTMENT, "hadjustment");
  g_object_class_override_property (object_class, PROP_VADJUSTMENT, "vadjustment");
  g_object_class_override_property (object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
  g_object_class_override_property (object_class, PROP_VSCROLL_POLICY, "vscroll-policy");

  widget_class->snapshot = o42_grid_snapshot;
  widget_class->measure = o42_grid_measure;
  widget_class->size_allocate = o42_grid_size_allocate;

  gtk_widget_class_set_css_name (widget_class, "o42grid");

  signals[SIGNAL_SELECTION_CHANGED] =
    g_signal_new ("selection-changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  signals[SIGNAL_SHEET_CHANGED] =
    g_signal_new ("sheet-changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /* A button on the sheet was pressed: the window runs the script it
   * names, since the grid knows nothing of Python. */
  signals[SIGNAL_RUN_SCRIPT] =
    g_signal_new ("run-script", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE,
                  1, G_TYPE_STRING);
}

/* The note under the pointer, as a tooltip. */
static gboolean
on_query_tooltip (GtkWidget *widget, int x, int y, gboolean keyboard,
                  GtkTooltip *tooltip, gpointer data)
{
  O42Grid *self = data;
  double sx = x, sy = y;
  int row, col;
  const char *note;

  (void) widget; (void) keyboard;

  if (self->sheet == NULL)
    return FALSE;

  sx /= self->zoom;
  sy /= self->zoom;
  unpin_point (self, &sx, &sy);
  row = row_at_y (self, sy);
  col = col_at_x (self, sx);
  if (row < 0 || col < 0)
    return FALSE;

  note = o42_sheet_get_note (self->sheet, row, col);
  if (note == NULL && o42_sheet_get_link (self->sheet, row, col) != NULL)
    {
      char *tip = g_strdup_printf ("%s\nCtrl+click to follow", o42_sheet_get_link (self->sheet, row, col));
      gtk_tooltip_set_text (tooltip, tip);
      g_free (tip);
      return TRUE;
    }
  if (note == NULL)
    return FALSE;

  gtk_tooltip_set_text (tooltip, note);
  return TRUE;
}

/* The pointer's place in the sheet's own pixels, which is what the
 * panes are measured in. */
static gboolean
on_scroll (GtkEventControllerScroll *scroll, double dx, double dy, gpointer data)
{
  O42Grid *self = data;
  double x = self->pointer_x, y = self->pointer_y;

  (void) dx; (void) scroll;
  if (!o42_grid_is_split (self))
    return FALSE;

  /* The band's own size is measured from the top left of the sheet, so
   * the pointer has to be measured the same way. */
  if (self->hadj != NULL && self->vadj != NULL)
    {
      GtkAdjustment *h = self->hadj;
      GtkAdjustment *v = self->vadj;

      x -= gtk_adjustment_get_value (h);
      y -= gtk_adjustment_get_value (v);
    }
  return o42_grid_scroll_pane (self, x, y, dy);
}

static void
o42_grid_init (O42Grid *self)
{
  GtkEventController *key, *motion;
  GtkGesture *click;

  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "cell");

  self->header_w = 42;   /* three digits and room; outline_sync grows it */
  self->layout = pango_layout_new (gtk_widget_get_pango_context (GTK_WIDGET (self)));

  key = gtk_event_controller_key_new ();
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), key);

  {
    /* The right button opens a menu; the left one does everything else. */
    GtkGesture *secondary = gtk_gesture_click_new ();

    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (secondary), GDK_BUTTON_SECONDARY);
    g_signal_connect (secondary, "pressed", G_CALLBACK (on_secondary_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (secondary));
  }

  click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_click_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_click_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

  motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);

  {
    /* The wheel over a split pane scrolls that pane; anywhere else it
     * goes on to the scrolled window, which moves the main one. */
    GtkEventController *scroll =
      gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);

    gtk_event_controller_set_propagation_phase (scroll, GTK_PHASE_CAPTURE);
    g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
    gtk_widget_add_controller (GTK_WIDGET (self), scroll);
  }

  self->tab_origin_col = -1;
  self->resize_col = self->resize_row = -1;
  self->resize_handle = -1;
  self->zoom = 1.0;

  gtk_widget_set_has_tooltip (GTK_WIDGET (self), TRUE);
  g_signal_connect (self, "query-tooltip", G_CALLBACK (on_query_tooltip), self);
  self->blink_on = TRUE;
  self->blink_id = g_timeout_add (500, on_blink, self);
}

GtkWidget *
o42_grid_new (void)
{
  return g_object_new (O42_TYPE_GRID, NULL);
}

void
o42_grid_set_sheet (O42Grid *self, O42Sheet *sheet)
{
  g_return_if_fail (O42_IS_GRID (self));

  if (self->editing)
    o42_grid_cancel_edit (self);

  self->sheet = sheet;
  self->active_row = self->anchor_row = 0;
  self->active_col = self->anchor_col = 0;
  self->frozen_rows = self->frozen_cols = 0;
  if (sheet != NULL)
    o42_sheet_get_frozen (sheet, &self->frozen_rows, &self->frozen_cols);
  self->selected_picture = 0;
  self->picture_drag = FALSE;

  outline_sync (self);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  selection_changed (self);
}

O42Sheet *
o42_grid_get_sheet (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), NULL);
  return self->sheet;
}

O42Chart *
o42_grid_selected_chart (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), NULL);
  if (self->sheet != NULL && o42_sheet_is_chart_sheet (self->sheet))
    return o42_sheet_the_chart (self->sheet);
  if (!self->selected_is_chart || self->selected_picture == 0)
    return NULL;
  return o42_sheet_find_chart (self->sheet, self->selected_picture);
}

void
o42_grid_insert_shape (O42Grid *self, O42ShapeKind kind, const char *text)
{
  O42Shape *shape;
  int row = 0, col = 0;

  g_return_if_fail (O42_IS_GRID (self) && self->sheet != NULL);
  o42_grid_get_active (self, &row, &col);
  shape = o42_sheet_add_shape (self->sheet, kind, row, col);
  if (shape == NULL)
    return;
  shape->dx = 8;
  shape->dy = 8;
  if (text != NULL)
    {
      g_free (shape->text);
      shape->text = g_strdup (text);
    }
  self->selected_picture = shape->id;
  self->selected_is_chart = FALSE;
  self->selected_is_shape = TRUE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

O42Shape *
o42_grid_selected_shape (O42Grid *self)
{
  g_return_val_if_fail (O42_IS_GRID (self), NULL);
  if (!self->selected_is_shape || self->selected_picture == 0)
    return NULL;
  return o42_sheet_find_shape (self->sheet, self->selected_picture);
}
