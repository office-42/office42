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
  O42_NUM_DATETIME,     /* both */
  O42_NUM_ACCOUNTING    /* the currency symbol at the left edge, the number at
                         * the right, a space between: Excel's Accounting */
} O42NumberFormat;

/* The most decimals a format can show, as in Excel: a code asking for
 * more shows this many, and a file asking for more is read as this. */
#define O42_MAX_DECIMALS 30

/* The currency symbol the Currency and Accounting presets show: the
 * locale's at start-up, or what Tools > Options was told.  Kept here
 * because a format is applied below the model and the window alike. */
const char *o42_numfmt_currency     (void);
void        o42_numfmt_set_currency (const char *symbol);   /* NULL for the locale's */
const char *o42_numfmt_currency_iso (void);   /* "USD" for "$", "NOK" for "kr" ... */

/* What a format asks of the painter that the text alone cannot say:
 * "*x" fills the cell's spare width with x -- Accounting's "* " is what
 * pushes the currency symbol to the left edge and the number to the
 * right -- and "_x" leaves a gap as wide as the glyph x.  The text has
 * a single space at each pad, and nothing at the fill; a painter that
 * knows glyph widths widens the pads and places the two halves apart,
 * and everything else shows the text as it is. */
#define O42_FORMAT_MAX_PADS 8
typedef struct {
  int      fill_at;       /* byte offset in the text, or -1 for no fill */
  gunichar fill_char;
  int      n_pads;
  struct {
    int      at;          /* byte offset of the space that stands in */
    gunichar glyph;       /* as wide as this */
  } pads[O42_FORMAT_MAX_PADS];
} O42FormatLayout;

/* The text for `n` in a format.  General is up to ten significant figures
 * with trailing zeroes dropped; the others are what their names say.
 * Locale-independent: the decimal point is a point and the thousands
 * separator a comma everywhere, so a file shows the same on every machine.
 * Caller frees. */
char *o42_number_format (double n, O42NumberFormat format, int decimals);

/* A number rounded to so many decimals the way a cell shows it: a half
 * away from zero, and decided at the fifteenth significant digit, so
 * that 1.005 at two decimals is 1.01 and not the 1.00 its double is. */
double o42_number_round_shown (double n, int places);

/* The number with every digit that matters: fifteen significant figures,
 * as Excel's "&" and TEXT give, or with `exact` the seventeen it takes to
 * read the same double back.  Whole numbers print without a point.  This
 * is what a cell's input text and a rewritten formula are made from, so
 * that copying, sorting, undoing and saving never lose a digit.  Caller
 * frees. */
/* The number as Excel sees it: fifteen significant figures, and a
 * denormal is zero. */
double o42_number_seen (double n);

char *o42_number_to_text (double n, gboolean exact);

/* The format as Excel and Gnumeric write it -- "#,##0.00", "0%",
 * "yyyy-mm-dd" -- and back.  Reading goes by the shape of the string, so
 * "[$$-409]#,##0.00" is Currency with two decimals; the full format
 * language is on the roadmap.  Caller frees the string. */
char    *o42_number_format_to_string (O42NumberFormat format, int decimals);
gboolean o42_number_format_parse     (const char *text,
                                      O42NumberFormat *format, int *decimals);

/* How a negative is shown, as Format Cells offers it. */
typedef enum {
  O42_NEG_MINUS = 0,      /* -1,234.10 */
  O42_NEG_RED,            /* 1,234.10 in red */
  O42_NEG_PARENS,         /* (1,234.10) */
  O42_NEG_RED_PARENS      /* (1,234.10) in red */
} O42NegativeStyle;

/* A Fixed, Comma, Currency or Accounting code with a symbol of the
 * caller's choosing (NULL or "" for none) and a way of showing
 * negatives; the same as o42_number_format_to_string when the symbol
 * is the machine's and negatives are plain.  Caller frees. */
char *o42_number_format_code (O42NumberFormat format, int decimals,
                              const char *symbol, O42NegativeStyle negative);

/* What a code says about its symbol and its negatives, for a dialog
 * showing the code's settings: `symbol` (caller frees) is what the
 * code names in [$...] or in quotes before the digits, or NULL. */
void  o42_number_format_details (const char *code, char **symbol, O42NegativeStyle *negative);

/* The format language itself: "#,##0.00;[Red](#,##0.00)", "yyyy-mm-dd",
 * "0.0%", "0.00E+00", "\"$\"#,##0" and the rest, applied to a number --
 * or to text, when `text` is not NULL and the format has a fourth
 * section with "@" in it.  Caller frees. */
char    *o42_format_string (const char *format, double n, const char *text);

/* The same, with the fill and the pads reported for a painter that can
 * honour them.  `layout` may be NULL, and then this is o42_format_string. */
char    *o42_format_string_layout (const char *format, double n, const char *text,
                                   O42FormatLayout *layout);

/* The preset applied to a number, with its layout (see above); the
 * plain o42_number_format is this with no layout asked for. */
char    *o42_number_format_layout (double n, O42NumberFormat format, int decimals,
                                   O42FormatLayout *layout);

/* The colour a format asks for on a value, as [Red] and its kin put it. */
gboolean o42_format_string_colour (const char *format, double n, guint32 *colour);

G_END_DECLS
