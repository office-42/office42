/* o42-ods.c - OpenDocument spreadsheets, .ods
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * content.xml carries nearly everything: the automatic styles first
 * (column widths, row heights, cell formats, number styles), then one
 * table:table per sheet made of rows of cells, each cell typed and
 * holding its formula in OpenFormula notation.  Frozen panes live in
 * settings.xml.  Reading walks the same elements with GMarkup.
 */

#include "o42-ods.h"

#include "o42-image.h"

#include "o42-zip.h"
#include "o42-sheet.h"
#include "o42-formula.h"
#include "o42-date.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#define MIME "application/vnd.oasis.opendocument.spreadsheet"

#define NS_HEAD \
  "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" " \
  "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" " \
  "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" " \
  "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" " \
  "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" " \
  "xmlns:number=\"urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0\" " \
  "xmlns:of=\"urn:oasis:names:tc:opendocument:xmlns:of:1.2\" " \
  "xmlns:config=\"urn:oasis:names:tc:opendocument:xmlns:config:1.0\" " \
  "xmlns:ooo=\"http://openoffice.org/2004/office\" " \
  "xmlns:xlink=\"http://www.w3.org/1999/xlink\" " \
  "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" " \
  "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" " \
  "xmlns:chart=\"urn:oasis:names:tc:opendocument:xmlns:chart:1.0\" " \
  "office:version=\"1.2\""

/* ====================================================================== */
/* Writing                                                                */
/* ====================================================================== */

/* ---- Formulas in OpenFormula notation --------------------------------- */

static int
op_precedence (const O42Node *node)
{
  if (node->type == O42_NODE_UNARY)
    return node->as.op.op == O42_OP_PERCENT ? 7 : 6;
  if (node->type != O42_NODE_BINARY)
    return 8;
  switch (node->as.op.op)
    {
    case O42_OP_EQ: case O42_OP_NE: case O42_OP_LT:
    case O42_OP_GT: case O42_OP_LE: case O42_OP_GE: return 1;
    case O42_OP_CONCAT: return 2;
    case O42_OP_ADD: case O42_OP_SUB: return 3;
    case O42_OP_MUL: case O42_OP_DIV: return 4;
    case O42_OP_POW: return 5;
    default: return 8;
    }
}

static const char *
op_text (O42Op op)
{
  switch (op)
    {
    case O42_OP_ADD: return "+";    case O42_OP_SUB: return "-";
    case O42_OP_MUL: return "*";    case O42_OP_DIV: return "/";
    case O42_OP_POW: return "^";    case O42_OP_CONCAT: return "&";
    case O42_OP_EQ: return "=";     case O42_OP_NE: return "<>";
    case O42_OP_LT: return "<";     case O42_OP_GT: return ">";
    case O42_OP_LE: return "<=";    case O42_OP_GE: return ">=";
    case O42_OP_NEG: return "-";    case O42_OP_POS: return "+";
    case O42_OP_PERCENT: return "%";
    }
  return "?";
}

static void
of_sheet_prefix (const char *sheet, GString *out)
{
  if (sheet == NULL)
    {
      g_string_append_c (out, '.');
      return;
    }
  g_string_append_c (out, '$');
  if (strpbrk (sheet, " '.!-+") != NULL || g_ascii_isdigit (sheet[0]))
    {
      g_string_append_c (out, '\'');
      for (const char *p = sheet; *p != '\0'; p++)
        {
          if (*p == '\'') g_string_append_c (out, '\'');
          g_string_append_c (out, *p);
        }
      g_string_append_c (out, '\'');
    }
  else
    g_string_append (out, sheet);
  g_string_append_c (out, '.');
}

static void of_write (const O42Node *node, GString *out);

static void
of_write_child (const O42Node *child, const O42Node *parent, gboolean right, GString *out)
{
  gboolean parens = child != NULL &&
    (op_precedence (child) < op_precedence (parent) ||
     (right && op_precedence (child) == op_precedence (parent) && child->type == O42_NODE_BINARY));
  if (parens) g_string_append_c (out, '(');
  of_write (child, out);
  if (parens) g_string_append_c (out, ')');
}

static void
of_write (const O42Node *node, GString *out)
{
  if (node == NULL)
    return;
  switch (node->type)
    {
    case O42_NODE_NUMBER:
      {
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_string_append (out, g_ascii_dtostr (buf, sizeof buf, node->as.number));
        break;
      }
    case O42_NODE_STRING:
      g_string_append_c (out, '"');
      for (const char *p = node->as.string; *p != '\0'; p++)
        {
          if (*p == '"') g_string_append (out, "\"\"");
          else g_string_append_c (out, *p);
        }
      g_string_append_c (out, '"');
      break;
    case O42_NODE_BOOL:
      g_string_append (out, node->as.boolean ? "TRUE()" : "FALSE()");
      break;
    case O42_NODE_NAME:
      g_string_append (out, node->as.name);
      break;
    case O42_NODE_ERROR:
      g_string_append (out, node->as.error == O42_ERR_NA ? "NA()" : "#VALUE!");
      break;
    case O42_NODE_EMPTY:
      break;
    case O42_NODE_ARRAY:
      g_string_append_c (out, '{');
      for (int i = 0; i < node->as.array.rows * node->as.array.cols &&
                      (guint) i < node->as.array.items->len; i++)
        {
          if (i > 0)
            g_string_append_c (out, (i % node->as.array.cols == 0) ? '|' : ';');
          of_write (g_ptr_array_index (node->as.array.items, i), out);
        }
      g_string_append_c (out, '}');
      break;
    case O42_NODE_REF:
      {
        char *name = o42_ref_name_full (node->as.ref.row, node->as.ref.col,
                                        (node->abs & O42_ABS_ROW0) != 0, (node->abs & O42_ABS_COL0) != 0);
        g_string_append_c (out, '[');
        of_sheet_prefix (node->sheet, out);
        g_string_append (out, name);
        if (node->sheet_last != NULL)
          {
            g_string_append_c (out, ':');
            of_sheet_prefix (node->sheet_last, out);
            g_string_append (out, name);
          }
        g_string_append_c (out, ']');
        g_free (name);
        break;
      }
    case O42_NODE_RANGE:
      {
        char *a = o42_ref_name_full (node->as.range.row0, node->as.range.col0,
                                     (node->abs & O42_ABS_ROW0) != 0, (node->abs & O42_ABS_COL0) != 0);
        char *b = o42_ref_name_full (node->as.range.row1, node->as.range.col1,
                                     (node->abs & O42_ABS_ROW1) != 0, (node->abs & O42_ABS_COL1) != 0);
        g_string_append_c (out, '[');
        of_sheet_prefix (node->sheet, out);
        g_string_append (out, a);
        g_string_append_c (out, ':');
        of_sheet_prefix (node->sheet_last, out);
        g_string_append (out, b);
        g_string_append_c (out, ']');
        g_free (a);
        g_free (b);
        break;
      }
    case O42_NODE_UNARY:
      if (node->as.op.op == O42_OP_PERCENT)
        {
          of_write_child (node->as.op.a, node, FALSE, out);
          g_string_append_c (out, '%');
        }
      else
        {
          g_string_append (out, op_text (node->as.op.op));
          of_write_child (node->as.op.a, node, TRUE, out);
        }
      break;
    case O42_NODE_BINARY:
      of_write_child (node->as.op.a, node, FALSE, out);
      g_string_append (out, op_text (node->as.op.op));
      of_write_child (node->as.op.b, node, TRUE, out);
      break;
    case O42_NODE_CALL:
      g_string_append (out, node->as.call.name);
      g_string_append_c (out, '(');
      if (node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          {
            if (i > 0) g_string_append_c (out, ';');
            of_write (g_ptr_array_index (node->as.call.args, i), out);
          }
      g_string_append_c (out, ')');
      break;
    default:
      break;
    }
}

static char *
of_formula (const char *input)
{
  O42Node *tree = o42_formula_parse (input + 1);
  GString *out = g_string_new ("of:=");
  of_write (tree, out);
  o42_node_free (tree);
  return g_string_free (out, FALSE);
}

/* ---- Styles ----------------------------------------------------------- */

typedef struct {
  GString    *styles;       /* the automatic styles */
  GHashTable *col_styles;   /* width (px) -> "co3" */
  GHashTable *row_styles;   /* height (px) -> "ro2" */
  GHashTable *cell_styles;  /* O42Fmt bytes -> "ce5" */
  GHashTable *num_styles;   /* number/decimals key -> "N7" */
  int         next_co, next_ro, next_ce, next_n, next_t;
} Styles;

static char *
length_cm (int px)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  return g_strdup_printf ("%scm", g_ascii_formatd (buf, sizeof buf, "%.3f", px * 2.54 / 96.0));
}

static const char *
col_style (Styles *s, int px)
{
  char *name = g_hash_table_lookup (s->col_styles, GINT_TO_POINTER (px));
  if (name == NULL)
    {
      char *cm = length_cm (px);
      name = g_strdup_printf ("co%d", ++s->next_co);
      g_string_append_printf (s->styles,
        "<style:style style:name=\"%s\" style:family=\"table-column\">"
        "<style:table-column-properties fo:break-before=\"auto\" style:column-width=\"%s\"/></style:style>", name, cm);
      g_hash_table_insert (s->col_styles, GINT_TO_POINTER (px), name);
      g_free (cm);
    }
  return name;
}

static const char *
row_style (Styles *s, int px)
{
  char *name = g_hash_table_lookup (s->row_styles, GINT_TO_POINTER (px));
  if (name == NULL)
    {
      char *cm = length_cm (px);
      name = g_strdup_printf ("ro%d", ++s->next_ro);
      g_string_append_printf (s->styles,
        "<style:style style:name=\"%s\" style:family=\"table-row\">"
        "<style:table-row-properties style:row-height=\"%s\" fo:break-before=\"auto\" style:use-optimal-row-height=\"false\"/></style:style>", name, cm);
      g_hash_table_insert (s->row_styles, GINT_TO_POINTER (px), name);
      g_free (cm);
    }
  return name;
}

/* A text style for part of a cell's text: only what the run says
 * differently from the cell it sits in. */
static char *
text_style (Styles *s, const O42Fmt *run, const O42Fmt *base)
{
  GString *properties = g_string_new (NULL);
  char *name;

  if (run->family != base->family && run->family != NULL)
    g_string_append_printf (properties, " style:font-name=\"%s\"", run->family);
  if (run->size != base->size)
    g_string_append_printf (properties, " fo:font-size=\"%gpt\"", run->size / 2.0);
  if (run->bold != base->bold)
    g_string_append_printf (properties, " fo:font-weight=\"%s\"", run->bold ? "bold" : "normal");
  if (run->italic != base->italic)
    g_string_append_printf (properties, " fo:font-style=\"%s\"", run->italic ? "italic" : "normal");
  if (run->underline != base->underline)
    g_string_append_printf (properties, " style:text-underline-style=\"%s\""
                                        " style:text-underline-width=\"auto\""
                                        " style:text-underline-color=\"font-color\"",
                            run->underline ? "solid" : "none");
  if (run->strikeout != base->strikeout)
    g_string_append_printf (properties, " style:text-line-through-style=\"%s\"",
                            run->strikeout ? "solid" : "none");
  if (run->colour != base->colour)
    g_string_append_printf (properties, " fo:color=\"#%06X\"", run->colour);

  if (properties->len == 0)
    {
      g_string_free (properties, TRUE);
      return NULL;
    }
  name = g_strdup_printf ("T%d", ++s->next_t);
  g_string_append_printf (s->styles,
    "<style:style style:name=\"%s\" style:family=\"text\">"
    "<style:text-properties%s/></style:style>", name, properties->str);
  g_string_free (properties, TRUE);
  return name;
}

/* A number style for a format, or NULL for General. */
static const char *
num_style (Styles *s, const O42Fmt *fmt)
{
  char key[64];
  char *name;
  int decimals = fmt->decimals;

  if (fmt->number == O42_NUM_GENERAL && fmt->custom == NULL)
    return NULL;
  g_snprintf (key, sizeof key, "%d/%d", (int) fmt->number, decimals);
  name = g_hash_table_lookup (s->num_styles, key);
  if (name != NULL)
    return name;
  name = g_strdup_printf ("N%d", ++s->next_n);
  switch (fmt->number)
    {
    case O42_NUM_FIXED:
    case O42_NUM_COMMA:
      g_string_append_printf (s->styles,
        "<number:number-style style:name=\"%s\"><number:number number:decimal-places=\"%d\" number:min-decimal-places=\"%d\" number:min-integer-digits=\"1\"%s/></number:number-style>",
        name, decimals, decimals, fmt->number == O42_NUM_COMMA ? " number:grouping=\"true\"" : "");
      break;
    case O42_NUM_CURRENCY:
      g_string_append_printf (s->styles,
        "<number:currency-style style:name=\"%s\"><number:currency-symbol>$</number:currency-symbol>"
        "<number:number number:decimal-places=\"%d\" number:min-decimal-places=\"%d\" number:min-integer-digits=\"1\" number:grouping=\"true\"/></number:currency-style>",
        name, decimals, decimals);
      break;
    case O42_NUM_PERCENT:
      g_string_append_printf (s->styles,
        "<number:percentage-style style:name=\"%s\"><number:number number:decimal-places=\"%d\" number:min-decimal-places=\"%d\" number:min-integer-digits=\"1\"/><number:text>%%</number:text></number:percentage-style>",
        name, decimals, decimals);
      break;
    case O42_NUM_SCIENTIFIC:
      g_string_append_printf (s->styles,
        "<number:number-style style:name=\"%s\"><number:scientific-number number:decimal-places=\"%d\" number:min-integer-digits=\"1\" number:min-exponent-digits=\"2\"/></number:number-style>",
        name, decimals);
      break;
    case O42_NUM_TEXT:
      g_string_append_printf (s->styles, "<number:text-style style:name=\"%s\"><number:text-content/></number:text-style>", name);
      break;
    case O42_NUM_DATE:
      g_string_append_printf (s->styles,
        "<number:date-style style:name=\"%s\"><number:year number:style=\"long\"/><number:text>-</number:text>"
        "<number:month number:style=\"long\"/><number:text>-</number:text><number:day number:style=\"long\"/></number:date-style>", name);
      break;
    case O42_NUM_TIME:
      g_string_append_printf (s->styles,
        "<number:time-style style:name=\"%s\"><number:hours number:style=\"long\"/><number:text>:</number:text>"
        "<number:minutes number:style=\"long\"/><number:text>:</number:text><number:seconds number:style=\"long\"/></number:time-style>", name);
      break;
    case O42_NUM_DATETIME:
      g_string_append_printf (s->styles,
        "<number:date-style style:name=\"%s\"><number:year number:style=\"long\"/><number:text>-</number:text>"
        "<number:month number:style=\"long\"/><number:text>-</number:text><number:day number:style=\"long\"/><number:text> </number:text>"
        "<number:hours number:style=\"long\"/><number:text>:</number:text><number:minutes number:style=\"long\"/></number:date-style>", name);
      break;
    default:
      g_string_append_printf (s->styles, "<number:number-style style:name=\"%s\"><number:number number:min-integer-digits=\"1\"/></number:number-style>", name);
      break;
    }
  g_hash_table_insert (s->num_styles, g_strdup (key), name);
  return name;
}

static guint
fmt_hash (gconstpointer key)
{
  const guchar *p = key;
  guint h = 5381;
  for (gsize i = 0; i < sizeof (O42Fmt); i++)
    h = h * 33 + p[i];
  return h;
}

static gboolean
fmt_equal (gconstpointer a, gconstpointer b)
{
  return memcmp (a, b, sizeof (O42Fmt)) == 0;
}

static const char *
cell_style (Styles *s, const O42Fmt *fmt)
{
  char *name = g_hash_table_lookup (s->cell_styles, fmt);
  const char *num;
  O42Fmt *key;

  if (name != NULL)
    return name;
  num = num_style (s, fmt);
  name = g_strdup_printf ("ce%d", ++s->next_ce);
  g_string_append_printf (s->styles, "<style:style style:name=\"%s\" style:family=\"table-cell\" style:parent-style-name=\"Default\"", name);
  if (num != NULL)
    g_string_append_printf (s->styles, " style:data-style-name=\"%s\"", num);
  g_string_append_c (s->styles, '>');

  g_string_append (s->styles, "<style:table-cell-properties");
  if (fmt->fill != O42_FILL_NONE)
    g_string_append_printf (s->styles, " fo:background-color=\"#%06x\"", fmt->fill & 0xFFFFFF);
  {
    static const char *sides[4] = { "top", "bottom", "left", "right" };
    for (int i = 0; i < 4; i++)
      if (fmt->border_style[i] != O42_BORDER_NONE)
        {
          O42BorderStyle st = fmt->border_style[i];
          const char *width = st == O42_BORDER_MEDIUM ? "1.5pt" : st == O42_BORDER_THICK ? "2.5pt" : "0.75pt";
          const char *kind = st == O42_BORDER_DOUBLE ? "double" : st == O42_BORDER_DASHED ? "dashed"
                           : st == O42_BORDER_DOTTED ? "dotted" : "solid";
          g_string_append_printf (s->styles, " fo:border-%s=\"%s %s #%06x\"", sides[i], width, kind, fmt->border_colour[i] & 0xFFFFFF);
        }
  }
  if (fmt->rotation != 0)
    g_string_append_printf (s->styles, " style:rotation-angle=\"%d\"", fmt->rotation < 0 ? 360 + fmt->rotation : fmt->rotation);
  if (fmt->wrap)
    g_string_append (s->styles, " fo:wrap-option=\"wrap\"");
  g_string_append_printf (s->styles, " style:vertical-align=\"%s\"",
                          fmt->valign == O42_VALIGN_TOP ? "top" : fmt->valign == O42_VALIGN_MIDDLE ? "middle" : "bottom");
  g_string_append (s->styles, "/>");

  if (fmt->halign != O42_HALIGN_GENERAL || fmt->indent > 0)
    {
      g_string_append (s->styles, "<style:paragraph-properties");
      if (fmt->halign != O42_HALIGN_GENERAL)
        g_string_append_printf (s->styles, " fo:text-align=\"%s\"",
                                fmt->halign == O42_HALIGN_LEFT ? "start" : fmt->halign == O42_HALIGN_CENTRE ? "center" : "end");
      if (fmt->indent > 0)
        g_string_append_printf (s->styles, " fo:margin-left=\"%d.%02dcm\"", fmt->indent / 4, (fmt->indent % 4) * 25);
      g_string_append (s->styles, "/>");
    }

  {
    char *family = g_markup_escape_text (fmt->family != NULL ? fmt->family : "Arial", -1);
    g_string_append_printf (s->styles, "<style:text-properties fo:font-family=\"%s\" fo:font-size=\"%dpt\"", family, fmt->size / 2);
    g_free (family);
  }
  if (fmt->bold)      g_string_append (s->styles, " fo:font-weight=\"bold\"");
  if (fmt->italic)    g_string_append (s->styles, " fo:font-style=\"italic\"");
  if (fmt->underline) g_string_append (s->styles, " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"");
  if (fmt->strikeout) g_string_append (s->styles, " style:text-line-through-style=\"solid\"");
  if (fmt->colour != 0)
    g_string_append_printf (s->styles, " fo:color=\"#%06x\"", fmt->colour & 0xFFFFFF);
  g_string_append (s->styles, "/></style:style>");

  key = g_memdup2 (fmt, sizeof *fmt);
  g_hash_table_insert (s->cell_styles, key, name);
  return name;
}

/* ---- Cells ------------------------------------------------------------ */

static void
append_text_runs (GString *out, Styles *s, const char *text, const O42TextRun *runs,
                  int n_runs, const O42Fmt *base)
{
  /* Text set in more than one font: a text:span per run, with a style
   * of its own for whatever the run says differently. */
  gsize length = strlen (text != NULL ? text : "");

  g_string_append (out, "<text:p>");
  for (int i = 0; i < n_runs; i++)
    {
      gsize start = CLAMP ((gsize) runs[i].start, 0, length);
      gsize end = (i + 1 < n_runs) ? CLAMP ((gsize) runs[i + 1].start, 0, length) : length;
      char *style, *escaped;

      if (end <= start)
        continue;
      style = text_style (s, &runs[i].fmt, base);
      escaped = g_markup_escape_text (text + start, (gssize) (end - start));
      if (style != NULL)
        g_string_append_printf (out, "<text:span text:style-name=\"%s\">%s</text:span>",
                                style, escaped);
      else
        g_string_append (out, escaped);
      g_free (escaped);
      g_free (style);
    }
  g_string_append (out, "</text:p>");
}

static void
append_text_p (GString *out, const char *text, const char *link)
{
  /* Lines become paragraphs; runs of spaces need text:s; a hyperlink
   * wraps each paragraph in text:a. */
  char **lines = g_strsplit (text != NULL ? text : "", "\n", -1);
  for (int i = 0; lines[i] != NULL; i++)
    {
      const char *p = lines[i];
      g_string_append (out, "<text:p>");
      if (link != NULL)
        {
          char *href = g_markup_escape_text (link, -1);
          g_string_append_printf (out, "<text:a xlink:href=\"%s\" xlink:type=\"simple\">", href);
          g_free (href);
        }
      while (*p != '\0')
        {
          if (*p == ' ' && (p[1] == ' ' || p == lines[i]))
            {
              int n = 0;
              while (*p == ' ') { n++; p++; }
              if (n == 1) g_string_append_c (out, ' ');
              else g_string_append_printf (out, "<text:s text:c=\"%d\"/>", n);
              continue;
            }
          if (*p == '\t')
            { g_string_append (out, "<text:tab/>"); p++; continue; }
          {
            const char *q = p;
            while (*q != '\0' && *q != '\t' && !(*q == ' ' && q[1] == ' ')) q++;
            {
              char *esc = g_markup_escape_text (p, q - p);
              g_string_append (out, esc);
              g_free (esc);
            }
            p = q;
          }
        }
      if (link != NULL)
        g_string_append (out, "</text:a>");
      g_string_append (out, "</text:p>");
    }
  g_strfreev (lines);
}

/* The merge whose top-left corner is here, if any. */
static const O42Range *
merge_at_head (O42Sheet *sheet, int row, int col)
{
  GArray *merges = o42_sheet_merges (sheet);
  for (guint i = 0; i < merges->len; i++)
    {
      const O42Range *m = &g_array_index (merges, O42Range, i);
      if (m->row0 == row && m->col0 == col)
        return m;
    }
  return NULL;
}

static gboolean
merge_covers (O42Sheet *sheet, int row, int col)
{
  GArray *merges = o42_sheet_merges (sheet);
  for (guint i = 0; i < merges->len; i++)
    {
      const O42Range *m = &g_array_index (merges, O42Range, i);
      if (o42_range_contains (m, row, col) && !(m->row0 == row && m->col0 == col))
        return TRUE;
    }
  return FALSE;
}

/* ---- Drawings: what floats over the cells ---------------------------- */

/* OpenDocument anchors a drawing inside the cell it hangs from, so
 * every picture, shape and chart is written into its own cell's
 * element.  Sizes are in centimetres, at the 96 dots to the inch the
 * grid is laid out in. */
#define PX_TO_CM (2.54 / 96.0)

/* The pictures of a book, gathered so that each has a file name of its
 * own: sheet index and picture index. */
typedef struct {
  int   index;        /* which of this sheet's objects */
  char *part;         /* the name it goes into the zip under */
} OdsPicture;

static void
append_frame_head (GString *out, const char *name, int z,
                   double dx, double dy, double w, double h)
{
  char *escaped = g_markup_escape_text (name, -1);

  g_string_append_printf (out,
    "<draw:frame draw:name=\"%s\" draw:z-index=\"%d\" table:end-cell-address=\"\" "
    "svg:x=\"%.3fcm\" svg:y=\"%.3fcm\" svg:width=\"%.3fcm\" svg:height=\"%.3fcm\">",
    escaped, z, dx * PX_TO_CM, dy * PX_TO_CM, w * PX_TO_CM, h * PX_TO_CM);
  g_free (escaped);
}

/* Everything anchored to one cell, drawn inside its element. */
static void
write_cell_drawings (GString *out, O42Sheet *sheet, int sheet_index, int row, int col)
{
  GPtrArray *pictures = o42_sheet_pictures (sheet);
  GPtrArray *shapes = o42_sheet_shapes (sheet);
  GPtrArray *charts = o42_sheet_charts (sheet);

  for (guint i = 0; i < pictures->len; i++)
    {
      const O42Picture *pic = g_ptr_array_index (pictures, i);
      char *name;

      if (pic->row != row || pic->col != col)
        continue;
      name = g_strdup_printf ("Picture %u", i + 1);
      append_frame_head (out, name, (int) i, pic->dx, pic->dy, pic->width, pic->height);
      g_string_append_printf (out,
        "<draw:image xlink:href=\"Pictures/sheet%d_image%u.%s\" xlink:type=\"simple\" "
        "xlink:show=\"embed\" xlink:actuate=\"onLoad\"/></draw:frame>",
        sheet_index + 1, i + 1, pic->format != NULL ? pic->format : "png");
      g_free (name);
    }

  for (guint i = 0; i < shapes->len; i++)
    {
      const O42Shape *shape = g_ptr_array_index (shapes, i);
      char *name;
      char *text;

      if (shape->row != row || shape->col != col)
        continue;
      name = g_strdup_printf ("Shape %u", i + 1);
      text = g_markup_escape_text (shape->text != NULL ? shape->text : "", -1);
      /* A line is a line; everything else is a box or an ellipse, and
       * the text inside it goes in a paragraph as it does anywhere. */
      if (shape->kind == O42_SHAPE_LINE || shape->kind == O42_SHAPE_ARROW)
        g_string_append_printf (out,
          "<draw:line draw:name=\"%s\" svg:x1=\"%.3fcm\" svg:y1=\"%.3fcm\" "
          "svg:x2=\"%.3fcm\" svg:y2=\"%.3fcm\"><text:p/></draw:line>",
          name, shape->dx * PX_TO_CM, shape->dy * PX_TO_CM,
          (shape->dx + shape->width) * PX_TO_CM, (shape->dy + shape->height) * PX_TO_CM);
      else
        g_string_append_printf (out,
          "<draw:%s draw:name=\"%s\" svg:x=\"%.3fcm\" svg:y=\"%.3fcm\" "
          "svg:width=\"%.3fcm\" svg:height=\"%.3fcm\"><text:p>%s</text:p></draw:%s>",
          shape->kind == O42_SHAPE_OVAL ? "ellipse" : "rect", name,
          shape->dx * PX_TO_CM, shape->dy * PX_TO_CM,
          shape->width * PX_TO_CM, shape->height * PX_TO_CM, text,
          shape->kind == O42_SHAPE_OVAL ? "ellipse" : "rect");
      g_free (text);
      g_free (name);
    }

  for (guint i = 0; i < charts->len; i++)
    {
      const O42Chart *chart = g_ptr_array_index (charts, i);
      char *name;

      if (chart->row != row || chart->col != col)
        continue;
      name = g_strdup_printf ("Chart %u", i + 1);
      append_frame_head (out, name, (int) (100 + i), chart->dx, chart->dy,
                         chart->width, chart->height);
      g_string_append_printf (out,
        "<draw:object xlink:href=\"./Sheet%dChart%u\" xlink:type=\"simple\" "
        "xlink:show=\"embed\" xlink:actuate=\"onLoad\"/></draw:frame>",
        sheet_index + 1, i + 1);
      g_free (name);
    }
}

/* Whether anything at all hangs from this cell. */
static gboolean
cell_has_drawings (O42Sheet *sheet, int row, int col)
{
  GPtrArray *lists[3];

  lists[0] = o42_sheet_pictures (sheet);
  lists[1] = o42_sheet_shapes (sheet);
  lists[2] = o42_sheet_charts (sheet);
  for (guint i = 0; i < o42_sheet_pictures (sheet)->len; i++)
    {
      const O42Picture *pic = g_ptr_array_index (lists[0], i);

      if (pic->row == row && pic->col == col)
        return TRUE;
    }
  for (guint i = 0; i < lists[1]->len; i++)
    {
      const O42Shape *shape = g_ptr_array_index (lists[1], i);

      if (shape->row == row && shape->col == col)
        return TRUE;
    }
  for (guint i = 0; i < lists[2]->len; i++)
    {
      const O42Chart *chart = g_ptr_array_index (lists[2], i);

      if (chart->row == row && chart->col == col)
        return TRUE;
    }
  return FALSE;
}

static void
write_cell (GString *out, Styles *s, O42Sheet *sheet, int sheet_index, int row, int col,
            O42FmtIdx default_idx)
{
  char *input = o42_sheet_get_input (sheet, row, col);
  O42Value value;
  const O42Fmt *fmt = o42_sheet_get_fmt (sheet, row, col);
  O42FmtIdx idx = o42_sheet_get_fmt_idx (sheet, row, col);
  const char *note = o42_sheet_get_note (sheet, row, col);
  const O42Range *merge = merge_at_head (sheet, row, col);
  gboolean formula = input != NULL && input[0] == '=';
  gboolean has_content = input != NULL && *input != '\0';

  if (merge_covers (sheet, row, col))
    {
      g_string_append (out, "<table:covered-table-cell/>");
      g_free (input);
      return;
    }

  o42_sheet_get_value (sheet, row, col, &value);
  g_string_append (out, "<table:table-cell");
  if (idx != default_idx)
    g_string_append_printf (out, " table:style-name=\"%s\"", cell_style (s, fmt));
  if (merge != NULL)
    g_string_append_printf (out, " table:number-columns-spanned=\"%d\" table:number-rows-spanned=\"%d\"",
                            merge->col1 - merge->col0 + 1, merge->row1 - merge->row0 + 1);
  if (formula)
    {
      char *of = of_formula (input);
      char *esc = g_markup_escape_text (of, -1);
      g_string_append_printf (out, " table:formula=\"%s\"", esc);
      g_free (esc);
      g_free (of);
    }
  if (has_content)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      switch (value.type)
        {
        case O42_VALUE_NUMBER:
          if (fmt->number == O42_NUM_DATE || fmt->number == O42_NUM_DATETIME)
            {
              int y, m, d, hh, mm, ss;
              if (o42_date_from_serial (value.as.number, &y, &m, &d))
                {
                  o42_time_from_serial (value.as.number, &hh, &mm, &ss);
                  if (fmt->number == O42_NUM_DATETIME)
                    g_string_append_printf (out, " office:value-type=\"date\" office:date-value=\"%04d-%02d-%02dT%02d:%02d:%02d\"", y, m, d, hh, mm, ss);
                  else
                    g_string_append_printf (out, " office:value-type=\"date\" office:date-value=\"%04d-%02d-%02d\"", y, m, d);
                  break;
                }
            }
          if (fmt->number == O42_NUM_TIME)
            {
              int hh, mm, ss;
              o42_time_from_serial (value.as.number, &hh, &mm, &ss);
              g_string_append_printf (out, " office:value-type=\"time\" office:time-value=\"PT%02dH%02dM%02dS\"", hh, mm, ss);
              break;
            }
          g_string_append_printf (out, " office:value-type=\"%s\" office:value=\"%s\"",
                                  fmt->number == O42_NUM_PERCENT ? "percentage" : fmt->number == O42_NUM_CURRENCY ? "currency" : "float",
                                  g_ascii_dtostr (buf, sizeof buf, value.as.number));
          if (fmt->number == O42_NUM_CURRENCY)
            g_string_append (out, " office:currency=\"USD\"");
          break;
        case O42_VALUE_BOOL:
          g_string_append_printf (out, " office:value-type=\"boolean\" office:boolean-value=\"%s\"", value.as.boolean ? "true" : "false");
          break;
        case O42_VALUE_TEXT:
        case O42_VALUE_ERROR:
          g_string_append (out, " office:value-type=\"string\"");
          break;
        default:
          break;
        }
    }
  g_string_append_c (out, '>');
  write_cell_drawings (out, sheet, sheet_index, row, col);
  if (note != NULL)
    {
      g_string_append (out, "<office:annotation office:display=\"false\">");
      append_text_p (out, note, NULL);
      g_string_append (out, "</office:annotation>");
    }
  if (has_content)
    {
      char *shown = o42_fmt_display (fmt, &value);
      const char *link = o42_sheet_get_link (sheet, row, col);
      char *href = NULL;
      if (link != NULL && link[0] == '#')
        {
          /* #Sheet!A1 is #Sheet.A1 in OpenDocument. */
          char *bang;
          href = g_strdup (link);
          bang = strchr (href, '!');
          if (bang != NULL) *bang = '.';
        }
      {
        int n_runs = 0;
        const O42TextRun *runs = (value.type == O42_VALUE_TEXT)
                                 ? o42_sheet_runs (sheet, row, col, &n_runs) : NULL;

        if (runs != NULL && link == NULL)
          append_text_runs (out, s, shown, runs, n_runs, fmt);
        else
          append_text_p (out, shown, link != NULL ? (href != NULL ? href : link) : NULL);
      }
      g_free (href);
      g_free (shown);
    }
  g_string_append (out, "</table:table-cell>");
  o42_value_clear (&value);
  g_free (input);
}

static void
write_table (GString *out, Styles *s, O42Sheet *sheet, int sheet_index)
{
  O42Range used;
  int default_width = o42_sheet_col_width (sheet, O42_MAX_COLS - 1);
  int default_height = o42_sheet_row_height (sheet, O42_MAX_ROWS - 1);
  O42FmtIdx default_idx = o42_fmt_table_default (o42_sheet_fmt_table (sheet));
  char *name = g_markup_escape_text (o42_sheet_get_name (sheet), -1);
  int last_col, last_row;

  o42_sheet_used_range (sheet, &used);
  last_col = used.col1 >= used.col0 ? used.col1 : -1;
  last_row = used.row1 >= used.row0 ? used.row1 : -1;
  /* A picture, shape or chart hangs from a cell, and OpenDocument
   * writes it inside that cell, so the table has to reach it. */
  {
    GPtrArray *pictures = o42_sheet_pictures (sheet);
    GPtrArray *shapes = o42_sheet_shapes (sheet);
    GPtrArray *charts = o42_sheet_charts (sheet);

    for (guint i = 0; i < pictures->len; i++)
      {
        const O42Picture *pic = g_ptr_array_index (pictures, i);

        last_row = MAX (last_row, pic->row);
        last_col = MAX (last_col, pic->col);
      }
    for (guint i = 0; i < shapes->len; i++)
      {
        const O42Shape *shape = g_ptr_array_index (shapes, i);

        last_row = MAX (last_row, shape->row);
        last_col = MAX (last_col, shape->col);
      }
    for (guint i = 0; i < charts->len; i++)
      {
        const O42Chart *chart = g_ptr_array_index (charts, i);

        last_row = MAX (last_row, chart->row);
        last_col = MAX (last_col, chart->col);
      }
  }

  /* Columns and rows with a width or height of their own count too. */
  for (int c = 0; c < O42_MAX_COLS - 1; c++)
    if (o42_sheet_col_width (sheet, c) != default_width || o42_sheet_col_hidden (sheet, c))
      last_col = MAX (last_col, c);
  for (int r = 0; r < MIN (O42_MAX_ROWS - 1, 4096); r++)
    if (o42_sheet_row_height (sheet, r) != default_height || o42_sheet_row_hidden (sheet, r))
      last_row = MAX (last_row, r);

  g_string_append_printf (out, "<table:table table:name=\"%s\">", name);
  g_free (name);

  /* Columns, in runs of the same width. */
  for (int c = 0; c <= last_col; )
    {
      int width = o42_sheet_col_width (sheet, c);
      gboolean hidden = o42_sheet_col_hidden (sheet, c);
      int n = 1;
      while (c + n <= last_col && o42_sheet_col_width (sheet, c + n) == width && o42_sheet_col_hidden (sheet, c + n) == hidden)
        n++;
      g_string_append_printf (out, "<table:table-column table:style-name=\"%s\"", col_style (s, hidden ? default_width : width));
      if (n > 1) g_string_append_printf (out, " table:number-columns-repeated=\"%d\"", n);
      if (hidden) g_string_append (out, " table:visibility=\"collapse\"");
      g_string_append (out, " table:default-cell-style-name=\"Default\"/>");
      c += n;
    }
  if (last_col < 0)
    g_string_append_printf (out, "<table:table-column table:style-name=\"%s\" table:default-cell-style-name=\"Default\"/>", col_style (s, default_width));

  for (int r = 0; r <= last_row; r++)
    {
      int height = o42_sheet_row_height (sheet, r);
      gboolean hidden = o42_sheet_row_hidden (sheet, r);
      int last_in_row = -1;

      for (int c = 0; c <= last_col; c++)
        {
          char *input = o42_sheet_get_input (sheet, r, c);
          if ((input != NULL && *input != '\0') || o42_sheet_get_fmt_idx (sheet, r, c) != default_idx ||
              o42_sheet_get_note (sheet, r, c) != NULL || merge_at_head (sheet, r, c) != NULL ||
              merge_covers (sheet, r, c) || cell_has_drawings (sheet, r, c))
            last_in_row = c;
          g_free (input);
        }

      g_string_append_printf (out, "<table:table-row table:style-name=\"%s\"", row_style (s, hidden ? default_height : height));
      if (hidden) g_string_append (out, " table:visibility=\"collapse\"");
      g_string_append_c (out, '>');
      for (int c = 0; c <= last_in_row; c++)
        write_cell (out, s, sheet, sheet_index, r, c, default_idx);
      if (last_in_row < 0)
        g_string_append (out, "<table:table-cell/>");
      g_string_append (out, "</table:table-row>");
    }
  if (last_row < 0)
    g_string_append (out, "<table:table-row><table:table-cell/></table:table-row>");
  g_string_append (out, "</table:table>");
}

static void
write_names (GString *out, O42Book *book)
{
  GList *names = o42_book_names (book);
  if (names == NULL)
    return;
  g_string_append (out, "<table:named-expressions>");
  for (GList *l = names; l != NULL; l = l->next)
    {
      O42Sheet *sheet = NULL;
      O42Range range;
      if (o42_book_lookup_name (book, l->data, &sheet, &range) && sheet != NULL)
        {
          char *ename = g_markup_escape_text (l->data, -1);
          GString *addr = g_string_new (NULL);
          char *a = o42_ref_name_full (range.row0, range.col0, TRUE, TRUE);
          char *b = o42_ref_name_full (range.row1, range.col1, TRUE, TRUE);
          of_sheet_prefix (o42_sheet_get_name (sheet), addr);
          g_string_append (addr, a);
          if (range.row1 != range.row0 || range.col1 != range.col0)
            {
              g_string_append_c (addr, ':');
              of_sheet_prefix (o42_sheet_get_name (sheet), addr);
              g_string_append (addr, b);
            }
          {
            char *eaddr = g_markup_escape_text (addr->str, -1);
            g_string_append_printf (out, "<table:named-range table:name=\"%s\" table:base-cell-address=\"%s\" table:cell-range-address=\"%s\"/>",
                                    ename, eaddr, eaddr);
            g_free (eaddr);
          }
          g_string_free (addr, TRUE);
          g_free (a); g_free (b); g_free (ename);
        }
    }
  g_string_append (out, "</table:named-expressions>");
  g_list_free (names);
}

static void
write_settings (GString *out, O42Book *book)
{
  g_string_append (out,
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-settings " NS_HEAD "><office:settings>"
    "<config:config-item-set config:name=\"ooo:view-settings\"><config:config-item-map-indexed config:name=\"Views\">"
    "<config:config-item-map-entry><config:config-item config:name=\"ViewId\" config:type=\"string\">view1</config:config-item>"
    "<config:config-item-map-named config:name=\"Tables\">");
  for (int i = 0; i < o42_book_n_sheets (book); i++)
    {
      O42Sheet *sheet = o42_book_sheet (book, i);
      int rows = 0, cols = 0;
      char *name = g_markup_escape_text (o42_sheet_get_name (sheet), -1);
      o42_sheet_get_frozen (sheet, &rows, &cols);
      g_string_append_printf (out, "<config:config-item-map-entry config:name=\"%s\">", name);
      if (rows > 0 || cols > 0)
        g_string_append_printf (out,
          "<config:config-item config:name=\"HorizontalSplitMode\" config:type=\"short\">%d</config:config-item>"
          "<config:config-item config:name=\"VerticalSplitMode\" config:type=\"short\">%d</config:config-item>"
          "<config:config-item config:name=\"HorizontalSplitPosition\" config:type=\"int\">%d</config:config-item>"
          "<config:config-item config:name=\"VerticalSplitPosition\" config:type=\"int\">%d</config:config-item>"
          "<config:config-item config:name=\"PositionLeft\" config:type=\"int\">0</config:config-item>"
          "<config:config-item config:name=\"PositionRight\" config:type=\"int\">%d</config:config-item>"
          "<config:config-item config:name=\"PositionTop\" config:type=\"int\">0</config:config-item>"
          "<config:config-item config:name=\"PositionBottom\" config:type=\"int\">%d</config:config-item>",
          cols > 0 ? 2 : 0, rows > 0 ? 2 : 0, cols, rows, cols, rows);
      g_string_append (out, "</config:config-item-map-entry>");
      g_free (name);
    }
  g_string_append (out, "</config:config-item-map-named></config:config-item-map-entry></config:config-item-map-indexed>"
                        "</config:config-item-set></office:settings></office:document-settings>");
}

/* ---- The parts a drawing needs beside content.xml -------------------- */

/* A chart in OpenDocument is a document of its own inside the zip,
 * named by the frame that shows it.  This writes the smallest one that
 * LibreOffice and Excel both draw: the plot type, the titles, and the
 * series as ranges of the sheet the chart reads. */
static char *
ods_chart_document (O42Sheet *sheet, const O42Chart *chart)
{
  static const char *const KINDS[] = {
    "chart:bar", "chart:line", "chart:circle", "chart:bar", "chart:area",
    "chart:scatter", "chart:bar", "chart:bar"
  };
  GString *out = g_string_new (
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content " NS_HEAD ">"
    "<office:body><office:chart>");
  const O42Range *d = &chart->data;
  const char *source = (chart->data_sheet != NULL && chart->data_sheet[0] != '\0')
                       ? chart->data_sheet : o42_sheet_get_name (sheet);
  int first_row = d->row0 + (chart->first_row_labels ? 1 : 0);
  int first_col = d->col0 + (chart->first_col_labels ? 1 : 0);
  char *title = g_markup_escape_text (chart->title != NULL ? chart->title : "", -1);

  g_string_append_printf (out,
    "<chart:chart chart:class=\"%s\" svg:width=\"%.3fcm\" svg:height=\"%.3fcm\">",
    KINDS[chart->kind <= O42_CHART_PERCENT ? chart->kind : 0],
    chart->width * PX_TO_CM, chart->height * PX_TO_CM);
  if (*title != '\0')
    g_string_append_printf (out, "<chart:title><text:p>%s</text:p></chart:title>", title);
  g_string_append (out, "<chart:plot-area>");
  g_string_append (out, "<chart:axis chart:dimension=\"x\" chart:name=\"primary-x\"/>");
  g_string_append (out, "<chart:axis chart:dimension=\"y\" chart:name=\"primary-y\"/>");

  /* One series per column (or per row, when the series lie that way). */
  {
    int lines = chart->series_in_rows ? (d->row1 - first_row + 1) : (d->col1 - first_col + 1);

    for (int i = 0; i < lines; i++)
      {
        GString *address = g_string_new (NULL);
        char *a, *b, *escaped;
        int r0, c0, r1, c1;

        if (chart->series_in_rows)
          { r0 = r1 = first_row + i; c0 = first_col; c1 = d->col1; }
        else
          { c0 = c1 = first_col + i; r0 = first_row; r1 = d->row1; }

        a = o42_ref_name_full (r0, c0, TRUE, TRUE);
        b = o42_ref_name_full (r1, c1, TRUE, TRUE);
        of_sheet_prefix (source, address);
        g_string_append (address, a);
        g_string_append_c (address, ':');
        of_sheet_prefix (source, address);
        g_string_append (address, b);
        escaped = g_markup_escape_text (address->str, -1);
        g_string_append_printf (out,
          "<chart:series chart:values-cell-range-address=\"%s\" chart:class=\"%s\"/>",
          escaped, KINDS[chart->kind <= O42_CHART_PERCENT ? chart->kind : 0]);
        g_free (escaped);
        g_free (a);
        g_free (b);
        g_string_free (address, TRUE);
      }
  }
  g_string_append (out, "</chart:plot-area></chart:chart></office:chart></office:body>"
                        "</office:document-content>");
  g_free (title);
  return g_string_free (out, FALSE);
}

/* The pictures and chart documents of the whole book, with the
 * manifest entries that name them. */
static void
write_drawing_parts (O42ZipWriter *zip, O42Book *book, GString *manifest)
{
  for (int i = 0; i < o42_book_n_sheets (book); i++)
    {
      O42Sheet *sheet = o42_book_sheet (book, i);
      GPtrArray *pictures = o42_sheet_pictures (sheet);
      GPtrArray *charts = o42_sheet_charts (sheet);

      for (guint p = 0; p < pictures->len; p++)
        {
          const O42Picture *pic = g_ptr_array_index (pictures, p);
          const char *format = pic->format != NULL ? pic->format : "png";
          char *part = g_strdup_printf ("Pictures/sheet%d_image%u.%s", i + 1, p + 1, format);

          o42_zip_writer_add (zip, part, g_bytes_get_data (pic->data, NULL),
                              g_bytes_get_size (pic->data));
          g_string_append_printf (manifest,
            "<manifest:file-entry manifest:full-path=\"%s\" manifest:media-type=\"image/%s\"/>",
            part, strcmp (format, "jpg") == 0 ? "jpeg" : format);
          g_free (part);
        }

      for (guint c = 0; c < charts->len; c++)
        {
          const O42Chart *chart = g_ptr_array_index (charts, c);
          char *dir = g_strdup_printf ("Sheet%dChart%u", i + 1, c + 1);
          char *part = g_strdup_printf ("%s/content.xml", dir);
          char *xml = ods_chart_document (sheet, chart);

          o42_zip_writer_add (zip, part, xml, strlen (xml));
          g_string_append_printf (manifest,
            "<manifest:file-entry manifest:full-path=\"%s/\" manifest:version=\"1.2\" "
            "manifest:media-type=\"application/vnd.oasis.opendocument.chart\"/>"
            "<manifest:file-entry manifest:full-path=\"%s\" manifest:media-type=\"text/xml\"/>",
            dir, part);
          g_free (xml);
          g_free (part);
          g_free (dir);
        }
    }
}

gboolean
o42_ods_save (O42Book *book, GFile *file, GError **error)
{
  O42ZipWriter *zip;
  Styles s;
  GString *body = g_string_new (NULL);
  GString *content, *settings;
  GBytes *bytes;
  gboolean ok;

  g_return_val_if_fail (book != NULL && G_IS_FILE (file), FALSE);

  s.styles = g_string_new (NULL);
  s.col_styles = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  s.row_styles = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  s.cell_styles = g_hash_table_new_full (fmt_hash, fmt_equal, g_free, g_free);
  s.num_styles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  s.next_co = s.next_ro = s.next_ce = s.next_n = s.next_t = 0;

  for (int i = 0; i < o42_book_n_sheets (book); i++)
    write_table (body, &s, o42_book_sheet (book, i), i);
  write_names (body, book);

  content = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content " NS_HEAD ">");
  g_string_append (content, "<office:font-face-decls><style:font-face style:name=\"Arial\" svg:font-family=\"Arial\" xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\"/></office:font-face-decls>");
  g_string_append (content, "<office:automatic-styles>");
  g_string_append (content, s.styles->str);
  g_string_append (content, "</office:automatic-styles><office:body><office:spreadsheet>");
  g_string_append (content, body->str);
  g_string_append (content, "</office:spreadsheet></office:body></office:document-content>");

  settings = g_string_new (NULL);
  write_settings (settings, book);

  zip = o42_zip_writer_new ();
  o42_zip_writer_add_stored (zip, "mimetype", MIME, strlen (MIME));
  {
    GString *manifest = g_string_new (
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
      "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"" MIME "\"/>"
      "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
      "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
      "<manifest:file-entry manifest:full-path=\"settings.xml\" manifest:media-type=\"text/xml\"/>");

    static const char styles[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles " NS_HEAD ">"
      "<office:styles><style:default-style style:family=\"table-cell\">"
      "<style:text-properties fo:font-family=\"Arial\" fo:font-size=\"10pt\"/></style:default-style>"
      "<style:style style:name=\"Default\" style:family=\"table-cell\"/></office:styles>"
      "</office:document-styles>";

    write_drawing_parts (zip, book, manifest);
    g_string_append (manifest, "</manifest:manifest>");
    o42_zip_writer_add (zip, "META-INF/manifest.xml", manifest->str, manifest->len);
    g_string_free (manifest, TRUE);
    o42_zip_writer_add (zip, "styles.xml", styles, strlen (styles));
  }
  o42_zip_writer_add (zip, "content.xml", content->str, content->len);
  o42_zip_writer_add (zip, "settings.xml", settings->str, settings->len);
  bytes = o42_zip_writer_finish (zip);
  ok = g_file_replace_contents (file, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes),
                                NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, error);
  g_bytes_unref (bytes);

  g_string_free (content, TRUE);
  g_string_free (settings, TRUE);
  g_string_free (body, TRUE);
  g_string_free (s.styles, TRUE);
  g_hash_table_unref (s.col_styles);
  g_hash_table_unref (s.row_styles);
  g_hash_table_unref (s.cell_styles);
  g_hash_table_unref (s.num_styles);
  if (ok)
    o42_book_set_modified (book, FALSE);
  return ok;
}

/* ====================================================================== */
/* Reading                                                                */
/* ====================================================================== */

typedef struct {
  O42Fmt   fmt;
  gboolean has_fmt;        /* anything set beyond the default */
  char    *data_style;     /* number style name, resolved after all styles are read */
  int      width;          /* columns: px, or 0 */
  int      height;         /* rows: px, or 0 */
} Style;

typedef struct {
  O42NumberFormat number;
  int decimals;
  gboolean grouping;
} NumStyle;

typedef struct {
  O42Book    *book;
  O42Sheet   *sheet;
  int         n_tables;
  GHashTable *styles;        /* name -> Style */
  GHashTable *num_styles;    /* name -> NumStyle */
  Style      *style;         /* the style being read */
  NumStyle   *num;           /* the number style being read */
  gboolean    in_num_style;

  /* Columns of the current table, as read. */
  int         col;           /* next column index for table-column */
  GPtrArray  *col_styles;    /* per column: the default cell style name, or NULL */

  /* The row and cell being read. */
  int         row, cell_col;
  int         row_repeat;
  char       *row_style;
  gboolean    row_hidden;
  gboolean    in_cell, covered;
  int         cell_repeat;
  int         span_cols, span_rows;
  char       *cell_style;
  char       *formula;
  char       *cell_link;
  char       *value_type;
  char       *value;
  GString    *text;          /* the cell's paragraphs */
  int         paragraphs;
  gboolean    in_p;
  gboolean    in_annotation;
  GString    *note;
  int         note_paragraphs;
  int         depth_in_cell;
  GHashTable *parts;         /* the zip, for the pictures a frame names */
  GArray     *cell_runs;     /* O42TextRun for the cell being read */
  O42Shape   *shape;         /* the shape being read, for its text */
  double      frame_x, frame_y, frame_w, frame_h;   /* the frame being read */

  /* Frozen panes from settings.xml. */
  char       *setting_table;
  char       *setting_name;
  GString    *setting_value;
  int         split_cols, split_rows, hmode, vmode;
} Reader;

static const char *
local (const char *name)
{
  const char *colon = strrchr (name, ':');
  return colon != NULL ? colon + 1 : name;
}

static const char *
attr (const char **names, const char **values, const char *want)
{
  for (int i = 0; names[i] != NULL; i++)
    if (strcmp (local (names[i]), want) == 0)
      return values[i];
  return NULL;
}

static int
attr_int (const char **names, const char **values, const char *want, int fallback)
{
  const char *v = attr (names, values, want);
  return v != NULL ? atoi (v) : fallback;
}

/* "2.54cm", "1in", "12pt", "25.4mm" -> pixels at 96 dpi. */
static int
length_px (const char *text)
{
  double n;
  char *end = NULL;
  if (text == NULL)
    return 0;
  n = g_ascii_strtod (text, &end);
  if (end == NULL || end == text)
    return 0;
  if (strcmp (end, "cm") == 0) return (int) (n / 2.54 * 96 + 0.5);
  if (strcmp (end, "mm") == 0) return (int) (n / 25.4 * 96 + 0.5);
  if (strcmp (end, "in") == 0) return (int) (n * 96 + 0.5);
  if (strcmp (end, "pt") == 0) return (int) (n / 72 * 96 + 0.5);
  if (strcmp (end, "px") == 0) return (int) (n + 0.5);
  return (int) (n + 0.5);
}

/* "0.75pt solid #ff0000": width, kind and colour. */
static void
ods_border_parse (const char *text, guint8 *style, guint32 *colour)
{
  double width = g_ascii_strtod (text, NULL);
  const char *unit = text;
  while (*unit != '\0' && (g_ascii_isdigit (*unit) || *unit == '.')) unit++;
  if (g_str_has_prefix (unit, "mm")) width *= 72 / 25.4;
  else if (g_str_has_prefix (unit, "cm")) width *= 72 / 2.54;
  else if (g_str_has_prefix (unit, "in")) width *= 72;
  else if (g_str_has_prefix (unit, "px")) width *= 0.75;
  if (strstr (text, "double") != NULL) *style = O42_BORDER_DOUBLE;
  else if (strstr (text, "dashed") != NULL || strstr (text, "dash") != NULL) *style = O42_BORDER_DASHED;
  else if (strstr (text, "dotted") != NULL) *style = O42_BORDER_DOTTED;
  else if (width >= 2.2) *style = O42_BORDER_THICK;
  else if (width >= 1.2) *style = O42_BORDER_MEDIUM;
  else *style = O42_BORDER_THIN;
  {
    const char *hash = strchr (text, '#');
    *colour = hash != NULL && strlen (hash) >= 7 ? (guint32) g_ascii_strtoull (hash + 1, NULL, 16) : 0;
  }
}

static guint32
colour_of (const char *text, guint32 fallback)
{
  if (text != NULL && text[0] == '#' && strlen (text) == 7)
    return (guint32) g_ascii_strtoull (text + 1, NULL, 16);
  return fallback;
}

static void
style_free (gpointer data)
{
  Style *s = data;
  g_free (s->data_style);
  g_free (s);
}

/* ---- Formulas from OpenFormula notation ------------------------------- */

/* [.A1] -> A1, [.A1:.B2] -> A1:B2, [$Sheet.A1] -> Sheet!A1,
 * ['My Sheet'.A1:.B2] -> 'My Sheet'!A1:B2; ';' between arguments
 * becomes ','; arrays {1;2|3;4} become {1,2;3,4}; TRUE() is TRUE. */
static char *
formula_from_of (const char *of)
{
  GString *out = g_string_new ("=");
  const char *p = of;
  int braces = 0;

  if (g_str_has_prefix (p, "of:")) p += 3;
  else if (g_str_has_prefix (p, "oooc:")) p += 5;
  if (*p == '=') p++;

  while (*p != '\0')
    {
      if (*p == '"')
        {
          g_string_append_c (out, *p++);
          while (*p != '\0')
            {
              g_string_append_c (out, *p);
              if (*p == '"' && p[1] == '"') { g_string_append_c (out, '"'); p += 2; continue; }
              if (*p++ == '"') break;
            }
          continue;
        }
      if (*p == '[')
        {
          const char *end = strchr (p, ']');
          char *inside, *colon;
          if (end == NULL) { g_string_append_c (out, *p++); continue; }
          inside = g_strndup (p + 1, end - p - 1);
          /* Each side: [$'sheet'.]REF or .REF */
          {
            char *sides[2] = { inside, NULL };
            colon = NULL;
            {
              /* The ':' separating the two sides is outside quotes. */
              gboolean q = FALSE;
              for (char *c = inside; *c != '\0'; c++)
                {
                  if (*c == '\'') q = !q;
                  else if (*c == ':' && !q) { colon = c; break; }
                }
            }
            if (colon != NULL) { *colon = '\0'; sides[1] = colon + 1; }
            {
              /* Each side may name a sheet; two different ones make a
               * 3-D reference, First:Last!A1:B2. */
              char *snames[2] = { NULL, NULL };
              const char *refs[2] = { NULL, NULL };
              for (int k = 0; k < 2 && sides[k] != NULL; k++)
                {
                  char *side = sides[k];
                  char *dot = NULL;
                  gboolean q = FALSE;
                  for (char *c = side; *c != '\0'; c++)
                    {
                      if (*c == '\'') q = !q;
                      else if (*c == '.' && !q) { dot = c; break; }
                    }
                  if (dot != NULL && dot != side)
                    {
                      char *sname = g_strndup (side + (side[0] == '$' ? 1 : 0), dot - side - (side[0] == '$' ? 1 : 0));
                      if (sname[0] == '\'' && strlen (sname) >= 2)
                        { sname[strlen (sname) - 1] = '\0'; memmove (sname, sname + 1, strlen (sname)); }
                      snames[k] = sname;
                    }
                  refs[k] = dot != NULL ? dot + 1 : side;
                }
              if (snames[0] != NULL)
                {
                  gboolean span = snames[1] != NULL && strcmp (snames[0], snames[1]) != 0;
                  char *pair = span ? g_strconcat (snames[0], ":", snames[1], NULL) : g_strdup (snames[0]);
                  if (strpbrk (pair, " -+") != NULL || (span && FALSE))
                    g_string_append_printf (out, "'%s'!", pair);
                  else
                    g_string_append_printf (out, "%s!", pair);
                  g_free (pair);
                }
              g_string_append (out, refs[0] != NULL ? refs[0] : "");
              if (sides[1] != NULL)
                {
                  g_string_append_c (out, ':');
                  g_string_append (out, refs[1] != NULL ? refs[1] : "");
                }
              g_free (snames[0]);
              g_free (snames[1]);
            }
          }
          g_free (inside);
          p = end + 1;
          continue;
        }
      if (*p == '{') { braces++; g_string_append_c (out, *p++); continue; }
      if (*p == '}') { braces--; g_string_append_c (out, *p++); continue; }
      if (*p == ';') { g_string_append_c (out, braces > 0 ? ',' : ','); p++; continue; }
      if (*p == '|' && braces > 0) { g_string_append_c (out, ';'); p++; continue; }
      if (g_str_has_prefix (p, "TRUE()")) { g_string_append (out, "TRUE"); p += 6; continue; }
      if (g_str_has_prefix (p, "FALSE()")) { g_string_append (out, "FALSE"); p += 7; continue; }
      g_string_append_c (out, *p++);
    }
  return g_string_free (out, FALSE);
}

/* ---- content.xml ------------------------------------------------------ */

static void
apply_named_style (Reader *r, const char *name, int row0, int col0, int row1, int col1)
{
  Style *st = name != NULL ? g_hash_table_lookup (r->styles, name) : NULL;
  O42Range range = { row0, col0, row1, col1 };
  O42Fmt fmt;
  if (st == NULL || !st->has_fmt || r->sheet == NULL)
    return;
  fmt = st->fmt;
  if (st->data_style != NULL)
    {
      NumStyle *ns = g_hash_table_lookup (r->num_styles, st->data_style);
      if (ns != NULL)
        {
          fmt.number = ns->number == O42_NUM_FIXED && ns->grouping ? O42_NUM_COMMA : ns->number;
          fmt.decimals = ns->decimals;
        }
    }
  o42_sheet_apply_fmt (r->sheet, &range, O42_FMT_ALL, &fmt);
}

static void
cell_finish (Reader *r)
{
  int repeat = MAX (r->cell_repeat, 1);
  char *input = NULL;

  /* Whatever runs were gathered belong to this cell alone. */
  #define CLEAR_RUNS() g_clear_pointer (&r->cell_runs, g_array_unref)

  if (r->sheet == NULL || r->covered)
    {
      r->cell_col += repeat;
      CLEAR_RUNS ();
      return;
    }
  if (r->formula != NULL)
    input = formula_from_of (r->formula);
  else if (r->value_type != NULL)
    {
      const char *t = r->value_type;
      if ((strcmp (t, "float") == 0 || strcmp (t, "percentage") == 0 || strcmp (t, "currency") == 0) && r->value != NULL)
        {
          char buf[G_ASCII_DTOSTR_BUF_SIZE];
          input = g_strdup (g_ascii_dtostr (buf, sizeof buf, g_ascii_strtod (r->value, NULL)));
        }
      else if (strcmp (t, "boolean") == 0 && r->value != NULL)
        input = g_strdup (strcmp (r->value, "true") == 0 ? "=TRUE" : "=FALSE");
      else if (strcmp (t, "date") == 0 && r->value != NULL)
        {
          int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
          if (sscanf (r->value, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &hh, &mm, &ss) >= 3)
            {
              char buf[G_ASCII_DTOSTR_BUF_SIZE];
              double serial = o42_date_serial (y, m, d) + o42_time_fraction (hh, mm, ss);
              input = g_strdup (g_ascii_dtostr (buf, sizeof buf, serial));
            }
        }
      else if (strcmp (t, "time") == 0 && r->value != NULL)
        {
          int hh = 0, mm = 0;
          double ss = 0;
          const char *v = r->value;
          if (v[0] == 'P') v++;
          if (v[0] == 'T') v++;
          {
            char *end;
            while (*v != '\0')
              {
                double n = g_ascii_strtod (v, &end);
                if (end == v) break;
                if (*end == 'H') hh = (int) n; else if (*end == 'M') mm = (int) n; else if (*end == 'S') ss = n;
                v = *end != '\0' ? end + 1 : end;
              }
          }
          {
            char buf[G_ASCII_DTOSTR_BUF_SIZE];
            input = g_strdup (g_ascii_dtostr (buf, sizeof buf, o42_time_fraction (hh, mm, ss)));
          }
        }
    }
  if (input == NULL && r->text != NULL && r->text->len > 0)
    {
      const char *text = r->text->str;
      O42Value probe = o42_value_text (text);
      double n;
      O42ErrorCode err = O42_ERR_VALUE;
      if (*text == '=' || *text == '\'' || o42_value_to_number (&probe, &n, &err) || o42_date_parse (text, NULL, NULL, NULL))
        input = g_strconcat ("'", text, NULL);
      else
        input = g_strdup (text);
      o42_value_clear (&probe);
    }

  for (int k = 0; k < repeat && r->cell_col + k < O42_MAX_COLS; k++)
    {
      int col = r->cell_col + k;
      if (input != NULL && r->row < O42_MAX_ROWS)
        o42_sheet_set_input (r->sheet, r->row, col, input);
      if (r->note != NULL && r->note->len > 0)
        o42_sheet_set_note (r->sheet, r->row, col, r->note->str);
      if (r->cell_link != NULL)
        o42_sheet_set_link (r->sheet, r->row, col, r->cell_link);
      if (r->cell_runs != NULL && r->cell_runs->len > 0 && r->row < O42_MAX_ROWS)
        o42_sheet_set_runs (r->sheet, r->row, col,
                            &g_array_index (r->cell_runs, O42TextRun, 0),
                            (int) r->cell_runs->len);
      if (r->span_cols > 1 || r->span_rows > 1)
        {
          O42Range m = { r->row, col, MIN (r->row + r->span_rows - 1, O42_MAX_ROWS - 1), MIN (col + r->span_cols - 1, O42_MAX_COLS - 1) };
          o42_sheet_merge (r->sheet, &m);
        }
      if (repeat <= 64 || input != NULL)
        {
          const char *sname = r->cell_style;
          if (sname == NULL && col < (int) r->col_styles->len)
            sname = g_ptr_array_index (r->col_styles, col);
          apply_named_style (r, sname, r->row, col, r->row, col);
        }
    }
  g_free (input);
  r->cell_col += repeat;
  CLEAR_RUNS ();
  #undef CLEAR_RUNS

}

static void
row_finish (Reader *r)
{
  int repeat = MAX (r->row_repeat, 1);
  if (r->sheet != NULL)
    {
      Style *st = r->row_style != NULL ? g_hash_table_lookup (r->styles, r->row_style) : NULL;
      int last = MIN (r->row + repeat, O42_MAX_ROWS);
      if (repeat <= 512 || r->cell_col > 0)
        for (int k = r->row; k < last; k++)
          {
            if (st != NULL && st->height > 0) o42_sheet_set_row_height (r->sheet, k, st->height);
            if (r->row_hidden) o42_sheet_set_row_hidden (r->sheet, k, TRUE);
          }
    }
  r->row += repeat;
}

/* A length as OpenDocument writes it -- 3.5cm, 42mm, 12pt, 1in -- in
 * the pixels the grid is laid out in. */
static double
ods_length (const char *text)
{
  double value;
  char *end = NULL;

  if (text == NULL)
    return 0;
  value = g_ascii_strtod (text, &end);
  if (end == NULL)
    return 0;
  while (*end == ' ')
    end++;
  if (g_str_has_prefix (end, "cm")) return value / 2.54 * 96.0;
  if (g_str_has_prefix (end, "mm")) return value / 25.4 * 96.0;
  if (g_str_has_prefix (end, "in")) return value * 96.0;
  if (g_str_has_prefix (end, "pt")) return value / 72.0 * 96.0;
  if (g_str_has_prefix (end, "pc")) return value / 6.0 * 96.0;
  return value;
}

/* ---- Reading a chart back --------------------------------------------- */

/* A chart in OpenDocument is a document of its own inside the zip; the
 * frame that shows it points at the directory it lives in.  This reads
 * as much of one as office42 draws: the kind, the title, and the cells
 * the series come from. */
typedef struct {
  O42ChartKind kind;
  gboolean     kind_known;
  GString     *title;
  gboolean     in_title;
  gboolean     in_paragraph;
  O42Range     box;
  gboolean     have_box;
  char        *sheet_name;   /* the sheet the series read */
  int          series;
} OdsChartReader;

/* "Sheet1.$B$2:.$B$5" or "Sheet1.B2:Sheet1.B5" into a rectangle. */
static gboolean
ods_range (const char *address, char **sheet_out, O42Range *out)
{
  char **halves;
  gboolean ok = TRUE;

  if (address == NULL)
    return FALSE;
  halves = g_strsplit (address, ":", 2);
  for (int i = 0; halves[i] != NULL && i < 2 && ok; i++)
    {
      char *text = halves[i];
      char *dot = strrchr (text, '.');
      int row, col;

      if (dot != NULL)
        {
          if (i == 0 && sheet_out != NULL && *sheet_out == NULL && dot > text)
            {
              char *name = g_strndup (text, dot - text);
              char *cleaned = g_strdup (name[0] == '$' ? name + 1 : name);

              g_free (*sheet_out);
              *sheet_out = cleaned;
              g_free (name);
            }
          text = dot + 1;
        }
      {
        GString *plain = g_string_new (NULL);

        for (const char *p = text; *p != '\0'; p++)
          if (*p != '$')
            g_string_append_c (plain, *p);
        ok = o42_ref_parse (plain->str, &row, &col, NULL);
        if (ok)
          {
            if (i == 0)
              { out->row0 = out->row1 = row; out->col0 = out->col1 = col; }
            else
              { out->row1 = row; out->col1 = col; }
          }
        g_string_free (plain, TRUE);
      }
    }
  g_strfreev (halves);
  return ok;
}

static void
ods_chart_start (GMarkupParseContext *ctx, const char *element, const char **names,
                 const char **values, gpointer user, GError **error)
{
  OdsChartReader *c = user;
  const char *name = local (element);
  (void) ctx; (void) error;

  if (strcmp (name, "chart") == 0)
    {
      const char *class_name = attr (names, values, "class");

      if (class_name != NULL)
        {
          const char *kind = local (class_name);

          c->kind_known = TRUE;
          if (strcmp (kind, "line") == 0) c->kind = O42_CHART_LINE;
          else if (strcmp (kind, "circle") == 0) c->kind = O42_CHART_PIE;
          else if (strcmp (kind, "ring") == 0) c->kind = O42_CHART_DOUGHNUT;
          else if (strcmp (kind, "area") == 0) c->kind = O42_CHART_AREA;
          else if (strcmp (kind, "scatter") == 0) c->kind = O42_CHART_SCATTER;
          else if (strcmp (kind, "radar") == 0 || strcmp (kind, "filled-radar") == 0)
            c->kind = O42_CHART_RADAR;
          else if (strcmp (kind, "bubble") == 0) c->kind = O42_CHART_BUBBLE;
          else c->kind = O42_CHART_COLUMN;
        }
    }
  else if (strcmp (name, "title") == 0)
    c->in_title = TRUE;
  else if (strcmp (name, "p") == 0)
    c->in_paragraph = TRUE;
  else if (strcmp (name, "series") == 0)
    {
      O42Range r;

      if (ods_range (attr (names, values, "values-cell-range-address"), &c->sheet_name, &r))
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
          c->series++;
        }
    }
}

static void
ods_chart_end (GMarkupParseContext *ctx, const char *element, gpointer user, GError **error)
{
  OdsChartReader *c = user;
  const char *name = local (element);
  (void) ctx; (void) error;

  if (strcmp (name, "title") == 0) c->in_title = FALSE;
  else if (strcmp (name, "p") == 0) c->in_paragraph = FALSE;
}

static void
ods_chart_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer user, GError **error)
{
  OdsChartReader *c = user;
  (void) ctx; (void) error;
  if (c->in_title && c->in_paragraph)
    g_string_append_len (c->title, text, (gssize) len);
}

/* Adds the chart the frame points at to the sheet, anchored where the
 * frame is. */
static void
read_chart_object (Reader *r, const char *href, int row, int col,
                   double dx, double dy, double width, double height)
{
  static const GMarkupParser parser = { ods_chart_start, ods_chart_end, ods_chart_text, NULL, NULL };
  OdsChartReader c;
  char *part;
  GBytes *bytes;

  if (href == NULL || r->parts == NULL)
    return;
  while (*href == '.' || *href == '/')
    href++;
  part = g_strdup_printf ("%s/content.xml", href);
  bytes = g_hash_table_lookup (r->parts, part);
  if (bytes == NULL)
    {
      g_free (part);
      return;
    }

  memset (&c, 0, sizeof c);
  c.kind = O42_CHART_COLUMN;
  c.title = g_string_new (NULL);
  {
    GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &c, NULL);
    gsize size = 0;
    const char *data = g_bytes_get_data (bytes, &size);

    g_markup_parse_context_parse (ctx, data, (gssize) size, NULL);
    g_markup_parse_context_end_parse (ctx, NULL);
    g_markup_parse_context_free (ctx);
  }

  if (c.have_box)
    {
      /* The heading above or beside the numbers is part of the table
       * the chart is drawn from, as it was when it was written. */
      O42Range box = c.box;
      O42Chart *chart;

      if (box.row0 > 0)
        box.row0--;
      if (box.col0 > 0)
        box.col0--;
      chart = o42_sheet_add_chart (r->sheet, c.kind, &box, row, col);
      if (chart != NULL)
        {
          chart->first_row_labels = TRUE;
          chart->first_col_labels = TRUE;
          chart->dx = dx;
          chart->dy = dy;
          if (width > 4) chart->width = width;
          if (height > 4) chart->height = height;
          if (c.title->len > 0)
            {
              g_free (chart->title);
              chart->title = g_strdup (c.title->str);
            }
          if (c.sheet_name != NULL &&
              g_ascii_strcasecmp (c.sheet_name, o42_sheet_get_name (r->sheet)) != 0)
            {
              g_free (chart->data_sheet);
              chart->data_sheet = g_strdup (c.sheet_name);
            }
        }
    }
  g_string_free (c.title, TRUE);
  g_free (c.sheet_name);
  g_free (part);
}

static void
content_start (GMarkupParseContext *ctx, const char *element, const char **names,
               const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  const char *name = local (element);
  (void) ctx; (void) error;

  if (r->in_cell)
    {
      r->depth_in_cell++;
      if (strcmp (name, "span") == 0)
        {
          /* Part of the text set in a style of its own. */
          const char *style = attr (names, values, "style-name");
          Style *span = style != NULL ? g_hash_table_lookup (r->styles, style) : NULL;
          Style *cell = r->cell_style != NULL
                        ? g_hash_table_lookup (r->styles, r->cell_style) : NULL;
          O42TextRun run;
          O42Fmt plain;

          if (r->cell_runs == NULL)
            r->cell_runs = g_array_new (FALSE, FALSE, sizeof (O42TextRun));
          o42_fmt_init_default (&plain);
          run.start = (int) r->text->len;
          run.fmt = cell != NULL ? cell->fmt : plain;
          if (span != NULL)
            {
              /* A text style says only what it changes, so anything it
               * left at the default keeps what the cell has. */
              const O42Fmt *f = &span->fmt;

              if (f->family != plain.family) run.fmt.family = f->family;
              if (f->size != plain.size) run.fmt.size = f->size;
              if (f->bold != plain.bold) run.fmt.bold = f->bold;
              if (f->italic != plain.italic) run.fmt.italic = f->italic;
              if (f->underline != plain.underline) run.fmt.underline = f->underline;
              if (f->strikeout != plain.strikeout) run.fmt.strikeout = f->strikeout;
              if (f->colour != plain.colour) run.fmt.colour = f->colour;
            }
          g_array_append_val (r->cell_runs, run);
        }
      else if (strcmp (name, "frame") == 0)
        {
          r->frame_x = ods_length (attr (names, values, "x"));
          r->frame_y = ods_length (attr (names, values, "y"));
          r->frame_w = ods_length (attr (names, values, "width"));
          r->frame_h = ods_length (attr (names, values, "height"));
        }
      else if (strcmp (name, "object") == 0 && r->parts != NULL)
        read_chart_object (r, attr (names, values, "href"), r->row, r->cell_col,
                           r->frame_x, r->frame_y, r->frame_w, r->frame_h);
      else if (strcmp (name, "image") == 0 && r->parts != NULL)
        {
          /* The picture is a file in the zip the frame points at. */
          const char *href = attr (names, values, "href");
          GBytes *data = NULL;

          if (href != NULL)
            {
              while (*href == '.' || *href == '/')
                href++;
              data = g_hash_table_lookup (r->parts, href);
            }
          if (data != NULL)
            {
              int pw, ph;
              const char *format;

              if (o42_image_probe (data, &pw, &ph, &format))
                {
                  O42Picture *pic = o42_sheet_add_picture (r->sheet, data, format, pw, ph,
                                                           r->row, r->cell_col);

                  if (pic != NULL)
                    {
                      pic->dx = r->frame_x;
                      pic->dy = r->frame_y;
                      if (r->frame_w > 1) pic->width = r->frame_w;
                      if (r->frame_h > 1) pic->height = r->frame_h;
                    }
                }
            }
        }
      else if (strcmp (name, "rect") == 0 || strcmp (name, "ellipse") == 0 ||
               strcmp (name, "circle") == 0 || strcmp (name, "line") == 0)
        {
          O42ShapeKind kind = strcmp (name, "line") == 0 ? O42_SHAPE_LINE
                              : (name[0] == 'r' ? O42_SHAPE_RECT : O42_SHAPE_OVAL);
          O42Shape *shape = o42_sheet_add_shape (r->sheet, kind, r->row, r->cell_col);

          if (shape != NULL)
            {
              if (kind == O42_SHAPE_LINE)
                {
                  double x1 = ods_length (attr (names, values, "x1"));
                  double y1 = ods_length (attr (names, values, "y1"));
                  double x2 = ods_length (attr (names, values, "x2"));
                  double y2 = ods_length (attr (names, values, "y2"));

                  shape->dx = MIN (x1, x2);
                  shape->dy = MIN (y1, y2);
                  shape->width = fabs (x2 - x1);
                  shape->height = fabs (y2 - y1);
                }
              else
                {
                  shape->dx = ods_length (attr (names, values, "x"));
                  shape->dy = ods_length (attr (names, values, "y"));
                  shape->width = ods_length (attr (names, values, "width"));
                  shape->height = ods_length (attr (names, values, "height"));
                }
              r->shape = shape;
            }
        }
      else if (strcmp (name, "annotation") == 0)
        {
          r->in_annotation = TRUE;
          if (r->note == NULL) r->note = g_string_new (NULL);
          g_string_truncate (r->note, 0);
          r->note_paragraphs = 0;
        }
      else if (strcmp (name, "p") == 0)
        {
          GString *target = r->in_annotation ? r->note : r->text;
          int *count = r->in_annotation ? &r->note_paragraphs : &r->paragraphs;
          if ((*count)++ > 0) g_string_append_c (target, '\n');
          r->in_p = TRUE;
        }
      else if (strcmp (name, "s") == 0 && r->in_p)
        {
          int n = attr_int (names, values, "c", 1);
          GString *target = r->in_annotation ? r->note : r->text;
          for (int i = 0; i < n; i++) g_string_append_c (target, ' ');
        }
      else if (strcmp (name, "a") == 0 && r->in_p && !r->in_annotation)
        {
          const char *href = attr (names, values, "href");
          if (href != NULL)
            {
              g_free (r->cell_link);
              if (href[0] == '#')
                {
                  /* #Sheet.A1 -> #Sheet!A1 */
                  char *dot;
                  r->cell_link = g_strdup (href);
                  dot = strrchr (r->cell_link, '.');
                  if (dot != NULL) *dot = '!';
                }
              else
                r->cell_link = g_strdup (href);
            }
        }
      else if (strcmp (name, "tab") == 0 && r->in_p)
        g_string_append_c (r->in_annotation ? r->note : r->text, '\t');
      else if (strcmp (name, "line-break") == 0 && r->in_p)
        g_string_append_c (r->in_annotation ? r->note : r->text, '\n');
      return;
    }

  /* Styles. */
  if (strcmp (name, "style") == 0 && attr (names, values, "family") != NULL)
    {
      const char *sname = attr (names, values, "name");
      Style *st = g_new0 (Style, 1);
      o42_fmt_init_default (&st->fmt);
      st->data_style = g_strdup (attr (names, values, "data-style-name"));
      if (st->data_style != NULL) st->has_fmt = TRUE;
      if (sname != NULL) g_hash_table_replace (r->styles, g_strdup (sname), st);
      else style_free (st);
      r->style = sname != NULL ? st : NULL;
      return;
    }
  if (r->style != NULL)
    {
      Style *st = r->style;
      if (strcmp (name, "table-column-properties") == 0)
        st->width = length_px (attr (names, values, "column-width"));
      else if (strcmp (name, "table-row-properties") == 0)
        st->height = length_px (attr (names, values, "row-height"));
      else if (strcmp (name, "table-cell-properties") == 0)
        {
          const char *bg = attr (names, values, "background-color");
          const char *wrap = attr (names, values, "wrap-option");
          const char *va = attr (names, values, "vertical-align");
          const char *border = attr (names, values, "border");
          st->has_fmt = TRUE;
          if (bg != NULL && strcmp (bg, "transparent") != 0) st->fmt.fill = colour_of (bg, O42_FILL_NONE);
          if (wrap != NULL && strcmp (wrap, "wrap") == 0) st->fmt.wrap = 1;
          if (va != NULL) st->fmt.valign = strcmp (va, "top") == 0 ? O42_VALIGN_TOP : strcmp (va, "middle") == 0 ? O42_VALIGN_MIDDLE : O42_VALIGN_BOTTOM;
          {
            const char *sides[4] = { "border-top", "border-bottom", "border-left", "border-right" };
            const char *rotation = attr (names, values, "rotation-angle");
            for (int i = 0; i < 4; i++)
              {
                const char *b = attr (names, values, sides[i]);
                if (b == NULL) b = border;
                if (b != NULL && strcmp (b, "none") != 0)
                  ods_border_parse (b, &st->fmt.border_style[i], &st->fmt.border_colour[i]);
              }
            o42_fmt_sync_borders (&st->fmt);
            if (rotation != NULL)
              {
                int rot = atoi (rotation);
                if (rot > 180) rot -= 360;
                st->fmt.rotation = (gint16) CLAMP (rot, -90, 90);
              }
          }
        }
      else if (strcmp (name, "paragraph-properties") == 0)
        {
          const char *ta = attr (names, values, "text-align");
          const char *ml = attr (names, values, "margin-left");
          if (ml != NULL)
            {
              st->has_fmt = TRUE;
              st->fmt.indent = (guint8) CLAMP ((int) (g_ascii_strtod (ml, NULL) * 4 + 0.5), 0, 15);
            }
          if (ta != NULL)
            {
              st->has_fmt = TRUE;
              st->fmt.halign = (strcmp (ta, "center") == 0) ? O42_HALIGN_CENTRE
                             : (strcmp (ta, "end") == 0 || strcmp (ta, "right") == 0) ? O42_HALIGN_RIGHT
                             : (strcmp (ta, "start") == 0 || strcmp (ta, "left") == 0) ? O42_HALIGN_LEFT : O42_HALIGN_GENERAL;
            }
        }
      else if (strcmp (name, "text-properties") == 0)
        {
          const char *w = attr (names, values, "font-weight");
          const char *fs = attr (names, values, "font-style");
          const char *ul = attr (names, values, "text-underline-style");
          const char *lt = attr (names, values, "text-line-through-style");
          const char *size = attr (names, values, "font-size");
          const char *family = attr (names, values, "font-family");
          const char *fname = attr (names, values, "font-name");
          const char *colour = attr (names, values, "color");
          st->has_fmt = TRUE;
          if (w != NULL && strcmp (w, "bold") == 0) st->fmt.bold = 1;
          if (fs != NULL && strcmp (fs, "italic") == 0) st->fmt.italic = 1;
          if (ul != NULL && strcmp (ul, "none") != 0) st->fmt.underline = 1;
          if (lt != NULL && strcmp (lt, "none") != 0) st->fmt.strikeout = 1;
          if (size != NULL && g_str_has_suffix (size, "pt")) st->fmt.size = (int) (g_ascii_strtod (size, NULL) * 2 + 0.5);
          if (family == NULL) family = fname;
          if (family != NULL)
            {
              char *clean = g_strdup (family);
              if (clean[0] == '\'') { memmove (clean, clean + 1, strlen (clean)); if (strlen (clean) > 0 && clean[strlen (clean) - 1] == '\'') clean[strlen (clean) - 1] = '\0'; }
              st->fmt.family = g_intern_string (clean);
              g_free (clean);
            }
          if (colour != NULL) st->fmt.colour = colour_of (colour, 0);
        }
      return;
    }

  /* Number styles. */
  if (strcmp (name, "number-style") == 0 || strcmp (name, "percentage-style") == 0 ||
      strcmp (name, "currency-style") == 0 || strcmp (name, "date-style") == 0 ||
      strcmp (name, "time-style") == 0 || strcmp (name, "text-style") == 0 ||
      strcmp (name, "boolean-style") == 0)
    {
      const char *sname = attr (names, values, "name");
      NumStyle *ns = g_new0 (NumStyle, 1);
      ns->number = strcmp (name, "percentage-style") == 0 ? O42_NUM_PERCENT
                 : strcmp (name, "currency-style") == 0 ? O42_NUM_CURRENCY
                 : strcmp (name, "date-style") == 0 ? O42_NUM_DATE
                 : strcmp (name, "time-style") == 0 ? O42_NUM_TIME
                 : strcmp (name, "text-style") == 0 ? O42_NUM_TEXT : O42_NUM_GENERAL;
      ns->decimals = 2;
      if (sname != NULL) g_hash_table_replace (r->num_styles, g_strdup (sname), ns);
      else g_free (ns);
      r->num = sname != NULL ? ns : NULL;
      r->in_num_style = TRUE;
      return;
    }
  if (r->in_num_style && r->num != NULL)
    {
      NumStyle *ns = r->num;
      if (strcmp (name, "number") == 0)
        {
          const char *dp = attr (names, values, "decimal-places");
          const char *grouping = attr (names, values, "grouping");
          if (ns->number == O42_NUM_GENERAL) ns->number = O42_NUM_FIXED;
          if (dp != NULL) ns->decimals = atoi (dp);
          else if (ns->number == O42_NUM_FIXED) ns->number = O42_NUM_GENERAL;
          if (grouping != NULL && strcmp (grouping, "true") == 0) ns->grouping = TRUE;
        }
      else if (strcmp (name, "scientific-number") == 0)
        {
          ns->number = O42_NUM_SCIENTIFIC;
          ns->decimals = attr_int (names, values, "decimal-places", 2);
        }
      else if (strcmp (name, "hours") == 0 && ns->number == O42_NUM_DATE)
        ns->number = O42_NUM_DATETIME;
      return;
    }

  /* Tables. */
  if (strcmp (name, "table") == 0)
    {
      const char *tname = attr (names, values, "name");
      if (r->n_tables == 0)
        {
          r->sheet = o42_book_sheet (r->book, 0);
          if (tname != NULL) o42_book_rename_sheet (r->book, 0, tname);
        }
      else
        r->sheet = o42_book_add_sheet (r->book, tname != NULL ? tname : "Sheet", -1);
      r->n_tables++;
      r->row = 0;
      r->col = 0;
      g_ptr_array_set_size (r->col_styles, 0);
      return;
    }
  if (strcmp (name, "table-column") == 0 && r->sheet != NULL)
    {
      int repeat = attr_int (names, values, "number-columns-repeated", 1);
      const char *sname = attr (names, values, "style-name");
      const char *cell_style_name = attr (names, values, "default-cell-style-name");
      const char *vis = attr (names, values, "visibility");
      Style *st = sname != NULL ? g_hash_table_lookup (r->styles, sname) : NULL;
      for (int k = 0; k < repeat && r->col + k < O42_MAX_COLS; k++)
        {
          if (st != NULL && st->width > 0) o42_sheet_set_col_width (r->sheet, r->col + k, st->width);
          if (vis != NULL && strcmp (vis, "collapse") == 0) o42_sheet_set_col_hidden (r->sheet, r->col + k, TRUE);
          if (repeat <= 256)
            {
              while ((int) r->col_styles->len <= r->col + k) g_ptr_array_add (r->col_styles, NULL);
              g_ptr_array_index (r->col_styles, r->col + k) =
                cell_style_name != NULL && strcmp (cell_style_name, "Default") != 0 ? g_strdup (cell_style_name) : NULL;
            }
        }
      r->col += repeat;
      return;
    }
  if (strcmp (name, "table-row") == 0 && r->sheet != NULL)
    {
      const char *vis = attr (names, values, "visibility");
      r->row_repeat = attr_int (names, values, "number-rows-repeated", 1);
      g_free (r->row_style);
      r->row_style = g_strdup (attr (names, values, "style-name"));
      r->row_hidden = vis != NULL && strcmp (vis, "collapse") == 0;
      r->cell_col = 0;
      return;
    }
  if ((strcmp (name, "table-cell") == 0 || strcmp (name, "covered-table-cell") == 0) && r->sheet != NULL)
    {
      r->in_cell = TRUE;
      r->depth_in_cell = 0;
      r->covered = name[0] == 'c';
      r->cell_repeat = attr_int (names, values, "number-columns-repeated", 1);
      r->span_cols = attr_int (names, values, "number-columns-spanned", 1);
      r->span_rows = attr_int (names, values, "number-rows-spanned", 1);
      g_free (r->cell_style);  r->cell_style = g_strdup (attr (names, values, "style-name"));
      g_free (r->formula);     r->formula = g_strdup (attr (names, values, "formula"));
      g_free (r->value_type);  r->value_type = g_strdup (attr (names, values, "value-type"));
      g_free (r->value);
      r->value = g_strdup (attr (names, values, "value"));
      if (r->value == NULL) r->value = g_strdup (attr (names, values, "date-value"));
      if (r->value == NULL) r->value = g_strdup (attr (names, values, "time-value"));
      if (r->value == NULL) r->value = g_strdup (attr (names, values, "boolean-value"));
      if (r->text == NULL) r->text = g_string_new (NULL);
      g_string_truncate (r->text, 0);
      r->paragraphs = 0;
      if (r->note != NULL) g_string_truncate (r->note, 0);
      r->note_paragraphs = 0;
      r->in_annotation = FALSE;
      g_clear_pointer (&r->cell_link, g_free);
      return;
    }
  if (strcmp (name, "named-range") == 0)
    {
      const char *nname = attr (names, values, "name");
      const char *addr = attr (names, values, "cell-range-address");
      if (nname != NULL && addr != NULL)
        {
          char *of = g_strdup_printf ("[%s]", addr);
          char *ours = formula_from_of (of);
          O42Node *tree = o42_formula_parse (ours + 1);
          if (tree != NULL && (tree->type == O42_NODE_REF || tree->type == O42_NODE_RANGE))
            {
              O42Sheet *sheet = tree->sheet != NULL ? o42_book_find_sheet (r->book, tree->sheet) : o42_book_sheet (r->book, 0);
              O42Range range;
              if (tree->type == O42_NODE_REF)
                { range.row0 = range.row1 = tree->as.ref.row; range.col0 = range.col1 = tree->as.ref.col; }
              else
                range = tree->as.range;
              if (sheet != NULL)
                o42_book_define_name (r->book, nname, sheet, &range);
            }
          o42_node_free (tree);
          g_free (ours);
          g_free (of);
        }
      return;
    }
}

static void
content_end (GMarkupParseContext *ctx, const char *element, gpointer user, GError **error)
{
  Reader *r = user;
  const char *name = local (element);
  (void) ctx; (void) error;

  if (r->in_cell && strcmp (name, "span") == 0 && r->cell_runs != NULL)
    {
      O42TextRun run;

      Style *cell = r->cell_style != NULL
                    ? g_hash_table_lookup (r->styles, r->cell_style) : NULL;

      run.start = (int) r->text->len;
      if (cell != NULL)
        run.fmt = cell->fmt;
      else
        o42_fmt_init_default (&run.fmt);
      g_array_append_val (r->cell_runs, run);
    }

  if (r->shape != NULL &&
      (strcmp (name, "rect") == 0 || strcmp (name, "ellipse") == 0 ||
       strcmp (name, "circle") == 0 || strcmp (name, "line") == 0))
    r->shape = NULL;

  if (r->in_cell)
    {
      if (r->depth_in_cell == 0)
        {
          cell_finish (r);
          r->in_cell = FALSE;
          return;
        }
      r->depth_in_cell--;
      if (strcmp (name, "p") == 0) r->in_p = FALSE;
      else if (strcmp (name, "annotation") == 0) r->in_annotation = FALSE;
      return;
    }
  if (strcmp (name, "style") == 0)
    r->style = NULL;
  else if (r->in_num_style && g_str_has_suffix (name, "-style"))
    { r->in_num_style = FALSE; r->num = NULL; }
  else if (strcmp (name, "table-row") == 0 && r->sheet != NULL)
    row_finish (r);
  else if (strcmp (name, "table") == 0)
    r->sheet = NULL;
}

static void
content_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (r->shape != NULL && r->in_p)
    {
      /* The words inside a shape belong to the shape, not the cell it
       * hangs from. */
      char *was = r->shape->text;

      r->shape->text = g_strconcat (was != NULL ? was : "", "", NULL);
      {
        char *more = g_strndup (text, len);
        char *both = g_strconcat (r->shape->text, more, NULL);

        g_free (r->shape->text);
        g_free (more);
        r->shape->text = both;
      }
      g_free (was);
      return;
    }
  if (r->in_cell && r->in_p)
    g_string_append_len (r->in_annotation ? r->note : r->text, text, (gssize) len);
}

/* ---- settings.xml: frozen panes --------------------------------------- */

static void
settings_start (GMarkupParseContext *ctx, const char *element, const char **names,
                const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  const char *name = local (element);
  (void) ctx; (void) error;
  if (strcmp (name, "config-item-map-entry") == 0 && attr (names, values, "name") != NULL)
    {
      g_free (r->setting_table);
      r->setting_table = g_strdup (attr (names, values, "name"));
      r->split_cols = r->split_rows = r->hmode = r->vmode = 0;
    }
  else if (strcmp (name, "config-item") == 0)
    {
      g_free (r->setting_name);
      r->setting_name = g_strdup (attr (names, values, "name"));
      if (r->setting_value == NULL) r->setting_value = g_string_new (NULL);
      g_string_truncate (r->setting_value, 0);
    }
}

static void
settings_end (GMarkupParseContext *ctx, const char *element, gpointer user, GError **error)
{
  Reader *r = user;
  const char *name = local (element);
  (void) ctx; (void) error;
  if (strcmp (name, "config-item") == 0 && r->setting_name != NULL && r->setting_value != NULL)
    {
      int v = atoi (r->setting_value->str);
      if (strcmp (r->setting_name, "HorizontalSplitMode") == 0) r->hmode = v;
      else if (strcmp (r->setting_name, "VerticalSplitMode") == 0) r->vmode = v;
      else if (strcmp (r->setting_name, "HorizontalSplitPosition") == 0) r->split_cols = v;
      else if (strcmp (r->setting_name, "VerticalSplitPosition") == 0) r->split_rows = v;
      g_clear_pointer (&r->setting_name, g_free);
    }
  else if (strcmp (name, "config-item-map-entry") == 0 && r->setting_table != NULL)
    {
      O42Sheet *sheet = o42_book_find_sheet (r->book, r->setting_table);
      if (sheet != NULL && (r->hmode == 2 || r->vmode == 2))
        o42_sheet_set_frozen (sheet, r->vmode == 2 ? r->split_rows : 0, r->hmode == 2 ? r->split_cols : 0);
      g_clear_pointer (&r->setting_table, g_free);
    }
}

static void
settings_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (r->setting_name != NULL && r->setting_value != NULL)
    g_string_append_len (r->setting_value, text, (gssize) len);
}

static gboolean
parse_part (GHashTable *parts, const char *path, const GMarkupParser *parser, Reader *r, GError **error)
{
  GBytes *bytes = g_hash_table_lookup (parts, path);
  GMarkupParseContext *ctx;
  gsize len;
  const char *xml;
  gboolean ok;

  if (bytes == NULL)
    return TRUE;
  xml = g_bytes_get_data (bytes, &len);
  ctx = g_markup_parse_context_new (parser, G_MARKUP_TREAT_CDATA_AS_TEXT, r, NULL);
  ok = g_markup_parse_context_parse (ctx, xml, (gssize) len, error) &&
       g_markup_parse_context_end_parse (ctx, error);
  g_markup_parse_context_free (ctx);
  return ok;
}

gboolean
o42_ods_load (O42Book *book, GFile *file, GError **error)
{
  static const GMarkupParser content_parser = { content_start, content_end, content_text, NULL, NULL };
  static const GMarkupParser settings_parser = { settings_start, settings_end, settings_text, NULL, NULL };
  GBytes *archive;
  GHashTable *parts;
  Reader r;
  gboolean ok;

  g_return_val_if_fail (book != NULL && G_IS_FILE (file), FALSE);

  archive = g_file_load_bytes (file, NULL, NULL, error);
  if (archive == NULL)
    return FALSE;
  parts = o42_zip_read (archive, error);
  g_bytes_unref (archive);
  if (parts == NULL)
    return FALSE;
  if (g_hash_table_lookup (parts, "content.xml") == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "This is not an OpenDocument spreadsheet.");
      g_hash_table_unref (parts);
      return FALSE;
    }

  o42_book_clear (book);
  memset (&r, 0, sizeof r);
  r.book = book;
  r.styles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, style_free);
  r.num_styles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  r.col_styles = g_ptr_array_new_with_free_func (g_free);

  /* LibreOffice keeps number styles in styles.xml; the same walker
   * reads them, finding no tables there. */
  r.parts = parts;
  ok = parse_part (parts, "styles.xml", &content_parser, &r, error) &&
       parse_part (parts, "content.xml", &content_parser, &r, error) &&
       parse_part (parts, "settings.xml", &settings_parser, &r, error);

  g_hash_table_unref (r.styles);
  g_hash_table_unref (r.num_styles);
  g_ptr_array_unref (r.col_styles);
  g_free (r.row_style); g_free (r.cell_style); g_free (r.formula); g_free (r.value_type); g_free (r.value);
  g_free (r.cell_link);
  if (r.text != NULL) g_string_free (r.text, TRUE);
  if (r.note != NULL) g_string_free (r.note, TRUE);
  g_free (r.setting_table); g_free (r.setting_name);
  if (r.setting_value != NULL) g_string_free (r.setting_value, TRUE);
  g_hash_table_unref (parts);

  for (int i = 0; i < o42_book_n_sheets (book); i++)
    o42_sheet_clear_undo (o42_book_sheet (book, i));
  o42_book_set_modified (book, FALSE);
  return ok;
}
