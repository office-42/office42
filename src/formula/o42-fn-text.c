/* o42-fn-text.c - see o42-eval-private.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-eval-private.h"

#include "o42-date.h"
#include "o42-numfmt.h"

#include <glib/gi18n.h>
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

/* The upper or lower case of each character on its own, which is how
 * Excel's UPPER and LOWER work: no ß to SS, no final sigma. */
static char *
case_by_char (const char *s, gboolean upper)
{
  GString *out = g_string_new (NULL);

  for (const char *p = s; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      g_string_append_unichar (out, upper ? g_unichar_toupper (c) : g_unichar_tolower (c));
    }
  return g_string_free (out, FALSE);
}

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
  /* Character by character, as Excel does it: ß stays ß rather than
   * becoming SS, and a final sigma lowers to the plain one. */
  result = case_by_char (s, TRUE);
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
  result = case_by_char (s, FALSE);
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

  /* The spaces at the ends go, and a run of spaces inside becomes one:
   * that is what TRIM has meant since Lotus, and it is why it is used
   * on text pasted from somewhere with columns lined up by spacing. */
  {
    GString *out = g_string_new (NULL);
    gboolean pending = FALSE;

    for (const char *p = s; *p != '\0'; p++)
      {
        if (*p == ' ')
          pending = TRUE;
        else
          {
            if (pending && out->len > 0)
              g_string_append_c (out, ' ');
            pending = FALSE;
            g_string_append_c (out, *p);
          }
      }
    result = g_string_free (out, FALSE);
  }
  g_free (s);
  return o42_value_take (result);
}

static O42Value
fn_concatenate (O42EvalContext *ctx, O42Operand *args, int n)
{
  GString *out = g_string_new (NULL);

  for (int i = 0; i < n; i++)
    {
      int rows, cols;

      /* CONCAT takes a range or an array whole, cell after cell. */
      o42_operand_dims (&args[i], &rows, &cols);
      for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
          {
            O42Value v;
            char *text;

            o42_operand_cell (ctx, &args[i], r, c, &v);
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

  /* Excel's cell holds 32,767 characters; more than that is #VALUE!. */
  if (count < 0 || (double) g_utf8_strlen (s, -1) * floor (count) > 32767)
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
  double instance = 0;
  char *result;

  ARG_TEXT (0, hay);
  ARG_TEXT (1, needle);
  ARG_TEXT (2, with);
  if (n >= 4)
    {
      O42Value v = operand_value (ctx, &args[3]);
      O42ErrorCode err = O42_ERR_VALUE;
      gboolean ok = o42_value_to_number (&v, &instance, &err);
      o42_value_clear (&v);
      if (!ok || instance < 1)
        { g_free (hay); g_free (needle); g_free (with); return o42_value_error (ok ? O42_ERR_VALUE : err); }
      instance = floor (instance);
    }

  if (*needle == '\0')
    { g_free (needle); g_free (with); return o42_value_take (hay); }

  /* Every occurrence, or only the nth when an instance is asked for. */
  {
    GString *out = g_string_new (NULL);
    size_t nlen = strlen (needle);
    const char *p = hay;
    int seen = 0;

    for (;;)
      {
        const char *found = strstr (p, needle);

        if (found == NULL)
          break;
        seen++;
        g_string_append_len (out, p, found - p);
        if (instance == 0 || seen == (int) instance)
          g_string_append (out, with);
        else
          g_string_append (out, needle);
        p = found + nlen;
      }
    g_string_append (out, p);
    result = g_string_free (out, FALSE);
  }

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
    {
      /* An error in place of the range is the answer; a value is one by one. */
      if (args[0].value.type == O42_VALUE_ERROR)
        return o42_value_copy (&args[0].value);
      return o42_value_number (1);
    }

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
  /* CHAR stops at 255, as Excel's does; UNICHAR goes on. */
  if (code < 1 || code > 255)
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
glob_match (const char *pattern, const char *text, gboolean whole)
{
  for (;;)
    {
      gunichar pc, tc;

      if (*pattern == '\0')
        return whole ? *text == '\0' : TRUE;

      pc = g_utf8_get_char (pattern);

      /* ~* and ~? are the characters themselves, and ~~ a tilde. */
      if (pc == '~' && pattern[1] != '\0')
        {
          pattern = g_utf8_next_char (pattern);
          pc = g_utf8_get_char (pattern);
          if (*text == '\0' || g_utf8_get_char (text) != pc)
            return FALSE;
          pattern = g_utf8_next_char (pattern);
          text = g_utf8_next_char (text);
          continue;
        }

      if (pc == '*')
        {
          const char *rest = g_utf8_next_char (pattern);
          for (const char *t = text; ; t = g_utf8_next_char (t))
            {
              if (glob_match (rest, t, whole))
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

static gboolean
glob_matches_prefix (const char *pattern, const char *text)
{
  return glob_match (pattern, text, FALSE);
}

/* The whole of a text against a wildcard pattern, both already
 * case-folded: what COUNTIF, MATCH and VLOOKUP mean by a match. */
gboolean
o42_glob_matches (const char *pattern, const char *text)
{
  return glob_match (pattern, text, TRUE);
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

  /* Excel's DOLLAR uses the currency format with negatives in
   * parentheses: DOLLAR(-1234.567, -2) is "($1,200)". */
  if (x < 0)
    {
      char *inner = o42_number_format (-x, O42_NUM_CURRENCY, (int) decimals);
      char *wrapped = g_strdup_printf ("(%s)", inner);
      g_free (inner);
      return o42_value_take (wrapped);
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

  if (v.type == O42_VALUE_TEXT)
    {
      /* Text that reads as a number or a date -- "2024-03-15" -- is that
       * number, as Excel takes it; other text passes through. */
      double as_number = 0;
      O42ErrorCode e = O42_ERR_VALUE;

      if (o42_value_to_number (&v, &as_number, &e))
        {
          o42_value_clear (&v);
          v = o42_value_number (as_number);
        }
    }
  if (v.type == O42_VALUE_BOOL)
    {
      /* TRUE and FALSE are the words. */
      char *word = g_strdup (v.as.boolean ? "TRUE" : "FALSE");
      o42_value_clear (&v);
      g_free (format);
      return o42_value_take (word);
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

  /* General shows every digit a double has, fifteen of them, as TEXT
   * does; the presets by their plain spellings go the preset way so
   * that TEXT(x, "0.00") and a cell formatted Fixed agree to the digit;
   * anything else is the format language. */
  if (g_ascii_strcasecmp (format, "General") == 0)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      result = g_strdup (g_ascii_formatd (buf, sizeof buf, "%.15G", v.as.number));
    }
  else
    result = o42_format_string (format, v.as.number, NULL);
  o42_value_clear (&v);
  g_free (format);
  return o42_value_take (result);
}

/* ---- The byte forms, the widths, the regular expressions ------------- */

/* Excel's ...B functions count bytes in a double-byte language and
 * characters elsewhere; here a character is a character, so each is
 * its plain twin. */

/* Fullwidth to halfwidth (ASC) and back (JIS, DBCS): the Latin letters,
 * digits and marks at U+FF01-FF5E stand in for U+0021-007E, and the
 * ideographic space for the space. */
static O42Value
width_convert (O42EvalContext *ctx, O42Operand *args, gboolean to_full)
{
  char *text = NULL;
  GString *out;

  ARG_TEXT (0, text);
  out = g_string_new (NULL);
  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (to_full && c >= 0x21 && c <= 0x7E) c += 0xFEE0;
      else if (to_full && c == ' ') c = 0x3000;
      else if (!to_full && c >= 0xFF01 && c <= 0xFF5E) c -= 0xFEE0;
      else if (!to_full && c == 0x3000) c = ' ';
      g_string_append_unichar (out, c);
    }
  g_free (text);
  return o42_value_take (g_string_free (out, FALSE));
}

static O42Value fn_jis (O42EvalContext *ctx, O42Operand *args, int n) { (void) n; return width_convert (ctx, args, TRUE); }
static O42Value fn_asc_width (O42EvalContext *ctx, O42Operand *args, int n) { (void) n; return width_convert (ctx, args, FALSE); }

/* A regular expression as GRegex reads it, which is Perl's, as Excel's
 * are; `flags` from the case argument (0 matters, 1 does not). */
static GRegex *
regex_of (const char *pattern, double case_mode, O42ErrorCode *error)
{
  GError *err = NULL;
  GRegex *re = g_regex_new (pattern, case_mode == 1 ? G_REGEX_CASELESS : 0, 0, &err);

  if (re == NULL)
    {
      g_clear_error (&err);
      *error = O42_ERR_VALUE;
    }
  return re;
}

static O42Value
fn_regextest (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *text = NULL, *pattern = NULL;
  double case_mode = 0;
  O42ErrorCode error = O42_ERR_VALUE;
  GRegex *re;
  gboolean hit;

  ARG_TEXT (0, text);
  ARG_TEXT (1, pattern);
  if (n >= 3) ARG_NUMBER (2, case_mode);
  re = regex_of (pattern, case_mode, &error);
  g_free (pattern);
  if (re == NULL)
    { g_free (text); return o42_value_error (error); }
  hit = g_regex_match (re, text, 0, NULL);
  g_regex_unref (re);
  g_free (text);
  return o42_value_bool (hit);
}

static O42Value
fn_regexreplace (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *text = NULL, *pattern = NULL, *replacement = NULL, *result;
  double occurrence = 0, case_mode = 0;
  O42ErrorCode error = O42_ERR_VALUE;
  GRegex *re;
  GError *err = NULL;

  ARG_TEXT (0, text);
  ARG_TEXT (1, pattern);
  ARG_TEXT (2, replacement);
  if (n >= 4) ARG_NUMBER (3, occurrence);
  if (n >= 5) ARG_NUMBER (4, case_mode);
  re = regex_of (pattern, case_mode, &error);
  g_free (pattern);
  if (re == NULL)
    { g_free (text); g_free (replacement); return o42_value_error (error); }

  if (occurrence == 0)
    result = g_regex_replace (re, text, -1, 0, replacement, 0, &err);
  else
    {
      /* Only the nth match (from the end when negative): the others
       * are left as they were. */
      GMatchInfo *info = NULL;
      GArray *starts = g_array_new (FALSE, FALSE, sizeof (int)), *ends = g_array_new (FALSE, FALSE, sizeof (int));
      int which;

      g_regex_match (re, text, 0, &info);
      while (g_match_info_matches (info))
        {
          int s, e;
          g_match_info_fetch_pos (info, 0, &s, &e);
          g_array_append_val (starts, s);
          g_array_append_val (ends, e);
          g_match_info_next (info, NULL);
        }
      g_match_info_free (info);
      which = occurrence > 0 ? (int) occurrence - 1 : (int) starts->len + (int) occurrence;
      if (which < 0 || which >= (int) starts->len)
        result = g_strdup (text);
      else
        {
          int s = g_array_index (starts, int, which), e = g_array_index (ends, int, which);
          char *piece = g_strndup (text + s, e - s);
          char *replaced = g_regex_replace (re, piece, -1, 0, replacement, 0, &err);
          result = g_strdup_printf ("%.*s%s%s", s, text, replaced != NULL ? replaced : piece, text + e);
          g_free (replaced);
          g_free (piece);
        }
      g_array_free (starts, TRUE);
      g_array_free (ends, TRUE);
    }
  g_regex_unref (re);
  g_free (text);
  g_free (replacement);
  if (result == NULL)
    { g_clear_error (&err); return o42_value_error (O42_ERR_VALUE); }
  return o42_value_take (result);
}

/* PERCENTOF(subset, all): the sum of one over the sum of the other. */
static O42Value
fn_percentof (O42EvalContext *ctx, O42Operand *args, int n)
{
  Accum part, whole;
  O42ErrorCode error = O42_ERR_VALUE;

  (void) n;
  o42_accum_init (&part, FALSE);
  o42_accum_init (&whole, FALSE);
  if (!o42_visit_numbers (ctx, &args[0], 1, o42_accumulate, &part, &error) ||
      !o42_visit_numbers (ctx, &args[1], 1, o42_accumulate, &whole, &error))
    return o42_value_error (error);
  if (whole.sum == 0)
    return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (part.sum / whole.sum);
}

/* ENCODEURL: everything but the unreserved characters as %XX, in UTF-8. */
static O42Value
fn_encodeurl (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *text = NULL;
  GString *out;

  (void) n;
  ARG_TEXT (0, text);
  out = g_string_new (NULL);
  for (const guchar *p = (const guchar *) text; *p != '\0'; p++)
    {
      if (g_ascii_isalnum (*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~')
        g_string_append_c (out, (char) *p);
      else
        g_string_append_printf (out, "%%%02X", *p);
    }
  g_free (text);
  return o42_value_take (g_string_free (out, FALSE));
}

const O42Function O42_FUNCS_TEXT[] = {
  { "LENB", 1, 1, fn_len },
  { "LEFTB", 1, 2, fn_left },
  { "RIGHTB", 1, 2, fn_right },
  { "MIDB", 3, 3, fn_mid },
  { "FINDB", 2, 3, fn_find },
  { "SEARCHB", 2, 3, fn_search },
  { "REPLACEB", 4, 4, fn_replace },
  { "JIS", 1, 1, fn_jis },
  { "DBCS", 1, 1, fn_jis },
  { "ASC", 1, 1, fn_asc_width },
  { "REGEXTEST", 2, 3, fn_regextest },
  { "REGEXREPLACE", 3, 5, fn_regexreplace },
  { "PERCENTOF", 2, 2, fn_percentof },
  { "ENCODEURL", 1, 1, fn_encodeurl },
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
  { "SUBSTITUTE", 3, 4, fn_substitute },
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
  { "LENB", "LENB(text)", N_("How many characters a text has; the same as LEN here.") },
  { "LEFTB", "LEFTB(text, count)", N_("The first characters of a text; the same as LEFT here.") },
  { "RIGHTB", "RIGHTB(text, count)", N_("The last characters of a text; the same as RIGHT here.") },
  { "MIDB", "MIDB(text, start, count)", N_("Characters from the middle of a text; the same as MID here.") },
  { "FINDB", "FINDB(find_text, within_text, start)", N_("Where one text starts inside another; the same as FIND here.") },
  { "SEARCHB", "SEARCHB(find_text, within_text, start)", N_("Where one text starts inside another; the same as SEARCH here.") },
  { "REPLACEB", "REPLACEB(old_text, start, count, new_text)", N_("Replaces part of a text by position; the same as REPLACE here.") },
  { "JIS", "JIS(text)", N_("Halfwidth letters, digits and marks made fullwidth.") },
  { "DBCS", "DBCS(text)", N_("Halfwidth letters, digits and marks made fullwidth; Excel's newer name for JIS.") },
  { "ASC", "ASC(text)", N_("Fullwidth letters, digits and marks made halfwidth.") },
  { "REGEXTEST", "REGEXTEST(text, pattern, [case])", N_("Whether a regular expression matches somewhere in the text.") },
  { "REGEXREPLACE", "REGEXREPLACE(text, pattern, replacement, [occurrence], [case])", N_("The text with what a regular expression matches replaced; \\1 for a group.") },
  { "PERCENTOF", "PERCENTOF(subset, all)", N_("The sum of a subset as a share of the sum of the whole.") },
  /* xgettext:no-c-format */
  { "ENCODEURL", "ENCODEURL(text)", N_("The text as it goes into a URL, %XX for what is not a letter or digit.") },
  { "COLUMNS", "COLUMNS(range)", N_("How many columns a range spans.") },
  { "CONCAT", "CONCAT(text1, text2, ...)", N_("Joins pieces of text into one; Excel's newer name for CONCATENATE.") },
  { "CONCATENATE", "CONCATENATE(text1, text2, ...)", N_("Joins pieces of text into one.") },
  { "FIND", "FIND(find_text, within_text, start)", N_("Where one text starts inside another; case matters.") },
  { "LEFT", "LEFT(text, count)", N_("The first characters of a text.") },
  { "LEN", "LEN(text)", N_("How many characters a text has.") },
  { "LOWER", "LOWER(text)", N_("Text in lower case.") },
  { "MID", "MID(text, start, count)", N_("Characters from the middle of a text.") },
  { "REPT", "REPT(text, count)", N_("A text repeated.") },
  { "RIGHT", "RIGHT(text, count)", N_("The last characters of a text.") },
  { "ROWS", "ROWS(range)", N_("How many rows a range spans.") },
  { "SUBSTITUTE", "SUBSTITUTE(text, old_text, new_text, [instance])", N_("Replaces old_text with new_text, every time or only the nth.") },
  { "TRIM", "TRIM(text)", N_("Text without spaces at the ends, and one between words.") },
  { "UPPER", "UPPER(text)", N_("Text in upper case.") },
  { "VALUE", "VALUE(text)", N_("Text read as a number.") },
  { "CHAR", "CHAR(number)", N_("The character with a given code.") },
  { "CLEAN", "CLEAN(text)", N_("Removes the characters that cannot be printed.") },
  { "CODE", "CODE(text)", N_("The code of the first character.") },
  { "DOLLAR", "DOLLAR(number, decimals)", N_("A number as currency text.") },
  { "EXACT", "EXACT(text1, text2)", N_("TRUE if two texts are identical, case included.") },
  { "FIXED", "FIXED(number, decimals, no_commas)", N_("A number as text with a fixed number of decimals.") },
  { "PROPER", "PROPER(text)", N_("Text with each word capitalised.") },
  { "REPLACE", "REPLACE(old_text, start, count, new_text)", N_("Replaces part of a text by position.") },
  { "SEARCH", "SEARCH(find_text, within_text, start)", N_("Where one text starts inside another; case ignored, wildcards allowed.") },
  { "TEXT", "TEXT(value, format)", N_("A number as text in a format such as \"#,##0.00\".") },
  { NULL, NULL, NULL }
};
