/* o42-eval-private.h - what the function families share
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The evaluator is one machine and several hundred functions.  The
 * machine -- operands, array constants, the tree walk -- lives in
 * o42-eval.c; the functions live in a file per family beside it.  This
 * header is the seam: the handful of helpers a family needs, the
 * macros that read its arguments, and the shape of the tables it hands
 * back.
 *
 * Nothing here is for anyone outside src/formula.
 */

#pragma once

#include "o42-eval.h"

G_BEGIN_DECLS

/* ---- The tables a family exports --------------------------------------- */

/* One function: how many arguments it takes at least, how many at most
 * (-1 for as many as you like), and what it does. */
typedef struct {
  const char *name;
  int         min_args;
  int         max_args;
  O42Value  (*fn) (O42EvalContext *ctx, O42Operand *args, int n_args);
} O42Function;

/* What the Function Wizard says about one. */
typedef struct {
  const char *name;
  const char *signature;
  const char *summary;
} O42FunctionHelp;

/* Each family's tables, both ended by an entry whose name is NULL.
 * o42-eval.c gathers them, sorts them and searches the lot. */
extern const O42Function     O42_FUNCS_FINANCE[];
extern const O42Function     O42_FUNCS_INFO[];
extern const O42Function     O42_FUNCS_BESSEL[];
extern const O42Function     O42_FUNCS_RANDOM[];
extern const O42Function     O42_FUNCS_OPTIONS[];
extern const O42FunctionHelp O42_HELP_FINANCE[];
extern const O42FunctionHelp O42_HELP_INFO[];
extern const O42FunctionHelp O42_HELP_BESSEL[];
extern const O42FunctionHelp O42_HELP_RANDOM[];
extern const O42FunctionHelp O42_HELP_OPTIONS[];

/* ---- The machine, for the families to lean on -------------------------- */

/* An operand as a single value: a range gives the cell that lines up
 * with the formula, which is Excel's implicit intersection. */
O42Value o42_operand_value (O42EvalContext *ctx, const O42Operand *operand);

/* Every number in the arguments, in order, or the error that stopped
 * the walk: where the statistical functions all start.  The array is
 * the caller's to free. */
gboolean o42_collect_numbers (O42EvalContext *ctx, O42Operand *args, int n,
                              GArray **out, O42ErrorCode *error);

/* The normal distribution, which half the functions here are built on. */
double o42_normal_cdf (double z);
double o42_normal_pdf (double x);

/* ---- Reading arguments -------------------------------------------------- */

/* Both of these return from the calling function with the error when
 * an argument will not become what is wanted, which is what every one
 * of the functions wants to do. */
#define ARG_NUMBER(index, target)                                        \
  G_STMT_START {                                                         \
    O42Value v_ = o42_operand_value (ctx, &args[index]);                 \
    O42ErrorCode e_ = O42_ERR_VALUE;                                     \
    gboolean ok_ = o42_value_to_number (&v_, &(target), &e_);            \
    o42_value_clear (&v_);                                               \
    if (!ok_) return o42_value_error (e_);                               \
  } G_STMT_END

#define ARG_TEXT(index, target)                                          \
  G_STMT_START {                                                         \
    O42Value v_ = o42_operand_value (ctx, &args[index]);                 \
    if (v_.type == O42_VALUE_ERROR)                                      \
      { O42ErrorCode e_ = v_.as.error; o42_value_clear (&v_);            \
        return o42_value_error (e_); }                                   \
    (target) = o42_value_to_text (&v_);                                  \
    o42_value_clear (&v_);                                               \
  } G_STMT_END

G_END_DECLS
