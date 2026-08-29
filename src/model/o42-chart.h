/* o42-chart.h - a chart floating over the grid, drawn from a range
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A chart is a picture of some cells: it names a range and is redrawn
 * from the cells' current values every time it is painted, so it is never
 * out of date.  It floats over the grid anchored to a cell, as a picture
 * does.  Three kinds, the ones Excel 5's ChartWizard offered first:
 * column, line and pie.  Cairo draws them, so the same code paints the
 * grid, the PDF and the printed page.
 */

#pragma once

#include <cairo.h>

#include "o42-types.h"
#include "o42-value.h"

G_BEGIN_DECLS

typedef enum {
  O42_CHART_COLUMN = 0,
  O42_CHART_LINE,
  O42_CHART_PIE,
  O42_CHART_BAR,        /* columns lying down */
  O42_CHART_AREA,       /* lines with the space under them filled */
  O42_CHART_SCATTER,    /* XY: the first series is x, the rest y */
  O42_CHART_STACKED,    /* columns stacked on one another */
  O42_CHART_PERCENT,    /* stacked, each category scaled to 100% */
  O42_CHART_DOUGHNUT,   /* a pie with a hole in it */
  O42_CHART_RADAR,      /* a spoke per category, a polygon per series */
  O42_CHART_BUBBLE,     /* XY where a third series is the size */
  O42_CHART_STOCK,      /* high-low-close, or open-high-low-close candles */
  O42_CHART_SURFACE,    /* the values as a height field, in bands of colour */

  /* The four Gnumeric draws and Excel does not. */
  O42_CHART_BOX,        /* a box and whiskers per series */
  O42_CHART_HISTOGRAM,  /* how many values fall in each of a few bins */
  O42_CHART_POLAR,      /* the angle is the category, the radius the value */
  O42_CHART_CONTOUR     /* the height field from above, with its contours */
} O42ChartKind;

/* What is drawn at each point of a line, a scatter or a radar -- and,
 * for a picture, what a column is filled with. */
typedef enum {
  O42_MARKER_AUTO = 0,   /* a filled circle */
  O42_MARKER_NONE,
  O42_MARKER_CIRCLE,     /* hollow */
  O42_MARKER_SQUARE,
  O42_MARKER_DIAMOND,
  O42_MARKER_TRIANGLE,
  O42_MARKER_CROSS,      /* an x */
  O42_MARKER_PLUS,
  O42_MARKER_STAR,
  O42_MARKER_PICTURE     /* the sheet's picture `marker_picture` */
} O42MarkerKind;

/* The curves Excel offers to fit through a series.  A polynomial's
 * order and a moving average's period are both `trend_order`. */
typedef enum {
  O42_TREND_NONE = 0,
  O42_TREND_LINEAR,      /* y = a + bx */
  O42_TREND_POLY,        /* y = a0 + a1x + ... + anx^n, n = trend_order */
  O42_TREND_EXP,         /* y = a e^(bx), fitted on ln y */
  O42_TREND_LOG,         /* y = a ln x + b */
  O42_TREND_POWER,       /* y = a x^b, fitted on ln x and ln y */
  O42_TREND_MOVING       /* the mean of the last trend_order points */
} O42TrendKind;

/* The error bars Excel draws above and below each point, and what
 * their length is measured in. */
typedef enum {
  O42_ERRBAR_NONE = 0,
  O42_ERRBAR_FIXED,     /* err_value, in the value axis' own units */
  O42_ERRBAR_PERCENT,   /* err_value per cent of the point */
  O42_ERRBAR_STDDEV,    /* err_value standard deviations of the series */
  O42_ERRBAR_STDERR     /* the standard error of the series' mean */
} O42ErrBarKind;

typedef struct {
  guint         id;
  guint         group;       /* objects grouped together share one; 0 for none */
  O42ChartKind  kind;
  O42Range      data;          /* the source cells */
  char         *data_sheet;    /* owned: the sheet they are on, "" for the
                                * sheet the chart is itself on.  A chart
                                * sheet has no cells of its own, so its
                                * chart always names another. */
  gboolean      first_row_labels;   /* the range's top row is a heading */
  gboolean      first_col_labels;   /* the range's left column is a heading */
  /* Which way the series lie.  A table is as often written with a series
   * along each row as down each column, and the same cells make a
   * different chart either way; Excel's ChartWizard asks.  With the series
   * down the columns the top row names them and the left column names the
   * categories, and with the series along the rows those two swap. */
  gboolean      series_in_rows;
  char         *title;         /* owned, may be empty */
  char         *x_title;       /* the category (or x) axis title, owned, may be empty */
  char         *y_title;       /* the value (or y) axis title */
  gboolean      legend;        /* show the legend (when there is more than one series) */
  gboolean      data_labels;   /* write each point's value beside it */
  O42TrendKind  trend;         /* a fitted curve through each series */
  int           trend_order;   /* a polynomial's order, or an average's period */
  char         *font_family;   /* owned: the face the chart's text is set
                                * in, "" for the default */
  double        font_size;     /* points, 0 for the default */
  O42ErrBarKind err_bars;      /* error bars on every series' points */
  double        err_value;     /* what the kind measures, ignored by stderr */
  char         *y_format;      /* owned: a number format code for the value
                                * axis and the data labels, "" for General */
  int           secondary_from; /* 1-based index of the first series drawn
                                 * against a second value axis on the right;
                                 * 0 for none */
  O42MarkerKind marker;        /* what is drawn at each point */
  double        marker_size;   /* across, in pixels; 0 for the default */
  guint         marker_picture; /* the picture's id on the sheet, for O42_MARKER_PICTURE */
  gboolean      three_d;       /* drawn with depth, as Excel's 3-D types are */
  gboolean      gridlines;     /* horizontal gridlines at the value axis' ticks */
  gboolean      has_min;       /* the value axis starts at `min` rather than a round number */
  gboolean      has_max;
  double        min, max;
  int           row;           /* the anchor cell */
  int           col;
  double        dx;            /* offset inside the anchor cell, pixels */
  double        dy;
  double        width;         /* pixels */
  double        height;
} O42Chart;

O42Chart *o42_chart_new  (O42ChartKind kind, const O42Range *data);
void      o42_chart_free (O42Chart *chart);

/* The chart's data as it stands: `get` fetches a cell of the sheet the
 * chart belongs to.  The evaluator's callback shape, so a sheet can hand
 * its own. */
typedef void (*O42ChartFetch) (gpointer user, int row, int col, O42Value *out);

/* The picture a chart draws its markers with, by the id it names.  The
 * sheet hands its own; NULL when there is no such picture. */
typedef cairo_surface_t *(*O42ChartPicture) (gpointer user, guint id);

/* Paints the chart into a width-by-height box at the cairo origin. */
void      o42_chart_draw (const O42Chart *chart, cairo_t *cr,
                          double width, double height,
                          O42ChartFetch fetch, gpointer user);

/* The same, able to find the picture a picture marker names. */
void      o42_chart_draw_full (const O42Chart *chart, cairo_t *cr,
                               double width, double height,
                               O42ChartFetch fetch, gpointer user,
                               O42ChartPicture picture, gpointer picture_user);

/* Guesses, from the range's contents, whether its first row and column
 * are labels: text over numbers is a heading. */
void      o42_chart_guess_labels (O42Chart *chart, O42ChartFetch fetch,
                                  gpointer user);

const char *o42_chart_kind_name (O42ChartKind kind);   /* "column", "line", "pie" */
gboolean    o42_chart_kind_parse (const char *name, O42ChartKind *kind);

/* "none", "fixed", "percent", "stddev", "stderr". */
const char *o42_errbar_kind_name  (O42ErrBarKind kind);
gboolean    o42_errbar_kind_parse (const char *name, O42ErrBarKind *kind);

/* "auto", "none", "circle", "square", "diamond", "triangle", "cross",
 * "plus", "star", "picture". */
const char *o42_marker_kind_name  (O42MarkerKind kind);
gboolean    o42_marker_kind_parse (const char *name, O42MarkerKind *kind);

/* "none", "linear", "poly", "exp", "log", "power", "average". */
const char *o42_trend_kind_name  (O42TrendKind kind);
gboolean    o42_trend_kind_parse (const char *name, O42TrendKind *kind);

/* Fits `kind` to the points and fills `coef`, which must hold at least
 * O42_TREND_MAX_COEF doubles, with the coefficients in rising order of
 * x for a polynomial or line, and with a and b for the other three.
 * The moving average has no coefficients and is not fitted here. */
#define O42_TREND_MAX_COEF 8
gboolean    o42_trend_fit  (O42TrendKind kind, int order,
                            const double *x, const double *y, int n, double *coef);
/* The fitted value at x. */
double      o42_trend_eval (O42TrendKind kind, int order, const double *coef, double x);

G_END_DECLS
