/* o42-hdate.h - the Hebrew calendar
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The calendar Gnumeric's HDATE functions work in: a lunisolar year of
 * twelve months, with a thirteenth added seven times in nineteen so
 * that Passover stays in the spring, and two months that lengthen or
 * shorten to keep the festivals off the weekdays they may not fall on.
 *
 * Months are numbered from Tishri, where the civil year begins: 1
 * Tishri, 2 Heshvan, 3 Kislev, 4 Tevet, 5 Shevat, 6 Adar (Adar I in a
 * leap year), 7 Adar II in a leap year and Nisan otherwise, and on to
 * 13 Elul in a leap year.  That is the numbering Gnumeric's plugin
 * uses and the one its month names are listed in.
 *
 * The arithmetic is the classical one -- the molad of Tishri, the four
 * postponements, and the two variable months -- as it is set out in
 * Reingold and Dershowitz, counting in days since the Gregorian first
 * of January in the year 1.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Days since 1 January 1 (Gregorian), which is day 1. */
gint64 o42_gregorian_absolute (int year, int month, int day);
void   o42_gregorian_from_absolute (gint64 absolute, int *year, int *month, int *day);

/* The Julian day number of a Gregorian date, for the functions that
 * ask for one. */
gint64 o42_julian_day (int year, int month, int day);

gboolean o42_hebrew_leap_year (int year);

/* How many days a Hebrew month has, and how many months its year has. */
int o42_hebrew_months_in_year (int year);
int o42_hebrew_month_days (int year, int month);

/* Between the two calendars.  A month that a plain year has not got
 * (13) comes back as the last month it has. */
gint64 o42_hebrew_absolute (int year, int month, int day);
void   o42_hebrew_from_absolute (gint64 absolute, int *year, int *month, int *day);

/* "Tishri", "Heshvan", ... in the numbering above; NULL past the end. */
const char *o42_hebrew_month_name (int year, int month);

G_END_DECLS
