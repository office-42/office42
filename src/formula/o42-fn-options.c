/* o42-fn-options.c - the option formulas
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What Gnumeric's derivatives plugin prices, in the formulations
 * Haug's book sets out: the generalised Black-Scholes-Merton price and
 * its greeks, and then the ones that are not it -- an American option
 * by Barone-Adesi and Whaley or by a tree, a currency option, jumps, a
 * chooser, a compound, a lookback, an exchange.
 *
 * A cost of carry b covers the underlyings between them: b = r a plain
 * share, b = 0 a future, b = r - q a share paying a dividend yield,
 * b = r - rf a currency.
 */

#include "o42-eval-private.h"

#include <math.h>
#include <string.h>

/* The two names the machine gives these under. */
#define normal_cdf o42_normal_cdf
#define normal_pdf o42_normal_pdf

/* ---- Gnumeric's Black and Scholes -------------------------------------- */

/* The generalised Black-Scholes-Merton price, the one Haug's book sets
 * out: a cost of carry b covers them all -- b = r a plain share, b = 0
 * a future, b = r - q a share paying a dividend yield, b = r - rf a
 * currency.  N() is the normal distribution the rest of the file uses.
 *
 * The flag is Gnumeric's: "c" for a call, "p" for a put. */

typedef struct {
  double d1, d2;
  double carry;      /* e^((b - r) T), the discount on the spot leg */
  double discount;   /* e^(-r T), on the strike leg */
  double root;       /* sigma sqrt(T) */
  gboolean call;
} BlackScholes;

static gboolean
bs_setup (const char *flag, double spot, double strike, double time,
          double rate, double volatility, double carry_rate, BlackScholes *out)
{
  if (spot <= 0 || strike <= 0 || time <= 0 || volatility <= 0)
    return FALSE;
  if (flag != NULL)
    {
      if (flag[0] != 'c' && flag[0] != 'C' && flag[0] != 'p' && flag[0] != 'P')
        return FALSE;
      out->call = flag[0] == 'c' || flag[0] == 'C';
    }
  else
    out->call = TRUE;

  out->root = volatility * sqrt (time);
  out->d1 = (log (spot / strike) + (carry_rate + volatility * volatility / 2) * time) / out->root;
  out->d2 = out->d1 - out->root;
  out->carry = exp ((carry_rate - rate) * time);
  out->discount = exp (-rate * time);
  return TRUE;
}

/* The seven arguments they nearly all take, the flag included. */
#define BS_ARGS(with_flag, bs)                                             \
  G_STMT_START {                                                           \
    double spot_, strike_, time_, rate_, vol_, carry_;                     \
    char *flag_ = NULL;                                                    \
    int at_ = 0;                                                           \
                                                                           \
    if (with_flag) { ARG_TEXT (0, flag_); at_ = 1; }                       \
    ARG_NUMBER (at_ + 0, spot_);                                           \
    ARG_NUMBER (at_ + 1, strike_);                                         \
    ARG_NUMBER (at_ + 2, time_);                                           \
    ARG_NUMBER (at_ + 3, rate_);                                           \
    ARG_NUMBER (at_ + 4, vol_);                                            \
    ARG_NUMBER (at_ + 5, carry_);                                          \
    rate = rate_; carry_rate = carry_; spot = spot_;                       \
    strike = strike_; time = time_; volatility = vol_;                     \
    (void) rate; (void) carry_rate; (void) spot;                           \
    (void) strike; (void) time; (void) volatility;                         \
    if (!bs_setup (flag_, spot_, strike_, time_, rate_, vol_, carry_, &(bs))) \
      { g_free (flag_); return o42_value_error (O42_ERR_NUM); }            \
    g_free (flag_);                                                        \
  } G_STMT_END

static O42Value
fn_opt_bs (O42EvalContext *ctx, O42Operand *args, int n)
{
  BlackScholes bs;
  double spot, strike, time, rate, volatility, carry_rate;

  (void) n;
  BS_ARGS (TRUE, bs);
  if (bs.call)
    return o42_value_number (spot * bs.carry * normal_cdf (bs.d1) -
                             strike * bs.discount * normal_cdf (bs.d2));
  return o42_value_number (strike * bs.discount * normal_cdf (-bs.d2) -
                           spot * bs.carry * normal_cdf (-bs.d1));
}

static O42Value
fn_opt_bs_delta (O42EvalContext *ctx, O42Operand *args, int n)
{
  BlackScholes bs;
  double spot, strike, time, rate, volatility, carry_rate;

  (void) n;
  BS_ARGS (TRUE, bs);
  return o42_value_number (bs.call ? bs.carry * normal_cdf (bs.d1)
                                   : bs.carry * (normal_cdf (bs.d1) - 1));
}

static O42Value
fn_opt_bs_gamma (O42EvalContext *ctx, O42Operand *args, int n)
{
  BlackScholes bs;
  double spot, strike, time, rate, volatility, carry_rate;

  (void) n;
  BS_ARGS (FALSE, bs);
  return o42_value_number (bs.carry * normal_pdf (bs.d1) / (spot * bs.root));
}

static O42Value
fn_opt_bs_vega (O42EvalContext *ctx, O42Operand *args, int n)
{
  BlackScholes bs;
  double spot, strike, time, rate, volatility, carry_rate;

  (void) n;
  BS_ARGS (FALSE, bs);
  return o42_value_number (spot * bs.carry * normal_pdf (bs.d1) * sqrt (time));
}

static O42Value
fn_opt_bs_theta (O42EvalContext *ctx, O42Operand *args, int n)
{
  BlackScholes bs;
  double spot, strike, time, rate, volatility, carry_rate;
  double decay;

  (void) n;
  BS_ARGS (TRUE, bs);
  decay = -spot * bs.carry * normal_pdf (bs.d1) * volatility / (2 * sqrt (time));
  if (bs.call)
    return o42_value_number (decay
                             - (carry_rate - rate) * spot * bs.carry * normal_cdf (bs.d1)
                             - rate * strike * bs.discount * normal_cdf (bs.d2));
  return o42_value_number (decay
                           + (carry_rate - rate) * spot * bs.carry * normal_cdf (-bs.d1)
                           + rate * strike * bs.discount * normal_cdf (-bs.d2));
}

static O42Value
fn_opt_bs_rho (O42EvalContext *ctx, O42Operand *args, int n)
{
  BlackScholes bs;
  double spot, strike, time, rate, volatility, carry_rate;

  (void) n;
  BS_ARGS (TRUE, bs);
  /* With no cost of carry the option is on a future, and the rate moves
   * only the discounting -- so rho is minus the time times the price. */
  if (carry_rate == 0)
    {
      double price = bs.call
                     ? spot * bs.carry * normal_cdf (bs.d1) - strike * bs.discount * normal_cdf (bs.d2)
                     : strike * bs.discount * normal_cdf (-bs.d2) - spot * bs.carry * normal_cdf (-bs.d1);

      return o42_value_number (-time * price);
    }
  return o42_value_number (bs.call
                           ? time * strike * bs.discount * normal_cdf (bs.d2)
                           : -time * strike * bs.discount * normal_cdf (-bs.d2));
}

static O42Value
fn_opt_bs_carrycost (O42EvalContext *ctx, O42Operand *args, int n)
{
  BlackScholes bs;
  double spot, strike, time, rate, volatility, carry_rate;

  (void) n;
  BS_ARGS (TRUE, bs);
  return o42_value_number (bs.call
                           ? time * spot * bs.carry * normal_cdf (bs.d1)
                           : -time * spot * bs.carry * normal_cdf (-bs.d1));
}

/* ---- Gnumeric's other options ----------------------------------------- */

/* The generalised Black-Scholes price on its own, for the formulas
 * below that are built out of it. */
static double
gbs (gboolean call, double spot, double strike, double time,
     double rate, double carry, double volatility)
{
  double root = volatility * sqrt (time);
  double d1, d2;

  if (spot <= 0 || strike <= 0)
    return NAN;
  if (time <= 0 || volatility <= 0)
    {
      double v = call ? spot - strike : strike - spot;

      return MAX (v, 0);
    }
  d1 = (log (spot / strike) + (carry + volatility * volatility / 2) * time) / root;
  d2 = d1 - root;
  if (call)
    return spot * exp ((carry - rate) * time) * normal_cdf (d1) -
           strike * exp (-rate * time) * normal_cdf (d2);
  return strike * exp (-rate * time) * normal_cdf (-d2) -
         spot * exp ((carry - rate) * time) * normal_cdf (-d1);
}

/* "c" or "p", the way Gnumeric asks for it. */
static gboolean
opt_flag (const char *text, gboolean *call)
{
  if (text == NULL || (text[0] != 'c' && text[0] != 'C' && text[0] != 'p' && text[0] != 'P'))
    return FALSE;
  *call = text[0] == 'c' || text[0] == 'C';
  return TRUE;
}

#define OPT_FLAG(index, target)                                          \
  G_STMT_START {                                                         \
    char *flag_ = NULL;                                                  \
    ARG_TEXT (index, flag_);                                             \
    if (!opt_flag (flag_, &(target)))                                    \
      { g_free (flag_); return o42_value_error (O42_ERR_NUM); }          \
    g_free (flag_);                                                      \
  } G_STMT_END

/* A currency option: the carry is the difference between the two
 * interest rates, which is what Garman and Kohlhagen saw. */
static O42Value
fn_opt_garman_kohlhagen (O42EvalContext *ctx, O42Operand *args, int n)
{
  gboolean call;
  double spot, strike, time, domestic, foreign, vol;

  (void) n;
  OPT_FLAG (0, call);
  ARG_NUMBER (1, spot);
  ARG_NUMBER (2, strike);
  ARG_NUMBER (3, time);
  ARG_NUMBER (4, domestic);
  ARG_NUMBER (5, foreign);
  ARG_NUMBER (6, vol);
  if (spot <= 0 || strike <= 0 || time < 0 || vol < 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (gbs (call, spot, strike, time, domestic,
                                domestic - foreign, vol));
}

/* French's: the variance grows with trading time and the carry with
 * calendar time, which are not the same number of days. */
static O42Value
fn_opt_french (O42EvalContext *ctx, O42Operand *args, int n)
{
  gboolean call;
  double spot, strike, time, ttime, rate, vol, carry;
  double root, d1, d2;

  (void) n;
  OPT_FLAG (0, call);
  ARG_NUMBER (1, spot);
  ARG_NUMBER (2, strike);
  ARG_NUMBER (3, time);
  ARG_NUMBER (4, ttime);
  ARG_NUMBER (5, rate);
  ARG_NUMBER (6, vol);
  ARG_NUMBER (7, carry);
  if (spot <= 0 || strike <= 0 || time <= 0 || ttime <= 0 || vol <= 0)
    return o42_value_error (O42_ERR_NUM);

  root = vol * sqrt (ttime);
  d1 = (log (spot / strike) + carry * time + vol * vol / 2 * ttime) / root;
  d2 = d1 - root;
  if (call)
    return o42_value_number (spot * exp ((carry - rate) * time) * normal_cdf (d1) -
                             strike * exp (-rate * time) * normal_cdf (d2));
  return o42_value_number (strike * exp (-rate * time) * normal_cdf (-d2) -
                           spot * exp ((carry - rate) * time) * normal_cdf (-d1));
}

/* Merton's jump diffusion: the price is the average of Black-Scholes
 * prices over how many jumps happen, which is Poisson. */
static O42Value
fn_opt_jump_diff (O42EvalContext *ctx, O42Operand *args, int n)
{
  gboolean call;
  double spot, strike, time, rate, vol, lambda, gamma;
  double delta2, z2, sum = 0, term = 1;

  (void) n;
  OPT_FLAG (0, call);
  ARG_NUMBER (1, spot);
  ARG_NUMBER (2, strike);
  ARG_NUMBER (3, time);
  ARG_NUMBER (4, rate);
  ARG_NUMBER (5, vol);
  ARG_NUMBER (6, lambda);
  ARG_NUMBER (7, gamma);
  if (spot <= 0 || strike <= 0 || time <= 0 || vol <= 0 || lambda <= 0 ||
      gamma <= 0 || gamma > 1)
    return o42_value_error (O42_ERR_NUM);

  delta2 = gamma * vol * vol / lambda;
  z2 = vol * vol - lambda * delta2;
  if (z2 < 0)
    return o42_value_error (O42_ERR_NUM);

  for (int i = 0; i < 51; i++)
    {
      double v = sqrt (z2 + delta2 * (i / time));

      if (i > 0)
        term *= lambda * time / i;
      sum += exp (-lambda * time) * term * gbs (call, spot, strike, time, rate, rate, v);
    }
  return o42_value_number (sum);
}

/* Cox, Ross and Rubinstein's tree, which is where an American option
 * that has no closed form is worked out. */
static O42Value
fn_opt_binomial (O42EvalContext *ctx, O42Operand *args, int n)
{
  gboolean call, american;
  char *kind = NULL;
  double steps, spot, strike, time, rate, vol, carry = 0;
  double dt, u, d, p, discount;
  double *value;
  int m;

  ARG_TEXT (0, kind);
  american = kind != NULL && (kind[0] == 'a' || kind[0] == 'A');
  if (kind == NULL || (kind[0] != 'a' && kind[0] != 'A' && kind[0] != 'e' && kind[0] != 'E'))
    { g_free (kind); return o42_value_error (O42_ERR_NUM); }
  g_free (kind);
  OPT_FLAG (1, call);
  ARG_NUMBER (2, steps);
  ARG_NUMBER (3, spot);
  ARG_NUMBER (4, strike);
  ARG_NUMBER (5, time);
  ARG_NUMBER (6, rate);
  ARG_NUMBER (7, vol);
  if (n >= 9)
    ARG_NUMBER (8, carry);
  else
    carry = rate;
  if (spot <= 0 || strike <= 0 || time <= 0 || vol <= 0 || steps < 1 || steps > 5000)
    return o42_value_error (O42_ERR_NUM);

  m = (int) steps;
  dt = time / m;
  u = exp (vol * sqrt (dt));
  d = 1 / u;
  p = (exp (carry * dt) - d) / (u - d);
  discount = exp (-rate * dt);
  if (p < 0 || p > 1)
    return o42_value_error (O42_ERR_NUM);

  value = g_new (double, (gsize) m + 1);
  for (int i = 0; i <= m; i++)
    {
      double s = spot * pow (u, m - i) * pow (d, i);

      value[i] = MAX (call ? s - strike : strike - s, 0);
    }
  for (int step = m - 1; step >= 0; step--)
    for (int i = 0; i <= step; i++)
      {
        value[i] = discount * (p * value[i] + (1 - p) * value[i + 1]);
        if (american)
          {
            double s = spot * pow (u, step - i) * pow (d, i);

            value[i] = MAX (value[i], call ? s - strike : strike - s);
          }
      }
  {
    double answer = value[0];

    g_free (value);
    return o42_value_number (answer);
  }
}

/* A time-switch option pays a little for every interval the spot
 * spends on the right side of the strike. */
static O42Value
fn_opt_time_switch (O42EvalContext *ctx, O42Operand *args, int n)
{
  gboolean call;
  double spot, strike, amount, time, m, dt, rate, carry, vol;
  double sum = 0;
  int steps;

  (void) n;
  OPT_FLAG (0, call);
  ARG_NUMBER (1, spot);
  ARG_NUMBER (2, strike);
  ARG_NUMBER (3, amount);
  ARG_NUMBER (4, time);
  ARG_NUMBER (5, m);
  ARG_NUMBER (6, dt);
  ARG_NUMBER (7, rate);
  ARG_NUMBER (8, carry);
  ARG_NUMBER (9, vol);
  if (spot <= 0 || strike <= 0 || time <= 0 || dt <= 0 || vol <= 0)
    return o42_value_error (O42_ERR_NUM);

  steps = (int) (time / dt);
  for (int i = 1; i <= steps; i++)
    {
      double t = i * dt;
      double d = (log (spot / strike) + (carry - vol * vol / 2) * t) / (vol * sqrt (t));

      sum += normal_cdf (call ? d : -d) * dt;
    }
  return o42_value_number (amount * exp (-rate * time) * (sum + dt * m));
}

/* A chooser: at time1 the holder says whether it was a call or a put
 * all along, and it runs to time2. */
static O42Value
fn_opt_simple_chooser (O42EvalContext *ctx, O42Operand *args, int n)
{
  double spot, strike, time1, time2, rate, carry, vol;
  double d, y;

  (void) n;
  ARG_NUMBER (0, spot);
  ARG_NUMBER (1, strike);
  ARG_NUMBER (2, time1);
  ARG_NUMBER (3, time2);
  ARG_NUMBER (4, rate);
  ARG_NUMBER (5, carry);
  ARG_NUMBER (6, vol);
  if (spot <= 0 || strike <= 0 || time1 <= 0 || time2 <= time1 || vol <= 0)
    return o42_value_error (O42_ERR_NUM);

  d = (log (spot / strike) + (carry + vol * vol / 2) * time2) / (vol * sqrt (time2));
  y = (log (spot / strike) + carry * time2 + vol * vol * time1 / 2) / (vol * sqrt (time1));

  return o42_value_number (spot * exp ((carry - rate) * time2) * normal_cdf (d) -
                           strike * exp (-rate * time2) * normal_cdf (d - vol * sqrt (time2)) -
                           spot * exp ((carry - rate) * time2) * normal_cdf (-y) +
                           strike * exp (-rate * time2) * normal_cdf (-y + vol * sqrt (time1)));
}

/* A forward-start option: the strike is settled at time1 as a fraction
 * of the spot then. */
static O42Value
fn_opt_forward_start (O42EvalContext *ctx, O42Operand *args, int n)
{
  gboolean call;
  double spot, alpha, time1, time, rate, vol, carry;

  (void) n;
  OPT_FLAG (0, call);
  ARG_NUMBER (1, spot);
  ARG_NUMBER (2, alpha);
  ARG_NUMBER (3, time1);
  ARG_NUMBER (4, time);
  ARG_NUMBER (5, rate);
  ARG_NUMBER (6, vol);
  ARG_NUMBER (7, carry);
  if (spot <= 0 || alpha <= 0 || time <= time1 || time1 < 0 || vol <= 0)
    return o42_value_error (O42_ERR_NUM);

  return o42_value_number (spot * exp ((carry - rate) * time1) *
                           gbs (call, 1, alpha, time - time1, rate, carry, vol));
}

/* Margrabe's: the right to swap one asset for the other, which needs
 * no strike and no interest rate to speak of. */
static O42Value
fn_opt_euro_exchange (O42EvalContext *ctx, O42Operand *args, int n)
{
  double spot1, spot2, qty1, qty2, time, rate, carry1, carry2, vol1, vol2, rho;
  double v, s1, s2, d1, d2;

  (void) n;
  ARG_NUMBER (0, spot1);
  ARG_NUMBER (1, spot2);
  ARG_NUMBER (2, qty1);
  ARG_NUMBER (3, qty2);
  ARG_NUMBER (4, time);
  ARG_NUMBER (5, rate);
  ARG_NUMBER (6, carry1);
  ARG_NUMBER (7, carry2);
  ARG_NUMBER (8, vol1);
  ARG_NUMBER (9, vol2);
  ARG_NUMBER (10, rho);
  if (spot1 <= 0 || spot2 <= 0 || qty1 <= 0 || qty2 <= 0 || time <= 0 ||
      vol1 < 0 || vol2 < 0 || rho < -1 || rho > 1)
    return o42_value_error (O42_ERR_NUM);

  v = sqrt (vol1 * vol1 + vol2 * vol2 - 2 * rho * vol1 * vol2);
  if (v <= 0)
    return o42_value_error (O42_ERR_NUM);
  s1 = qty1 * spot1;
  s2 = qty2 * spot2;
  d1 = (log (s1 / s2) + (carry1 - carry2 + v * v / 2) * time) / (v * sqrt (time));
  d2 = d1 - v * sqrt (time);
  return o42_value_number (s1 * exp ((carry1 - rate) * time) * normal_cdf (d1) -
                           s2 * exp ((carry2 - rate) * time) * normal_cdf (d2));
}

/* An executive option is a plain one that is forfeited at a rate. */
static O42Value
fn_opt_exec (O42EvalContext *ctx, O42Operand *args, int n)
{
  gboolean call;
  double spot, strike, time, rate, vol, carry, lambda;

  (void) n;
  OPT_FLAG (0, call);
  ARG_NUMBER (1, spot);
  ARG_NUMBER (2, strike);
  ARG_NUMBER (3, time);
  ARG_NUMBER (4, rate);
  ARG_NUMBER (5, vol);
  ARG_NUMBER (6, carry);
  ARG_NUMBER (7, lambda);
  if (spot <= 0 || strike <= 0 || time <= 0 || vol <= 0 || lambda < 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (exp (-lambda * time) *
                           gbs (call, spot, strike, time, rate, carry, vol));
}

/* Kirk's approximation to an option on the spread between two
 * futures. */
static O42Value
fn_opt_spread_approx (O42EvalContext *ctx, O42Operand *args, int n)
{
  gboolean call;
  double f1, f2, strike, time, rate, vol1, vol2, rho;
  double v, s;

  (void) n;
  OPT_FLAG (0, call);
  ARG_NUMBER (1, f1);
  ARG_NUMBER (2, f2);
  ARG_NUMBER (3, strike);
  ARG_NUMBER (4, time);
  ARG_NUMBER (5, rate);
  ARG_NUMBER (6, vol1);
  ARG_NUMBER (7, vol2);
  ARG_NUMBER (8, rho);
  if (f1 <= 0 || f2 <= 0 || f2 + strike <= 0 || time <= 0 || rho < -1 || rho > 1)
    return o42_value_error (O42_ERR_NUM);

  s = f2 / (f2 + strike);
  v = sqrt (vol1 * vol1 + vol2 * s * (vol2 * s - 2 * rho * vol1));
  if (v <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number ((f2 + strike) * exp (-rate * time) *
                           gbs (call, f1 / (f2 + strike), 1, time, 0, 0, v));
}

/* The two lookbacks: one settles against the best the spot reached,
 * the other against a fixed strike. */
static O42Value
fn_opt_float_strk_lkbk (O42EvalContext *ctx, O42Operand *args, int n)
{
  gboolean call;
  double spot, low, high, time, rate, carry, vol;
  double m, root, a1, a2, x;

  (void) n;
  OPT_FLAG (0, call);
  ARG_NUMBER (1, spot);
  ARG_NUMBER (2, low);
  ARG_NUMBER (3, high);
  ARG_NUMBER (4, time);
  ARG_NUMBER (5, rate);
  ARG_NUMBER (6, carry);
  ARG_NUMBER (7, vol);
  if (spot <= 0 || low <= 0 || high <= 0 || time <= 0 || vol <= 0)
    return o42_value_error (O42_ERR_NUM);

  m = call ? low : high;
  root = vol * sqrt (time);
  a1 = (log (spot / m) + (carry + vol * vol / 2) * time) / root;
  a2 = a1 - root;
  x = 2 * carry / (vol * vol);

  if (call)
    return o42_value_number (spot * exp ((carry - rate) * time) * normal_cdf (a1) -
                             m * exp (-rate * time) * normal_cdf (a2) +
                             exp (-rate * time) * vol * vol / (2 * carry) * spot *
                             (pow (spot / m, -x) * normal_cdf (-a1 + x * root) -
                              exp (carry * time) * normal_cdf (-a1)));
  return o42_value_number (m * exp (-rate * time) * normal_cdf (-a2) -
                           spot * exp ((carry - rate) * time) * normal_cdf (-a1) +
                           exp (-rate * time) * vol * vol / (2 * carry) * spot *
                           (-pow (spot / m, -x) * normal_cdf (a1 - x * root) +
                            exp (carry * time) * normal_cdf (a1)));
}

static O42Value
fn_opt_fixed_strk_lkbk (O42EvalContext *ctx, O42Operand *args, int n)
{
  gboolean call;
  double spot, low, high, strike, time, rate, carry, vol;
  double root, x, d1, d2, e1, e2, m;

  (void) n;
  OPT_FLAG (0, call);
  ARG_NUMBER (1, spot);
  ARG_NUMBER (2, low);
  ARG_NUMBER (3, high);
  ARG_NUMBER (4, strike);
  ARG_NUMBER (5, time);
  ARG_NUMBER (6, rate);
  ARG_NUMBER (7, carry);
  ARG_NUMBER (8, vol);
  if (spot <= 0 || low <= 0 || high <= 0 || strike <= 0 || time <= 0 || vol <= 0)
    return o42_value_error (O42_ERR_NUM);

  root = vol * sqrt (time);
  x = 2 * carry / (vol * vol);
  m = call ? high : low;

  if (call && strike > m)
    {
      d1 = (log (spot / strike) + (carry + vol * vol / 2) * time) / root;
      d2 = d1 - root;
      return o42_value_number (spot * exp ((carry - rate) * time) * normal_cdf (d1) -
                               strike * exp (-rate * time) * normal_cdf (d2) +
                               spot * exp (-rate * time) * vol * vol / (2 * carry) *
                               (-pow (spot / strike, -x) * normal_cdf (d1 - x * root) +
                                exp (carry * time) * normal_cdf (d1)));
    }
  if (call)
    {
      e1 = (log (spot / m) + (carry + vol * vol / 2) * time) / root;
      e2 = e1 - root;
      return o42_value_number (exp (-rate * time) * (m - strike) +
                               spot * exp ((carry - rate) * time) * normal_cdf (e1) -
                               m * exp (-rate * time) * normal_cdf (e2) +
                               spot * exp (-rate * time) * vol * vol / (2 * carry) *
                               (-pow (spot / m, -x) * normal_cdf (e1 - x * root) +
                                exp (carry * time) * normal_cdf (e1)));
    }
  if (strike < m)
    {
      d1 = (log (spot / strike) + (carry + vol * vol / 2) * time) / root;
      d2 = d1 - root;
      return o42_value_number (strike * exp (-rate * time) * normal_cdf (-d2) -
                               spot * exp ((carry - rate) * time) * normal_cdf (-d1) +
                               spot * exp (-rate * time) * vol * vol / (2 * carry) *
                               (pow (spot / strike, -x) * normal_cdf (-d1 + x * root) -
                                exp (carry * time) * normal_cdf (-d1)));
    }
  e1 = (log (spot / m) + (carry + vol * vol / 2) * time) / root;
  e2 = e1 - root;
  return o42_value_number (exp (-rate * time) * (strike - m) -
                           spot * exp ((carry - rate) * time) * normal_cdf (-e1) +
                           m * exp (-rate * time) * normal_cdf (-e2) +
                           spot * exp (-rate * time) * vol * vol / (2 * carry) *
                           (pow (spot / m, -x) * normal_cdf (-e1 + x * root) -
                            exp (carry * time) * normal_cdf (-e1)));
}

/* Barone-Adesi and Whaley's approximation to an American option: the
 * European price plus what the right to exercise early is worth.  The
 * spot at which exercising becomes the better of the two is found by
 * Newton's method, in the form Haug sets it out in. */
static double
baw_critical (gboolean call, double strike, double time, double rate,
              double carry, double vol)
{
  double v2 = vol * vol;
  double mm = 2 * rate / v2, nn = 2 * carry / v2;
  double kk = 1 - exp (-rate * time);
  double root = vol * sqrt (time);
  double q, q_inf, s_inf, h, si, d1, lhs, rhs, bi;

  if (call)
    {
      q_inf = (-(nn - 1) + sqrt ((nn - 1) * (nn - 1) + 4 * mm)) / 2;
      s_inf = strike / (1 - 1 / q_inf);
      h = -(carry * time + 2 * root) * strike / (s_inf - strike);
      si = strike + (s_inf - strike) * (1 - exp (h));
      q = (-(nn - 1) + sqrt ((nn - 1) * (nn - 1) + 4 * mm / kk)) / 2;

      for (int i = 0; i < 200; i++)
        {
          d1 = (log (si / strike) + (carry + v2 / 2) * time) / root;
          lhs = si - strike;
          rhs = gbs (TRUE, si, strike, time, rate, carry, vol) +
                (1 - exp ((carry - rate) * time) * normal_cdf (d1)) * si / q;
          bi = exp ((carry - rate) * time) * normal_cdf (d1) * (1 - 1 / q) +
               (1 - exp ((carry - rate) * time) * normal_pdf (d1) / root) / q;
          if (fabs (lhs - rhs) / strike < 1e-8 || fabs (1 - bi) < 1e-12)
            break;
          si = (strike + rhs - bi * si) / (1 - bi);
          if (si <= 0 || !isfinite (si))
            { si = strike; break; }
        }
      return si;
    }

  q_inf = (-(nn - 1) - sqrt ((nn - 1) * (nn - 1) + 4 * mm)) / 2;
  s_inf = strike / (1 - 1 / q_inf);
  h = (carry * time - 2 * root) * strike / (strike - s_inf);
  si = s_inf + (strike - s_inf) * exp (h);
  q = (-(nn - 1) - sqrt ((nn - 1) * (nn - 1) + 4 * mm / kk)) / 2;

  for (int i = 0; i < 200; i++)
    {
      d1 = (log (si / strike) + (carry + v2 / 2) * time) / root;
      lhs = strike - si;
      rhs = gbs (FALSE, si, strike, time, rate, carry, vol) -
            (1 - exp ((carry - rate) * time) * normal_cdf (-d1)) * si / q;
      bi = -exp ((carry - rate) * time) * normal_cdf (-d1) * (1 - 1 / q) -
           (1 + exp ((carry - rate) * time) * normal_pdf (-d1) / root) / q;
      if (fabs (lhs - rhs) / strike < 1e-8 || fabs (1 + bi) < 1e-12)
        break;
      si = (strike - rhs + bi * si) / (1 + bi);
      if (si <= 0 || !isfinite (si))
        { si = strike; break; }
    }
  return si;
}

static O42Value
fn_opt_baw_amer (O42EvalContext *ctx, O42Operand *args, int n)
{
  gboolean call;
  double spot, strike, time, rate, carry, vol;
  double v2, mm, nn, kk, q, trigger, a, root, d1;

  (void) n;
  OPT_FLAG (0, call);
  ARG_NUMBER (1, spot);
  ARG_NUMBER (2, strike);
  ARG_NUMBER (3, time);
  ARG_NUMBER (4, rate);
  ARG_NUMBER (5, carry);
  ARG_NUMBER (6, vol);
  if (spot <= 0 || strike <= 0 || time <= 0 || vol <= 0)
    return o42_value_error (O42_ERR_NUM);

  /* With a carry at or above the rate a call is never exercised early,
   * and the European price is the answer. */
  if (call && carry >= rate)
    return o42_value_number (gbs (TRUE, spot, strike, time, rate, carry, vol));

  v2 = vol * vol;
  mm = 2 * rate / v2;
  nn = 2 * carry / v2;
  kk = 1 - exp (-rate * time);
  trigger = baw_critical (call, strike, time, rate, carry, vol);
  root = vol * sqrt (time);
  d1 = (log (trigger / strike) + (carry + v2 / 2) * time) / root;

  if (call)
    {
      q = (-(nn - 1) + sqrt ((nn - 1) * (nn - 1) + 4 * mm / kk)) / 2;
      if (spot >= trigger)
        return o42_value_number (spot - strike);
      a = (trigger / q) * (1 - exp ((carry - rate) * time) * normal_cdf (d1));
      return o42_value_number (gbs (TRUE, spot, strike, time, rate, carry, vol) +
                               a * pow (spot / trigger, q));
    }

  q = (-(nn - 1) - sqrt ((nn - 1) * (nn - 1) + 4 * mm / kk)) / 2;
  if (spot <= trigger)
    return o42_value_number (strike - spot);
  a = -(trigger / q) * (1 - exp ((carry - rate) * time) * normal_cdf (-d1));
  return o42_value_number (gbs (FALSE, spot, strike, time, rate, carry, vol) +
                           a * pow (spot / trigger, q));
}

/* ---- The options that want the bivariate normal ----------------------- */

/* The chance that two standard normals with correlation rho are both
 * below their limits.  Drezner and Wesolowsky's quadrature, in the
 * arrangement Haug's book uses, which is what Gnumeric works from. */
static double
bivariate_normal_cdf (double a, double b, double rho)
{
  static const double W[] = { 0.24840615, 0.39233107, 0.21141819,
                              0.03324666, 0.00082485334 };
  static const double X[] = { 0.10024215, 0.48281397, 1.06094972,
                              1.77972941, 2.66976035 };
  double sum = 0;

  if (a <= 0 && b <= 0 && rho <= 0)
    {
      double root = sqrt (2 * (1 - rho * rho));
      double a1 = a / root, b1 = b / root;

      for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++)
          sum += W[i] * W[j] * exp (a1 * (2 * X[i] - a1) + b1 * (2 * X[j] - b1) +
                                    2 * rho * (X[i] - a1) * (X[j] - b1));
      return sqrt (1 - rho * rho) / G_PI * sum;
    }
  if (a <= 0 && b >= 0 && rho >= 0)
    return normal_cdf (a) - bivariate_normal_cdf (a, -b, -rho);
  if (a >= 0 && b <= 0 && rho >= 0)
    return normal_cdf (b) - bivariate_normal_cdf (-a, b, -rho);
  if (a >= 0 && b >= 0 && rho <= 0)
    return normal_cdf (a) + normal_cdf (b) - 1 + bivariate_normal_cdf (-a, -b, rho);
  if (a * b * rho > 0)
    {
      double sign_a = a >= 0 ? 1 : -1;
      double sign_b = b >= 0 ? 1 : -1;
      double denom = sqrt (a * a - 2 * rho * a * b + b * b);
      double rho1 = (rho * a - b) * sign_a / denom;
      double rho2 = (rho * b - a) * sign_b / denom;
      double delta = (1 - sign_a * sign_b) / 4;

      return bivariate_normal_cdf (a, 0, rho1) + bivariate_normal_cdf (b, 0, rho2) - delta;
    }
  return 0;
}

/* Roll, Geske and Whaley: an American call on a share that pays one
 * known dividend before it expires.  Exercising early, if it happens
 * at all, happens the moment before the dividend is paid. */
static O42Value
fn_opt_rgw (O42EvalContext *ctx, O42Operand *args, int n)
{
  double spot, strike, t1, time, rate, dividend, vol;
  double sx, ci, high, low, mid, root, rho, a1, a2, b1, b2;

  (void) n;
  ARG_NUMBER (0, spot);
  ARG_NUMBER (1, strike);
  ARG_NUMBER (2, t1);
  ARG_NUMBER (3, time);
  ARG_NUMBER (4, rate);
  ARG_NUMBER (5, dividend);
  ARG_NUMBER (6, vol);
  if (spot <= 0 || strike <= 0 || t1 <= 0 || time <= t1 || vol <= 0 || dividend < 0)
    return o42_value_error (O42_ERR_NUM);

  sx = spot - dividend * exp (-rate * t1);
  /* A dividend too small to make up for the interest on the strike is
   * never worth exercising for. */
  if (dividend <= strike * (1 - exp (-rate * (time - t1))))
    return o42_value_number (gbs (TRUE, sx, strike, time, rate, rate, vol));

  /* The spot at which exercising just before the dividend is as good
   * as holding on, by bisection. */
  low = strike;
  high = MAX (spot, strike) * 4 + dividend;
  for (int i = 0; i < 200; i++)
    {
      double c;

      mid = (low + high) / 2;
      c = gbs (TRUE, mid, strike, time - t1, rate, rate, vol);
      if (c - mid - dividend + strike > 0)
        low = mid;
      else
        high = mid;
      if (high - low < 1e-9 * strike)
        break;
    }
  ci = (low + high) / 2;

  root = vol * sqrt (time);
  rho = -sqrt (t1 / time);
  a1 = (log (sx / strike) + (rate + vol * vol / 2) * time) / root;
  a2 = a1 - root;
  b1 = (log (sx / ci) + (rate + vol * vol / 2) * t1) / (vol * sqrt (t1));
  b2 = b1 - vol * sqrt (t1);

  return o42_value_number (sx * normal_cdf (b1)
                           + sx * bivariate_normal_cdf (a1, -b1, rho)
                           - strike * exp (-rate * time) * bivariate_normal_cdf (a2, -b2, rho)
                           - (strike - dividend) * exp (-rate * t1) * normal_cdf (b2));
}

/* A complex chooser: at `time` the holder takes either a call with one
 * strike and life or a put with another. */
static O42Value
fn_opt_complex_chooser (O42EvalContext *ctx, O42Operand *args, int n)
{
  double spot, xc, xp, time, tc, tp, rate, carry, vol;
  double i, di, d1, d2, y1, y2, rho1, rho2;

  (void) n;
  ARG_NUMBER (0, spot);
  ARG_NUMBER (1, xc);
  ARG_NUMBER (2, xp);
  ARG_NUMBER (3, time);
  ARG_NUMBER (4, tc);
  ARG_NUMBER (5, tp);
  ARG_NUMBER (6, rate);
  ARG_NUMBER (7, carry);
  ARG_NUMBER (8, vol);
  if (spot <= 0 || xc <= 0 || xp <= 0 || time <= 0 || tc <= time || tp <= time || vol <= 0)
    return o42_value_error (O42_ERR_NUM);

  /* The spot at which the call and the put are worth the same, by
   * bisection: monotone in the spot, so it converges from any
   * bracket. */
  {
    double lo = 1e-6 * spot, hi = 1000 * spot;

    for (int k = 0; k < 200; k++)
      {
        i = (lo + hi) / 2;
        di = gbs (TRUE, i, xc, tc - time, rate, carry, vol) -
             gbs (FALSE, i, xp, tp - time, rate, carry, vol);
        if (di > 0)
          hi = i;
        else
          lo = i;
        if (hi - lo < 1e-10 * spot)
          break;
      }
    i = (lo + hi) / 2;
  }

  d1 = (log (spot / i) + (carry + vol * vol / 2) * time) / (vol * sqrt (time));
  d2 = d1 - vol * sqrt (time);
  y1 = (log (spot / xc) + (carry + vol * vol / 2) * tc) / (vol * sqrt (tc));
  y2 = (log (spot / xp) + (carry + vol * vol / 2) * tp) / (vol * sqrt (tp));
  rho1 = sqrt (time / tc);
  rho2 = sqrt (time / tp);

  return o42_value_number (
      spot * exp ((carry - rate) * tc) * bivariate_normal_cdf (d1, y1, rho1)
    - xc * exp (-rate * tc) * bivariate_normal_cdf (d2, y1 - vol * sqrt (tc), rho1)
    - spot * exp ((carry - rate) * tp) * bivariate_normal_cdf (-d1, -y2, rho2)
    + xp * exp (-rate * tp) * bivariate_normal_cdf (-d2, -y2 + vol * sqrt (tp), rho2));
}

/* An option on an option: "cc" a call on a call, "cp" a call on a put,
 * "pc" a put on a call, "pp" a put on a put. */
static O42Value
fn_opt_on_options (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *kind = NULL;
  double spot, x1, x2, t1, t2, rate, carry, vol;
  gboolean outer_call, inner_call;
  double i, y1, y2, z1, z2, rho;

  (void) n;
  ARG_TEXT (0, kind);
  if (kind == NULL || strlen (kind) < 2)
    { g_free (kind); return o42_value_error (O42_ERR_NUM); }
  outer_call = kind[0] == 'c' || kind[0] == 'C';
  inner_call = kind[1] == 'c' || kind[1] == 'C';
  g_free (kind);

  ARG_NUMBER (1, spot);
  ARG_NUMBER (2, x1);
  ARG_NUMBER (3, x2);
  ARG_NUMBER (4, t1);
  ARG_NUMBER (5, t2);
  ARG_NUMBER (6, rate);
  ARG_NUMBER (7, carry);
  ARG_NUMBER (8, vol);
  if (spot <= 0 || x1 <= 0 || x2 <= 0 || t1 <= 0 || t2 <= t1 || vol <= 0)
    return o42_value_error (O42_ERR_NUM);

  /* The spot at which the option underneath is worth exactly the
   * strike of the one on top. */
  {
    double lo = 1e-6 * spot, hi = 1000 * spot;

    for (int k = 0; k < 200; k++)
      {
        double v;

        i = (lo + hi) / 2;
        v = gbs (inner_call, i, x1, t2 - t1, rate, carry, vol) - x2;
        if ((inner_call && v > 0) || (!inner_call && v < 0))
          hi = i;
        else
          lo = i;
        if (hi - lo < 1e-10 * spot)
          break;
      }
    i = (lo + hi) / 2;
  }

  rho = sqrt (t1 / t2);
  y1 = (log (spot / i) + (carry + vol * vol / 2) * t1) / (vol * sqrt (t1));
  y2 = y1 - vol * sqrt (t1);
  z1 = (log (spot / x1) + (carry + vol * vol / 2) * t2) / (vol * sqrt (t2));
  z2 = z1 - vol * sqrt (t2);

  if (inner_call && outer_call)
    return o42_value_number (
        spot * exp ((carry - rate) * t2) * bivariate_normal_cdf (z1, y1, rho)
      - x1 * exp (-rate * t2) * bivariate_normal_cdf (z2, y2, rho)
      - x2 * exp (-rate * t1) * normal_cdf (y2));
  if (inner_call && !outer_call)
    return o42_value_number (
        x1 * exp (-rate * t2) * bivariate_normal_cdf (z2, -y2, -rho)
      - spot * exp ((carry - rate) * t2) * bivariate_normal_cdf (z1, -y1, -rho)
      + x2 * exp (-rate * t1) * normal_cdf (-y2));
  if (!inner_call && outer_call)
    return o42_value_number (
        x1 * exp (-rate * t2) * bivariate_normal_cdf (-z2, -y2, rho)
      - spot * exp ((carry - rate) * t2) * bivariate_normal_cdf (-z1, -y1, rho)
      - x2 * exp (-rate * t1) * normal_cdf (-y2));
  return o42_value_number (
      spot * exp ((carry - rate) * t2) * bivariate_normal_cdf (-z1, y1, -rho)
    - x1 * exp (-rate * t2) * bivariate_normal_cdf (-z2, y2, -rho)
    + x2 * exp (-rate * t1) * normal_cdf (y2));
}

/* An option the writer will extend, at a second strike, if it is out
 * of the money when it first expires. */
static O42Value
fn_opt_extendible_writer (O42EvalContext *ctx, O42Operand *args, int n)
{
  gboolean call;
  double spot, x1, x2, t1, t2, rate, carry, vol;
  double rho, z1, z2;

  (void) n;
  OPT_FLAG (0, call);
  ARG_NUMBER (1, spot);
  ARG_NUMBER (2, x1);
  ARG_NUMBER (3, x2);
  ARG_NUMBER (4, t1);
  ARG_NUMBER (5, t2);
  ARG_NUMBER (6, rate);
  ARG_NUMBER (7, carry);
  ARG_NUMBER (8, vol);
  if (spot <= 0 || x1 <= 0 || x2 <= 0 || t1 <= 0 || t2 <= t1 || vol <= 0)
    return o42_value_error (O42_ERR_NUM);

  rho = sqrt (t1 / t2);
  z1 = (log (spot / x2) + (carry + vol * vol / 2) * t2) / (vol * sqrt (t2));
  z2 = (log (spot / x1) + (carry + vol * vol / 2) * t1) / (vol * sqrt (t1));

  if (call)
    return o42_value_number (
        gbs (TRUE, spot, x1, t1, rate, carry, vol)
      + spot * exp ((carry - rate) * t2) * bivariate_normal_cdf (z1, -z2, -rho)
      - x2 * exp (-rate * t2) * bivariate_normal_cdf (z1 - vol * sqrt (t2),
                                                      -z2 + vol * sqrt (t1), -rho));
  return o42_value_number (
      gbs (FALSE, spot, x1, t1, rate, carry, vol)
    + x2 * exp (-rate * t2) * bivariate_normal_cdf (-z1 + vol * sqrt (t2),
                                                    z2 - vol * sqrt (t1), -rho)
    - spot * exp ((carry - rate) * t2) * bivariate_normal_cdf (-z1, z2, -rho));
}

/* The American right to swap one asset for the other.  Under the
 * second asset as the unit of account it is an American call on the
 * ratio with a strike of one, which is where Barone-Adesi and Whaley
 * can price it. */
static O42Value
fn_opt_amer_exchange (O42EvalContext *ctx, O42Operand *args, int n)
{
  double spot1, spot2, qty1, qty2, time, rate, carry1, carry2, vol1, vol2, rho;
  double v, s1, s2, trigger, price, v2, mm, nn, kk, q, a, root, d1;

  (void) n; (void) rate;
  ARG_NUMBER (0, spot1);
  ARG_NUMBER (1, spot2);
  ARG_NUMBER (2, qty1);
  ARG_NUMBER (3, qty2);
  ARG_NUMBER (4, time);
  ARG_NUMBER (5, rate);
  ARG_NUMBER (6, carry1);
  ARG_NUMBER (7, carry2);
  ARG_NUMBER (8, vol1);
  ARG_NUMBER (9, vol2);
  ARG_NUMBER (10, rho);
  if (spot1 <= 0 || spot2 <= 0 || qty1 <= 0 || qty2 <= 0 || time <= 0 ||
      vol1 < 0 || vol2 < 0 || rho < -1 || rho > 1)
    return o42_value_error (O42_ERR_NUM);

  v = sqrt (vol1 * vol1 + vol2 * vol2 - 2 * rho * vol1 * vol2);
  if (v <= 0)
    return o42_value_error (O42_ERR_NUM);
  s1 = qty1 * spot1;
  s2 = qty2 * spot2;

  /* In the ratio's world the interest rate is the second asset's carry
   * and the carry is the difference between the two. */
  {
    double r2 = carry2;
    double b2 = carry1 - carry2;
    double x = s1 / s2;

    if (b2 >= r2)
      {
        /* Never exercised early: Margrabe's price. */
        double d = (log (x) + (b2 + v * v / 2) * time) / (v * sqrt (time));

        return o42_value_number (s2 * (x * exp ((b2 - r2) * time) * normal_cdf (d) -
                                       exp (-r2 * time) * normal_cdf (d - v * sqrt (time))));
      }

    v2 = v * v;
    mm = 2 * r2 / v2;
    nn = 2 * b2 / v2;
    kk = 1 - exp (-r2 * time);
    trigger = baw_critical (TRUE, 1, time, r2, b2, v);
    root = v * sqrt (time);
    d1 = (log (trigger) + (b2 + v2 / 2) * time) / root;
    q = (-(nn - 1) + sqrt ((nn - 1) * (nn - 1) + 4 * mm / kk)) / 2;
    if (x >= trigger)
      return o42_value_number (s1 - s2);
    a = (trigger / q) * (1 - exp ((b2 - r2) * time) * normal_cdf (d1));
    price = gbs (TRUE, x, 1, time, r2, b2, v) + a * pow (x / trigger, q);
    return o42_value_number (s2 * price);
  }
}

const O42Function O42_FUNCS_OPTIONS[] = {
  { "OPT_AMER_EXCHANGE", 11, 11, fn_opt_amer_exchange },
  { "OPT_BAW_AMER", 7, 7, fn_opt_baw_amer },
  { "OPT_BINOMIAL", 8, 9, fn_opt_binomial },
  { "OPT_BS", 7, 7, fn_opt_bs },
  { "OPT_BS_CARRYCOST", 7, 7, fn_opt_bs_carrycost },
  { "OPT_BS_DELTA", 7, 7, fn_opt_bs_delta },
  { "OPT_BS_GAMMA", 6, 6, fn_opt_bs_gamma },
  { "OPT_BS_RHO", 7, 7, fn_opt_bs_rho },
  { "OPT_BS_THETA", 7, 7, fn_opt_bs_theta },
  { "OPT_BS_VEGA", 6, 6, fn_opt_bs_vega },
  { "OPT_COMPLEX_CHOOSER", 9, 9, fn_opt_complex_chooser },
  { "OPT_EURO_EXCHANGE", 11, 11, fn_opt_euro_exchange },
  { "OPT_EXEC", 8, 8, fn_opt_exec },
  { "OPT_EXTENDIBLE_WRITER", 9, 9, fn_opt_extendible_writer },
  { "OPT_FIXED_STRK_LKBK", 9, 9, fn_opt_fixed_strk_lkbk },
  { "OPT_FLOAT_STRK_LKBK", 8, 8, fn_opt_float_strk_lkbk },
  { "OPT_FORWARD_START", 8, 8, fn_opt_forward_start },
  { "OPT_FRENCH", 8, 8, fn_opt_french },
  { "OPT_GARMAN_KOHLHAGEN", 7, 7, fn_opt_garman_kohlhagen },
  { "OPT_JUMP_DIFF", 8, 8, fn_opt_jump_diff },
  { "OPT_ON_OPTIONS", 9, 9, fn_opt_on_options },
  { "OPT_RGW", 7, 7, fn_opt_rgw },
  { "OPT_SIMPLE_CHOOSER", 7, 7, fn_opt_simple_chooser },
  { "OPT_SPREAD_APPROX", 9, 9, fn_opt_spread_approx },
  { "OPT_TIME_SWITCH", 10, 10, fn_opt_time_switch },
  { NULL, 0, 0, NULL }
};

const O42FunctionHelp O42_HELP_OPTIONS[] = {
  { "OPT_AMER_EXCHANGE", "OPT_AMER_EXCHANGE(spot1, spot2, qty1, qty2, time, rate, carry1, carry2, vol1, vol2, rho)", "The American right to swap one asset for another." },
  { "OPT_BAW_AMER", "OPT_BAW_AMER(call_put, spot, strike, time, rate, cost_of_carry, volatility)", "Barone-Adesi and Whaley's price for an American option." },
  { "OPT_BINOMIAL", "OPT_BINOMIAL(amer_euro, call_put, steps, spot, strike, time, rate, volatility, cost_of_carry)", "The price from a binomial tree, American or European." },
  { "OPT_BS", "OPT_BS(call_put, spot, strike, time, rate, volatility, carry)", "The Black-Scholes price of a European option." },
  { "OPT_BS_CARRYCOST", "OPT_BS_CARRYCOST(call_put, spot, strike, time, rate, volatility, carry)", "How its price moves with the cost of carry." },
  { "OPT_BS_DELTA", "OPT_BS_DELTA(call_put, spot, strike, time, rate, volatility, carry)", "How its price moves with the spot price." },
  { "OPT_BS_GAMMA", "OPT_BS_GAMMA(spot, strike, time, rate, volatility, carry)", "How its delta moves with the spot price." },
  { "OPT_BS_RHO", "OPT_BS_RHO(call_put, spot, strike, time, rate, volatility, carry)", "How its price moves with the interest rate." },
  { "OPT_BS_THETA", "OPT_BS_THETA(call_put, spot, strike, time, rate, volatility, carry)", "How its price falls as the time runs out." },
  { "OPT_BS_VEGA", "OPT_BS_VEGA(spot, strike, time, rate, volatility, carry)", "How its price moves with the volatility." },
  { "OPT_COMPLEX_CHOOSER", "OPT_COMPLEX_CHOOSER(spot, strike_call, strike_put, time, time_call, time_put, rate, cost_of_carry, volatility)", "A chooser whose call and put differ in strike and life." },
  { "OPT_EURO_EXCHANGE", "OPT_EURO_EXCHANGE(spot1, spot2, qty1, qty2, time, rate, carry1, carry2, vol1, vol2, rho)", "Margrabe's: the right to swap one asset for another." },
  { "OPT_EXEC", "OPT_EXEC(call_put, spot, strike, time, rate, volatility, cost_of_carry, lambda)", "An executive option, forfeited at a rate." },
  { "OPT_EXTENDIBLE_WRITER", "OPT_EXTENDIBLE_WRITER(call_put, spot, strike1, strike2, time1, time2, rate, cost_of_carry, volatility)", "An option the writer extends if it expires out of the money." },
  { "OPT_FIXED_STRK_LKBK", "OPT_FIXED_STRK_LKBK(call_put, spot, spot_min, spot_max, strike, time, rate, cost_of_carry, volatility)", "A lookback settled against a fixed strike." },
  { "OPT_FLOAT_STRK_LKBK", "OPT_FLOAT_STRK_LKBK(call_put, spot, spot_min, spot_max, time, rate, cost_of_carry, volatility)", "A lookback settled against the best the spot reached." },
  { "OPT_FORWARD_START", "OPT_FORWARD_START(call_put, spot, alpha, time1, time, rate, volatility, cost_of_carry)", "An option whose strike is settled later." },
  { "OPT_FRENCH", "OPT_FRENCH(call_put, spot, strike, time, ttime, rate, volatility, cost_of_carry)", "French's: the variance counted in trading time." },
  { "OPT_GARMAN_KOHLHAGEN", "OPT_GARMAN_KOHLHAGEN(call_put, spot, strike, time, domestic_rate, foreign_rate, volatility)", "A currency option." },
  { "OPT_JUMP_DIFF", "OPT_JUMP_DIFF(call_put, spot, strike, time, rate, volatility, lambda, gamma)", "Merton's jump diffusion price." },
  { "OPT_ON_OPTIONS", "OPT_ON_OPTIONS(type, spot, strike1, strike2, time1, time2, rate, cost_of_carry, volatility)", "An option on an option: cc, cp, pc or pp." },
  { "OPT_RGW", "OPT_RGW(spot, strike, time1, time2, rate, dividend, volatility)", "Roll, Geske and Whaley's American call on a share paying one dividend." },
  { "OPT_SIMPLE_CHOOSER", "OPT_SIMPLE_CHOOSER(spot, strike, time1, time2, rate, cost_of_carry, volatility)", "An option that becomes a call or a put later." },
  { "OPT_SPREAD_APPROX", "OPT_SPREAD_APPROX(call_put, fut_price1, fut_price2, strike, time, rate, vol1, vol2, rho)", "Kirk's approximation for an option on a spread." },
  { "OPT_TIME_SWITCH", "OPT_TIME_SWITCH(call_put, spot, strike, amount, time, m, dt, rate, cost_of_carry, volatility)", "An option paying for each interval spent in the money." },
  { NULL, NULL, NULL }
};
