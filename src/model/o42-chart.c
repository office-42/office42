/* o42-chart.c - see o42-chart.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-chart.h"

#include "o42-numfmt.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

/* Excel 5's default series colours, near enough: navy, maroon, green,
 * teal, purple, olive, grey, blue. */
static const guint32 SERIES_COLOURS[] = {
  0x000080, 0x800000, 0x008000, 0x008080, 0x800080, 0x808000, 0x808080, 0x0000FF
};

O42Chart *
o42_chart_new (O42ChartKind kind, const O42Range *data)
{
  O42Chart *chart = g_new0 (O42Chart, 1);

  chart->kind = kind;
  chart->data = *data;
  chart->title = g_strdup ("");
  chart->x_title = g_strdup ("");
  chart->y_title = g_strdup ("");
  chart->y_format = g_strdup ("");
  chart->legend = TRUE;
  chart->gridlines = TRUE;
  chart->trend_order = 2;
  chart->font_family = g_strdup ("");
  chart->data_sheet = g_strdup ("");
  chart->width = 360;
  chart->height = 220;

  return chart;
}

void
o42_chart_free (O42Chart *chart)
{
  if (chart == NULL)
    return;
  g_free (chart->title);
  g_free (chart->x_title);
  g_free (chart->y_title);
  g_free (chart->font_family);
  g_free (chart->data_sheet);
  g_free (chart->y_format);
  g_free (chart);
}

const char *
o42_chart_kind_name (O42ChartKind kind)
{
  switch (kind)
    {
    case O42_CHART_LINE:    return "line";
    case O42_CHART_PIE:     return "pie";
    case O42_CHART_BAR:     return "bar";
    case O42_CHART_AREA:    return "area";
    case O42_CHART_SCATTER: return "scatter";
    case O42_CHART_STACKED: return "stacked";
    case O42_CHART_PERCENT: return "percent";
    case O42_CHART_DOUGHNUT: return "doughnut";
    case O42_CHART_RADAR:   return "radar";
    case O42_CHART_BUBBLE:  return "bubble";
    case O42_CHART_STOCK:   return "stock";
    case O42_CHART_SURFACE: return "surface";
    case O42_CHART_BOX:     return "box";
    case O42_CHART_HISTOGRAM: return "histogram";
    case O42_CHART_POLAR:   return "polar";
    case O42_CHART_CONTOUR: return "contour";
    default:                return "column";
    }
}

gboolean
o42_chart_kind_parse (const char *name, O42ChartKind *kind)
{
  if (name == NULL)
    return FALSE;
  if (g_ascii_strcasecmp (name, "column") == 0)
    { *kind = O42_CHART_COLUMN; return TRUE; }
  if (g_ascii_strcasecmp (name, "bar") == 0)
    { *kind = O42_CHART_BAR; return TRUE; }
  if (g_ascii_strcasecmp (name, "line") == 0)
    { *kind = O42_CHART_LINE; return TRUE; }
  if (g_ascii_strcasecmp (name, "area") == 0)
    { *kind = O42_CHART_AREA; return TRUE; }
  if (g_ascii_strcasecmp (name, "scatter") == 0 || g_ascii_strcasecmp (name, "xy") == 0)
    { *kind = O42_CHART_SCATTER; return TRUE; }
  if (g_ascii_strcasecmp (name, "pie") == 0)
    { *kind = O42_CHART_PIE; return TRUE; }
  if (g_ascii_strcasecmp (name, "stacked") == 0)
    { *kind = O42_CHART_STACKED; return TRUE; }
  if (g_ascii_strcasecmp (name, "percent") == 0)
    { *kind = O42_CHART_PERCENT; return TRUE; }
  if (g_ascii_strcasecmp (name, "doughnut") == 0 || g_ascii_strcasecmp (name, "donut") == 0)
    { *kind = O42_CHART_DOUGHNUT; return TRUE; }
  if (g_ascii_strcasecmp (name, "stock") == 0)
    { *kind = O42_CHART_STOCK; return TRUE; }
  if (g_ascii_strcasecmp (name, "surface") == 0)
    { *kind = O42_CHART_SURFACE; return TRUE; }
  if (g_ascii_strcasecmp (name, "radar") == 0)
    { *kind = O42_CHART_RADAR; return TRUE; }
  if (g_ascii_strcasecmp (name, "bubble") == 0)
    { *kind = O42_CHART_BUBBLE; return TRUE; }
  if (g_ascii_strcasecmp (name, "box") == 0)
    { *kind = O42_CHART_BOX; return TRUE; }
  if (g_ascii_strcasecmp (name, "histogram") == 0)
    { *kind = O42_CHART_HISTOGRAM; return TRUE; }
  if (g_ascii_strcasecmp (name, "polar") == 0)
    { *kind = O42_CHART_POLAR; return TRUE; }
  if (g_ascii_strcasecmp (name, "contour") == 0)
    { *kind = O42_CHART_CONTOUR; return TRUE; }
  return FALSE;
}

void
o42_chart_guess_labels (O42Chart *chart, O42ChartFetch fetch, gpointer user)
{
  const O42Range *r = &chart->data;
  O42Value v;
  gboolean text;

  /* Excel's rule: a text cell at the top of the second column means the
   * first row is headings, a text cell at the left of the second row
   * means the first column is categories.  A row of years over the data
   * counts as data by this rule -- there is no telling "1992" from a
   * value -- and the ChartWizard has a box to say otherwise. */
  chart->first_row_labels = FALSE;
  chart->first_col_labels = FALSE;

  if (r->col1 > r->col0 && r->row1 > r->row0)
    {
      fetch (user, r->row0 + 1, r->col0, &v);
      text = (v.type == O42_VALUE_TEXT);
      o42_value_clear (&v);
      if (text)
        chart->first_col_labels = TRUE;
    }

  if (r->row1 > r->row0)
    {
      int col = (r->col1 > r->col0) ? r->col0 + 1 : r->col0;

      fetch (user, r->row0, col, &v);
      text = (v.type == O42_VALUE_TEXT);
      o42_value_clear (&v);
      if (text)
        chart->first_row_labels = TRUE;
    }
}

/* ---------------------------------------------------------------------- */
/* Drawing                                                                 */
/* ---------------------------------------------------------------------- */

typedef struct {
  int        n_series;
  int        n_points;
  double    *values;      /* n_series * n_points; NAN where not a number */
  char     **series;      /* n_series names, owned */
  char     **categories;  /* n_points names, owned */
  double     min, max;
  cairo_surface_t *marker_picture;   /* for O42_MARKER_PICTURE; not owned */
} ChartData;

static void
chart_data_free (ChartData *d)
{
  g_free (d->values);
  for (int i = 0; i < d->n_series; i++) g_free (d->series[i]);
  for (int i = 0; i < d->n_points; i++) g_free (d->categories[i]);
  g_free (d->series);
  g_free (d->categories);
}

/* Reads the range: series down the columns, one point per row, which is
 * how a table of categories and years is usually laid out. */
/* The cells, read into series and points.  Which way round that is depends
 * on the orientation: with the series down the columns a point is a row,
 * and with the series along the rows a point is a column.  The two heading
 * flags stay attached to the range -- the top row and the left column --
 * so it is the orientation that decides which of them names the series. */
static void
chart_data_read (const O42Chart *chart, O42ChartFetch fetch, gpointer user,
                 ChartData *d)
{
  const O42Range *r = &chart->data;
  gboolean rows = chart->series_in_rows;
  int row0 = r->row0 + (chart->first_row_labels ? 1 : 0);
  int col0 = r->col0 + (chart->first_col_labels ? 1 : 0);
  gboolean series_named = rows ? chart->first_col_labels : chart->first_row_labels;
  gboolean points_named = rows ? chart->first_row_labels : chart->first_col_labels;

  memset (d, 0, sizeof *d);
  d->n_series = rows ? MAX (0, r->row1 - row0 + 1) : MAX (0, r->col1 - col0 + 1);
  d->n_points = rows ? MAX (0, r->col1 - col0 + 1) : MAX (0, r->row1 - row0 + 1);
  d->values = g_new (double, (gsize) d->n_series * d->n_points + 1);
  d->series = g_new0 (char *, (gsize) d->n_series + 1);
  d->categories = g_new0 (char *, (gsize) d->n_points + 1);
  d->min = 0;
  d->max = 0;

  for (int s = 0; s < d->n_series; s++)
    {
      if (series_named)
        {
          O42Value v;

          if (rows)
            fetch (user, row0 + s, r->col0, &v);
          else
            fetch (user, r->row0, col0 + s, &v);
          d->series[s] = o42_value_to_text (&v);
          o42_value_clear (&v);
        }
      else if (rows)
        d->series[s] = g_strdup_printf ("Series %d", row0 + s + 1);
      else
        {
          char name[8];
          o42_col_name (col0 + s, name, sizeof name);
          d->series[s] = g_strdup_printf ("Series %s", name);
        }

      for (int p = 0; p < d->n_points; p++)
        {
          O42Value v;
          double x = NAN;

          if (rows)
            fetch (user, row0 + s, col0 + p, &v);
          else
            fetch (user, row0 + p, col0 + s, &v);
          if (v.type == O42_VALUE_NUMBER)
            x = v.as.number;
          o42_value_clear (&v);

          d->values[s * d->n_points + p] = x;
          if (!isnan (x))
            {
              d->min = MIN (d->min, x);
              d->max = MAX (d->max, x);
            }
        }
    }

  for (int p = 0; p < d->n_points; p++)
    {
      if (points_named)
        {
          O42Value v;

          if (rows)
            fetch (user, r->row0, col0 + p, &v);
          else
            fetch (user, row0 + p, r->col0, &v);
          d->categories[p] = o42_value_to_text (&v);
          o42_value_clear (&v);
        }
      else
        d->categories[p] = g_strdup_printf ("%d", p + 1);
    }
}

static void
set_rgb (cairo_t *cr, guint32 colour)
{
  cairo_set_source_rgb (cr, ((colour >> 16) & 0xff) / 255.0,
                            ((colour >> 8) & 0xff) / 255.0,
                            (colour & 0xff) / 255.0);
}

static void
show_text (const O42Chart *chart, cairo_t *cr, PangoLayout *layout, const char *text,
           double x, double y, double halign, gboolean bold)
{
  /* The chart's own face and size when it has been given one, and
   * Excel's small sans-serif when it has not.  A title is a point
   * larger than the labels around the plot, as it is there. */
  const char *family = (chart->font_family != NULL && chart->font_family[0] != '\0')
                       ? chart->font_family : "Sans";
  double size = chart->font_size > 0 ? chart->font_size : 8;
  PangoFontDescription *desc = pango_font_description_new ();
  int w, h;

  /* Set piece by piece rather than parsed from a string, or a family
   * whose last word is a style word -- Times New Roman -- loses it. */
  pango_font_description_set_family (desc, family);
  pango_font_description_set_weight (desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_size (desc, (int) ((bold ? size + 1 : size) * PANGO_SCALE));

  pango_layout_set_font_description (layout, desc);
  pango_font_description_free (desc);
  pango_layout_set_text (layout, text, -1);
  pango_layout_get_pixel_size (layout, &w, &h);
  cairo_move_to (cr, x - w * halign, y);
  pango_cairo_show_layout (cr, layout);
}

/* A "nice" step for the value axis: 1, 2 or 5 times a power of ten, so
 * that there are about five gridlines. */
static double
nice_step (double range)
{
  double raw, mag, norm;

  if (range <= 0)
    return 1;
  raw = range / 5;
  mag = pow (10, floor (log10 (raw)));
  norm = raw / mag;
  if (norm < 1.5) return mag;
  if (norm < 3.5) return 2 * mag;
  if (norm < 7.5) return 5 * mag;
  return 10 * mag;
}

/* The value axis for a stacked chart runs to the tallest stack. */
static void
stacked_extent (const ChartData *d, double *min, double *max)
{
  *min = 0;
  *max = 0;
  for (int p = 0; p < d->n_points; p++)
    {
      double up = 0, down = 0;
      for (int s = 0; s < d->n_series; s++)
        {
          double v = d->values[s * d->n_points + p];
          if (isnan (v)) continue;
          if (v >= 0) up += v; else down += v;
        }
      *max = MAX (*max, up);
      *min = MIN (*min, down);
    }
}

/* A point's value written beside it. */
/* A number as the chart's own format spells it, General by default. */
static char *
chart_number (const O42Chart *chart, double v)
{
  if (chart != NULL && chart->y_format != NULL && *chart->y_format != '\0')
    {
      O42NumberFormat number = O42_NUM_GENERAL;
      int decimals = 2;
      if (o42_number_format_parse (chart->y_format, &number, &decimals))
        return o42_number_format (v, number, decimals);
      return o42_format_string (chart->y_format, v, NULL);
    }
  return o42_number_format (v, O42_NUM_GENERAL, 0);
}

static void
draw_value_label (const O42Chart *chart, cairo_t *cr, PangoLayout *layout, double v, double x, double y, gboolean above)
{
  char *text = chart_number (chart, v);
  cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
  show_text (chart, cr, layout, text, x, above ? y - 13 : y + 2, 0.5, FALSE);
  g_free (text);
}

/* The least-squares line through a series, as (slope, intercept) over
 * the point index; FALSE when there are too few points. */
/* ---- Markers ----------------------------------------------------------- */

static const char *MARKER_NAMES[] = {
  "auto", "none", "circle", "square", "diamond",
  "triangle", "cross", "plus", "star", "picture"
};

const char *
o42_marker_kind_name (O42MarkerKind kind)
{
  return (guint) kind < G_N_ELEMENTS (MARKER_NAMES) ? MARKER_NAMES[kind] : "auto";
}

gboolean
o42_marker_kind_parse (const char *name, O42MarkerKind *kind)
{
  for (guint i = 0; name != NULL && i < G_N_ELEMENTS (MARKER_NAMES); i++)
    if (g_ascii_strcasecmp (name, MARKER_NAMES[i]) == 0)
      { *kind = (O42MarkerKind) i; return TRUE; }
  return FALSE;
}

/* One point of a line, a scatter or a radar.  The colour is already
 * set; a picture marker ignores it. */
static void
draw_marker (const O42Chart *chart, cairo_t *cr, const ChartData *d,
             double x, double y)
{
  double r = (chart->marker_size > 0 ? chart->marker_size : 6) / 2;

  switch (chart->marker)
    {
    case O42_MARKER_NONE:
      return;

    case O42_MARKER_PICTURE:
      {
        cairo_surface_t *surface = d != NULL ? d->marker_picture : NULL;
        double w, h;

        if (surface == NULL)
          break;   /* no such picture: fall back on the plain dot */
        w = cairo_image_surface_get_width (surface);
        h = cairo_image_surface_get_height (surface);
        if (w <= 0 || h <= 0)
          break;
        cairo_save (cr);
        cairo_translate (cr, x - r * 2, y - r * 2);
        cairo_scale (cr, r * 4 / w, r * 4 / h);
        cairo_set_source_surface (cr, surface, 0, 0);
        cairo_paint (cr);
        cairo_restore (cr);
        return;
      }

    case O42_MARKER_SQUARE:
      cairo_rectangle (cr, x - r, y - r, r * 2, r * 2);
      cairo_fill (cr);
      return;

    case O42_MARKER_DIAMOND:
      cairo_move_to (cr, x, y - r);
      cairo_line_to (cr, x + r, y);
      cairo_line_to (cr, x, y + r);
      cairo_line_to (cr, x - r, y);
      cairo_close_path (cr);
      cairo_fill (cr);
      return;

    case O42_MARKER_TRIANGLE:
      cairo_move_to (cr, x, y - r);
      cairo_line_to (cr, x + r, y + r);
      cairo_line_to (cr, x - r, y + r);
      cairo_close_path (cr);
      cairo_fill (cr);
      return;

    case O42_MARKER_CROSS:
    case O42_MARKER_PLUS:
    case O42_MARKER_STAR:
      {
        double width_was = cairo_get_line_width (cr);

        cairo_set_line_width (cr, MAX (r / 2.5, 1));
        if (chart->marker != O42_MARKER_PLUS)
          {
            cairo_move_to (cr, x - r, y - r); cairo_line_to (cr, x + r, y + r);
            cairo_move_to (cr, x + r, y - r); cairo_line_to (cr, x - r, y + r);
          }
        if (chart->marker != O42_MARKER_CROSS)
          {
            cairo_move_to (cr, x - r, y); cairo_line_to (cr, x + r, y);
            cairo_move_to (cr, x, y - r); cairo_line_to (cr, x, y + r);
          }
        cairo_stroke (cr);
        cairo_set_line_width (cr, width_was);
        return;
      }

    case O42_MARKER_CIRCLE:
      {
        double width_was = cairo_get_line_width (cr);

        cairo_set_line_width (cr, MAX (r / 3, 1));
        cairo_new_sub_path (cr);
        cairo_arc (cr, x, y, r, 0, 2 * G_PI);
        cairo_stroke (cr);
        cairo_set_line_width (cr, width_was);
        return;
      }

    default:
      break;
    }

  cairo_new_sub_path (cr);
  cairo_arc (cr, x, y, r, 0, 2 * G_PI);
  cairo_fill (cr);
}

/* A bar or a column filled with a picture, stretched to it as Excel's
 * picture fill does.  FALSE when there is no picture to fill with, and
 * the caller paints the colour it would have. */
static gboolean
fill_with_picture (const O42Chart *chart, cairo_t *cr, const ChartData *d,
                   double x, double y, double w, double h)
{
  cairo_surface_t *surface;
  double sw, sh;

  if (chart->marker != O42_MARKER_PICTURE || w <= 0 || h <= 0)
    return FALSE;
  surface = d != NULL ? d->marker_picture : NULL;
  if (surface == NULL)
    return FALSE;
  sw = cairo_image_surface_get_width (surface);
  sh = cairo_image_surface_get_height (surface);
  if (sw <= 0 || sh <= 0)
    return FALSE;

  cairo_save (cr);
  cairo_rectangle (cr, x, y, w, h);
  cairo_clip (cr);
  cairo_translate (cr, x, y);
  cairo_scale (cr, w / sw, h / sh);
  cairo_set_source_surface (cr, surface, 0, 0);
  cairo_paint (cr);
  cairo_restore (cr);
  return TRUE;
}

/* How far back a 3-D chart's solids go, and how far up: the isometric
 * offset every face is drawn from.  Excel's 3-D charts turn the plot
 * on two axes; this is the simple version of the same picture. */
#define DEPTH_X 9.0
#define DEPTH_Y 7.0

/* A colour lightened or darkened, for the faces that catch less light. */
static guint32
shaded (guint32 colour, double factor)
{
  int r = CLAMP ((int) (((colour >> 16) & 0xFF) * factor), 0, 255);
  int g = CLAMP ((int) (((colour >> 8) & 0xFF) * factor), 0, 255);
  int b = CLAMP ((int) ((colour & 0xFF) * factor), 0, 255);

  return (guint32) ((r << 16) | (g << 8) | b);
}

/* A box drawn as a face with a top and a side behind it. */
static void
draw_solid (cairo_t *cr, double x, double y, double w, double h, guint32 colour)
{
  if (w <= 0)
    w = 1;

  /* The top face. */
  set_rgb (cr, shaded (colour, 1.35));
  cairo_move_to (cr, x, y);
  cairo_line_to (cr, x + DEPTH_X, y - DEPTH_Y);
  cairo_line_to (cr, x + w + DEPTH_X, y - DEPTH_Y);
  cairo_line_to (cr, x + w, y);
  cairo_close_path (cr);
  cairo_fill (cr);

  /* The side. */
  set_rgb (cr, shaded (colour, 0.7));
  cairo_move_to (cr, x + w, y);
  cairo_line_to (cr, x + w + DEPTH_X, y - DEPTH_Y);
  cairo_line_to (cr, x + w + DEPTH_X, y + h - DEPTH_Y);
  cairo_line_to (cr, x + w, y + h);
  cairo_close_path (cr);
  cairo_fill (cr);

  /* The front, last, so that it is whole. */
  set_rgb (cr, colour);
  cairo_rectangle (cr, x, y, w, h);
  cairo_fill (cr);
}

/* ---------------------------------------------------------------------- */
/* Fitting a curve                                                         */
/* ---------------------------------------------------------------------- */

const char *
o42_errbar_kind_name (O42ErrBarKind kind)
{
  switch (kind)
    {
    case O42_ERRBAR_FIXED:   return "fixed";
    case O42_ERRBAR_PERCENT: return "percent";
    case O42_ERRBAR_STDDEV:  return "stddev";
    case O42_ERRBAR_STDERR:  return "stderr";
    default:                 return "none";
    }
}

gboolean
o42_errbar_kind_parse (const char *name, O42ErrBarKind *kind)
{
  static const char *names[] = { "none", "fixed", "percent", "stddev", "stderr" };

  if (name == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (g_ascii_strcasecmp (name, names[i]) == 0)
      {
        *kind = (O42ErrBarKind) i;
        return TRUE;
      }
  /* The names .xlsx writes for the same four. */
  if (g_ascii_strcasecmp (name, "fixedVal") == 0)   { *kind = O42_ERRBAR_FIXED; return TRUE; }
  if (g_ascii_strcasecmp (name, "percentage") == 0) { *kind = O42_ERRBAR_PERCENT; return TRUE; }
  if (g_ascii_strcasecmp (name, "stdDev") == 0)     { *kind = O42_ERRBAR_STDDEV; return TRUE; }
  if (g_ascii_strcasecmp (name, "stdErr") == 0)     { *kind = O42_ERRBAR_STDERR; return TRUE; }
  return FALSE;
}

const char *
o42_trend_kind_name (O42TrendKind kind)
{
  switch (kind)
    {
    case O42_TREND_LINEAR: return "linear";
    case O42_TREND_POLY:   return "poly";
    case O42_TREND_EXP:    return "exp";
    case O42_TREND_LOG:    return "log";
    case O42_TREND_POWER:  return "power";
    case O42_TREND_MOVING: return "average";
    default:               return "none";
    }
}

gboolean
o42_trend_kind_parse (const char *name, O42TrendKind *kind)
{
  static const char *names[] = { "none", "linear", "poly", "exp", "log", "power", "average" };

  if (name == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (g_ascii_strcasecmp (name, names[i]) == 0)
      {
        *kind = (O42TrendKind) i;
        return TRUE;
      }
  /* The names other people write for the same curves. */
  if (g_ascii_strcasecmp (name, "movingAvg") == 0 || g_ascii_strcasecmp (name, "moving") == 0)
    { *kind = O42_TREND_MOVING; return TRUE; }
  if (g_ascii_strcasecmp (name, "logarithmic") == 0)
    { *kind = O42_TREND_LOG; return TRUE; }
  if (g_ascii_strcasecmp (name, "polynomial") == 0)
    { *kind = O42_TREND_POLY; return TRUE; }
  if (g_ascii_strcasecmp (name, "exponential") == 0)
    { *kind = O42_TREND_EXP; return TRUE; }
  return FALSE;
}

/* Least squares for a polynomial of `order` in x: the normal equations
 * solved by Gaussian elimination with partial pivoting.  Orders here
 * are at most six, as Excel's are, so the plain method is honest
 * enough and the matrix is never bigger than seven by seven. */
static gboolean
poly_fit (const double *x, const double *y, int n, int order, double *coef)
{
  int m = order + 1;
  double a[O42_TREND_MAX_COEF][O42_TREND_MAX_COEF + 1];
  int used = 0;

  if (m > O42_TREND_MAX_COEF)
    return FALSE;
  for (int i = 0; i < m; i++)
    for (int j = 0; j <= m; j++)
      a[i][j] = 0;
  for (int p = 0; p < n; p++)
    {
      if (isnan (x[p]) || isnan (y[p]))
        continue;
      used++;
      /* a[i][j] gathers sum x^(i+j), and a[i][m] sum x^i y. */
      for (int i = 0; i < m; i++)
        {
          double xi = pow (x[p], i);

          for (int j = 0; j < m; j++)
            a[i][j] += xi * pow (x[p], j);
          a[i][m] += xi * y[p];
        }
    }
  if (used <= order)
    return FALSE;

  for (int i = 0; i < m; i++)
    {
      int pivot = i;

      for (int r = i + 1; r < m; r++)
        if (fabs (a[r][i]) > fabs (a[pivot][i]))
          pivot = r;
      if (fabs (a[pivot][i]) < 1e-12)
        return FALSE;
      if (pivot != i)
        for (int j = 0; j <= m; j++)
          {
            double t = a[i][j]; a[i][j] = a[pivot][j]; a[pivot][j] = t;
          }
      for (int r = 0; r < m; r++)
        {
          double f;

          if (r == i)
            continue;
          f = a[r][i] / a[i][i];
          for (int j = i; j <= m; j++)
            a[r][j] -= f * a[i][j];
        }
    }
  for (int i = 0; i < m; i++)
    coef[i] = a[i][m] / a[i][i];
  return TRUE;
}

gboolean
o42_trend_fit (O42TrendKind kind, int order, const double *x, const double *y,
               int n, double *coef)
{
  double *tx, *ty;
  gboolean ok;

  if (kind == O42_TREND_NONE || kind == O42_TREND_MOVING || n < 2)
    return FALSE;
  if (kind == O42_TREND_LINEAR)
    return poly_fit (x, y, n, 1, coef);
  if (kind == O42_TREND_POLY)
    return poly_fit (x, y, n, CLAMP (order, 2, 6), coef);

  /* The other three are straight lines once the axes are logged; a
   * point that cannot be logged is dropped, as Excel drops it. */
  tx = g_new (double, n);
  ty = g_new (double, n);
  for (int p = 0; p < n; p++)
    {
      double xv = x[p], yv = y[p];

      if (kind == O42_TREND_LOG || kind == O42_TREND_POWER)
        xv = xv > 0 ? log (xv) : NAN;
      if (kind == O42_TREND_EXP || kind == O42_TREND_POWER)
        yv = yv > 0 ? log (yv) : NAN;
      tx[p] = xv;
      ty[p] = yv;
    }
  ok = poly_fit (tx, ty, n, 1, coef);
  g_free (tx);
  g_free (ty);
  if (!ok)
    return FALSE;
  /* coef[0] is the intercept of the logged fit; the curves want it
   * back the way it was written: y = a e^(bx), y = a x^b. */
  if (kind == O42_TREND_EXP || kind == O42_TREND_POWER)
    coef[0] = exp (coef[0]);
  return TRUE;
}

double
o42_trend_eval (O42TrendKind kind, int order, const double *coef, double x)
{
  switch (kind)
    {
    case O42_TREND_LINEAR: return coef[0] + coef[1] * x;
    case O42_TREND_POLY:
      {
        double v = 0;

        for (int i = CLAMP (order, 2, 6); i >= 0; i--)
          v = v * x + coef[i];
        return v;
      }
    case O42_TREND_EXP:   return coef[0] * exp (coef[1] * x);
    case O42_TREND_LOG:   return x > 0 ? coef[0] + coef[1] * log (x) : NAN;
    case O42_TREND_POWER: return x > 0 ? coef[0] * pow (x, coef[1]) : NAN;
    default:              return NAN;
    }
}

/* Half the length of the error bar at a point: what a fixed value, a
 * percentage, a multiple of the series' standard deviation, or the
 * standard error of its mean comes to.  The last two are the same for
 * every point of the series, as they are in Excel.  NAN for none. */
static double
error_length (const O42Chart *chart, const double *ys, int n, double y)
{
  double sum = 0, sq = 0, mean, var;
  int used = 0;

  switch (chart->err_bars)
    {
    case O42_ERRBAR_FIXED:
      return fabs (chart->err_value);
    case O42_ERRBAR_PERCENT:
      return fabs (y) * fabs (chart->err_value) / 100;
    case O42_ERRBAR_STDDEV:
    case O42_ERRBAR_STDERR:
      break;
    default:
      return NAN;
    }

  for (int p = 0; p < n; p++)
    if (!isnan (ys[p]))
      { sum += ys[p]; used++; }
  if (used < 2)
    return NAN;
  mean = sum / used;
  for (int p = 0; p < n; p++)
    if (!isnan (ys[p]))
      sq += (ys[p] - mean) * (ys[p] - mean);
  var = sq / (used - 1);          /* the sample standard deviation */
  if (chart->err_bars == O42_ERRBAR_STDERR)
    return sqrt (var / used);
  return sqrt (var) * fabs (chart->err_value);
}

/* An I bar from `y0` to `y1` at `x`, in pixels. */
static void
draw_error_bar (cairo_t *cr, double x, double y0, double y1, double top, double bottom)
{
  double cap = 3;

  if (isnan (y0) || isnan (y1))
    return;
  y0 = CLAMP (y0, top, bottom);
  y1 = CLAMP (y1, top, bottom);
  cairo_move_to (cr, floor (x) + 0.5, y0);
  cairo_line_to (cr, floor (x) + 0.5, y1);
  cairo_move_to (cr, floor (x) + 0.5 - cap, y0);
  cairo_line_to (cr, floor (x) + 0.5 + cap, y0);
  cairo_move_to (cr, floor (x) + 0.5 - cap, y1);
  cairo_line_to (cr, floor (x) + 0.5 + cap, y1);
  cairo_stroke (cr);
}

/* The points of a chart's trendline through (xs, ys), in the chart's
 * own units: a fitted curve is sampled evenly across the data's span,
 * a moving average has one point per data point.  Returns how many
 * were written, at most `max`; a y of NAN is a gap. */
#define TREND_SAMPLES 64

static int
trend_points (const O42Chart *chart, const double *xs, const double *ys, int n,
              double *out_x, double *out_y, int max)
{
  double coef[O42_TREND_MAX_COEF];
  double x0 = NAN, x1 = NAN;
  int count = 0;

  if (chart->trend == O42_TREND_NONE || n < 2)
    return 0;

  if (chart->trend == O42_TREND_MOVING)
    {
      int period = CLAMP (chart->trend_order, 2, n);

      for (int p = 0; p < n && count < max; p++)
        {
          double sum = 0;
          int used = 0;

          for (int q = p - period + 1; q <= p; q++)
            if (q >= 0 && !isnan (ys[q]))
              { sum += ys[q]; used++; }
          out_x[count] = xs[p];
          out_y[count] = (p >= period - 1 && used == period) ? sum / period : NAN;
          count++;
        }
      return count;
    }

  if (!o42_trend_fit (chart->trend, chart->trend_order, xs, ys, n, coef))
    return 0;
  for (int p = 0; p < n; p++)
    {
      if (isnan (xs[p]) || isnan (ys[p]))
        continue;
      if (isnan (x0) || xs[p] < x0) x0 = xs[p];
      if (isnan (x1) || xs[p] > x1) x1 = xs[p];
    }
  if (isnan (x0) || x1 <= x0)
    return 0;
  /* A line needs its two ends and no more; a curve is drawn as a
   * polyline fine enough that the joins do not show. */
  if (chart->trend == O42_TREND_LINEAR)
    max = MIN (max, 2);
  else
    max = MIN (max, TREND_SAMPLES);
  for (int i = 0; i < max; i++)
    {
      double x = x0 + (x1 - x0) * i / (double) (max - 1);

      out_x[count] = x;
      out_y[count] = o42_trend_eval (chart->trend, chart->trend_order, coef, x);
      count++;
    }
  return count;
}

static void
draw_axes_chart (const O42Chart *chart, cairo_t *cr, PangoLayout *layout,
                 const ChartData *d, double x0, double y0, double w, double h)
{
  double dmin = d->min, dmax = d->max;
  double step, lo, hi;
  double smin = 0, smax = 0, slo = 0, shi = 1;
  gboolean secondary = FALSE;

  if (chart->kind == O42_CHART_STACKED)
    stacked_extent (d, &dmin, &dmax);
  else if (chart->kind == O42_CHART_PERCENT)
    { dmin = 0; dmax = 100; }

  /* Series from secondary_from on are drawn against their own scale,
   * whose labels run down the right. */
  if (chart->secondary_from > 0 && chart->secondary_from <= d->n_series)
    {
      dmin = dmax = 0;
      for (int s = 0; s < chart->secondary_from - 1; s++)
        for (int p = 0; p < d->n_points; p++)
          {
            double v = d->values[s * d->n_points + p];
            if (isnan (v)) continue;
            dmin = MIN (dmin, v);
            dmax = MAX (dmax, v);
          }
      for (int s = chart->secondary_from - 1; s < d->n_series; s++)
        for (int p = 0; p < d->n_points; p++)
          {
            double v = d->values[s * d->n_points + p];
            if (isnan (v)) continue;
            smin = MIN (smin, v);
            smax = MAX (smax, v);
          }
      {
        double sstep = nice_step (smax - smin);
        slo = floor (smin / sstep) * sstep;
        shi = ceil (smax / sstep) * sstep;
        if (shi <= slo) shi = slo + sstep;
        secondary = TRUE;
      }
    }
  if (chart->has_min) dmin = MIN (chart->min, dmin);
  if (chart->has_max) dmax = MAX (chart->max, dmax);
  if (chart->has_min && chart->has_max && chart->max > chart->min)
    { dmin = chart->min; dmax = chart->max; }
  step = nice_step (dmax - dmin);
  lo = chart->has_min ? chart->min : floor (dmin / step) * step;
  hi = chart->has_max ? chart->max : ceil (dmax / step) * step;
  double left = x0 + 44, right = x0 + w - 8;
  double top = y0 + 8, bottom = y0 + h - 22;
  double plot_w = right - left, plot_h = bottom - top;

  if (hi <= lo)
    hi = lo + step;

  #define VY(v) (bottom - ((v) - lo) / (hi - lo) * plot_h)

  /* Gridlines and the value axis. */
  cairo_set_line_width (cr, 1);
  for (double v = lo; v <= hi + step / 2; v += step)
    {
      char *label = chart_number (chart, v);

      if (chart->gridlines)
        {
          cairo_set_source_rgb (cr, 0.85, 0.85, 0.85);
          cairo_move_to (cr, left, floor (VY (v)) + 0.5);
          cairo_line_to (cr, right, floor (VY (v)) + 0.5);
          cairo_stroke (cr);
        }

      cairo_set_source_rgb (cr, 0, 0, 0);
      show_text (chart, cr, layout, label, left - 4, VY (v) - 7, 1.0, FALSE);
      g_free (label);
    }

  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_move_to (cr, floor (left) + 0.5, top);
  cairo_line_to (cr, floor (left) + 0.5, bottom);
  cairo_move_to (cr, left, floor (VY (0)) + 0.5);
  cairo_line_to (cr, right, floor (VY (0)) + 0.5);
  cairo_stroke (cr);

  #define VY2(v) (bottom - ((v) - slo) / (shi - slo) * plot_h)
  #define VYS(s, v) ((secondary && (s) >= chart->secondary_from - 1) ? VY2 (v) : VY (v))

  if (secondary)
    {
      /* The second scale's labels and axis, down the right. */
      double sstep = nice_step (shi - slo);
      cairo_set_source_rgb (cr, 0, 0, 0);
      for (double v = slo; v <= shi + sstep / 2; v += sstep)
        {
          /* The chart's number format belongs to the first axis; the
           * second one is on its own scale and keeps General. */
          char *label = o42_number_format (v, O42_NUM_GENERAL, 0);
          show_text (chart, cr, layout, label, right + 4, VY2 (v) - 7, 0.0, FALSE);
          g_free (label);
        }
      cairo_move_to (cr, floor (right) + 0.5, top);
      cairo_line_to (cr, floor (right) + 0.5, bottom);
      cairo_stroke (cr);
    }

  if (d->n_points == 0 || d->n_series == 0)
    return;

  {
    double slot = plot_w / d->n_points;

    /* Category labels, every one if they fit and every other otherwise. */
    for (int p = 0; p < d->n_points; p++)
      {
        int every = (slot < 40) ? 2 : 1;
        if (p % every == 0)
          show_text (chart, cr, layout, d->categories[p], left + slot * (p + 0.5), bottom + 3, 0.5, FALSE);
      }

    if (chart->kind == O42_CHART_STACKED || chart->kind == O42_CHART_PERCENT)
      {
        double gap = slot * 0.3;

        for (int p = 0; p < d->n_points; p++)
          {
            double up = 0, down = 0, total = 0;

            if (chart->kind == O42_CHART_PERCENT)
              for (int s = 0; s < d->n_series; s++)
                {
                  double v = d->values[s * d->n_points + p];
                  if (!isnan (v)) total += fabs (v);
                }

            for (int s = 0; s < d->n_series; s++)
              {
                double v = d->values[s * d->n_points + p];
                double from, to;

                if (isnan (v))
                  continue;
                if (chart->kind == O42_CHART_PERCENT)
                  v = (total > 0) ? 100 * fabs (v) / total : 0;

                if (v >= 0) { from = up; up += v; to = up; }
                else        { from = down; down += v; to = down; }

                if (chart->three_d)
                  draw_solid (cr, left + slot * p + gap / 2, VY (MAX (from, to)),
                              slot - gap, fabs (VY (to) - VY (from)),
                              SERIES_COLOURS[s % G_N_ELEMENTS (SERIES_COLOURS)]);
                else
                  {
                    set_rgb (cr, SERIES_COLOURS[s % G_N_ELEMENTS (SERIES_COLOURS)]);
                    cairo_rectangle (cr, left + slot * p + gap / 2, VY (MAX (from, to)),
                                     slot - gap, fabs (VY (to) - VY (from)));
                    cairo_fill (cr);
                  }
                if (chart->data_labels && fabs (VY (to) - VY (from)) > 12)
                  draw_value_label (chart, cr, layout, v, left + slot * (p + 0.5),
                                    (VY (from) + VY (to)) / 2 + 6, FALSE);
              }
          }
      }
    else if (chart->kind == O42_CHART_COLUMN)
      {
        double gap = slot * 0.2;
        double bar = (slot - gap) / d->n_series;

        for (int s = 0; s < d->n_series; s++)
          {
            set_rgb (cr, SERIES_COLOURS[s % G_N_ELEMENTS (SERIES_COLOURS)]);
            for (int p = 0; p < d->n_points; p++)
              {
                double v = d->values[s * d->n_points + p];
                double bx, by, bh;

                if (isnan (v))
                  continue;
                bx = left + slot * p + gap / 2 + bar * s;
                by = VYS (s, MAX (v, 0));
                bh = fabs (VYS (s, v) - VYS (s, 0));
                if (chart->three_d)
                  draw_solid (cr, bx, by, MAX (bar - 1, 1), bh,
                              SERIES_COLOURS[s % G_N_ELEMENTS (SERIES_COLOURS)]);
                else if (!fill_with_picture (chart, cr, d, bx, by, MAX (bar - 1, 1), bh))
                  {
                    cairo_rectangle (cr, bx, by, MAX (bar - 1, 1), bh);
                    cairo_fill (cr);
                  }
              }
          }
        if (chart->data_labels)
          for (int s = 0; s < d->n_series; s++)
            for (int p = 0; p < d->n_points; p++)
              {
                double v = d->values[s * d->n_points + p];
                if (isnan (v)) continue;
                draw_value_label (chart, cr, layout, v, left + slot * p + gap / 2 + bar * s + bar / 2,
                                  VY (MAX (v, 0)), v >= 0);
              }
      }
    else
      {
        cairo_set_line_width (cr, 2);
        for (int s = 0; s < d->n_series; s++)
          {
            gboolean started = FALSE;
            double first_x = 0, last_x = 0;

            set_rgb (cr, SERIES_COLOURS[s % G_N_ELEMENTS (SERIES_COLOURS)]);
            for (int p = 0; p < d->n_points; p++)
              {
                double v = d->values[s * d->n_points + p];
                double px = left + slot * (p + 0.5);

                if (isnan (v)) { started = FALSE; continue; }
                if (started) cairo_line_to (cr, px, VYS (s, v));
                else         { cairo_move_to (cr, px, VYS (s, v)); first_x = px; }
                last_x = px;
                started = TRUE;
              }

            /* An area chart fills down to the axis, translucently so
             * the series behind still show. */
            if (chart->kind == O42_CHART_AREA && started)
              {
                guint32 c = SERIES_COLOURS[s % G_N_ELEMENTS (SERIES_COLOURS)];
                cairo_line_to (cr, last_x, VYS (s, 0));
                cairo_line_to (cr, first_x, VYS (s, 0));
                cairo_close_path (cr);
                cairo_set_source_rgba (cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0,
                                       (c & 0xff) / 255.0, 0.45);
                cairo_fill (cr);
                continue;
              }
            cairo_stroke (cr);

            for (int p = 0; p < d->n_points; p++)
              {
                double v = d->values[s * d->n_points + p];
                if (isnan (v)) continue;
                draw_marker (chart, cr, d, left + slot * (p + 0.5), VYS (s, v));
              }
          }
        cairo_set_line_width (cr, 1);
      }
  }
    /* Data labels and trendlines, over everything already drawn. */
  {
    double slot2 = plot_w / MAX (d->n_points, 1);

    if (chart->data_labels && chart->kind != O42_CHART_COLUMN &&
        chart->kind != O42_CHART_STACKED && chart->kind != O42_CHART_PERCENT)
      for (int s = 0; s < d->n_series; s++)
        for (int p = 0; p < d->n_points; p++)
          {
            double v = d->values[s * d->n_points + p];
            if (isnan (v)) continue;
            draw_value_label (chart, cr, layout, v, left + slot2 * (p + 0.5), VY (v), TRUE);
          }

    if (chart->err_bars != O42_ERRBAR_NONE && chart->kind != O42_CHART_PERCENT &&
        chart->kind != O42_CHART_STACKED)
      {
        /* At the middle of the point: the bar's own middle in a column
         * chart, the marker's place in a line or area chart. */
        double gap = slot2 * 0.2;
        double bar = (slot2 - gap) / MAX (d->n_series, 1);

        cairo_save (cr);
        cairo_set_line_width (cr, 1);
        for (int s = 0; s < d->n_series; s++)
          {
            const double *ys = d->values + (gsize) s * d->n_points;

            set_rgb (cr, SERIES_COLOURS[s % G_N_ELEMENTS (SERIES_COLOURS)]);
            for (int p = 0; p < d->n_points; p++)
              {
                double v = ys[p];
                double e = error_length (chart, ys, d->n_points, v);
                double px = chart->kind == O42_CHART_COLUMN
                            ? left + slot2 * p + gap / 2 + bar * s + bar / 2
                            : left + slot2 * (p + 0.5);

                if (isnan (v) || isnan (e))
                  continue;
                draw_error_bar (cr, px, VYS (s, v - e), VYS (s, v + e), top, bottom);
              }
          }
        cairo_restore (cr);
      }

    if (chart->trend != O42_TREND_NONE && chart->kind != O42_CHART_PERCENT)
      {
        static const double dashes[] = { 5, 4 };
        double *xs = g_new (double, d->n_points);
        double *cx = g_new (double, TREND_SAMPLES);
        double *cy = g_new (double, TREND_SAMPLES);

        /* The categories count from one, as they do in Excel, so that
         * a logarithmic or a power curve has something to take the
         * logarithm of at the first point. */
        for (int p = 0; p < d->n_points; p++)
          xs[p] = p + 1;
        cairo_save (cr);
        cairo_set_dash (cr, dashes, 2, 0);
        cairo_set_line_width (cr, 1.5);
        for (int s = 0; s < d->n_series; s++)
          {
            int count = trend_points (chart, xs, d->values + (gsize) s * d->n_points,
                                      d->n_points, cx, cy, TREND_SAMPLES);
            gboolean down = FALSE;

            set_rgb (cr, SERIES_COLOURS[s % G_N_ELEMENTS (SERIES_COLOURS)]);
            for (int i = 0; i < count; i++)
              {
                double px, py;

                if (isnan (cy[i]))
                  { down = FALSE; continue; }
                px = left + slot2 * (cx[i] - 0.5);
                py = CLAMP (VY (cy[i]), top, bottom);
                if (down) cairo_line_to (cr, px, py);
                else cairo_move_to (cr, px, py);
                down = TRUE;
              }
            cairo_stroke (cr);
          }
        cairo_restore (cr);
        g_free (xs);
        g_free (cx);
        g_free (cy);
      }
  }

  #undef VY
}

/* A bubble chart: the first series is x, the second y, and the third
 * how big each bubble is, which is Excel's arrangement. */
static void
draw_bubble (const O42Chart *chart, cairo_t *cr, PangoLayout *layout, const ChartData *d,
             double x0, double y0, double w, double h)
{
  double left = x0 + 44, right = x0 + w - 8;
  double top = y0 + 8, bottom = y0 + h - 22;
  double xlo = NAN, xhi = NAN, ylo = NAN, yhi = NAN, biggest = 0;

  if (d->n_series < 2 || d->n_points == 0)
    return;

  for (int p = 0; p < d->n_points; p++)
    {
      double x = d->values[p], y = d->values[d->n_points + p];
      double size = d->n_series > 2 ? d->values[2 * d->n_points + p] : 1;

      if (isnan (x) || isnan (y))
        continue;
      if (isnan (xlo) || x < xlo) xlo = x;
      if (isnan (xhi) || x > xhi) xhi = x;
      if (isnan (ylo) || y < ylo) ylo = y;
      if (isnan (yhi) || y > yhi) yhi = y;
      if (!isnan (size) && fabs (size) > biggest) biggest = fabs (size);
    }
  if (isnan (xlo) || xhi <= xlo) { xlo = xlo - 1; xhi = xlo + 2; }
  if (isnan (ylo) || yhi <= ylo) { ylo = ylo - 1; yhi = ylo + 2; }
  if (biggest <= 0) biggest = 1;

  /* Room for the biggest bubble, so that none of them is cut off by
   * the edge of the plot. */
  #define BUBBLE_PAD 28.0
  #define BX(v) (left + BUBBLE_PAD + ((v) - xlo) / (xhi - xlo) *                  MAX (right - left - 2 * BUBBLE_PAD, 1))
  #define BY(v) (bottom - BUBBLE_PAD - ((v) - ylo) / (yhi - ylo) *                  MAX (bottom - top - 2 * BUBBLE_PAD, 1))

  /* The axes, with a value at each end. */
  cairo_set_line_width (cr, 1);
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_move_to (cr, floor (left) + 0.5, top);
  cairo_line_to (cr, floor (left) + 0.5, floor (bottom) + 0.5);
  cairo_line_to (cr, right, floor (bottom) + 0.5);
  cairo_stroke (cr);
  {
    char *text = o42_number_format (ylo, O42_NUM_GENERAL, 0);

    show_text (chart, cr, layout, text, left - 4, BY (ylo) - 7, 1.0, FALSE);
    g_free (text);
    text = o42_number_format (yhi, O42_NUM_GENERAL, 0);
    show_text (chart, cr, layout, text, left - 4, BY (yhi) - 7, 1.0, FALSE);
    g_free (text);
    text = o42_number_format (xlo, O42_NUM_GENERAL, 0);
    show_text (chart, cr, layout, text, BX (xlo), bottom + 3, 0.5, FALSE);
    g_free (text);
    text = o42_number_format (xhi, O42_NUM_GENERAL, 0);
    show_text (chart, cr, layout, text, BX (xhi), bottom + 3, 0.5, FALSE);
    g_free (text);
  }

  for (int p = 0; p < d->n_points; p++)
    {
      double x = d->values[p], y = d->values[d->n_points + p];
      double size = d->n_series > 2 ? d->values[2 * d->n_points + p] : biggest;
      double r;

      if (isnan (x) || isnan (y) || isnan (size))
        continue;
      /* The area of a bubble follows the value, as Excel's does, so
       * the radius follows its square root. */
      r = 4 + 22 * sqrt (fabs (size) / biggest);
      set_rgb (cr, SERIES_COLOURS[p % G_N_ELEMENTS (SERIES_COLOURS)]);
      cairo_new_path (cr);
      cairo_arc (cr, BX (x), BY (y), r, 0, 2 * G_PI);
      cairo_fill_preserve (cr);
      cairo_set_source_rgb (cr, 1, 1, 1);
      cairo_set_line_width (cr, 1);
      cairo_stroke (cr);
      if (chart->data_labels)
        draw_value_label (chart, cr, layout, size, BX (x), BY (y) - r, TRUE);
    }

  #undef BX
  #undef BY
  #undef BUBBLE_PAD
}

/* A radar chart: a spoke for every category, and a closed polygon for
 * every series, which is how Excel draws one. */
static void
draw_radar (const O42Chart *chart, cairo_t *cr, PangoLayout *layout, const ChartData *d,
            double x0, double y0, double w, double h)
{
  double cx = x0 + w / 2, cy = y0 + h / 2;
  double radius = MIN (w, h) / 2 - 24;
  double top = d->max > 0 ? d->max : 1;
  double step;

  if (d->n_points < 3 || radius <= 8)
    return;
  step = nice_step (top);
  top = ceil (top / step) * step;

  /* The web: the rings and the spokes. */
  cairo_set_line_width (cr, 1);
  cairo_set_source_rgb (cr, 0.8, 0.8, 0.8);
  for (double v = step; v <= top + step / 2; v += step)
    {
      double r = radius * v / top;

      for (int p = 0; p <= d->n_points; p++)
        {
          double at = -G_PI / 2 + 2 * G_PI * (p % d->n_points) / d->n_points;
          double px = cx + cos (at) * r, py = cy + sin (at) * r;

          if (p == 0) cairo_move_to (cr, px, py);
          else cairo_line_to (cr, px, py);
        }
      cairo_stroke (cr);
    }
  for (int p = 0; p < d->n_points; p++)
    {
      double at = -G_PI / 2 + 2 * G_PI * p / d->n_points;

      cairo_move_to (cr, cx, cy);
      cairo_line_to (cr, cx + cos (at) * radius, cy + sin (at) * radius);
    }
  cairo_stroke (cr);

  /* The categories, at the end of each spoke. */
  cairo_set_source_rgb (cr, 0, 0, 0);
  for (int p = 0; p < d->n_points && d->categories != NULL; p++)
    {
      double at = -G_PI / 2 + 2 * G_PI * p / d->n_points;
      const char *label = d->categories[p];

      if (label == NULL || *label == '\0')
        continue;
      show_text (chart, cr, layout, label,
                 cx + cos (at) * (radius + 12), cy + sin (at) * (radius + 12) - 7, 0.5, FALSE);
    }

  /* A polygon per series. */
  cairo_set_line_width (cr, 2);
  for (int series = 0; series < d->n_series; series++)
    {
      gboolean started = FALSE;

      set_rgb (cr, SERIES_COLOURS[series % G_N_ELEMENTS (SERIES_COLOURS)]);
      for (int p = 0; p <= d->n_points; p++)
        {
          int index = p % d->n_points;
          double v = d->values[series * d->n_points + index];
          double at = -G_PI / 2 + 2 * G_PI * index / d->n_points;
          double r = radius * CLAMP (v, 0, top) / top;

          if (isnan (v))
            continue;
          if (!started) { cairo_move_to (cr, cx + cos (at) * r, cy + sin (at) * r); started = TRUE; }
          else cairo_line_to (cr, cx + cos (at) * r, cy + sin (at) * r);
        }
      if (started)
        {
          cairo_close_path (cr);
          cairo_stroke (cr);
        }
    }
}

/* Bars lie along the categories, which run down the left. */
static void
draw_bar_chart (const O42Chart *chart, cairo_t *cr, PangoLayout *layout, const ChartData *d,
                double x0, double y0, double w, double h)
{
  double step = nice_step (d->max - d->min);
  double lo = floor (d->min / step) * step;
  double hi = ceil (d->max / step) * step;
  double left = x0 + 60, right = x0 + w - 8;
  double top = y0 + 8, bottom = y0 + h - 22;
  double plot_w = right - left, plot_h = bottom - top;

  if (hi <= lo)
    hi = lo + step;

  #define VX(v) (left + ((v) - lo) / (hi - lo) * plot_w)

  cairo_set_line_width (cr, 1);
  for (double v = lo; v <= hi + step / 2; v += step)
    {
      char *label = o42_number_format (v, O42_NUM_GENERAL, 0);

      cairo_set_source_rgb (cr, 0.85, 0.85, 0.85);
      cairo_move_to (cr, floor (VX (v)) + 0.5, top);
      cairo_line_to (cr, floor (VX (v)) + 0.5, bottom);
      cairo_stroke (cr);
      cairo_set_source_rgb (cr, 0, 0, 0);
      show_text (chart, cr, layout, label, VX (v), bottom + 3, 0.5, FALSE);
      g_free (label);
    }

  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_move_to (cr, floor (VX (0)) + 0.5, top);
  cairo_line_to (cr, floor (VX (0)) + 0.5, bottom);
  cairo_move_to (cr, left, floor (bottom) + 0.5);
  cairo_line_to (cr, right, floor (bottom) + 0.5);
  cairo_stroke (cr);

  if (d->n_points == 0 || d->n_series == 0)
    return;

  {
    double slot = plot_h / d->n_points;
    double gap = slot * 0.2;
    double bar = (slot - gap) / d->n_series;

    for (int p = 0; p < d->n_points; p++)
      show_text (chart, cr, layout, d->categories[p], left - 4, top + slot * (p + 0.5) - 7, 1.0, FALSE);

    for (int s = 0; s < d->n_series; s++)
      {
        set_rgb (cr, SERIES_COLOURS[s % G_N_ELEMENTS (SERIES_COLOURS)]);
        for (int p = 0; p < d->n_points; p++)
          {
            double v = d->values[s * d->n_points + p];
            double by, bx, bw;

            if (isnan (v))
              continue;
            by = top + slot * p + gap / 2 + bar * s;
            bx = VX (MIN (v, 0));
            bw = fabs (VX (v) - VX (0));
            if (!fill_with_picture (chart, cr, d, bx, by, bw, MAX (bar - 1, 1)))
              {
                cairo_rectangle (cr, bx, by, bw, MAX (bar - 1, 1));
                cairo_fill (cr);
              }
          }
      }
  }
  #undef VX
}

/* XY: the first series supplies x, each other series is a set of points
 * at those x, with nice-stepped axes both ways. */
/* A stock chart: the day's range as a vertical line, with the open and
 * the close marked.  Three series are high, low and close, which Excel
 * draws as a bar with a tick to the right; four are open, high, low and
 * close, which it draws as a candle -- hollow when the close is above
 * the open, filled when it is below. */
static void
draw_stock (const O42Chart *chart, cairo_t *cr, PangoLayout *layout, const ChartData *d,
            double x0, double y0, double w, double h)
{
  double lo = 0, hi = 0, step, ylo, yhi, slot;
  double left = x0 + 44, right = x0 + w - 8;
  double top = y0 + 8, bottom = y0 + h - 22;
  gboolean any = FALSE, candles;
  int high_at, low_at, close_at, open_at = -1;

  if (d->n_points == 0 || (d->n_series != 3 && d->n_series != 4))
    return;
  candles = d->n_series == 4;
  if (candles)
    { open_at = 0; high_at = 1; low_at = 2; close_at = 3; }
  else
    { high_at = 0; low_at = 1; close_at = 2; }

  for (int s = 0; s < d->n_series; s++)
    for (int p = 0; p < d->n_points; p++)
      {
        double v = d->values[s * d->n_points + p];

        if (isnan (v)) continue;
        if (!any) { lo = hi = v; any = TRUE; }
        lo = MIN (lo, v);
        hi = MAX (hi, v);
      }
  if (!any)
    return;

  step = nice_step (hi - lo);
  ylo = chart->has_min ? chart->min : floor (lo / step) * step;
  yhi = chart->has_max ? chart->max : ceil (hi / step) * step;
  if (yhi <= ylo) yhi = ylo + step;

  #define KY(v) (bottom - ((v) - ylo) / (yhi - ylo) * (bottom - top))

  cairo_set_line_width (cr, 1);
  for (double v = ylo; v <= yhi + step / 2; v += step)
    {
      char *label = o42_number_format (v, O42_NUM_GENERAL, 0);

      if (chart->gridlines)
        {
          cairo_set_source_rgb (cr, 0.85, 0.85, 0.85);
          cairo_move_to (cr, left, floor (KY (v)) + 0.5);
          cairo_line_to (cr, right, floor (KY (v)) + 0.5);
          cairo_stroke (cr);
        }
      cairo_set_source_rgb (cr, 0, 0, 0);
      show_text (chart, cr, layout, label, left - 4, KY (v) - 7, 1.0, FALSE);
      g_free (label);
    }
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_rectangle (cr, floor (left) + 0.5, floor (top) + 0.5, right - left, bottom - top);
  cairo_stroke (cr);

  slot = (right - left) / MAX (d->n_points, 1);
  for (int p = 0; p < d->n_points; p++)
    {
      double centre = left + slot * (p + 0.5);
      double high = d->values[high_at * d->n_points + p];
      double low = d->values[low_at * d->n_points + p];
      double close = d->values[close_at * d->n_points + p];
      double open = candles ? d->values[open_at * d->n_points + p] : 0;
      double tick = MIN (slot * 0.3, 8);

      if (isnan (high) || isnan (low))
        continue;

      cairo_set_source_rgb (cr, 0, 0, 0);
      cairo_move_to (cr, floor (centre) + 0.5, KY (high));
      cairo_line_to (cr, floor (centre) + 0.5, KY (low));
      cairo_stroke (cr);

      if (candles && !isnan (open) && !isnan (close))
        {
          double body_top = KY (MAX (open, close));
          double body_bottom = KY (MIN (open, close));
          double body = MAX (body_bottom - body_top, 1);

          cairo_rectangle (cr, floor (centre - tick) + 0.5, floor (body_top) + 0.5,
                           tick * 2, body);
          /* A day that rose is hollow, one that fell is filled: the
           * candle every chart of prices has been drawn with since
           * Homma's rice market. */
          if (close >= open)
            cairo_set_source_rgb (cr, 1, 1, 1);
          cairo_fill_preserve (cr);
          cairo_set_source_rgb (cr, 0, 0, 0);
          cairo_stroke (cr);
        }
      else
        {
          if (!isnan (close))
            {
              cairo_move_to (cr, floor (centre) + 0.5, floor (KY (close)) + 0.5);
              cairo_line_to (cr, floor (centre + tick) + 0.5, floor (KY (close)) + 0.5);
              cairo_stroke (cr);
            }
        }

      if (d->categories[p] != NULL && slot > 24)
        show_text (chart, cr, layout, d->categories[p], centre, bottom + 3, 0.5, FALSE);
    }

  if (chart->data_labels)
    for (int p = 0; p < d->n_points; p++)
      {
        double close = d->values[close_at * d->n_points + p];

        if (!isnan (close))
          draw_value_label (chart, cr, layout, close, left + slot * (p + 0.5), KY (close), TRUE);
      }
  #undef KY
}

/* The colour of a band of a surface, low to high: a ramp through blue,
 * green, yellow and red, which is how a map colours height and how
 * Excel colours a surface. */
static guint32
surface_band (int which, int bands)
{
  static const guint32 STOPS[] = { 0x1F3B8C, 0x2E8B57, 0xE8C13A, 0xB03028 };
  double at = bands > 1 ? (double) which / (bands - 1) : 0;
  double scaled = at * (G_N_ELEMENTS (STOPS) - 1);
  int first = CLAMP ((int) scaled, 0, (int) G_N_ELEMENTS (STOPS) - 2);
  double f = scaled - first;
  guint32 a = STOPS[first], b = STOPS[first + 1];
  int r = (int) (((a >> 16) & 0xFF) * (1 - f) + ((b >> 16) & 0xFF) * f);
  int g = (int) (((a >> 8) & 0xFF) * (1 - f) + ((b >> 8) & 0xFF) * f);
  int bl = (int) ((a & 0xFF) * (1 - f) + (b & 0xFF) * f);

  return (guint32) ((r << 16) | (g << 8) | bl);
}

/* A surface chart: the values as a height field over the category and
 * series axes, drawn in the same isometric the 3-D bars use, with the
 * height in bands of colour.  Each cell of the mesh is a quadrilateral,
 * painted back to front so the near ones cover the far. */
static void
draw_surface (const O42Chart *chart, cairo_t *cr, PangoLayout *layout, const ChartData *d,
              double x0, double y0, double w, double h)
{
  double lo = 0, hi = 0, step, ylo, yhi;
  double left = x0 + 44, right = x0 + w - 8;
  double top = y0 + 8, bottom = y0 + h - 22;
  double depth_x, depth_y;
  gboolean any = FALSE;
  int bands;

  if (d->n_points < 2 || d->n_series < 2)
    return;

  for (int s = 0; s < d->n_series; s++)
    for (int p = 0; p < d->n_points; p++)
      {
        double v = d->values[s * d->n_points + p];

        if (isnan (v)) continue;
        if (!any) { lo = hi = v; any = TRUE; }
        lo = MIN (lo, v);
        hi = MAX (hi, v);
      }
  if (!any)
    return;

  step = nice_step (hi - lo);
  ylo = chart->has_min ? chart->min : floor (lo / step) * step;
  yhi = chart->has_max ? chart->max : ceil (hi / step) * step;
  if (yhi <= ylo) yhi = ylo + step;
  bands = (int) MAX (round ((yhi - ylo) / step), 1);

  /* The plot is drawn on a parallelogram: one series back is a step up
   * and to the right, and the rest of the box is height. */
  depth_x = MIN ((right - left) * 0.28, 90);
  depth_y = MIN ((bottom - top) * 0.28, 70);

  #define UX(p, s) (left + (right - left - depth_x) * ((double) (p) / MAX (d->n_points - 1, 1)) \
                    + depth_x * ((double) (s) / MAX (d->n_series - 1, 1)))
  #define UY(p, s, v) (bottom - depth_y * ((double) (s) / MAX (d->n_series - 1, 1))              \
                       - ((v) - ylo) / (yhi - ylo) * (bottom - top - depth_y))

  cairo_set_line_width (cr, 1);
  for (double v = ylo; v <= yhi + step / 2; v += step)
    {
      char *label = o42_number_format (v, O42_NUM_GENERAL, 0);

      cairo_set_source_rgb (cr, 0, 0, 0);
      show_text (chart, cr, layout, label, left - 4, UY (0, 0, v) - 7, 1.0, FALSE);
      g_free (label);
    }

  /* Back to front, so the near quadrilaterals cover the far ones. */
  for (int s = d->n_series - 2; s >= 0; s--)
    for (int p = 0; p < d->n_points - 1; p++)
      {
        double corners[4];
        double mean = 0;
        int which;
        guint32 colour;

        corners[0] = d->values[s * d->n_points + p];
        corners[1] = d->values[s * d->n_points + p + 1];
        corners[2] = d->values[(s + 1) * d->n_points + p + 1];
        corners[3] = d->values[(s + 1) * d->n_points + p];
        if (isnan (corners[0]) || isnan (corners[1]) ||
            isnan (corners[2]) || isnan (corners[3]))
          continue;
        for (int i = 0; i < 4; i++)
          mean += corners[i];
        mean /= 4;

        which = (int) ((mean - ylo) / (yhi - ylo) * bands);
        which = CLAMP (which, 0, bands - 1);
        colour = surface_band (which, bands);

        cairo_move_to (cr, UX (p, s), UY (p, s, corners[0]));
        cairo_line_to (cr, UX (p + 1, s), UY (p + 1, s, corners[1]));
        cairo_line_to (cr, UX (p + 1, s + 1), UY (p + 1, s + 1, corners[2]));
        cairo_line_to (cr, UX (p, s + 1), UY (p, s + 1, corners[3]));
        cairo_close_path (cr);
        set_rgb (cr, colour);
        cairo_fill_preserve (cr);
        cairo_set_source_rgb (cr, 0.25, 0.25, 0.25);
        cairo_stroke (cr);
      }

  /* The category names along the front edge, and the series names up
   * the right-hand one, where there is room for them. */
  cairo_set_source_rgb (cr, 0, 0, 0);
  for (int p = 0; p < d->n_points; p++)
    if (d->categories[p] != NULL && (right - left - depth_x) / d->n_points > 24)
      show_text (chart, cr, layout, d->categories[p], UX (p, 0), bottom + 3, 0.5, FALSE);
  for (int s = 0; s < d->n_series; s++)
    if (d->series[s] != NULL && depth_y / d->n_series > 10)
      show_text (chart, cr, layout, d->series[s],
                 UX (d->n_points - 1, s) + 3, UY (d->n_points - 1, s, ylo) - 7, 0.0, FALSE);
  #undef UX
  #undef UY
}

static void
draw_scatter (const O42Chart *chart, cairo_t *cr, PangoLayout *layout, const ChartData *d,
              double x0, double y0, double w, double h)
{
  double xmin = 0, xmax = 0, ymin = 0, ymax = 0;
  double xstep, ystep, xlo, xhi, ylo, yhi;
  double left = x0 + 44, right = x0 + w - 8;
  double top = y0 + 8, bottom = y0 + h - 22;
  gboolean any = FALSE;

  if (d->n_series < 2 || d->n_points == 0)
    return;

  for (int p = 0; p < d->n_points; p++)
    {
      double x = d->values[p];
      if (isnan (x)) continue;
      if (!any) { xmin = xmax = x; any = TRUE; }
      xmin = MIN (xmin, x); xmax = MAX (xmax, x);
    }
  for (int s = 1; s < d->n_series; s++)
    for (int p = 0; p < d->n_points; p++)
      {
        double y = d->values[s * d->n_points + p];
        if (isnan (y)) continue;
        ymin = MIN (ymin, y); ymax = MAX (ymax, y);
      }

  xstep = nice_step (xmax - xmin); ystep = nice_step (ymax - ymin);
  xlo = floor (xmin / xstep) * xstep; xhi = ceil (xmax / xstep) * xstep;
  ylo = floor (ymin / ystep) * ystep; yhi = ceil (ymax / ystep) * ystep;
  if (xhi <= xlo) xhi = xlo + xstep;
  if (yhi <= ylo) yhi = ylo + ystep;

  #define SX(v) (left + ((v) - xlo) / (xhi - xlo) * (right - left))
  #define SY(v) (bottom - ((v) - ylo) / (yhi - ylo) * (bottom - top))

  cairo_set_line_width (cr, 1);
  for (double v = ylo; v <= yhi + ystep / 2; v += ystep)
    {
      char *label = o42_number_format (v, O42_NUM_GENERAL, 0);
      cairo_set_source_rgb (cr, 0.85, 0.85, 0.85);
      cairo_move_to (cr, left, floor (SY (v)) + 0.5);
      cairo_line_to (cr, right, floor (SY (v)) + 0.5);
      cairo_stroke (cr);
      cairo_set_source_rgb (cr, 0, 0, 0);
      show_text (chart, cr, layout, label, left - 4, SY (v) - 7, 1.0, FALSE);
      g_free (label);
    }
  for (double v = xlo; v <= xhi + xstep / 2; v += xstep)
    {
      char *label = o42_number_format (v, O42_NUM_GENERAL, 0);
      cairo_set_source_rgb (cr, 0.85, 0.85, 0.85);
      cairo_move_to (cr, floor (SX (v)) + 0.5, top);
      cairo_line_to (cr, floor (SX (v)) + 0.5, bottom);
      cairo_stroke (cr);
      cairo_set_source_rgb (cr, 0, 0, 0);
      show_text (chart, cr, layout, label, SX (v), bottom + 3, 0.5, FALSE);
      g_free (label);
    }

  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_rectangle (cr, floor (left) + 0.5, floor (top) + 0.5, right - left, bottom - top);
  cairo_stroke (cr);

  for (int s = 1; s < d->n_series; s++)
    {
      set_rgb (cr, SERIES_COLOURS[(s - 1) % G_N_ELEMENTS (SERIES_COLOURS)]);
      for (int p = 0; p < d->n_points; p++)
        {
          double x = d->values[p], y = d->values[s * d->n_points + p];
          if (isnan (x) || isnan (y)) continue;
          draw_marker (chart, cr, d, SX (x), SY (y));
          if (chart->data_labels)
            draw_value_label (chart, cr, layout, y, SX (x), SY (y), TRUE);
        }
      if (chart->err_bars != O42_ERRBAR_NONE)
        {
          const double *ys = d->values + (gsize) s * d->n_points;

          cairo_save (cr);
          cairo_set_line_width (cr, 1);
          set_rgb (cr, SERIES_COLOURS[(s - 1) % G_N_ELEMENTS (SERIES_COLOURS)]);
          for (int p = 0; p < d->n_points; p++)
            {
              double x = d->values[p], y = ys[p];
              double e = error_length (chart, ys, d->n_points, y);

              if (isnan (x) || isnan (y) || isnan (e))
                continue;
              draw_error_bar (cr, SX (x), SY (y - e), SY (y + e), top, bottom);
            }
          cairo_restore (cr);
        }

      if (chart->trend != O42_TREND_NONE)
        {
          /* The curve is fitted to y against the first series' x. */
          static const double dashes[] = { 5, 4 };
          double *cx = g_new (double, TREND_SAMPLES);
          double *cy = g_new (double, TREND_SAMPLES);
          int count = trend_points (chart, d->values, d->values + (gsize) s * d->n_points,
                                    d->n_points, cx, cy, TREND_SAMPLES);
          gboolean down = FALSE;

          cairo_save (cr);
          cairo_set_dash (cr, dashes, 2, 0);
          cairo_set_line_width (cr, 1.5);
          set_rgb (cr, SERIES_COLOURS[(s - 1) % G_N_ELEMENTS (SERIES_COLOURS)]);
          for (int i = 0; i < count; i++)
            {
              double px, py;

              if (isnan (cy[i]) || isnan (cx[i]))
                { down = FALSE; continue; }
              px = SX (cx[i]);
              py = CLAMP (SY (cy[i]), top, bottom);
              if (down) cairo_line_to (cr, px, py);
              else cairo_move_to (cr, px, py);
              down = TRUE;
            }
          cairo_stroke (cr);
          cairo_restore (cr);
          g_free (cx);
          g_free (cy);
        }
    }
  #undef SX
  #undef SY
}

static void
draw_pie (const O42Chart *chart, cairo_t *cr, PangoLayout *layout, const ChartData *d,
          double x0, double y0, double w, double h)
{
  double total = 0;
  double cx = x0 + w * 0.38, cy = y0 + h / 2;
  double radius = MIN (w * 0.34, h / 2 - 10);
  double angle = -G_PI / 2;

  if (d->n_points == 0 || d->n_series == 0)
    return;

  /* A pie shows the first series; the categories are its slices. */
  for (int p = 0; p < d->n_points; p++)
    {
      double v = d->values[p];
      if (!isnan (v) && v > 0)
        total += v;
    }
  if (total <= 0)
    return;

  for (int p = 0; p < d->n_points; p++)
    {
      double v = d->values[p];
      double sweep;

      if (isnan (v) || v <= 0)
        continue;
      sweep = 2 * G_PI * v / total;

      /* A 3-D pie is an ellipse standing on a wall of its own colour:
       * the wall first, so that the slice sits on top of it. */
      if (chart->three_d)
        {
          double ry = radius * 0.55;
          double thickness = MAX (radius * 0.20, 6);
          int steps = MAX (2, (int) (sweep * 16));

          set_rgb (cr, shaded (SERIES_COLOURS[p % G_N_ELEMENTS (SERIES_COLOURS)], 0.6));
          cairo_new_path (cr);
          for (int i = 0; i <= steps; i++)
            {
              double at = angle + sweep * i / (double) steps;
              double ex = cx + cos (at) * radius, ey = cy + sin (at) * ry;

              if (i == 0) cairo_move_to (cr, ex, ey);
              else cairo_line_to (cr, ex, ey);
            }
          for (int i = steps; i >= 0; i--)
            {
              double at = angle + sweep * i / (double) steps;

              cairo_line_to (cr, cx + cos (at) * radius, cy + sin (at) * ry + thickness);
            }
          cairo_close_path (cr);
          cairo_fill (cr);
        }

      set_rgb (cr, SERIES_COLOURS[p % G_N_ELEMENTS (SERIES_COLOURS)]);
      cairo_new_path (cr);
      cairo_move_to (cr, cx, cy);
      if (chart->three_d)
        {
          int steps = MAX (2, (int) (sweep * 16));

          for (int i = 0; i <= steps; i++)
            {
              double at = angle + sweep * i / (double) steps;

              cairo_line_to (cr, cx + cos (at) * radius, cy + sin (at) * radius * 0.55);
            }
        }
      else
        cairo_arc (cr, cx, cy, radius, angle, angle + sweep);
      cairo_close_path (cr);
      cairo_fill_preserve (cr);
      cairo_set_source_rgb (cr, 1, 1, 1);
      cairo_set_line_width (cr, 1);
      cairo_stroke (cr);
      if (chart->kind == O42_CHART_DOUGHNUT)
        {
          /* The hole, painted over the slice's point. */
          cairo_set_source_rgb (cr, 1, 1, 1);
          cairo_new_path (cr);
          cairo_arc (cr, cx, cy, radius * 0.45, angle, angle + sweep);
          cairo_line_to (cr, cx, cy);
          cairo_close_path (cr);
          cairo_fill (cr);
        }

      if (chart->data_labels)
        {
          char *text = g_strdup_printf ("%.0f%%", 100 * v / total);
          double mid = angle + sweep / 2;
          cairo_set_source_rgb (cr, 1, 1, 1);
          show_text (chart, cr, layout, text, cx + cos (mid) * radius * 0.65,
                     cy + sin (mid) * radius * (chart->three_d ? 0.36 : 0.65) - 7, 0.5, FALSE);
          g_free (text);
        }
      angle += sweep;
    }

  /* The legend, down the right. */
  for (int p = 0; p < d->n_points && p < 12; p++)
    {
      double ly = y0 + 12 + p * 16;
      char *label;
      double v = d->values[p];

      if (isnan (v) || v <= 0)
        continue;
      set_rgb (cr, SERIES_COLOURS[p % G_N_ELEMENTS (SERIES_COLOURS)]);
      cairo_rectangle (cr, x0 + w * 0.76, ly + 2, 10, 10);
      cairo_fill (cr);
      cairo_set_source_rgb (cr, 0, 0, 0);
      label = g_strdup_printf ("%s (%.0f%%)", d->categories[p], 100 * v / total);
      show_text (chart, cr, layout, label, x0 + w * 0.76 + 14, ly, 0.0, FALSE);
      g_free (label);
    }
}

static void
draw_legend (const O42Chart *chart, cairo_t *cr, PangoLayout *layout, const ChartData *d,
             double x0, double y0, double w, int first)
{
  double x = x0 + 48;

  for (int s = first; s < d->n_series && s < first + 6; s++)
    {
      int tw, th;

      set_rgb (cr, SERIES_COLOURS[(s - first) % G_N_ELEMENTS (SERIES_COLOURS)]);
      cairo_rectangle (cr, x, y0 + 3, 10, 10);
      cairo_fill (cr);
      cairo_set_source_rgb (cr, 0, 0, 0);
      show_text (chart, cr, layout, d->series[s], x + 14, y0, 0.0, FALSE);
      pango_layout_get_pixel_size (layout, &tw, &th);
      x += 14 + tw + 12;
      if (x > x0 + w - 40)
        break;
    }
}

/* ---- The four plots Gnumeric has and Excel has not --------------------- */

/* The five numbers a box plot stands on, with the whiskers reaching to
 * the furthest point still within one and a half times the box. */
typedef struct {
  double low, q1, median, q3, high;
  double *outliers;
  int     n_outliers;
  gboolean any;
} FiveNumbers;

static int
compare_doubles (const void *a, const void *b)
{
  double x = *(const double *) a, y = *(const double *) b;

  return x < y ? -1 : x > y ? 1 : 0;
}

/* The quantile at p of a sorted array, the way a spreadsheet's
 * PERCENTILE reads it: between the two neighbours. */
static double
quantile_of (const double *sorted, int n, double p)
{
  double at = p * (n - 1);
  int i = (int) floor (at);
  double frac = at - i;

  if (n < 1)
    return NAN;
  if (i >= n - 1)
    return sorted[n - 1];
  return sorted[i] + frac * (sorted[i + 1] - sorted[i]);
}

static void
five_numbers (const double *values, int n, FiveNumbers *out)
{
  double *sorted = g_new (double, MAX (n, 1));
  int kept = 0;
  double iqr, lo_fence, hi_fence;

  memset (out, 0, sizeof *out);
  for (int i = 0; i < n; i++)
    if (!isnan (values[i]))
      sorted[kept++] = values[i];
  if (kept == 0)
    { g_free (sorted); return; }
  qsort (sorted, kept, sizeof (double), compare_doubles);

  out->q1 = quantile_of (sorted, kept, 0.25);
  out->median = quantile_of (sorted, kept, 0.5);
  out->q3 = quantile_of (sorted, kept, 0.75);
  iqr = out->q3 - out->q1;
  lo_fence = out->q1 - 1.5 * iqr;
  hi_fence = out->q3 + 1.5 * iqr;

  out->low = out->q1;
  out->high = out->q3;
  out->outliers = g_new (double, kept);
  for (int i = 0; i < kept; i++)
    {
      if (sorted[i] < lo_fence || sorted[i] > hi_fence)
        out->outliers[out->n_outliers++] = sorted[i];
      else
        {
          out->low = MIN (out->low, sorted[i]);
          out->high = MAX (out->high, sorted[i]);
        }
    }
  out->any = TRUE;
  g_free (sorted);
}

/* A box and its whiskers for each series: the middle half of the
 * numbers in the box, the median across it, and anything more than one
 * and a half boxes out drawn as a point of its own. */
static void
draw_box_plot (const O42Chart *chart, cairo_t *cr, PangoLayout *layout,
               const ChartData *d, double x0, double y0, double w, double h)
{
  double low = 0, high = 0;
  FiveNumbers *boxes;
  double plot_bottom, plot_top, span, slot;
  gboolean first = TRUE;

  if (d->n_series < 1 || d->n_points < 1)
    return;
  boxes = g_new0 (FiveNumbers, d->n_series);
  for (int s = 0; s < d->n_series; s++)
    {
      five_numbers (&d->values[(gsize) s * d->n_points], d->n_points, &boxes[s]);
      if (!boxes[s].any)
        continue;
      for (int k = 0; k < boxes[s].n_outliers; k++)
        {
          low = first ? boxes[s].outliers[k] : MIN (low, boxes[s].outliers[k]);
          high = first ? boxes[s].outliers[k] : MAX (high, boxes[s].outliers[k]);
          first = FALSE;
        }
      low = first ? boxes[s].low : MIN (low, boxes[s].low);
      high = first ? boxes[s].high : MAX (high, boxes[s].high);
      first = FALSE;
    }
  if (first)
    { g_free (boxes); return; }
  if (high <= low)
    high = low + 1;

  plot_top = y0 + 4;
  plot_bottom = y0 + h - 18;
  span = high - low;
  slot = w / d->n_series;

#define BOX_Y(v) (plot_bottom - (plot_bottom - plot_top) * ((v) - low) / span)

  /* The axis and its labels. */
  cairo_set_line_width (cr, 1);
  cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
  cairo_move_to (cr, x0 + 0.5, plot_top);
  cairo_line_to (cr, x0 + 0.5, plot_bottom + 0.5);
  cairo_line_to (cr, x0 + w, plot_bottom + 0.5);
  cairo_stroke (cr);

  for (int s = 0; s < d->n_series; s++)
    {
      double centre = x0 + slot * (s + 0.5);
      double half = MIN (slot * 0.3, 30);
      guint32 colour = SERIES_COLOURS[s % G_N_ELEMENTS (SERIES_COLOURS)];

      if (!boxes[s].any)
        continue;

      /* The whiskers first, so the box covers where they meet it. */
      cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
      cairo_move_to (cr, centre, BOX_Y (boxes[s].low));
      cairo_line_to (cr, centre, BOX_Y (boxes[s].high));
      cairo_stroke (cr);
      for (int end = 0; end < 2; end++)
        {
          double v = end == 0 ? boxes[s].low : boxes[s].high;

          cairo_move_to (cr, centre - half / 2, BOX_Y (v));
          cairo_line_to (cr, centre + half / 2, BOX_Y (v));
        }
      cairo_stroke (cr);

      set_rgb (cr, colour);
      cairo_rectangle (cr, centre - half, BOX_Y (boxes[s].q3),
                       half * 2, BOX_Y (boxes[s].q1) - BOX_Y (boxes[s].q3));
      cairo_fill_preserve (cr);
      cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
      cairo_stroke (cr);

      cairo_move_to (cr, centre - half, BOX_Y (boxes[s].median));
      cairo_line_to (cr, centre + half, BOX_Y (boxes[s].median));
      cairo_stroke (cr);

      for (int k = 0; k < boxes[s].n_outliers; k++)
        {
          cairo_arc (cr, centre, BOX_Y (boxes[s].outliers[k]), 2, 0, 2 * G_PI);
          cairo_stroke (cr);
        }

      if (d->series != NULL && d->series[s] != NULL)
        {
          cairo_set_source_rgb (cr, 0, 0, 0);
          show_text (chart, cr, layout, d->series[s], centre, plot_bottom + 2, 0.5, FALSE);
        }
    }

#undef BOX_Y

  for (int s = 0; s < d->n_series; s++)
    g_free (boxes[s].outliers);
  g_free (boxes);
}

/* How many of the values fall in each of a few equal bins: the plot a
 * histogram is, rather than the bars of a column chart. */
static void
draw_histogram_plot (const O42Chart *chart, cairo_t *cr, PangoLayout *layout,
                     const ChartData *d, double x0, double y0, double w, double h)
{
  int n = d->n_series * d->n_points;
  double low = 0, high = 0, width, plot_top, plot_bottom;
  int bins, tallest = 0;
  int *counts;
  gboolean first = TRUE;

  for (int i = 0; i < n; i++)
    if (!isnan (d->values[i]))
      {
        low = first ? d->values[i] : MIN (low, d->values[i]);
        high = first ? d->values[i] : MAX (high, d->values[i]);
        first = FALSE;
      }
  if (first)
    return;
  if (high <= low)
    high = low + 1;

  /* Sturges' rule, which is what a spreadsheet reaches for when it is
   * not told how many bins to use. */
  bins = (int) CLAMP (ceil (log2 (MAX (n, 2)) + 1), 3, 30);
  width = (high - low) / bins;
  counts = g_new0 (int, bins);
  for (int i = 0; i < n; i++)
    if (!isnan (d->values[i]))
      {
        int b = (int) ((d->values[i] - low) / width);

        counts[CLAMP (b, 0, bins - 1)]++;
      }
  for (int b = 0; b < bins; b++)
    tallest = MAX (tallest, counts[b]);
  if (tallest == 0)
    { g_free (counts); return; }

  plot_top = y0 + 4;
  plot_bottom = y0 + h - 18;

  cairo_set_line_width (cr, 1);
  cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
  cairo_move_to (cr, x0 + 0.5, plot_top);
  cairo_line_to (cr, x0 + 0.5, plot_bottom + 0.5);
  cairo_line_to (cr, x0 + w, plot_bottom + 0.5);
  cairo_stroke (cr);

  for (int b = 0; b < bins; b++)
    {
      double bx = x0 + 1 + (w - 2) * b / bins;
      double bw = (w - 2) / bins;
      double bh = (plot_bottom - plot_top) * counts[b] / tallest;

      if (bh <= 0)
        continue;
      set_rgb (cr, SERIES_COLOURS[0]);
      cairo_rectangle (cr, bx, plot_bottom - bh, bw - 1, bh);
      cairo_fill_preserve (cr);
      cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
      cairo_stroke (cr);
    }

  /* The ends of the range, which is all a histogram's axis needs. */
  cairo_set_source_rgb (cr, 0, 0, 0);
  {
    char *text = chart_number (chart, low);

    show_text (chart, cr, layout, text, x0 + 2, plot_bottom + 2, 0, FALSE);
    g_free (text);
    text = chart_number (chart, high);
    show_text (chart, cr, layout, text, x0 + w - 2, plot_bottom + 2, 1, FALSE);
    g_free (text);
  }
  g_free (counts);
}

/* A polar plot: the angle is the category, going round, and the radius
 * the value -- a radar without the web, drawn as a curve. */
static void
draw_polar_plot (const O42Chart *chart, cairo_t *cr, PangoLayout *layout,
                 const ChartData *d, double x0, double y0, double w, double h)
{
  double cx = x0 + w / 2, cy = y0 + h / 2;
  double radius = MIN (w, h) / 2 - 16;
  double top = d->max > 0 ? d->max : 1;
  double step;

  if (d->n_points < 2 || radius <= 8)
    return;
  step = nice_step (top);
  top = ceil (top / step) * step;

  cairo_set_line_width (cr, 1);
  cairo_set_source_rgb (cr, 0.85, 0.85, 0.85);
  for (double v = step; v <= top + step / 2; v += step)
    {
      cairo_new_path (cr);
      cairo_arc (cr, cx, cy, radius * v / top, 0, 2 * G_PI);
      cairo_stroke (cr);
    }
  for (int k = 0; k < 12; k++)
    {
      double at = 2 * G_PI * k / 12;

      cairo_move_to (cr, cx, cy);
      cairo_line_to (cr, cx + cos (at) * radius, cy + sin (at) * radius);
    }
  cairo_stroke (cr);

  for (int s = 0; s < d->n_series; s++)
    {
      gboolean started = FALSE;

      set_rgb (cr, SERIES_COLOURS[s % G_N_ELEMENTS (SERIES_COLOURS)]);
      cairo_set_line_width (cr, 2);
      for (int p = 0; p < d->n_points; p++)
        {
          double v = d->values[(gsize) s * d->n_points + p];
          double at, r, px, py;

          if (isnan (v))
            continue;
          at = 2 * G_PI * p / d->n_points;
          r = radius * CLAMP (v, 0, top) / top;
          px = cx + cos (at) * r;
          py = cy + sin (at) * r;
          if (!started)
            { cairo_move_to (cr, px, py); started = TRUE; }
          else
            cairo_line_to (cr, px, py);
        }
      if (started)
        cairo_close_path (cr);
      cairo_stroke (cr);
    }

  cairo_set_source_rgb (cr, 0, 0, 0);
  {
    char *text = chart_number (chart, top);

    show_text (chart, cr, layout, text, cx + radius, cy, 0.5, FALSE);
    g_free (text);
  }
  (void) layout;
}

/* A contour plot: the same height field a surface chart draws, seen
 * from above, with a line where the height crosses each level. */
static void
draw_contour_plot (const O42Chart *chart, cairo_t *cr, PangoLayout *layout,
                   const ChartData *d, double x0, double y0, double w, double h)
{
  double low = d->min, high = d->max;
  double cell_w, cell_h;
  int levels = 8;

  if (d->n_series < 2 || d->n_points < 2)
    return;
  if (high <= low)
    high = low + 1;
  cell_w = w / d->n_series;
  cell_h = h / d->n_points;

  /* The bands first, in the colours a surface uses, then the lines
   * between them. */
  for (int s = 0; s < d->n_series; s++)
    for (int p = 0; p < d->n_points; p++)
      {
        double v = d->values[(gsize) s * d->n_points + p];

        if (isnan (v))
          continue;
        set_rgb (cr, surface_band ((int) ((v - low) / (high - low) * 15), 16));
        cairo_rectangle (cr, x0 + s * cell_w, y0 + p * cell_h, cell_w + 0.5, cell_h + 0.5);
        cairo_fill (cr);
      }

  cairo_set_line_width (cr, 1);
  cairo_set_source_rgb (cr, 0.25, 0.25, 0.25);
  for (int k = 1; k < levels; k++)
    {
      double level = low + (high - low) * k / levels;

      /* Marching squares: where the level crosses the four edges of
       * a cell of the grid, joined up.  Two crossings make one
       * segment; four make two, and which pair goes with which is the
       * saddle no contour plot answers the same way twice. */
      for (int s = 0; s + 1 < d->n_series; s++)
        for (int p = 0; p + 1 < d->n_points; p++)
          {
            double corner[4];
            double cross_x[4], cross_y[4];
            int found = 0;
            double px = x0 + (s + 0.5) * cell_w, py = y0 + (p + 0.5) * cell_h;

            corner[0] = d->values[(gsize) s * d->n_points + p];             /* top left */
            corner[1] = d->values[(gsize) (s + 1) * d->n_points + p];       /* top right */
            corner[2] = d->values[(gsize) (s + 1) * d->n_points + p + 1];   /* bottom right */
            corner[3] = d->values[(gsize) s * d->n_points + p + 1];         /* bottom left */
            if (isnan (corner[0]) || isnan (corner[1]) ||
                isnan (corner[2]) || isnan (corner[3]))
              continue;

            for (int e = 0; e < 4; e++)
              {
                double a = corner[e], b = corner[(e + 1) % 4], t;

                if ((a - level) * (b - level) >= 0)
                  continue;
                t = (level - a) / (b - a);
                switch (e)
                  {
                  case 0: cross_x[found] = px + t * cell_w; cross_y[found] = py; break;
                  case 1: cross_x[found] = px + cell_w; cross_y[found] = py + t * cell_h; break;
                  case 2: cross_x[found] = px + cell_w - t * cell_w; cross_y[found] = py + cell_h; break;
                  default: cross_x[found] = px; cross_y[found] = py + cell_h - t * cell_h; break;
                  }
                found++;
              }
            for (int pair = 0; pair + 1 < found; pair += 2)
              {
                cairo_move_to (cr, cross_x[pair], cross_y[pair]);
                cairo_line_to (cr, cross_x[pair + 1], cross_y[pair + 1]);
              }
          }
      cairo_stroke (cr);
    }
  (void) layout;
  (void) chart;
}

void
o42_chart_draw (const O42Chart *chart, cairo_t *cr, double width, double height,
                O42ChartFetch fetch, gpointer user)
{
  o42_chart_draw_full (chart, cr, width, height, fetch, user, NULL, NULL);
}

void
o42_chart_draw_full (const O42Chart *chart, cairo_t *cr, double width, double height,
                     O42ChartFetch fetch, gpointer user,
                     O42ChartPicture picture, gpointer picture_user)
{
  ChartData d;
  PangoLayout *layout;
  double top = 0;

  g_return_if_fail (chart != NULL);
  g_return_if_fail (cr != NULL);

  chart_data_read (chart, fetch, user, &d);
  d.marker_picture = (picture != NULL && chart->marker == O42_MARKER_PICTURE)
                     ? picture (picture_user, chart->marker_picture) : NULL;
  layout = pango_cairo_create_layout (cr);

  /* Paper, with a hairline frame. */
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_rectangle (cr, 0, 0, width, height);
  cairo_fill (cr);
  cairo_set_source_rgb (cr, 0.5, 0.5, 0.5);
  cairo_set_line_width (cr, 1);
  cairo_rectangle (cr, 0.5, 0.5, width - 1, height - 1);
  cairo_stroke (cr);

  cairo_set_source_rgb (cr, 0, 0, 0);
  if (chart->title != NULL && *chart->title != '\0')
    {
      show_text (chart, cr, layout, chart->title, width / 2, 4, 0.5, TRUE);
      top = 18;
    }

  if (chart->kind == O42_CHART_PIE || chart->kind == O42_CHART_DOUGHNUT)
    draw_pie (chart, cr, layout, &d, 4, top + 4, width - 8, height - top - 8);
  else
    {
      /* A bubble chart's three columns are one series between them, so
       * there is nothing for a legend to name. */
      double legend_h = (d.n_series > 1 && chart->legend &&
                         chart->kind != O42_CHART_BUBBLE &&
                         chart->kind != O42_CHART_STOCK &&
                         chart->kind != O42_CHART_SURFACE &&
                         chart->kind != O42_CHART_HISTOGRAM &&
                         chart->kind != O42_CHART_CONTOUR) ? 16 : 0;
      gboolean has_x = chart->x_title != NULL && *chart->x_title != '\0';
      gboolean has_y = chart->y_title != NULL && *chart->y_title != '\0';
      double x_title_h = has_x ? 16 : 0;
      double y_title_w = has_y ? 16 : 0;
      double plot_x = 4 + y_title_w, plot_y = top + 4;
      double plot_w = width - 8 - y_title_w;
      double plot_h = height - top - 8 - legend_h - x_title_h;

      if (chart->kind == O42_CHART_BAR)
        draw_bar_chart (chart, cr, layout, &d, plot_x, plot_y, plot_w, plot_h);
      else if (chart->kind == O42_CHART_RADAR)
        draw_radar (chart, cr, layout, &d, plot_x, plot_y, plot_w, plot_h);
      else if (chart->kind == O42_CHART_BUBBLE)
        draw_bubble (chart, cr, layout, &d, plot_x, plot_y, plot_w, plot_h);
      else if (chart->kind == O42_CHART_SCATTER)
        draw_scatter (chart, cr, layout, &d, plot_x, plot_y, plot_w, plot_h);
      else if (chart->kind == O42_CHART_STOCK)
        draw_stock (chart, cr, layout, &d, plot_x, plot_y, plot_w, plot_h);
      else if (chart->kind == O42_CHART_SURFACE)
        draw_surface (chart, cr, layout, &d, plot_x, plot_y, plot_w, plot_h);
      else if (chart->kind == O42_CHART_BOX)
        draw_box_plot (chart, cr, layout, &d, plot_x, plot_y, plot_w, plot_h);
      else if (chart->kind == O42_CHART_HISTOGRAM)
        draw_histogram_plot (chart, cr, layout, &d, plot_x, plot_y, plot_w, plot_h);
      else if (chart->kind == O42_CHART_POLAR)
        draw_polar_plot (chart, cr, layout, &d, plot_x, plot_y, plot_w, plot_h);
      else if (chart->kind == O42_CHART_CONTOUR)
        draw_contour_plot (chart, cr, layout, &d, plot_x, plot_y, plot_w, plot_h);
      else
        draw_axes_chart (chart, cr, layout, &d, plot_x, plot_y, plot_w, plot_h);

      cairo_set_source_rgb (cr, 0, 0, 0);
      if (has_x)
        show_text (chart, cr, layout, chart->x_title, plot_x + plot_w / 2, plot_y + plot_h + 1, 0.5, FALSE);
      if (has_y)
        {
          int tw, th;
          pango_layout_set_text (layout, chart->y_title, -1);
          pango_layout_get_pixel_size (layout, &tw, &th);
          cairo_save (cr);
          cairo_translate (cr, 4 + (16 - th) / 2.0, plot_y + plot_h / 2 + tw / 2.0);
          cairo_rotate (cr, -G_PI / 2);
          cairo_move_to (cr, 0, 0);
          pango_cairo_show_layout (cr, layout);
          cairo_restore (cr);
        }
      if (legend_h > 0)
        draw_legend (chart, cr, layout, &d, 4, height - legend_h - 4, width - 8,
                     chart->kind == O42_CHART_SCATTER ? 1 : 0);
    }

  g_object_unref (layout);
  chart_data_free (&d);
}
