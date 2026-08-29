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

G_END_DECLS
