/* o42-python.h - Python inside the spreadsheet
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An embedded CPython with an `office42` module over the book: what
 * VBA is to Excel, what the Python plugin is to Gnumeric.  Scripts run
 * in one interpreter that lives as long as the program; a console keeps
 * its namespace between lines, as a REPL does.  Built only with
 * -Dpython=enabled; without it every call here says so and does
 * nothing.  This layer sees the model and the evaluator, never GTK.
 */

#pragma once

#include "o42-book.h"
#include <gio/gio.h>

G_BEGIN_DECLS

/* Whether Python was built in; and its version ("3.14.4"), or NULL. */
gboolean    o42_python_available (void);
const char *o42_python_version   (void);

/* Runs `code` with `book` and `sheet` bound as the module's `book`
 * and `sheet`.  What the code printed, and any traceback, comes back
 * in `output` (never NULL; free it).  TRUE unless an exception got
 * out.  A single expression is evaluated and its value printed, as
 * in a console.  `filename` names the code in tracebacks. */
gboolean o42_python_run      (O42Book *book, O42Sheet *sheet, const char *code,
                              const char *filename, char **output);
gboolean o42_python_run_file (O42Book *book, O42Sheet *sheet, GFile *file,
                              char **output);

/* Forgets the console's variables and the functions scripts defined. */
void o42_python_reset (void);

G_END_DECLS
