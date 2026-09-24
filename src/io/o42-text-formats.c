/* o42-text-formats.c - see o42-text-formats.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-text-formats.h"
#include "o42-file.h"
#include "o42-csv.h"

#include "o42-book.h"
#include "o42-entry.h"
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
  gboolean ok = o42_file_replace (file, out->str, out->len, error);

  g_string_free (out, TRUE);
  return ok;
}

/* DIF and SYLK are older than UTF-8, and Excel, LibreOffice and
 * Gnumeric all read them in Windows' Western code page.  A file whose
 * every character that code page holds is written in it; one with
 * anything more is left in UTF-8, which is how this program's own
 * reader, trying UTF-8 first, gets all of it back. */
static gboolean
write_legacy_text (GFile *file, GString *out, GError **error)
{
  gsize written = 0;
  char *ansi = g_convert (out->str, (gssize) out->len, "WINDOWS-1252", "UTF-8",
                          NULL, &written, NULL);

  if (ansi != NULL)
    {
      g_string_truncate (out, 0);
      g_string_append_len (out, ansi, (gssize) written);
      g_free (ansi);
    }
  return write_text (file, out, error);
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
          else if (value.type == O42_VALUE_BOOL)
            /* A truth value is a number with its word as the indicator. */
            g_string_append (out, value.as.boolean ? "0,1\r\nTRUE\r\n" : "0,0\r\nFALSE\r\n");
          else if (value.type == O42_VALUE_ERROR)
            g_string_append (out, value.as.error == O42_ERR_NA ? "0,0\r\nNA\r\n" : "0,0\r\nERROR\r\n");
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

  return write_legacy_text (file, out, error);
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
  {
    /* These formats predate UTF-8; the file is whatever its writer's
     * code page was. */
    char *utf8 = o42_text_to_utf8 (contents, (gssize) length);
    g_free (contents);
    contents = utf8;
  }

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
          if (lines[i + 1] == NULL)   /* a record cut short at the end */
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
            o42_sheet_set_input (sheet, row, col,
                                 strcmp (under, "NA") == 0 ? "#N/A"
                                 : strcmp (under, "ERROR") == 0 ? "#VALUE!" : under);
          col++;
          if (lines[i + 1] == NULL)   /* a record cut short at the end */
            break;
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
          /* A string is a text, even one that looks like a number. */
          {
            char *quoted = o42_entry_quote_text (text);

            o42_sheet_set_input (sheet, row, col, quoted);
            g_free (quoted);
          }
          g_free (text);
          col++;
          if (lines[i + 1] == NULL)   /* a record cut short at the end */
            break;
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
    case O42_OP_UNION:   return ",";
    case O42_OP_ISECT:   return " ";
    case O42_OP_RANGE:   return ":";
    default:             return "";
    }
}

static void
r1c1_part (GString *out, char axis, int at, gboolean absolute, int from)
{
  if (absolute)
    g_string_append_printf (out, "%c%d", axis, at + 1);
  else if (at == from)
    g_string_append_c (out, axis);
  else
    g_string_append_printf (out, "%c[%d]", axis, at - from);
}

static void
r1c1_ref (GString *out, int row, int col, gboolean row_abs, gboolean col_abs,
          int from_row, int from_col)
{
  r1c1_part (out, 'R', row, row_abs, from_row);
  r1c1_part (out, 'C', col, col_abs, from_col);
}

/* A reference to another sheet keeps its sheet: without it, it would
 * name the same cell of this one. */
static void
r1c1_sheet (const O42Node *node, GString *out)
{
  char *prefix = o42_node_sheet_prefix (node);

  g_string_append (out, prefix);
  g_free (prefix);
}

/* A line break inside a text, as Excel and LibreOffice write it in
 * SYLK, where a record is one line. */
#define SYLK_LF "\033 :"

static void
sylk_text (GString *out, const char *text)
{
  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p == '\n')
        g_string_append (out, SYLK_LF);
      else if (*p != '\r')
        g_string_append_c (out, *p);
    }
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

        g_string_append_c (out, '"');
        sylk_text (out, escaped);
        g_string_append_c (out, '"');
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
      r1c1_sheet (node, out);
      r1c1_ref (out, node->as.ref.row, node->as.ref.col,
                (node->abs & O42_ABS_ROW0) != 0, (node->abs & O42_ABS_COL0) != 0,
                row, col);
      break;

    case O42_NODE_RANGE:
      r1c1_sheet (node, out);
      /* A:A is C1 and 1:1 is R1, as Excel spells whole columns and
       * rows; the row numbers of a whole column are this program's
       * last row, which no other program has. */
      if (node->abs & O42_WHOLE_COLS)
        {
          r1c1_part (out, 'C', node->as.range.col0, (node->abs & O42_ABS_COL0) != 0, col);
          g_string_append_c (out, ':');
          r1c1_part (out, 'C', node->as.range.col1, (node->abs & O42_ABS_COL1) != 0, col);
          break;
        }
      if (node->abs & O42_WHOLE_ROWS)
        {
          r1c1_part (out, 'R', node->as.range.row0, (node->abs & O42_ABS_ROW0) != 0, row);
          g_string_append_c (out, ':');
          r1c1_part (out, 'R', node->as.range.row1, (node->abs & O42_ABS_ROW1) != 0, row);
          break;
        }
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

    case O42_NODE_ARRAY:
      g_string_append_c (out, '{');
      for (guint i = 0; i < node->as.array.items->len; i++)
        {
          if (i > 0)
            g_string_append_c (out, i % (guint) MAX (node->as.array.cols, 1) == 0 ? ';' : ',');
          r1c1_write (g_ptr_array_index (node->as.array.items, i), out, row, col);
        }
      g_string_append_c (out, '}');
      break;

    case O42_NODE_APPLY:
      r1c1_write (node->as.apply.callee, out, row, col);
      g_string_append_c (out, '(');
      for (guint i = 0; node->as.apply.args != NULL && i < node->as.apply.args->len; i++)
        {
          if (i > 0)
            g_string_append_c (out, ',');
          r1c1_write (g_ptr_array_index (node->as.apply.args, i), out, row, col);
        }
      g_string_append_c (out, ')');
      break;

    case O42_NODE_EMPTY:
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

  /* The names the formulas use, as NN records: those of ranges on this
   * sheet and those that stand for a formula.  A name on another sheet
   * has nowhere to point in a one-sheet file. */
  if (o42_sheet_get_book (sheet) != NULL)
    {
      O42Book *book = o42_sheet_get_book (sheet);
      GList *names = o42_book_names (book);

      for (GList *l = names; l != NULL; l = l->next)
        {
          const char *name = l->data;
          const char *formula = o42_book_lookup_name_formula (book, name);
          O42Sheet *on = NULL;
          O42Range range;
          GString *expr = g_string_new (NULL);

          if (formula != NULL)
            {
              O42Node *node = o42_formula_parse (formula);

              r1c1_write (node, expr, 0, 0);
              o42_node_free (node);
            }
          else if (o42_book_lookup_name (book, name, &on, &range) && on == sheet)
            {
              r1c1_ref (expr, range.row0, range.col0, TRUE, TRUE, 0, 0);
              if (range.row1 != range.row0 || range.col1 != range.col0)
                {
                  g_string_append_c (expr, ':');
                  r1c1_ref (expr, range.row1, range.col1, TRUE, TRUE, 0, 0);
                }
            }
          if (expr->len > 0)
            {
              g_string_append (out, "NN;N");
              sylk_field (out, name);
              g_string_append (out, ";E");
              sylk_field (out, expr->str);
              g_string_append (out, "\r\n");
            }
          g_string_free (expr, TRUE);
        }
      g_list_free (names);
    }

  for (int row = 0; row <= used.row1; row++)
    for (int col = 0; col <= used.col1; col++)
      {
        char *input;
        O42Value value;
        O42Range block;

        /* What a formula spilled is worked out again from it; written
         * as a constant it would stand in the spill's way. */
        if (o42_sheet_array_range (sheet, row, col, &block) &&
            o42_sheet_array_is_dynamic (sheet, row, col) &&
            (row != block.row0 || col != block.col0))
          continue;
        input = o42_sheet_get_input (sheet, row, col);
        if (input == NULL || *input == '\0')
          { g_free (input); continue; }

        o42_sheet_get_value (sheet, row, col, &value);
        g_string_append_printf (out, "C;Y%d;X%d;K", row + 1, col + 1);
        if (value.type == O42_VALUE_NUMBER)
          append_number (out, value.as.number);
        else if (value.type == O42_VALUE_BOOL)
          g_string_append (out, value.as.boolean ? "TRUE" : "FALSE");
        else if (value.type == O42_VALUE_ERROR)
          /* An error is written bare; in quotes it is a text. */
          g_string_append (out, o42_error_name (value.as.error));
        else
          {
            char *shown = o42_sheet_get_display (sheet, row, col);
            GString *text = g_string_new (NULL);

            sylk_text (text, shown);
            g_string_append_c (out, '"');
            sylk_field (out, text->str);
            g_string_append_c (out, '"');
            g_string_free (text, TRUE);
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
  return write_legacy_text (file, out, error);
}

/* One axis of an R1C1 reference -- R, R5 or R[-1], and the same for
 * C -- as this cell sees it. */
static gboolean
r1c1_axis (const char **pp, char axis, int from, int *at, gboolean *absolute)
{
  const char *q = *pp;
  char *end;

  if (g_ascii_toupper (*q) != axis)
    return FALSE;
  q++;
  if (*q == '[')
    {
      long n = strtol (q + 1, &end, 10);

      if (*end != ']')
        return FALSE;
      *at = (int) CLAMP ((long) from + n, -1L, (long) G_MAXINT);
      *absolute = FALSE;
      q = end + 1;
    }
  else if (g_ascii_isdigit (*q))
    {
      long n = strtol (q, &end, 10);

      *at = (int) CLAMP (n - 1, -1L, (long) G_MAXINT);
      *absolute = TRUE;
      q = end;
    }
  else
    {
      *at = from;
      *absolute = FALSE;
    }
  *pp = q;
  return TRUE;
}

static gboolean
is_name_char (char c)
{
  return g_ascii_isalnum (c) || c == '_' || c == '.' || (guchar) c >= 0x80;
}

static void
append_row (GString *out, int row, gboolean absolute)
{
  g_string_append_printf (out, "%s%d", absolute ? "$" : "", row + 1);
}

static void
append_col (GString *out, int col, gboolean absolute)
{
  char name[8];

  o42_col_name (col, name, sizeof name);
  g_string_append_printf (out, "%s%s", absolute ? "$" : "", name);
}

/* R1C1 as this cell sees it, turned into the A1 the parser reads:
 * cells, and whole rows (R1:R3) and columns (C2:C2).  Everything that
 * is not a reference is copied over as it stands -- strings, quoted
 * sheet names, and names and functions with an R or a C in them. */
static char *
r1c1_to_a1 (const char *expr, int row, int col)
{
  GString *out = g_string_new (NULL);
  const char *p = expr;

  while (*p != '\0')
    {
      if (*p == '"' || *p == '\'')
        {
          char quote = *p;

          g_string_append_c (out, *p++);
          while (*p != '\0' && *p != quote)
            g_string_append_c (out, *p++);
          if (*p == quote)
            g_string_append_c (out, *p++);
          continue;
        }

      if ((p == expr || !is_name_char (p[-1])) &&
          (g_ascii_toupper (*p) == 'R' || g_ascii_toupper (*p) == 'C'))
        {
          const char *q = p;
          int r = row, c = col, r2 = row, c2 = col;
          gboolean ra = FALSE, ca = FALSE, ra2 = FALSE, ca2 = FALSE;
          gboolean has_r = r1c1_axis (&q, 'R', row, &r, &ra);
          gboolean has_c = r1c1_axis (&q, 'C', col, &c, &ca);

          if (has_r && has_c && !is_name_char (*q) && *q != '(' &&
              r >= 0 && c >= 0 && r < O42_MAX_ROWS && c < O42_MAX_COLS)
            {
              char *name = o42_ref_name_full (r, c, ra, ca);

              g_string_append (out, name);
              g_free (name);
              p = q;
              continue;
            }
          if (has_r != has_c && *q == ':')
            {
              /* A whole row or column needs its other end to be one. */
              const char *q2 = q + 1;
              gboolean ok = has_r ? r1c1_axis (&q2, 'R', row, &r2, &ra2) && !r1c1_axis (&q2, 'C', col, &c2, &ca2)
                                  : r1c1_axis (&q2, 'C', col, &c2, &ca2);

              if (ok && !is_name_char (*q2) && *q2 != '(')
                {
                  if (has_r && r >= 0 && r2 >= 0 && r < O42_MAX_ROWS && r2 < O42_MAX_ROWS)
                    {
                      append_row (out, r, ra);
                      g_string_append_c (out, ':');
                      append_row (out, r2, ra2);
                      p = q2;
                      continue;
                    }
                  if (has_c && c >= 0 && c2 >= 0 && c < O42_MAX_COLS && c2 < O42_MAX_COLS)
                    {
                      append_col (out, c, ca);
                      g_string_append_c (out, ':');
                      append_col (out, c2, ca2);
                      p = q2;
                      continue;
                    }
                }
            }
          /* Not a reference: the letter and the name it begins. */
          while (is_name_char (*p))
            g_string_append_c (out, *p++);
          continue;
        }

      g_string_append_c (out, *p++);
    }
  return g_string_free (out, FALSE);
}

/* A field as SYLK writes it, with its line breaks put back. */
static char *
sylk_untext (const char *field)
{
  GString *out = g_string_new (NULL);

  for (const char *p = field; *p != '\0'; p++)
    {
      if (p[0] == '\033' && p[1] == ' ' && p[2] == ':')
        {
          g_string_append_c (out, '\n');
          p += 2;
        }
      else
        g_string_append_c (out, *p);
    }
  return g_string_free (out, FALSE);
}

/* The fields of a record after its type, with a doubled semicolon
 * standing for one inside a field. */
static char **
sylk_fields (const char *line)
{
  GPtrArray *fields = g_ptr_array_new ();
  GString *field = g_string_new (NULL);
  const char *p = line;

  while (TRUE)
    {
      if (*p == ';' && p[1] == ';')
        {
          g_string_append_c (field, ';');
          p += 2;
          continue;
        }
      if (*p == ';' || *p == '\0')
        {
          if (field->len > 0)
            g_ptr_array_add (fields, sylk_untext (field->str));
          g_string_truncate (field, 0);
          if (*p == '\0')
            break;
          p++;
          continue;
        }
      g_string_append_c (field, *p++);
    }
  g_string_free (field, TRUE);
  g_ptr_array_add (fields, NULL);
  return (char **) g_ptr_array_free (fields, FALSE);
}

/* NN;Nname;Eexpression: a name, which is a range of this sheet when
 * the expression is one and a formula when it is not. */
static void
sylk_name (O42Sheet *sheet, const char *line)
{
  O42Book *book = o42_sheet_get_book (sheet);
  char **fields = sylk_fields (line);
  const char *name = NULL, *expr = NULL;

  for (int i = 0; fields[i] != NULL; i++)
    {
      if (fields[i][0] == 'N' && name == NULL)
        name = fields[i] + 1;
      else if (fields[i][0] == 'E' && expr == NULL)
        expr = fields[i] + 1;
    }
  if (book != NULL && name != NULL && expr != NULL)
    {
      char *a1 = r1c1_to_a1 (expr, 0, 0);
      O42Node *node = o42_formula_parse (a1);

      if (node != NULL && node->sheet == NULL && node->type == O42_NODE_REF)
        {
          O42Range cell = { node->as.ref.row, node->as.ref.col, node->as.ref.row, node->as.ref.col };
          o42_book_define_name (book, name, sheet, &cell);
        }
      else if (node != NULL && node->sheet == NULL && node->type == O42_NODE_RANGE &&
               (node->abs & (O42_WHOLE_ROWS | O42_WHOLE_COLS)) == 0)
        o42_book_define_name (book, name, sheet, &node->as.range);
      else
        o42_book_define_name_formula (book, name, a1);
      o42_node_free (node);
      g_free (a1);
    }
  g_strfreev (fields);
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
  {
    /* These formats predate UTF-8; the file is whatever its writer's
     * code page was. */
    char *utf8 = o42_text_to_utf8 (contents, (gssize) length);
    g_free (contents);
    contents = utf8;
  }

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
        char **fields;

        if (line[0] == 'N' && line[1] == 'N' && line[2] == ';')
          {
            sylk_name (sheet, line + 3);
            continue;
          }
        if (line[0] != 'C' || line[1] != ';')
          continue;

        fields = sylk_fields (line + 2);
        for (int f = 0; fields[f] != NULL; f++)
          {
            char code = fields[f][0];
            const char *rest = fields[f] + 1;

            if (code == 'Y') row = CLAMP (atoi (rest) - 1, 0, O42_MAX_ROWS);
            else if (code == 'X') col = CLAMP (atoi (rest) - 1, 0, O42_MAX_COLS);
            else if (code == 'K') { g_free (value); value = g_strdup (rest); }
            else if (code == 'E') { g_free (expr); expr = g_strdup (rest); }
          }
        g_strfreev (fields);

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
                    /* In quotes it is a text, whatever it looks like. */
                    char *text = g_strndup (value + 1, n - 2);
                    char *quoted = o42_entry_quote_text (text);

                    o42_sheet_set_input (sheet, row, col, quoted);
                    g_free (quoted);
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
