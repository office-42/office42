/* o42-fn-finance.c - see o42-eval-private.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-eval-private.h"

#include "o42-date.h"
#include "o42-numfmt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The names this file has always called them by. */
#define operand_value o42_operand_value
#define normal_cdf o42_normal_cdf
#define normal_pdf o42_normal_pdf
#define collect_numbers o42_collect_numbers

/* ---- Bonds: coupon dates, discounted paper and Treasury bills ------- */

/* The day-count bases are YEARFRAC's: 0 US 30/360, 1 actual/actual,
 * 2 actual/360, 3 actual/365, 4 European 30/360. */

/* Days between two serial dates on a 30/360 count, US or European. */
static double
days_360 (double s1, double s2, gboolean european)
{
  int y1, m1, d1, y2, m2, d2;

  if (!o42_date_from_serial (s1, &y1, &m1, &d1) ||
      !o42_date_from_serial (s2, &y2, &m2, &d2))
    return 0;
  if (european)
    {
      if (d1 == 31) d1 = 30;
      if (d2 == 31) d2 = 30;
    }
  else
    {
      if (d1 == 31) d1 = 30;
      if (d2 == 31 && d1 >= 30) d2 = 30;
    }
  return (y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1);
}

/* Days between two dates on the given basis. */
static double
basis_days (double s1, double s2, int basis)
{
  if (basis == 0)
    return days_360 (s1, s2, FALSE);
  if (basis == 4)
    return days_360 (s1, s2, TRUE);
  return floor (s2) - floor (s1);
}

/* What a year counts as on the given basis.  Actual/actual has no fixed
 * answer, so the term is passed in and the average length of the years
 * it touches comes back -- which is what a leap day in the middle of
 * one comes to. */
static double
basis_year (double from, double to, int basis)
{
  int y1, m1, d1, y2, m2, d2;

  if (basis == 0 || basis == 2 || basis == 4)
    return 360;
  if (basis == 3)
    return 365;
  if (!o42_date_from_serial (from, &y1, &m1, &d1) ||
      !o42_date_from_serial (to, &y2, &m2, &d2))
    return 365;

  /* Actual/actual counts a year as 366 days when the term is a year or
   * less and a 29 February falls inside it, and 365 when it does not;
   * only a term longer than a year takes the average of the years it
   * touches. */
  if (floor (to) - floor (from) <= 365)
    {
      /* A term inside one leap year counts 366 whether or not it takes
       * in the leap day itself. */
      if (y1 == y2 && o42_date_serial (y1, 3, 0) == o42_date_serial (y1, 2, 29))
        return 366;
      for (int y = y1; y <= y2; y++)
        {
          double leap_day = o42_date_serial (y, 2, 29);

          if (o42_date_serial (y, 3, 0) == leap_day &&
              leap_day >= floor (from) && leap_day <= floor (to))
            return 366;
        }
      return 365;
    }
  {
    double first = o42_date_serial (y1, 1, 1);
    double after = o42_date_serial (y2 + 1, 1, 1);

    return (after - first) / (y2 - y1 + 1);
  }
}

static gboolean
bond_basis_ok (double basis)
{
  return basis >= 0 && basis <= 4;
}

static gboolean
bond_frequency_ok (double frequency)
{
  return frequency == 1 || frequency == 2 || frequency == 4;
}

/* `months` months on from a date, keeping the day of the month where
 * the shorter month has one and taking its last day where it does not:
 * a bond whose coupons fall on the 31st pays on the 28th in February. */
static double
add_months (double serial, int months)
{
  int y, m, d, total;
  double last, wanted;

  if (!o42_date_from_serial (serial, &y, &m, &d))
    return serial;
  total = y * 12 + (m - 1) + months;
  y = total / 12;
  m = total % 12 + 1;
  last = o42_date_serial (y, m + 1, 0);
  wanted = o42_date_serial (y, m, d);
  return wanted > last ? last : wanted;
}

/* How many whole coupon periods back from maturity the settlement lies:
 * the coupon dates are counted from maturity, never from issue, which
 * is what makes an odd first period odd and the rest regular. */
static int
coupon_steps (double settlement, double maturity, int frequency)
{
  int step = 12 / frequency;
  int k = 0;

  while (add_months (maturity, -step * k) > settlement && k < 4000)
    k++;
  return k;
}

/* The six COUP functions, which differ only in what they report. */
static O42Value
coupon_call (O42EvalContext *ctx, O42Operand *args, int n, int which)
{
  double settlement, maturity, frequency, basis = 0;
  double previous, next;
  int step, k;

  ARG_NUMBER (0, settlement);
  ARG_NUMBER (1, maturity);
  ARG_NUMBER (2, frequency);
  if (n >= 4)
    ARG_NUMBER (3, basis);
  settlement = floor (settlement);
  maturity = floor (maturity);
  basis = floor (basis);
  if (settlement >= maturity || !bond_frequency_ok (frequency) || !bond_basis_ok (basis))
    return o42_value_error (O42_ERR_NUM);

  step = 12 / (int) frequency;
  k = coupon_steps (settlement, maturity, (int) frequency);
  previous = add_months (maturity, -step * k);
  next = add_months (maturity, -step * (k - 1));

  switch (which)
    {
    case 0:   /* COUPPCD */
      return o42_value_number (previous);
    case 1:   /* COUPNCD */
      return o42_value_number (next);
    case 2:   /* COUPNUM: the coupons still to be paid */
      return o42_value_number (k);
    case 3:   /* COUPDAYBS: from the period's start to the settlement */
      return o42_value_number (basis_days (previous, settlement, (int) basis));
    case 4:   /* COUPDAYSNC: from the settlement to the next coupon */
      if ((int) basis == 0 || (int) basis == 4)
        return o42_value_number (basis_days (settlement, next, (int) basis));
      return o42_value_number (next - settlement);
    default:  /* COUPDAYS: the whole period the settlement falls in */
      if ((int) basis == 1)
        return o42_value_number (next - previous);
      if ((int) basis == 3)
        return o42_value_number (365.0 / frequency);
      return o42_value_number (360.0 / frequency);
    }
}

static O42Value fn_couppcd    (O42EvalContext *ctx, O42Operand *a, int n) { return coupon_call (ctx, a, n, 0); }
static O42Value fn_coupncd    (O42EvalContext *ctx, O42Operand *a, int n) { return coupon_call (ctx, a, n, 1); }
static O42Value fn_coupnum    (O42EvalContext *ctx, O42Operand *a, int n) { return coupon_call (ctx, a, n, 2); }
static O42Value fn_coupdaybs  (O42EvalContext *ctx, O42Operand *a, int n) { return coupon_call (ctx, a, n, 3); }
static O42Value fn_coupdaysnc (O42EvalContext *ctx, O42Operand *a, int n) { return coupon_call (ctx, a, n, 4); }
static O42Value fn_coupdays   (O42EvalContext *ctx, O42Operand *a, int n) { return coupon_call (ctx, a, n, 5); }

/* The five that price paper sold at a discount, or invested whole.
 * They share their arguments and differ in one line of arithmetic. */
static O42Value
discount_call (O42EvalContext *ctx, O42Operand *args, int n, int which)
{
  double settlement, maturity, third, fourth, basis = 0;
  double year, days;

  ARG_NUMBER (0, settlement);
  ARG_NUMBER (1, maturity);
  ARG_NUMBER (2, third);
  ARG_NUMBER (3, fourth);
  if (n >= 5)
    ARG_NUMBER (4, basis);
  settlement = floor (settlement);
  maturity = floor (maturity);
  basis = floor (basis);
  if (third <= 0 || fourth <= 0 || settlement >= maturity || !bond_basis_ok (basis))
    return o42_value_error (O42_ERR_NUM);

  year = basis_year (settlement, maturity, (int) basis);
  days = basis_days (settlement, maturity, (int) basis);
  if (days == 0 || year == 0)
    return o42_value_error (O42_ERR_DIV0);

  switch (which)
    {
    case 0:   /* DISC (pr, redemption) */
      return o42_value_number ((fourth - third) / fourth * (year / days));
    case 1:   /* PRICEDISC (discount, redemption) */
      return o42_value_number (fourth - third * fourth * days / year);
    case 2:   /* YIELDDISC (pr, redemption) */
      return o42_value_number ((fourth - third) / third * (year / days));
    case 3:   /* INTRATE (investment, redemption) */
      return o42_value_number ((fourth - third) / third * (year / days));
    default:  /* RECEIVED (investment, discount) */
      {
        double denominator = 1 - fourth * days / year;

        if (denominator == 0)
          return o42_value_error (O42_ERR_DIV0);
        return o42_value_number (third / denominator);
      }
    }
}

static O42Value fn_disc      (O42EvalContext *ctx, O42Operand *a, int n) { return discount_call (ctx, a, n, 0); }
static O42Value fn_pricedisc (O42EvalContext *ctx, O42Operand *a, int n) { return discount_call (ctx, a, n, 1); }
static O42Value fn_yielddisc (O42EvalContext *ctx, O42Operand *a, int n) { return discount_call (ctx, a, n, 2); }
static O42Value fn_intrate   (O42EvalContext *ctx, O42Operand *a, int n) { return discount_call (ctx, a, n, 3); }
static O42Value fn_received  (O42EvalContext *ctx, O42Operand *a, int n) { return discount_call (ctx, a, n, 4); }

/* The days a Treasury bill runs: a 30/360 count that takes the last
 * day of February for the thirtieth and counts both ends, which is
 * what makes a bill dated the last of March and due the first of June
 * run 62 days rather than 61. */
static double
tbill_days (double settlement, double maturity)
{
  int y1, m1, d1, y2, m2, d2;

  if (!o42_date_from_serial (settlement, &y1, &m1, &d1) ||
      !o42_date_from_serial (maturity, &y2, &m2, &d2))
    return 0;
  if (d1 == 31 || (m1 == 2 && settlement == o42_date_serial (y1, 3, 0)))
    d1 = 30;
  if (d2 == 31 || (m2 == 2 && maturity == o42_date_serial (y2, 3, 0)))
    d2 = 30;
  return (y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1) + 1;
}

/* The three Treasury bill functions.  A bill counts on a 360 day year
 * and never runs more than a year, which is why they are apart from
 * the rest. */
static O42Value
tbill_call (O42EvalContext *ctx, O42Operand *args, int n, int which)
{
  double settlement, maturity, third, days;

  (void) n;
  ARG_NUMBER (0, settlement);
  ARG_NUMBER (1, maturity);
  ARG_NUMBER (2, third);
  settlement = floor (settlement);
  maturity = floor (maturity);
  days = tbill_days (settlement, maturity);
  if (third <= 0 || maturity <= settlement || days > 365)
    return o42_value_error (O42_ERR_NUM);

  if (which == 0)   /* TBILLPRICE (discount) */
    return o42_value_number (100 * (1 - third * days / 360));
  if (which == 1)   /* TBILLYIELD (price) */
    return o42_value_number ((100 - third) / third * (360 / days));

  /* TBILLEQ (discount): the yield a coupon bond would have to pay to
   * match the bill, on a 365 day year. */
  {
    double denominator = 360 - third * days;

    if (denominator == 0)
      return o42_value_error (O42_ERR_DIV0);
    return o42_value_number (365 * third / denominator);
  }
}

static O42Value fn_tbillprice (O42EvalContext *ctx, O42Operand *a, int n) { return tbill_call (ctx, a, n, 0); }
static O42Value fn_tbillyield (O42EvalContext *ctx, O42Operand *a, int n) { return tbill_call (ctx, a, n, 1); }
static O42Value fn_tbilleq    (O42EvalContext *ctx, O42Operand *a, int n) { return tbill_call (ctx, a, n, 2); }

/* FVSCHEDULE: a principal grown by a series of different rates. */
static O42Value
fn_fvschedule (O42EvalContext *ctx, O42Operand *args, int n)
{
  double principal, total;
  GArray *rates;
  O42ErrorCode err = O42_ERR_VALUE;

  ARG_NUMBER (0, principal);
  if (!collect_numbers (ctx, args + 1, n - 1, &rates, &err))
    return o42_value_error (err);

  total = principal;
  for (guint i = 0; i < rates->len; i++)
    total *= 1 + g_array_index (rates, double, i);
  g_array_free (rates, TRUE);
  return o42_value_number (total);
}

static double o42_bond_yearfrac (double from, double to, int basis);

/* ---- The odd first and last periods, and French depreciation -------- */

/* ODDLPRICE and ODDLYIELD: a bond whose last period is not a whole
 * one.  Everything before that last period is regular, so the price is
 * the redemption and the coupons discounted over the odd stub. */
static O42Value
oddl_call (O42EvalContext *ctx, O42Operand *args, int n, gboolean want_yield)
{
  double settlement, maturity, last, rate, sixth, redemption, frequency, basis = 0;
  double dsc, dcs, a, e, coupon, factor;

  ARG_NUMBER (0, settlement);
  ARG_NUMBER (1, maturity);
  ARG_NUMBER (2, last);
  ARG_NUMBER (3, rate);
  ARG_NUMBER (4, sixth);          /* the yield, or the price */
  ARG_NUMBER (5, redemption);
  ARG_NUMBER (6, frequency);
  if (n >= 8)
    ARG_NUMBER (7, basis);
  settlement = floor (settlement);
  maturity = floor (maturity);
  last = floor (last);
  basis = floor (basis);
  if (settlement >= maturity || last >= settlement || rate < 0 || redemption <= 0 ||
      !bond_frequency_ok (frequency) || !bond_basis_ok (basis))
    return o42_value_error (O42_ERR_NUM);

  /* The odd last period runs from the last coupon to maturity; the
   * settlement sits inside it. */
  e = basis_days (last, maturity, (int) basis);
  a = basis_days (last, settlement, (int) basis);
  dsc = basis_days (settlement, maturity, (int) basis);
  dcs = basis_year (last, maturity, (int) basis) / frequency;
  if (dcs == 0 || e == 0)
    return o42_value_error (O42_ERR_DIV0);

  coupon = 100 * rate / frequency * (e / dcs);
  factor = dsc / dcs;
  if (want_yield)
    {
      double price = sixth;
      double accrued = 100 * rate / frequency * (a / dcs);

      if (price + accrued == 0 || factor == 0)
        return o42_value_error (O42_ERR_DIV0);
      return o42_value_number ((redemption + coupon - (price + accrued))
                               / (price + accrued) * frequency / factor);
    }
  {
    double yld = sixth;
    double accrued = 100 * rate / frequency * (a / dcs);
    double bottom = factor * yld / frequency + 1;

    if (bottom == 0)
      return o42_value_error (O42_ERR_DIV0);
    return o42_value_number ((redemption + coupon) / bottom - accrued);
  }
}

static O42Value fn_oddlprice (O42EvalContext *ctx, O42Operand *a, int n) { return oddl_call (ctx, a, n, FALSE); }
static O42Value fn_oddlyield (O42EvalContext *ctx, O42Operand *a, int n) { return oddl_call (ctx, a, n, TRUE); }

/* ODDFPRICE and ODDFYIELD: a bond whose first period is not a whole
 * one.  The odd stub is discounted on its own and the regular coupons
 * behind it in the usual way. */
/* The days in a regular coupon period ending on `date`. */
static double
period_days (double date, int frequency, int basis)
{
  double previous = add_months (date, -(12 / frequency));

  if (basis == 1)
    return date - previous;
  if (basis == 3)
    return 365.0 / frequency;
  return 360.0 / frequency;
}

static double
oddf_price (double settlement, double maturity, double issue, double first,
            double rate, double yld, double redemption, double frequency, int basis)
{
  int step = 12 / (int) frequency;
  double e = period_days (first, (int) frequency, basis);
  double dfc = basis_days (issue, first, basis);   /* the odd first period */
  double coupon = 100 * rate / frequency;
  double discount = 1 + yld / frequency;
  double dsc, a, price;
  int coupons;

  if (e <= 0)
    return NAN;

  if (settlement < first)
    {
      /* Still inside the odd period: the first coupon is short, and
       * the seller has earned the days since the issue. */
      dsc = basis_days (settlement, first, basis);
      a = basis_days (issue, settlement, basis);
      coupons = coupon_steps (settlement, maturity, (int) frequency);
      price = redemption / pow (discount, coupons - 1 + dsc / e);
      price += coupon * (dfc / e) / pow (discount, dsc / e);
      for (int k = 2; k <= coupons; k++)
        price += coupon / pow (discount, k - 1 + dsc / e);
      return price - coupon * (a / e);
    }
  {
    /* Past the first coupon the bond is a regular one. */
    int k = coupon_steps (settlement, maturity, (int) frequency);
    double previous = add_months (maturity, -step * k);
    double next = add_months (maturity, -step * (k - 1));
    double period = period_days (next, (int) frequency, basis);

    dsc = basis_days (settlement, next, basis);
    a = basis_days (previous, settlement, basis);
    price = redemption / pow (discount, k - 1 + dsc / period);
    for (int i = 1; i <= k; i++)
      price += coupon / pow (discount, i - 1 + dsc / period);
    return price - coupon * (a / period);
  }
}

static O42Value
oddf_call (O42EvalContext *ctx, O42Operand *args, int n, gboolean want_yield)
{
  double settlement, maturity, issue, first, rate, sixth, redemption, frequency, basis = 0;

  ARG_NUMBER (0, settlement);
  ARG_NUMBER (1, maturity);
  ARG_NUMBER (2, issue);
  ARG_NUMBER (3, first);
  ARG_NUMBER (4, rate);
  ARG_NUMBER (5, sixth);
  ARG_NUMBER (6, redemption);
  ARG_NUMBER (7, frequency);
  if (n >= 9)
    ARG_NUMBER (8, basis);
  settlement = floor (settlement);
  maturity = floor (maturity);
  issue = floor (issue);
  first = floor (first);
  basis = floor (basis);
  if (settlement >= maturity || issue >= settlement || first >= maturity ||
      rate < 0 || redemption <= 0 ||
      !bond_frequency_ok (frequency) || !bond_basis_ok (basis))
    return o42_value_error (O42_ERR_NUM);

  if (!want_yield)
    return o42_value_number (oddf_price (settlement, maturity, issue, first, rate,
                                         sixth, redemption, frequency, (int) basis));
  {
    /* The price falls as the yield rises, so bisection finds it. */
    double low = -0.99, high = 100.0;

    for (int i = 0; i < 200; i++)
      {
        double mid = (low + high) / 2;
        double price = oddf_price (settlement, maturity, issue, first, rate, mid,
                                   redemption, frequency, (int) basis);

        if (isnan (price))
          return o42_value_error (O42_ERR_NUM);
        if (price > sixth)
          low = mid;
        else
          high = mid;
      }
    return o42_value_number ((low + high) / 2);
  }
}

static O42Value fn_oddfprice (O42EvalContext *ctx, O42Operand *a, int n) { return oddf_call (ctx, a, n, FALSE); }
static O42Value fn_oddfyield (O42EvalContext *ctx, O42Operand *a, int n) { return oddf_call (ctx, a, n, TRUE); }

/* AMORLINC and AMORDEGRC: the French accounting depreciations, where
 * an asset bought part way through a period is written down for the
 * part of the period it was owned. */
static O42Value
amor_call (O42EvalContext *ctx, O42Operand *args, int n, gboolean degressive)
{
  double cost, purchased, first_period, salvage, period, rate, basis = 0;
  double first_amount, fraction;

  ARG_NUMBER (0, cost);
  ARG_NUMBER (1, purchased);
  ARG_NUMBER (2, first_period);
  ARG_NUMBER (3, salvage);
  ARG_NUMBER (4, period);
  ARG_NUMBER (5, rate);
  if (n >= 7)
    ARG_NUMBER (6, basis);
  purchased = floor (purchased);
  first_period = floor (first_period);
  basis = floor (basis);
  if (cost <= 0 || salvage < 0 || salvage > cost || rate <= 0 || period < 0 ||
      purchased > first_period || !bond_basis_ok (basis))
    return o42_value_error (O42_ERR_NUM);

  /* The first period is short: only the days from the purchase to the
   * end of it count. */
  fraction = o42_bond_yearfrac (purchased, first_period, (int) basis);

  if (!degressive)
    {
      /* Straight line: the same amount every full period, whatever is
       * left over in the one after them, and nothing after that. */
      double each = cost * rate;
      double writable = cost - salvage;
      int full;

      first_amount = fraction * rate * cost;
      if (period == 0)
        return o42_value_number (first_amount);
      if (each == 0)
        return o42_value_error (O42_ERR_DIV0);
      full = (int) ((writable - first_amount) / each);
      if (period <= full)
        return o42_value_number (each);
      if (period == full + 1)
        return o42_value_number (writable - each * full - first_amount);
      return o42_value_number (0);
    }

  {
    /* Degressive: a coefficient on the rate that depends on how long
     * the asset is expected to last, whole francs, and the last of it
     * written off in halves once the book value would pass the
     * salvage. */
    double life = 1 / rate;
    double coefficient = life < 3 ? 1 : life <= 4 ? 1.5 : life <= 6 ? 2 : 2.5;
    double left, amount;
    int periods = (int) period;

    rate *= coefficient;
    first_amount = floor (fraction * rate * cost + 0.5);
    if (period == 0)
      return o42_value_number (first_amount);

    cost -= first_amount;
    left = cost - salvage;
    amount = first_amount;
    for (int i = 0; i < periods; i++)
      {
        amount = floor (rate * cost + 0.5);
        left -= amount;
        if (left < 0)
          amount = (periods - i <= 1) ? floor (cost * 0.5 + 0.5) : 0;
        cost -= amount;
      }
    return o42_value_number (amount);
  }
}

static O42Value fn_amorlinc  (O42EvalContext *ctx, O42Operand *a, int n) { return amor_call (ctx, a, n, FALSE); }
static O42Value fn_amordegrc (O42EvalContext *ctx, O42Operand *a, int n) { return amor_call (ctx, a, n, TRUE); }

/* BAHTTEXT: a number written out in Thai, in baht and satang.  The
 * rules are the ones Thai counting has always had: the one in the
 * units place of a compound number is "et" rather than "nueng", the
 * two in the tens place is "yi" rather than "song", and the ten itself
 * needs no "nueng" in front of it. */
static void
baht_group (GString *out, int number)
{
  static const char *const DIGITS[] = {
    "", "\u0e2b\u0e19\u0e36\u0e48\u0e07", "\u0e2a\u0e2d\u0e07", "\u0e2a\u0e32\u0e21",
    "\u0e2a\u0e35\u0e48", "\u0e2b\u0e49\u0e32", "\u0e2b\u0e01", "\u0e40\u0e08\u0e47\u0e14",
    "\u0e41\u0e1b\u0e14", "\u0e40\u0e01\u0e49\u0e32"
  };
  static const char *const PLACES[] = {
    "", "\u0e2a\u0e34\u0e1a", "\u0e23\u0e49\u0e2d\u0e22", "\u0e1e\u0e31\u0e19",
    "\u0e2b\u0e21\u0e37\u0e48\u0e19", "\u0e41\u0e2a\u0e19"
  };
  int digits[6];
  int count = 0;

  if (number == 0)
    return;
  while (number > 0 && count < 6)
    {
      digits[count++] = number % 10;
      number /= 10;
    }
  for (int i = count - 1; i >= 0; i--)
    {
      int d = digits[i];

      if (d == 0)
        continue;
      if (i == 1 && d == 1)
        g_string_append (out, PLACES[1]);            /* ten, not one ten */
      else if (i == 1 && d == 2)
        g_string_append (out, "\u0e22\u0e35\u0e48\u0e2a\u0e34\u0e1a");   /* twenty */
      else if (i == 0 && d == 1 && count > 1)
        g_string_append (out, "\u0e40\u0e2d\u0e47\u0e14");                 /* and one */
      else
        {
          g_string_append (out, DIGITS[d]);
          g_string_append (out, PLACES[i]);
        }
    }
}

static O42Value
fn_bahttext (O42EvalContext *ctx, O42Operand *args, int n)
{
  double value;
  GString *out = g_string_new (NULL);
  gint64 baht;
  int satang;
  gboolean negative;

  (void) n;
  ARG_NUMBER (0, value);
  negative = value < 0;
  value = fabs (value);
  baht = (gint64) floor (value);
  satang = (int) floor ((value - baht) * 100 + 0.5);
  if (satang >= 100)
    { baht++; satang = 0; }

  if (negative)
    g_string_append (out, "\u0e25\u0e1a");        /* minus */

  if (baht == 0 && satang == 0)
    g_string_append (out, "\u0e28\u0e39\u0e19\u0e22\u0e4c");   /* zero */
  else
    {
      /* Thai counts in millions: everything above a million is said
       * first, then "lan", then the rest. */
      gint64 millions = baht / 1000000;

      if (millions > 0)
        {
          baht_group (out, (int) (millions % 1000000));
          g_string_append (out, "\u0e25\u0e49\u0e32\u0e19");
        }
      baht_group (out, (int) (baht % 1000000));
    }
  g_string_append (out, "\u0e1a\u0e32\u0e17");   /* baht */

  if (satang == 0)
    g_string_append (out, "\u0e16\u0e49\u0e27\u0e19");          /* exactly */
  else
    {
      baht_group (out, satang);
      g_string_append (out, "\u0e2a\u0e15\u0e32\u0e07\u0e04\u0e4c");   /* satang */
    }
  return o42_value_take (g_string_free (out, FALSE));
}

/* ASC and PHONETIC do nothing outside Japanese Excel, and BAHTTEXT
 * nothing outside Thai; the first two hand the text back and the
 * third spells a number the Thai way, which is what they are for. */
static O42Value
fn_asc (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *text = NULL;

  (void) n;
  ARG_TEXT (0, text);
  return o42_value_take (text);
}

static O42Value
fn_phonetic (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *text = NULL;

  (void) n;
  ARG_TEXT (0, text);
  return o42_value_take (text);
}

/* The term in years on the given basis: what YEARFRAC returns, in a
 * form the bond functions can call. */
static double
o42_bond_yearfrac (double from, double to, int basis)
{
  double days = basis_days (from, to, basis);
  double year = basis_year (from, to, basis);

  return year != 0 ? days / year : 0;
}

/* ---- Bonds: what one is worth, and what it yields ------------------- */

/* The three day counts a coupon bond's price is made of: the days
 * behind the settlement in its coupon period, the days ahead of it,
 * and the length of the period. */
typedef struct {
  int    coupons;    /* still to be paid */
  double behind;     /* COUPDAYBS */
  double ahead;      /* COUPDAYSNC */
  double period;     /* COUPDAYS */
} BondTerm;

static gboolean
bond_term (double settlement, double maturity, int frequency, int basis, BondTerm *term)
{
  int step = 12 / frequency;
  int k = coupon_steps (settlement, maturity, frequency);
  double previous = add_months (maturity, -step * k);
  double next = add_months (maturity, -step * (k - 1));

  term->coupons = k;
  term->behind = basis_days (previous, settlement, basis);
  term->ahead = (basis == 0 || basis == 4) ? basis_days (settlement, next, basis)
                                           : next - settlement;
  if (basis == 1)
    term->period = next - previous;
  else if (basis == 3)
    term->period = 365.0 / frequency;
  else
    term->period = 360.0 / frequency;
  return term->period > 0;
}

/* The price per 100 of a coupon bond at a given yield: the redemption
 * and every coupon discounted back to the settlement, less the
 * interest the seller has already earned in the running period. */
static double
bond_price (const BondTerm *term, double rate, double yld, double redemption,
            double frequency)
{
  double fraction = term->ahead / term->period;
  double coupon = 100 * rate / frequency;
  double discount = 1 + yld / frequency;
  double price;

  if (yld <= -frequency)
    return NAN;
  price = redemption / pow (discount, term->coupons - 1 + fraction);
  for (int k = 1; k <= term->coupons; k++)
    price += coupon / pow (discount, k - 1 + fraction);
  return price - coupon * term->behind / term->period;
}

static O42Value
fn_price (O42EvalContext *ctx, O42Operand *args, int n)
{
  double settlement, maturity, rate, yld, redemption, frequency, basis = 0;
  BondTerm term;
  double price;

  ARG_NUMBER (0, settlement);
  ARG_NUMBER (1, maturity);
  ARG_NUMBER (2, rate);
  ARG_NUMBER (3, yld);
  ARG_NUMBER (4, redemption);
  ARG_NUMBER (5, frequency);
  if (n >= 7)
    ARG_NUMBER (6, basis);
  settlement = floor (settlement);
  maturity = floor (maturity);
  basis = floor (basis);
  if (settlement >= maturity || rate < 0 || yld < 0 || redemption <= 0 ||
      !bond_frequency_ok (frequency) || !bond_basis_ok (basis))
    return o42_value_error (O42_ERR_NUM);
  if (!bond_term (settlement, maturity, (int) frequency, (int) basis, &term))
    return o42_value_error (O42_ERR_NUM);

  price = bond_price (&term, rate, yld, redemption, frequency);
  if (isnan (price))
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (price);
}

/* YIELD is PRICE read backwards.  The price falls as the yield rises,
 * so a bisection between nothing and a hundredfold return finds it
 * without ever running away, which Newton's method can do on a bond
 * priced far from par. */
static O42Value
fn_yield (O42EvalContext *ctx, O42Operand *args, int n)
{
  double settlement, maturity, rate, pr, redemption, frequency, basis = 0;
  BondTerm term;
  double low = -0.99, high = 100.0;

  ARG_NUMBER (0, settlement);
  ARG_NUMBER (1, maturity);
  ARG_NUMBER (2, rate);
  ARG_NUMBER (3, pr);
  ARG_NUMBER (4, redemption);
  ARG_NUMBER (5, frequency);
  if (n >= 7)
    ARG_NUMBER (6, basis);
  settlement = floor (settlement);
  maturity = floor (maturity);
  basis = floor (basis);
  if (settlement >= maturity || rate < 0 || pr <= 0 || redemption <= 0 ||
      !bond_frequency_ok (frequency) || !bond_basis_ok (basis))
    return o42_value_error (O42_ERR_NUM);
  if (!bond_term (settlement, maturity, (int) frequency, (int) basis, &term))
    return o42_value_error (O42_ERR_NUM);

  if (bond_price (&term, rate, low, redemption, frequency) < pr ||
      bond_price (&term, rate, high, redemption, frequency) > pr)
    return o42_value_error (O42_ERR_NUM);
  for (int i = 0; i < 200; i++)
    {
      double mid = (low + high) / 2;
      double price = bond_price (&term, rate, mid, redemption, frequency);

      if (isnan (price))
        return o42_value_error (O42_ERR_NUM);
      if (price > pr)
        low = mid;
      else
        high = mid;
    }
  return o42_value_number ((low + high) / 2);
}

/* A security that pays all its interest at maturity: the price, the
 * yield, and the interest itself. */
static O42Value
maturity_call (O42EvalContext *ctx, O42Operand *args, int n, int which)
{
  double settlement, maturity, issue, rate, fifth = 0, basis = 0;
  double year, dim, dis, dsm;

  ARG_NUMBER (0, settlement);
  ARG_NUMBER (1, maturity);
  ARG_NUMBER (2, issue);
  ARG_NUMBER (3, rate);
  if (which != 2)
    {
      ARG_NUMBER (4, fifth);
      if (n >= 6)
        ARG_NUMBER (5, basis);
    }
  else if (n >= 5)
    ARG_NUMBER (4, basis);
  settlement = floor (settlement);
  maturity = floor (maturity);
  issue = floor (issue);
  basis = floor (basis);
  if (settlement >= maturity || issue >= settlement || rate < 0 || !bond_basis_ok (basis))
    return o42_value_error (O42_ERR_NUM);

  /* Each span is measured in years in its own right, which matters on
   * the actual/actual basis where a year is not a fixed length. */
  year = 1;
  dim = o42_bond_yearfrac (issue, maturity, (int) basis);
  dis = o42_bond_yearfrac (issue, settlement, (int) basis);
  dsm = o42_bond_yearfrac (settlement, maturity, (int) basis);

  switch (which)
    {
    case 0:   /* PRICEMAT (yld) */
      {
        double bottom = 1 + dsm / year * fifth;

        if (bottom == 0)
          return o42_value_error (O42_ERR_DIV0);
        return o42_value_number ((100 + dim / year * rate * 100) / bottom
                                 - dis / year * rate * 100);
      }
    case 1:   /* YIELDMAT (pr) */
      {
        double bottom = fifth / 100 + dis / year * rate;

        if (bottom == 0 || dsm == 0)
          return o42_value_error (O42_ERR_DIV0);
        return o42_value_number (((1 + dim / year * rate) / bottom - 1) * year / dsm);
      }
    default:  /* ACCRINTM (par in `fifth` when given) */
      return o42_value_number ((fifth > 0 ? fifth : 1000) * rate * dim / year);
    }
}

static O42Value fn_pricemat (O42EvalContext *ctx, O42Operand *a, int n) { return maturity_call (ctx, a, n, 0); }
static O42Value fn_yieldmat (O42EvalContext *ctx, O42Operand *a, int n) { return maturity_call (ctx, a, n, 1); }

/* ACCRINTM(issue, settlement, rate, par, basis): all the interest a
 * pay-at-maturity security has earned. */
static O42Value
fn_accrintm (O42EvalContext *ctx, O42Operand *args, int n)
{
  double issue, settlement, rate, par = 1000, basis = 0;
  double year, days;

  ARG_NUMBER (0, issue);
  ARG_NUMBER (1, settlement);
  ARG_NUMBER (2, rate);
  if (n >= 4)
    ARG_NUMBER (3, par);
  if (n >= 5)
    ARG_NUMBER (4, basis);
  issue = floor (issue);
  settlement = floor (settlement);
  basis = floor (basis);
  if (issue >= settlement || rate <= 0 || par <= 0 || !bond_basis_ok (basis))
    return o42_value_error (O42_ERR_NUM);

  year = basis_year (issue, settlement, (int) basis);
  days = basis_days (issue, settlement, (int) basis);
  if (year == 0)
    return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (par * rate * days / year);
}

/* ACCRINT(issue, first_interest, settlement, rate, par, frequency,
 * basis, calc_method): the interest a coupon security has earned since
 * it was issued, or since its last coupon when calc_method is FALSE. */
static O42Value
fn_accrint (O42EvalContext *ctx, O42Operand *args, int n)
{
  double issue, first, settlement, rate, par = 1000, frequency = 1, basis = 0;
  double from_date, year, days;
  double method = 1;

  ARG_NUMBER (0, issue);
  ARG_NUMBER (1, first);
  ARG_NUMBER (2, settlement);
  ARG_NUMBER (3, rate);
  if (n >= 5)
    ARG_NUMBER (4, par);
  if (n >= 6)
    ARG_NUMBER (5, frequency);
  if (n >= 7)
    ARG_NUMBER (6, basis);
  if (n >= 8)
    ARG_NUMBER (7, method);
  issue = floor (issue);
  first = floor (first);
  settlement = floor (settlement);
  basis = floor (basis);
  if (issue >= settlement || rate <= 0 || par <= 0 ||
      !bond_frequency_ok (frequency) || !bond_basis_ok (basis))
    return o42_value_error (O42_ERR_NUM);

  /* From the issue when the interest is wanted from the start, and
   * from the last coupon before the settlement when it is not. */
  from_date = issue;
  if (method == 0)
    {
      int step = 12 / (int) frequency;
      int k = 0;

      while (add_months (first, -step * k) > settlement && k < 4000)
        k++;
      from_date = MAX (add_months (first, -step * k), issue);
    }

  year = basis_year (from_date, settlement, (int) basis);
  days = basis_days (from_date, settlement, (int) basis);
  if (year == 0)
    return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (par * rate * days / year);
}

/* DURATION and MDURATION: how long, in years, the money in a bond
 * waits on average -- each payment weighted by what it is worth now. */
static O42Value
duration_call (O42EvalContext *ctx, O42Operand *args, int n, gboolean modified)
{
  double settlement, maturity, coupon, yld, frequency, basis = 0;
  BondTerm term;
  double fraction, discount, weighted = 0, total = 0;

  ARG_NUMBER (0, settlement);
  ARG_NUMBER (1, maturity);
  ARG_NUMBER (2, coupon);
  ARG_NUMBER (3, yld);
  ARG_NUMBER (4, frequency);
  if (n >= 6)
    ARG_NUMBER (5, basis);
  settlement = floor (settlement);
  maturity = floor (maturity);
  basis = floor (basis);
  if (settlement >= maturity || coupon < 0 || yld < 0 ||
      !bond_frequency_ok (frequency) || !bond_basis_ok (basis))
    return o42_value_error (O42_ERR_NUM);
  if (!bond_term (settlement, maturity, (int) frequency, (int) basis, &term))
    return o42_value_error (O42_ERR_NUM);

  /* How much of the first coupon period has already gone, measured
   * the way the Analysis ToolPak measures it: the whole term in years
   * against the number of coupons left. */
  fraction = term.coupons - o42_bond_yearfrac (settlement, maturity, (int) basis) * frequency;
  discount = 1 + yld / frequency;
  for (int k = 1; k <= term.coupons; k++)
    {
      double periods = k - fraction;
      double years = periods / frequency;
      double flow = 100 * coupon / frequency + (k == term.coupons ? 100 : 0);
      double present = flow / pow (discount, periods);

      weighted += years * present;
      total += present;
    }
  if (total == 0)
    return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (modified ? (weighted / total) / discount : weighted / total);
}

static O42Value fn_duration  (O42EvalContext *ctx, O42Operand *a, int n) { return duration_call (ctx, a, n, FALSE); }
static O42Value fn_mduration (O42EvalContext *ctx, O42Operand *a, int n) { return duration_call (ctx, a, n, TRUE); }

/* YEARFRAC's day-count bases: 0 US 30/360, 1 actual/actual, 2 actual/360,
 * 3 actual/365, 4 European 30/360. */
static O42Value
fn_yearfrac (O42EvalContext *ctx, O42Operand *args, int n)
{
  double s1, s2, basis = 0;
  int y1, m1, d1, y2, m2, d2;
  double days;

  ARG_NUMBER (0, s1);
  ARG_NUMBER (1, s2);
  if (n >= 3)
    ARG_NUMBER (2, basis);
  if (s1 > s2) { double t = s1; s1 = s2; s2 = t; }

  if (!o42_date_from_serial (s1, &y1, &m1, &d1) ||
      !o42_date_from_serial (s2, &y2, &m2, &d2))
    return o42_value_error (O42_ERR_NUM);

  switch ((int) basis)
    {
    case 0:
    case 4:
      {
        gboolean eu = ((int) basis == 4);

        if (eu)
          {
            if (d1 == 31) d1 = 30;
            if (d2 == 31) d2 = 30;
          }
        else
          {
            gboolean d1_last_feb = (m1 == 2 && o42_date_serial (y1, 3, 0) == floor (s1));
            gboolean d2_last_feb = (m2 == 2 && o42_date_serial (y2, 3, 0) == floor (s2));

            if (d1_last_feb && d2_last_feb) d2 = 30;
            if (d1_last_feb) d1 = 30;
            if (d2 == 31 && d1 >= 30) d2 = 30;
            if (d1 == 31) d1 = 30;
          }
        days = (y2 - y1) * 360 + (m2 - m1) * 30 + (d2 - d1);
        return o42_value_number (days / 360.0);
      }

    case 1:
      /* Actual days over the length of a year, which is 366 when the
       * term is a year or less and takes in a 29 February. */
      return o42_value_number ((floor (s2) - floor (s1)) / basis_year (s1, s2, 1));

    case 2:  return o42_value_number ((floor (s2) - floor (s1)) / 360.0);
    case 3:  return o42_value_number ((floor (s2) - floor (s1)) / 365.0);
    default: return o42_value_error (O42_ERR_NUM);
    }
}

/* DATEDIF's units: "Y", "M", "D" whole years, months or days between,
 * "MD" days ignoring months and years, "YM" months ignoring years, "YD"
 * days ignoring years.  Lotus's function, kept by Excel unadvertised. */
static O42Value
fn_datedif (O42EvalContext *ctx, O42Operand *args, int n)
{
  double s1, s2;
  char *unit = NULL;
  int y1, m1, d1, y2, m2, d2;
  int years, months;
  O42Value result;

  (void) n;
  ARG_NUMBER (0, s1);
  ARG_NUMBER (1, s2);
  ARG_TEXT (2, unit);

  if (s2 < s1 || !o42_date_from_serial (s1, &y1, &m1, &d1) ||
      !o42_date_from_serial (s2, &y2, &m2, &d2))
    { g_free (unit); return o42_value_error (O42_ERR_NUM); }

  years = y2 - y1;
  if (m2 < m1 || (m2 == m1 && d2 < d1))
    years--;
  months = (y2 - y1) * 12 + (m2 - m1);
  if (d2 < d1)
    months--;

  if (g_ascii_strcasecmp (unit, "Y") == 0)
    result = o42_value_number (years);
  else if (g_ascii_strcasecmp (unit, "M") == 0)
    result = o42_value_number (months);
  else if (g_ascii_strcasecmp (unit, "D") == 0)
    result = o42_value_number (floor (s2) - floor (s1));
  else if (g_ascii_strcasecmp (unit, "YM") == 0)
    result = o42_value_number (months - years * 12);
  else if (g_ascii_strcasecmp (unit, "YD") == 0)
    {
      double anniversary = o42_date_serial (y1 + years, m1, d1);
      result = o42_value_number (floor (s2) - anniversary);
    }
  else if (g_ascii_strcasecmp (unit, "MD") == 0)
    {
      double from = o42_date_serial (y2, d2 >= d1 ? m2 : m2 - 1, d1);
      result = o42_value_number (floor (s2) - from);
    }
  else
    result = o42_value_error (O42_ERR_NUM);

  g_free (unit);
  return result;
}

static O42Value
fn_weeknum (O42EvalContext *ctx, O42Operand *args, int n)
{
  double serial, type = 1;
  int y, m, d;
  double jan1;
  int jan1_wd, offset;

  ARG_NUMBER (0, serial);
  if (n >= 2)
    ARG_NUMBER (1, type);
  if (!o42_date_from_serial (serial, &y, &m, &d))
    return o42_value_error (O42_ERR_NUM);

  jan1 = o42_date_serial (y, 1, 1);
  jan1_wd = o42_date_weekday (jan1);    /* Monday 1 .. Sunday 7 */

  /* Weeks begin on Sunday (type 1) or Monday (type 2), and the week that
   * holds 1 January is week 1. */
  offset = ((int) type == 2) ? jan1_wd - 1 : jan1_wd % 7;
  return o42_value_number (floor ((floor (serial) - jan1 + offset) / 7) + 1);
}

static O42Value
fn_isoweeknum (O42EvalContext *ctx, O42Operand *args, int n)
{
  double serial;
  int wd;
  double thursday;
  int y, m, d;

  (void) n;
  ARG_NUMBER (0, serial);
  serial = floor (serial);

  /* The ISO week is the week of its Thursday, counted in that Thursday's
   * year. */
  wd = o42_date_weekday (serial);
  thursday = serial + (4 - wd);
  if (!o42_date_from_serial (thursday, &y, &m, &d))
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (floor ((thursday - o42_date_serial (y, 1, 1)) / 7) + 1);
}

const O42Function O42_FUNCS_FINANCE[] = {
  { "ACCRINT", 4, 8, fn_accrint },
  { "ACCRINTM", 3, 5, fn_accrintm },
  { "AMORDEGRC", 6, 7, fn_amordegrc },
  { "AMORLINC", 6, 7, fn_amorlinc },
  { "ASC", 1, 1, fn_asc },
  { "BAHTTEXT", 1, 1, fn_bahttext },
  { "COUPDAYBS", 3, 4, fn_coupdaybs },
  { "COUPDAYS", 3, 4, fn_coupdays },
  { "COUPDAYSNC", 3, 4, fn_coupdaysnc },
  { "COUPNCD", 3, 4, fn_coupncd },
  { "COUPNUM", 3, 4, fn_coupnum },
  { "COUPPCD", 3, 4, fn_couppcd },
  { "DATEDIF", 3, 3, fn_datedif },
  { "DISC", 4, 5, fn_disc },
  { "DURATION", 5, 6, fn_duration },
  { "FVSCHEDULE", 2, -1, fn_fvschedule },
  { "INTRATE", 4, 5, fn_intrate },
  { "ISOWEEKNUM", 1, 1, fn_isoweeknum },
  { "MDURATION", 5, 6, fn_mduration },
  { "ODDFPRICE", 8, 9, fn_oddfprice },
  { "ODDFYIELD", 8, 9, fn_oddfyield },
  { "ODDLPRICE", 7, 8, fn_oddlprice },
  { "ODDLYIELD", 7, 8, fn_oddlyield },
  { "PHONETIC", 1, 1, fn_phonetic },
  { "PRICE", 6, 7, fn_price },
  { "PRICEDISC", 4, 5, fn_pricedisc },
  { "PRICEMAT", 5, 6, fn_pricemat },
  { "RECEIVED", 4, 5, fn_received },
  { "TBILLEQ", 3, 3, fn_tbilleq },
  { "TBILLPRICE", 3, 3, fn_tbillprice },
  { "TBILLYIELD", 3, 3, fn_tbillyield },
  { "WEEKNUM", 1, 2, fn_weeknum },
  { "YEARFRAC", 2, 3, fn_yearfrac },
  { "YIELD", 6, 7, fn_yield },
  { "YIELDDISC", 4, 5, fn_yielddisc },
  { "YIELDMAT", 5, 6, fn_yieldmat },
  { NULL, 0, 0, NULL }
};

const O42FunctionHelp O42_HELP_FINANCE[] = {
  { "ACCRINT", "ACCRINT(issue, first_interest, settlement, rate, par, frequency, basis, method)", "Accrued interest on a security that pays periodically." },
  { "ACCRINTM", "ACCRINTM(issue, settlement, rate, par, basis)", "Accrued interest on a security that pays at maturity." },
  { "AMORDEGRC", "AMORDEGRC(cost, purchased, first_period, salvage, period, rate, basis)", "French degressive depreciation for a period." },
  { "AMORLINC", "AMORLINC(cost, purchased, first_period, salvage, period, rate, basis)", "French straight-line depreciation for a period." },
  { "ASC", "ASC(text)", "Full-width letters as half-width ones; the text itself here." },
  { "BAHTTEXT", "BAHTTEXT(number)", "A number written out in Thai, in baht and satang." },
  { "COUPDAYBS", "COUPDAYBS(settlement, maturity, frequency, basis)", "Days from the start of the coupon period to the settlement." },
  { "COUPDAYS", "COUPDAYS(settlement, maturity, frequency, basis)", "Days in the coupon period the settlement falls in." },
  { "COUPDAYSNC", "COUPDAYSNC(settlement, maturity, frequency, basis)", "Days from the settlement to the next coupon." },
  { "COUPNCD", "COUPNCD(settlement, maturity, frequency, basis)", "The next coupon date after the settlement." },
  { "COUPNUM", "COUPNUM(settlement, maturity, frequency, basis)", "How many coupons are payable between settlement and maturity." },
  { "COUPPCD", "COUPPCD(settlement, maturity, frequency, basis)", "The coupon date on or before the settlement." },
  { "DATEDIF", "DATEDIF(start_date, end_date, unit)", "The time between two dates in years, months or days: \"Y\", \"M\", \"D\", \"YM\", \"YD\", \"MD\"." },
  { "DISC", "DISC(settlement, maturity, pr, redemption, basis)", "The discount rate of a security." },
  { "DURATION", "DURATION(settlement, maturity, coupon, yld, frequency, basis)", "A bond's Macaulay duration in years." },
  { "FVSCHEDULE", "FVSCHEDULE(principal, schedule)", "A principal grown by a series of interest rates." },
  { "INTRATE", "INTRATE(settlement, maturity, investment, redemption, basis)", "The interest rate of a fully invested security." },
  { "ISOWEEKNUM", "ISOWEEKNUM(serial)", "The ISO week number of a date." },
  { "MDURATION", "MDURATION(settlement, maturity, coupon, yld, frequency, basis)", "A bond's modified duration." },
  { "ODDFPRICE", "ODDFPRICE(settlement, maturity, issue, first_coupon, rate, yld, redemption, frequency, basis)", "The price of a bond with an odd first period." },
  { "ODDFYIELD", "ODDFYIELD(settlement, maturity, issue, first_coupon, rate, pr, redemption, frequency, basis)", "The yield of a bond with an odd first period." },
  { "ODDLPRICE", "ODDLPRICE(settlement, maturity, last_interest, rate, yld, redemption, frequency, basis)", "The price of a bond with an odd last period." },
  { "ODDLYIELD", "ODDLYIELD(settlement, maturity, last_interest, rate, pr, redemption, frequency, basis)", "The yield of a bond with an odd last period." },
  { "PHONETIC", "PHONETIC(reference)", "The furigana in a cell; the text itself here." },
  { "PRICE", "PRICE(settlement, maturity, rate, yld, redemption, frequency, basis)", "The price per 100 of a bond paying periodic interest." },
  { "PRICEDISC", "PRICEDISC(settlement, maturity, discount, redemption, basis)", "The price per 100 of a discounted security." },
  { "PRICEMAT", "PRICEMAT(settlement, maturity, issue, rate, yld, basis)", "The price per 100 of a security paying at maturity." },
  { "RECEIVED", "RECEIVED(settlement, maturity, investment, discount, basis)", "What a fully invested security pays at maturity." },
  { "TBILLEQ", "TBILLEQ(settlement, maturity, discount)", "A Treasury bill's bond-equivalent yield." },
  { "TBILLPRICE", "TBILLPRICE(settlement, maturity, discount)", "The price per 100 of a Treasury bill." },
  { "TBILLYIELD", "TBILLYIELD(settlement, maturity, pr)", "A Treasury bill's yield." },
  { "WEEKNUM", "WEEKNUM(serial, return_type)", "The week of the year a date falls in." },
  { "YEARFRAC", "YEARFRAC(start_date, end_date, basis)", "The fraction of a year between two dates." },
  { "YIELD", "YIELD(settlement, maturity, rate, pr, redemption, frequency, basis)", "The yield of a bond paying periodic interest." },
  { "YIELDDISC", "YIELDDISC(settlement, maturity, pr, redemption, basis)", "The annual yield of a discounted security." },
  { "YIELDMAT", "YIELDMAT(settlement, maturity, issue, rate, pr, basis)", "The yield of a security paying at maturity." },
  { NULL, NULL, NULL }
};
