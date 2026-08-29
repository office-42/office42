/* o42-fn-bessel.c - see o42-eval-private.h
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

/* ---- Bessel functions, by their integral representations ---- */

/* Simpson's rule on [a, b] with an even number of panels. */
typedef double (*Integrand) (double t, double x, double order);

static double
integrate (Integrand f, double a, double b, double x, double order, int panels)
{
  double h = (b - a) / panels, sum = f (a, x, order) + f (b, x, order);
  for (int i = 1; i < panels; i++)
    sum += (i % 2 ? 4 : 2) * f (a + i * h, x, order);
  return sum * h / 3;
}

static double bessel_j_core (double t, double x, double nu) { return cos (nu * t - x * sin (t)); }
static double bessel_i_core (double t, double x, double nu) { return exp (x * cos (t)) * cos (nu * t); }
static double bessel_y_core (double t, double x, double nu) { return sin (x * sin (t) - nu * t); }
static double
bessel_y_tail (double t, double x, double nu)
{
  return (exp (nu * t) + cos (nu * G_PI) * exp (-nu * t)) * exp (-x * sinh (t));
}
static double bessel_k_core (double t, double x, double nu) { return exp (-x * cosh (t)) * cosh (nu * t); }

static O42Value
fn_bessel (O42EvalContext *ctx, O42Operand *args, int n, char kind)
{
  double x, order, r;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, order);
  order = floor (order);
  if (order < 0) return o42_value_error (O42_ERR_NUM);
  switch (kind)
    {
    case 'J':
      r = integrate (bessel_j_core, 0, G_PI, x, order, 4000) / G_PI;
      break;
    case 'I':
      r = integrate (bessel_i_core, 0, G_PI, x, order, 4000) / G_PI;
      break;
    case 'Y':
      if (x <= 0) return o42_value_error (O42_ERR_NUM);
      {
        /* The tail integral falls off as exp(-x sinh t); stop where it
         * is below anything the double can hold. */
        double top = asinh (700 / x) + 1;
        r = (integrate (bessel_y_core, 0, G_PI, x, order, 4000)
             - integrate (bessel_y_tail, 0, top, x, order, 8000)) / G_PI;
      }
      break;
    default:
      if (x <= 0) return o42_value_error (O42_ERR_NUM);
      {
        double top = acosh (700 / x) + 1;
        r = integrate (bessel_k_core, 0, top, x, order, 8000);
      }
      break;
    }
  if (isnan (r) || isinf (r)) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (r);
}

static O42Value fn_besselj (O42EvalContext *c, O42Operand *a, int n) { return fn_bessel (c, a, n, 'J'); }
static O42Value fn_bessely (O42EvalContext *c, O42Operand *a, int n) { return fn_bessel (c, a, n, 'Y'); }
static O42Value fn_besseli (O42EvalContext *c, O42Operand *a, int n) { return fn_bessel (c, a, n, 'I'); }
static O42Value fn_besselk (O42EvalContext *c, O42Operand *a, int n) { return fn_bessel (c, a, n, 'K'); }

const O42Function O42_FUNCS_BESSEL[] = {
  { "BESSELI", 2, 2, fn_besseli },
  { "BESSELJ", 2, 2, fn_besselj },
  { "BESSELK", 2, 2, fn_besselk },
  { "BESSELY", 2, 2, fn_bessely },
  { NULL, 0, 0, NULL }
};

const O42FunctionHelp O42_HELP_BESSEL[] = {
  { "BESSELI", "BESSELI(x, n)", "The modified Bessel function In(x)." },
  { "BESSELJ", "BESSELJ(x, n)", "The Bessel function Jn(x)." },
  { "BESSELK", "BESSELK(x, n)", "The modified Bessel function Kn(x)." },
  { "BESSELY", "BESSELY(x, n)", "The Bessel function Yn(x)." },
  { NULL, NULL, NULL }
};
