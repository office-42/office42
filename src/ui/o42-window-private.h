/* o42-window-private.h - what the dialogs share with the window
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The window is a menu bar, two toolbars, a grid and a great many
 * dialogs.  The dialogs are a file apiece by the menu they hang off;
 * this header is the seam: the window's own shape, the frame every
 * dialog is built in, and the handful of helpers they all use.
 *
 * Nothing here is for anyone outside src/ui.
 */

#pragma once

#include "o42-window.h"

#include "o42-grid.h"
#include "o42-sql.h"

G_BEGIN_DECLS

struct _O42Window {
  GtkApplicationWindow parent_instance;

  O42Book    *book;
  O42Sheet   *sheet;           /* the sheet on show, one of the book's */
  gpointer    python_console;  /* the PyConsole while its window is open */
  GtkWidget  *scripts_bar;     /* "this book has scripts", shown on opening one */
  O42Grid    *grid;
  GtkWidget  *tabs;

  GFile      *file;            /* where the book lives, or NULL for a new one */
  gboolean    close_after_save;
  int         view_number;     /* 0 for the only window on the book, else 1, 2... */
  gboolean    telling;         /* inside o42_book_changed, to skip our own echo */

  GtkPageSetup     *page_setup;      /* from Page Setup, or NULL for the default */
  GtkPrintSettings *print_settings;  /* remembered between prints */

  GtkWidget  *title_label;
  GtkWidget  *name_box;
  GtkWidget  *formula_entry;
  O42Db      *db;              /* the book's database, opened when first wanted */
  GtkWidget  *status_label;
  GtkWidget  *status_sum;

  GtkWidget  *font_drop;
  GtkWidget  *size_drop;
  GtkWidget  *bold_btn, *italic_btn, *underline_btn;
  GtkWidget  *align_btn[3];

  GListModel *families;
  GHashTable *family_index;
  gboolean    updating;
};

/* ---- The frame a dialog is built in ------------------------------------ */

/* A transient window with a content box and a row for its buttons: the
 * shape every Excel 5 dialog had. */
GtkWidget *o42_dialog_frame (O42Window *self, const char *title, gboolean modal,
                             GtkWidget **content, GtkWidget **buttons);
GtkWidget *o42_dialog_button (GtkWidget *buttons, const char *label,
                              GCallback cb, gpointer data);
void       o42_dialog_close_clicked (GtkWidget *w, gpointer dialog);

/* Puts the keyboard back on the grid when a dialog goes away. */
void       o42_dialog_destroy_refocus (GtkWidget *w, gpointer grid);

/* A label to the left of a control, on one row of a grid; the control
 * comes back. */
GtkWidget *o42_labelled (GtkWidget *grid, int row, const char *label, GtkWidget *control);

/* A button that opens the colour chooser, showing the colour it holds. */
GtkWidget *o42_colour_button (guint32 colour, const char *title);

/* A page of a notebook, laid out as a grid. */
GtkWidget *o42_page_grid (GtkWidget *notebook, const char *title);

/* A drop-down over a list of strings the program owns, translated. */
GtkWidget *o42_drop_down_of (const char *const *names);

/* The two halves of the factory a list view is built with. */
void o42_wizard_setup_item (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data);
void o42_wizard_bind_item  (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data);

/* Colours, between the two ways of writing one. */
guint32 o42_colour_from_rgba (const GdkRGBA *rgba);
void    o42_rgba_from_colour (guint32 colour, GdkRGBA *rgba);

/* ---- The window itself -------------------------------------------------- */

/* The conditions a rule or a validation tests, in the order the
 * drop-downs list them. */
extern const char *O42_COND_NAMES[];

/* Shows the nth sheet of the book. */
void o42_window_show_sheet (O42Window *self, int index);

/* Runs one of the book's scripts, and takes away the bar that offers
 * to run them. */
gboolean o42_window_run_script (O42Window *self, const char *name, const char *code);
void o42_scripts_bar_hide  (O42Window *self);

/* Everything the toolbars and the status bar show, worked out again. */
void o42_window_sync (O42Window *self);

/* Tells the other windows on this book that something changed. */
void o42_window_tell_book (O42Window *self, const char *what);

/* A message in a dialog of its own, for what has gone wrong. */
void o42_window_show_error (O42Window *self, const char *heading, GError *error);

/* The actions dialogs-data.c answers. */
void action_advanced_filter (GSimpleAction *a, GVariant *p, gpointer data);
void action_consolidate (GSimpleAction *a, GVariant *p, gpointer data);
void action_remove_duplicates (GSimpleAction *a, GVariant *p, gpointer data);
void action_scenarios (GSimpleAction *a, GVariant *p, gpointer data);
void action_sort (GSimpleAction *a, GVariant *p, gpointer data);
void action_subtotals (GSimpleAction *a, GVariant *p, gpointer data);
void action_table (GSimpleAction *a, GVariant *p, gpointer data);

/* The actions dialogs-data.c answers. */
void action_pivot (GSimpleAction *a, GVariant *p, gpointer data);
void action_refresh_pivot (GSimpleAction *a, GVariant *p, gpointer data);
void action_text_to_columns (GSimpleAction *a, GVariant *p, gpointer data);
void action_validation (GSimpleAction *a, GVariant *p, gpointer data);
void action_group_rows (GSimpleAction *a, GVariant *p, gpointer data);
void action_group_cols (GSimpleAction *a, GVariant *p, gpointer data);
void action_ungroup_rows (GSimpleAction *a, GVariant *p, gpointer data);
void action_ungroup_cols (GSimpleAction *a, GVariant *p, gpointer data);

/* The actions dialogs-tools.c answers. */
void action_analysis (GSimpleAction *a, GVariant *p, gpointer data);
void action_clear_arrows (GSimpleAction *a, GVariant *p, gpointer data);
void action_custom_lists (GSimpleAction *a, GVariant *p, gpointer data);
void action_custom_views (GSimpleAction *a, GVariant *p, gpointer data);
void action_goal_seek (GSimpleAction *a, GVariant *p, gpointer data);
void action_group_objects (GSimpleAction *a, GVariant *p, gpointer data);
void action_page_breaks (GSimpleAction *a, GVariant *p, gpointer data);
void action_protect (GSimpleAction *a, GVariant *p, gpointer data);
void action_python_console (GSimpleAction *a, GVariant *p, gpointer data);
void action_python_run (GSimpleAction *a, GVariant *p, gpointer data);
void action_record_macro (GSimpleAction *a, GVariant *p, gpointer data);
void action_scripts (GSimpleAction *a, GVariant *p, gpointer data);
void action_scripts_run_all (GSimpleAction *a, GVariant *p, gpointer data);
void action_solver (GSimpleAction *a, GVariant *p, gpointer data);
void action_spelling (GSimpleAction *a, GVariant *p, gpointer data);
void action_trace_dependents (GSimpleAction *a, GVariant *p, gpointer data);
void action_trace_precedents (GSimpleAction *a, GVariant *p, gpointer data);
void action_ungroup_objects (GSimpleAction *a, GVariant *p, gpointer data);

/* The actions dialogs-format.c answers. */
void action_conditional (GSimpleAction *a, GVariant *p, gpointer data);
void action_format_cells (GSimpleAction *a, GVariant *p, gpointer data);

/* The actions dialogs-format.c answers. */


/* The actions dialogs-format.c answers. */
void action_autoformat (GSimpleAction *a, GVariant *p, gpointer data);
void action_format_painter (GSimpleAction *a, GVariant *p, gpointer data);

/* Whether a dialog holds the window while it is open; the tests turn
 * it off so that a screenshot catches both. */
extern gboolean o42_dialogs_modal;

void action_merge_cells (GSimpleAction *a, GVariant *p, gpointer data);
void action_unmerge_cells (GSimpleAction *a, GVariant *p, gpointer data);
void action_hide_rows (GSimpleAction *a, GVariant *p, gpointer data);
void action_unhide_rows (GSimpleAction *a, GVariant *p, gpointer data);
void action_hide_columns (GSimpleAction *a, GVariant *p, gpointer data);
void action_unhide_columns (GSimpleAction *a, GVariant *p, gpointer data);
void action_filter (GSimpleAction *a, GVariant *p, gpointer data);
void action_column_width (GSimpleAction *a, GVariant *p, gpointer data);
void action_row_height (GSimpleAction *a, GVariant *p, gpointer data);
void action_autofit (GSimpleAction *a, GVariant *p, gpointer data);

G_END_DECLS
