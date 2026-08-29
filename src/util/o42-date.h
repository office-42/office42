/* o42-date.h - serial dates, the way a spreadsheet counts days
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A date in a spreadsheet is a number: the count of days since the end of
 * 1899, with the time of day as the fraction.  Serial 1 is 1 January 1900
 * and 2 January 2026 is 46024.  Lotus 1-2-3 believed 1900 was a leap year,
 * so its serial 60 was a 29 February that never happened; Excel kept the
 * bug for compatibility and so does everything that reads Excel files,
 * Gnumeric included.  office42 does the same: serials from 61 on count
 * from 30 December 1899, and 1 to 59 are shifted by one to land where they
 * should.  Serial 60 shows as 1 March 1900.
 */

#pragma once

#include "o42-numfmt.h"

G_BEGIN_DECLS

/* Serial for a calendar date.  Months and days outside their range roll
 * over, so DATE(2026, 13, 1) is January 2027 and DATE(2026, 3, 0) is the
 * last day of February, which is how Excel's DATE has always worked and is
 * the idiom for "end of month". */
double   o42_date_serial      (int year, int month, int day);
gboolean o42_date_from_serial (double serial, int *year, int *month, int *day);

/* The time of day as a fraction of a day, and back. */
double   o42_time_fraction    (double hour, double minute, double second);
void     o42_time_from_serial (double serial, int *hour, int *minute,
                               int *second);

/* Now, as a serial with the time in the fraction. */
double   o42_date_now (void);

/* Reads "2026-08-27", "2026-08-27 09:30", "09:30" and "09:30:15" -- the
 * forms that are unambiguous everywhere.  Reports which parts it found so
 * the caller can pick a format. */
gboolean o42_date_parse (const char *text, double *serial,
                         gboolean *has_date, gboolean *has_time);

/* Writes a serial as a date, a time, or both.  Caller frees. */
char    *o42_date_format (double serial, O42NumberFormat format);

/* 1 for Monday through 7 for Sunday: ISO's numbering, which the WEEKDAY
 * function turns into whichever numbering it was asked for. */
int      o42_date_weekday (double serial);

G_END_DECLS
