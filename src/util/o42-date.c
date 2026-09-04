/* o42-date.c - see o42-date.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-date.h"

#include <math.h>
#include <string.h>

/* Days from the Julian day count to a serial: the Julian day of 30 December
 * 1899, the day serial 0 would be if 1900 had not been given a leap day. */
static guint32
epoch_julian (void)
{
  static guint32 julian = 0;

  if (julian == 0)
    {
      GDate d;
      g_date_clear (&d, 1);
      g_date_set_dmy (&d, 30, 12, 1899);
      julian = g_date_get_julian (&d);
    }

  return julian;
}

double
o42_date_serial (int year, int month, int day)
{
  GDate d;
  int y = year, m = month;
  gint32 days;

  /* Roll months over first, then add the days as a count, so that day 0
   * and day 32 mean what people use them to mean. */
  y += (m - 1) / 12;
  m = (m - 1) % 12;
  if (m < 0) { m += 12; y -= 1; }
  m += 1;

  if (y < 1 || y > 9999)
    return -1;

  g_date_clear (&d, 1);
  g_date_set_dmy (&d, 1, (GDateMonth) m, (GDateYear) y);
  if (!g_date_valid (&d))
    return -1;

  days = (gint32) g_date_get_julian (&d) - (gint32) epoch_julian () + (day - 1);

  if (days < 61)
    days -= 1;        /* the 1900 leap-day bug, as Lotus and Excel have it */

  return days;
}

gboolean
o42_date_from_serial (double serial, int *year, int *month, int *day)
{
  GDate d;
  gint32 days = (gint32) floor (serial);

  if (days < 61)
    days += 1;

  if ((gint64) epoch_julian () + days < 1)
    return FALSE;

  g_date_clear (&d, 1);
  g_date_set_julian (&d, (guint32) ((gint64) epoch_julian () + days));
  if (!g_date_valid (&d))
    return FALSE;

  if (year)  *year  = g_date_get_year (&d);
  if (month) *month = g_date_get_month (&d);
  if (day)   *day   = g_date_get_day (&d);

  return TRUE;
}

double
o42_time_fraction (double hour, double minute, double second)
{
  return (hour * 3600.0 + minute * 60.0 + second) / 86400.0;
}

void
o42_time_from_serial (double serial, int *hour, int *minute, int *second)
{
  double frac = serial - floor (serial);
  /* Rounded to the nearest second, so that 0.5 is 12:00:00 and not
   * 11:59:59.999. */
  int total = (int) floor (frac * 86400.0 + 0.5);

  if (total >= 86400)
    total = 0;

  if (hour)   *hour   = total / 3600;
  if (minute) *minute = (total / 60) % 60;
  if (second) *second = total % 60;
}

double
o42_date_now (void)
{
  GDateTime *now = g_date_time_new_now_local ();
  double serial = o42_date_serial (g_date_time_get_year (now),
                                   g_date_time_get_month (now),
                                   g_date_time_get_day_of_month (now));

  serial += o42_time_fraction (g_date_time_get_hour (now),
                               g_date_time_get_minute (now),
                               g_date_time_get_second (now));
  g_date_time_unref (now);

  return serial;
}

/* A month written as a word: "jan", "January", "sept".  Three letters or
 * more, and every letter given must belong to the name. */
static gboolean
read_month_name (const char **p, int *month)
{
  static const char *const NAMES[12] = {
    "january", "february", "march", "april", "may", "june", "july",
    "august", "september", "october", "november", "december",
  };
  const char *q = *p;
  size_t len = 0;

  while (g_ascii_isalpha (q[len]))
    len++;
  if (len < 3)
    return FALSE;

  for (int m = 0; m < 12; m++)
    if (strlen (NAMES[m]) >= len && g_ascii_strncasecmp (q, NAMES[m], len) == 0)
      {
        *month = m + 1;
        *p += len;
        return TRUE;
      }

  return FALSE;
}

/* Two digits of year mean what Excel takes them to mean: 00 to 29 are
 * this century and 30 to 99 the last. */
static int
widen_year (int y, int digits)
{
  if (digits > 2)
    return y;
  return y < 30 ? 2000 + y : 1900 + y;
}

static int
this_year (void)
{
  GDateTime *now = g_date_time_new_now_local ();
  int y = g_date_time_get_year (now);

  g_date_time_unref (now);
  return y;
}

/* The digits at `p`, and how many of them there were. */
static gboolean
read_number (const char **p, int *out, int *digits)
{
  const char *q = *p;
  int n = 0, count = 0;

  while (g_ascii_isdigit (*q) && count < 9)
    {
      n = n * 10 + (*q - '0');
      q++;
      count++;
    }
  if (count == 0 || g_ascii_isdigit (*q))
    return FALSE;

  *p = q;
  *out = n;
  *digits = count;
  return TRUE;
}

static void
skip_spaces (const char **p)
{
  while (**p == ' ')
    (*p)++;
}

/* The calendar date at `p`, in any of the ways people write one:
 *
 *   2026-01-02   2026/01/02   2026.01.02          year first, any separator
 *   1/2/2026     1/2/26       1/2                 month/day[/year] -- 14/2 is day/month, as it can be nothing else
 *   1-2-2026     1-2-26       1-2                 the same, with dashes
 *   2.1.2026     2.1.26                           day.month.year, as most of Europe writes it
 *   2 Jan 2026   2-Jan-26     2 January           day, month, [year]
 *   Jan 2, 2026  January 2    Jan 26    Jan 2026  month, then a day if it fits in one, else a year
 *   Jan-26                                        the first of that month
 *
 * A year left out is this year.  The day is checked against the month, so
 * 30 February is not a date and 31/6 is text. */
static gboolean
read_date (const char **p, double *serial)
{
  const char *q = *p;
  int a, b, c, da, db, dc;
  int y = -1, mo = 0, d = 0;

  if (read_number (&q, &a, &da))
    {
      char sep = *q;

      if (sep == '/' || sep == '-' || sep == '.')
        {
          q++;
          if (da == 4)
            {
              /* yyyy-mm-dd */
              if (!read_number (&q, &b, &db) || *q != sep)
                return FALSE;
              q++;
              if (!read_number (&q, &c, &dc))
                return FALSE;
              y = a; mo = b; d = c;
            }
          else if (read_number (&q, &b, &db))
            {
              gboolean day_first = (sep == '.');

              if (*q == sep)
                {
                  const char *r = q + 1;

                  if (!read_number (&r, &c, &dc))
                    return FALSE;
                  q = r;
                  y = widen_year (c, dc);
                }
              if (!day_first && a > 12 && b <= 12)
                day_first = TRUE;
              if (day_first) { d = a; mo = b; }
              else           { mo = a; d = b; }
            }
          else if (sep != '.' && read_month_name (&q, &mo))
            {
              /* 2-Jan[-2026] */
              d = a;
              if (*q == sep)
                {
                  const char *r = q + 1;

                  if (!read_number (&r, &c, &dc))
                    return FALSE;
                  q = r;
                  y = widen_year (c, dc);
                }
            }
          else
            return FALSE;
        }
      else if (sep == ' ')
        {
          /* 2 Jan [2026] */
          skip_spaces (&q);
          if (!read_month_name (&q, &mo))
            return FALSE;
          d = a;
          {
            const char *r = q;

            skip_spaces (&r);
            if (*r == ',') { r++; skip_spaces (&r); }
            if (read_number (&r, &c, &dc))
              {
                y = widen_year (c, dc);
                q = r;
              }
          }
        }
      else
        return FALSE;
    }
  else if (read_month_name (&q, &mo))
    {
      /* Jan 2[, 2026]   Jan 2026   Jan-26 */
      const char *r = q;
      gboolean dashed = FALSE;

      skip_spaces (&r);
      if (*r == '-' || *r == '/') { r++; dashed = TRUE; }
      skip_spaces (&r);
      if (!read_number (&r, &b, &db))
        return FALSE;
      q = r;
      if (db == 4 || (dashed && db == 2) || b > 31)
        {
          y = widen_year (b, db);
          d = 1;
        }
      else
        {
          d = b;
          r = q;
          skip_spaces (&r);
          if (*r == ',') { r++; skip_spaces (&r); }
          if (read_number (&r, &c, &dc))
            {
              y = widen_year (c, dc);
              q = r;
            }
        }
    }
  else
    return FALSE;

  if (y < 0)
    y = this_year ();
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || y < 1 || y > 9999)
    return FALSE;

  {
    double s = o42_date_serial (y, mo, d);
    int y2, m2, d2;

    if (s < 0 || !o42_date_from_serial (s, &y2, &m2, &d2) ||
        y2 != y || m2 != mo || d2 != d)
      return FALSE;
    *serial = s;
  }
  *p = q;
  return TRUE;
}

/* h:mm[:ss][ AM|PM], or a bare hour with AM or PM after it. */
static gboolean
read_time (const char **p, double *fraction)
{
  const char *q = *p;
  int h, mi = 0, s = 0, dh, dm, ds;
  gboolean colon;

  if (!read_number (&q, &h, &dh) || dh > 2)
    return FALSE;
  colon = (*q == ':');
  if (colon)
    {
      q++;
      if (!read_number (&q, &mi, &dm) || dm > 2)
        return FALSE;
      if (*q == ':')
        {
          q++;
          if (!read_number (&q, &s, &ds) || ds > 2)
            return FALSE;
        }
    }

  {
    const char *r = q;
    gboolean meridian = FALSE;

    skip_spaces (&r);
    if ((r[0] == 'a' || r[0] == 'A' || r[0] == 'p' || r[0] == 'P') &&
        (r[1] == 'm' || r[1] == 'M') && !g_ascii_isalpha (r[2]))
      {
        gboolean pm = (r[0] == 'p' || r[0] == 'P');

        if (h < 1 || h > 12)
          return FALSE;
        if (h == 12)
          h = 0;
        if (pm)
          h += 12;
        q = r + 2;
        meridian = TRUE;
      }
    if (!colon && !meridian)
      return FALSE;
  }

  if (h > 23 || mi > 59 || s > 59)
    return FALSE;

  *fraction = o42_time_fraction (h, mi, s);
  *p = q;
  return TRUE;
}

gboolean
o42_date_parse (const char *text, double *serial,
                gboolean *has_date, gboolean *has_time)
{
  const char *p = text;
  double result = 0, fraction;
  gboolean date = FALSE, time = FALSE;

  g_return_val_if_fail (text != NULL, FALSE);

  while (g_ascii_isspace (*p))
    p++;

  if (read_date (&p, &result))
    {
      date = TRUE;
      while (*p == ' ' || *p == 'T')
        p++;
    }

  if (read_time (&p, &fraction))
    {
      result += fraction;
      time = TRUE;
    }

  while (g_ascii_isspace (*p))
    p++;

  if (*p != '\0' || (!date && !time))
    return FALSE;

  if (serial)   *serial = result;
  if (has_date) *has_date = date;
  if (has_time) *has_time = time;

  return TRUE;
}

char *
o42_date_format (double serial, O42NumberFormat format)
{
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;

  if (format == O42_NUM_TIME)
    {
      o42_time_from_serial (serial, &h, &mi, &s);
      return g_strdup_printf ("%02d:%02d:%02d", h, mi, s);
    }

  if (!o42_date_from_serial (serial, &y, &mo, &d))
    return g_strdup ("#####");

  if (format == O42_NUM_DATETIME)
    {
      o42_time_from_serial (serial, &h, &mi, &s);
      return g_strdup_printf ("%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
    }

  return g_strdup_printf ("%04d-%02d-%02d", y, mo, d);
}

int
o42_date_weekday (double serial)
{
  /* Serial 1, 1 January 1900, was a Monday; the leap-day shift below 61
   * has to be undone to count from there. */
  gint32 days = (gint32) floor (serial);

  if (days >= 61)
    days -= 1;

  return (int) (((days - 1) % 7 + 7) % 7) + 1;
}
