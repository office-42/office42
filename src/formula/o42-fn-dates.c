/* o42-fn-dates.c - see o42-eval-private.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-eval-private.h"

#include "o42-date.h"
#include "o42-numfmt.h"

#include <glib/gi18n.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The names this file has always called them by. */
#define operand_value o42_operand_value
#define normal_cdf o42_normal_cdf
#define normal_pdf o42_normal_pdf
#define collect_numbers o42_collect_numbers
#define is_holiday o42_is_holiday

/* ---- Dates and times -------------------------------------------------- */

static O42Value
fn_today (O42EvalContext *ctx, O42Operand *args, int n)
{
  (void) ctx; (void) args; (void) n;
  return o42_value_number (floor (o42_date_now ()));
}

static O42Value
fn_now (O42EvalContext *ctx, O42Operand *args, int n)
{
  (void) ctx; (void) args; (void) n;
  return o42_value_number (o42_date_now ());
}

static O42Value
fn_date (O42EvalContext *ctx, O42Operand *args, int n)
{
  double y, m, d, serial;
  (void) n;
  ARG_NUMBER (0, y);
  ARG_NUMBER (1, m);
  ARG_NUMBER (2, d);

  /* Two-digit years are 1900s, as they have been since 1900. */
  if (y >= 0 && y < 1900)
    y += 1900;

  serial = o42_date_serial ((int) y, (int) m, (int) d);
  if (serial < 0 || serial > 2958465)
    return o42_value_error (O42_ERR_NUM);   /* before 1900 or past 9999-12-31 */
  return o42_value_number (serial);
}

static O42Value
fn_time (O42EvalContext *ctx, O42Operand *args, int n)
{
  double h, m, s, frac;
  (void) n;
  ARG_NUMBER (0, h);
  ARG_NUMBER (1, m);
  ARG_NUMBER (2, s);
  /* Excel's TIME takes nothing at or past 32768 in any part. */
  if (h >= 32768 || m >= 32768 || s >= 32768)
    return o42_value_error (O42_ERR_NUM);
  frac = o42_time_fraction (trunc (h), trunc (m), trunc (s));
  if (frac < 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (frac - floor (frac));
}

/* A time on its own -- "25:00", "2:30 p", "14:05:30 PM" -- as
 * TIMEVALUE takes it: hours past twenty-four wrap, and a bare a or p
 * serves for am and pm.  FALSE for anything else. */
static gboolean
o42_time_of_day (const char *text, double *serial)
{
  const char *p = text;
  int h, m = 0, sec = 0;
  double fraction = 0;    /* "15:45:30.5": the half second */
  char *end;
  gboolean pm = FALSE, am = FALSE;

  while (g_ascii_isspace (*p)) p++;
  if (!g_ascii_isdigit (*p))
    return FALSE;
  h = (int) strtol (p, &end, 10);
  if (*end != ':')
    return FALSE;
  p = end + 1;
  if (!g_ascii_isdigit (*p))
    return FALSE;
  m = (int) strtol (p, &end, 10);
  p = end;
  if (*p == ':')
    {
      if (!g_ascii_isdigit (p[1]))
        return FALSE;
      sec = (int) strtol (p + 1, &end, 10);
      p = end;
      if (*p == '.' && g_ascii_isdigit (p[1]))
        {
          fraction = g_ascii_strtod (p, &end);
          p = end;
        }
    }
  while (g_ascii_isspace (*p)) p++;
  if (g_ascii_tolower (*p) == 'a' || g_ascii_tolower (*p) == 'p')
    {
      pm = g_ascii_tolower (*p) == 'p';
      am = !pm;
      p++;
      if (g_ascii_tolower (*p) == 'm') p++;
    }
  while (g_ascii_isspace (*p)) p++;
  if (*p != '\0' || m < 0 || m >= 60 || sec < 0 || sec >= 60 || h < 0)
    return FALSE;
  if ((am || pm) && (h < 1 || h > 12))
    return FALSE;
  if (pm && h < 12) h += 12;
  if (am && h == 12) h = 0;
  *serial = (h * 3600.0 + m * 60.0 + sec + fraction) / 86400.0;
  *serial -= floor (*serial);
  return TRUE;
}

static O42Value
fn_datevalue_timevalue (O42EvalContext *ctx, O42Operand *args, int n, gboolean date)
{
  char *s = NULL;
  double serial;
  gboolean ok;
  (void) n;
  ARG_TEXT (0, s);
  ok = !date && o42_time_of_day (s, &serial);
  if (!ok)
    ok = o42_date_parse (s, &serial, NULL, NULL);
  g_free (s);
  if (!ok)
    return o42_value_error (O42_ERR_VALUE);
  return o42_value_number (date ? floor (serial) : serial - floor (serial));
}

static O42Value fn_datevalue (O42EvalContext *c, O42Operand *a, int n) { return fn_datevalue_timevalue (c, a, n, TRUE); }
static O42Value fn_timevalue (O42EvalContext *c, O42Operand *a, int n) { return fn_datevalue_timevalue (c, a, n, FALSE); }

typedef enum { PART_YEAR, PART_MONTH, PART_DAY, PART_HOUR, PART_MINUTE, PART_SECOND } DatePart;

static O42Value
fn_date_part (O42EvalContext *ctx, O42Operand *args, int n, DatePart part)
{
  double serial;
  int y, mo, d, h, mi, s;
  (void) n;
  ARG_NUMBER (0, serial);

  if (serial < 0 || serial >= 2958466)
    return o42_value_error (O42_ERR_NUM);   /* before 1900 or past 9999-12-31 */

  if (part <= PART_DAY)
    {
      if (!o42_date_from_serial (serial, &y, &mo, &d))
        return o42_value_error (O42_ERR_NUM);
      return o42_value_number (part == PART_YEAR ? y : part == PART_MONTH ? mo : d);
    }

  o42_time_from_serial (serial, &h, &mi, &s);
  return o42_value_number (part == PART_HOUR ? h : part == PART_MINUTE ? mi : s);
}

static O42Value fn_year   (O42EvalContext *c, O42Operand *a, int n) { return fn_date_part (c, a, n, PART_YEAR); }
static O42Value fn_month  (O42EvalContext *c, O42Operand *a, int n) { return fn_date_part (c, a, n, PART_MONTH); }
static O42Value fn_day    (O42EvalContext *c, O42Operand *a, int n) { return fn_date_part (c, a, n, PART_DAY); }
static O42Value fn_hour   (O42EvalContext *c, O42Operand *a, int n) { return fn_date_part (c, a, n, PART_HOUR); }
static O42Value fn_minute (O42EvalContext *c, O42Operand *a, int n) { return fn_date_part (c, a, n, PART_MINUTE); }
static O42Value fn_second (O42EvalContext *c, O42Operand *a, int n) { return fn_date_part (c, a, n, PART_SECOND); }

static O42Value
fn_weekday (O42EvalContext *ctx, O42Operand *args, int n)
{
  double serial, type = 1;
  int iso;

  ARG_NUMBER (0, serial);
  if (n >= 2)
    ARG_NUMBER (1, type);

  if (serial < 0)
    return o42_value_error (O42_ERR_NUM);

  iso = o42_date_weekday (serial);      /* Monday 1 .. Sunday 7 */

  switch ((int) type)
    {
    case 1:  return o42_value_number (iso % 7 + 1);   /* Sunday 1 .. Saturday 7 */
    case 2:  return o42_value_number (iso);           /* Monday 1 .. Sunday 7 */
    case 3:  return o42_value_number (iso - 1);       /* Monday 0 .. Sunday 6 */
    case 11: case 12: case 13: case 14: case 15: case 16: case 17:
      /* The week starting on Monday (11) through Sunday (17), that
       * day counted 1. */
      return o42_value_number ((iso - ((int) type - 11) + 6) % 7 + 1);
    default: return o42_value_error (O42_ERR_NUM);
    }
}

static O42Value
fn_edate_eomonth (O42EvalContext *ctx, O42Operand *args, int n, gboolean end)
{
  double serial, months, result;
  int y, m, d;
  (void) n;
  ARG_NUMBER (0, serial);
  ARG_NUMBER (1, months);

  if (serial < 0 || !o42_date_from_serial (serial, &y, &m, &d))
    return o42_value_error (O42_ERR_NUM);

  if (end)
    result = o42_date_serial (y, m + (int) months + 1, 0);
  else
    {
      /* The same day that many months on, or the last day of the month
       * if it has no such day: 31 January plus a month is 28 February. */
      double first = o42_date_serial (y, m + (int) months, 1);
      double last = o42_date_serial (y, m + (int) months + 1, 0);
      result = MIN (first + d - 1, last);
    }

  if (result < 0 || result > 2958465)
    return o42_value_error (O42_ERR_NUM);   /* before 1900 or past 9999 */
  return o42_value_number (result);
}

static O42Value fn_edate   (O42EvalContext *c, O42Operand *a, int n) { return fn_edate_eomonth (c, a, n, FALSE); }
static O42Value fn_eomonth (O42EvalContext *c, O42Operand *a, int n) { return fn_edate_eomonth (c, a, n, TRUE); }

/* The US (NASD) method: months of thirty days, with the end-of-month
 * adjustments the bond markets settled on. */
static O42Value
fn_days360 (O42EvalContext *ctx, O42Operand *args, int n)
{
  double s1, s2;
  int y1, m1, d1, y2, m2, d2;
  gboolean european = FALSE;

  ARG_NUMBER (0, s1);
  ARG_NUMBER (1, s2);
  if (n >= 3)
    {
      O42Value v = operand_value (ctx, &args[2]);
      O42ErrorCode err = O42_ERR_VALUE;
      gboolean ok = o42_value_to_bool (&v, &european, &err);
      o42_value_clear (&v);
      if (!ok) return o42_value_error (err);
    }

  if (!o42_date_from_serial (s1, &y1, &m1, &d1) ||
      !o42_date_from_serial (s2, &y2, &m2, &d2))
    return o42_value_error (O42_ERR_NUM);

  if (european)
    {
      if (d1 == 31) d1 = 30;
      if (d2 == 31) d2 = 30;
    }
  else
    {
      gboolean d1_last = (d1 == 31) ||
        (m1 == 2 && d1 >= 28 && o42_date_serial (y1, 3, 0) == floor (s1));

      if (d1_last) d1 = 30;
      if (d2 == 31 && d1 == 30) d2 = 30;
    }

  return o42_value_number ((y2 - y1) * 360 + (m2 - m1) * 30 + (d2 - d1));
}

static O42Value
fn_days (O42EvalContext *ctx, O42Operand *args, int n)
{
  double end, start;
  (void) n;
  ARG_NUMBER (0, end);
  ARG_NUMBER (1, start);
  return o42_value_number (floor (end) - floor (start));
}
/* ---- More dates -------------------------------------------------------- */

static gboolean
is_weekend (double serial)
{
  int wd = o42_date_weekday (serial);    /* Monday 1 .. Sunday 7 */
  return wd >= 6;
}

/* Is the day one of the holidays in the optional range? */
gboolean
o42_is_holiday (O42EvalContext *ctx, const O42Operand *holidays, double serial)
{
  const O42Range *r;

  if (holidays == NULL)
    return FALSE;
  if (!holidays->is_range)
    {
      /* One date, as WORKDAY(A1, 5, DATE(2021,12,27)) gives it. */
      O42Value v = o42_operand_value (ctx, holidays);
      gboolean hit = v.type == O42_VALUE_NUMBER && floor (v.as.number) == floor (serial);
      o42_value_clear (&v);
      return hit;
    }

  r = &holidays->range;
  for (int row = r->row0; row <= r->row1; row++)
    for (int col = r->col0; col <= r->col1; col++)
      {
        O42Value v;
        gboolean hit;

        ctx->get_cell (ctx, holidays->sheet, row, col, &v);
        hit = (v.type == O42_VALUE_NUMBER && floor (v.as.number) == floor (serial));
        o42_value_clear (&v);
        if (hit)
          return TRUE;
      }

  return FALSE;
}

static O42Value
fn_networkdays (O42EvalContext *ctx, O42Operand *args, int n)
{
  double start, end;
  int count = 0, sign = 1;

  ARG_NUMBER (0, start);
  ARG_NUMBER (1, end);
  start = floor (start);
  end = floor (end);
  if (end < start) { double t = start; start = end; end = t; sign = -1; }

  for (double d = start; d <= end; d += 1)
    if (!is_weekend (d) && !is_holiday (ctx, n >= 3 ? &args[2] : NULL, d))
      count++;

  return o42_value_number (sign * count);
}

static O42Value
fn_workday (O42EvalContext *ctx, O42Operand *args, int n)
{
  double start, days, d;
  int step;

  ARG_NUMBER (0, start);
  ARG_NUMBER (1, days);
  d = floor (start);
  step = (days < 0) ? -1 : 1;
  days = fabs (trunc (days));

  while (days > 0)
    {
      d += step;
      if (d < 0 || d > 2958465)
        return o42_value_error (O42_ERR_NUM);   /* past 9999-12-31, or before 1900 */
      if (!is_weekend (d) && !is_holiday (ctx, n >= 3 ? &args[2] : NULL, d))
        days--;
    }

  return o42_value_number (d);
}

const O42Function O42_FUNCS_DATES[] = {
  { "DATE", 3, 3, fn_date },
  { "DATEVALUE", 1, 1, fn_datevalue },
  { "DAY", 1, 1, fn_day },
  { "DAYS", 2, 2, fn_days },
  { "DAYS360", 2, 3, fn_days360 },
  { "EDATE", 2, 2, fn_edate },
  { "EOMONTH", 2, 2, fn_eomonth },
  { "HOUR", 1, 1, fn_hour },
  { "MINUTE", 1, 1, fn_minute },
  { "MONTH", 1, 1, fn_month },
  { "NOW", 0, 0, fn_now },
  { "SECOND", 1, 1, fn_second },
  { "TIME", 3, 3, fn_time },
  { "TIMEVALUE", 1, 1, fn_timevalue },
  { "TODAY", 0, 0, fn_today },
  { "WEEKDAY", 1, 2, fn_weekday },
  { "YEAR", 1, 1, fn_year },
  { "NETWORKDAYS", 2, 3, fn_networkdays },
  { "WORKDAY", 2, 3, fn_workday },
  { NULL, 0, 0, NULL }
};

const O42FunctionHelp O42_HELP_DATES[] = {
  { "DATE", "DATE(year, month, day)", N_("The serial number of a date; months and days roll over.") },
  { "DATEVALUE", "DATEVALUE(text)", N_("The serial number of a date written as text.") },
  { "DAY", "DAY(serial)", N_("The day of the month, 1 to 31.") },
  { "DAYS", "DAYS(end_date, start_date)", N_("The number of days between two dates.") },
  { "DAYS360", "DAYS360(start_date, end_date, method)", N_("Days between two dates on a 360-day year.") },
  { "EDATE", "EDATE(start_date, months)", N_("The date a number of months before or after another.") },
  { "EOMONTH", "EOMONTH(start_date, months)", N_("The last day of the month a number of months away.") },
  { "HOUR", "HOUR(serial)", N_("The hour of a time, 0 to 23.") },
  { "MINUTE", "MINUTE(serial)", N_("The minute of a time, 0 to 59.") },
  { "MONTH", "MONTH(serial)", N_("The month of a date, 1 to 12.") },
  { "NOW", "NOW()", N_("The current date and time.") },
  { "SECOND", "SECOND(serial)", N_("The second of a time, 0 to 59.") },
  { "TIME", "TIME(hour, minute, second)", N_("The fraction of a day for a time.") },
  { "TIMEVALUE", "TIMEVALUE(text)", N_("The fraction of a day for a time written as text.") },
  { "TODAY", "TODAY()", N_("Today's date.") },
  { "WEEKDAY", "WEEKDAY(serial, type)", N_("The day of the week as a number.") },
  { "YEAR", "YEAR(serial)", N_("The year of a date.") },
  { "NETWORKDAYS", "NETWORKDAYS(start_date, end_date, holidays)", N_("The working days between two dates, weekends and holidays left out.") },
  { "WORKDAY", "WORKDAY(start_date, days, holidays)", N_("The date a number of working days away.") },
  { NULL, NULL, NULL }
};
