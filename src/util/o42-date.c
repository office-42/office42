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

static gboolean
read_int (const char **p, int digits_min, int digits_max, int *out)
{
  int n = 0, count = 0;

  while (g_ascii_isdigit (**p) && count < digits_max)
    {
      n = n * 10 + (**p - '0');
      (*p)++;
      count++;
    }

  if (count < digits_min)
    return FALSE;

  *out = n;
  return TRUE;
}

gboolean
o42_date_parse (const char *text, double *serial,
                gboolean *has_date, gboolean *has_time)
{
  const char *p = text;
  int y, mo, d, h, mi, s = 0;
  double result = 0;
  gboolean date = FALSE, time = FALSE;

  g_return_val_if_fail (text != NULL, FALSE);

  while (g_ascii_isspace (*p))
    p++;

  /* yyyy-mm-dd */
  {
    const char *q = p;

    if (read_int (&q, 4, 4, &y) && *q == '-' && (q++, read_int (&q, 1, 2, &mo)) &&
        *q == '-' && (q++, read_int (&q, 1, 2, &d)))
      {
        if (mo < 1 || mo > 12 || d < 1 || d > 31)
          return FALSE;
        result = o42_date_serial (y, mo, d);
        if (result < 0)
          return FALSE;
        date = TRUE;
        p = q;
        while (*p == ' ' || *p == 'T')
          p++;
      }
  }

  /* hh:mm[:ss] */
  {
    const char *q = p;

    if (read_int (&q, 1, 2, &h) && *q == ':' && (q++, read_int (&q, 2, 2, &mi)))
      {
        if (*q == ':')
          {
            q++;
            if (!read_int (&q, 2, 2, &s))
              return FALSE;
          }
        if (h > 23 || mi > 59 || s > 59)
          return FALSE;
        result += o42_time_fraction (h, mi, s);
        time = TRUE;
        p = q;
      }
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
