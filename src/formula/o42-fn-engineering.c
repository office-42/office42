/* o42-fn-engineering.c - see o42-eval-private.h
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
#define complex_parse o42_complex_parse
#define complex_format o42_complex_format

/* ---- number bases ---- */

/* Excel's bases are two's complement in 10 digits: binary from -512 to
 * 511, octal and hex from -2^29 to 2^29-1. */
static gboolean
base_to_number (const char *text, int base, double *out)
{
  gint64 v = 0;
  gsize len = strlen (text);
  int bits = base == 2 ? 10 : base == 8 ? 30 : 40;

  if (len == 0 || len > 10) return FALSE;
  for (const char *p = text; *p; p++)
    {
      int d = g_ascii_isdigit (*p) ? *p - '0' : g_ascii_isalpha (*p) ? g_ascii_toupper (*p) - 'A' + 10 : 99;
      if (d >= base) return FALSE;
      v = v * base + d;
    }
  if (len == 10 && (v >> (bits - 1)) & 1)
    v -= (gint64) 1 << bits;
  *out = (double) v;
  return TRUE;
}

static O42Value
number_to_base (double v, int base, int places, gboolean have_places)
{
  const char *digits = "0123456789ABCDEF";
  int bits = base == 2 ? 10 : base == 8 ? 30 : 40;
  double lo = -pow (2, bits - 1), hi = pow (2, bits - 1) - 1;
  gint64 u;
  char buf[16];
  int len = 0;

  if (v < lo || v > hi) return o42_value_error (O42_ERR_NUM);
  u = (gint64) trunc (v);
  if (u < 0) u += (gint64) 1 << bits;
  do { buf[len++] = digits[u % base]; u /= base; } while (u > 0);
  if (have_places)
    {
      if (places < len || places > 10) return o42_value_error (O42_ERR_NUM);
      if (v >= 0)
        while (len < places) buf[len++] = '0';
    }
  {
    char *out = g_new (char, len + 1);
    for (int i = 0; i < len; i++) out[i] = buf[len - 1 - i];
    out[len] = '\0';
    return o42_value_take (out);
  }
}

static O42Value
fn_base_convert (O42EvalContext *ctx, O42Operand *args, int n, int from, int to)
{
  O42Value v = operand_value (ctx, &args[0]);
  char *text;
  double number, places = 0;

  if (v.type == O42_VALUE_ERROR) return v;
  text = o42_value_to_text (&v);
  o42_value_clear (&v);
  if (from == 10)
    {
      O42ErrorCode e = O42_ERR_VALUE;
      O42Value probe = o42_value_text (text);
      gboolean ok = o42_value_to_number (&probe, &number, &e);
      o42_value_clear (&probe);
      if (!ok) { g_free (text); return o42_value_error (O42_ERR_VALUE); }
    }
  else if (!base_to_number (text, from, &number))
    { g_free (text); return o42_value_error (O42_ERR_NUM); }
  g_free (text);
  if (to == 10)
    return o42_value_number (number);
  if (n >= 2) ARG_NUMBER (1, places);
  return number_to_base (number, to, (int) places, n >= 2);
}

static O42Value fn_bin2dec (O42EvalContext *c, O42Operand *a, int n) { return fn_base_convert (c, a, n, 2, 10); }
static O42Value fn_bin2hex (O42EvalContext *c, O42Operand *a, int n) { return fn_base_convert (c, a, n, 2, 16); }
static O42Value fn_bin2oct (O42EvalContext *c, O42Operand *a, int n) { return fn_base_convert (c, a, n, 2, 8); }
static O42Value fn_dec2bin (O42EvalContext *c, O42Operand *a, int n) { return fn_base_convert (c, a, n, 10, 2); }
static O42Value fn_dec2hex (O42EvalContext *c, O42Operand *a, int n) { return fn_base_convert (c, a, n, 10, 16); }
static O42Value fn_dec2oct (O42EvalContext *c, O42Operand *a, int n) { return fn_base_convert (c, a, n, 10, 8); }
static O42Value fn_hex2bin (O42EvalContext *c, O42Operand *a, int n) { return fn_base_convert (c, a, n, 16, 2); }
static O42Value fn_hex2dec (O42EvalContext *c, O42Operand *a, int n) { return fn_base_convert (c, a, n, 16, 10); }
static O42Value fn_hex2oct (O42EvalContext *c, O42Operand *a, int n) { return fn_base_convert (c, a, n, 16, 8); }
static O42Value fn_oct2bin (O42EvalContext *c, O42Operand *a, int n) { return fn_base_convert (c, a, n, 8, 2); }
static O42Value fn_oct2dec (O42EvalContext *c, O42Operand *a, int n) { return fn_base_convert (c, a, n, 8, 10); }
static O42Value fn_oct2hex (O42EvalContext *c, O42Operand *a, int n) { return fn_base_convert (c, a, n, 8, 16); }

/* ---- bits ---- */

static O42Value
fn_bits (O42EvalContext *ctx, O42Operand *args, int n, char op)
{
  double a, b;
  guint64 x, y, r;
  (void) n;
  ARG_NUMBER (0, a);
  ARG_NUMBER (1, b);
  if (a < 0 || a != floor (a) || a >= 281474976710656.0 ||
      (op != '<' && op != '>' && (b < 0 || b != floor (b) || b >= 281474976710656.0)))
    return o42_value_error (O42_ERR_NUM);
  x = (guint64) a;
  switch (op)
    {
    case '&': r = x & (guint64) b; break;
    case '|': r = x | (guint64) b; break;
    case '^': r = x ^ (guint64) b; break;
    case '<':
      if (fabs (b) > 53) return o42_value_error (O42_ERR_NUM);
      r = b >= 0 ? x << (int) b : x >> (int) -b;
      break;
    default:
      if (fabs (b) > 53) return o42_value_error (O42_ERR_NUM);
      r = b >= 0 ? x >> (int) b : x << (int) -b;
      break;
    }
  y = r;
  if (y >= 281474976710656.0) return o42_value_error (O42_ERR_NUM);
  return o42_value_number ((double) y);
}

static O42Value fn_bitand    (O42EvalContext *c, O42Operand *a, int n) { return fn_bits (c, a, n, '&'); }
static O42Value fn_bitor     (O42EvalContext *c, O42Operand *a, int n) { return fn_bits (c, a, n, '|'); }
static O42Value fn_bitxor    (O42EvalContext *c, O42Operand *a, int n) { return fn_bits (c, a, n, '^'); }
static O42Value fn_bitlshift (O42EvalContext *c, O42Operand *a, int n) { return fn_bits (c, a, n, '<'); }
static O42Value fn_bitrshift (O42EvalContext *c, O42Operand *a, int n) { return fn_bits (c, a, n, '>'); }

/* ---- steps and error functions ---- */

static O42Value
fn_delta (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a, b = 0;
  ARG_NUMBER (0, a);
  if (n >= 2) ARG_NUMBER (1, b);
  return o42_value_number (a == b ? 1 : 0);
}

static O42Value
fn_gestep (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a, step = 0;
  ARG_NUMBER (0, a);
  if (n >= 2) ARG_NUMBER (1, step);
  return o42_value_number (a >= step ? 1 : 0);
}

static O42Value
fn_erf (O42EvalContext *ctx, O42Operand *args, int n)
{
  double lo, hi;
  ARG_NUMBER (0, lo);
  if (n >= 2)
    {
      ARG_NUMBER (1, hi);
      return o42_value_number (erf (hi) - erf (lo));
    }
  return o42_value_number (erf (lo));
}

static O42Value
fn_erfc (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  return o42_value_number (erfc (x));
}

/* ---- complex numbers, as Excel keeps them: text like "3+4i" ---- */

gboolean
o42_complex_parse (const char *text, double *re, double *im, char *suffix)
{
  char *s = g_strstrip (g_strdup (text));
  gsize len = strlen (s);
  char unit = 'i';
  gboolean ok = TRUE;

  *re = *im = 0;
  if (len == 0) { g_free (s); return TRUE; }
  if (s[len - 1] == 'i' || s[len - 1] == 'j')
    {
      /* Split at the last sign that is not the first character and
       * does not follow an exponent's e. */
      gssize split = -1;
      unit = s[len - 1];
      s[len - 1] = '\0';
      len--;
      for (gssize k = (gssize) len - 1; k > 0; k--)
        if ((s[k] == '+' || s[k] == '-') && s[k - 1] != 'e' && s[k - 1] != 'E')
          { split = k; break; }
      if (split > 0)
        {
          char *end_ptr;
          char *im_part = g_strdup (s + split);   /* the sign included */

          s[split] = '\0';
          *re = g_ascii_strtod (s, &end_ptr);
          if (*end_ptr != '\0') ok = FALSE;
          if (strcmp (im_part, "+") == 0) *im = 1;
          else if (strcmp (im_part, "-") == 0) *im = -1;
          else
            {
              *im = g_ascii_strtod (im_part, &end_ptr);
              if (*end_ptr != '\0') ok = FALSE;
            }
          g_free (im_part);
        }
      else
        {
          char *end_ptr;
          if (len == 0) *im = 1;
          else if (strcmp (s, "+") == 0) *im = 1;
          else if (strcmp (s, "-") == 0) *im = -1;
          else
            {
              *im = g_ascii_strtod (s, &end_ptr);
              if (*end_ptr != '\0') ok = FALSE;
            }
        }
    }
  else
    {
      char *end_ptr;
      *re = g_ascii_strtod (s, &end_ptr);
      if (*end_ptr != '\0') ok = FALSE;
    }
  if (suffix != NULL) *suffix = unit;
  g_free (s);
  return ok;
}

char *
o42_complex_format (double re, double im, char suffix)
{
  char rbuf[G_ASCII_DTOSTR_BUF_SIZE], ibuf[G_ASCII_DTOSTR_BUF_SIZE];

  g_ascii_formatd (rbuf, sizeof rbuf, "%.15g", re);
  g_ascii_formatd (ibuf, sizeof ibuf, "%.15g", fabs (im));
  if (im == 0)
    return g_strdup (rbuf);
  if (re == 0)
    return g_strdup_printf ("%s%s%c", im < 0 ? "-" : "", im == 1 || im == -1 ? "" : ibuf, suffix);
  return g_strdup_printf ("%s%s%s%c", rbuf, im < 0 ? "-" : "+", im == 1 || im == -1 ? "" : ibuf, suffix);
}



static O42Value
fn_complex (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im;
  char suffix = 'i';
  ARG_NUMBER (0, re);
  ARG_NUMBER (1, im);
  if (n >= 3)
    {
      char *s;
      ARG_TEXT (2, s);
      if (strcmp (s, "i") != 0 && strcmp (s, "j") != 0 && s[0] != '\0')
        { g_free (s); return o42_value_error (O42_ERR_VALUE); }
      if (s[0]) suffix = s[0];
      g_free (s);
    }
  return o42_value_take (complex_format (re, im, suffix));
}

static O42Value
fn_imreal (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  return o42_value_number (re);
}

static O42Value
fn_imaginary (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  return o42_value_number (im);
}

static O42Value
fn_imabs (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  return o42_value_number (hypot (re, im));
}

static O42Value
fn_imargument (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  if (re == 0 && im == 0) return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (atan2 (im, re));
}

static O42Value
fn_imconjugate (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  return o42_value_take (complex_format (re, -im, sfx));
}

static O42Value
fn_imsum (O42EvalContext *ctx, O42Operand *args, int n)
{
  double sre = 0, sim = 0; char sfx = 'i';
  for (int i = 0; i < n; i++)
    {
      double re, im;
      ARG_COMPLEX (i, re, im, sfx);
      sre += re; sim += im;
    }
  return o42_value_take (complex_format (sre, sim, sfx));
}

static O42Value
fn_imsub (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a, b, c, d; char sfx, sfx2;
  (void) n;
  ARG_COMPLEX (0, a, b, sfx);
  ARG_COMPLEX (1, c, d, sfx2);
  return o42_value_take (complex_format (a - c, b - d, sfx));
}

static O42Value
fn_improduct (O42EvalContext *ctx, O42Operand *args, int n)
{
  double pre = 1, pim = 0; char sfx = 'i';
  for (int i = 0; i < n; i++)
    {
      double re, im, nre;
      ARG_COMPLEX (i, re, im, sfx);
      nre = pre * re - pim * im;
      pim = pre * im + pim * re;
      pre = nre;
    }
  return o42_value_take (complex_format (pre, pim, sfx));
}

static O42Value
fn_imdiv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a, b, c, d, den; char sfx, sfx2;
  (void) n;
  ARG_COMPLEX (0, a, b, sfx);
  ARG_COMPLEX (1, c, d, sfx2);
  den = c * c + d * d;
  if (den == 0) return o42_value_error (O42_ERR_NUM);
  return o42_value_take (complex_format ((a * c + b * d) / den, (b * c - a * d) / den, sfx));
}

static O42Value
fn_impower (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im, p, r, theta; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  ARG_NUMBER (1, p);
  r = hypot (re, im);
  if (r == 0) return o42_value_take (complex_format (p == 0 ? 1 : 0, 0, sfx));
  theta = atan2 (im, re);
  return o42_value_take (complex_format (pow (r, p) * cos (p * theta), pow (r, p) * sin (p * theta), sfx));
}

static O42Value
fn_imsqrt (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im, r, theta; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  r = sqrt (hypot (re, im));
  theta = atan2 (im, re) / 2;
  return o42_value_take (complex_format (r * cos (theta), r * sin (theta), sfx));
}

static O42Value
fn_imexp (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  return o42_value_take (complex_format (exp (re) * cos (im), exp (re) * sin (im), sfx));
}

static O42Value
fn_imln_base (O42EvalContext *ctx, O42Operand *args, int n, double base)
{
  double re, im, lr, th; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  if (re == 0 && im == 0) return o42_value_error (O42_ERR_NUM);
  lr = log (hypot (re, im)) / log (base);
  th = atan2 (im, re) / log (base);
  return o42_value_take (complex_format (lr, th, sfx));
}

static O42Value fn_imln    (O42EvalContext *c, O42Operand *a, int n) { return fn_imln_base (c, a, n, G_E); }
static O42Value fn_imlog10 (O42EvalContext *c, O42Operand *a, int n) { return fn_imln_base (c, a, n, 10); }
static O42Value fn_imlog2  (O42EvalContext *c, O42Operand *a, int n) { return fn_imln_base (c, a, n, 2); }

static O42Value
fn_imsin (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  return o42_value_take (complex_format (sin (re) * cosh (im), cos (re) * sinh (im), sfx));
}

static O42Value
fn_imcos (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  return o42_value_take (complex_format (cos (re) * cosh (im), -sin (re) * sinh (im), sfx));
}

const O42Function O42_FUNCS_ENGINEERING[] = {
  { "BIN2DEC", 1, 1, fn_bin2dec },
  { "BIN2HEX", 1, 2, fn_bin2hex },
  { "BIN2OCT", 1, 2, fn_bin2oct },
  { "BITAND", 2, 2, fn_bitand },
  { "BITLSHIFT", 2, 2, fn_bitlshift },
  { "BITOR", 2, 2, fn_bitor },
  { "BITRSHIFT", 2, 2, fn_bitrshift },
  { "BITXOR", 2, 2, fn_bitxor },
  { "COMPLEX", 2, 3, fn_complex },
  { "DEC2BIN", 1, 2, fn_dec2bin },
  { "DEC2HEX", 1, 2, fn_dec2hex },
  { "DEC2OCT", 1, 2, fn_dec2oct },
  { "DELTA", 1, 2, fn_delta },
  { "ERF", 1, 2, fn_erf },
  { "ERF.PRECISE", 1, 1, fn_erf },
  { "ERFC", 1, 1, fn_erfc },
  { "ERFC.PRECISE", 1, 1, fn_erfc },
  { "GESTEP", 1, 2, fn_gestep },
  { "HEX2BIN", 1, 2, fn_hex2bin },
  { "HEX2DEC", 1, 1, fn_hex2dec },
  { "HEX2OCT", 1, 2, fn_hex2oct },
  { "IMABS", 1, 1, fn_imabs },
  { "IMAGINARY", 1, 1, fn_imaginary },
  { "IMARGUMENT", 1, 1, fn_imargument },
  { "IMCONJUGATE", 1, 1, fn_imconjugate },
  { "IMCOS", 1, 1, fn_imcos },
  { "IMDIV", 2, 2, fn_imdiv },
  { "IMEXP", 1, 1, fn_imexp },
  { "IMLN", 1, 1, fn_imln },
  { "IMLOG10", 1, 1, fn_imlog10 },
  { "IMLOG2", 1, 1, fn_imlog2 },
  { "IMPOWER", 2, 2, fn_impower },
  { "IMPRODUCT", 1, -1, fn_improduct },
  { "IMREAL", 1, 1, fn_imreal },
  { "IMSIN", 1, 1, fn_imsin },
  { "IMSQRT", 1, 1, fn_imsqrt },
  { "IMSUB", 2, 2, fn_imsub },
  { "IMSUM", 1, -1, fn_imsum },
  { "OCT2BIN", 1, 2, fn_oct2bin },
  { "OCT2DEC", 1, 1, fn_oct2dec },
  { "OCT2HEX", 1, 2, fn_oct2hex },
  { NULL, 0, 0, NULL }
};

const O42FunctionHelp O42_HELP_ENGINEERING[] = {
  { "BIN2DEC", "BIN2DEC(number)", "A binary number as decimal." },
  { "BIN2HEX", "BIN2HEX(number, places)", "A binary number as hexadecimal." },
  { "BIN2OCT", "BIN2OCT(number, places)", "A binary number as octal." },
  { "BITAND", "BITAND(number1, number2)", "Bitwise AND of two numbers." },
  { "BITLSHIFT", "BITLSHIFT(number, shift_amount)", "A number shifted left by some bits." },
  { "BITOR", "BITOR(number1, number2)", "Bitwise OR of two numbers." },
  { "BITRSHIFT", "BITRSHIFT(number, shift_amount)", "A number shifted right by some bits." },
  { "BITXOR", "BITXOR(number1, number2)", "Bitwise exclusive OR of two numbers." },
  { "COMPLEX", "COMPLEX(real_num, i_num, suffix)", "A complex number as text, like 3+4i." },
  { "DEC2BIN", "DEC2BIN(number, places)", "A decimal number as binary." },
  { "DEC2HEX", "DEC2HEX(number, places)", "A decimal number as hexadecimal." },
  { "DEC2OCT", "DEC2OCT(number, places)", "A decimal number as octal." },
  { "DELTA", "DELTA(number1, number2)", "1 if the two numbers are equal, else 0." },
  { "ERF", "ERF(lower_limit, upper_limit)", "The error function between two limits." },
  { "ERF.PRECISE", "ERF.PRECISE(x)", "The error function." },
  { "ERFC", "ERFC(x)", "The complementary error function." },
  { "ERFC.PRECISE", "ERFC.PRECISE(x)", "The complementary error function." },
  { "GESTEP", "GESTEP(number, step)", "1 if the number is at least the step, else 0." },
  { "HEX2BIN", "HEX2BIN(number, places)", "A hexadecimal number as binary." },
  { "HEX2DEC", "HEX2DEC(number)", "A hexadecimal number as decimal." },
  { "HEX2OCT", "HEX2OCT(number, places)", "A hexadecimal number as octal." },
  { "IMABS", "IMABS(inumber)", "The modulus of a complex number." },
  { "IMAGINARY", "IMAGINARY(inumber)", "The imaginary part of a complex number." },
  { "IMARGUMENT", "IMARGUMENT(inumber)", "The argument of a complex number, in radians." },
  { "IMCONJUGATE", "IMCONJUGATE(inumber)", "The conjugate of a complex number." },
  { "IMCOS", "IMCOS(inumber)", "The cosine of a complex number." },
  { "IMDIV", "IMDIV(inumber1, inumber2)", "The quotient of two complex numbers." },
  { "IMEXP", "IMEXP(inumber)", "The exponential of a complex number." },
  { "IMLN", "IMLN(inumber)", "The natural logarithm of a complex number." },
  { "IMLOG10", "IMLOG10(inumber)", "The base-10 logarithm of a complex number." },
  { "IMLOG2", "IMLOG2(inumber)", "The base-2 logarithm of a complex number." },
  { "IMPOWER", "IMPOWER(inumber, number)", "A complex number raised to a power." },
  { "IMPRODUCT", "IMPRODUCT(inumber1, inumber2, ...)", "The product of complex numbers." },
  { "IMREAL", "IMREAL(inumber)", "The real part of a complex number." },
  { "IMSIN", "IMSIN(inumber)", "The sine of a complex number." },
  { "IMSQRT", "IMSQRT(inumber)", "The square root of a complex number." },
  { "IMSUB", "IMSUB(inumber1, inumber2)", "The difference of two complex numbers." },
  { "IMSUM", "IMSUM(inumber1, inumber2, ...)", "The sum of complex numbers." },
  { "OCT2BIN", "OCT2BIN(number, places)", "An octal number as binary." },
  { "OCT2DEC", "OCT2DEC(number)", "An octal number as decimal." },
  { "OCT2HEX", "OCT2HEX(number, places)", "An octal number as hexadecimal." },
  { NULL, NULL, NULL }
};
