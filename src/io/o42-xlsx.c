/* o42-xlsx.c - Excel's file format, the Office Open XML one
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-xlsx.h"

#include "o42-pattern.h"

#include <stdio.h>
#include <string.h>

/* A <border> part's four sides. */
typedef struct {
  guint8  style[4];     /* O42BorderStyle by O42_SIDE_* */
  guint32 colour[4];
} BorderDef;

static void
append_border_side (GString *out, const char *side, O42BorderStyle style, guint32 colour)
{
  static const char *names[] = { "none", "thin", "medium", "thick", "double", "dashed", "dotted" };
  if (style == O42_BORDER_NONE)
    g_string_append_printf (out, "<%s/>", side);
  else
    g_string_append_printf (out, "<%s style=\"%s\"><color rgb=\"FF%06X\"/></%s>", side, names[style], colour & 0xFFFFFF, side);
}
#include <stdlib.h>

#include "o42-zip.h"
#include "o42-xlsx-draw.h"
#include "o42-formula.h"
#include "o42-eval.h"

static guint
key_hash_64 (gconstpointer p)
{
  guint64 k = *(const guint64 *) p;
  return (guint) (k ^ (k >> 32));
}

static gboolean
key_equal_64 (gconstpointer a, gconstpointer b)
{
  return *(const guint64 *) a == *(const guint64 *) b;
}


/* Excel measures columns in characters of the default font and rows in
 * points.  At 96dpi with Calibri 11 a column of width w is about 7w+5
 * pixels; the 5 is the padding, and 7 the digit width. */
#define PX_TO_CHARS(px) (((px) - 5) / 7.0)
#define CHARS_TO_PX(ch) ((int) ((ch) * 7.0 + 5.0 + 0.5))
#define PX_TO_PT(px)    ((px) * 0.75)
#define PT_TO_PX(pt)    ((int) ((pt) / 0.75 + 0.5))

#define NS_MAIN "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
#define NS_REL  "http://schemas.openxmlformats.org/officeDocument/2006/relationships"

/* ====================================================================== */
/* Writing                                                                 */
/* ====================================================================== */

typedef struct
{
  GArray     *fmts;      /* O42Fmt, index = xf index */
  GArray     *fmt_styles;  /* guint, beside `fmts`: the cell style the xf
                            * wears, 0 for none (Excel's Normal) */
  GPtrArray  *styles;      /* the book's cell style names, 1-based here */
  GArray     *style_xf;    /* guint: the xf each style's own look is in */
  GPtrArray  *strings;   /* shared strings in order */
  GPtrArray  *string_runs;  /* GArray of O42TextRun beside `strings`, or NULL
                             * where the string is all of a piece */
  GHashTable *string_idx;
  GArray     *dxfs;      /* O42Condition: the differential formats, by index */
  GHashTable *link_rids;   /* guint64 key -> rId of an external hyperlink, per sheet */
} Writer;

/* An xf for this look worn under this style: two cells that look alike
 * but wear different styles need an xf each, since the style is on the
 * xf and not on the cell. */
static guint
xf_for_style (Writer *w, const O42Fmt *fmt, guint style)
{
  for (guint i = 0; i < w->fmts->len; i++)
    if (g_array_index (w->fmt_styles, guint, i) == style &&
        memcmp (&g_array_index (w->fmts, O42Fmt, i), fmt, sizeof *fmt) == 0)
      return i;
  g_array_append_vals (w->fmts, fmt, 1);
  g_array_append_val (w->fmt_styles, style);
  return w->fmts->len - 1;
}

static guint
xf_for (Writer *w, const O42Fmt *fmt)
{
  return xf_for_style (w, fmt, 0);
}

/* Which of the book's styles a cell wears, 1-based; 0 for none. */
static guint
style_index (Writer *w, O42Sheet *sheet, int row, int col)
{
  const char *name = o42_sheet_cell_style (sheet, row, col);

  if (name == NULL)
    return 0;
  for (guint i = 0; i < w->styles->len; i++)
    if (strcmp (g_ptr_array_index (w->styles, i), name) == 0)
      return i + 1;
  return 0;
}

/* g_array_unref through a GDestroyNotify that takes NULL in its
 * stride, since a plain string has no runs. */
static void
g_array_unref_or_null (gpointer data)
{
  if (data != NULL)
    g_array_unref (data);
}

static guint
string_for (Writer *w, const char *text)
{
  gpointer found;

  if (g_hash_table_lookup_extended (w->string_idx, text, NULL, &found))
    return GPOINTER_TO_UINT (found);
  {
    char *copy = g_strdup (text);
    g_ptr_array_add (w->strings, copy);
    g_ptr_array_add (w->string_runs, NULL);
    g_hash_table_insert (w->string_idx, copy, GUINT_TO_POINTER (w->strings->len - 1));
    return w->strings->len - 1;
  }
}

/* Text set in more than one font goes in unshared: two cells reading
 * alike may be set differently, and the runs hang off the string. */
static guint
rich_string_for (Writer *w, const char *text, const O42TextRun *runs, int n_runs)
{
  GArray *copy = g_array_sized_new (FALSE, FALSE, sizeof (O42TextRun), n_runs);

  g_array_append_vals (copy, runs, n_runs);
  g_ptr_array_add (w->strings, g_strdup (text));
  g_ptr_array_add (w->string_runs, copy);
  return w->strings->len - 1;
}

/* The shortest decimal that reads back as the same double: fifteen
 * digits when that is enough, seventeen when it is not.  Excel writes
 * "4.3", not "4.2999999999999998", and so do we. */
static void
append_number (GString *out, double n)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_ascii_formatd (buf, sizeof buf, "%.15g", n);
  if (g_ascii_strtod (buf, NULL) != n)
    g_ascii_formatd (buf, sizeof buf, "%.17g", n);
  g_string_append (out, buf);
}

/* Excel wants an "s" for shared strings, "str" for formula text results,
 * "b", "e", or nothing for numbers. */
static void
append_cell (Writer *w, GString *out, O42Sheet *sheet, int row, int col, guint xf)
{
  char *ref = o42_ref_name (row, col);
  char *input = o42_sheet_get_input (sheet, row, col);
  O42Value value;

  o42_sheet_get_value (sheet, row, col, &value);
  g_string_append_printf (out, "<c r=\"%s\"", ref);
  if (xf != 0)
    g_string_append_printf (out, " s=\"%u\"", xf);

  if (input != NULL && input[0] == '=')
    {
      char *escaped;
      O42Range block;

      /* Excel wants _xlfn. on the functions it added after 2007. */
      {
        O42Node *tree = o42_formula_parse (input + 1);
        char *spelled;
        o42_node_prefix_functions (tree, o42_function_is_future, "_xlfn.");
        spelled = o42_node_to_string (tree);
        escaped = g_markup_escape_text (spelled, -1);
        g_free (spelled);
        o42_node_free (tree);
      }

      switch (value.type)
        {
        case O42_VALUE_TEXT:  g_string_append (out, " t=\"str\""); break;
        case O42_VALUE_BOOL:  g_string_append (out, " t=\"b\""); break;
        case O42_VALUE_ERROR: g_string_append (out, " t=\"e\""); break;
        default: break;
        }
      if (o42_sheet_array_range (sheet, row, col, &block))
        {
          char *a = o42_ref_name (block.row0, block.col0);
          char *b = o42_ref_name (block.row1, block.col1);
          g_string_append_printf (out, "><f t=\"array\" ref=\"%s:%s\">%s</f>", a, b, escaped);
          g_free (a);
          g_free (b);
        }
      else
        g_string_append_printf (out, "><f>%s</f>", escaped);
      g_free (escaped);
      switch (value.type)
        {
        case O42_VALUE_NUMBER:
          g_string_append (out, "<v>");
          append_number (out, value.as.number);
          g_string_append (out, "</v>");
          break;
        case O42_VALUE_TEXT:
          escaped = g_markup_escape_text (value.as.text, -1);
          g_string_append_printf (out, "<v>%s</v>", escaped);
          g_free (escaped);
          break;
        case O42_VALUE_BOOL:
          g_string_append_printf (out, "<v>%d</v>", value.as.boolean ? 1 : 0);
          break;
        case O42_VALUE_ERROR:
          g_string_append_printf (out, "<v>%s</v>", o42_error_name (value.as.error));
          break;
        default:
          break;
        }
      g_string_append (out, "</c>");
    }
  else
    {
      switch (value.type)
        {
        case O42_VALUE_NUMBER:
          g_string_append (out, "><v>");
          append_number (out, value.as.number);
          g_string_append (out, "</v></c>");
          break;
        case O42_VALUE_TEXT:
          {
            int n_runs = 0;
            const O42TextRun *runs = o42_sheet_runs (sheet, row, col, &n_runs);
            guint index = (runs != NULL) ? rich_string_for (w, value.as.text, runs, n_runs)
                                         : string_for (w, value.as.text);

            g_string_append_printf (out, " t=\"s\"><v>%u</v></c>", index);
          }
          break;
        case O42_VALUE_BOOL:
          g_string_append_printf (out, " t=\"b\"><v>%d</v></c>", value.as.boolean ? 1 : 0);
          break;
        case O42_VALUE_ERROR:
          g_string_append_printf (out, " t=\"e\"><v>%s</v></c>", o42_error_name (value.as.error));
          break;
        default:
          g_string_append (out, "/>");
          break;
        }
    }

  o42_value_clear (&value);
  g_free (input);
  g_free (ref);
}

static void
collect_key (O42Sheet *sheet, int row, int col, gpointer user)
{
  (void) sheet;
  guint64 key = o42_key (row, col);
  g_array_append_val ((GArray *) user, key);
}

static int
compare_keys (gconstpointer a, gconstpointer b)
{
  guint64 x = *(const guint64 *) a, y = *(const guint64 *) b;
  return x < y ? -1 : x > y ? 1 : 0;
}

/* A name as a workbook-scope reference: 'Sheet 1'!$A$1:$B$2. */
static char *
name_reference (O42Sheet *sheet, const O42Range *r)
{
  char *quoted = o42_sheet_name_quote (o42_sheet_get_name (sheet));
  char *a = o42_ref_name_full (r->row0, r->col0, TRUE, TRUE);
  char *b = o42_ref_name_full (r->row1, r->col1, TRUE, TRUE);
  char *text = (r->row0 == r->row1 && r->col0 == r->col1)
               ? g_strdup_printf ("%s!%s", quoted, a)
               : g_strdup_printf ("%s!%s:%s", quoted, a, b);
  g_free (quoted);
  g_free (a);
  g_free (b);
  return text;
}

/* Where a sheet's part lives: Excel keeps chart sheets apart from
 * worksheets, in a directory of their own and with their own content
 * type, and both are listed together in the workbook. */
static char *
sheet_part_name (O42Sheet *sheet, int index)
{
  return g_strdup_printf (o42_sheet_is_chart_sheet (sheet)
                          ? "xl/chartsheets/sheet%d.xml" : "xl/worksheets/sheet%d.xml", index);
}

/* A chart sheet holds nothing but its drawing. */
static char *
write_chart_sheet (O42Sheet *sheet, int drawing_rid)
{
  GString *out = g_string_new (
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<chartsheet xmlns=\"" NS_MAIN "\" xmlns:r=\"" NS_REL "\">"
    "<sheetPr/><sheetViews><sheetView zoomScale=\"100\" workbookViewId=\"0\"/></sheetViews>"
    "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>");

  if (drawing_rid > 0)
    g_string_append_printf (out, "<drawing r:id=\"rId%d\"/>", drawing_rid);
  g_string_append (out, "</chartsheet>");
  return g_string_free (out, FALSE);
}

static char *
write_sheet (Writer *w, O42Sheet *sheet, gboolean selected, int drawing_rid, int legacy_rid,
             int table_rid, int n_tables)
{
  GString *out = g_string_new (
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<worksheet xmlns=\"" NS_MAIN "\" xmlns:r=\"" NS_REL "\">");
  O42Range used;
  int frozen_rows, frozen_cols;
  int default_width = o42_sheet_col_width (sheet, O42_MAX_COLS - 1);
  int default_height = o42_sheet_row_height (sheet, O42_MAX_ROWS - 1);
  GArray *keys = g_array_new (FALSE, FALSE, sizeof (guint64));
  O42FmtTable *table = o42_sheet_fmt_table (sheet);
  O42FmtIdx default_idx = o42_fmt_table_default (table);

  {
    /* Excel only fits the sheet to pages when the sheet properties
     * say so; it must be the first element. */
    const O42PrintSetup *ps = o42_sheet_print_setup (sheet);
    guint32 tab = o42_sheet_tab_colour (sheet);
    gboolean fit = ps->fit_wide > 0 || ps->fit_tall > 0;

    if (fit || tab != O42_TAB_NO_COLOUR)
      {
        g_string_append (out, "<sheetPr>");
        if (tab != O42_TAB_NO_COLOUR)
          g_string_append_printf (out, "<tabColor rgb=\"FF%06X\"/>", tab & 0xFFFFFF);
        if (fit)
          g_string_append (out, "<pageSetUpPr fitToPage=\"1\"/>");
        g_string_append (out, "</sheetPr>");
      }
  }

  o42_sheet_used_range (sheet, &used);
  {
    char *a = o42_ref_name (used.row0, used.col0);
    char *b = o42_ref_name (used.row1, used.col1);
    g_string_append_printf (out, "<dimension ref=\"%s:%s\"/>", a, b);
    g_free (a);
    g_free (b);
  }

  o42_sheet_get_frozen (sheet, &frozen_rows, &frozen_cols);
  g_string_append_printf (out, "<sheetViews><sheetView workbookViewId=\"0\"%s>",
                          selected ? " tabSelected=\"1\"" : "");
  if (frozen_rows > 0 || frozen_cols > 0)
    {
      char *top_left = o42_ref_name (frozen_rows, frozen_cols);
      const char *pane = frozen_rows > 0 && frozen_cols > 0 ? "bottomRight"
                         : frozen_rows > 0 ? "bottomLeft" : "topRight";
      g_string_append (out, "<pane");
      if (frozen_cols > 0) g_string_append_printf (out, " xSplit=\"%d\"", frozen_cols);
      if (frozen_rows > 0) g_string_append_printf (out, " ySplit=\"%d\"", frozen_rows);
      g_string_append_printf (out, " topLeftCell=\"%s\" activePane=\"%s\" state=\"frozen\"/>",
                              top_left, pane);
      g_free (top_left);
    }
  g_string_append (out, "</sheetView></sheetViews>");
  g_string_append_printf (out, "<sheetFormatPr defaultColWidth=\"%.6g\" defaultRowHeight=\"%.6g\"/>",
                          PX_TO_CHARS (default_width), PX_TO_PT (default_height));

  /* Columns that differ from the default, one <col> each. */
  {
    GString *cols = g_string_new (NULL);
    for (int col = 0; col < O42_MAX_COLS - 1; col++)
      {
        int width = o42_sheet_col_width (sheet, col);
        gboolean hidden = o42_sheet_col_hidden (sheet, col);
        int level = o42_sheet_col_level (sheet, col);
        if (width == default_width && !hidden && level == 0)
          continue;
        /* A hidden column has no width of its own to speak of; give it
         * the default so it comes back sensible when unhidden. */
        g_string_append_printf (cols, "<col min=\"%d\" max=\"%d\" width=\"%.6g\" customWidth=\"1\"%s",
                                col + 1, col + 1, PX_TO_CHARS (hidden ? default_width : width),
                                hidden ? " hidden=\"1\"" : "");
        if (level > 0)
          g_string_append_printf (cols, " outlineLevel=\"%d\"", level);
        g_string_append (cols, "/>");
      }
    if (cols->len > 0)
      g_string_append_printf (out, "<cols>%s</cols>", cols->str);
    g_string_free (cols, TRUE);
  }

  /* Rows: every row with a cell or a height or hidden flag of its own. */
  g_string_append (out, "<sheetData>");
  o42_sheet_foreach_cell (sheet, collect_key, keys);
  g_array_sort (keys, compare_keys);
  {
    guint i = 0;
    int row = 0;
    while (row < O42_MAX_ROWS)
      {
        int next_key_row = i < keys->len ? o42_key_row (g_array_index (keys, guint64, i)) : O42_MAX_ROWS;
        int height = o42_sheet_row_height (sheet, row);
        gboolean hidden = o42_sheet_row_hidden_by_hand (sheet, row);
        int level = o42_sheet_row_level (sheet, row);
        gboolean own = height != default_height || hidden || level > 0;

        if (row < next_key_row && !own)
          {
            /* Skip ahead: rows up to the next interesting one are plain. */
            if (row > used.row1 && i >= keys->len)
              break;
            row++;
            continue;
          }
        g_string_append_printf (out, "<row r=\"%d\"", row + 1);
        if (height != default_height)
          g_string_append_printf (out, " ht=\"%.6g\" customHeight=\"1\"", PX_TO_PT (height));
        if (hidden)
          g_string_append (out, " hidden=\"1\"");
        if (level > 0)
          g_string_append_printf (out, " outlineLevel=\"%d\"", level);
        g_string_append_c (out, '>');
        while (i < keys->len && o42_key_row (g_array_index (keys, guint64, i)) == row)
          {
            int col = o42_key_col (g_array_index (keys, guint64, i));
            O42FmtIdx idx = o42_sheet_get_fmt_idx (sheet, row, col);
            guint style = style_index (w, sheet, row, col);
            guint xf = (idx == default_idx && style == 0)
                       ? 0 : xf_for_style (w, o42_fmt_table_get (table, idx), style);
            append_cell (w, out, sheet, row, col, xf);
            i++;
          }
        g_string_append (out, "</row>");
        row++;
      }
  }
  g_string_append (out, "</sheetData>");

  if (o42_sheet_protected (sheet))
    g_string_append (out, "<sheetProtection sheet=\"1\" objects=\"1\" scenarios=\"1\"/>");

  if (o42_sheet_n_scenarios (sheet) > 0)
    {
      /* Excel keeps scenarios in the sheet, before the filter. */
      g_string_append (out, "<scenarios current=\"0\" show=\"0\">");
      for (int i = 0; i < o42_sheet_n_scenarios (sheet); i++)
        {
          const char *sname = o42_sheet_scenario_name (sheet, i);
          GArray *cells = NULL;
          GPtrArray *values = NULL;
          const char *comment = NULL;
          char *ename, *ecomment;

          if (!o42_sheet_scenario_cells (sheet, sname, &cells, &values, &comment))
            continue;
          ename = g_markup_escape_text (sname, -1);
          ecomment = g_markup_escape_text (comment != NULL ? comment : "", -1);
          g_string_append_printf (out, "<scenario name=\"%s\" locked=\"0\" count=\"%u\" user=\"office42\" comment=\"%s\">",
                                  ename, cells->len, ecomment);
          for (guint k = 0; k < cells->len && k < values->len; k++)
            {
              guint64 key = g_array_index (cells, guint64, k);
              char *ref = o42_ref_name (o42_key_row (key), o42_key_col (key));
              char *val = g_markup_escape_text (g_ptr_array_index (values, k), -1);
              g_string_append_printf (out, "<inputCells r=\"%s\" val=\"%s\"/>", ref, val);
              g_free (ref);
              g_free (val);
            }
          g_string_append (out, "</scenario>");
          g_free (ename);
          g_free (ecomment);
        }
      g_string_append (out, "</scenarios>");
    }

  {
    O42Range filter;
    if (o42_sheet_get_autofilter (sheet, &filter))
      {
        char *a = o42_ref_name (filter.row0, filter.col0);
        char *b = o42_ref_name (filter.row1, filter.col1);
        g_string_append_printf (out, "<autoFilter ref=\"%s:%s\">", a, b);
        for (int col = filter.col0; col <= filter.col1; col++)
          {
            const char *choice = o42_sheet_autofilter_choice (sheet, col);
            static const char *prefixes[] = { "<>", ">=", "<=", "=", ">", "<" };
            static const char *ops[] = { "notEqual", "greaterThanOrEqual", "lessThanOrEqual", "equal", "greaterThan", "lessThan" };
            const char *op = NULL, *value;
            char *esc;
            if (choice == NULL)
              continue;
            value = choice;
            for (guint k = 0; k < G_N_ELEMENTS (prefixes); k++)
              if (g_str_has_prefix (choice, prefixes[k]))
                { op = ops[k]; value = choice + strlen (prefixes[k]); break; }
            if (op == NULL && (strchr (choice, '*') || strchr (choice, '?')))
              op = "equal";   /* Excel matches wildcards on equal */
            esc = g_markup_escape_text (value, -1);
            if (op != NULL)
              g_string_append_printf (out, "<filterColumn colId=\"%d\"><customFilters><customFilter operator=\"%s\" val=\"%s\"/></customFilters></filterColumn>",
                                      col - filter.col0, op, esc);
            else
              g_string_append_printf (out, "<filterColumn colId=\"%d\"><filters><filter val=\"%s\"/></filters></filterColumn>",
                                      col - filter.col0, esc);
            g_free (esc);
          }
        g_string_append (out, "</autoFilter>");
        g_free (a);
        g_free (b);
      }
  }

  {
    GArray *merges = o42_sheet_merges (sheet);
    if (merges->len > 0)
      {
        g_string_append_printf (out, "<mergeCells count=\"%u\">", merges->len);
        for (guint i = 0; i < merges->len; i++)
          {
            const O42Range *m = &g_array_index (merges, O42Range, i);
            char *a = o42_ref_name (m->row0, m->col0);
            char *b = o42_ref_name (m->row1, m->col1);
            g_string_append_printf (out, "<mergeCell ref=\"%s:%s\"/>", a, b);
            g_free (a);
            g_free (b);
          }
        g_string_append (out, "</mergeCells>");
      }
  }

  /* Conditional formats: one cellIs rule each, its look as a dxf. */
  {
    GArray *conds = o42_sheet_conditions (sheet);
    for (guint i = 0; i < conds->len; i++)
      {
        const O42Condition *c = &g_array_index (conds, O42Condition, i);
        static const char *ops[] = { "between", "notBetween", "equal", "notEqual",
                                     "greaterThan", "lessThan", "greaterThanOrEqual", "lessThanOrEqual" };
        char *a = o42_ref_name (c->range.row0, c->range.col0);
        char *b = o42_ref_name (c->range.row1, c->range.col1);
        char v[G_ASCII_DTOSTR_BUF_SIZE], v2[G_ASCII_DTOSTR_BUF_SIZE];

        g_array_append_vals (w->dxfs, c, 1);
        g_ascii_dtostr (v, sizeof v, c->value);
        g_ascii_dtostr (v2, sizeof v2, c->value2);
        g_string_append_printf (out,
          "<conditionalFormatting sqref=\"%s:%s\"><cfRule type=\"cellIs\" dxfId=\"%u\" priority=\"%u\" operator=\"%s\">"
          "<formula>%s</formula>", a, b, w->dxfs->len - 1, i + 1, ops[c->op], v);
        if (c->op == O42_COND_BETWEEN || c->op == O42_COND_NOT_BETWEEN)
          g_string_append_printf (out, "<formula>%s</formula>", v2);
        g_string_append (out, "</cfRule></conditionalFormatting>");
        g_free (a);
        g_free (b);
      }
  }

  {
    GArray *rules = o42_sheet_validations (sheet);
    if (rules->len > 0)
      {
        static const char *types[] = { "none", "whole", "decimal", "list", "date", "time", "textLength" };
        static const char *ops[] = { "between", "notBetween", "equal", "notEqual",
                                     "greaterThan", "lessThan", "greaterThanOrEqual", "lessThanOrEqual" };
        g_string_append_printf (out, "<dataValidations count=\"%u\">", rules->len);
        for (guint i = 0; i < rules->len; i++)
          {
            const O42Validation *v = &g_array_index (rules, O42Validation, i);
            char *a = o42_ref_name (v->range.row0, v->range.col0);
            char *b = o42_ref_name (v->range.row1, v->range.col1);
            char *f1, *f2 = NULL, *msg = g_markup_escape_text (v->message ? v->message : "", -1);
            if (v->kind == O42_VALID_LIST)
              {
                char *quoted = g_strdup_printf ("\"%s\"", v->value ? v->value : "");
                f1 = g_markup_escape_text (quoted, -1);
                g_free (quoted);
              }
            else
              f1 = g_markup_escape_text (v->value ? v->value : "", -1);
            if (v->value2 != NULL && v->value2[0] != '\0')
              f2 = g_markup_escape_text (v->value2, -1);
            g_string_append_printf (out,
              "<dataValidation type=\"%s\" operator=\"%s\" allowBlank=\"%d\" showInputMessage=\"0\" "
              "showErrorMessage=\"1\"%s%s%s sqref=\"%s:%s\"><formula1>%s</formula1>",
              types[v->kind], ops[v->op], v->allow_blank ? 1 : 0,
              msg[0] ? " error=\"" : "", msg, msg[0] ? "\"" : "", a, b, f1);
            if (f2 != NULL)
              g_string_append_printf (out, "<formula2>%s</formula2>", f2);
            g_string_append (out, "</dataValidation>");
            g_free (a); g_free (b); g_free (f1); g_free (f2); g_free (msg);
          }
        g_string_append (out, "</dataValidations>");
      }
  }

  if (g_hash_table_size (o42_sheet_links (sheet)) > 0)
    {
      GHashTableIter lit;
      gpointer lk, lv;
      g_string_append (out, "<hyperlinks>");
      g_hash_table_iter_init (&lit, o42_sheet_links (sheet));
      while (g_hash_table_iter_next (&lit, &lk, &lv))
        {
          guint64 key = *(guint64 *) lk;
          const char *target = lv;
          char *ref = o42_ref_name (o42_key_row (key), o42_key_col (key));
          if (target[0] == '#')
            {
              char *loc = g_markup_escape_text (target + 1, -1);
              g_string_append_printf (out, "<hyperlink ref=\"%s\" location=\"%s\"/>", ref, loc);
              g_free (loc);
            }
          else
            g_string_append_printf (out, "<hyperlink ref=\"%s\" r:id=\"rId%d\"/>", ref,
                                    GPOINTER_TO_INT (g_hash_table_lookup (w->link_rids, &key)));
          g_free (ref);
        }
      g_string_append (out, "</hyperlinks>");
    }
  {
    const O42PrintSetup *ps = o42_sheet_print_setup (sheet);
    double inches = ps->margin / 72.0;

    /* Manual page breaks, before the print options as the schema
     * wants them. */
    {
      GArray *rb = o42_sheet_page_breaks (sheet, TRUE);
      GArray *cb = o42_sheet_page_breaks (sheet, FALSE);
      if (rb->len > 0)
        {
          g_string_append_printf (out, "<rowBreaks count=\"%u\" manualBreakCount=\"%u\">", rb->len, rb->len);
          for (guint i = 0; i < rb->len; i++)
            g_string_append_printf (out, "<brk id=\"%d\" max=\"16383\" man=\"1\"/>", g_array_index (rb, int, i));
          g_string_append (out, "</rowBreaks>");
        }
      if (cb->len > 0)
        {
          g_string_append_printf (out, "<colBreaks count=\"%u\" manualBreakCount=\"%u\">", cb->len, cb->len);
          for (guint i = 0; i < cb->len; i++)
            g_string_append_printf (out, "<brk id=\"%d\" max=\"1048575\" man=\"1\"/>", g_array_index (cb, int, i));
          g_string_append (out, "</colBreaks>");
        }
    }
    if (ps->gridlines || ps->headings)
      g_string_append_printf (out, "<printOptions%s%s/>", ps->gridlines ? " gridLines=\"1\"" : "",
                              ps->headings ? " headings=\"1\"" : "");
    g_string_append_printf (out, "<pageMargins left=\"%.3f\" right=\"%.3f\" top=\"%.3f\" bottom=\"%.3f\" "
                                 "header=\"0.3\" footer=\"0.3\"/>", inches, inches, inches, inches);
    if (ps->scale != 100 || ps->fit_wide > 0 || ps->fit_tall > 0)
      {
        GString *setup_attrs = g_string_new (NULL);

        if (ps->fit_wide > 0 || ps->fit_tall > 0)
          {
            /* Excel wants to be told the sheet is fitted, in the
             * sheet properties, as well as to how many pages. */
            if (ps->fit_wide > 0) g_string_append_printf (setup_attrs, " fitToWidth=\"%d\"", ps->fit_wide);
            if (ps->fit_tall > 0) g_string_append_printf (setup_attrs, " fitToHeight=\"%d\"", ps->fit_tall);
          }
        else
          g_string_append_printf (setup_attrs, " scale=\"%d\"", ps->scale);
        g_string_append_printf (out, "<pageSetup%s/>", setup_attrs->str);
        g_string_free (setup_attrs, TRUE);
      }
    if ((ps->header != NULL && *ps->header != '\0') || (ps->footer != NULL && *ps->footer != '\0'))
      {
        char *h = g_markup_escape_text (ps->header != NULL ? ps->header : "", -1);
        char *f = g_markup_escape_text (ps->footer != NULL ? ps->footer : "", -1);
        g_string_append_printf (out, "<headerFooter><oddHeader>%s</oddHeader><oddFooter>%s</oddFooter></headerFooter>", h, f);
        g_free (h);
        g_free (f);
      }
  }
  if (drawing_rid > 0)
    g_string_append_printf (out, "<drawing r:id=\"rId%d\"/>", drawing_rid);
  if (legacy_rid > 0)
    g_string_append_printf (out, "<legacyDrawing r:id=\"rId%d\"/>", legacy_rid);
  if (n_tables > 0)
    {
      g_string_append_printf (out, "<tableParts count=\"%d\">", n_tables);
      for (int i = 0; i < n_tables; i++)
        g_string_append_printf (out, "<tablePart r:id=\"rId%d\"/>", table_rid + i);
      g_string_append (out, "</tableParts>");
    }
  g_string_append (out, "</worksheet>");
  g_array_unref (keys);
  return g_string_free (out, FALSE);
}

static void
append_rgb (GString *out, guint32 colour)
{
  g_string_append_printf (out, "FF%06X", colour & 0xFFFFFF);
}

static const char *
xlsx_halign (O42HAlign h)
{
  switch (h)
    {
    case O42_HALIGN_LEFT:   return "left";
    case O42_HALIGN_CENTRE: return "center";
    case O42_HALIGN_RIGHT:  return "right";
    default:                return NULL;
    }
}

static const char *
xlsx_valign (O42VAlign v)
{
  switch (v)
    {
    case O42_VALIGN_TOP:    return "top";
    case O42_VALIGN_MIDDLE: return "center";
    default:                return NULL;   /* bottom is Excel's default too */
    }
}

static char *
write_styles (Writer *w)
{
  GString *out = g_string_new (
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<styleSheet xmlns=\"" NS_MAIN "\">");
  guint n = w->fmts->len;
  GArray *numfmt_ids = g_array_new (FALSE, FALSE, sizeof (int));
  GString *numfmts = g_string_new (NULL);
  int next_id = 164;

  /* Number formats: built-in ids for the ones Excel has, custom ones
   * (from 164 up) for the rest. */
  for (guint i = 0; i < n; i++)
    {
      const O42Fmt *f = &g_array_index (w->fmts, O42Fmt, i);
      char *code = o42_fmt_format_string (f);
      int id;

      if (strcmp (code, "General") == 0) id = 0;
      else if (strcmp (code, "0") == 0) id = 1;
      else if (strcmp (code, "0.00") == 0) id = 2;
      else if (strcmp (code, "#,##0") == 0) id = 3;
      else if (strcmp (code, "#,##0.00") == 0) id = 4;
      else if (strcmp (code, "0%") == 0) id = 9;
      else if (strcmp (code, "0.00%") == 0) id = 10;
      else if (strcmp (code, "0.00E+00") == 0) id = 11;
      else if (strcmp (code, "@") == 0) id = 49;
      else
        {
          char *escaped = g_markup_escape_text (code, -1);
          id = next_id++;
          g_string_append_printf (numfmts, "<numFmt numFmtId=\"%d\" formatCode=\"%s\"/>", id, escaped);
          g_free (escaped);
        }
      g_array_append_val (numfmt_ids, id);
      g_free (code);
    }
  if (next_id > 164)
    g_string_append_printf (out, "<numFmts count=\"%d\">%s</numFmts>", next_id - 164, numfmts->str);
  g_string_free (numfmts, TRUE);

  /* One font per format; Excel does not mind repeats. */
  g_string_append_printf (out, "<fonts count=\"%u\">", n);
  for (guint i = 0; i < n; i++)
    {
      const O42Fmt *f = &g_array_index (w->fmts, O42Fmt, i);
      char *family = g_markup_escape_text (f->family ? f->family : "Calibri", -1);
      g_string_append (out, "<font>");
      if (f->bold) g_string_append (out, "<b/>");
      if (f->italic) g_string_append (out, "<i/>");
      if (f->underline) g_string_append (out, "<u/>");
      if (f->strikeout) g_string_append (out, "<strike/>");
      g_string_append_printf (out, "<sz val=\"%g\"/>", f->size / 2.0);
      if (f->colour != 0)
        {
          g_string_append (out, "<color rgb=\"");
          append_rgb (out, f->colour);
          g_string_append (out, "\"/>");
        }
      g_string_append_printf (out, "<name val=\"%s\"/></font>", family);
      g_free (family);
    }
  g_string_append (out, "</fonts>");

  /* Fills 0 and 1 are fixed by Excel: none and the gray125 pattern. */
  g_string_append_printf (out, "<fills count=\"%u\"><fill><patternFill patternType=\"none\"/></fill>"
                          "<fill><patternFill patternType=\"gray125\"/></fill>", n + 2);
  for (guint i = 0; i < n; i++)
    {
      const O42Fmt *f = &g_array_index (w->fmts, O42Fmt, i);
      if (f->pattern != O42_PATTERN_NONE)
        {
          /* A pattern's own colour is the foreground and the shading
           * behind it the background, which is how Excel writes one. */
          g_string_append_printf (out, "<fill><patternFill patternType=\"%s\"><fgColor rgb=\"",
                                  o42_pattern_name ((O42Pattern) f->pattern));
          append_rgb (out, f->pattern_colour);
          g_string_append (out, "\"/>");
          if (f->fill != O42_FILL_NONE)
            {
              g_string_append (out, "<bgColor rgb=\"");
              append_rgb (out, f->fill);
              g_string_append (out, "\"/>");
            }
          else
            g_string_append (out, "<bgColor indexed=\"64\"/>");
          g_string_append (out, "</patternFill></fill>");
        }
      else if (f->fill == O42_FILL_NONE)
        g_string_append (out, "<fill><patternFill patternType=\"none\"/></fill>");
      else
        {
          g_string_append (out, "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"");
          append_rgb (out, f->fill);
          g_string_append (out, "\"/><bgColor indexed=\"64\"/></patternFill></fill>");
        }
    }
  g_string_append (out, "</fills>");

  g_string_append_printf (out, "<borders count=\"%u\">", n + 1);
  g_string_append (out, "<border><left/><right/><top/><bottom/><diagonal/></border>");
  for (guint i = 0; i < n; i++)
    {
      const O42Fmt *f = &g_array_index (w->fmts, O42Fmt, i);
      g_string_append (out, "<border>");
      append_border_side (out, "left", f->border_style[O42_SIDE_LEFT], f->border_colour[O42_SIDE_LEFT]);
      append_border_side (out, "right", f->border_style[O42_SIDE_RIGHT], f->border_colour[O42_SIDE_RIGHT]);
      append_border_side (out, "top", f->border_style[O42_SIDE_TOP], f->border_colour[O42_SIDE_TOP]);
      append_border_side (out, "bottom", f->border_style[O42_SIDE_BOTTOM], f->border_colour[O42_SIDE_BOTTOM]);
      g_string_append (out, "<diagonal/></border>");
    }
  g_string_append (out, "</borders>");

  /* One cellStyleXf per named style, and the styles themselves.  A
   * style's own look is in an xf of its own, put there when the book
   * was walked, so that Excel shows the style with its formatting and
   * not merely its name. */
  g_string_append_printf (out, "<cellStyleXfs count=\"%u\">"
                          "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/>",
                          w->styles->len + 1);
  for (guint i = 0; i < w->styles->len; i++)
    {
      guint x = g_array_index (w->style_xf, guint, i);

      g_string_append_printf (out,
        "<xf numFmtId=\"%d\" fontId=\"%u\" fillId=\"%u\" borderId=\"%u\""
        " applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"/>",
        g_array_index (numfmt_ids, int, x), x, x + 2, x + 1);
    }
  g_string_append (out, "</cellStyleXfs>");
  g_string_append_printf (out, "<cellXfs count=\"%u\">", n);
  for (guint i = 0; i < n; i++)
    {
      const O42Fmt *f = &g_array_index (w->fmts, O42Fmt, i);
      const char *h = xlsx_halign (f->halign), *v = xlsx_valign (f->valign);
      gboolean align = h != NULL || v != NULL || f->wrap || f->indent > 0 || f->rotation != 0;

      g_string_append_printf (out, "<xf numFmtId=\"%d\" fontId=\"%u\" fillId=\"%u\" borderId=\"%u\" xfId=\"%u\""
                              " applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\"",
                              g_array_index (numfmt_ids, int, i), i, i + 2, i + 1,
                              g_array_index (w->fmt_styles, guint, i));
      if (!f->locked || f->hidden)
        g_string_append_printf (out, " applyProtection=\"1\"");
      if (!align && (f->locked && !f->hidden))
        g_string_append (out, "/>");
      else if (!align)
        g_string_append_printf (out, "><protection locked=\"%d\" hidden=\"%d\"/></xf>",
                                f->locked ? 1 : 0, f->hidden ? 1 : 0);
      else
        {
          g_string_append (out, " applyAlignment=\"1\"><alignment");
          if (h) g_string_append_printf (out, " horizontal=\"%s\"", h);
          if (v) g_string_append_printf (out, " vertical=\"%s\"", v);
          if (f->wrap) g_string_append (out, " wrapText=\"1\"");
          if (f->indent > 0) g_string_append_printf (out, " indent=\"%d\"", f->indent);
          if (f->rotation != 0) g_string_append_printf (out, " textRotation=\"%d\"", f->rotation >= 0 ? f->rotation : 90 - f->rotation);
          g_string_append (out, "/>");
          if (!f->locked || f->hidden)
            g_string_append_printf (out, "<protection locked=\"%d\" hidden=\"%d\"/>",
                                    f->locked ? 1 : 0, f->hidden ? 1 : 0);
          g_string_append (out, "</xf>");
        }
    }
  g_string_append (out, "</cellXfs>");

  g_string_append_printf (out, "<cellStyles count=\"%u\">"
                          "<cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/>",
                          w->styles->len + 1);
  for (guint i = 0; i < w->styles->len; i++)
    {
      char *name = g_markup_escape_text (g_ptr_array_index (w->styles, i), -1);

      g_string_append_printf (out, "<cellStyle name=\"%s\" xfId=\"%u\"/>", name, i + 1);
      g_free (name);
    }
  g_string_append (out, "</cellStyles>");
  g_string_append (out, "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>");
  g_string_append_printf (out, "<dxfs count=\"%u\">", w->dxfs->len);
  for (guint i = 0; i < w->dxfs->len; i++)
    {
      const O42Condition *c = &g_array_index (w->dxfs, O42Condition, i);
      const O42Fmt *f = &c->fmt;
      g_string_append (out, "<dxf>");
      if (c->mask & (O42_FMT_BOLD | O42_FMT_ITALIC | O42_FMT_UNDERLINE | O42_FMT_STRIKEOUT | O42_FMT_COLOUR))
        {
          g_string_append (out, "<font>");
          if ((c->mask & O42_FMT_BOLD) && f->bold) g_string_append (out, "<b/>");
          if ((c->mask & O42_FMT_ITALIC) && f->italic) g_string_append (out, "<i/>");
          if ((c->mask & O42_FMT_STRIKEOUT) && f->strikeout) g_string_append (out, "<strike/>");
          if ((c->mask & O42_FMT_UNDERLINE) && f->underline) g_string_append (out, "<u/>");
          if (c->mask & O42_FMT_COLOUR)
            {
              g_string_append (out, "<color rgb=\"");
              append_rgb (out, f->colour);
              g_string_append (out, "\"/>");
            }
          g_string_append (out, "</font>");
        }
      if ((c->mask & O42_FMT_FILL) && f->fill != O42_FILL_NONE)
        {
          g_string_append (out, "<fill><patternFill><bgColor rgb=\"");
          append_rgb (out, f->fill);
          g_string_append (out, "\"/></patternFill></fill>");
        }
      if (c->mask & O42_FMT_BORDERS)
        {
          g_string_append (out, "<border>");
          if (f->border_left) g_string_append (out, "<left style=\"thin\"><color auto=\"1\"/></left>");
          if (f->border_right) g_string_append (out, "<right style=\"thin\"><color auto=\"1\"/></right>");
          if (f->border_top) g_string_append (out, "<top style=\"thin\"><color auto=\"1\"/></top>");
          if (f->border_bottom) g_string_append (out, "<bottom style=\"thin\"><color auto=\"1\"/></bottom>");
          g_string_append (out, "</border>");
        }
      g_string_append (out, "</dxf>");
    }
  g_string_append (out, "</dxfs></styleSheet>");
  g_array_unref (numfmt_ids);
  return g_string_free (out, FALSE);
}

/* Excel keeps its form controls in the sheet's legacy drawing -- the
 * same VML part the notes are in -- as a shape apiece with an
 * x:ClientData that says what kind it is and which cell it drives.
 * That is how Excel 2003 wrote them into .xls, how Excel 2007 wrote
 * them into .xlsx beside the newer ctrlProps parts, and what every
 * reader still understands.
 *
 * The map from office42's kinds to Excel's names. */
static const char *
vml_control_type (O42ShapeKind kind)
{
  switch (kind)
    {
    case O42_SHAPE_BUTTON:    return "Button";
    case O42_SHAPE_CHECKBOX:  return "Checkbox";
    case O42_SHAPE_OPTION:    return "Radio";
    case O42_SHAPE_SPINNER:   return "Spin";
    case O42_SHAPE_SCROLLBAR: return "Scroll";
    case O42_SHAPE_LISTBOX:   return "List";
    case O42_SHAPE_COMBO:     return "Drop";
    case O42_SHAPE_LABEL:     return "Label";
    case O42_SHAPE_GROUPBOX:  return "GBox";
    default:                  return NULL;
    }
}

/* A cell reference the way a control's formula wants it: $B$3. */
static char *
vml_absolute_ref (const char *text)
{
  int row, col;

  if (text == NULL || *text == 0 || !o42_ref_parse (text, &row, &col, NULL))
    return NULL;
  {
    char *plain = o42_ref_name (row, col);
    char *out = g_strdup_printf ("$%c$%s", plain[0], plain + 1);

    /* o42_ref_name writes AB12; the dollars go before the letters and
     * before the digits, so the letters are copied one by one. */
    g_free (out);
    {
      GString *dollars = g_string_new ("$");
      const char *q = plain;

      while (g_ascii_isalpha (*q))
        g_string_append_c (dollars, *q++);
      g_string_append_c (dollars, '$');
      g_string_append (dollars, q);
      g_free (plain);
      return g_string_free (dollars, FALSE);
    }
  }
}

static void
write_control_vml (GString *vml, O42Sheet *sheet, const O42Shape *shape, int shape_id)
{
  const char *type = vml_control_type (shape->kind);
  char *link = vml_absolute_ref (shape->link);
  char *caption = g_markup_escape_text (shape->text != NULL ? shape->text : "", -1);
  double left = 0, top = 0;
  double value = 0;
  gboolean has_value;

  if (type == NULL)
    return;

  for (int c = 0; c < shape->col; c++)
    left += o42_sheet_col_width (sheet, c);
  for (int r = 0; r < shape->row; r++)
    top += o42_sheet_row_height (sheet, r);
  left = (left + shape->dx) * 0.75;    /* pixels to points */
  top = (top + shape->dy) * 0.75;
  has_value = o42_sheet_control_value (sheet, shape, &value);

  g_string_append_printf (vml,
    "<v:shape id=\"_x0000_s%d\" type=\"#_x0000_t201\" style=\"position:absolute;"
    "margin-left:%gpt;margin-top:%gpt;width:%gpt;height:%gpt;z-index:%d\" o:button=\"t\" "
    "fillcolor=\"buttonFace [67]\" strokecolor=\"windowText [64]\">"
    "<v:textbox style=\"mso-direction-alt:auto\"><div style=\"text-align:center\">"
    "<font face=\"Arial\" size=\"200\">%s</font></div></v:textbox>"
    "<x:ClientData ObjectType=\"%s\">"
    "<x:MoveWithCells/><x:SizeWithCells/>"
    "<x:Anchor>%d, 0, %d, 0, %d, 0, %d, 0</x:Anchor>"
    "<x:AutoFill>False</x:AutoFill>",
    shape_id, left, top, shape->width * 0.75, shape->height * 0.75, shape_id - 1024,
    caption, type,
    shape->col, shape->row, shape->col + 2, shape->row + 2);

  if (link != NULL)
    g_string_append_printf (vml, "<x:FmlaLink>%s</x:FmlaLink>", link);
  if (shape->script != NULL && *shape->script != 0)
    {
      /* Excel's word for what a button runs.  office42's scripts are
       * Python and Excel's are not, but the name travels. */
      char *macro = g_markup_escape_text (shape->script, -1);

      g_string_append_printf (vml, "<x:FmlaMacro>%s</x:FmlaMacro>", macro);
      g_free (macro);
    }
  if (shape->source != NULL && *shape->source != 0)
    g_string_append_printf (vml, "<x:FmlaRange>%s</x:FmlaRange>", shape->source);

  switch (shape->kind)
    {
    case O42_SHAPE_CHECKBOX:
      g_string_append_printf (vml, "<x:Checked>%d</x:Checked>",
                              (has_value && value != 0) ? 1 : 0);
      break;
    case O42_SHAPE_OPTION:
      g_string_append_printf (vml, "<x:Checked>%d</x:Checked>",
                              (has_value && value == (shape->value != 0 ? shape->value : 1)) ? 1 : 0);
      break;
    case O42_SHAPE_SPINNER:
    case O42_SHAPE_SCROLLBAR:
      g_string_append_printf (vml,
        "<x:Val>%d</x:Val><x:Min>%d</x:Min><x:Max>%d</x:Max><x:Inc>%d</x:Inc>",
        (int) (has_value ? value : shape->min), (int) shape->min, (int) shape->max,
        (int) (shape->step > 0 ? shape->step : 1));
      if (shape->kind == O42_SHAPE_SCROLLBAR)
        g_string_append_printf (vml, "<x:Page>%d</x:Page><x:Horiz>%d</x:Horiz>",
                                (int) (shape->page > 0 ? shape->page : 10),
                                shape->width >= shape->height ? 1 : 0);
      break;
    case O42_SHAPE_LISTBOX:
    case O42_SHAPE_COMBO:
      if (has_value)
        g_string_append_printf (vml, "<x:Sel>%d</x:Sel>", (int) value);
      g_string_append (vml, "<x:SelType>Single</x:SelType>");
      break;
    default:
      break;
    }

  g_string_append (vml, "</x:ClientData></v:shape>");
  g_free (link);
  g_free (caption);
}

/* Notes go out as Excel's comments: a comments part with the text, and
 * the VML drawing Excel needs before it will show them.  Returns the
 * relationship id of the VML part, or 0 if the sheet has no notes. */
/* A table part per table on the sheet, and the relationships that
 * point at them; the count comes back and the first relationship's id
 * goes in `first_rid`. */
static int
write_tables (O42ZipWriter *zip, O42Sheet *sheet, int index, GString *content_types,
              GString *sheet_rels, int *next_rid, int *first_rid)
{
  GArray *tables = o42_sheet_tables (sheet);
  static int part_number = 0;

  *first_rid = *next_rid;
  for (guint i = 0; i < tables->len; i++)
    {
      const O42Table *t = &g_array_index (tables, O42Table, i);
      GString *xml = g_string_new (NULL);
      char *a1 = o42_ref_name (t->range.row0, t->range.col0);
      char *b1 = o42_ref_name (t->range.row1, t->range.col1);
      char *name = g_markup_escape_text (t->name, -1);
      char *part;

      part_number++;
      g_string_append_printf (xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<table xmlns=\"" NS_MAIN "\" id=\"%d\" name=\"%s\" displayName=\"%s\" ref=\"%s:%s\" "
        "headerRowCount=\"%d\" totalsRowShown=\"0\">",
        part_number, name, name, a1, b1, t->has_headers ? 1 : 0);
      g_string_append_printf (xml, "<tableColumns count=\"%d\">", t->range.col1 - t->range.col0 + 1);
      for (int col = t->range.col0; col <= t->range.col1; col++)
        {
          char *head = t->has_headers ? o42_sheet_get_display (sheet, t->range.row0, col) : NULL;
          char *label = head != NULL && *head != '\0' ? g_markup_escape_text (head, -1)
                                                     : g_strdup_printf ("Column%d", col - t->range.col0 + 1);
          g_string_append_printf (xml, "<tableColumn id=\"%d\" name=\"%s\"/>", col - t->range.col0 + 1, label);
          g_free (label);
          g_free (head);
        }
      g_string_append (xml, "</tableColumns>");
      g_string_append (xml, "<tableStyleInfo name=\"TableStyleMedium2\" showFirstColumn=\"0\" "
                            "showLastColumn=\"0\" showRowStripes=\"1\" showColumnStripes=\"0\"/>");
      g_string_append (xml, "</table>");

      part = g_strdup_printf ("xl/tables/table%d.xml", part_number);
      o42_zip_writer_add (zip, part, xml->str, xml->len);
      g_string_append_printf (content_types,
        "<Override PartName=\"/%s\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.table+xml\"/>", part);
      g_string_append_printf (sheet_rels,
        "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/table\" Target=\"../tables/table%d.xml\"/>",
        *next_rid, part_number);
      (*next_rid)++;
      g_free (part);
      g_free (name);
      g_free (a1);
      g_free (b1);
      g_string_free (xml, TRUE);
      (void) index;
    }
  return (int) tables->len;
}

static int
write_comments (O42ZipWriter *zip, O42Sheet *sheet, int index, GString *content_types,
                GHashTable *extensions_seen, GString *sheet_rels, int *next_rid)
{
  GHashTable *notes = o42_sheet_notes (sheet);
  GPtrArray *shapes = o42_sheet_shapes (sheet);
  GHashTableIter iter;
  gpointer key, value;
  GString *comments, *vml;
  char *part;
  int comments_rid = 0, vml_rid, shape = 1025;
  int n_controls = 0;

  for (guint i = 0; i < shapes->len; i++)
    if (o42_shape_is_control (((O42Shape *) g_ptr_array_index (shapes, i))->kind))
      n_controls++;

  if (g_hash_table_size (notes) == 0 && n_controls == 0)
    return 0;

  comments = g_string_new (
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<comments xmlns=\"" NS_MAIN "\"><authors><author>office42</author></authors><commentList>");
  vml = g_string_new (
    "<xml xmlns:v=\"urn:schemas-microsoft-com:vml\" xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
    "xmlns:x=\"urn:schemas-microsoft-com:office:excel\">"
    "<o:shapelayout v:ext=\"edit\"><o:idmap v:ext=\"edit\" data=\"1\"/></o:shapelayout>"
    "<v:shapetype id=\"_x0000_t202\" coordsize=\"21600,21600\" o:spt=\"202\" path=\"m,l,21600r21600,l21600,xe\">"
    "<v:stroke joinstyle=\"miter\"/><v:path gradientshapeok=\"t\" o:connecttype=\"rect\"/></v:shapetype>"
    "<v:shapetype id=\"_x0000_t201\" coordsize=\"21600,21600\" o:spt=\"201\" path=\"m,l,21600r21600,l21600,xe\">"
    "<v:stroke joinstyle=\"miter\"/><v:path shadowok=\"t\" o:extrusionok=\"t\" gradientshapeok=\"t\" "
    "o:connecttype=\"rect\"/></v:shapetype>");

  g_hash_table_iter_init (&iter, notes);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      guint64 k = *(guint64 *) key;
      int row = o42_key_row (k), col = o42_key_col (k);
      char *ref = o42_ref_name (row, col);
      char *text = g_markup_escape_text (value, -1);

      g_string_append_printf (comments,
        "<comment ref=\"%s\" authorId=\"0\"><text><r><t xml:space=\"preserve\">%s</t></r></text></comment>", ref, text);
      g_string_append_printf (vml,
        "<v:shape id=\"_x0000_s%d\" type=\"#_x0000_t202\" style=\"position:absolute;margin-left:80pt;margin-top:%dpt;"
        "width:120pt;height:60pt;z-index:%d;visibility:hidden\" fillcolor=\"#ffffe1\" o:insetmode=\"auto\">"
        "<v:fill color2=\"#ffffe1\"/><v:shadow on=\"t\" color=\"black\" obscured=\"t\"/><v:path o:connecttype=\"none\"/>"
        "<v:textbox style=\"mso-direction-alt:auto\"><div style=\"text-align:left\"></div></v:textbox>"
        "<x:ClientData ObjectType=\"Note\"><x:MoveWithCells/><x:SizeWithCells/>"
        "<x:Anchor>%d, 15, %d, 2, %d, 15, %d, 15</x:Anchor><x:AutoFill>False</x:AutoFill>"
        "<x:Row>%d</x:Row><x:Column>%d</x:Column></x:ClientData></v:shape>",
        shape, row * 15, shape - 1024, col + 1, MAX (row - 1, 0), col + 3, row + 3, row, col);
      shape++;
      g_free (text);
      g_free (ref);
    }
  /* The controls share the notes' drawing: one VML part a sheet. */
  for (guint i = 0; i < shapes->len; i++)
    {
      const O42Shape *sh = g_ptr_array_index (shapes, i);

      if (o42_shape_is_control (sh->kind))
        write_control_vml (vml, sheet, sh, shape++);
    }

  g_string_append (comments, "</commentList></comments>");
  g_string_append (vml, "</xml>");

  if (g_hash_table_size (notes) > 0)
    {
      part = g_strdup_printf ("xl/comments%d.xml", index);
      o42_zip_writer_add (zip, part, comments->str, comments->len);
      g_string_append_printf (content_types,
        "<Override PartName=\"/%s\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.comments+xml\"/>", part);
      comments_rid = (*next_rid)++;
      g_string_append_printf (sheet_rels,
        "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments\" Target=\"../comments%d.xml\"/>",
        comments_rid, index);
      g_free (part);
    }

  part = g_strdup_printf ("xl/drawings/vmlDrawing%d.vml", index);
  o42_zip_writer_add (zip, part, vml->str, vml->len);
  if (!g_hash_table_contains (extensions_seen, "vml"))
    {
      g_hash_table_add (extensions_seen, g_strdup ("vml"));
      g_string_append (content_types,
        "<Default Extension=\"vml\" ContentType=\"application/vnd.openxmlformats-officedocument.vmlDrawing\"/>");
    }
  vml_rid = (*next_rid)++;
  g_string_append_printf (sheet_rels,
    "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/vmlDrawing\" Target=\"../drawings/vmlDrawing%d.vml\"/>",
    vml_rid, index);
  g_free (part);

  g_string_free (comments, TRUE);
  g_string_free (vml, TRUE);
  return vml_rid;
}

/* ---- Reading the form controls back out of the legacy drawing -------- */

/* The other half of write_control_vml: a VML shape whose x:ClientData
 * says what kind of control it is, which cell it drives, and where it
 * sits.  Excel's own files are read the same way, which is the point --
 * a book that comes from Excel with a check box on it arrives with the
 * check box. */

typedef struct {
  O42Sheet   *sheet;
  gboolean    in_shape;
  gboolean    in_client;
  char       *style;         /* the shape's own style attribute */
  char       *object_type;   /* Checkbox, Spin, Scroll, Drop, List, ... */
  char       *field;         /* the ClientData child being read */
  GString    *text;          /* its text */
  char       *link, *range, *macro;
  GString    *caption;
  gboolean    in_textbox;
  double      value, min, max, step, page;
  gboolean    checked, horizontal;
  int         anchor_row, anchor_col;
} VmlReader;

static O42ShapeKind
vml_kind (const char *type)
{
  if (type == NULL)                     return O42_SHAPE_RECT;
  if (strcmp (type, "Checkbox") == 0)   return O42_SHAPE_CHECKBOX;
  if (strcmp (type, "Radio") == 0)      return O42_SHAPE_OPTION;
  if (strcmp (type, "Spin") == 0)       return O42_SHAPE_SPINNER;
  if (strcmp (type, "Scroll") == 0)     return O42_SHAPE_SCROLLBAR;
  if (strcmp (type, "List") == 0)       return O42_SHAPE_LISTBOX;
  if (strcmp (type, "Drop") == 0)       return O42_SHAPE_COMBO;
  if (strcmp (type, "Button") == 0)     return O42_SHAPE_BUTTON;
  if (strcmp (type, "Label") == 0)      return O42_SHAPE_LABEL;
  if (strcmp (type, "GBox") == 0)       return O42_SHAPE_GROUPBOX;
  return O42_SHAPE_RECT;
}

/* One measurement out of a VML style string: "width:120pt" in points,
 * answered in pixels. */
static double
vml_style_px (const char *style, const char *name, double fallback)
{
  const char *at;
  char *key;

  if (style == NULL)
    return fallback;
  key = g_strdup_printf ("%s:", name);
  at = strstr (style, key);
  g_free (key);
  if (at == NULL)
    return fallback;
  at = strchr (at, ':') + 1;
  return g_ascii_strtod (at, NULL) / 0.75;   /* points to pixels */
}

static void
vml_start (GMarkupParseContext *ctx, const char *element, const char **names,
           const char **values, gpointer user, GError **error)
{
  VmlReader *r = user;
  const char *n = strchr (element, ':') ? strchr (element, ':') + 1 : element;

  (void) ctx; (void) error;
  if (strcmp (n, "shape") == 0)
    {
      r->in_shape = TRUE;
      g_clear_pointer (&r->style, g_free);
      g_clear_pointer (&r->object_type, g_free);
      g_clear_pointer (&r->link, g_free);
      g_clear_pointer (&r->range, g_free);
      g_clear_pointer (&r->macro, g_free);
      if (r->caption == NULL)
        r->caption = g_string_new (NULL);
      g_string_truncate (r->caption, 0);
      r->value = r->min = r->page = 0;
      r->max = 100;
      r->step = 1;
      r->checked = FALSE;
      r->horizontal = TRUE;
      r->anchor_row = r->anchor_col = 0;
      for (int i = 0; names[i] != NULL; i++)
        if (strcmp (names[i], "style") == 0)
          r->style = g_strdup (values[i]);
      return;
    }
  if (strcmp (n, "textbox") == 0)
    {
      r->in_textbox = TRUE;
      return;
    }
  if (strcmp (n, "ClientData") == 0)
    {
      r->in_client = TRUE;
      for (int i = 0; names[i] != NULL; i++)
        if (strcmp (names[i], "ObjectType") == 0)
          r->object_type = g_strdup (values[i]);
      return;
    }
  if (r->in_client)
    {
      g_clear_pointer (&r->field, g_free);
      r->field = g_strdup (n);
      if (r->text == NULL)
        r->text = g_string_new (NULL);
      g_string_truncate (r->text, 0);
    }
}

static void
vml_text (GMarkupParseContext *ctx, const char *text, gsize length,
          gpointer user, GError **error)
{
  VmlReader *r = user;

  (void) ctx; (void) error;
  if (r->in_client && r->field != NULL && r->text != NULL)
    g_string_append_len (r->text, text, (gssize) length);
  else if (r->in_textbox && r->caption != NULL)
    g_string_append_len (r->caption, text, (gssize) length);
}

static void
vml_end (GMarkupParseContext *ctx, const char *element, gpointer user, GError **error)
{
  VmlReader *r = user;
  const char *n = strchr (element, ':') ? strchr (element, ':') + 1 : element;

  (void) ctx; (void) error;

  if (r->in_client && r->field != NULL && strcmp (n, r->field) == 0 && r->text != NULL)
    {
      const char *v = r->text->str;

      if (strcmp (n, "FmlaLink") == 0)        { g_free (r->link); r->link = g_strdup (v); }
      else if (strcmp (n, "FmlaRange") == 0)  { g_free (r->range); r->range = g_strdup (v); }
      else if (strcmp (n, "FmlaMacro") == 0)  { g_free (r->macro); r->macro = g_strdup (v); }
      else if (strcmp (n, "Val") == 0)        r->value = g_ascii_strtod (v, NULL);
      else if (strcmp (n, "Min") == 0)        r->min = g_ascii_strtod (v, NULL);
      else if (strcmp (n, "Max") == 0)        r->max = g_ascii_strtod (v, NULL);
      else if (strcmp (n, "Inc") == 0)        r->step = g_ascii_strtod (v, NULL);
      else if (strcmp (n, "Page") == 0)       r->page = g_ascii_strtod (v, NULL);
      else if (strcmp (n, "Sel") == 0)        r->value = g_ascii_strtod (v, NULL);
      else if (strcmp (n, "Checked") == 0)    r->checked = g_ascii_strtod (v, NULL) != 0;
      else if (strcmp (n, "Horiz") == 0)      r->horizontal = g_ascii_strtod (v, NULL) != 0;
      else if (strcmp (n, "Anchor") == 0)
        {
          int col = 0, coff = 0, row = 0;

          if (sscanf (v, "%d, %d, %d", &col, &coff, &row) >= 3)
            { r->anchor_col = col; r->anchor_row = row; }
        }
      g_clear_pointer (&r->field, g_free);
      return;
    }

  if (strcmp (n, "textbox") == 0)
    {
      r->in_textbox = FALSE;
      return;
    }

  if (strcmp (n, "ClientData") == 0)
    {
      r->in_client = FALSE;
      return;
    }

  if (strcmp (n, "shape") == 0 && r->in_shape)
    {
      O42ShapeKind kind = vml_kind (r->object_type);

      r->in_shape = FALSE;
      /* A note is a shape here too, and is read from the comments part
       * instead; only the controls are wanted. */
      if (r->object_type != NULL && o42_shape_is_control (kind))
        {
          O42Shape *shape = o42_sheet_add_shape (r->sheet, kind,
                                                 CLAMP (r->anchor_row, 0, O42_MAX_ROWS - 1),
                                                 CLAMP (r->anchor_col, 0, O42_MAX_COLS - 1));

          if (shape != NULL)
            {
              shape->width = vml_style_px (r->style, "width", shape->width);
              shape->height = vml_style_px (r->style, "height", shape->height);
              shape->min = r->min;
              shape->max = r->max;
              shape->step = r->step > 0 ? r->step : 1;
              shape->page = r->page > 0 ? r->page : 10;
              if (r->link != NULL && *r->link != 0)
                {
                  /* $B$3 and Sheet1!$B$3 both come down to B3. */
                  const char *bare = strchr (r->link, '!');
                  GString *plain = g_string_new (NULL);

                  for (const char *q = bare ? bare + 1 : r->link; *q != 0; q++)
                    if (*q != '$')
                      g_string_append_c (plain, *q);
                  g_free (shape->link);
                  shape->link = g_string_free (plain, FALSE);
                }
              if (r->range != NULL && *r->range != 0)
                {
                  const char *bare = strchr (r->range, '!');
                  GString *plain = g_string_new (NULL);

                  for (const char *q = bare ? bare + 1 : r->range; *q != 0; q++)
                    if (*q != '$')
                      g_string_append_c (plain, *q);
                  g_free (shape->source);
                  shape->source = g_string_free (plain, FALSE);
                }
              if (r->caption != NULL && r->caption->len > 0)
                {
                  char *trimmed = g_strstrip (g_strdup (r->caption->str));

                  g_free (shape->text);
                  shape->text = trimmed;
                }
              if (r->macro != NULL && *r->macro != 0)
                {
                  g_free (shape->script);
                  shape->script = g_strdup (r->macro);
                }
              if (kind == O42_SHAPE_OPTION)
                shape->value = 1;
            }
        }
      g_clear_pointer (&r->object_type, g_free);
    }
}

static const GMarkupParser vml_parser = { vml_start, vml_end, vml_text, NULL, NULL };

gboolean
o42_xlsx_save (O42Book *book, GFile *file, GError **error)
{
  Writer w;
  O42ZipWriter *zip = o42_zip_writer_new ();
  int n_sheets = o42_book_n_sheets (book);
  GPtrArray *sheet_xml = g_ptr_array_new_with_free_func (g_free);
  GString *s;
  GString *extra_types = g_string_new (NULL);
  gboolean ok;

  w.fmts = g_array_new (FALSE, FALSE, sizeof (O42Fmt));
  w.strings = g_ptr_array_new_with_free_func (g_free);
  w.string_runs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_array_unref_or_null);
  w.string_idx = g_hash_table_new (g_str_hash, g_str_equal);
  w.dxfs = g_array_new (FALSE, FALSE, sizeof (O42Condition));
  w.fmt_styles = g_array_new (FALSE, FALSE, sizeof (guint));
  w.styles = g_ptr_array_new ();
  w.style_xf = g_array_new (FALSE, FALSE, sizeof (guint));
  w.link_rids = g_hash_table_new_full (key_hash_64, key_equal_64, g_free, NULL);
  {
    /* xf 0 is the default look, whatever any sheet says its default is,
     * and it wears no style. */
    O42Fmt plain;
    guint none = 0;

    o42_fmt_init_default (&plain);
    g_array_append_val (w.fmts, plain);
    g_array_append_val (w.fmt_styles, none);
  }

  /* The book's cell styles: each gets an xf of its own holding the
   * look the style carries, and a name in cellStyles. */
  for (int i = 0; i < o42_book_n_styles (book); i++)
    {
      const char *name = o42_book_style_name (book, i);
      O42Fmt fmt;
      O42FmtMask mask = 0;
      O42Fmt resolved;
      guint x;

      /* Excel's Normal is the absence of a style, and it writes that
       * one itself, so ours is not written again. */
      if (g_ascii_strcasecmp (name, "Normal") == 0)
        continue;
      o42_fmt_init_default (&fmt);
      if (!o42_book_style (book, name, &fmt, &mask))
        continue;
      o42_fmt_init_default (&resolved);
      o42_fmt_apply_mask (&resolved, mask, &fmt);
      x = xf_for (&w, &resolved);
      g_ptr_array_add (w.styles, (gpointer) name);
      g_array_append_val (w.style_xf, x);
    }

  /* Pictures, charts and notes first: their parts add content types
   * and relationships the sheet part refers to. */
  {
    GHashTable *exts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (int i = 0; i < n_sheets; i++)
      {
        O42Sheet *sheet = o42_book_sheet (book, i);
        GString *rels = g_string_new (NULL);
        int next_rid = 1;
        int drawing_rid = o42_xlsx_draw_write (zip, sheet, i + 1, extra_types, exts, rels, &next_rid);
        int legacy_rid = write_comments (zip, sheet, i + 1, extra_types, exts, rels, &next_rid);

        /* External hyperlinks are relationships; ones into the book are not. */
        g_hash_table_remove_all (w.link_rids);
        {
          GHashTableIter lit;
          gpointer lk, lv;
          g_hash_table_iter_init (&lit, o42_sheet_links (sheet));
          while (g_hash_table_iter_next (&lit, &lk, &lv))
            if (((const char *) lv)[0] != '#')
              {
                guint64 *stored = g_new (guint64, 1);
                char *target = g_markup_escape_text (lv, -1);
                *stored = *(guint64 *) lk;
                g_hash_table_insert (w.link_rids, stored, GINT_TO_POINTER (next_rid));
                g_string_append_printf (rels,
                  "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" Target=\"%s\" TargetMode=\"External\"/>",
                  next_rid, target);
                g_free (target);
                next_rid++;
              }
        }

        {
          int table_rid = 0;
          int n_tables = write_tables (zip, sheet, i + 1, extra_types, rels, &next_rid, &table_rid);

          if (o42_sheet_is_chart_sheet (sheet))
            g_ptr_array_add (sheet_xml, write_chart_sheet (sheet, drawing_rid));
          else
            g_ptr_array_add (sheet_xml, write_sheet (&w, sheet, i == 0, drawing_rid, legacy_rid, table_rid, n_tables));
        }
        if (rels->len > 0)
          {
            char *part = g_strdup_printf (o42_sheet_is_chart_sheet (sheet)
                                          ? "xl/chartsheets/_rels/sheet%d.xml.rels"
                                          : "xl/worksheets/_rels/sheet%d.xml.rels", i + 1);
            g_string_prepend (rels,
              "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
              "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
            g_string_append (rels, "</Relationships>");
            o42_zip_writer_add (zip, part, rels->str, rels->len);
            g_free (part);
          }
        g_string_free (rels, TRUE);
      }
    g_hash_table_unref (exts);
  }

  s = g_string_new (
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
    "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
    "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>");
  for (int i = 0; i < n_sheets; i++)
    {
      gboolean cs = o42_sheet_is_chart_sheet (o42_book_sheet (book, i));
      char *part = sheet_part_name (o42_book_sheet (book, i), i + 1);

      g_string_append_printf (s, "<Override PartName=\"/%s\" ContentType=\"%s\"/>", part,
                              cs ? "application/vnd.openxmlformats-officedocument.spreadsheetml.chartsheet+xml"
                                 : "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml");
      g_free (part);
    }
  if (o42_book_n_views (book) > 0)
    {
      /* Custom views in a part of our own, beside the scripts. */
      GString *vx = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<views xmlns=\"http://office42.net/views\">");

      for (int i = 0; i < o42_book_n_views (book); i++)
        {
          const O42BookView *view = o42_book_view (book, i);
          char *vname = g_markup_escape_text (view->name, -1);
          char *vsheet = g_markup_escape_text (view->sheet != NULL ? view->sheet : "", -1);

          g_string_append_printf (vx,
            "<view name=\"%s\" sheet=\"%s\" selection=\"%d %d %d %d\" active=\"%d %d\" "
            "zoom=\"%g\" frozen=\"%d %d\" split=\"%d\"/>",
            vname, vsheet, view->selection.row0, view->selection.col0,
            view->selection.row1, view->selection.col1,
            view->active_row, view->active_col, view->zoom,
            view->frozen_rows, view->frozen_cols, view->split ? 1 : 0);
          g_free (vname);
          g_free (vsheet);
        }
      g_string_append (vx, "</views>");
      o42_zip_writer_add (zip, "xl/o42/views.xml", vx->str, vx->len);
      g_string_free (vx, TRUE);
      g_string_append (extra_types, "<Override PartName=\"/xl/o42/views.xml\" ContentType=\"application/xml\"/>");
    }

  if (o42_book_n_scripts (book) > 0)
    {
      /* Scripts in a part of our own; Excel and LibreOffice pass over
       * parts nothing refers to. */
      GString *sx = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<scripts xmlns=\"http://office42.net/scripts\">");
      for (int i = 0; i < o42_book_n_scripts (book); i++)
        {
          const char *sname = o42_book_script_name (book, i);
          char *ename = g_markup_escape_text (sname, -1);
          char *ecode = g_markup_escape_text (o42_book_script_code (book, sname), -1);
          g_string_append_printf (sx, "<script name=\"%s\" language=\"python\">%s</script>", ename, ecode);
          g_free (ename);
          g_free (ecode);
        }
      g_string_append (sx, "</scripts>");
      o42_zip_writer_add (zip, "xl/o42/scripts.xml", sx->str, sx->len);
      g_string_free (sx, TRUE);
      g_string_append (extra_types, "<Override PartName=\"/xl/o42/scripts.xml\" ContentType=\"application/xml\"/>");
    }
  g_string_append (s, extra_types->str);
  g_string_append (s, "</Types>");
  o42_zip_writer_add (zip, "[Content_Types].xml", s->str, s->len);
  g_string_free (extra_types, TRUE);

  g_string_assign (s,
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
    "</Relationships>");
  o42_zip_writer_add (zip, "_rels/.rels", s->str, s->len);

  g_string_assign (s,
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<workbook xmlns=\"" NS_MAIN "\" xmlns:r=\"" NS_REL "\">"
    "<bookViews><workbookView activeTab=\"0\"/></bookViews><sheets>");
  for (int i = 0; i < n_sheets; i++)
    {
      char *name = g_markup_escape_text (o42_sheet_get_name (o42_book_sheet (book, i)), -1);
      g_string_append_printf (s, "<sheet name=\"%s\" sheetId=\"%d\" r:id=\"rId%d\"/>", name, i + 1, i + 1);
      g_free (name);
    }
  g_string_append (s, "</sheets>");
  {
    GList *names = o42_book_names (book);
    GString *defs = g_string_new (NULL);

    for (GList *l = names; l != NULL; l = l->next)
      {
        O42Sheet *target;
        O42Range range;
        if (o42_book_lookup_name (book, l->data, &target, &range))
          {
            char *ref = name_reference (target, &range);
            char *n = g_markup_escape_text (l->data, -1);
            char *r = g_markup_escape_text (ref, -1);
            g_string_append_printf (defs, "<definedName name=\"%s\">%s</definedName>", n, r);
            g_free (r);
            g_free (n);
            g_free (ref);
          }
      }
    /* Pivot definitions, as hidden sheet-scoped names holding a text:
     * Excel and Calc carry them without complaint, and only office42
     * reads them.  The laid-out values are in the cells. */
    for (int i = 0; i < n_sheets; i++)
      {
        {
          const O42PrintSetup *ps = o42_sheet_print_setup (o42_book_sheet (book, i));
          char *sname = g_markup_escape_text (o42_sheet_get_name (o42_book_sheet (book, i)), -1);
          if (ps->has_area)
            {
              char *a = o42_ref_name_full (ps->area.row0, ps->area.col0, TRUE, TRUE);
              char *b = o42_ref_name_full (ps->area.row1, ps->area.col1, TRUE, TRUE);
              g_string_append_printf (defs, "<definedName name=\"_xlnm.Print_Area\" localSheetId=\"%d\">'%s'!%s:%s</definedName>", i, sname, a, b);
              g_free (a); g_free (b);
            }
          if (ps->title_rows > 0)
            g_string_append_printf (defs, "<definedName name=\"_xlnm.Print_Titles\" localSheetId=\"%d\">'%s'!$1:$%d</definedName>", i, sname, ps->title_rows);
          g_free (sname);
        }

        GArray *pivots = o42_sheet_pivots (o42_book_sheet (book, i));
        for (guint k = 0; k < pivots->len; k++)
          {
            const O42Pivot *p = &g_array_index (pivots, O42Pivot, k);
            char *rf = o42_pivot_fields_to_string (p->row_fields);
            char *cf = o42_pivot_fields_to_string (p->col_fields);
            char *a1 = o42_ref_name (p->source.row0, p->source.col0);
            char *b1 = o42_ref_name (p->source.row1, p->source.col1);
            char *at = o42_ref_name (p->row, p->col);
            char *text = g_strdup_printf ("%s;%s:%s;%s;%s;%s;%d;%s;%d;%d;%s;%s",
                                          p->source_sheet ? p->source_sheet : "", a1, b1, rf, cf,
                                          p->data_field ? p->data_field : "", (int) p->agg, at, p->rows, p->cols,
                                          p->filter_field ? p->filter_field : "", p->filter_value ? p->filter_value : "");
            char *quoted = g_strdup_printf ("\"%s\"", text);
            char *esc = g_markup_escape_text (quoted, -1);
            g_string_append_printf (defs, "<definedName name=\"_o42.pivot.%u\" localSheetId=\"%d\" hidden=\"1\">%s</definedName>",
                                    k + 1, i, esc);
            g_free (esc); g_free (quoted); g_free (text); g_free (at); g_free (b1); g_free (a1); g_free (cf); g_free (rf);
          }
      }
    for (int i = 0; i < n_sheets; i++)
      {
        O42Range filter;
        if (o42_sheet_get_autofilter (o42_book_sheet (book, i), &filter))
          {
            char *ref = name_reference (o42_book_sheet (book, i), &filter);
            char *r = g_markup_escape_text (ref, -1);
            g_string_append_printf (defs, "<definedName name=\"_xlnm._FilterDatabase\" localSheetId=\"%d\" hidden=\"1\">%s</definedName>", i, r);
            g_free (r);
            g_free (ref);
          }
      }
    if (defs->len > 0)
      g_string_append_printf (s, "<definedNames>%s</definedNames>", defs->str);
    g_string_free (defs, TRUE);
    g_list_free (names);
  }
  {
    int max = 100;
    double tolerance = 0.001;
    gboolean iterate = o42_book_iteration (book, &max, &tolerance);

    g_string_append_printf (s, "<calcPr fullCalcOnLoad=\"1\" calcMode=\"%s\" "
                               "iterate=\"%d\" iterateCount=\"%d\" iterateDelta=\"%g\"/></workbook>",
                            o42_book_manual (book) ? "manual" : "auto",
                            iterate ? 1 : 0, max, tolerance);
  }
  o42_zip_writer_add (zip, "xl/workbook.xml", s->str, s->len);

  g_string_assign (s,
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
  for (int i = 0; i < n_sheets; i++)
    {
      gboolean cs = o42_sheet_is_chart_sheet (o42_book_sheet (book, i));

      g_string_append_printf (s,
        "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/%s\" Target=\"%s/sheet%d.xml\"/>",
        i + 1, cs ? "chartsheet" : "worksheet", cs ? "chartsheets" : "worksheets", i + 1);
    }
  g_string_append_printf (s,
    "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
    "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>"
    "</Relationships>", n_sheets + 1, n_sheets + 2);
  o42_zip_writer_add (zip, "xl/_rels/workbook.xml.rels", s->str, s->len);

  for (int i = 0; i < n_sheets; i++)
    {
      char *name = sheet_part_name (o42_book_sheet (book, i), i + 1);
      const char *xml = g_ptr_array_index (sheet_xml, i);
      o42_zip_writer_add (zip, name, xml, strlen (xml));
      g_free (name);
    }

  {
    char *styles = write_styles (&w);
    o42_zip_writer_add (zip, "xl/styles.xml", styles, strlen (styles));
    g_free (styles);
  }

  g_string_assign (s, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
  g_string_append_printf (s, "<sst xmlns=\"" NS_MAIN "\" count=\"%u\" uniqueCount=\"%u\">",
                          w.strings->len, w.strings->len);
  for (guint i = 0; i < w.strings->len; i++)
    {
      const char *text = g_ptr_array_index (w.strings, i);
      GArray *runs = g_ptr_array_index (w.string_runs, i);
      char *escaped = g_markup_escape_text (text, -1);
      gboolean edge_space = text[0] == ' ' || (text[0] != '\0' && text[strlen (text) - 1] == ' ');

      if (runs != NULL && runs->len > 0)
        {
          gsize length = strlen (text);

          g_string_append (s, "<si>");
          for (guint k = 0; k < runs->len; k++)
            {
              const O42TextRun *run = &g_array_index (runs, O42TextRun, k);
              gsize start = CLAMP ((gsize) run->start, 0, length);
              gsize end = (k + 1 < runs->len)
                          ? CLAMP ((gsize) g_array_index (runs, O42TextRun, k + 1).start, 0, length)
                          : length;
              char *part;

              if (end <= start)
                continue;
              part = g_markup_escape_text (text + start, (gssize) (end - start));
              g_string_append (s, "<r><rPr>");
              if (run->fmt.bold) g_string_append (s, "<b/>");
              if (run->fmt.italic) g_string_append (s, "<i/>");
              if (run->fmt.strikeout) g_string_append (s, "<strike/>");
              if (run->fmt.underline) g_string_append (s, "<u/>");
              g_string_append_printf (s, "<sz val=\"%g\"/>", run->fmt.size / 2.0);
              g_string_append (s, "<color rgb=\"");
              append_rgb (s, run->fmt.colour);
              g_string_append_printf (s, "\"/><rFont val=\"%s\"/></rPr>",
                                      run->fmt.family != NULL ? run->fmt.family : "Arial");
              g_string_append_printf (s, "<t xml:space=\"preserve\">%s</t></r>", part);
              g_free (part);
            }
          g_string_append (s, "</si>");
        }
      else
        g_string_append_printf (s, "<si><t%s>%s</t></si>",
                                edge_space ? " xml:space=\"preserve\"" : "", escaped);
      g_free (escaped);
    }
  g_string_append (s, "</sst>");
  o42_zip_writer_add (zip, "xl/sharedStrings.xml", s->str, s->len);
  g_string_free (s, TRUE);

  {
    GBytes *bytes = o42_zip_writer_finish (zip);
    ok = g_file_replace_contents (file, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes),
                                  NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, error);
    g_bytes_unref (bytes);
  }

  g_ptr_array_unref (sheet_xml);
  g_hash_table_unref (w.string_idx);
  g_ptr_array_unref (w.strings);
  g_array_unref (w.fmts);
  g_array_unref (w.dxfs);
  g_array_unref (w.fmt_styles);
  g_array_unref (w.style_xf);
  g_ptr_array_unref (w.styles);
  g_ptr_array_unref (w.string_runs);
  g_hash_table_unref (w.link_rids);
  return ok;
}

/* ====================================================================== */
/* Reading                                                                 */
/* ====================================================================== */

/* Element names may carry a namespace prefix ("x:c"); the local part is
 * what matters. */
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

static double
attr_double (const char **names, const char **values, const char *want, double fallback)
{
  const char *v = attr (names, values, want);
  return v ? g_ascii_strtod (v, NULL) : fallback;
}

static int
attr_int (const char **names, const char **values, const char *want, int fallback)
{
  const char *v = attr (names, values, want);
  return v ? atoi (v) : fallback;
}

static gboolean
attr_flag (const char **names, const char **values, const char *want)
{
  const char *v = attr (names, values, want);
  return v != NULL && (strcmp (v, "1") == 0 || strcmp (v, "true") == 0);
}

/* A general-purpose element collector: every parser below shares the
 * same shape, a stack-less state machine keyed on element names, so one
 * struct holds all their state. */
typedef struct
{
  O42Book    *book;
  int         in_hf;            /* 1 in oddHeader, 2 in oddFooter */
  int         in_breaks;        /* 1 in rowBreaks, 2 in colBreaks */
  GHashTable *sheet_rels;       /* the sheet part's relationships, id -> target */
  char       *scenario_name;    /* the scenario being read */
  char       *scenario_comment;
  gboolean    in_script;        /* xl/o42/scripts.xml */
  char       *script_name;
  GString    *script_code;

  /* Relationships: Id -> Target */
  GHashTable *rels;

  /* Workbook */
  GPtrArray  *sheet_names;
  GPtrArray  *sheet_rids;
  GPtrArray  *names;        /* name, text pairs */
  GString    *name_text;
  char       *name_name;
  gboolean    in_defined_name;

  /* Shared strings */
  GPtrArray  *strings;
  GString    *si;
  gboolean    in_si, in_t, in_run;
  GPtrArray  *string_runs;    /* GArray of O42TextRun beside `strings`, or NULL */
  GArray     *cell_runs;      /* the runs of the cell being read, borrowed */
  GArray     *si_runs;        /* the runs of the string being read */
  O42Fmt      run_fmt;

  /* Styles */
  GArray     *dxfs;         /* O42Condition: fmt and mask of each differential format */
  gboolean    in_dxfs, in_dxf, in_dxf_font, in_dxf_fill, in_dxf_border;
  O42Condition cur_dxf;
  GHashTable *numfmts;      /* id -> code */
  GArray     *fonts;        /* O42Fmt with font fields */
  GArray     *fills;        /* guint32 */
  GArray     *borders;      /* guint8 bit mask: 1 left 2 right 4 top 8 bottom */
  GArray     *bdefs;        /* BorderDef, one per <border> */
  BorderDef   cur_bdef;
  int         bside;
  GArray     *xfs;          /* O42Fmt, resolved */
  GArray     *xf_styles;    /* int, beside `xfs`: the cellStyleXf it wears */
  GArray     *style_xfs;    /* O42Fmt: the cellStyleXfs themselves */
  GHashTable *style_names;  /* xfId -> the style's name */
  gboolean    in_style_xfs;
  GArray     *xf_dates;     /* gboolean */
  gboolean    in_fonts, in_fills, in_borders, in_xfs;
  O42Fmt      cur_font;
  guint32     cur_fill;
  gboolean    cur_fill_solid;
  O42Pattern  cur_pattern;
  guint32     cur_pattern_colour;
  GArray     *patterns;        /* O42Pattern, alongside `fills` */
  GArray     *pattern_colours; /* guint32, alongside `fills` */
  guint8      cur_border;
  const char *cur_side;

  /* Sheet */
  O42Sheet   *sheet;
  int         row, col;
  int         xf;
  char       *type;
  GString    *f, *v, *is;
  gboolean    in_f, in_v, in_is, has_f;
  char       *shared_si;
  char       *array_ref;    /* <f t="array" ref=...>: the block to spread over */
  GHashTable *shared;       /* si -> master formula "row,col,text" */
  int         default_width, default_height;
  char       *drawing_rid;  /* the sheet's <drawing r:id>, if any */
  int         filter_col;   /* the filterColumn being read, or -1 */

  /* Conditional formatting being read */
  O42Range    cf_range;
  gboolean    cf_have_range, in_cf_rule, cf_is_cellis, in_cf_formula;
  int         cf_dxf, cf_n_formulas;
  O42CondOp   cf_op;
  double      cf_values[2];
  GString    *cf_formula;

  /* A dataValidation being read */
  O42Validation dv;
  gboolean    in_dv, in_dv_formula;
  int         dv_formula;
  GString    *dv_text;

  /* Comments being read */
  gboolean    in_comment, in_comment_t;
  int         comment_row, comment_col;
  GString    *comment_text;
} Reader;

/* ---- relationships ---- */

static void
rels_start (GMarkupParseContext *ctx, const char *name, const char **names,
            const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (strcmp (local (name), "Relationship") == 0)
    {
      const char *id = attr (names, values, "Id");
      const char *target = attr (names, values, "Target");
      if (id && target)
        g_hash_table_insert (r->rels, g_strdup (id), g_strdup (target));
    }
}

/* ---- workbook ---- */

static void
workbook_start (GMarkupParseContext *ctx, const char *name, const char **names,
                const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  const char *n = local (name);
  (void) ctx; (void) error;

  if (strcmp (n, "sheet") == 0)
    {
      const char *sname = attr (names, values, "name");
      const char *rid = attr (names, values, "id");
      g_ptr_array_add (r->sheet_names, g_strdup (sname ? sname : "Sheet"));
      g_ptr_array_add (r->sheet_rids, g_strdup (rid ? rid : ""));
    }
  else if (strcmp (n, "definedName") == 0)
    {
      const char *dname = attr (names, values, "name");
      if (dname != NULL && g_str_has_prefix (dname, "_o42.pivot.") && attr (names, values, "localSheetId") != NULL)
        {
          /* A pivot definition: kept with its sheet index for later. */
          r->name_name = g_strdup_printf ("\001%s", attr (names, values, "localSheetId"));
          r->in_defined_name = TRUE;
          g_string_truncate (r->name_text, 0);
        }
      else if (dname != NULL && (strcmp (dname, "_xlnm.Print_Area") == 0 || strcmp (dname, "_xlnm.Print_Titles") == 0) &&
               attr (names, values, "localSheetId") != NULL)
        {
          r->name_name = g_strdup_printf ("\002%s%s", dname[6] == 'P' && dname[12] == 'A' ? "A" : "T", attr (names, values, "localSheetId"));
          r->in_defined_name = TRUE;
          g_string_truncate (r->name_text, 0);
        }
      else if (dname != NULL && !g_str_has_prefix (dname, "_xlnm.") &&
          attr (names, values, "localSheetId") == NULL)
        {
          r->name_name = g_strdup (dname);
          r->in_defined_name = TRUE;
          g_string_truncate (r->name_text, 0);
        }
    }
}

static void
workbook_end (GMarkupParseContext *ctx, const char *name, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (strcmp (local (name), "definedName") == 0 && r->in_defined_name)
    {
      g_ptr_array_add (r->names, r->name_name);
      g_ptr_array_add (r->names, g_strdup (r->name_text->str));
      r->name_name = NULL;
      r->in_defined_name = FALSE;
    }
}

static void
workbook_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (r->in_defined_name)
    g_string_append_len (r->name_text, text, (gssize) len);
}

/* ---- shared strings ---- */

static guint32 rgb_attr (const char **names, const char **values);

static void
sst_start (GMarkupParseContext *ctx, const char *name, const char **names,
           const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  const char *n = local (name);
  (void) ctx; (void) names; (void) values; (void) error;
  if (strcmp (n, "si") == 0)
    {
      r->in_si = TRUE;
      r->in_run = FALSE;
      g_string_truncate (r->si, 0);
      g_clear_pointer (&r->si_runs, g_array_unref);
    }
  else if (strcmp (n, "t") == 0)
    r->in_t = TRUE;
  else if (strcmp (n, "r") == 0 && r->in_si)
    {
      /* A run starts where the text read so far ends. */
      O42TextRun run;

      r->in_run = TRUE;
      o42_fmt_init_default (&r->run_fmt);
      if (r->si_runs == NULL)
        r->si_runs = g_array_new (FALSE, FALSE, sizeof (O42TextRun));
      run.start = (int) r->si->len;
      run.fmt = r->run_fmt;
      g_array_append_val (r->si_runs, run);
    }
  else if (r->in_run && r->si_runs != NULL && r->si_runs->len > 0)
    {
      O42Fmt *fmt = &g_array_index (r->si_runs, O42TextRun, r->si_runs->len - 1).fmt;

      if (strcmp (n, "b") == 0) fmt->bold = 1;
      else if (strcmp (n, "i") == 0) fmt->italic = 1;
      else if (strcmp (n, "strike") == 0) fmt->strikeout = 1;
      else if (strcmp (n, "u") == 0) fmt->underline = 1;
      else if (strcmp (n, "sz") == 0)
        {
          double points = attr_double (names, values, "val", 10);
          fmt->size = (guint8) CLAMP ((int) (points * 2 + 0.5), 2, 800);
        }
      else if (strcmp (n, "color") == 0)
        {
          guint32 colour = rgb_attr (names, values);
          if (colour != 0xFFFFFFFFu) fmt->colour = colour;
        }
      else if (strcmp (n, "rFont") == 0)
        {
          const char *face = attr (names, values, "val");
          if (face != NULL) fmt->family = g_intern_string (face);
        }
    }
}

static void
sst_end (GMarkupParseContext *ctx, const char *name, gpointer user, GError **error)
{
  Reader *r = user;
  const char *n = local (name);
  (void) ctx; (void) error;
  if (strcmp (n, "si") == 0)
    {
      g_ptr_array_add (r->strings, g_strdup (r->si->str));
      g_ptr_array_add (r->string_runs, r->si_runs);   /* may be NULL */
      r->si_runs = NULL;
      r->in_si = FALSE;
    }
  else if (strcmp (n, "t") == 0)
    r->in_t = FALSE;
  else if (strcmp (n, "r") == 0)
    r->in_run = FALSE;
}

static void
sst_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (r->in_si && r->in_t)
    g_string_append_len (r->si, text, (gssize) len);
}

/* ---- styles ---- */

const char *
o42_xlsx_builtin_number_format (int id)
{
  switch (id)
    {
    case 1: return "0";
    case 2: return "0.00";
    case 3: return "#,##0";
    case 4: return "#,##0.00";
    case 9: return "0%";
    case 10: return "0.00%";
    case 11: return "0.00E+00";
    case 12: return "# ?/?";
    case 13: return "# ?\?/??";
    case 14: return "yyyy-mm-dd";
    case 15: return "d-mmm-yy";
    case 16: return "d-mmm";
    case 17: return "mmm-yy";
    case 18: return "h:mm AM/PM";
    case 19: return "h:mm:ss AM/PM";
    case 20: return "h:mm";
    case 21: return "hh:mm:ss";
    case 22: return "yyyy-mm-dd hh:mm:ss";
    case 37: return "#,##0 ;(#,##0)";
    case 38: return "#,##0 ;[Red](#,##0)";
    case 39: return "#,##0.00;(#,##0.00)";
    case 40: return "#,##0.00;[Red](#,##0.00)";
    case 45: return "mm:ss";
    case 46: return "[h]:mm:ss";
    case 47: return "mm:ss.0";
    case 48: return "##0.0E+0";
    case 49: return "@";
    default: return NULL;
    }
}

/* Whether a format code shows a date or time: it has day, month, year
 * or hour letters outside of quotes and brackets. */
static gboolean
code_is_date (const char *code)
{
  gboolean quoted = FALSE, bracket = FALSE;
  for (const char *p = code; *p; p++)
    {
      if (*p == '"') quoted = !quoted;
      else if (quoted) continue;
      else if (*p == '[') bracket = TRUE;
      else if (*p == ']') bracket = FALSE;
      else if (bracket) continue;
      else if (*p == '\\') { if (p[1]) p++; }
      else if (strchr ("ymdhs", g_ascii_tolower (*p)))
        return TRUE;
    }
  return FALSE;
}

gboolean
o42_xlsx_apply_format_code (O42Fmt *fmt, const char *code)
{
  O42NumberFormat preset;
  int decimals;
  gboolean is_date;

  if (strcmp (code, "General") == 0)
    return FALSE;
  is_date = code_is_date (code);
  /* A code with a bracketed section or more than one part says more
   * than a preset can -- a currency and its locale, a colour for the
   * negatives, a condition -- so it is kept as it was written and the
   * formatter reads it whole.  Dates keep their preset, which is what
   * the rest of the program tests for. */
  if (!is_date && (strchr (code, '[') != NULL || strchr (code, ';') != NULL))
    fmt->custom = g_intern_string (code);
  else if (o42_number_format_parse (code, &preset, &decimals))
    { fmt->number = preset; fmt->decimals = decimals; }
  else if (is_date)
    {
      gboolean day = strchr (code, 'y') || strchr (code, 'd') || strchr (code, 'Y') || strchr (code, 'D');
      gboolean clock = strchr (code, 'h') || strchr (code, 's') || strchr (code, 'H') || strchr (code, 'S');
      fmt->number = day && clock ? O42_NUM_DATETIME : day ? O42_NUM_DATE : O42_NUM_TIME;
    }
  else
    fmt->custom = g_intern_string (code);
  return is_date;
}

static O42BorderStyle
xlsx_border_style (const char *name)
{
  if (strcmp (name, "medium") == 0 || strcmp (name, "mediumDashed") == 0 || strcmp (name, "mediumDashDot") == 0 ||
      strcmp (name, "mediumDashDotDot") == 0 || strcmp (name, "slantDashDot") == 0) return O42_BORDER_MEDIUM;
  if (strcmp (name, "thick") == 0) return O42_BORDER_THICK;
  if (strcmp (name, "double") == 0) return O42_BORDER_DOUBLE;
  if (strcmp (name, "dashed") == 0 || strcmp (name, "dashDot") == 0 || strcmp (name, "dashDotDot") == 0) return O42_BORDER_DASHED;
  if (strcmp (name, "dotted") == 0 || strcmp (name, "hair") == 0) return O42_BORDER_DOTTED;
  return O42_BORDER_THIN;
}

static guint32
rgb_attr (const char **names, const char **values)
{
  const char *rgb = attr (names, values, "rgb");
  if (rgb != NULL && strlen (rgb) >= 6)
    return (guint32) g_ascii_strtoull (rgb + strlen (rgb) - 6, NULL, 16);
  return 0xFFFFFFFFu;
}

static void
styles_start (GMarkupParseContext *ctx, const char *name, const char **names,
              const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  const char *n = local (name);
  (void) ctx; (void) error;

  if (strcmp (n, "numFmt") == 0)
    {
      const char *code = attr (names, values, "formatCode");
      int id = attr_int (names, values, "numFmtId", -1);
      if (code && id >= 0)
        g_hash_table_insert (r->numfmts, GINT_TO_POINTER (id), g_strdup (code));
    }
  else if (strcmp (n, "dxfs") == 0) r->in_dxfs = TRUE;
  else if (r->in_dxfs)
    {
      O42Condition *c = &r->cur_dxf;
      if (strcmp (n, "dxf") == 0)
        {
          memset (c, 0, sizeof *c);
          o42_fmt_init_default (&c->fmt);
          r->in_dxf = TRUE;
        }
      else if (!r->in_dxf) {}
      else if (strcmp (n, "font") == 0) r->in_dxf_font = TRUE;
      else if (strcmp (n, "fill") == 0) r->in_dxf_fill = TRUE;
      else if (strcmp (n, "border") == 0) r->in_dxf_border = TRUE;
      else if (r->in_dxf_font)
        {
          if (strcmp (n, "b") == 0) { c->fmt.bold = !attr (names, values, "val") || attr_flag (names, values, "val"); c->mask |= O42_FMT_BOLD; }
          else if (strcmp (n, "i") == 0) { c->fmt.italic = !attr (names, values, "val") || attr_flag (names, values, "val"); c->mask |= O42_FMT_ITALIC; }
          else if (strcmp (n, "u") == 0) { c->fmt.underline = TRUE; c->mask |= O42_FMT_UNDERLINE; }
          else if (strcmp (n, "strike") == 0) { c->fmt.strikeout = TRUE; c->mask |= O42_FMT_STRIKEOUT; }
          else if (strcmp (n, "color") == 0)
            {
              guint32 colour = rgb_attr (names, values);
              if (colour != 0xFFFFFFFFu) { c->fmt.colour = colour; c->mask |= O42_FMT_COLOUR; }
            }
        }
      else if (r->in_dxf_fill && (strcmp (n, "bgColor") == 0 || strcmp (n, "fgColor") == 0))
        {
          guint32 colour = rgb_attr (names, values);
          if (colour != 0xFFFFFFFFu && !(c->mask & O42_FMT_FILL && strcmp (n, "fgColor") == 0))
            { c->fmt.fill = colour; c->mask |= O42_FMT_FILL; }
        }
      else if (r->in_dxf_border && attr (names, values, "style") != NULL && strcmp (attr (names, values, "style"), "none") != 0)
        {
          c->mask |= O42_FMT_BORDERS;
          if (strcmp (n, "left") == 0) c->fmt.border_left = TRUE;
          else if (strcmp (n, "right") == 0) c->fmt.border_right = TRUE;
          else if (strcmp (n, "top") == 0) c->fmt.border_top = TRUE;
          else if (strcmp (n, "bottom") == 0) c->fmt.border_bottom = TRUE;
        }
    }
  else if (strcmp (n, "fonts") == 0) r->in_fonts = TRUE;
  else if (strcmp (n, "fills") == 0) r->in_fills = TRUE;
  else if (strcmp (n, "borders") == 0) r->in_borders = TRUE;
  else if (strcmp (n, "cellXfs") == 0) r->in_xfs = TRUE;
  else if (strcmp (n, "cellStyleXfs") == 0) { r->in_xfs = TRUE; r->in_style_xfs = TRUE; }
  else if (strcmp (n, "cellStyle") == 0)
    {
      /* The name a style goes by, against the cellStyleXf it uses. */
      const char *style = attr (names, values, "name");
      int xf_id = attr_int (names, values, "xfId", -1);

      if (style != NULL && xf_id > 0)
        g_hash_table_insert (r->style_names, GINT_TO_POINTER (xf_id), g_strdup (style));
    }
  else if (r->in_fonts)
    {
      if (strcmp (n, "font") == 0)
        o42_fmt_init_default (&r->cur_font);
      else if (strcmp (n, "b") == 0) r->cur_font.bold = !attr (names, values, "val") || attr_flag (names, values, "val");
      else if (strcmp (n, "i") == 0) r->cur_font.italic = !attr (names, values, "val") || attr_flag (names, values, "val");
      else if (strcmp (n, "u") == 0) r->cur_font.underline = TRUE;
      else if (strcmp (n, "strike") == 0) r->cur_font.strikeout = TRUE;
      else if (strcmp (n, "sz") == 0) r->cur_font.size = (int) (attr_double (names, values, "val", 10) * 2 + 0.5);
      else if (strcmp (n, "name") == 0)
        {
          const char *val = attr (names, values, "val");
          if (val) r->cur_font.family = g_intern_string (val);
        }
      else if (strcmp (n, "color") == 0)
        {
          guint32 c = rgb_attr (names, values);
          if (c != 0xFFFFFFFFu) r->cur_font.colour = c;
        }
    }
  else if (r->in_fills)
    {
      if (strcmp (n, "fill") == 0)
        {
          r->cur_fill = O42_FILL_NONE;
          r->cur_fill_solid = FALSE;
          r->cur_pattern = O42_PATTERN_NONE;
          r->cur_pattern_colour = 0;
        }
      else if (strcmp (n, "patternFill") == 0)
        {
          const char *type = attr (names, values, "patternType");
          O42Pattern pattern = O42_PATTERN_NONE;

          if (type != NULL)
            o42_pattern_parse (type, &pattern);
          /* "solid" is a plain shading in the foreground colour; any
           * other pattern keeps its own colour over the background. */
          r->cur_fill_solid = pattern == O42_PATTERN_SOLID;
          r->cur_pattern = pattern > O42_PATTERN_SOLID ? pattern : O42_PATTERN_NONE;
        }
      else if (strcmp (n, "fgColor") == 0 && (r->cur_fill_solid || r->cur_pattern != O42_PATTERN_NONE))
        {
          guint32 c = rgb_attr (names, values);
          if (c != 0xFFFFFFFFu)
            {
              if (r->cur_fill_solid) r->cur_fill = c;
              else r->cur_pattern_colour = c;
            }
        }
      else if (strcmp (n, "bgColor") == 0 && r->cur_pattern != O42_PATTERN_NONE)
        {
          guint32 c = rgb_attr (names, values);
          if (c != 0xFFFFFFFFu) r->cur_fill = c;
        }
    }
  else if (r->in_borders)
    {
      if (strcmp (n, "border") == 0)
        {
          r->cur_border = 0;
          memset (&r->cur_bdef, 0, sizeof r->cur_bdef);
          r->bside = -1;
        }
      else if (strcmp (n, "left") == 0 || strcmp (n, "right") == 0 || strcmp (n, "top") == 0 || strcmp (n, "bottom") == 0)
        {
          const char *style = attr (names, values, "style");
          int side = n[0] == 't' ? O42_SIDE_TOP : n[0] == 'b' ? O42_SIDE_BOTTOM : n[0] == 'l' ? O42_SIDE_LEFT : O42_SIDE_RIGHT;
          r->bside = side;
          if (style != NULL && strcmp (style, "none") != 0)
            {
              r->cur_border |= n[0] == 'l' ? 1 : n[0] == 'r' ? 2 : n[0] == 't' ? 4 : 8;
              r->cur_bdef.style[side] = xlsx_border_style (style);
            }
        }
      else if (strcmp (n, "color") == 0 && r->bside >= 0)
        {
          guint32 rgb = rgb_attr (names, values);
          if (rgb != 0xFFFFFFFFu)
            r->cur_bdef.colour[r->bside] = rgb;
        }
    }
  else if (r->in_xfs)
    {
      if (strcmp (n, "xf") == 0)
        {
          O42Fmt fmt;
          int font = attr_int (names, values, "fontId", 0);
          int fill = attr_int (names, values, "fillId", 0);
          int border = attr_int (names, values, "borderId", 0);
          int numfmt = attr_int (names, values, "numFmtId", 0);
          const char *code = g_hash_table_lookup (r->numfmts, GINT_TO_POINTER (numfmt));
          gboolean is_date = FALSE;

          if (font >= 0 && (guint) font < r->fonts->len)
            fmt = g_array_index (r->fonts, O42Fmt, font);
          else
            o42_fmt_init_default (&fmt);
          if (fill >= 0 && (guint) fill < r->fills->len)
            {
              fmt.fill = g_array_index (r->fills, guint32, fill);
              fmt.pattern = (guint8) g_array_index (r->patterns, O42Pattern, fill);
              fmt.pattern_colour = g_array_index (r->pattern_colours, guint32, fill);
            }
          if (border >= 0 && (guint) border < r->bdefs->len)
            {
              const BorderDef *bd = &g_array_index (r->bdefs, BorderDef, border);
              for (int i = 0; i < 4; i++)
                {
                  fmt.border_style[i] = bd->style[i];
                  fmt.border_colour[i] = bd->colour[i];
                }
              o42_fmt_sync_borders (&fmt);
            }
          if (code == NULL)
            code = o42_xlsx_builtin_number_format (numfmt);
          if (code != NULL)
            is_date = o42_xlsx_apply_format_code (&fmt, code);
          if (r->in_style_xfs)
            g_array_append_val (r->style_xfs, fmt);
          else
            {
              int wears = attr_int (names, values, "xfId", 0);

              g_array_append_val (r->xfs, fmt);
              g_array_append_val (r->xf_dates, is_date);
              g_array_append_val (r->xf_styles, wears);
            }
        }
      else if (strcmp (n, "protection") == 0 && !r->in_style_xfs && r->xfs->len > 0)
        {
          O42Fmt *fmt = &g_array_index (r->xfs, O42Fmt, r->xfs->len - 1);
          fmt->locked = attr_int (names, values, "locked", 1) != 0;
          fmt->hidden = attr_int (names, values, "hidden", 0) != 0;
        }
      else if (strcmp (n, "alignment") == 0 && !r->in_style_xfs && r->xfs->len > 0)
        {
          O42Fmt *fmt = &g_array_index (r->xfs, O42Fmt, r->xfs->len - 1);
          const char *h = attr (names, values, "horizontal");
          const char *v = attr (names, values, "vertical");
          if (h)
            {
              if (strcmp (h, "left") == 0) fmt->halign = O42_HALIGN_LEFT;
              else if (strcmp (h, "center") == 0 || strcmp (h, "centerContinuous") == 0) fmt->halign = O42_HALIGN_CENTRE;
              else if (strcmp (h, "right") == 0) fmt->halign = O42_HALIGN_RIGHT;
            }
          if (v)
            {
              if (strcmp (v, "top") == 0) fmt->valign = O42_VALIGN_TOP;
              else if (strcmp (v, "center") == 0) fmt->valign = O42_VALIGN_MIDDLE;
            }
          fmt->wrap = attr_flag (names, values, "wrapText");
          fmt->indent = (guint8) CLAMP (attr_int (names, values, "indent", 0), 0, 15);
          {
            int rot = attr_int (names, values, "textRotation", 0);
            fmt->rotation = rot <= 90 ? (gint16) rot : rot <= 180 ? (gint16) (90 - rot) : 0;
          }
        }
    }
}

static void
styles_end (GMarkupParseContext *ctx, const char *name, gpointer user, GError **error)
{
  Reader *r = user;
  const char *n = local (name);
  (void) ctx; (void) error;

  if (strcmp (n, "dxfs") == 0) r->in_dxfs = FALSE;
  else if (r->in_dxfs)
    {
      if (strcmp (n, "dxf") == 0) { g_array_append_val (r->dxfs, r->cur_dxf); r->in_dxf = FALSE; }
      else if (strcmp (n, "font") == 0) r->in_dxf_font = FALSE;
      else if (strcmp (n, "fill") == 0) r->in_dxf_fill = FALSE;
      else if (strcmp (n, "border") == 0) r->in_dxf_border = FALSE;
    }
  else if (strcmp (n, "fonts") == 0) r->in_fonts = FALSE;
  else if (strcmp (n, "fills") == 0) r->in_fills = FALSE;
  else if (strcmp (n, "borders") == 0) r->in_borders = FALSE;
  else if (strcmp (n, "cellXfs") == 0) r->in_xfs = FALSE;
  else if (strcmp (n, "cellStyleXfs") == 0) { r->in_xfs = FALSE; r->in_style_xfs = FALSE; }
  else if (r->in_fonts && strcmp (n, "font") == 0)
    g_array_append_val (r->fonts, r->cur_font);
  else if (r->in_fills && strcmp (n, "fill") == 0)
    {
      g_array_append_val (r->fills, r->cur_fill);
      g_array_append_val (r->patterns, r->cur_pattern);
      g_array_append_val (r->pattern_colours, r->cur_pattern_colour);
    }
  else if (r->in_borders && strcmp (n, "border") == 0)
    {
      g_array_append_val (r->borders, r->cur_border);
      g_array_append_val (r->bdefs, r->cur_bdef);
    }
}

/* ---- our own custom views part ---- */

static void
views_start (GMarkupParseContext *ctx, const char *name, const char **names,
             const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  const char *n = local (name);
  (void) ctx; (void) error;

  if (strcmp (n, "view") == 0)
    {
      O42BookView view;
      const char *text;

      memset (&view, 0, sizeof view);
      view.name = (char *) attr (names, values, "name");
      view.sheet = (char *) attr (names, values, "sheet");
      text = attr (names, values, "selection");
      if (text != NULL)
        sscanf (text, "%d %d %d %d", &view.selection.row0, &view.selection.col0,
                &view.selection.row1, &view.selection.col1);
      text = attr (names, values, "active");
      if (text != NULL)
        sscanf (text, "%d %d", &view.active_row, &view.active_col);
      text = attr (names, values, "zoom");
      view.zoom = text != NULL ? g_ascii_strtod (text, NULL) : 1.0;
      text = attr (names, values, "frozen");
      if (text != NULL)
        sscanf (text, "%d %d", &view.frozen_rows, &view.frozen_cols);
      view.split = attr_int (names, values, "split", 0) != 0;
      if (view.name != NULL)
        o42_book_set_view (r->book, &view);
    }
}

/* ---- a worksheet ---- */

/* Excel prefixes functions newer than 2007 with _xlfn. in the file. */
static char *
strip_xlfn (const char *formula)
{
  GString *out = g_string_new (NULL);
  for (const char *p = formula; *p; )
    {
      if (g_str_has_prefix (p, "_xlfn."))
        p += 6;
      else
        g_string_append_c (out, *p++);
    }
  return g_string_free (out, FALSE);
}

static void
sheet_start (GMarkupParseContext *ctx, const char *name, const char **names,
             const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  const char *n = local (name);
  (void) ctx; (void) error;

  if (strcmp (n, "tabColor") == 0 && r->sheet != NULL)
    {
      guint32 colour = rgb_attr (names, values);

      if (colour != 0xFFFFFFFFu)
        o42_sheet_set_tab_colour (r->sheet, colour);
      return;
    }

  if (strcmp (n, "chartsheet") == 0)
    o42_sheet_set_chart_sheet (r->sheet, TRUE);   /* one chart, no cells */
  else if (strcmp (n, "sheetFormatPr") == 0)
    {
      double w = attr_double (names, values, "defaultColWidth", -1);
      double h = attr_double (names, values, "defaultRowHeight", -1);
      if (attr_flag (names, values, "customHeight") && h > 0)
        r->default_height = PT_TO_PX (h);
      if (w > 0)
        {
          /* The file's default column width, for every column; <cols>
           * that follow override it where they say so. */
          r->default_width = CHARS_TO_PX (w);
          for (int c = 0; c < O42_MAX_COLS; c++)
            o42_sheet_set_col_width (r->sheet, c, r->default_width);
        }
    }
  else if (strcmp (n, "brk") == 0 && r->in_breaks)
    o42_sheet_toggle_page_break (r->sheet, r->in_breaks == 1, attr_int (names, values, "id", 0));
  else if (strcmp (n, "rowBreaks") == 0)
    r->in_breaks = 1;
  else if (strcmp (n, "colBreaks") == 0)
    r->in_breaks = 2;
  else if (strcmp (n, "pageMargins") == 0)
    {
      const char *left = attr (names, values, "left");
      if (left != NULL)
        o42_sheet_set_print_margin (r->sheet, g_ascii_strtod (left, NULL) * 72.0);
    }
  else if (strcmp (n, "pageSetup") == 0)
    o42_sheet_set_print_scale (r->sheet, attr_int (names, values, "scale", 100),
                               attr_int (names, values, "fitToWidth", 0),
                               attr_int (names, values, "fitToHeight", 0));
  else if (strcmp (n, "printOptions") == 0)
    {
      const O42PrintSetup *ps = o42_sheet_print_setup (r->sheet);
      o42_sheet_set_print_options (r->sheet, attr_int (names, values, "gridLines", 0) != 0,
                                   attr_int (names, values, "headings", 0) != 0, ps->title_rows);
    }
  else if (strcmp (n, "headerFooter") == 0)
    o42_sheet_set_header_footer (r->sheet, "", "");
  else if (strcmp (n, "oddHeader") == 0 || strcmp (n, "oddFooter") == 0)
    {
      r->in_hf = n[3] == 'H' ? 1 : 2;
      g_string_truncate (r->f, 0);
    }
  else if (strcmp (n, "pane") == 0)
    {
      const char *state = attr (names, values, "state");
      if (state && strcmp (state, "frozen") == 0)
        o42_sheet_set_frozen (r->sheet, attr_int (names, values, "ySplit", 0),
                              attr_int (names, values, "xSplit", 0));
    }
  else if (strcmp (n, "col") == 0)
    {
      int min = attr_int (names, values, "min", 1) - 1;
      int max = attr_int (names, values, "max", 1) - 1;
      double width = attr_double (names, values, "width", -1);
      gboolean hidden = attr_flag (names, values, "hidden");
      gboolean custom = attr_flag (names, values, "customWidth") || width > 0;
      int level = attr_int (names, values, "outlineLevel", 0);
      if (max >= O42_MAX_COLS) max = O42_MAX_COLS - 1;
      for (int c = MAX (min, 0); c <= max; c++)
        {
          if (custom && width > 0)
            o42_sheet_set_col_width (r->sheet, c, CHARS_TO_PX (width));
          if (hidden)
            o42_sheet_set_col_hidden (r->sheet, c, TRUE);
          if (level > 0)
            o42_sheet_set_col_level (r->sheet, c, level);
        }
    }
  else if (strcmp (n, "row") == 0)
    {
      int row = attr_int (names, values, "r", r->row + 2) - 1;
      double ht = attr_double (names, values, "ht", -1);
      r->row = row;
      r->col = -1;
      if (row >= 0 && row < O42_MAX_ROWS)
        {
          if (ht > 0 && attr_flag (names, values, "customHeight"))
            o42_sheet_set_row_height (r->sheet, row, PT_TO_PX (ht));
          if (attr_flag (names, values, "hidden"))
            o42_sheet_set_row_hidden (r->sheet, row, TRUE);
          if (attr_int (names, values, "outlineLevel", 0) > 0)
            o42_sheet_set_row_level (r->sheet, row, attr_int (names, values, "outlineLevel", 0));
        }
    }
  else if (strcmp (n, "c") == 0)
    {
      const char *ref = attr (names, values, "r");
      const char *type = attr (names, values, "t");
      int row, col;
      if (ref != NULL && o42_ref_parse (ref, &row, &col, NULL))
        { r->row = row; r->col = col; }
      else
        r->col++;
      r->xf = attr_int (names, values, "s", 0);
      g_free (r->type);
      r->type = g_strdup (type ? type : "n");
      g_string_truncate (r->f, 0);
      g_string_truncate (r->v, 0);
      g_string_truncate (r->is, 0);
      r->has_f = FALSE;
      g_clear_pointer (&r->shared_si, g_free);
      g_clear_pointer (&r->array_ref, g_free);
    }
  else if (strcmp (n, "f") == 0)
    {
      const char *t = attr (names, values, "t");
      const char *si = attr (names, values, "si");
      r->in_f = TRUE;
      r->has_f = TRUE;
      if (t != NULL && strcmp (t, "shared") == 0 && si != NULL)
        r->shared_si = g_strdup (si);
      if (t != NULL && strcmp (t, "array") == 0 && attr (names, values, "ref") != NULL)
        r->array_ref = g_strdup (attr (names, values, "ref"));
    }
  else if (strcmp (n, "v") == 0)
    r->in_v = TRUE;
  else if (strcmp (n, "is") == 0)
    r->in_is = TRUE;
  else if (strcmp (n, "t") == 0 && r->in_is)
    r->in_t = TRUE;
  else if (strcmp (n, "hyperlink") == 0)
    {
      const char *ref = attr (names, values, "ref");
      const char *rid = attr (names, values, "id");
      const char *location = attr (names, values, "location");
      int lrow, lcol;
      if (ref != NULL && o42_ref_parse (ref, &lrow, &lcol, NULL))
        {
          const char *target = rid != NULL && r->sheet_rels != NULL ? g_hash_table_lookup (r->sheet_rels, rid) : NULL;
          if (target != NULL)
            o42_sheet_set_link (r->sheet, lrow, lcol, target);
          else if (location != NULL)
            {
              char *internal = g_strconcat ("#", location, NULL);
              o42_sheet_set_link (r->sheet, lrow, lcol, internal);
              g_free (internal);
            }
        }
    }
  else if (strcmp (n, "mergeCell") == 0)
    {
      const char *ref = attr (names, values, "ref");
      O42Range m;
      gsize used;
      if (ref != NULL && o42_ref_parse (ref, &m.row0, &m.col0, &used) && ref[used] == ':' &&
          o42_ref_parse (ref + used + 1, &m.row1, &m.col1, NULL))
        {
          m = o42_range_normalise (m.row0, m.col0, m.row1, m.col1);
          o42_sheet_merge (r->sheet, &m);
        }
    }
  else if (strcmp (n, "drawing") == 0)
    {
      const char *id = attr (names, values, "id");
      g_free (r->drawing_rid);
      r->drawing_rid = g_strdup (id);
    }
  else if (strcmp (n, "conditionalFormatting") == 0)
    {
      const char *sqref = attr (names, values, "sqref");
      gsize used;
      r->cf_have_range = FALSE;
      if (sqref != NULL && o42_ref_parse (sqref, &r->cf_range.row0, &r->cf_range.col0, &used))
        {
          if (sqref[used] == ':')
            r->cf_have_range = o42_ref_parse (sqref + used + 1, &r->cf_range.row1, &r->cf_range.col1, NULL);
          else
            {
              r->cf_range.row1 = r->cf_range.row0;
              r->cf_range.col1 = r->cf_range.col0;
              r->cf_have_range = TRUE;
            }
          if (r->cf_have_range)
            r->cf_range = o42_range_normalise (r->cf_range.row0, r->cf_range.col0, r->cf_range.row1, r->cf_range.col1);
        }
    }
  else if (strcmp (n, "dataValidation") == 0)
    {
      static const char *types[] = { "none", "whole", "decimal", "list", "date", "time", "textLength" };
      static const char *ops[] = { "between", "notBetween", "equal", "notEqual",
                                   "greaterThan", "lessThan", "greaterThanOrEqual", "lessThanOrEqual" };
      const char *type = attr (names, values, "type");
      const char *op = attr (names, values, "operator");
      const char *sqref = attr (names, values, "sqref");
      const char *err_text = attr (names, values, "error");
      gsize used;

      memset (&r->dv, 0, sizeof r->dv);
      r->in_dv = FALSE;
      for (guint i = 0; type != NULL && i < G_N_ELEMENTS (types); i++)
        if (strcmp (type, types[i]) == 0) r->dv.kind = (O42ValidKind) i;
      r->dv.op = O42_COND_BETWEEN;
      for (guint i = 0; op != NULL && i < G_N_ELEMENTS (ops); i++)
        if (strcmp (op, ops[i]) == 0) r->dv.op = (O42CondOp) i;
      r->dv.allow_blank = attr_flag (names, values, "allowBlank");
      r->dv.message = g_strdup (err_text ? err_text : "");
      r->dv.value = g_strdup ("");
      r->dv.value2 = g_strdup ("");
      if (sqref != NULL && o42_ref_parse (sqref, &r->dv.range.row0, &r->dv.range.col0, &used))
        {
          if (sqref[used] == ':')
            r->in_dv = o42_ref_parse (sqref + used + 1, &r->dv.range.row1, &r->dv.range.col1, NULL);
          else
            {
              r->dv.range.row1 = r->dv.range.row0;
              r->dv.range.col1 = r->dv.range.col0;
              r->in_dv = TRUE;
            }
          r->dv.range = o42_range_normalise (r->dv.range.row0, r->dv.range.col0, r->dv.range.row1, r->dv.range.col1);
        }
      r->in_dv = r->in_dv && r->dv.kind != O42_VALID_ANY;
    }
  else if ((strcmp (n, "formula1") == 0 || strcmp (n, "formula2") == 0) && r->in_dv)
    {
      r->in_dv_formula = TRUE;
      r->dv_formula = n[7] - '1';
      g_string_truncate (r->dv_text, 0);
    }
  else if (strcmp (n, "cfRule") == 0)
    {
      const char *type = attr (names, values, "type");
      const char *op = attr (names, values, "operator");
      static const char *ops[] = { "between", "notBetween", "equal", "notEqual",
                                   "greaterThan", "lessThan", "greaterThanOrEqual", "lessThanOrEqual" };
      r->in_cf_rule = TRUE;
      r->cf_is_cellis = type != NULL && strcmp (type, "cellIs") == 0 && op != NULL;
      r->cf_dxf = attr_int (names, values, "dxfId", -1);
      r->cf_n_formulas = 0;
      r->cf_op = O42_COND_EQUAL;
      for (guint i = 0; op != NULL && i < G_N_ELEMENTS (ops); i++)
        if (strcmp (op, ops[i]) == 0)
          r->cf_op = (O42CondOp) i;
    }
  else if (strcmp (n, "formula") == 0 && r->in_cf_rule)
    {
      r->in_cf_formula = TRUE;
      g_string_truncate (r->cf_formula, 0);
    }
  else if (strcmp (n, "sheetProtection") == 0)
    o42_sheet_set_protected (r->sheet, attr_int (names, values, "sheet", 1) != 0);
  else if (strcmp (n, "scenario") == 0)
    {
      g_free (r->scenario_name);
      r->scenario_name = g_strdup (attr (names, values, "name"));
      g_free (r->scenario_comment);
      r->scenario_comment = g_strdup (attr (names, values, "comment"));
      if (r->scenario_name != NULL)
        o42_sheet_define_scenario (r->sheet, r->scenario_name, r->scenario_comment, -1, -1, NULL);
    }
  else if (strcmp (n, "inputCells") == 0 && r->scenario_name != NULL)
    {
      const char *ref = attr (names, values, "r");
      const char *val = attr (names, values, "val");
      int srow, scol;
      if (ref != NULL && o42_ref_parse (ref, &srow, &scol, NULL))
        o42_sheet_define_scenario (r->sheet, r->scenario_name, r->scenario_comment, srow, scol,
                                   val != NULL ? val : "");
    }
  else if (strcmp (n, "autoFilter") == 0)
    {
      const char *ref = attr (names, values, "ref");
      O42Range f;
      gsize used;
      if (ref != NULL && o42_ref_parse (ref, &f.row0, &f.col0, &used) && ref[used] == ':' &&
          o42_ref_parse (ref + used + 1, &f.row1, &f.col1, NULL))
        {
          f = o42_range_normalise (f.row0, f.col0, f.row1, f.col1);
          o42_sheet_set_autofilter (r->sheet, &f);
        }
    }
  else if (strcmp (n, "filterColumn") == 0)
    r->filter_col = attr_int (names, values, "colId", -1);
  else if ((strcmp (n, "filter") == 0 || strcmp (n, "customFilter") == 0) && r->filter_col >= 0)
    {
      const char *val = attr (names, values, "val");
      const char *op = attr (names, values, "operator");
      static const char *ops[] = { "equal", "notEqual", "greaterThan", "greaterThanOrEqual", "lessThan", "lessThanOrEqual" };
      static const char *prefixes[] = { "", "<>", ">", ">=", "<", "<=" };
      const char *prefix = "";
      O42Range f;
      for (guint k = 0; op != NULL && k < G_N_ELEMENTS (ops); k++)
        if (strcmp (op, ops[k]) == 0) prefix = prefixes[k];
      /* Only the first criterion of a column is kept. */
      if (val != NULL && o42_sheet_get_autofilter (r->sheet, &f) &&
          o42_sheet_autofilter_choice (r->sheet, f.col0 + r->filter_col) == NULL)
        {
          char *criterion = g_strconcat (prefix, val, NULL);
          o42_sheet_autofilter_choose (r->sheet, f.col0 + r->filter_col, criterion);
          g_free (criterion);
        }
    }
}

static void
finish_cell (Reader *r)
{
  char *input = NULL;

  if (r->row < 0 || r->row >= O42_MAX_ROWS || r->col < 0 || r->col >= O42_MAX_COLS)
    return;

  if (r->has_f && r->f->len > 0)
    {
      char *plain = strip_xlfn (r->f->str);
      input = g_strconcat ("=", plain, NULL);
      g_free (plain);
      if (r->shared_si != NULL)
        {
          /* The master of a shared formula: remember where it sits. */
          char *rec = g_strdup_printf ("%d,%d,%s", r->row, r->col, plain);
          g_hash_table_insert (r->shared, g_strdup (r->shared_si), rec);
        }
    }
  else if (r->has_f && r->shared_si != NULL)
    {
      const char *rec = g_hash_table_lookup (r->shared, r->shared_si);
      if (rec != NULL)
        {
          int mrow = atoi (rec);
          const char *comma = strchr (rec, ',');
          int mcol = comma ? atoi (comma + 1) : 0;
          const char *text = comma ? strchr (comma + 1, ',') : NULL;
          if (text != NULL)
            {
              O42Node *tree = o42_formula_parse (text + 1);
              char *moved;
              o42_node_relocate (tree, r->row - mrow, r->col - mcol);
              moved = o42_node_to_string (tree);
              input = g_strconcat ("=", moved, NULL);
              g_free (moved);
              o42_node_free (tree);
            }
        }
    }

  if (input == NULL)
    {
      const char *t = r->type;
      if (strcmp (t, "s") == 0)
        {
          guint idx = (guint) atoi (r->v->str);
          if (idx < r->strings->len)
            {
              GArray *runs = (idx < r->string_runs->len)
                             ? g_ptr_array_index (r->string_runs, idx) : NULL;

              input = g_strdup (g_ptr_array_index (r->strings, idx));
              /* The runs go on after the text: setting the text takes
               * them off again, as typing into a cell does. */
              if (runs != NULL && runs->len > 0)
                r->cell_runs = runs;
            }
        }
      else if (strcmp (t, "inlineStr") == 0)
        input = g_strdup (r->is->str);
      else if (strcmp (t, "str") == 0)
        input = g_strdup (r->v->str);
      else if (strcmp (t, "b") == 0)
        input = g_strdup (strcmp (r->v->str, "1") == 0 ? "TRUE" : "FALSE");
      else if (strcmp (t, "e") == 0)
        input = g_strdup (r->v->str);
      else if (r->v->len > 0)
        {
          /* A number: pass it through as text; the sheet parses it. */
          char buf[G_ASCII_DTOSTR_BUF_SIZE];
          input = g_strdup (g_ascii_dtostr (buf, sizeof buf, g_ascii_strtod (r->v->str, NULL)));
        }
    }

  if (input != NULL && input[0] == '=' && r->array_ref != NULL)
    {
      O42Range block;
      gsize used;
      if (o42_ref_parse (r->array_ref, &block.row0, &block.col0, &used) &&
          (r->array_ref[used] == '\0' ||
           (r->array_ref[used] == ':' && o42_ref_parse (r->array_ref + used + 1, &block.row1, &block.col1, NULL))))
        {
          if (r->array_ref[used] == '\0') { block.row1 = block.row0; block.col1 = block.col0; }
          o42_sheet_set_array_formula (r->sheet, &block, input);
        }
    }
  else if (input != NULL && input[0] != '\0' && !o42_sheet_array_range (r->sheet, r->row, r->col, NULL))
    o42_sheet_set_input (r->sheet, r->row, r->col, input);
  g_free (input);

  if (r->cell_runs != NULL)
    {
      o42_sheet_set_runs (r->sheet, r->row, r->col,
                          &g_array_index (r->cell_runs, O42TextRun, 0),
                          (int) r->cell_runs->len);
      r->cell_runs = NULL;
    }

  if (r->xf > 0 && (guint) r->xf < r->xfs->len)
    {
      O42Range one = { r->row, r->col, r->row, r->col };
      int wears = g_array_index (r->xf_styles, int, r->xf);
      const char *style = wears > 0 ? g_hash_table_lookup (r->style_names,
                                                           GINT_TO_POINTER (wears)) : NULL;

      o42_sheet_apply_fmt (r->sheet, &one, O42_FMT_ALL,
                           &g_array_index (r->xfs, O42Fmt, r->xf));
      if (style != NULL)
        o42_sheet_apply_style (r->sheet, &one, style);
    }
}

static void
sheet_end (GMarkupParseContext *ctx, const char *name, gpointer user, GError **error)
{
  Reader *r = user;
  const char *n = local (name);
  (void) ctx; (void) error;

  if (strcmp (n, "rowBreaks") == 0 || strcmp (n, "colBreaks") == 0)
    {
      r->in_breaks = 0;
      return;
    }

  if (r->in_hf && (strcmp (n, "oddHeader") == 0 || strcmp (n, "oddFooter") == 0))
    {
      o42_sheet_set_header_footer (r->sheet, r->in_hf == 1 ? r->f->str : NULL, r->in_hf == 2 ? r->f->str : NULL);
      r->in_hf = 0;
      return;
    }

  if (strcmp (n, "c") == 0)
    finish_cell (r);
  else if (strcmp (n, "formula") == 0 && r->in_cf_formula)
    {
      char *end_ptr = NULL;
      double v = g_ascii_strtod (r->cf_formula->str, &end_ptr);
      r->in_cf_formula = FALSE;
      if (end_ptr != NULL && *end_ptr == '\0' && r->cf_formula->len > 0 && r->cf_n_formulas < 2)
        r->cf_values[r->cf_n_formulas] = v;
      else
        r->cf_is_cellis = FALSE;   /* a formula, not a number: not a rule we keep */
      r->cf_n_formulas++;
    }
  else if ((strcmp (n, "formula1") == 0 || strcmp (n, "formula2") == 0) && r->in_dv_formula)
    {
      char *text = g_strdup (r->dv_text->str);
      gsize len = strlen (text);
      char **slot = r->dv_formula == 0 ? &r->dv.value : &r->dv.value2;
      r->in_dv_formula = FALSE;
      if (len >= 2 && text[0] == '"' && text[len - 1] == '"')
        { text[len - 1] = '\0'; memmove (text, text + 1, len - 1); }
      g_free (*slot);
      *slot = text;
    }
  else if (strcmp (n, "dataValidation") == 0)
    {
      if (r->in_dv)
        o42_sheet_add_validation (r->sheet, &r->dv);
      g_free (r->dv.value); g_free (r->dv.value2); g_free (r->dv.message);
      memset (&r->dv, 0, sizeof r->dv);
      r->in_dv = FALSE;
    }
  else if (strcmp (n, "cfRule") == 0)
    {
      r->in_cf_rule = FALSE;
      if (r->cf_is_cellis && r->cf_have_range && r->cf_n_formulas >= 1 &&
          r->cf_dxf >= 0 && (guint) r->cf_dxf < r->dxfs->len)
        {
          O42Condition c = g_array_index (r->dxfs, O42Condition, r->cf_dxf);
          c.range = r->cf_range;
          c.op = r->cf_op;
          c.value = r->cf_values[0];
          c.value2 = r->cf_n_formulas >= 2 ? r->cf_values[1] : c.value;
          o42_sheet_add_condition (r->sheet, &c);
        }
    }
  else if (strcmp (n, "f") == 0) r->in_f = FALSE;
  else if (strcmp (n, "v") == 0) r->in_v = FALSE;
  else if (strcmp (n, "is") == 0) r->in_is = FALSE;
  else if (strcmp (n, "t") == 0) r->in_t = FALSE;
}

static void
sheet_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (r->in_hf) g_string_append_len (r->f, text, (gssize) len);
  else if (r->in_dv_formula) g_string_append_len (r->dv_text, text, (gssize) len);
  else if (r->in_cf_formula) g_string_append_len (r->cf_formula, text, (gssize) len);
  else if (r->in_f) g_string_append_len (r->f, text, (gssize) len);
  else if (r->in_v) g_string_append_len (r->v, text, (gssize) len);
  else if (r->in_is && r->in_t) g_string_append_len (r->is, text, (gssize) len);
}

/* ---- tables ---- */

static void
tables_start (GMarkupParseContext *ctx, const char *name, const char **names,
              const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (strcmp (local (name), "table") == 0 && r->sheet != NULL)
    {
      const char *tname = attr (names, values, "displayName");
      const char *ref = attr (names, values, "ref");
      O42Range range;
      gsize used;

      if (tname == NULL) tname = attr (names, values, "name");
      if (tname != NULL && ref != NULL &&
          o42_ref_parse (ref, &range.row0, &range.col0, &used) && ref[used] == ':' &&
          o42_ref_parse (ref + used + 1, &range.row1, &range.col1, NULL))
        o42_sheet_add_table (r->sheet, tname, &range,
                             attr_int (names, values, "headerRowCount", 1) != 0);
    }
}

/* ---- comments ---- */

static void
comments_start (GMarkupParseContext *ctx, const char *name, const char **names,
                const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  const char *n = local (name);
  (void) ctx; (void) error;
  if (strcmp (n, "comment") == 0)
    {
      const char *ref = attr (names, values, "ref");
      r->in_comment = ref != NULL && o42_ref_parse (ref, &r->comment_row, &r->comment_col, NULL);
      g_string_truncate (r->comment_text, 0);
    }
  else if (strcmp (n, "t") == 0 && r->in_comment)
    r->in_comment_t = TRUE;
}

static void
comments_end (GMarkupParseContext *ctx, const char *name, gpointer user, GError **error)
{
  Reader *r = user;
  const char *n = local (name);
  (void) ctx; (void) error;
  if (strcmp (n, "comment") == 0)
    {
      if (r->in_comment && r->comment_text->len > 0)
        o42_sheet_set_note (r->sheet, r->comment_row, r->comment_col, r->comment_text->str);
      r->in_comment = FALSE;
    }
  else if (strcmp (n, "t") == 0)
    r->in_comment_t = FALSE;
}

static void
comments_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (r->in_comment && r->in_comment_t)
    g_string_append_len (r->comment_text, text, (gssize) len);
}

/* ---- driving the parts ---- */

/* xl/o42/scripts.xml: <script name="...">code</script>. */
static void
scripts_start (GMarkupParseContext *ctx, const char *name, const char **names,
               const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (strcmp (name, "script") == 0)
    {
      g_free (r->script_name);
      r->script_name = g_strdup (attr (names, values, "name") != NULL ? attr (names, values, "name") : "Script");
      if (r->script_code == NULL) r->script_code = g_string_new (NULL);
      g_string_truncate (r->script_code, 0);
      r->in_script = TRUE;
    }
}

static void
scripts_end (GMarkupParseContext *ctx, const char *name, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (strcmp (name, "script") == 0 && r->in_script)
    {
      o42_book_set_script (r->book, r->script_name, r->script_code->str);
      r->in_script = FALSE;
    }
}

static void
scripts_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (r->in_script)
    g_string_append_len (r->script_code, text, (gssize) len);
}

static gboolean
parse_part (GHashTable *parts, const char *path, const GMarkupParser *parser,
            gpointer r, GError **error)
{
  GBytes *bytes = g_hash_table_lookup (parts, path);
  GMarkupParseContext *ctx;
  gboolean ok;
  gsize len;
  const char *xml;

  if (bytes == NULL)
    return TRUE;   /* an optional part; nothing to read */
  xml = g_bytes_get_data (bytes, &len);
  ctx = g_markup_parse_context_new (parser, G_MARKUP_TREAT_CDATA_AS_TEXT | G_MARKUP_PREFIX_ERROR_POSITION, r, NULL);
  ok = g_markup_parse_context_parse (ctx, xml, (gssize) len, error) &&
       g_markup_parse_context_end_parse (ctx, error);
  g_markup_parse_context_free (ctx);
  return ok;
}

/* A relationship target, resolved against xl/: "worksheets/sheet1.xml"
 * or "/xl/worksheets/sheet1.xml" both give the part name. */
static char *
resolve_target (const char *target)
{
  if (target[0] == '/')
    return g_strdup (target + 1);
  return g_strconcat ("xl/", target, NULL);
}

gboolean
o42_xlsx_load (O42Book *book, GFile *file, GError **error)
{
  static const GMarkupParser rels_parser = { rels_start, NULL, NULL, NULL, NULL };
  static const GMarkupParser workbook_parser = { workbook_start, workbook_end, workbook_text, NULL, NULL };
  static const GMarkupParser sst_parser = { sst_start, sst_end, sst_text, NULL, NULL };
  static const GMarkupParser styles_parser = { styles_start, styles_end, NULL, NULL, NULL };
  static const GMarkupParser sheet_parser = { sheet_start, sheet_end, sheet_text, NULL, NULL };
  static const GMarkupParser comments_parser = { comments_start, comments_end, comments_text, NULL, NULL };
  static const GMarkupParser tables_parser = { tables_start, NULL, NULL, NULL, NULL };
  GBytes *archive;
  GHashTable *parts;
  Reader r;
  gboolean ok = TRUE;

  archive = g_file_load_bytes (file, NULL, NULL, error);
  if (archive == NULL)
    return FALSE;
  parts = o42_zip_read (archive, error);
  g_bytes_unref (archive);
  if (parts == NULL)
    return FALSE;
  if (g_hash_table_lookup (parts, "xl/workbook.xml") == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "This is not an Excel workbook.");
      g_hash_table_unref (parts);
      return FALSE;
    }

  memset (&r, 0, sizeof r);
  r.book = book;
  r.rels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  r.sheet_names = g_ptr_array_new_with_free_func (g_free);
  r.sheet_rids = g_ptr_array_new_with_free_func (g_free);
  r.names = g_ptr_array_new_with_free_func (g_free);
  r.name_text = g_string_new (NULL);
  r.strings = g_ptr_array_new_with_free_func (g_free);
  r.si = g_string_new (NULL);
  r.numfmts = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  r.fonts = g_array_new (FALSE, FALSE, sizeof (O42Fmt));
  r.fills = g_array_new (FALSE, FALSE, sizeof (guint32));
  r.patterns = g_array_new (FALSE, FALSE, sizeof (O42Pattern));
  r.xf_styles = g_array_new (FALSE, FALSE, sizeof (int));
  r.style_xfs = g_array_new (FALSE, FALSE, sizeof (O42Fmt));
  r.style_names = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  r.string_runs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_array_unref_or_null);
  r.pattern_colours = g_array_new (FALSE, FALSE, sizeof (guint32));
  r.borders = g_array_new (FALSE, FALSE, sizeof (guint8));
  r.bdefs = g_array_new (FALSE, TRUE, sizeof (BorderDef));
  r.xfs = g_array_new (FALSE, FALSE, sizeof (O42Fmt));
  r.xf_dates = g_array_new (FALSE, FALSE, sizeof (gboolean));
  r.dxfs = g_array_new (FALSE, FALSE, sizeof (O42Condition));
  r.cf_formula = g_string_new (NULL);
  r.dv_text = g_string_new (NULL);
  r.comment_text = g_string_new (NULL);
  r.f = g_string_new (NULL);
  r.v = g_string_new (NULL);
  r.is = g_string_new (NULL);
  r.shared = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  ok = parse_part (parts, "xl/_rels/workbook.xml.rels", &rels_parser, &r, error) &&
       parse_part (parts, "xl/workbook.xml", &workbook_parser, &r, error) &&
       parse_part (parts, "xl/sharedStrings.xml", &sst_parser, &r, error) &&
       parse_part (parts, "xl/styles.xml", &styles_parser, &r, error);

  if (ok)
    {
      o42_book_clear (book);

      /* Every named style becomes one of the book's own, with the look
       * its cellStyleXf carries. */
      {
        GHashTableIter it;
        gpointer key, value;

        g_hash_table_iter_init (&it, r.style_names);
        while (g_hash_table_iter_next (&it, &key, &value))
          {
            int xf_id = GPOINTER_TO_INT (key);

            if (xf_id >= 0 && (guint) xf_id < r.style_xfs->len)
              o42_book_set_style (book, value,
                                  &g_array_index (r.style_xfs, O42Fmt, xf_id), O42_FMT_ALL);
          }
      }

      for (guint i = 0; i < r.sheet_names->len && ok; i++)
        {
          const char *sname = g_ptr_array_index (r.sheet_names, i);
          const char *rid = g_ptr_array_index (r.sheet_rids, i);
          const char *target = g_hash_table_lookup (r.rels, rid);
          char *part = target ? resolve_target (target) : g_strdup_printf ("xl/worksheets/sheet%u.xml", i + 1);

          if (i == 0)
            {
              r.sheet = o42_book_sheet (book, 0);
              o42_book_rename_sheet (book, 0, sname);
            }
          else
            r.sheet = o42_book_add_sheet (book, sname, -1);

          r.row = -1;
          r.col = -1;
          r.default_width = o42_sheet_col_width (r.sheet, O42_MAX_COLS - 1);
          r.default_height = o42_sheet_row_height (r.sheet, O42_MAX_ROWS - 1);
          g_hash_table_remove_all (r.shared);
          g_clear_pointer (&r.drawing_rid, g_free);
          r.filter_col = -1;
          r.sheet_rels = o42_xlsx_read_rels (parts, part);
          ok = parse_part (parts, part, &sheet_parser, &r, error);
          g_clear_pointer (&r.sheet_rels, g_hash_table_unref);
          if (ok)
            {
              o42_sheet_autofilter_refresh (r.sheet);
              if (r.drawing_rid != NULL)
                o42_xlsx_draw_read (parts, part, r.drawing_rid, r.sheet);
              /* Comments hang off the sheet's relationships by type. */
              {
                GHashTable *rels = o42_xlsx_read_rels (parts, part);
                GHashTableIter it;
                gpointer k, v;
                g_hash_table_iter_init (&it, rels);
                while (ok && g_hash_table_iter_next (&it, &k, &v))
                  {
                    const char *ctarget = v;
                    const char *base = strrchr (ctarget, '/');
                    if (g_str_has_prefix (base ? base + 1 : ctarget, "comments"))
                      {
                        char *cpart = o42_xlsx_resolve (part, ctarget);
                        ok = parse_part (parts, cpart, &comments_parser, &r, error);
                        g_free (cpart);
                      }
                    else if (g_str_has_suffix (ctarget, ".vml"))
                      {
                        /* The legacy drawing: the form controls are in
                         * it, each as a shape with an x:ClientData. */
                        char *vpart = o42_xlsx_resolve (part, ctarget);
                        VmlReader vr;

                        memset (&vr, 0, sizeof vr);
                        vr.sheet = r.sheet;
                        vr.max = 100;
                        vr.step = 1;
                        parse_part (parts, vpart, &vml_parser, &vr, NULL);
                        g_clear_pointer (&vr.style, g_free);
                        g_clear_pointer (&vr.object_type, g_free);
                        g_clear_pointer (&vr.field, g_free);
                        g_clear_pointer (&vr.link, g_free);
                        g_clear_pointer (&vr.range, g_free);
                        g_clear_pointer (&vr.macro, g_free);
                        if (vr.text != NULL)
                          g_string_free (vr.text, TRUE);
                        if (vr.caption != NULL)
                          g_string_free (vr.caption, TRUE);
                        g_free (vpart);
                      }
                    else if (g_str_has_prefix (base ? base + 1 : ctarget, "table"))
                      {
                        char *tpart = o42_xlsx_resolve (part, ctarget);
                        ok = parse_part (parts, tpart, &tables_parser, &r, error);
                        g_free (tpart);
                      }
                  }
                g_hash_table_unref (rels);
              }
            }
          g_free (part);
        }

      /* Names last, when every sheet they may point at exists. */
      for (guint i = 0; ok && i + 1 < r.names->len; i += 2)
        {
          const char *nm = g_ptr_array_index (r.names, i);
          const char *val = g_ptr_array_index (r.names, i + 1);
          O42Node *tree;

          if (nm[0] == '\002')
            {
              /* Print_Area (A) or Print_Titles (T) of sheet nm+2: 'Sheet'!$A$1:$C$5 or 'Sheet'!$1:$2. */
              O42Sheet *target = o42_book_sheet (book, atoi (nm + 2));
              const char *bang = strrchr (val, '!');
              const char *ref = bang != NULL ? bang + 1 : val;
              if (target != NULL && nm[1] == 'A')
                {
                  O42Range area;
                  char *clean = g_strdup (ref);
                  char *w = clean;
                  for (const char *q = ref; *q != '\0'; q++) if (*q != '$') *w++ = *q;
                  *w = '\0';
                  {
                    gsize used = 0;
                    if (o42_ref_parse (clean, &area.row0, &area.col0, &used) && clean[used] == ':' &&
                        o42_ref_parse (clean + used + 1, &area.row1, &area.col1, NULL))
                      o42_sheet_set_print_area (target, &area);
                  }
                  g_free (clean);
                }
              else if (target != NULL)
                {
                  const char *colon = strchr (ref, ':');
                  int last = colon != NULL ? atoi (colon[1] == '$' ? colon + 2 : colon + 1) : 0;
                  const O42PrintSetup *ps = o42_sheet_print_setup (target);
                  if (last > 0)
                    o42_sheet_set_print_options (target, ps->gridlines, ps->headings, last);
                }
              continue;
            }
          if (nm[0] == '\001')
            {
              /* A pivot definition on sheet nm+1, as the writer spells it. */
              int sheet_index = atoi (nm + 1);
              O42Sheet *target = o42_book_sheet (book, sheet_index);
              char *text = g_strdup (val);
              char **f;
              gsize len = strlen (text);
              if (len >= 2 && text[0] == '"' && text[len - 1] == '"')
                { text[len - 1] = '\0'; memmove (text, text + 1, len - 1); }
              f = g_strsplit (text, ";", -1);
              if (target != NULL && g_strv_length (f) >= 10)
                {
                  O42Pivot p;
                  gsize used;
                  memset (&p, 0, sizeof p);
                  if (o42_ref_parse (f[1], &p.source.row0, &p.source.col0, &used) && f[1][used] == ':' &&
                      o42_ref_parse (f[1] + used + 1, &p.source.row1, &p.source.col1, NULL) &&
                      o42_ref_parse (f[6], &p.row, &p.col, NULL))
                    {
                      p.source_sheet = f[0][0] ? f[0] : NULL;
                      p.row_fields = o42_pivot_fields_from_string (f[2]);
                      p.col_fields = o42_pivot_fields_from_string (f[3]);
                      p.data_field = f[4];
                      p.agg = (O42PivotAgg) atoi (f[5]);
                      p.rows = atoi (f[7]);
                      p.cols = atoi (f[8]);
                      if (g_strv_length (f) >= 11)
                        {
                          p.filter_field = f[9][0] ? f[9] : NULL;
                          p.filter_value = f[10];
                        }
                      o42_sheet_define_pivot (target, &p);
                      g_strfreev (p.row_fields);
                      g_strfreev (p.col_fields);
                    }
                }
              g_strfreev (f);
              g_free (text);
              continue;
            }
          tree = o42_formula_parse (val[0] == '=' ? val + 1 : val);
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
          if (usable)
            {
              O42Sheet *target = tree->sheet ? o42_book_find_sheet (book, tree->sheet)
                                             : o42_book_sheet (book, 0);
              if (target != NULL)
                o42_book_define_name (book, nm, target, &range);
            }
          o42_node_free (tree);
        }

      for (int i = 0; i < o42_book_n_sheets (book); i++)
        {
          o42_sheet_clear_undo (o42_book_sheet (book, i));
          o42_sheet_set_modified (o42_book_sheet (book, i), FALSE);
        }
    }

  g_hash_table_unref (r.rels);
  g_ptr_array_unref (r.sheet_names);
  g_ptr_array_unref (r.sheet_rids);
  g_ptr_array_unref (r.names);
  g_string_free (r.name_text, TRUE);
  g_free (r.script_name);
  g_free (r.scenario_name);
  g_free (r.scenario_comment);
  if (r.script_code != NULL) g_string_free (r.script_code, TRUE);
  g_free (r.name_name);
  g_ptr_array_unref (r.strings);
  g_string_free (r.si, TRUE);
  g_hash_table_unref (r.numfmts);
  g_array_unref (r.fonts);
  g_array_unref (r.fills);
  g_array_unref (r.patterns);
  g_array_unref (r.xf_styles);
  g_array_unref (r.style_xfs);
  g_hash_table_unref (r.style_names);
  g_ptr_array_unref (r.string_runs);
  if (r.si_runs != NULL)
    g_array_unref (r.si_runs);
  g_array_unref (r.pattern_colours);
  g_array_unref (r.borders);
  g_array_unref (r.bdefs);
  g_array_unref (r.xfs);
  g_array_unref (r.xf_dates);
  g_array_unref (r.dxfs);
  g_string_free (r.cf_formula, TRUE);
  g_string_free (r.dv_text, TRUE);
  g_string_free (r.comment_text, TRUE);
  g_string_free (r.f, TRUE);
  g_string_free (r.v, TRUE);
  g_string_free (r.is, TRUE);
  g_free (r.type);
  g_free (r.shared_si);
  g_free (r.array_ref);
  g_free (r.drawing_rid);
  g_hash_table_unref (r.shared);
  if (ok)
    {
      static const GMarkupParser scripts_parser = { scripts_start, scripts_end, scripts_text, NULL, NULL };
      static const GMarkupParser views_parser = { views_start, NULL, NULL, NULL, NULL };

      ok = parse_part (parts, "xl/o42/scripts.xml", &scripts_parser, &r, error) &&
           parse_part (parts, "xl/o42/views.xml", &views_parser, &r, error);
    }
  g_hash_table_unref (parts);
  return ok;
}
