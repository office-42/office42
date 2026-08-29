/* o42-types.c - see o42-types.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-types.h"

#include <string.h>

void
o42_col_name (int col, char *buffer, gsize size)
{
  char scratch[8];
  int n = 0;

  g_return_if_fail (buffer != NULL && size > 0);

  if (col < 0)
    {
      buffer[0] = '\0';
      return;
    }

  /* Column names are bijective base-26: after Z comes AA, not BA.  The
   * "- 1" on each round is what makes it bijective rather than ordinary
   * base-26 with a silent zero digit. */
  do
    {
      scratch[n++] = (char) ('A' + (col % 26));
      col = col / 26 - 1;
    }
  while (col >= 0 && n < (int) sizeof scratch);

  if ((gsize) n >= size)
    n = (int) size - 1;

  for (int i = 0; i < n; i++)
    buffer[i] = scratch[n - 1 - i];

  buffer[n] = '\0';
}

char *
o42_ref_name (int row, int col)
{
  char letters[8];

  o42_col_name (col, letters, sizeof letters);

  return g_strdup_printf ("%s%d", letters, row + 1);
}

char *
o42_ref_name_full (int row, int col, gboolean row_abs, gboolean col_abs)
{
  char letters[8];

  o42_col_name (col, letters, sizeof letters);

  return g_strdup_printf ("%s%s%s%d", col_abs ? "$" : "", letters,
                          row_abs ? "$" : "", row + 1);
}

gboolean
o42_ref_parse (const char *text, int *row, int *col, gsize *length)
{
  return o42_ref_parse_full (text, row, col, NULL, NULL, length);
}

gboolean
o42_ref_parse_full (const char *text, int *row, int *col,
                    gboolean *row_abs, gboolean *col_abs, gsize *length)
{
  const char *p = text;
  int c = 0;
  int r = 0;
  int letters = 0;
  int digits = 0;
  gboolean cabs = FALSE, rabs = FALSE;

  g_return_val_if_fail (text != NULL, FALSE);

  if (*p == '$')
    {
      cabs = TRUE;
      p++;
    }

  while (g_ascii_isalpha (*p) && letters < 3)
    {
      c = c * 26 + (g_ascii_toupper (*p) - 'A' + 1);
      p++;
      letters++;
    }

  if (letters == 0)
    return FALSE;

  if (*p == '$')
    {
      rabs = TRUE;
      p++;
    }

  while (g_ascii_isdigit (*p) && digits < 7)
    {
      r = r * 10 + (*p - '0');
      p++;
      digits++;
    }

  if (digits == 0 || r == 0)
    return FALSE;

  c -= 1;      /* the count above is one-based; the program is not */
  r -= 1;

  if (c < 0 || c >= O42_MAX_COLS || r < 0 || r >= O42_MAX_ROWS)
    return FALSE;

  if (row)     *row = r;
  if (col)     *col = c;
  if (row_abs) *row_abs = rabs;
  if (col_abs) *col_abs = cabs;
  if (length)  *length = (gsize) (p - text);

  return TRUE;
}
