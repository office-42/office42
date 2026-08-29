/* o42-fn-random.c - see o42-eval-private.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-eval-private.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The names this file has always called them. */
#define operand_value o42_operand_value
#define normal_cdf o42_normal_cdf
#define normal_pdf o42_normal_pdf
#define collect_numbers o42_collect_numbers

/* ---- Random numbers from the distributions --------------------------- */

/* Gnumeric draws from three dozen distributions; these are the ones
 * that matter, built on the uniform draw the rest of the program
 * already uses.  Every one of them is volatile, as RAND is.  */

static double
rand_uniform_open (void)
{
  /* In (0, 1): the logarithms below cannot take a zero. */
  double u;

  do
    u = g_random_double ();
  while (u <= 0 || u >= 1);
  return u;
}

/* A standard normal by the polar form of Box and Muller, which needs
 * no trigonometry and gives two at a time; the spare is kept. */
static double
rand_standard_normal (void)
{
  static gboolean have_spare = FALSE;
  static double spare = 0;
  double u, v, s;

  if (have_spare)
    {
      have_spare = FALSE;
      return spare;
    }
  do
    {
      u = 2 * g_random_double () - 1;
      v = 2 * g_random_double () - 1;
      s = u * u + v * v;
    }
  while (s >= 1 || s == 0);
  s = sqrt (-2 * log (s) / s);
  spare = v * s;
  have_spare = TRUE;
  return u * s;
}

/* A gamma variate by the method of Marsaglia and Tsang, with the
 * boost that carries a shape below one. */
static double
rand_gamma (double shape, double scale)
{
  double d, c, x, v, u;

  if (shape <= 0 || scale <= 0)
    return NAN;
  if (shape < 1)
    return rand_gamma (shape + 1, scale) * pow (rand_uniform_open (), 1 / shape);

  d = shape - 1.0 / 3.0;
  c = 1 / sqrt (9 * d);
  for (;;)
    {
      do
        {
          x = rand_standard_normal ();
          v = 1 + c * x;
        }
      while (v <= 0);
      v = v * v * v;
      u = rand_uniform_open ();
      if (u < 1 - 0.0331 * x * x * x * x)
        return d * v * scale;
      if (log (u) < 0.5 * x * x + d * (1 - v + log (v)))
        return d * v * scale;
    }
}

static double
rand_chisq (double df)
{
  return rand_gamma (df / 2, 2);
}

static double
rand_poisson (double lambda)
{
  if (lambda <= 0)
    return 0;
  if (lambda < 30)
    {
      /* Knuth's method: multiply uniforms until they fall below e^-l. */
      double limit = exp (-lambda), product = 1;
      int count = 0;

      do
        {
          product *= g_random_double ();
          count++;
        }
      while (product > limit && count < 1000000);
      return count - 1;
    }
  /* Far out, a normal with a continuity correction is close enough and
   * does not take a million multiplications. */
  return MAX (0, floor (lambda + sqrt (lambda) * rand_standard_normal () + 0.5));
}

static double
rand_binomial (double p, double trials)
{
  double count = 0;

  if (p <= 0 || trials <= 0)
    return 0;
  if (p >= 1)
    return floor (trials);
  if (trials > 1000)
    {
      /* Many trials: the normal approximation, rounded. */
      double mean = trials * p, sd = sqrt (trials * p * (1 - p));

      return CLAMP (floor (mean + sd * rand_standard_normal () + 0.5), 0, floor (trials));
    }
  for (int i = 0; i < (int) trials; i++)
    if (g_random_double () < p)
      count++;
  return count;
}

/* The arguments every one of these takes, with the count. */
#define RAND_ARGS(one, two, three)                                       \
  G_STMT_START {                                                         \
    if (n >= 1) ARG_NUMBER (0, one);                                     \
    if (n >= 2) ARG_NUMBER (1, two);                                     \
    if (n >= 3) ARG_NUMBER (2, three);                                   \
  } G_STMT_END

static O42Value
fn_randuniform (O42EvalContext *ctx, O42Operand *args, int n)
{
  double lo = 0, hi = 1, unused = 0;

  RAND_ARGS (lo, hi, unused);
  if (hi < lo)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (lo + (hi - lo) * g_random_double ());
}

static O42Value
fn_randnorm (O42EvalContext *ctx, O42Operand *args, int n)
{
  double mean = 0, sd = 1, unused = 0;

  RAND_ARGS (mean, sd, unused);
  if (sd < 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (mean + sd * rand_standard_normal ());
}

static O42Value
fn_randsnorm (O42EvalContext *ctx, O42Operand *args, int n)
{
  (void) ctx; (void) args; (void) n;
  return o42_value_number (rand_standard_normal ());
}

static O42Value
fn_randbernoulli (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p = 0.5, unused = 0;

  RAND_ARGS (p, unused, unused);
  if (p < 0 || p > 1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (g_random_double () < p ? 1 : 0);
}

static O42Value
fn_randbinom (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p = 0.5, trials = 1, unused = 0;

  RAND_ARGS (p, trials, unused);
  if (p < 0 || p > 1 || trials < 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (rand_binomial (p, floor (trials)));
}

static O42Value
fn_randnegbinom (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p = 0.5, failures = 1, count = 0, unused = 0;

  RAND_ARGS (p, failures, unused);
  /* p of one is every draw a success and no failure ever, so there is
   * no number to give back: the distribution has none. */
  if (p <= 0 || p >= 1 || failures < 1)
    return o42_value_error (O42_ERR_NUM);
  /* How many successes before the given number of failures. */
  for (int seen = 0; seen < (int) failures; )
    {
      if (g_random_double () < p)
        count++;
      else
        seen++;
    }
  return o42_value_number (count);
}

static O42Value
fn_randpoisson (O42EvalContext *ctx, O42Operand *args, int n)
{
  double lambda = 1, unused = 0;

  RAND_ARGS (lambda, unused, unused);
  if (lambda < 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (rand_poisson (lambda));
}

static O42Value
fn_randgeom (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p = 0.5, unused = 0;

  RAND_ARGS (p, unused, unused);
  if (p <= 0 || p > 1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (ceil (log (rand_uniform_open ()) / log (1 - p)));
}

static O42Value
fn_randhyperg (O42EvalContext *ctx, O42Operand *args, int n)
{
  double n1 = 1, n2 = 1, taken = 1;
  double left1, left2, drawn = 0;

  RAND_ARGS (n1, n2, taken);
  if (n1 < 0 || n2 < 0 || taken < 0 || taken > n1 + n2)
    return o42_value_error (O42_ERR_NUM);

  /* Drawing from an urn without putting anything back. */
  left1 = floor (n1);
  left2 = floor (n2);
  for (int i = 0; i < (int) taken; i++)
    {
      double total = left1 + left2;

      if (total <= 0)
        break;
      if (g_random_double () < left1 / total)
        { drawn++; left1--; }
      else
        left2--;
    }
  return o42_value_number (drawn);
}

static O42Value
fn_randexp (O42EvalContext *ctx, O42Operand *args, int n)
{
  double b = 1, unused = 0;

  RAND_ARGS (b, unused, unused);
  if (b <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (-b * log (rand_uniform_open ()));
}

static O42Value
fn_randgamma (O42EvalContext *ctx, O42Operand *args, int n)
{
  double shape = 1, scale = 1, unused = 0;
  double v;

  RAND_ARGS (shape, scale, unused);
  v = rand_gamma (shape, scale);
  if (isnan (v))
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (v);
}

static O42Value
fn_randbeta (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a = 1, b = 1, unused = 0;
  double x, y;

  RAND_ARGS (a, b, unused);
  if (a <= 0 || b <= 0)
    return o42_value_error (O42_ERR_NUM);
  x = rand_gamma (a, 1);
  y = rand_gamma (b, 1);
  if (x + y <= 0)
    return o42_value_number (0);
  return o42_value_number (x / (x + y));
}

static O42Value
fn_randchisq (O42EvalContext *ctx, O42Operand *args, int n)
{
  double df = 1, unused = 0;

  RAND_ARGS (df, unused, unused);
  if (df <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (rand_chisq (df));
}

static O42Value
fn_randtdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double df = 1, unused = 0;
  double chi;

  RAND_ARGS (df, unused, unused);
  if (df <= 0)
    return o42_value_error (O42_ERR_NUM);
  chi = rand_chisq (df);
  if (chi <= 0)
    return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (rand_standard_normal () / sqrt (chi / df));
}

static O42Value
fn_randfdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double df1 = 1, df2 = 1, unused = 0;
  double a, b;

  RAND_ARGS (df1, df2, unused);
  if (df1 <= 0 || df2 <= 0)
    return o42_value_error (O42_ERR_NUM);
  a = rand_chisq (df1) / df1;
  b = rand_chisq (df2) / df2;
  if (b <= 0)
    return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (a / b);
}

static O42Value
fn_randcauchy (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a = 1, unused = 0;

  RAND_ARGS (a, unused, unused);
  if (a <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (a * tan (G_PI * (rand_uniform_open () - 0.5)));
}

static O42Value
fn_randlaplace (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a = 1, u, unused = 0;

  RAND_ARGS (a, unused, unused);
  if (a <= 0)
    return o42_value_error (O42_ERR_NUM);
  u = rand_uniform_open () - 0.5;
  return o42_value_number (-a * (u < 0 ? -1 : 1) * log (1 - 2 * fabs (u)));
}

static O42Value
fn_randlogistic (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a = 1, u, unused = 0;

  RAND_ARGS (a, unused, unused);
  if (a <= 0)
    return o42_value_error (O42_ERR_NUM);
  u = rand_uniform_open ();
  return o42_value_number (a * log (u / (1 - u)));
}

static O42Value
fn_randlognorm (O42EvalContext *ctx, O42Operand *args, int n)
{
  double zeta = 0, sigma = 1, unused = 0;

  RAND_ARGS (zeta, sigma, unused);
  if (sigma < 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (exp (zeta + sigma * rand_standard_normal ()));
}

static O42Value
fn_randpareto (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a = 1, b = 1, unused = 0;

  RAND_ARGS (a, b, unused);
  if (a <= 0 || b <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (b / pow (rand_uniform_open (), 1 / a));
}

static O42Value
fn_randrayleigh (O42EvalContext *ctx, O42Operand *args, int n)
{
  double sigma = 1, unused = 0;

  RAND_ARGS (sigma, unused, unused);
  if (sigma <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (sigma * sqrt (-2 * log (rand_uniform_open ())));
}

static O42Value
fn_randrayleightail (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a = 1, sigma = 1, unused = 0;

  RAND_ARGS (a, sigma, unused);
  if (sigma <= 0 || a < 0)
    return o42_value_error (O42_ERR_NUM);
  /* The Rayleigh distribution from `a` upwards. */
  return o42_value_number (sqrt (a * a - 2 * sigma * sigma * log (rand_uniform_open ())));
}

static O42Value
fn_randweibull (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a = 1, b = 1, unused = 0;

  RAND_ARGS (a, b, unused);
  if (a <= 0 || b <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (a * pow (-log (rand_uniform_open ()), 1 / b));
}

static O42Value
fn_randgumbel (O42EvalContext *ctx, O42Operand *args, int n, gboolean second)
{
  double a = 1, b = 1, unused = 0;
  double u;

  RAND_ARGS (a, b, unused);
  if (a <= 0 || b <= 0)
    return o42_value_error (O42_ERR_NUM);
  u = rand_uniform_open ();
  if (second)
    return o42_value_number (pow (-b / log (u), 1 / a));
  return o42_value_number (-log (-log (u) / b) / a);
}

static O42Value fn_randgumbel1 (O42EvalContext *c, O42Operand *a, int n) { return fn_randgumbel (c, a, n, FALSE); }
static O42Value fn_randgumbel2 (O42EvalContext *c, O42Operand *a, int n) { return fn_randgumbel (c, a, n, TRUE); }

static O42Value
fn_randexppow (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a = 1, b = 2, unused = 0;
  double v;

  RAND_ARGS (a, b, unused);
  if (a <= 0 || b <= 0)
    return o42_value_error (O42_ERR_NUM);
  /* |x| has a gamma distribution with shape 1/b; the sign is even. */
  v = a * pow (rand_gamma (1 / b, 1), 1 / b);
  return o42_value_number (g_random_boolean () ? v : -v);
}

static O42Value
fn_randlog (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p = 0.5, unused = 0;
  double u, v, q;

  RAND_ARGS (p, unused, unused);
  if (p <= 0 || p >= 1)
    return o42_value_error (O42_ERR_NUM);
  /* Kemp's method for the logarithmic distribution. */
  q = 1 - p;
  u = rand_uniform_open ();
  if (u > p)
    return o42_value_number (1);
  v = rand_uniform_open ();
  {
    double top = 1 - pow (q, v);

    if (top <= 0)
      return o42_value_number (1);
    return o42_value_number (MAX (1, floor (log (u) / log (top) + 1)));
  }
}

/* RANDDISCRETE(values, [probabilities]): one of the values, drawn with
 * the given weights, or evenly when none are given. */
static O42Value
fn_randdiscrete (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values, *weights = NULL;
  O42ErrorCode err = O42_ERR_VALUE;
  double total = 0, point;
  guint chosen = 0;

  if (!collect_numbers (ctx, args, 1, &values, &err))
    return o42_value_error (err);
  if (values->len == 0)
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NUM); }

  if (n >= 2 && !collect_numbers (ctx, &args[1], 1, &weights, &err))
    { g_array_free (values, TRUE); return o42_value_error (err); }

  if (weights != NULL && weights->len == values->len)
    {
      for (guint i = 0; i < weights->len; i++)
        total += MAX (g_array_index (weights, double, i), 0);
      if (total > 0)
        {
          point = g_random_double () * total;
          for (guint i = 0; i < weights->len; i++)
            {
              point -= MAX (g_array_index (weights, double, i), 0);
              if (point <= 0)
                { chosen = i; break; }
              chosen = i;
            }
        }
    }
  else
    chosen = (guint) g_random_int_range (0, (gint32) values->len);

  point = g_array_index (values, double, MIN (chosen, values->len - 1));
  g_array_free (values, TRUE);
  if (weights != NULL)
    g_array_free (weights, TRUE);
  return o42_value_number (point);
}

/* ---- More of Gnumeric's random numbers -------------------------------- */

/* A standard normal, which is RANDNORM(0, 1) said shorter. */
static O42Value
fn_randstnorm (O42EvalContext *ctx, O42Operand *args, int n)
{
  (void) ctx; (void) args; (void) n;
  return o42_value_number (rand_standard_normal ());
}

/* The tail of a normal distribution above `a`, by Marsaglia's method:
 * drawing from the whole distribution and throwing away everything
 * below the cut would take forever when the cut is far out. */
static O42Value
fn_randnormtail (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a, sigma = 1, unused = 0;
  double s, u, v, x;

  RAND_ARGS (a, sigma, unused);
  if (sigma <= 0 || a <= 0)
    return o42_value_error (O42_ERR_NUM);
  s = a / sigma;
  do
    {
      u = rand_uniform_open ();
      do
        v = rand_uniform_open ();
      while (v == 0);
      x = sqrt (s * s - 2 * log (v));
    }
  while (x * u > s);
  return o42_value_number (x * sigma);
}

/* The logarithmic distribution: the number of times something is
 * counted when each further count is less likely by p. */
static O42Value
fn_randlogarithmic (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p, unused = 0, unused2 = 0;
  double c, q, u, v;

  RAND_ARGS (p, unused, unused2);
  if (p <= 0 || p >= 1)
    return o42_value_error (O42_ERR_NUM);

  c = log1p (-p);
  u = rand_uniform_open ();
  if (u >= p)
    return o42_value_number (1);
  v = rand_uniform_open ();
  q = -expm1 (c * v);
  if (u <= q * q)
    {
      double k = floor (1 + log (u) / log (q));

      return o42_value_number (k < 1 ? 1 : k);
    }
  return o42_value_number (u <= q ? 2 : 1);
}

/* Levy's alpha-stable distribution, by the method of Chambers, Mallows
 * and Stuck: with alpha of 2 it is the normal, with 1 Cauchy's, and
 * between them the heavy tails that have no variance.  `beta` skews
 * it. */
static O42Value
fn_randlevy (O42EvalContext *ctx, O42Operand *args, int n)
{
  double c, alpha, beta = 0;
  double u, v, t, s;

  RAND_ARGS (c, alpha, beta);
  if (alpha <= 0 || alpha > 2 || beta < -1 || beta > 1)
    return o42_value_error (O42_ERR_NUM);

  do
    u = G_PI * (rand_uniform_open () - 0.5);
  while (u == 0);
  do
    v = rand_uniform_open ();
  while (v == 0);
  v = -log (v);            /* an exponential deviate */

  if (beta == 0)
    {
      if (alpha == 1)
        return o42_value_number (c * tan (u));
      return o42_value_number (c * pow (v * cos ((1 - alpha) * u) , 1 / alpha - 1) *
                               sin (alpha * u) / pow (cos (u), 1 / alpha));
    }

  if (fabs (alpha - 1) < 1e-12)
    {
      t = G_PI_2 + beta * u;
      s = t * tan (u) - beta * log (G_PI_2 * v * cos (u) / t);
      return o42_value_number (c * 2 / G_PI * s);
    }

  t = beta * tan (G_PI * alpha / 2);
  s = pow (1 + t * t, 1 / (2 * alpha));
  t = atan (t) / alpha;
  return o42_value_number (c * s * sin (alpha * (u + t)) / pow (cos (u), 1 / alpha) *
                           pow (cos (u - alpha * (u + t)) / v, (1 - alpha) / alpha));
}

#undef RAND_ARGS

const O42Function O42_FUNCS_RANDOM[] = {
  { "RANDBERNOULLI", 1, 1, fn_randbernoulli },
  { "RANDBETA", 2, 2, fn_randbeta },
  { "RANDBINOM", 2, 2, fn_randbinom },
  { "RANDCAUCHY", 1, 1, fn_randcauchy },
  { "RANDCHISQ", 1, 1, fn_randchisq },
  { "RANDDISCRETE", 1, 2, fn_randdiscrete },
  { "RANDEXP", 1, 1, fn_randexp },
  { "RANDEXPPOW", 2, 2, fn_randexppow },
  { "RANDFDIST", 2, 2, fn_randfdist },
  { "RANDGAMMA", 2, 2, fn_randgamma },
  { "RANDGEOM", 1, 1, fn_randgeom },
  { "RANDGUMBEL1", 2, 2, fn_randgumbel1 },
  { "RANDGUMBEL2", 2, 2, fn_randgumbel2 },
  { "RANDHYPERG", 3, 3, fn_randhyperg },
  { "RANDLAPLACE", 1, 1, fn_randlaplace },
  { "RANDLEVY", 2, 3, fn_randlevy },
  { "RANDLOG", 1, 1, fn_randlog },
  { "RANDLOGARITHMIC", 1, 1, fn_randlogarithmic },
  { "RANDLOGISTIC", 1, 1, fn_randlogistic },
  { "RANDLOGNORM", 2, 2, fn_randlognorm },
  { "RANDNEGBINOM", 2, 2, fn_randnegbinom },
  { "RANDNORM", 2, 2, fn_randnorm },
  { "RANDNORMTAIL", 1, 2, fn_randnormtail },
  { "RANDPARETO", 2, 2, fn_randpareto },
  { "RANDPOISSON", 1, 1, fn_randpoisson },
  { "RANDRAYLEIGH", 1, 1, fn_randrayleigh },
  { "RANDRAYLEIGHTAIL", 2, 2, fn_randrayleightail },
  { "RANDSNORM", 0, 0, fn_randsnorm },
  { "RANDSTNORM", 0, 0, fn_randstnorm },
  { "RANDTDIST", 1, 1, fn_randtdist },
  { "RANDUNIFORM", 2, 2, fn_randuniform },
  { "RANDWEIBULL", 2, 2, fn_randweibull },
  { NULL, 0, 0, NULL }
};

const O42FunctionHelp O42_HELP_RANDOM[] = {
  { "RANDBERNOULLI", "RANDBERNOULLI(p)", "One or nothing, with the given chance of one." },
  { "RANDBETA", "RANDBETA(a, b)", "A random number from the beta distribution." },
  { "RANDBINOM", "RANDBINOM(p, trials)", "How many successes in so many trials." },
  { "RANDCAUCHY", "RANDCAUCHY(a)", "A random number from Cauchy's distribution." },
  { "RANDCHISQ", "RANDCHISQ(df)", "A random number from the chi-squared distribution." },
  { "RANDDISCRETE", "RANDDISCRETE(values, probabilities)", "One of the values, drawn with the given weights." },
  { "RANDEXP", "RANDEXP(b)", "A random number from the exponential distribution." },
  { "RANDEXPPOW", "RANDEXPPOW(a, b)", "A random number from the exponential power distribution." },
  { "RANDFDIST", "RANDFDIST(df1, df2)", "A random number from Fisher's F distribution." },
  { "RANDGAMMA", "RANDGAMMA(shape, scale)", "A random number from the gamma distribution." },
  { "RANDGEOM", "RANDGEOM(p)", "How many trials until the first success." },
  { "RANDGUMBEL1", "RANDGUMBEL1(a, b)", "A random number from Gumbel's first distribution." },
  { "RANDGUMBEL2", "RANDGUMBEL2(a, b)", "A random number from Gumbel's second distribution." },
  { "RANDHYPERG", "RANDHYPERG(n1, n2, t)", "How many of the first kind in a draw from an urn." },
  { "RANDLAPLACE", "RANDLAPLACE(a)", "A random number from Laplace's distribution." },
  { "RANDLEVY", "RANDLEVY(c, alpha, beta)", "A random number from Levy's alpha-stable distribution." },
  { "RANDLOG", "RANDLOG(p)", "A random number from the logarithmic distribution." },
  { "RANDLOGARITHMIC", "RANDLOGARITHMIC(p)", "A random number from the logarithmic distribution." },
  { "RANDLOGISTIC", "RANDLOGISTIC(a)", "A random number from the logistic distribution." },
  { "RANDLOGNORM", "RANDLOGNORM(zeta, sigma)", "A random number from the log-normal distribution." },
  { "RANDNEGBINOM", "RANDNEGBINOM(p, failures)", "How many successes before so many failures." },
  { "RANDNORM", "RANDNORM(mean, stdev)", "A random number from the normal distribution." },
  { "RANDNORMTAIL", "RANDNORMTAIL(a, sigma)", "A random number from the tail of a normal distribution above a." },
  { "RANDPARETO", "RANDPARETO(a, b)", "A random number from Pareto's distribution." },
  { "RANDPOISSON", "RANDPOISSON(lambda)", "A random number from Poisson's distribution." },
  { "RANDRAYLEIGH", "RANDRAYLEIGH(sigma)", "A random number from Rayleigh's distribution." },
  { "RANDRAYLEIGHTAIL", "RANDRAYLEIGHTAIL(a, sigma)", "Rayleigh's distribution from a upwards." },
  { "RANDSNORM", "RANDSNORM()", "A random number from the standard normal distribution." },
  { "RANDSTNORM", "RANDSTNORM()", "A random number from the standard normal distribution." },
  { "RANDTDIST", "RANDTDIST(df)", "A random number from Student's t distribution." },
  { "RANDUNIFORM", "RANDUNIFORM(lower, upper)", "A random number between the two, evenly." },
  { "RANDWEIBULL", "RANDWEIBULL(a, b)", "A random number from Weibull's distribution." },
  { NULL, NULL, NULL }
};
