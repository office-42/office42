/* o42-grid.h - the grid: cells, headers, selection, in-cell editing
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "o42-sheet.h"

G_BEGIN_DECLS

#define O42_TYPE_GRID (o42_grid_get_type ())
G_DECLARE_FINAL_TYPE (O42Grid, o42_grid, O42, GRID, GtkWidget)

GtkWidget *o42_grid_new (void);

void      o42_grid_set_sheet (O42Grid *self, O42Sheet *sheet);
O42Sheet *o42_grid_get_sheet (O42Grid *self);

/* The active cell and the selection around it.  The selection always
 * contains the active cell; with nothing dragged out, it is just that cell. */
void o42_grid_get_active    (O42Grid *self, int *row, int *col);
void o42_grid_set_active    (O42Grid *self, int row, int col);
void o42_grid_get_selection (O42Grid *self, O42Range *range);
void o42_grid_select_range  (O42Grid *self, const O42Range *range);

/* ---- Editing ---------------------------------------------------------- */

gboolean o42_grid_is_editing    (O42Grid *self);
void     o42_grid_set_mirror    (O42Grid *self, GtkWidget *entry);
void     o42_grid_mirror_set_text (O42Grid *self, const char *text);
/* As if the pointer had pressed on the rectangle's first corner and
 * been dragged to its last: a reference while a formula is being
 * typed, a selection otherwise. */
void     o42_grid_click_range (O42Grid *self, const O42Range *range);

/* One key of what is typed into a formula besides its text: "left",
 * "shift-down", "ctrl-right" or "f4", each going the way the key does. */
gboolean o42_grid_point_step (O42Grid *self, const char *direction);

void     o42_grid_begin_edit    (O42Grid *self, const char *initial);
void     o42_grid_commit_edit   (O42Grid *self);
void     o42_grid_cancel_edit   (O42Grid *self);

/* Puts text into the active cell without opening the editor: what the
 * formula bar does when you press Enter in it. */
void     o42_grid_set_active_input (O42Grid *self, const char *text);

void o42_grid_delete_selection (O42Grid *self);
void o42_grid_undo (O42Grid *self);
void o42_grid_redo (O42Grid *self);
void o42_grid_cut   (O42Grid *self);
void o42_grid_copy  (O42Grid *self);
void o42_grid_paste (O42Grid *self);
void o42_grid_select_all (O42Grid *self);

/* Edit > Paste Special, from our own last copy. */
void o42_grid_paste_special (O42Grid *self, O42PasteMode mode, gboolean transpose);
gboolean o42_grid_has_own_copy (O42Grid *self);

/* Fill Down copies the selection's top row into the rows below it; Fill
 * Right its left column into the columns to the right. */
void o42_grid_fill (O42Grid *self, gboolean down);

/* Inserts as many rows (columns) as the selection spans, above (left of)
 * it; deletes the rows (columns) it spans. */
void o42_grid_insert_rows    (O42Grid *self);
void o42_grid_insert_columns (O42Grid *self);
void o42_grid_delete_rows    (O42Grid *self);
void o42_grid_delete_columns (O42Grid *self);

/* Sizes for every column (row) the selection spans, in pixels; and the
 * width each column needs to show its widest cell, which is what a
 * double-click on the header boundary does. */
void o42_grid_set_column_width (O42Grid *self, int width);
void o42_grid_set_row_height   (O42Grid *self, int height);
void o42_grid_autofit_columns  (O42Grid *self);

/* ---- Pictures --------------------------------------------------------- */

/* Puts a picture at the active cell and selects it.  A picture wider than
 * about six columns is shown scaled down; the sheet keeps the original. */
void o42_grid_insert_picture (O42Grid *self, GBytes *data, const char *format,
                              int pixel_w, int pixel_h);

/* Puts a chart of the selection at the cell below and right of it, and
 * selects the chart.  The label flags say whether the first row names
 * the series and the first column the categories. */
void o42_grid_insert_chart (O42Grid *self, O42ChartKind kind, const char *title,
                            gboolean series_in_rows,
                            gboolean first_row_labels, gboolean first_col_labels);

/* Puts a shape near the active cell and selects it. */
void o42_grid_insert_shape (O42Grid *self, O42ShapeKind kind, const char *text);

/* The shape selected by clicking it, or NULL. */
O42Shape *o42_grid_selected_shape (O42Grid *self);

/* The chart selected by clicking it, or NULL. */
O42Chart *o42_grid_selected_chart (O42Grid *self);

/* The guess the chart would make about the selection's labels. */
void o42_grid_guess_chart_labels (O42Grid *self, gboolean *first_row,
                                  gboolean *first_col);

/* ---- Formatting ------------------------------------------------------- */

void o42_grid_apply_fmt (O42Grid *self, O42FmtMask mask, const O42Fmt *value);
const O42Fmt *o42_grid_active_fmt (O42Grid *self);

/* Tells the grid the sheet changed under it, so it repaints. */
void o42_grid_refresh (O42Grid *self);

/* Grows every row that holds wrapped text to the height that text
 * needs, unless a height was set for the row by hand.  Called when a
 * book is opened, since a file says a cell wraps without saying how
 * tall that makes its row. */
void o42_grid_fit_wrapped_rows (O42Grid *self);

/* The format painter: picks up the look of the active cell and puts the
 * pointer in a state where the next cell or range clicked is given it.
 * Clicking anywhere else, or pressing Escape, puts the brush down. */
void     o42_grid_pick_up_format (O42Grid *self);

/* Every rectangle of the selection -- the one being made and the ones
 * Ctrl+click put behind it -- appended to `out`, an array of O42Range.
 * There is always at least one. */
void     o42_grid_selection_ranges (O42Grid *self, GArray *out);
gboolean o42_grid_is_painting    (O42Grid *self);

/* Hides or shows the rows (columns) the selection spans.  Unhide works
 * on the hidden ones inside the selection. */
void o42_grid_hide_rows      (O42Grid *self, gboolean hide);
void o42_grid_hide_columns   (O42Grid *self, gboolean hide);

/* Data > Filter > AutoFilter on the selection (or the block around the
 * active cell), or off again. */
void o42_grid_toggle_autofilter (O42Grid *self);

/* Format > Merge Cells joins the selection into one cell; Unmerge splits
 * whatever merges the selection touches. */
void o42_grid_merge_cells (O42Grid *self, gboolean merge);

/* View > Freeze Panes: the rows above and the columns left of the active
 * cell stay put while the rest scrolls; again to unfreeze. */
void     o42_grid_freeze_panes (O42Grid *self);
gboolean o42_grid_has_frozen_panes (O42Grid *self);

/* Window > Split: bands that scroll on their own rather than staying
 * pinned.  o42_grid_scroll_pane scrolls whichever band the pointer is
 * over, and says whether it did. */
/* Excel's auditing arrows: from each rectangle a formula reads to the
 * formula itself, or from a cell to each formula that reads it.  The
 * arrows stay until they are cleared, and are not kept in the file --
 * they are a way of looking, not part of the book. */
void     o42_grid_trace            (O42Grid *self, gboolean precedents);
void     o42_grid_clear_arrows     (O42Grid *self);
gboolean o42_grid_has_arrows       (O42Grid *self);

void     o42_grid_show_page_breaks  (O42Grid *self, gboolean show);
gboolean o42_grid_page_breaks_shown (O42Grid *self);

/* Format > Group: the objects anchored in the selection move together
 * from now on.  FALSE takes them apart again. */
void     o42_grid_group_objects (O42Grid *self, gboolean group);

void     o42_grid_split_panes  (O42Grid *self);
gboolean o42_grid_is_split     (O42Grid *self);
gboolean o42_grid_scroll_pane  (O42Grid *self, double x, double y, double dy);

/* View > Zoom: 1.0 is 100%.  Clamped to 25%..400%. */
void   o42_grid_set_zoom (O42Grid *self, double zoom);
double o42_grid_get_zoom (O42Grid *self);

/* View > Options: gridlines on or off, zero values shown or blank. */
void     o42_grid_set_show_gridlines (O42Grid *self, gboolean show);
gboolean o42_grid_get_show_gridlines (O42Grid *self);
void     o42_grid_set_show_zeros     (O42Grid *self, gboolean show);
gboolean o42_grid_get_show_zeros     (O42Grid *self);

G_END_DECLS
