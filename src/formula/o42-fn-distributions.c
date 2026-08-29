/* o42-fn-distributions.c - see o42-eval-private.h
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
#define chi_cdf o42_chi_cdf
#define beta_i o42_beta_i
#define invert_cdf o42_invert_cdf
#define t_cdf o42_t_cdf
#define f_cdf o42_f_cdf
#define gamma_p o42_gamma_p
#define compare_doubles o42_compare_doubles
#define optional_bool o42_optional_bool
#define moments o42_moments
#define normal_inverse o42_normal_inverse

/* ---- Distributions ----------------------------------------------------- */

/* The regularised incomplete gamma function P(a, x), by the series for
 * x < a + 1 and the continued fraction (modified Lentz) beyond, which is
 * the standard split and accurate to about 1e-14 either side. */
double
o42_gamma_p (double a, double x)
{
  if (x <= 0)
    return 0;

  if (x < a + 1)
    {
      double sum = 1.0 / a, term = 1.0 / a, ap = a;

      for (int i = 0; i < 1000; i++)
        {
          ap += 1;
          term *= x / ap;
          sum += term;
          if (fabs (term) < fabs (sum) * 1e-16)
            break;
        }
      return sum * exp (-x + a * log (x) - lgamma (a));
    }
  else
    {
      double b = x + 1 - a, c = 1e300, d = 1 / b, h = d;

      for (int i = 1; i < 1000; i++)
        {
          double an = -i * (i - a), del;

          b += 2;
          d = an * d + b;
          if (fabs (d) < 1e-300) d = 1e-300;
          c = b + an / c;
          if (fabs (c) < 1e-300) c = 1e-300;
          d = 1 / d;
          del = d * c;
          h *= del;
          if (fabs (del - 1) < 1e-16)
            break;
        }
      return 1 - exp (-x + a * log (x) - lgamma (a)) * h;
    }
}

/* The regularised incomplete beta function I_x(a, b), by the continued
 * fraction, with the symmetry that keeps it converging fast. */
static double
beta_cf (double a, double b, double x)
{
  double qab = a + b, qap = a + 1, qam = a - 1;
  double c = 1, d = 1 - qab * x / qap, h;

  if (fabs (d) < 1e-300) d = 1e-300;
  d = 1 / d;
  h = d;

  for (int m = 1; m <= 1000; m++)
    {
      int m2 = 2 * m;
      double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
      double del;

      d = 1 + aa * d; if (fabs (d) < 1e-300) d = 1e-300;
      c = 1 + aa / c; if (fabs (c) < 1e-300) c = 1e-300;
      d = 1 / d;
      h *= d * c;

      aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
      d = 1 + aa * d; if (fabs (d) < 1e-300) d = 1e-300;
      c = 1 + aa / c; if (fabs (c) < 1e-300) c = 1e-300;
      d = 1 / d;
      del = d * c;
      h *= del;
      if (fabs (del - 1) < 1e-16)
        break;
    }

  return h;
}

double
o42_beta_i (double a, double b, double x)
{
  double bt;

  if (x <= 0) return 0;
  if (x >= 1) return 1;

  bt = exp (lgamma (a + b) - lgamma (a) - lgamma (b) + a * log (x) + b * log (1 - x));

  if (x < (a + 1) / (a + b + 2))
    return bt * beta_cf (a, b, x) / a;
  return 1 - bt * beta_cf (b, a, 1 - x) / b;
}

/* Inverts a monotone distribution function by bisection on [lo, hi],
 * which is slow but certain -- sixty halvings are all a double needs. */
typedef double (*Cdf) (double x, double p1, double p2);

double
o42_invert_cdf (Cdf cdf, double p, double p1, double p2, double lo, double hi)
{
  for (int i = 0; i < 200; i++)
    {
      double mid = (lo + hi) / 2;

      if (cdf (mid, p1, p2) < p) lo = mid;
      else                       hi = mid;
      if (hi - lo < 1e-15 * MAX (1.0, fabs (hi)))
        break;
    }
  return (lo + hi) / 2;
}

gboolean
o42_optional_bool (O42EvalContext *ctx, O42Operand *args, int n, int index,
               gboolean fallback, gboolean *out)
{
  O42Value v;
  O42ErrorCode err = O42_ERR_VALUE;
  gboolean ok;

  if (n <= index)
    { *out = fallback; return TRUE; }
  v = operand_value (ctx, &args[index]);
  ok = o42_value_to_bool (&v, out, &err);
  o42_value_clear (&v);
  return ok;
}

double o42_chi_cdf (double x, double df, double unused) { (void) unused; return gamma_p (df / 2, x / 2); }
static double gamma_cdf (double x, double alpha, double beta) { return gamma_p (alpha, x / beta); }
static double beta_cdf (double x, double a, double b) { return beta_i (a, b, x); }

double
o42_t_cdf (double t, double df, double unused)
{
  double x = df / (df + t * t);
  double tail = 0.5 * beta_i (df / 2, 0.5, x);
  (void) unused;
  return (t >= 0) ? 1 - tail : tail;
}

double
o42_f_cdf (double x, double d1, double d2)
{
  if (x <= 0) return 0;
  return beta_i (d1 / 2, d2 / 2, d1 * x / (d1 * x + d2));
}

/* CHIDIST is the right tail, as Excel's is. */
static O42Value
fn_chidist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, df;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, df);
  if (x < 0 || df < 1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (1 - chi_cdf (x, floor (df), 0));
}

static O42Value
fn_chiinv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p, df;
  (void) n;
  ARG_NUMBER (0, p);
  ARG_NUMBER (1, df);
  if (p <= 0 || p > 1 || df < 1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (invert_cdf (chi_cdf, 1 - p, floor (df), 0, 0, 1e6));
}

/* TDIST(x, df, tails): the tail beyond |x|, one or both. */
static O42Value
fn_tdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, df, tails;
  double tail;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, df);
  ARG_NUMBER (2, tails);
  if (x < 0 || df < 1 || (tails != 1 && tails != 2))
    return o42_value_error (O42_ERR_NUM);
  tail = 1 - t_cdf (x, floor (df), 0);
  return o42_value_number (tails == 2 ? 2 * tail : tail);
}

/* TINV is two-tailed, as Excel's is: the |x| beyond which p lies. */
static O42Value
fn_tinv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p, df;
  (void) n;
  ARG_NUMBER (0, p);
  ARG_NUMBER (1, df);
  if (p <= 0 || p > 1 || df < 1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (invert_cdf (t_cdf, 1 - p / 2, floor (df), 0, 0, 1e6));
}

static O42Value
fn_fdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, d1, d2;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, d1);
  ARG_NUMBER (2, d2);
  if (x < 0 || d1 < 1 || d2 < 1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (1 - f_cdf (x, floor (d1), floor (d2)));
}

static O42Value
fn_finv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p, d1, d2;
  (void) n;
  ARG_NUMBER (0, p);
  ARG_NUMBER (1, d1);
  ARG_NUMBER (2, d2);
  if (p <= 0 || p > 1 || d1 < 1 || d2 < 1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (invert_cdf (f_cdf, 1 - p, floor (d1), floor (d2), 0, 1e6));
}

static O42Value
fn_gammadist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, alpha, beta;
  gboolean cumulative;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, alpha);
  ARG_NUMBER (2, beta);
  if (!optional_bool (ctx, args, n, 3, TRUE, &cumulative))
    return o42_value_error (O42_ERR_VALUE);
  if (x < 0 || alpha <= 0 || beta <= 0)
    return o42_value_error (O42_ERR_NUM);
  if (cumulative)
    return o42_value_number (gamma_cdf (x, alpha, beta));
  return o42_value_number (exp ((alpha - 1) * log (x) - x / beta - lgamma (alpha) - alpha * log (beta)));
}

static O42Value
fn_gammainv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p, alpha, beta;
  (void) n;
  ARG_NUMBER (0, p);
  ARG_NUMBER (1, alpha);
  ARG_NUMBER (2, beta);
  if (p < 0 || p >= 1 || alpha <= 0 || beta <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (invert_cdf (gamma_cdf, p, alpha, beta, 0, 1e7));
}

static O42Value
fn_betadist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, a, b, lo = 0, hi = 1;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, a);
  ARG_NUMBER (2, b);
  if (n >= 4) ARG_NUMBER (3, lo);
  if (n >= 5) ARG_NUMBER (4, hi);
  if (a <= 0 || b <= 0 || x < lo || x > hi || lo == hi)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (beta_cdf ((x - lo) / (hi - lo), a, b));
}

static O42Value
fn_betainv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p, a, b, lo = 0, hi = 1;
  ARG_NUMBER (0, p);
  ARG_NUMBER (1, a);
  ARG_NUMBER (2, b);
  if (n >= 4) ARG_NUMBER (3, lo);
  if (n >= 5) ARG_NUMBER (4, hi);
  if (a <= 0 || b <= 0 || p <= 0 || p >= 1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (lo + (hi - lo) * invert_cdf (beta_cdf, p, a, b, 0, 1));
}

static O42Value
fn_expondist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, lambda;
  gboolean cumulative;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, lambda);
  if (!optional_bool (ctx, args, n, 2, TRUE, &cumulative))
    return o42_value_error (O42_ERR_VALUE);
  if (x < 0 || lambda <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (cumulative ? 1 - exp (-lambda * x) : lambda * exp (-lambda * x));
}

static O42Value
fn_poisson (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, mean;
  gboolean cumulative;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, mean);
  if (!optional_bool (ctx, args, n, 2, TRUE, &cumulative))
    return o42_value_error (O42_ERR_VALUE);
  x = floor (x);
  if (x < 0 || mean < 0)
    return o42_value_error (O42_ERR_NUM);
  if (!cumulative)
    return o42_value_number (exp (x * log (mean) - mean - lgamma (x + 1)));
  /* The cumulative Poisson is the complement of an incomplete gamma. */
  return o42_value_number (1 - gamma_p (x + 1, mean));
}

static O42Value
fn_binomdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double k, trials, p;
  gboolean cumulative;
  double total = 0;
  (void) n;
  ARG_NUMBER (0, k);
  ARG_NUMBER (1, trials);
  ARG_NUMBER (2, p);
  if (!optional_bool (ctx, args, n, 3, FALSE, &cumulative))
    return o42_value_error (O42_ERR_VALUE);
  k = floor (k); trials = floor (trials);
  if (k < 0 || k > trials || p < 0 || p > 1)
    return o42_value_error (O42_ERR_NUM);

  for (double i = cumulative ? 0 : k; i <= k; i += 1)
    total += exp (lgamma (trials + 1) - lgamma (i + 1) - lgamma (trials - i + 1)
                  + (i > 0 ? i * log (p) : 0) + (trials - i > 0 ? (trials - i) * log (1 - p) : 0));
  return o42_value_number (total);
}

static O42Value
fn_lognormdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, mean, sd;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, mean);
  ARG_NUMBER (2, sd);
  if (x <= 0 || sd <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (normal_cdf ((log (x) - mean) / sd));
}

static O42Value
fn_weibull (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, alpha, beta;
  gboolean cumulative;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, alpha);
  ARG_NUMBER (2, beta);
  if (!optional_bool (ctx, args, n, 3, TRUE, &cumulative))
    return o42_value_error (O42_ERR_VALUE);
  if (x < 0 || alpha <= 0 || beta <= 0)
    return o42_value_error (O42_ERR_NUM);
  if (cumulative)
    return o42_value_number (1 - exp (-pow (x / beta, alpha)));
  return o42_value_number (alpha / beta * pow (x / beta, alpha - 1) * exp (-pow (x / beta, alpha)));
}

static O42Value
fn_gammaln (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  if (x <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (lgamma (x));
}

static O42Value
fn_confidence (O42EvalContext *ctx, O42Operand *args, int n)
{
  double alpha, sd, size;
  (void) n;
  ARG_NUMBER (0, alpha);
  ARG_NUMBER (1, sd);
  ARG_NUMBER (2, size);
  if (alpha <= 0 || alpha >= 1 || sd <= 0 || size < 1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (normal_inverse (1 - alpha / 2) * sd / sqrt (floor (size)));
}

/* TRIMMEAN drops a fraction of the values from each end, rounded down to
 * an even count, then averages the rest. */
static O42Value
fn_trimmean (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double fraction, sum = 0;
  guint drop, count;

  (void) n;
  ARG_NUMBER (1, fraction);
  if (fraction < 0 || fraction >= 1)
    return o42_value_error (O42_ERR_NUM);
  if (!collect_numbers (ctx, args, 1, &values, &err))
    return o42_value_error (err);

  count = values->len;
  if (count == 0)
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NUM); }

  g_array_sort (values, compare_doubles);
  drop = (guint) floor (count * fraction / 2);
  for (guint i = drop; i < count - drop; i++)
    sum += g_array_index (values, double, i);

  g_array_free (values, TRUE);
  return o42_value_number (sum / (count - 2 * drop));
}

static O42Value
fn_skew_kurt (O42EvalContext *ctx, O42Operand *args, int n, gboolean kurt)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double mean, ssd, sd, m3 = 0, m4 = 0;
  double count;

  if (!collect_numbers (ctx, args, n, &values, &err))
    return o42_value_error (err);

  count = values->len;
  if (count < (kurt ? 4 : 3))
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_DIV0); }

  moments (values, &mean, &ssd);
  sd = sqrt (ssd / (count - 1));
  if (sd == 0)
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_DIV0); }

  for (guint i = 0; i < values->len; i++)
    {
      double z = (g_array_index (values, double, i) - mean) / sd;
      m3 += z * z * z;
      m4 += z * z * z * z;
    }
  g_array_free (values, TRUE);

  if (!kurt)
    return o42_value_number (count / ((count - 1) * (count - 2)) * m3);

  return o42_value_number (count * (count + 1) / ((count - 1) * (count - 2) * (count - 3)) * m4
                           - 3 * (count - 1) * (count - 1) / ((count - 2) * (count - 3)));
}

static O42Value fn_skew (O42EvalContext *c, O42Operand *a, int n) { return fn_skew_kurt (c, a, n, FALSE); }
static O42Value fn_kurt (O42EvalContext *c, O42Operand *a, int n) { return fn_skew_kurt (c, a, n, TRUE); }

const O42Function O42_FUNCS_DISTRIBUTIONS[] = {
  { "BETA.INV", 3, 5, fn_betainv },
  { "BETADIST", 3, 5, fn_betadist },
  { "BETAINV", 3, 5, fn_betainv },
  { "BINOM.DIST", 4, 4, fn_binomdist },
  { "BINOMDIST", 4, 4, fn_binomdist },
  { "CHIDIST", 2, 2, fn_chidist },
  { "CHIINV", 2, 2, fn_chiinv },
  { "CHISQ.DIST.RT", 2, 2, fn_chidist },
  { "CHISQ.INV.RT", 2, 2, fn_chiinv },
  { "CONFIDENCE", 3, 3, fn_confidence },
  { "CONFIDENCE.NORM", 3, 3, fn_confidence },
  { "EXPON.DIST", 3, 3, fn_expondist },
  { "EXPONDIST", 2, 3, fn_expondist },
  { "F.DIST.RT", 3, 3, fn_fdist },
  { "F.INV.RT", 3, 3, fn_finv },
  { "FDIST", 3, 3, fn_fdist },
  { "FINV", 3, 3, fn_finv },
  { "GAMMA.DIST", 4, 4, fn_gammadist },
  { "GAMMA.INV", 3, 3, fn_gammainv },
  { "GAMMADIST", 3, 4, fn_gammadist },
  { "GAMMAINV", 3, 3, fn_gammainv },
  { "GAMMALN", 1, 1, fn_gammaln },
  { "GAMMALN.PRECISE", 1, 1, fn_gammaln },
  { "KURT", 1, -1, fn_kurt },
  { "LOGNORMDIST", 3, 3, fn_lognormdist },
  { "POISSON", 2, 3, fn_poisson },
  { "POISSON.DIST", 3, 3, fn_poisson },
  { "SKEW", 1, -1, fn_skew },
  { "T.INV.2T", 2, 2, fn_tinv },
  { "TDIST", 3, 3, fn_tdist },
  { "TINV", 2, 2, fn_tinv },
  { "TRIMMEAN", 2, 2, fn_trimmean },
  { "WEIBULL", 3, 4, fn_weibull },
  { "WEIBULL.DIST", 4, 4, fn_weibull },
  { NULL, 0, 0, NULL }
};

const O42FunctionHelp O42_HELP_DISTRIBUTIONS[] = {
  { "BETA.INV", "BETA.INV(probability, alpha, beta, A, B)", "The inverse of the cumulative beta distribution." },
  { "BETADIST", "BETADIST(x, alpha, beta, A, B)", "The cumulative beta distribution." },
  { "BETAINV", "BETAINV(probability, alpha, beta, A, B)", "The inverse of the cumulative beta distribution." },
  { "BINOM.DIST", "BINOM.DIST(number_s, trials, probability_s, cumulative)", "The binomial distribution." },
  { "BINOMDIST", "BINOMDIST(successes, trials, probability, cumulative)", "The binomial distribution." },
  { "CHIDIST", "CHIDIST(x, degrees_freedom)", "The right tail of the chi-squared distribution." },
  { "CHIINV", "CHIINV(probability, degrees_freedom)", "The inverse of the right-tailed chi-squared distribution." },
  { "CHISQ.DIST.RT", "CHISQ.DIST.RT(x, deg_freedom)", "The right tail of the chi-squared distribution." },
  { "CHISQ.INV.RT", "CHISQ.INV.RT(probability, deg_freedom)", "The inverse of the right-tailed chi-squared distribution." },
  { "CONFIDENCE", "CONFIDENCE(alpha, standard_dev, size)", "The half-width of a confidence interval for a mean." },
  { "CONFIDENCE.NORM", "CONFIDENCE.NORM(alpha, standard_dev, size)", "Half the width of a confidence interval for a mean." },
  { "EXPON.DIST", "EXPON.DIST(x, lambda, cumulative)", "The exponential distribution." },
  { "EXPONDIST", "EXPONDIST(x, lambda, cumulative)", "The exponential distribution." },
  { "F.DIST.RT", "F.DIST.RT(x, deg_freedom1, deg_freedom2)", "The right tail of the F distribution." },
  { "F.INV.RT", "F.INV.RT(probability, deg_freedom1, deg_freedom2)", "The inverse of the right-tailed F distribution." },
  { "FDIST", "FDIST(x, degrees_freedom1, degrees_freedom2)", "The right tail of the F distribution." },
  { "FINV", "FINV(probability, degrees_freedom1, degrees_freedom2)", "The inverse of the right-tailed F distribution." },
  { "GAMMA.DIST", "GAMMA.DIST(x, alpha, beta, cumulative)", "The gamma distribution." },
  { "GAMMA.INV", "GAMMA.INV(probability, alpha, beta)", "The inverse of the cumulative gamma distribution." },
  { "GAMMADIST", "GAMMADIST(x, alpha, beta, cumulative)", "The gamma distribution." },
  { "GAMMAINV", "GAMMAINV(probability, alpha, beta)", "The inverse of the cumulative gamma distribution." },
  { "GAMMALN", "GAMMALN(x)", "The natural logarithm of the gamma function." },
  { "GAMMALN.PRECISE", "GAMMALN.PRECISE(x)", "The natural logarithm of the gamma function." },
  { "KURT", "KURT(number1, number2, ...)", "The kurtosis of a sample." },
  { "LOGNORMDIST", "LOGNORMDIST(x, mean, standard_dev)", "The cumulative lognormal distribution." },
  { "POISSON", "POISSON(x, mean, cumulative)", "The Poisson distribution." },
  { "POISSON.DIST", "POISSON.DIST(x, mean, cumulative)", "The Poisson distribution." },
  { "SKEW", "SKEW(number1, number2, ...)", "The skewness of a sample." },
  { "T.INV.2T", "T.INV.2T(probability, deg_freedom)", "The inverse of the two-tailed t distribution." },
  { "TDIST", "TDIST(x, degrees_freedom, tails)", "The tail of Student's t distribution, one or both." },
  { "TINV", "TINV(probability, degrees_freedom)", "The two-tailed inverse of Student's t distribution." },
  { "TRIMMEAN", "TRIMMEAN(range, percent)", "The mean with a fraction of the extremes left out." },
  { "WEIBULL", "WEIBULL(x, alpha, beta, cumulative)", "The Weibull distribution." },
  { "WEIBULL.DIST", "WEIBULL.DIST(x, alpha, beta, cumulative)", "The Weibull distribution." },
  { NULL, NULL, NULL }
};
