/* o42-csv.c - see o42-csv.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-csv.h"

#include <string.h>

static void
append_field (GString *out, const char *text)
{
  gboolean quote = FALSE;

  for (const char *p = text; *p != '\0'; p++)
    if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r')
      {
        quote = TRUE;
        break;
      }

  /* Leading or trailing spaces are quoted too, so they survive a reader
   * that trims. */
  if (*text == ' ' || (*text != '\0' && text[strlen (text) - 1] == ' '))
    quote = TRUE;

  if (!quote)
    {
      g_string_append (out, text);
      return;
    }

  g_string_append_c (out, '"');
  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p == '"')
        g_string_append_c (out, '"');
      g_string_append_c (out, *p);
    }
  g_string_append_c (out, '"');
}

gboolean
o42_csv_save (O42Sheet *sheet, GFile *file, GError **error)
{
  O42Range used;
  GString *out = g_string_new (NULL);
  gboolean ok;

  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  o42_sheet_used_range (sheet, &used);

  /* From A1, not from the used range's corner, so that a sheet with its
   * data in C3 comes back in C3. */
  for (int row = 0; row <= used.row1; row++)
    {
      for (int col = 0; col <= used.col1; col++)
        {
          char *text = o42_sheet_get_display (sheet, row, col);

          if (col > 0)
            g_string_append_c (out, ',');
          append_field (out, text);
          g_free (text);
        }
      g_string_append (out, "\r\n");
    }

  ok = g_file_replace_contents (file, out->str, out->len, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, error);
  g_string_free (out, TRUE);

  return ok;
}

/* Reads one field starting at *p, leaving *p on the separator that ended
 * it -- a comma, a newline, or the end.  Caller frees. */
static char *
read_field (const char **p, char sep)
{
  GString *out = g_string_new (NULL);
  const char *s = *p;

  if (*s == '"')
    {
      s++;
      while (*s != '\0')
        {
          if (*s == '"')
            {
              if (s[1] == '"')
                {
                  g_string_append_c (out, '"');
                  s += 2;
                  continue;
                }
              s++;
              break;
            }
          g_string_append_c (out, *s);
          s++;
        }

      /* Anything between the closing quote and the separator is a
       * malformed file; keeping it is kinder than dropping it. */
      while (*s != '\0' && *s != sep && *s != '\n' && *s != '\r')
        g_string_append_c (out, *s++);
    }
  else
    {
      while (*s != '\0' && *s != sep && *s != '\n' && *s != '\r')
        g_string_append_c (out, *s++);
    }

  *p = s;
  return g_string_free (out, FALSE);
}

char *
o42_text_to_utf8 (const char *text, gssize length)
{
  char *converted;

  if (text == NULL)
    return g_strdup ("");
  if (length < 0)
    length = (gssize) strlen (text);
  if (g_utf8_validate (text, length, NULL))
    return g_strndup (text, (gsize) length);
  converted = g_locale_to_utf8 (text, length, NULL, NULL, NULL);
  if (converted == NULL)
    converted = g_convert (text, length, "UTF-8", "WINDOWS-1252", NULL, NULL, NULL);
  if (converted == NULL)
    {
      char *copy = g_strndup (text, (gsize) length);
      converted = g_utf8_make_valid (copy, -1);
      g_free (copy);
    }
  return converted;
}

gboolean
o42_csv_load (O42Sheet *sheet, GFile *file, GError **error)
{
  char *contents = NULL;
  gsize length = 0;
  const char *p;
  int row = 0, col = 0;
  O42Range everything = { 0, 0, O42_MAX_ROWS - 1, O42_MAX_COLS - 1 };
  char sep = ',';

  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return FALSE;

  /* A stray NUL is not the end of the file: it becomes a space, so
   * that the rest of the file is read rather than the whole refused. */
  for (gsize i = 0; i < length; i++)
    if (contents[i] == '\0')
      contents[i] = ' ';
  if (!g_utf8_validate (contents, (gssize) length, NULL))
    {
      char *converted = o42_text_to_utf8 (contents, (gssize) length);
      g_free (contents);
      contents = converted;
    }

  p = contents;
  if (g_str_has_prefix (p, "\357\273\277"))     /* a byte-order mark */
    p += 3;

  /* Half the world's Excels write semicolons, because their decimal point
   * is a comma.  If the first line has semicolons and no commas, that is
   * the file's separator. */
  {
    const char *nl = strchr (p, '\n');
    gsize first = (nl != NULL) ? (gsize) (nl - p) : strlen (p);

    if (memchr (p, ';', first) != NULL && memchr (p, ',', first) == NULL)
      sep = ';';
  }

  o42_sheet_begin_group (sheet);
  o42_sheet_clear_range (sheet, &everything);
  o42_sheet_clear_formats (sheet, &everything);

  while (*p != '\0')
    {
      char *field = read_field (&p, sep);

      if (*field != '\0' && row < O42_MAX_ROWS && col < O42_MAX_COLS)
        o42_sheet_set_input (sheet, row, col, field);
      g_free (field);

      if (*p == sep)
        {
          p++;
          col++;
          continue;
        }

      if (*p == '\r')
        p++;
      if (*p == '\n')
        p++;

      row++;
      col = 0;
    }

  o42_sheet_end_group (sheet);
  g_free (contents);

  return TRUE;
}
