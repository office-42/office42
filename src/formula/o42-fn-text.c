/* o42-fn-text.c - see o42-eval-private.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-eval-private.h"

#include "o42-date.h"
#include "o42-numfmt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The names this file has always called them by. */
#define operand_value o42_operand_value
#define normal_cdf o42_normal_cdf
#define normal_pdf o42_normal_pdf
#define collect_numbers o42_collect_numbers
#define round_half_away o42_round_half_away

/* ---- Text ------------------------------------------------------------- */

static O42Value
fn_len (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *s = NULL;
  glong length;

  (void) n;
  ARG_TEXT (0, s);
  length = g_utf8_strlen (s, -1);
  g_free (s);

  return o42_value_number ((double) length);
}

static O42Value
fn_left_right (O42EvalContext *ctx, O42Operand *args, int n, gboolean from_left)
{
  char *s = NULL;
  double count = 1;
  glong length;
  char *result;

  ARG_TEXT (0, s);
  if (n >= 2)
    {
      O42Value v = operand_value (ctx, &args[1]);
      O42ErrorCode err = O42_ERR_VALUE;
      gboolean ok = o42_value_to_number (&v, &count, &err);
      o42_value_clear (&v);
      if (!ok) { g_free (s); return o42_value_error (err); }
    }

  if (count < 0)
    { g_free (s); return o42_value_error (O42_ERR_VALUE); }

  length = g_utf8_strlen (s, -1);
  if (count > length)
    count = length;

  if (from_left)
    result = g_utf8_substring (s, 0, (glong) count);
  else
    result = g_utf8_substring (s, length - (glong) count, length);

  g_free (s);
  return o42_value_take (result);
}

static O42Value fn_left  (O42EvalContext *c, O42Operand *a, int n) { return fn_left_right (c, a, n, TRUE); }
static O42Value fn_right (O42EvalContext *c, O42Operand *a, int n) { return fn_left_right (c, a, n, FALSE); }

static O42Value
fn_mid (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *s = NULL;
  double start, count;
  glong length;
  char *result;

  (void) n;
  ARG_TEXT (0, s);
  ARG_NUMBER (1, start);
  ARG_NUMBER (2, count);

  if (start < 1 || count < 0)
    { g_free (s); return o42_value_error (O42_ERR_VALUE); }

  length = g_utf8_strlen (s, -1);
  if (start > length)
    { g_free (s); return o42_value_text (""); }

  if (start - 1 + count > length)
    count = length - (start - 1);

  result = g_utf8_substring (s, (glong) start - 1, (glong) (start - 1 + count));
  g_free (s);
  return o42_value_take (result);
}

static O42Value
fn_upper (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *s = NULL;
  char *result;

  (void) n;
  ARG_TEXT (0, s);
  result = g_utf8_strup (s, -1);
  g_free (s);
  return o42_value_take (result);
}

static O42Value
fn_lower (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *s = NULL;
  char *result;

  (void) n;
  ARG_TEXT (0, s);
  result = g_utf8_strdown (s, -1);
  g_free (s);
  return o42_value_take (result);
}

static O42Value
fn_trim (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *s = NULL;
  char *result;

  (void) n;
  ARG_TEXT (0, s);
  result = g_strstrip (g_strdup (s));
  g_free (s);
  return o42_value_take (result);
}

static O42Value
fn_concatenate (O42EvalContext *ctx, O42Operand *args, int n)
{
  GString *out = g_string_new (NULL);

  for (int i = 0; i < n; i++)
    {
      O42Value v = operand_value (ctx, &args[i]);
      char *text;

      if (v.type == O42_VALUE_ERROR)
        {
          O42ErrorCode err = v.as.error;
          o42_value_clear (&v);
          g_string_free (out, TRUE);
          return o42_value_error (err);
        }

      text = o42_value_to_text (&v);
      g_string_append (out, text);
      g_free (text);
      o42_value_clear (&v);
    }

  return o42_value_take (g_string_free (out, FALSE));
}

static O42Value
fn_rept (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *s = NULL;
  double count;
  GString *out;

  (void) n;
  ARG_TEXT (0, s);
  ARG_NUMBER (1, count);

  if (count < 0 || count > 10000)
    { g_free (s); return o42_value_error (O42_ERR_VALUE); }

  out = g_string_new (NULL);
  for (int i = 0; i < (int) count; i++)
    g_string_append (out, s);

  g_free (s);
  return o42_value_take (g_string_free (out, FALSE));
}

static O42Value
fn_find (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *needle = NULL, *hay = NULL;
  const char *found;
  double start = 1;
  const char *from;
  O42Value result;

  ARG_TEXT (0, needle);
  ARG_TEXT (1, hay);
  if (n >= 3)
    {
      O42Value v = operand_value (ctx, &args[2]);
      O42ErrorCode err = O42_ERR_VALUE;
      gboolean ok = o42_value_to_number (&v, &start, &err);
      o42_value_clear (&v);
      if (!ok) { g_free (needle); g_free (hay); return o42_value_error (err); }
    }

  if (start < 1 || start > g_utf8_strlen (hay, -1) + 1)
    { g_free (needle); g_free (hay); return o42_value_error (O42_ERR_VALUE); }

  from = g_utf8_offset_to_pointer (hay, (glong) start - 1);
  found = strstr (from, needle);

  result = (found == NULL)
    ? o42_value_error (O42_ERR_VALUE)
    : o42_value_number ((double) g_utf8_pointer_to_offset (hay, found) + 1);

  g_free (needle);
  g_free (hay);
  return result;
}

static O42Value
fn_substitute (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *hay = NULL, *needle = NULL, *with = NULL;
  char **parts;
  char *result;

  (void) n;
  ARG_TEXT (0, hay);
  ARG_TEXT (1, needle);
  ARG_TEXT (2, with);

  if (*needle == '\0')
    { g_free (needle); g_free (with); return o42_value_take (hay); }

  parts = g_strsplit (hay, needle, -1);
  result = g_strjoinv (with, parts);

  g_strfreev (parts);
  g_free (hay);
  g_free (needle);
  g_free (with);

  return o42_value_take (result);
}

static O42Value
fn_value (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  return o42_value_number (x);
}

static O42Value
fn_rows_cols (O42EvalContext *ctx, O42Operand *args, int n, gboolean rows)
{
  (void) ctx; (void) n;

  if (!args[0].is_range)
    return o42_value_number (1);

  return o42_value_number (rows
    ? args[0].range.row1 - args[0].range.row0 + 1
    : args[0].range.col1 - args[0].range.col0 + 1);
}

static O42Value fn_rows    (O42EvalContext *c, O42Operand *a, int n) { return fn_rows_cols (c, a, n, TRUE); }
static O42Value fn_columns (O42EvalContext *c, O42Operand *a, int n) { return fn_rows_cols (c, a, n, FALSE); }
/* ---- More text -------------------------------------------------------- */

static O42Value
fn_proper (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *s = NULL;
  GString *out;
  gboolean start = TRUE;

  (void) n;
  ARG_TEXT (0, s);

  out = g_string_new (NULL);
  for (const char *p = s; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_isalpha (c))
        {
          g_string_append_unichar (out, start ? g_unichar_totitle (c)
                                              : g_unichar_tolower (c));
          start = FALSE;
        }
      else
        {
          g_string_append_unichar (out, c);
          start = TRUE;
        }
    }

  g_free (s);
  return o42_value_take (g_string_free (out, FALSE));
}

static O42Value
fn_exact (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *a = NULL, *b = NULL;
  gboolean same;
  (void) n;
  ARG_TEXT (0, a);
  ARG_TEXT (1, b);
  same = strcmp (a, b) == 0;
  g_free (a);
  g_free (b);
  return o42_value_bool (same);
}

static O42Value
fn_char (O42EvalContext *ctx, O42Operand *args, int n)
{
  double code;
  char buf[8];
  int len;
  (void) n;
  ARG_NUMBER (0, code);
  if (code < 1 || code > 0x10FFFF)
    return o42_value_error (O42_ERR_VALUE);
  len = g_unichar_to_utf8 ((gunichar) code, buf);
  buf[len] = '\0';
  return o42_value_text (buf);
}

static O42Value
fn_code (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *s = NULL;
  gunichar c;
  (void) n;
  ARG_TEXT (0, s);
  if (*s == '\0')
    { g_free (s); return o42_value_error (O42_ERR_VALUE); }
  c = g_utf8_get_char (s);
  g_free (s);
  return o42_value_number (c);
}

static O42Value
fn_clean (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *s = NULL;
  GString *out;
  (void) n;
  ARG_TEXT (0, s);
  out = g_string_new (NULL);
  for (const char *p = s; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (c >= 32)
        g_string_append_unichar (out, c);
    }
  g_free (s);
  return o42_value_take (g_string_free (out, FALSE));
}

/* Does `pattern`, which may hold * and ?, match a prefix of `text`?  Used
 * by SEARCH, which wants the earliest position at which the pattern
 * begins. */
static gboolean
glob_matches_prefix (const char *pattern, const char *text)
{
  for (;;)
    {
      gunichar pc, tc;

      if (*pattern == '\0')
        return TRUE;

      pc = g_utf8_get_char (pattern);

      if (pc == '*')
        {
          const char *rest = g_utf8_next_char (pattern);
          for (const char *t = text; ; t = g_utf8_next_char (t))
            {
              if (glob_matches_prefix (rest, t))
                return TRUE;
              if (*t == '\0')
                return FALSE;
            }
        }

      if (*text == '\0')
        return FALSE;

      tc = g_utf8_get_char (text);
      if (pc != '?' && pc != tc)
        return FALSE;

      pattern = g_utf8_next_char (pattern);
      text = g_utf8_next_char (text);
    }
}

static O42Value
fn_search (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *needle = NULL, *hay = NULL, *fneedle, *fhay;
  double start = 1;
  glong length;
  O42Value result = o42_value_error (O42_ERR_VALUE);

  ARG_TEXT (0, needle);
  ARG_TEXT (1, hay);
  if (n >= 3)
    {
      O42Value v = operand_value (ctx, &args[2]);
      O42ErrorCode err = O42_ERR_VALUE;
      gboolean ok = o42_value_to_number (&v, &start, &err);
      o42_value_clear (&v);
      if (!ok) { g_free (needle); g_free (hay); return o42_value_error (err); }
    }

  length = g_utf8_strlen (hay, -1);
  if (start < 1 || start > length + 1)
    { g_free (needle); g_free (hay); return o42_value_error (O42_ERR_VALUE); }

  /* Case-insensitive, unlike FIND, and with wildcards.  Folding both
   * sides keeps the character positions in step only if folding does not
   * change lengths, which it can for a few letters; SEARCH("ß", ...) may
   * be a character off, and that is a corner not worth a slower path. */
  fneedle = g_utf8_casefold (needle, -1);
  fhay = g_utf8_casefold (hay, -1);

  {
    const char *p = g_utf8_offset_to_pointer (fhay, (glong) start - 1);
    glong pos = (glong) start - 1;

    for (; ; p = g_utf8_next_char (p), pos++)
      {
        if (glob_matches_prefix (fneedle, p))
          {
            result = o42_value_number (pos + 1);
            break;
          }
        if (*p == '\0')
          break;
      }
  }

  g_free (fneedle);
  g_free (fhay);
  g_free (needle);
  g_free (hay);
  return result;
}

static O42Value
fn_replace (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *old = NULL, *with = NULL;
  double start, count;
  glong length;
  GString *out;

  (void) n;
  ARG_TEXT (0, old);
  ARG_NUMBER (1, start);
  ARG_NUMBER (2, count);
  {
    O42Value v = operand_value (ctx, &args[3]);
    if (v.type == O42_VALUE_ERROR)
      { O42ErrorCode e = v.as.error; o42_value_clear (&v); g_free (old); return o42_value_error (e); }
    with = o42_value_to_text (&v);
    o42_value_clear (&v);
  }

  if (start < 1 || count < 0)
    { g_free (old); g_free (with); return o42_value_error (O42_ERR_VALUE); }

  length = g_utf8_strlen (old, -1);
  if (start - 1 > length) start = length + 1;
  if (start - 1 + count > length) count = length - (start - 1);

  out = g_string_new (NULL);
  g_string_append_len (out, old,
                       g_utf8_offset_to_pointer (old, (glong) start - 1) - old);
  g_string_append (out, with);
  g_string_append (out, g_utf8_offset_to_pointer (old, (glong) (start - 1 + count)));

  g_free (old);
  g_free (with);
  return o42_value_take (g_string_free (out, FALSE));
}

static O42Value
fn_fixed (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, decimals = 2;
  gboolean no_commas = FALSE;

  ARG_NUMBER (0, x);
  if (n >= 2) ARG_NUMBER (1, decimals);
  if (n >= 3)
    {
      O42Value b = operand_value (ctx, &args[2]);
      O42ErrorCode err = O42_ERR_VALUE;
      gboolean ok = o42_value_to_bool (&b, &no_commas, &err);
      o42_value_clear (&b);
      if (!ok) return o42_value_error (err);
    }

  /* Negative decimals round to the left of the point. */
  if (decimals < 0)
    {
      x = round_half_away (x, (int) decimals);
      decimals = 0;
    }

  return o42_value_take (o42_number_format (x, no_commas ? O42_NUM_FIXED
                                                         : O42_NUM_COMMA,
                                            (int) decimals));
}

static O42Value
fn_dollar (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, decimals = 2;

  ARG_NUMBER (0, x);
  if (n >= 2) ARG_NUMBER (1, decimals);

  if (decimals < 0)
    {
      x = round_half_away (x, (int) decimals);
      decimals = 0;
    }

  return o42_value_take (o42_number_format (x, O42_NUM_CURRENCY, (int) decimals));
}

/* TEXT(value, format) with the formats people actually type: "0", "0.00",
 * "#,##0.00", "0%", "0.00E+00", "yyyy-mm-dd", "hh:mm:ss".  The full
 * format language is on the roadmap; until then the format string is
 * read for its shape rather than obeyed letter by letter. */
static O42Value
fn_text (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v = operand_value (ctx, &args[0]);
  char *format = NULL;
  char *result;

  (void) n;

  if (v.type == O42_VALUE_ERROR)
    return v;

  {
    O42Value f = operand_value (ctx, &args[1]);
    if (f.type == O42_VALUE_ERROR)
      { O42ErrorCode e = f.as.error; o42_value_clear (&f); o42_value_clear (&v); return o42_value_error (e); }
    format = o42_value_to_text (&f);
    o42_value_clear (&f);
  }

  if (v.type != O42_VALUE_NUMBER)
    {
      char *text = o42_value_to_text (&v);
      char *shown = o42_format_string (format, 0, text);
      o42_value_clear (&v);
      g_free (format);
      g_free (text);
      return o42_value_take (shown);
    }

  /* General, and the presets by their plain spellings, go the preset way
   * so that TEXT(x, "0.00") and a cell formatted Fixed agree to the digit;
   * anything else is the format language. */
  if (g_ascii_strcasecmp (format, "General") == 0)
    result = o42_number_format (v.as.number, O42_NUM_GENERAL, 0);
  else
    result = o42_format_string (format, v.as.number, NULL);
  o42_value_clear (&v);
  g_free (format);
  return o42_value_take (result);
}

const O42Function O42_FUNCS_TEXT[] = {
  { "COLUMNS", 1, 1, fn_columns },
  { "CONCAT", 1, -1, fn_concatenate },
  { "CONCATENATE", 1, -1, fn_concatenate },
  { "FIND", 2, 3, fn_find },
  { "LEFT", 1, 2, fn_left },
  { "LEN", 1, 1, fn_len },
  { "LOWER", 1, 1, fn_lower },
  { "MID", 3, 3, fn_mid },
  { "REPT", 2, 2, fn_rept },
  { "RIGHT", 1, 2, fn_right },
  { "ROWS", 1, 1, fn_rows },
  { "SUBSTITUTE", 3, 3, fn_substitute },
  { "TRIM", 1, 1, fn_trim },
  { "UPPER", 1, 1, fn_upper },
  { "VALUE", 1, 1, fn_value },
  { "CHAR", 1, 1, fn_char },
  { "CLEAN", 1, 1, fn_clean },
  { "CODE", 1, 1, fn_code },
  { "DOLLAR", 1, 2, fn_dollar },
  { "EXACT", 2, 2, fn_exact },
  { "FIXED", 1, 3, fn_fixed },
  { "PROPER", 1, 1, fn_proper },
  { "REPLACE", 4, 4, fn_replace },
  { "SEARCH", 2, 3, fn_search },
  { "TEXT", 2, 2, fn_text },
  { NULL, 0, 0, NULL }
};

const O42FunctionHelp O42_HELP_TEXT[] = {
  { "COLUMNS", "COLUMNS(range)", "How many columns a range spans." },
  { "CONCAT", "CONCAT(text1, text2, ...)", "Joins pieces of text into one; Excel's newer name for CONCATENATE." },
  { "CONCATENATE", "CONCATENATE(text1, text2, ...)", "Joins pieces of text into one." },
  { "FIND", "FIND(find_text, within_text, start)", "Where one text starts inside another; case matters." },
  { "LEFT", "LEFT(text, count)", "The first characters of a text." },
  { "LEN", "LEN(text)", "How many characters a text has." },
  { "LOWER", "LOWER(text)", "Text in lower case." },
  { "MID", "MID(text, start, count)", "Characters from the middle of a text." },
  { "REPT", "REPT(text, count)", "A text repeated." },
  { "RIGHT", "RIGHT(text, count)", "The last characters of a text." },
  { "ROWS", "ROWS(range)", "How many rows a range spans." },
  { "SUBSTITUTE", "SUBSTITUTE(text, old_text, new_text)", "Replaces text by content." },
  { "TRIM", "TRIM(text)", "Text without leading and trailing spaces." },
  { "UPPER", "UPPER(text)", "Text in upper case." },
  { "VALUE", "VALUE(text)", "Text read as a number." },
  { "CHAR", "CHAR(number)", "The character with a given code." },
  { "CLEAN", "CLEAN(text)", "Removes the characters that cannot be printed." },
  { "CODE", "CODE(text)", "The code of the first character." },
  { "DOLLAR", "DOLLAR(number, decimals)", "A number as currency text." },
  { "EXACT", "EXACT(text1, text2)", "TRUE if two texts are identical, case included." },
  { "FIXED", "FIXED(number, decimals, no_commas)", "A number as text with a fixed number of decimals." },
  { "PROPER", "PROPER(text)", "Text with each word capitalised." },
  { "REPLACE", "REPLACE(old_text, start, count, new_text)", "Replaces part of a text by position." },
  { "SEARCH", "SEARCH(find_text, within_text, start)", "Where one text starts inside another; case ignored, wildcards allowed." },
  { "TEXT", "TEXT(value, format)", "A number as text in a format such as \"#,##0.00\"." },
  { NULL, NULL, NULL }
};
