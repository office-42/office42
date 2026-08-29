/* o42-value.h - what a cell can hold
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A spreadsheet value is one of five things, and the rules for turning one
 * into another are the whole of what makes =1+"2" behave the way people
 * expect.  Those rules live here rather than in the evaluator so that the
 * evaluator can be about evaluation.
 */

#pragma once

#include "o42-types.h"

G_BEGIN_DECLS

typedef enum {
  O42_VALUE_EMPTY = 0,
  O42_VALUE_NUMBER,
  O42_VALUE_TEXT,
  O42_VALUE_BOOL,
  O42_VALUE_ERROR
} O42ValueType;

/* The error values a spreadsheet can hold.  They are values, not failures:
 * an error propagates through arithmetic and can be tested for, which is why
 * IFERROR can exist at all. */
typedef enum {
  O42_ERR_NONE = 0,
  O42_ERR_NULL,      /* #NULL!   */
  O42_ERR_DIV0,      /* #DIV/0!  */
  O42_ERR_VALUE,     /* #VALUE!  */
  O42_ERR_REF,       /* #REF!    */
  O42_ERR_NAME,      /* #NAME?   */
  O42_ERR_NUM,       /* #NUM!    */
  O42_ERR_NA,        /* #N/A     */
  O42_ERR_CIRCULAR,  /* a circular reference; Excel says #REF! but saying so
                      * outright is more use than being compatible about it */
  O42_ERR_SPILL      /* a formula's array result has no room to spill into */
} O42ErrorCode;

typedef struct {
  O42ValueType type;
  union {
    double        number;
    char         *text;     /* owned */
    gboolean      boolean;
    O42ErrorCode  error;
  } as;
} O42Value;

/* Values are passed by value and own their text, so every one that has been
 * assigned to must be cleared exactly once. */
void      o42_value_clear  (O42Value *value);
O42Value  o42_value_copy   (const O42Value *value);

O42Value  o42_value_empty  (void);
O42Value  o42_value_number (double number);
O42Value  o42_value_text   (const char *text);      /* copies */
O42Value  o42_value_take   (char *text);            /* takes ownership */
O42Value  o42_value_bool   (gboolean boolean);
O42Value  o42_value_error  (O42ErrorCode code);

gboolean  o42_value_is_error (const O42Value *value);
const char *o42_error_name (O42ErrorCode code);

/* ---- Coercion --------------------------------------------------------- */

/* Excel's rules, which are not arbitrary once you see them: an empty cell is
 * zero in arithmetic and "" in text, TRUE is one, and a text value is a
 * number only if the whole of it parses as one.  A coercion that cannot be
 * made yields #VALUE! rather than a guess. */
gboolean o42_value_to_number (const O42Value *value, double *out,
                              O42ErrorCode *error);
char    *o42_value_to_text   (const O42Value *value);   /* caller frees */
gboolean o42_value_to_bool   (const O42Value *value, gboolean *out,
                              O42ErrorCode *error);

/* Ordering for the comparison operators.  Returns -1, 0 or 1.  Text compares
 * case-insensitively, as it does in Excel, and a number always sorts before
 * text, which is what makes =1<"a" true. */
int o42_value_compare (const O42Value *a, const O42Value *b);

/* The text a cell shows when it has no explicit number format.  Numbers lose
 * their trailing zeroes; this is the "General" format. */
char *o42_value_display (const O42Value *value);

G_END_DECLS
