/* o42-book.h - a workbook: the sheets, in order, and what passes between them
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Excel 5 was the version that put several sheets in one file, with tabs
 * along the bottom, and =Sheet2!A1 to reach across.  A book owns its sheets
 * and is the go-between when one sheet's formula reads another's cell,
 * when a change on one sheet has to stale formulas on another, and when
 * rows inserted or a sheet renamed have to be reflected in every formula
 * that points there.
 */

#pragma once

#include "o42-sheet.h"

G_BEGIN_DECLS

O42Book  *o42_book_new  (void);          /* with one sheet, "Sheet1" */
void      o42_book_free (O42Book *book);

/* A book may be on show in several windows (Window > New Window).  Each
 * holds a reference; the last one to let go frees it. */
O42Book  *o42_book_ref   (O42Book *book);
void      o42_book_unref (O42Book *book);

/* Whoever shows the book asks to be told when another viewer changed
 * it: the sheets, their cells, or their names.  `what` is a hint. */
typedef void (*O42BookWatcher) (O42Book *book, const char *what, gpointer user);
void      o42_book_watch   (O42Book *book, O42BookWatcher watcher, gpointer user);
void      o42_book_unwatch (O42Book *book, O42BookWatcher watcher, gpointer user);
void      o42_book_changed (O42Book *book, const char *what);

int       o42_book_n_sheets    (O42Book *book);
O42Sheet *o42_book_sheet       (O42Book *book, int index);
O42Sheet *o42_book_find_sheet  (O42Book *book, const char *name);

/* The sheet holding the table of that name, or NULL: tables are named
 * across the book, as Excel names them. */
O42Sheet *o42_book_find_table  (O42Book *book, const char *name);
int       o42_book_sheet_index (O42Book *book, O42Sheet *sheet);

/* Adds a sheet before `index` (or at the end when index is -1), named
 * `name` or the first free "SheetN".  Removing the last sheet is refused.
 * Renaming rewrites every formula that pointed at the old name, and is
 * refused if the name is taken or empty. */
O42Sheet *o42_book_add_sheet    (O42Book *book, const char *name, int index);
gboolean  o42_book_remove_sheet (O42Book *book, int index);
gboolean  o42_book_rename_sheet (O42Book *book, int index, const char *name);

/* ---- Defined names ------------------------------------------------------ */

/* Insert > Name > Define: a name for a rectangle on a sheet, usable in
 * any formula in the book.  Names are compared without regard to case.
 * Defining rewrites nothing; formulas find the name when they are next
 * evaluated, and every formula using it is staled. */
gboolean     o42_book_define_name   (O42Book *book, const char *name,
                                     O42Sheet *sheet, const O42Range *range);
gboolean     o42_book_undefine_name (O42Book *book, const char *name);
gboolean     o42_book_lookup_name   (O42Book *book, const char *name,
                                     O42Sheet **sheet, O42Range *range);
GList       *o42_book_names         (O42Book *book);   /* sorted; free with g_list_free */

/* Back to one empty sheet called Sheet1 with no names: what a file
 * loader starts from. */
void      o42_book_clear (O42Book *book);

/* ---- Cell styles ---------------------------------------------------- */

/* A named set of format properties, as Excel's cell styles are: a cell
 * carries the style's name, so redefining the style restyles every
 * cell that wears it.  Direct formatting on top of a style stays; the
 * style's own properties are the ones its mask names. */
int          o42_book_n_styles     (O42Book *book);
const char  *o42_book_style_name   (O42Book *book, int index);
gboolean     o42_book_style        (O42Book *book, const char *name,
                                    O42Fmt *fmt, O42FmtMask *mask);
/* Defines or redefines a style and restyles the cells wearing it. */
void         o42_book_set_style    (O42Book *book, const char *name,
                                    const O42Fmt *fmt, O42FmtMask mask);
gboolean     o42_book_remove_style (O42Book *book, const char *name);

/* ---- Custom views ---------------------------------------------------- */

/* A named window state to come back to, as Excel's custom views are:
 * which sheet, what was selected on it, the zoom, and whether the
 * panes were frozen or split.  They are the book's, and travel in its
 * file. */
typedef struct {
  char    *name;          /* owned by the book */
  char    *sheet;         /* the sheet the view shows */
  O42Range selection;
  int      active_row, active_col;
  double   zoom;
  int      frozen_rows, frozen_cols;
  gboolean split;
} O42BookView;

int                o42_book_n_views     (O42Book *book);
const O42BookView *o42_book_view        (O42Book *book, int index);
const O42BookView *o42_book_find_view   (O42Book *book, const char *name);
/* Adds a view, or replaces the one of that name. */
void               o42_book_set_view    (O42Book *book, const O42BookView *view);
gboolean           o42_book_remove_view (O42Book *book, const char *name);

/* ---- Recording ------------------------------------------------------ */

/* Excel records a macro by writing down what you do; office42 writes
 * Python, the language it runs.  What is recorded is what the Python
 * API can put back: the text typed into cells and the formats applied
 * to them, wherever they came from -- so an insert of rows is recorded
 * as the cells it moved, which replays to the same sheet. */
void      o42_book_record_start (O42Book *book);
gboolean  o42_book_recording    (O42Book *book);
/* The script recorded so far, and an end to the recording.  The caller
 * owns the text. */
char     *o42_book_record_stop  (O42Book *book);

/* For the model to write a line into; nothing happens when the book is
 * not recording. */
void      o42_book_record_line  (O42Book *book, const char *line);
/* The sheet a recorded line is about, so that "sheet = book[...]" is
 * written when it changes.  Returns FALSE when nothing is recording. */
gboolean  o42_book_record_sheet (O42Book *book, const char *sheet_name);

/* Scripts kept in the book, and so in its file: Python source under a
 * name, as Excel keeps macros.  The book only stores them; running is
 * the script layer's business, and never happens on opening. */
int         o42_book_n_scripts     (O42Book *book);
const char *o42_book_script_name   (O42Book *book, int index);
const char *o42_book_script_code   (O42Book *book, const char *name);   /* NULL if none */
void        o42_book_set_script    (O42Book *book, const char *name, const char *code);
gboolean    o42_book_remove_script (O42Book *book, const char *name);

/* ---- The book's database --------------------------------------------- */

/* A book may have one SQLite database: a file beside it, or one carried
 * inside the book, which travels with it and is written back out when
 * the book is saved.  The book only remembers where it is; opening it
 * and asking it things is the io layer's business. */
void        o42_book_set_database (O42Book *book, const char *path, gboolean embedded);
const char *o42_book_database     (O42Book *book, gboolean *embedded);

/* ---- How the book calculates ------------------------------------------ */

/* Excel offers two things office42 now offers too.  The first is
 * iteration: a formula that depends on itself is an error unless you
 * say to go round the loop, at most `max` times or until nothing moves
 * by more than `tolerance`.  The second is manual calculation, where
 * nothing is worked out until it is asked for -- F9. */
/* ---- Custom lists ------------------------------------------------------ */

/* The runs the fill handle continues besides the days and the months:
 * a list of names in the order they go round.  Excel keeps these in
 * its options; office42 keeps them with the book, so that a book that
 * needs them carries them.  The array and its lists belong to the
 * book. */
GPtrArray *o42_book_custom_lists   (O42Book *book);            /* GStrv per entry */
void       o42_book_add_custom_list (O42Book *book, char **items);   /* takes them */
void       o42_book_remove_custom_list (O42Book *book, guint index);

/* The list `text` belongs to and where in it, or -1.  The lists are
 * searched in order, and a name in two of them belongs to the first. */
int        o42_book_custom_list_find (O42Book *book, const char *text, int *at);

void     o42_book_set_iteration (O42Book *book, gboolean on, int max, double tolerance);
gboolean o42_book_iteration     (O42Book *book, int *max, double *tolerance);
void     o42_book_set_manual    (O42Book *book, gboolean manual);
gboolean o42_book_manual        (O42Book *book);

/* TRUE if any sheet, or the scripts, changed since it was saved. */
gboolean  o42_book_is_modified  (O42Book *book);
void      o42_book_set_modified (O42Book *book, gboolean modified);

/* Undo's way of taking a sheet out of the book and putting it back
 * without freeing it: the sheet keeps its book pointer while detached.
 * Detaching the last sheet is refused. */
gboolean  o42_book_detach_sheet (O42Book *book, int index);
gboolean  o42_book_rename_sheet_unrecorded (O42Book *book, int index, const char *name);
void      o42_book_attach_sheet (O42Book *book, O42Sheet *sheet, int index);

/* ---- Called by sheets, not by anyone else -------------------------------- */

/* The one undo history every sheet of the book records into. */
O42UndoStack *o42_book_undo_stack (O42Book *book);

/* A cell on `sheet` changed: stale the formulas elsewhere that read it. */
void      o42_book_cell_changed  (O42Book *book, O42Sheet *sheet, int row, int col);

/* Rows (or columns) moved on `sheet`: rewrite the formulas elsewhere that
 * point into it. */
void      o42_book_sheet_shifted (O42Book *book, O42Sheet *sheet, gboolean rows,
                                  int at, int count);

G_END_DECLS
