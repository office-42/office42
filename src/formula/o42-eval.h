/* o42-eval.h - working out what a formula comes to
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The evaluator does not know what a sheet is.  It asks for cell values
 * through a callback, which is what lets the sheet own recalculation order
 * and cycle detection while the evaluator stays a pure function of the tree
 * and the values it is handed.
 */

#pragma once

#include "o42-formula.h"

G_BEGIN_DECLS

typedef struct _O42EvalContext O42EvalContext;

struct _O42EvalContext {
  /* Fills `out` with the value of a cell on the named sheet, or on the
   * formula's own sheet when `sheet` is NULL; a sheet that does not exist
   * gives #REF!.  The context owns nothing the evaluator passes in, and the
   * evaluator owns what comes back. */
  void     (*get_cell) (O42EvalContext *ctx, const char *sheet,
                        int row, int col, O42Value *out);
  gpointer   user_data;

  /* Resolves a defined name to a rectangle on a sheet (NULL for the
   * formula's own); FALSE if there is no such name.  May be NULL when
   * there are no names to be had. */
  gboolean (*get_name) (O42EvalContext *ctx, const char *name,
                        const char **sheet, O42Range *range);

  /* The cell whose formula is being evaluated, for ROW() and COLUMN()
   * without an argument. */
  int        row;
  int        col;

  /* Answers the questions CELL() asks that the formula engine cannot
   * see for itself: "format", "width", "protect", "prefix" and
   * "filename".  FALSE, or NULL altogether, when the caller has
   * nothing to say, and CELL returns #N/A. */
  gboolean (*get_cell_info) (O42EvalContext *ctx, const char *sheet,
                             int row, int col, const char *what, O42Value *out);

  /* The sheets from `first` to `last` in tab order, both included, for
   * a 3-D reference; interned names in an array the caller frees, and
   * how many.  Zero when either is missing.  May be NULL. */
  int      (*sheets_between) (O42EvalContext *ctx, const char *first,
                              const char *last, const char ***names);

  /* The rectangle that holds every stored cell on the named sheet (NULL
   * for the formula's own), so that =SUM(A:A) walks the rows that have
   * something in them and not the million the column has.  FALSE when
   * the sheet is empty or unknown.  May be NULL, and then a whole
   * column is walked whole. */
  gboolean (*get_extent) (O42EvalContext *ctx, const char *sheet, O42Range *used);
};

/* An argument is either a single value or a rectangle of them.  Keeping the
 * distinction is what lets SUM(A1:A9) see nine cells while SUM(A1) sees one,
 * without the caller having to say which it meant. */
typedef struct {
  gboolean    is_range;
  const char *sheet;    /* the range's sheet, NULL for the formula's own */
  const char *sheet_last;   /* the last sheet of a 3-D range, else NULL; a
                             * function sees such an operand as one range
                             * per sheet, spread out before it is called */
  O42Range    range;
  O42Value    value;    /* meaningful when is_range is FALSE */
  const void *lambda;   /* a LAMBDA(...) node, when the operand is one */
} O42Operand;

O42Value o42_eval (O42EvalContext *ctx, const O42Node *node);

/* Evaluates a formula for every cell of its result: what an array
 * formula (Ctrl+Shift+Enter) spreads over its range.  A scalar is one
 * by one.  `values` gets rows*cols values to clear and free. */
gboolean o42_eval_array (O42EvalContext *ctx, const O42Node *node,
                         int *rows, int *cols, O42Value **values);

/* TRUE if `name` is a function office42 knows, for telling an unknown name
 * from a misspelt one. */
gboolean o42_function_exists (const char *name);

/* Functions defined outside the evaluator -- by a script, say.  They
 * are found after the built-in table, take the same operands, and
 * show up in o42_function_names and o42_function_help like the
 * others.  `max_args` of -1 means any number.  Registering a name
 * again replaces the earlier definition. */
typedef O42Value (*O42ExternalFunction) (O42EvalContext *ctx, const char *name,
                                         O42Operand *args, int n_args, gpointer user);
void     o42_function_register_external   (const char *name, int min_args, int max_args,
                                           const char *signature, const char *summary,
                                           O42ExternalFunction impl, gpointer user);
gboolean o42_function_unregister_external (const char *name);
gboolean o42_function_is_external         (const char *name);

/* Reads a cell of an operand through the context: the value itself, or
 * the cell at (i, j) of a range.  For external functions, which do not
 * see the evaluator's insides. */
void     o42_operand_cell (O42EvalContext *ctx, const O42Operand *operand,
                           int i, int j, O42Value *out);
void     o42_operand_dims (const O42Operand *operand, int *rows, int *cols);

/* TRUE if Excel writes `name` with an _xlfn. prefix in its files (the
 * functions added after Excel 2007). */
gboolean o42_function_is_future (const char *name);

/* COUNTIF's criteria, for the AutoFilter: ">5", "<>Japan", "*land",
 * or a plain value to equal.  `is_condition` says whether a text is
 * one of the former rather than a value to match exactly. */
gboolean o42_criterion_matches      (const char *criterion, const O42Value *value);
gboolean o42_criterion_is_condition (const char *text);

/* Every function name office42 implements, sorted.  For documentation and
 * for the function list in the interface. */
const char * const *o42_function_names (guint *n_names);

/* The signature and a one-line summary of a function, for the Function
 * Wizard.  Static strings; FALSE if the name is not a function. */
gboolean o42_function_help (const char *name, const char **signature,
                            const char **summary);

G_END_DECLS
