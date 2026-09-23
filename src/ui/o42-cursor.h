/* o42-cursor.h - pointer shapes that every cursor theme can show
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GTK takes a pointer shape by name, and the names it takes are the CSS
 * ones: "cell", "col-resize", "se-resize".  A cursor theme need not
 * carry any of them.  Themes that came from X11 -- Humanity, DMZ and
 * the rest -- carry the old names instead, and a name a theme has
 * nothing for leaves GTK with no cursor image at all: on Wayland that
 * hides the pointer, which is what "the mouse cursor disappears over
 * the sheet" looks like.
 *
 * So every shape is asked for with the older names behind it and
 * "default" at the end, which every theme has.  The cursors are made
 * once and kept.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Give `widget` the named pointer shape, falling back to a name the
 * theme does have.  NULL takes the shape away again, so that the
 * widget shows whatever its parent does. */
void o42_set_cursor_name (GtkWidget *widget, const char *name);

G_END_DECLS
