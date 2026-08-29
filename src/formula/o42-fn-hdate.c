/* o42-fn-hdate.c - see o42-eval-private.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-eval-private.h"

#include "o42-date.h"
#include "o42-hdate.h"
#include "o42-numfmt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The names this file has always called them by. */
#define operand_value o42_operand_value
#define normal_cdf o42_normal_cdf
#define normal_pdf o42_normal_pdf
#define collect_numbers o42_collect_numbers

/* ---- The Hebrew calendar, as Gnumeric's HDATE functions give it ------- */

/* Every one of these takes either a serial date or a year, a month and
 * a day; this reads whichever was given and answers the absolute day. */
static gboolean
hdate_absolute (O42EvalContext *ctx, O42Operand *args, int n, gint64 *out,
                O42ErrorCode *err)
{
  int y = 0, m = 1, d = 1;

  *err = O42_ERR_VALUE;
  if (n >= 3)
    {
      O42Value v0 = operand_value (ctx, &args[0]);
      O42Value v1 = operand_value (ctx, &args[1]);
      O42Value v2 = operand_value (ctx, &args[2]);
      gboolean ok = v0.type != O42_VALUE_ERROR && v1.type != O42_VALUE_ERROR &&
                    v2.type != O42_VALUE_ERROR;

      if (ok)
        {
          double a = 0, b = 0, c = 0;
          O42ErrorCode e = O42_ERR_VALUE;

          ok = o42_value_to_number (&v0, &a, &e) &&
               o42_value_to_number (&v1, &b, &e) &&
               o42_value_to_number (&v2, &c, &e);
          y = (int) a; m = (int) b; d = (int) c;
        }
      o42_value_clear (&v0);
      o42_value_clear (&v1);
      o42_value_clear (&v2);
      if (!ok)
        return FALSE;
    }
  else if (n >= 1)
    {
      O42Value v = operand_value (ctx, &args[0]);
      double serial = 0;
      O42ErrorCode e = O42_ERR_VALUE;

      if (!o42_value_to_number (&v, &serial, &e))
        { o42_value_clear (&v); return FALSE; }
      o42_value_clear (&v);
      if (!o42_date_from_serial (serial, &y, &m, &d))
        { *err = O42_ERR_NUM; return FALSE; }
    }
  else
    return FALSE;

  if (y < 1 || m < 1 || m > 12 || d < 1 || d > 31)
    { *err = O42_ERR_NUM; return FALSE; }
  *out = o42_gregorian_absolute (y, m, d);
  return TRUE;
}

/* HDATE, and the three that pick one field out of it. */
static O42Value
hdate_part (O42EvalContext *ctx, O42Operand *args, int n, int what)
{
  gint64 absolute;
  O42ErrorCode err;
  int hy = 0, hm = 1, hd = 1;

  if (!hdate_absolute (ctx, args, n, &absolute, &err))
    return o42_value_error (err);
  o42_hebrew_from_absolute (absolute, &hy, &hm, &hd);

  switch (what)
    {
    case 1: return o42_value_number (hy);
    case 2: return o42_value_number (hm);
    case 3: return o42_value_number (hd);
    case 4: return o42_value_number ((double) (absolute + 1721425));
    default:
      {
        const char *name = o42_hebrew_month_name (hy, hm);

        return o42_value_take (g_strdup_printf ("%d %s %d", hd,
                                                name != NULL ? name : "", hy));
      }
    }
}

static O42Value fn_hdate       (O42EvalContext *c, O42Operand *a, int n) { return hdate_part (c, a, n, 0); }
static O42Value fn_hdate_year  (O42EvalContext *c, O42Operand *a, int n) { return hdate_part (c, a, n, 1); }
static O42Value fn_hdate_month (O42EvalContext *c, O42Operand *a, int n) { return hdate_part (c, a, n, 2); }
static O42Value fn_hdate_day   (O42EvalContext *c, O42Operand *a, int n) { return hdate_part (c, a, n, 3); }
static O42Value fn_hdate_julian (O42EvalContext *c, O42Operand *a, int n) { return hdate_part (c, a, n, 4); }

/* The same from a serial date, which is what a cell holds. */
static O42Value
fn_date2hdate (O42EvalContext *ctx, O42Operand *args, int n)
{
  return hdate_part (ctx, args, n, 0);
}

/* And the other way: a Hebrew date to a Julian day, or to the serial
 * date a cell can do arithmetic with. */
static O42Value
hebrew_to (O42EvalContext *ctx, O42Operand *args, int n, gboolean julian)
{
  double y, m, d;
  gint64 absolute;
  int gy = 0, gm = 1, gd = 1;

  (void) n;
  ARG_NUMBER (0, y);
  ARG_NUMBER (1, m);
  ARG_NUMBER (2, d);
  if (y < 1 || m < 1 || m > 13 || d < 1 || d > 30)
    return o42_value_error (O42_ERR_NUM);
  absolute = o42_hebrew_absolute ((int) y, (int) m, (int) d);
  if (julian)
    return o42_value_number ((double) (absolute + 1721425));
  o42_gregorian_from_absolute (absolute, &gy, &gm, &gd);
  return o42_value_number (o42_date_serial (gy, gm, gd));
}

static O42Value fn_hdate2julian (O42EvalContext *c, O42Operand *a, int n) { return hebrew_to (c, a, n, TRUE); }
static O42Value fn_hdate2date   (O42EvalContext *c, O42Operand *a, int n) { return hebrew_to (c, a, n, FALSE); }

const O42Function O42_FUNCS_HDATE[] = {
  { "DATE2HDATE", 1, 3, fn_date2hdate },
  { "HDATE", 1, 3, fn_hdate },
  { "HDATE2DATE", 3, 3, fn_hdate2date },
  { "HDATE2JULIAN", 3, 3, fn_hdate2julian },
  { "HDATE_DAY", 1, 3, fn_hdate_day },
  { "HDATE_JULIAN", 1, 3, fn_hdate_julian },
  { "HDATE_MONTH", 1, 3, fn_hdate_month },
  { "HDATE_YEAR", 1, 3, fn_hdate_year },
  { NULL, 0, 0, NULL }
};

const O42FunctionHelp O42_HELP_HDATE[] = {
  { "DATE2HDATE", "DATE2HDATE(date)", "The Hebrew date a serial date falls on, written out." },
  { "HDATE", "HDATE(year, month, day)", "The Hebrew date a Gregorian one falls on, written out." },
  { "HDATE2DATE", "HDATE2DATE(year, month, day)", "The serial date a Hebrew date falls on." },
  { "HDATE2JULIAN", "HDATE2JULIAN(year, month, day)", "The Julian day a Hebrew date falls on." },
  { "HDATE_DAY", "HDATE_DAY(year, month, day)", "The day of the Hebrew month." },
  { "HDATE_JULIAN", "HDATE_JULIAN(year, month, day)", "The Julian day of a Gregorian date." },
  { "HDATE_MONTH", "HDATE_MONTH(year, month, day)", "The Hebrew month, counting Tishri as one." },
  { "HDATE_YEAR", "HDATE_YEAR(year, month, day)", "The Hebrew year." },
  { NULL, NULL, NULL }
};
