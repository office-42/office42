/* o42-text-formats.c - see o42-text-formats.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-text-formats.h"

#include "o42-formula.h"

#include <stdlib.h>
#include <string.h>

/* A number as the file wants it: the C locale, whatever the machine's
 * is, because a decimal comma would be read as a field separator. */
static void
append_number (GString *out, double value)
{
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];

  g_string_append (out, g_ascii_formatd (buffer, sizeof buffer, "%.15g", value));
}

static gboolean
write_text (GFile *file, GString *out, GError **error)
{
  gboolean ok = g_file_replace_contents (file, out->str, out->len, NULL, FALSE,
                                         G_FILE_CREATE_NONE, NULL, NULL, error);

  g_string_free (out, TRUE);
  return ok;
}

/* ====================================================================== */
/* DIF                                                                    */
/* ====================================================================== */

/* A DIF file is a header of triples -- a keyword, two numbers, a
 * string -- and then the data: one BOT per row and three lines per
 * cell.  A number is type 0 with the number beside it and V under it;
 * anything else is type 1 with the text under it in quotes. */

gboolean
o42_dif_save (O42Sheet *sheet, GFile *file, GError **error)
{
  O42Range used;
  GString *out = g_string_new (NULL);

  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  o42_sheet_used_range (sheet, &used);
  g_string_append_printf (out,
    "TABLE\r\n0,1\r\n\"%s\"\r\n"
    "VECTORS\r\n0,%d\r\n\"\"\r\n"
    "TUPLES\r\n0,%d\r\n\"\"\r\n"
    "DATA\r\n0,0\r\n\"\"\r\n",
    o42_sheet_get_name (sheet) != NULL ? o42_sheet_get_name (sheet) : "Sheet1",
    used.col1 + 1, used.row1 + 1);

  for (int row = 0; row <= used.row1; row++)
    {
      g_string_append (out, "-1,0\r\nBOT\r\n");
      for (int col = 0; col <= used.col1; col++)
        {
          O42Value value;

          o42_sheet_get_value (sheet, row, col, &value);
          if (value.type == O42_VALUE_NUMBER)
            {
              g_string_append (out, "0,");
              append_number (out, value.as.number);
              g_string_append (out, "\r\nV\r\n");
            }
          else if (value.type == O42_VALUE_EMPTY)
            g_string_append (out, "1,0\r\n\"\"\r\n");
          else
            {
              char *shown = o42_sheet_get_display (sheet, row, col);
              char **parts = g_strsplit (shown, "\"", -1);
              char *escaped = g_strjoinv ("\"\"", parts);

              g_string_append_printf (out, "1,0\r\n\"%s\"\r\n", escaped);
              g_free (escaped);
              g_strfreev (parts);
              g_free (shown);
            }
          o42_value_clear (&value);
        }
    }
  g_string_append (out, "-1,0\r\nEOD\r\n");

  return write_text (file, out, error);
}

gboolean
o42_dif_load (O42Sheet *sheet, GFile *file, GError **error)
{
  char *contents = NULL;
  gsize length = 0;
  char **lines;
  gboolean in_data = FALSE;
  int row = -1, col = 0;

  g_return_val_if_fail (sheet != NULL, FALSE);
  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return FALSE;

  /* The lines, with the empty ones left out: a DIF file counts
   * lines, and the blank a CRLF leaves behind would put the count
   * out. */
  {
    char **split = g_strsplit_set (contents, "\r\n", -1);
    GPtrArray *kept = g_ptr_array_new ();

    for (int i = 0; split[i] != NULL; i++)
      if (*split[i] != '\0')
        g_ptr_array_add (kept, g_strdup (split[i]));
    g_ptr_array_add (kept, NULL);
    lines = (char **) g_ptr_array_free (kept, FALSE);
    g_strfreev (split);
  }

  o42_sheet_begin_group (sheet);
  {
    O42Range all = { 0, 0, O42_MAX_ROWS - 1, O42_MAX_COLS - 1 };

    o42_sheet_clear_range (sheet, &all);
  }

  for (int i = 0; lines[i] != NULL; i++)
    {
      const char *line = lines[i];

      if (!in_data)
        {
          /* The header runs until the DATA triple's own string. */
          if (strcmp (line, "DATA") == 0)
            {
              /* the two lines after it belong to the header */
              if (lines[i + 1] != NULL && lines[i + 2] != NULL)
                i += 2;
              in_data = TRUE;
            }
          continue;
        }

      if (line[0] == '-' && line[1] == '1')
        {
          /* BOT or EOD stands on the next line. */
          const char *what = lines[i + 1] != NULL ? lines[i + 1] : "";

          if (strcmp (what, "BOT") == 0)
            { row++; col = 0; }
          else if (strcmp (what, "EOD") == 0)
            break;
          i++;
          continue;
        }

      if (row < 0 || row >= O42_MAX_ROWS || col >= O42_MAX_COLS)
        continue;

      if (line[0] == '0' && line[1] == ',')
        {
          /* A number, with V under it -- or NA, TRUE and the rest,
           * which are words we take as they are. */
          const char *under = lines[i + 1] != NULL ? lines[i + 1] : "V";

          if (strcmp (under, "V") == 0)
            {
              /* The number.  A second comma in it is a decimal comma:
               * LibreOffice writes the machine's separator here, where
               * the format and Excel both write a point. */
              const char *number = line + 2;
              char *fixed = NULL;
              const char *comma = strchr (number, ',');

              if (comma != NULL && strchr (comma + 1, ',') == NULL)
                {
                  fixed = g_strdup (number);
                  fixed[comma - number] = '.';
                }
              o42_sheet_set_input (sheet, row, col, fixed != NULL ? fixed : number);
              g_free (fixed);
            }
          else
            o42_sheet_set_input (sheet, row, col, under);
          col++;
          i++;
        }
      else if (line[0] == '1' && line[1] == ',')
        {
          const char *under = lines[i + 1] != NULL ? lines[i + 1] : "\"\"";
          gsize n = strlen (under);
          char *text;

          if (n >= 2 && under[0] == '"' && under[n - 1] == '"')
            {
              char *inner = g_strndup (under + 1, n - 2);
              char **parts = g_strsplit (inner, "\"\"", -1);

              text = g_strjoinv ("\"", parts);
              g_strfreev (parts);
              g_free (inner);
            }
          else
            text = g_strdup (under);
          o42_sheet_set_input (sheet, row, col, text);
          g_free (text);
          col++;
          i++;
        }
    }

  o42_sheet_end_group (sheet);
  g_strfreev (lines);
  g_free (contents);
  return TRUE;
}

/* ====================================================================== */
/* SYLK                                                                   */
/* ====================================================================== */

/* SYLK counts from one and writes its formulas in R1C1: a reference is
 * R and C with the offset from this cell in brackets when it is
 * relative, and the number itself when it is absolute. */

static void r1c1_write (const O42Node *node, GString *out, int row, int col);

static const char *
sylk_op_text (O42Op op)
{
  switch (op)
    {
    case O42_OP_ADD:     return "+";
    case O42_OP_SUB:     return "-";
    case O42_OP_MUL:     return "*";
    case O42_OP_DIV:     return "/";
    case O42_OP_POW:     return "^";
    case O42_OP_CONCAT:  return "&";
    case O42_OP_EQ:      return "=";
    case O42_OP_NE:      return "<>";
    case O42_OP_LT:      return "<";
    case O42_OP_GT:      return ">";
    case O42_OP_LE:      return "<=";
    case O42_OP_GE:      return ">=";
    case O42_OP_NEG:     return "-";
    case O42_OP_POS:     return "+";
    default:             return "";
    }
}

static void
r1c1_ref (GString *out, int row, int col, gboolean row_abs, gboolean col_abs,
          int from_row, int from_col)
{
  if (row_abs)
    g_string_append_printf (out, "R%d", row + 1);
  else if (row == from_row)
    g_string_append (out, "R");
  else
    g_string_append_printf (out, "R[%d]", row - from_row);

  if (col_abs)
    g_string_append_printf (out, "C%d", col + 1);
  else if (col == from_col)
    g_string_append (out, "C");
  else
    g_string_append_printf (out, "C[%d]", col - from_col);
}

static void
r1c1_write (const O42Node *node, GString *out, int row, int col)
{
  if (node == NULL)
    return;
  switch (node->type)
    {
    case O42_NODE_NUMBER:
      append_number (out, node->as.number);
      break;

    case O42_NODE_STRING:
      {
        char **parts = g_strsplit (node->as.string, "\"", -1);
        char *escaped = g_strjoinv ("\"\"", parts);

        g_string_append_printf (out, "\"%s\"", escaped);
        g_free (escaped);
        g_strfreev (parts);
      }
      break;

    case O42_NODE_BOOL:
      g_string_append (out, node->as.boolean ? "TRUE" : "FALSE");
      break;

    case O42_NODE_ERROR:
      g_string_append (out, o42_error_name (node->as.error));
      break;

    case O42_NODE_REF:
      r1c1_ref (out, node->as.ref.row, node->as.ref.col,
                (node->abs & O42_ABS_ROW0) != 0, (node->abs & O42_ABS_COL0) != 0,
                row, col);
      break;

    case O42_NODE_RANGE:
      r1c1_ref (out, node->as.range.row0, node->as.range.col0,
                (node->abs & O42_ABS_ROW0) != 0, (node->abs & O42_ABS_COL0) != 0,
                row, col);
      g_string_append_c (out, ':');
      r1c1_ref (out, node->as.range.row1, node->as.range.col1,
                (node->abs & O42_ABS_ROW1) != 0, (node->abs & O42_ABS_COL1) != 0,
                row, col);
      break;

    case O42_NODE_UNARY:
      if (node->as.op.op == O42_OP_PERCENT)
        {
          r1c1_write (node->as.op.a, out, row, col);
          g_string_append_c (out, '%');
        }
      else
        {
          g_string_append (out, sylk_op_text (node->as.op.op));
          g_string_append_c (out, '(');
          r1c1_write (node->as.op.a, out, row, col);
          g_string_append_c (out, ')');
        }
      break;

    case O42_NODE_BINARY:
      /* Every operand in brackets: SYLK readers differ about
       * precedence, and brackets mean the same thing to all of them. */
      g_string_append_c (out, '(');
      r1c1_write (node->as.op.a, out, row, col);
      g_string_append (out, sylk_op_text (node->as.op.op));
      r1c1_write (node->as.op.b, out, row, col);
      g_string_append_c (out, ')');
      break;

    case O42_NODE_CALL:
      g_string_append (out, node->as.call.name);
      g_string_append_c (out, '(');
      if (node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          {
            if (i > 0)
              g_string_append_c (out, ',');
            r1c1_write (g_ptr_array_index (node->as.call.args, i), out, row, col);
          }
      g_string_append_c (out, ')');
      break;

    case O42_NODE_NAME:
      g_string_append (out, node->as.name);
      break;

    default:
      break;
    }
}

/* A field of a SYLK record: a semicolon inside one is doubled. */
static void
sylk_field (GString *out, const char *text)
{
  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p == ';')
        g_string_append_c (out, ';');
      g_string_append_c (out, *p);
    }
}

gboolean
o42_sylk_save (O42Sheet *sheet, GFile *file, GError **error)
{
  O42Range used;
  GString *out = g_string_new (NULL);

  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  o42_sheet_used_range (sheet, &used);
  g_string_append (out, "ID;PWXL;N;E\r\n");
  g_string_append_printf (out, "B;Y%d;X%d;D0 0 %d %d\r\n",
                          used.row1 + 1, used.col1 + 1, used.row1, used.col1);

  for (int row = 0; row <= used.row1; row++)
    for (int col = 0; col <= used.col1; col++)
      {
        char *input = o42_sheet_get_input (sheet, row, col);
        O42Value value;

        if (input == NULL || *input == '\0')
          { g_free (input); continue; }

        o42_sheet_get_value (sheet, row, col, &value);
        g_string_append_printf (out, "C;Y%d;X%d;K", row + 1, col + 1);
        if (value.type == O42_VALUE_NUMBER)
          append_number (out, value.as.number);
        else if (value.type == O42_VALUE_BOOL)
          g_string_append (out, value.as.boolean ? "TRUE" : "FALSE");
        else
          {
            char *shown = o42_sheet_get_display (sheet, row, col);

            g_string_append_c (out, '"');
            sylk_field (out, shown);
            g_string_append_c (out, '"');
            g_free (shown);
          }

        if (input[0] == '=')
          {
            O42Node *node = o42_formula_parse (input + 1);
            GString *expr = g_string_new (NULL);

            r1c1_write (node, expr, row, col);
            if (expr->len > 0)
              {
                g_string_append (out, ";E");
                sylk_field (out, expr->str);
              }
            g_string_free (expr, TRUE);
            o42_node_free (node);
          }
        g_string_append (out, "\r\n");
        o42_value_clear (&value);
        g_free (input);
      }

  /* The column widths, in characters, as SYLK counts them. */
  for (int col = 0; col <= used.col1; col++)
    {
      int width = o42_sheet_col_width (sheet, col);

      if (width != o42_sheet_col_width (sheet, O42_MAX_COLS - 1))
        g_string_append_printf (out, "F;W%d %d %d\r\n", col + 1, col + 1,
                                MAX (width / 7, 1));
    }

  g_string_append (out, "E\r\n");
  return write_text (file, out, error);
}

/* R1C1 as this cell sees it, turned into the A1 the parser reads.
 * Everything that is not a reference is copied over as it stands. */
static char *
r1c1_to_a1 (const char *expr, int row, int col)
{
  GString *out = g_string_new (NULL);
  const char *p = expr;

  while (*p != '\0')
    {
      if (*p == '"')
        {
          g_string_append_c (out, *p++);
          while (*p != '\0' && *p != '"')
            g_string_append_c (out, *p++);
          if (*p == '"')
            g_string_append_c (out, *p++);
          continue;
        }

      if ((*p == 'R' || *p == 'r') && (p[1] == 'C' || p[1] == 'c' || p[1] == '[' ||
                                       g_ascii_isdigit (p[1])))
        {
          /* R, R5, R[-1], each followed by the same for C. */
          const char *q = p + 1;
          int r = row, c = col;
          gboolean row_abs = FALSE, col_abs = FALSE, good = TRUE;

          if (*q == '[')
            { r = row + atoi (q + 1); q = strchr (q, ']'); good = q != NULL; if (good) q++; }
          else if (g_ascii_isdigit (*q))
            { r = atoi (q) - 1; row_abs = TRUE; while (g_ascii_isdigit (*q)) q++; }

          if (good && (*q == 'C' || *q == 'c'))
            {
              q++;
              if (*q == '[')
                { c = col + atoi (q + 1); q = strchr (q, ']'); good = q != NULL; if (good) q++; }
              else if (g_ascii_isdigit (*q))
                { c = atoi (q) - 1; col_abs = TRUE; while (g_ascii_isdigit (*q)) q++; }
            }
          else
            good = FALSE;

          if (good && r >= 0 && c >= 0 && r < O42_MAX_ROWS && c < O42_MAX_COLS)
            {
              char *name = o42_ref_name_full (r, c, row_abs, col_abs);

              g_string_append (out, name);
              g_free (name);
              p = q;
              continue;
            }
        }

      g_string_append_c (out, *p++);
    }
  return g_string_free (out, FALSE);
}

gboolean
o42_sylk_load (O42Sheet *sheet, GFile *file, GError **error)
{
  char *contents = NULL;
  gsize length = 0;
  char **lines;

  g_return_val_if_fail (sheet != NULL, FALSE);
  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return FALSE;

  lines = g_strsplit_set (contents, "\r\n", -1);
  o42_sheet_begin_group (sheet);
  {
    O42Range all = { 0, 0, O42_MAX_ROWS - 1, O42_MAX_COLS - 1 };

    o42_sheet_clear_range (sheet, &all);
  }

  {
    int row = 0, col = 0;   /* a record without Y or X means the last one */

    for (int i = 0; lines[i] != NULL; i++)
      {
        const char *line = lines[i];
        char *value = NULL, *expr = NULL;

        if (line[0] != 'C' || line[1] != ';')
          continue;

        /* Fields are semicolon-separated, and a doubled semicolon is
         * one inside a field. */
        {
          GString *field = g_string_new (NULL);
          const char *p = line + 2;

          while (TRUE)
            {
              if (*p == ';' && p[1] == ';')
                { g_string_append_c (field, ';'); p += 2; continue; }
              if (*p == ';' || *p == '\0')
                {
                  if (field->len > 0)
                    {
                      char code = field->str[0];
                      const char *rest = field->str + 1;

                      if (code == 'Y') row = MAX (atoi (rest) - 1, 0);
                      else if (code == 'X') col = MAX (atoi (rest) - 1, 0);
                      else if (code == 'K') { g_free (value); value = g_strdup (rest); }
                      else if (code == 'E') { g_free (expr); expr = g_strdup (rest); }
                    }
                  g_string_truncate (field, 0);
                  if (*p == '\0')
                    break;
                  p++;
                  continue;
                }
              g_string_append_c (field, *p++);
            }
          g_string_free (field, TRUE);
        }

        if (row < O42_MAX_ROWS && col < O42_MAX_COLS)
          {
            if (expr != NULL)
              {
                char *a1 = r1c1_to_a1 (expr, row, col);
                char *input = g_strconcat ("=", a1, NULL);

                o42_sheet_set_input (sheet, row, col, input);
                g_free (input);
                g_free (a1);
              }
            else if (value != NULL)
              {
                gsize n = strlen (value);

                if (n >= 2 && value[0] == '"' && value[n - 1] == '"')
                  {
                    char *text = g_strndup (value + 1, n - 2);

                    o42_sheet_set_input (sheet, row, col, text);
                    g_free (text);
                  }
                else
                  o42_sheet_set_input (sheet, row, col, value);
              }
          }
        g_free (value);
        g_free (expr);
      }
  }

  o42_sheet_end_group (sheet);
  g_strfreev (lines);
  g_free (contents);
  return TRUE;
}

/* ====================================================================== */
/* LaTeX                                                                  */
/* ====================================================================== */

static void
latex_escape (GString *out, const char *text)
{
  for (const char *p = text; *p != '\0'; p++)
    switch (*p)
      {
      case '&': case '%': case '$': case '#': case '_': case '{': case '}':
        g_string_append_c (out, '\\');
        g_string_append_c (out, *p);
        break;
      case '~':  g_string_append (out, "\\textasciitilde{}"); break;
      case '^':  g_string_append (out, "\\textasciicircum{}"); break;
      case '\\': g_string_append (out, "\\textbackslash{}"); break;
      default:   g_string_append_c (out, *p); break;
      }
}

gboolean
o42_latex_save (O42Sheet *sheet, GFile *file, GError **error)
{
  O42Range used;
  GString *out = g_string_new (NULL);

  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  o42_sheet_used_range (sheet, &used);
  g_string_append (out, "% Written by office42.\n");
  g_string_append (out, "\\begin{tabular}{|");
  for (int col = 0; col <= used.col1; col++)
    {
      /* A column of numbers is set to the right, as a table of numbers
       * should be; anything else to the left. */
      gboolean numbers = FALSE;

      for (int row = 0; row <= used.row1 && !numbers; row++)
        {
          O42Value value;

          o42_sheet_get_value (sheet, row, col, &value);
          numbers = value.type == O42_VALUE_NUMBER;
          o42_value_clear (&value);
        }
      g_string_append (out, numbers ? "r|" : "l|");
    }
  g_string_append (out, "}\n\\hline\n");

  for (int row = 0; row <= used.row1; row++)
    {
      for (int col = 0; col <= used.col1; col++)
        {
          char *shown = o42_sheet_get_display (sheet, row, col);
          const O42Fmt *fmt = o42_sheet_get_fmt (sheet, row, col);

          if (col > 0)
            g_string_append (out, " & ");
          if (fmt != NULL && fmt->bold)
            g_string_append (out, "\\textbf{");
          if (fmt != NULL && fmt->italic)
            g_string_append (out, "\\emph{");
          latex_escape (out, shown);
          if (fmt != NULL && fmt->italic)
            g_string_append_c (out, '}');
          if (fmt != NULL && fmt->bold)
            g_string_append_c (out, '}');
          g_free (shown);
        }
      g_string_append (out, " \\\\\n\\hline\n");
    }

  g_string_append (out, "\\end{tabular}\n");
  return write_text (file, out, error);
}
