/* o42-window.h - the workbook window: menus, toolbars, formula bar, tabs
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "o42-book.h"

G_BEGIN_DECLS

#define O42_TYPE_WINDOW (o42_window_get_type ())
G_DECLARE_FINAL_TYPE (O42Window, o42_window, O42, WINDOW, GtkApplicationWindow)

GtkWidget *o42_window_new (GtkApplication *app);

/* Window > New Window: another window on the same book, numbered as
 * Excel numbers them, Book1:2. */
GtkWidget *o42_window_new_on_book (GtkApplication *app, O42Book *book, GFile *file);

/* Loads a file into the window, replacing what it holds.  Reports failure
 * to the user itself; returns whether it succeeded. */
gboolean o42_window_open_file (O42Window *self, GFile *file);

/* Whether the file is text the Text Import Wizard reads: .csv, .txt,
 * .tsv, .tab or .prn. */
gboolean o42_window_is_text_file (GFile *file);

/* File > Open of a text file: the Text Import Wizard, and the book it
 * makes in this window when it is blank or in a new one when it is
 * not.  With `open` FALSE it is File > Import Text File instead, and
 * the records go into the sheet on show at the active cell. */
void o42_window_import_text (O42Window *self, GFile *file, gboolean open);

/* TRUE if the window holds nothing worth keeping: a fresh, unmodified
 * book.  Open reuses such a window rather than making another. */
gboolean o42_window_is_blank (O42Window *self);

/* Makes a cell the active one, as the name box does. */
/* Starts typing into the active cell, as --type does: the text goes
 * into the editor as if it had been typed there. */
void o42_window_type (O42Window *self, const char *text);
void o42_window_type_bar (O42Window *self, const char *text);

/* Points at a cell or a range while a formula is being typed, as
 * clicking and dragging over the grid does. */
void o42_window_point (O42Window *self, const O42Range *range);
gboolean o42_window_point_step (O42Window *self, const char *direction);

void o42_window_select_cell (O42Window *self, int row, int col);

/* Runs Python against the window's book, as the console does, and
 * prints what it printed on standard output: for --py. */
void o42_window_run_python (O42Window *self, const char *code);

/* Whether the small dialogs are modal.  They are, except when a picture
 * is being taken of them: see --screenshot. */
void o42_window_set_dialogs_modal (gboolean modal);

G_END_DECLS
