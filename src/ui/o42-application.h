/* o42-application.h - the GtkApplication for office42
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define O42_TYPE_APPLICATION (o42_application_get_type ())
G_DECLARE_FINAL_TYPE (O42Application, o42_application, O42, APPLICATION, GtkApplication)

O42Application *o42_application_new (void);

/* The few settings that are the program's rather than a book's, kept
 * in options.ini under the user's configuration directory: read at
 * start-up, and written back whenever Options is told something new.
 * `key` is "currency" or "fixed_decimals"; a missing key reads NULL. */
char *o42_prefs_get (const char *key);
void  o42_prefs_set (const char *key, const char *value);   /* NULL removes it */

G_END_DECLS
