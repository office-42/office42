/* o42-analysis.h - the statistical analysis tools
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What Gnumeric puts under Tools > Statistical Analysis and Excel
 * under the Analysis ToolPak: a table of numbers in, a labelled table
 * of results out, written into the sheet as ordinary cells so that
 * everything else -- formats, printing, files -- works on them without
 * knowing anything about statistics.
 *
 * Every one of them is a single undo step.
 */

#pragma once

#include "o42-sheet.h"

G_BEGIN_DECLS

/* Which way the data lies: one variable per column, or per row. */
typedef enum {
  O42_ANALYSIS_COLUMNS,
  O42_ANALYSIS_ROWS
} O42AnalysisLayout;

typedef struct {
  O42Range          input;
  O42AnalysisLayout layout;
  gboolean          labels;      /* the first row (or column) names the variables */
  int               out_row;     /* where the results go */
  int               out_col;
  double            confidence;  /* for the confidence level, 0.95 by default */
  int               bins;        /* histogram: how many, 0 to choose */
  double            interval;    /* moving average: how many terms */
} O42AnalysisOptions;

/* Count, mean, standard error, median, mode, standard deviation,
 * variance, kurtosis, skew, range, smallest, largest, sum and the
 * confidence level, for each variable. */
gboolean o42_analysis_descriptive (O42Sheet *sheet, const O42AnalysisOptions *options);

/* The matrix of correlations, or of covariances, between variables. */
gboolean o42_analysis_correlation (O42Sheet *sheet, const O42AnalysisOptions *options);
gboolean o42_analysis_covariance  (O42Sheet *sheet, const O42AnalysisOptions *options);

/* Least squares of the last variable on the others: the coefficients,
 * their standard errors and t statistics, R squared, and the analysis
 * of variance for the fit. */
gboolean o42_analysis_regression  (O42Sheet *sheet, const O42AnalysisOptions *options);

/* How many values fall in each bin, with the cumulative percentage. */
gboolean o42_analysis_histogram   (O42Sheet *sheet, const O42AnalysisOptions *options);

/* One-factor analysis of variance: the groups summarised, then the
 * table of sums of squares with F and its probability. */
gboolean o42_analysis_anova       (O42Sheet *sheet, const O42AnalysisOptions *options);

/* Every value with its rank and its percentile. */
gboolean o42_analysis_rank        (O42Sheet *sheet, const O42AnalysisOptions *options);

/* The moving average over `interval` terms of each variable. */
gboolean o42_analysis_moving      (O42Sheet *sheet, const O42AnalysisOptions *options);

G_END_DECLS
