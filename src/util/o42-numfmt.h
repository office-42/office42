/* o42-numfmt.h - writing a number out the way a format asks
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The number formats a cell can carry, and the routine that applies one to
 * a number.  This sits below both the model and the formula engine because
 * both need it: a cell's display goes through it, and so do TEXT, FIXED
 * and DOLLAR.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  O42_NUM_GENERAL = 0,
  O42_NUM_FIXED,        /* 0.00 with a chosen number of decimals */
  O42_NUM_COMMA,        /* #,##0.00 */
  O42_NUM_CURRENCY,
  O42_NUM_PERCENT,
  O42_NUM_SCIENTIFIC,
  O42_NUM_TEXT,         /* show the number as typed, do not format it */
  O42_NUM_DATE,         /* yyyy-mm-dd, from a serial date */
  O42_NUM_TIME,         /* hh:mm:ss, from the fraction of one */
  O42_NUM_DATETIME      /* both */
} O42NumberFormat;

/* The text for `n` in a format.  General is up to ten significant figures
 * with trailing zeroes dropped; the others are what their names say.
 * Locale-independent: the decimal point is a point and the thousands
 * separator a comma everywhere, so a file shows the same on every machine.
 * Caller frees. */
char *o42_number_format (double n, O42NumberFormat format, int decimals);

/* The number with every digit that matters: fifteen significant figures,
 * as Excel's "&" and TEXT give, or with `exact` the seventeen it takes to
 * read the same double back.  Whole numbers print without a point.  This
 * is what a cell's input text and a rewritten formula are made from, so
 * that copying, sorting, undoing and saving never lose a digit.  Caller
 * frees. */
char *o42_number_to_text (double n, gboolean exact);

/* The format as Excel and Gnumeric write it -- "#,##0.00", "0%",
 * "yyyy-mm-dd" -- and back.  Reading goes by the shape of the string, so
 * "[$$-409]#,##0.00" is Currency with two decimals; the full format
 * language is on the roadmap.  Caller frees the string. */
char    *o42_number_format_to_string (O42NumberFormat format, int decimals);
gboolean o42_number_format_parse     (const char *text,
                                      O42NumberFormat *format, int *decimals);

/* The format language itself: "#,##0.00;[Red](#,##0.00)", "yyyy-mm-dd",
 * "0.0%", "0.00E+00", "\"$\"#,##0" and the rest, applied to a number --
 * or to text, when `text` is not NULL and the format has a fourth
 * section with "@" in it.  Caller frees. */
char    *o42_format_string (const char *format, double n, const char *text);

/* The colour a format asks for on a value, as [Red] and its kin put it. */
gboolean o42_format_string_colour (const char *format, double n, guint32 *colour);

G_END_DECLS
