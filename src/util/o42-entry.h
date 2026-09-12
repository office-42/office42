/* o42-entry.h - what a piece of text means when it is typed into a cell
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A spreadsheet reads more into what is typed than strtod does: 5% is a
 * twentieth, $1,000 is a thousand, (5) is minus five, 1/2/2026 is a day
 * and 3:45 PM a time, and each of them tells the cell how to show itself
 * afterwards.  The same reading is what VALUE and the arithmetic operators
 * give text, so ="5%"+1 is 1.05 as it is in Excel.  This is the one place
 * that knows the rules, so entry, coercion and the file readers agree.
 */

#pragma once

#include "o42-numfmt.h"

G_BEGIN_DECLS

typedef struct {
  double          number;
  O42NumberFormat format;    /* the format the text asks for, or General */
  int             decimals;  /* for Percent, Currency and Comma */
  const char     *custom;    /* a code no preset says, interned; "[h]:mm:ss" for 25:30 */
} O42Entry;

/* Reads `text` as a number, a date or a time.  FALSE means it is text. */
gboolean o42_entry_parse (const char *text, O42Entry *out);

/* Excel's "fixed decimal": with it set to two, typing 1234 means
 * 12.34.  It is a habit of the person typing rather than of the book,
 * so it is the program's: -1 for off.  o42_entry_fixed_decimals_apply
 * gives the text as it should be taken -- a plain number with no point
 * or exponent in it is moved that many places -- or NULL when the text
 * is to be taken as typed.  Only what is typed goes through it; a
 * file's numbers do not. */
void  o42_entry_set_fixed_decimals   (int places);
int   o42_entry_fixed_decimals       (void);
char *o42_entry_fixed_decimals_apply (const char *text);

/* `text` as it has to be typed to stay text: with an apostrophe in front
 * when a cell would otherwise read it as a number, a date, TRUE or a
 * formula.  It is what a file reader gives a cell it knows holds text.
 * Caller frees. */
char    *o42_entry_quote_text (const char *text);

G_END_DECLS
