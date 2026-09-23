/* o42-eval-steps.h - a formula worked out one step at a time
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What Tools > Formula Auditing > Evaluate Formula shows: the formula
 * with the part that is worked out next underlined, and, at each step,
 * that part replaced by what it came to, until one value is left.  The
 * stepper keeps its own copy of the tree and rewrites it in place, so
 * the cell's formula is never touched.
 */

#pragma once

#include "o42-eval.h"

G_BEGIN_DECLS

typedef struct _O42Stepper O42Stepper;

/* A stepper over a copy of `formula`, evaluating with `ctx` as though
 * the formula stood at `row`, `col`.  The context is borrowed. */
O42Stepper *o42_stepper_new  (O42EvalContext *ctx, const O42Node *formula,
                              int row, int col);
void        o42_stepper_free (O42Stepper *stepper);

/* The formula as it stands, without the "=", and where the part that
 * goes next starts and how long it is (bytes); -1 and 0 once it is done. */
char       *o42_stepper_text (O42Stepper *stepper, int *start, int *length);

/* TRUE when nothing is left to work out. */
gboolean    o42_stepper_done (O42Stepper *stepper);

/* Works out the underlined part, and moves on. */
void        o42_stepper_step (O42Stepper *stepper);

/* Whether the underlined part is a single cell, and which: what Step In
 * can open.  `sheet` is NULL for the formula's own sheet. */
gboolean    o42_stepper_next_is_cell (O42Stepper *stepper, const char **sheet,
                                      int *row, int *col);

/* Puts a value in place of the underlined part: what Step Out does with
 * what the inner formula came to. */
void        o42_stepper_substitute (O42Stepper *stepper, const O42Value *value);

/* Back to the beginning. */
void        o42_stepper_restart (O42Stepper *stepper);

G_END_DECLS
