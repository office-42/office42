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
  gpointer    scripts_prompt;  /* the Scripts dialog while it is open, for the debugger */
  GtkEventController *macro_keys;  /* Ctrl+Shift+letter for the book's macros */
  GtkWidget  *scripts_bar;     /* "this book has scripts", shown on opening one */
  GtkWidget  *scripts_bar_label, *scripts_bar_run;
  O42Grid    *grid;
  GtkWidget  *tabs;

  GFile      *file;            /* where the book lives, or NULL for a new one */
  gboolean    close_after_save;
  int         view_number;     /* 0 for the only window on the book, else 1, 2... */
  gboolean    telling;         /* inside o42_book_changed, to skip our own echo */

  GtkPrintSettings *print_settings;  /* remembered between prints */
  struct _SetupPrompt *last_setup;   /* the Page Setup dialog open, if one is */

  GtkWidget  *title_label;
  GtkWidget  *name_box;
  GtkWidget  *formula_entry;
  O42Db      *db;              /* the book's database, opened when first wanted */
  GtkWidget  *status_label;
  char       *status_text;     /* what a script set the status bar to, or NULL */
  GtkWidget  *status_sum;

  GtkWidget  *font_drop;
  GtkWidget  *size_drop;
  GtkWidget  *bold_btn, *italic_btn, *underline_btn;
  GtkWidget  *align_btn[3];

  GListModel *families;
  GHashTable *family_index;
  gboolean    updating;
  gboolean    applying_view;    /* the sheet's view is being put on the grid */
};

/* ---- The frame a dialog is built in ------------------------------------ */

/* A transient window with a content box and a row for its buttons: the
 * shape every Excel 97 dialog had. */
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
void     o42_window_fire_event (O42Window *self, const char *event, const O42Range *range);

/* The Scripts dialog's half of the step debugger: shows the line and
 * the variables, waits for Step, Continue or Stop; 0, 1 or 2. */
int      o42_window_debug_pause (O42Window *self, const char *filename, int line, const char *variables);
/* Opens Scripts in this Book on the script of that name (NULL for the first). */
void     o42_window_edit_script (O42Window *self, const char *which);
void o42_scripts_bar_hide  (O42Window *self);
gboolean window_book_calls (O42Window *self, const char *name);

/* Everything the toolbars and the status bar show, worked out again. */
void o42_window_sync (O42Window *self);

/* Tells the other windows on this book that something changed. */
void o42_window_tell_book (O42Window *self, const char *what);

/* A message in a dialog of its own, for what has gone wrong. */
void o42_window_show_error (O42Window *self, const char *heading, GError *error);

/* The actions dialogs-edit.c answers: the File, Edit and Insert menus' dialogs. */
void action_properties (GSimpleAction *a, GVariant *p, gpointer data);
void action_fill_series (GSimpleAction *a, GVariant *p, gpointer data);
void action_move_copy_sheet (GSimpleAction *a, GVariant *p, gpointer data);
void action_paste_name (GSimpleAction *a, GVariant *p, gpointer data);
void action_create_names (GSimpleAction *a, GVariant *p, gpointer data);
void action_apply_names (GSimpleAction *a, GVariant *p, gpointer data);

/* The actions dialogs-data.c answers. */
void action_advanced_filter (GSimpleAction *a, GVariant *p, gpointer data);
void action_consolidate (GSimpleAction *a, GVariant *p, gpointer data);
void action_remove_duplicates (GSimpleAction *a, GVariant *p, gpointer data);
void action_scenarios (GSimpleAction *a, GVariant *p, gpointer data);
void action_sort (GSimpleAction *a, GVariant *p, gpointer data);
void action_data_form (GSimpleAction *a, GVariant *p, gpointer data);
void action_subtotals (GSimpleAction *a, GVariant *p, gpointer data);
void action_table (GSimpleAction *a, GVariant *p, gpointer data);

/* The actions dialogs-data.c answers. */
void action_pivot (GSimpleAction *a, GVariant *p, gpointer data);
void action_refresh_pivot (GSimpleAction *a, GVariant *p, gpointer data);
void action_text_to_columns (GSimpleAction *a, GVariant *p, gpointer data);
void action_validation (GSimpleAction *a, GVariant *p, gpointer data);
void action_circle_invalid (GSimpleAction *a, GVariant *p, gpointer data);
void action_outline_settings (GSimpleAction *a, GVariant *p, gpointer data);
void action_euro_convert (GSimpleAction *a, GVariant *p, gpointer data);
void action_new_from_template (GSimpleAction *a, GVariant *p, gpointer data);

/* A book that came from a template is nobody's file yet. */
void o42_window_forget_file (O42Window *self);
void action_clear_circles (GSimpleAction *a, GVariant *p, gpointer data);
void action_group_rows (GSimpleAction *a, GVariant *p, gpointer data);
void action_group_cols (GSimpleAction *a, GVariant *p, gpointer data);
void action_ungroup_rows (GSimpleAction *a, GVariant *p, gpointer data);
void action_ungroup_cols (GSimpleAction *a, GVariant *p, gpointer data);
void action_auto_outline (GSimpleAction *a, GVariant *p, gpointer data);
void action_clear_outline (GSimpleAction *a, GVariant *p, gpointer data);
void action_show_detail (GSimpleAction *a, GVariant *p, gpointer data);
void action_hide_detail (GSimpleAction *a, GVariant *p, gpointer data);

/* The actions dialogs-tools.c answers. */
void action_analysis (GSimpleAction *a, GVariant *p, gpointer data);
void action_clear_arrows (GSimpleAction *a, GVariant *p, gpointer data);
void action_evaluate_formula (GSimpleAction *a, GVariant *p, gpointer data);
void action_trace_error (GSimpleAction *a, GVariant *p, gpointer data);
void action_watch_window (GSimpleAction *a, GVariant *p, gpointer data);
/* The Watch Window's values, worked out again; nothing when it is shut. */
void o42_watch_window_refresh (O42Window *self);
void action_custom_lists (GSimpleAction *a, GVariant *p, gpointer data);
void action_custom_views (GSimpleAction *a, GVariant *p, gpointer data);
void action_goal_seek (GSimpleAction *a, GVariant *p, gpointer data);
void action_group_objects (GSimpleAction *a, GVariant *p, gpointer data);
void action_page_breaks (GSimpleAction *a, GVariant *p, gpointer data);
void action_protect (GSimpleAction *a, GVariant *p, gpointer data);
void action_python_console (GSimpleAction *a, GVariant *p, gpointer data);
void action_python_run (GSimpleAction *a, GVariant *p, gpointer data);
void action_record_macro (GSimpleAction *a, GVariant *p, gpointer data);
void action_stop_recording (GSimpleAction *a, GVariant *p, gpointer data);
void action_relative_refs (GSimpleAction *a, GVariant *p, gpointer data);
void action_macros (GSimpleAction *a, GVariant *p, gpointer data);
void action_run_macro (GSimpleAction *a, GVariant *p, gpointer data);
void action_scripts (GSimpleAction *a, GVariant *p, gpointer data);
void action_script_step (GSimpleAction *a, GVariant *p, gpointer data);
void action_script_continue (GSimpleAction *a, GVariant *p, gpointer data);
void action_script_stop (GSimpleAction *a, GVariant *p, gpointer data);

/* Binds Ctrl+Shift+letter to the book's macros that ask for one; called
 * whenever the scripts change. */
void o42_window_bind_macro_keys (O42Window *self);
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
void action_autofit_rows (GSimpleAction *a, GVariant *p, gpointer data);
void action_standard_width (GSimpleAction *a, GVariant *p, gpointer data);
void action_filter_show_all (GSimpleAction *a, GVariant *p, gpointer data);
void action_zoom_dialog (GSimpleAction *a, GVariant *p, gpointer data);

G_END_DECLS
