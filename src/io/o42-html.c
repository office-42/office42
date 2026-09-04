/* o42-html.c - a book out as HTML tables, and tables back in
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Writing is straightforward: a table per sheet with inline style.
 * Reading cannot use an XML parser, because pages in the wild are not
 * XML: unclosed <td>, bare ampersands, attributes without quotes.  So
 * the reader is a small tolerant scanner that looks for the table
 * tags, keeps the text between them, and ignores everything else.
 */

#include "o42-html.h"
#include "o42-csv.h"

#include "o42-sheet.h"
#include "o42-date.h"

#include <string.h>

/* ====================================================================== */
/* Writing                                                                */
/* ====================================================================== */

static void
append_style (GString *out, const O42Fmt *fmt, const O42Value *value)
{
  GString *style = g_string_new (NULL);
  O42HAlign halign = o42_fmt_effective_halign (fmt, value);
  guint32 colour = fmt->colour;

  o42_fmt_display_colour (fmt, value, &colour);
  if (fmt->bold) g_string_append (style, "font-weight:bold;");
  if (fmt->italic) g_string_append (style, "font-style:italic;");
  if (fmt->underline && fmt->strikeout) g_string_append (style, "text-decoration:underline line-through;");
  else if (fmt->underline) g_string_append (style, "text-decoration:underline;");
  else if (fmt->strikeout) g_string_append (style, "text-decoration:line-through;");
  if (colour != 0)
    g_string_append_printf (style, "color:#%06x;", colour & 0xFFFFFF);
  if (fmt->fill != O42_FILL_NONE)
    g_string_append_printf (style, "background:#%06x;", fmt->fill & 0xFFFFFF);
  if (fmt->family != NULL)
    {
      /* The family may hold anything; only a quote would end the
       * attribute, and there is none in a font name we would keep. */
      char *family = g_strdup (fmt->family);
      for (char *q = family; *q != '\0'; q++)
        if (*q == '"' || *q == '<' || *q == '&') *q = ' ';
      g_string_append_printf (style, "font-family:%s;", family);
      g_free (family);
    }
  g_string_append_printf (style, "font-size:%dpt;", fmt->size / 2);
  if (halign == O42_HALIGN_RIGHT) g_string_append (style, "text-align:right;");
  else if (halign == O42_HALIGN_CENTRE) g_string_append (style, "text-align:center;");
  if (fmt->valign == O42_VALIGN_TOP) g_string_append (style, "vertical-align:top;");
  else if (fmt->valign == O42_VALIGN_MIDDLE) g_string_append (style, "vertical-align:middle;");
  if (fmt->wrap) g_string_append (style, "white-space:normal;");
  if (fmt->indent > 0) g_string_append_printf (style, "padding-left:%dpx;", fmt->indent * 9);
  {
    static const char *sides[4] = { "top", "bottom", "left", "right" };
    for (int i = 0; i < 4; i++)
      if (fmt->border_style[i] != O42_BORDER_NONE)
        {
          O42BorderStyle st = fmt->border_style[i];
          const char *kind = st == O42_BORDER_DOUBLE ? "double" : st == O42_BORDER_DASHED ? "dashed"
                           : st == O42_BORDER_DOTTED ? "dotted" : "solid";
          int px = st == O42_BORDER_THICK ? 3 : st == O42_BORDER_MEDIUM ? 2 : 1;
          g_string_append_printf (style, "border-%s:%dpx %s #%06x;", sides[i], px, kind,
                                  fmt->border_colour[i] & 0xFFFFFF);
        }
  }
  if (style->len > 0)
    g_string_append_printf (out, " style=\"%s\"", style->str);
  g_string_free (style, TRUE);
}

/* The merge whose top-left corner is here, and whether a cell is
 * covered by one that started earlier. */
static const O42Range *
merge_head (O42Sheet *sheet, int row, int col)
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

gboolean
o42_html_save (O42Book *book, GFile *file, GError **error)
{
  GString *out = g_string_new (NULL);
  gboolean ok;
  char *name = g_file_get_basename (file);
  char *title = g_markup_escape_text (name != NULL ? name : "office42", -1);

  g_return_val_if_fail (book != NULL && G_IS_FILE (file), FALSE);

  g_string_append_printf (out,
    "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>%s</title>\n"
    "<style>\nbody { font-family: sans-serif; }\n"
    "table { border-collapse: collapse; }\n"
    "td, th { padding: 1px 4px; }\n</style>\n</head>\n<body>\n", title);
  g_free (title);
  g_free (name);

  for (int i = 0; i < o42_book_n_sheets (book); i++)
    {
      O42Sheet *sheet = o42_book_sheet (book, i);
      O42Range used;
      char *sheet_name = g_markup_escape_text (o42_sheet_get_name (sheet), -1);

      o42_sheet_used_range (sheet, &used);
      g_string_append_printf (out, "<h2>%s</h2>\n", sheet_name);
      g_free (sheet_name);
      if (used.row1 < used.row0 || used.col1 < used.col0)
        {
          g_string_append (out, "<table></table>\n");
          continue;
        }
      g_string_append (out, "<table>\n");
      for (int row = used.row0; row <= used.row1; row++)
        {
          if (o42_sheet_row_hidden (sheet, row))
            continue;
          g_string_append (out, "<tr>");
          for (int col = used.col0; col <= used.col1; col++)
            {
              const O42Range *m;
              const O42Fmt *fmt;
              O42Value value;
              char *text, *escaped;
              const char *link;

              if (o42_sheet_col_hidden (sheet, col) || merge_covers (sheet, row, col))
                continue;
              m = merge_head (sheet, row, col);
              fmt = o42_sheet_get_fmt (sheet, row, col);
              o42_sheet_get_value (sheet, row, col, &value);
              text = o42_fmt_display (fmt, &value);
              escaped = g_markup_escape_text (text, -1);
              link = o42_sheet_get_link (sheet, row, col);

              g_string_append (out, "<td");
              if (m != NULL && m->col1 > m->col0)
                g_string_append_printf (out, " colspan=\"%d\"", m->col1 - m->col0 + 1);
              if (m != NULL && m->row1 > m->row0)
                g_string_append_printf (out, " rowspan=\"%d\"", m->row1 - m->row0 + 1);
              append_style (out, fmt, &value);
              g_string_append_c (out, '>');
              if (link != NULL && link[0] != '#')
                {
                  char *href = g_markup_escape_text (link, -1);
                  g_string_append_printf (out, "<a href=\"%s\">%s</a>", href, escaped);
                  g_free (href);
                }
              else
                g_string_append (out, escaped);
              g_string_append (out, "</td>");

              g_free (escaped);
              g_free (text);
              o42_value_clear (&value);
            }
          g_string_append (out, "</tr>\n");
        }
      g_string_append (out, "</table>\n");
    }
  g_string_append (out, "</body>\n</html>\n");

  ok = g_file_replace_contents (file, out->str, out->len, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, error);
  g_string_free (out, TRUE);
  return ok;
}

/* ====================================================================== */
/* Reading                                                                */
/* ====================================================================== */

/* &amp; and its kin, and the numeric ones. */
static void
append_entity (GString *out, const char *name, gsize len)
{
  static const struct { const char *name; const char *text; } NAMED[] = {
    { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" },
    { "nbsp", " " }, { "ndash", "\342\200\223" }, { "mdash", "\342\200\224" },
    { "hellip", "\342\200\246" }, { "pound", "\302\243" }, { "euro", "\342\202\254" },
    { "copy", "\302\251" }, { "deg", "\302\260" }
  };

  if (len > 1 && name[0] == '#')
    {
      gunichar c = (gunichar) (name[1] == 'x' || name[1] == 'X'
                               ? g_ascii_strtoull (name + 2, NULL, 16)
                               : g_ascii_strtoull (name + 1, NULL, 10));
      if (c > 0 && g_unichar_validate (c))
        {
          char buf[8];
          int n = g_unichar_to_utf8 (c, buf);
          g_string_append_len (out, buf, n);
          return;
        }
    }
  for (guint i = 0; i < G_N_ELEMENTS (NAMED); i++)
    if (strlen (NAMED[i].name) == len && g_ascii_strncasecmp (name, NAMED[i].name, len) == 0)
      {
        g_string_append (out, NAMED[i].text);
        return;
      }
  /* Not one we know: leave it as it was written. */
  g_string_append_c (out, '&');
  g_string_append_len (out, name, (gssize) len);
  g_string_append_c (out, ';');
}

/* The tag name at `p` (just past '<'), lower case, and where its '>'
 * is.  `closing` says whether it began with '/'. */
static char *
read_tag (const char *p, const char **end, gboolean *closing, char **attrs)
{
  GString *name = g_string_new (NULL);
  const char *q = p;
  const char *attr_start;

  *closing = FALSE;
  if (*q == '/')
    { *closing = TRUE; q++; }
  while (*q != '\0' && !g_ascii_isspace (*q) && *q != '>' && *q != '/')
    g_string_append_c (name, g_ascii_tolower (*q++));
  attr_start = q;
  while (*q != '\0' && *q != '>')
    {
      /* Skip over quoted attribute values so a '>' inside one does
       * not end the tag. */
      if (*q == '"' || *q == '\'')
        {
          char quote = *q++;
          while (*q != '\0' && *q != quote) q++;
          if (*q != '\0') q++;
          continue;
        }
      q++;
    }
  if (attrs != NULL)
    *attrs = g_strndup (attr_start, (gsize) (q - attr_start));
  *end = (*q == '>') ? q + 1 : q;
  return g_string_free (name, FALSE);
}

/* An attribute's value from a tag's attribute text. */
static int
attr_int (const char *attrs, const char *want, int fallback)
{
  const char *p = attrs;
  gsize len = strlen (want);

  while (p != NULL && *p != '\0')
    {
      while (*p != '\0' && (g_ascii_isspace (*p) || *p == '/')) p++;
      if (g_ascii_strncasecmp (p, want, len) == 0 && (p[len] == '=' || g_ascii_isspace (p[len])))
        {
          const char *v = strchr (p, '=');
          if (v == NULL)
            return fallback;
          v++;
          while (*v == '"' || *v == '\'' || g_ascii_isspace (*v)) v++;
          return atoi (v);
        }
      while (*p != '\0' && !g_ascii_isspace (*p)) p++;
    }
  return fallback;
}

/* Writes the cell being read, if there is one: pages leave <td>
 * unclosed often enough that the next tag has to finish it. */
static void
flush_cell (O42Sheet *sheet, GString *cell, gboolean *in_cell, int row, int *col, int *span)
{
  char *shown;

  if (!*in_cell)
    return;
  shown = g_strstrip (g_strdup (cell->str));
  if (*shown != '\0' && row < O42_MAX_ROWS && *col < O42_MAX_COLS)
    o42_sheet_set_input (sheet, row, *col, shown);
  g_free (shown);
  *col += *span;
  *in_cell = FALSE;
  *span = 1;
  g_string_truncate (cell, 0);
}

gboolean
o42_html_load (O42Sheet *sheet, GFile *file, GError **error)
{
  char *text = NULL;
  gsize length = 0;
  const char *p;
  GString *cell = g_string_new (NULL);
  int row = 0, col = 0;
  int span = 1;
  gboolean in_cell = FALSE, in_row = FALSE, in_table = FALSE, skipping = FALSE;
  O42Range used;
  GArray *pending;   /* rowspans still to skip: row, col, count */

  g_return_val_if_fail (sheet != NULL && G_IS_FILE (file), FALSE);
  if (!g_file_load_contents (file, NULL, &text, &length, NULL, error))
    return FALSE;
  {
    /* A page that is not UTF-8 is read as the code page it is likely
     * in, rather than put into the model as bytes it cannot save. */
    char *utf8 = o42_text_to_utf8 (text, (gssize) length);
    g_free (text);
    text = utf8;
    length = strlen (text);
  }

  /* Tables land under whatever the sheet already holds; an empty
   * sheet still answers 0,0 for its used range, so A1 decides. */
  o42_sheet_used_range (sheet, &used);
  row = 0;
  if (used.row1 > used.row0 || used.col1 > used.col0)
    row = used.row1 + 2;
  else
    {
      char *first = o42_sheet_get_input (sheet, 0, 0);
      if (*first != '\0')
        row = used.row1 + 2;
      g_free (first);
    }
  pending = g_array_new (FALSE, FALSE, sizeof (int) * 3);

  o42_sheet_begin_group (sheet);
  for (p = text; *p != '\0'; )
    {
      if (*p == '<')
        {
          gboolean closing;
          char *attrs = NULL;
          const char *end;
          char *tag = read_tag (p + 1, &end, &closing, &attrs);

          if (strcmp (tag, "script") == 0 || strcmp (tag, "style") == 0)
            skipping = !closing;
          else if (strcmp (tag, "table") == 0)
            {
              flush_cell (sheet, cell, &in_cell, row, &col, &span);
              if (!closing)
                { in_table = TRUE; in_row = FALSE; }
              else
                {
                  if (in_row) row++;
                  in_row = FALSE;
                  in_table = FALSE;
                  row += 2;   /* a blank row between tables */
                  col = 0;
                }
            }
          else if (in_table && (strcmp (tag, "tr") == 0))
            {
              flush_cell (sheet, cell, &in_cell, row, &col, &span);
              if (!closing)
                {
                  /* A page may leave the last row open too. */
                  if (in_row) row++;
                  in_row = TRUE;
                  col = 0;
                }
              else if (in_row)
                { in_row = FALSE; row++; }
            }
          else if (in_table && (strcmp (tag, "td") == 0 || strcmp (tag, "th") == 0))
            {
              flush_cell (sheet, cell, &in_cell, row, &col, &span);
              if (!closing)
                {
                  int rowspan;

                  /* A cell held open by a rowspan above pushes this
                   * one along. */
                  for (guint i = 0; i < pending->len; i++)
                    {
                      int *e = &g_array_index (pending, int, i * 3);
                      if (e[0] == row && e[1] == col && e[2] > 0)
                        { col += 1; i = 0; }
                    }
                  in_cell = TRUE;
                  g_string_truncate (cell, 0);
                  span = MAX (attr_int (attrs, "colspan", 1), 1);
                  rowspan = MAX (attr_int (attrs, "rowspan", 1), 1);
                  if (rowspan > 1)
                    for (int r = 1; r < rowspan; r++)
                      {
                        int entry[3] = { row + r, col, 1 };
                        g_array_append_vals (pending, entry, 3);
                      }
                }
            }
          else if (in_cell && (strcmp (tag, "br") == 0 || strcmp (tag, "p") == 0))
            g_string_append_c (cell, '\n');

          g_free (tag);
          g_free (attrs);
          p = end;
          continue;
        }

      if (*p == '&')
        {
          const char *semi = strchr (p, ';');
          if (semi != NULL && semi - p <= 10)
            {
              if (in_cell && !skipping)
                append_entity (cell, p + 1, (gsize) (semi - p - 1));
              p = semi + 1;
              continue;
            }
        }
      if (in_cell && !skipping)
        {
          /* Runs of blank space become one space, as a browser shows them. */
          if (g_ascii_isspace (*p))
            {
              if (cell->len > 0 && cell->str[cell->len - 1] != ' ' && cell->str[cell->len - 1] != '\n')
                g_string_append_c (cell, ' ');
            }
          else
            g_string_append_c (cell, *p);
        }
      p++;
    }
  flush_cell (sheet, cell, &in_cell, row, &col, &span);
  o42_sheet_end_group (sheet);

  g_array_unref (pending);
  g_string_free (cell, TRUE);
  g_free (text);
  return TRUE;
}
