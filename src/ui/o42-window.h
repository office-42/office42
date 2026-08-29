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

/* TRUE if the window holds nothing worth keeping: a fresh, unmodified
 * book.  Open reuses such a window rather than making another. */
gboolean o42_window_is_blank (O42Window *self);

/* Makes a cell the active one, as the name box does. */
void o42_window_select_cell (O42Window *self, int row, int col);

/* Whether the small dialogs are modal.  They are, except when a picture
 * is being taken of them: see --screenshot. */
void o42_window_set_dialogs_modal (gboolean modal);

G_END_DECLS
