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

static int fixed_decimals = -1;

void
o42_entry_set_fixed_decimals (int places)
{
  fixed_decimals = places < 0 ? -1 : MIN (places, 15);
}

int
o42_entry_fixed_decimals (void)
{
  return fixed_decimals;
}

char *
o42_entry_fixed_decimals_apply (const char *text)
{
  const char *p = text;
  gboolean digits = FALSE;
  double value, scale = 1;

  if (fixed_decimals < 0 || text == NULL)
    return NULL;
  while (*p == ' ') p++;
  if (*p == '-' || *p == '+') p++;
  if (*p == '\0')
    return NULL;
  for (; *p != '\0'; p++)
    {
      if (g_ascii_isdigit (*p)) digits = TRUE;
      else if (*p != ',' && *p != ' ') return NULL;   /* a point, an exponent, a sign: as typed */
    }
  if (!digits)
    return NULL;
  {
    O42Entry entry;
    if (!o42_entry_parse (text, &entry) || entry.format != O42_NUM_GENERAL)
      return NULL;
    value = entry.number;
  }
  for (int i = 0; i < fixed_decimals; i++)
    scale *= 10;
  value /= scale;
  return o42_number_to_text (value, TRUE);
}

/* "1 1/2", "-2 3/4", "0 1/8": a whole number, a space and a fraction,
 * which is how a fraction is typed so that it is not read as a date. */
static gboolean
read_fraction (const char *text, O42Entry *out)
{
  const char *p = text;
  gboolean negative = FALSE;
  double whole, num, den;
  char *end;

  while (g_ascii_isspace (*p)) p++;
  if (*p == '-' || *p == '+')
    negative = *p++ == '-';
  if (!g_ascii_isdigit (*p))
    return FALSE;
  whole = g_ascii_strtod (p, &end);
  if (end == p || *end != ' ')
    return FALSE;
  p = end;
  while (*p == ' ') p++;
  if (!g_ascii_isdigit (*p))
    return FALSE;
  num = g_ascii_strtod (p, &end);
  if (end == p || *end != '/')
    return FALSE;
  p = end + 1;
  if (!g_ascii_isdigit (*p))
    return FALSE;
  den = g_ascii_strtod (p, &end);
  if (end == p || den == 0)
    return FALSE;
  p = end;
  while (g_ascii_isspace (*p)) p++;
  if (*p != '\0')
    return FALSE;
  out->number = (whole + num / den) * (negative ? -1 : 1);
  out->format = O42_NUM_GENERAL;
  out->decimals = 0;
  return TRUE;
}

gboolean
o42_entry_parse (const char *text, O42Entry *out)
{
  O42Entry entry = { 0, O42_NUM_GENERAL, 0, NULL };
  gboolean has_date = FALSE, has_time = FALSE;

  g_return_val_if_fail (out != NULL, FALSE);

  if (text == NULL)
    return FALSE;

  if (read_plain_number (text, &entry))
    {
      *out = entry;
      return TRUE;
    }

  if (read_fraction (text, &entry))
    {
      *out = entry;
      return TRUE;
    }

  if (o42_date_parse (text, &entry.number, &has_date, &has_time))
    {
      entry.format = (has_date && has_time) ? O42_NUM_DATETIME
                   : has_date ? O42_NUM_DATE : O42_NUM_TIME;
      /* A time past a day -- 24:00, 25:30 -- shows as elapsed hours,
       * which is what Excel gives such an entry. */
      if (!has_date && has_time && entry.number >= 1.0)
        entry.custom = g_intern_static_string ("[h]:mm:ss");
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
