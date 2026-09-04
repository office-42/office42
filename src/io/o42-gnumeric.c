/* o42-gnumeric.c - see o42-gnumeric.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-gnumeric.h"

#include "o42-pattern.h"

#include "o42-entry.h"
#include "o42-date.h"
#include "o42-image.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Gnumeric measures columns and rows in points; the grid lays out at 96
 * dpi, so a pixel is three quarters of a point. */
#define PX_TO_PT(px) ((px) * 72.0 / 96.0)
#define PT_TO_PX(pt) ((pt) * 96.0 / 72.0)

/* ---------------------------------------------------------------------- */
/* Writing                                                                 */
/* ---------------------------------------------------------------------- */

/* Gnumeric writes a colour as three 16-bit hex channels. */
static void
append_colour (GString *out, guint32 rgb)
{
  g_string_append_printf (out, "%04X:%04X:%04X",
                          ((rgb >> 16) & 0xff) * 0x101,
                          ((rgb >> 8) & 0xff) * 0x101,
                          (rgb & 0xff) * 0x101);
}

static int
gnm_halign (O42HAlign h)
{
  switch (h)
    {
    case O42_HALIGN_LEFT:   return 2;
    case O42_HALIGN_RIGHT:  return 4;
    case O42_HALIGN_CENTRE: return 8;
    default:                return 1;
    }
}

static int
gnm_valign (O42VAlign v)
{
  switch (v)
    {
    case O42_VALIGN_TOP:    return 1;
    case O42_VALIGN_MIDDLE: return 4;
    default:                return 2;
    }
}

static void
append_style (GString *out, const O42Fmt *fmt, const O42Range *r)
{
  char *format = o42_fmt_format_string (fmt);
  char *escaped = g_markup_escape_text (format, -1);

  g_string_append_printf (out,
    "      <gnm:StyleRegion startCol=\"%d\" startRow=\"%d\" endCol=\"%d\" endRow=\"%d\">\n",
    r->col0, r->row0, r->col1, r->row1);
  g_string_append_printf (out,
    "        <gnm:Style HAlign=\"%d\" VAlign=\"%d\" WrapText=\"%d\" ShrinkToFit=\"0\" "
    "Rotation=\"%d\" Shade=\"%d\" Indent=\"%d\" Locked=\"%d\" Hidden=\"%d\" Fore=\"",
    gnm_halign (fmt->halign), gnm_valign (fmt->valign), fmt->wrap ? 1 : 0,
    fmt->rotation < 0 ? 360 + fmt->rotation : fmt->rotation,
    fmt->pattern != O42_PATTERN_NONE ? o42_pattern_to_shade ((O42Pattern) fmt->pattern)
                                     : (fmt->fill != O42_FILL_NONE ? 1 : 0),
    fmt->indent, fmt->locked ? 1 : 0, fmt->hidden ? 1 : 0);
  append_colour (out, fmt->colour);
  g_string_append (out, "\" Back=\"");
  append_colour (out, fmt->fill != O42_FILL_NONE ? fmt->fill : 0xFFFFFF);
  g_string_append (out, "\" PatternColor=\"");
  append_colour (out, fmt->pattern != O42_PATTERN_NONE ? fmt->pattern_colour : 0x000000);
  g_string_append_printf (out, "\" Format=\"%s\">\n", escaped);

  {
    char *family = g_markup_escape_text (fmt->family ? fmt->family : "Sans", -1);
    g_string_append_printf (out,
      "          <gnm:Font Unit=\"%g\" Bold=\"%d\" Italic=\"%d\" Underline=\"%d\" "
      "StrikeThrough=\"%d\" Script=\"0\">%s</gnm:Font>\n",
      fmt->size / 2.0, fmt->bold, fmt->italic, fmt->underline, fmt->strikeout,
      family);
    g_free (family);
  }

  if (fmt->border_top || fmt->border_bottom || fmt->border_left || fmt->border_right)
    {
      static const char *sides[4] = { "Top", "Bottom", "Left", "Right" };
      static const int codes[] = { 0, 1, 2, 5, 6, 3, 4 };   /* ours -> Gnumeric's */
      g_string_append (out, "          <gnm:StyleBorder>\n");
      for (int i = 0; i < 4; i++)
        if (fmt->border_style[i] != O42_BORDER_NONE)
          {
            g_string_append_printf (out, "            <gnm:%s Style=\"%d\" Color=\"", sides[i], codes[fmt->border_style[i]]);
            append_colour (out, fmt->border_colour[i]);
            g_string_append (out, "\"/>\n");
          }
      g_string_append (out, "          </gnm:StyleBorder>\n");
    }

  g_string_append (out, "        </gnm:Style>\n      </gnm:StyleRegion>\n");

  g_free (escaped);
  g_free (format);
}


/* "RRRR:GGGG:BBBB", sixteen bits a channel, to 0x00RRGGBB. */
static guint32
gnm_colour_parse (const char *text)
{
  guint r = 0, g = 0, b = 0;
  if (text == NULL || sscanf (text, "%x:%x:%x", &r, &g, &b) != 3)
    return 0;
  return ((r >> 8) << 16) | ((g >> 8) << 8) | (b >> 8);
}

/* ---- Headers and footers: Excel's codes to Gnumeric's and back ------- */

/* "&Lleft&Ccentre&Rright" with &P &N &D &T &F &A -> three escaped
 * parts with &[PAGE] &[PAGES] &[DATE] &[TIME] &[FILE] &[TAB]. */
static void
hf_split_gnumeric (const char *text, char **left, char **centre, char **right)
{
  GString *parts[3] = { g_string_new (NULL), g_string_new (NULL), g_string_new (NULL) };
  int which = 1;
  char *raw[3];

  for (const char *p = text != NULL ? text : ""; *p != '\0'; p++)
    {
      if (*p == '&' && p[1] != '\0')
        {
          char code = g_ascii_toupper (p[1]);
          p++;
          switch (code)
            {
            case 'L': which = 0; break;
            case 'C': which = 1; break;
            case 'R': which = 2; break;
            case 'P': g_string_append (parts[which], "&[PAGE]"); break;
            case 'N': g_string_append (parts[which], "&[PAGES]"); break;
            case 'D': g_string_append (parts[which], "&[DATE]"); break;
            case 'T': g_string_append (parts[which], "&[TIME]"); break;
            case 'F': g_string_append (parts[which], "&[FILE]"); break;
            case 'A': g_string_append (parts[which], "&[TAB]"); break;
            case '&': g_string_append_c (parts[which], '&'); break;
            default: break;
            }
          continue;
        }
      g_string_append_c (parts[which], *p);
    }
  for (int i = 0; i < 3; i++)
    raw[i] = g_string_free (parts[i], FALSE);
  *left = g_markup_escape_text (raw[0], -1);
  *centre = g_markup_escape_text (raw[1], -1);
  *right = g_markup_escape_text (raw[2], -1);
  for (int i = 0; i < 3; i++)
    g_free (raw[i]);
}

/* Gnumeric's three parts back to one text in Excel's notation. */
static char *
hf_join_excel (const char *left, const char *middle, const char *right)
{
  GString *out = g_string_new (NULL);
  const char *parts[3] = { left, middle, right };
  const char *codes[3] = { "&L", "&C", "&R" };

  for (int i = 0; i < 3; i++)
    {
      if (parts[i] == NULL || *parts[i] == '\0')
        continue;
      g_string_append (out, codes[i]);
      for (const char *p = parts[i]; *p != '\0'; p++)
        {
          if (*p == '&' && p[1] == '[')
            {
              const char *end = strchr (p, ']');
              if (end != NULL)
                {
                  char *code = g_strndup (p + 2, end - p - 2);
                  if (strcmp (code, "PAGE") == 0) g_string_append (out, "&P");
                  else if (strcmp (code, "PAGES") == 0) g_string_append (out, "&N");
                  else if (strcmp (code, "DATE") == 0) g_string_append (out, "&D");
                  else if (strcmp (code, "TIME") == 0) g_string_append (out, "&T");
                  else if (strcmp (code, "FILE") == 0) g_string_append (out, "&F");
                  else if (strcmp (code, "TAB") == 0) g_string_append (out, "&A");
                  g_free (code);
                  p = end;
                  continue;
                }
            }
          if (*p == '&') g_string_append (out, "&&");
          else g_string_append_c (out, *p);
        }
    }
  return g_string_free (out, FALSE);
}

typedef struct {
  GString   *out;
  O42FmtIdx  default_fmt;
  GArray    *keys;      /* guint64, for sorting */
} Writer;

static void
collect_key (O42Sheet *sheet, int row, int col, gpointer user)
{
  Writer *w = user;
  guint64 key = o42_key (row, col);

  (void) sheet;
  g_array_append_val (w->keys, key);
}

static int
compare_keys (gconstpointer a, gconstpointer b)
{
  guint64 x = *(const guint64 *) a, y = *(const guint64 *) b;
  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void
write_cell (Writer *w, O42Sheet *sheet, int row, int col)
{
  O42Value v;
  char *text = NULL, *escaped;
  int value_type = -1;

  if (o42_sheet_has_formula (sheet, row, col))
    {
      text = o42_sheet_get_input (sheet, row, col);
    }
  else
    {
      o42_sheet_get_value (sheet, row, col, &v);
      switch (v.type)
        {
        case O42_VALUE_NUMBER:
          {
            char buf[G_ASCII_DTOSTR_BUF_SIZE];

            /* The fewest digits that read back as the same double, so 4.3
             * is written as 4.3 and not 4.2999999999999998, and a number
             * survives a round trip unchanged either way. */
            for (int digits = 15; digits <= 17; digits++)
              {
                char spec[8];

                g_snprintf (spec, sizeof spec, "%%.%dg", digits);
                g_ascii_formatd (buf, sizeof buf, spec, v.as.number);
                if (g_ascii_strtod (buf, NULL) == v.as.number)
                  break;
              }
            text = g_strdup (buf);
            value_type = 40;
            break;
          }
        case O42_VALUE_TEXT:  text = g_strdup (v.as.text); value_type = 60; break;
        case O42_VALUE_BOOL:  text = g_strdup (v.as.boolean ? "TRUE" : "FALSE"); value_type = 20; break;
        case O42_VALUE_ERROR: text = g_strdup (o42_error_name (v.as.error)); value_type = 10; break;
        default: break;
        }
      o42_value_clear (&v);
    }

  if (text == NULL)
    return;

  {
    O42Range block;
    if (o42_sheet_array_range (sheet, row, col, &block))
      {
        /* The head of an array formula carries the block's size; the
         * other cells are its results and are left out. */
        if (block.row0 != row || block.col0 != col)
          { g_free (text); return; }
        escaped = g_markup_escape_text (text, -1);
        g_string_append_printf (w->out,
          "      <gnm:Cell Row=\"%d\" Col=\"%d\" Rows=\"%d\" Cols=\"%d\">%s</gnm:Cell>\n",
          row, col, block.row1 - block.row0 + 1, block.col1 - block.col0 + 1, escaped);
        g_free (escaped);
        g_free (text);
        return;
      }
  }

  escaped = g_markup_escape_text (text, -1);
  {
    /* The cell style it wears, in an attribute of our own, and the
     * runs of its text where it is set in more than one font: start,
     * flags, size, colour and face, one run per semicolon.  Gnumeric's
     * format has nowhere else to put them. */
    const char *style = o42_sheet_cell_style (sheet, row, col);
    char *wearing = style != NULL ? g_markup_printf_escaped (" o42-style=\"%s\"", style) : g_strdup ("");
    char *marked = g_strdup ("");
    int n_runs = 0;
    const O42TextRun *runs = o42_sheet_runs (sheet, row, col, &n_runs);

    if (runs != NULL)
      {
        GString *encoded = g_string_new (NULL);

        for (int i = 0; i < n_runs; i++)
          {
            const O42Fmt *f = &runs[i].fmt;
            int flags = (f->bold ? 1 : 0) | (f->italic ? 2 : 0) |
                        (f->underline ? 4 : 0) | (f->strikeout ? 8 : 0);

            g_string_append_printf (encoded, "%s%d,%d,%d,%06X,%s",
                                    encoded->len > 0 ? ";" : "", runs[i].start, flags,
                                    f->size, f->colour,
                                    f->family != NULL ? f->family : "Arial");
          }
        g_free (marked);
        marked = g_markup_printf_escaped (" o42-runs=\"%s\"", encoded->str);
        g_string_free (encoded, TRUE);
      }

    if (value_type >= 0)
      g_string_append_printf (w->out,
        "      <gnm:Cell Row=\"%d\" Col=\"%d\" ValueType=\"%d\"%s%s>%s</gnm:Cell>\n",
        row, col, value_type, wearing, marked, escaped);
    else
      g_string_append_printf (w->out,
        "      <gnm:Cell Row=\"%d\" Col=\"%d\"%s%s>%s</gnm:Cell>\n",
        row, col, wearing, marked, escaped);
    g_free (wearing);
    g_free (marked);
  }

  g_free (escaped);
  g_free (text);
}

/* Which cell a point `px` pixels along the columns falls in, and how far
 * into it as a fraction: Gnumeric's anchor for a picture's corner. */
static void
locate (O42Sheet *sheet, gboolean cols, double px, int *index, double *frac)
{
  int limit = cols ? O42_MAX_COLS : O42_MAX_ROWS;
  double edge = 0;

  for (int i = 0; i < limit; i++)
    {
      double size = cols ? o42_sheet_col_width (sheet, i)
                         : o42_sheet_row_height (sheet, i);

      if (px < edge + size)
        {
          *index = i;
          *frac = (px - edge) / size;
          return;
        }
      edge += size;
    }

  *index = limit - 1;
  *frac = 1.0;
}

static double
offset_px (O42Sheet *sheet, gboolean cols, int index)
{
  double px = 0;

  for (int i = 0; i < index; i++)
    px += cols ? o42_sheet_col_width (sheet, i) : o42_sheet_row_height (sheet, i);

  return px;
}

static void
write_picture (GString *out, O42Sheet *sheet, const O42Picture *pic)
{
  double x0 = offset_px (sheet, TRUE, pic->col) + pic->dx;
  double y0 = offset_px (sheet, FALSE, pic->row) + pic->dy;
  int c0, r0, c1, r1;
  double fx0, fy0, fx1, fy1;
  char *a, *b, *encoded;
  char fx0s[32], fy0s[32], fx1s[32], fy1s[32];

  locate (sheet, TRUE, x0, &c0, &fx0);
  locate (sheet, FALSE, y0, &r0, &fy0);
  locate (sheet, TRUE, x0 + pic->width, &c1, &fx1);
  locate (sheet, FALSE, y0 + pic->height, &r1, &fy1);

  a = o42_ref_name (r0, c0);
  b = o42_ref_name (r1, c1);
  g_ascii_dtostr (fx0s, sizeof fx0s, fx0);
  g_ascii_dtostr (fy0s, sizeof fy0s, fy0);
  g_ascii_dtostr (fx1s, sizeof fx1s, fx1);
  g_ascii_dtostr (fy1s, sizeof fy1s, fy1);

  g_string_append_printf (out,
    "      <gnm:SheetObjectImage ObjectBound=\"%s:%s\" ObjectOffset=\"%s %s %s %s\" "
    "ObjectAnchorType=\"16 16 16 16\" Direction=\"17\" "
    "crop-top=\"0\" crop-bottom=\"0\" crop-left=\"0\" crop-right=\"0\">\n",
    a, b, fx0s, fy0s, fx1s, fy1s);

  encoded = g_base64_encode (g_bytes_get_data (pic->data, NULL),
                             g_bytes_get_size (pic->data));
  g_string_append_printf (out,
    "        <gnm:Content image-type=\"%s\" size-bytes=\"%" G_GSIZE_FORMAT "\">%s</gnm:Content>\n",
    pic->format, g_bytes_get_size (pic->data), encoded);
  g_string_append (out, "      </gnm:SheetObjectImage>\n");

  g_free (encoded);
  g_free (a);
  g_free (b);
}

/* A chart as Gnumeric's SheetObjectGraph: a GogGraph tree with one
 * chart, one plot of the right type, a series per data column and the
 * ranges spelled as Gnumeric spells them.  Gnumeric's own files carry a
 * great deal more; this is the part the two programs can agree on. */
static void
write_chart (GString *out, O42Sheet *sheet, const O42Chart *chart)
{
  char *bounds, *yfmt;
  double x0 = offset_px (sheet, TRUE, chart->col) + chart->dx;
  double y0 = offset_px (sheet, FALSE, chart->row) + chart->dy;
  int c0, r0, c1, r1;
  double fx0, fy0, fx1, fy1;
  char *a, *b, *title, *sheet_name;
  char fx0s[32], fy0s[32], fx1s[32], fy1s[32];
  const char *plot = (chart->kind == O42_CHART_LINE) ? "GogLinePlot"
                   : (chart->kind == O42_CHART_STACKED || chart->kind == O42_CHART_PERCENT) ? "GogBarColPlot"
                   : (chart->kind == O42_CHART_PIE) ? "GogPiePlot"
                   : (chart->kind == O42_CHART_AREA) ? "GogAreaPlot"
                   : (chart->kind == O42_CHART_SCATTER) ? "GogXYPlot" : "GogBarColPlot";
  const O42Range *d = &chart->data;
  int row0 = d->row0 + (chart->first_row_labels ? 1 : 0);
  int col0 = d->col0 + (chart->first_col_labels ? 1 : 0);

  locate (sheet, TRUE, x0, &c0, &fx0);
  locate (sheet, FALSE, y0, &r0, &fy0);
  locate (sheet, TRUE, x0 + chart->width, &c1, &fx1);
  locate (sheet, FALSE, y0 + chart->height, &r1, &fy1);
  a = o42_ref_name (r0, c0);
  b = o42_ref_name (r1, c1);
  g_ascii_dtostr (fx0s, sizeof fx0s, fx0);
  g_ascii_dtostr (fy0s, sizeof fy0s, fy0);
  g_ascii_dtostr (fx1s, sizeof fx1s, fx1);
  g_ascii_dtostr (fy1s, sizeof fy1s, fy1);
  title = g_markup_escape_text (chart->title != NULL ? chart->title : "", -1);
  sheet_name = o42_sheet_name_quote (o42_sheet_get_name (sheet));
  yfmt = g_markup_escape_text (chart->y_format != NULL ? chart->y_format : "", -1);
  {
    /* The value axis bounds, as attributes of our own. */
    GString *bs = g_string_new (NULL);
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    if (chart->has_min) g_string_append_printf (bs, " o42-min=\"%s\"", g_ascii_dtostr (num, sizeof num, chart->min));
    if (chart->has_max) g_string_append_printf (bs, " o42-max=\"%s\"", g_ascii_dtostr (num, sizeof num, chart->max));
    bounds = g_string_free (bs, FALSE);
  }

  g_string_append_printf (out,
    "      <gnm:SheetObjectGraph ObjectBound=\"%s:%s\" ObjectOffset=\"%s %s %s %s\" "
    "ObjectAnchorType=\"16 16 16 16\" Direction=\"17\" "
    "o42-kind=\"%s\" o42-first-row-labels=\"%d\" o42-first-col-labels=\"%d\" "
    "o42-series-in-rows=\"%d\" o42-legend=\"%d\" o42-gridlines=\"%d\" o42-labels=\"%d\" "
    "o42-trend=\"%s\" o42-trend-order=\"%d\" o42-errbars=\"%s\" o42-errvalue=\"%g\" "
    "o42-font=\"%s\" o42-fontsize=\"%g\" o42-data-sheet=\"%s\" o42-3d=\"%d\" o42-group=\"%u\" "
    "o42-yformat=\"%s\" o42-secondary=\"%d\" "
    "o42-marker=\"%s\" o42-marker-size=\"%g\" o42-marker-picture=\"%u\"%s>\n"
    "        <gnm:GogObject type=\"GogGraph\">\n"
    "          <GogObject role=\"Chart\" type=\"GogChart\">\n",
    a, b, fx0s, fy0s, fx1s, fy1s, o42_chart_kind_name (chart->kind),
    chart->first_row_labels, chart->first_col_labels, chart->series_in_rows,
    chart->legend ? 1 : 0, chart->gridlines ? 1 : 0,
    chart->data_labels ? 1 : 0, o42_trend_kind_name (chart->trend), chart->trend_order,
    o42_errbar_kind_name (chart->err_bars), chart->err_value,
    chart->font_family != NULL ? chart->font_family : "", chart->font_size,
    chart->data_sheet != NULL ? chart->data_sheet : "", chart->three_d ? 1 : 0, chart->group,
    yfmt, chart->secondary_from,
    o42_marker_kind_name (chart->marker), chart->marker_size,
    chart->marker_picture, bounds);

  if (*title != '\0')
    g_string_append_printf (out,
      "            <GogObject role=\"Title\" type=\"GogLabel\">\n"
      "              <data><dimension id=\"0\">&quot;%s&quot;</dimension></data>\n"
      "            </GogObject>\n", title);

  /* Axes with their titles and gridlines, and the legend, as Gnumeric
   * keeps them; a pie has none. */
  if (chart->kind != O42_CHART_PIE)
    {
      const char *roles[2] = { "X-Axis", "Y-Axis" };
      const char *titles[2] = { chart->x_title, chart->y_title };
      for (int i = 0; i < 2; i++)
        {
          g_string_append_printf (out, "            <GogObject role=\"%s\" type=\"GogAxis\">\n", roles[i]);
          if (titles[i] != NULL && *titles[i] != '\0')
            {
              char *t = g_markup_escape_text (titles[i], -1);
              g_string_append_printf (out,
                "              <GogObject role=\"Label\" type=\"GogLabel\">\n"
                "                <data><dimension id=\"0\">&quot;%s&quot;</dimension></data>\n"
                "              </GogObject>\n", t);
              g_free (t);
            }
          if (i == 1 && chart->gridlines)
            g_string_append (out, "              <GogObject role=\"MajorGrid\" type=\"GogGridLine\"/>\n");
          g_string_append (out, "            </GogObject>\n");
        }
    }
  if (chart->legend)
    g_string_append (out, "            <GogObject role=\"Legend\" type=\"GogLegend\"/>\n");

  g_string_append_printf (out, "            <GogObject role=\"Plot\" type=\"%s\">\n", plot);
  if (chart->kind == O42_CHART_COLUMN || chart->kind == O42_CHART_BAR ||
      chart->kind == O42_CHART_STACKED || chart->kind == O42_CHART_PERCENT)
    g_string_append_printf (out, "              <property name=\"horizontal\">%s</property>\n"
                                 "              <property name=\"type\">%s</property>\n",
                            chart->kind == O42_CHART_BAR ? "true" : "false",
                            chart->kind == O42_CHART_STACKED ? "stacked"
                            : chart->kind == O42_CHART_PERCENT ? "as_percentage" : "normal");

  /* A series per column, or per row when they lie that way.  The heading
   * that names the series and the one that names the categories swap with
   * the orientation, exactly as they do when the chart is drawn. */
  {
    int first = chart->series_in_rows ? row0 : col0;
    int last  = chart->series_in_rows ? d->row1 : d->col1;
    gboolean series_named = chart->series_in_rows ? chart->first_col_labels
                                                  : chart->first_row_labels;
    gboolean points_named = chart->series_in_rows ? chart->first_row_labels
                                                  : chart->first_col_labels;

    for (int i = first; i <= last; i++)
      {
        char *v0 = chart->series_in_rows ? o42_ref_name_full (i, col0, TRUE, TRUE)
                                         : o42_ref_name_full (row0, i, TRUE, TRUE);
        char *v1 = chart->series_in_rows ? o42_ref_name_full (i, d->col1, TRUE, TRUE)
                                         : o42_ref_name_full (d->row1, i, TRUE, TRUE);

        g_string_append (out, "              <GogObject role=\"Series\" type=\"GogSeries\">\n"
                              "                <data>\n");
        if (points_named)
          {
            char *l0 = chart->series_in_rows ? o42_ref_name_full (d->row0, col0, TRUE, TRUE)
                                             : o42_ref_name_full (row0, d->col0, TRUE, TRUE);
            char *l1 = chart->series_in_rows ? o42_ref_name_full (d->row0, d->col1, TRUE, TRUE)
                                             : o42_ref_name_full (d->row1, d->col0, TRUE, TRUE);
            g_string_append_printf (out, "                  <dimension id=\"0\">%s!%s:%s</dimension>\n",
                                    sheet_name, l0, l1);
            g_free (l0); g_free (l1);
          }
        g_string_append_printf (out, "                  <dimension id=\"1\">%s!%s:%s</dimension>\n",
                                sheet_name, v0, v1);
        if (series_named)
          {
            char *n = chart->series_in_rows ? o42_ref_name_full (i, d->col0, TRUE, TRUE)
                                            : o42_ref_name_full (d->row0, i, TRUE, TRUE);
            g_string_append_printf (out, "                  <dimension id=\"-1\">%s!%s</dimension>\n",
                                    sheet_name, n);
            g_free (n);
          }
        g_string_append (out, "                </data>\n              </GogObject>\n");
        g_free (v0); g_free (v1);
      }
  }

  g_string_append (out,
    "            </GogObject>\n"
    "            <GogObject role=\"Legend\" type=\"GogLegend\"/>\n"
    "          </GogObject>\n"
    "        </gnm:GogObject>\n"
    "      </gnm:SheetObjectGraph>\n");

  g_free (a); g_free (b); g_free (bounds); g_free (yfmt);
  g_free (title); g_free (sheet_name);
}

static gboolean
write_gzipped (GFile *file, const char *data, gsize length, GError **error)
{
  GFileOutputStream *raw;
  GZlibCompressor *compressor;
  GOutputStream *zipped;
  gboolean ok;

  raw = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
  if (raw == NULL)
    return FALSE;

  compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
  zipped = g_converter_output_stream_new (G_OUTPUT_STREAM (raw),
                                          G_CONVERTER (compressor));

  ok = g_output_stream_write_all (zipped, data, length, NULL, NULL, error) &&
       g_output_stream_close (zipped, NULL, error);

  g_object_unref (zipped);
  g_object_unref (compressor);
  g_object_unref (raw);

  return ok;
}

static void
write_sheet (GString *out, O42Sheet *sheet)
{
  Writer w;
  O42Range used;
  O42FmtTable *table;
  char *name;

  w.out = out;
  w.keys = g_array_new (FALSE, FALSE, sizeof (guint64));
  table = o42_sheet_fmt_table (sheet);
  w.default_fmt = o42_fmt_table_default (table);
  o42_sheet_used_range (sheet, &used);
  name = g_markup_escape_text (o42_sheet_get_name (sheet), -1);

  g_string_append_printf (w.out,
    "    <gnm:Sheet DisplayFormulas=\"0\" HideZero=\"0\" HideGrid=\"0\" HideColHeader=\"0\" "
    "HideRowHeader=\"0\" DisplayOutlines=\"1\" OutlineSymbolsBelow=\"1\" OutlineSymbolsRight=\"1\" "
    "Visibility=\"GNM_SHEET_VISIBILITY_VISIBLE\" GridColor=\"0:0:0\" o42-Protected=\"%d\" "
    "o42-chart-sheet=\"%d\" o42-tab-colour=\"%u\" o42-password=\"%u\">\n"
    "      <gnm:Name>%s</gnm:Name>\n"
    "      <gnm:MaxCol>%d</gnm:MaxCol>\n      <gnm:MaxRow>%d</gnm:MaxRow>\n"
    "      <gnm:Zoom>1</gnm:Zoom>\n",
    o42_sheet_protected (sheet) ? 1 : 0, o42_sheet_is_chart_sheet (sheet) ? 1 : 0,
    o42_sheet_tab_colour (sheet), o42_sheet_password_hash (sheet),
    name, used.col1, used.row1);

  /* The print setup, in Gnumeric's own element and codes; the print
   * area is the sheet-level name Print_Area, as Gnumeric keeps it. */
  {
    const O42PrintSetup *ps = o42_sheet_print_setup (sheet);
    char *hl, *hc, *hr, *fl, *fc, *fr;

    if (ps->has_area)
      {
        char *a = o42_ref_name_full (ps->area.row0, ps->area.col0, TRUE, TRUE);
        char *b = o42_ref_name_full (ps->area.row1, ps->area.col1, TRUE, TRUE);
        g_string_append_printf (w.out,
          "      <gnm:Names>\n        <gnm:Name>\n          <gnm:name>Print_Area</gnm:name>\n"
          "          <gnm:value>%s:%s</gnm:value>\n          <gnm:position>A1</gnm:position>\n        </gnm:Name>\n      </gnm:Names>\n", a, b);
        g_free (a); g_free (b);
      }
    hf_split_gnumeric (ps->header, &hl, &hc, &hr);
    hf_split_gnumeric (ps->footer, &fl, &fc, &fr);
    g_string_append_printf (w.out,
      "      <gnm:PrintInformation>\n"
      "        <gnm:Scale type=\"percentage\" percentage=\"%d\"/>\n"
      "        <gnm:vcenter value=\"0\"/>\n        <gnm:hcenter value=\"0\"/>\n"
      "        <gnm:grid value=\"%d\"/>\n        <gnm:even_if_only_styles value=\"0\"/>\n"
      "        <gnm:monochrome value=\"0\"/>\n        <gnm:draft value=\"0\"/>\n"
      "        <gnm:titles value=\"%d\"/>\n"
      "        <gnm:o42-Print Scale=\"%d\" FitWide=\"%d\" FitTall=\"%d\" Margin=\"%g\"/>\n",
      ps->scale, ps->gridlines ? 1 : 0, ps->headings ? 1 : 0,
      ps->scale, ps->fit_wide, ps->fit_tall, ps->margin);
    {
      GArray *rb = o42_sheet_page_breaks (sheet, TRUE);
      GArray *cb = o42_sheet_page_breaks (sheet, FALSE);
      for (guint i = 0; i < rb->len; i++)
        g_string_append_printf (w.out, "        <gnm:o42-PageBreak Rows=\"1\" At=\"%d\"/>\n",
                                g_array_index (rb, int, i));
      for (guint i = 0; i < cb->len; i++)
        g_string_append_printf (w.out, "        <gnm:o42-PageBreak Rows=\"0\" At=\"%d\"/>\n",
                                g_array_index (cb, int, i));
    }
    if (ps->title_rows > 0)
      g_string_append_printf (w.out, "        <gnm:repeat_top value=\"A1:IV%d\"/>\n", ps->title_rows);
    g_string_append_printf (w.out,
      "        <gnm:order>d_then_r</gnm:order>\n        <gnm:orientation>landscape</gnm:orientation>\n"
      "        <gnm:Header Left=\"%s\" Middle=\"%s\" Right=\"%s\"/>\n"
      "        <gnm:Footer Left=\"%s\" Middle=\"%s\" Right=\"%s\"/>\n"
      "        <gnm:paper>na_letter</gnm:paper>\n"
      "      </gnm:PrintInformation>\n",
      hl, hc, hr, fl, fc, fr);
    g_free (hl); g_free (hc); g_free (hr); g_free (fl); g_free (fc); g_free (fr);
  }

  /* Styles: the default over the whole sheet, then one region per cell
   * that differs.  Gnumeric merges regions itself on loading. */
  g_string_append (w.out, "      <gnm:Styles>\n");
  {
    O42Range all = { 0, 0, O42_MAX_ROWS - 1, O42_MAX_COLS - 1 };
    append_style (w.out, o42_fmt_table_get (table, w.default_fmt), &all);
  }

  o42_sheet_foreach_cell (sheet, collect_key, &w);
  g_array_sort (w.keys, compare_keys);

  for (guint i = 0; i < w.keys->len; i++)
    {
      guint64 key = g_array_index (w.keys, guint64, i);
      int row = o42_key_row (key), col = o42_key_col (key);
      O42FmtIdx idx = o42_sheet_get_fmt_idx (sheet, row, col);

      if (idx != w.default_fmt)
        {
          O42Range one = { row, col, row, col };
          append_style (w.out, o42_fmt_table_get (table, idx), &one);
        }
    }
  /* Conditional formats: Gnumeric puts gnm:Condition elements inside a
   * StyleRegion's Style, each carrying the style to switch to. */
  {
    GArray *conds = o42_sheet_conditions (sheet);

    for (guint i = 0; i < conds->len; i++)
      {
        const O42Condition *c = &g_array_index (conds, O42Condition, i);
        char v0[G_ASCII_DTOSTR_BUF_SIZE], v1[G_ASCII_DTOSTR_BUF_SIZE];
        O42Fmt shown = *o42_fmt_table_get (table, w.default_fmt);

        o42_fmt_apply_mask (&shown, c->mask, &c->fmt);
        g_ascii_dtostr (v0, sizeof v0, c->value);
        g_ascii_dtostr (v1, sizeof v1, c->value2);

        g_string_append_printf (w.out,
          "      <gnm:StyleRegion startCol=\"%d\" startRow=\"%d\" endCol=\"%d\" endRow=\"%d\">\n"
          "        <gnm:Style o42-conditional=\"1\">\n"
          "          <gnm:Condition Operator=\"%d\" Value0=\"%s\" Value1=\"%s\" o42-mask=\"%u\">\n",
          c->range.col0, c->range.row0, c->range.col1, c->range.row1,
          (int) c->op, v0, v1, (unsigned) c->mask);
        {
          /* The style inside is written the ordinary way, indented a
           * little wrongly, which XML does not mind. */
          O42Range whole = c->range;
          GString *inner = g_string_new (NULL);
          char *body;
          const char *start, *end;

          append_style (inner, &shown, &whole);
          /* Keep only the <gnm:Style ...> ... </gnm:Style> part. */
          start = strstr (inner->str, "<gnm:Style ");
          end = strstr (inner->str, "</gnm:Style>");
          body = (start != NULL && end != NULL)
                 ? g_strndup (start, (gsize) (end - start) + strlen ("</gnm:Style>")) : g_strdup ("");
          g_string_append (w.out, body);
          g_string_append_c (w.out, '\n');
          g_free (body);
          g_string_free (inner, TRUE);
        }
        g_string_append (w.out,
          "          </gnm:Condition>\n"
          "        </gnm:Style>\n"
          "      </gnm:StyleRegion>\n");
      }
  }

  /* Validation rules: Gnumeric puts a gnm:Validation inside a style. */
  {
    GArray *rules = o42_sheet_validations (sheet);

    for (guint i = 0; i < rules->len; i++)
      {
        const O42Validation *v = &g_array_index (rules, O42Validation, i);
        char *message = g_markup_escape_text (v->message ? v->message : "", -1);
        char *e0, *e1;

        if (v->kind == O42_VALID_LIST)
          {
            char *quoted = g_strdup_printf ("\"%s\"", v->value ? v->value : "");
            e0 = g_markup_escape_text (quoted, -1);
            g_free (quoted);
          }
        else
          e0 = g_markup_escape_text (v->value ? v->value : "", -1);
        e1 = g_markup_escape_text (v->value2 ? v->value2 : "", -1);

        g_string_append_printf (w.out,
          "      <gnm:StyleRegion startCol=\"%d\" startRow=\"%d\" endCol=\"%d\" endRow=\"%d\">\n"
          "        <gnm:Style o42-validation=\"1\">\n"
          "          <gnm:Validation Style=\"1\" Type=\"%d\" Operator=\"%d\" AllowBlank=\"%d\" "
          "UseDropdown=\"%d\" Title=\"\" Message=\"%s\">\n"
          "            <gnm:Expression0>%s</gnm:Expression0>\n",
          v->range.col0, v->range.row0, v->range.col1, v->range.row1,
          (int) v->kind, (int) v->op, v->allow_blank ? 1 : 0, v->kind == O42_VALID_LIST ? 1 : 0,
          message, e0);
        if (v->value2 != NULL && v->value2[0] != '\0')
          g_string_append_printf (w.out, "            <gnm:Expression1>%s</gnm:Expression1>\n", e1);
        g_string_append (w.out,
          "          </gnm:Validation>\n"
          "        </gnm:Style>\n"
          "      </gnm:StyleRegion>\n");
        g_free (message);
        g_free (e0);
        g_free (e1);
      }
  }
  /* Hyperlinks: a region per linked cell, its style with a HyperLink
   * child, as Gnumeric keeps them. */
  {
    GHashTableIter lit;
    gpointer lk, lv;
    g_hash_table_iter_init (&lit, o42_sheet_links (sheet));
    while (g_hash_table_iter_next (&lit, &lk, &lv))
      {
        guint64 key = *(guint64 *) lk;
        const char *target = lv;
        O42Range one = { o42_key_row (key), o42_key_col (key), o42_key_row (key), o42_key_col (key) };
        GString *region = g_string_new (NULL);
        const char *tail = "        </gnm:Style>\n      </gnm:StyleRegion>\n";
        char *escaped = g_markup_escape_text (target[0] == '#' ? target + 1 : target, -1);
        const char *type = target[0] == '#' ? "GnmHLinkCurWorkbook"
                         : g_str_has_prefix (target, "mailto:") ? "GnmHLinkEMail" : "GnmHLinkURL";

        append_style (region, o42_sheet_get_fmt (sheet, one.row0, one.col0), &one);
        if (g_str_has_suffix (region->str, tail))
          g_string_truncate (region, region->len - strlen (tail));
        g_string_append_printf (region, "          <gnm:HyperLink type=\"%s\" target=\"%s\"/>\n%s", type, escaped, tail);
        g_string_append (w.out, region->str);
        g_string_free (region, TRUE);
        g_free (escaped);
      }
  }
  g_string_append (w.out, "      </gnm:Styles>\n");

  /* Column widths and row heights that are not the default. */
  g_string_append_printf (w.out, "      <gnm:Cols DefaultSizePts=\"%g\">\n",
                          PX_TO_PT (o42_sheet_col_width (sheet, O42_MAX_COLS - 1)));
  for (int col = 0; col <= used.col1; col++)
    {
      gboolean hidden = o42_sheet_col_hidden (sheet, col);
      int width = o42_sheet_col_width (sheet, col);

      int level = o42_sheet_col_level (sheet, col);
      char outline[40];

      g_snprintf (outline, sizeof outline, level > 0 ? " OutlineLevel=\"%d\"" : "", level);
      if (hidden)
        g_string_append_printf (w.out, "        <gnm:ColInfo No=\"%d\" Unit=\"%g\" Hidden=\"1\"%s/>\n",
                                col, PX_TO_PT (80), outline);
      else if (width != o42_sheet_col_width (sheet, O42_MAX_COLS - 1) || level > 0)
        g_string_append_printf (w.out, "        <gnm:ColInfo No=\"%d\" Unit=\"%g\"%s/>\n",
                                col, PX_TO_PT (width), outline);
    }
  g_string_append (w.out, "      </gnm:Cols>\n");

  g_string_append_printf (w.out, "      <gnm:Rows DefaultSizePts=\"%g\">\n",
                          PX_TO_PT (o42_sheet_row_height (sheet, O42_MAX_ROWS - 1)));
  for (int row = 0; row <= used.row1; row++)
    {
      gboolean hidden = o42_sheet_row_hidden_by_hand (sheet, row);
      int height = o42_sheet_row_height (sheet, row);

      int level = o42_sheet_row_level (sheet, row);
      char outline[40];

      g_snprintf (outline, sizeof outline, level > 0 ? " OutlineLevel=\"%d\"" : "", level);
      if (hidden)
        g_string_append_printf (w.out, "        <gnm:RowInfo No=\"%d\" Unit=\"%g\" Hidden=\"1\"%s/>\n",
                                row, PX_TO_PT (20), outline);
      else if (height != o42_sheet_row_height (sheet, O42_MAX_ROWS - 1) || level > 0)
        g_string_append_printf (w.out, "        <gnm:RowInfo No=\"%d\" Unit=\"%g\"%s/>\n",
                                row, PX_TO_PT (height), outline);
    }
  g_string_append (w.out, "      </gnm:Rows>\n");

  /* Pivot tables: office42's own element, since Gnumeric has none. */
  for (int i = 0; i < o42_sheet_n_scenarios (sheet); i++)
    {
      const char *sname = o42_sheet_scenario_name (sheet, i);
      GArray *keys = NULL;
      GPtrArray *values = NULL;
      const char *comment = NULL;
      char *ename, *ecomment;

      if (!o42_sheet_scenario_cells (sheet, sname, &keys, &values, &comment))
        continue;
      ename = g_markup_escape_text (sname, -1);
      ecomment = g_markup_escape_text (comment != NULL ? comment : "", -1);
      g_string_append_printf (w.out, "      <gnm:o42-Scenario Name=\"%s\" Comment=\"%s\">\n", ename, ecomment);
      for (guint k = 0; k < keys->len && k < values->len; k++)
        {
          guint64 key = g_array_index (keys, guint64, k);
          char *ref = o42_ref_name (o42_key_row (key), o42_key_col (key));
          char *val = g_markup_escape_text (g_ptr_array_index (values, k), -1);
          g_string_append_printf (w.out, "        <gnm:o42-ScenarioCell Ref=\"%s\" Value=\"%s\"/>\n", ref, val);
          g_free (ref);
          g_free (val);
        }
      g_string_append (w.out, "      </gnm:o42-Scenario>\n");
      g_free (ename);
      g_free (ecomment);
    }

  {
    GPtrArray *shapes = o42_sheet_shapes (sheet);
    for (guint i = 0; i < shapes->len; i++)
      {
        const O42Shape *sh = g_ptr_array_index (shapes, i);
        char *at = o42_ref_name (sh->row, sh->col);
        char *body = g_markup_escape_text (sh->text != NULL ? sh->text : "", -1);
        char *control = NULL;

        /* A form control carries the cell it drives and the rest of
         * what makes it one; a plain shape writes none of it. */
        if (o42_shape_is_control (sh->kind))
          {
            char *link = g_markup_escape_text (sh->link != NULL ? sh->link : "", -1);
            char *source = g_markup_escape_text (sh->source != NULL ? sh->source : "", -1);
            char *script = g_markup_escape_text (sh->script != NULL ? sh->script : "", -1);

            control = g_strdup_printf (
              " Link=\"%s\" Source=\"%s\" Script=\"%s\" Value=\"%g\" "
              "Min=\"%g\" Max=\"%g\" Step=\"%g\" Page=\"%g\"",
              link, source, script, sh->value, sh->min, sh->max, sh->step, sh->page);
            g_free (link);
            g_free (source);
            g_free (script);
          }

        g_string_append_printf (w.out,
          "      <gnm:o42-Shape Kind=\"%s\" At=\"%s\" Dx=\"%g\" Dy=\"%g\" W=\"%g\" H=\"%g\" "
          "Fill=\"%u\" Line=\"%u\" LineWidth=\"%g\" Group=\"%u\"%s>%s</gnm:o42-Shape>\n",
          o42_shape_kind_name (sh->kind), at, sh->dx, sh->dy, sh->width, sh->height,
          (guint) sh->fill, (guint) sh->line, sh->line_width, sh->group,
          control != NULL ? control : "", body);
        g_free (control);
        g_free (at);
        g_free (body);
      }
  }

  {
    GArray *queries = o42_sheet_queries (sheet);
    for (guint i = 0; i < queries->len; i++)
      {
        const O42Query *q = &g_array_index (queries, O42Query, i);
        char *at = o42_ref_name (q->at.row0, q->at.col0);
        char *to = o42_ref_name (q->at.row1, q->at.col1);
        char *esql = g_markup_escape_text (q->sql, -1);

        g_string_append_printf (w.out,
          "      <gnm:o42-Query At=\"%s:%s\" Headings=\"%d\">%s</gnm:o42-Query>\n",
          at, to, q->headings ? 1 : 0, esql);
        g_free (at);
        g_free (to);
        g_free (esql);
      }
  }

  {
    GArray *tables = o42_sheet_tables (sheet);
    for (guint i = 0; i < tables->len; i++)
      {
        const O42Table *t = &g_array_index (tables, O42Table, i);
        char *tname = g_markup_escape_text (t->name, -1);
        char *a1 = o42_ref_name (t->range.row0, t->range.col0);
        char *b1 = o42_ref_name (t->range.row1, t->range.col1);
        g_string_append_printf (w.out,
          "      <gnm:o42-Table Name=\"%s\" Range=\"%s:%s\" Headers=\"%d\"/>\n",
          tname, a1, b1, t->has_headers ? 1 : 0);
        g_free (tname); g_free (a1); g_free (b1);
      }
  }

  {
    GArray *pivots = o42_sheet_pivots (sheet);
    for (guint i = 0; i < pivots->len; i++)
      {
        const O42Pivot *p = &g_array_index (pivots, O42Pivot, i);
        char *src = g_markup_escape_text (p->source_sheet ? p->source_sheet : "", -1);
        char *rfs = o42_pivot_fields_to_string (p->row_fields);
        char *cfs = o42_pivot_fields_to_string (p->col_fields);
        char *rf = g_markup_escape_text (rfs, -1);
        char *cf = g_markup_escape_text (cfs, -1);
        g_free (rfs);
        g_free (cfs);
        char *df = g_markup_escape_text (p->data_field ? p->data_field : "", -1);
        char *ff = g_markup_escape_text (p->filter_field ? p->filter_field : "", -1);
        char *fv = g_markup_escape_text (p->filter_value ? p->filter_value : "", -1);
        char *a1 = o42_ref_name (p->source.row0, p->source.col0);
        char *b1 = o42_ref_name (p->source.row1, p->source.col1);
        char *at = o42_ref_name (p->row, p->col);
        g_string_append_printf (w.out,
          "      <gnm:o42-Pivot Source=\"%s\" Range=\"%s:%s\" RowField=\"%s\" ColField=\"%s\" "
          "DataField=\"%s\" Agg=\"%d\" At=\"%s\" Rows=\"%d\" Cols=\"%d\" Filter=\"%s\" FilterValue=\"%s\"/>\n",
          src, a1, b1, rf, cf, df, (int) p->agg, at, p->rows, p->cols, ff, fv);
        g_free (src); g_free (rf); g_free (cf); g_free (df); g_free (a1); g_free (b1); g_free (at);
        g_free (ff); g_free (fv);
      }
  }

  {
    O42Range filter;

    if (o42_sheet_get_autofilter (sheet, &filter))
      {
        char *a = o42_ref_name (filter.row0, filter.col0);
        char *b = o42_ref_name (filter.row1, filter.col1);
        g_string_append_printf (w.out,
          "      <gnm:Filters>\n        <gnm:Filter Area=\"%s:%s\">\n", a, b);
        for (int col = filter.col0; col <= filter.col1; col++)
          {
            const char *choice = o42_sheet_autofilter_choice (sheet, col);
            if (choice != NULL)
              {
                /* Gnumeric's operators; a wildcard pattern stays a text
                 * to equal, which Gnumeric also matches as a pattern. */
                static const char *prefixes[] = { "<>", ">=", "<=", "=", ">", "<" };
                static const char *ops[] = { "ne", "gte", "lte", "eq", "gt", "lt" };
                const char *op = "eq", *value = choice;
                char *esc;
                for (guint k = 0; k < G_N_ELEMENTS (prefixes); k++)
                  if (g_str_has_prefix (choice, prefixes[k]))
                    { op = ops[k]; value = choice + strlen (prefixes[k]); break; }
                esc = g_markup_escape_text (value, -1);
                g_string_append_printf (w.out,
                  "          <gnm:Field Index=\"%d\" Type=\"value\" Op0=\"%s\" Value0=\"%s\" ValueType0=\"%d\"/>\n",
                  col - filter.col0, op, esc, g_ascii_strtod (value, NULL) != 0 || value[0] == '0' ? 40 : 60);
                g_free (esc);
              }
          }
        g_string_append (w.out, "        </gnm:Filter>\n      </gnm:Filters>\n");
        g_free (a);
        g_free (b);
      }
  }

  {
    GArray *merges = o42_sheet_merges (sheet);

    if (merges->len > 0)
      {
        g_string_append (w.out, "      <gnm:MergedRegions>\n");
        for (guint i = 0; i < merges->len; i++)
          {
            const O42Range *m = &g_array_index (merges, O42Range, i);
            char *a = o42_ref_name (m->row0, m->col0), *b = o42_ref_name (m->row1, m->col1);
            g_string_append_printf (w.out, "        <gnm:Merge>%s:%s</gnm:Merge>\n", a, b);
            g_free (a); g_free (b);
          }
        g_string_append (w.out, "      </gnm:MergedRegions>\n");
      }
  }

  {
    int frozen_rows = 0, frozen_cols = 0;

    o42_sheet_get_frozen (sheet, &frozen_rows, &frozen_cols);
    if (frozen_rows > 0 || frozen_cols > 0)
      {
        char *unfrozen = o42_ref_name (frozen_rows, frozen_cols);
        g_string_append_printf (w.out,
          "      <gnm:SheetLayout TopLeft=\"A1\">\n"
          "        <gnm:FreezePanes FrozenTopLeft=\"A1\" UnfrozenTopLeft=\"%s\"/>\n"
          "      </gnm:SheetLayout>\n", unfrozen);
        g_free (unfrozen);
      }
  }

  g_string_append (w.out,
    "      <gnm:Selections CursorCol=\"0\" CursorRow=\"0\">\n"
    "        <gnm:Selection startCol=\"0\" startRow=\"0\" endCol=\"0\" endRow=\"0\"/>\n"
    "      </gnm:Selections>\n");

  {
    GPtrArray *pictures = o42_sheet_pictures (sheet);
    GPtrArray *charts = o42_sheet_charts (sheet);

    GHashTable *notes = o42_sheet_notes (sheet);

    if (pictures->len > 0 || charts->len > 0 || g_hash_table_size (notes) > 0)
      {
        GHashTableIter iter;
        gpointer key_ptr, value;

        g_string_append (w.out, "      <gnm:Objects>\n");
        for (guint i = 0; i < pictures->len; i++)
          write_picture (w.out, sheet, g_ptr_array_index (pictures, i));
        for (guint i = 0; i < charts->len; i++)
          write_chart (w.out, sheet, g_ptr_array_index (charts, i));

        g_hash_table_iter_init (&iter, notes);
        while (g_hash_table_iter_next (&iter, &key_ptr, &value))
          {
            guint64 key = *(guint64 *) key_ptr;
            char *at = o42_ref_name (o42_key_row (key), o42_key_col (key));
            char *text = g_markup_escape_text (value, -1);

            g_string_append_printf (w.out,
              "      <gnm:Comment ObjectBound=\"%s\" ObjectOffset=\"0 0 0 0\" "
              "ObjectAnchorType=\"16 16 16 16\" Direction=\"17\" Text=\"%s\"/>\n",
              at, text);
            g_free (at);
            g_free (text);
          }
        g_string_append (w.out, "      </gnm:Objects>\n");
      }
  }

  g_string_append (w.out, "      <gnm:Cells>\n");
  for (guint i = 0; i < w.keys->len; i++)
    {
      guint64 key = g_array_index (w.keys, guint64, i);
      write_cell (&w, sheet, o42_key_row (key), o42_key_col (key));
    }
  g_string_append (w.out,
    "      </gnm:Cells>\n"
    "    </gnm:Sheet>\n");

  g_array_free (w.keys, TRUE);
  g_free (name);
}

gboolean
o42_gnumeric_save (O42Book *book, GFile *file, GError **error)
{
  GString *out;
  gboolean ok;
  int n;

  g_return_val_if_fail (book != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  n = o42_book_n_sheets (book);
  out = g_string_new (NULL);

  {
    int iteration_max = 100;
    double iteration_tolerance = 0.001;
    gboolean iteration_on = o42_book_iteration (book, &iteration_max,
                                                &iteration_tolerance);

    g_string_append_printf (out,
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<gnm:Workbook xmlns:gnm=\"http://www.gnumeric.org/v10.dtd\" "
    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
    "xsi:schemaLocation=\"http://www.gnumeric.org/v9.xsd\">\n"
    "  <gnm:Version Epoch=\"1\" Major=\"12\" Minor=\"50\" Full=\"1.12.50\"/>\n"
    "  <gnm:Calculation ManualRecalc=\"%d\" EnableIteration=\"%d\" "
    "MaxIterations=\"%d\" IterationTolerance=\"%g\" FloatRadix=\"2\" FloatDigits=\"53\"/>\n"
    "  <gnm:SheetNameIndex>\n",
    o42_book_manual (book) ? 1 : 0, iteration_on ? 1 : 0,
    iteration_max, iteration_tolerance);
  }
  for (int i = 0; i < n; i++)
    {
      char *name = g_markup_escape_text (o42_sheet_get_name (o42_book_sheet (book, i)), -1);
      g_string_append_printf (out,
        "    <gnm:SheetName gnm:Cols=\"%d\" gnm:Rows=\"%d\">%s</gnm:SheetName>\n",
        O42_MAX_COLS, O42_MAX_ROWS, name);
      g_free (name);
    }
  g_string_append (out,
    "  </gnm:SheetNameIndex>\n"
    "  <gnm:Geometry Width=\"960\" Height=\"700\"/>\n");

  {
    GList *names = o42_book_names (book);

    if (names != NULL)
      g_string_append (out, "  <gnm:Names>\n");
    for (GList *l = names; l != NULL; l = l->next)
      {
        O42Sheet *target = NULL;
        O42Range range;

        if (o42_book_lookup_name (book, l->data, &target, &range))
          {
            char *sheet_name = o42_sheet_name_quote (o42_sheet_get_name (target));
            char *a = o42_ref_name_full (range.row0, range.col0, TRUE, TRUE);
            char *b = o42_ref_name_full (range.row1, range.col1, TRUE, TRUE);
            char *esc = g_markup_escape_text (l->data, -1);

            g_string_append_printf (out,
              "    <gnm:Name>\n      <gnm:name>%s</gnm:name>\n"
              "      <gnm:value>%s!%s:%s</gnm:value>\n      <gnm:position>A1</gnm:position>\n"
              "    </gnm:Name>\n", esc, sheet_name, a, b);
            g_free (sheet_name); g_free (a); g_free (b); g_free (esc);
          }
      }
    if (names != NULL)
      g_string_append (out, "  </gnm:Names>\n");
    g_list_free (names);
  }

  if (o42_book_n_styles (book) > 0)
    {
      /* The book's cell styles, in an element of our own: each holds a
       * gnm:Style like a region's, and the mask says which of its
       * properties the style owns. */
      g_string_append (out, "  <gnm:o42-CellStyles>\n");
      for (int i = 0; i < o42_book_n_styles (book); i++)
        {
          const char *sname = o42_book_style_name (book, i);
          O42Fmt fmt;
          O42FmtMask mask = 0;
          char *escaped = g_markup_escape_text (sname, -1);
          O42Range one = { 0, 0, 0, 0 };

          if (o42_book_style (book, sname, &fmt, &mask))
            {
              g_string_append_printf (out, "    <gnm:o42-CellStyle Name=\"%s\" Mask=\"%u\">\n", escaped, (guint) mask);
              append_style (out, &fmt, &one);
              g_string_append (out, "    </gnm:o42-CellStyle>\n");
            }
          g_free (escaped);
        }
      g_string_append (out, "  </gnm:o42-CellStyles>\n");
    }

  g_string_append (out, "  <gnm:Sheets>\n");

  for (int i = 0; i < n; i++)
    write_sheet (out, o42_book_sheet (book, i));

  g_string_append (out, "  </gnm:Sheets>\n");

  /* Custom views, in an element of our own like the scripts below. */
  if (o42_book_n_views (book) > 0)
    {
      g_string_append (out, "  <gnm:o42-Views>\n");
      for (int i = 0; i < o42_book_n_views (book); i++)
        {
          const O42BookView *view = o42_book_view (book, i);
          char *vname = g_markup_escape_text (view->name, -1);
          char *vsheet = g_markup_escape_text (view->sheet != NULL ? view->sheet : "", -1);

          g_string_append_printf (out,
            "    <gnm:o42-View Name=\"%s\" Sheet=\"%s\" Selection=\"%d %d %d %d\" "
            "Active=\"%d %d\" Zoom=\"%g\" Frozen=\"%d %d\" Split=\"%d\"/>\n",
            vname, vsheet, view->selection.row0, view->selection.col0,
            view->selection.row1, view->selection.col1,
            view->active_row, view->active_col, view->zoom,
            view->frozen_rows, view->frozen_cols, view->split ? 1 : 0);
          g_free (vname);
          g_free (vsheet);
        }
      g_string_append (out, "  </gnm:o42-Views>\n");
    }

  /* The book's database: a path to a file beside it, or the file
   * itself, carried in the book so that it travels with it. */
  {
    gboolean embedded = FALSE;
    const char *db = o42_book_database (book, &embedded);

    if (db != NULL && !embedded)
      {
        char *epath = g_markup_escape_text (db, -1);

        g_string_append_printf (out, "  <gnm:o42-Database Path=\"%s\"/>\n", epath);
        g_free (epath);
      }
    else if (db != NULL)
      {
        char *data = NULL;
        gsize length = 0;

        if (g_file_get_contents (db, &data, &length, NULL))
          {
            char *base64 = g_base64_encode ((const guchar *) data, length);

            g_string_append_printf (out,
              "  <gnm:o42-Database Embedded=\"1\">%s</gnm:o42-Database>\n", base64);
            g_free (base64);
          }
        g_free (data);
      }
  }

  /* The lists the fill handle continues, one element apiece: Excel
   * keeps these in its options, and office42 with the book, so that a
   * book that needs them carries them. */
  {
    GPtrArray *lists = o42_book_custom_lists (book);

    for (guint i = 0; i < lists->len; i++)
      {
        char *joined = g_strjoinv ("\t", g_ptr_array_index (lists, i));
        char *escaped = g_markup_escape_text (joined, -1);

        g_string_append_printf (out, "  <gnm:o42-CustomList>%s</gnm:o42-CustomList>' + N + '", escaped);
        g_free (escaped);
        g_free (joined);
      }
  }

  /* Scripts, in an element of our own that Gnumeric passes over. */
  if (o42_book_n_scripts (book) > 0)
    {
      g_string_append (out, "  <gnm:o42-Scripts>\n");
      for (int i = 0; i < o42_book_n_scripts (book); i++)
        {
          const char *sname = o42_book_script_name (book, i);
          char *ename = g_markup_escape_text (sname, -1);
          char *ecode = g_markup_escape_text (o42_book_script_code (book, sname), -1);
          g_string_append_printf (out, "    <gnm:o42-Script Name=\"%s\">%s</gnm:o42-Script>\n", ename, ecode);
          g_free (ename);
          g_free (ecode);
        }
      g_string_append (out, "  </gnm:o42-Scripts>\n");
    }

  g_string_append (out,
    "  <gnm:UIData SelectedTab=\"0\"/>\n"
    "</gnm:Workbook>\n");

  ok = write_gzipped (file, out->str, out->len, error);
  g_string_free (out, TRUE);

  return ok;
}

/* ---------------------------------------------------------------------- */
/* Reading                                                                 */
/* ---------------------------------------------------------------------- */

typedef struct {
  O42Book    *book;
  O42Sheet   *sheet;            /* the sheet being read */
  int         sheet_index;      /* which gnm:Sheet we are in, from 1 */
  gboolean    in_name;
  GString    *name;
  gboolean    in_merge;
  GString    *merge;
  gboolean    name_on_sheet;    /* the gnm:Name being read is a sheet's */
  char       *style_link;       /* a gnm:HyperLink in the style being read */
  char       *style_name;       /* the o42-CellStyle being read */
  char       *cell_style;       /* the style the cell being read wears */
  char       *cell_runs;        /* o42-runs, the runs of its text */
  O42Shape   *shape;            /* the o42-Shape being read */
  gboolean    in_shape;
  GString    *shape_text;
  char       *scenario_name;    /* the o42-Scenario being read */
  char       *scenario_comment;
  O42FmtMask  style_mask;
  gboolean    in_names;         /* inside gnm:Names */
  gboolean    in_script;        /* gnm:o42-Script, workbook level */
  gboolean    in_database;      /* gnm:o42-Database with the file inside it */
  gboolean    in_custom_list;   /* gnm:o42-CustomList, whose text is the list */
  GString    *custom_list;
  GString    *database;         /* its base64 */
  gboolean    in_query;         /* gnm:o42-Query, sheet level */
  GString    *query_sql;
  O42Range    query_at;
  gboolean    query_headings;
  char       *script_name;
  GString    *script_code;

  /* A gnm:Validation inside a style, with its expressions. */
  O42Validation validation;
  gboolean    in_validation;
  int         expr_index;     /* 0 or 1 while inside an Expression, else -1 */
  GString    *expr;

  /* The style region being read. */
  gboolean    in_style;
  O42Range    region;
  O42Fmt      fmt;
  O42FmtMask  mask;

  /* A gnm:Condition inside it: the style that follows is the rule's. */
  gboolean    in_condition;
  O42Condition condition;
  gboolean    region_is_conditional;   /* a region we wrote for a rule only */
  GString    *font_name;
  gboolean    in_font;

  /* The cell being read. */
  gboolean    in_cell;
  int         cell_row, cell_col, cell_type, cell_expr_id;
  int         cell_rows, cell_cols;   /* an array formula's block, on its head */
  GString    *cell_text;
  GHashTable *shared_exprs;     /* ExprID -> formula text */

  /* The chart being read: the object's bounds, then the first series'
   * value range and the plot type from the Gog tree. */
  gboolean    in_graph;
  O42ChartKind graph_kind;
  gboolean    graph_kind_known;
  int         graph_first_row, graph_first_col;   /* -1 when not ours */
  int         graph_in_rows;                      /* -1 when not ours */
  O42Range    graph_data;
  gboolean    graph_data_known;
  int         graph_dimension;
  gboolean    in_dimension;
  GString    *dimension;
  GString    *graph_title;
  gboolean    in_title;
  int         graph_axis;       /* 1 in X-Axis, 2 in Y-Axis, else 0 */
  GString    *graph_x_title, *graph_y_title;
  int         graph_legend, graph_gridlines;   /* -1 unknown */
  int         graph_labels, graph_trend, graph_trend_order, graph_secondary;
  gboolean    graph_3d;
  guint       graph_group;
  char       *graph_trend_name, *graph_err_name, *graph_font, *graph_data_sheet;
  char       *graph_marker_name;
  double      graph_marker_size;
  guint       graph_marker_picture;
  double      graph_err_value, graph_font_size;
  char       *graph_yformat;
  gboolean    graph_has_min, graph_has_max;
  double      graph_min, graph_max;

  /* The picture being read. */
  gboolean    in_object;
  gboolean    in_content;
  O42Range    object_bound;
  double      object_offset[4];
  GString    *content;
  char       *content_type;

  gboolean    seen_cell;

  /* Defined names, resolved once every sheet is there. */
  gboolean    in_name_def, in_name_name, in_name_value;
  GString    *name_name, *name_value;
  GPtrArray  *pending_names;   /* alternating name, value strings */
} Reader;

static const char *
attr (const char **names, const char **values, const char *want)
{
  for (int i = 0; names[i] != NULL; i++)
    if (strcmp (names[i], want) == 0)
      return values[i];
  return NULL;
}

static int
attr_int (const char **names, const char **values, const char *want, int fallback)
{
  const char *v = attr (names, values, want);
  return (v != NULL) ? atoi (v) : fallback;
}

static double
attr_double (const char **names, const char **values, const char *want, double fallback)
{
  const char *v = attr (names, values, want);
  return (v != NULL) ? g_ascii_strtod (v, NULL) : fallback;
}

static gboolean
parse_colour (const char *text, guint32 *rgb)
{
  unsigned r, g, b;

  if (text == NULL || sscanf (text, "%x:%x:%x", &r, &g, &b) != 3)
    return FALSE;

  *rgb = ((r >> 8) << 16) | ((g >> 8) << 8) | (b >> 8);
  return TRUE;
}

/* Strips the "gnm:" prefix, and tolerates files that used none. */
static const char *
local_name (const char *element)
{
  const char *colon = strchr (element, ':');
  return (colon != NULL) ? colon + 1 : element;
}

static void
start_element (GMarkupParseContext *context, const char *element,
               const char **names, const char **values,
               gpointer user, GError **error)
{
  Reader *r = user;
  const char *name = local_name (element);

  (void) context; (void) error;

  if (r->sheet != NULL && strcmp (name, "o42-Print") == 0)
    {
      o42_sheet_set_print_scale (r->sheet, attr_int (names, values, "Scale", 100),
                                 attr_int (names, values, "FitWide", 0),
                                 attr_int (names, values, "FitTall", 0));
      o42_sheet_set_print_margin (r->sheet, attr_double (names, values, "Margin", 36));
      return;
    }

  if (r->sheet != NULL && strcmp (name, "o42-PageBreak") == 0)
    {
      o42_sheet_toggle_page_break (r->sheet, attr_int (names, values, "Rows", 1) != 0,
                                   attr_int (names, values, "At", 0));
      return;
    }

  if (r->sheet != NULL && (strcmp (name, "grid") == 0 || strcmp (name, "titles") == 0 ||
                           strcmp (name, "repeat_top") == 0 || strcmp (name, "Header") == 0 ||
                           strcmp (name, "Footer") == 0))
    {
      const O42PrintSetup *ps = o42_sheet_print_setup (r->sheet);
      const char *v = attr (names, values, "value");
      if (strcmp (name, "grid") == 0)
        o42_sheet_set_print_options (r->sheet, v != NULL && atoi (v) != 0, ps->headings, ps->title_rows);
      else if (strcmp (name, "titles") == 0)
        o42_sheet_set_print_options (r->sheet, ps->gridlines, v != NULL && atoi (v) != 0, ps->title_rows);
      else if (strcmp (name, "repeat_top") == 0)
        {
          const char *colon = v != NULL ? strchr (v, ':') : NULL;
          const char *digits = colon != NULL ? colon + 1 : NULL;
          while (digits != NULL && *digits != '\0' && !g_ascii_isdigit (*digits)) digits++;
          if (digits != NULL && *digits != '\0')
            o42_sheet_set_print_options (r->sheet, ps->gridlines, ps->headings, atoi (digits));
        }
      else
        {
          char *joined = hf_join_excel (attr (names, values, "Left"), attr (names, values, "Middle"),
                                        attr (names, values, "Right"));
          o42_sheet_set_header_footer (r->sheet, name[0] == 'H' ? joined : NULL, name[0] == 'F' ? joined : NULL);
          g_free (joined);
        }
      return;
    }

  if (strcmp (name, "Names") == 0)
    {
      r->in_names = TRUE;
      return;
    }

  if (strcmp (name, "o42-View") == 0)
    {
      O42BookView view;
      const char *text;

      memset (&view, 0, sizeof view);
      view.name = (char *) attr (names, values, "Name");
      view.sheet = (char *) attr (names, values, "Sheet");
      text = attr (names, values, "Selection");
      if (text != NULL)
        sscanf (text, "%d %d %d %d", &view.selection.row0, &view.selection.col0,
                &view.selection.row1, &view.selection.col1);
      text = attr (names, values, "Active");
      if (text != NULL)
        sscanf (text, "%d %d", &view.active_row, &view.active_col);
      text = attr (names, values, "Zoom");
      view.zoom = text != NULL ? g_ascii_strtod (text, NULL) : 1.0;
      text = attr (names, values, "Frozen");
      if (text != NULL)
        sscanf (text, "%d %d", &view.frozen_rows, &view.frozen_cols);
      view.split = attr_int (names, values, "Split", 0) != 0;
      if (view.name != NULL)
        o42_book_set_view (r->book, &view);
      return;
    }

  if (strcmp (name, "Calculation") == 0)
    {
      o42_book_set_manual (r->book, attr_int (names, values, "ManualRecalc", 0) != 0);
      o42_book_set_iteration (r->book,
                              attr_int (names, values, "EnableIteration", 0) != 0,
                              attr_int (names, values, "MaxIterations", 100),
                              attr_double (names, values, "IterationTolerance", 0.001));
      return;
    }

  if (strcmp (name, "o42-CustomList") == 0)
    {
      r->in_custom_list = TRUE;
      if (r->custom_list == NULL)
        r->custom_list = g_string_new (NULL);
      g_string_truncate (r->custom_list, 0);
      return;
    }

  if (strcmp (name, "o42-Database") == 0)
    {
      const char *path = attr (names, values, "Path");

      if (path != NULL && *path != 0)
        o42_book_set_database (r->book, path, FALSE);
      else
        {
          /* The database is inside the book: it is written out to a
           * file of its own, since SQLite reads files, and that file is
           * read back in when the book is saved. */
          r->in_database = TRUE;
          if (r->database == NULL)
            r->database = g_string_new (NULL);
          g_string_truncate (r->database, 0);
        }
      return;
    }

  if (strcmp (name, "o42-Query") == 0 && r->sheet != NULL)
    {
      const char *at = attr (names, values, "At");
      gsize len = 0;

      r->query_headings = attr_int (names, values, "Headings", 1) != 0;
      r->query_at.row0 = r->query_at.col0 = 0;
      r->query_at.row1 = r->query_at.col1 = 0;
      if (at != NULL && o42_ref_parse (at, &r->query_at.row0, &r->query_at.col0, &len))
        {
          if (at[len] == ':')
            o42_ref_parse (at + len + 1, &r->query_at.row1, &r->query_at.col1, NULL);
          else
            {
              r->query_at.row1 = r->query_at.row0;
              r->query_at.col1 = r->query_at.col0;
            }
        }
      r->in_query = TRUE;
      if (r->query_sql == NULL)
        r->query_sql = g_string_new (NULL);
      g_string_truncate (r->query_sql, 0);
      return;
    }

  if (strcmp (name, "o42-Script") == 0)
    {
      const char *sname = NULL;
      for (int i = 0; names[i] != NULL; i++)
        if (strcmp (names[i], "Name") == 0) sname = values[i];
      r->in_script = TRUE;
      g_free (r->script_name);
      r->script_name = g_strdup (sname != NULL ? sname : "Script");
      if (r->script_code == NULL) r->script_code = g_string_new (NULL);
      g_string_truncate (r->script_code, 0);
      return;
    }

  /* gnm:Names at the workbook or sheet level: gnm:Name holding gnm:name
   * and gnm:value.  The tag names differ only in case. */
  if (strcmp (element, "gnm:Name") == 0 || strcmp (element, "Name") == 0)
    {
      if (!r->in_name_def && r->in_names)
        {
          /* Workbook names are the book's; of a sheet's, only
           * Print_Area is wanted.  A gnm:Name outside gnm:Names is a
           * sheet's own name, read elsewhere. */
          r->in_name_def = TRUE;
          r->name_on_sheet = r->sheet != NULL;
          g_string_truncate (r->name_name, 0);
          g_string_truncate (r->name_value, 0);
          return;
        }
    }
  if (r->in_name_def)
    {
      if (strcmp (element, "gnm:name") == 0 || strcmp (element, "name") == 0)
        r->in_name_name = TRUE;
      else if (strcmp (element, "gnm:value") == 0 || strcmp (element, "value") == 0)
        r->in_name_value = TRUE;
      return;
    }

  if (strcmp (name, "Sheet") == 0)
    {
      /* The book starts with one sheet; that one takes the file's first
       * and the rest are added after it. */
      r->sheet_index++;
      if (r->sheet_index == 1)
        r->sheet = o42_book_sheet (r->book, 0);
      else
        r->sheet = o42_book_add_sheet (r->book, NULL, -1);
      if (r->sheet != NULL)
        {
          o42_sheet_set_protected (r->sheet, attr_int (names, values, "o42-Protected", 0) != 0);
          o42_sheet_set_chart_sheet (r->sheet, attr_int (names, values, "o42-chart-sheet", 0) != 0);
          {
            const char *tab = attr (names, values, "o42-tab-colour");

            if (tab != NULL)
              o42_sheet_set_tab_colour (r->sheet, (guint32) g_ascii_strtoull (tab, NULL, 10));
          }
          {
            const char *pass = attr (names, values, "o42-password");

            if (pass != NULL)
              o42_sheet_set_password_hash (r->sheet, (guint16) g_ascii_strtoull (pass, NULL, 10));
          }
        }
      g_hash_table_remove_all (r->shared_exprs);
      return;
    }

  if (r->sheet == NULL)
    return;

  if (strcmp (name, "Name") == 0)
    {
      r->in_name = TRUE;
      g_string_truncate (r->name, 0);
      return;
    }

  if (strcmp (name, "StyleRegion") == 0)
    {
      r->in_style = TRUE;
      r->region.col0 = attr_int (names, values, "startCol", 0);
      r->region.row0 = attr_int (names, values, "startRow", 0);
      r->region.col1 = attr_int (names, values, "endCol", O42_MAX_COLS - 1);
      r->region.row1 = attr_int (names, values, "endRow", O42_MAX_ROWS - 1);
      o42_fmt_init_default (&r->fmt);
      r->mask = 0;
      g_clear_pointer (&r->style_link, g_free);
      return;
    }

  if (r->in_style && strcmp (name, "Validation") == 0)
    {
      O42Validation *v = &r->validation;
      memset (v, 0, sizeof *v);
      v->range = r->region;
      v->range.row1 = MIN (v->range.row1, O42_MAX_ROWS - 1);
      v->range.col1 = MIN (v->range.col1, O42_MAX_COLS - 1);
      v->kind = (O42ValidKind) attr_int (names, values, "Type", 0);
      v->op = (O42CondOp) attr_int (names, values, "Operator", 0);
      v->allow_blank = attr_int (names, values, "AllowBlank", 1) != 0;
      v->message = g_strdup (attr (names, values, "Message") ? attr (names, values, "Message") : "");
      v->value = g_strdup ("");
      v->value2 = g_strdup ("");
      r->in_validation = TRUE;
      return;
    }

  if (r->in_validation && (strcmp (name, "Expression0") == 0 || strcmp (name, "Expression1") == 0))
    {
      r->expr_index = name[10] - '0';
      g_string_truncate (r->expr, 0);
      return;
    }

  if (r->in_style && strcmp (name, "Condition") == 0)
    {
      r->in_condition = TRUE;
      memset (&r->condition, 0, sizeof r->condition);
      r->condition.range = r->region;
      r->condition.op = (O42CondOp) attr_int (names, values, "Operator", 0);
      r->condition.value = attr_double (names, values, "Value0", 0);
      r->condition.value2 = attr_double (names, values, "Value1", 0);
      r->condition.mask = (O42FmtMask) attr_int (names, values, "o42-mask", 0);
      o42_fmt_init_default (&r->condition.fmt);
      /* The nested style is read into fmt/mask like any other; it is
       * taken over when the Condition closes. */
      o42_fmt_init_default (&r->fmt);
      r->mask = 0;
      return;
    }

  if (r->in_style && strcmp (name, "Style") == 0)
    {
      const char *v;
      guint32 rgb;

      if (!r->in_condition && (attr_int (names, values, "o42-conditional", 0) != 0 ||
                               attr_int (names, values, "o42-validation", 0) != 0))
        r->region_is_conditional = TRUE;

      switch (attr_int (names, values, "HAlign", 1))
        {
        case 2:  r->fmt.halign = O42_HALIGN_LEFT;   break;
        case 4:  r->fmt.halign = O42_HALIGN_RIGHT;  break;
        case 8:  r->fmt.halign = O42_HALIGN_CENTRE; break;
        default: r->fmt.halign = O42_HALIGN_GENERAL; break;
        }
      switch (attr_int (names, values, "VAlign", 2))
        {
        case 1:  r->fmt.valign = O42_VALIGN_TOP;    break;
        case 4:  r->fmt.valign = O42_VALIGN_MIDDLE; break;
        default: r->fmt.valign = O42_VALIGN_BOTTOM; break;
        }
      r->fmt.wrap = attr_int (names, values, "WrapText", 0) != 0;
      r->fmt.indent = (guint8) CLAMP (attr_int (names, values, "Indent", 0), 0, 15);
      r->fmt.locked = attr_int (names, values, "Locked", 1) != 0;
      r->fmt.hidden = attr_int (names, values, "Hidden", 0) != 0;
      r->mask |= O42_FMT_PROTECTION;
      {
        int rot = attr_int (names, values, "Rotation", 0);
        if (rot > 180) rot -= 360;
        r->fmt.rotation = (gint16) CLAMP (rot, -90, 90);
      }
      r->mask |= O42_FMT_HALIGN | O42_FMT_VALIGN | O42_FMT_WRAP | O42_FMT_INDENT | O42_FMT_ROTATION;

      if (parse_colour (attr (names, values, "Fore"), &rgb))
        {
          r->fmt.colour = rgb;
          r->mask |= O42_FMT_COLOUR;
        }
      {
        /* Gnumeric's Shade is the pattern; 1 is a plain fill and 0 none. */
        int shade = attr_int (names, values, "Shade", 0);

        if (shade != 0 && parse_colour (attr (names, values, "Back"), &rgb))
          r->fmt.fill = rgb;
        else
          r->fmt.fill = O42_FILL_NONE;
        r->fmt.pattern = (guint8) (shade > 1 ? o42_pattern_from_shade (shade) : O42_PATTERN_NONE);
        r->fmt.pattern_colour = 0;
        if (r->fmt.pattern != O42_PATTERN_NONE &&
            parse_colour (attr (names, values, "PatternColor"), &rgb))
          r->fmt.pattern_colour = rgb;
        r->mask |= O42_FMT_FILL | O42_FMT_PATTERN;
      }

      v = attr (names, values, "Format");
      if (v != NULL)
        {
          /* A preset when the string is exactly what the preset writes;
           * otherwise the string itself, so nothing Gnumeric wrote is
           * lost in translation. */
          O42NumberFormat number;
          int decimals;
          gboolean preset = FALSE;

          if (o42_number_format_parse (v, &number, &decimals))
            {
              char *ours = o42_number_format_to_string (number, decimals);
              preset = (g_ascii_strcasecmp (ours, v) == 0);
              g_free (ours);
            }

          if (preset)
            {
              r->fmt.number = number;
              r->fmt.decimals = decimals;
              r->fmt.custom = NULL;
            }
          else
            {
              r->fmt.number = O42_NUM_GENERAL;
              r->fmt.custom = g_intern_string (v);
            }
          r->mask |= O42_FMT_NUMBER | O42_FMT_DECIMALS;
        }
      return;
    }

  if (r->in_style && strcmp (name, "HyperLink") == 0)
    {
      const char *type = attr (names, values, "type");
      const char *target = attr (names, values, "target");
      g_free (r->style_link);
      r->style_link = NULL;
      if (target != NULL)
        r->style_link = type != NULL && strcmp (type, "GnmHLinkCurWorkbook") == 0
                        ? g_strconcat ("#", target, NULL) : g_strdup (target);
      return;
    }

  if (r->in_style && strcmp (name, "Font") == 0)
    {
      r->fmt.size = (int) (attr_double (names, values, "Unit", 10) * 2 + 0.5);
      r->fmt.bold = attr_int (names, values, "Bold", 0) != 0;
      r->fmt.italic = attr_int (names, values, "Italic", 0) != 0;
      r->fmt.underline = attr_int (names, values, "Underline", 0) != 0;
      r->fmt.strikeout = attr_int (names, values, "StrikeThrough", 0) != 0;
      r->mask |= O42_FMT_SIZE | O42_FMT_BOLD | O42_FMT_ITALIC |
                 O42_FMT_UNDERLINE | O42_FMT_STRIKEOUT;
      r->in_font = TRUE;
      g_string_truncate (r->font_name, 0);
      return;
    }

  if (r->in_style && strcmp (name, "StyleBorder") == 0)
    {
      r->mask |= O42_FMT_BORDERS;
      return;
    }

  if (r->in_style && (r->mask & O42_FMT_BORDERS))
    {
      static const O42BorderStyle styles[] = { O42_BORDER_NONE, O42_BORDER_THIN, O42_BORDER_MEDIUM, O42_BORDER_DASHED,
                                               O42_BORDER_DOTTED, O42_BORDER_THICK, O42_BORDER_DOUBLE, O42_BORDER_DOTTED,
                                               O42_BORDER_MEDIUM, O42_BORDER_MEDIUM, O42_BORDER_MEDIUM, O42_BORDER_DASHED,
                                               O42_BORDER_MEDIUM, O42_BORDER_MEDIUM };
      int code = attr_int (names, values, "Style", 0);
      int side = strcmp (name, "Top") == 0 ? O42_SIDE_TOP : strcmp (name, "Bottom") == 0 ? O42_SIDE_BOTTOM
               : strcmp (name, "Left") == 0 ? O42_SIDE_LEFT : strcmp (name, "Right") == 0 ? O42_SIDE_RIGHT : -1;
      if (side >= 0)
        {
          r->fmt.border_style[side] = code >= 0 && code < (int) G_N_ELEMENTS (styles) ? styles[code] : O42_BORDER_THIN;
          r->fmt.border_colour[side] = gnm_colour_parse (attr (names, values, "Color"));
          o42_fmt_sync_borders (&r->fmt);
        }
      return;
    }

  if (strcmp (name, "ColInfo") == 0 || strcmp (name, "RowInfo") == 0)
    {
      int no = attr_int (names, values, "No", -1);
      int count = attr_int (names, values, "Count", 1);
      double unit = attr_double (names, values, "Unit", -1);

      if (no >= 0 && unit > 0)
        for (int i = no; i < no + count; i++)
          {
            gboolean hidden = attr_int (names, values, "Hidden", 0) != 0;
            int level = attr_int (names, values, "OutlineLevel", 0);

            if (name[0] == 'C')
              {
                o42_sheet_set_col_width (r->sheet, i, (int) (PT_TO_PX (unit) + 0.5));
                if (hidden) o42_sheet_set_col_hidden (r->sheet, i, TRUE);
                if (level > 0) o42_sheet_set_col_level (r->sheet, i, level);
              }
            else
              {
                o42_sheet_set_row_height (r->sheet, i, (int) (PT_TO_PX (unit) + 0.5));
                if (hidden) o42_sheet_set_row_hidden (r->sheet, i, TRUE);
                if (level > 0) o42_sheet_set_row_level (r->sheet, i, level);
              }
          }
      return;
    }

  if (strcmp (name, "o42-CellStyle") == 0)
    {
      g_free (r->style_name);
      r->style_name = g_strdup (attr (names, values, "Name"));
      r->style_mask = (O42FmtMask) attr_int (names, values, "Mask", 0);
      r->in_style = TRUE;
      o42_fmt_init_default (&r->fmt);
      r->mask = 0;
      return;
    }

  if (strcmp (name, "o42-Shape") == 0 && r->sheet != NULL)
    {
      O42ShapeKind kind = O42_SHAPE_RECT;
      const char *at = attr (names, values, "At");
      int srow = 0, scol = 0;

      o42_shape_kind_parse (attr (names, values, "Kind"), &kind);
      if (at != NULL)
        o42_ref_parse (at, &srow, &scol, NULL);
      r->shape = o42_sheet_add_shape (r->sheet, kind, srow, scol);
      if (r->shape != NULL)
        {
          r->shape->dx = attr_double (names, values, "Dx", 0);
          r->shape->dy = attr_double (names, values, "Dy", 0);
          r->shape->width = attr_double (names, values, "W", 100);
          r->shape->height = attr_double (names, values, "H", 50);
          r->shape->fill = (guint32) attr_int (names, values, "Fill", 0);
          r->shape->line = (guint32) attr_int (names, values, "Line", 0);
          r->shape->line_width = attr_double (names, values, "LineWidth", 1.5);
          r->shape->group = (guint) attr_int (names, values, "Group", 0);
          if (o42_shape_is_control (kind))
            {
              const char *link = attr (names, values, "Link");
              const char *source = attr (names, values, "Source");
              const char *script = attr (names, values, "Script");

              r->shape->link = (link != NULL && *link != 0) ? g_strdup (link) : NULL;
              r->shape->source = (source != NULL && *source != 0) ? g_strdup (source) : NULL;
              r->shape->script = (script != NULL && *script != 0) ? g_strdup (script) : NULL;
              r->shape->value = attr_double (names, values, "Value", 0);
              r->shape->min = attr_double (names, values, "Min", 0);
              r->shape->max = attr_double (names, values, "Max", 100);
              r->shape->step = attr_double (names, values, "Step", 1);
              r->shape->page = attr_double (names, values, "Page", 10);
            }
          g_free (r->shape->text);
          r->shape->text = g_strdup ("");
        }
      r->in_shape = TRUE;
      g_string_truncate (r->shape_text, 0);
      return;
    }

  if (strcmp (name, "o42-Scenario") == 0)
    {
      g_free (r->scenario_name);
      r->scenario_name = g_strdup (attr (names, values, "Name"));
      g_free (r->scenario_comment);
      r->scenario_comment = g_strdup (attr (names, values, "Comment"));
      if (r->sheet != NULL && r->scenario_name != NULL)
        o42_sheet_define_scenario (r->sheet, r->scenario_name, r->scenario_comment, -1, -1, NULL);
      return;
    }

  if (strcmp (name, "o42-ScenarioCell") == 0 && r->scenario_name != NULL && r->sheet != NULL)
    {
      const char *ref = attr (names, values, "Ref");
      const char *val = attr (names, values, "Value");
      int srow, scol;

      if (ref != NULL && o42_ref_parse (ref, &srow, &scol, NULL))
        o42_sheet_define_scenario (r->sheet, r->scenario_name, r->scenario_comment, srow, scol,
                                   val != NULL ? val : "");
      return;
    }

  if (strcmp (name, "o42-Table") == 0)
    {
      const char *tname = attr (names, values, "Name");
      const char *range = attr (names, values, "Range");
      O42Range tr;
      gsize used;

      if (r->sheet != NULL && tname != NULL && range != NULL &&
          o42_ref_parse (range, &tr.row0, &tr.col0, &used) && range[used] == ':' &&
          o42_ref_parse (range + used + 1, &tr.row1, &tr.col1, NULL))
        o42_sheet_add_table (r->sheet, tname, &tr, attr_int (names, values, "Headers", 1) != 0);
      return;
    }

  if (strcmp (name, "o42-Pivot") == 0)
    {
      /* The definition only: the values are in the cells already, and
       * the extent lets a refresh clear them. */
      O42Pivot p;
      const char *range = attr (names, values, "Range"), *at = attr (names, values, "At");
      const char *src = attr (names, values, "Source");
      gsize used;

      memset (&p, 0, sizeof p);
      if (range != NULL && at != NULL &&
          o42_ref_parse (range, &p.source.row0, &p.source.col0, &used) && range[used] == ':' &&
          o42_ref_parse (range + used + 1, &p.source.row1, &p.source.col1, NULL) &&
          o42_ref_parse (at, &p.row, &p.col, NULL))
        {
          p.source_sheet = src && src[0] ? (char *) src : NULL;
          p.row_fields = o42_pivot_fields_from_string (attr (names, values, "RowField"));
          p.col_fields = o42_pivot_fields_from_string (attr (names, values, "ColField"));
          p.data_field = (char *) attr (names, values, "DataField");
          p.filter_field = (char *) attr (names, values, "Filter");
          p.filter_value = (char *) attr (names, values, "FilterValue");
          p.agg = (O42PivotAgg) attr_int (names, values, "Agg", 0);
          p.rows = attr_int (names, values, "Rows", 0);
          p.cols = attr_int (names, values, "Cols", 0);
          o42_sheet_define_pivot (r->sheet, &p);
          g_strfreev (p.row_fields);
          g_strfreev (p.col_fields);
        }
      return;
    }

  if (strcmp (name, "FreezePanes") == 0)
    {
      const char *unfrozen = attr (names, values, "UnfrozenTopLeft");
      int row, col;

      if (unfrozen != NULL && o42_ref_parse (unfrozen, &row, &col, NULL))
        o42_sheet_set_frozen (r->sheet, row, col);
      return;
    }

  if (strcmp (name, "Field") == 0)
    {
      O42Range f;
      int index = attr_int (names, values, "Index", -1);
      const char *value = attr (names, values, "Value0");
      const char *op = attr (names, values, "Op0");

      if (o42_sheet_get_autofilter (r->sheet, &f) && index >= 0 && value != NULL)
        {
          static const char *ops[] = { "eq", "ne", "gt", "gte", "lt", "lte" };
          static const char *prefixes[] = { "", "<>", ">", ">=", "<", "<=" };
          const char *prefix = NULL;
          for (guint k = 0; k < G_N_ELEMENTS (ops); k++)
            if (op == NULL ? k == 0 : strcmp (op, ops[k]) == 0)
              prefix = prefixes[k];
          if (prefix != NULL)
            {
              char *criterion = g_strconcat (prefix, value, NULL);
              o42_sheet_autofilter_choose (r->sheet, f.col0 + index, criterion);
              g_free (criterion);
            }
        }
      return;
    }

  if (strcmp (name, "Comment") == 0)
    {
      const char *bound = attr (names, values, "ObjectBound");
      const char *text = attr (names, values, "Text");
      int row, col;

      if (bound != NULL && text != NULL && o42_ref_parse (bound, &row, &col, NULL))
        o42_sheet_set_note (r->sheet, row, col, text);
      return;
    }

  if (strcmp (name, "Merge") == 0)
    {
      r->in_merge = TRUE;
      g_string_truncate (r->merge, 0);
      return;
    }

  if (strcmp (name, "Filter") == 0)
    {
      const char *area = attr (names, values, "Area");
      O42Range f;
      gsize used = 0;

      if (area != NULL && o42_ref_parse (area, &f.row0, &f.col0, &used) &&
          area[used] == ':' && o42_ref_parse (area + used + 1, &f.row1, &f.col1, NULL))
        o42_sheet_set_autofilter (r->sheet, &f);
      return;
    }

  if (strcmp (name, "Cell") == 0)
    {
      r->in_cell = TRUE;
      r->cell_row = attr_int (names, values, "Row", -1);
      r->cell_col = attr_int (names, values, "Col", -1);
      r->cell_type = attr_int (names, values, "ValueType", -1);
      r->cell_rows = attr_int (names, values, "Rows", 0);
      r->cell_cols = attr_int (names, values, "Cols", 0);
      r->cell_expr_id = attr_int (names, values, "ExprID", -1);
      g_free (r->cell_style);
      r->cell_style = g_strdup (attr (names, values, "o42-style"));
      g_free (r->cell_runs);
      r->cell_runs = g_strdup (attr (names, values, "o42-runs"));
      g_string_truncate (r->cell_text, 0);
      return;
    }

  if (strcmp (name, "SheetObjectGraph") == 0 || (r->in_graph && strcmp (name, "GogObject") == 0) ||
      (r->in_graph && strcmp (name, "dimension") == 0))
    {
      if (strcmp (name, "SheetObjectGraph") == 0)
        {
          const char *bound = attr (names, values, "ObjectBound");
          const char *offset = attr (names, values, "ObjectOffset");
          const char *kind = attr (names, values, "o42-kind");
          gsize used = 0;

          r->in_graph = FALSE;
          r->graph_kind_known = FALSE;
          r->graph_data_known = FALSE;
          r->graph_first_row = attr_int (names, values, "o42-first-row-labels", -1);
          r->graph_first_col = attr_int (names, values, "o42-first-col-labels", -1);
          r->graph_in_rows = attr_int (names, values, "o42-series-in-rows", -1);
          r->graph_legend = attr_int (names, values, "o42-legend", -1);
          r->graph_labels = attr_int (names, values, "o42-labels", -1);
          g_free (r->graph_yformat);
          r->graph_yformat = g_strdup (attr (names, values, "o42-yformat"));
          r->graph_secondary = attr_int (names, values, "o42-secondary", 0);
          /* Old files wrote a flag here, newer ones the curve's name. */
          r->graph_trend = attr_int (names, values, "o42-trend", -1);
          g_free (r->graph_trend_name);
          r->graph_trend_name = g_strdup (attr (names, values, "o42-trend"));
          r->graph_trend_order = attr_int (names, values, "o42-trend-order", 2);
          r->graph_3d = attr_int (names, values, "o42-3d", 0) != 0;
          g_free (r->graph_marker_name);
          r->graph_marker_name = g_strdup (attr (names, values, "o42-marker"));
          r->graph_marker_size = attr_double (names, values, "o42-marker-size", 0);
          r->graph_marker_picture = (guint) attr_int (names, values, "o42-marker-picture", 0);
          r->graph_group = (guint) attr_int (names, values, "o42-group", 0);
          g_free (r->graph_err_name);
          r->graph_err_name = g_strdup (attr (names, values, "o42-errbars"));
          g_free (r->graph_font);
          r->graph_font = g_strdup (attr (names, values, "o42-font"));
          g_free (r->graph_data_sheet);
          r->graph_data_sheet = g_strdup (attr (names, values, "o42-data-sheet"));
          {
            const char *v = attr (names, values, "o42-fontsize");
            r->graph_font_size = v != NULL ? g_ascii_strtod (v, NULL) : 0;
          }
          {
            const char *v = attr (names, values, "o42-errvalue");
            r->graph_err_value = v != NULL ? g_ascii_strtod (v, NULL) : 0;
          }
          r->graph_gridlines = attr_int (names, values, "o42-gridlines", -1);
          r->graph_has_min = attr (names, values, "o42-min") != NULL;
          r->graph_has_max = attr (names, values, "o42-max") != NULL;
          r->graph_min = r->graph_has_min ? g_ascii_strtod (attr (names, values, "o42-min"), NULL) : 0;
          r->graph_max = r->graph_has_max ? g_ascii_strtod (attr (names, values, "o42-max"), NULL) : 0;
          r->graph_axis = 0;
          g_string_truncate (r->graph_title, 0);
          g_string_truncate (r->graph_x_title, 0);
          g_string_truncate (r->graph_y_title, 0);
          r->object_offset[0] = r->object_offset[1] = 0;
          r->object_offset[2] = r->object_offset[3] = 1;
          if (kind != NULL && o42_chart_kind_parse (kind, &r->graph_kind))
            r->graph_kind_known = TRUE;

          if (bound != NULL &&
              o42_ref_parse (bound, &r->object_bound.row0, &r->object_bound.col0, &used))
            {
              if (!(bound[used] == ':' &&
                    o42_ref_parse (bound + used + 1, &r->object_bound.row1,
                                   &r->object_bound.col1, NULL)))
                {
                  r->object_bound.row1 = r->object_bound.row0;
                  r->object_bound.col1 = r->object_bound.col0;
                }
              r->in_graph = TRUE;
            }
          if (offset != NULL)
            {
              char **parts = g_strsplit (offset, " ", -1);
              for (int i = 0; i < 4 && parts[i] != NULL; i++)
                r->object_offset[i] = g_ascii_strtod (parts[i], NULL);
              g_strfreev (parts);
            }
        }
      else if (strcmp (name, "GogObject") == 0)
        {
          const char *type = attr (names, values, "type");
          const char *role = attr (names, values, "role");

          if (type != NULL && !r->graph_kind_known)
            {
              if (strcmp (type, "GogLinePlot") == 0)   { r->graph_kind = O42_CHART_LINE; r->graph_kind_known = TRUE; }
              if (strcmp (type, "GogPiePlot") == 0)    { r->graph_kind = O42_CHART_PIE; r->graph_kind_known = TRUE; }
              if (strcmp (type, "GogBarColPlot") == 0) { r->graph_kind = O42_CHART_COLUMN; r->graph_kind_known = TRUE; }
              if (strcmp (type, "GogAreaPlot") == 0)   { r->graph_kind = O42_CHART_AREA; r->graph_kind_known = TRUE; }
              if (strcmp (type, "GogXYPlot") == 0)     { r->graph_kind = O42_CHART_SCATTER; r->graph_kind_known = TRUE; }
            }
          if (role != NULL && strcmp (role, "X-Axis") == 0) r->graph_axis = 1;
          else if (role != NULL && strcmp (role, "Y-Axis") == 0) r->graph_axis = 2;
          else if (role != NULL && (strcmp (role, "Plot") == 0 || strcmp (role, "Series") == 0 ||
                                    strcmp (role, "Legend") == 0 || strcmp (role, "Title") == 0))
            r->graph_axis = 0;
          if (role != NULL && strcmp (role, "Legend") == 0 && r->graph_legend < 0)
            r->graph_legend = 1;
          if (role != NULL && strcmp (role, "MajorGrid") == 0 && r->graph_axis == 2 && r->graph_gridlines < 0)
            r->graph_gridlines = 1;
          r->in_title = role != NULL && (strcmp (role, "Title") == 0 || (strcmp (role, "Label") == 0 && r->graph_axis > 0));
        }
      else
        {
          r->graph_dimension = attr_int (names, values, "id", -2);
          r->in_dimension = TRUE;
          g_string_truncate (r->dimension, 0);
        }
      return;
    }

  if (strcmp (name, "SheetObjectImage") == 0)
    {
      const char *bound = attr (names, values, "ObjectBound");
      const char *offset = attr (names, values, "ObjectOffset");
      gsize used = 0;

      r->in_object = FALSE;
      r->object_offset[0] = r->object_offset[1] = 0;
      r->object_offset[2] = r->object_offset[3] = 1;

      if (bound != NULL &&
          o42_ref_parse (bound, &r->object_bound.row0, &r->object_bound.col0, &used))
        {
          if (bound[used] == ':' &&
              o42_ref_parse (bound + used + 1, &r->object_bound.row1,
                             &r->object_bound.col1, NULL))
            r->in_object = TRUE;
          else
            {
              r->object_bound.row1 = r->object_bound.row0;
              r->object_bound.col1 = r->object_bound.col0;
              r->in_object = TRUE;
            }
        }

      if (offset != NULL)
        {
          char **parts = g_strsplit (offset, " ", -1);
          for (int i = 0; i < 4 && parts[i] != NULL; i++)
            r->object_offset[i] = g_ascii_strtod (parts[i], NULL);
          g_strfreev (parts);
        }
      return;
    }

  if (r->in_object && strcmp (name, "Content") == 0)
    {
      const char *type = attr (names, values, "image-type");

      r->in_content = TRUE;
      g_string_truncate (r->content, 0);
      g_free (r->content_type);
      r->content_type = g_strdup (type != NULL ? type : "png");
      return;
    }
}

static void
finish_cell (Reader *r)
{
  const char *text = r->cell_text->str;
  char *forced = NULL;

  if (r->cell_row < 0 || r->cell_col < 0 ||
      r->cell_row >= O42_MAX_ROWS || r->cell_col >= O42_MAX_COLS)
    return;

  /* A shared expression: the first cell carries the text and an ID, the
   * others only the ID. */
  if (r->cell_expr_id >= 0)
    {
      if (*text != '\0')
        g_hash_table_insert (r->shared_exprs, GINT_TO_POINTER (r->cell_expr_id),
                             g_strdup_printf ("%d,%d,%s", r->cell_row, r->cell_col, text));
      else
        {
          /* The expression is the first cell's, with relative
           * references moved by the distance from that cell to this. */
          const char *shared = g_hash_table_lookup (r->shared_exprs,
                                                    GINT_TO_POINTER (r->cell_expr_id));
          const char *comma, *body;
          int origin_row, origin_col;

          if (shared == NULL)
            return;
          origin_row = atoi (shared);
          comma = strchr (shared, ',');
          origin_col = comma ? atoi (comma + 1) : 0;
          body = comma ? strchr (comma + 1, ',') : NULL;
          if (body == NULL)
            return;
          body++;
          if (body[0] == '=')
            {
              O42Node *tree = o42_formula_parse (body + 1);
              char *moved;
              o42_node_relocate (tree, r->cell_row - origin_row, r->cell_col - origin_col);
              moved = o42_node_to_string (tree);
              g_free (forced);
              forced = g_strconcat ("=", moved, NULL);
              g_free (moved);
              o42_node_free (tree);
              text = forced;
            }
          else
            text = body;
        }
    }

  if (*text == '\0')
    return;

  switch (r->cell_type)
    {
    case 60:
      /* Text that would read as a number or a date has to be forced. */
      forced = o42_entry_quote_text (text);
      break;

    case 20:
      forced = g_strconcat ("=", text, NULL);
      break;

    default:
      break;
    }

  if (r->cell_rows > 0 && r->cell_cols > 0 && text[0] == '=')
    {
      O42Range block = { r->cell_row, r->cell_col,
                         MIN (r->cell_row + r->cell_rows - 1, O42_MAX_ROWS - 1),
                         MIN (r->cell_col + r->cell_cols - 1, O42_MAX_COLS - 1) };
      o42_sheet_set_array_formula (r->sheet, &block, text);
    }
  else
    o42_sheet_set_input (r->sheet, r->cell_row, r->cell_col,
                         forced != NULL ? forced : text);
  g_free (forced);
  r->seen_cell = TRUE;
  if (r->cell_runs != NULL && r->sheet != NULL)
    {
      char **parts = g_strsplit (r->cell_runs, ";", -1);
      GArray *runs = g_array_new (FALSE, FALSE, sizeof (O42TextRun));

      for (int i = 0; parts[i] != NULL; i++)
        {
          char **fields = g_strsplit (parts[i], ",", 5);
          O42TextRun run;

          if (g_strv_length (fields) >= 5)
            {
              int flags = atoi (fields[1]);

              o42_fmt_init_default (&run.fmt);
              run.start = atoi (fields[0]);
              run.fmt.bold = (flags & 1) != 0;
              run.fmt.italic = (flags & 2) != 0;
              run.fmt.underline = (flags & 4) != 0;
              run.fmt.strikeout = (flags & 8) != 0;
              run.fmt.size = (guint8) CLAMP (atoi (fields[2]), 2, 800);
              run.fmt.colour = (guint32) g_ascii_strtoull (fields[3], NULL, 16);
              run.fmt.family = g_intern_string (fields[4]);
              g_array_append_val (runs, run);
            }
          g_strfreev (fields);
        }
      if (runs->len > 0)
        o42_sheet_set_runs (r->sheet, r->cell_row, r->cell_col,
                            &g_array_index (runs, O42TextRun, 0), (int) runs->len);
      g_array_unref (runs);
      g_strfreev (parts);
      g_clear_pointer (&r->cell_runs, g_free);
    }

  if (r->cell_style != NULL && r->sheet != NULL)
    {
      O42Range one = { r->cell_row, r->cell_col, r->cell_row, r->cell_col };
      O42Fmt known;
      O42FmtMask mask = 0;

      /* Gnumeric's format has no place to define a cell style, only
       * the name we write on the cell, so the first cell wearing one
       * defines it: its own look becomes the style's. */
      o42_fmt_init_default (&known);
      if (r->book != NULL && !o42_book_style (r->book, r->cell_style, &known, &mask))
        o42_book_set_style (r->book, r->cell_style,
                            o42_sheet_get_fmt (r->sheet, r->cell_row, r->cell_col),
                            O42_FMT_ALL);
      o42_sheet_apply_style (r->sheet, &one, r->cell_style);
      g_clear_pointer (&r->cell_style, g_free);
    }

}

static void
finish_picture (Reader *r)
{
  guchar *bytes;
  gsize length = 0;
  GBytes *data;
  int width = 0, height = 0;
  const char *format = NULL;
  O42Picture *pic;
  double x0, y0, x1, y1;

  if (r->content->len == 0)
    return;

  bytes = g_base64_decode (r->content->str, &length);
  data = g_bytes_new_take (bytes, length);

  if (!o42_image_probe (data, &width, &height, &format))
    {
      g_bytes_unref (data);
      return;
    }

  pic = o42_sheet_add_picture (r->sheet, data, format, width, height,
                               r->object_bound.row0, r->object_bound.col0);
  g_bytes_unref (data);
  if (pic == NULL)
    return;

  x0 = offset_px (r->sheet, TRUE, r->object_bound.col0) +
       r->object_offset[0] * o42_sheet_col_width (r->sheet, r->object_bound.col0);
  y0 = offset_px (r->sheet, FALSE, r->object_bound.row0) +
       r->object_offset[1] * o42_sheet_row_height (r->sheet, r->object_bound.row0);
  x1 = offset_px (r->sheet, TRUE, r->object_bound.col1) +
       r->object_offset[2] * o42_sheet_col_width (r->sheet, r->object_bound.col1);
  y1 = offset_px (r->sheet, FALSE, r->object_bound.row1) +
       r->object_offset[3] * o42_sheet_row_height (r->sheet, r->object_bound.row1);

  pic->dx = x0 - offset_px (r->sheet, TRUE, r->object_bound.col0);
  pic->dy = y0 - offset_px (r->sheet, FALSE, r->object_bound.row0);
  if (x1 > x0 && y1 > y0)
    {
      pic->width = x1 - x0;
      pic->height = y1 - y0;
    }
}

static void
end_element (GMarkupParseContext *context, const char *element,
             gpointer user, GError **error)
{
  Reader *r = user;
  const char *name = local_name (element);

  (void) context; (void) error;

  if (r->in_query && strcmp (name, "o42-Query") == 0)
    {
      if (r->sheet != NULL && r->query_sql != NULL && r->query_sql->len > 0)
        o42_sheet_add_query (r->sheet, r->query_sql->str, &r->query_at, r->query_headings);
      r->in_query = FALSE;
      return;
    }

  if (r->in_custom_list && strcmp (name, "o42-CustomList") == 0)
    {
      o42_book_add_custom_list (r->book, g_strsplit (r->custom_list->str, "\t", -1));
      r->in_custom_list = FALSE;
      return;
    }

  if (r->in_database && strcmp (name, "o42-Database") == 0)
    {
      guchar *data;
      gsize length = 0;

      r->in_database = FALSE;
      data = r->database != NULL
             ? g_base64_decode (r->database->str, &length) : NULL;
      if (data != NULL && length > 0)
        {
          /* Somewhere to keep it while the book is open.  The name is
           * the book's, so two books do not tread on each other. */
          char *path = NULL;
          int fd = g_file_open_tmp ("office42-XXXXXX.sqlite", &path, NULL);

          if (fd >= 0)
            {
              g_close (fd, NULL);
              if (g_file_set_contents (path, (const char *) data, length, NULL))
                o42_book_set_database (r->book, path, TRUE);
            }
          g_free (path);
        }
      g_free (data);
      return;
    }

  if (r->in_script && strcmp (name, "o42-Script") == 0)
    {
      o42_book_set_script (r->book, r->script_name, r->script_code->str);
      r->in_script = FALSE;
      return;
    }
  if (strcmp (name, "Names") == 0)
    {
      r->in_names = FALSE;
      return;
    }
  if (strcmp (name, "o42-Shape") == 0 && r->in_shape)
    {
      if (r->shape != NULL)
        {
          g_free (r->shape->text);
          r->shape->text = g_strdup (r->shape_text->str);
        }
      r->in_shape = FALSE;
      r->shape = NULL;
      return;
    }
  if (strcmp (name, "o42-CellStyle") == 0)
    {
      if (r->style_name != NULL)
        o42_book_set_style (r->book, r->style_name, &r->fmt, r->style_mask);
      g_clear_pointer (&r->style_name, g_free);
      r->in_style = FALSE;
      return;
    }

  if (r->in_name_def)
    {
      if (strcmp (element, "gnm:name") == 0 || strcmp (element, "name") == 0)
        r->in_name_name = FALSE;
      else if (strcmp (element, "gnm:value") == 0 || strcmp (element, "value") == 0)
        r->in_name_value = FALSE;
      else if (strcmp (element, "gnm:Name") == 0 || strcmp (element, "Name") == 0)
        {
          r->in_name_def = FALSE;
          if (r->name_on_sheet)
            {
              if (r->sheet != NULL && strcmp (r->name_name->str, "Print_Area") == 0)
                {
                  O42Range area;
                  char *clean = g_strdup (r->name_value->str);
                  char *w = clean;
                  const char *bang = strrchr (r->name_value->str, '!');
                  for (const char *q = bang != NULL ? bang + 1 : r->name_value->str; *q != '\0'; q++)
                    if (*q != '$') *w++ = *q;
                  *w = '\0';
                  {
                    gsize used = 0;
                    if (o42_ref_parse (clean, &area.row0, &area.col0, &used) && clean[used] == ':' &&
                        o42_ref_parse (clean + used + 1, &area.row1, &area.col1, NULL))
                      o42_sheet_set_print_area (r->sheet, &area);
                  }
                  g_free (clean);
                }
              return;
            }
          g_ptr_array_add (r->pending_names, g_strdup (r->name_name->str));
          g_ptr_array_add (r->pending_names, g_strdup (r->name_value->str));
        }
      return;
    }

  if (r->sheet == NULL)
    return;

  /* The filter's choices were applied before the cells arrived. */
  if (strcmp (name, "Sheet") == 0)
    {
      o42_sheet_autofilter_refresh (r->sheet);
      return;
    }

  if (strcmp (name, "Name") == 0 && r->in_name)
    {
      r->in_name = FALSE;
      if (r->name->len > 0)
        o42_book_rename_sheet (r->book, o42_book_sheet_index (r->book, r->sheet),
                               r->name->str);
      return;
    }

  if (strcmp (name, "Font") == 0 && r->in_font)
    {
      r->in_font = FALSE;
      if (r->font_name->len > 0)
        {
          r->fmt.family = g_intern_string (r->font_name->str);
          r->mask |= O42_FMT_FAMILY;
        }
      return;
    }

  if (r->in_validation && (strcmp (name, "Expression0") == 0 || strcmp (name, "Expression1") == 0))
    {
      char *text = g_strdup (r->expr->str);
      gsize len = strlen (text);
      char **slot = r->expr_index == 0 ? &r->validation.value : &r->validation.value2;

      /* A list is a quoted string; the quotes are the file's, not ours. */
      if (len >= 2 && text[0] == '"' && text[len - 1] == '"')
        {
          text[len - 1] = '\0';
          memmove (text, text + 1, len - 1);
        }
      else if (text[0] == '=')
        memmove (text, text + 1, len);
      g_free (*slot);
      *slot = text;
      r->expr_index = -1;
      return;
    }

  if (strcmp (name, "Validation") == 0 && r->in_validation)
    {
      r->in_validation = FALSE;
      if (r->validation.kind != O42_VALID_ANY && r->validation.range.row1 - r->validation.range.row0 < 2000)
        o42_sheet_add_validation (r->sheet, &r->validation);
      g_free (r->validation.value);
      g_free (r->validation.value2);
      g_free (r->validation.message);
      memset (&r->validation, 0, sizeof r->validation);
      return;
    }

  if (strcmp (name, "Condition") == 0 && r->in_condition)
    {
      r->in_condition = FALSE;
      r->condition.fmt = r->fmt;
      /* Without our mask attribute (a file from Gnumeric), take every
       * field the nested style set. */
      if (r->condition.mask == 0)
        r->condition.mask = r->mask;
      o42_sheet_add_condition (r->sheet, &r->condition);
      r->mask = 0;
      return;
    }

  if (strcmp (name, "StyleRegion") == 0 && r->in_style)
    {
      O42Range region = r->region;
      gboolean conditional_only = r->region_is_conditional;

      r->in_style = FALSE;
      r->region_is_conditional = FALSE;
      if (conditional_only)
        return;
      region.col1 = MIN (region.col1, O42_MAX_COLS - 1);
      region.row1 = MIN (region.row1, O42_MAX_ROWS - 1);

      /* A region over the whole sheet is the default style; applying it
       * cell by cell would create four million cells.  It becomes the
       * table's default instead -- but only if it differs from ours. */
      if (region.col0 == 0 && region.row0 == 0 &&
          region.col1 == O42_MAX_COLS - 1 && region.row1 == O42_MAX_ROWS - 1)
        return;

      /* Whole columns and rows would be sixteen thousand cells apiece; the
       * cells they affect are the ones that hold something, and those are
       * formatted as they are read.  Clamp to a workable rectangle. */
      if (region.row1 - region.row0 > 2000)
        region.row1 = region.row0 + 2000;
      if (region.col1 - region.col0 > 255)
        region.col1 = region.col0 + 255;

      if (r->mask != 0)
        o42_sheet_apply_fmt (r->sheet, &region, r->mask, &r->fmt);
      if (r->style_link != NULL && (region.row1 - region.row0 + 1) * (region.col1 - region.col0 + 1) <= 4096)
        {
          for (int lr = region.row0; lr <= region.row1; lr++)
            for (int lc = region.col0; lc <= region.col1; lc++)
              o42_sheet_set_link (r->sheet, lr, lc, r->style_link);
          g_clear_pointer (&r->style_link, g_free);
        }
      return;
    }

  if (strcmp (name, "Cell") == 0 && r->in_cell)
    {
      r->in_cell = FALSE;
      finish_cell (r);
      return;
    }

  if (strcmp (name, "Merge") == 0 && r->in_merge)
    {
      O42Range m;
      gsize used = 0;

      r->in_merge = FALSE;
      if (o42_ref_parse (r->merge->str, &m.row0, &m.col0, &used) &&
          r->merge->str[used] == ':' &&
          o42_ref_parse (r->merge->str + used + 1, &m.row1, &m.col1, NULL))
        {
          /* Merging empties the other cells, but the file's cells come
           * later; the range is recorded straight into the sheet's list. */
          g_array_append_val (o42_sheet_merges (r->sheet), m);
        }
      return;
    }

  if (strcmp (name, "Content") == 0 && r->in_content)
    {
      r->in_content = FALSE;
      finish_picture (r);
      return;
    }

  if (strcmp (name, "SheetObjectImage") == 0)
    {
      r->in_object = FALSE;
      return;
    }

  if (r->in_graph && strcmp (name, "dimension") == 0 && r->in_dimension)
    {
      r->in_dimension = FALSE;

      /* The first series' values (id 1) give the data range; with our
       * own label flags missing, the categories (id 0) widen it by the
       * label column and the series name (id -1) by the heading row. */
      if (r->graph_dimension == 1 || r->graph_dimension == 0 || r->graph_dimension == -1)
        {
          const char *text = r->dimension->str;
          const char *bang = strrchr (text, '!');
          O42Range range;
          gsize used = 0;

          if (bang != NULL)
            text = bang + 1;
          if (o42_ref_parse (text, &range.row0, &range.col0, &used))
            {
              if (text[used] == ':' &&
                  o42_ref_parse (text + used + 1, &range.row1, &range.col1, NULL))
                range = o42_range_normalise (range.row0, range.col0, range.row1, range.col1);
              else
                {
                  range.row1 = range.row0;
                  range.col1 = range.col0;
                }

              if (!r->graph_data_known)
                {
                  r->graph_data = range;
                  r->graph_data_known = TRUE;
                }
              else
                {
                  r->graph_data.row0 = MIN (r->graph_data.row0, range.row0);
                  r->graph_data.col0 = MIN (r->graph_data.col0, range.col0);
                  r->graph_data.row1 = MAX (r->graph_data.row1, range.row1);
                  r->graph_data.col1 = MAX (r->graph_data.col1, range.col1);
                }
            }
        }
      else if (r->in_title && r->graph_dimension == 0 && r->graph_title->len == 0)
        {
          /* Already handled above when id is 0; a title's text is quoted. */
        }
      if (r->in_title)
        {
          const char *t = r->dimension->str;
          gsize len = strlen (t);
          GString *target = r->graph_axis == 1 ? r->graph_x_title : r->graph_axis == 2 ? r->graph_y_title : r->graph_title;
          if (len >= 2 && t[0] == '"' && t[len - 1] == '"')
            g_string_append_len (target, t + 1, (gssize) (len - 2));
        }
      return;
    }

  if (strcmp (name, "SheetObjectGraph") == 0 && r->in_graph)
    {
      r->in_graph = FALSE;
      if (r->graph_data_known)
        {
          O42Chart *chart = o42_sheet_add_chart (r->sheet,
                                                 r->graph_kind_known ? r->graph_kind : O42_CHART_COLUMN,
                                                 &r->graph_data,
                                                 r->object_bound.row0, r->object_bound.col0);
          double x0, y0, x1, y1;

          if (chart == NULL)
            return;
          if (r->graph_first_row >= 0) chart->first_row_labels = r->graph_first_row != 0;
          if (r->graph_first_col >= 0) chart->first_col_labels = r->graph_first_col != 0;
          if (r->graph_in_rows >= 0) chart->series_in_rows = r->graph_in_rows != 0;
          g_free (chart->title);
          chart->title = g_strdup (r->graph_title->str);
          g_free (chart->x_title);
          chart->x_title = g_strdup (r->graph_x_title->str);
          g_free (chart->y_title);
          chart->y_title = g_strdup (r->graph_y_title->str);
          if (r->graph_marker_name != NULL)
            o42_marker_kind_parse (r->graph_marker_name, &chart->marker);
          chart->marker_size = r->graph_marker_size;
          chart->marker_picture = r->graph_marker_picture;
          if (r->graph_legend >= 0) chart->legend = r->graph_legend != 0;
          if (r->graph_labels >= 0) chart->data_labels = r->graph_labels != 0;
          if (r->graph_yformat != NULL)
            {
              g_free (chart->y_format);
              chart->y_format = g_strdup (r->graph_yformat);
            }
          chart->secondary_from = r->graph_secondary;
          if (r->graph_trend_name != NULL &&
              o42_trend_kind_parse (r->graph_trend_name, &chart->trend))
            chart->trend_order = r->graph_trend_order;
          else if (r->graph_trend >= 0)
            chart->trend = r->graph_trend != 0 ? O42_TREND_LINEAR : O42_TREND_NONE;
          if (r->graph_err_name != NULL &&
              o42_errbar_kind_parse (r->graph_err_name, &chart->err_bars))
            chart->err_value = r->graph_err_value;
          if (r->graph_font != NULL)
            {
              g_free (chart->font_family);
              chart->font_family = g_strdup (r->graph_font);
              chart->font_size = r->graph_font_size;
            }
          chart->three_d = r->graph_3d;
          chart->group = r->graph_group;
          if (r->graph_data_sheet != NULL)
            {
              g_free (chart->data_sheet);
              chart->data_sheet = g_strdup (r->graph_data_sheet);
            }
          if (r->graph_gridlines >= 0) chart->gridlines = r->graph_gridlines != 0;
          chart->has_min = r->graph_has_min; chart->min = r->graph_min;
          chart->has_max = r->graph_has_max; chart->max = r->graph_max;

          x0 = offset_px (r->sheet, TRUE, r->object_bound.col0) +
               r->object_offset[0] * o42_sheet_col_width (r->sheet, r->object_bound.col0);
          y0 = offset_px (r->sheet, FALSE, r->object_bound.row0) +
               r->object_offset[1] * o42_sheet_row_height (r->sheet, r->object_bound.row0);
          x1 = offset_px (r->sheet, TRUE, r->object_bound.col1) +
               r->object_offset[2] * o42_sheet_col_width (r->sheet, r->object_bound.col1);
          y1 = offset_px (r->sheet, FALSE, r->object_bound.row1) +
               r->object_offset[3] * o42_sheet_row_height (r->sheet, r->object_bound.row1);
          chart->dx = x0 - offset_px (r->sheet, TRUE, r->object_bound.col0);
          chart->dy = y0 - offset_px (r->sheet, FALSE, r->object_bound.row0);
          if (x1 > x0 + 20 && y1 > y0 + 20)
            {
              chart->width = x1 - x0;
              chart->height = y1 - y0;
            }
        }
      return;
    }
}

static void
text_handler (GMarkupParseContext *context, const char *text, gsize length,
              gpointer user, GError **error)
{
  Reader *r = user;

  (void) context; (void) error;

  if (r->in_shape)
    { g_string_append_len (r->shape_text, text, (gssize) length); return; }
  if (r->in_script)
    { g_string_append_len (r->script_code, text, (gssize) length); return; }
  if (r->in_database)
    { g_string_append_len (r->database, text, (gssize) length); return; }
  if (r->in_custom_list)
    { g_string_append_len (r->custom_list, text, (gssize) length); return; }
  if (r->in_query)
    { g_string_append_len (r->query_sql, text, (gssize) length); return; }
  if (r->in_name_name)
    { g_string_append_len (r->name_name, text, (gssize) length); return; }
  if (r->in_name_value)
    { g_string_append_len (r->name_value, text, (gssize) length); return; }

  if (r->sheet == NULL)
    return;

  if (r->in_cell)
    g_string_append_len (r->cell_text, text, (gssize) length);
  else if (r->in_merge)
    g_string_append_len (r->merge, text, (gssize) length);
  else if (r->in_validation && r->expr_index >= 0)
    g_string_append_len (r->expr, text, (gssize) length);
  else if (r->in_dimension)
    g_string_append_len (r->dimension, text, (gssize) length);
  else if (r->in_name)
    g_string_append_len (r->name, text, (gssize) length);
  else if (r->in_font)
    g_string_append_len (r->font_name, text, (gssize) length);
  else if (r->in_content)
    g_string_append_len (r->content, text, (gssize) length);
}

static char *
read_maybe_gzipped (GFile *file, gsize *length, GError **error)
{
  char *raw = NULL;
  gsize raw_length = 0;

  if (!g_file_load_contents (file, NULL, &raw, &raw_length, NULL, error))
    return NULL;

  if (raw_length >= 2 && (guchar) raw[0] == 0x1f && (guchar) raw[1] == 0x8b)
    {
      GInputStream *memory = g_memory_input_stream_new_from_data (raw, (gssize) raw_length, g_free);
      GZlibDecompressor *decompressor = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
      GInputStream *unzipped = g_converter_input_stream_new (memory, G_CONVERTER (decompressor));
      GByteArray *out = g_byte_array_new ();
      guchar buffer[65536];
      gssize n;

      while ((n = g_input_stream_read (unzipped, buffer, sizeof buffer, NULL, error)) > 0)
        g_byte_array_append (out, buffer, (guint) n);

      g_object_unref (unzipped);
      g_object_unref (decompressor);
      g_object_unref (memory);

      if (n < 0)
        {
          g_byte_array_free (out, TRUE);
          return NULL;
        }

      *length = out->len;
      g_byte_array_append (out, (const guchar *) "", 1);
      return (char *) g_byte_array_free (out, FALSE);
    }

  *length = raw_length;
  return raw;
}

gboolean
o42_gnumeric_load (O42Book *book, GFile *file, GError **error)
{
  static const GMarkupParser parser = {
    start_element, end_element, text_handler, NULL, NULL
  };
  Reader r;
  GMarkupParseContext *context;
  char *xml;
  gsize length = 0;
  gboolean ok;
  O42Range everything = { 0, 0, O42_MAX_ROWS - 1, O42_MAX_COLS - 1 };

  g_return_val_if_fail (book != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  xml = read_maybe_gzipped (file, &length, error);
  if (xml == NULL)
    return FALSE;

  if (strstr (xml, "gnm:Workbook") == NULL && strstr (xml, "<Workbook") == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "This is not a Gnumeric file.");
      g_free (xml);
      return FALSE;
    }

  memset (&r, 0, sizeof r);
  r.book = book;
  r.font_name = g_string_new (NULL);
  r.cell_text = g_string_new (NULL);
  r.content = g_string_new (NULL);
  r.name = g_string_new (NULL);
  r.merge = g_string_new (NULL);
  r.expr = g_string_new (NULL);
  r.expr_index = -1;
  r.dimension = g_string_new (NULL);
  r.graph_title = g_string_new (NULL);
  r.shape_text = g_string_new (NULL);
  r.graph_x_title = g_string_new (NULL);
  r.graph_y_title = g_string_new (NULL);
  r.name_name = g_string_new (NULL);
  r.name_value = g_string_new (NULL);
  r.pending_names = g_ptr_array_new_with_free_func (g_free);
  r.shared_exprs = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);

  /* Down to one empty sheet, which takes the file's first. */
  while (o42_book_n_sheets (book) > 1)
    o42_book_remove_sheet (book, o42_book_n_sheets (book) - 1);
  {
    O42Sheet *first = o42_book_sheet (book, 0);
    GPtrArray *pictures = o42_sheet_pictures (first);

    o42_sheet_clear_range (first, &everything);
    o42_sheet_clear_formats (first, &everything);
    while (pictures->len > 0)
      o42_sheet_remove_picture (first, ((O42Picture *) g_ptr_array_index (pictures, 0))->id);
    while (o42_sheet_charts (first)->len > 0)
      o42_sheet_remove_chart (first, ((O42Chart *) g_ptr_array_index (o42_sheet_charts (first), 0))->id);
    g_array_set_size (o42_sheet_merges (first), 0);
    g_hash_table_remove_all (o42_sheet_notes (first));
    o42_sheet_set_frozen (first, 0, 0);
    o42_sheet_clear_conditions (first, NULL);
    o42_book_rename_sheet (book, 0, "Sheet1");
  }

  context = g_markup_parse_context_new (&parser, G_MARKUP_TREAT_CDATA_AS_TEXT, &r, NULL);
  ok = g_markup_parse_context_parse (context, xml, (gssize) length, error) &&
       g_markup_parse_context_end_parse (context, error);
  g_markup_parse_context_free (context);
  g_free (r.script_name);
  g_free (r.style_link);
  g_free (r.style_name);
  g_free (r.cell_style);
  g_free (r.cell_runs);
  g_free (r.scenario_name);
  g_free (r.scenario_comment);
  g_free (r.graph_yformat);
  if (r.script_code != NULL) g_string_free (r.script_code, TRUE);

  g_string_free (r.font_name, TRUE);
  g_string_free (r.cell_text, TRUE);
  g_string_free (r.content, TRUE);
  g_string_free (r.name, TRUE);
  g_string_free (r.merge, TRUE);
  g_string_free (r.expr, TRUE);
  /* Names last, when every sheet they may point at exists.  The value is
   * a formula, Sheet1!$A$1:$B$2 or a bare range; anything else is passed
   * over, since a name can hold any expression in Gnumeric. */
  for (guint i = 0; i + 1 < r.pending_names->len; i += 2)
    {
      const char *nm = g_ptr_array_index (r.pending_names, i);
      const char *val = g_ptr_array_index (r.pending_names, i + 1);
      O42Node *tree = o42_formula_parse (val[0] == '=' ? val + 1 : val);
      O42Sheet *target = o42_book_sheet (book, 0);
      O42Range range;
      gboolean usable = FALSE;

      if (tree->type == O42_NODE_RANGE)
        { range = tree->as.range; usable = TRUE; }
      else if (tree->type == O42_NODE_REF)
        {
          range.row0 = range.row1 = tree->as.ref.row;
          range.col0 = range.col1 = tree->as.ref.col;
          usable = TRUE;
        }
      if (usable && tree->sheet != NULL)
        {
          target = o42_book_find_sheet (book, tree->sheet);
          usable = (target != NULL);
        }
      if (usable)
        o42_book_define_name (book, nm, target, &range);
      o42_node_free (tree);
    }
  g_ptr_array_free (r.pending_names, TRUE);
  g_string_free (r.name_name, TRUE);
  g_string_free (r.name_value, TRUE);

  g_string_free (r.dimension, TRUE);
  g_string_free (r.graph_title, TRUE);
  g_string_free (r.shape_text, TRUE);
  g_string_free (r.graph_x_title, TRUE);
  g_string_free (r.graph_y_title, TRUE);
  g_hash_table_destroy (r.shared_exprs);
  g_free (r.content_type);
  g_free (xml);

  for (int i = 0; i < o42_book_n_sheets (book); i++)
    o42_sheet_clear_undo (o42_book_sheet (book, i));
  o42_book_set_modified (book, FALSE);

  return ok;
}
