/* o42-entry.c - see o42-entry.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-entry.h"

#include "o42-date.h"

#include <string.h>

/* A number the way people type one:
 *
 *   1234   -12.5   1e3   +7            what strtod reads
 *   1,234,567.89                        grouped in threes, and only in threes
 *   5%   12.5%                          a percentage
 *   $1,000   -$5   $-5                  money
 *   (5)   ($1,000)                      an accountant's minus
 *
 * The groups have to be right for the commas to count: 1,5 is text, not
 * fifteen, because it is what half the world types for one and a half. */
static gboolean
read_plain_number (const char *text, O42Entry *out)
{
  const char *p = text;
  gboolean paren = FALSE, negative = FALSE, money = FALSE, grouped = FALSE;
  gboolean percent = FALSE;
  int int_digits = 0, frac_digits = 0;
  GString *digits;
  double n;
  char *end;

  while (g_ascii_isspace (*p))
    p++;

  if (*p == '(')
    {
      paren = TRUE;
      p++;
      while (*p == ' ')
        p++;
    }

  if (*p == '-' || *p == '+')
    {
      negative = (*p == '-');
      p++;
    }
  if (*p == '$')
    {
      money = TRUE;
      p++;
      if (!negative && (*p == '-' || *p == '+'))
        {
          negative = (*p == '-');
          p++;
        }
    }

  digits = g_string_new (NULL);

  /* The integer part, with or without its thousands separators. */
  while (g_ascii_isdigit (*p))
    {
      g_string_append_c (digits, *p);
      p++;
      int_digits++;
    }
  if (*p == ',' && int_digits >= 1 && int_digits <= 3)
    {
      while (*p == ',')
        {
          const char *q = p + 1;

          if (!(g_ascii_isdigit (q[0]) && g_ascii_isdigit (q[1]) &&
                g_ascii_isdigit (q[2]) && !g_ascii_isdigit (q[3])))
            goto text;
          g_string_append_len (digits, q, 3);
          int_digits += 3;
          p = q + 3;
        }
      grouped = TRUE;
    }

  if (*p == '.')
    {
      g_string_append_c (digits, '.');
      p++;
      while (g_ascii_isdigit (*p))
        {
          g_string_append_c (digits, *p);
          p++;
          frac_digits++;
        }
    }

  if (int_digits + frac_digits == 0)
    goto text;

  if ((*p == 'e' || *p == 'E') && !grouped && !money)
    {
      const char *q = p + 1;

      if (*q == '+' || *q == '-')
        q++;
      if (g_ascii_isdigit (*q))
        {
          g_string_append_len (digits, p, q - p);
          while (g_ascii_isdigit (*q))
            g_string_append_c (digits, *q++);
          p = q;
        }
    }

  if (*p == '%')
    {
      percent = TRUE;
      p++;
    }

  if (paren)
    {
      while (*p == ' ')
        p++;
      if (*p != ')')
        goto text;
      p++;
      negative = TRUE;
    }

  while (g_ascii_isspace (*p))
    p++;
  if (*p != '\0')
    goto text;

  n = g_ascii_strtod (digits->str, &end);
  if (end == NULL || *end != '\0')
    goto text;
  g_string_free (digits, TRUE);

  if (negative)
    n = -n;
  if (percent)
    n /= 100.0;

  out->number = n;
  out->format = percent ? O42_NUM_PERCENT
              : money   ? O42_NUM_CURRENCY
              : grouped ? O42_NUM_COMMA
              :           O42_NUM_GENERAL;
  /* Excel keeps two decimals when any were typed, and none otherwise:
   * $1,000.5 shows as $1,000.50 and 5% as 5%. */
  out->decimals = (out->format != O42_NUM_GENERAL && frac_digits > 0) ? 2 : 0;
  return TRUE;

text:
  g_string_free (digits, TRUE);
  return FALSE;
}

gboolean
o42_entry_parse (const char *text, O42Entry *out)
{
  O42Entry entry = { 0, O42_NUM_GENERAL, 0 };
  gboolean has_date = FALSE, has_time = FALSE;

  g_return_val_if_fail (out != NULL, FALSE);

  if (text == NULL)
    return FALSE;

  if (read_plain_number (text, &entry))
    {
      *out = entry;
      return TRUE;
    }

  if (o42_date_parse (text, &entry.number, &has_date, &has_time))
    {
      entry.format = (has_date && has_time) ? O42_NUM_DATETIME
                   : has_date ? O42_NUM_DATE : O42_NUM_TIME;
      *out = entry;
      return TRUE;
    }

  return FALSE;
}

char *
o42_entry_quote_text (const char *text)
{
  O42Entry entry;

  if (text == NULL)
    return g_strdup ("");

  if (text[0] == '=' || text[0] == '\'' ||
      g_ascii_strcasecmp (text, "TRUE") == 0 ||
      g_ascii_strcasecmp (text, "FALSE") == 0 ||
      o42_entry_parse (text, &entry))
    return g_strconcat ("'", text, NULL);

  return g_strdup (text);
}
