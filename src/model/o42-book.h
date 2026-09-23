/* o42-book.h - a workbook: the sheets, in order, and what passes between them
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Excel 97's book is several sheets in one file, with tabs
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
int       o42_book_ref_count (O42Book *book);

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
/* A name for a formula rather than a rectangle: a constant (=0.25), an
 * expression (=SUM(Sheet1!A1:A3)*2), or a LAMBDA, which a formula then
 * calls by the name.  `formula` may begin with "=".  Such a name has
 * no rectangle, so o42_book_lookup_name says FALSE for it and
 * o42_book_lookup_name_formula gives the text. */
gboolean     o42_book_define_name_formula (O42Book *book, const char *name, const char *formula);
const char  *o42_book_lookup_name_formula (O42Book *book, const char *name);
gboolean     o42_book_undefine_name (O42Book *book, const char *name);

/* Insert > Name > Create: names made from the labels along the edges
 * of a range -- its top row and (or) left column, its bottom row and
 * (or) right column -- each naming the cells of its column (row) inside
 * the range.  A label is made a legal name as Excel makes it, spaces
 * to underscores and a leading digit given one; an empty label makes no
 * name.  How many names were made. */
int          o42_book_create_names  (O42Book *book, O42Sheet *sheet, const O42Range *range,
                                     gboolean top, gboolean left, gboolean bottom, gboolean right);
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

/* ---- AutoCorrect ------------------------------------------------------- */

/* Tools > AutoCorrect: what Excel 97 does to text as it is typed.
 * Two initial capitals are made one, the letter after a full stop is
 * made a capital, a day's name typed in lower case is given its
 * capital, and words in the replacement list are replaced -- (c) by
 * the copyright sign, teh by the.  Excel keeps the list in its
 * options; office42 keeps it with the book, so a book that needs it
 * carries it.  A new book starts with a short list. */
typedef enum {
  O42_AUTOCORRECT_INITIALS,
  O42_AUTOCORRECT_SENTENCES,
  O42_AUTOCORRECT_DAYS,
  O42_AUTOCORRECT_REPLACE,
  O42_N_AUTOCORRECT_OPTIONS
} O42AutocorrectOption;

gboolean    o42_book_autocorrect_option     (O42Book *book, O42AutocorrectOption which);
void        o42_book_set_autocorrect_option (O42Book *book, O42AutocorrectOption which, gboolean on);
const char *o42_autocorrect_option_name     (O42AutocorrectOption which);   /* "initials", "sentences", "days", "replace" */
gboolean    o42_autocorrect_option_parse    (const char *name, O42AutocorrectOption *which);

int         o42_book_n_autocorrections   (O42Book *book);
const char *o42_book_autocorrection      (O42Book *book, int index, const char **to);
/* Adds one, or changes what `from` is replaced with. */
void        o42_book_add_autocorrection  (O42Book *book, const char *from, const char *to);
gboolean    o42_book_remove_autocorrection (O42Book *book, const char *from);
/* Forgets the list, for a file that carries its own. */
void        o42_book_clear_autocorrections (O42Book *book);

/* `text` as typed, corrected, or NULL when nothing in it changes.  A
 * formula or a quoted text is never touched.  Caller frees. */
char       *o42_book_autocorrect (O42Book *book, const char *text);

/* ---- Protecting the structure ---------------------------------------- */

/* Tools > Protection > Protect Workbook: no sheet may be added,
 * deleted, renamed, moved, copied, hidden or unhidden while it is on.
 * The password is kept as Excel's short hash, as a sheet's is, and
 * guards the window: a script or a file loader may still do all of it.
 * The books' files carry it. */
void     o42_book_set_protected     (O42Book *book, gboolean on);
gboolean o42_book_protected         (O42Book *book);
void     o42_book_set_password_hash (O42Book *book, guint16 hash);
guint16  o42_book_password_hash     (O42Book *book);

/* ---- Document properties --------------------------------------------- */

/* File > Properties: what the Summary tab of Excel's holds.  They go
 * into the file -- an .xlsx's docProps, an .ods's meta.xml and a
 * .gnumeric's office:document-meta -- and come back from it. */
typedef enum {
  O42_PROP_TITLE,
  O42_PROP_SUBJECT,
  O42_PROP_AUTHOR,
  O42_PROP_MANAGER,
  O42_PROP_COMPANY,
  O42_PROP_CATEGORY,
  O42_PROP_KEYWORDS,
  O42_PROP_COMMENTS,
  O42_N_PROPS
} O42Property;

const char *o42_book_property     (O42Book *book, O42Property which);   /* "" for none */
void        o42_book_set_property (O42Book *book, O42Property which, const char *value);

/* The names the Python API and office42-calc use: "title", "subject",
 * "author", "manager", "company", "category", "keywords", "comments". */
const char *o42_property_name  (O42Property which);
gboolean    o42_property_parse (const char *name, O42Property *which);

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

/* ---- Watches ----------------------------------------------------------- */

/* The cells Excel's Watch Window keeps an eye on: a sheet's name and a
 * cell on it.  The book's, and in its file. */
typedef struct {
  char *sheet;            /* owned by the book */
  int   row, col;
} O42Watch;

int             o42_book_n_watches    (O42Book *book);
const O42Watch *o42_book_watch_at     (O42Book *book, int index);
/* Adds one, unless it is there already; TRUE if it was added. */
gboolean        o42_book_add_watch    (O42Book *book, const char *sheet, int row, int col);
gboolean        o42_book_remove_watch (O42Book *book, int index);

/* ---- Recording ------------------------------------------------------ */

/* Excel records a macro by writing down what you do; office42 writes
 * Python, the language it runs.  What is recorded is what the Python
 * API can put back: the text typed into cells, the formats applied to
 * them, and the operations on whole ranges, rows, columns and sheets
 * -- an insert of rows is one line, sheet.insert_rows(at, count), and
 * the cells it moves are not written down one by one. */
void      o42_book_record_start (O42Book *book);
/* A file is being read in between these two.  While it is, the sheets
 * keep no undo, tell no formula that a cell it reads has changed --
 * every formula read in is already waiting to be worked out, and the
 * cells it reads arrive in any order -- and put off spilling a formula
 * over its block until the end, when everything it reads is there.  A
 * book of a million cells reads in seconds this way rather than
 * minutes.  The end tidies every sheet: see o42_sheet_finish_load. */
void      o42_book_begin_load   (O42Book *book);
void      o42_book_end_load     (O42Book *book);
gboolean  o42_book_loading      (const O42Book *book);

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

/* An operation the API has one call for: the line is written (when
 * recording) and everything the model does until the matching end --
 * the cells an insert moves, the formats a paste carries -- is not.
 * The pair nests, and is balanced whether recording or not. */
void      o42_book_record_op_begin (O42Book *book, const char *sheet_name, const char *line);
void      o42_book_record_op_end   (O42Book *book);

/* The window's selection changed while recording: written down as
 * sheet["B2:C5"].select() -- but only when something is then done, so
 * that wandering about the sheet does not fill the macro. */
void      o42_book_record_selection (O42Book *book, const char *sheet_name,
                                     const O42Range *range, int active_row, int active_col);

/* Excel's "Relative References": cells are written down relative to
 * the active cell -- office42.active_cell.offset(1, 0) -- so that the
 * macro replays wherever it is run, rather than at the cells it was
 * recorded on.  The base is the active cell now; each recorded
 * selection moves it.  The setting outlives a recording. */
void      o42_book_record_set_relative (O42Book *book, gboolean relative, int row, int col);
gboolean  o42_book_record_relative     (O42Book *book);

/* sheet["A1:B2"] or, recording relatively, office42.active_cell.offset
 * (r, c).resize(n, m); caller frees. */
char     *o42_book_record_range_text   (O42Book *book, const O42Range *range);

/* Moves the sheet at `from` so that it sits at `to`; one undo step. */
gboolean  o42_book_move_sheet (O42Book *book, int from, int to);

/* Edit > Fill > Across Worksheets: the cells of `range` on `source`
 * copied to the same cells on each of the `n` `targets` -- everything,
 * the contents only or the formats only, as Excel's dialog offers.  A
 * cell empty on the source empties its twin.  One undo step. */
void      o42_book_fill_across (O42Book *book, O42Sheet *source, const O42Range *range,
                                O42Sheet **targets, int n, O42PasteMode mode);

/* Edit > Move or Copy Sheet with "Create a copy": a copy of the sheet
 * at `from`, put at `to` (or at the end when `to` is -1), named `name`
 * or, as Excel names it, "Sheet1 (2)".  Formulas on the copy that named
 * the original by name now name the copy, and a table on it is renamed
 * so that the book still has one of each name.  One undo step. */
O42Sheet *o42_book_copy_sheet (O42Book *book, int from, int to, const char *name);

/* Scripts kept in the book, and so in its file: Python source under a
 * name, as Excel keeps macros.  The book only stores them; running is
 * the script layer's business, and never happens on opening. */
int         o42_book_n_scripts     (O42Book *book);

/* Parts of an Excel file that office42 does not read but keeps for
 * Excel: a Visual Basic project (xl/vbaProject.bin and its
 * neighbours) from an .xlsm.  Names are the zip's; the bytes are held
 * as they came and written back as they were. */
void        o42_book_keep_part     (O42Book *book, const char *name, GBytes *bytes);   /* NULL forgets */
GBytes     *o42_book_kept_part     (O42Book *book, const char *name);
GList      *o42_book_kept_parts    (O42Book *book);    /* the names; free the list, not them */
gboolean    o42_book_has_vba       (O42Book *book);    /* an xl/vbaProject.bin is kept */

/* Whether the book's Python may run: a script in it, or =PY() in a
 * cell.  A new book's may; a book read from a file may not until the
 * user runs its scripts, as Excel's "Enable content" has it, so that a
 * file from elsewhere cannot run code by being opened. */
gboolean    o42_book_scripts_trusted     (O42Book *book);
void        o42_book_set_scripts_trusted (O42Book *book, gboolean trusted);
const char *o42_book_script_name   (O42Book *book, int index);
const char *o42_book_script_code   (O42Book *book, const char *name);   /* NULL if none */
void        o42_book_set_script    (O42Book *book, const char *name, const char *code);
gboolean    o42_book_remove_script (O42Book *book, const char *name);

/* What Excel's Macro Options keeps: a shortcut key, Ctrl+Shift and a
 * letter ('\0' for none), and a line about the macro.  Both are saved
 * with the script. */
void        o42_book_set_script_options (O42Book *book, const char *name,
                                         char shortcut, const char *description);
char        o42_book_script_shortcut    (O42Book *book, const char *name);
const char *o42_book_script_description (O42Book *book, const char *name);   /* "" if none */
/* The script bound to Ctrl+Shift+letter, or NULL. */
const char *o42_book_script_for_shortcut (O42Book *book, char shortcut);

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

/* The 1904 date system (see o42_date_set_1904): a book that counts
 * its days from 1 January 1904, as one from Excel for the Macintosh
 * does.  Setting it makes it the system in use, and every sheet is
 * worked out again. */
void     o42_book_set_date_1904 (O42Book *book, gboolean on);
gboolean o42_book_date_1904     (O42Book *book);

/* Precision as displayed: a number in a cell with a fixed number of
 * decimals is kept rounded to them, as Excel's option has it, so that
 * a column of shown values adds up to the shown total.  Turning it on
 * rounds what is there; there is no getting the digits back. */
void     o42_book_set_precision_as_displayed (O42Book *book, gboolean on);
gboolean o42_book_precision_as_displayed     (O42Book *book);

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
