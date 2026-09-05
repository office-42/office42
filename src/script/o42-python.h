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

/* Forgets the console's variables and the functions the current
 * book's scripts defined; the personal scripts' stay. */
void o42_python_reset (void);

/* A book is going: the functions its scripts defined go with it, so
 * that another book's NPV2 is its own. */
void o42_python_forget_book (O42Book *book);

/* Personal scripts: every .py in the user's office42/scripts folder is
 * run when Python starts, so what they define -- functions for cells,
 * helpers for the console -- is there in every book.  They are the
 * user's own and need no trusting.  The folder (caller frees), made
 * if it is not there; and the scripts in it, sorted (caller frees the
 * strv). */
char  *o42_python_personal_folder  (void);
char **o42_python_personal_scripts (void);

/* Starts Python now if there are personal scripts, so that the
 * functions they define are known before a file's formulas ask for
 * them; otherwise it starts with the first script run.  TRUE if it
 * is running. */
gboolean o42_python_start (void);

/* ---- What the window does for a script ------------------------------- */

/* Excel's macros see the selection, put up message boxes and save
 * files: things that belong to the window, which this layer never
 * sees.  The window fills in this table of calls and the script layer
 * asks through it -- office42.selection, msgbox(), book.save().  Any
 * of them may be NULL, and a script that asks for one then gets an
 * error saying there is no window.  The book is passed so that the
 * window showing it can answer; `user` is whatever was given with the
 * table. */
typedef struct {
  gpointer user;

  /* The selection on the sheet on show for `book`, and the active
   * cell; FALSE if no window shows the book. */
  gboolean (*get_selection) (gpointer user, O42Book *book, O42Sheet **sheet,
                             O42Range *range, int *active_row, int *active_col);
  /* Selects a range and makes a cell of it active, showing the sheet. */
  void     (*set_selection) (gpointer user, O42Book *book, O42Sheet *sheet,
                             const O42Range *range, int active_row, int active_col);
  /* A message box, waited for; and a line asked of the user, NULL if
   * they cancelled (the caller frees it). */
  void     (*message)       (gpointer user, O42Book *book, const char *text);
  char    *(*input)         (gpointer user, O42Book *book, const char *prompt,
                             const char *initial);
  /* The status bar's text. */
  void     (*status)        (gpointer user, O42Book *book, const char *text);
  /* The file the book came from, or NULL (caller frees). */
  char    *(*path)          (gpointer user, O42Book *book);
  /* Saves the book: as it is, or to `path`.  FALSE with a message. */
  gboolean (*save)          (gpointer user, O42Book *book, const char *path, char **message);
  /* Opens a file in a window of its own. */
  gboolean (*open)          (gpointer user, const char *path, char **message);
  /* Closes the window showing the book, asking about unsaved work as
   * the close button would. */
  void     (*close)         (gpointer user, O42Book *book);
} O42PythonHost;

void o42_python_set_host (const O42PythonHost *host);

G_END_DECLS
