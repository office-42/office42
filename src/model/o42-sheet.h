/* o42-sheet.h - the grid: sparse cells, recalculation, undo
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A sheet is nominally sixteen thousand rows by two hundred and fifty-six
 * columns, which is four million cells.  Almost all of them are empty, so
 * they are not stored: the sheet is a hash table from a packed address to a
 * cell, and an empty cell costs nothing at all.
 *
 * Recalculation is demand-driven.  A cell holds its last computed value and
 * a dirty flag; asking for a value evaluates it if the flag is set, pulling
 * on whatever it references and marking itself as in-progress so that a
 * cycle is caught rather than followed.  Changing a cell marks the formulas
 * that read it dirty, and theirs in turn.
 */

#pragma once

#include "o42-fmt.h"
#include "o42-formula.h"
#include "o42-picture.h"
#include "o42-chart.h"
#include "o42-shape.h"

G_BEGIN_DECLS

typedef struct _O42Sheet     O42Sheet;
typedef struct _O42Book      O42Book;
typedef struct _O42UndoStack O42UndoStack;

O42Sheet   *o42_sheet_new  (const char *name);
void        o42_sheet_free (O42Sheet *sheet);

const char *o42_sheet_get_name (O42Sheet *sheet);
void        o42_sheet_set_name (O42Sheet *sheet, const char *name);

/* The book a sheet belongs to, through which =Sheet2!A1 is resolved.  A
 * sheet with no book can still do everything within itself. */
void        o42_sheet_set_book (O42Sheet *sheet, O42Book *book);
O42Book    *o42_sheet_get_book (O42Sheet *sheet);

/* ---- Between sheets: called by the book ------------------------------ */

/* A cell on the sheet named `sheet_name` changed: stale every formula here
 * that reads it. */
void o42_sheet_invalidate_from (O42Sheet *sheet, const char *sheet_name,
                                int row, int col);

/* Rows (columns) were inserted or deleted on the sheet named `target`:
 * rewrite every formula here that points into it. */
void o42_sheet_shift_references (O42Sheet *sheet, const char *target,
                                 gboolean rows, int at, int count);

/* A sheet was renamed: rewrite every formula here that names it. */
void o42_sheet_rename_references (O42Sheet *sheet, const char *old_name,
                                  const char *new_name);

/* A defined name (upper case, interned) was defined, changed or removed:
 * every formula here that uses it is staled and its precedents redone. */
void o42_sheet_name_changed (O42Sheet *sheet, const char *upper);

O42FmtTable *o42_sheet_fmt_table (O42Sheet *sheet);

/* ---- Content ---------------------------------------------------------- */

/* Puts what the user typed into a cell.  Text beginning with "=" is parsed
 * as a formula; text that reads entirely as a number becomes one; everything
 * else is text.  Passing NULL or "" empties the cell. */
void  o42_sheet_set_input (O42Sheet *sheet, int row, int col, const char *text);

/* What the user typed, for editing: the formula source for a formula cell,
 * otherwise the value as text.  Caller frees. */
char *o42_sheet_get_input (O42Sheet *sheet, int row, int col);

/* The cell's value, recalculating if it is stale.  Caller clears. */
void  o42_sheet_get_value (O42Sheet *sheet, int row, int col, O42Value *out);

/* The text the cell shows, which is its value put through its format. */
char *o42_sheet_get_display (O42Sheet *sheet, int row, int col);

gboolean o42_sheet_has_formula (O42Sheet *sheet, int row, int col);
gboolean o42_sheet_is_empty    (O42Sheet *sheet, int row, int col);

void o42_sheet_clear_range   (O42Sheet *sheet, const O42Range *range);
void o42_sheet_clear_formats (O42Sheet *sheet, const O42Range *range);

/* ---- Moving cells about ----------------------------------------------- */

/* The cell's input as it would read copied `drow` rows down and `dcol`
 * columns across: relative references move with it, absolute ones stay.
 * Caller frees. */
char *o42_sheet_get_input_relocated (O42Sheet *sheet, int row, int col,
                                     int drow, int dcol);

/* Copies a rectangle -- content, formulas relocated, and formatting -- so
 * that its top-left corner lands on `row`,`col`.  One undo record. */
void o42_sheet_copy_range (O42Sheet *sheet, const O42Range *source,
                           int row, int col);

/* Paste Special: what to carry, and whether to turn the rectangle on its
 * side.  VALUES pastes what formulas came to, not the formulas. */
typedef enum {
  O42_PASTE_ALL,
  O42_PASTE_VALUES,
  O42_PASTE_FORMATS,
  O42_PASTE_FORMULAS
} O42PasteMode;

void o42_sheet_copy_range_special (O42Sheet *sheet, const O42Range *source,
                                   int row, int col, O42PasteMode mode,
                                   gboolean transpose);

/* Fill Down and Fill Right: the first row (or column) of the range is
 * copied into every other row (or column) of it. */
void o42_sheet_fill (O42Sheet *sheet, const O42Range *range, gboolean down);

/* What dragging the fill handle does: extends `source` to cover `target`
 * (which contains it and reaches past it in one direction) by continuing
 * whatever series the source holds.  Two or more numbers continue their
 * arithmetic progression; one number is copied; "Item1" becomes "Item2";
 * Jan and Monday cycle; dates step by the day; formulas are copied with
 * their references moved, as Fill Down does.  One undo step. */
void o42_sheet_autofill (O42Sheet *sheet, const O42Range *source,
                         const O42Range *target);

/* Excel's Ctrl+arrow: from `row`,`col`, the last cell of the run of filled
 * cells in a direction, or the next filled cell across a run of blanks, or
 * the edge of the sheet.  drow/dcol are -1, 0 or 1. */
void o42_sheet_edge (O42Sheet *sheet, int *row, int *col, int drow, int dcol);

/* Inserts `count` empty rows before row `at`, or deletes the `count` rows
 * starting there, moving everything below and adjusting every formula on
 * the sheet: a reference to a moved cell follows it, one to a deleted cell
 * becomes #REF!.  The column variants are the same across. */
void o42_sheet_insert_rows (O42Sheet *sheet, int at, int count);
void o42_sheet_delete_rows (O42Sheet *sheet, int at, int count);
void o42_sheet_insert_cols (O42Sheet *sheet, int at, int count);
void o42_sheet_delete_cols (O42Sheet *sheet, int at, int count);

/* Insert > Cells and Edit > Delete with "shift cells": the cells of
 * `range` and everything below them (down) or to their right (right)
 * move by the range's height (width); with `insert` FALSE the range's
 * cells are removed and what lay beyond moves in.  Formulas pointing at
 * the moved cells follow them.  One undo step. */
void o42_sheet_shift_cells (O42Sheet *sheet, const O42Range *range,
                            gboolean down, gboolean insert);

/* Moves a block of cells, with their formats, to another place: what
 * dragging a selection does.  The formulas inside travel unchanged and
 * the formulas elsewhere that pointed into the block follow it, as
 * Excel's cut and paste does.  One undo step. */
void o42_sheet_move_range (O42Sheet *sheet, const O42Range *from, int to_row, int to_col);

/* ---- Sort, find, replace --------------------------------------------- */

/* Reorders the rows of `range` by the values in column `key` (an absolute
 * column index inside the range), numbers before text before booleans and
 * blanks last whichever way round, keeping each row's cells together and
 * its formulas' relative references pointing at the same relative places.
 * With `has_header` the first row stays where it is.  One undo step. */
void o42_sheet_sort (O42Sheet *sheet, const O42Range *range, int key,
                     gboolean ascending, gboolean has_header);

/* The same by up to three keys, each with its own direction, as Excel
 * 5's Sort dialog offered: rows equal on the first key are ordered by
 * the second, and so on. */
void o42_sheet_sort_keys (O42Sheet *sheet, const O42Range *range,
                          const int *keys, const gboolean *ascending,
                          int n_keys, gboolean has_header);

/* ---- Printing ------------------------------------------------------ */

/* What File > Page Setup keeps per sheet: the print area (the used
 * range when there is none), a header and footer in Excel's notation
 * (&L, &C, &R start the left, centre and right parts; &P the page
 * number, &N the page count, &D the date, &T the time, &F the file
 * name, &A the sheet name; && an ampersand), whether gridlines and the
 * row and column headings print, and rows repeated at the top of every
 * page.  New sheets have Excel 5's "&A" and "Page &P". */
typedef struct {
  gboolean  has_area;
  O42Range  area;
  char     *header;
  char     *footer;
  gboolean  gridlines;
  gboolean  headings;
  int       title_rows;     /* rows 0..title_rows-1 repeat; 0 for none */
  int       scale;          /* per cent, 100 for life size */
  int       fit_wide;       /* fit the sheet into this many pages across,
                             * and `fit_tall` down; 0 for neither, and
                             * then `scale` is used */
  int       fit_tall;
  double    margin;         /* points around the page */
} O42PrintSetup;

const O42PrintSetup *o42_sheet_print_setup       (O42Sheet *sheet);
void                 o42_sheet_set_print_area    (O42Sheet *sheet, const O42Range *area);  /* NULL clears */
void                 o42_sheet_set_header_footer (O42Sheet *sheet, const char *header, const char *footer);
void                 o42_sheet_set_print_options (O42Sheet *sheet, gboolean gridlines,
                                                  gboolean headings, int title_rows);
/* Life size, a percentage, or fitted into so many pages across and
 * down (either may be zero for "as many as it takes"). */
void                 o42_sheet_set_print_scale   (O42Sheet *sheet, int scale, int fit_wide, int fit_tall);
void                 o42_sheet_set_print_margin  (O42Sheet *sheet, double points);

/* Manual page breaks: the printing starts a new page at this row (or
 * column).  Setting one again takes it away. */
void      o42_sheet_toggle_page_break (O42Sheet *sheet, gboolean rows, int at);
gboolean  o42_sheet_page_break        (O42Sheet *sheet, gboolean rows, int at);
GArray   *o42_sheet_page_breaks       (O42Sheet *sheet, gboolean rows);   /* int, sorted */

/* Data > Remove Duplicates: rows of `range` whose cells in the given
 * columns (absolute indices) show the same text as an earlier row's,
 * case-insensitively, are removed and the rows below them in the range
 * moved up, as Excel does.  Returns how many went.  One undo step. */
int  o42_sheet_remove_duplicates (O42Sheet *sheet, const O42Range *range,
                                  const int *cols, int n_cols, gboolean has_header);

/* Data > Subtotals: at each change in column `group_col` of `range`
 * (which should be sorted by it) a row is inserted with "<value> Total"
 * and =SUBTOTAL(function_num, ...) over the group in each of `sum_cols`,
 * a Grand Total row at the end, and the rows grouped in an outline.
 * function_num is SUBTOTAL's: 9 SUM, 1 AVERAGE, 2 COUNT, 4 MAX, 5 MIN,
 * 6 PRODUCT.  With `replace`, earlier subtotal rows go first.  Returns
 * the range with the rows added.  One undo step. */
O42Range o42_sheet_subtotal (O42Sheet *sheet, const O42Range *range, int group_col,
                             const int *sum_cols, int n_sum, int function_num,
                             gboolean has_header, gboolean replace);

/* Takes the subtotal rows out of `range` again: the rows whose cells
 * hold SUBTOTAL formulas and a "... Total" label.  Returns how many. */
int  o42_sheet_remove_subtotals (O42Sheet *sheet, const O42Range *range);

/* The next cell after `row`,`col` in reading order whose input or display
 * contains `needle`, wrapping round to the start; FALSE if there is none.
 * A search starts from just after the active cell, which is why the
 * starting cell itself is not a match. */
gboolean o42_sheet_find (O42Sheet *sheet, const char *needle,
                         gboolean match_case, gboolean whole_cell,
                         int *row, int *col);

/* Replaces every occurrence of `needle` in the inputs of the range (or of
 * the whole sheet when NULL) and returns how many cells changed.  One
 * undo step. */
int o42_sheet_replace (O42Sheet *sheet, const O42Range *range,
                       const char *needle, const char *replacement,
                       gboolean match_case);

/* ---- Formatting ------------------------------------------------------- */

O42FmtIdx      o42_sheet_get_fmt_idx (O42Sheet *sheet, int row, int col);
const O42Fmt  *o42_sheet_get_fmt     (O42Sheet *sheet, int row, int col);
void           o42_sheet_apply_fmt   (O42Sheet *sheet, const O42Range *range,
                                      O42FmtMask mask, const O42Fmt *value);

/* The book's cell styles, worn by a cell: applying one puts its
 * properties on the cells and remembers the name, so redefining the
 * style restyles them.  `name` NULL takes the style off. */
void           o42_sheet_apply_style (O42Sheet *sheet, const O42Range *range, const char *name);
const char    *o42_sheet_cell_style  (O42Sheet *sheet, int row, int col);   /* NULL for none */
void           o42_sheet_restyle     (O42Sheet *sheet, const char *name);   /* after the style changed */

/* ---- Geometry --------------------------------------------------------- */

/* Widths and heights are in pixels at the 96 dpi reference resolution the
 * grid lays out at, the same convention word42 uses. */
int  o42_sheet_col_width  (O42Sheet *sheet, int col);
void o42_sheet_set_col_width (O42Sheet *sheet, int col, int width);
/* Where a row or a column starts, in pixels from the top or the left
 * of the sheet, and which one is at an offset.  Both are answered from
 * the rows that differ from the default height -- there are few of
 * those and a million rows -- so neither walks the sheet. */
double o42_sheet_row_offset (O42Sheet *sheet, int row);
double o42_sheet_col_offset (O42Sheet *sheet, int col);
int    o42_sheet_row_at     (O42Sheet *sheet, double offset);
int    o42_sheet_col_at     (O42Sheet *sheet, double offset);

int  o42_sheet_row_height (O42Sheet *sheet, int row);
/* TRUE when a height was set for this row rather than inherited from
 * the default, which is what says whether it may be fitted. */
gboolean o42_sheet_row_height_set (O42Sheet *sheet, int row);
void o42_sheet_set_row_height (O42Sheet *sheet, int row, int height);

/* Hidden rows and columns keep their size and take no room.  Widths and
 * heights above report 0 for a hidden one; these say why. */
void     o42_sheet_set_row_hidden (O42Sheet *sheet, int row, gboolean hidden);
gboolean o42_sheet_row_hidden     (O42Sheet *sheet, int row);
gboolean o42_sheet_row_hidden_by_hand (O42Sheet *sheet, int row);   /* not by the filter */
void     o42_sheet_set_col_hidden (O42Sheet *sheet, int col, gboolean hidden);
gboolean o42_sheet_col_hidden     (O42Sheet *sheet, int col);

/* ---- Conditional formatting -------------------------------------------- */

/* A rule: when a cell's value in `range` stands in `op` to `value` (and
 * `value2` for between), the cell is shown with `fmt`'s `mask`ed fields
 * over its own format.  Gnumeric's operator numbering, so files agree. */
typedef enum {
  O42_COND_BETWEEN = 0,
  O42_COND_NOT_BETWEEN,
  O42_COND_EQUAL,
  O42_COND_NOT_EQUAL,
  O42_COND_GREATER,
  O42_COND_LESS,
  O42_COND_GREATER_EQUAL,
  O42_COND_LESS_EQUAL
} O42CondOp;

typedef struct {
  O42Range    range;
  O42CondOp   op;
  double      value;
  double      value2;
  O42FmtMask  mask;
  O42Fmt      fmt;
} O42Condition;

void       o42_sheet_add_condition    (O42Sheet *sheet, const O42Condition *cond);
void       o42_sheet_clear_conditions (O42Sheet *sheet, const O42Range *range);   /* those touching it */
GArray    *o42_sheet_conditions       (O42Sheet *sheet);   /* O42Condition, owned by the sheet */

/* The format a cell shows with its conditions applied, in `out`; FALSE
 * (and `out` untouched) when no rule applies. */
gboolean   o42_sheet_conditional_fmt  (O42Sheet *sheet, int row, int col, O42Fmt *out);

/* ---- Outline groups ------------------------------------------------------ */

/* Data > Group: rows (or columns) at an outline level from 1 to 7, so
 * that a run of rows at a level can be collapsed with the button the
 * grid shows against the row below the run.  Collapsing hides the
 * rows by hand; the level is what the file keeps. */
int  o42_sheet_row_level (O42Sheet *sheet, int row);
int  o42_sheet_col_level (O42Sheet *sheet, int col);
void o42_sheet_set_row_level (O42Sheet *sheet, int row, int level);
void o42_sheet_set_col_level (O42Sheet *sheet, int col, int level);
int  o42_sheet_max_row_level (O42Sheet *sheet);
int  o42_sheet_max_col_level (O42Sheet *sheet);

/* Group raises the level of every row (column) in lo..hi by one;
 * ungroup lowers it. */
void o42_sheet_group (O42Sheet *sheet, gboolean rows, int lo, int hi, gboolean group);

/* ---- Pivot tables -------------------------------------------------------- */

/* A pivot table: a source table with a header row, a field whose values
 * become the rows, one whose values become the columns (or none), a
 * field to aggregate and how.  The table is laid out as plain values
 * at `at` on the sheet that holds the definition, and laid out again
 * on refresh.  Fields are named by their headers. */
typedef enum { O42_PIVOT_SUM = 0, O42_PIVOT_COUNT, O42_PIVOT_AVERAGE, O42_PIVOT_MIN, O42_PIVOT_MAX } O42PivotAgg;

typedef struct {
  char        *source_sheet;   /* owned; NULL for the pivot's own sheet */
  O42Range     source;
  char       **row_fields;     /* owned NULL-terminated list, one or more */
  char       **col_fields;     /* owned; NULL or empty for none */
  char        *data_field;     /* owned; a header, or "=expression" over the
                                * headers (a calculated field: "=Sales-Costs") */
  O42PivotAgg  agg;
  char        *filter_field;   /* owned; a page filter: only the source rows
                                * whose cell in this field shows filter_value
                                * count.  NULL or "" for none */
  char        *filter_value;   /* owned */
  int          row, col;       /* where the table is laid out */
  int          rows, cols;     /* the extent of the last layout, for clearing */
} O42Pivot;

/* The fields of one axis as text, "Region|Year", and back: how the
 * files and office42-calc spell them. */
char  *o42_pivot_fields_to_string (char **fields);
char **o42_pivot_fields_from_string (const char *text);

/* Adds a pivot (copying the strings) and lays it out; `define` only
 * remembers one, for a file whose cells already hold the layout;
 * refresh lays every pivot on the sheet out again from its source; a
 * pivot at a cell is found for the dialog and for removal. */
void       o42_sheet_add_pivot     (O42Sheet *sheet, const O42Pivot *pivot);
void       o42_sheet_define_pivot  (O42Sheet *sheet, const O42Pivot *pivot);
void       o42_sheet_refresh_pivots (O42Sheet *sheet);
O42Pivot  *o42_sheet_pivot_at      (O42Sheet *sheet, int row, int col);
void       o42_sheet_remove_pivot  (O42Sheet *sheet, O42Pivot *pivot);
GArray    *o42_sheet_pivots        (O42Sheet *sheet);   /* O42Pivot, owned by the sheet */

/* ---- Rich text ----------------------------------------------------------- */

/* A cell whose text is set in more than one font: a list of runs, each
 * saying where in the text it starts (a byte offset) and how that part
 * looks.  The cell's own format is the ground the runs stand on -- a
 * run that only turns bold keeps the rest of the cell's font -- and
 * anything before the first run's start wears the cell's format alone.
 * Runs are dropped when the cell is typed into again, as Excel drops
 * them when the text is replaced. */
typedef struct {
  int    start;    /* byte offset into the cell's text */
  O42Fmt fmt;      /* the whole look of that part */
} O42TextRun;

void              o42_sheet_set_runs (O42Sheet *sheet, int row, int col,
                                      const O42TextRun *runs, int n_runs);
/* The cell's runs, or NULL when its text is all of a piece. */
const O42TextRun *o42_sheet_runs     (O42Sheet *sheet, int row, int col, int *n_runs);

/* Every formula on the sheet is stale and will be worked out again:
 * what a sheet arriving in or leaving the book does to the formulas
 * that name it. */
void o42_sheet_stale_formulas (O42Sheet *sheet);

/* Works out every formula on the sheet again, whatever the book's
 * calculation mode says.  What F9 does, and what the end of an
 * iteration does. */
void o42_sheet_recalculate (O42Sheet *sheet);

/* ---- Grouped objects ----------------------------------------------------- */

/* Pictures, shapes and charts anchored inside the range are put in a
 * group of their own: dragging one then moves them all, and deleting
 * one deletes them all, as Excel's grouping does.  Returns the group's
 * number, or 0 when there was nothing to group. */
guint    o42_sheet_group_objects   (O42Sheet *sheet, const O42Range *range);
/* Takes the objects anchored in the range out of their groups. */
gboolean o42_sheet_ungroup_objects (O42Sheet *sheet, const O42Range *range);
/* The group an object belongs to, 0 for none. */
guint    o42_sheet_object_group    (O42Sheet *sheet, guint id);

/* ---- Chart sheets -------------------------------------------------------- */

/* A chart sheet holds one chart and no cells: the chart fills the
 * window and prints one to a page, as Excel's does.  The chart is the
 * first one on the sheet; a sheet with no chart on it shows nothing. */
void      o42_sheet_set_chart_sheet (O42Sheet *sheet, gboolean chart_sheet);
gboolean  o42_sheet_is_chart_sheet  (O42Sheet *sheet);
O42Chart *o42_sheet_the_chart       (O42Sheet *sheet);   /* its one chart, or NULL */

/* ---- Protection ---------------------------------------------------------- */

/* A protected sheet refuses to be typed into where the cell's format
 * says it is locked, and does not show the formulas of cells marked
 * hidden.  It guards the window, not the file: a script or the
 * terminal front-end can still write, as they can in any spreadsheet
 * whose protection is not a password on the bytes. */
void     o42_sheet_set_protected  (O42Sheet *sheet, gboolean protect);
gboolean o42_sheet_protected      (O42Sheet *sheet);

/* The password a protected sheet asks for.  It is kept as the
 * sixteen-bit hash Excel invented for this and every spreadsheet since
 * has had to keep: it cannot be turned back into the password, and it
 * cannot be relied on either -- a hash that short collides, and
 * anything reading the file can take the protection off.  It is a
 * guard against a slip of the hand, not a secret, and the dialog says
 * so.  Zero means no password. */
void     o42_sheet_set_password   (O42Sheet *sheet, const char *password);
guint16  o42_sheet_password_hash  (O42Sheet *sheet);
void     o42_sheet_set_password_hash (O42Sheet *sheet, guint16 hash);
gboolean o42_sheet_password_matches (O42Sheet *sheet, const char *password);

/* The hash itself, for the file formats that store one. */
guint16  o42_password_hash (const char *password);
/* Whether a cell may be typed into, and whether its formula shows. */
gboolean o42_sheet_cell_editable  (O42Sheet *sheet, int row, int col);
gboolean o42_sheet_formula_hidden (O42Sheet *sheet, int row, int col);

/* ---- Solver -------------------------------------------------------------- */

/* Tools > Solver: make a target cell as large or as small as it goes,
 * or equal to a value, by changing a few other cells, while keeping
 * some cells within bounds.  Goal Seek does the one-variable case
 * exactly; this is a downhill simplex with the broken constraints
 * added to the objective as a penalty -- not the simplex method, and
 * not guaranteed to find the best answer, but it finds a good one for
 * the small problems a spreadsheet poses.  The whole search is one
 * undo step. */
typedef enum { O42_SOLVER_MAX = 0, O42_SOLVER_MIN, O42_SOLVER_VALUE } O42SolverGoal;
typedef enum { O42_SOLVER_LE = 0, O42_SOLVER_GE, O42_SOLVER_EQ } O42SolverOp;

typedef struct {
  int          row, col;   /* the cell that must stay in bounds */
  O42SolverOp  op;
  double       value;
} O42SolverBound;

gboolean o42_sheet_solve (O42Sheet *sheet, int target_row, int target_col,
                          O42SolverGoal goal, double goal_value,
                          const O42Ref *changing, int n_changing,
                          const O42SolverBound *bounds, int n_bounds,
                          double *reached);

/* ---- Advanced filter ----------------------------------------------------- */

/* Data > Filter > Advanced Filter: the rows of `list` (whose first row
 * names the fields) that answer `criteria` -- whose first row names
 * fields too, each row after it a set of conditions that must all
 * hold, any one row being enough.  A condition is what COUNTIF takes:
 * ">5", "<>Japan", "*land" or a value to equal.  With `dest_row` below
 * zero the other rows are hidden where they are; otherwise the header
 * and the answering rows are copied there.  `unique` drops repeats.
 * Returns how many rows answered.  One undo step. */
int o42_sheet_advanced_filter (O42Sheet *sheet, const O42Range *list, const O42Range *criteria,
                               int dest_row, int dest_col, gboolean unique);

/* ---- Consolidate --------------------------------------------------------- */

/* Data > Consolidate: several ranges, on this sheet or others, brought
 * together at `row`,`col` with one of the pivot's aggregates.  Without
 * labels the cells are matched by position; with them the top row
 * and/or the left column of each source names its columns and rows and
 * matching names are added together, so tables of different shapes
 * still line up.  One undo step; returns the rectangle written. */
O42Range o42_sheet_consolidate (O42Sheet *sheet, const O42SheetRange *sources, int n_sources,
                                int row, int col, O42PivotAgg agg,
                                gboolean labels_top, gboolean labels_left);

/* ---- Scenarios ----------------------------------------------------------- */

/* A named set of values for a few cells, as Excel's scenarios are:
 * showing one puts its values in those cells, so a sheet of formulas
 * can be looked at under several assumptions.  Adding one takes the
 * cells' present inputs. */
void        o42_sheet_add_scenario    (O42Sheet *sheet, const char *name,
                                       const O42Range *cells, const char *comment);
gboolean    o42_sheet_show_scenario   (O42Sheet *sheet, const char *name);
gboolean    o42_sheet_remove_scenario (O42Sheet *sheet, const char *name);
int         o42_sheet_n_scenarios     (O42Sheet *sheet);
const char *o42_sheet_scenario_name   (O42Sheet *sheet, int index);
/* What a scenario holds: the cells it sets and, through `values`, the
 * inputs it would put there.  Both arrays are the sheet's. */
gboolean    o42_sheet_scenario_cells  (O42Sheet *sheet, const char *name,
                                       GArray **keys, GPtrArray **values, const char **comment);
/* Puts a scenario together cell by cell, for the file readers. */
void        o42_sheet_define_scenario (O42Sheet *sheet, const char *name, const char *comment,
                                       int row, int col, const char *value);

/* ---- Tables -------------------------------------------------------------- */

/* A table is a named rectangle with a header row, as Excel's lists and
 * tables are: formulas name its parts with Table1[Sales],
 * Table1[#Headers], Table1[#Data], Table1[#All] and Table1[@Sales] for
 * the cell on the formula's own row.  It grows and shrinks with the
 * rows and columns inside it. */
typedef struct {
  char     *name;          /* owned */
  O42Range  range;         /* the header row included */
  gboolean  has_headers;
} O42Table;

void       o42_sheet_add_table    (O42Sheet *sheet, const char *name,
                                   const O42Range *range, gboolean has_headers);
gboolean   o42_sheet_remove_table (O42Sheet *sheet, const char *name);
O42Table  *o42_sheet_find_table   (O42Sheet *sheet, const char *name);
O42Table  *o42_sheet_table_at     (O42Sheet *sheet, int row, int col);
GArray    *o42_sheet_tables       (O42Sheet *sheet);   /* O42Table, owned */

/* The range a structured reference names, "Table1[Sales]" and its kin,
 * as seen from `row` (for [@...]).  FALSE if there is no such table or
 * column. */
gboolean   o42_sheet_table_range  (O42Sheet *sheet, const char *text, int row, O42Range *out);

/* ---- The tab's colour --------------------------------------------------- */

/* Excel colours a sheet tab; so does Gnumeric.  O42_TAB_NO_COLOUR is
 * the plain tab everything starts with. */
#define O42_TAB_NO_COLOUR 0xFFFFFFFFu
void    o42_sheet_set_tab_colour (O42Sheet *sheet, guint32 colour);
guint32 o42_sheet_tab_colour     (O42Sheet *sheet);

/* ---- Auditing --------------------------------------------------------- */

/* What a cell's formula reads, and which cells read it, both as
 * rectangles on this sheet: what Excel's Trace Precedents and Trace
 * Dependents draw arrows for.  Each array is the caller's to free. */
GArray *o42_sheet_precedents (O42Sheet *sheet, int row, int col);
GArray *o42_sheet_dependents (O42Sheet *sheet, int row, int col);

/* ---- AutoFormat -------------------------------------------------------- */

/* Ready-made looks for a table whose first row is its heading. */
int         o42_auto_format_count (void);
const char *o42_auto_format_name  (int which);
gboolean    o42_auto_format_parse (const char *name, int *which);
void        o42_sheet_auto_format (O42Sheet *sheet, const O42Range *range, int which);

/* ---- What-If tables ---------------------------------------------------- */

/* Excel's Data > Table.  The rectangle's edges hold what an input may
 * be and its corner the formula -- or, with one variable, the top row
 * or left column holds the formulas.  Each value is put into the input
 * cell named, everything is worked out, and the answer is written into
 * the inside of the rectangle.  Pass -1 for an input that is not used;
 * FALSE if neither is given or the rectangle is too small. */
gboolean o42_sheet_data_table (O42Sheet *sheet, const O42Range *range,
                               int row_input_row, int row_input_col,
                               int col_input_row, int col_input_col);

/* ---- Database queries --------------------------------------------------- */

/* A query put into the sheet: the SQL, and where its answer was laid
 * out.  The sheet remembers it so that Data > Refresh can run it again
 * and lay the answer out afresh; running it is the io layer's business,
 * since the model knows nothing of SQLite. */
typedef struct {
  char     *sql;        /* owned */
  O42Range  at;         /* the rectangle the last answer filled */
  gboolean  headings;   /* were the column names written above it */
} O42Query;

/* Adds a query, or replaces the one anchored at the same cell. */
void     o42_sheet_add_query    (O42Sheet *sheet, const char *sql,
                                 const O42Range *at, gboolean headings);
gboolean o42_sheet_remove_query (O42Sheet *sheet, int row, int col);
GArray  *o42_sheet_queries      (O42Sheet *sheet);   /* O42Query, owned */

/* ---- Array formulas ----------------------------------------------------- */

/* One formula over a block of cells, entered with Ctrl+Shift+Enter: the
 * top-left cell holds it and the block shows its result cell by cell,
 * as {=TRANSPOSE(A1:C1)} does.  Setting any cell of the block by other
 * means dissolves it, leaving the last values behind. */
void     o42_sheet_set_array_formula (O42Sheet *sheet, const O42Range *range, const char *text);
gboolean o42_sheet_array_range (O42Sheet *sheet, int row, int col, O42Range *out);

/* A plain formula whose result is an array spills into the empty cells
 * beside and below it, as Excel's dynamic arrays do, and shows #SPILL!
 * when they are not empty.  Such a block is found by
 * o42_sheet_array_range like the others; this tells them apart. */
gboolean o42_sheet_array_is_dynamic (O42Sheet *sheet, int row, int col);

/* ---- Data > Validation --------------------------------------------------- */

/* What a cell in `range` may be given: a whole number, a decimal, a
 * date, a time or a text of some length standing in `op` to `value`
 * (and `value2` for between), or one of a list -- `value` holding the
 * entries comma-separated, or a range whose cells hold them.  The
 * numbering is Gnumeric's ValidationType, so files agree. */
typedef enum {
  O42_VALID_ANY = 0,
  O42_VALID_WHOLE,
  O42_VALID_DECIMAL,
  O42_VALID_LIST,
  O42_VALID_DATE,
  O42_VALID_TIME,
  O42_VALID_LENGTH
} O42ValidKind;

typedef struct {
  O42Range      range;
  O42ValidKind  kind;
  O42CondOp     op;
  char         *value;        /* owned by the sheet once added */
  char         *value2;
  char         *message;      /* shown when an entry is refused; may be NULL */
  gboolean      allow_blank;
} O42Validation;

void       o42_sheet_add_validation    (O42Sheet *sheet, const O42Validation *v);   /* copies */
void       o42_sheet_clear_validations (O42Sheet *sheet, const O42Range *range);   /* those touching it; NULL for all */
GArray    *o42_sheet_validations       (O42Sheet *sheet);   /* O42Validation, owned by the sheet */

/* Whether `input` may go into the cell under the sheet's rules.  When
 * not, `message` (if given) receives the rule's message, to free. */
gboolean   o42_sheet_validate (O42Sheet *sheet, int row, int col,
                               const char *input, char **message);

/* ---- Text to Columns --------------------------------------------------- */

/* Splits the text in each cell of the range's first column at `delimiter`
 * (a comma, a tab, a space...) into the columns to the right, each piece
 * going through the same number-or-text decision a typed cell gets.
 * Returns how many rows changed.  One undo step. */
int o42_sheet_text_to_columns (O42Sheet *sheet, const O42Range *range,
                               const char *delimiter);

/* ---- View state -------------------------------------------------------- */

/* How many rows and columns are frozen at the top and left: a view
 * setting, kept on the sheet so a file can carry it. */
void o42_sheet_set_frozen (O42Sheet *sheet, int rows, int cols);
void o42_sheet_get_frozen (O42Sheet *sheet, int *rows, int *cols);

/* ---- Goal Seek --------------------------------------------------------- */

/* Tools > Goal Seek: finds the number to put in `var` that makes the
 * formula in `target` come to `goal`, by the secant method from the
 * cell's present value with a bisection fallback.  Leaves the value in
 * place as one undo step and returns TRUE; on failure puts the original
 * back and returns FALSE.  `found` gets the value of `target` reached. */
gboolean o42_sheet_goal_seek (O42Sheet *sheet, int target_row, int target_col,
                              double goal, int var_row, int var_col, double *found);

/* ---- Notes ------------------------------------------------------------- */

/* A note on a cell: Excel 5's cell note, shown as a small mark in the
 * corner and read on hover.  NULL or "" removes it. */
void        o42_sheet_set_note (O42Sheet *sheet, int row, int col, const char *text);
const char *o42_sheet_get_note (O42Sheet *sheet, int row, int col);   /* NULL if none */
GHashTable *o42_sheet_notes    (O42Sheet *sheet);   /* guint64 key -> char*, owned */

/* Hyperlinks: a URL ("https://...", "mailto:...") or, starting with
 * '#', a place in the book ("#Sheet2!B4").  Shown blue and underlined,
 * followed with Ctrl+click; they move with their rows and undo. */
void        o42_sheet_set_link (O42Sheet *sheet, int row, int col, const char *target);  /* NULL or "" removes */
const char *o42_sheet_get_link (O42Sheet *sheet, int row, int col);   /* NULL if none */
GHashTable *o42_sheet_links    (O42Sheet *sheet);   /* guint64 key -> char*, owned */

/* ---- Merged cells ------------------------------------------------------ */

/* A merged range shows as one cell: the top-left cell's content, spread
 * over the rectangle; the others are hidden.  Merging keeps the top-left
 * cell's content and clears the rest, as Excel does. */
void        o42_sheet_merge   (O42Sheet *sheet, const O42Range *range);
void        o42_sheet_unmerge (O42Sheet *sheet, const O42Range *range);

/* The merged range a cell belongs to, or FALSE. */
gboolean    o42_sheet_merged_at (O42Sheet *sheet, int row, int col, O42Range *out);
GArray     *o42_sheet_merges    (O42Sheet *sheet);   /* O42Range, owned by the sheet */

/* ---- AutoFilter ------------------------------------------------------- */

/* Excel 5's AutoFilter: a range whose first row is headings, each with a
 * dropdown of the values below it; choosing one hides every row whose
 * cell is different.  One choice per column; NULL means "(All)". */
void        o42_sheet_set_autofilter   (O42Sheet *sheet, const O42Range *range);
gboolean    o42_sheet_get_autofilter   (O42Sheet *sheet, O42Range *range);
void        o42_sheet_clear_autofilter (O42Sheet *sheet);
void        o42_sheet_autofilter_choose (O42Sheet *sheet, int col, const char *value);
const char *o42_sheet_autofilter_choice (O42Sheet *sheet, int col);
void        o42_sheet_autofilter_refresh (O42Sheet *sheet);   /* after the cells changed under it */

/* The distinct values shown in a filter column, sorted, for its dropdown.
 * Caller frees the strv. */
char      **o42_sheet_autofilter_values (O42Sheet *sheet, int col);

/* The smallest rectangle holding everything the sheet contains.  Empty
 * sheets give back a zero-sized range at A1. */
void o42_sheet_used_range (O42Sheet *sheet, O42Range *out);

/* The value of a formula ("=SUM(A1:A9)" or without the '=') on this
 * sheet, as if it stood in A1, without putting it anywhere.  Clear the
 * result.  #NAME? for a formula that does not parse. */
O42Value o42_sheet_evaluate_formula (O42Sheet *sheet, const char *text);

/* Every stored cell -- one with content or a format -- in no particular
 * order.  The sheet is sparse, and this is how a writer visits what is
 * there without walking every cell of the used rectangle. */
typedef void (*O42CellFunc) (O42Sheet *sheet, int row, int col, gpointer user);
void o42_sheet_foreach_cell (O42Sheet *sheet, O42CellFunc func, gpointer user);

/* ---- Modified ---------------------------------------------------------- */

/* Set by every change, cleared by whoever saves the sheet. */
gboolean o42_sheet_is_modified  (O42Sheet *sheet);
void     o42_sheet_set_modified (O42Sheet *sheet, gboolean modified);

/* ---- Pictures --------------------------------------------------------- */

/* Pictures float over the grid, anchored to a cell.  They are not part of
 * the undo history yet; the roadmap says so. */
O42Picture *o42_sheet_add_picture    (O42Sheet *sheet, GBytes *data,
                                      const char *format, int pixel_w,
                                      int pixel_h, int row, int col);
void        o42_sheet_remove_picture (O42Sheet *sheet, guint id);
O42Picture *o42_sheet_find_picture   (O42Sheet *sheet, guint id);
GPtrArray  *o42_sheet_pictures       (O42Sheet *sheet);   /* owned by the sheet */

/* ---- Shapes ------------------------------------------------------------ */

/* Rectangles, ovals, lines, arrows and text boxes over the grid,
 * anchored and moved like pictures, and in the undo history. */
O42Shape   *o42_sheet_add_shape    (O42Sheet *sheet, O42ShapeKind kind, int row, int col);
void        o42_sheet_remove_shape (O42Sheet *sheet, guint id);
O42Shape   *o42_sheet_find_shape   (O42Sheet *sheet, guint id);
GPtrArray  *o42_sheet_shapes       (O42Sheet *sheet);   /* owned by the sheet */

/* ---- Form controls ----------------------------------------------------- */

/* A form control -- a button, a check box, a spinner and the rest -- is
 * a shape with a linked cell.  It keeps no state of its own: what it
 * shows is what the cell says, and clicking it writes the cell. */

/* What the linked cell says, as a number (TRUE counts as one).  FALSE
 * if there is no link or the cell holds no number. */
gboolean o42_sheet_control_value (O42Sheet *sheet, const O42Shape *shape, double *value);

/* The items of a list box or a combo box, read out of its source
 * range.  NULL if it has none; free with g_strfreev. */
char   **o42_sheet_control_items (O42Sheet *sheet, const O42Shape *shape);

/* Writes the linked cell, as one undoable step. */
void     o42_sheet_control_set   (O42Sheet *sheet, const O42Shape *shape, double value);

/* Works the control at a point inside its box: toggles a check box,
 * chooses an option or a list row, steps a spinner or a scroll bar.
 * TRUE if the sheet changed.  A button changes nothing -- the window
 * runs its script. */
gboolean o42_sheet_control_click (O42Sheet *sheet, const O42Shape *shape,
                                  double x, double y, double width, double height);

/* Draws any shape, a control included, with what its cell says. */
void     o42_sheet_draw_shape    (O42Sheet *sheet, const O42Shape *shape,
                                  cairo_t *cr, double width, double height);

/* ---- Charts ------------------------------------------------------------ */

/* Charts float like pictures and, like them, are not in the undo history
 * yet.  The sheet owns them. */
O42Chart   *o42_sheet_add_chart    (O42Sheet *sheet, O42ChartKind kind,
                                    const O42Range *data, int row, int col);
void        o42_sheet_remove_chart (O42Sheet *sheet, guint id);
O42Chart   *o42_sheet_find_chart   (O42Sheet *sheet, guint id);
GPtrArray  *o42_sheet_charts       (O42Sheet *sheet);   /* owned by the sheet */

/* Paints a chart from the sheet's current values. */
void        o42_sheet_draw_chart   (O42Sheet *sheet, const O42Chart *chart,
                                    cairo_t *cr, double width, double height);

/* Before moving or resizing a picture or chart by hand, inside a
 * begin/end group, so the drag is one undo step. */
void        o42_sheet_capture_object (O42Sheet *sheet, guint id);

/* Called by the book, inside a group: remembers the sheet's name, or
 * whether and where it is in the book, before that changes. */
void        o42_sheet_undo_capture_sheet (O42Sheet *sheet, gboolean name);

/* ---- Undo ------------------------------------------------------------- */

/* A sheet in a book shares the book's undo history, so one step can
 * touch several sheets -- inserting rows on Sheet1 rewrites Sheet2's
 * references to it, and undoing puts both back.  A sheet on its own has
 * a history of its own. */
O42UndoStack *o42_undo_stack_new  (void);
void          o42_undo_stack_free (O42UndoStack *stack);

void     o42_sheet_begin_group (O42Sheet *sheet);
void     o42_sheet_end_group   (O42Sheet *sheet);
gboolean o42_sheet_can_undo    (O42Sheet *sheet);
gboolean o42_sheet_can_redo    (O42Sheet *sheet);
gboolean o42_sheet_undo        (O42Sheet *sheet, O42Range *touched);
gboolean o42_sheet_redo        (O42Sheet *sheet, O42Range *touched);

/* The same, saying which sheet the step's first change was on, which is
 * the one to show. */
gboolean o42_sheet_undo_full   (O42Sheet *sheet, O42Sheet **target, O42Range *touched);
gboolean o42_sheet_redo_full   (O42Sheet *sheet, O42Sheet **target, O42Range *touched);

/* Forgets the whole history, the book's when the sheet is in one. */
void     o42_sheet_clear_undo  (O42Sheet *sheet);

G_END_DECLS
