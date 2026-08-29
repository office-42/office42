/* o42-fn-statistics.c - see o42-eval-private.h
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
#define collect_pairs o42_collect_pairs
#define complex_parse o42_complex_parse
#define complex_format o42_complex_format
#define round_half_away o42_round_half_away
#define chi_cdf o42_chi_cdf
#define beta_i o42_beta_i
#define gamma_p o42_gamma_p
#define f_cdf o42_f_cdf
#define t_cdf o42_t_cdf
#define invert_cdf o42_invert_cdf
#define optional_bool o42_optional_bool
#define normal_inverse o42_normal_inverse
#define compare_doubles o42_compare_doubles
#define moments o42_moments
#define visit_numbers o42_visit_numbers
#define accum_clear o42_accum_clear
#define accum_init o42_accum_init
#define accumulate o42_accumulate

/* ---- Statistics ------------------------------------------------------- */

/* Every number in the arguments, in order, or the error that stopped the
 * walk.  The statistical functions all start here. */
gboolean
o42_collect_numbers (O42EvalContext *ctx, O42Operand *args, int n,
                 GArray **values, O42ErrorCode *err)
{
  Accum a;

  accum_init (&a, TRUE);
  if (!visit_numbers (ctx, args, n, accumulate, &a, err))
    {
      accum_clear (&a);
      return FALSE;
    }

  *values = a.values;
  return TRUE;
}

/* The mean and the sum of squared deviations from it, in two passes.  The
 * one-pass formula sum(x^2) - n*mean^2 cancels catastrophically on data
 * with a large mean and a small spread; two passes are what Gnumeric does
 * and what keeps STDEV of 1000000.1, 1000000.2, 1000000.3 at 0.1 rather
 * than at something with a stray digit in it. */
void
o42_moments (const GArray *values, double *mean, double *ssd)
{
  double sum = 0, m, s = 0, correction = 0;
  guint n = values->len;

  for (guint i = 0; i < n; i++)
    sum += g_array_index (values, double, i);
  m = (n > 0) ? sum / n : 0;

  for (guint i = 0; i < n; i++)
    {
      double d = g_array_index (values, double, i) - m;
      s += d * d;
      correction += d;
    }

  /* The residual of the deviations should be zero; whatever is left is
   * rounding error in the mean, and this takes it back out. */
  if (n > 0)
    s -= correction * correction / n;

  *mean = m;
  *ssd = s;
}

typedef enum { STAT_VAR, STAT_VARP, STAT_STDEV, STAT_STDEVP, STAT_AVEDEV,
               STAT_DEVSQ } StatKind;

static O42Value
fn_spread (O42EvalContext *ctx, O42Operand *args, int n, StatKind kind)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double mean, ssd, r;
  guint count;

  if (!collect_numbers (ctx, args, n, &values, &err))
    return o42_value_error (err);

  count = values->len;
  moments (values, &mean, &ssd);

  switch (kind)
    {
    case STAT_VAR:
    case STAT_STDEV:
      if (count < 2) { g_array_free (values, TRUE); return o42_value_error (O42_ERR_DIV0); }
      r = ssd / (count - 1);
      if (kind == STAT_STDEV) r = sqrt (r);
      break;

    case STAT_VARP:
    case STAT_STDEVP:
      if (count < 1) { g_array_free (values, TRUE); return o42_value_error (O42_ERR_DIV0); }
      r = ssd / count;
      if (kind == STAT_STDEVP) r = sqrt (r);
      break;

    case STAT_AVEDEV:
      if (count < 1) { g_array_free (values, TRUE); return o42_value_error (O42_ERR_DIV0); }
      r = 0;
      for (guint i = 0; i < count; i++)
        r += fabs (g_array_index (values, double, i) - mean);
      r /= count;
      break;

    case STAT_DEVSQ:
    default:
      r = ssd;
      break;
    }

  g_array_free (values, TRUE);
  return o42_value_number (r);
}

static O42Value fn_var    (O42EvalContext *c, O42Operand *a, int n) { return fn_spread (c, a, n, STAT_VAR); }
static O42Value fn_varp   (O42EvalContext *c, O42Operand *a, int n) { return fn_spread (c, a, n, STAT_VARP); }
static O42Value fn_stdev  (O42EvalContext *c, O42Operand *a, int n) { return fn_spread (c, a, n, STAT_STDEV); }
static O42Value fn_stdevp (O42EvalContext *c, O42Operand *a, int n) { return fn_spread (c, a, n, STAT_STDEVP); }
static O42Value fn_avedev (O42EvalContext *c, O42Operand *a, int n) { return fn_spread (c, a, n, STAT_AVEDEV); }
static O42Value fn_devsq  (O42EvalContext *c, O42Operand *a, int n) { return fn_spread (c, a, n, STAT_DEVSQ); }

static O42Value
fn_geomean_harmean (O42EvalContext *ctx, O42Operand *args, int n, gboolean geometric)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double acc = 0;

  if (!collect_numbers (ctx, args, n, &values, &err))
    return o42_value_error (err);

  if (values->len == 0)
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NUM); }

  for (guint i = 0; i < values->len; i++)
    {
      double v = g_array_index (values, double, i);

      if (v <= 0)
        { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NUM); }
      acc += geometric ? log (v) : 1.0 / v;
    }

  acc = geometric ? exp (acc / values->len) : values->len / acc;
  g_array_free (values, TRUE);
  return o42_value_number (acc);
}

static O42Value fn_geomean (O42EvalContext *c, O42Operand *a, int n) { return fn_geomean_harmean (c, a, n, TRUE); }
static O42Value fn_harmean (O42EvalContext *c, O42Operand *a, int n) { return fn_geomean_harmean (c, a, n, FALSE); }

static O42Value
fn_large_small (O42EvalContext *ctx, O42Operand *args, int n, gboolean large)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double k;
  O42Value result;

  (void) n;
  ARG_NUMBER (1, k);

  if (!collect_numbers (ctx, args, 1, &values, &err))
    return o42_value_error (err);

  if (k < 1 || k > values->len)
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NUM); }

  g_array_sort (values, compare_doubles);
  result = o42_value_number (g_array_index (values, double,
                               large ? values->len - (guint) k : (guint) k - 1));
  g_array_free (values, TRUE);
  return result;
}

static O42Value fn_large (O42EvalContext *c, O42Operand *a, int n) { return fn_large_small (c, a, n, TRUE); }
static O42Value fn_small (O42EvalContext *c, O42Operand *a, int n) { return fn_large_small (c, a, n, FALSE); }

static O42Value
fn_rank (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double x, order = 0;
  int rank = 1;
  gboolean found = FALSE;

  ARG_NUMBER (0, x);
  if (n >= 3)
    ARG_NUMBER (2, order);

  if (!collect_numbers (ctx, args + 1, 1, &values, &err))
    return o42_value_error (err);

  for (guint i = 0; i < values->len; i++)
    {
      double v = g_array_index (values, double, i);

      if (v == x) found = TRUE;
      else if (order == 0 ? v > x : v < x) rank++;
    }

  g_array_free (values, TRUE);
  return found ? o42_value_number (rank) : o42_value_error (O42_ERR_NA);
}

/* Excel's PERCENTILE interpolates linearly between the order statistics,
 * with the smallest value at 0 and the largest at 1. */
static O42Value
percentile_of (GArray *values, double k)
{
  double pos, frac;
  guint lo;
  O42Value result;

  if (values->len == 0 || k < 0 || k > 1)
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NUM); }

  g_array_sort (values, compare_doubles);
  pos = k * (values->len - 1);
  lo = (guint) floor (pos);
  frac = pos - lo;

  if (lo + 1 >= values->len)
    result = o42_value_number (g_array_index (values, double, values->len - 1));
  else
    result = o42_value_number (g_array_index (values, double, lo) * (1 - frac) +
                               g_array_index (values, double, lo + 1) * frac);

  g_array_free (values, TRUE);
  return result;
}

static O42Value
fn_percentile (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double k;

  (void) n;
  ARG_NUMBER (1, k);
  if (!collect_numbers (ctx, args, 1, &values, &err))
    return o42_value_error (err);
  return percentile_of (values, k);
}

static O42Value
fn_quartile (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double q;

  (void) n;
  ARG_NUMBER (1, q);
  q = trunc (q);
  if (q < 0 || q > 4)
    return o42_value_error (O42_ERR_NUM);
  if (!collect_numbers (ctx, args, 1, &values, &err))
    return o42_value_error (err);
  return percentile_of (values, q / 4.0);
}

static O42Value
fn_mode (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double best = 0;
  int best_count = 1;

  if (!collect_numbers (ctx, args, n, &values, &err))
    return o42_value_error (err);

  /* The first of the most frequent values, in the order they appear,
   * which is the tie-break Excel uses. */
  for (guint i = 0; i < values->len; i++)
    {
      double v = g_array_index (values, double, i);
      int count = 0;

      for (guint j = 0; j < values->len; j++)
        if (g_array_index (values, double, j) == v)
          count++;

      if (count > best_count)
        {
          best = v;
          best_count = count;
        }
    }

  g_array_free (values, TRUE);
  return (best_count > 1) ? o42_value_number (best) : o42_value_error (O42_ERR_NA);
}

/* Two ranges walked in step, keeping only the positions where both hold a
 * number: what every function of two variables wants. */
gboolean
o42_collect_pairs (O42EvalContext *ctx, const O42Operand *a, const O42Operand *b,
               GArray **xs, GArray **ys, O42ErrorCode *err)
{
  int rows, cols;

  if (!a->is_range || !b->is_range)
    { *err = O42_ERR_VALUE; return FALSE; }

  rows = a->range.row1 - a->range.row0 + 1;
  cols = a->range.col1 - a->range.col0 + 1;
  if (rows != b->range.row1 - b->range.row0 + 1 ||
      cols != b->range.col1 - b->range.col0 + 1)
    { *err = O42_ERR_NA; return FALSE; }

  *xs = g_array_new (FALSE, FALSE, sizeof (double));
  *ys = g_array_new (FALSE, FALSE, sizeof (double));

  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      {
        O42Value va, vb;

        ctx->get_cell (ctx, a->sheet, a->range.row0 + r, a->range.col0 + c, &va);
        ctx->get_cell (ctx, b->sheet, b->range.row0 + r, b->range.col0 + c, &vb);

        if (va.type == O42_VALUE_NUMBER && vb.type == O42_VALUE_NUMBER)
          {
            g_array_append_val (*xs, va.as.number);
            g_array_append_val (*ys, vb.as.number);
          }
        o42_value_clear (&va);
        o42_value_clear (&vb);
      }

  return TRUE;
}

typedef enum { PAIR_CORREL, PAIR_COVAR, PAIR_SLOPE, PAIR_INTERCEPT, PAIR_RSQ,
               PAIR_PEARSON } PairKind;

static O42Value
fn_pair (O42EvalContext *ctx, O42Operand *args, int n, PairKind kind)
{
  GArray *xs, *ys;
  O42ErrorCode err = O42_ERR_VALUE;
  double mx, my, sxx, syy, sxy = 0, r;
  guint count;

  (void) n;

  /* SLOPE, INTERCEPT and RSQ take known_y's first; the others x first. */
  if (kind == PAIR_SLOPE || kind == PAIR_INTERCEPT || kind == PAIR_RSQ)
    {
      if (!collect_pairs (ctx, &args[1], &args[0], &xs, &ys, &err))
        return o42_value_error (err);
    }
  else if (!collect_pairs (ctx, &args[0], &args[1], &xs, &ys, &err))
    return o42_value_error (err);

  count = xs->len;
  moments (xs, &mx, &sxx);
  moments (ys, &my, &syy);
  for (guint i = 0; i < count; i++)
    sxy += (g_array_index (xs, double, i) - mx) * (g_array_index (ys, double, i) - my);

  g_array_free (xs, TRUE);
  g_array_free (ys, TRUE);

  if (count == 0)
    return o42_value_error (O42_ERR_DIV0);

  switch (kind)
    {
    case PAIR_COVAR:
      r = sxy / count;
      break;
    case PAIR_SLOPE:
      if (sxx == 0) return o42_value_error (O42_ERR_DIV0);
      r = sxy / sxx;
      break;
    case PAIR_INTERCEPT:
      if (sxx == 0) return o42_value_error (O42_ERR_DIV0);
      r = my - (sxy / sxx) * mx;
      break;
    case PAIR_RSQ:
      if (sxx == 0 || syy == 0) return o42_value_error (O42_ERR_DIV0);
      r = (sxy * sxy) / (sxx * syy);
      break;
    case PAIR_CORREL:
    case PAIR_PEARSON:
    default:
      if (sxx == 0 || syy == 0) return o42_value_error (O42_ERR_DIV0);
      r = sxy / sqrt (sxx * syy);
      break;
    }

  return o42_value_number (r);
}

static O42Value fn_correl    (O42EvalContext *c, O42Operand *a, int n) { return fn_pair (c, a, n, PAIR_CORREL); }
static O42Value fn_pearson   (O42EvalContext *c, O42Operand *a, int n) { return fn_pair (c, a, n, PAIR_PEARSON); }
static O42Value fn_covar     (O42EvalContext *c, O42Operand *a, int n) { return fn_pair (c, a, n, PAIR_COVAR); }
static O42Value fn_slope     (O42EvalContext *c, O42Operand *a, int n) { return fn_pair (c, a, n, PAIR_SLOPE); }
static O42Value fn_intercept (O42EvalContext *c, O42Operand *a, int n) { return fn_pair (c, a, n, PAIR_INTERCEPT); }
static O42Value fn_rsq       (O42EvalContext *c, O42Operand *a, int n) { return fn_pair (c, a, n, PAIR_RSQ); }

static O42Value
fn_forecast (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  O42Value slope, intercept;
  double result;

  (void) n;
  ARG_NUMBER (0, x);

  slope = fn_pair (ctx, args + 1, 2, PAIR_SLOPE);
  if (slope.type == O42_VALUE_ERROR) return slope;
  intercept = fn_pair (ctx, args + 1, 2, PAIR_INTERCEPT);
  if (intercept.type == O42_VALUE_ERROR) return intercept;

  result = intercept.as.number + slope.as.number * x;
  return o42_value_number (result);
}

/* The standard normal distribution.  erfc is accurate in the tails where
 * 1 - erf would lose everything. */
double
o42_normal_cdf (double z)
{
  return 0.5 * erfc (-z / G_SQRT2);
}

/* Its density, which the option Greeks are full of. */
double
o42_normal_pdf (double x)
{
  return exp (-x * x / 2) / sqrt (2 * G_PI);
}

/* Its inverse: Acklam's rational approximation, then one step of Newton's
 * method on the CDF, which takes the error from about 1e-9 to the last
 * digit of a double. */
double
o42_normal_inverse (double p)
{
  static const double a[] = { -3.969683028665376e+01, 2.209460984245205e+02,
                              -2.759285104469687e+02, 1.383577518672690e+02,
                              -3.066479806614716e+01, 2.506628277459239e+00 };
  static const double b[] = { -5.447609879822406e+01, 1.615858368580409e+02,
                              -1.556989798598866e+02, 6.680131188771972e+01,
                              -1.328068155288572e+01 };
  static const double c[] = { -7.784894002430293e-03, -3.223964580411365e-01,
                              -2.400758277161838e+00, -2.549732539343734e+00,
                               4.374664141464968e+00, 2.938163982698783e+00 };
  static const double d[] = { 7.784695709041462e-03, 3.224671290700398e-01,
                              2.445134137142996e+00, 3.754408661907416e+00 };
  const double plow = 0.02425, phigh = 1 - 0.02425;
  double q, r, x;

  if (p < plow)
    {
      q = sqrt (-2 * log (p));
      x = (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) /
          ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1);
    }
  else if (p <= phigh)
    {
      q = p - 0.5;
      r = q * q;
      x = (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q /
          (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1);
    }
  else
    {
      q = sqrt (-2 * log (1 - p));
      x = -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) /
           ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1);
    }

  /* Newton refinement. */
  {
    double e = normal_cdf (x) - p;
    double u = e * sqrt (2 * G_PI) * exp (x * x / 2);
    x = x - u / (1 + x * u / 2);
  }

  return x;
}

static O42Value
fn_normsdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double z;
  (void) n;
  ARG_NUMBER (0, z);
  return o42_value_number (normal_cdf (z));
}

static O42Value
fn_normsinv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p;
  (void) n;
  ARG_NUMBER (0, p);
  if (p <= 0 || p >= 1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (normal_inverse (p));
}

static O42Value
fn_normdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, mean, sd;
  gboolean cumulative = TRUE;

  ARG_NUMBER (0, x);
  ARG_NUMBER (1, mean);
  ARG_NUMBER (2, sd);
  if (n >= 4)
    {
      O42Value v = operand_value (ctx, &args[3]);
      O42ErrorCode err = O42_ERR_VALUE;
      gboolean ok = o42_value_to_bool (&v, &cumulative, &err);
      o42_value_clear (&v);
      if (!ok) return o42_value_error (err);
    }

  if (sd <= 0)
    return o42_value_error (O42_ERR_NUM);

  if (cumulative)
    return o42_value_number (normal_cdf ((x - mean) / sd));

  return o42_value_number (exp (-0.5 * ((x - mean) / sd) * ((x - mean) / sd)) /
                           (sd * sqrt (2 * G_PI)));
}

static O42Value
fn_norminv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p, mean, sd;
  (void) n;
  ARG_NUMBER (0, p);
  ARG_NUMBER (1, mean);
  ARG_NUMBER (2, sd);
  if (p <= 0 || p >= 1 || sd <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (mean + sd * normal_inverse (p));
}

static O42Value
fn_standardize (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, mean, sd;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, mean);
  ARG_NUMBER (2, sd);
  if (sd <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number ((x - mean) / sd);
}

const O42Function O42_FUNCS_STATISTICS[] = {
  { "AVEDEV", 1, -1, fn_avedev },
  { "CORREL", 2, 2, fn_correl },
  { "COVAR", 2, 2, fn_covar },
  { "COVARIANCE.P", 2, 2, fn_covar },
  { "DEVSQ", 1, -1, fn_devsq },
  { "FORECAST", 3, 3, fn_forecast },
  { "FORECAST.LINEAR", 3, 3, fn_forecast },
  { "GEOMEAN", 1, -1, fn_geomean },
  { "HARMEAN", 1, -1, fn_harmean },
  { "INTERCEPT", 2, 2, fn_intercept },
  { "LARGE", 2, 2, fn_large },
  { "MODE", 1, -1, fn_mode },
  { "MODE.SNGL", 1, -1, fn_mode },
  { "NORM.DIST", 4, 4, fn_normdist },
  { "NORM.INV", 3, 3, fn_norminv },
  { "NORM.S.INV", 1, 1, fn_normsinv },
  { "NORMDIST", 3, 4, fn_normdist },
  { "NORMINV", 3, 3, fn_norminv },
  { "NORMSDIST", 1, 1, fn_normsdist },
  { "NORMSINV", 1, 1, fn_normsinv },
  { "PEARSON", 2, 2, fn_pearson },
  { "PERCENTILE", 2, 2, fn_percentile },
  { "PERCENTILE.INC", 2, 2, fn_percentile },
  { "QUARTILE", 2, 2, fn_quartile },
  { "QUARTILE.INC", 2, 2, fn_quartile },
  { "RANK", 2, 3, fn_rank },
  { "RANK.EQ", 2, 3, fn_rank },
  { "RSQ", 2, 2, fn_rsq },
  { "SLOPE", 2, 2, fn_slope },
  { "SMALL", 2, 2, fn_small },
  { "STANDARDIZE", 3, 3, fn_standardize },
  { "STDEV", 1, -1, fn_stdev },
  { "STDEV.P", 1, -1, fn_stdevp },
  { "STDEV.S", 1, -1, fn_stdev },
  { "STDEVP", 1, -1, fn_stdevp },
  { "VAR", 1, -1, fn_var },
  { "VAR.P", 1, -1, fn_varp },
  { "VAR.S", 1, -1, fn_var },
  { "VARP", 1, -1, fn_varp },
  { NULL, 0, 0, NULL }
};

const O42FunctionHelp O42_HELP_STATISTICS[] = {
  { "AVEDEV", "AVEDEV(number1, number2, ...)", "The average absolute deviation from the mean." },
  { "CORREL", "CORREL(array1, array2)", "The correlation coefficient of two sets." },
  { "COVAR", "COVAR(array1, array2)", "The population covariance of two sets." },
  { "COVARIANCE.P", "COVARIANCE.P(array1, array2)", "The population covariance." },
  { "DEVSQ", "DEVSQ(number1, number2, ...)", "The sum of squared deviations from the mean." },
  { "FORECAST", "FORECAST(x, known_y's, known_x's)", "A value on the regression line through known points." },
  { "FORECAST.LINEAR", "FORECAST.LINEAR(x, known_ys, known_xs)", "A value on the regression line through the points." },
  { "GEOMEAN", "GEOMEAN(number1, number2, ...)", "The geometric mean." },
  { "HARMEAN", "HARMEAN(number1, number2, ...)", "The harmonic mean." },
  { "INTERCEPT", "INTERCEPT(known_y's, known_x's)", "Where the regression line crosses the y axis." },
  { "LARGE", "LARGE(range, k)", "The k-th largest value." },
  { "MODE", "MODE(number1, number2, ...)", "The most frequent value." },
  { "MODE.SNGL", "MODE.SNGL(number1, number2, ...)", "The most frequent value." },
  { "NORM.DIST", "NORM.DIST(x, mean, standard_dev, cumulative)", "The normal distribution." },
  { "NORM.INV", "NORM.INV(probability, mean, standard_dev)", "The inverse of the cumulative normal distribution." },
  { "NORM.S.INV", "NORM.S.INV(probability)", "The inverse of the standard normal distribution." },
  { "NORMDIST", "NORMDIST(x, mean, standard_dev, cumulative)", "The normal distribution." },
  { "NORMINV", "NORMINV(probability, mean, standard_dev)", "The inverse of the normal distribution." },
  { "NORMSDIST", "NORMSDIST(z)", "The standard normal cumulative distribution." },
  { "NORMSINV", "NORMSINV(probability)", "The inverse of the standard normal distribution." },
  { "PEARSON", "PEARSON(array1, array2)", "The Pearson correlation coefficient." },
  { "PERCENTILE", "PERCENTILE(range, k)", "The value below which a fraction k of the values lie." },
  { "PERCENTILE.INC", "PERCENTILE.INC(array, k)", "The k-th percentile, inclusive." },
  { "QUARTILE", "QUARTILE(range, quart)", "A quartile of the values: 0 to 4." },
  { "QUARTILE.INC", "QUARTILE.INC(array, quart)", "A quartile, inclusive." },
  { "RANK", "RANK(number, range, order)", "The rank of a number among others." },
  { "RANK.EQ", "RANK.EQ(number, ref, order)", "The rank of a number in a list." },
  { "RSQ", "RSQ(known_y's, known_x's)", "The square of the correlation coefficient." },
  { "SLOPE", "SLOPE(known_y's, known_x's)", "The slope of the regression line." },
  { "SMALL", "SMALL(range, k)", "The k-th smallest value." },
  { "STANDARDIZE", "STANDARDIZE(x, mean, standard_dev)", "A value as a number of standard deviations from the mean." },
  { "STDEV", "STDEV(number1, number2, ...)", "The standard deviation of a sample." },
  { "STDEV.P", "STDEV.P(number1, number2, ...)", "The population standard deviation." },
  { "STDEV.S", "STDEV.S(number1, number2, ...)", "The sample standard deviation." },
  { "STDEVP", "STDEVP(number1, number2, ...)", "The standard deviation of a whole population." },
  { "VAR", "VAR(number1, number2, ...)", "The variance of a sample." },
  { "VAR.P", "VAR.P(number1, number2, ...)", "The population variance." },
  { "VAR.S", "VAR.S(number1, number2, ...)", "The sample variance." },
  { "VARP", "VARP(number1, number2, ...)", "The variance of a whole population." },
  { NULL, NULL, NULL }
};
