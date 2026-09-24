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
#include "o42-xlsx.h"

#include "o42-image.h"

#include "o42-zip.h"
#include "o42-sheet.h"
#include "o42-formula.h"
#include "o42-entry.h"
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
  "xmlns:form=\"urn:oasis:names:tc:opendocument:xmlns:form:1.0\" " \
  "xmlns:calcext=\"urn:org:documentfoundation:names:experimental:calc:xmlns:calcext:1.0\" " \
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
    return 11;
  switch (node->as.op.op)
    {
    case O42_OP_EQ: case O42_OP_NE: case O42_OP_LT:
    case O42_OP_GT: case O42_OP_LE: case O42_OP_GE: return 1;
    case O42_OP_CONCAT: return 2;
    case O42_OP_ADD: case O42_OP_SUB: return 3;
    case O42_OP_MUL: case O42_OP_DIV: return 4;
    case O42_OP_POW: return 5;
    /* OpenFormula binds ":" tighter than "!", and "!" tighter than "~". */
    case O42_OP_RANGE: return 10;
    case O42_OP_ISECT: return 9;
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
    /* OpenFormula's reference operators: ~ joins, ! intersects, and a
     * scalar context intersects implicitly without being asked. */
    case O42_OP_UNION: return "~";  case O42_OP_ISECT: return "!";
    case O42_OP_IMPLICIT: return "";
    case O42_OP_RANGE: return ":";
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
        char *a, *b;

        /* A:A is [.A:.A] in OpenFormula, as LibreOffice writes it. */
        if (node->abs & O42_WHOLE_COLS)
          {
            char letters[8];
            o42_col_name (node->as.range.col0, letters, sizeof letters);
            a = g_strdup_printf ("%s%s", (node->abs & O42_ABS_COL0) ? "$" : "", letters);
            o42_col_name (node->as.range.col1, letters, sizeof letters);
            b = g_strdup_printf ("%s%s", (node->abs & O42_ABS_COL1) ? "$" : "", letters);
          }
        else if (node->abs & O42_WHOLE_ROWS)
          {
            a = g_strdup_printf ("%s%d", (node->abs & O42_ABS_ROW0) ? "$" : "", node->as.range.row0 + 1);
            b = g_strdup_printf ("%s%d", (node->abs & O42_ABS_ROW1) ? "$" : "", node->as.range.row1 + 1);
          }
        else
          {
            a = o42_ref_name_full (node->as.range.row0, node->as.range.col0,
                                   (node->abs & O42_ABS_ROW0) != 0, (node->abs & O42_ABS_COL0) != 0);
            b = o42_ref_name_full (node->as.range.row1, node->as.range.col1,
                                   (node->abs & O42_ABS_ROW1) != 0, (node->abs & O42_ABS_COL1) != 0);
          }
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
  int         next_co, next_ro, next_ce, next_n, next_t, next_ta;
  GString    *page_layouts; /* styles.xml: a page layout per sheet */
  GString    *master_pages; /* and the master page that uses it, with the header and footer */
  GHashTable *hf_styles;    /* "B1I0U0S12F" -> "MT3", text styles the header parts wear */
  GString    *hf_style_xml;
  GString    *cond_styles;  /* styles.xml: the named styles conditional formats switch to */
  int         next_cond;
  GString    *fill_defs;    /* styles.xml: the gradients and hatches the shapes use */
} Styles;

/* A break in a row or column style: the same size, with
 * fo:break-before="page", under a name of its own. */
static const char *
break_style (Styles *s, int px, gboolean row)
{
  GHashTable *table = row ? s->row_styles : s->col_styles;
  char *name = g_hash_table_lookup (table, GINT_TO_POINTER (-px - 1));

  if (name == NULL)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      char *cm = g_strdup_printf ("%scm", g_ascii_formatd (buf, sizeof buf, "%.3f", px * 2.54 / 96.0));

      name = g_strdup_printf (row ? "rob%d" : "cob%d", row ? ++s->next_ro : ++s->next_co);
      if (row)
        g_string_append_printf (s->styles,
          "<style:style style:name=\"%s\" style:family=\"table-row\">"
          "<style:table-row-properties style:row-height=\"%s\" fo:break-before=\"page\" style:use-optimal-row-height=\"false\"/></style:style>", name, cm);
      else
        g_string_append_printf (s->styles,
          "<style:style style:name=\"%s\" style:family=\"table-column\">"
          "<style:table-column-properties fo:break-before=\"page\" style:column-width=\"%s\"/></style:style>", name, cm);
      g_hash_table_insert (table, GINT_TO_POINTER (-px - 1), name);
      g_free (cm);
    }
  return name;
}

/* A header part in Excel's notation as OpenDocument paragraphs: the
 * fields as text:page-number and the like, the styles as spans. */
static void
hf_part_xml (Styles *s, GString *out, const char *text)
{
  gboolean bold = FALSE, italic = FALSE, underline = FALSE;
  int size = 0;
  char *family = NULL;
  gboolean open = FALSE;

#define HF_SPAN() G_STMT_START {                                                    \
    if (open) { g_string_append (out, "</text:span>"); open = FALSE; }               \
    if (bold || italic || underline || size > 0 || family != NULL)                   \
      {                                                                              \
        char *key = g_strdup_printf ("B%dI%dU%dS%dF%s", bold, italic, underline, size, family != NULL ? family : ""); \
        char *sname = g_hash_table_lookup (s->hf_styles, key);                       \
        if (sname == NULL)                                                           \
          {                                                                          \
            sname = g_strdup_printf ("MT%u", g_hash_table_size (s->hf_styles) + 1);  \
            g_string_append_printf (s->hf_style_xml, "<style:style style:name=\"%s\" style:family=\"text\"><style:text-properties", sname); \
            if (bold) g_string_append (s->hf_style_xml, " fo:font-weight=\"bold\"");  \
            if (italic) g_string_append (s->hf_style_xml, " fo:font-style=\"italic\""); \
            if (underline) g_string_append (s->hf_style_xml, " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\""); \
            if (size > 0) g_string_append_printf (s->hf_style_xml, " fo:font-size=\"%dpt\"", size); \
            if (family != NULL) { char *e = g_markup_escape_text (family, -1); g_string_append_printf (s->hf_style_xml, " style:font-name=\"%s\"", e); g_free (e); } \
            g_string_append (s->hf_style_xml, "/></style:style>");                   \
            g_hash_table_insert (s->hf_styles, g_strdup (key), sname);               \
          }                                                                          \
        g_string_append_printf (out, "<text:span text:style-name=\"%s\">", sname);  \
        open = TRUE;                                                                 \
        g_free (key);                                                                \
      }                                                                              \
  } G_STMT_END

  g_string_append (out, "<text:p>");
  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p != '&' || p[1] == '\0')
        {
          char c[2] = { *p, 0 };
          char *e = g_markup_escape_text (c, -1);
          if (*p == '\n') g_string_append (out, "</text:p><text:p>");
          else g_string_append (out, e);
          g_free (e);
          continue;
        }
      p++;
      switch (g_ascii_toupper (*p))
        {
        case 'P': g_string_append (out, "<text:page-number>1</text:page-number>"); break;
        case 'N': g_string_append (out, "<text:page-count>1</text:page-count>"); break;
        case 'A': g_string_append (out, "<text:sheet-name>Sheet1</text:sheet-name>"); break;
        case 'D': g_string_append (out, "<text:date>2026-01-01</text:date>"); break;
        case 'T': g_string_append (out, "<text:time>00:00</text:time>"); break;
        case 'F': g_string_append (out, "<text:file-name text:display=\"name\">Book1</text:file-name>"); break;
        case 'Z': g_string_append (out, "<text:file-name text:display=\"path\"></text:file-name>"); break;
        case '&': g_string_append (out, "&amp;"); break;
        case 'B': bold = !bold; HF_SPAN (); break;
        case 'I': italic = !italic; HF_SPAN (); break;
        case 'U': underline = !underline; HF_SPAN (); break;
        case '"':
          {
            const char *close = strchr (p + 1, '"');
            char *spec = close != NULL ? g_strndup (p + 1, close - p - 1) : g_strdup (p + 1);
            char *comma = strchr (spec, ',');

            if (comma != NULL) *comma++ = '\0';
            g_free (family);
            family = (*spec != '\0' && strcmp (spec, "-") != 0) ? g_strdup (spec) : NULL;
            if (comma != NULL)
              {
                bold = strstr (comma, "old") != NULL;
                italic = strstr (comma, "talic") != NULL;
              }
            g_free (spec);
            p = close != NULL ? close : p + strlen (p) - 1;
            HF_SPAN ();
            break;
          }
        default:
          if (g_ascii_isdigit (*p))
            {
              size = atoi (p);
              while (g_ascii_isdigit (p[1])) p++;
              HF_SPAN ();
            }
          break;
        }
    }
  if (open)
    g_string_append (out, "</text:span>");
  g_string_append (out, "</text:p>");
  g_free (family);
#undef HF_SPAN
}

/* The header or footer of a master page: its three regions. */
static void
hf_xml (Styles *s, GString *out, const char *text, gboolean footer)
{
  GString *parts[3] = { g_string_new (NULL), g_string_new (NULL), g_string_new (NULL) };
  int which = 1;
  gboolean any = text != NULL && *text != '\0';

  for (const char *p = any ? text : ""; *p != '\0'; p++)
    {
      if (*p == '&' && (g_ascii_toupper (p[1]) == 'L' || g_ascii_toupper (p[1]) == 'C' || g_ascii_toupper (p[1]) == 'R'))
        { which = g_ascii_toupper (p[1]) == 'L' ? 0 : g_ascii_toupper (p[1]) == 'C' ? 1 : 2; p++; continue; }
      if (*p == '&' && p[1] == '&')
        { g_string_append (parts[which], "&&"); p++; continue; }
      g_string_append_c (parts[which], *p);
    }
  g_string_append_printf (out, "<style:%s%s>", footer ? "footer" : "header", any ? "" : " style:display=\"false\"");
  if (any)
    {
      static const char *const regions[3] = { "left", "center", "right" };
      for (int i = 0; i < 3; i++)
        {
          g_string_append_printf (out, "<style:region-%s>", regions[i]);
          hf_part_xml (s, out, parts[i]->str);
          g_string_append_printf (out, "</style:region-%s>", regions[i]);
        }
    }
  g_string_append_printf (out, "</style:%s>", footer ? "footer" : "header");
  for (int i = 0; i < 3; i++)
    g_string_free (parts[i], TRUE);
}

/* The page layout and master page of a sheet, from its Page Setup. */
static void
write_page_style (Styles *s, O42Sheet *sheet, int index)
{
  const O42PrintSetup *ps = o42_sheet_print_setup (sheet);
  char buf[8][G_ASCII_DTOSTR_BUF_SIZE];
  double pw, ph;
  gboolean has_header = ps->header != NULL && *ps->header != '\0';
  gboolean has_footer = ps->footer != NULL && *ps->footer != '\0';
  double header_h = MAX (ps->margin_top - ps->margin_header, 0);
  double footer_h = MAX (ps->margin_bottom - ps->margin_footer, 0);

  o42_paper_size (ps->paper, &pw, &ph);
  if (ps->landscape) { double t = pw; pw = ph; ph = t; }
#define CM(i, pt) g_ascii_formatd (buf[i], sizeof buf[i], "%.3f", (pt) * 2.54 / 72.0)
  g_string_append_printf (s->page_layouts,
    "<style:page-layout style:name=\"pm%d\"><style:page-layout-properties fo:page-width=\"%scm\" fo:page-height=\"%scm\" "
    "style:num-format=\"1\" style:print-orientation=\"%s\" fo:margin-top=\"%scm\" fo:margin-bottom=\"%scm\" "
    "fo:margin-left=\"%scm\" fo:margin-right=\"%scm\" style:print-page-order=\"%s\" style:first-page-number=\"%d\" ",
    index + 1, CM (0, pw), CM (1, ph), ps->landscape ? "landscape" : "portrait",
    CM (2, has_header ? ps->margin_header : ps->margin_top), CM (3, has_footer ? ps->margin_footer : ps->margin_bottom),
    CM (4, ps->margin_left), CM (5, ps->margin_right), ps->down_then_over ? "ttb" : "ltr", ps->first_page);
  if (ps->fit_wide > 0 || ps->fit_tall > 0)
    {
      if (ps->fit_wide > 0) g_string_append_printf (s->page_layouts, "style:scale-to-X=\"%d\" ", ps->fit_wide);
      if (ps->fit_tall > 0) g_string_append_printf (s->page_layouts, "style:scale-to-Y=\"%d\" ", ps->fit_tall);
    }
  else
    g_string_append_printf (s->page_layouts, "style:scale-to=\"%d%%\" ", ps->scale);
  g_string_append_printf (s->page_layouts, "style:table-centering=\"%s\" style:print=\"%s%s%s%s%s\"/>",
    ps->hcenter && ps->vcenter ? "both" : ps->hcenter ? "horizontal" : ps->vcenter ? "vertical" : "none",
    ps->draft ? "" : "objects charts drawings zero-values ",
    ps->gridlines ? "grid " : "", ps->headings ? "headers " : "",
    ps->notes != O42_PRINT_NOTES_NONE ? "annotations " : "", "formulas-0");
  /* The header and footer bands: from the paper's edge to the header,
   * then the header's own height, take the body to its margin. */
  g_string_append_printf (s->page_layouts,
    "<style:header-style><style:header-footer-properties fo:min-height=\"%scm\" fo:margin-left=\"0cm\" fo:margin-right=\"0cm\" fo:margin-bottom=\"0cm\"/></style:header-style>"
    "<style:footer-style><style:header-footer-properties fo:min-height=\"%scm\" fo:margin-left=\"0cm\" fo:margin-right=\"0cm\" fo:margin-top=\"0cm\"/></style:footer-style>"
    "</style:page-layout>", CM (6, has_header ? header_h : 0), CM (7, has_footer ? footer_h : 0));
#undef CM
  g_string_append_printf (s->master_pages, "<style:master-page style:name=\"MP%d\" style:page-layout-name=\"pm%d\">", index + 1, index + 1);
  hf_xml (s, s->master_pages, ps->header, FALSE);
  hf_xml (s, s->master_pages, ps->footer, TRUE);
  g_string_append (s->master_pages, "</style:master-page>");
}

static char *
length_cm (int px)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  return g_strdup_printf ("%scm", g_ascii_formatd (buf, sizeof buf, "%.3f", px * 2.54 / 96.0));
}

/* A shape's words as paragraphs, one per line, in a paragraph style
 * that aligns them and a text style that sets them; both styles are
 * the shape's own. */
static char *
shape_text_xml (Styles *s, const O42Shape *shape, int sheet_index, guint i)
{
  GString *out = g_string_new (NULL);
  char **lines = g_strsplit (shape->text != NULL ? shape->text : "", "\n", -1);
  O42HAlign ha = o42_shape_text_halign (shape);
  char size[G_ASCII_DTOSTR_BUF_SIZE];
  char *family = g_markup_escape_text (shape->font != NULL ? shape->font : "Arial", -1);

  g_string_append_printf (s->styles,
    "<style:style style:name=\"Pgr%d_%u\" style:family=\"paragraph\"><style:paragraph-properties fo:text-align=\"%s\"/></style:style>"
    "<style:style style:name=\"Tgr%d_%u\" style:family=\"text\"><style:text-properties fo:font-family=\"%s\" fo:font-size=\"%spt\" "
    "fo:font-weight=\"%s\" fo:font-style=\"%s\" fo:color=\"#%06x\"/></style:style>",
    sheet_index, i, ha == O42_HALIGN_LEFT ? "start" : ha == O42_HALIGN_RIGHT ? "end" : "center",
    sheet_index, i, family, g_ascii_formatd (size, sizeof size, "%g", shape->font_size > 0 ? shape->font_size : 10),
    shape->bold ? "bold" : "normal", shape->italic ? "italic" : "normal", shape->text_colour & 0xFFFFFF);
  for (int k = 0; lines[k] != NULL; k++)
    {
      char *e = g_markup_escape_text (lines[k], -1);
      g_string_append_printf (out, "<text:p text:style-name=\"Pgr%d_%u\"><text:span text:style-name=\"Tgr%d_%u\">%s</text:span></text:p>",
                              sheet_index, i, sheet_index, i, e);
      g_free (e);
    }
  g_strfreev (lines);
  g_free (family);
  return g_string_free (out, FALSE);
}

/* A table style carries the tab's colour and names the master page
 * the sheet prints on; ODF hangs it off table:style-name the way a
 * column hangs off a column style. */
static char *
table_style (Styles *s, guint32 colour, int index, gboolean shown)
{
  char *name = g_strdup_printf ("ta%d", ++s->next_ta);

  g_string_append_printf (s->styles,
    "<style:style style:name=\"%s\" style:family=\"table\" style:master-page-name=\"MP%d\">"
    "<style:table-properties table:display=\"%s\"", name, index + 1, shown ? "true" : "false");
  if (colour != O42_TAB_NO_COLOUR)
    g_string_append_printf (s->styles, " table:tab-color=\"#%06x\"", colour & 0xFFFFFF);
  g_string_append (s->styles, "/></style:style>");
  return name;
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
/* Whatever stood between two fields of a date code, written as ODF
 * wants it and the buffer emptied. */
static void
flush_literal (GString *out, GString *literal)
{
  char *escaped;

  if (literal->len == 0)
    return;
  escaped = g_markup_escape_text (literal->str, (gssize) literal->len);
  g_string_append_printf (out, "<number:text>%s</number:text>", escaped);
  g_free (escaped);
  g_string_truncate (literal, 0);
}

/* The language of a Windows LCID as a two-letter tag, for the handful
 * a date style can name.  An unknown one is left out. */
static const char *
language_tag (guint lang)
{
  switch (lang)
    {
    case 0x06: return "da";
    case 0x07: return "de";
    case 0x09: return "en";
    case 0x0A: return "es";
    case 0x0B: return "fi";
    case 0x0C: return "fr";
    case 0x10: return "it";
    case 0x13: return "nl";
    case 0x14: return "nb";
    case 0x15: return "pl";
    case 0x16: return "pt";
    case 0x1D: return "sv";
    default:   return "en";
    }
}

/* A custom date or time code, written as ODF writes one: an element
 * per field and number:text for everything between them.  ODF has no
 * place for a format string, so the shape of the code has to become
 * the shape of the style. */
static void
append_date_style (GString *out, const char *name, const char *code)
{
  const char *p = code;
  gboolean last_was_hour = FALSE, clock = FALSE, ampm = FALSE;
  guint lang = 0;
  GString *literal = g_string_new (NULL);

  for (const char *q = code; *q != 0; q++)
    if (g_ascii_tolower (*q) == 'h')
      clock = TRUE;
  if (strstr (code, "AM/PM") != NULL || strstr (code, "am/pm") != NULL)
    ampm = TRUE;
  for (const char *q = code; (q = strchr (q, '[')) != NULL; )
    {
      const char *close = strchr (q, ']'), *dash = NULL;

      if (close == NULL)
        break;
      if (q[1] == '$' && (dash = memchr (q, '-', (gsize) (close - q))) != NULL)
        lang = (guint) g_ascii_strtoull (dash + 1, NULL, 16);
      q = close + 1;
    }

  g_string_append_printf (out, "<number:%s-style style:name=\"%s\"",
                          clock && !strpbrk (code, "yYdD") ? "time" : "date", name);
  if (lang != 0)
    g_string_append_printf (out, " number:language=\"%s\"", language_tag (lang & 0x3FF));
  g_string_append (out, " number:automatic-order=\"false\">");

  while (*p != 0)
    {
      char c = *p;
      int run = 1;

      if (c == '[')
        {
          const char *close = strchr (p, ']');

          p = close != NULL ? close + 1 : p + 1;
          continue;
        }
      if (c == '"')
        {
          p++;
          while (*p != 0 && *p != '"')
            g_string_append_c (literal, *p++);
          if (*p == '"')
            p++;
          continue;
        }
      if (c == '\\')
        {
          if (p[1] != 0)
            g_string_append_c (literal, p[1]);
          p += 2;
          continue;
        }
      if (g_ascii_strncasecmp (p, "AM/PM", 5) == 0 || g_ascii_strncasecmp (p, "A/P", 3) == 0)
        {
          flush_literal (out, literal);
          g_string_append (out, "<number:am-pm/>");
          p += g_ascii_strncasecmp (p, "AM/PM", 5) == 0 ? 5 : 3;
          continue;
        }
      if (strchr ("yYmMdDhHsS", c) == NULL)
        {
          g_string_append_c (literal, c);
          p++;
          continue;
        }

      while (g_ascii_tolower (p[run]) == g_ascii_tolower (c))
        run++;
      flush_literal (out, literal);
      switch (g_ascii_tolower (c))
        {
        case 'y':
          g_string_append_printf (out, "<number:year%s/>", run >= 4 ? " number:style=\"long\"" : "");
          last_was_hour = FALSE;
          break;
        case 'm':
          {
            const char *after = p + run;
            gboolean minutes = last_was_hour;

            while (*after == ':' || *after == ' ' || *after == '.')
              after++;
            if (*after == 's' || *after == 'S')
              minutes = TRUE;
            if (minutes)
              g_string_append_printf (out, "<number:minutes%s/>", run >= 2 ? " number:style=\"long\"" : "");
            else if (run >= 3)
              g_string_append_printf (out, "<number:month number:textual=\"true\"%s/>",
                                      run >= 4 ? " number:style=\"long\"" : "");
            else
              g_string_append_printf (out, "<number:month%s/>", run >= 2 ? " number:style=\"long\"" : "");
            last_was_hour = FALSE;
          }
          break;
        case 'd':
          if (run >= 3)
            g_string_append_printf (out, "<number:day-of-week%s/>", run >= 4 ? " number:style=\"long\"" : "");
          else
            g_string_append_printf (out, "<number:day%s/>", run >= 2 ? " number:style=\"long\"" : "");
          last_was_hour = FALSE;
          break;
        case 'h':
          g_string_append_printf (out, "<number:hours%s/>", run >= 2 ? " number:style=\"long\"" : "");
          last_was_hour = TRUE;
          break;
        case 's':
          g_string_append_printf (out, "<number:seconds%s/>", run >= 2 ? " number:style=\"long\"" : "");
          last_was_hour = FALSE;
          break;
        default:
          break;
        }
      p += run;
    }
  flush_literal (out, literal);
  (void) ampm;
  g_string_append_printf (out, "</number:%s-style>",
                          clock && !strpbrk (code, "yYdD") ? "time" : "date");
  g_string_free (literal, TRUE);
}

/* The eight colours a format code can name, as ODF spells them. */
static const struct { const char *name; const char *hex; } CODE_COLOURS[] = {
  { "Black", "#000000" }, { "Blue", "#0000ff" }, { "Cyan", "#00ffff" }, { "Green", "#00ff00" },
  { "Magenta", "#ff00ff" }, { "Red", "#ff0000" }, { "White", "#ffffff" }, { "Yellow", "#ffff00" },
};

/* One section of a number code -- "#,##0.00", "[Red]($#,##0.00)",
 * "0.0%", "0.00E+00", "#,##0 \"kg\"" -- as the body of an ODF number
 * style: the literals as number:text, the digit places as
 * number:number, the colour as text properties and the currency
 * symbol where ODF keeps it.  FALSE for a shape ODF has no element
 * for: a fraction, a condition, General, @. */
static gboolean
number_section_body (GString *body, GString *props, const char *sec, gsize len,
                     gboolean *percent, gboolean *currency)
{
  GString *literal = g_string_new (NULL);
  const char *p = sec, *end = sec + len;
  gboolean have_number = FALSE, ok = TRUE;

  while (p < end && ok)
    {
      char c = *p;

      if (c == '"')
        {
          p++;
          while (p < end && *p != '"') g_string_append_c (literal, *p++);
          if (p < end) p++;
          continue;
        }
      if (c == '\\')
        {
          if (p + 1 < end)
            {
              const char *next = g_utf8_next_char (p + 1);
              g_string_append_len (literal, p + 1, next - (p + 1));
              p = next;
            }
          else
            p++;
          continue;
        }
      if (c == '_')
        {
          /* A pad the width of a character: a space is the nearest
           * ODF comes. */
          g_string_append_c (literal, ' ');
          p = p + 1 < end ? g_utf8_next_char (p + 1) : end;
          continue;
        }
      if (c == '*')
        {
          if (p + 1 < end)
            {
              const char *next = g_utf8_next_char (p + 1);
              char *esc = g_markup_escape_text (p + 1, next - (p + 1));
              flush_literal (body, literal);
              g_string_append_printf (body, "<number:fill-character>%s</number:fill-character>", esc);
              g_free (esc);
              p = next;
            }
          else
            p++;
          continue;
        }
      if (c == '[')
        {
          const char *close = memchr (p, ']', (gsize) (end - p));
          gboolean known = FALSE;

          if (close == NULL) { ok = FALSE; break; }
          if (p[1] == '$')
            {
              /* [$€-407]: the symbol, and the language after the dash. */
              const char *dash = memchr (p, '-', (gsize) (close - p));
              const char *sym_end = dash != NULL ? dash : close;
              char *esc = g_markup_escape_text (p + 2, sym_end - (p + 2));

              flush_literal (body, literal);
              g_string_append (body, "<number:currency-symbol");
              if (dash != NULL)
                {
                  guint lang = (guint) g_ascii_strtoull (dash + 1, NULL, 16);
                  if (lang != 0)
                    g_string_append_printf (body, " number:language=\"%s\"", language_tag (lang & 0x3FF));
                }
              g_string_append_printf (body, ">%s</number:currency-symbol>", esc);
              g_free (esc);
              *currency = TRUE;
              known = TRUE;
            }
          else
            for (guint i = 0; i < G_N_ELEMENTS (CODE_COLOURS); i++)
              if ((gsize) (close - p - 1) == strlen (CODE_COLOURS[i].name) &&
                  g_ascii_strncasecmp (p + 1, CODE_COLOURS[i].name, (gsize) (close - p - 1)) == 0)
                {
                  g_string_printf (props, "<style:text-properties fo:color=\"%s\"/>", CODE_COLOURS[i].hex);
                  known = TRUE;
                }
          if (!known) { ok = FALSE; break; }   /* a condition, or an elapsed time */
          p = close + 1;
          continue;
        }
      if (c == '%')
        {
          *percent = TRUE;
          g_string_append_c (literal, '%');
          p++;
          continue;
        }
      if (c == '#' || c == '0' || c == '?' || (c == '.' && p + 1 < end && strchr ("#0?", p[1]) != NULL))
        {
          /* The digit places: the integer ones, the point and the
           * decimal ones, with grouping commas among the former and
           * scaling commas after the last, and an exponent if one
           * follows. */
          int int_zeros = 0, dec_places = 0, dec_zeros = 0, scale = 0, exp_digits = 0;
          gboolean grouping = FALSE, seen_point = FALSE, exponent = FALSE, exp_plus = FALSE;
          const char *run_end = p;

          while (run_end < end && strchr ("#0?", *run_end) != NULL) run_end++;
          if (run_end < end && *run_end == '/')
            {
              /* A fraction: these places are the numerator's, what
               * follows the slash the denominator's places or the
               * denominator itself, and a number written just before,
               * with a space between, the whole part. */
              const char *q = run_end + 1;
              int num_places = (int) (run_end - p), den_digits = 0, whole = 0;
              double den_value = 0;
              char *prior = strstr (body->str, "<number:number ");

              while (q < end && strchr ("#0?", *q) != NULL) { den_digits++; q++; }
              if (den_digits == 0)
                while (q < end && g_ascii_isdigit (*q)) { den_value = den_value * 10 + (*q - '0'); q++; }
              if (den_digits == 0 && den_value == 0) { ok = FALSE; break; }
              if (have_number && prior != NULL && literal->len == 1 && literal->str[0] == ' ')
                {
                  g_string_truncate (body, (gsize) (prior - body->str));
                  g_string_truncate (literal, 0);
                  whole = 1;
                }
              else if (have_number) { ok = FALSE; break; }
              have_number = TRUE;
              flush_literal (body, literal);
              g_string_append_printf (body, "<number:fraction number:min-integer-digits=\"%d\" number:min-numerator-digits=\"%d\"",
                                      whole, num_places);
              if (den_digits > 0)
                g_string_append_printf (body, " number:min-denominator-digits=\"%d\"/>", den_digits);
              else
                g_string_append_printf (body, " number:denominator-value=\"%.0f\"/>", den_value);
              p = q;
              continue;
            }

          if (have_number) { ok = FALSE; break; }
          have_number = TRUE;
          flush_literal (body, literal);
          while (p < end)
            {
              if (*p == '#' || *p == '0' || *p == '?')
                {
                  if (seen_point) { dec_places++; if (*p == '0') dec_zeros++; }
                  else if (*p == '0') int_zeros++;
                  p++;
                }
              else if (*p == '.' && !seen_point)
                { seen_point = TRUE; p++; }
              else if (*p == ',')
                {
                  const char *q = p;
                  while (q < end && *q == ',') q++;
                  if (q < end && strchr ("#0?", *q) != NULL && !seen_point)
                    grouping = TRUE;
                  else
                    scale += (int) (q - p);
                  p = q;
                }
              else if ((*p == 'E' || *p == 'e') && p + 1 < end && (p[1] == '+' || p[1] == '-'))
                {
                  exponent = TRUE;
                  exp_plus = (p[1] == '+');
                  p += 2;
                  while (p < end && *p == '0') { exp_digits++; p++; }
                  break;
                }
              else
                break;
            }
          if (exponent)
            g_string_append_printf (body,
              "<number:scientific-number number:decimal-places=\"%d\" number:min-decimal-places=\"%d\" number:min-integer-digits=\"%d\" number:min-exponent-digits=\"%d\"%s/>",
              dec_places, dec_zeros, int_zeros, exp_digits, exp_plus ? " number:forced-exponent-sign=\"true\"" : "");
          else
            {
              g_string_append_printf (body,
                "<number:number number:decimal-places=\"%d\" number:min-decimal-places=\"%d\" number:min-integer-digits=\"%d\"",
                dec_places, dec_zeros, int_zeros);
              if (grouping)
                g_string_append (body, " number:grouping=\"true\"");
              if (scale > 0)
                g_string_append_printf (body, " number:display-factor=\"%.0f\"", pow (1000, scale));
              g_string_append (body, "/>");
            }
          continue;
        }
      if (c == '/' || c == '@' || g_ascii_strncasecmp (p, "General", 7) == 0)
        { ok = FALSE; break; }
      if (c == '$')
        {
          flush_literal (body, literal);
          g_string_append (body, "<number:currency-symbol>$</number:currency-symbol>");
          *currency = TRUE;
          p++;
          continue;
        }
      {
        const char *next = g_utf8_next_char (p);
        g_string_append_len (literal, p, next - p);
        p = next;
      }
    }
  flush_literal (body, literal);
  g_string_free (literal, TRUE);
  return ok && have_number;
}

/* A custom number code as ODF number styles: one style per section,
 * the positive and the negative ones marked volatile and the last
 * carrying the maps that choose among them by the value's sign --
 * the arrangement LibreOffice writes and reads.  FALSE, with nothing
 * written, for a code that will not go. */
static gboolean
append_number_style (GString *out, const char *name, const char *code)
{
  const char *starts[4];
  gsize lens[4];
  int count = 0, numeric;
  const char *p = code, *sec = code;
  GString *bodies[3], *props[3], *all;
  gboolean percent[3] = { FALSE, FALSE, FALSE }, currency[3] = { FALSE, FALSE, FALSE };
  gboolean ok = TRUE;

  /* The sections: positive, negative, zero, and a fourth for text
   * that has no place here. */
  for (;; p++)
    {
      if (*p == '"') { p++; while (*p != '\0' && *p != '"') p++; if (*p == '\0') break; continue; }
      if (*p == '[') { while (*p != '\0' && *p != ']') p++; if (*p == '\0') break; continue; }
      if (*p == ';' || *p == '\0')
        {
          if (count < 4) { starts[count] = sec; lens[count] = (gsize) (p - sec); count++; }
          sec = p + 1;
          if (*p == '\0') break;
        }
    }
  numeric = MIN (count, 3);
  if (numeric == 0)
    return FALSE;

  for (int i = 0; i < numeric; i++)
    {
      bodies[i] = g_string_new (NULL);
      props[i] = g_string_new (NULL);
      if (ok && !number_section_body (bodies[i], props[i], starts[i], lens[i], &percent[i], &currency[i]))
        ok = FALSE;
    }

  all = g_string_new (NULL);
  if (ok)
    {
      for (int i = 0; i < numeric; i++)
        {
          const char *kind = currency[i] ? "currency" : percent[i] ? "percentage" : "number";
          gboolean last = (i == numeric - 1);

          if (last)
            g_string_append_printf (all, "<number:%s-style style:name=\"%s\">%s%s", kind, name, props[i]->str, bodies[i]->str);
          else
            g_string_append_printf (all, "<number:%s-style style:name=\"%sP%d\" style:volatile=\"true\">%s%s</number:%s-style>",
                                    kind, name, i, props[i]->str, bodies[i]->str, kind);
          if (last && numeric == 2)
            g_string_append_printf (all, "<style:map style:condition=\"value()&gt;=0\" style:apply-style-name=\"%sP0\"/>", name);
          if (last && numeric == 3)
            g_string_append_printf (all, "<style:map style:condition=\"value()&gt;0\" style:apply-style-name=\"%sP0\"/>"
                                         "<style:map style:condition=\"value()&lt;0\" style:apply-style-name=\"%sP1\"/>", name, name);
          if (last)
            g_string_append_printf (all, "</number:%s-style>", kind);
        }
      g_string_append (out, all->str);
    }
  for (int i = 0; i < numeric; i++)
    {
      g_string_free (bodies[i], TRUE);
      g_string_free (props[i], TRUE);
    }
  g_string_free (all, TRUE);
  return ok;
}

/* Does a custom code draw a date or a time: a day, year, hour or
 * second letter outside quotes and brackets, and no digit place. */
static gboolean
code_is_dated (const char *code)
{
  gboolean dated = FALSE;

  for (const char *p = code; *p != '\0'; p++)
    {
      if (*p == '"') { p++; while (*p != '\0' && *p != '"') p++; if (*p == '\0') break; continue; }
      if (*p == '\\') { if (p[1] != '\0') p++; continue; }
      if (*p == '[') { while (*p != '\0' && *p != ']') p++; if (*p == '\0') break; continue; }
      if (strchr ("yYdDhHsS", *p) != NULL) dated = TRUE;
      if (strchr ("#0?", *p) != NULL) return FALSE;
    }
  return dated;
}

static const char *
num_style (Styles *s, const O42Fmt *fmt)
{
  char key[64];
  char *name;
  int decimals = fmt->decimals;

  if (fmt->number == O42_NUM_GENERAL && fmt->custom == NULL)
    return NULL;
  if (fmt->custom != NULL)
    g_snprintf (key, sizeof key, "c:%.60s", fmt->custom);
  else
    g_snprintf (key, sizeof key, "%d/%d", (int) fmt->number, decimals);
  name = g_hash_table_lookup (s->num_styles, key);
  if (name != NULL)
    return name;
  name = g_strdup_printf ("N%d", ++s->next_n);

  /* A code of its own, if it is one ODF can hold: a date or a time,
   * where the shape of the code becomes the shape of the style. */
  if (fmt->custom != NULL && (fmt->number == O42_NUM_DATE || fmt->number == O42_NUM_TIME ||
                              fmt->number == O42_NUM_DATETIME || code_is_dated (fmt->custom)))
    {
      append_date_style (s->styles, name, fmt->custom);
      g_hash_table_insert (s->num_styles, g_strdup (key), name);
      return name;
    }
  /* Any other code: its sections as styles, if ODF has the elements. */
  if (fmt->custom != NULL && append_number_style (s->styles, name, fmt->custom))
    {
      g_hash_table_insert (s->num_styles, g_strdup (key), name);
      return name;
    }

  switch (fmt->number)
    {
    case O42_NUM_FIXED:
    case O42_NUM_COMMA:
      g_string_append_printf (s->styles,
        "<number:number-style style:name=\"%s\"><number:number number:decimal-places=\"%d\" number:min-decimal-places=\"%d\" number:min-integer-digits=\"1\"%s/></number:number-style>",
        name, decimals, decimals, fmt->number == O42_NUM_COMMA ? " number:grouping=\"true\"" : "");
      break;
    case O42_NUM_CURRENCY:
    case O42_NUM_ACCOUNTING:
      {
        /* Accounting is a currency style with the fill character ODF
         * keeps for it, which is what pushes the symbol to the edge. */
        char *symbol = g_markup_escape_text (o42_numfmt_currency (), -1);
        g_string_append_printf (s->styles,
          "<number:currency-style style:name=\"%s\"><number:currency-symbol>%s</number:currency-symbol>%s"
          "<number:number number:decimal-places=\"%d\" number:min-decimal-places=\"%d\" number:min-integer-digits=\"1\" number:grouping=\"true\"/></number:currency-style>",
          name, symbol, fmt->number == O42_NUM_ACCOUNTING ? "<number:fill-character> </number:fill-character>" : "",
          decimals, decimals);
        g_free (symbol);
      }
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
  if (fmt->shrink)
    g_string_append (s->styles, " style:shrink-to-fit=\"true\"");
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
      {
        /* A line inside a cell is a break, not a paragraph, when the
         * text is set in more than one font: the runs are counted
         * against the whole string and paragraphs would cut them. */
        char **lines = g_strsplit (escaped, "\n", -1);
        GString *body = g_string_new (NULL);

        for (int k = 0; lines[k] != NULL; k++)
          {
            if (k > 0)
              g_string_append (body, "<text:line-break/>");
            g_string_append (body, lines[k]);
          }
        if (style != NULL)
          g_string_append_printf (out, "<text:span text:style-name=\"%s\">%s</text:span>",
                                  style, body->str);
        else
          g_string_append (out, body->str);
        g_string_free (body, TRUE);
        g_strfreev (lines);
      }
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

/* The far corner of an object as OpenDocument anchors it: the cell it
 * reaches and the offset within, for an object that moves and sizes
 * with the cells; nothing for one that does not.  Caller frees. */
static char *
ods_end_cell (O42Sheet *sheet, int row, int col, double dx, double dy, double w, double h,
              O42AnchorMode mode)
{
  double x1 = o42_sheet_col_offset (sheet, col) + dx + w;
  double y1 = o42_sheet_row_offset (sheet, row) + dy + h;
  int to_col, to_row;
  char *quoted, *ref, *attrs;

  if (mode != O42_ANCHOR_TWO_CELL)
    return g_strdup ("");
  to_col = MIN (o42_sheet_col_at (sheet, x1), O42_MAX_COLS - 1);
  to_row = MIN (o42_sheet_row_at (sheet, y1), O42_MAX_ROWS - 1);
  quoted = g_markup_escape_text (o42_sheet_get_name (sheet), -1);
  ref = o42_ref_name (to_row, to_col);
  attrs = g_strdup_printf (" table:end-cell-address=\"%s%s%s.%s\" table:end-x=\"%.3fcm\" table:end-y=\"%.3fcm\"",
                           strpbrk (quoted, " '.-") != NULL ? "&apos;" : "", quoted,
                           strpbrk (quoted, " '.-") != NULL ? "&apos;" : "", ref,
                           (x1 - o42_sheet_col_offset (sheet, to_col)) * PX_TO_CM,
                           (y1 - o42_sheet_row_offset (sheet, to_row)) * PX_TO_CM);
  g_free (quoted);
  g_free (ref);
  return attrs;
}

static void
append_frame_head (GString *out, const char *name, int z,
                   double dx, double dy, double w, double h, const char *end_cell)
{
  char *escaped = g_markup_escape_text (name, -1);

  g_string_append_printf (out,
    "<draw:frame draw:name=\"%s\" draw:z-index=\"%d\"%s "
    "svg:x=\"%.3fcm\" svg:y=\"%.3fcm\" svg:width=\"%.3fcm\" svg:height=\"%.3fcm\">",
    escaped, z, end_cell, dx * PX_TO_CM, dy * PX_TO_CM, w * PX_TO_CM, h * PX_TO_CM);
  g_free (escaped);
}

/* ---- Form controls ------------------------------------------------------ */

/* A reference as ODF writes it: the sheet's name, a dot, the cell.  A
 * name that is not a plain word is quoted, which is what the format
 * asks for and what LibreOffice writes. */
static char *
ods_ref (O42Sheet *sheet, const char *ref)
{
  const char *name = o42_sheet_get_name (sheet);
  gboolean plain = name != NULL && *name != 0;
  char *escaped, *out;

  if (ref == NULL || *ref == 0)
    return NULL;
  for (const char *p = name; plain && *p != 0; p++)
    if (!g_ascii_isalnum (*p) && *p != '_')
      plain = FALSE;
  {
    const char *colon = strchr (ref, ':');
    char *left = colon != NULL ? g_strndup (ref, (gsize) (colon - ref)) : g_strdup (ref);

    /* ODF names the sheet on both sides of a range. */
    if (plain)
      out = colon != NULL ? g_strdup_printf ("%s.%s:%s.%s", name, left, name, colon + 1)
                          : g_strdup_printf ("%s.%s", name, left);
    else
      out = colon != NULL ? g_strdup_printf ("$'%s'.%s:$'%s'.%s", name, left, name, colon + 1)
                          : g_strdup_printf ("$'%s'.%s", name, left);
    g_free (left);
  }
  escaped = g_markup_escape_text (out, -1);
  g_free (out);
  return escaped;
}

/* The controls of a sheet, as an ODF form.  Each one is named
 * "controlN" here and the draw:control in the cell points back at that
 * name; LibreOffice and Excel both read the pair. */
static void
write_forms (GString *out, O42Sheet *sheet)
{
  GPtrArray *shapes = o42_sheet_shapes (sheet);
  gboolean any = FALSE;

  for (guint i = 0; i < shapes->len; i++)
    if (o42_shape_is_control (((const O42Shape *) g_ptr_array_index (shapes, i))->kind))
      any = TRUE;
  if (!any)
    return;

  g_string_append (out,
    "<office:forms form:automatic-focus=\"false\" form:apply-design-mode=\"false\">"
    "<form:form form:name=\"Standard\" form:apply-filter=\"true\" "
    "form:control-implementation=\"ooo:com.sun.star.form.component.Form\" "
    "office:target-frame=\"\">");

  for (guint i = 0; i < shapes->len; i++)
    {
      const O42Shape *shape = g_ptr_array_index (shapes, i);
      char *label, *link, *source;
      double value = 0;
      gboolean has_value;

      if (!o42_shape_is_control (shape->kind))
        continue;
      label = g_markup_escape_text (shape->text != NULL ? shape->text : "", -1);
      link = ods_ref (sheet, shape->link);
      source = ods_ref (sheet, shape->source);
      has_value = o42_sheet_control_value (sheet, shape, &value);

      switch (shape->kind)
        {
        case O42_SHAPE_BUTTON:
          g_string_append_printf (out,
            "<form:button form:name=\"Control %u\" "
            "form:control-implementation=\"ooo:com.sun.star.form.component.CommandButton\" "
            "xml:id=\"control%u\" form:id=\"control%u\" "
            "form:label=\"%s\" form:image-position=\"center\"/>", i + 1, i + 1, i + 1, label);
          break;

        case O42_SHAPE_CHECKBOX:
        case O42_SHAPE_OPTION:
          {
            gboolean on = shape->kind == O42_SHAPE_CHECKBOX
              ? (has_value && value != 0)
              : (has_value && value == (shape->value != 0 ? shape->value : 1));

            g_string_append_printf (out,
              "<form:%s form:name=\"Control %u\" xml:id=\"control%u\" form:id=\"control%u\" "
              "form:label=\"%s\" form:image-position=\"center\" form:current-state=\"%s\"",
              shape->kind == O42_SHAPE_CHECKBOX ? "checkbox" : "radio",
              i + 1, i + 1, i + 1, label, on ? "checked" : "unchecked");
            if (link != NULL)
              g_string_append_printf (out, " form:linked-cell=\"%s\"", link);
            g_string_append_printf (out, "/>");
          }
          break;

        case O42_SHAPE_SPINNER:
        case O42_SHAPE_SCROLLBAR:
          g_string_append_printf (out,
            "<form:value-range form:name=\"Control %u\" xml:id=\"control%u\" form:id=\"control%u\" "
            "form:control-implementation=\"ooo:com.sun.star.form.component.%s\" "
            "form:orientation=\"%s\" form:value=\"%d\" form:min-value=\"%d\" "
            "form:max-value=\"%d\" form:step-size=\"%d\" form:page-step-size=\"%d\"",
            i + 1, i + 1, i + 1,
            shape->kind == O42_SHAPE_SPINNER ? "SpinButton" : "ScrollBar",
            shape->kind == O42_SHAPE_SCROLLBAR && shape->width >= shape->height
              ? "horizontal" : "vertical",
            (int) (has_value ? value : shape->min), (int) shape->min, (int) shape->max,
            (int) (shape->step > 0 ? shape->step : 1),
            (int) (shape->page > 0 ? shape->page : 10));
          if (link != NULL)
            g_string_append_printf (out, " form:linked-cell=\"%s\"", link);
          g_string_append (out, "/>");
          break;

        case O42_SHAPE_LISTBOX:
        case O42_SHAPE_COMBO:
          g_string_append_printf (out,
            "<form:listbox form:name=\"Control %u\" "
            "form:control-implementation=\"ooo:com.sun.star.form.component.ListBox\" "
            "xml:id=\"control%u\" form:id=\"control%u\" "
            "form:bound-column=\"1\"%s", i + 1, i + 1, i + 1,
            shape->kind == O42_SHAPE_COMBO ? " form:dropdown=\"true\" form:size=\"1\"" : "");
          if (source != NULL)
            g_string_append_printf (out, " form:source-cell-range=\"%s\"", source);
          if (link != NULL)
            g_string_append_printf (out, " form:linked-cell=\"%s\"", link);
          g_string_append (out, "/>");
          break;

        case O42_SHAPE_LABEL:
          g_string_append_printf (out,
            "<form:fixed-text form:name=\"Control %u\" "
            "form:control-implementation=\"ooo:com.sun.star.form.component.FixedText\" "
            "xml:id=\"control%u\" form:id=\"control%u\" "
            "form:label=\"%s\" form:multi-line=\"true\"/>", i + 1, i + 1, i + 1, label);
          break;

        case O42_SHAPE_GROUPBOX:
          g_string_append_printf (out,
            "<form:frame form:name=\"Control %u\" "
            "form:control-implementation=\"ooo:com.sun.star.form.component.GroupBox\" "
            "xml:id=\"control%u\" form:id=\"control%u\" "
            "form:label=\"%s\"/>", i + 1, i + 1, i + 1, label);
          break;

        default:
          break;
        }
      g_free (label);
      g_free (link);
      g_free (source);
    }

  g_string_append (out, "</form:form></office:forms>");
}

/* Everything anchored to one cell, drawn inside its element. */
/* ---- Data validation: table:content-validation ---- */

/* The validation over a cell as an index into the sheet's list, or -1. */
static int
validation_index_at (O42Sheet *sheet, int row, int col)
{
  GArray *rules = o42_sheet_validations (sheet);
  for (guint i = 0; i < rules->len; i++)
    if (o42_range_contains (&g_array_index (rules, O42Validation, i).range, row, col))
      return (int) i;
  return -1;
}

/* A rule's condition in OpenFormula's words: of:cell-content-is-whole-number()
 * and of:cell-content-is-between(1,10), of:cell-content-is-in-list("a";"b"). */
static char *
validation_condition (O42Sheet *sheet, const O42Validation *v)
{
  GString *c = g_string_new ("of:");
  static const char *const ops[] = { "", "", "=", "!=", ">", "<", ">=", "<=" };
  const char *a = v->value != NULL ? v->value : "", *b = v->value2 != NULL ? v->value2 : "";

  if (v->kind == O42_VALID_LIST)
    {
      O42Range r;
      gsize len = 0;
      const char *text = a;

      while (*text == '=' || *text == ' ') text++;
      if (o42_ref_parse (text, &r.row0, &r.col0, &len) &&
          (text[len] == '\0' || (text[len] == ':' && o42_ref_parse (text + len + 1, &r.row1, &r.col1, NULL))))
        {
          char *plain = g_strdup (text);
          char *colon = strchr (plain, ':');
          if (colon != NULL) *colon = '\0';
          g_string_append_printf (c, "cell-content-is-in-list([.%s%s%s])", plain, colon != NULL ? ":." : "", colon != NULL ? colon + 1 : "");
          g_free (plain);
        }
      else
        {
          char **items = o42_sheet_validation_items (sheet, v);
          g_string_append (c, "cell-content-is-in-list(");
          for (int i = 0; items[i] != NULL; i++)
            {
              char *esc = g_strescape (items[i], NULL);
              g_string_append_printf (c, "%s\"%s\"", i > 0 ? ";" : "", esc);
              g_free (esc);
            }
          g_string_append_c (c, ')');
          g_strfreev (items);
        }
      return g_string_free (c, FALSE);
    }
  if (v->kind == O42_VALID_LENGTH)
    {
      if (v->op == O42_COND_BETWEEN || v->op == O42_COND_NOT_BETWEEN)
        g_string_append_printf (c, "cell-content-text-length-is-%sbetween(%s,%s)", v->op == O42_COND_NOT_BETWEEN ? "not-" : "", a, b);
      else
        g_string_append_printf (c, "cell-content-text-length()%s%s", ops[v->op], a);
      return g_string_free (c, FALSE);
    }
  switch (v->kind)
    {
    case O42_VALID_WHOLE: g_string_append (c, "cell-content-is-whole-number()"); break;
    case O42_VALID_DECIMAL: g_string_append (c, "cell-content-is-decimal-number()"); break;
    case O42_VALID_DATE: g_string_append (c, "cell-content-is-date()"); break;
    case O42_VALID_TIME: g_string_append (c, "cell-content-is-time()"); break;
    default: break;
    }
  /* LibreOffice writes the second call without the of: prefix, and
   * reads only that form. */
  if (v->op == O42_COND_BETWEEN || v->op == O42_COND_NOT_BETWEEN)
    g_string_append_printf (c, " and cell-content-is-%sbetween(%s,%s)", v->op == O42_COND_NOT_BETWEEN ? "not-" : "", a, b);
  else
    g_string_append_printf (c, " and cell-content()%s%s", ops[v->op], a);
  return g_string_free (c, FALSE);
}

/* Every sheet's rules, named valS_I, before the tables. */
static void
write_validations (GString *out, O42Book *book)
{
  static const char *const kinds[] = { "stop", "warning", "information" };
  gboolean any = FALSE;

  for (int i = 0; i < o42_book_n_sheets (book); i++)
    if (o42_sheet_validations (o42_book_sheet (book, i))->len > 0)
      any = TRUE;
  if (!any)
    return;
  g_string_append (out, "<table:content-validations>");
  for (int i = 0; i < o42_book_n_sheets (book); i++)
    {
      O42Sheet *sheet = o42_book_sheet (book, i);
      GArray *rules = o42_sheet_validations (sheet);

      for (guint k = 0; k < rules->len; k++)
        {
          const O42Validation *v = &g_array_index (rules, O42Validation, k);
          char *cond = validation_condition (sheet, v);
          char *esc = g_markup_escape_text (cond, -1);
          char *base = o42_ref_name (v->range.row0, v->range.col0);
          char *sname = g_markup_escape_text (o42_sheet_get_name (sheet), -1);

          g_string_append_printf (out,
            "<table:content-validation table:name=\"val%d_%u\" table:condition=\"%s\" table:allow-empty-cell=\"%s\" "
            "table:base-cell-address=\"%s.%s\"%s>",
            i, k, esc, v->allow_blank ? "true" : "false", sname, base,
            v->kind == O42_VALID_LIST ? (v->no_dropdown ? " table:display-list=\"no\"" : " table:display-list=\"unsorted\"") : "");
          if ((v->prompt_title != NULL && *v->prompt_title) || (v->prompt != NULL && *v->prompt))
            {
              char *t = g_markup_escape_text (v->prompt_title ? v->prompt_title : "", -1);
              char *m = g_markup_escape_text (v->prompt ? v->prompt : "", -1);
              g_string_append_printf (out, "<table:help-message table:title=\"%s\" table:display=\"true\"><text:p>%s</text:p></table:help-message>", t, m);
              g_free (t); g_free (m);
            }
          {
            char *t = g_markup_escape_text (v->title ? v->title : "", -1);
            char *m = g_markup_escape_text (v->message ? v->message : "", -1);
            g_string_append_printf (out, "<table:error-message table:title=\"%s\" table:message-type=\"%s\" table:display=\"%s\"><text:p>%s</text:p></table:error-message>",
                                    t, kinds[CLAMP (v->style, 0, 2)], v->no_error ? "false" : "true", m);
            g_free (t); g_free (m);
          }
          g_string_append (out, "</table:content-validation>");
          g_free (cond); g_free (esc); g_free (base); g_free (sname);
        }
    }
  g_string_append (out, "</table:content-validations>");
}

static void
write_cell_drawings (GString *out, Styles *s, O42Sheet *sheet, int sheet_index, int row, int col)
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
      {
        char *end = ods_end_cell (sheet, pic->row, pic->col, pic->dx, pic->dy, pic->width, pic->height, pic->anchor);
        append_frame_head (out, name, (int) i, pic->dx, pic->dy, pic->width, pic->height, end);
        g_free (end);
      }
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
      char *end_cell;

      if (shape->row != row || shape->col != col)
        continue;
      if (o42_shape_is_control (shape->kind))
        {
          /* A control's box points at the form entry written above. */
          g_string_append_printf (out,
            "<draw:control draw:name=\"Control %u\" draw:z-index=\"%u\" "
            "svg:x=\"%.3fcm\" svg:y=\"%.3fcm\" svg:width=\"%.3fcm\" svg:height=\"%.3fcm\" "
            "draw:control=\"control%u\"/>",
            i + 1, 200 + i, shape->dx * PX_TO_CM, shape->dy * PX_TO_CM,
            shape->width * PX_TO_CM, shape->height * PX_TO_CM, i + 1);
          continue;
        }
      name = g_strdup_printf ("Shape %u", i + 1);
      text = shape_text_xml (s, shape, sheet_index, i);
      end_cell = ods_end_cell (sheet, shape->row, shape->col, shape->dx, shape->dy, shape->width, shape->height, shape->anchor);
      /* The shape's fill and line as a graphic style of its own; the
       * dashes and the heads are named in styles.xml. */
      {
        static const char *const DASH_NAMES[O42_N_DASHES] = {
          NULL, "Dash", "Dot", "Dash_20_Dot", "Long_20_Dash", "Short_20_Dash", "Short_20_Dot"
        };
        static const char *const HEAD_NAMES[O42_N_HEADS] = {
          NULL, "Triangle", "Stealth", "Diamond", "Circle", "Open_20_Arrow"
        };
        static const double HEAD_WIDTH[3] = { 0.2, 0.3, 0.45 };
        gboolean line_kind = shape->kind == O42_SHAPE_LINE || shape->kind == O42_SHAPE_ARROW;

        {
          gboolean filled = !line_kind && shape->fill != O42_FILL_NONE &&
                            (shape->kind != O42_SHAPE_FREEFORM || shape->closed);
          const char *fill = !filled ? "none" : shape->fill_kind == O42_SHAPE_FILL_GRADIENT ? "gradient"
                           : shape->fill_kind == O42_SHAPE_FILL_PATTERN ? "hatch" : "solid";

          g_string_append_printf (s->styles,
            "<style:style style:name=\"gr%d_%u\" style:family=\"graphic\"><style:graphic-properties "
            "draw:stroke=\"%s\" svg:stroke-width=\"%.3fcm\" svg:stroke-color=\"#%06x\" "
            "draw:fill=\"%s\" draw:fill-color=\"#%06x\"",
            sheet_index, i, shape->dash != O42_DASH_SOLID ? "dash" : "solid",
            shape->line_width * PX_TO_CM, shape->line & 0xFFFFFF, fill,
            (shape->fill != O42_FILL_NONE ? shape->fill : 0xFFFFFF) & 0xFFFFFF);
          if (filled && shape->fill_kind == O42_SHAPE_FILL_GRADIENT)
            {
              /* ODF's angle counts counter-clockwise in tenths of a degree
               * from a run bottom to top; ours clockwise from left to right. */
              int angle = (int) fmod (fmod (90 - shape->gradient_angle, 360) + 360, 360) * 10;

              g_string_append_printf (s->fill_defs,
                "<draw:gradient draw:name=\"Gr%d_%u\" draw:style=\"linear\" draw:start-color=\"#%06x\" draw:end-color=\"#%06x\" "
                "draw:start-intensity=\"100%%\" draw:end-intensity=\"100%%\" draw:angle=\"%d\" draw:border=\"0%%\"/>",
                sheet_index, i, shape->fill & 0xFFFFFF, shape->fill2 & 0xFFFFFF, angle);
              g_string_append_printf (s->styles, " draw:fill-gradient-name=\"Gr%d_%u\"", sheet_index, i);
            }
          else if (filled && shape->fill_kind == O42_SHAPE_FILL_PATTERN)
            {
              /* A hatch: lines at an angle, or two sets crossed, in the
               * pattern's colour over the fill. */
              const char *hstyle = "single";
              int rotation = 0;
              double distance = 0.1;

              switch (shape->pattern)
                {
                case O42_PATTERN_VERTICAL: case O42_PATTERN_THIN_VERTICAL: rotation = 900; break;
                case O42_PATTERN_UP: case O42_PATTERN_THIN_UP: rotation = 450; break;
                case O42_PATTERN_DOWN: case O42_PATTERN_THIN_DOWN: rotation = 1350; break;
                case O42_PATTERN_GRID: case O42_PATTERN_THIN_GRID: hstyle = "double"; break;
                case O42_PATTERN_TRELLIS: case O42_PATTERN_THIN_TRELLIS: hstyle = "double"; rotation = 450; break;
                case O42_PATTERN_GRAY75: case O42_PATTERN_GRAY50: hstyle = "triple"; distance = 0.05; break;
                case O42_PATTERN_GRAY25: case O42_PATTERN_GRAY125: case O42_PATTERN_GRAY0625: hstyle = "double"; distance = 0.15; break;
                default: break;
                }
              if (shape->pattern == O42_PATTERN_THIN_HORIZONTAL || shape->pattern == O42_PATTERN_THIN_VERTICAL ||
                  shape->pattern == O42_PATTERN_THIN_UP || shape->pattern == O42_PATTERN_THIN_DOWN ||
                  shape->pattern == O42_PATTERN_THIN_GRID || shape->pattern == O42_PATTERN_THIN_TRELLIS)
                distance = 0.2;
              g_string_append_printf (s->fill_defs,
                "<draw:hatch draw:name=\"Ha%d_%u\" draw:style=\"%s\" draw:color=\"#%06x\" draw:distance=\"%.2fcm\" draw:rotation=\"%d\"/>",
                sheet_index, i, hstyle, shape->fill2 & 0xFFFFFF, distance, rotation);
              g_string_append_printf (s->styles, " draw:fill-hatch-name=\"Ha%d_%u\" draw:fill-hatch-solid=\"true\"", sheet_index, i);
            }
          if (shape->shadow)
            g_string_append_printf (s->styles,
              " draw:shadow=\"visible\" draw:shadow-offset-x=\"%.3fcm\" draw:shadow-offset-y=\"%.3fcm\" draw:shadow-color=\"#%06x\"",
              shape->shadow_dx * PX_TO_CM, shape->shadow_dy * PX_TO_CM, shape->shadow_colour & 0xFFFFFF);
        }
        if (shape->dash != O42_DASH_SOLID)
          g_string_append_printf (s->styles, " draw:stroke-dash=\"%s\"", DASH_NAMES[shape->dash]);
        if (line_kind && shape->head_start != O42_HEAD_NONE)
          g_string_append_printf (s->styles, " draw:marker-start=\"%s\" draw:marker-start-width=\"%.2fcm\"",
                                  HEAD_NAMES[shape->head_start], HEAD_WIDTH[CLAMP (shape->head_start_size, 0, 2)]);
        if (line_kind && shape->head_end != O42_HEAD_NONE)
          g_string_append_printf (s->styles, " draw:marker-end=\"%s\" draw:marker-end-width=\"%.2fcm\"",
                                  HEAD_NAMES[shape->head_end], HEAD_WIDTH[CLAMP (shape->head_end_size, 0, 2)]);
        {
          O42HAlign ha = o42_shape_text_halign (shape);
          O42VAlign va = o42_shape_text_valign (shape);
          char pad[G_ASCII_DTOSTR_BUF_SIZE];

          g_string_append_printf (s->styles,
            " draw:textarea-horizontal-align=\"%s\" draw:textarea-vertical-align=\"%s\" fo:padding=\"%scm\"%s/></style:style>",
            ha == O42_HALIGN_LEFT ? "left" : ha == O42_HALIGN_RIGHT ? "right" : "center",
            va == O42_VALIGN_TOP ? "top" : va == O42_VALIGN_MIDDLE ? "middle" : "bottom",
            g_ascii_formatd (pad, sizeof pad, "%.3f", MAX (shape->text_inset, 0) * PX_TO_CM),
            shape->text_nowrap ? " fo:wrap-option=\"no-wrap\"" : " fo:wrap-option=\"wrap\"");
        }
      }
      /* A line is a line; a freeform is a polygon, a polyline or a path;
       * everything else is a box or an ellipse, and the text inside it
       * goes in a paragraph as it does anywhere. */
      if (shape->kind == O42_SHAPE_FREEFORM && shape->path != NULL)
        {
          /* Points in a view box of hundredths of a pixel, so the numbers
           * stay whole. */
          double vw = MAX (floor (shape->width * 100 + 0.5), 1), vh = MAX (floor (shape->height * 100 + 0.5), 1);
          gboolean curved = FALSE;
          GString *pts = g_string_new (NULL);

          for (guint k = 0; k < shape->path->len; k++)
            {
              const O42PathPoint *pp = &g_array_index (shape->path, O42PathPoint, k);
              if (pp->op == 'C') curved = TRUE;
            }
          if (!curved)
            {
              for (guint k = 0; k < shape->path->len; k++)
                {
                  const O42PathPoint *pp = &g_array_index (shape->path, O42PathPoint, k);
                  g_string_append_printf (pts, "%s%.0f,%.0f", k > 0 ? " " : "", pp->x * vw, pp->y * vh);
                }
              g_string_append_printf (out,
                "<draw:%s draw:name=\"%s\" draw:style-name=\"gr%d_%u\"%s svg:x=\"%.3fcm\" svg:y=\"%.3fcm\" "
                "svg:width=\"%.3fcm\" svg:height=\"%.3fcm\" svg:viewBox=\"0 0 %.0f %.0f\" draw:points=\"%s\">%s</draw:%s>",
                shape->closed ? "polygon" : "polyline", name, sheet_index, i, end_cell,
                shape->dx * PX_TO_CM, shape->dy * PX_TO_CM, shape->width * PX_TO_CM, shape->height * PX_TO_CM,
                vw, vh, pts->str, text, shape->closed ? "polygon" : "polyline");
            }
          else
            {
              for (guint k = 0; k < shape->path->len; k++)
                {
                  const O42PathPoint *pp = &g_array_index (shape->path, O42PathPoint, k);
                  if (pp->op == 'C')
                    g_string_append_printf (pts, "C %.0f %.0f %.0f %.0f %.0f %.0f ", pp->x1 * vw, pp->y1 * vh, pp->x2 * vw, pp->y2 * vh, pp->x * vw, pp->y * vh);
                  else
                    g_string_append_printf (pts, "%c %.0f %.0f ", pp->op, pp->x * vw, pp->y * vh);
                }
              if (shape->closed) g_string_append (pts, "Z");
              g_string_append_printf (out,
                "<draw:path draw:name=\"%s\" draw:style-name=\"gr%d_%u\"%s svg:x=\"%.3fcm\" svg:y=\"%.3fcm\" "
                "svg:width=\"%.3fcm\" svg:height=\"%.3fcm\" svg:viewBox=\"0 0 %.0f %.0f\" svg:d=\"%s\">%s</draw:path>",
                name, sheet_index, i, end_cell, shape->dx * PX_TO_CM, shape->dy * PX_TO_CM,
                shape->width * PX_TO_CM, shape->height * PX_TO_CM, vw, vh, g_strstrip (pts->str), text);
            }
          g_string_free (pts, TRUE);
        }
      else if (shape->kind == O42_SHAPE_LINE || shape->kind == O42_SHAPE_ARROW)
        g_string_append_printf (out,
          "<draw:line draw:name=\"%s\" draw:style-name=\"gr%d_%u\"%s svg:x1=\"%.3fcm\" svg:y1=\"%.3fcm\" "
          "svg:x2=\"%.3fcm\" svg:y2=\"%.3fcm\"><text:p/></draw:line>",
          name, sheet_index, i, end_cell, shape->dx * PX_TO_CM, shape->dy * PX_TO_CM,
          (shape->dx + shape->width) * PX_TO_CM, (shape->dy + shape->height) * PX_TO_CM);
      else if (o42_shape_ods_type (shape) != NULL)
        /* An AutoShape is a custom shape whose enhanced geometry names
         * the outline; LibreOffice draws it from the name. */
        g_string_append_printf (out,
          "<draw:custom-shape draw:name=\"%s\" draw:style-name=\"gr%d_%u\"%s svg:x=\"%.3fcm\" svg:y=\"%.3fcm\" "
          "svg:width=\"%.3fcm\" svg:height=\"%.3fcm\">%s"
          "<draw:enhanced-geometry draw:type=\"%s\"/></draw:custom-shape>",
          name, sheet_index, i, end_cell, shape->dx * PX_TO_CM, shape->dy * PX_TO_CM,
          shape->width * PX_TO_CM, shape->height * PX_TO_CM, text,
          o42_shape_ods_type (shape));
      else
        g_string_append_printf (out,
          "<draw:%s draw:name=\"%s\" draw:style-name=\"gr%d_%u\"%s svg:x=\"%.3fcm\" svg:y=\"%.3fcm\" "
          "svg:width=\"%.3fcm\" svg:height=\"%.3fcm\">%s</draw:%s>",
          shape->kind == O42_SHAPE_OVAL ? "ellipse" : "rect", name, sheet_index, i, end_cell,
          shape->dx * PX_TO_CM, shape->dy * PX_TO_CM,
          shape->width * PX_TO_CM, shape->height * PX_TO_CM, text,
          shape->kind == O42_SHAPE_OVAL ? "ellipse" : "rect");
      g_free (end_cell);
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
      {
        char *end = ods_end_cell (sheet, chart->row, chart->col, chart->dx, chart->dy, chart->width, chart->height, chart->anchor);
        append_frame_head (out, name, (int) (100 + i), chart->dx, chart->dy,
                           chart->width, chart->height, end);
        g_free (end);
      }
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
  {
    int vi = validation_index_at (sheet, row, col);
    if (vi >= 0)
      g_string_append_printf (out, " table:content-validation-name=\"val%d_%d\"", sheet_index, vi);
  }
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
                                  fmt->number == O42_NUM_PERCENT ? "percentage" : (fmt->number == O42_NUM_CURRENCY || fmt->number == O42_NUM_ACCOUNTING) ? "currency" : "float",
                                  g_ascii_dtostr (buf, sizeof buf, value.as.number));
          if (fmt->number == O42_NUM_CURRENCY || fmt->number == O42_NUM_ACCOUNTING)
            g_string_append_printf (out, " office:currency=\"%s\"", o42_numfmt_currency_iso ());
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
  write_cell_drawings (out, s, sheet, sheet_index, row, col);
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

static void write_conditional_formats (GString *out, Styles *s, O42Sheet *sheet, const char *table_name);

static void
count_dropped_cell (O42Sheet *sheet, int row, int col, gpointer user)
{
  (void) sheet; (void) col; (void) user;
  if (row >= O42_EXCEL_MAX_ROWS)
    o42_xlsx_dropped_cells++;
}

static void
write_table (GString *out, Styles *s, O42Sheet *sheet, int sheet_index)
{
  O42Range used;
  int default_width = o42_sheet_default_col_width (sheet);
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
  /* LibreOffice's grid ends where Excel's does; the cells beyond are
   * counted and left out, and the caller says so. */
  if (last_row >= O42_EXCEL_MAX_ROWS)
    {
      o42_sheet_foreach_cell (sheet, count_dropped_cell, NULL);
      last_row = O42_EXCEL_MAX_ROWS - 1;
    }

  {
    guint32 tab = o42_sheet_tab_colour (sheet);
    const O42PrintSetup *ps = o42_sheet_print_setup (sheet);
    char *style = table_style (s, tab, sheet_index, !o42_sheet_hidden (sheet));

    write_page_style (s, sheet, sheet_index);
    g_string_append_printf (out, "<table:table table:name=\"%s\" table:style-name=\"%s\"", name, style);
    g_free (style);
    if (ps->has_area)
      {
        /* 'Sheet 1'.A1:'Sheet 1'.C9, the sheet quoted when it needs it;
         * several areas a space apart. */
        gboolean quote = strpbrk (o42_sheet_get_name (sheet), " '.-") != NULL;

        g_string_append (out, " table:print-ranges=\"");
        for (int k = 0; k < MAX (ps->n_areas, 1); k++)
          {
            const O42Range *area = ps->n_areas > 0 ? &ps->areas[k] : &ps->area;
            char *a = o42_ref_name (area->row0, area->col0);
            char *b = o42_ref_name (area->row1, area->col1);

            g_string_append_printf (out, "%s%s%s%s.%s:%s%s%s.%s", k > 0 ? " " : "",
                                    quote ? "&apos;" : "", name, quote ? "&apos;" : "", a,
                                    quote ? "&apos;" : "", name, quote ? "&apos;" : "", b);
            g_free (a); g_free (b);
          }
        g_string_append_c (out, '"');
      }
    g_string_append (out, ">");
    /* The repeated rows and columns are wrapped as headers, so the
     * columns come in two runs when there are any. */
    if (ps->title_cols > 0) last_col = MAX (last_col, ps->title_col_first + ps->title_cols - 1);
    if (ps->title_rows > 0) last_row = MAX (last_row, MIN (ps->title_row_first + ps->title_rows, 4096) - 1);
    {
      /* A page break is a row or column style, so the rows and columns
       * up to the last break are written out. */
      GArray *rb = o42_sheet_page_breaks (sheet, TRUE);
      GArray *cb = o42_sheet_page_breaks (sheet, FALSE);
      for (guint i = 0; i < rb->len; i++)
        last_row = MAX (last_row, MIN (g_array_index (rb, int, i), 4095));
      for (guint i = 0; i < cb->len; i++)
        last_col = MAX (last_col, MIN (g_array_index (cb, int, i), 255));
    }
    {
      /* A validation names the cells it covers, empty ones too. */
      GArray *rules = o42_sheet_validations (sheet);
      for (guint i = 0; i < rules->len; i++)
        {
          const O42Validation *v = &g_array_index (rules, O42Validation, i);
          last_row = MAX (last_row, MIN (v->range.row1, 4095));
          last_col = MAX (last_col, MIN (v->range.col1, 255));
        }
    }
  }
  g_free (name);

  write_forms (out, sheet);

  /* Columns, in runs of the same width, a run ending where a page
   * break or the repeated columns do. */
  {
    const O42PrintSetup *ps = o42_sheet_print_setup (sheet);
    int title_c0 = ps->title_cols > 0 ? ps->title_col_first : -1;
    int title_c1 = ps->title_cols > 0 ? ps->title_col_first + ps->title_cols : -1;   /* one past */
    for (int c = 0; c <= last_col; )
      {
        int width = o42_sheet_col_width (sheet, c);
        gboolean hidden = o42_sheet_col_hidden (sheet, c);
        gboolean brk = o42_sheet_page_break (sheet, FALSE, c);
        int n = 1;
        if (c == title_c0)
          g_string_append (out, "<table:table-header-columns>");
        while (c + n <= last_col && o42_sheet_col_width (sheet, c + n) == width &&
               o42_sheet_col_hidden (sheet, c + n) == hidden && c + n != title_c1 && c + n != title_c0 &&
               !o42_sheet_page_break (sheet, FALSE, c + n))
          n++;
        g_string_append_printf (out, "<table:table-column table:style-name=\"%s\"",
                                brk ? break_style (s, hidden ? default_width : width, FALSE)
                                    : col_style (s, hidden ? default_width : width));
        if (n > 1) g_string_append_printf (out, " table:number-columns-repeated=\"%d\"", n);
        if (hidden) g_string_append (out, " table:visibility=\"collapse\"");
        g_string_append (out, " table:default-cell-style-name=\"Default\"/>");
        c += n;
        if (c == title_c1)
          g_string_append (out, "</table:table-header-columns>");
      }
    if (last_col < 0)
      g_string_append_printf (out, "<table:table-column table:style-name=\"%s\" table:default-cell-style-name=\"Default\"/>", col_style (s, default_width));
  }

  for (int r = 0; r <= last_row; r++)
    {
      int height = o42_sheet_row_height (sheet, r);
      gboolean hidden = o42_sheet_row_hidden (sheet, r);
      gboolean brk = o42_sheet_page_break (sheet, TRUE, r);
      const O42PrintSetup *tps = o42_sheet_print_setup (sheet);
      int last_in_row = -1;

      if (tps->title_rows > 0 && r == tps->title_row_first)
        g_string_append (out, "<table:table-header-rows>");

      for (int c = 0; c <= last_col; c++)
        {
          char *input = o42_sheet_get_input (sheet, r, c);
          if ((input != NULL && *input != '\0') || o42_sheet_get_fmt_idx (sheet, r, c) != default_idx ||
              o42_sheet_get_note (sheet, r, c) != NULL || merge_at_head (sheet, r, c) != NULL ||
              merge_covers (sheet, r, c) || cell_has_drawings (sheet, r, c) ||
              o42_sheet_validation_at (sheet, r, c) != NULL)
            last_in_row = c;
          g_free (input);
        }

      g_string_append_printf (out, "<table:table-row table:style-name=\"%s\"",
                              brk ? break_style (s, hidden ? default_height : height, TRUE)
                                  : row_style (s, hidden ? default_height : height));
      if (hidden) g_string_append (out, " table:visibility=\"collapse\"");
      g_string_append_c (out, '>');
      for (int c = 0; c <= last_in_row; c++)
        write_cell (out, s, sheet, sheet_index, r, c, default_idx);
      if (last_in_row < 0)
        g_string_append (out, "<table:table-cell/>");
      g_string_append (out, "</table:table-row>");
      if (tps->title_rows > 0 && r + 1 == tps->title_row_first + tps->title_rows)
        g_string_append (out, "</table:table-header-rows>");
    }
  if (last_row < 0)
    g_string_append (out, "<table:table-row><table:table-cell/></table:table-row>");
  write_conditional_formats (out, s, sheet, o42_sheet_get_name (sheet));
  g_string_append (out, "</table:table>");
}

/* A rule's operand as a condition writes it: the number, or the
 * formula in OpenFormula without its "of:=". */
static char *
cond_operand (const char *expr, double value)
{
  if (expr != NULL)
    {
      char *with = g_strconcat ("=", expr + (expr[0] == '='), NULL);
      char *of = of_formula (with);
      char *plain = g_strdup (g_str_has_prefix (of, "of:=") ? of + 4 : of);

      g_free (with);
      g_free (of);
      return plain;
    }
  {
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    return g_strdup (g_ascii_dtostr (buffer, sizeof buffer, value));
  }
}

/* The conditional formats of a sheet, as LibreOffice keeps them: a
 * calcext:conditional-formats element at the end of the table, each
 * rule a condition naming a style in styles.xml, or a colour scale
 * with its entries. */
static void
write_conditional_formats (GString *out, Styles *s, O42Sheet *sheet, const char *table_name)
{
  GArray *conds = o42_sheet_conditions (sheet);
  char *esc;

  if (conds->len == 0)
    return;
  esc = g_markup_escape_text (table_name, -1);
  g_string_append (out, "<calcext:conditional-formats>");
  for (guint i = 0; i < conds->len; i++)
    {
      const O42Condition *c = &g_array_index (conds, O42Condition, i);
      char *a = o42_ref_name (c->range.row0, c->range.col0);
      char *b = o42_ref_name (c->range.row1, c->range.col1);

      g_string_append_printf (out, "<calcext:conditional-format calcext:target-range-address=\"%s.%s:%s.%s\">", esc, a, esc, b);
      if (c->kind == O42_COND_SCALE)
        {
          static const char *const kinds[] = { "minimum", "maximum", "number", "percent", "percentile" };

          g_string_append (out, "<calcext:color-scale>");
          for (int k = 0; k < CLAMP (c->stops, 2, 3); k++)
            {
              char sv[G_ASCII_DTOSTR_BUF_SIZE];
              g_ascii_dtostr (sv, sizeof sv, c->stop_value[k]);
              g_string_append_printf (out, "<calcext:color-scale-entry calcext:value=\"%s\" calcext:type=\"%s\" calcext:color=\"#%06x\"/>",
                                      sv, kinds[CLAMP (c->stop_type[k], 0, 4)], c->stop_colour[k] & 0xFFFFFF);
            }
          g_string_append (out, "</calcext:color-scale>");
        }
      else
        {
          static const char *const ops[] = { "between", "not-between", "=", "!=", ">", "<", ">=", "<=" };
          char *style = g_strdup_printf ("ConditionalStyle_%d", ++s->next_cond);
          char *v1 = cond_operand (c->expr1, c->value);
          char *v2 = cond_operand (c->expr2, c->value2);
          GString *value = g_string_new (NULL);
          char *escaped;

          /* The style: only what the rule sets, over the cell's own. */
          g_string_append_printf (s->cond_styles,
            "<style:style style:name=\"%s\" style:family=\"table-cell\" style:parent-style-name=\"Default\">", style);
          if ((c->mask & O42_FMT_FILL) && c->fmt.fill != O42_FILL_NONE)
            g_string_append_printf (s->cond_styles, "<style:table-cell-properties fo:background-color=\"#%06x\"/>", c->fmt.fill & 0xFFFFFF);
          g_string_append (s->cond_styles, "<style:text-properties");
          if (c->mask & O42_FMT_BOLD)      g_string_append_printf (s->cond_styles, " fo:font-weight=\"%s\"", c->fmt.bold ? "bold" : "normal");
          if (c->mask & O42_FMT_ITALIC)    g_string_append_printf (s->cond_styles, " fo:font-style=\"%s\"", c->fmt.italic ? "italic" : "normal");
          if ((c->mask & O42_FMT_UNDERLINE) && c->fmt.underline)
            g_string_append (s->cond_styles, " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"");
          if (c->mask & O42_FMT_COLOUR)    g_string_append_printf (s->cond_styles, " fo:color=\"#%06x\"", c->fmt.colour & 0xFFFFFF);
          g_string_append (s->cond_styles, "/></style:style>");

          if (c->is_formula)
            g_string_append_printf (value, "formula-is(%s)", v1);
          else if (c->op == O42_COND_BETWEEN || c->op == O42_COND_NOT_BETWEEN)
            g_string_append_printf (value, "%s(%s,%s)", ops[c->op], v1, v2);
          else
            g_string_append_printf (value, "%s%s", ops[CLAMP ((int) c->op, 0, 7)], v1);
          escaped = g_markup_escape_text (value->str, -1);
          g_string_append_printf (out, "<calcext:condition calcext:apply-style-name=\"%s\" calcext:value=\"%s\" calcext:base-cell-address=\"%s.%s\"/>",
                                  style, escaped, esc, a);
          g_free (escaped);
          g_string_free (value, TRUE);
          g_free (v1); g_free (v2); g_free (style);
        }
      g_string_append (out, "</calcext:conditional-format>");
      g_free (a);
      g_free (b);
    }
  g_string_append (out, "</calcext:conditional-formats>");
  g_free (esc);
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
      const O42SheetView *view = o42_sheet_view (sheet);
      int rows = 0, cols = 0;
      char *name = g_markup_escape_text (o42_sheet_get_name (sheet), -1);
      o42_sheet_get_frozen (sheet, &rows, &cols);
      g_string_append_printf (out, "<config:config-item-map-entry config:name=\"%s\">", name);
      g_string_append_printf (out,
        "<config:config-item config:name=\"CursorPositionX\" config:type=\"int\">%d</config:config-item>"
        "<config:config-item config:name=\"CursorPositionY\" config:type=\"int\">%d</config:config-item>"
        "<config:config-item config:name=\"ZoomType\" config:type=\"short\">0</config:config-item>"
        "<config:config-item config:name=\"ZoomValue\" config:type=\"int\">%d</config:config-item>"
        "<config:config-item config:name=\"ShowGrid\" config:type=\"boolean\">%s</config:config-item>",
        view->active_col, view->active_row, view->zoom, view->gridlines ? "true" : "false");
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
  g_string_append (out, "</config:config-item-map-named>");
  {
    /* The sheet the book opens on, and what shows, which LibreOffice
     * keeps once for the view: the shown sheet's say. */
    O42Sheet *shown = o42_book_sheet (book, 0);
    char *name;

    for (int i = 0; i < o42_book_n_sheets (book); i++)
      if (o42_sheet_view (o42_book_sheet (book, i))->selected)
        { shown = o42_book_sheet (book, i); break; }
    name = g_markup_escape_text (o42_sheet_get_name (shown), -1);
    g_string_append_printf (out,
      "<config:config-item config:name=\"ActiveTable\" config:type=\"string\">%s</config:config-item>"
      "<config:config-item config:name=\"ZoomValue\" config:type=\"int\">%d</config:config-item>"
      "<config:config-item config:name=\"ShowGrid\" config:type=\"boolean\">%s</config:config-item>"
      "<config:config-item config:name=\"ShowZeroValues\" config:type=\"boolean\">%s</config:config-item>",
      name, o42_sheet_view (shown)->zoom, o42_sheet_view (shown)->gridlines ? "true" : "false",
      o42_sheet_view (shown)->zeros ? "true" : "false");
    g_free (name);
  }
  g_string_append (out, "</config:config-item-map-entry></config:config-item-map-indexed>"
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
  o42_xlsx_dropped_cells = 0;
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
  s.next_co = s.next_ro = s.next_ce = s.next_n = s.next_t = s.next_ta = 0;
  s.page_layouts = g_string_new (NULL);
  s.master_pages = g_string_new (NULL);
  s.hf_styles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  s.hf_style_xml = g_string_new (NULL);
  s.cond_styles = g_string_new (NULL);
  s.next_cond = 0;
  s.fill_defs = g_string_new (NULL);

  for (int i = 0; i < o42_book_n_sheets (book); i++)
    write_table (body, &s, o42_book_sheet (book, i), i);
  write_names (body, book);

  content = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content " NS_HEAD ">");
  g_string_append (content, "<office:font-face-decls><style:font-face style:name=\"Arial\" svg:font-family=\"Arial\" xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\"/></office:font-face-decls>");
  g_string_append (content, "<office:automatic-styles>");
  g_string_append (content, s.styles->str);
  g_string_append (content, "</office:automatic-styles><office:body><office:spreadsheet");
  if (o42_book_protected (book))
    g_string_append (content, " table:structure-protected=\"true\"");
  g_string_append (content, ">");
  if (o42_book_date_1904 (book) || o42_book_precision_as_displayed (book))
    g_string_append_printf (content, "<table:calculation-settings%s>%s</table:calculation-settings>",
                            o42_book_precision_as_displayed (book) ? " table:precision-as-shown=\"true\"" : "",
                            o42_book_date_1904 (book) ? "<table:null-date table:date-value=\"1904-01-01\"/>" : "");
  write_validations (content, book);
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
      "<manifest:file-entry manifest:full-path=\"settings.xml\" manifest:media-type=\"text/xml\"/>"
      "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>");

    GString *styles = g_string_new (
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles " NS_HEAD ">"
      "<office:styles><style:default-style style:family=\"table-cell\">"
      "<style:text-properties fo:font-family=\"Arial\" fo:font-size=\"10pt\"/></style:default-style>"
      "<style:style style:name=\"Default\" style:family=\"table-cell\"/>"
      /* The dashes, in lengths of the line's width, and the heads a
       * line can wear, by the names the graphic styles use. */
      "<draw:stroke-dash draw:name=\"Dash\" draw:style=\"rect\" draw:dots1=\"1\" draw:dots1-length=\"400%\" draw:distance=\"300%\"/>"
      "<draw:stroke-dash draw:name=\"Dot\" draw:style=\"rect\" draw:dots1=\"1\" draw:dots1-length=\"100%\" draw:distance=\"300%\"/>"
      "<draw:stroke-dash draw:name=\"Dash_20_Dot\" draw:display-name=\"Dash Dot\" draw:style=\"rect\" draw:dots1=\"1\" draw:dots1-length=\"400%\" draw:dots2=\"1\" draw:dots2-length=\"100%\" draw:distance=\"300%\"/>"
      "<draw:stroke-dash draw:name=\"Long_20_Dash\" draw:display-name=\"Long Dash\" draw:style=\"rect\" draw:dots1=\"1\" draw:dots1-length=\"800%\" draw:distance=\"300%\"/>"
      "<draw:stroke-dash draw:name=\"Short_20_Dash\" draw:display-name=\"Short Dash\" draw:style=\"rect\" draw:dots1=\"1\" draw:dots1-length=\"300%\" draw:distance=\"100%\"/>"
      "<draw:stroke-dash draw:name=\"Short_20_Dot\" draw:display-name=\"Short Dot\" draw:style=\"rect\" draw:dots1=\"1\" draw:dots1-length=\"100%\" draw:distance=\"100%\"/>"
      "<draw:marker draw:name=\"Triangle\" svg:viewBox=\"0 0 20 30\" svg:d=\"M10 0 0 30h20z\"/>"
      "<draw:marker draw:name=\"Stealth\" svg:viewBox=\"0 0 20 30\" svg:d=\"M10 0 0 30 10-9 10 9z\"/>"
      "<draw:marker draw:name=\"Diamond\" svg:viewBox=\"0 0 20 30\" svg:d=\"M10 0 0 15 10 15 10-15z\"/>"
      "<draw:marker draw:name=\"Circle\" svg:viewBox=\"0 0 20 20\" svg:d=\"M10 0c-5.5 0-10 4.5-10 10s4.5 10 10 10 10-4.5 10-10-4.5-10-10-10z\"/>"
      "<draw:marker draw:name=\"Open_20_Arrow\" draw:display-name=\"Open Arrow\" svg:viewBox=\"0 0 20 30\" svg:d=\"M10 0 0 30h3l7-21 7 21h3z\"/>");
    g_string_append (styles, s.fill_defs->str);
    g_string_append (styles, s.cond_styles->str);
    g_string_append (styles, "</office:styles><office:automatic-styles>");

    g_string_append (styles, s.hf_style_xml->str);
    g_string_append (styles, s.page_layouts->str);
    g_string_append (styles, "</office:automatic-styles><office:master-styles>");
    g_string_append (styles, s.master_pages->str);
    g_string_append (styles, "</office:master-styles></office:document-styles>");

    write_drawing_parts (zip, book, manifest);
    g_string_append (manifest, "</manifest:manifest>");
    o42_zip_writer_add (zip, "META-INF/manifest.xml", manifest->str, manifest->len);
    g_string_free (manifest, TRUE);
    o42_zip_writer_add (zip, "styles.xml", styles->str, styles->len);
    g_string_free (styles, TRUE);
  }
  o42_zip_writer_add (zip, "content.xml", content->str, content->len);
  o42_zip_writer_add (zip, "settings.xml", settings->str, settings->len);

  /* File > Properties, in meta.xml as LibreOffice keeps them. */
  {
    static const char *const ELEMENTS[O42_N_PROPS] = {
      "dc:title", "dc:subject", "meta:initial-creator", NULL, NULL, NULL, "meta:keyword", "dc:description"
    };
    static const char *const USER[O42_N_PROPS] = {
      NULL, NULL, NULL, "Manager", "Company", "Category", NULL, NULL
    };
    GString *meta = g_string_new (
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
      "xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" office:version=\"1.2\"><office:meta>"
      "<meta:generator>Office42 Spreadsheet</meta:generator>");

    for (int i = 0; i < O42_N_PROPS; i++)
      {
        const char *value = o42_book_property (book, (O42Property) i);
        char *escaped;

        if (*value == '\0')
          continue;
        escaped = g_markup_escape_text (value, -1);
        if (ELEMENTS[i] != NULL)
          g_string_append_printf (meta, "<%s>%s</%s>", ELEMENTS[i], escaped, ELEMENTS[i]);
        else
          g_string_append_printf (meta, "<meta:user-defined meta:name=\"%s\">%s</meta:user-defined>", USER[i], escaped);
        g_free (escaped);
      }
    g_string_append (meta, "</office:meta></office:document-meta>");
    o42_zip_writer_add (zip, "meta.xml", meta->str, meta->len);
    g_string_free (meta, TRUE);
  }
  bytes = o42_zip_writer_finish (zip);
  ok = g_file_replace_contents (file, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes),
                                NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, error);
  g_bytes_unref (bytes);

  g_string_free (content, TRUE);
  g_string_free (settings, TRUE);
  g_string_free (body, TRUE);
  g_string_free (s.styles, TRUE);
  g_string_free (s.page_layouts, TRUE);
  g_string_free (s.master_pages, TRUE);
  g_string_free (s.hf_style_xml, TRUE);
  g_string_free (s.cond_styles, TRUE);
  g_string_free (s.fill_defs, TRUE);
  g_hash_table_unref (s.hf_styles);
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

/* A gradient or a hatch as styles.xml defines it, by name. */
typedef struct { guint32 start, end; double angle; } OdsGradient;
typedef struct { guint32 colour; O42Pattern pattern; } OdsHatch;

typedef struct {
  O42Fmt   fmt;
  gboolean has_fmt;        /* anything set beyond the default */
  char    *data_style;     /* number style name, resolved after all styles are read */
  int      width;          /* columns: px, or 0 */
  int      height;         /* rows: px, or 0 */
  guint32  tab_colour;     /* tables: the tab's colour, or O42_TAB_NO_COLOUR */
  gboolean table_hidden;   /* tables: table:display="false" */
  gboolean page_break;     /* rows and columns: fo:break-before="page" */
  char    *master_page;    /* tables: the master page they print on */

  /* Graphic styles: a shape's fill and line, and how its text sits. */
  gboolean graphic;
  char    *gradient_name;  /* draw:fill="gradient": which */
  char    *hatch_name;     /* draw:fill="hatch": which */
  gboolean shadow;
  guint32  shadow_colour;
  double   shadow_dx, shadow_dy;
  int      text_valign;    /* an O42VAlign, or -1 for unsaid */
  double   text_padding;   /* px, or -1 */
  int      text_nowrap;    /* 1, 0, or -1 */
  gboolean fill_none;
  guint32  fill;
  guint32  line;
  double   line_width;     /* px, or 0 for unsaid */
  gboolean stroke_none;
  O42Dash  dash;
  O42Head  head_start, head_end;
  O42HeadSize head_start_size, head_end_size;
} Style;

/* A page layout from styles.xml, and the master page that uses it:
 * together, a sheet's Page Setup. */
typedef struct {
  O42PrintSetup ps;        /* header and footer owned */
  double   header_h;       /* the header band's height, points */
  double   footer_h;
  gboolean header_shown, footer_shown;
} PageLayout;

typedef struct {
  O42NumberFormat number;
  int decimals;
  gboolean grouping;
  GString *code;      /* the style rebuilt as a format code */
  guint    lang;      /* the language it named, as an Excel LCID */
  char     symbol[32];   /* a currency style's symbol, as written */
  gboolean in_symbol;    /* reading it */
  gboolean in_currency_symbol;   /* the symbol element itself, for the code */
  gboolean in_fill;      /* reading a fill character */
  gboolean custom;       /* more than a preset holds: text, a colour, a map */
  char    *map_ge, *map_gt, *map_lt;   /* the styles its maps name, by the sign */
} NumStyle;

static void
num_style_free (gpointer data)
{
  NumStyle *ns = data;

  if (ns->code != NULL)
    g_string_free (ns->code, TRUE);
  g_free (ns->map_ge);
  g_free (ns->map_gt);
  g_free (ns->map_lt);
  g_free (ns);
}

typedef struct {
  O42Book    *book;
  O42Sheet   *sheet;
  int         n_tables;
  GHashTable *styles;        /* name -> Style */
  GHashTable *dashes;        /* a draw:stroke-dash name -> its O42Dash, by its look */
  GHashTable *gradients;     /* a draw:gradient name -> OdsGradient */
  GHashTable *hatches;       /* a draw:hatch name -> OdsHatch */
  GHashTable *num_styles;    /* name -> NumStyle */
  Style      *style;         /* the style being read */
  /* A calcext:conditional-format being read: its range, and the
   * colour scale gathering its entries. */
  O42Range     cf_range;
  gboolean     cf_have_range;
  O42Condition cf_scale;
  gboolean     cf_in_scale;
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
  int         meta_prop;     /* the property a meta.xml element is, or -1 */
  GString    *meta_text;
  GArray     *cell_runs;     /* O42TextRun for the cell being read */
  O42Shape   *shape;         /* the shape being read, for its text */
  double      frame_x, frame_y, frame_w, frame_h;   /* the frame being read */
  O42AnchorMode frame_anchor;                       /* and how it is anchored */
  GHashTable *form_controls;  /* form:id -> FormControl, for draw:control */
  GArray     *loose_controls; /* LooseControl: shapes outside any cell */
  gboolean    in_form;        /* inside <office:forms>, where a frame is a group box */

  /* Data validation: the rules by name, the cells that wear each. */
  GHashTable *valid_defs;     /* name -> O42Validation* (strings owned) */
  GHashTable *valid_cells;    /* name -> GArray of guint64 keys */
  O42Validation *valid_cur;   /* the one being read */
  int         valid_msg;      /* 1 in its help-message, 2 in its error-message */
  char       *cell_valid;     /* the cell's content-validation-name */

  /* Page layouts and master pages from styles.xml. */
  GHashTable *font_faces;    /* font-face name -> family */
  GHashTable *page_layouts;  /* name -> PageLayout */
  GHashTable *master_pages;  /* name -> PageLayout (the layout's, with the header and footer) */
  PageLayout *layout;        /* the one being read */
  int         hf_side;       /* 1 in the master page's header, 2 its footer */
  int         hf_region;     /* 0 left, 1 centre, 2 right */
  GString    *hf_parts[2][3];
  O42Fmt      hf_cur, hf_want;      /* the style the codes have set, and the span's */
  int         hf_paragraphs;        /* in the region so far */
  int         in_header_rows, in_header_cols;   /* rows or columns wrapped as repeated */
  int         header_rows_from, header_cols_from;

  /* Frozen panes from settings.xml. */
  char       *setting_table;
  char       *setting_name;
  GString    *setting_value;
  int         split_cols, split_rows, hmode, vmode;
  int         setting_cursor_x, setting_cursor_y, setting_zoom, setting_grid;
  int         view_zeros;
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

static double
attr_double (const char **names, const char **values, const char *want, double fallback)
{
  const char *v = attr (names, values, want);
  return v != NULL ? g_ascii_strtod (v, NULL) : fallback;
}

static int
attr_int (const char **names, const char **values, const char *want, int fallback)
{
  const char *v = attr (names, values, want);
  return v != NULL ? atoi (v) : fallback;
}

/* A count of digit places in a number style: decimals, leading noughts,
 * a fraction's or an exponent's figures.  A file may say anything, and
 * each place is a character of the code built from it, so it is held
 * to what a format can show; a style of min-integer-digits="1500000000"
 * would otherwise be a code of a gigabyte and a minute and a half. */
static int
attr_places (const char **names, const char **values, const char *want, int fallback)
{
  return CLAMP (attr_int (names, values, want, fallback), 0, O42_MAX_DECIMALS);
}

/* The Excel language of an ODF language tag, the other way round
 * from language_tag. */
static guint
language_lcid (const char *tag)
{
  static const struct { const char *tag; guint lang; } TAGS[] = {
    { "da", 0x06 }, { "de", 0x07 }, { "en", 0x09 }, { "es", 0x0A },
    { "fi", 0x0B }, { "fr", 0x0C }, { "it", 0x10 }, { "nl", 0x13 },
    { "nb", 0x14 }, { "nn", 0x14 }, { "no", 0x14 }, { "pl", 0x15 },
    { "pt", 0x16 }, { "sv", 0x1D }
  };

  for (gsize i = 0; i < G_N_ELEMENTS (TAGS); i++)
    if (g_ascii_strcasecmp (TAGS[i].tag, tag) == 0)
      return TAGS[i].lang;
  return 0;
}

static double
attr_num (const char **names, const char **values, const char *want, double fallback)
{
  const char *v = attr (names, values, want);
  return v != NULL ? g_ascii_strtod (v, NULL) : fallback;
}

/* ---- Form controls ----------------------------------------------------- */

/* A draw:control that came in <table:shapes> rather than in a cell:
 * its place is measured from the sheet's corner, and the columns and
 * rows it falls in are only known once they have all been read. */
typedef struct {
  char  *id;
  double x, y, w, h;
} LooseControl;

/* What one form:* element said.  The draw:control that names it says
 * where the control goes on the sheet. */
typedef struct {
  O42ShapeKind kind;
  char        *label;
  char        *link;
  char        *source;
  double       min, max, step, page;
} FormControl;

static void
form_control_free (gpointer data)
{
  FormControl *c = data;

  g_free (c->label);
  g_free (c->link);
  g_free (c->source);
  g_free (c);
}

/* A reference without the sheet in front of it: office42 keeps a
 * control's link as a plain cell on the control's own sheet, which is
 * what ODF's own writers mean by it in every file seen here. */
static char *
ref_without_sheet (const char *text)
{
  GString *out;
  char **parts;

  if (text == NULL || *text == 0)
    return NULL;
  out = g_string_new (NULL);
  parts = g_strsplit (text, ":", 2);
  for (int i = 0; parts[i] != NULL; i++)
    {
      const char *dot = strrchr (parts[i], '.');

      if (i > 0)
        g_string_append_c (out, ':');
      g_string_append (out, dot != NULL ? dot + 1 : parts[i]);
    }
  g_strfreev (parts);
  return g_string_free (out, FALSE);
}

/* The form:* elements of a sheet, gathered by their form:id. */
static void
read_form_control (Reader *r, const char *name, const char **names, const char **values)
{
  const char *id = attr (names, values, "id");
  const char *impl = attr (names, values, "control-implementation");
  const char *label = attr (names, values, "label");
  FormControl *c;

  if (id == NULL)
    return;
  c = g_new0 (FormControl, 1);
  c->step = 1;
  c->page = 10;
  if (strcmp (name, "button") == 0)
    c->kind = O42_SHAPE_BUTTON;
  else if (strcmp (name, "checkbox") == 0)
    c->kind = O42_SHAPE_CHECKBOX;
  else if (strcmp (name, "radio") == 0)
    c->kind = O42_SHAPE_OPTION;
  else if (strcmp (name, "fixed-text") == 0)
    c->kind = O42_SHAPE_LABEL;
  else if (strcmp (name, "frame") == 0)
    c->kind = O42_SHAPE_GROUPBOX;
  else if (strcmp (name, "value-range") == 0)
    {
      c->kind = impl != NULL && strstr (impl, "ScrollBar") != NULL
        ? O42_SHAPE_SCROLLBAR : O42_SHAPE_SPINNER;
      c->min = attr_num (names, values, "min-value", 0);
      c->max = attr_num (names, values, "max-value", 100);
      c->step = attr_num (names, values, "step-size", 1);
      c->page = attr_num (names, values, "page-step-size", 10);
    }
  else   /* listbox, with or without its drop-down */
    {
      const char *drop = attr (names, values, "dropdown");

      c->kind = drop != NULL && strcmp (drop, "true") == 0 ? O42_SHAPE_COMBO : O42_SHAPE_LISTBOX;
    }

  c->label = g_strdup (label != NULL ? label : "");
  c->link = ref_without_sheet (attr (names, values, "linked-cell"));
  c->source = ref_without_sheet (attr (names, values, "source-cell-range"));
  g_hash_table_replace (r->form_controls, g_strdup (id), c);
}

/* The control itself, put on the sheet at a cell with an offset
 * inside it. */
static void
place_control (Reader *r, FormControl *c, int row, int col,
               double dx, double dy, double w, double h)
{
  O42Shape *shape = o42_sheet_add_shape (r->sheet, c->kind, row, col);

  if (shape == NULL)
    return;
  shape->dx = dx;
  shape->dy = dy;
  shape->width = MAX (w, 12);
  shape->height = MAX (h, 12);
  if (c->label != NULL)
    { g_free (shape->text); shape->text = g_strdup (c->label); }
  if (c->link != NULL)
    { g_free (shape->link); shape->link = g_strdup (c->link); }
  if (c->source != NULL)
    { g_free (shape->source); shape->source = g_strdup (c->source); }
  shape->min = c->min;
  shape->max = c->max;
  shape->step = c->step;
  shape->page = c->page;
}

/* The controls that came in <table:shapes>, once the columns and rows
 * of the sheet are known: each is measured from the sheet's corner, so
 * walking the widths and heights says which cell it starts in. */
static void
place_loose_controls (Reader *r)
{
  for (guint i = 0; r->loose_controls != NULL && i < r->loose_controls->len; i++)
    {
      LooseControl *l = &g_array_index (r->loose_controls, LooseControl, i);
      FormControl *c = g_hash_table_lookup (r->form_controls, l->id);
      double x = 0, y = 0;
      int col = 0, row = 0;

      if (c != NULL && r->sheet != NULL)
        {
          while (col < O42_MAX_COLS - 1 && x + o42_sheet_col_width (r->sheet, col) <= l->x)
            x += o42_sheet_col_width (r->sheet, col++);
          while (row < O42_MAX_ROWS - 1 && y + o42_sheet_row_height (r->sheet, row) <= l->y)
            y += o42_sheet_row_height (r->sheet, row++);
          place_control (r, c, row, col, l->x - x, l->y - y, l->w, l->h);
        }
      g_free (l->id);
    }
  if (r->loose_controls != NULL)
    g_array_set_size (r->loose_controls, 0);
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

static double ods_length (const char *text);

/* A draw:stroke-dash's look, from its definition: a second dot makes
 * it dash-dot, a long first one a long dash, a short gap a system
 * dash or dot.  The lengths are in per cent of the line's width, or
 * absolute, in which case a dash is anything over a millimetre. */
static O42Dash
dash_from_definition (const char *dots1_length, const char *dots2, const char *distance)
{
  double len = 300, gap = 300;
  gboolean percent = dots1_length != NULL && strchr (dots1_length, '%') != NULL;

  if (dots2 != NULL && atoi (dots2) > 0)
    return O42_DASH_DASH_DOT;
  if (dots1_length != NULL)
    len = percent ? g_ascii_strtod (dots1_length, NULL) : ods_length (dots1_length) * PX_TO_CM * 1000;
  if (distance != NULL)
    gap = strchr (distance, '%') != NULL ? g_ascii_strtod (distance, NULL) : ods_length (distance) * PX_TO_CM * 1000;
  if (!percent)
    { len = len / 0.4; gap = gap / 0.4; }   /* a 0.04cm line: a millimetre is 250% */
  if (len >= 700)
    return O42_DASH_LONG_DASH;
  if (len >= 250)
    return gap <= 150 ? O42_DASH_SYS_DASH : O42_DASH_DASH;
  return gap <= 150 ? O42_DASH_SYS_DOT : O42_DASH_DOT;
}

/* A dash by the name a graphic style gives it: the definitions read
 * from the file first, then office42's own names, then a guess from
 * the name. */
static O42Dash
graphic_dash (Reader *r, const char *name)
{
  O42Dash dash;
  char *lower;
  gpointer known;

  if (name == NULL)
    return O42_DASH_SOLID;
  if (g_hash_table_lookup_extended (r->dashes, name, NULL, &known))
    return (O42Dash) GPOINTER_TO_INT (known);
  if (o42_dash_parse (name, &dash))
    return dash;
  lower = g_ascii_strdown (name, -1);
  if (strstr (lower, "dot") != NULL && strstr (lower, "dash") != NULL) dash = O42_DASH_DASH_DOT;
  else if (strstr (lower, "long") != NULL) dash = O42_DASH_LONG_DASH;
  else if (strstr (lower, "short") != NULL || strstr (lower, "fine") != NULL)
    dash = strstr (lower, "dot") != NULL ? O42_DASH_SYS_DOT : O42_DASH_SYS_DASH;
  else if (strstr (lower, "dot") != NULL) dash = O42_DASH_DOT;
  else dash = O42_DASH_DASH;
  g_free (lower);
  return dash;
}

/* A head by its marker's name, likewise. */
static O42Head
graphic_head (const char *name)
{
  O42Head head;
  char *lower;

  if (name == NULL || *name == '\0')
    return O42_HEAD_NONE;
  if (o42_head_parse (name, &head))
    return head;
  lower = g_ascii_strdown (name, -1);
  if (strstr (lower, "stealth") != NULL || strstr (lower, "concave") != NULL) head = O42_HEAD_STEALTH;
  else if (strstr (lower, "diamond") != NULL || strstr (lower, "rhombus") != NULL) head = O42_HEAD_DIAMOND;
  else if (strstr (lower, "circle") != NULL || strstr (lower, "oval") != NULL || strstr (lower, "dot") != NULL) head = O42_HEAD_OVAL;
  else if (strstr (lower, "open") != NULL || strstr (lower, "line") != NULL) head = O42_HEAD_ARROW;
  else head = O42_HEAD_TRIANGLE;
  g_free (lower);
  return head;
}

static O42HeadSize
graphic_head_size (const char *width)
{
  double cm = width != NULL ? ods_length (width) * PX_TO_CM : 0.3;

  return cm < 0.25 ? O42_HEAD_SMALL : cm > 0.4 ? O42_HEAD_LARGE : O42_HEAD_MEDIUM;
}

static void
page_layout_free (gpointer data)
{
  PageLayout *pl = data;
  g_free (pl->ps.header);
  g_free (pl->ps.footer);
  g_free (pl);
}

static void
style_free (gpointer data)
{
  Style *s = data;
  g_free (s->master_page);
  g_free (s->gradient_name);
  g_free (s->hatch_name);
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
      /* [.A1:.B5]![.B2:.C9]: OpenFormula's intersection is a space in
       * ours.  (The "!" of #DIV/0! follows no bracket.) */
      if (*p == '!' && p > of && (p[-1] == ']' || p[-1] == ')'))
        { g_string_append_c (out, ' '); p++; continue; }
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
          if (ns->code != NULL && ns->code->len > 0 &&
              (ns->custom || ns->number == O42_NUM_DATE || ns->number == O42_NUM_TIME ||
               ns->number == O42_NUM_DATETIME))
            {
              fmt.custom = g_intern_string (ns->code->str);
              if (ns->custom)
                fmt.number = O42_NUM_GENERAL;   /* the code says it all */
            }
          /* A currency the file names that is not this machine's stays
           * as it was written, as a code: a $ from an American file must
           * not turn into kr here. */
          if ((ns->number == O42_NUM_CURRENCY || ns->number == O42_NUM_ACCOUNTING) && !ns->custom &&
              ns->symbol[0] != '\0' && strcmp (ns->symbol, o42_numfmt_currency ()) != 0)
            {
              GString *digits = g_string_new ("#,##0");
              char *code;

              if (ns->decimals > 0)
                {
                  g_string_append_c (digits, '.');
                  for (int i = 0; i < ns->decimals; i++) g_string_append_c (digits, '0');
                }
              if (ns->number == O42_NUM_ACCOUNTING)
                code = g_strdup_printf ("_(\"%s\"* %s_);_(\"%s\"* (%s);_(\"%s\"* \"-\"??_);_(@_)",
                                        ns->symbol, digits->str, ns->symbol, digits->str, ns->symbol);
              else
                code = g_strdup_printf ("\"%s\"%s", ns->symbol, digits->str);
              fmt.custom = g_intern_string (code);
              g_free (code);
              g_string_free (digits, TRUE);
            }
        }
    }
  o42_sheet_apply_fmt (r->sheet, &range, O42_FMT_ALL, &fmt);
}

/* An OpenFormula condition back into a rule: the kind from the
 * of:cell-content-is-* call, the operator and values from what follows. */
static void
ods_validation_condition (const char *cond, O42Validation *v)
{
  const char *p;
  char *a = NULL, *b = NULL;

  v->kind = O42_VALID_ANY;
  v->op = O42_COND_BETWEEN;
  if (strstr (cond, "cell-content-is-whole-number") != NULL) v->kind = O42_VALID_WHOLE;
  else if (strstr (cond, "cell-content-is-decimal-number") != NULL) v->kind = O42_VALID_DECIMAL;
  else if (strstr (cond, "cell-content-is-date") != NULL) v->kind = O42_VALID_DATE;
  else if (strstr (cond, "cell-content-is-time") != NULL) v->kind = O42_VALID_TIME;
  else if (strstr (cond, "cell-content-text-length") != NULL) v->kind = O42_VALID_LENGTH;
  else if (strstr (cond, "cell-content-is-in-list") != NULL) v->kind = O42_VALID_LIST;

  if (v->kind == O42_VALID_LIST)
    {
      const char *open = strchr (cond, '(');
      const char *close = open != NULL ? strrchr (open, ')') : NULL;
      GString *items = g_string_new (NULL);

      if (open == NULL || close == NULL)
        return;
      if (open[1] == '[')
        {
          /* [.A1:.A5] or [Sheet.A1:.A5]: the range, without the dots. */
          char *inner = g_strndup (open + 2, close - open - 3);
          char *plain = g_strdup (inner);
          char *w = plain;
          for (const char *q = inner; *q != '\0'; q++)
            {
              if (*q == '.') { if (q == inner || q[-1] == ':') continue; w = plain; continue; }
              if (*q == '$') continue;
              *w++ = *q;
            }
          *w = '\0';
          g_free (v->value);
          v->value = plain;
          g_free (inner);
          g_string_free (items, TRUE);
          return;
        }
      for (p = open + 1; p < close; )
        {
          if (*p == '"')
            {
              const char *end = p + 1;
              while (*end != '\0' && *end != '"') end++;
              if (items->len > 0) g_string_append_c (items, ',');
              g_string_append_len (items, p + 1, end - p - 1);
              p = *end == '"' ? end + 1 : end;
            }
          else
            p++;
        }
      g_free (v->value);
      v->value = g_string_free (items, FALSE);
      return;
    }

  /* between(a,b), not-between(a,b), or cell-content() OP a. */
  if ((p = strstr (cond, "not-between(")) != NULL)
    { v->op = O42_COND_NOT_BETWEEN; p += 12; }
  else if ((p = strstr (cond, "between(")) != NULL)
    { v->op = O42_COND_BETWEEN; p += 8; }
  if (p != NULL)
    {
      const char *comma = strchr (p, ','), *close = strchr (p, ')');
      if (comma != NULL && close != NULL && comma < close)
        {
          a = g_strndup (p, comma - p);
          b = g_strndup (comma + 1, close - comma - 1);
        }
    }
  else if ((p = strstr (cond, "()")) != NULL)
    {
      p += 2;
      while (*p == ' ') p++;
      if (p[0] == '>' && p[1] == '=') { v->op = O42_COND_GREATER_EQUAL; p += 2; }
      else if (p[0] == '<' && p[1] == '=') { v->op = O42_COND_LESS_EQUAL; p += 2; }
      else if (p[0] == '!' && p[1] == '=') { v->op = O42_COND_NOT_EQUAL; p += 2; }
      else if (p[0] == '<' && p[1] == '>') { v->op = O42_COND_NOT_EQUAL; p += 2; }
      else if (p[0] == '>') { v->op = O42_COND_GREATER; p++; }
      else if (p[0] == '<') { v->op = O42_COND_LESS; p++; }
      else if (p[0] == '=') { v->op = O42_COND_EQUAL; p++; }
      else p = NULL;
      if (p != NULL)
        {
          while (*p == ' ') p++;
          a = g_strdup (p);
          g_strstrip (a);
        }
    }
  if (a != NULL) { g_free (v->value); v->value = a; }
  if (b != NULL) { g_free (v->value2); v->value2 = b; }
}

/* The cells that named a rule, made into ranges: the whole rectangle
 * when they fill it, else a run per row. */
static void
apply_validations (Reader *r)
{
  GHashTableIter iter;
  gpointer key, val;

  if (r->sheet == NULL)
    return;
  g_hash_table_iter_init (&iter, r->valid_cells);
  while (g_hash_table_iter_next (&iter, &key, &val))
    {
      const O42Validation *def = g_hash_table_lookup (r->valid_defs, key);
      GArray *cells = val;
      O42Range box;
      guint area;

      if (def == NULL || cells->len == 0 || def->kind == O42_VALID_ANY)
        continue;
      box.row0 = box.row1 = o42_key_row (g_array_index (cells, guint64, 0));
      box.col0 = box.col1 = o42_key_col (g_array_index (cells, guint64, 0));
      for (guint i = 1; i < cells->len; i++)
        {
          int row = o42_key_row (g_array_index (cells, guint64, i)), col = o42_key_col (g_array_index (cells, guint64, i));
          box.row0 = MIN (box.row0, row); box.row1 = MAX (box.row1, row);
          box.col0 = MIN (box.col0, col); box.col1 = MAX (box.col1, col);
        }
      area = (guint) (box.row1 - box.row0 + 1) * (guint) (box.col1 - box.col0 + 1);
      if (area == cells->len)
        {
          O42Validation v = *def;
          v.range = box;
          o42_sheet_add_validation (r->sheet, &v);
        }
      else
        for (guint i = 0; i < cells->len; )
          {
            O42Validation v = *def;
            int row = o42_key_row (g_array_index (cells, guint64, i)), col = o42_key_col (g_array_index (cells, guint64, i));
            guint j = i + 1;
            while (j < cells->len && o42_key_row (g_array_index (cells, guint64, j)) == row &&
                   o42_key_col (g_array_index (cells, guint64, j)) == col + (int) (j - i))
              j++;
            v.range.row0 = v.range.row1 = row;
            v.range.col0 = col;
            v.range.col1 = col + (int) (j - i) - 1;
            o42_sheet_add_validation (r->sheet, &v);
            i = j;
          }
    }
  g_hash_table_remove_all (r->valid_cells);
}

static void
validation_free (gpointer data)
{
  O42Validation *v = data;
  g_free (v->value); g_free (v->value2); g_free (v->message);
  g_free (v->title); g_free (v->prompt_title); g_free (v->prompt);
  g_free (v);
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
  if (r->cell_valid != NULL && repeat <= 1024)
    {
      GArray *cells = g_hash_table_lookup (r->valid_cells, r->cell_valid);
      if (cells == NULL)
        {
          cells = g_array_new (FALSE, FALSE, sizeof (guint64));
          g_hash_table_insert (r->valid_cells, g_strdup (r->cell_valid), cells);
        }
      for (int k = 0; k < repeat; k++)
        {
          guint64 key = o42_key (r->row, r->cell_col + k);
          g_array_append_val (cells, key);
        }
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
        /* TRUE typed into a cell is a boolean, not a formula that
         * returns one, and that is what the file meant. */
        input = g_strdup (strcmp (r->value, "true") == 0 ? "TRUE" : "FALSE");
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
      input = o42_entry_quote_text (r->text->str);
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
      if (st != NULL && st->page_break && r->row > 0 && !o42_sheet_page_break (r->sheet, TRUE, r->row))
        o42_sheet_toggle_page_break (r->sheet, TRUE, r->row);
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
  if (chart != NULL) chart->anchor = r->frame_anchor;
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

/* Before a header's words or a field: the codes that take the style
 * from what the last ones set to what the span in force wants. */
static void
hf_sync (Reader *r)
{
  GString *part = r->hf_parts[r->hf_side - 1][r->hf_region];
  O42Fmt *cur = &r->hf_cur, *want = &r->hf_want;

  if (cur->bold != want->bold) g_string_append (part, "&B");
  if (cur->italic != want->italic) g_string_append (part, "&I");
  if (cur->underline != want->underline) g_string_append (part, "&U");
  if (cur->size != want->size && want->size > 0)
    g_string_append_printf (part, "&%d", want->size / 2);
  if (g_strcmp0 (cur->family, want->family) != 0 && want->family != NULL)
    g_string_append_printf (part, "&\"%s\"", want->family);
  *cur = *want;
}

/* A polygon's or polyline's points, or a path's d, into a freeform's
 * outline as fractions of the view box. */
static void
ods_read_freeform (O42Shape *shape, const char *element, const char *viewbox,
                   const char *points, const char *d)
{
  double vx = 0, vy = 0, vw = 1, vh = 1;

  if (viewbox != NULL)
    {
      char **vb = g_strsplit_set (viewbox, " ,", -1);
      if (g_strv_length (vb) >= 4)
        {
          vx = g_ascii_strtod (vb[0], NULL); vy = g_ascii_strtod (vb[1], NULL);
          vw = g_ascii_strtod (vb[2], NULL); vh = g_ascii_strtod (vb[3], NULL);
        }
      g_strfreev (vb);
    }
  if (vw <= 0) vw = 1;
  if (vh <= 0) vh = 1;
  shape->closed = strcmp (element, "polygon") == 0;
  if (points != NULL && strcmp (element, "path") != 0)
    {
      char **pts = g_strsplit (points, " ", -1);
      for (int i = 0; pts[i] != NULL; i++)
        {
          char *comma = strchr (pts[i], ',');
          if (comma == NULL || *pts[i] == '\0') continue;
          o42_shape_path_add (shape, i == 0 ? 'M' : 'L',
                              (g_ascii_strtod (pts[i], NULL) - vx) / vw,
                              (g_ascii_strtod (comma + 1, NULL) - vy) / vh, 0, 0, 0, 0);
        }
      g_strfreev (pts);
    }
  else if (d != NULL)
    {
      /* The SVG path grammar, the part a spreadsheet meets: M L C Z and
       * their relative forms, numbers run together. */
      const char *p = d;
      char op = 0;
      double cx = 0, cy = 0;
      gboolean relative = FALSE;

      while (*p != '\0')
        {
          double v[6];
          int n = 0;

          while (*p == ' ' || *p == ',') p++;
          if (g_ascii_isalpha (*p))
            {
              op = g_ascii_toupper (*p);
              relative = g_ascii_islower (*p);
              p++;
              if (op == 'Z')
                { shape->closed = TRUE; continue; }
            }
          if (op == 0) break;
          {
            int want = op == 'C' ? 6 : op == 'Q' ? 4 : op == 'H' || op == 'V' ? 1 : 2;
            while (n < want)
              {
                char *end;
                while (*p == ' ' || *p == ',') p++;
                v[n] = g_ascii_strtod (p, &end);
                if (end == p) break;
                p = end;
                n++;
              }
            if (n < want) break;
          }
          if (op == 'H') { v[1] = cy; if (relative) v[0] += cx; }
          else if (op == 'V') { v[1] = v[0]; v[0] = cx; if (relative) v[1] += cy; }
          else if (relative)
            for (int k = 0; k < n; k += 2) { v[k] += cx; v[k + 1] += cy; }
          if (op == 'M' || op == 'L' || op == 'H' || op == 'V')
            {
              o42_shape_path_add (shape, op == 'M' ? 'M' : 'L', (v[0] - vx) / vw, (v[1] - vy) / vh, 0, 0, 0, 0);
              cx = v[0]; cy = v[1];
              if (op == 'M') op = 'L';   /* points after a move are lines */
            }
          else if (op == 'C')
            {
              o42_shape_path_add (shape, 'C', (v[4] - vx) / vw, (v[5] - vy) / vh,
                                  (v[0] - vx) / vw, (v[1] - vy) / vh, (v[2] - vx) / vw, (v[3] - vy) / vh);
              cx = v[4]; cy = v[5];
            }
          else if (op == 'Q')
            {
              double x1 = cx + 2.0 / 3 * (v[0] - cx), y1 = cy + 2.0 / 3 * (v[1] - cy);
              double x2 = v[2] + 2.0 / 3 * (v[0] - v[2]), y2 = v[3] + 2.0 / 3 * (v[1] - v[3]);
              o42_shape_path_add (shape, 'C', (v[2] - vx) / vw, (v[3] - vy) / vh,
                                  (x1 - vx) / vw, (y1 - vy) / vh, (x2 - vx) / vw, (y2 - vy) / vh);
              cx = v[2]; cy = v[3];
            }
          else
            break;
        }
    }
  if (!shape->closed)
    shape->fill = O42_FILL_NONE;
}

static void
content_start (GMarkupParseContext *ctx, const char *element, const char **names,
               const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  const char *name = local (element);
  (void) ctx; (void) error;

  if (strcmp (name, "spreadsheet") == 0)
    {
      const char *locked = attr (names, values, "structure-protected");
      if (locked != NULL && strcmp (locked, "true") == 0)
        o42_book_set_protected (r->book, TRUE);
      return;
    }

  if (r->in_cell && r->shape != NULL && (strcmp (name, "p") == 0 || strcmp (name, "span") == 0))
    {
      /* A shape's paragraphs and spans: theirs, not the cell's. */
      r->depth_in_cell++;
      if (strcmp (name, "p") == 0)
        {
          const char *pstyle = attr (names, values, "style-name");
          const Style *ps = pstyle != NULL ? g_hash_table_lookup (r->styles, pstyle) : NULL;

          if (r->shape->text != NULL && *r->shape->text != '\0')
            {
              char *more = g_strconcat (r->shape->text, "\n", NULL);
              g_free (r->shape->text);
              r->shape->text = more;
            }
          if (ps != NULL && ps->has_fmt && ps->fmt.halign != O42_HALIGN_GENERAL)
            r->shape->text_halign = ps->fmt.halign;
          r->in_p = TRUE;
        }
      else if (r->in_p)
        {
          const char *tstyle = attr (names, values, "style-name");
          const Style *ts = tstyle != NULL ? g_hash_table_lookup (r->styles, tstyle) : NULL;

          if (ts != NULL && ts->has_fmt)
            {
              if (ts->fmt.family != NULL && g_ascii_strcasecmp (ts->fmt.family, "Arial") != 0)
                r->shape->font = ts->fmt.family;
              if (ts->fmt.size > 0 && ts->fmt.size != 20)
                r->shape->font_size = ts->fmt.size / 2.0;
              r->shape->bold = ts->fmt.bold;
              r->shape->italic = ts->fmt.italic;
              r->shape->text_colour = ts->fmt.colour;
            }
        }
      return;
    }

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
      else if (strcmp (name, "control") == 0)
        {
          /* A form control: the form said what it is, this says where. */
          const char *id = attr (names, values, "control");
          FormControl *c = id != NULL ? g_hash_table_lookup (r->form_controls, id) : NULL;

          if (c != NULL)
            place_control (r, c, r->row, r->cell_col,
                           ods_length (attr (names, values, "x")),
                           ods_length (attr (names, values, "y")),
                           ods_length (attr (names, values, "width")),
                           ods_length (attr (names, values, "height")));
        }
      else if (strcmp (name, "frame") == 0 && !r->in_form)
        {
          const char *end = attr (names, values, "end-cell-address");
          r->frame_anchor = end != NULL && *end != '\0' ? O42_ANCHOR_TWO_CELL : O42_ANCHOR_ONE_CELL;
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
                      pic->anchor = r->frame_anchor;
                      pic->dx = r->frame_x;
                      pic->dy = r->frame_y;
                      if (r->frame_w > 1) pic->width = r->frame_w;
                      if (r->frame_h > 1) pic->height = r->frame_h;
                    }
                }
            }
        }
      else if (strcmp (name, "enhanced-geometry") == 0 && r->shape != NULL)
        o42_shape_apply_ods_type (r->shape, attr (names, values, "type"));
      else if (strcmp (name, "rect") == 0 || strcmp (name, "ellipse") == 0 ||
               strcmp (name, "circle") == 0 || strcmp (name, "line") == 0 ||
               strcmp (name, "custom-shape") == 0 || strcmp (name, "polygon") == 0 ||
               strcmp (name, "polyline") == 0 || strcmp (name, "path") == 0)
        {
          gboolean freeform = name[0] == 'p';
          O42ShapeKind kind = strcmp (name, "line") == 0 ? O42_SHAPE_LINE
                              : freeform ? O42_SHAPE_FREEFORM
                              : (name[0] == 'r' || name[1] == 'u') ? O42_SHAPE_RECT : O42_SHAPE_OVAL;
          O42Shape *shape = o42_sheet_add_shape (r->sheet, kind, r->row, r->cell_col);
          const char *style_name = attr (names, values, "style-name");
          const Style *st = style_name != NULL ? g_hash_table_lookup (r->styles, style_name) : NULL;

          if (shape != NULL && st != NULL && st->graphic)
            {
              /* The style's fill and line, dashes and heads. */
              shape->fill = (kind != O42_SHAPE_LINE && !st->fill_none) ? st->fill : O42_FILL_NONE;
              shape->line = st->line;
              if (st->line_width > 0)
                shape->line_width = st->line_width;
              else if (st->stroke_none)
                shape->line_width = 0.5;
              shape->dash = st->dash;
              if (st->gradient_name != NULL)
                {
                  const OdsGradient *g = g_hash_table_lookup (r->gradients, st->gradient_name);
                  if (g != NULL && shape->fill != O42_FILL_NONE)
                    {
                      shape->fill_kind = O42_SHAPE_FILL_GRADIENT;
                      shape->fill = g->start;
                      shape->fill2 = g->end;
                      shape->gradient_angle = fmod (fmod (90 - g->angle, 360) + 360, 360);
                    }
                }
              else if (st->hatch_name != NULL)
                {
                  const OdsHatch *h = g_hash_table_lookup (r->hatches, st->hatch_name);
                  if (h != NULL && shape->fill != O42_FILL_NONE)
                    {
                      shape->fill_kind = O42_SHAPE_FILL_PATTERN;
                      shape->pattern = h->pattern;
                      shape->fill2 = h->colour;
                    }
                }
              if (st->shadow)
                {
                  shape->shadow = TRUE;
                  shape->shadow_colour = st->shadow_colour;
                  shape->shadow_dx = st->shadow_dx;
                  shape->shadow_dy = st->shadow_dy;
                }
              if (st->text_valign >= 0) shape->text_valign = (O42VAlign) st->text_valign;
              if (st->text_padding >= 0) shape->text_inset = floor (st->text_padding + 0.5);
              if (st->text_nowrap >= 0) shape->text_nowrap = st->text_nowrap == 1;
              if (kind == O42_SHAPE_LINE)
                {
                  shape->head_start = st->head_start;
                  shape->head_end = st->head_end;
                  shape->head_start_size = st->head_start_size;
                  shape->head_end_size = st->head_end_size;
                  if (st->head_end != O42_HEAD_NONE)
                    shape->kind = O42_SHAPE_ARROW;
                }
            }
          if (shape != NULL)
            {
              const char *end = attr (names, values, "end-cell-address");
              shape->anchor = end != NULL && *end != '\0' ? O42_ANCHOR_TWO_CELL : O42_ANCHOR_ONE_CELL;
            }
          if (shape != NULL && freeform)
            ods_read_freeform (shape, name, attr (names, values, "viewBox"),
                               attr (names, values, "points"), attr (names, values, "d"));
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

  if (strcmp (name, "font-face") == 0 && attr (names, values, "name") != NULL &&
      attr (names, values, "font-family") != NULL)
    {
      char *fam = g_strdup (attr (names, values, "font-family"));
      size_t n = strlen (fam);

      if (n >= 2 && fam[0] == '\'' && fam[n - 1] == '\'')
        { memmove (fam, fam + 1, n - 2); fam[n - 2] = '\0'; }
      g_hash_table_replace (r->font_faces, g_strdup (attr (names, values, "name")), fam);
      return;
    }

  /* Page layouts, in styles.xml. */
  if (strcmp (name, "page-layout") == 0 && attr (names, values, "name") != NULL)
    {
      PageLayout *pl = g_new0 (PageLayout, 1);
      O42Sheet *fresh = o42_sheet_new ("x");

      pl->ps = *o42_sheet_print_setup (fresh);
      pl->ps.header = g_strdup ("");
      pl->ps.footer = g_strdup ("");
      o42_sheet_free (fresh);
      g_hash_table_replace (r->page_layouts, g_strdup (attr (names, values, "name")), pl);
      r->layout = pl;
      r->hf_side = 0;
      return;
    }
  if (r->layout != NULL && r->hf_side == 0 && strcmp (name, "page-layout-properties") == 0)
    {
      O42PrintSetup *ps = &r->layout->ps;
      const char *v;
      double pw = ods_length (attr (names, values, "page-width")) * 0.75;
      double ph = ods_length (attr (names, values, "page-height")) * 0.75;

      if ((v = attr (names, values, "print-orientation")) != NULL)
        ps->landscape = strcmp (v, "landscape") == 0;
      if (pw > 0 && ph > 0)
        {
          int code = o42_paper_code (pw, ph);
          if (code != 0) ps->paper = code;
          if (pw > ph) ps->landscape = TRUE;
        }
      if ((v = attr (names, values, "margin-top")) != NULL) ps->margin_top = ps->margin_header = ods_length (v) * 0.75;
      if ((v = attr (names, values, "margin-bottom")) != NULL) ps->margin_bottom = ps->margin_footer = ods_length (v) * 0.75;
      if ((v = attr (names, values, "margin-left")) != NULL) ps->margin_left = ods_length (v) * 0.75;
      if ((v = attr (names, values, "margin-right")) != NULL) ps->margin_right = ods_length (v) * 0.75;
      if ((v = attr (names, values, "print-page-order")) != NULL) ps->down_then_over = strcmp (v, "ltr") != 0;
      if ((v = attr (names, values, "first-page-number")) != NULL && g_ascii_isdigit (*v)) ps->first_page = atoi (v);
      if ((v = attr (names, values, "scale-to")) != NULL) ps->scale = CLAMP (atoi (v), 10, 400);
      if ((v = attr (names, values, "scale-to-X")) != NULL) ps->fit_wide = atoi (v);
      if ((v = attr (names, values, "scale-to-Y")) != NULL) ps->fit_tall = atoi (v);
      if ((v = attr (names, values, "scale-to-pages")) != NULL) { ps->fit_wide = atoi (v); ps->fit_tall = 0; }
      if ((v = attr (names, values, "table-centering")) != NULL)
        {
          ps->hcenter = strcmp (v, "horizontal") == 0 || strcmp (v, "both") == 0;
          ps->vcenter = strcmp (v, "vertical") == 0 || strcmp (v, "both") == 0;
        }
      if ((v = attr (names, values, "print")) != NULL)
        {
          ps->gridlines = strstr (v, "grid") != NULL;
          ps->headings = strstr (v, "headers") != NULL;
          ps->notes = strstr (v, "annotations") != NULL ? O42_PRINT_NOTES_IN_PLACE : O42_PRINT_NOTES_NONE;
          ps->draft = strstr (v, "objects") == NULL && strstr (v, "charts") == NULL;
        }
      return;
    }
  if (r->layout != NULL && (strcmp (name, "header-style") == 0 || strcmp (name, "footer-style") == 0))
    {
      r->hf_side = name[0] == 'h' ? 1 : 2;
      return;
    }
  if (r->layout != NULL && r->hf_side != 0 && strcmp (name, "header-footer-properties") == 0)
    {
      double h = ods_length (attr (names, values, "min-height")) * 0.75;
      double gap = ods_length (attr (names, values, r->hf_side == 1 ? "margin-bottom" : "margin-top")) * 0.75;

      if (r->hf_side == 1) r->layout->header_h = h + gap;
      else r->layout->footer_h = h + gap;
      return;
    }
  if (strcmp (name, "master-page") == 0)
    {
      const char *layout = attr (names, values, "page-layout-name");
      const char *mname = attr (names, values, "name");
      PageLayout *pl = layout != NULL ? g_hash_table_lookup (r->page_layouts, layout) : NULL;

      if (pl != NULL && mname != NULL)
        {
          g_hash_table_replace (r->master_pages, g_strdup (mname), pl);
          r->layout = pl;
          r->hf_side = 0;
        }
      else
        r->layout = NULL;
      return;
    }
  if (r->layout != NULL && (strcmp (name, "header") == 0 || strcmp (name, "footer") == 0) &&
      g_hash_table_size (r->master_pages) > 0)
    {
      const char *display = attr (names, values, "display");
      gboolean shown = display == NULL || strcmp (display, "false") != 0;

      r->hf_side = name[0] == 'h' ? 1 : 2;
      r->hf_region = 1;
      o42_fmt_init_default (&r->hf_cur);
      o42_fmt_init_default (&r->hf_want);
      r->hf_paragraphs = 0;
      if (r->hf_side == 1) r->layout->header_shown = shown;
      else r->layout->footer_shown = shown;
      for (int i = 0; i < 3; i++)
        {
          if (r->hf_parts[r->hf_side - 1][i] == NULL)
            r->hf_parts[r->hf_side - 1][i] = g_string_new (NULL);
          g_string_truncate (r->hf_parts[r->hf_side - 1][i], 0);
        }
      return;
    }
  if (r->layout != NULL && r->hf_side != 0 && g_str_has_prefix (name, "region-"))
    {
      r->hf_region = name[7] == 'l' ? 0 : name[7] == 'c' ? 1 : 2;
      o42_fmt_init_default (&r->hf_cur);
      o42_fmt_init_default (&r->hf_want);
      r->hf_paragraphs = 0;
      return;
    }
  if (r->layout != NULL && r->hf_side != 0 && g_hash_table_size (r->master_pages) > 0)
    {
      /* The fields of a header, as Excel's codes. */
      GString *part = r->hf_parts[r->hf_side - 1][r->hf_region];
      const char *code = strcmp (name, "page-number") == 0 ? "&P" : strcmp (name, "page-count") == 0 ? "&N"
                       : strcmp (name, "sheet-name") == 0 ? "&A" : strcmp (name, "date") == 0 ? "&D"
                       : strcmp (name, "time") == 0 ? "&T" : strcmp (name, "file-name") == 0 ? "&F" : NULL;

      if (code != NULL)
        {
          hf_sync (r);
          g_string_append (part, code);
          r->in_p = FALSE;   /* the field's own text is not copied */
          return;
        }
      if (strcmp (name, "s") == 0 || strcmp (name, "tab") == 0)
        {
          int n = attr_int (names, values, "c", 1);
          hf_sync (r);
          for (int i = 0; i < n; i++)
            g_string_append_c (part, name[0] == 's' ? ' ' : '\t');
          return;
        }
      if (strcmp (name, "line-break") == 0)
        {
          g_string_append_c (part, '\n');
          return;
        }
      if (strcmp (name, "span") == 0)
        {
          const char *sname = attr (names, values, "style-name");
          Style *st = sname != NULL ? g_hash_table_lookup (r->styles, sname) : NULL;

          if (st != NULL)
            r->hf_want = st->fmt;
        }
      if (strcmp (name, "p") == 0)
        {
          if (r->hf_paragraphs++ > 0)
            g_string_append_c (part, '\n');
          r->in_p = TRUE;
        }
      return;
    }

  /* Styles. */
  if (strcmp (name, "gradient") == 0 && attr (names, values, "name") != NULL)
    {
      OdsGradient *g = g_new0 (OdsGradient, 1);
      const char *angle = attr (names, values, "angle");

      g->start = colour_of (attr (names, values, "start-color"), 0xFFFFFF);
      g->end = colour_of (attr (names, values, "end-color"), 0x000000);
      g->angle = angle != NULL ? g_ascii_strtod (angle, NULL) / (strstr (angle, "deg") != NULL ? 1 : 10) : 0;
      g_hash_table_replace (r->gradients, g_strdup (attr (names, values, "name")), g);
      return;
    }
  if (strcmp (name, "hatch") == 0 && attr (names, values, "name") != NULL)
    {
      OdsHatch *h = g_new0 (OdsHatch, 1);
      const char *hstyle = attr (names, values, "style");
      const char *rotation = attr (names, values, "rotation");
      double rot = rotation != NULL ? fmod (g_ascii_strtod (rotation, NULL) / (strstr (rotation, "deg") != NULL ? 1 : 10), 180) : 0;
      gboolean thin = ods_length (attr (names, values, "distance")) > 5;
      gboolean crossed = hstyle != NULL && strcmp (hstyle, "single") != 0;

      h->colour = colour_of (attr (names, values, "color"), 0);
      if (crossed)
        h->pattern = rot > 22 && rot < 68 ? (thin ? O42_PATTERN_THIN_TRELLIS : O42_PATTERN_TRELLIS)
                                          : (thin ? O42_PATTERN_THIN_GRID : O42_PATTERN_GRID);
      else if (rot > 22 && rot < 68) h->pattern = thin ? O42_PATTERN_THIN_UP : O42_PATTERN_UP;
      else if (rot >= 68 && rot < 112) h->pattern = thin ? O42_PATTERN_THIN_VERTICAL : O42_PATTERN_VERTICAL;
      else if (rot >= 112 && rot < 158) h->pattern = thin ? O42_PATTERN_THIN_DOWN : O42_PATTERN_DOWN;
      else h->pattern = thin ? O42_PATTERN_THIN_HORIZONTAL : O42_PATTERN_HORIZONTAL;
      g_hash_table_replace (r->hatches, g_strdup (attr (names, values, "name")), h);
      return;
    }
  if (strcmp (name, "stroke-dash") == 0 && attr (names, values, "name") != NULL)
    {
      g_hash_table_replace (r->dashes, g_strdup (attr (names, values, "name")),
                            GINT_TO_POINTER (dash_from_definition (attr (names, values, "dots1-length"),
                                                                   attr (names, values, "dots2"),
                                                                   attr (names, values, "distance"))));
      return;
    }
  if (strcmp (name, "style") == 0 && attr (names, values, "family") != NULL)
    {
      const char *sname = attr (names, values, "name");
      Style *st = g_new0 (Style, 1);
      o42_fmt_init_default (&st->fmt);
      st->tab_colour = O42_TAB_NO_COLOUR;
      st->data_style = g_strdup (attr (names, values, "data-style-name"));
      st->master_page = g_strdup (attr (names, values, "master-page-name"));
      if (st->data_style != NULL) st->has_fmt = TRUE;
      if (sname != NULL) g_hash_table_replace (r->styles, g_strdup (sname), st);
      else style_free (st);
      r->style = sname != NULL ? st : NULL;
      return;
    }
  if (r->style != NULL)
    {
      Style *st = r->style;
      if (strcmp (name, "table-properties") == 0)
        {
          const char *tab = attr (names, values, "tab-color");
          const char *display = attr (names, values, "display");

          if (tab != NULL)
            st->tab_colour = colour_of (tab, O42_TAB_NO_COLOUR);
          st->table_hidden = display != NULL && strcmp (display, "false") == 0;
        }
      else if (strcmp (name, "table-column-properties") == 0)
        {
          const char *brk = attr (names, values, "break-before");
          st->width = length_px (attr (names, values, "column-width"));
          st->page_break = brk != NULL && strcmp (brk, "page") == 0;
        }
      else if (strcmp (name, "table-row-properties") == 0)
        {
          const char *brk = attr (names, values, "break-before");
          st->height = length_px (attr (names, values, "row-height"));
          st->page_break = brk != NULL && strcmp (brk, "page") == 0;
        }
      else if (strcmp (name, "graphic-properties") == 0)
        {
          const char *fill = attr (names, values, "fill");
          const char *fill_colour = attr (names, values, "fill-color");
          const char *stroke = attr (names, values, "stroke");
          const char *stroke_colour = attr (names, values, "stroke-color");
          const char *stroke_width = attr (names, values, "stroke-width");
          const char *dash = attr (names, values, "stroke-dash");
          const char *ms = attr (names, values, "marker-start");
          const char *me = attr (names, values, "marker-end");
          const char *msw = attr (names, values, "marker-start-width");
          const char *mew = attr (names, values, "marker-end-width");

          {
            const char *tva = attr (names, values, "textarea-vertical-align");
            const char *pad = attr (names, values, "padding");
            const char *wrap = attr (names, values, "wrap-option");

            st->text_valign = tva == NULL ? -1 : strcmp (tva, "top") == 0 ? O42_VALIGN_TOP
                            : strcmp (tva, "middle") == 0 ? O42_VALIGN_MIDDLE : O42_VALIGN_BOTTOM;
            st->text_padding = pad != NULL ? ods_length (pad) : -1;
            st->text_nowrap = wrap == NULL ? -1 : strcmp (wrap, "no-wrap") == 0 ? 1 : 0;
          }
          st->graphic = TRUE;
          st->fill_none = fill != NULL && strcmp (fill, "none") == 0;
          if (fill != NULL && strcmp (fill, "gradient") == 0)
            st->gradient_name = g_strdup (attr (names, values, "fill-gradient-name"));
          else if (fill != NULL && strcmp (fill, "hatch") == 0)
            st->hatch_name = g_strdup (attr (names, values, "fill-hatch-name"));
          {
            const char *shadow = attr (names, values, "shadow");
            if (shadow != NULL && strcmp (shadow, "visible") == 0)
              {
                st->shadow = TRUE;
                st->shadow_colour = colour_of (attr (names, values, "shadow-color"), 0x808080);
                st->shadow_dx = ods_length (attr (names, values, "shadow-offset-x"));
                st->shadow_dy = ods_length (attr (names, values, "shadow-offset-y"));
              }
          }
          st->fill = fill_colour != NULL ? colour_of (fill_colour, 0xFFFFFF) : 0xFFFFFF;
          st->stroke_none = stroke != NULL && strcmp (stroke, "none") == 0;
          st->line = stroke_colour != NULL ? colour_of (stroke_colour, 0) : 0;
          st->line_width = stroke_width != NULL ? floor (ods_length (stroke_width) * 100 + 0.5) / 100 : 0;
          st->dash = graphic_dash (r, stroke != NULL && strcmp (stroke, "dash") == 0 ? dash : NULL);
          st->head_start = graphic_head (ms);
          st->head_end = graphic_head (me);
          st->head_start_size = graphic_head_size (msw);
          st->head_end_size = graphic_head_size (mew);
        }
      else if (strcmp (name, "table-cell-properties") == 0)
        {
          const char *bg = attr (names, values, "background-color");
          const char *wrap = attr (names, values, "wrap-option");
          const char *shrink = attr (names, values, "shrink-to-fit");
          const char *va = attr (names, values, "vertical-align");
          const char *border = attr (names, values, "border");
          st->has_fmt = TRUE;
          if (bg != NULL && strcmp (bg, "transparent") != 0) st->fmt.fill = colour_of (bg, O42_FILL_NONE);
          if (wrap != NULL && strcmp (wrap, "wrap") == 0) st->fmt.wrap = 1;
          if (shrink != NULL && strcmp (shrink, "true") == 0) st->fmt.shrink = 1;
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
          if (family == NULL && fname != NULL)
            {
              /* A font-face declaration's name, which LibreOffice makes
               * "Arial1" for a second Arial; the face says the family. */
              const char *declared = g_hash_table_lookup (r->font_faces, fname);
              family = declared != NULL ? declared : fname;
            }
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
      if (ns->number != O42_NUM_TEXT)
        {
          /* A style says its shape in elements; they are put back
           * together as the code they came from, so the format keeps
           * the order and the names it was written with.  A number
           * style's code is used only when it holds more than a
           * preset does. */
          const char *lang = attr (names, values, "language");

          ns->code = g_string_new (NULL);
          ns->lang = lang != NULL ? language_lcid (lang) : 0;
        }
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
          if (dp != NULL) ns->decimals = attr_places (names, values, "decimal-places", 0);
          else if (ns->number == O42_NUM_FIXED) ns->number = O42_NUM_GENERAL;
          if (grouping != NULL && strcmp (grouping, "true") == 0) ns->grouping = TRUE;
          if (ns->code != NULL)
            {
              /* The digit places back as a code: "#,##0.00" and its kin. */
              int mi = attr_places (names, values, "min-integer-digits", 1);
              int places = attr_places (names, values, "decimal-places", 0);
              int min_places = attr_places (names, values, "min-decimal-places", places);
              double factor = attr_double (names, values, "display-factor", 1);
              GString *digits = g_string_new (NULL);

              for (int i = 0; i < mi; i++) g_string_append_c (digits, '0');
              if (mi == 0) g_string_append_c (digits, '#');
              if (ns->grouping)
                {
                  GString *grouped = g_string_new (NULL);
                  while (digits->len < 4) g_string_prepend_c (digits, '#');
                  for (gsize i = 0; i < digits->len; i++)
                    {
                      if (i > 0 && (digits->len - i) % 3 == 0) g_string_append_c (grouped, ',');
                      g_string_append_c (grouped, digits->str[i]);
                    }
                  g_string_free (digits, TRUE);
                  digits = grouped;
                }
              if (places > 0)
                {
                  g_string_append_c (digits, '.');
                  for (int i = 0; i < places; i++) g_string_append_c (digits, i < min_places ? '0' : '#');
                }
              /* A comma a thousand; an infinite factor would be commas
               * for ever, and a code has room for about a hundred. */
              for (int k = 0; factor >= 1000 && k < 100; factor /= 1000, k++)
                g_string_append_c (digits, ',');
              g_string_append (ns->code, digits->str);
              g_string_free (digits, TRUE);
              /* More than a preset says: a scale, several leading
               * noughts or none, or optional decimals. */
              if (attr_double (names, values, "display-factor", 1) >= 1000 || mi != 1 || min_places != places)
                ns->custom = TRUE;
            }
        }
      else if (strcmp (name, "fraction") == 0 && ns->code != NULL)
        {
          /* "# ?/?", "# ??/??" or "# ?/8": the whole number when the
           * style asks for integer digits, then the numerator's places
           * over the denominator's, or over the denominator itself. */
          int mi = attr_places (names, values, "min-integer-digits", 0);
          int num = attr_places (names, values, "min-numerator-digits", 1);
          int den = attr_places (names, values, "min-denominator-digits", 0);
          double den_value = attr_double (names, values, "denominator-value", 0);

          if (mi > 0) g_string_append (ns->code, "# ");
          for (int i = 0; i < MAX (num, 1); i++) g_string_append_c (ns->code, '?');
          g_string_append_c (ns->code, '/');
          if (den_value > 0 && den_value < 1e9) g_string_append_printf (ns->code, "%.0f", den_value);
          else for (int i = 0; i < MAX (den, 1); i++) g_string_append_c (ns->code, '?');
          ns->custom = TRUE;
        }
      else if (strcmp (name, "scientific-number") == 0)
        {
          int exp_digits = attr_places (names, values, "min-exponent-digits", 2);
          int mi = attr_places (names, values, "min-integer-digits", 1);
          ns->number = O42_NUM_SCIENTIFIC;
          ns->decimals = attr_places (names, values, "decimal-places", 2);
          if (ns->code != NULL)
            {
              for (int i = 0; i < MAX (mi, 1); i++) g_string_append_c (ns->code, '0');
              if (ns->decimals > 0) g_string_append_c (ns->code, '.');
              for (int i = 0; i < ns->decimals; i++) g_string_append_c (ns->code, '0');
              g_string_append (ns->code, "E+");
              for (int i = 0; i < MAX (exp_digits, 1); i++) g_string_append_c (ns->code, '0');
            }
        }
      else if (strcmp (name, "hours") == 0 && ns->number == O42_NUM_DATE)
        ns->number = O42_NUM_DATETIME;
      else if (strcmp (name, "fill-character") == 0)
        {
          if (ns->number == O42_NUM_CURRENCY)
            ns->number = O42_NUM_ACCOUNTING;
          else
            ns->custom = TRUE;
          ns->in_fill = TRUE;
        }
      else if (strcmp (name, "map") == 0)
        {
          /* value()>=0, value()>0, value()<0: the other sections. */
          const char *condition = attr (names, values, "condition");
          const char *apply = attr (names, values, "apply-style-name");

          if (condition != NULL && apply != NULL)
            {
              if (strstr (condition, ">=0") != NULL) { g_free (ns->map_ge); ns->map_ge = g_strdup (apply); }
              else if (strstr (condition, ">0") != NULL) { g_free (ns->map_gt); ns->map_gt = g_strdup (apply); }
              else if (strstr (condition, "<0") != NULL) { g_free (ns->map_lt); ns->map_lt = g_strdup (apply); }
              ns->custom = TRUE;
            }
        }
      else if (strcmp (name, "text-properties") == 0 && ns->code != NULL)
        {
          /* A colour on a number style: [Red] and the others. */
          const char *colour = attr (names, values, "color");

          if (colour != NULL)
            for (guint i = 0; i < G_N_ELEMENTS (CODE_COLOURS); i++)
              if (g_ascii_strcasecmp (colour, CODE_COLOURS[i].hex) == 0)
                {
                  g_string_append_printf (ns->code, "[%s]", CODE_COLOURS[i].name);
                  ns->custom = TRUE;
                }
        }
      else if (strcmp (name, "currency-symbol") == 0)
        {
          const char *lang = attr (names, values, "language");

          ns->in_symbol = (ns->number == O42_NUM_CURRENCY || ns->number == O42_NUM_ACCOUNTING);
          ns->in_currency_symbol = TRUE;
          if (ns->code != NULL && lang != NULL && language_lcid (lang) != 0)
            g_string_append_printf (ns->code, "[$%s-%03X]", "", 0x400 | language_lcid (lang));
        }
      else if (strcmp (name, "text") == 0 &&
               (ns->number == O42_NUM_CURRENCY || ns->number == O42_NUM_ACCOUNTING))
        ns->in_symbol = TRUE;

      if (ns->code != NULL)
        {
          gboolean long_form = g_strcmp0 (attr (names, values, "style"), "long") == 0;
          gboolean textual = g_strcmp0 (attr (names, values, "textual"), "true") == 0;

          if (strcmp (name, "year") == 0)         g_string_append (ns->code, long_form ? "yyyy" : "yy");
          else if (strcmp (name, "month") == 0)   g_string_append (ns->code, textual ? (long_form ? "mmmm" : "mmm") : (long_form ? "mm" : "m"));
          else if (strcmp (name, "day") == 0)     g_string_append (ns->code, long_form ? "dd" : "d");
          else if (strcmp (name, "day-of-week") == 0) g_string_append (ns->code, long_form ? "dddd" : "ddd");
          else if (strcmp (name, "hours") == 0)   g_string_append (ns->code, long_form ? "hh" : "h");
          else if (strcmp (name, "minutes") == 0) g_string_append (ns->code, long_form ? "mm" : "m");
          else if (strcmp (name, "seconds") == 0) g_string_append (ns->code, long_form ? "ss" : "s");
          else if (strcmp (name, "am-pm") == 0)   g_string_append (ns->code, "AM/PM");
        }
      return;
    }

  /* Tables. */
  if (strcmp (name, "forms") == 0)
    {
      r->in_form = TRUE;
      return;
    }
  if (r->in_form)
    {
      if (strcmp (name, "button") == 0 || strcmp (name, "checkbox") == 0 ||
          strcmp (name, "radio") == 0 || strcmp (name, "value-range") == 0 ||
          strcmp (name, "listbox") == 0 || strcmp (name, "combobox") == 0 ||
          strcmp (name, "fixed-text") == 0 || strcmp (name, "frame") == 0)
        read_form_control (r, name, names, values);
      return;
    }

  if (strcmp (name, "control") == 0)
    {
      /* A control in <table:shapes>, placed from the sheet's corner
       * once the columns and rows are read. */
      LooseControl l;
      const char *id = attr (names, values, "control");

      if (id != NULL)
        {
          l.id = g_strdup (id);
          l.x = ods_length (attr (names, values, "x"));
          l.y = ods_length (attr (names, values, "y"));
          l.w = ods_length (attr (names, values, "width"));
          l.h = ods_length (attr (names, values, "height"));
          if (r->loose_controls == NULL)
            r->loose_controls = g_array_new (FALSE, FALSE, sizeof (LooseControl));
          g_array_append_val (r->loose_controls, l);
        }
      return;
    }

  if (strcmp (name, "calculation-settings") == 0)
    {
      o42_book_set_precision_as_displayed (r->book, g_strcmp0 (attr (names, values, "precision-as-shown"), "true") == 0);
      return;
    }

  if (strcmp (name, "null-date") == 0)
    {
      /* The day serial 0 falls on: 1904-01-01 is the Macintosh epoch,
       * and 1899-12-30 the usual one. */
      const char *when = attr (names, values, "date-value");
      o42_book_set_date_1904 (r->book, when != NULL && g_str_has_prefix (when, "1904"));
      return;
    }

  if (strcmp (name, "conditional-format") == 0 && r->sheet != NULL)
    {
      char *sheet_name = NULL;

      r->cf_have_range = ods_range (attr (names, values, "target-range-address"), &sheet_name, &r->cf_range);
      g_free (sheet_name);
      return;
    }
  if (strcmp (name, "condition") == 0 && r->cf_have_range && r->sheet != NULL)
    {
      /* "=5", ">=[.B1]", "between(1,5)", "formula-is([.B1]>2)": the rule,
       * and the named style it switches to. */
      const char *value = attr (names, values, "value");
      const char *sname = attr (names, values, "apply-style-name");
      Style *st = sname != NULL ? g_hash_table_lookup (r->styles, sname) : NULL;
      O42Condition c;
      static const struct { const char *prefix; O42CondOp op; } OPS[] = {
        { "!=", O42_COND_NOT_EQUAL }, { ">=", O42_COND_GREATER_EQUAL }, { "<=", O42_COND_LESS_EQUAL },
        { "=", O42_COND_EQUAL }, { ">", O42_COND_GREATER }, { "<", O42_COND_LESS },
      };
      const char *operand = NULL;
      char *inner = NULL;

      if (value == NULL || st == NULL)
        return;
      memset (&c, 0, sizeof c);
      c.range = r->cf_range;
      c.fmt = st->fmt;
      if (st->fmt.bold) c.mask |= O42_FMT_BOLD;
      if (st->fmt.italic) c.mask |= O42_FMT_ITALIC;
      if (st->fmt.underline) c.mask |= O42_FMT_UNDERLINE;
      if (st->fmt.colour != 0) c.mask |= O42_FMT_COLOUR;
      if (st->fmt.fill != O42_FILL_NONE) c.mask |= O42_FMT_FILL;
      if (g_str_has_prefix (value, "formula-is(") && g_str_has_suffix (value, ")"))
        {
          inner = g_strndup (value + 11, strlen (value) - 12);
          c.is_formula = TRUE;
          c.op = O42_COND_EQUAL;
          {
            char *f = formula_from_of (inner);
            c.expr1 = g_intern_string (f);
            g_free (f);
          }
        }
      else if ((g_str_has_prefix (value, "between(") || g_str_has_prefix (value, "not-between(")) &&
               g_str_has_suffix (value, ")"))
        {
          const char *open = strchr (value, '(');
          char *comma;

          c.op = value[0] == 'n' ? O42_COND_NOT_BETWEEN : O42_COND_BETWEEN;
          inner = g_strndup (open + 1, strlen (open) - 2);
          comma = strrchr (inner, ',');
          if (comma != NULL)
            {
              char *end = NULL;
              double n;

              *comma = '\0';
              n = g_ascii_strtod (comma + 1, &end);
              if (end != NULL && *end == '\0' && end != comma + 1)
                c.value2 = n;
              else
                { char *f = formula_from_of (comma + 1); c.expr2 = g_intern_string (f); g_free (f); }
            }
          operand = inner;
        }
      else
        {
          for (guint i = 0; i < G_N_ELEMENTS (OPS); i++)
            if (g_str_has_prefix (value, OPS[i].prefix))
              { c.op = OPS[i].op; operand = value + strlen (OPS[i].prefix); break; }
          if (operand == NULL)
            return;
        }
      if (operand != NULL)
        {
          char *end = NULL;
          double n = g_ascii_strtod (operand, &end);

          if (end != NULL && *end == '\0' && end != operand)
            c.value = n;
          else
            { char *f = formula_from_of (operand); c.expr1 = g_intern_string (f); g_free (f); }
        }
      if (c.expr2 == NULL && c.op != O42_COND_BETWEEN && c.op != O42_COND_NOT_BETWEEN)
        c.value2 = c.value;
      o42_sheet_add_condition (r->sheet, &c);
      g_free (inner);
      return;
    }
  if (strcmp (name, "color-scale") == 0 && r->cf_have_range)
    {
      memset (&r->cf_scale, 0, sizeof r->cf_scale);
      o42_fmt_init_default (&r->cf_scale.fmt);
      r->cf_scale.range = r->cf_range;
      r->cf_scale.kind = O42_COND_SCALE;
      r->cf_in_scale = TRUE;
      return;
    }
  if (strcmp (name, "color-scale-entry") == 0 && r->cf_in_scale && r->cf_scale.stops < 3)
    {
      const char *type = attr (names, values, "type");
      const char *val = attr (names, values, "value");
      const char *colour = attr (names, values, "color");
      int k = r->cf_scale.stops;
      int kind = O42_SCALE_MIN;

      if (type != NULL)
        {
          if (strcmp (type, "maximum") == 0) kind = O42_SCALE_MAX;
          else if (strcmp (type, "number") == 0 || strcmp (type, "formula") == 0) kind = O42_SCALE_NUM;
          else if (strcmp (type, "percent") == 0) kind = O42_SCALE_PERCENT;
          else if (strcmp (type, "percentile") == 0) kind = O42_SCALE_PERCENTILE;
        }
      r->cf_scale.stop_type[k] = kind;
      r->cf_scale.stop_value[k] = val != NULL ? g_ascii_strtod (val, NULL) : 0;
      r->cf_scale.stop_colour[k] = colour != NULL ? colour_of (colour, 0xFFFFFF) : 0xFFFFFF;
      r->cf_scale.stops = k + 1;
      return;
    }
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
      {
        const char *sname = attr (names, values, "style-name");
        const char *ranges = attr (names, values, "print-ranges");
        Style *st = sname != NULL ? g_hash_table_lookup (r->styles, sname) : NULL;
        PageLayout *pl = st != NULL && st->master_page != NULL
                         ? g_hash_table_lookup (r->master_pages, st->master_page) : NULL;

        if (st != NULL && st->tab_colour != O42_TAB_NO_COLOUR && r->sheet != NULL)
          o42_sheet_set_tab_colour (r->sheet, st->tab_colour);
        if (st != NULL && st->table_hidden && r->sheet != NULL)
          o42_sheet_set_hidden (r->sheet, TRUE);
        if (pl != NULL && r->sheet != NULL)
          {
            /* The header band takes the body down from the paper's edge
             * to the header margin and on by its height. */
            O42PrintSetup ps = pl->ps;

            if (pl->header_shown)
              ps.margin_top = ps.margin_header + pl->header_h;
            else
              ps.header = (char *) "";
            if (pl->footer_shown)
              ps.margin_bottom = ps.margin_footer + pl->footer_h;
            else
              ps.footer = (char *) "";
            o42_sheet_set_print_setup (r->sheet, &ps);
          }
        if (ranges != NULL && r->sheet != NULL)
          {
            /* Sheet.A1:Sheet.C5 Sheet.E1:Sheet.F9: an area per word. */
            O42Range areas[O42_PRINT_AREAS_MAX];
            int n = 0;
            char **words = g_strsplit (ranges, " ", -1);

            for (int k = 0; words[k] != NULL && n < O42_PRINT_AREAS_MAX; k++)
              if (*words[k] != '\0' && ods_range (words[k], NULL, &areas[n]))
                n++;
            g_strfreev (words);
            if (n > 0)
              o42_sheet_set_print_areas (r->sheet, areas, n);
          }
      }
      r->in_header_rows = r->in_header_cols = 0;
      r->n_tables++;
      r->row = 0;
      r->col = 0;
      g_ptr_array_set_size (r->col_styles, 0);
      return;
    }
  if (strcmp (name, "table-header-columns") == 0 && r->sheet != NULL)
    {
      r->in_header_cols = 1;
      r->header_cols_from = r->col;
      return;
    }
  if (strcmp (name, "table-header-rows") == 0 && r->sheet != NULL)
    {
      r->in_header_rows = 1;
      r->header_rows_from = r->row;
      return;
    }
  if (strcmp (name, "table-column") == 0 && r->sheet != NULL)
    {
      int repeat = attr_int (names, values, "number-columns-repeated", 1);
      const char *sname = attr (names, values, "style-name");
      const char *cell_style_name = attr (names, values, "default-cell-style-name");
      const char *vis = attr (names, values, "visibility");
      Style *st = sname != NULL ? g_hash_table_lookup (r->styles, sname) : NULL;
      if (st != NULL && st->page_break && r->col > 0 && !o42_sheet_page_break (r->sheet, FALSE, r->col))
        o42_sheet_toggle_page_break (r->sheet, FALSE, r->col);
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
  if (strcmp (name, "content-validation") == 0 && attr (names, values, "name") != NULL)
    {
      O42Validation *v = g_new0 (O42Validation, 1);
      const char *cond = attr (names, values, "condition");
      const char *empty = attr (names, values, "allow-empty-cell");
      const char *list = attr (names, values, "display-list");

      v->allow_blank = empty == NULL || strcmp (empty, "false") != 0;
      v->no_dropdown = list != NULL && (strcmp (list, "none") == 0 || strcmp (list, "no") == 0);
      v->value = g_strdup ("");
      v->value2 = g_strdup ("");
      v->message = g_strdup ("");
      v->title = g_strdup ("");
      v->prompt = g_strdup ("");
      v->prompt_title = g_strdup ("");
      if (cond != NULL)
        ods_validation_condition (cond, v);
      g_hash_table_replace (r->valid_defs, g_strdup (attr (names, values, "name")), v);
      r->valid_cur = v;
      return;
    }
  if (r->valid_cur != NULL && (strcmp (name, "help-message") == 0 || strcmp (name, "error-message") == 0))
    {
      const char *title = attr (names, values, "title");
      const char *kind = attr (names, values, "message-type");
      const char *display = attr (names, values, "display");

      r->valid_msg = name[0] == 'h' ? 1 : 2;
      if (r->valid_msg == 1)
        { g_free (r->valid_cur->prompt_title); r->valid_cur->prompt_title = g_strdup (title ? title : ""); }
      else
        {
          g_free (r->valid_cur->title);
          r->valid_cur->title = g_strdup (title ? title : "");
          r->valid_cur->style = kind != NULL && strcmp (kind, "warning") == 0 ? O42_VALID_WARNING
                              : kind != NULL && strcmp (kind, "information") == 0 ? O42_VALID_INFORMATION : O42_VALID_STOP;
          r->valid_cur->no_error = display != NULL && strcmp (display, "false") == 0;
        }
      return;
    }
  if (r->valid_msg != 0 && strcmp (name, "p") == 0)
    {
      r->in_p = TRUE;
      return;
    }
  if ((strcmp (name, "table-cell") == 0 || strcmp (name, "covered-table-cell") == 0) && r->sheet != NULL)
    {
      r->in_cell = TRUE;
      r->depth_in_cell = 0;
      r->covered = name[0] == 'c';
      g_free (r->cell_valid);
      r->cell_valid = g_strdup (attr (names, values, "content-validation-name"));
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
       strcmp (name, "circle") == 0 || strcmp (name, "line") == 0 ||
       strcmp (name, "custom-shape") == 0 || strcmp (name, "polygon") == 0 ||
       strcmp (name, "polyline") == 0 || strcmp (name, "path") == 0))
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
  if (r->layout != NULL && r->hf_side != 0 && g_hash_table_size (r->master_pages) > 0 &&
      (strcmp (name, "header") == 0 || strcmp (name, "footer") == 0))
    {
      /* The three regions, joined in Excel's notation. */
      GString *joined = g_string_new (NULL);
      static const char *const codes[3] = { "&L", "&C", "&R" };

      for (int i = 0; i < 3; i++)
        if (r->hf_parts[r->hf_side - 1][i]->len > 0)
          {
            g_string_append (joined, codes[i]);
            g_string_append (joined, r->hf_parts[r->hf_side - 1][i]->str);
          }
      if (r->hf_side == 1) { g_free (r->layout->ps.header); r->layout->ps.header = g_string_free (joined, FALSE); }
      else { g_free (r->layout->ps.footer); r->layout->ps.footer = g_string_free (joined, FALSE); }
      r->hf_side = 0;
      r->in_p = FALSE;
      return;
    }
  if (r->layout != NULL && r->hf_side != 0 && g_hash_table_size (r->master_pages) > 0)
    {
      if (strcmp (name, "span") == 0)
        o42_fmt_init_default (&r->hf_want);
      else if (strcmp (name, "p") == 0)
        r->in_p = FALSE;
      else if (strcmp (name, "page-number") == 0 || strcmp (name, "page-count") == 0 ||
               strcmp (name, "sheet-name") == 0 || strcmp (name, "date") == 0 ||
               strcmp (name, "time") == 0 || strcmp (name, "file-name") == 0)
        r->in_p = TRUE;
      return;
    }
  if (strcmp (name, "master-page") == 0 || strcmp (name, "page-layout") == 0)
    {
      r->layout = NULL;
      r->hf_side = 0;
      return;
    }
  if (strcmp (name, "table-header-columns") == 0 && r->sheet != NULL)
    {
      const O42PrintSetup *ps = o42_sheet_print_setup (r->sheet);
      if (r->col > r->header_cols_from)
        o42_sheet_set_print_title_ranges (r->sheet, ps->title_row_first, ps->title_row_first + ps->title_rows - 1,
                                          r->header_cols_from, r->col - 1);
      r->in_header_cols = 0;
      return;
    }
  if (strcmp (name, "table-header-rows") == 0 && r->sheet != NULL)
    {
      const O42PrintSetup *ps = o42_sheet_print_setup (r->sheet);
      if (r->row > r->header_rows_from)
        o42_sheet_set_print_title_ranges (r->sheet, r->header_rows_from, r->row - 1,
                                          ps->title_col_first, ps->title_col_first + ps->title_cols - 1);
      r->in_header_rows = 0;
      return;
    }
  if (strcmp (name, "style") == 0)
    r->style = NULL;
  else if (r->in_num_style && g_str_has_suffix (name, "-style"))
    {
      if (r->num != NULL && r->num->code != NULL && r->num->lang != 0 &&
          r->num->code->len > 0)
        {
          /* Excel's LCID for the default form of a language: the
           * sub-language is 1, which is the 0x400 above it. */
          char *lead = g_strdup_printf ("[$-%03X]", 0x400 | r->num->lang);

          g_string_prepend (r->num->code, lead);
          g_free (lead);
        }
      if (r->num != NULL && r->num->code != NULL &&
          (r->num->map_ge != NULL || (r->num->map_gt != NULL && r->num->map_lt != NULL)))
        {
          /* The sections the maps name come first, in the order a code
           * has them: positive, negative, and this one for zero -- or
           * this one for the negatives when there are two. */
          NumStyle *pos = g_hash_table_lookup (r->num_styles, r->num->map_ge != NULL ? r->num->map_ge : r->num->map_gt);
          NumStyle *neg = r->num->map_lt != NULL ? g_hash_table_lookup (r->num_styles, r->num->map_lt) : NULL;
          GString *whole = g_string_new (NULL);

          if (pos != NULL && pos->code != NULL)
            g_string_append_printf (whole, "%s;", pos->code->str);
          if (neg != NULL && neg->code != NULL)
            g_string_append_printf (whole, "%s;", neg->code->str);
          g_string_append (whole, r->num->code->str);
          g_string_assign (r->num->code, whole->str);
          g_string_free (whole, TRUE);
        }
      r->in_num_style = FALSE;
      r->num = NULL;
    }
  else if (strcmp (name, "color-scale") == 0 && r->cf_in_scale)
    {
      r->cf_in_scale = FALSE;
      if (r->cf_scale.stops >= 2 && r->sheet != NULL)
        o42_sheet_add_condition (r->sheet, &r->cf_scale);
    }
  else if (strcmp (name, "conditional-format") == 0)
    r->cf_have_range = FALSE;
  else if (strcmp (name, "table-row") == 0 && r->sheet != NULL)
    row_finish (r);
  else if (strcmp (name, "forms") == 0)
    r->in_form = FALSE;
  else if (strcmp (name, "table") == 0)
    {
      place_loose_controls (r);
      apply_validations (r);
      r->sheet = NULL;
    }
  else if (strcmp (name, "content-validation") == 0)
    r->valid_cur = NULL;
  else if (r->valid_msg != 0 && (strcmp (name, "help-message") == 0 || strcmp (name, "error-message") == 0))
    r->valid_msg = 0;
  else if (r->valid_msg != 0 && strcmp (name, "p") == 0)
    r->in_p = FALSE;
}

/* ---- meta.xml: File > Properties ---- */

static void
meta_start (GMarkupParseContext *ctx, const char *element, const char **names,
            const char **values, gpointer user, GError **error)
{
  Reader *r = user;
  const char *name = local (element);
  const char *user_name = attr (names, values, "name");
  O42Property which;
  (void) ctx; (void) error;
  r->meta_prop = -1;
  if (strcmp (name, "title") == 0)                r->meta_prop = O42_PROP_TITLE;
  else if (strcmp (name, "subject") == 0)         r->meta_prop = O42_PROP_SUBJECT;
  else if (strcmp (name, "initial-creator") == 0) r->meta_prop = O42_PROP_AUTHOR;
  else if (strcmp (name, "creator") == 0 && *o42_book_property (r->book, O42_PROP_AUTHOR) == '\0')
    r->meta_prop = O42_PROP_AUTHOR;
  else if (strcmp (name, "keyword") == 0)         r->meta_prop = O42_PROP_KEYWORDS;
  else if (strcmp (name, "description") == 0)     r->meta_prop = O42_PROP_COMMENTS;
  else if (strcmp (name, "user-defined") == 0 && user_name != NULL && o42_property_parse (user_name, &which))
    r->meta_prop = which;
  if (r->meta_prop >= 0)
    {
      if (r->meta_text == NULL)
        r->meta_text = g_string_new (NULL);
      g_string_truncate (r->meta_text, 0);
    }
}

static void
meta_end (GMarkupParseContext *ctx, const char *element, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) element; (void) error;
  if (r->meta_prop >= 0)
    {
      /* Several keywords are one list, as Excel keeps them. */
      if (r->meta_prop == O42_PROP_KEYWORDS && *o42_book_property (r->book, O42_PROP_KEYWORDS) != '\0')
        {
          char *joined = g_strconcat (o42_book_property (r->book, O42_PROP_KEYWORDS), ", ", r->meta_text->str, NULL);
          o42_book_set_property (r->book, O42_PROP_KEYWORDS, joined);
          g_free (joined);
        }
      else
        o42_book_set_property (r->book, (O42Property) r->meta_prop, r->meta_text->str);
      r->meta_prop = -1;
    }
}

static void
meta_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (r->meta_prop >= 0)
    g_string_append_len (r->meta_text, text, (gssize) len);
}

static void
content_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer user, GError **error)
{
  Reader *r = user;
  (void) ctx; (void) error;
  if (r->in_num_style && r->num != NULL && r->num->in_fill)
    {
      /* "* " in a code: the character that fills the cell. */
      if (r->num->code != NULL && len > 0)
        {
          g_string_append_c (r->num->code, '*');
          g_string_append_len (r->num->code, text, g_utf8_next_char (text) - text);
        }
      r->num->in_fill = FALSE;
      return;
    }
  if (r->in_num_style && r->num != NULL && r->num->in_currency_symbol)
    {
      /* The symbol goes into the code in brackets, [$€-407] when the
       * element named a language and [$€] when not; the language was
       * opened already. */
      char *piece = g_strndup (text, len);

      if (r->num->code != NULL && *piece != '\0')
        {
          if (r->num->code->len >= 2 && g_str_has_suffix (r->num->code->str, "]") &&
              strrchr (r->num->code->str, '[') != NULL && strrchr (r->num->code->str, '[')[1] == '$' &&
              strrchr (r->num->code->str, '[')[2] == '-')
            {
              /* "[$-407]" is open for the symbol: put it after the "$". */
              char *open = strrchr (r->num->code->str, '[');
              gsize at = (gsize) (open + 2 - r->num->code->str);
              g_string_insert (r->num->code, (gssize) at, piece);
            }
          else
            g_string_append_printf (r->num->code, "[$%s]", piece);
        }
      if (r->num->in_symbol)
        {
          char *stripped = g_strstrip (g_strdup (piece));
          if (*stripped != '\0' && strlen (r->num->symbol) + strlen (stripped) < sizeof r->num->symbol)
            strcat (r->num->symbol, stripped);
          g_free (stripped);
          r->num->in_symbol = FALSE;
        }
      g_free (piece);
      r->num->in_currency_symbol = FALSE;
      return;
    }
  if (r->in_num_style && r->num != NULL && r->num->in_symbol)
    {
      /* The currency's symbol, or the text before or after the number
       * that stands for one: spaces around it are not the symbol. */
      char *piece = g_strstrip (g_strndup (text, len));

      if (*piece != '\0' && strlen (r->num->symbol) + strlen (piece) < sizeof r->num->symbol)
        strcat (r->num->symbol, piece);
      g_free (piece);
      r->num->in_symbol = FALSE;
      /* And into the code, where "-" before the symbol is what marks a
       * negative section. */
      if (r->num->code != NULL)
        {
          char *literal = g_strndup (text, len);
          gboolean plain = *literal != 0;

          for (const char *q = literal; plain && *q != 0; q++)
            if (strchr ("/-.:, ()", *q) == NULL)
              plain = FALSE;
          if (*literal != 0)
            g_string_append_printf (r->num->code, plain ? "%s" : "\"%s\"", literal);
          if (*literal != 0 && g_strstrip (literal)[0] != '\0' && strcmp (literal, "-") != 0)
            r->num->custom = TRUE;
          g_free (literal);
        }
      return;
    }
  if (r->in_num_style && r->num != NULL && r->num->code != NULL)
    {
      /* What stands between the fields of a date style is part of the
       * code, quoted so that a letter in it stays a letter; in a
       * number style it is what makes the code more than a preset --
       * except the % that every percentage carries. */
      char *literal = g_strndup (text, len);
      gboolean plain = *literal != 0;

      /* A separator that the code language reads as itself needs no
       * quotes; anything with a letter in it does. */
      for (const char *q = literal; plain && *q != 0; q++)
        if (strchr ("/-.:, ()%", *q) == NULL)
          plain = FALSE;
      if (*literal != 0)
        g_string_append_printf (r->num->code, plain ? "%s" : "\"%s\"", literal);
      if (*literal != 0 && r->num->number != O42_NUM_DATE && r->num->number != O42_NUM_TIME &&
          r->num->number != O42_NUM_DATETIME &&
          !(r->num->number == O42_NUM_PERCENT && strcmp (literal, "%") == 0))
        r->num->custom = TRUE;
      g_free (literal);
      return;
    }
  if (r->valid_cur != NULL && r->valid_msg != 0 && r->in_p)
    {
      char **slot = r->valid_msg == 1 ? &r->valid_cur->prompt : &r->valid_cur->message;
      char *more = g_strndup (text, len);
      char *both = g_strconcat (*slot != NULL ? *slot : "", more, NULL);
      g_free (*slot);
      *slot = both;
      g_free (more);
      return;
    }
  if (r->layout != NULL && r->hf_side != 0 && r->in_p && g_hash_table_size (r->master_pages) > 0)
    {
      /* A header's own words; an ampersand is doubled, as Excel has it.
       * An empty span gives an empty text, which sets no style. */
      GString *part = r->hf_parts[r->hf_side - 1][r->hf_region];
      if (len == 0)
        return;
      hf_sync (r);
      for (gsize i = 0; i < len; i++)
        {
          if (text[i] == '&') g_string_append (part, "&&");
          else g_string_append_c (part, text[i]);
        }
      return;
    }
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
      r->setting_cursor_x = r->setting_cursor_y = 0;
      r->setting_zoom = 0;
      r->setting_grid = -1;
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
      gboolean truth = strcmp (r->setting_value->str, "true") == 0;
      if (strcmp (r->setting_name, "HorizontalSplitMode") == 0) r->hmode = v;
      else if (strcmp (r->setting_name, "VerticalSplitMode") == 0) r->vmode = v;
      else if (strcmp (r->setting_name, "HorizontalSplitPosition") == 0) r->split_cols = v;
      else if (strcmp (r->setting_name, "VerticalSplitPosition") == 0) r->split_rows = v;
      else if (strcmp (r->setting_name, "CursorPositionX") == 0) r->setting_cursor_x = v;
      else if (strcmp (r->setting_name, "CursorPositionY") == 0) r->setting_cursor_y = v;
      else if (strcmp (r->setting_name, "ZoomValue") == 0) r->setting_zoom = v;
      else if (strcmp (r->setting_name, "ShowGrid") == 0) r->setting_grid = truth ? 1 : 0;
      else if (strcmp (r->setting_name, "ShowZeroValues") == 0 && r->setting_table == NULL)
        r->view_zeros = truth ? 1 : 0;
      else if (strcmp (r->setting_name, "ActiveTable") == 0 && r->setting_table == NULL)
        {
          O42Sheet *sheet = o42_book_find_sheet (r->book, r->setting_value->str);
          for (int i = 0; sheet != NULL && i < o42_book_n_sheets (r->book); i++)
            {
              O42SheetView view = *o42_sheet_view (o42_book_sheet (r->book, i));
              view.selected = o42_book_sheet (r->book, i) == sheet;
              o42_sheet_set_view (o42_book_sheet (r->book, i), &view);
            }
        }
      /* The view's own ZoomValue and ShowGrid, outside any table, are
       * the shown sheet's; the tables' own entries came first. */
      if (r->setting_table == NULL &&
          (strcmp (r->setting_name, "ZoomValue") == 0 || strcmp (r->setting_name, "ShowGrid") == 0 ||
           strcmp (r->setting_name, "ShowZeroValues") == 0))
        for (int i = 0; i < o42_book_n_sheets (r->book); i++)
          {
            O42Sheet *sheet = o42_book_sheet (r->book, i);
            O42SheetView view = *o42_sheet_view (sheet);
            gboolean any_shown = FALSE;
            for (int k = 0; k < o42_book_n_sheets (r->book); k++)
              any_shown = any_shown || o42_sheet_view (o42_book_sheet (r->book, k))->selected;
            if (strcmp (r->setting_name, "ShowZeroValues") == 0) view.zeros = truth;
            else if (any_shown && !view.selected) continue;
            else if (strcmp (r->setting_name, "ZoomValue") == 0 && v > 0) view.zoom = v;
            else if (strcmp (r->setting_name, "ShowGrid") == 0) view.gridlines = truth;
            o42_sheet_set_view (sheet, &view);
          }
      g_clear_pointer (&r->setting_name, g_free);
    }
  else if (strcmp (name, "config-item-map-entry") == 0 && r->setting_table != NULL)
    {
      O42Sheet *sheet = o42_book_find_sheet (r->book, r->setting_table);
      if (sheet != NULL && (r->hmode == 2 || r->vmode == 2))
        o42_sheet_set_frozen (sheet, r->vmode == 2 ? r->split_rows : 0, r->hmode == 2 ? r->split_cols : 0);
      if (sheet != NULL)
        {
          O42SheetView view = *o42_sheet_view (sheet);
          view.active_row = r->setting_cursor_y;
          view.active_col = r->setting_cursor_x;
          view.selection = o42_range_normalise (view.active_row, view.active_col, view.active_row, view.active_col);
          if (r->setting_zoom > 0) view.zoom = r->setting_zoom;
          if (r->setting_grid >= 0) view.gridlines = r->setting_grid != 0;
          o42_sheet_set_view (sheet, &view);
        }
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
  if (xml == NULL || len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s is empty", path);
      return FALSE;
    }
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
  static const GMarkupParser meta_parser = { meta_start, meta_end, meta_text, NULL, NULL };
  GBytes *archive;
  GHashTable *parts;
  Reader r;
  gboolean ok;

  g_return_val_if_fail (book != NULL && G_IS_FILE (file), FALSE);

  archive = g_file_load_bytes (file, NULL, NULL, error);
  if (archive == NULL)
    return FALSE;
  {
    /* A flat OpenDocument file (.fods) is the one XML document with
     * the styles, the content and the settings in it: it stands for
     * every part at once, and is walked once for the content. */
    gsize len = 0;
    const char *head = g_bytes_get_data (archive, &len);
    gboolean flat = len > 5 && (g_str_has_prefix (head, "<?xml") || g_str_has_prefix (head, "<office:"));

    if (flat)
      {
        parts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
        g_hash_table_insert (parts, g_strdup ("content.xml"), g_bytes_ref (archive));
        g_hash_table_insert (parts, g_strdup ("settings.xml"), g_bytes_ref (archive));
      }
    else
      parts = o42_zip_read (archive, error);
  }
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
  r.page_layouts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, page_layout_free);
  r.valid_defs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, validation_free);
  r.valid_cells = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_array_unref);
  r.font_faces = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  r.master_pages = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  r.dashes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  r.gradients = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  r.hatches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  r.form_controls = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, form_control_free);
  r.num_styles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, num_style_free);
  r.col_styles = g_ptr_array_new_with_free_func (g_free);

  /* LibreOffice keeps number styles in styles.xml; the same walker
   * reads them, finding no tables there. */
  r.parts = parts;
  ok = (g_hash_table_lookup (parts, "styles.xml") == NULL ||
        parse_part (parts, "styles.xml", &content_parser, &r, error)) &&
       parse_part (parts, "content.xml", &content_parser, &r, error) &&
       parse_part (parts, "settings.xml", &settings_parser, &r, error);
  r.meta_prop = -1;
  if (ok && g_hash_table_lookup (parts, "meta.xml") != NULL)
    parse_part (parts, "meta.xml", &meta_parser, &r, NULL);
  if (r.meta_text != NULL)
    g_string_free (r.meta_text, TRUE);

  g_hash_table_unref (r.styles);
  g_hash_table_unref (r.master_pages);
  g_hash_table_unref (r.font_faces);
  g_hash_table_unref (r.page_layouts);
  g_hash_table_unref (r.valid_defs);
  g_hash_table_unref (r.valid_cells);
  g_free (r.cell_valid);
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 3; j++)
      if (r.hf_parts[i][j] != NULL)
        g_string_free (r.hf_parts[i][j], TRUE);
  g_hash_table_unref (r.dashes);
  g_hash_table_unref (r.gradients);
  g_hash_table_unref (r.hatches);
  g_hash_table_unref (r.form_controls);
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
