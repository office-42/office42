/* o42-hdate.c - see o42-hdate.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-hdate.h"

/* The Hebrew epoch: 1 Tishri 1 fell on this absolute day, which is in
 * the autumn of 3761 before the common era. */
#define HEBREW_EPOCH (-1373429)

/* A modulus that is never negative, which every calendar needs and C's
 * % does not give. */
static gint64
mod (gint64 a, gint64 b)
{
  gint64 r = a % b;

  return r < 0 ? r + b : r;
}

static gboolean
gregorian_leap (int year)
{
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

gint64
o42_gregorian_absolute (int year, int month, int day)
{
  static const int BEFORE[] = { 0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
  gint64 prior = year - 1;

  if (month < 1) month = 1;
  if (month > 12) month = 12;
  return (gint64) day + BEFORE[month]
       + (month > 2 && gregorian_leap (year) ? 1 : 0)
       + 365 * prior + prior / 4 - prior / 100 + prior / 400;
}

void
o42_gregorian_from_absolute (gint64 absolute, int *year, int *month, int *day)
{
  int y = (int) (absolute / 366);
  int m = 1;

  /* Walk up from a year that is certainly not too late, then along the
   * months: both loops run a handful of times at most. */
  while (o42_gregorian_absolute (y + 1, 1, 1) <= absolute)
    y++;
  while (m < 12 && o42_gregorian_absolute (y, m + 1, 1) <= absolute)
    m++;
  if (year != NULL)  *year = y;
  if (month != NULL) *month = m;
  if (day != NULL)   *day = (int) (absolute - o42_gregorian_absolute (y, m, 1) + 1);
}

gint64
o42_julian_day (int year, int month, int day)
{
  /* Absolute day 1 is 1 January 1 Gregorian, which is Julian day
   * 1721426. */
  return o42_gregorian_absolute (year, month, day) + 1721425;
}

gboolean
o42_hebrew_leap_year (int year)
{
  return mod (7 * (gint64) year + 1, 19) < 7;
}

int
o42_hebrew_months_in_year (int year)
{
  return o42_hebrew_leap_year (year) ? 13 : 12;
}

/* The days from the Hebrew epoch to the molad of Tishri of `year`,
 * with the four postponements applied: the moon's mean month is 29
 * days, 12 hours and 793 parts of an hour of 1080. */
static gint64
hebrew_elapsed_days (int year)
{
  gint64 months = 235 * (((gint64) year - 1) / 19)
                + 12 * mod ((gint64) year - 1, 19)
                + (7 * mod ((gint64) year - 1, 19) + 1) / 19;
  gint64 parts = 204 + 793 * mod (months, 1080);
  gint64 hours = 5 + 12 * months + 793 * (months / 1080) + parts / 1080;
  gint64 day = 1 + 29 * months + hours / 24;
  gint64 rest = mod (hours, 24) * 1080 + mod (parts, 1080);
  gint64 alt;

  /* The molad falls after noon, or on a Tuesday or a Monday in the two
   * cases that would make the year too long or too short. */
  if (rest >= 19440
      || (mod (day, 7) == 2 && rest >= 9924 && !o42_hebrew_leap_year (year))
      || (mod (day, 7) == 1 && rest >= 16789 && o42_hebrew_leap_year (year - 1)))
    day++;

  /* And the new year never falls on a Sunday, a Wednesday or a Friday. */
  alt = mod (day, 7);
  if (alt == 0 || alt == 3 || alt == 5)
    day++;
  return day;
}

static int
hebrew_year_days (int year)
{
  return (int) (hebrew_elapsed_days (year + 1) - hebrew_elapsed_days (year));
}

int
o42_hebrew_month_days (int year, int month)
{
  int months = o42_hebrew_months_in_year (year);
  int days = hebrew_year_days (year);

  if (month < 1)
    month = 1;
  if (month > months)
    month = months;

  switch (month)
    {
    case 1:  return 30;                  /* Tishri */
    case 2:  return days % 10 == 5 ? 30 : 29;   /* Heshvan, long in a full year */
    case 3:  return days % 10 == 3 ? 29 : 30;   /* Kislev, short in a lacking one */
    case 4:  return 29;                  /* Tevet */
    case 5:  return 30;                  /* Shevat */
    case 6:  return months == 13 ? 30 : 29;     /* Adar, or Adar I */
    default: break;
    }
  if (months == 13 && month == 7)
    return 29;                           /* Adar II */

  /* Nisan onwards: thirty and twenty-nine about turn. */
  {
    int from_nisan = month - (months == 13 ? 8 : 7);   /* 0 for Nisan */

    return from_nisan % 2 == 0 ? 30 : 29;
  }
}

gint64
o42_hebrew_absolute (int year, int month, int day)
{
  gint64 total = HEBREW_EPOCH + hebrew_elapsed_days (year) + day;
  int months = o42_hebrew_months_in_year (year);

  if (month < 1)
    month = 1;
  if (month > months)
    month = months;
  for (int m = 1; m < month; m++)
    total += o42_hebrew_month_days (year, m);
  return total;
}

void
o42_hebrew_from_absolute (gint64 absolute, int *year, int *month, int *day)
{
  int y = (int) ((absolute - HEBREW_EPOCH) / 366);
  int m = 1;

  while (o42_hebrew_absolute (y + 1, 1, 1) <= absolute)
    y++;
  while (m < o42_hebrew_months_in_year (y) &&
         o42_hebrew_absolute (y, m + 1, 1) <= absolute)
    m++;
  if (year != NULL)  *year = y;
  if (month != NULL) *month = m;
  if (day != NULL)   *day = (int) (absolute - o42_hebrew_absolute (y, m, 1) + 1);
}

const char *
o42_hebrew_month_name (int year, int month)
{
  static const char *const PLAIN[] = {
    "Tishri", "Heshvan", "Kislev", "Tevet", "Shevat", "Adar",
    "Nisan", "Iyar", "Sivan", "Tammuz", "Av", "Elul"
  };
  static const char *const LEAP[] = {
    "Tishri", "Heshvan", "Kislev", "Tevet", "Shevat", "Adar I", "Adar II",
    "Nisan", "Iyar", "Sivan", "Tammuz", "Av", "Elul"
  };

  if (month < 1)
    return NULL;
  if (o42_hebrew_leap_year (year))
    return month <= 13 ? LEAP[month - 1] : NULL;
  return month <= 12 ? PLAIN[month - 1] : NULL;
}
