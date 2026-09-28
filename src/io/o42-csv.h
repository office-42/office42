/* o42-csv.h - comma-separated values, both ways
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The oldest exchange format there is, and the one every program reads.
 * Writing puts out what the cells show, as Excel and Gnumeric do, since a
 * formula means nothing to whoever reads a CSV.  Reading follows RFC 4180:
 * fields are separated by commas, a field holding a comma, a quote or a
 * newline is wrapped in quotes, and a quote inside is doubled.  Each field
 * then goes through the same number-or-text decision a typed cell gets.
 *
 * "Comma" is a manner of speaking.  Half the world's Excels write
 * semicolons, because their decimal point is a comma; a .txt has tabs;
 * a report printed to a file lines its columns up with spaces and has
 * no separator at all; and the file may be UTF-16 from "Unicode Text",
 * or Windows-1252 from a Windows of thirty years ago.  O42CsvOptions is
 * everything Excel's Text Import Wizard asks about such a file, and the
 * few things a writer must choose; o42_csv_sniff fills it in from the
 * file the way Text Import's first guess does.
 */

#pragma once

#include <gio/gio.h>

#include "o42-sheet.h"

G_BEGIN_DECLS

/* Text from a file that may not be UTF-8 -- a Lotus, SYLK, DIF or HTML
 * file from a Windows of thirty years ago -- made into UTF-8: kept as it
 * is when it already is, else read as the machine's code page, else
 * Windows-1252, else with the bad bytes replaced.  Never NULL; the model
 * must never hold text it cannot write out again.  Caller frees. */
char *o42_text_to_utf8 (const char *text, gssize length);

/* The order a date column's day, month and year come in, as the third
 * step of Excel's wizard offers it. */
typedef enum {
  O42_DATE_MDY = 0,
  O42_DATE_DMY,
  O42_DATE_YMD,
  O42_DATE_MYD,
  O42_DATE_DYM,
  O42_DATE_YDM
} O42DateOrder;

/* "MDY", "DMY"... for the drop-down and the terminal. */
extern const char *const O42_DATE_ORDER_NAMES[];

/* How one column of the file is taken: as typed, as text whatever it
 * looks like, as a date in the given order, or not at all. */
typedef struct {
  O42SplitType type;
  O42DateOrder order;
} O42CsvColumn;

typedef struct {
  /* Both ways. */
  char    *encoding;         /* iconv's name for it, "UTF-8", "UTF-16LE",
                              * "WINDOWS-1252"...; NULL to guess on
                              * reading and UTF-8 on writing */
  char    *separators;       /* UTF-8: every character in it ends a field;
                              * the writer uses the first */
  char     quote;            /* the text qualifier, '"' or '\'', or '\0' */

  /* Reading. */
  gboolean fixed_width;      /* cut at `breaks` rather than at separators */
  gboolean merge_separators; /* two separators together make one */
  int      start_row;        /* lines of the file passed over, from the top */
  GArray  *breaks;           /* int: the characters at which a fixed-width
                              * column starts, ascending */
  GArray  *columns;          /* O42CsvColumn, from the first; any column
                              * beyond is General */
  char     decimal;          /* the decimal separator the file uses */
  char     thousands;        /* its thousands separator, or '\0' */
  gboolean trailing_minus;   /* 5- is minus five, as a mainframe prints it */

  /* What o42_csv_sniff found, for the writer to give back the same. */
  gboolean bom;              /* a byte-order mark in front */
  gboolean crlf;             /* lines end CR LF rather than LF */

  /* Writing. */
  gboolean as_shown;         /* what the cells show, not their values */
  gboolean formulas;         /* a formula's text rather than its value */
  gboolean quote_all;        /* every text field quoted, not just those
                              * that need it */
} O42CsvOptions;

/* Excel's defaults: comma, double quote, the encoding guessed, CR LF,
 * values as shown. */
O42CsvOptions *o42_csv_options_new  (void);
O42CsvOptions *o42_csv_options_copy (const O42CsvOptions *options);
void           o42_csv_options_free (O42CsvOptions *options);

/* The same, set up for writing to `file`: a tab for .txt, .tsv and
 * .tab, else a comma. */
O42CsvOptions *o42_csv_options_for_file (GFile *file);

void o42_csv_options_set_separators (O42CsvOptions *options, const char *separators);
void o42_csv_options_set_encoding   (O42CsvOptions *options, const char *encoding);

/* The column's settings, General when nothing was said about it. */
O42CsvColumn o42_csv_options_column (const O42CsvOptions *options, int col);
void         o42_csv_options_set_column (O42CsvOptions *options, int col,
                                         O42SplitType type, O42DateOrder order);

/* The file's bytes made into UTF-8 in `encoding` (NULL to guess), with
 * the byte-order mark taken off and a stray NUL made a space; a byte
 * the encoding has no meaning for becomes U+FFFD rather than the end of
 * the file.  `found`, when given, is set to the encoding used (static)
 * and `bom` to whether there was a mark.  Caller frees. */
char *o42_csv_decode (const char *bytes, gsize length, const char *encoding,
                      const char **found, gboolean *bom);

/* Fills in what Text Import first guesses from the decoded text
 * and the file's name: the separator, delimited or fixed width and
 * where its columns start, the decimal separator its numbers use, the
 * line ends.  `name` may be NULL. */
void o42_csv_sniff (const char *text, const char *name, O42CsvOptions *options);

/* The file read, decoded and guessed at: its text, and in `options`
 * (new, to free) everything o42_csv_sniff says with the encoding it
 * was found to be in.  NULL on failure. */
char *o42_csv_read_file (GFile *file, O42CsvOptions **options, GError **error);

/* How many lines the text has, the last one counted even when it has no
 * newline. */
int o42_csv_count_lines (const char *text);

/* The first `max_lines` lines of the text as they are, for the preview
 * of the file before it is cut.  Frees its strings. */
GPtrArray *o42_csv_lines (const char *text, int max_lines);

/* The first `max_rows` records of the text as the options cut them,
 * each a NULL-terminated vector of fields as they are in the file
 * (quotes taken off, nothing converted).  For Text Import's preview. */
GPtrArray *o42_csv_preview (const char *text, const O42CsvOptions *options, int max_rows);

/* A field put into a cell as a column of that kind takes it: General as
 * if typed, Text kept as text, Date read in the column's order, with
 * the options' decimal and thousands separators understood. */
void o42_csv_set_cell (O42Sheet *sheet, int row, int col, const char *field,
                       const O42CsvColumn *column, const O42CsvOptions *options);

/* Puts the decoded text's records into the sheet with their first field
 * at (row, col), as one undo step.  With `replace` the sheet is emptied
 * first.  Skipped columns take no room.  Returns how many records went
 * in; `columns`, when given, is set to the widest. */
int o42_csv_import (O42Sheet *sheet, const char *text, const O42CsvOptions *options,
                    int row, int col, gboolean replace, int *columns);

/* The file read with the options (NULL to guess them all), replacing
 * the sheet's cells as one undo step. */
gboolean o42_csv_load_with (O42Sheet *sheet, GFile *file, const O42CsvOptions *options,
                            GError **error);

/* The sheet written with the options, or with the defaults for the
 * file's name when NULL. */
gboolean o42_csv_save_with (O42Sheet *sheet, GFile *file, const O42CsvOptions *options,
                            GError **error);

gboolean o42_csv_save (O42Sheet *sheet, GFile *file, GError **error);

/* Whether the last save had characters its encoding has no way to
 * write -- a Chinese name in a Windows-1252 file -- and wrote a
 * question mark for them. */
extern gboolean o42_csv_lost_characters;

/* Replaces the sheet's cells with the file's, as one undo step. */
gboolean o42_csv_load (O42Sheet *sheet, GFile *file, GError **error);

G_END_DECLS
