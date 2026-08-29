/* o42-pdf.c - see o42-pdf.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-pdf.h"
#include "o42-pattern.h"
#include "o42-richtext.h"

#include <cairo-pdf.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#ifdef HAVE_POPPLER
#include <poppler.h>
#endif

/* ====================================================================== */
/* Export                                                                  */
/* ====================================================================== */

/* Landscape US Letter with half-inch margins, in points. */
#define PAGE_W   792.0
#define PAGE_H   612.0
#define MARGIN    36.0
#define PX_TO_PT   0.75      /* the grid lays out at 96 dpi; PDF is 72 */

typedef struct {
  int    first;      /* first column (or row) on this band */
  int    last;
  double offset;     /* pixel position of `first` in the sheet */
} Band;

/* Cuts a run of columns (or rows) into bands that each fit the page,
 * starting a new one at every manual page break. */
static GArray *
make_bands (O42Sheet *sheet, int from, int to, gboolean columns, double limit_px)
{
  GArray *bands = g_array_new (FALSE, FALSE, sizeof (Band));
  Band band = { from, from, 0.0 };
  double used = 0.0;
  double pos = 0.0;

  for (int i = from; i <= to; i++)
    {
      double size = columns ? o42_sheet_col_width (sheet, i)
                            : o42_sheet_row_height (sheet, i);

      if (used > 0.0 && (used + size > limit_px || o42_sheet_page_break (sheet, !columns, i)))
        {
          band.last = i - 1;
          g_array_append_val (bands, band);
          band.first = i;
          band.offset = pos;
          used = 0.0;
        }

      used += size;
      pos += size;
    }

  band.last = to;
  g_array_append_val (bands, band);

  return bands;
}

struct _O42Pages {
  O42Sheet *sheet;
  GArray   *col_bands;
  GArray   *row_bands;
  char     *document;     /* for &F */
  double    header_h;     /* pixels reserved above the cells, 0 for none */
  double    footer_h;
  double    headings_w;   /* row numbers down the left, 0 for none */
  double    headings_h;   /* column letters across the top */
  double    titles_h;     /* the repeated rows */
  int       title_rows;
  double    page_w_px, page_h_px;
  double    scale;        /* 1.0 for life size */
};

#define HF_H        22.0     /* a header or footer line, in pixels */
#define HEADING_W   40.0
#define HEADING_H   20.0

static cairo_status_t
write_to_stream (void *closure, const unsigned char *data, unsigned int length)
{
  gsize written = 0;

  if (!g_output_stream_write_all (closure, data, length, &written, NULL, NULL))
    return CAIRO_STATUS_WRITE_ERROR;

  return CAIRO_STATUS_SUCCESS;
}

static void
set_rgb (cairo_t *cr, guint32 colour)
{
  cairo_set_source_rgb (cr,
                        ((colour >> 16) & 0xff) / 255.0,
                        ((colour >> 8) & 0xff) / 255.0,
                        (colour & 0xff) / 255.0);
}

/* One page: the cells of a column band by a row band, in sheet pixels with
 * the band's corner at the origin. */
static void
draw_page (cairo_t      *cr,
           O42Sheet     *sheet,
           const Band   *cols,
           const Band   *rows,
           PangoLayout  *layout,
           gboolean      gridlines)
{
  double x, y;
  double band_w = 0.0, band_h = 0.0;

  for (int c = cols->first; c <= cols->last; c++)
    band_w += o42_sheet_col_width (sheet, c);
  for (int r = rows->first; r <= rows->last; r++)
    band_h += o42_sheet_row_height (sheet, r);

  /* Fills. */
  y = 0.0;
  for (int r = rows->first; r <= rows->last; r++)
    {
      double h = o42_sheet_row_height (sheet, r);

      x = 0.0;
      for (int c = cols->first; c <= cols->last; c++)
        {
          double w = o42_sheet_col_width (sheet, c);
          const O42Fmt *fmt = o42_sheet_get_fmt (sheet, r, c);
          O42Fmt conditional;

          if (o42_sheet_conditional_fmt (sheet, r, c, &conditional))
            fmt = &conditional;

          o42_pattern_fill (fmt, cr, x, y, w, h);
          x += w;
        }
      y += h;
    }

  /* Gridlines, light, the way Excel prints them when asked to. */
  if (gridlines)
    {
      cairo_set_source_rgb (cr, 0.75, 0.75, 0.75);
      cairo_set_line_width (cr, 0.5);
      x = 0.0;
      for (int c = cols->first; c <= cols->last + 1; c++)
        {
          cairo_move_to (cr, x, 0);
          cairo_line_to (cr, x, band_h);
          if (c <= cols->last)
            x += o42_sheet_col_width (sheet, c);
        }
      y = 0.0;
      for (int r = rows->first; r <= rows->last + 1; r++)
        {
          cairo_move_to (cr, 0, y);
          cairo_line_to (cr, band_w, y);
          if (r <= rows->last)
            y += o42_sheet_row_height (sheet, r);
        }
      cairo_stroke (cr);
    }

  /* Text, with each cell's own font, alignment and number format. */
  y = 0.0;
  for (int r = rows->first; r <= rows->last; r++)
    {
      double h = o42_sheet_row_height (sheet, r);

      x = 0.0;
      for (int c = cols->first; c <= cols->last; c++)
        {
          double w = o42_sheet_col_width (sheet, c);
          O42Value value;
          const O42Fmt *fmt;
          char *text;
          PangoFontDescription *desc;
          int tw, th;
          double tx, ty;

          o42_sheet_get_value (sheet, r, c, &value);
          if (value.type == O42_VALUE_EMPTY)
            {
              o42_value_clear (&value);
              x += w;
              continue;
            }

          /* A merged range prints as one cell, from its top-left corner;
           * the cells inside it print nothing. */
          {
            O42Range merged;

            if (o42_sheet_merged_at (sheet, r, c, &merged))
              {
                if (r != merged.row0 || c != merged.col0)
                  {
                    o42_value_clear (&value);
                    x += w;
                    continue;
                  }
                w = 0;
                for (int cc = merged.col0; cc <= merged.col1; cc++)
                  w += o42_sheet_col_width (sheet, cc);
                h = 0;
                for (int rr = merged.row0; rr <= merged.row1; rr++)
                  h += o42_sheet_row_height (sheet, rr);
              }
          }

          fmt = o42_sheet_get_fmt (sheet, r, c);
          {
            static O42Fmt conditional;
            if (o42_sheet_conditional_fmt (sheet, r, c, &conditional))
              fmt = &conditional;
          }
          text = o42_fmt_display (fmt, &value);

          desc = pango_font_description_new ();
          pango_font_description_set_family (desc, fmt->family ? fmt->family : "Sans");
          pango_font_description_set_size (desc, (fmt->size / 2) * PANGO_SCALE);
          pango_font_description_set_weight (desc, fmt->bold ? PANGO_WEIGHT_BOLD
                                                             : PANGO_WEIGHT_NORMAL);
          pango_font_description_set_style (desc, fmt->italic ? PANGO_STYLE_ITALIC
                                                              : PANGO_STYLE_NORMAL);
          pango_layout_set_font_description (layout, desc);
          pango_font_description_free (desc);
          pango_layout_set_text (layout, text, -1);
          /* Wrapped text is laid out to its cell's width, as on screen. */
          if (fmt->wrap)
            {
              pango_layout_set_width (layout, (int) MAX (w - 6, 1) * PANGO_SCALE);
              pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
            }
          else
            pango_layout_set_width (layout, -1);
          pango_layout_get_pixel_size (layout, &tw, &th);

          /* The grid's rules for what does not fit: a General number is
           * shown with fewer digits, any other number as hashes, and a
           * label runs on over the empty cells to its right. */
          if (value.type == O42_VALUE_NUMBER && tw + 6 > w)
            {
              gboolean fits = FALSE;

              if (fmt->number == O42_NUM_GENERAL && fmt->custom == NULL)
                for (int digits = 9; digits >= 1 && !fits; digits--)
                  {
                    char spec[8], buffer[G_ASCII_DTOSTR_BUF_SIZE];

                    g_snprintf (spec, sizeof spec, "%%.%dg", digits);
                    g_ascii_formatd (buffer, sizeof buffer, spec, value.as.number);
                    pango_layout_set_text (layout, buffer, -1);
                    pango_layout_get_pixel_size (layout, &tw, &th);
                    if (tw + 6 <= w)
                      {
                        g_free (text);
                        text = g_strdup (buffer);
                        fits = TRUE;
                      }
                  }

              if (!fits)
                {
                  GString *hashes = g_string_new ("#");

                  pango_layout_set_text (layout, "#", -1);
                  pango_layout_get_pixel_size (layout, &tw, &th);
                  while (tw > 0 && (hashes->len + 1) * tw + 6 <= w)
                    g_string_append_c (hashes, '#');
                  g_free (text);
                  text = g_string_free (hashes, FALSE);
                  pango_layout_set_text (layout, text, -1);
                  pango_layout_get_pixel_size (layout, &tw, &th);
                }
            }

          {
            double clip_w = w;
            int n_runs = 0;
            const O42TextRun *runs = (value.type == O42_VALUE_TEXT)
                                     ? o42_sheet_runs (sheet, r, c, &n_runs) : NULL;
            PangoAttrList *attrs = o42_runs_attributes (runs, n_runs, fmt, text);

            if (attrs != NULL)
              {
                pango_layout_set_attributes (layout, attrs);
                pango_attr_list_unref (attrs);
                pango_layout_get_pixel_size (layout, &tw, &th);
              }

            if (value.type == O42_VALUE_TEXT && tw + 6 > w &&
                o42_fmt_effective_halign (fmt, &value) == O42_HALIGN_LEFT)
              for (int cc = c + 1; cc <= cols->last; cc++)
                {
                  if (!o42_sheet_is_empty (sheet, r, cc))
                    break;
                  clip_w += o42_sheet_col_width (sheet, cc);
                }

            switch (o42_fmt_effective_halign (fmt, &value))
              {
              case O42_HALIGN_RIGHT:  tx = x + w - 3 - tw;      break;
              case O42_HALIGN_CENTRE: tx = x + (w - tw) / 2.0;  break;
              default:                tx = x + 3;               break;
              }
            ty = y + h - th - 1;

            cairo_save (cr);
            cairo_rectangle (cr, x, y, clip_w, h);
            cairo_clip (cr);
          }
          {
            guint32 colour = fmt->colour;
            o42_fmt_display_colour (fmt, &value, &colour);
            set_rgb (cr, colour);
          }
          cairo_move_to (cr, tx, ty);
          pango_cairo_show_layout (cr, layout);
          cairo_restore (cr);
            pango_layout_set_attributes (layout, NULL);

          g_free (text);
          o42_value_clear (&value);
          x += o42_sheet_col_width (sheet, c);
        }
      y += o42_sheet_row_height (sheet, r);
    }

  /* Borders. */
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_set_line_width (cr, 1.0);
  y = 0.0;
  for (int r = rows->first; r <= rows->last; r++)
    {
      double h = o42_sheet_row_height (sheet, r);

      x = 0.0;
      for (int c = cols->first; c <= cols->last; c++)
        {
          double w = o42_sheet_col_width (sheet, c);
          const O42Fmt *fmt = o42_sheet_get_fmt (sheet, r, c);

          if (fmt->border_top)    o42_draw_border_line (cr, fmt->border_style[O42_SIDE_TOP], fmt->border_colour[O42_SIDE_TOP], x, y, x + w, y);
          if (fmt->border_bottom) o42_draw_border_line (cr, fmt->border_style[O42_SIDE_BOTTOM], fmt->border_colour[O42_SIDE_BOTTOM], x, y + h, x + w, y + h);
          if (fmt->border_left)   o42_draw_border_line (cr, fmt->border_style[O42_SIDE_LEFT], fmt->border_colour[O42_SIDE_LEFT], x, y, x, y + h);
          if (fmt->border_right)  o42_draw_border_line (cr, fmt->border_style[O42_SIDE_RIGHT], fmt->border_colour[O42_SIDE_RIGHT], x + w, y, x + w, y + h);
          x += w;
        }
      y += h;
    }
  cairo_stroke (cr);

  /* Pictures, clipped to the band they overlap. */
  {
    GPtrArray *pictures = o42_sheet_pictures (sheet);

    for (guint i = 0; i < pictures->len; i++)
      {
        O42Picture *pic = g_ptr_array_index (pictures, i);
        cairo_surface_t *surface;
        double px = 0.0, py = 0.0;

        /* Its position relative to the band's corner. */
        for (int c = 0; c < pic->col; c++)
          px += o42_sheet_col_width (sheet, c);
        for (int r = 0; r < pic->row; r++)
          py += o42_sheet_row_height (sheet, r);
        px += pic->dx - cols->offset;
        py += pic->dy - rows->offset;

        if (px > band_w || py > band_h ||
            px + pic->width < 0 || py + pic->height < 0)
          continue;

        surface = o42_picture_surface (pic);
        if (surface == NULL)
          continue;

        cairo_save (cr);
        cairo_rectangle (cr, 0, 0, band_w, band_h);
        cairo_clip (cr);
        cairo_translate (cr, px, py);
        cairo_scale (cr, pic->width / cairo_image_surface_get_width (surface),
                         pic->height / cairo_image_surface_get_height (surface));
        cairo_set_source_surface (cr, surface, 0, 0);
        cairo_paint (cr);
        cairo_restore (cr);
      }
  }

  /* Shapes, likewise. */
  {
    GPtrArray *shapes = o42_sheet_shapes (sheet);

    for (guint i = 0; i < shapes->len; i++)
      {
        O42Shape *shape = g_ptr_array_index (shapes, i);
        double px = 0.0, py = 0.0;

        for (int c = 0; c < shape->col; c++)
          px += o42_sheet_col_width (sheet, c);
        for (int r = 0; r < shape->row; r++)
          py += o42_sheet_row_height (sheet, r);
        px += shape->dx - cols->offset;
        py += shape->dy - rows->offset;

        if (px > band_w || py > band_h ||
            px + MAX (shape->width, 1) < 0 || py + MAX (shape->height, 1) < 0)
          continue;

        cairo_save (cr);
        cairo_rectangle (cr, 0, 0, band_w, band_h);
        cairo_clip (cr);
        cairo_translate (cr, px, py);
        o42_sheet_draw_shape (sheet, shape, cr, shape->width, shape->height);
        cairo_restore (cr);
      }
  }

  /* Charts, likewise. */

  {
    GPtrArray *charts = o42_sheet_charts (sheet);

    for (guint i = 0; i < charts->len; i++)
      {
        O42Chart *chart = g_ptr_array_index (charts, i);
        double px = 0.0, py = 0.0;

        for (int c = 0; c < chart->col; c++)
          px += o42_sheet_col_width (sheet, c);
        for (int r = 0; r < chart->row; r++)
          py += o42_sheet_row_height (sheet, r);
        px += chart->dx - cols->offset;
        py += chart->dy - rows->offset;

        if (px > band_w || py > band_h ||
            px + chart->width < 0 || py + chart->height < 0)
          continue;

        cairo_save (cr);
        cairo_rectangle (cr, 0, 0, band_w, band_h);
        cairo_clip (cr);
        cairo_translate (cr, px, py);
        o42_sheet_draw_chart (sheet, chart, cr, chart->width, chart->height);
        cairo_restore (cr);
      }
  }
}

O42Pages *
o42_pages_new (O42Sheet *sheet, double width_pt, double height_pt)
{
  O42Pages *pages = g_new0 (O42Pages, 1);
  O42Range used;

  g_return_val_if_fail (sheet != NULL, NULL);

  pages->sheet = sheet;
  pages->page_w_px = width_pt / PX_TO_PT;
  pages->page_h_px = height_pt / PX_TO_PT;
  pages->scale = 1;
  if (o42_sheet_is_chart_sheet (sheet))
    {
      /* One page, the chart across the whole of it. */
      Band whole = { 0, 0, 0 };

      pages->col_bands = g_array_new (FALSE, FALSE, sizeof (Band));
      pages->row_bands = g_array_new (FALSE, FALSE, sizeof (Band));
      g_array_append_val (pages->col_bands, whole);
      g_array_append_val (pages->row_bands, whole);
      return pages;
    }
  o42_sheet_used_range (sheet, &used);
  {
    const O42PrintSetup *setup = o42_sheet_print_setup (sheet);
    if (setup->has_area)
      used = setup->area;
    if (setup->header != NULL && *setup->header != '\0') pages->header_h = HF_H;
    if (setup->footer != NULL && *setup->footer != '\0') pages->footer_h = HF_H;
    if (setup->headings)
      {
        pages->headings_w = HEADING_W;
        pages->headings_h = HEADING_H;
      }
    pages->title_rows = setup->title_rows;
    for (int r = 0; r < setup->title_rows; r++)
      pages->titles_h += o42_sheet_row_height (sheet, r);
  }

  /* Pictures extend the printed area past the last cell. */
  {
    GPtrArray *pictures = o42_sheet_pictures (sheet);

    for (guint i = 0; i < pictures->len; i++)
      {
        O42Picture *pic = g_ptr_array_index (pictures, i);
        int col = pic->col, row = pic->row;
        double reach = pic->dx + pic->width, down = pic->dy + pic->height;

        while (col < O42_MAX_COLS - 1 && reach > o42_sheet_col_width (sheet, col))
          { reach -= o42_sheet_col_width (sheet, col); col++; }
        while (row < O42_MAX_ROWS - 1 && down > o42_sheet_row_height (sheet, row))
          { down -= o42_sheet_row_height (sheet, row); row++; }

        used.col1 = MAX (used.col1, col);
        used.row1 = MAX (used.row1, row);
      }
  }

  {
    GPtrArray *charts = o42_sheet_charts (sheet);

    for (guint i = 0; i < charts->len; i++)
      {
        O42Chart *chart = g_ptr_array_index (charts, i);
        int col = chart->col, row = chart->row;
        double reach = chart->dx + chart->width, down = chart->dy + chart->height;

        while (col < O42_MAX_COLS - 1 && reach > o42_sheet_col_width (sheet, col))
          { reach -= o42_sheet_col_width (sheet, col); col++; }
        while (row < O42_MAX_ROWS - 1 && down > o42_sheet_row_height (sheet, row))
          { down -= o42_sheet_row_height (sheet, row); row++; }

        used.col1 = MAX (used.col1, col);
        used.row1 = MAX (used.row1, row);
      }
  }

  {
    /* A shape reaches past the last cell as a picture does. */
    GPtrArray *shapes = o42_sheet_shapes (sheet);

    for (guint i = 0; i < shapes->len; i++)
      {
        O42Shape *shape = g_ptr_array_index (shapes, i);
        int col = shape->col, row = shape->row;
        double reach = shape->dx + MAX (shape->width, 1), down = shape->dy + MAX (shape->height, 1);

        while (col < O42_MAX_COLS - 1 && reach > o42_sheet_col_width (sheet, col))
          { reach -= o42_sheet_col_width (sheet, col); col++; }
        while (row < O42_MAX_ROWS - 1 && down > o42_sheet_row_height (sheet, row))
          { down -= o42_sheet_row_height (sheet, row); row++; }

        used.col1 = MAX (used.col1, col);
        used.row1 = MAX (used.row1, row);
      }
  }

  {
    /* The scale the setup asks for, or the one that fits the sheet
     * into the pages it allows. */
    const O42PrintSetup *setup = o42_sheet_print_setup (sheet);
    double across = width_pt / PX_TO_PT - pages->headings_w;
    double down = height_pt / PX_TO_PT - pages->header_h - pages->footer_h
                  - pages->headings_h - pages->titles_h;

    pages->scale = CLAMP (setup->scale, 10, 400) / 100.0;
    if (setup->fit_wide > 0 || setup->fit_tall > 0)
      {
        double total_w = 0, total_h = 0;
        double fit = 1.0;

        for (int c = used.col0; c <= used.col1; c++)
          total_w += o42_sheet_col_width (sheet, c);
        for (int r = used.row0; r <= used.row1; r++)
          total_h += o42_sheet_row_height (sheet, r);
        if (setup->fit_wide > 0 && total_w > 0)
          fit = MIN (fit, across * setup->fit_wide / total_w);
        if (setup->fit_tall > 0 && total_h > 0)
          fit = MIN (fit, down * setup->fit_tall / total_h);
        pages->scale = CLAMP (fit, 0.1, 1.0);
      }
    across /= pages->scale;
    down /= pages->scale;
    pages->col_bands = make_bands (sheet, used.col0, used.col1, TRUE, across);
    pages->row_bands = make_bands (sheet, used.row0, used.row1, FALSE, down);
    pages->page_h_px = height_pt / PX_TO_PT / pages->scale;
    pages->page_w_px = width_pt / PX_TO_PT / pages->scale;
  }

  return pages;
}

void
o42_pages_set_document (O42Pages *pages, const char *name)
{
  g_return_if_fail (pages != NULL);
  g_free (pages->document);
  pages->document = g_strdup (name);
}

/* A header or footer part with its codes filled in. */
static char *
expand_codes (O42Pages *pages, const char *text, int page)
{
  GString *out = g_string_new (NULL);
  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p != '&' || p[1] == '\0')
        { g_string_append_c (out, *p); continue; }
      p++;
      switch (g_ascii_toupper (*p))
        {
        case 'P': g_string_append_printf (out, "%d", page + 1); break;
        case 'N': g_string_append_printf (out, "%d", o42_pages_count (pages)); break;
        case 'A': g_string_append (out, o42_sheet_get_name (pages->sheet)); break;
        case 'F': g_string_append (out, pages->document != NULL ? pages->document : "Book1"); break;
        case 'D':
          {
            GDateTime *now = g_date_time_new_now_local ();
            char *s = g_date_time_format (now, "%Y-%m-%d");
            g_string_append (out, s);
            g_free (s);
            g_date_time_unref (now);
            break;
          }
        case 'T':
          {
            GDateTime *now = g_date_time_new_now_local ();
            char *s = g_date_time_format (now, "%H:%M");
            g_string_append (out, s);
            g_free (s);
            g_date_time_unref (now);
            break;
          }
        case '&': g_string_append_c (out, '&'); break;
        case '"':
          /* &"Times New Roman,Normal" names a face and a style: office42
           * sets headers in one face, so the name is stepped over rather
           * than printed, which is what it did before. */
          {
            const char *close = strchr (p + 1, '"');

            p = (close != NULL) ? close : p + strlen (p) - 1;
            break;
          }
        default:
          /* &12 is a size in points, and &B &I &U &S &X &Y are styles:
           * all of them are stepped over.  Anything else is dropped, as
           * &L &C &R already have been. */
          if (g_ascii_isdigit (*p))
            while (p[1] != 0 && g_ascii_isdigit (p[1]))
              p++;
          break;
        }
    }
  return g_string_free (out, FALSE);
}

/* Splits "&Lleft&Ccentre&Rright" into its parts; text before any code
 * is the centre, as Excel takes it. */
static void
split_parts (const char *text, char **left, char **centre, char **right)
{
  GString *parts[3] = { g_string_new (NULL), g_string_new (NULL), g_string_new (NULL) };
  int which = 1;
  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p == '&' && (p[1] == 'L' || p[1] == 'C' || p[1] == 'R' || p[1] == 'l' || p[1] == 'c' || p[1] == 'r'))
        {
          which = g_ascii_toupper (p[1]) == 'L' ? 0 : g_ascii_toupper (p[1]) == 'C' ? 1 : 2;
          p++;
          continue;
        }
      if (*p == '&' && p[1] == '&')
        { g_string_append (parts[which], "&&"); p++; continue; }
      g_string_append_c (parts[which], *p);
    }
  *left = g_string_free (parts[0], FALSE);
  *centre = g_string_free (parts[1], FALSE);
  *right = g_string_free (parts[2], FALSE);
}

static void
draw_header_footer (O42Pages *pages, cairo_t *cr, PangoLayout *layout, const char *text,
                    double y, double width, int page)
{
  char *l, *c, *r;
  PangoFontDescription *desc = pango_font_description_from_string ("Arial 9");
  char *parts[3];

  split_parts (text, &l, &c, &r);
  parts[0] = expand_codes (pages, l, page);
  parts[1] = expand_codes (pages, c, page);
  parts[2] = expand_codes (pages, r, page);
  pango_layout_set_font_description (layout, desc);
  cairo_set_source_rgb (cr, 0, 0, 0);
  for (int i = 0; i < 3; i++)
    {
      int tw, th;
      if (*parts[i] != '\0')
        {
          pango_layout_set_text (layout, parts[i], -1);
          pango_layout_get_pixel_size (layout, &tw, &th);
          cairo_move_to (cr, i == 0 ? 0 : i == 1 ? (width - tw) / 2 : width - tw, y + (HF_H - th) / 2);
          pango_cairo_show_layout (cr, layout);
        }
      g_free (parts[i]);
    }
  pango_font_description_free (desc);
  g_free (l); g_free (c); g_free (r);
}

static void
draw_headings (O42Pages *pages, cairo_t *cr, PangoLayout *layout, const Band *cols, const Band *rows,
               double x0, double y0, gboolean with_titles)
{
  PangoFontDescription *desc = pango_font_description_from_string ("Arial 8");
  double x, y;
  int tw, th;

  pango_layout_set_font_description (layout, desc);
  cairo_set_line_width (cr, 0.5);

  /* Column letters across the top. */
  x = x0;
  for (int c = cols->first; c <= cols->last; c++)
    {
      double w = o42_sheet_col_width (pages->sheet, c);
      char name[8];
      o42_col_name (c, name, sizeof name);
      cairo_set_source_rgb (cr, 0.92, 0.92, 0.92);
      cairo_rectangle (cr, x, y0 - HEADING_H, w, HEADING_H);
      cairo_fill_preserve (cr);
      cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
      cairo_stroke (cr);
      pango_layout_set_text (layout, name, -1);
      pango_layout_get_pixel_size (layout, &tw, &th);
      cairo_set_source_rgb (cr, 0, 0, 0);
      cairo_move_to (cr, x + (w - tw) / 2, y0 - HEADING_H + (HEADING_H - th) / 2);
      pango_cairo_show_layout (cr, layout);
      x += w;
    }

  /* Row numbers down the left: the title rows first, then the band. */
  y = y0;
  for (int pass = 0; pass < 2; pass++)
    {
      int first = pass == 0 ? 0 : rows->first;
      int last = pass == 0 ? pages->title_rows - 1 : rows->last;
      if (pass == 0 && !with_titles)
        continue;
      for (int r = first; r <= last; r++)
        {
          double h = o42_sheet_row_height (pages->sheet, r);
          char num[16];
          g_snprintf (num, sizeof num, "%d", r + 1);
          cairo_set_source_rgb (cr, 0.92, 0.92, 0.92);
          cairo_rectangle (cr, x0 - HEADING_W, y, HEADING_W, h);
          cairo_fill_preserve (cr);
          cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
          cairo_stroke (cr);
          pango_layout_set_text (layout, num, -1);
          pango_layout_get_pixel_size (layout, &tw, &th);
          cairo_set_source_rgb (cr, 0, 0, 0);
          cairo_move_to (cr, x0 - HEADING_W + (HEADING_W - tw) / 2, y + (h - th) / 2);
          pango_cairo_show_layout (cr, layout);
          y += h;
        }
    }
  pango_font_description_free (desc);
}

int
o42_pages_count (O42Pages *pages)
{
  g_return_val_if_fail (pages != NULL, 0);
  return (int) (pages->col_bands->len * pages->row_bands->len);
}

/* Across, then down: every column band of the first row band, then the
 * next row band, which is Excel's default and reads like a book. */
void
o42_pages_draw (O42Pages *pages, int page, cairo_t *cr)
{
  PangoLayout *layout;
  guint cb, rb;

  g_return_if_fail (pages != NULL);
  g_return_if_fail (cr != NULL);

  if (page < 0 || page >= o42_pages_count (pages))
    return;

  if (o42_sheet_is_chart_sheet (pages->sheet))
    {
      O42Chart *chart = o42_sheet_the_chart (pages->sheet);

      if (chart != NULL)
        {
          cairo_save (cr);
          cairo_scale (cr, PX_TO_PT, PX_TO_PT);
          o42_sheet_draw_chart (pages->sheet, chart, cr,
                                pages->page_w_px, pages->page_h_px);
          cairo_restore (cr);
        }
      return;
    }

  cb = (guint) page % pages->col_bands->len;
  rb = (guint) page / pages->col_bands->len;

  layout = pango_cairo_create_layout (cr);

  cairo_save (cr);
  cairo_scale (cr, PX_TO_PT * pages->scale, PX_TO_PT * pages->scale);
  pango_cairo_update_layout (cr, layout);
  {
    const O42PrintSetup *setup = o42_sheet_print_setup (pages->sheet);
    const Band *cols = &g_array_index (pages->col_bands, Band, cb);
    const Band *rows = &g_array_index (pages->row_bands, Band, rb);
    gboolean with_titles = pages->title_rows > 0 && rows->first > pages->title_rows - 1;
    double x0 = pages->headings_w;
    double y0 = pages->header_h + pages->headings_h;

    if (pages->header_h > 0)
      draw_header_footer (pages, cr, layout, setup->header, 0, pages->page_w_px, page);
    if (pages->footer_h > 0)
      draw_header_footer (pages, cr, layout, setup->footer, pages->page_h_px - HF_H, pages->page_w_px, page);
    if (pages->headings_w > 0)
      draw_headings (pages, cr, layout, cols, rows, x0, y0, with_titles);

    if (with_titles)
      {
        Band titles = { 0, pages->title_rows - 1, 0.0 };
        cairo_save (cr);
        cairo_translate (cr, x0, y0);
        draw_page (cr, pages->sheet, cols, &titles, layout, setup->gridlines);
        cairo_restore (cr);
        y0 += pages->titles_h;
      }
    cairo_save (cr);
    cairo_translate (cr, x0, y0);
    draw_page (cr, pages->sheet, cols, rows, layout, setup->gridlines);
    cairo_restore (cr);
  }
  cairo_restore (cr);

  g_object_unref (layout);
}

static int
pages_breaks (GArray *bands, int **out)
{
  int n = bands != NULL ? (int) bands->len - 1 : 0;

  *out = NULL;
  if (n <= 0)
    return 0;
  *out = g_new (int, n);
  for (int i = 0; i < n; i++)
    (*out)[i] = g_array_index (bands, Band, i + 1).first;
  return n;
}

int
o42_pages_row_breaks (O42Pages *pages, int **rows)
{
  g_return_val_if_fail (pages != NULL && rows != NULL, 0);
  return pages_breaks (pages->row_bands, rows);
}

int
o42_pages_col_breaks (O42Pages *pages, int **cols)
{
  g_return_val_if_fail (pages != NULL && cols != NULL, 0);
  return pages_breaks (pages->col_bands, cols);
}

void
o42_pages_free (O42Pages *pages)
{
  if (pages == NULL)
    return;

  g_array_free (pages->col_bands, TRUE);
  g_array_free (pages->row_bands, TRUE);
  g_free (pages->document);
  g_free (pages);
}

/* The pages of one sheet onto a surface already open: what both the
 * one-sheet export and the whole-book one draw. */
static gboolean
export_sheet_pages (O42Sheet *sheet, cairo_t *cr, const char *document)
{
  double margin = o42_sheet_print_setup (sheet)->margin;
  O42Pages *pages = o42_pages_new (sheet, PAGE_W - 2 * margin, PAGE_H - 2 * margin);

  if (document != NULL)
    o42_pages_set_document (pages, document);
  for (int page = 0; page < o42_pages_count (pages); page++)
    {
      cairo_save (cr);
      cairo_translate (cr, margin, margin);
      o42_pages_draw (pages, page, cr);
      cairo_restore (cr);
      cairo_show_page (cr);
    }
  o42_pages_free (pages);
  return TRUE;
}

gboolean
o42_pdf_export_book (O42Book *book, GFile *file, GError **error)
{
  GFileOutputStream *stream;
  cairo_surface_t *surface;
  cairo_t *cr;
  char *base;
  gboolean ok = TRUE;

  g_return_val_if_fail (book != NULL && G_IS_FILE (file), FALSE);

  stream = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
  if (stream == NULL)
    return FALSE;

  base = g_file_get_basename (file);
  surface = cairo_pdf_surface_create_for_stream (write_to_stream, stream, PAGE_W, PAGE_H);
  cr = cairo_create (surface);

  for (int i = 0; i < o42_book_n_sheets (book); i++)
    export_sheet_pages (o42_book_sheet (book, i), cr, base);

  if (cairo_status (cr) != CAIRO_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "office42 could not write the PDF: %s",
                   cairo_status_to_string (cairo_status (cr)));
      ok = FALSE;
    }

  cairo_destroy (cr);
  cairo_surface_finish (surface);
  cairo_surface_destroy (surface);
  g_free (base);

  if (!g_output_stream_close (G_OUTPUT_STREAM (stream), NULL, ok ? error : NULL))
    ok = FALSE;
  g_object_unref (stream);
  return ok;
}

gboolean
o42_pdf_export (O42Sheet *sheet, GFile *file, GError **error)
{
  GFileOutputStream *stream;
  cairo_surface_t *surface;
  cairo_t *cr;
  O42Pages *pages;
  gboolean ok = TRUE;
  double margin;

  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  stream = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
  if (stream == NULL)
    return FALSE;

  margin = o42_sheet_print_setup (sheet)->margin;
  pages = o42_pages_new (sheet, PAGE_W - 2 * margin, PAGE_H - 2 * margin);
  {
    char *base = g_file_get_basename (file);
    o42_pages_set_document (pages, base);
    g_free (base);
  }

  surface = cairo_pdf_surface_create_for_stream (write_to_stream, stream,
                                                 PAGE_W, PAGE_H);
  cr = cairo_create (surface);

  for (int page = 0; page < o42_pages_count (pages); page++)
    {
      cairo_save (cr);
      cairo_translate (cr, margin, margin);
      o42_pages_draw (pages, page, cr);
      cairo_restore (cr);
      cairo_show_page (cr);
    }

  if (cairo_status (cr) != CAIRO_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "office42 could not write the PDF: %s",
                   cairo_status_to_string (cairo_status (cr)));
      ok = FALSE;
    }

  cairo_destroy (cr);
  cairo_surface_finish (surface);
  cairo_surface_destroy (surface);
  o42_pages_free (pages);

  if (!g_output_stream_close (G_OUTPUT_STREAM (stream), NULL, ok ? error : NULL))
    ok = FALSE;

  g_object_unref (stream);
  return ok;
}

/* ====================================================================== */
/* Import                                                                  */
/* ====================================================================== */

gboolean
o42_pdf_import_available (void)
{
#ifdef HAVE_POPPLER
  return TRUE;
#else
  return FALSE;
#endif
}

#ifdef HAVE_POPPLER

typedef struct {
  gunichar c;
  double   x1, y1, x2, y2;
} Glyph;

typedef struct {
  double   x;        /* where the cell starts on the page, in points */
  GString *text;
} Cell;

typedef struct {
  double  y;         /* the line's centre */
  GArray *glyphs;    /* Glyph */
} Line;

static int
compare_glyph_x (gconstpointer a, gconstpointer b)
{
  const Glyph *ga = a, *gb = b;
  return (ga->x1 < gb->x1) ? -1 : (ga->x1 > gb->x1) ? 1 : 0;
}

static int
compare_line_y (gconstpointer a, gconstpointer b)
{
  const Line *la = a, *lb = b;
  return (la->y < lb->y) ? -1 : (la->y > lb->y) ? 1 : 0;
}

static int
compare_double (gconstpointer a, gconstpointer b)
{
  double x = *(const double *) a, y = *(const double *) b;
  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* Characters into lines: a character joins the line whose centre is within
 * half a character height of its own, else starts a new one. */
static GArray *
group_lines (GArray *glyphs)
{
  GArray *lines = g_array_new (FALSE, FALSE, sizeof (Line));

  for (guint i = 0; i < glyphs->len; i++)
    {
      const Glyph *g = &g_array_index (glyphs, Glyph, i);
      double cy = (g->y1 + g->y2) / 2.0;
      double h = fabs (g->y2 - g->y1);
      Line *home = NULL;

      for (guint l = 0; l < lines->len; l++)
        {
          Line *line = &g_array_index (lines, Line, l);
          if (fabs (line->y - cy) <= MAX (h, 2.0) * 0.5)
            {
              home = line;
              break;
            }
        }

      if (home == NULL)
        {
          Line line;
          line.y = cy;
          line.glyphs = g_array_new (FALSE, FALSE, sizeof (Glyph));
          g_array_append_val (lines, line);
          home = &g_array_index (lines, Line, lines->len - 1);
        }

      g_array_append_val (home->glyphs, *g);
    }

  g_array_sort (lines, compare_line_y);
  return lines;
}

/* A line into cells: a new cell starts wherever the gap from the previous
 * character is wider than a character.  Two spaces in a row count as a gap
 * too, because a table typed with spaces is still a table. */
static GArray *
split_cells (const Line *line)
{
  GArray *cells = g_array_new (FALSE, FALSE, sizeof (Cell));
  Cell cell = { 0.0, NULL };
  double prev_x2 = 0.0;
  double avg_w = 0.0;
  int pending_spaces = 0;

  g_array_sort (line->glyphs, compare_glyph_x);

  for (guint i = 0; i < line->glyphs->len; i++)
    {
      const Glyph *g = &g_array_index (line->glyphs, Glyph, i);
      double w = fabs (g->x2 - g->x1);
      gboolean space = (g->c == ' ' || g->c == 0xA0 || g->c == '\t');

      if (w > 0)
        avg_w = (avg_w > 0) ? (avg_w * 0.8 + w * 0.2) : w;

      if (space)
        {
          pending_spaces++;
          prev_x2 = g->x2;
          continue;
        }

      if (cell.text != NULL)
        {
          double gap = g->x1 - prev_x2;
          double threshold = MAX (avg_w * 1.6, 4.0);

          if (gap > threshold || pending_spaces >= 2)
            {
              g_array_append_val (cells, cell);
              cell.text = NULL;
            }
          else if (pending_spaces > 0)
            {
              g_string_append_c (cell.text, ' ');
            }
        }

      if (cell.text == NULL)
        {
          cell.x = g->x1;
          cell.text = g_string_new (NULL);
        }

      g_string_append_unichar (cell.text, g->c);
      prev_x2 = g->x2;
      pending_spaces = 0;
    }

  if (cell.text != NULL)
    g_array_append_val (cells, cell);

  return cells;
}

gboolean
o42_pdf_import (O42Sheet *sheet, GFile *file, GError **error)
{
  PopplerDocument *document;
  int n_pages;
  O42Range used;
  int next_row;

  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  document = poppler_document_new_from_gfile (file, NULL, NULL, error);
  if (document == NULL)
    return FALSE;

  n_pages = poppler_document_get_n_pages (document);

  o42_sheet_used_range (sheet, &used);
  next_row = (used.row1 > 0 || !o42_sheet_is_empty (sheet, 0, 0))
               ? used.row1 + 2 : 0;

  o42_sheet_begin_group (sheet);

  for (int p = 0; p < n_pages; p++)
    {
      PopplerPage *page = poppler_document_get_page (document, p);
      PopplerRectangle *rects = NULL;
      guint n_rects = 0;
      char *text;
      GArray *glyphs, *lines, *starts;
      GArray *line_cells;    /* GArray* of Cell, per line */

      if (page == NULL)
        continue;

      text = poppler_page_get_text (page);
      if (text == NULL || !poppler_page_get_text_layout (page, &rects, &n_rects))
        {
          g_free (text);
          g_object_unref (page);
          continue;
        }

      /* One rectangle per character of the text, in order. */
      glyphs = g_array_new (FALSE, FALSE, sizeof (Glyph));
      {
        const char *q = text;
        guint i = 0;

        while (*q != '\0' && i < n_rects)
          {
            gunichar c = g_utf8_get_char (q);

            if (c != '\n' && c != '\r')
              {
                Glyph g = { c, rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2 };
                g_array_append_val (glyphs, g);
              }

            q = g_utf8_next_char (q);
            i++;
          }
      }
      g_free (rects);
      g_free (text);

      lines = group_lines (glyphs);

      /* Cells of every line, and every cell's start, for finding columns. */
      line_cells = g_array_new (FALSE, FALSE, sizeof (GArray *));
      starts = g_array_new (FALSE, FALSE, sizeof (double));
      for (guint l = 0; l < lines->len; l++)
        {
          GArray *cells = split_cells (&g_array_index (lines, Line, l));

          g_array_append_val (line_cells, cells);
          for (guint c = 0; c < cells->len; c++)
            g_array_append_val (starts, g_array_index (cells, Cell, c).x);
        }

      /* Columns are where cells start, clustered: two starts within twelve
       * points of each other are the same column. */
      g_array_sort (starts, compare_double);
      {
        GArray *columns = g_array_new (FALSE, FALSE, sizeof (double));
        GArray *col_of;        /* per line, per cell: the column index */
        GArray *row_of;        /* per line: the sheet row it lands on */
        gboolean merged;

        for (guint i = 0; i < starts->len; i++)
          {
            double x = g_array_index (starts, double, i);

            if (columns->len == 0 ||
                x - g_array_index (columns, double, columns->len - 1) > 12.0)
              g_array_append_val (columns, x);
          }

        /* Assign every cell to the last column starting at or before it. */
        col_of = g_array_new (FALSE, FALSE, sizeof (GArray *));
        for (guint l = 0; l < lines->len; l++)
          {
            GArray *cells = g_array_index (line_cells, GArray *, l);
            GArray *cols = g_array_new (FALSE, FALSE, sizeof (int));

            for (guint c = 0; c < cells->len; c++)
              {
                const Cell *cell = &g_array_index (cells, Cell, c);
                int col = 0;

                for (guint k = 0; k < columns->len; k++)
                  if (cell->x >= g_array_index (columns, double, k) - 12.0)
                    col = (int) k;

                g_array_append_val (cols, col);
              }
            g_array_append_val (col_of, cols);
          }

        /* A left-aligned heading over right-aligned numbers starts at a
         * different x from them and so lands in a column of its own.  The
         * two never share a row, though: whenever one column has a cell on
         * a line, the other is empty on that line.  Two neighbouring columns
         * that never co-occupy a line and are not far apart are one column
         * with mixed alignment, and are folded together. */
        do
          {
            merged = FALSE;

            for (guint k = 0; k + 1 < columns->len && !merged; k++)
              {
                gboolean co_occupied = FALSE;
                double gap = g_array_index (columns, double, k + 1) -
                             g_array_index (columns, double, k);

                if (gap > 72.0)
                  continue;

                for (guint l = 0; l < lines->len && !co_occupied; l++)
                  {
                    GArray *cols = g_array_index (col_of, GArray *, l);
                    gboolean has_k = FALSE, has_k1 = FALSE;

                    for (guint c = 0; c < cols->len; c++)
                      {
                        int col = g_array_index (cols, int, c);
                        if (col == (int) k)     has_k = TRUE;
                        if (col == (int) k + 1) has_k1 = TRUE;
                      }
                    co_occupied = has_k && has_k1;
                  }

                if (co_occupied)
                  continue;

                /* Fold k+1 into k. */
                for (guint l = 0; l < lines->len; l++)
                  {
                    GArray *cols = g_array_index (col_of, GArray *, l);

                    for (guint c = 0; c < cols->len; c++)
                      {
                        int *col = &g_array_index (cols, int, c);
                        if (*col > (int) k)
                          (*col)--;
                      }
                  }
                g_array_remove_index (columns, k + 1);
                merged = TRUE;
              }
          }
        while (merged);

        /* Rows: a gap between lines of more than one and a half line
         * pitches is an empty row, or several.  Without this a table with a
         * blank line under its title comes back one row up. */
        row_of = g_array_new (FALSE, FALSE, sizeof (int));
        {
          double pitch = 0.0;
          int row = next_row;

          if (lines->len > 1)
            {
              GArray *gaps = g_array_new (FALSE, FALSE, sizeof (double));

              for (guint l = 1; l < lines->len; l++)
                {
                  double d = g_array_index (lines, Line, l).y -
                             g_array_index (lines, Line, l - 1).y;
                  g_array_append_val (gaps, d);
                }
              g_array_sort (gaps, compare_double);
              pitch = g_array_index (gaps, double, gaps->len / 2);
              g_array_free (gaps, TRUE);
            }

          for (guint l = 0; l < lines->len; l++)
            {
              if (l > 0 && pitch > 0.0)
                {
                  double d = g_array_index (lines, Line, l).y -
                             g_array_index (lines, Line, l - 1).y;
                  int skipped = (int) floor (d / pitch + 0.5) - 1;

                  row += 1 + MAX (0, MIN (skipped, 50));
                }
              g_array_append_val (row_of, row);
            }

          next_row = row + 2;
        }

        for (guint l = 0; l < lines->len; l++)
          {
            GArray *cells = g_array_index (line_cells, GArray *, l);
            GArray *cols = g_array_index (col_of, GArray *, l);
            int row = g_array_index (row_of, int, l);

            for (guint c = 0; c < cells->len; c++)
              {
                Cell *cell = &g_array_index (cells, Cell, c);
                int col = g_array_index (cols, int, c);
                char *stripped = g_strstrip (g_strdup (cell->text->str));

                if (*stripped != '\0' && col < O42_MAX_COLS && row < O42_MAX_ROWS)
                  o42_sheet_set_input (sheet, row, col, stripped);

                g_free (stripped);
                g_string_free (cell->text, TRUE);
              }

            g_array_free (cells, TRUE);
            g_array_free (cols, TRUE);
          }

        g_array_free (col_of, TRUE);
        g_array_free (row_of, TRUE);
        g_array_free (columns, TRUE);
      }

      for (guint l = 0; l < lines->len; l++)
        g_array_free (g_array_index (lines, Line, l).glyphs, TRUE);
      g_array_free (lines, TRUE);
      g_array_free (line_cells, TRUE);
      g_array_free (starts, TRUE);
      g_array_free (glyphs, TRUE);
      g_object_unref (page);
    }

  o42_sheet_end_group (sheet);
  g_object_unref (document);

  return TRUE;
}

#else  /* !HAVE_POPPLER */

gboolean
o42_pdf_import (O42Sheet *sheet, GFile *file, GError **error)
{
  (void) sheet; (void) file;

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This build of office42 cannot read PDF files. "
               "It was built without poppler.");
  return FALSE;
}

#endif
