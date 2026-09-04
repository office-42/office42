/* o42-formula.c - see o42-formula.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-formula.h"
#include "o42-numfmt.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Tokens                                                                  */
/* ---------------------------------------------------------------------- */

typedef enum {
  TOK_END,
  TOK_NUMBER,
  TOK_STRING,
  TOK_IDENT,      /* a function name, a reference, TRUE/FALSE */
  TOK_ERROR,      /* an error literal such as #N/A */
  TOK_OP,
  TOK_LPAREN,
  TOK_RPAREN,
  TOK_COMMA,
  TOK_SEMI,                  /* the row separator in an array constant */
  TOK_LBRACE,
  TOK_RBRACE,
  TOK_COLON
} TokenType;

typedef struct {
  TokenType     type;
  double        number;
  char         *text;        /* owned by the lexer, freed on the next token */
  char         *sheet;       /* owned: the part before "!" of Sheet1!A1 */
  char         *sheet_last;  /* owned: the second sheet of Sheet1:Sheet3!A1 */
  O42ErrorCode  error;
  char          op[3];
} Token;

typedef struct {
  const char *input;
  const char *p;
  Token       tok;
} Parser;

static void
token_clear (Token *t)
{
  g_clear_pointer (&t->text, g_free);
  g_clear_pointer (&t->sheet, g_free);
  g_clear_pointer (&t->sheet_last, g_free);
}

/* 'First:Last'!A1 keeps both sheets in the quotes; they part here.  A
 * sheet name cannot hold a colon, so the first one is the split. */
static void
split_sheet_span (Token *tok)
{
  char *colon = tok->sheet != NULL ? strchr (tok->sheet, ':') : NULL;
  if (colon != NULL && colon != tok->sheet && colon[1] != '\0')
    {
      tok->sheet_last = g_strdup (colon + 1);
      *colon = '\0';
    }
}

static const struct {
  const char   *name;
  O42ErrorCode  code;
} ERROR_LITERALS[] = {
  { "#NULL!",   O42_ERR_NULL  },
  { "#DIV/0!",  O42_ERR_DIV0  },
  { "#VALUE!",  O42_ERR_VALUE },
  { "#REF!",    O42_ERR_REF   },
  { "#NAME?",   O42_ERR_NAME  },
  { "#NUM!",    O42_ERR_NUM   },
  { "#N/A",     O42_ERR_NA    },
};

static void
next_token (Parser *ps)
{
  const char *p = ps->p;

  token_clear (&ps->tok);
  memset (&ps->tok, 0, sizeof ps->tok);

  while (g_ascii_isspace (*p))
    p++;

  if (*p == '\0')
    {
      ps->tok.type = TOK_END;
      ps->p = p;
      return;
    }

  /* A number.  A leading sign belongs to the unary operator, not here. */
  if (g_ascii_isdigit (*p) || (*p == '.' && g_ascii_isdigit (p[1])))
    {
      char *end = NULL;

      ps->tok.type = TOK_NUMBER;
      ps->tok.number = g_ascii_strtod (p, &end);
      ps->p = (end != NULL) ? end : p + 1;
      return;
    }

  /* A string.  Two quotes in a row are one quote, which is how a spreadsheet
   * has always escaped them. */
  if (*p == '"')
    {
      GString *out = g_string_new (NULL);

      p++;
      while (*p != '\0')
        {
          if (*p == '"')
            {
              if (p[1] == '"')
                {
                  g_string_append_c (out, '"');
                  p += 2;
                  continue;
                }
              p++;
              break;
            }
          g_string_append_c (out, *p);
          p++;
        }

      ps->tok.type = TOK_STRING;
      ps->tok.text = g_string_free (out, FALSE);
      ps->p = p;
      return;
    }

  /* An error literal. */
  if (*p == '#')
    {
      for (guint i = 0; i < G_N_ELEMENTS (ERROR_LITERALS); i++)
        {
          gsize n = strlen (ERROR_LITERALS[i].name);

          if (g_ascii_strncasecmp (p, ERROR_LITERALS[i].name, n) == 0)
            {
              ps->tok.type = TOK_ERROR;
              ps->tok.error = ERROR_LITERALS[i].code;
              ps->p = p + n;
              return;
            }
        }

      ps->tok.type = TOK_ERROR;
      ps->tok.error = O42_ERR_NAME;
      ps->p = p + 1;
      return;
    }

  /* A quoted sheet name: 'Sales 1993'!A1.  Two quotes inside are one. */
  if (*p == '\'')
    {
      GString *name = g_string_new (NULL);

      p++;
      while (*p != '\0')
        {
          if (*p == '\'')
            {
              if (p[1] == '\'') { g_string_append_c (name, '\''); p += 2; continue; }
              p++;
              break;
            }
          g_string_append_c (name, *p++);
        }

      if (*p == '!')
        {
          const char *start = ++p;

          while (g_ascii_isalnum (*p) || *p == '_' || *p == '.' || *p == '$')
            p++;

          ps->tok.type = TOK_IDENT;
          ps->tok.sheet = g_string_free (name, FALSE);
          split_sheet_span (&ps->tok);
          ps->tok.text = g_strndup (start, (gsize) (p - start));
          ps->p = p;
          return;
        }

      /* A quote with no "!" after it is not anything. */
      g_string_free (name, TRUE);
      ps->tok.type = TOK_ERROR;
      ps->tok.error = O42_ERR_NAME;
      ps->p = p;
      return;
    }

  /* An identifier: a function name, a cell reference, or a word we will not
   * recognise.  Dollar signs are part of it so that $A$1 lexes as one
   * thing, and "!" splits Sheet1!A1 into a sheet and a reference. */
  if (g_ascii_isalpha (*p) || *p == '_' || *p == '$')
    {
      const char *start = p;

      while (g_ascii_isalnum (*p) || *p == '_' || *p == '.' || *p == '$')
        p++;

      /* Table1[Sales], Table1[#Data], Table1[@Sales]: the brackets are
       * part of the name, and the sheet resolves it to a range. */
      if (*p == '[')
        {
          int depth = 0;

          do
            {
              if (*p == '[') depth++;
              else if (*p == ']') depth--;
              p++;
            }
          while (*p != '\0' && depth > 0);
          ps->tok.type = TOK_IDENT;
          ps->tok.text = g_strndup (start, (gsize) (p - start));
          ps->p = p;
          return;
        }

      if (*p == '!')
        {
          const char *ref = ++p;

          ps->tok.sheet = g_strndup (start, (gsize) (p - 1 - start));
          while (g_ascii_isalnum (*p) || *p == '_' || *p == '.' || *p == '$')
            p++;
          ps->tok.type = TOK_IDENT;
          ps->tok.text = g_strndup (ref, (gsize) (p - ref));
          ps->p = p;
          return;
        }

      /* Sheet1:'Sheet 3'!A1: the second name quoted. */
      if (*p == ':' && p[1] == '\'')
        {
          GString *second = g_string_new (NULL);
          const char *q = p + 2;
          while (*q != '\0')
            {
              if (*q == '\'')
                {
                  if (q[1] == '\'') { g_string_append_c (second, '\''); q += 2; continue; }
                  q++;
                  break;
                }
              g_string_append_c (second, *q++);
            }
          if (*q == '!')
            {
              const char *ref = q + 1;
              ps->tok.sheet = g_strndup (start, (gsize) (p - start));
              ps->tok.sheet_last = g_string_free (second, FALSE);
              p = ref;
              while (g_ascii_isalnum (*p) || *p == '_' || *p == '.' || *p == '$')
                p++;
              ps->tok.type = TOK_IDENT;
              ps->tok.text = g_strndup (ref, (gsize) (p - ref));
              ps->p = p;
              return;
            }
          g_string_free (second, TRUE);
        }

      /* Sheet1:Sheet3!A1: a second name after a colon, then "!". */
      if (*p == ':' && (g_ascii_isalpha (p[1]) || p[1] == '_'))
        {
          const char *second = p + 1;
          const char *q = second;

          while (g_ascii_isalnum (*q) || *q == '_' || *q == '.')
            q++;
          if (*q == '!')
            {
              const char *ref = q + 1;

              ps->tok.sheet = g_strndup (start, (gsize) (p - start));
              ps->tok.sheet_last = g_strndup (second, (gsize) (q - second));
              p = ref;
              while (g_ascii_isalnum (*p) || *p == '_' || *p == '.' || *p == '$')
                p++;
              ps->tok.type = TOK_IDENT;
              ps->tok.text = g_strndup (ref, (gsize) (p - ref));
              ps->p = p;
              return;
            }
        }

      ps->tok.type = TOK_IDENT;
      ps->tok.text = g_strndup (start, (gsize) (p - start));
      ps->p = p;
      return;
    }

  switch (*p)
    {
    case '(': ps->tok.type = TOK_LPAREN; ps->p = p + 1; return;
    case ')': ps->tok.type = TOK_RPAREN; ps->p = p + 1; return;
    case ':': ps->tok.type = TOK_COLON;  ps->p = p + 1; return;
    case ',': ps->tok.type = TOK_COMMA;  ps->p = p + 1; return;
    case ';': ps->tok.type = TOK_SEMI;   ps->p = p + 1; return;   /* also SUM(1;2) in some locales */
    case '{': ps->tok.type = TOK_LBRACE; ps->p = p + 1; return;
    case '}': ps->tok.type = TOK_RBRACE; ps->p = p + 1; return;
    default:
      break;
    }

  ps->tok.type = TOK_OP;

  /* The two-character comparisons have to be tried before the one-character
   * ones, or "<=" lexes as "<" followed by "=". */
  if ((p[0] == '<' && p[1] == '=') ||
      (p[0] == '>' && p[1] == '=') ||
      (p[0] == '<' && p[1] == '>'))
    {
      ps->tok.op[0] = p[0];
      ps->tok.op[1] = p[1];
      ps->p = p + 2;
      return;
    }

  ps->tok.op[0] = *p;
  ps->p = p + 1;
}

/* ---------------------------------------------------------------------- */
/* Nodes                                                                   */
/* ---------------------------------------------------------------------- */

static O42Node *
node_new (O42NodeType type)
{
  O42Node *n = g_new0 (O42Node, 1);
  n->type = type;
  return n;
}

static O42Node *
node_error (O42ErrorCode code)
{
  O42Node *n = node_new (O42_NODE_ERROR);
  n->as.error = code;
  return n;
}

void
o42_node_free (O42Node *node)
{
  if (node == NULL)
    return;

  switch (node->type)
    {
    case O42_NODE_STRING:
      g_free (node->as.string);
      break;

    case O42_NODE_NAME:
      g_free (node->as.name);
      break;

    case O42_NODE_UNARY:
    case O42_NODE_BINARY:
      o42_node_free (node->as.op.a);
      o42_node_free (node->as.op.b);
      break;

    case O42_NODE_CALL:
      g_free (node->as.call.name);
      if (node->as.call.args != NULL)
        g_ptr_array_free (node->as.call.args, TRUE);
      break;

    case O42_NODE_ARRAY:
      if (node->as.array.items != NULL)
        g_ptr_array_free (node->as.array.items, TRUE);
      break;

    case O42_NODE_APPLY:
      o42_node_free (node->as.apply.callee);
      if (node->as.apply.args != NULL)
        g_ptr_array_free (node->as.apply.args, TRUE);
      break;

    default:
      break;
    }

  g_free (node);
}

/* ---------------------------------------------------------------------- */
/* Grammar                                                                 */
/* ---------------------------------------------------------------------- */

static O42Node *parse_expr (Parser *ps);
static O42Node *parse_whole_range (Parser *ps, gboolean cols, int first, gboolean first_abs,
                                   const char *sheet, const char *sheet_last);
static gboolean parse_row_only (const char *text, int *row, gboolean *abs);
static gboolean parse_col_only (const char *text, int *col, gboolean *abs);

static gboolean
op_is (Parser *ps, const char *what)
{
  return ps->tok.type == TOK_OP && strcmp (ps->tok.op, what) == 0;
}

static O42Node *
make_binary (O42Op op, O42Node *a, O42Node *b)
{
  O42Node *n = node_new (O42_NODE_BINARY);

  n->as.op.op = op;
  n->as.op.a = a;
  n->as.op.b = b;

  return n;
}

static O42Node *
make_unary (O42Op op, O42Node *a)
{
  O42Node *n = node_new (O42_NODE_UNARY);

  n->as.op.op = op;
  n->as.op.a = a;
  n->as.op.b = NULL;

  return n;
}

/* {1,2;3,4}: items across, rows separated by semicolons.  Every row
 * must be the same length, or the whole thing is an error node. */
static O42Node *
parse_array (Parser *ps)
{
  O42Node *n = node_new (O42_NODE_ARRAY);
  int cols = -1, in_row = 0;

  n->as.array.items = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_node_free);
  n->as.array.rows = 0;
  next_token (ps);   /* past the brace */

  for (;;)
    {
      if (ps->tok.type == TOK_RBRACE || ps->tok.type == TOK_END)
        break;
      g_ptr_array_add (n->as.array.items, parse_expr (ps));
      in_row++;
      if (ps->tok.type == TOK_COMMA)
        { next_token (ps); continue; }
      if (ps->tok.type == TOK_SEMI || ps->tok.type == TOK_RBRACE || ps->tok.type == TOK_END)
        {
          if (cols < 0) cols = in_row;
          else if (cols != in_row)
            {
              o42_node_free (n);
              n = node_new (O42_NODE_ERROR);
              n->as.error = O42_ERR_VALUE;
              while (ps->tok.type != TOK_RBRACE && ps->tok.type != TOK_END)
                next_token (ps);
              if (ps->tok.type == TOK_RBRACE) next_token (ps);
              return n;
            }
          n->as.array.rows++;
          in_row = 0;
          if (ps->tok.type == TOK_SEMI) { next_token (ps); continue; }
          break;
        }
      /* Anything else is a stray token: stop here. */
      break;
    }
  if (ps->tok.type == TOK_RBRACE)
    next_token (ps);
  n->as.array.cols = MAX (cols, 0);
  if (n->as.array.rows == 0 || n->as.array.cols == 0)
    {
      o42_node_free (n);
      n = node_new (O42_NODE_ERROR);
      n->as.error = O42_ERR_VALUE;
    }
  return n;
}

static O42Node *
parse_primary (Parser *ps)
{
  if (ps->tok.type == TOK_LBRACE)
    return parse_array (ps);

  switch (ps->tok.type)
    {
    case TOK_NUMBER:
      {
        O42Node *n;
        double number = ps->tok.number;

        next_token (ps);
        /* 1:1 is the whole of row 1, not a number and a stray colon. */
        if (ps->tok.type == TOK_COLON && number >= 1 && number <= O42_MAX_ROWS &&
            number == (int) number)
          return parse_whole_range (ps, FALSE, (int) number - 1, FALSE, NULL, NULL);
        n = node_new (O42_NODE_NUMBER);
        n->as.number = number;
        return n;
      }

    case TOK_STRING:
      {
        O42Node *n = node_new (O42_NODE_STRING);
        n->as.string = g_strdup (ps->tok.text != NULL ? ps->tok.text : "");
        next_token (ps);
        return n;
      }

    case TOK_ERROR:
      {
        O42Node *n = node_error (ps->tok.error);
        next_token (ps);
        return n;
      }

    case TOK_LPAREN:
      {
        O42Node *inner;

        next_token (ps);
        inner = parse_expr (ps);

        if (ps->tok.type == TOK_RPAREN)
          next_token (ps);

        return inner;
      }

    case TOK_IDENT:
      {
        char *name = g_strdup (ps->tok.text != NULL ? ps->tok.text : "");
        const char *sheet = (ps->tok.sheet != NULL) ? g_intern_string (ps->tok.sheet) : NULL;
        const char *sheet_last = (ps->tok.sheet_last != NULL) ? g_intern_string (ps->tok.sheet_last) : NULL;
        int row = 0, col = 0;
        gboolean row_abs = FALSE, col_abs = FALSE;
        gsize used = 0;

        next_token (ps);

        /* A name followed by "(" is a call, whatever else it might have
         * been.  That is what keeps LOG10( from being read as a cell. */
        if (ps->tok.type == TOK_LPAREN)
          {
            O42Node *n = node_new (O42_NODE_CALL);

            n->as.call.name = g_ascii_strup (name, -1);
            n->as.call.args = g_ptr_array_new_with_free_func (
                                (GDestroyNotify) o42_node_free);
            g_free (name);

            next_token (ps);

            if (ps->tok.type != TOK_RPAREN)
              {
                for (;;)
                  {
                    /* An argument left out, as in PMT(r,n,pv,,1), is
                     * an empty node; the function sees an empty value. */
                    if (ps->tok.type == TOK_COMMA || ps->tok.type == TOK_SEMI ||
                        ps->tok.type == TOK_RPAREN)
                      g_ptr_array_add (n->as.call.args, node_new (O42_NODE_EMPTY));
                    else
                      g_ptr_array_add (n->as.call.args, parse_expr (ps));

                    if (ps->tok.type == TOK_COMMA || ps->tok.type == TOK_SEMI)
                      {
                        next_token (ps);
                        continue;
                      }
                    break;
                  }
              }

            if (ps->tok.type == TOK_RPAREN)
              next_token (ps);

            return n;
          }

        if (g_ascii_strcasecmp (name, "TRUE") == 0 ||
            g_ascii_strcasecmp (name, "FALSE") == 0)
          {
            O42Node *n = node_new (O42_NODE_BOOL);
            n->as.boolean = g_ascii_strcasecmp (name, "TRUE") == 0;
            g_free (name);
            return n;
          }

        /* A reference, if the whole of the name is one. */
        if (o42_ref_parse_full (name, &row, &col, &row_abs, &col_abs, &used) &&
            used == strlen (name))
          {
            if (ps->tok.type == TOK_COLON)
              {
                int row1 = 0, col1 = 0;
                gboolean row1_abs = FALSE, col1_abs = FALSE;
                gsize used1 = 0;

                next_token (ps);

                if (ps->tok.type == TOK_IDENT && ps->tok.text != NULL &&
                    o42_ref_parse_full (ps->tok.text, &row1, &col1,
                                        &row1_abs, &col1_abs, &used1) &&
                    used1 == strlen (ps->tok.text))
                  {
                    O42Node *n = node_new (O42_NODE_RANGE);

                    /* Normalising swaps corners, and the dollar signs have
                     * to travel with the coordinate they were attached to. */
                    n->as.range = o42_range_normalise (row, col, row1, col1);
                    if (row > row1) { gboolean t = row_abs; row_abs = row1_abs; row1_abs = t; }
                    if (col > col1) { gboolean t = col_abs; col_abs = col1_abs; col1_abs = t; }
                    n->abs = (row_abs ? O42_ABS_ROW0 : 0) | (col_abs ? O42_ABS_COL0 : 0)
                           | (row1_abs ? O42_ABS_ROW1 : 0) | (col1_abs ? O42_ABS_COL1 : 0);
                    /* A1:A1048576 is A:A, as Excel reads it too, and
                     * files that cannot write A:A spell it that way. */
                    if (n->as.range.row0 == 0 && n->as.range.row1 == O42_MAX_ROWS - 1)
                      n->abs |= O42_WHOLE_COLS;
                    if (n->as.range.col0 == 0 && n->as.range.col1 == O42_MAX_COLS - 1)
                      n->abs |= O42_WHOLE_ROWS;
                    n->sheet = sheet;
                    n->sheet_last = sheet_last;
                    g_free (name);
                    next_token (ps);
                    return n;
                  }

                g_free (name);
                return node_error (O42_ERR_REF);
              }

            {
              O42Node *n = node_new (O42_NODE_REF);
              n->as.ref.row = row;
              n->as.ref.col = col;
              n->abs = (row_abs ? O42_ABS_ROW0 : 0) | (col_abs ? O42_ABS_COL0 : 0);
              n->sheet = sheet;
              n->sheet_last = sheet_last;
              g_free (name);
              return n;
            }
          }

        /* A:A and $1:$1: a column or a row on its own before a colon is
         * the whole of it. */
        if (ps->tok.type == TOK_COLON)
          {
            int index = 0;
            gboolean abs = FALSE;

            if (parse_col_only (name, &index, &abs))
              {
                g_free (name);
                return parse_whole_range (ps, TRUE, index, abs, sheet, sheet_last);
              }
            if (parse_row_only (name, &index, &abs))
              {
                g_free (name);
                return parse_whole_range (ps, FALSE, index, abs, sheet, sheet_last);
              }
          }

        /* Anything else is a defined name -- or will be #NAME? when it is
         * looked up and is not one. */
        if (sheet != NULL)
          {
            g_free (name);
            return node_error (O42_ERR_REF);
          }
        {
          O42Node *n = node_new (O42_NODE_NAME);
          n->as.name = name;
          return n;
        }
      }

    default:
      next_token (ps);
      return node_error (O42_ERR_NAME);
    }
}

/* "A", "$XFD": a column on its own, as the halves of A:A are written.
 * TRUE with the column's index and whether it had a dollar. */
static gboolean
parse_col_only (const char *text, int *col, gboolean *abs)
{
  int value = 0, n = 0;

  if (text == NULL)
    return FALSE;
  *abs = (*text == '$');
  if (*abs)
    text++;
  for (; g_ascii_isalpha (*text); text++, n++)
    value = value * 26 + (g_ascii_toupper (*text) - 'A' + 1);
  if (n < 1 || n > 3 || *text != '\0' || value > O42_MAX_COLS)
    return FALSE;
  *col = value - 1;
  return TRUE;
}

/* "1", "$65536": a row on its own, as the halves of 1:1 are written. */
static gboolean
parse_row_only (const char *text, int *row, gboolean *abs)
{
  long value;
  char *end = NULL;

  if (text == NULL)
    return FALSE;
  *abs = (*text == '$');
  if (*abs)
    text++;
  if (!g_ascii_isdigit (*text))
    return FALSE;
  value = strtol (text, &end, 10);
  if (end == NULL || *end != '\0' || value < 1 || value > O42_MAX_ROWS)
    return FALSE;
  *row = (int) value - 1;
  return TRUE;
}

/* The second half of A:A or 1:1, which is the current token: a column
 * or a row on its own.  A bare row lexes as a number, so both shapes
 * are looked at. */
static gboolean
parse_whole_end (Parser *ps, gboolean cols, int *index, gboolean *abs)
{
  if (ps->tok.type == TOK_IDENT && ps->tok.sheet == NULL)
    return cols ? parse_col_only (ps->tok.text, index, abs)
                : parse_row_only (ps->tok.text, index, abs);
  if (!cols && ps->tok.type == TOK_NUMBER &&
      ps->tok.number >= 1 && ps->tok.number <= O42_MAX_ROWS &&
      ps->tok.number == (int) ps->tok.number)
    {
      *index = (int) ps->tok.number - 1;
      *abs = FALSE;
      return TRUE;
    }
  return FALSE;
}

/* A:A, $A:$C, 1:1, Sheet2!B:B: the whole of some columns or rows.  The
 * first half has been read and the colon is the current token; on
 * success the range is returned with the token after it read. */
static O42Node *
parse_whole_range (Parser *ps, gboolean cols, int first, gboolean first_abs,
                   const char *sheet, const char *sheet_last)
{
  int last = 0;
  gboolean last_abs = FALSE;
  O42Node *n;

  next_token (ps);
  if (!parse_whole_end (ps, cols, &last, &last_abs))
    return node_error (O42_ERR_REF);
  next_token (ps);

  n = node_new (O42_NODE_RANGE);
  if (first > last)
    {
      int t = first; first = last; last = t;
      gboolean b = first_abs; first_abs = last_abs; last_abs = b;
    }
  if (cols)
    {
      n->as.range.col0 = first;
      n->as.range.col1 = last;
      n->as.range.row0 = 0;
      n->as.range.row1 = O42_MAX_ROWS - 1;
      n->abs = O42_WHOLE_COLS | (first_abs ? O42_ABS_COL0 : 0) | (last_abs ? O42_ABS_COL1 : 0);
    }
  else
    {
      n->as.range.row0 = first;
      n->as.range.row1 = last;
      n->as.range.col0 = 0;
      n->as.range.col1 = O42_MAX_COLS - 1;
      n->abs = O42_WHOLE_ROWS | (first_abs ? O42_ABS_ROW0 : 0) | (last_abs ? O42_ABS_ROW1 : 0);
    }
  n->sheet = sheet;
  n->sheet_last = sheet_last;
  return n;
}

/* Trailing % divides by a hundred, and binds tighter than anything
 * else; a "(" after a call or a name calls what it came to, which is
 * how LAMBDA(x,x+1)(4) and a LET-bound function are written. */
static O42Node *
parse_postfix (Parser *ps)
{
  O42Node *n = parse_primary (ps);

  for (;;)
    {
      if (op_is (ps, "%"))
        {
          next_token (ps);
          n = make_unary (O42_OP_PERCENT, n);
          continue;
        }
      if (ps->tok.type == TOK_LPAREN &&
          (n->type == O42_NODE_CALL || n->type == O42_NODE_APPLY || n->type == O42_NODE_NAME))
        {
          O42Node *call = node_new (O42_NODE_APPLY);

          call->as.apply.callee = n;
          call->as.apply.args = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_node_free);
          next_token (ps);
          if (ps->tok.type != TOK_RPAREN)
            for (;;)
              {
                if (ps->tok.type == TOK_COMMA || ps->tok.type == TOK_RPAREN)
                  g_ptr_array_add (call->as.apply.args, node_new (O42_NODE_EMPTY));
                else
                  g_ptr_array_add (call->as.apply.args, parse_expr (ps));
                if (ps->tok.type != TOK_COMMA)
                  break;
                next_token (ps);
              }
          if (ps->tok.type != TOK_RPAREN)
            {
              o42_node_free (call);
              return node_error (O42_ERR_NAME);
            }
          next_token (ps);
          n = call;
          continue;
        }
      break;
    }

  return n;
}

/* Unary minus binds tighter than "^", so -2^2 is 4 and not -4.  That is
 * Excel's precedence, and it surprises people often enough to be worth
 * saying out loud. */
static O42Node *
parse_unary (Parser *ps)
{
  if (op_is (ps, "-"))
    {
      next_token (ps);
      return make_unary (O42_OP_NEG, parse_unary (ps));
    }

  if (op_is (ps, "+"))
    {
      next_token (ps);
      return make_unary (O42_OP_POS, parse_unary (ps));
    }

  return parse_postfix (ps);
}

static O42Node *
parse_power (Parser *ps)
{
  O42Node *a = parse_unary (ps);

  /* Left associative, as Excel has it: 2^3^2 is (2^3)^2, 64, not the
   * 512 mathematics would give. */
  while (op_is (ps, "^"))
    {
      next_token (ps);
      a = make_binary (O42_OP_POW, a, parse_unary (ps));
    }

  return a;
}

static O42Node *
parse_multiplicative (Parser *ps)
{
  O42Node *a = parse_power (ps);

  for (;;)
    {
      if (op_is (ps, "*"))
        {
          next_token (ps);
          a = make_binary (O42_OP_MUL, a, parse_power (ps));
        }
      else if (op_is (ps, "/"))
        {
          next_token (ps);
          a = make_binary (O42_OP_DIV, a, parse_power (ps));
        }
      else
        {
          return a;
        }
    }
}

static O42Node *
parse_additive (Parser *ps)
{
  O42Node *a = parse_multiplicative (ps);

  for (;;)
    {
      if (op_is (ps, "+"))
        {
          next_token (ps);
          a = make_binary (O42_OP_ADD, a, parse_multiplicative (ps));
        }
      else if (op_is (ps, "-"))
        {
          next_token (ps);
          a = make_binary (O42_OP_SUB, a, parse_multiplicative (ps));
        }
      else
        {
          return a;
        }
    }
}

static O42Node *
parse_concat (Parser *ps)
{
  O42Node *a = parse_additive (ps);

  while (op_is (ps, "&"))
    {
      next_token (ps);
      a = make_binary (O42_OP_CONCAT, a, parse_additive (ps));
    }

  return a;
}

static O42Node *
parse_expr (Parser *ps)
{
  O42Node *a = parse_concat (ps);

  for (;;)
    {
      O42Op op;

      if      (op_is (ps, "="))  op = O42_OP_EQ;
      else if (op_is (ps, "<>")) op = O42_OP_NE;
      else if (op_is (ps, "<=")) op = O42_OP_LE;
      else if (op_is (ps, ">=")) op = O42_OP_GE;
      else if (op_is (ps, "<"))  op = O42_OP_LT;
      else if (op_is (ps, ">"))  op = O42_OP_GT;
      else return a;

      next_token (ps);
      a = make_binary (op, a, parse_concat (ps));
    }
}

O42Node *
o42_formula_parse (const char *text)
{
  Parser ps;
  O42Node *node;

  if (text == NULL || *text == '\0')
    return node_error (O42_ERR_NAME);

  memset (&ps, 0, sizeof ps);
  ps.input = text;
  ps.p = text;

  next_token (&ps);
  node = parse_expr (&ps);

  token_clear (&ps.tok);
  return node;
}

/* ---------------------------------------------------------------------- */
/* Copying and moving references                                           */
/* ---------------------------------------------------------------------- */

O42Node *
o42_node_copy (const O42Node *node)
{
  O42Node *n;

  if (node == NULL)
    return NULL;

  n = g_memdup2 (node, sizeof *node);

  switch (node->type)
    {
    case O42_NODE_STRING:
      n->as.string = g_strdup (node->as.string);
      break;

    case O42_NODE_NAME:
      n->as.name = g_strdup (node->as.name);
      break;

    case O42_NODE_UNARY:
    case O42_NODE_BINARY:
      n->as.op.a = o42_node_copy (node->as.op.a);
      n->as.op.b = o42_node_copy (node->as.op.b);
      break;

    case O42_NODE_CALL:
      n->as.call.name = g_strdup (node->as.call.name);
      n->as.call.args = g_ptr_array_new_with_free_func (
                          (GDestroyNotify) o42_node_free);
      if (node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          g_ptr_array_add (n->as.call.args,
                           o42_node_copy (g_ptr_array_index (node->as.call.args, i)));
      break;

    case O42_NODE_ARRAY:
      n->as.array.items = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_node_free);
      if (node->as.array.items != NULL)
        for (guint i = 0; i < node->as.array.items->len; i++)
          g_ptr_array_add (n->as.array.items,
                           o42_node_copy (g_ptr_array_index (node->as.array.items, i)));
      break;

    case O42_NODE_APPLY:
      n->as.apply.callee = o42_node_copy (node->as.apply.callee);
      n->as.apply.args = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_node_free);
      if (node->as.apply.args != NULL)
        for (guint i = 0; i < node->as.apply.args->len; i++)
          g_ptr_array_add (n->as.apply.args,
                           o42_node_copy (g_ptr_array_index (node->as.apply.args, i)));
      break;

    default:
      break;
    }

  return n;
}

/* A reference that has moved off the sheet is #REF!, in place.  Refs and
 * ranges own nothing, so the node can simply change type. */
static void
node_become_ref_error (O42Node *node)
{
  node->type = O42_NODE_ERROR;
  node->abs = 0;
  node->as.error = O42_ERR_REF;
}

static gboolean
in_sheet (int row, int col)
{
  return row >= 0 && row < O42_MAX_ROWS && col >= 0 && col < O42_MAX_COLS;
}

typedef gboolean (*RefVisitor) (O42Node *node, gpointer user);

/* Applies `visit` to every ref and range node.  Returns TRUE if any visit
 * reported a change. */
static gboolean
visit_refs (O42Node *node, RefVisitor visit, gpointer user)
{
  gboolean changed = FALSE;

  if (node == NULL)
    return FALSE;

  switch (node->type)
    {
    case O42_NODE_REF:
    case O42_NODE_RANGE:
      return visit (node, user);

    case O42_NODE_UNARY:
    case O42_NODE_BINARY:
      changed |= visit_refs (node->as.op.a, visit, user);
      changed |= visit_refs (node->as.op.b, visit, user);
      return changed;

    case O42_NODE_CALL:
      if (node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          changed |= visit_refs (g_ptr_array_index (node->as.call.args, i),
                                 visit, user);
      return changed;

    case O42_NODE_APPLY:
      changed |= visit_refs (node->as.apply.callee, visit, user);
      if (node->as.apply.args != NULL)
        for (guint i = 0; i < node->as.apply.args->len; i++)
          changed |= visit_refs (g_ptr_array_index (node->as.apply.args, i), visit, user);
      return changed;

    default:
      return FALSE;
    }
}

typedef struct { int drow, dcol; } Relocation;

static gboolean
relocate_visit (O42Node *node, gpointer user)
{
  const Relocation *d = user;

  if (node->type == O42_NODE_REF)
    {
      int row = node->as.ref.row + ((node->abs & O42_ABS_ROW0) ? 0 : d->drow);
      int col = node->as.ref.col + ((node->abs & O42_ABS_COL0) ? 0 : d->dcol);

      if (row == node->as.ref.row && col == node->as.ref.col)
        return FALSE;

      if (!in_sheet (row, col))
        node_become_ref_error (node);
      else
        {
          node->as.ref.row = row;
          node->as.ref.col = col;
        }
      return TRUE;
    }
  else
    {
      O42Range *r = &node->as.range;
      int drow = (node->abs & O42_WHOLE_COLS) ? 0 : d->drow;
      int dcol = (node->abs & O42_WHOLE_ROWS) ? 0 : d->dcol;
      int row0 = r->row0 + ((node->abs & O42_ABS_ROW0) ? 0 : drow);
      int col0 = r->col0 + ((node->abs & O42_ABS_COL0) ? 0 : dcol);
      int row1 = r->row1 + ((node->abs & O42_ABS_ROW1) ? 0 : drow);
      int col1 = r->col1 + ((node->abs & O42_ABS_COL1) ? 0 : dcol);

      if (row0 == r->row0 && col0 == r->col0 && row1 == r->row1 && col1 == r->col1)
        return FALSE;

      if (!in_sheet (row0, col0) || !in_sheet (row1, col1))
        node_become_ref_error (node);
      else
        {
          r->row0 = row0; r->col0 = col0;
          r->row1 = row1; r->col1 = col1;
        }
      return TRUE;
    }
}

gboolean
o42_node_relocate (O42Node *node, int drow, int dcol)
{
  Relocation d = { drow, dcol };

  if (drow == 0 && dcol == 0)
    return FALSE;

  return visit_refs (node, relocate_visit, &d);
}

typedef struct {
  gboolean    rows;
  int         at, count;
  int         band_lo, band_hi;   /* on the other axis; -1 for the whole sheet */
  const char *own;       /* the formula's sheet */
  const char *target;    /* the sheet whose rows moved */
} Shift;

/* Is the node inside the band on the other axis?  A range must lie
 * wholly inside to move; one that straddles the edge stays. */
static gboolean
in_band (const O42Node *node, const Shift *s)
{
  int lo, hi;

  if (s->band_lo < 0)
    return TRUE;

  if (node->type == O42_NODE_REF)
    lo = hi = s->rows ? node->as.ref.col : node->as.ref.row;
  else
    {
      lo = s->rows ? node->as.range.col0 : node->as.range.row0;
      hi = s->rows ? node->as.range.col1 : node->as.range.row1;
    }

  return lo >= s->band_lo && hi <= s->band_hi;
}

/* Does a reference node point into the sheet that changed? */
static gboolean
shift_applies (const O42Node *node, const Shift *s)
{
  const char *into = (node->sheet != NULL) ? node->sheet : s->own;

  if (into == NULL || s->target == NULL)
    return into == s->target;
  return strcmp (into, s->target) == 0;
}

/* Where a single index lands after the band moves.  -1 means it was
 * deleted. */
static int
shift_index (int i, int at, int count)
{
  if (count > 0)
    return (i >= at) ? i + count : i;

  if (i >= at - count)     /* past the deleted band */
    return i + count;
  if (i >= at)             /* inside it */
    return -1;
  return i;
}

static gboolean
shift_visit (O42Node *node, gpointer user)
{
  const Shift *s = user;

  if (!shift_applies (node, s) || !in_band (node, s))
    return FALSE;

  /* A whole column still holds every row after rows are put in or taken
   * out, and a whole row every column. */
  if (node->type == O42_NODE_RANGE &&
      (node->abs & (s->rows ? O42_WHOLE_COLS : O42_WHOLE_ROWS)))
    return FALSE;

  if (node->type == O42_NODE_REF)
    {
      int *idx = s->rows ? &node->as.ref.row : &node->as.ref.col;
      int moved = shift_index (*idx, s->at, s->count);

      if (moved == *idx)
        return FALSE;

      if (moved < 0 || !in_sheet (s->rows ? moved : node->as.ref.row,
                                  s->rows ? node->as.ref.col : moved))
        node_become_ref_error (node);
      else
        *idx = moved;
      return TRUE;
    }
  else
    {
      O42Range *r = &node->as.range;
      int *lo = s->rows ? &r->row0 : &r->col0;
      int *hi = s->rows ? &r->row1 : &r->col1;
      int new_lo = shift_index (*lo, s->at, s->count);
      int new_hi = shift_index (*hi, s->at, s->count);
      int limit = s->rows ? O42_MAX_ROWS : O42_MAX_COLS;

      /* A corner inside a deleted band snaps to the band's edge: the first
       * row after it for the top, the last row before it for the bottom.
       * If that leaves nothing, the whole range is gone. */
      if (s->count < 0)
        {
          if (new_lo < 0) new_lo = s->at;
          if (new_hi < 0) new_hi = s->at - 1;
        }

      if (new_lo == *lo && new_hi == *hi)
        return FALSE;

      if (new_hi < new_lo || new_hi >= limit)
        node_become_ref_error (node);
      else
        {
          *lo = new_lo;
          *hi = new_hi;
        }
      return TRUE;
    }
}

gboolean
o42_node_shift (O42Node *node, gboolean rows, int at, int count,
                const char *own, const char *target)
{
  Shift s = { rows, at, count, -1, -1, own, target };

  if (count == 0)
    return FALSE;

  return visit_refs (node, shift_visit, &s);
}

gboolean
o42_node_shift_within (O42Node *node, gboolean rows, int at, int count,
                       int band_lo, int band_hi,
                       const char *own, const char *target)
{
  Shift s = { rows, at, count, band_lo, band_hi, own, target };

  if (count == 0)
    return FALSE;

  return visit_refs (node, shift_visit, &s);
}

typedef struct {
  O42Range    from;
  int         drow, dcol;
  const char *own;
  const char *target;
} Move;

static gboolean
move_visit (O42Node *node, gpointer user)
{
  const Move *m = user;
  const char *sheet = node->sheet != NULL ? node->sheet : m->own;
  int r0, c0, r1, c1;

  if (m->target != NULL && (sheet == NULL || g_ascii_strcasecmp (sheet, m->target) != 0))
    return FALSE;

  if (node->type == O42_NODE_REF)
    { r0 = r1 = node->as.ref.row; c0 = c1 = node->as.ref.col; }
  else if (node->abs & (O42_WHOLE_COLS | O42_WHOLE_ROWS))
    return FALSE;
  else
    { r0 = node->as.range.row0; c0 = node->as.range.col0;
      r1 = node->as.range.row1; c1 = node->as.range.col1; }

  if (r0 < m->from.row0 || r1 > m->from.row1 || c0 < m->from.col0 || c1 > m->from.col1)
    return FALSE;
  if (r0 + m->drow < 0 || c0 + m->dcol < 0 ||
      r1 + m->drow >= O42_MAX_ROWS || c1 + m->dcol >= O42_MAX_COLS)
    return FALSE;

  if (node->type == O42_NODE_REF)
    {
      node->as.ref.row += m->drow;
      node->as.ref.col += m->dcol;
    }
  else
    {
      node->as.range.row0 += m->drow; node->as.range.row1 += m->drow;
      node->as.range.col0 += m->dcol; node->as.range.col1 += m->dcol;
    }
  return TRUE;
}

gboolean
o42_node_move_refs (O42Node *node, const O42Range *from, int drow, int dcol,
                    const char *own, const char *target)
{
  Move m = { *from, drow, dcol, own, target };

  if ((drow == 0 && dcol == 0) || from == NULL)
    return FALSE;
  return visit_refs (node, move_visit, &m);
}

typedef struct { const char *old_name, *new_name; } Rename;

static gboolean
rename_visit (O42Node *node, gpointer user)
{
  const Rename *r = user;

  gboolean changed = FALSE;

  if (node->sheet_last != NULL && strcmp (node->sheet_last, r->old_name) == 0)
    {
      node->sheet_last = g_intern_string (r->new_name);
      changed = TRUE;
    }
  if (node->sheet == NULL || strcmp (node->sheet, r->old_name) != 0)
    return changed;

  node->sheet = g_intern_string (r->new_name);
  return TRUE;
}

gboolean
o42_node_rename_sheet (O42Node *node, const char *old_name, const char *new_name)
{
  Rename r = { old_name, new_name };

  if (old_name == NULL || new_name == NULL || strcmp (old_name, new_name) == 0)
    return FALSE;

  return visit_refs (node, rename_visit, &r);
}

/* ---------------------------------------------------------------------- */
/* What a tree reads                                                       */
/* ---------------------------------------------------------------------- */

void
o42_node_collect_refs (const O42Node *node, GArray *ranges)
{
  if (node == NULL)
    return;

  switch (node->type)
    {
    case O42_NODE_REF:
      {
        O42SheetRange r;
        r.sheet = node->sheet;
        r.range.row0 = r.range.row1 = node->as.ref.row;
        r.range.col0 = r.range.col1 = node->as.ref.col;
        g_array_append_val (ranges, r);
        break;
      }

    case O42_NODE_RANGE:
      {
        O42SheetRange r;
        r.sheet = node->sheet;
        r.range = node->as.range;
        g_array_append_val (ranges, r);
        break;
      }

    case O42_NODE_UNARY:
    case O42_NODE_BINARY:
      o42_node_collect_refs (node->as.op.a, ranges);
      o42_node_collect_refs (node->as.op.b, ranges);
      break;

    case O42_NODE_CALL:
      if (node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          o42_node_collect_refs (g_ptr_array_index (node->as.call.args, i),
                                 ranges);
      break;

    case O42_NODE_APPLY:
      o42_node_collect_refs (node->as.apply.callee, ranges);
      if (node->as.apply.args != NULL)
        for (guint i = 0; i < node->as.apply.args->len; i++)
          o42_node_collect_refs (g_ptr_array_index (node->as.apply.args, i), ranges);
      break;

    default:
      break;
    }
}

/* ---------------------------------------------------------------------- */
/* Back to text                                                            */
/* ---------------------------------------------------------------------- */

static const char *
op_text (O42Op op)
{
  switch (op)
    {
    case O42_OP_ADD:    return "+";
    case O42_OP_SUB:    return "-";
    case O42_OP_MUL:    return "*";
    case O42_OP_DIV:    return "/";
    case O42_OP_POW:    return "^";
    case O42_OP_CONCAT: return "&";
    case O42_OP_EQ:     return "=";
    case O42_OP_NE:     return "<>";
    case O42_OP_LT:     return "<";
    case O42_OP_GT:     return ">";
    case O42_OP_LE:     return "<=";
    case O42_OP_GE:     return ">=";
    case O42_OP_NEG:    return "-";
    case O42_OP_POS:    return "+";
    case O42_OP_PERCENT: return "%";
    default:            return "?";
    }
}

void
o42_node_collect_names (const O42Node *node, GPtrArray *names)
{
  if (node == NULL)
    return;

  switch (node->type)
    {
    case O42_NODE_NAME:
      {
        char *upper = g_ascii_strup (node->as.name, -1);
        g_ptr_array_add (names, (gpointer) g_intern_string (upper));
        g_free (upper);
        break;
      }

    case O42_NODE_UNARY:
    case O42_NODE_BINARY:
      o42_node_collect_names (node->as.op.a, names);
      o42_node_collect_names (node->as.op.b, names);
      break;

    case O42_NODE_CALL:
      if (node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          o42_node_collect_names (g_ptr_array_index (node->as.call.args, i), names);
      break;

    case O42_NODE_APPLY:
      o42_node_collect_names (node->as.apply.callee, names);
      if (node->as.apply.args != NULL)
        for (guint k = 0; k < node->as.apply.args->len; k++)
          o42_node_collect_names (g_ptr_array_index (node->as.apply.args, k), names);
      break;

    default:
      break;
    }
}

char *
o42_sheet_name_quote (const char *name)
{
  gboolean plain = (*name != '\0' && !g_ascii_isdigit (*name));
  GString *out;

  for (const char *p = name; plain && *p != '\0'; p++)
    if (!g_ascii_isalnum (*p) && *p != '_')
      plain = FALSE;

  if (plain)
    return g_strdup (name);

  out = g_string_new ("'");
  for (const char *p = name; *p != '\0'; p++)
    {
      if (*p == '\'')
        g_string_append_c (out, '\'');
      g_string_append_c (out, *p);
    }
  g_string_append_c (out, '\'');
  return g_string_free (out, FALSE);
}

static void
write_sheet_prefix (const O42Node *node, GString *out)
{
  if (node->sheet != NULL && node->sheet_last != NULL)
    {
      /* Excel quotes the pair as one: 'Sheet 1:Sheet 3'!A1. */
      char *pair = g_strconcat (node->sheet, ":", node->sheet_last, NULL);
      char *q1 = o42_sheet_name_quote (node->sheet), *q2 = o42_sheet_name_quote (node->sheet_last);
      if (q1[0] == '\'' || q2[0] == '\'')
        {
          char *quoted = o42_sheet_name_quote (pair);
          g_string_append (out, quoted);
          g_free (quoted);
        }
      else
        g_string_append (out, pair);
      g_string_append_c (out, '!');
      g_free (pair); g_free (q1); g_free (q2);
    }
  else if (node->sheet != NULL)
    {
      char *quoted = o42_sheet_name_quote (node->sheet);
      g_string_append (out, quoted);
      g_string_append_c (out, '!');
      g_free (quoted);
    }
}

/* How tightly an operator binds, from the grammar above.  Used to put back
 * only the parentheses a formula needs, so that A1+B1 reads back as A1+B1
 * and not (A1+B1). */
static int
op_precedence (const O42Node *node)
{
  if (node->type == O42_NODE_UNARY)
    return (node->as.op.op == O42_OP_PERCENT) ? 7 : 6;

  if (node->type != O42_NODE_BINARY)
    return 100;

  switch (node->as.op.op)
    {
    case O42_OP_POW:    return 5;
    case O42_OP_MUL:
    case O42_OP_DIV:    return 4;
    case O42_OP_ADD:
    case O42_OP_SUB:    return 3;
    case O42_OP_CONCAT: return 2;
    default:            return 1;
    }
}

static void node_write (const O42Node *node, GString *out);

static void
node_write_child (const O42Node *child, const O42Node *parent,
                  gboolean right_side, GString *out)
{
  int pc = op_precedence (parent);
  int cc = op_precedence (child);
  gboolean parens;

  /* A child that binds looser needs brackets.  One that binds equally
   * needs them on the side the operator does not associate to, which
   * in a spreadsheet is the right of every operator, "^" included. */
  if (cc < pc)
    parens = TRUE;
  else if (cc == pc && parent->type == O42_NODE_BINARY)
    parens = right_side;
  else
    parens = FALSE;

  if (parens) g_string_append_c (out, '(');
  node_write (child, out);
  if (parens) g_string_append_c (out, ')');
}

static void
node_write (const O42Node *node, GString *out)
{
  if (node == NULL)
    return;

  switch (node->type)
    {
    case O42_NODE_NUMBER:
      {
        /* Every digit, or copying =A1*3.14159265358979 down a column
         * would leave 3.141592654 in the copies. */
        char *text = o42_number_to_text (node->as.number, TRUE);
        g_string_append (out, text);
        g_free (text);
        break;
      }

    case O42_NODE_STRING:
      {
        g_string_append_c (out, '"');
        for (const char *p = node->as.string; *p != '\0'; p++)
          {
            if (*p == '"')
              g_string_append (out, "\"\"");
            else
              g_string_append_c (out, *p);
          }
        g_string_append_c (out, '"');
        break;
      }

    case O42_NODE_BOOL:
      g_string_append (out, node->as.boolean ? "TRUE" : "FALSE");
      break;

    case O42_NODE_NAME:
      g_string_append (out, node->as.name);
      break;

    case O42_NODE_ERROR:
      g_string_append (out, o42_error_name (node->as.error));
      break;

    case O42_NODE_EMPTY:
      break;

    case O42_NODE_APPLY:
      node_write (node->as.apply.callee, out);
      g_string_append_c (out, '(');
      if (node->as.apply.args != NULL)
        for (guint i = 0; i < node->as.apply.args->len; i++)
          {
            if (i > 0)
              g_string_append_c (out, ',');
            node_write (g_ptr_array_index (node->as.apply.args, i), out);
          }
      g_string_append_c (out, ')');
      break;

    case O42_NODE_ARRAY:
      g_string_append_c (out, '{');
      for (int i = 0; i < node->as.array.rows * node->as.array.cols &&
                      (guint) i < node->as.array.items->len; i++)
        {
          if (i > 0)
            g_string_append_c (out, (i % node->as.array.cols == 0) ? ';' : ',');
          node_write (g_ptr_array_index (node->as.array.items, i), out);
        }
      g_string_append_c (out, '}');
      break;

    case O42_NODE_REF:
      {
        char *name = o42_ref_name_full (node->as.ref.row, node->as.ref.col,
                                        (node->abs & O42_ABS_ROW0) != 0,
                                        (node->abs & O42_ABS_COL0) != 0);
        write_sheet_prefix (node, out);
        g_string_append (out, name);
        g_free (name);
        break;
      }

    case O42_NODE_RANGE:
      {
        char *a, *b;

        if (node->abs & (O42_WHOLE_COLS | O42_WHOLE_ROWS))
          {
            char letters0[8], letters1[8];

            write_sheet_prefix (node, out);
            if (node->abs & O42_WHOLE_COLS)
              {
                o42_col_name (node->as.range.col0, letters0, sizeof letters0);
                o42_col_name (node->as.range.col1, letters1, sizeof letters1);
                g_string_append_printf (out, "%s%s:%s%s",
                                        (node->abs & O42_ABS_COL0) ? "$" : "", letters0,
                                        (node->abs & O42_ABS_COL1) ? "$" : "", letters1);
              }
            else
              g_string_append_printf (out, "%s%d:%s%d",
                                      (node->abs & O42_ABS_ROW0) ? "$" : "", node->as.range.row0 + 1,
                                      (node->abs & O42_ABS_ROW1) ? "$" : "", node->as.range.row1 + 1);
            break;
          }

        a = o42_ref_name_full (node->as.range.row0, node->as.range.col0,
                               (node->abs & O42_ABS_ROW0) != 0,
                               (node->abs & O42_ABS_COL0) != 0);
        b = o42_ref_name_full (node->as.range.row1, node->as.range.col1,
                               (node->abs & O42_ABS_ROW1) != 0,
                               (node->abs & O42_ABS_COL1) != 0);
        write_sheet_prefix (node, out);
        g_string_append_printf (out, "%s:%s", a, b);
        g_free (a);
        g_free (b);
        break;
      }

    case O42_NODE_UNARY:
      if (node->as.op.op == O42_OP_PERCENT)
        {
          node_write_child (node->as.op.a, node, FALSE, out);
          g_string_append_c (out, '%');
        }
      else
        {
          g_string_append (out, op_text (node->as.op.op));
          node_write_child (node->as.op.a, node, TRUE, out);
        }
      break;

    case O42_NODE_BINARY:
      node_write_child (node->as.op.a, node, FALSE, out);
      g_string_append (out, op_text (node->as.op.op));
      node_write_child (node->as.op.b, node, TRUE, out);
      break;

    case O42_NODE_CALL:
      g_string_append (out, node->as.call.name);
      g_string_append_c (out, '(');
      if (node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          {
            if (i > 0)
              g_string_append_c (out, ',');
            node_write (g_ptr_array_index (node->as.call.args, i), out);
          }
      g_string_append_c (out, ')');
      break;

    default:
      break;
    }
}

char *
o42_node_to_string (const O42Node *node)
{
  GString *out = g_string_new (NULL);

  node_write (node, out);

  return g_string_free (out, FALSE);
}

void
o42_node_prefix_functions (O42Node *node, gboolean (*is_future) (const char *),
                           const char *prefix)
{
  if (node == NULL)
    return;
  switch (node->type)
    {
    case O42_NODE_UNARY:
    case O42_NODE_BINARY:
      o42_node_prefix_functions (node->as.op.a, is_future, prefix);
      o42_node_prefix_functions (node->as.op.b, is_future, prefix);
      break;
    case O42_NODE_CALL:
      if (is_future (node->as.call.name))
        {
          char *renamed = g_strconcat (prefix, node->as.call.name, NULL);
          g_free (node->as.call.name);
          node->as.call.name = renamed;
        }
      if (node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          o42_node_prefix_functions (g_ptr_array_index (node->as.call.args, i), is_future, prefix);
      break;
    case O42_NODE_ARRAY:
      if (node->as.array.items != NULL)
        for (guint i = 0; i < node->as.array.items->len; i++)
          o42_node_prefix_functions (g_ptr_array_index (node->as.array.items, i), is_future, prefix);
      break;

    case O42_NODE_APPLY:
      o42_node_prefix_functions (node->as.apply.callee, is_future, prefix);
      if (node->as.apply.args != NULL)
        for (guint i = 0; i < node->as.apply.args->len; i++)
          o42_node_prefix_functions (g_ptr_array_index (node->as.apply.args, i), is_future, prefix);
      break;
    default:
      break;
    }
}
