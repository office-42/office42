/* o42-analysis.c - the statistical analysis tools
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-analysis.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* One variable: its name and its numbers, in the order they were read. */
typedef struct {
  char   *name;      /* owned */
  GArray *values;    /* double */
} Variable;

static void
variable_clear (Variable *v)
{
  g_free (v->name);
  if (v->values != NULL)
    g_array_free (v->values, TRUE);
}

/* Reads the input range into one variable per column (or per row),
 * taking the first cell of each as its name when `labels` says so. */
static GArray *
read_variables (O42Sheet *sheet, const O42AnalysisOptions *options)
{
  GArray *variables = g_array_new (FALSE, FALSE, sizeof (Variable));
  O42Range r = o42_range_normalise (options->input.row0, options->input.col0,
                                    options->input.row1, options->input.col1);
  gboolean by_columns = options->layout == O42_ANALYSIS_COLUMNS;
  int outer_first = by_columns ? r.col0 : r.row0;
  int outer_last = by_columns ? r.col1 : r.row1;
  int inner_first = by_columns ? r.row0 : r.col0;
  int inner_last = by_columns ? r.row1 : r.col1;

  if (options->labels)
    inner_first++;

  for (int outer = outer_first; outer <= outer_last; outer++)
    {
      Variable v;

      v.values = g_array_new (FALSE, FALSE, sizeof (double));
      v.name = NULL;
      if (options->labels)
        {
          int row = by_columns ? r.row0 : outer;
          int col = by_columns ? outer : r.col0;
          char *shown = o42_sheet_get_display (sheet, row, col);

          if (shown != NULL && *shown != '\0')
            v.name = shown;
          else
            g_free (shown);
        }
      if (v.name == NULL)
        v.name = g_strdup_printf ("Column %d", outer - outer_first + 1);

      for (int inner = inner_first; inner <= inner_last; inner++)
        {
          int row = by_columns ? inner : outer;
          int col = by_columns ? outer : inner;
          O42Value value;

          o42_sheet_get_value (sheet, row, col, &value);
          if (value.type == O42_VALUE_NUMBER)
            {
              double number = value.as.number;

              g_array_append_val (v.values, number);
            }
          o42_value_clear (&value);
        }
      g_array_append_val (variables, v);
    }
  return variables;
}

static void
free_variables (GArray *variables)
{
  for (guint i = 0; i < variables->len; i++)
    variable_clear (&g_array_index (variables, Variable, i));
  g_array_free (variables, TRUE);
}

/* Writing the results: a text, or a number as its own text, so that
 * the sheet holds ordinary cells and not a special kind. */
static void
put_text (O42Sheet *sheet, int row, int col, const char *text)
{
  o42_sheet_set_input (sheet, row, col, text != NULL ? text : "");
}

static void
put_number (O42Sheet *sheet, int row, int col, double value)
{
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];

  if (isnan (value) || isinf (value))
    {
      o42_sheet_set_input (sheet, row, col, "#N/A");
      return;
    }
  g_ascii_formatd (buffer, sizeof buffer, "%.10g", value);
  o42_sheet_set_input (sheet, row, col, buffer);
}

/* ---- the statistics themselves --------------------------------------- */

static double
mean_of (const GArray *values)
{
  double sum = 0;

  if (values->len == 0)
    return NAN;
  for (guint i = 0; i < values->len; i++)
    sum += g_array_index (values, double, i);
  return sum / values->len;
}

/* The sum of the deviations raised to a power, which every moment
 * needs. */
static double
moment_of (const GArray *values, double mean, int power)
{
  double total = 0;

  for (guint i = 0; i < values->len; i++)
    {
      double d = g_array_index (values, double, i) - mean;

      total += pow (d, power);
    }
  return total;
}

static int
compare_double (gconstpointer a, gconstpointer b)
{
  double x = *(const double *) a, y = *(const double *) b;

  return (x > y) - (x < y);
}

static double
median_of (const GArray *values)
{
  GArray *sorted;
  double answer;

  if (values->len == 0)
    return NAN;
  sorted = g_array_sized_new (FALSE, FALSE, sizeof (double), values->len);
  g_array_append_vals (sorted, values->data, values->len);
  g_array_sort (sorted, compare_double);
  if (sorted->len % 2 == 1)
    answer = g_array_index (sorted, double, sorted->len / 2);
  else
    answer = (g_array_index (sorted, double, sorted->len / 2 - 1) +
              g_array_index (sorted, double, sorted->len / 2)) / 2;
  g_array_free (sorted, TRUE);
  return answer;
}

/* The value that turns up most often, or #N/A when none does twice. */
static double
mode_of (const GArray *values)
{
  double best = NAN;
  guint best_count = 1;

  for (guint i = 0; i < values->len; i++)
    {
      double v = g_array_index (values, double, i);
      guint count = 0;

      for (guint j = 0; j < values->len; j++)
        if (g_array_index (values, double, j) == v)
          count++;
      if (count > best_count)
        {
          best_count = count;
          best = v;
        }
    }
  return best;
}

/* Student's t for a two-tailed confidence level, by bisection on the
 * distribution the evaluator already knows how to integrate. */
static double
incomplete_beta (double a, double b, double x);

static double
t_distribution_tail (double t, double df)
{
  /* The probability of |T| > t. */
  double x = df / (df + t * t);

  return incomplete_beta (df / 2, 0.5, x);
}

/* The regularised incomplete beta function by its continued fraction,
 * which is what the t and F distributions are made of. */
static double
beta_continued_fraction (double a, double b, double x)
{
  const int limit = 300;
  const double tiny = 1e-30;
  double qab = a + b, qap = a + 1, qam = a - 1;
  double c = 1, d = 1 - qab * x / qap;
  double h;

  if (fabs (d) < tiny) d = tiny;
  d = 1 / d;
  h = d;
  for (int m = 1; m <= limit; m++)
    {
      int m2 = 2 * m;
      double aa = m * (b - m) * x / ((qam + m2) * (a + m2));

      d = 1 + aa * d;
      if (fabs (d) < tiny) d = tiny;
      c = 1 + aa / c;
      if (fabs (c) < tiny) c = tiny;
      d = 1 / d;
      h *= d * c;
      aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
      d = 1 + aa * d;
      if (fabs (d) < tiny) d = tiny;
      c = 1 + aa / c;
      if (fabs (c) < tiny) c = tiny;
      d = 1 / d;
      h *= d * c;
      if (fabs (d * c - 1) < 1e-12)
        break;
    }
  return h;
}

static double
incomplete_beta (double a, double b, double x)
{
  double front;

  if (x <= 0) return 0;
  if (x >= 1) return 1;
  front = exp (lgamma (a + b) - lgamma (a) - lgamma (b) +
               a * log (x) + b * log (1 - x));
  if (x < (a + 1) / (a + b + 2))
    return front * beta_continued_fraction (a, b, x) / a;
  return 1 - exp (lgamma (a + b) - lgamma (a) - lgamma (b) +
                  b * log (1 - x) + a * log (x)) *
             beta_continued_fraction (b, a, 1 - x) / b;
}

/* The probability that F exceeds the given value. */
static double
f_distribution_tail (double f, double df1, double df2)
{
  if (f <= 0 || df1 <= 0 || df2 <= 0)
    return 1;
  return incomplete_beta (df2 / 2, df1 / 2, df2 / (df2 + df1 * f));
}

/* The t value with the given two-tailed probability, found by
 * bisection: the confidence interval needs it. */
static double
t_inverse (double probability, double df)
{
  double low = 0, high = 200;

  if (df <= 0 || probability <= 0 || probability >= 1)
    return NAN;
  for (int i = 0; i < 200; i++)
    {
      double mid = (low + high) / 2;

      if (t_distribution_tail (mid, df) > probability)
        low = mid;
      else
        high = mid;
    }
  return (low + high) / 2;
}

/* ---- Descriptive statistics ------------------------------------------ */

gboolean
o42_analysis_descriptive (O42Sheet *sheet, const O42AnalysisOptions *options)
{
  static const char *const ROWS[] = {
    "Mean", "Standard Error", "Median", "Mode", "Standard Deviation",
    "Sample Variance", "Kurtosis", "Skewness", "Range", "Minimum",
    "Maximum", "Sum", "Count"
  };
  GArray *variables;
  double level;

  g_return_val_if_fail (sheet != NULL && options != NULL, FALSE);
  variables = read_variables (sheet, options);
  level = options->confidence > 0 ? options->confidence : 0.95;

  o42_sheet_begin_group (sheet);
  for (guint i = 0; i < G_N_ELEMENTS (ROWS); i++)
    put_text (sheet, options->out_row + 1 + (int) i, options->out_col, ROWS[i]);
  {
    char *label = g_strdup_printf ("Confidence Level(%.4g%%)", level * 100);

    put_text (sheet, options->out_row + 1 + (int) G_N_ELEMENTS (ROWS), options->out_col, label);
    g_free (label);
  }

  for (guint v = 0; v < variables->len; v++)
    {
      const Variable *var = &g_array_index (variables, Variable, v);
      const GArray *values = var->values;
      int col = options->out_col + 1 + (int) v;
      int row = options->out_row;
      guint n = values->len;
      double mean = mean_of (values);
      double m2 = moment_of (values, mean, 2);
      double variance = n > 1 ? m2 / (n - 1) : NAN;
      double sd = sqrt (variance);
      double lowest = NAN, highest = NAN, sum = 0;

      for (guint i = 0; i < n; i++)
        {
          double x = g_array_index (values, double, i);

          if (isnan (lowest) || x < lowest) lowest = x;
          if (isnan (highest) || x > highest) highest = x;
          sum += x;
        }

      put_text (sheet, row, col, var->name);
      put_number (sheet, ++row, col, mean);
      put_number (sheet, ++row, col, n > 0 ? sd / sqrt (n) : NAN);
      put_number (sheet, ++row, col, median_of (values));
      put_number (sheet, ++row, col, mode_of (values));
      put_number (sheet, ++row, col, sd);
      put_number (sheet, ++row, col, variance);
      /* The sample kurtosis and skew Excel reports, which correct for
       * the size of the sample. */
      if (n > 3 && variance > 0)
        {
          double m4 = moment_of (values, mean, 4);
          double term = n * (n + 1.0) / ((n - 1.0) * (n - 2) * (n - 3));
          double last = 3.0 * (n - 1) * (n - 1) / ((n - 2.0) * (n - 3));

          put_number (sheet, ++row, col, term * m4 / (variance * variance) - last);
        }
      else
        put_text (sheet, ++row, col, "#DIV/0!");
      if (n > 2 && variance > 0)
        {
          double m3 = moment_of (values, mean, 3);

          put_number (sheet, ++row, col,
                      n / ((n - 1.0) * (n - 2)) * m3 / (sd * sd * sd));
        }
      else
        put_text (sheet, ++row, col, "#DIV/0!");
      put_number (sheet, ++row, col, highest - lowest);
      put_number (sheet, ++row, col, lowest);
      put_number (sheet, ++row, col, highest);
      put_number (sheet, ++row, col, sum);
      put_number (sheet, ++row, col, n);
      put_number (sheet, ++row, col,
                  n > 1 ? t_inverse (1 - level, n - 1) * sd / sqrt (n) : NAN);
    }

  o42_sheet_end_group (sheet);
  free_variables (variables);
  return TRUE;
}

/* ---- Correlation and covariance -------------------------------------- */

static gboolean
matrix_tool (O42Sheet *sheet, const O42AnalysisOptions *options, gboolean correlation)
{
  GArray *variables;

  g_return_val_if_fail (sheet != NULL && options != NULL, FALSE);
  variables = read_variables (sheet, options);

  o42_sheet_begin_group (sheet);
  for (guint i = 0; i < variables->len; i++)
    {
      const Variable *a = &g_array_index (variables, Variable, i);

      put_text (sheet, options->out_row, options->out_col + 1 + (int) i, a->name);
      put_text (sheet, options->out_row + 1 + (int) i, options->out_col, a->name);
    }

  for (guint i = 0; i < variables->len; i++)
    for (guint j = 0; j <= i; j++)
      {
        const Variable *a = &g_array_index (variables, Variable, i);
        const Variable *b = &g_array_index (variables, Variable, j);
        guint n = MIN (a->values->len, b->values->len);
        double mean_a = mean_of (a->values), mean_b = mean_of (b->values);
        double sum = 0, var_a = 0, var_b = 0;

        for (guint k = 0; k < n; k++)
          {
            double da = g_array_index (a->values, double, k) - mean_a;
            double db = g_array_index (b->values, double, k) - mean_b;

            sum += da * db;
            var_a += da * da;
            var_b += db * db;
          }
        if (n == 0)
          continue;
        if (correlation)
          put_number (sheet, options->out_row + 1 + (int) i, options->out_col + 1 + (int) j,
                      (var_a > 0 && var_b > 0) ? sum / sqrt (var_a * var_b) : NAN);
        else
          put_number (sheet, options->out_row + 1 + (int) i, options->out_col + 1 + (int) j,
                      sum / n);   /* the population covariance, as Excel gives it */
      }

  o42_sheet_end_group (sheet);
  free_variables (variables);
  return TRUE;
}

gboolean
o42_analysis_correlation (O42Sheet *sheet, const O42AnalysisOptions *options)
{
  return matrix_tool (sheet, options, TRUE);
}

gboolean
o42_analysis_covariance (O42Sheet *sheet, const O42AnalysisOptions *options)
{
  return matrix_tool (sheet, options, FALSE);
}

/* ---- Regression ------------------------------------------------------ */

/* Solves the normal equations by Gaussian elimination with partial
 * pivoting, and hands back the inverse too, which the standard errors
 * of the coefficients are made of. */
static gboolean
solve_normal (double *a, double *b, double *inverse, int n)
{
  int *pivot = g_new (int, n);
  gboolean ok = TRUE;

  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      inverse[i * n + j] = (i == j) ? 1 : 0;

  for (int i = 0; i < n && ok; i++)
    {
      int best = i;

      for (int r = i + 1; r < n; r++)
        if (fabs (a[r * n + i]) > fabs (a[best * n + i]))
          best = r;
      if (fabs (a[best * n + i]) < 1e-12)
        { ok = FALSE; break; }
      if (best != i)
        {
          for (int c = 0; c < n; c++)
            {
              double t = a[i * n + c]; a[i * n + c] = a[best * n + c]; a[best * n + c] = t;
              t = inverse[i * n + c]; inverse[i * n + c] = inverse[best * n + c]; inverse[best * n + c] = t;
            }
          { double t = b[i]; b[i] = b[best]; b[best] = t; }
        }
      pivot[i] = i;
      {
        double d = a[i * n + i];

        for (int c = 0; c < n; c++)
          {
            a[i * n + c] /= d;
            inverse[i * n + c] /= d;
          }
        b[i] /= d;
      }
      for (int r = 0; r < n; r++)
        {
          double f;

          if (r == i)
            continue;
          f = a[r * n + i];
          if (f == 0)
            continue;
          for (int c = 0; c < n; c++)
            {
              a[r * n + c] -= f * a[i * n + c];
              inverse[r * n + c] -= f * inverse[i * n + c];
            }
          b[r] -= f * b[i];
        }
    }
  g_free (pivot);
  return ok;
}

gboolean
o42_analysis_regression (O42Sheet *sheet, const O42AnalysisOptions *options)
{
  GArray *variables;
  int k, n, terms;
  double *a, *b, *inverse;
  double mean_y = 0, ss_total = 0, ss_residual = 0;
  int row;

  g_return_val_if_fail (sheet != NULL && options != NULL, FALSE);
  variables = read_variables (sheet, options);
  if (variables->len < 2)
    { free_variables (variables); return FALSE; }

  /* The last variable is the one being explained; the rest explain it. */
  k = (int) variables->len - 1;
  n = (int) g_array_index (variables, Variable, variables->len - 1).values->len;
  for (int i = 0; i < k; i++)
    n = MIN (n, (int) g_array_index (variables, Variable, i).values->len);
  terms = k + 1;
  if (n <= terms)
    { free_variables (variables); return FALSE; }

  a = g_new0 (double, terms * terms);
  b = g_new0 (double, terms);
  inverse = g_new0 (double, terms * terms);

  for (int row_i = 0; row_i < n; row_i++)
    {
      double y = g_array_index (g_array_index (variables, Variable, variables->len - 1).values,
                                double, row_i);
      double *x = g_new (double, terms);

      x[0] = 1;
      for (int i = 0; i < k; i++)
        x[i + 1] = g_array_index (g_array_index (variables, Variable, i).values, double, row_i);
      for (int i = 0; i < terms; i++)
        {
          for (int j = 0; j < terms; j++)
            a[i * terms + j] += x[i] * x[j];
          b[i] += x[i] * y;
        }
      mean_y += y;
      g_free (x);
    }
  mean_y /= n;

  if (!solve_normal (a, b, inverse, terms))
    {
      g_free (a); g_free (b); g_free (inverse);
      free_variables (variables);
      return FALSE;
    }

  for (int row_i = 0; row_i < n; row_i++)
    {
      double y = g_array_index (g_array_index (variables, Variable, variables->len - 1).values,
                                double, row_i);
      double fitted = b[0];

      for (int i = 0; i < k; i++)
        fitted += b[i + 1] * g_array_index (g_array_index (variables, Variable, i).values,
                                            double, row_i);
      ss_total += (y - mean_y) * (y - mean_y);
      ss_residual += (y - fitted) * (y - fitted);
    }

  o42_sheet_begin_group (sheet);
  row = options->out_row;
  put_text (sheet, row, options->out_col, "SUMMARY OUTPUT");
  row += 2;
  {
    double ss_model = ss_total - ss_residual;
    double r2 = ss_total > 0 ? ss_model / ss_total : NAN;
    double adjusted = 1 - (1 - r2) * (n - 1.0) / (n - terms);
    double mse = ss_residual / (n - terms);
    double standard_error = sqrt (mse);
    double f = (ss_model / k) / mse;

    put_text (sheet, row, options->out_col, "Multiple R");
    put_number (sheet, row++, options->out_col + 1, sqrt (r2));
    put_text (sheet, row, options->out_col, "R Square");
    put_number (sheet, row++, options->out_col + 1, r2);
    put_text (sheet, row, options->out_col, "Adjusted R Square");
    put_number (sheet, row++, options->out_col + 1, adjusted);
    put_text (sheet, row, options->out_col, "Standard Error");
    put_number (sheet, row++, options->out_col + 1, standard_error);
    put_text (sheet, row, options->out_col, "Observations");
    put_number (sheet, row++, options->out_col + 1, n);
    row++;

    /* The analysis of variance for the fit. */
    put_text (sheet, row++, options->out_col, "ANOVA");
    put_text (sheet, row, options->out_col + 1, "df");
    put_text (sheet, row, options->out_col + 2, "SS");
    put_text (sheet, row, options->out_col + 3, "MS");
    put_text (sheet, row, options->out_col + 4, "F");
    put_text (sheet, row++, options->out_col + 5, "Significance F");
    put_text (sheet, row, options->out_col, "Regression");
    put_number (sheet, row, options->out_col + 1, k);
    put_number (sheet, row, options->out_col + 2, ss_model);
    put_number (sheet, row, options->out_col + 3, ss_model / k);
    put_number (sheet, row, options->out_col + 4, f);
    put_number (sheet, row++, options->out_col + 5, f_distribution_tail (f, k, n - terms));
    put_text (sheet, row, options->out_col, "Residual");
    put_number (sheet, row, options->out_col + 1, n - terms);
    put_number (sheet, row, options->out_col + 2, ss_residual);
    put_number (sheet, row++, options->out_col + 3, mse);
    put_text (sheet, row, options->out_col, "Total");
    put_number (sheet, row, options->out_col + 1, n - 1);
    put_number (sheet, row++, options->out_col + 2, ss_total);
    row++;

    /* The coefficients, with their standard errors. */
    put_text (sheet, row, options->out_col + 1, "Coefficients");
    put_text (sheet, row, options->out_col + 2, "Standard Error");
    put_text (sheet, row, options->out_col + 3, "t Stat");
    put_text (sheet, row++, options->out_col + 4, "P-value");
    for (int i = 0; i < terms; i++)
      {
        double se = sqrt (MAX (inverse[i * terms + i], 0) * mse);
        double t = se > 0 ? b[i] / se : NAN;

        if (i == 0)
          put_text (sheet, row, options->out_col, "Intercept");
        else
          put_text (sheet, row, options->out_col,
                    g_array_index (variables, Variable, i - 1).name);
        put_number (sheet, row, options->out_col + 1, b[i]);
        put_number (sheet, row, options->out_col + 2, se);
        put_number (sheet, row, options->out_col + 3, t);
        put_number (sheet, row++, options->out_col + 4,
                    isnan (t) ? NAN : t_distribution_tail (fabs (t), n - terms));
      }
  }
  o42_sheet_end_group (sheet);

  g_free (a);
  g_free (b);
  g_free (inverse);
  free_variables (variables);
  return TRUE;
}

/* ---- Histogram ------------------------------------------------------- */

gboolean
o42_analysis_histogram (O42Sheet *sheet, const O42AnalysisOptions *options)
{
  GArray *variables;
  GArray *all;
  double lowest = NAN, highest = NAN, width;
  int bins, row;
  guint *counts;
  guint total = 0;

  g_return_val_if_fail (sheet != NULL && options != NULL, FALSE);
  variables = read_variables (sheet, options);
  all = g_array_new (FALSE, FALSE, sizeof (double));
  for (guint v = 0; v < variables->len; v++)
    {
      const GArray *values = g_array_index (variables, Variable, v).values;

      g_array_append_vals (all, values->data, values->len);
    }
  if (all->len == 0)
    {
      g_array_free (all, TRUE);
      free_variables (variables);
      return FALSE;
    }

  for (guint i = 0; i < all->len; i++)
    {
      double x = g_array_index (all, double, i);

      if (isnan (lowest) || x < lowest) lowest = x;
      if (isnan (highest) || x > highest) highest = x;
    }
  /* The square root of the count, rounded up, is the usual choice when
   * no number of bins is asked for. */
  bins = options->bins > 0 ? options->bins : (int) ceil (sqrt ((double) all->len));
  bins = CLAMP (bins, 1, 1000);
  width = (highest - lowest) / bins;
  if (width <= 0)
    width = 1;
  counts = g_new0 (guint, bins);
  for (guint i = 0; i < all->len; i++)
    {
      double x = g_array_index (all, double, i);
      int which = (int) floor ((x - lowest) / width);

      counts[CLAMP (which, 0, bins - 1)]++;
      total++;
    }

  o42_sheet_begin_group (sheet);
  row = options->out_row;
  put_text (sheet, row, options->out_col, "Bin");
  put_text (sheet, row, options->out_col + 1, "Frequency");
  put_text (sheet, row++, options->out_col + 2, "Cumulative %");
  {
    guint running = 0;

    for (int i = 0; i < bins; i++)
      {
        running += counts[i];
        put_number (sheet, row, options->out_col, lowest + width * (i + 1));
        put_number (sheet, row, options->out_col + 1, counts[i]);
        put_number (sheet, row++, options->out_col + 2, 100.0 * running / total);
      }
  }
  o42_sheet_end_group (sheet);

  g_free (counts);
  g_array_free (all, TRUE);
  free_variables (variables);
  return TRUE;
}

/* ---- Analysis of variance, one factor -------------------------------- */

gboolean
o42_analysis_anova (O42Sheet *sheet, const O42AnalysisOptions *options)
{
  GArray *variables;
  double grand_sum = 0, grand_mean;
  guint grand_count = 0;
  double ss_between = 0, ss_within = 0;
  int row, groups;

  g_return_val_if_fail (sheet != NULL && options != NULL, FALSE);
  variables = read_variables (sheet, options);
  groups = (int) variables->len;
  if (groups < 2)
    { free_variables (variables); return FALSE; }

  for (int g = 0; g < groups; g++)
    {
      const GArray *values = g_array_index (variables, Variable, g).values;

      for (guint i = 0; i < values->len; i++)
        grand_sum += g_array_index (values, double, i);
      grand_count += values->len;
    }
  if (grand_count <= (guint) groups)
    { free_variables (variables); return FALSE; }
  grand_mean = grand_sum / grand_count;

  o42_sheet_begin_group (sheet);
  row = options->out_row;
  put_text (sheet, row++, options->out_col, "SUMMARY");
  put_text (sheet, row, options->out_col, "Groups");
  put_text (sheet, row, options->out_col + 1, "Count");
  put_text (sheet, row, options->out_col + 2, "Sum");
  put_text (sheet, row, options->out_col + 3, "Average");
  put_text (sheet, row++, options->out_col + 4, "Variance");

  for (int g = 0; g < groups; g++)
    {
      const Variable *v = &g_array_index (variables, Variable, g);
      const GArray *values = v->values;
      double mean = mean_of (values);
      double m2 = moment_of (values, mean, 2);
      double sum = mean * values->len;

      put_text (sheet, row, options->out_col, v->name);
      put_number (sheet, row, options->out_col + 1, values->len);
      put_number (sheet, row, options->out_col + 2, sum);
      put_number (sheet, row, options->out_col + 3, mean);
      put_number (sheet, row++, options->out_col + 4,
                  values->len > 1 ? m2 / (values->len - 1) : NAN);

      ss_between += values->len * (mean - grand_mean) * (mean - grand_mean);
      ss_within += m2;
    }

  row++;
  put_text (sheet, row++, options->out_col, "ANOVA");
  put_text (sheet, row, options->out_col, "Source of Variation");
  put_text (sheet, row, options->out_col + 1, "SS");
  put_text (sheet, row, options->out_col + 2, "df");
  put_text (sheet, row, options->out_col + 3, "MS");
  put_text (sheet, row, options->out_col + 4, "F");
  put_text (sheet, row++, options->out_col + 5, "P-value");
  {
    int df_between = groups - 1;
    int df_within = (int) grand_count - groups;
    double ms_between = ss_between / df_between;
    double ms_within = ss_within / df_within;
    double f = ms_within > 0 ? ms_between / ms_within : NAN;

    put_text (sheet, row, options->out_col, "Between Groups");
    put_number (sheet, row, options->out_col + 1, ss_between);
    put_number (sheet, row, options->out_col + 2, df_between);
    put_number (sheet, row, options->out_col + 3, ms_between);
    put_number (sheet, row, options->out_col + 4, f);
    put_number (sheet, row++, options->out_col + 5,
                isnan (f) ? NAN : f_distribution_tail (f, df_between, df_within));
    put_text (sheet, row, options->out_col, "Within Groups");
    put_number (sheet, row, options->out_col + 1, ss_within);
    put_number (sheet, row, options->out_col + 2, df_within);
    put_number (sheet, row++, options->out_col + 3, ms_within);
    put_text (sheet, row, options->out_col, "Total");
    put_number (sheet, row, options->out_col + 1, ss_between + ss_within);
    put_number (sheet, row++, options->out_col + 2, (int) grand_count - 1);
  }
  o42_sheet_end_group (sheet);

  free_variables (variables);
  return TRUE;
}

/* ---- Rank and percentile --------------------------------------------- */

/* ---- Two-factor analysis of variance ---------------------------------- */

/* The input as a rectangle of numbers, without the labels: rows by
 * columns, with NAN where a cell holds no number.  A two-factor
 * analysis reads its input as a table and not as a list of variables,
 * because the position of a value in the table is what it means. */
static double *
read_matrix (O42Sheet *sheet, const O42AnalysisOptions *options,
             int *rows_out, int *cols_out, int *first_row, int *first_col)
{
  O42Range r = o42_range_normalise (options->input.row0, options->input.col0,
                                    options->input.row1, options->input.col1);
  int row0 = r.row0 + (options->labels ? 1 : 0);
  int col0 = r.col0 + (options->labels ? 1 : 0);
  int rows = r.row1 - row0 + 1;
  int cols = r.col1 - col0 + 1;
  double *cells;

  if (rows < 1 || cols < 1)
    return NULL;
  cells = g_new (double, (gsize) rows * cols);
  for (int i = 0; i < rows; i++)
    for (int j = 0; j < cols; j++)
      {
        O42Value value;

        o42_sheet_get_value (sheet, row0 + i, col0 + j, &value);
        cells[(gsize) i * cols + j] = value.type == O42_VALUE_NUMBER ? value.as.number : NAN;
        o42_value_clear (&value);
      }
  *rows_out = rows;
  *cols_out = cols;
  *first_row = row0;
  *first_col = col0;
  return cells;
}

/* The name in the margin of the table, or "Row 3" when there is none. */
static char *
margin_name (O42Sheet *sheet, const O42AnalysisOptions *options,
             gboolean down, int index, int first_row, int first_col)
{
  O42Range r = o42_range_normalise (options->input.row0, options->input.col0,
                                    options->input.row1, options->input.col1);
  char *shown = NULL;

  if (options->labels)
    shown = down ? o42_sheet_get_display (sheet, first_row + index, r.col0)
                 : o42_sheet_get_display (sheet, r.row0, first_col + index);
  if (shown != NULL && *shown != '\0')
    return shown;
  g_free (shown);
  return g_strdup_printf (down ? "Row %d" : "Column %d", index + 1);
}

/* The summary line a two-factor table puts in front of every row and
 * every column: how many, their sum, their mean and their variance. */
static void
put_summary (O42Sheet *sheet, int row, int col, const char *name,
             const double *values, int n)
{
  double sum = 0, mean, m2 = 0;
  int count = 0;

  for (int i = 0; i < n; i++)
    if (!isnan (values[i]))
      { sum += values[i]; count++; }
  mean = count > 0 ? sum / count : NAN;
  for (int i = 0; i < n; i++)
    if (!isnan (values[i]))
      m2 += (values[i] - mean) * (values[i] - mean);

  put_text (sheet, row, col, name);
  put_number (sheet, row, col + 1, count);
  put_number (sheet, row, col + 2, sum);
  put_number (sheet, row, col + 3, mean);
  put_number (sheet, row, col + 4, count > 1 ? m2 / (count - 1) : NAN);
}

/* One line of the ANOVA table. */
static void
put_anova_line (O42Sheet *sheet, int row, int col, const char *source,
                double ss, int df, gboolean with_f, double ms_error, int df_error)
{
  double ms = df > 0 ? ss / df : NAN;
  double f = with_f && ms_error > 0 ? ms / ms_error : NAN;

  put_text (sheet, row, col, source);
  put_number (sheet, row, col + 1, ss);
  put_number (sheet, row, col + 2, df);
  if (df > 0)
    put_number (sheet, row, col + 3, ms);
  if (!isnan (f))
    {
      put_number (sheet, row, col + 4, f);
      put_number (sheet, row, col + 5, f_distribution_tail (f, df, df_error));
    }
}

gboolean
o42_analysis_anova2 (O42Sheet *sheet, const O42AnalysisOptions *options)
{
  int rows = 0, cols = 0, first_row = 0, first_col = 0;
  double *cells;
  int per = MAX (options->per_sample, 1);
  int samples;
  double grand_sum = 0, grand_mean;
  int grand_count = 0;
  int row;

  g_return_val_if_fail (sheet != NULL && options != NULL, FALSE);
  cells = read_matrix (sheet, options, &rows, &cols, &first_row, &first_col);
  if (cells == NULL)
    return FALSE;
  samples = rows / per;
  if (cols < 2 || samples < 2 || rows % per != 0)
    { g_free (cells); return FALSE; }

  for (int i = 0; i < rows * cols; i++)
    if (!isnan (cells[i]))
      { grand_sum += cells[i]; grand_count++; }
  if (grand_count < rows * cols)
    { g_free (cells); return FALSE; }    /* a gap makes the sums of squares meaningless */
  grand_mean = grand_sum / grand_count;

  o42_sheet_begin_group (sheet);
  row = options->out_row;
  put_text (sheet, row++, options->out_col, "SUMMARY");
  put_text (sheet, row, options->out_col + 1, "Count");
  put_text (sheet, row, options->out_col + 2, "Sum");
  put_text (sheet, row, options->out_col + 3, "Average");
  put_text (sheet, row++, options->out_col + 4, "Variance");

  /* Every row of the table (or every sample of rows, when the samples
   * repeat), then every column. */
  {
    double *line = g_new (double, (gsize) MAX (rows, cols) * per);

    for (int s = 0; s < samples; s++)
      {
        char *name = margin_name (sheet, options, TRUE, s * per, first_row, first_col);
        int n = 0;

        for (int i = s * per; i < (s + 1) * per; i++)
          for (int j = 0; j < cols; j++)
            line[n++] = cells[(gsize) i * cols + j];
        put_summary (sheet, row++, options->out_col, name, line, n);
        g_free (name);
      }
    row++;
    for (int j = 0; j < cols; j++)
      {
        char *name = margin_name (sheet, options, FALSE, j, first_row, first_col);

        for (int i = 0; i < rows; i++)
          line[i] = cells[(gsize) i * cols + j];
        put_summary (sheet, row++, options->out_col, name, line, rows);
        g_free (name);
      }
    g_free (line);
  }

  row++;
  put_text (sheet, row++, options->out_col, "ANOVA");
  put_text (sheet, row, options->out_col, "Source of Variation");
  put_text (sheet, row, options->out_col + 1, "SS");
  put_text (sheet, row, options->out_col + 2, "df");
  put_text (sheet, row, options->out_col + 3, "MS");
  put_text (sheet, row, options->out_col + 4, "F");
  put_text (sheet, row++, options->out_col + 5, "P-value");

  {
    double ss_rows = 0, ss_cols = 0, ss_within = 0, ss_total = 0, ss_inter;
    int df_rows = samples - 1, df_cols = cols - 1;
    int df_within = per > 1 ? samples * cols * (per - 1) : df_rows * df_cols;
    int df_inter = per > 1 ? df_rows * df_cols : 0;
    double ms_within;

    for (int s = 0; s < samples; s++)
      {
        double sum = 0;

        for (int i = s * per; i < (s + 1) * per; i++)
          for (int j = 0; j < cols; j++)
            sum += cells[(gsize) i * cols + j];
        {
          double mean = sum / (per * cols);

          ss_rows += per * cols * (mean - grand_mean) * (mean - grand_mean);
        }
      }
    for (int j = 0; j < cols; j++)
      {
        double sum = 0;

        for (int i = 0; i < rows; i++)
          sum += cells[(gsize) i * cols + j];
        {
          double mean = sum / rows;

          ss_cols += rows * (mean - grand_mean) * (mean - grand_mean);
        }
      }
    for (int i = 0; i < rows * cols; i++)
      ss_total += (cells[i] - grand_mean) * (cells[i] - grand_mean);

    if (per > 1)
      {
        /* With repeats, what is left inside a cell of the table is the
         * error, and what is left over between them is the interaction. */
        for (int s = 0; s < samples; s++)
          for (int j = 0; j < cols; j++)
            {
              double sum = 0, mean;

              for (int i = s * per; i < (s + 1) * per; i++)
                sum += cells[(gsize) i * cols + j];
              mean = sum / per;
              for (int i = s * per; i < (s + 1) * per; i++)
                {
                  double d = cells[(gsize) i * cols + j] - mean;

                  ss_within += d * d;
                }
            }
        ss_inter = ss_total - ss_rows - ss_cols - ss_within;
      }
    else
      {
        ss_within = ss_total - ss_rows - ss_cols;
        ss_inter = 0;
      }
    ms_within = df_within > 0 ? ss_within / df_within : NAN;

    put_anova_line (sheet, row++, options->out_col,
                    per > 1 ? "Sample" : "Rows", ss_rows, df_rows, TRUE, ms_within, df_within);
    put_anova_line (sheet, row++, options->out_col, "Columns", ss_cols, df_cols, TRUE, ms_within, df_within);
    if (per > 1)
      put_anova_line (sheet, row++, options->out_col, "Interaction", ss_inter, df_inter, TRUE, ms_within, df_within);
    put_anova_line (sheet, row++, options->out_col,
                    per > 1 ? "Within" : "Error", ss_within, df_within, FALSE, 0, 0);
    put_anova_line (sheet, row++, options->out_col, "Total", ss_total, rows * cols - 1, FALSE, 0, 0);
  }

  o42_sheet_end_group (sheet);
  g_free (cells);
  return TRUE;
}

/* ---- Sampling ---------------------------------------------------------- */

gboolean
o42_analysis_sampling (O42Sheet *sheet, const O42AnalysisOptions *options)
{
  GArray *variables;
  int wanted = options->sample_size;
  int row = options->out_row;

  g_return_val_if_fail (sheet != NULL && options != NULL, FALSE);
  variables = read_variables (sheet, options);
  if (variables->len == 0 || wanted < 1)
    { free_variables (variables); return FALSE; }

  o42_sheet_begin_group (sheet);
  for (guint v = 0; v < variables->len; v++)
    {
      const Variable *var = &g_array_index (variables, Variable, v);
      const GArray *values = var->values;
      int col = options->out_col + (int) v;
      int at = options->out_row;

      put_text (sheet, at++, col, var->name);
      if (values->len == 0)
        continue;

      if (options->periodic)
        {
          /* Every nth value, starting at the nth: what Excel and
           * Gnumeric both mean by a periodic sample. */
          int period = wanted;

          for (guint i = period - 1; i < values->len; i += period)
            put_number (sheet, at++, col, g_array_index (values, double, i));
        }
      else
        {
          for (int i = 0; i < wanted; i++)
            put_number (sheet, at++, col,
                        g_array_index (values, double,
                                       g_random_int_range (0, (gint32) values->len)));
        }
      row = MAX (row, at);
    }
  o42_sheet_end_group (sheet);

  free_variables (variables);
  return TRUE;
}

gboolean
o42_analysis_rank (O42Sheet *sheet, const O42AnalysisOptions *options)
{
  GArray *variables;
  int row;

  g_return_val_if_fail (sheet != NULL && options != NULL, FALSE);
  variables = read_variables (sheet, options);

  o42_sheet_begin_group (sheet);
  row = options->out_row;
  put_text (sheet, row, options->out_col, "Point");
  put_text (sheet, row, options->out_col + 1,
            variables->len > 0 ? g_array_index (variables, Variable, 0).name : "Column 1");
  put_text (sheet, row, options->out_col + 2, "Rank");
  put_text (sheet, row++, options->out_col + 3, "Percent");

  if (variables->len > 0)
    {
      const GArray *values = g_array_index (variables, Variable, 0).values;
      guint n = values->len;
      GArray *order = g_array_sized_new (FALSE, FALSE, sizeof (guint), n);

      for (guint i = 0; i < n; i++)
        g_array_append_val (order, i);
      /* Largest first, as Excel's tool reports it. */
      for (guint i = 0; i < n; i++)
        for (guint j = i + 1; j < n; j++)
          {
            guint ai = g_array_index (order, guint, i);
            guint aj = g_array_index (order, guint, j);

            if (g_array_index (values, double, aj) > g_array_index (values, double, ai))
              {
                g_array_index (order, guint, i) = aj;
                g_array_index (order, guint, j) = ai;
              }
          }
      for (guint i = 0; i < n; i++)
        {
          guint which = g_array_index (order, guint, i);
          double value = g_array_index (values, double, which);
          guint rank = 1;

          for (guint j = 0; j < n; j++)
            if (g_array_index (values, double, j) > value)
              rank++;
          put_number (sheet, row, options->out_col, which + 1);
          put_number (sheet, row, options->out_col + 1, value);
          put_number (sheet, row, options->out_col + 2, rank);
          put_number (sheet, row++, options->out_col + 3,
                      n > 1 ? 100.0 * (n - rank) / (n - 1) : 100.0);
        }
      g_array_free (order, TRUE);
    }
  o42_sheet_end_group (sheet);

  free_variables (variables);
  return TRUE;
}

/* ---- Moving average --------------------------------------------------- */

gboolean
o42_analysis_moving (O42Sheet *sheet, const O42AnalysisOptions *options)
{
  GArray *variables;
  int terms = (int) (options->interval > 0 ? options->interval : 3);

  g_return_val_if_fail (sheet != NULL && options != NULL, FALSE);
  variables = read_variables (sheet, options);

  o42_sheet_begin_group (sheet);
  for (guint v = 0; v < variables->len; v++)
    {
      const Variable *var = &g_array_index (variables, Variable, v);
      const GArray *values = var->values;
      int col = options->out_col + (int) v;
      int row = options->out_row;

      put_text (sheet, row++, col, var->name);
      for (guint i = 0; i < values->len; i++)
        {
          if ((int) i + 1 < terms)
            put_text (sheet, row++, col, "#N/A");
          else
            {
              double sum = 0;

              for (int k = 0; k < terms; k++)
                sum += g_array_index (values, double, i - k);
              put_number (sheet, row++, col, sum / terms);
            }
        }
    }
  o42_sheet_end_group (sheet);

  free_variables (variables);
  return TRUE;
}
