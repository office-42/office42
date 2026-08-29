/* o42-xlsx-draw.c - pictures and charts in .xlsx: the drawing parts
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-xlsx-draw.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "o42-formula.h"
#include "o42-image.h"
#include "o42-shape.h"

#define EMU_PER_PX 9525.0

#define NS_XDR "http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing"
#define NS_A   "http://schemas.openxmlformats.org/drawingml/2006/main"
#define NS_C   "http://schemas.openxmlformats.org/drawingml/2006/chart"
#define NS_R   "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
#define NS_PKG "http://schemas.openxmlformats.org/package/2006/relationships"

/* ---- geometry ---- */

static double
offset_px (O42Sheet *sheet, gboolean cols, int index)
{
  double px = 0;
  for (int i = 0; i < index; i++)
    px += cols ? o42_sheet_col_width (sheet, i) : o42_sheet_row_height (sheet, i);
  return px;
}

/* The cell a pixel position falls in, and how far into it. */
static void
cell_at (O42Sheet *sheet, gboolean cols, double px, int *index, double *offset)
{
  int limit = cols ? O42_MAX_COLS : O42_MAX_ROWS;
  double at = 0;
  for (int i = 0; i < limit - 1; i++)
    {
      double size = cols ? o42_sheet_col_width (sheet, i) : o42_sheet_row_height (sheet, i);
      if (px < at + size)
        {
          *index = i;
          *offset = MAX (px - at, 0);
          return;
        }
      at += size;
    }
  *index = limit - 1;
  *offset = 0;
}

static const char *
local (const char *name)
{
  const char *colon = strrchr (name, ':');
  return colon ? colon + 1 : name;
}

static const char *
attr (const char **names, const char **values, const char *want)
{
  for (int i = 0; names[i] != NULL; i++)
    if (strcmp (local (names[i]), want) == 0)
      return values[i];
  return NULL;
}

/* ====================================================================== */
/* Writing                                                                 */
/* ====================================================================== */

static const char *
mime_for (const char *format)
{
  if (strcmp (format, "png") == 0) return "image/png";
  if (strcmp (format, "jpeg") == 0 || strcmp (format, "jpg") == 0) return "image/jpeg";
  if (strcmp (format, "gif") == 0) return "image/gif";
  if (strcmp (format, "bmp") == 0) return "image/bmp";
  if (strcmp (format, "tiff") == 0) return "image/tiff";
  return "image/png";
}

static void
append_anchor (GString *dr, O42Sheet *sheet, int row, int col, double dx, double dy,
               double width, double height)
{
  double x0 = offset_px (sheet, TRUE, col) + dx;
  double y0 = offset_px (sheet, FALSE, row) + dy;
  int to_col, to_row;
  double to_dx, to_dy;

  cell_at (sheet, TRUE, x0 + width, &to_col, &to_dx);
  cell_at (sheet, FALSE, y0 + height, &to_row, &to_dy);
  g_string_append_printf (dr,
    "<xdr:twoCellAnchor editAs=\"oneCell\">"
    "<xdr:from><xdr:col>%d</xdr:col><xdr:colOff>%.0f</xdr:colOff><xdr:row>%d</xdr:row><xdr:rowOff>%.0f</xdr:rowOff></xdr:from>"
    "<xdr:to><xdr:col>%d</xdr:col><xdr:colOff>%.0f</xdr:colOff><xdr:row>%d</xdr:row><xdr:rowOff>%.0f</xdr:rowOff></xdr:to>",
    col, dx * EMU_PER_PX, row, dy * EMU_PER_PX,
    to_col, to_dx * EMU_PER_PX, to_row, to_dy * EMU_PER_PX);
}

/* A series reference: 'Sheet 1'!$B$2:$B$5. */
static char *
ref_text (const char *sheet_name, int row0, int col0, int row1, int col1)
{
  char *quoted = o42_sheet_name_quote (sheet_name);
  char *a = o42_ref_name_full (row0, col0, TRUE, TRUE);
  char *b = o42_ref_name_full (row1, col1, TRUE, TRUE);
  char *text = (row0 == row1 && col0 == col1) ? g_strdup_printf ("%s!%s", quoted, a)
                                              : g_strdup_printf ("%s!%s:%s", quoted, a, b);
  char *escaped = g_markup_escape_text (text, -1);
  g_free (text);
  g_free (a);
  g_free (b);
  g_free (quoted);
  return escaped;
}

/* The cells one line of the table occupies -- a series, the category
 * axis, or a scatter's x values.  Which way a line runs is the whole of
 * the difference between series in columns and series in rows, so every
 * range in the chart part comes through here. */
static void
line_range (const O42Chart *chart, int first_row, int first_col, int i,
            int *r0, int *c0, int *r1, int *c1)
{
  const O42Range *d = &chart->data;

  if (chart->series_in_rows)
    { *r0 = *r1 = i; *c0 = first_col; *c1 = d->col1; }
  else
    { *c0 = *c1 = i; *r0 = first_row; *r1 = d->row1; }
}


/* An axis title element, or an empty string for none. */
static char *
axis_title_xml (const char *text)
{
  char *t, *xml;
  if (text == NULL || *text == '\0')
    return g_strdup ("");
  t = g_markup_escape_text (text, -1);
  xml = g_strdup_printf ("<c:title><c:tx><c:rich><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>%s</a:t></a:r></a:p></c:rich></c:tx>"
                         "<c:overlay val=\"0\"/></c:title>", t);
  g_free (t);
  return xml;
}

static char *
chart_xml (O42Sheet *sheet, const O42Chart *chart)
{
  GString *out = g_string_new (
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<c:chartSpace xmlns:c=\"" NS_C "\" xmlns:a=\"" NS_A "\" xmlns:r=\"" NS_R "\">"
    "<c:roundedCorners val=\"0\"/><c:chart>");
  const O42Range *d = &chart->data;
  int first_row = d->row0 + (chart->first_row_labels ? 1 : 0);
  int first_col = d->col0 + (chart->first_col_labels ? 1 : 0);
  gboolean scatter = chart->kind == O42_CHART_SCATTER;
  gboolean pie = chart->kind == O42_CHART_PIE;
  gboolean secondary = chart->secondary_from > 0 && !pie;
  /* The sheet the cells are on, which is not the chart's own when it
   * sits on a chart sheet. */
  const char *source = (chart->data_sheet != NULL && chart->data_sheet[0] != '\0')
                       ? chart->data_sheet : o42_sheet_get_name (sheet);
  const char *element;
  int series = 0;
  /* The series colours o42_chart_draw uses, written out so readers
   * that take an absent style as "no fill" (LibreOffice) show bars. */
  static const guint32 COLOURS[] = {
    0x000080, 0x800000, 0x008000, 0x008080, 0x800080, 0x808000, 0x808080, 0x0000FF
  };

  if (chart->title != NULL && chart->title[0] != '\0')
    {
      char *t = g_markup_escape_text (chart->title, -1);
      g_string_append_printf (out,
        "<c:title><c:tx><c:rich><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>%s</a:t></a:r></a:p></c:rich></c:tx>"
        "<c:overlay val=\"0\"/></c:title><c:autoTitleDeleted val=\"0\"/>", t);
      g_free (t);
    }
  else
    g_string_append (out, "<c:autoTitleDeleted val=\"1\"/>");

  g_string_append (out, "<c:plotArea><c:layout/>");
  /* Excel has a 3-D element of its own for each kind that can have
   * depth; a scatter cannot. */
  switch (chart->kind)
    {
    case O42_CHART_LINE:    element = chart->three_d ? "line3DChart" : "lineChart"; break;
    case O42_CHART_PIE:     element = chart->three_d ? "pie3DChart" : "pieChart"; break;
    case O42_CHART_DOUGHNUT: element = "doughnutChart"; break;
    case O42_CHART_RADAR:   element = "radarChart"; break;
    case O42_CHART_BUBBLE:  element = "bubbleChart"; break;
    case O42_CHART_STOCK:   element = "stockChart"; break;
    case O42_CHART_SURFACE: element = "surface3DChart"; break;
    case O42_CHART_AREA:    element = chart->three_d ? "area3DChart" : "areaChart"; break;
    case O42_CHART_SCATTER: element = "scatterChart"; break;
    default:                element = chart->three_d ? "bar3DChart" : "barChart"; break;
    }
  if (chart->three_d)
    g_string_append (out, "<c:view3D><c:rotX val=\"15\"/><c:rotY val=\"20\"/>"
                          "<c:rAngAx val=\"1\"/></c:view3D>");
  /* Series from secondary_from on go into a second chart group with
   * its own pair of axes, which is how Excel writes a secondary axis. */
  for (int group = 0; group < (secondary ? 2 : 1); group++)
    {
  g_string_append_printf (out, "<c:%s>", element);
  switch (chart->kind)
    {
    case O42_CHART_COLUMN:  g_string_append (out, "<c:barDir val=\"col\"/><c:grouping val=\"clustered\"/><c:varyColors val=\"0\"/>"); break;
    case O42_CHART_BAR:     g_string_append (out, "<c:barDir val=\"bar\"/><c:grouping val=\"clustered\"/><c:varyColors val=\"0\"/>"); break;
    case O42_CHART_STACKED: g_string_append (out, "<c:barDir val=\"col\"/><c:grouping val=\"stacked\"/><c:varyColors val=\"0\"/>"); break;
    case O42_CHART_PERCENT: g_string_append (out, "<c:barDir val=\"col\"/><c:grouping val=\"percentStacked\"/><c:varyColors val=\"0\"/>"); break;
    case O42_CHART_LINE:    g_string_append (out, "<c:grouping val=\"standard\"/><c:varyColors val=\"0\"/>"); break;
    case O42_CHART_AREA:    g_string_append (out, "<c:grouping val=\"standard\"/><c:varyColors val=\"0\"/>"); break;
    case O42_CHART_PIE:     g_string_append (out, "<c:varyColors val=\"1\"/>"); break;
    case O42_CHART_SCATTER: g_string_append (out, "<c:scatterStyle val=\"lineMarker\"/><c:varyColors val=\"0\"/>"); break;
    case O42_CHART_DOUGHNUT: g_string_append (out, "<c:varyColors val=\"1\"/>"); break;
    case O42_CHART_RADAR:   g_string_append (out, "<c:radarStyle val=\"marker\"/><c:varyColors val=\"0\"/>"); break;
    case O42_CHART_BUBBLE:  g_string_append (out, "<c:varyColors val=\"0\"/>"); break;
    case O42_CHART_STOCK:   g_string_append (out, "<c:varyColors val=\"0\"/>"); break;
    case O42_CHART_SURFACE: g_string_append (out, "<c:wireframe val=\"0\"/>"); break;
    }

  /* One series per data column, or per data row when the series lie that
   * way.  The heading that names the series and the one that names the
   * categories swap with the orientation, as they do when it is drawn. */
  {
    gboolean rows = chart->series_in_rows;
    gboolean series_named = rows ? chart->first_col_labels : chart->first_row_labels;
    gboolean points_named = rows ? chart->first_row_labels : chart->first_col_labels;
    int cat_line = rows ? d->row0 : d->col0;
    int first = rows ? first_row : first_col;
    int last = rows ? d->row1 : d->col1;
    int x_line = -1;
    int r0, c0, r1, c1;

    /* A scatter's first line is x, not a series of its own. */
    if (scatter)
      { x_line = cat_line; first = cat_line + 1; }

    for (int i = first; i <= last && (!pie || series == 0); i++)
      {
        int ordinal = i - first;

        if (secondary && (ordinal >= chart->secondary_from - 1) != (group == 1))
          continue;
        g_string_append_printf (out, "<c:ser><c:idx val=\"%d\"/><c:order val=\"%d\"/>", ordinal, ordinal);
        if (series_named)
          {
            int nr = rows ? i : d->row0;
            int nc = rows ? d->col0 : i;
            char *ref = ref_text (source, nr, nc, nr, nc);
            g_string_append_printf (out, "<c:tx><c:strRef><c:f>%s</c:f></c:strRef></c:tx>", ref);
            g_free (ref);
          }
        {
          guint32 colour = COLOURS[ordinal % G_N_ELEMENTS (COLOURS)];
          if (chart->kind == O42_CHART_LINE || scatter)
            g_string_append_printf (out,
              "<c:spPr><a:ln w=\"28575\"><a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill></a:ln></c:spPr>", colour);
          else if (!pie)
            g_string_append_printf (out,
              "<c:spPr><a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill></c:spPr>", colour);
          if (chart->kind == O42_CHART_COLUMN || chart->kind == O42_CHART_BAR ||
              chart->kind == O42_CHART_STACKED || chart->kind == O42_CHART_PERCENT)
            g_string_append (out, "<c:invertIfNegative val=\"0\"/>");
        }
        line_range (chart, first_row, first_col, i, &r0, &c0, &r1, &c1);
        if (scatter)
          {
            int xr0, xc0, xr1, xc1;
            char *yref = ref_text (source, r0, c0, r1, c1);
            char *xref;

            line_range (chart, first_row, first_col, x_line, &xr0, &xc0, &xr1, &xc1);
            xref = ref_text (source, xr0, xc0, xr1, xc1);
            g_string_append_printf (out,
              "<c:marker><c:symbol val=\"circle\"/></c:marker>"
              "<c:xVal><c:numRef><c:f>%s</c:f></c:numRef></c:xVal>"
              "<c:yVal><c:numRef><c:f>%s</c:f></c:numRef></c:yVal>", xref, yref);
            g_free (xref);
            g_free (yref);
          }
        else
          {
            char *vref = ref_text (source, r0, c0, r1, c1);
            if (points_named)
              {
                int cr0, cc0, cr1, cc1;
                char *cref;

                line_range (chart, first_row, first_col, cat_line, &cr0, &cc0, &cr1, &cc1);
                cref = ref_text (source, cr0, cc0, cr1, cc1);
                g_string_append_printf (out, "<c:cat><c:strRef><c:f>%s</c:f></c:strRef></c:cat>", cref);
                g_free (cref);
              }
            g_string_append_printf (out, "<c:val><c:numRef><c:f>%s</c:f></c:numRef></c:val>", vref);
            g_free (vref);
          }
        if (chart->data_labels)
          g_string_append (out, "<c:dLbls><c:showLegendKey val=\"0\"/><c:showVal val=\"1\"/><c:showCatName val=\"0\"/>"
                                "<c:showSerName val=\"0\"/><c:showPercent val=\"0\"/><c:showBubbleSize val=\"0\"/></c:dLbls>");
        if (chart->trend != O42_TREND_NONE && !pie)
          {
            static const char *types[] = { "", "linear", "poly", "exp", "log", "power", "movingAvg" };

            g_string_append_printf (out, "<c:trendline><c:trendlineType val=\"%s\"/>",
                                    types[chart->trend]);
            if (chart->trend == O42_TREND_POLY)
              g_string_append_printf (out, "<c:order val=\"%d\"/>", CLAMP (chart->trend_order, 2, 6));
            else if (chart->trend == O42_TREND_MOVING)
              g_string_append_printf (out, "<c:period val=\"%d\"/>", MAX (chart->trend_order, 2));
            g_string_append (out, "</c:trendline>");
          }
        if (chart->err_bars != O42_ERRBAR_NONE && !pie)
          {
            static const char *types[] = { "", "fixedVal", "percentage", "stdDev", "stdErr" };

            g_string_append_printf (out,
              "<c:errBars><c:errDir val=\"y\"/><c:errBarType val=\"both\"/>"
              "<c:errValType val=\"%s\"/><c:noEndCap val=\"0\"/>", types[chart->err_bars]);
            if (chart->err_bars != O42_ERRBAR_STDERR)
              {
                char buf[G_ASCII_DTOSTR_BUF_SIZE];

                g_ascii_dtostr (buf, sizeof buf, chart->err_value);
                g_string_append_printf (out, "<c:val val=\"%s\"/>", buf);
              }
            g_string_append (out, "</c:errBars>");
          }
        g_string_append (out, "</c:ser>");
        series++;
      }
  }

  if (chart->kind == O42_CHART_STACKED || chart->kind == O42_CHART_PERCENT)
    g_string_append (out, "<c:gapWidth val=\"150\"/><c:overlap val=\"100\"/>");
  else if (chart->kind == O42_CHART_COLUMN || chart->kind == O42_CHART_BAR)
    g_string_append (out, "<c:gapWidth val=\"150\"/>");
  else if (chart->kind == O42_CHART_LINE)
    g_string_append (out, "<c:marker val=\"1\"/>");
  if (chart->kind == O42_CHART_SURFACE)
    /* A surface stands on three: the categories across, the values up,
     * and the series into the page. */
    g_string_append (out, "<c:axId val=\"10001\"/><c:axId val=\"10002\"/>"
                          "<c:axId val=\"10005\"/>");
  else if (!pie)
    g_string_append_printf (out, "<c:axId val=\"%d\"/><c:axId val=\"%d\"/>",
                            group == 0 ? 10001 : 10003, group == 0 ? 10002 : 10004);
  g_string_append_printf (out, "</c:%s>", element);
    }

  if (!pie)
    {
      const char *cat_axis = scatter ? "valAx" : "catAx";
      gboolean bar = chart->kind == O42_CHART_BAR;
      char *xt = axis_title_xml (chart->x_title);
      char *yt = axis_title_xml (chart->y_title);
      const char *code = chart->y_format != NULL && *chart->y_format != '\0' ? chart->y_format : "General";
      char minmax[96] = "";

      if (chart->has_max || chart->has_min)
        {
          char a[G_ASCII_DTOSTR_BUF_SIZE], b[G_ASCII_DTOSTR_BUF_SIZE];
          g_snprintf (minmax, sizeof minmax, "%s%s%s%s%s%s",
                      chart->has_max ? "<c:max val=\"" : "", chart->has_max ? g_ascii_dtostr (a, sizeof a, chart->max) : "", chart->has_max ? "\"/>" : "",
                      chart->has_min ? "<c:min val=\"" : "", chart->has_min ? g_ascii_dtostr (b, sizeof b, chart->min) : "", chart->has_min ? "\"/>" : "");
        }
      g_string_append_printf (out,
        "<c:%s><c:axId val=\"10001\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/>"
        "<c:axPos val=\"%s\"/>%s<c:numFmt formatCode=\"General\" sourceLinked=\"1\"/><c:tickLblPos val=\"nextTo\"/>"
        "<c:crossAx val=\"10002\"/><c:crosses val=\"autoZero\"/>%s</c:%s>",
        cat_axis, bar ? "l" : "b", xt,
        scatter ? "<c:crossBetween val=\"midCat\"/>" : "<c:auto val=\"1\"/><c:lblAlgn val=\"ctr\"/><c:lblOffset val=\"100\"/>",
        cat_axis);
      g_string_append_printf (out,
        "<c:valAx><c:axId val=\"10002\"/><c:scaling><c:orientation val=\"minMax\"/>%s</c:scaling><c:delete val=\"0\"/>"
        "<c:axPos val=\"%s\"/>%s%s<c:numFmt formatCode=\"%s\" sourceLinked=\"%d\"/>"
        "<c:tickLblPos val=\"nextTo\"/><c:crossAx val=\"10001\"/><c:crosses val=\"autoZero\"/>"
        "<c:crossBetween val=\"%s\"/></c:valAx>",
        minmax, bar ? "b" : "l", chart->gridlines ? "<c:majorGridlines/>" : "", yt,
        code, *code == 'G' ? 1 : 0,
        scatter ? "midCat" : "between");
      if (chart->kind == O42_CHART_SURFACE)
        g_string_append (out,
          "<c:serAx><c:axId val=\"10005\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling>"
          "<c:delete val=\"0\"/><c:axPos val=\"b\"/><c:tickLblPos val=\"nextTo\"/>"
          "<c:crossAx val=\"10002\"/></c:serAx>");

      if (secondary)
        {
          /* The hidden category axis the second group needs, and the
           * value axis down the right. */
          g_string_append_printf (out,
            "<c:%s><c:axId val=\"10003\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"1\"/>"
            "<c:axPos val=\"%s\"/><c:crossAx val=\"10004\"/></c:%s>", cat_axis, bar ? "l" : "b", cat_axis);
          g_string_append_printf (out,
            "<c:valAx><c:axId val=\"10004\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/>"
            "<c:axPos val=\"%s\"/><c:numFmt formatCode=\"General\" sourceLinked=\"1\"/>"
            "<c:tickLblPos val=\"nextTo\"/><c:crossAx val=\"10003\"/><c:crosses val=\"max\"/>"
            "<c:crossBetween val=\"between\"/></c:valAx>", bar ? "t" : "r");
        }
      g_free (xt);
      g_free (yt);
    }
  g_string_append (out, "</c:plotArea>");
  if ((series > 1 || pie) && chart->legend)
    g_string_append (out, "<c:legend><c:legendPos val=\"r\"/><c:overlay val=\"0\"/></c:legend>");
  g_string_append (out, "<c:plotVisOnly val=\"1\"/><c:dispBlanksAs val=\"gap\"/></c:chart>");
  /* The face and size the whole chart's text is set in, which is where
   * Excel keeps it too: one c:txPr on the chart space. */
  if ((chart->font_family != NULL && chart->font_family[0] != '\0') || chart->font_size > 0)
    {
      char *face = g_markup_escape_text (chart->font_family != NULL ? chart->font_family : "", -1);

      g_string_append (out, "<c:txPr><a:bodyPr/><a:lstStyle/><a:p><a:pPr><a:defRPr");
      if (chart->font_size > 0)
        g_string_append_printf (out, " sz=\"%d\"", (int) (chart->font_size * 100));
      g_string_append (out, ">");
      if (face[0] != '\0')
        g_string_append_printf (out, "<a:latin typeface=\"%s\"/>", face);
      g_string_append (out, "</a:defRPr></a:pPr><a:endParaRPr lang=\"en-US\"/></a:p></c:txPr>");
      g_free (face);
    }
  g_string_append (out, "</c:chartSpace>");
  return g_string_free (out, FALSE);
}

int
o42_xlsx_draw_write (O42ZipWriter *zip, O42Sheet *sheet, int index,
                     GString *content_types, GHashTable *extensions_seen,
                     GString *sheet_rels, int *next_rid)
{
  GPtrArray *pictures = o42_sheet_pictures (sheet);
  GPtrArray *charts = o42_sheet_charts (sheet);
  GPtrArray *shapes = o42_sheet_shapes (sheet);
  GString *dr, *rels, *s;
  int rid = 1, shape = 2, own_rid;

  if (pictures->len == 0 && charts->len == 0 && shapes->len == 0)
    return 0;

  dr = g_string_new (
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<xdr:wsDr xmlns:xdr=\"" NS_XDR "\" xmlns:a=\"" NS_A "\" xmlns:r=\"" NS_R "\">");
  rels = g_string_new (
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"" NS_PKG "\">");

  for (guint i = 0; i < pictures->len; i++)
    {
      const O42Picture *pic = g_ptr_array_index (pictures, i);
      const char *ext = pic->format ? pic->format : "png";
      char *part = g_strdup_printf ("xl/media/image%d_%u.%s", index, i + 1, ext);

      o42_zip_writer_add (zip, part, g_bytes_get_data (pic->data, NULL), g_bytes_get_size (pic->data));
      if (!g_hash_table_contains (extensions_seen, ext))
        {
          g_hash_table_add (extensions_seen, g_strdup (ext));
          g_string_append_printf (content_types, "<Default Extension=\"%s\" ContentType=\"%s\"/>", ext, mime_for (ext));
        }
      g_string_append_printf (rels,
        "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image%d_%u.%s\"/>",
        rid, index, i + 1, ext);

      append_anchor (dr, sheet, pic->row, pic->col, pic->dx, pic->dy, pic->width, pic->height);
      g_string_append_printf (dr,
        "<xdr:pic><xdr:nvPicPr><xdr:cNvPr id=\"%d\" name=\"Picture %u\"/><xdr:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></xdr:cNvPicPr></xdr:nvPicPr>"
        "<xdr:blipFill><a:blip r:embed=\"rId%d\"/><a:stretch><a:fillRect/></a:stretch></xdr:blipFill>"
        "<xdr:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%.0f\" cy=\"%.0f\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></xdr:spPr></xdr:pic>"
        "<xdr:clientData/></xdr:twoCellAnchor>",
        shape, i + 1, rid, pic->width * EMU_PER_PX, pic->height * EMU_PER_PX);
      rid++;
      shape++;
      g_free (part);
    }

  for (guint i = 0; i < charts->len; i++)
    {
      const O42Chart *chart = g_ptr_array_index (charts, i);
      char *part = g_strdup_printf ("xl/charts/chart%d_%u.xml", index, i + 1);
      char *xml = chart_xml (sheet, chart);

      o42_zip_writer_add (zip, part, xml, strlen (xml));
      g_string_append_printf (content_types,
        "<Override PartName=\"/%s\" ContentType=\"application/vnd.openxmlformats-officedocument.drawingml.chart+xml\"/>", part);
      g_string_append_printf (rels,
        "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart\" Target=\"../charts/chart%d_%u.xml\"/>",
        rid, index, i + 1);

      /* A chart sheet has no cells to anchor to, so its chart is
       * placed absolutely, as Excel places one. */
      if (o42_sheet_is_chart_sheet (sheet))
        g_string_append_printf (dr,
          "<xdr:absoluteAnchor><xdr:pos x=\"0\" y=\"0\"/><xdr:ext cx=\"%.0f\" cy=\"%.0f\"/>",
          chart->width * EMU_PER_PX, chart->height * EMU_PER_PX);
      else
        append_anchor (dr, sheet, chart->row, chart->col, chart->dx, chart->dy,
                       chart->width, chart->height);
      g_string_append_printf (dr,
        "<xdr:graphicFrame macro=\"\"><xdr:nvGraphicFramePr><xdr:cNvPr id=\"%d\" name=\"Chart %u\"/><xdr:cNvGraphicFramePr/></xdr:nvGraphicFramePr>"
        "<xdr:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/></xdr:xfrm>"
        "<a:graphic><a:graphicData uri=\"" NS_C "\"><c:chart xmlns:c=\"" NS_C "\" r:id=\"rId%d\"/></a:graphicData></a:graphic>"
        "</xdr:graphicFrame><xdr:clientData/>%s",
        shape, i + 1, rid,
        o42_sheet_is_chart_sheet (sheet) ? "</xdr:absoluteAnchor>" : "</xdr:twoCellAnchor>");
      rid++;
      shape++;
      g_free (xml);
      g_free (part);
    }

  /* Shapes are drawn by the file rather than carried in it: a preset
   * geometry, a fill, an outline and whatever is written inside. */
  for (guint i = 0; i < shapes->len; i++)
    {
      const O42Shape *sh = g_ptr_array_index (shapes, i);
      gboolean stroke = sh->kind == O42_SHAPE_LINE || sh->kind == O42_SHAPE_ARROW;

      append_anchor (dr, sheet, sh->row, sh->col, sh->dx, sh->dy, sh->width, sh->height);
      g_string_append_printf (dr,
        "<xdr:sp macro=\"\" textlink=\"\"><xdr:nvSpPr><xdr:cNvPr id=\"%d\" name=\"%s %u\"/>"
        "<xdr:cNvSpPr%s/></xdr:nvSpPr>"
        "<xdr:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%.0f\" cy=\"%.0f\"/></a:xfrm>"
        "<a:prstGeom prst=\"%s\"><a:avLst/></a:prstGeom>",
        shape, sh->kind == O42_SHAPE_TEXT ? "TextBox" : "Shape", i + 1,
        sh->kind == O42_SHAPE_TEXT ? " txBox=\"1\"" : "",
        sh->width * EMU_PER_PX, sh->height * EMU_PER_PX,
        sh->kind == O42_SHAPE_OVAL ? "ellipse" : stroke ? "line" : "rect");
      if (sh->fill == O42_FILL_NONE || stroke)
        g_string_append (dr, "<a:noFill/>");
      else
        g_string_append_printf (dr, "<a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill>",
                                sh->fill & 0xFFFFFFu);
      g_string_append_printf (dr,
        "<a:ln w=\"%.0f\"><a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill>",
        sh->line_width * EMU_PER_PX, sh->line & 0xFFFFFFu);
      if (sh->kind == O42_SHAPE_ARROW)
        g_string_append (dr, "<a:tailEnd type=\"triangle\"/>");
      g_string_append (dr, "</a:ln></xdr:spPr>");
      g_string_append (dr,
        "<xdr:txBody><a:bodyPr vertOverflow=\"clip\" wrap=\"square\"/><a:lstStyle/><a:p>");
      if (sh->text != NULL && sh->text[0] != '\0')
        {
          char *t = g_markup_escape_text (sh->text, -1);
          g_string_append_printf (dr, "<a:r><a:rPr lang=\"en-US\"/><a:t>%s</a:t></a:r>", t);
          g_free (t);
        }
      g_string_append (dr, "</a:p></xdr:txBody></xdr:sp><xdr:clientData/></xdr:twoCellAnchor>");
      shape++;
    }

  g_string_append (dr, "</xdr:wsDr>");
  g_string_append (rels, "</Relationships>");

  s = g_string_new (NULL);
  g_string_printf (s, "xl/drawings/drawing%d.xml", index);
  o42_zip_writer_add (zip, s->str, dr->str, dr->len);
  g_string_append_printf (content_types,
    "<Override PartName=\"/xl/drawings/drawing%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.drawing+xml\"/>", index);
  g_string_printf (s, "xl/drawings/_rels/drawing%d.xml.rels", index);
  o42_zip_writer_add (zip, s->str, rels->str, rels->len);

  own_rid = (*next_rid)++;
  g_string_append_printf (sheet_rels,
    "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/drawing\" Target=\"../drawings/drawing%d.xml\"/>",
    own_rid, index);

  g_string_free (s, TRUE);
  g_string_free (dr, TRUE);
  g_string_free (rels, TRUE);
  return own_rid;
}

/* ====================================================================== */
/* Reading                                                                 */
/* ====================================================================== */

/* "xl/worksheets/sheet1.xml" -> "xl/worksheets/" */
static char *
part_dir (const char *part)
{
  const char *slash = strrchr (part, '/');
  return slash ? g_strndup (part, slash - part + 1) : g_strdup ("");
}

/* A relationship target against the directory of the part it is in:
 * "../media/image1.png" from "xl/drawings/" is "xl/media/image1.png". */
static char *
resolve (const char *dir, const char *target)
{
  char **parts;
  GPtrArray *stack = g_ptr_array_new ();
  char *joined, *result;

  if (target[0] == '/')
    joined = g_strdup (target + 1);
  else
    joined = g_strconcat (dir, target, NULL);
  parts = g_strsplit (joined, "/", -1);
  for (int i = 0; parts[i] != NULL; i++)
    {
      if (strcmp (parts[i], "..") == 0)
        { if (stack->len > 0) g_ptr_array_remove_index (stack, stack->len - 1); }
      else if (strcmp (parts[i], ".") != 0 && parts[i][0] != '\0')
        g_ptr_array_add (stack, parts[i]);
    }
  g_ptr_array_add (stack, NULL);
  result = g_strjoinv ("/", (char **) stack->pdata);
  g_ptr_array_unref (stack);
  g_strfreev (parts);
  g_free (joined);
  return result;
}

char *
o42_xlsx_resolve (const char *part, const char *target)
{
  char *dir = part_dir (part);
  char *result = resolve (dir, target);
  g_free (dir);
  return result;
}

static char *
rels_part_for (const char *part)
{
  char *dir = part_dir (part);
  const char *base = strrchr (part, '/');
  char *rels = g_strdup_printf ("%s_rels/%s.rels", dir, base ? base + 1 : part);
  g_free (dir);
  return rels;
}

static void
rels_start (GMarkupParseContext *ctx, const char *name, const char **names,
            const char **values, gpointer user, GError **error)
{
  (void) ctx; (void) error;
  if (strcmp (local (name), "Relationship") == 0)
    {
      const char *id = attr (names, values, "Id");
      const char *target = attr (names, values, "Target");
      if (id && target)
        g_hash_table_insert (user, g_strdup (id), g_strdup (target));
    }
}

static gboolean
parse_part (GHashTable *parts, const char *path, const GMarkupParser *parser, gpointer user)
{
  GBytes *bytes = g_hash_table_lookup (parts, path);
  GMarkupParseContext *ctx;
  gboolean ok;
  gsize len;
  const char *xml;

  if (bytes == NULL)
    return FALSE;
  xml = g_bytes_get_data (bytes, &len);
  ctx = g_markup_parse_context_new (parser, G_MARKUP_TREAT_CDATA_AS_TEXT, user, NULL);
  ok = g_markup_parse_context_parse (ctx, xml, (gssize) len, NULL) &&
       g_markup_parse_context_end_parse (ctx, NULL);
  g_markup_parse_context_free (ctx);
  return ok;
}

GHashTable *
o42_xlsx_read_rels (GHashTable *parts, const char *part)
{
  static const GMarkupParser parser = { rels_start, NULL, NULL, NULL, NULL };
  GHashTable *rels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  char *rels_part = rels_part_for (part);
  parse_part (parts, rels_part, &parser, rels);
  g_free (rels_part);
  return rels;
}

/* ---- a chart part ---- */

typedef struct
{
  O42ChartKind kind;
  gboolean     kind_known;
  gboolean     in_f, in_title, in_t;
  int          in_axis;        /* 1 in catAx (or the first valAx of a scatter), 2 in valAx */
  gboolean     saw_valax, saw_grid, saw_legend, saw_labels, has_min, has_max, in_err;
  gboolean     three_d;
  char        *font_family;
  double       font_size;
  O42ErrBarKind err_bars;
  double        err_value;
  O42TrendKind trend;
  int          trend_order;
  int          groups, n_ser, secondary_at;
  double       min, max;
  GString     *x_title, *y_title, *y_format;
  const char  *role;      /* tx, cat, val, xVal, yVal */
  GString     *f;
  GString     *title;
  O42Range     box;
  const char  *sheet;     /* interned, from the first reference */
  gboolean     have_box, have_tx, have_cat;
} ChartReader;

static void
chart_start (GMarkupParseContext *ctx, const char *name, const char **names,
             const char **values, gpointer user, GError **error)
{
  ChartReader *c = user;
  const char *n = local (name);
  (void) ctx; (void) error;

  /* Each plot element opens a chart group; a second group is what a
   * secondary axis looks like in the file. */
  if (g_str_has_suffix (n, "Chart"))
    {
      c->groups++;
      if (g_str_has_suffix (n, "3DChart"))
        c->three_d = TRUE;
    }

  if (strcmp (n, "barChart") == 0 || strcmp (n, "bar3DChart") == 0)
    { c->kind = O42_CHART_COLUMN; c->kind_known = TRUE; }
  else if (strcmp (n, "lineChart") == 0 || strcmp (n, "line3DChart") == 0)
    { c->kind = O42_CHART_LINE; c->kind_known = TRUE; }
  else if (strcmp (n, "pieChart") == 0 || strcmp (n, "pie3DChart") == 0)
    { c->kind = O42_CHART_PIE; c->kind_known = TRUE; }
  else if (strcmp (n, "doughnutChart") == 0)
    { c->kind = O42_CHART_DOUGHNUT; c->kind_known = TRUE; }
  else if (strcmp (n, "radarChart") == 0)
    { c->kind = O42_CHART_RADAR; c->kind_known = TRUE; }
  else if (strcmp (n, "bubbleChart") == 0)
    { c->kind = O42_CHART_BUBBLE; c->kind_known = TRUE; }
  else if (strcmp (n, "areaChart") == 0 || strcmp (n, "area3DChart") == 0)
    { c->kind = O42_CHART_AREA; c->kind_known = TRUE; }
  else if (strcmp (n, "scatterChart") == 0)
    { c->kind = O42_CHART_SCATTER; c->kind_known = TRUE; }
  else if (strcmp (n, "surfaceChart") == 0 || strcmp (n, "surface3DChart") == 0)
    { c->kind = O42_CHART_SURFACE; c->kind_known = TRUE; }
  else if (strcmp (n, "stockChart") == 0)
    { c->kind = O42_CHART_STOCK; c->kind_known = TRUE; }
  else if (strcmp (n, "barDir") == 0)
    {
      const char *v = attr (names, values, "val");
      if (v && strcmp (v, "bar") == 0 && c->kind == O42_CHART_COLUMN)
        c->kind = O42_CHART_BAR;
    }
  else if (strcmp (n, "grouping") == 0)
    {
      const char *v = attr (names, values, "val");
      if (v && c->kind == O42_CHART_COLUMN)
        {
          if (strcmp (v, "stacked") == 0) c->kind = O42_CHART_STACKED;
          else if (strcmp (v, "percentStacked") == 0) c->kind = O42_CHART_PERCENT;
        }
    }
  else if (strcmp (n, "val") == 0 && c->in_err)
    {
      const char *v = attr (names, values, "val");

      if (v != NULL)
        c->err_value = g_ascii_strtod (v, NULL);
      c->in_err = FALSE;
    }
  else if (strcmp (n, "tx") == 0 || strcmp (n, "cat") == 0 || strcmp (n, "val") == 0 ||
           strcmp (n, "xVal") == 0 || strcmp (n, "yVal") == 0)
    c->role = g_intern_string (n);
  else if (strcmp (n, "f") == 0)
    { c->in_f = TRUE; g_string_truncate (c->f, 0); }
  else if (strcmp (n, "ser") == 0)
    {
      /* Series in the second chart group are the ones on a secondary axis. */
      c->n_ser++;
      if (c->groups > 1 && c->secondary_at == 0)
        c->secondary_at = c->n_ser;
    }
  else if (strcmp (n, "catAx") == 0)
    c->in_axis = 1;
  else if (strcmp (n, "valAx") == 0)
    {
      /* A scatter has two value axes: the first is x. */
      c->in_axis = (c->kind == O42_CHART_SCATTER && !c->saw_valax) ? 1 : 2;
      if (c->in_axis == 2) c->saw_valax = TRUE;
    }
  else if (strcmp (n, "majorGridlines") == 0 && c->in_axis == 2)
    c->saw_grid = TRUE;
  else if (strcmp (n, "legend") == 0)
    c->saw_legend = TRUE;
  else if (strcmp (n, "numFmt") == 0 && c->in_axis == 2 && c->y_format->len == 0)
    {
      const char *code = attr (names, values, "formatCode");
      if (code != NULL && strcmp (code, "General") != 0)
        g_string_append (c->y_format, code);
    }
  else if (strcmp (n, "defRPr") == 0 && c->font_size == 0)
    {
      const char *sz = attr (names, values, "sz");

      if (sz != NULL)
        c->font_size = g_ascii_strtod (sz, NULL) / 100;
    }
  else if (strcmp (n, "latin") == 0 && c->font_family == NULL)
    {
      const char *face = attr (names, values, "typeface");

      if (face != NULL && face[0] != '\0' && face[0] != '+')
        c->font_family = g_strdup (face);
    }
  else if (strcmp (n, "errValType") == 0)
    {
      const char *v = attr (names, values, "val");

      if (v == NULL || !o42_errbar_kind_parse (v, &c->err_bars))
        c->err_bars = O42_ERRBAR_FIXED;
      c->in_err = TRUE;
    }
  else if (strcmp (n, "trendlineType") == 0)
    {
      const char *v = attr (names, values, "val");

      if (v == NULL || !o42_trend_kind_parse (v, &c->trend))
        c->trend = O42_TREND_LINEAR;
    }
  else if ((strcmp (n, "order") == 0 || strcmp (n, "period") == 0) &&
           c->trend != O42_TREND_NONE)
    {
      const char *v = attr (names, values, "val");

      if (v != NULL)
        c->trend_order = CLAMP (atoi (v), 2, 6);
    }
  else if (strcmp (n, "showVal") == 0)
    {
      const char *v = attr (names, values, "val");
      if (v != NULL && (strcmp (v, "1") == 0 || strcmp (v, "true") == 0))
        c->saw_labels = TRUE;
    }
  else if ((strcmp (n, "min") == 0 || strcmp (n, "max") == 0) && c->in_axis == 2)
    {
      const char *v = attr (names, values, "val");
      if (v != NULL && n[1] == 'i') { c->has_min = TRUE; c->min = g_ascii_strtod (v, NULL); }
      if (v != NULL && n[1] == 'a') { c->has_max = TRUE; c->max = g_ascii_strtod (v, NULL); }
    }
  else if (strcmp (n, "title") == 0)
    c->in_title = TRUE;
  else if (strcmp (n, "t") == 0 && c->in_title)
    c->in_t = TRUE;
}

static void
chart_end (GMarkupParseContext *ctx, const char *name, gpointer user, GError **error)
{
  ChartReader *c = user;
  const char *n = local (name);
  (void) ctx; (void) error;

  if (strcmp (n, "f") == 0 && c->in_f)
    {
      O42Node *tree = o42_formula_parse (c->f->str);
      O42Range r;
      gboolean usable = FALSE;

      c->in_f = FALSE;
      if (tree->type == O42_NODE_RANGE)
        { r = tree->as.range; usable = TRUE; }
      else if (tree->type == O42_NODE_REF)
        {
          r.row0 = r.row1 = tree->as.ref.row;
          r.col0 = r.col1 = tree->as.ref.col;
          usable = TRUE;
        }
      /* Only the chart's own sheet's cells can be charted here. */
      if (usable && c->role != NULL && c->role != g_intern_string ("title"))
        {
          if (c->sheet == NULL)
            c->sheet = tree->sheet;
          if (tree->sheet == c->sheet)
            {
              if (!c->have_box)
                { c->box = r; c->have_box = TRUE; }
              else
                {
                  c->box.row0 = MIN (c->box.row0, r.row0);
                  c->box.col0 = MIN (c->box.col0, r.col0);
                  c->box.row1 = MAX (c->box.row1, r.row1);
                  c->box.col1 = MAX (c->box.col1, r.col1);
                }
              if (strcmp (c->role, "tx") == 0) c->have_tx = TRUE;
              if (strcmp (c->role, "cat") == 0) c->have_cat = TRUE;
            }
        }
      o42_node_free (tree);
    }
  else if (strcmp (n, "tx") == 0 || strcmp (n, "cat") == 0 || strcmp (n, "val") == 0 ||
           strcmp (n, "xVal") == 0 || strcmp (n, "yVal") == 0)
    c->role = NULL;
  else if (strcmp (n, "title") == 0)
    c->in_title = FALSE;
  else if (strcmp (n, "t") == 0)
    c->in_t = FALSE;
  else if (strcmp (n, "catAx") == 0 || strcmp (n, "valAx") == 0)
    c->in_axis = 0;
}

static void
chart_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer user, GError **error)
{
  ChartReader *c = user;
  (void) ctx; (void) error;
  if (c->in_f)
    g_string_append_len (c->f, text, (gssize) len);
  else if (c->in_title && c->in_t)
    g_string_append_len (c->in_axis == 1 ? c->x_title : c->in_axis == 2 ? c->y_title : c->title, text, (gssize) len);
}

static void
add_chart_from_part (GHashTable *parts, const char *part, O42Sheet *sheet,
                     int row, int col, double dx, double dy, double width, double height)
{
  static const GMarkupParser parser = { chart_start, chart_end, chart_text, NULL, NULL };
  ChartReader c;

  memset (&c, 0, sizeof c);
  c.f = g_string_new (NULL);
  c.title = g_string_new (NULL);
  c.x_title = g_string_new (NULL);
  c.y_title = g_string_new (NULL);
  c.y_format = g_string_new (NULL);
  if (parse_part (parts, part, &parser, &c) && c.have_box)
    {
      O42Chart *chart = o42_sheet_add_chart (sheet, c.kind_known ? c.kind : O42_CHART_COLUMN, &c.box, row, col);
      if (chart != NULL)
        {
          chart->first_row_labels = c.have_tx;
          chart->first_col_labels = c.have_cat || c.kind == O42_CHART_SCATTER;
          g_free (chart->title);
          chart->title = g_strdup (c.title->str);
          g_free (chart->x_title);
          chart->x_title = g_strdup (c.x_title->str);
          g_free (chart->y_title);
          chart->y_title = g_strdup (c.y_title->str);
          chart->legend = c.saw_legend;
          chart->data_labels = c.saw_labels;
          g_free (chart->y_format);
          chart->y_format = g_strdup (c.y_format->str);
          chart->trend = c.trend;
          chart->trend_order = c.trend_order > 0 ? c.trend_order : 2;
          chart->three_d = c.three_d;
          chart->err_bars = c.err_bars;
          chart->err_value = c.err_value;
          if (c.sheet != NULL && g_ascii_strcasecmp (c.sheet, o42_sheet_get_name (sheet)) != 0)
            {
              g_free (chart->data_sheet);
              chart->data_sheet = g_strdup (c.sheet);
            }
          if (c.font_family != NULL)
            {
              g_free (chart->font_family);
              chart->font_family = g_strdup (c.font_family);
            }
          chart->font_size = c.font_size;
          chart->secondary_from = c.secondary_at;
          chart->gridlines = !c.saw_valax || c.saw_grid;
          chart->has_min = c.has_min; chart->min = c.min;
          chart->has_max = c.has_max; chart->max = c.max;
          chart->dx = dx;
          chart->dy = dy;
          chart->width = width;
          chart->height = height;
        }
    }
  g_string_free (c.f, TRUE);
  g_string_free (c.title, TRUE);
  g_string_free (c.x_title, TRUE);
  g_string_free (c.y_title, TRUE);
  g_string_free (c.y_format, TRUE);
  g_free (c.font_family);
}

/* ---- the drawing part ---- */

typedef struct
{
  GHashTable *parts;
  GHashTable *rels;        /* the drawing's relationships */
  char       *dir;         /* the drawing part's directory */
  O42Sheet   *sheet;

  gboolean    in_anchor, in_from, in_to;
  int         from_col, from_row, to_col, to_row;
  double      from_coff, from_roff, to_coff, to_roff;
  double      abs_x, abs_y;
  gboolean    absolute, have_to, have_ext;
  double      ext_cx, ext_cy;
  const char *field;       /* col, row, colOff, rowOff being read */
  GString    *text;
  char       *blip, *chart;

  gboolean    is_shape;    /* an xdr:sp: a shape the file describes */
  gboolean    in_line;     /* inside a:ln, so a colour is the outline's */
  gboolean    in_body;     /* inside xdr:txBody, so a:t is the shape's text */
  char        geom[24];    /* the a:prstGeom preset */
  gboolean    arrow, text_box;
  guint32     fill, line;
  double      line_width;
  GString    *body;
} DrawReader;

/* An a:srgbClr or a:sysClr as 0x00RRGGBB. */
static gboolean
draw_colour (const char **names, const char **values, guint32 *out)
{
  const char *v = attr (names, values, "val");
  const char *last = attr (names, values, "lastClr");

  if (v == NULL || strcmp (v, "window") == 0 || strcmp (v, "windowText") == 0)
    v = last;
  if (v == NULL || strlen (v) != 6)
    return FALSE;
  *out = (guint32) g_ascii_strtoull (v, NULL, 16) & 0xFFFFFFu;
  return TRUE;
}

static void
draw_start (GMarkupParseContext *ctx, const char *name, const char **names,
            const char **values, gpointer user, GError **error)
{
  DrawReader *d = user;
  const char *n = local (name);
  (void) ctx; (void) error;

  if (strcmp (n, "oneCellAnchor") == 0 || strcmp (n, "twoCellAnchor") == 0 || strcmp (n, "absoluteAnchor") == 0)
    {
      d->in_anchor = TRUE;
      d->in_from = d->in_to = FALSE;
      d->from_col = d->from_row = d->to_col = d->to_row = 0;
      d->from_coff = d->from_roff = d->to_coff = d->to_roff = 0;
      d->absolute = strcmp (n, "absoluteAnchor") == 0;
      d->have_to = d->have_ext = FALSE;
      d->abs_x = d->abs_y = 0;
      d->is_shape = d->in_line = d->in_body = d->arrow = d->text_box = FALSE;
      d->geom[0] = '\0';
      d->fill = O42_FILL_NONE;
      d->line = 0x000000u;
      d->line_width = 1;
      g_string_truncate (d->body, 0);
      g_clear_pointer (&d->blip, g_free);
      g_clear_pointer (&d->chart, g_free);
    }
  else if (!d->in_anchor)
    return;
  else if (strcmp (n, "from") == 0) d->in_from = TRUE;
  else if (strcmp (n, "to") == 0) { d->in_to = TRUE; d->have_to = TRUE; }
  else if (strcmp (n, "pos") == 0)
    {
      const char *x = attr (names, values, "x"), *y = attr (names, values, "y");
      if (x) d->abs_x = g_ascii_strtod (x, NULL) / EMU_PER_PX;
      if (y) d->abs_y = g_ascii_strtod (y, NULL) / EMU_PER_PX;
    }
  else if (strcmp (n, "ext") == 0 && !d->have_ext)
    {
      const char *cx = attr (names, values, "cx"), *cy = attr (names, values, "cy");
      if (cx && cy && strcmp (cx, "0") != 0)
        {
          d->ext_cx = g_ascii_strtod (cx, NULL) / EMU_PER_PX;
          d->ext_cy = g_ascii_strtod (cy, NULL) / EMU_PER_PX;
          d->have_ext = TRUE;
        }
    }
  else if ((d->in_from || d->in_to) &&
           (strcmp (n, "col") == 0 || strcmp (n, "row") == 0 || strcmp (n, "colOff") == 0 || strcmp (n, "rowOff") == 0))
    {
      d->field = g_intern_string (n);
      g_string_truncate (d->text, 0);
    }
  else if (strcmp (n, "sp") == 0)
    d->is_shape = TRUE;
  else if (strcmp (n, "cNvSpPr") == 0)
    {
      const char *box = attr (names, values, "txBox");
      d->text_box = box != NULL && strcmp (box, "0") != 0;
    }
  else if (strcmp (n, "prstGeom") == 0)
    {
      const char *prst = attr (names, values, "prst");
      if (prst != NULL)
        g_strlcpy (d->geom, prst, sizeof d->geom);
    }
  else if (strcmp (n, "ln") == 0 && d->is_shape)
    {
      const char *w = attr (names, values, "w");
      d->in_line = TRUE;
      if (w != NULL)
        {
          /* EMU are whole numbers, so 1.5px comes back as 1.50005. */
          double px = g_ascii_strtod (w, NULL) / EMU_PER_PX;
          d->line_width = floor (px * 100 + 0.5) / 100;
        }
      if (d->line_width < 0.5)
        d->line_width = 1;
    }
  else if ((strcmp (n, "tailEnd") == 0 || strcmp (n, "headEnd") == 0) && d->is_shape)
    {
      const char *type = attr (names, values, "type");
      if (type != NULL && strcmp (type, "none") != 0)
        d->arrow = TRUE;
    }
  else if (strcmp (n, "noFill") == 0 && d->is_shape && !d->in_line)
    d->fill = O42_FILL_NONE;
  else if ((strcmp (n, "srgbClr") == 0 || strcmp (n, "sysClr") == 0) && d->is_shape)
    {
      guint32 colour;

      if (draw_colour (names, values, &colour))
        {
          if (d->in_line) d->line = colour;
          else if (!d->in_body) d->fill = colour;
        }
    }
  else if (strcmp (n, "txBody") == 0)
    d->in_body = TRUE;
  else if (strcmp (n, "p") == 0 && d->in_body && d->body->len > 0)
    g_string_append_c (d->body, '\n');   /* the line before this one ended */
  else if (strcmp (n, "t") == 0 && d->in_body)
    {
      d->field = g_intern_static_string ("t");
      g_string_truncate (d->text, 0);
    }
  else if (strcmp (n, "blip") == 0)
    {
      const char *embed = attr (names, values, "embed");
      if (embed) { g_free (d->blip); d->blip = g_strdup (embed); }
    }
  else if (strcmp (n, "chart") == 0)
    {
      const char *id = attr (names, values, "id");
      if (id) { g_free (d->chart); d->chart = g_strdup (id); }
    }
}

static void
finish_anchor (DrawReader *d)
{
  int row, col;
  double dx, dy, width, height, x0, y0;

  if (d->absolute)
    {
      x0 = d->abs_x;
      y0 = d->abs_y;
      cell_at (d->sheet, TRUE, x0, &col, &dx);
      cell_at (d->sheet, FALSE, y0, &row, &dy);
    }
  else
    {
      row = CLAMP (d->from_row, 0, O42_MAX_ROWS - 1);
      col = CLAMP (d->from_col, 0, O42_MAX_COLS - 1);
      dx = d->from_coff;
      dy = d->from_roff;
      x0 = offset_px (d->sheet, TRUE, col) + dx;
      y0 = offset_px (d->sheet, FALSE, row) + dy;
    }
  if (d->have_ext)
    { width = d->ext_cx; height = d->ext_cy; }
  else if (d->have_to)
    {
      width = offset_px (d->sheet, TRUE, CLAMP (d->to_col, 0, O42_MAX_COLS - 1)) + d->to_coff - x0;
      height = offset_px (d->sheet, FALSE, CLAMP (d->to_row, 0, O42_MAX_ROWS - 1)) + d->to_roff - y0;
    }
  else
    { width = 200; height = 150; }
  if (!d->is_shape)
    {
      if (width < 4) width = 4;
      if (height < 4) height = 4;
    }
  if (width < 0) width = 0;
  if (height < 0) height = 0;

  if (d->blip != NULL)
    {
      const char *target = g_hash_table_lookup (d->rels, d->blip);
      char *part = target ? resolve (d->dir, target) : NULL;
      GBytes *data = part ? g_hash_table_lookup (d->parts, part) : NULL;
      int pw, ph;
      const char *format;

      if (data != NULL && o42_image_probe (data, &pw, &ph, &format))
        {
          O42Picture *pic = o42_sheet_add_picture (d->sheet, data, format, pw, ph, row, col);
          if (pic != NULL)
            {
              pic->dx = dx;
              pic->dy = dy;
              pic->width = width;
              pic->height = height;
            }
        }
      g_free (part);
    }
  else if (d->is_shape)
    {
      O42ShapeKind kind = O42_SHAPE_RECT;
      O42Shape *sh;

      if (strcmp (d->geom, "ellipse") == 0)
        kind = O42_SHAPE_OVAL;
      else if (strcmp (d->geom, "line") == 0 || g_str_has_prefix (d->geom, "straightConnector"))
        kind = d->arrow ? O42_SHAPE_ARROW : O42_SHAPE_LINE;
      else if (d->text_box || d->body->len > 0)
        kind = O42_SHAPE_TEXT;   /* Excel says txBox; others just write in it */

      sh = o42_sheet_add_shape (d->sheet, kind, row, col);
      if (sh != NULL)
        {
          sh->dx = dx;
          sh->dy = dy;
          sh->width = width;
          sh->height = height;
          sh->fill = d->fill;
          sh->line = d->line;
          sh->line_width = d->line_width;
          if (d->body->len > 0)
            {
              g_free (sh->text);
              sh->text = g_strdup (d->body->str);
            }
        }
    }
  else if (d->chart != NULL)
    {
      const char *target = g_hash_table_lookup (d->rels, d->chart);
      char *part = target ? resolve (d->dir, target) : NULL;
      if (part != NULL)
        add_chart_from_part (d->parts, part, d->sheet, row, col, dx, dy, width, height);
      g_free (part);
    }
}

static void
draw_end (GMarkupParseContext *ctx, const char *name, gpointer user, GError **error)
{
  DrawReader *d = user;
  const char *n = local (name);
  (void) ctx; (void) error;

  if (strcmp (n, "oneCellAnchor") == 0 || strcmp (n, "twoCellAnchor") == 0 || strcmp (n, "absoluteAnchor") == 0)
    {
      if (d->in_anchor)
        finish_anchor (d);
      d->in_anchor = FALSE;
    }
  else if (strcmp (n, "from") == 0) d->in_from = FALSE;
  else if (strcmp (n, "to") == 0) d->in_to = FALSE;
  else if (strcmp (n, "ln") == 0) d->in_line = FALSE;
  else if (strcmp (n, "txBody") == 0) d->in_body = FALSE;
  else if (strcmp (n, "t") == 0 && d->field != NULL)
    {
      g_string_append (d->body, d->text->str);
      d->field = NULL;
    }
  else if (d->field != NULL &&
           (strcmp (n, "col") == 0 || strcmp (n, "row") == 0 || strcmp (n, "colOff") == 0 || strcmp (n, "rowOff") == 0))
    {
      double v = g_ascii_strtod (d->text->str, NULL);
      if (d->in_from)
        {
          if (strcmp (n, "col") == 0) d->from_col = (int) v;
          else if (strcmp (n, "row") == 0) d->from_row = (int) v;
          else if (strcmp (n, "colOff") == 0) d->from_coff = v / EMU_PER_PX;
          else d->from_roff = v / EMU_PER_PX;
        }
      else if (d->in_to)
        {
          if (strcmp (n, "col") == 0) d->to_col = (int) v;
          else if (strcmp (n, "row") == 0) d->to_row = (int) v;
          else if (strcmp (n, "colOff") == 0) d->to_coff = v / EMU_PER_PX;
          else d->to_roff = v / EMU_PER_PX;
        }
      d->field = NULL;
    }
}

static void
draw_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer user, GError **error)
{
  DrawReader *d = user;
  (void) ctx; (void) error;
  if (d->field != NULL)
    g_string_append_len (d->text, text, (gssize) len);
}

void
o42_xlsx_draw_read (GHashTable *parts, const char *sheet_part, const char *rid, O42Sheet *sheet)
{
  static const GMarkupParser parser = { draw_start, draw_end, draw_text, NULL, NULL };
  GHashTable *sheet_rels = o42_xlsx_read_rels (parts, sheet_part);
  const char *target = g_hash_table_lookup (sheet_rels, rid);
  char *sheet_dir, *drawing_part;
  DrawReader d;

  if (target == NULL)
    {
      g_hash_table_unref (sheet_rels);
      return;
    }
  sheet_dir = part_dir (sheet_part);
  drawing_part = resolve (sheet_dir, target);

  memset (&d, 0, sizeof d);
  d.parts = parts;
  d.sheet = sheet;
  d.rels = o42_xlsx_read_rels (parts, drawing_part);
  d.dir = part_dir (drawing_part);
  d.text = g_string_new (NULL);
  d.body = g_string_new (NULL);
  parse_part (parts, drawing_part, &parser, &d);

  g_string_free (d.text, TRUE);
  g_string_free (d.body, TRUE);
  g_free (d.blip);
  g_free (d.chart);
  g_free (d.dir);
  g_hash_table_unref (d.rels);
  g_free (drawing_part);
  g_free (sheet_dir);
  g_hash_table_unref (sheet_rels);
}
