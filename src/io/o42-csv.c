/* o42-csv.c - see o42-csv.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-csv.h"

#include "o42-entry.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

const char *const O42_DATE_ORDER_NAMES[] = { "MDY", "DMY", "YMD", "MYD", "DYM", "YDM", NULL };

gboolean o42_csv_lost_characters = FALSE;

/* ---- Options ------------------------------------------------------------ */

O42CsvOptions *
o42_csv_options_new (void)
{
  O42CsvOptions *options = g_new0 (O42CsvOptions, 1);

  options->separators = g_strdup (",");
  options->quote = '"';
  options->breaks = g_array_new (FALSE, FALSE, sizeof (int));
  options->columns = g_array_new (FALSE, TRUE, sizeof (O42CsvColumn));
  options->decimal = '.';
  options->thousands = ',';
  options->trailing_minus = TRUE;
  options->crlf = TRUE;
  options->as_shown = TRUE;
  return options;
}

O42CsvOptions *
o42_csv_options_copy (const O42CsvOptions *options)
{
  O42CsvOptions *copy;

  g_return_val_if_fail (options != NULL, NULL);

  copy = g_new (O42CsvOptions, 1);
  *copy = *options;
  copy->encoding = g_strdup (options->encoding);
  copy->separators = g_strdup (options->separators);
  copy->breaks = g_array_copy (options->breaks);
  copy->columns = g_array_copy (options->columns);
  return copy;
}

void
o42_csv_options_free (O42CsvOptions *options)
{
  if (options == NULL)
    return;
  g_free (options->encoding);
  g_free (options->separators);
  g_array_unref (options->breaks);
  g_array_unref (options->columns);
  g_free (options);
}

static gboolean
name_is_prn (const char *name)
{
  char *folded = name != NULL ? g_ascii_strdown (name, -1) : NULL;
  gboolean prn = folded != NULL && g_str_has_suffix (folded, ".prn");
  g_free (folded);
  return prn;
}

/* Is the name one Excel writes with tabs between the fields: .txt,
 * .tsv or .tab rather than .csv? */
static gboolean
name_is_tabbed (const char *name)
{
  char *folded = name != NULL ? g_ascii_strdown (name, -1) : NULL;
  gboolean tabbed = folded != NULL && (g_str_has_suffix (folded, ".txt") || g_str_has_suffix (folded, ".tsv") ||
                                       g_str_has_suffix (folded, ".tab"));
  g_free (folded);
  return tabbed;
}

O42CsvOptions *
o42_csv_options_for_file (GFile *file)
{
  O42CsvOptions *options = o42_csv_options_new ();
  char *name = file != NULL ? g_file_get_basename (file) : NULL;

  if (name_is_tabbed (name))
    o42_csv_options_set_separators (options, "\t");
  /* Excel's "Formatted Text (Space delimited)": columns padded out. */
  options->fixed_width = name_is_prn (name);
  g_free (name);
  return options;
}

void
o42_csv_options_set_separators (O42CsvOptions *options, const char *separators)
{
  g_return_if_fail (options != NULL);
  g_free (options->separators);
  options->separators = g_strdup (separators != NULL ? separators : "");
}

void
o42_csv_options_set_encoding (O42CsvOptions *options, const char *encoding)
{
  g_return_if_fail (options != NULL);
  g_free (options->encoding);
  options->encoding = encoding != NULL && *encoding != '\0' ? g_strdup (encoding) : NULL;
}

O42CsvColumn
o42_csv_options_column (const O42CsvOptions *options, int col)
{
  O42CsvColumn general = { O42_SPLIT_GENERAL, O42_DATE_MDY };

  if (options == NULL || col < 0 || (guint) col >= options->columns->len)
    return general;
  return g_array_index (options->columns, O42CsvColumn, col);
}

void
o42_csv_options_set_column (O42CsvOptions *options, int col,
                            O42SplitType type, O42DateOrder order)
{
  O42CsvColumn *column;

  g_return_if_fail (options != NULL);
  if (col < 0 || col >= O42_MAX_COLS)
    return;
  if ((guint) col >= options->columns->len)
    g_array_set_size (options->columns, col + 1);   /* cleared: General */
  column = &g_array_index (options->columns, O42CsvColumn, col);
  column->type = type;
  column->order = order;
}

/* ---- Bytes to text ------------------------------------------------------ */

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

/* The bytes in `from` made UTF-8, a character the encoding cannot read
 * becoming U+FFFD and the reading going on after it: a file with one
 * bad byte in a million is a file to open, not one to refuse.  NULL
 * when iconv does not know the encoding at all. */
static char *
convert_forgiving (const char *bytes, gsize length, const char *from)
{
  GIConv cd = g_iconv_open ("UTF-8", from);
  GString *out;
  char buffer[8192];
  char *in = (char *) bytes;
  gsize in_left = length;
  gsize unit = g_ascii_strncasecmp (from, "UTF-16", 6) == 0 ? 2 : 1;

  if (cd == (GIConv) -1)
    return NULL;

  out = g_string_sized_new (length + length / 4 + 16);
  while (in_left > 0)
    {
      char *o = buffer;
      gsize o_left = sizeof buffer;
      gsize done = g_iconv (cd, &in, &in_left, &o, &o_left);
      int why = errno;

      g_string_append_len (out, buffer, o - buffer);
      if (done != (gsize) -1)
        break;
      if (why == E2BIG)
        continue;
      /* A byte that means nothing here, or a character cut off at the
       * end: the replacement character, and on. */
      g_string_append (out, "\357\277\275");
      if (why != EILSEQ || in_left < unit)
        break;
      in += unit;
      in_left -= unit;
      g_iconv (cd, NULL, NULL, NULL, NULL);
    }
  {
    char *o = buffer;
    gsize o_left = sizeof buffer;

    g_iconv (cd, NULL, NULL, &o, &o_left);
    g_string_append_len (out, buffer, o - buffer);
  }
  g_iconv_close (cd);

  /* A NUL in the middle would end the text for everything after it;
   * it is a space instead. */
  for (gsize i = 0; i < out->len; i++)
    if (out->str[i] == '\0')
      out->str[i] = ' ';
  return g_string_free (out, FALSE);
}

/* Whether the bytes read in the encoding without a single fault. */
static gboolean
converts_cleanly (const char *bytes, gsize length, const char *from)
{
  char *converted = g_convert (bytes, (gssize) length, "UTF-8", from, NULL, NULL, NULL);
  gboolean clean = converted != NULL;

  g_free (converted);
  return clean;
}

char *
o42_csv_decode (const char *bytes, gsize length, const char *encoding,
                const char **found, gboolean *bom)
{
  const char *from = encoding;
  gsize skip = 0;
  char *text = NULL;

  if (bom != NULL)
    *bom = FALSE;
  if (bytes == NULL)
    length = 0;

  /* A byte-order mark says what the file is, and is not part of it. */
  if (length >= 3 && memcmp (bytes, "\357\273\277", 3) == 0 &&
      (from == NULL || g_ascii_strcasecmp (from, "UTF-8") == 0))
    { from = "UTF-8"; skip = 3; }
  else if (length >= 2 && (guchar) bytes[0] == 0xFF && (guchar) bytes[1] == 0xFE &&
           (from == NULL || g_ascii_strncasecmp (from, "UTF-16", 6) == 0))
    { from = from != NULL && g_ascii_strcasecmp (from, "UTF-16BE") == 0 ? from : "UTF-16LE"; skip = 2; }
  else if (length >= 2 && (guchar) bytes[0] == 0xFE && (guchar) bytes[1] == 0xFF &&
           (from == NULL || g_ascii_strncasecmp (from, "UTF-16", 6) == 0))
    { from = from != NULL && g_ascii_strcasecmp (from, "UTF-16LE") == 0 ? from : "UTF-16BE"; skip = 2; }
  if (skip > 0 && bom != NULL)
    *bom = TRUE;

  /* UTF-16 with no mark, which Excel's "Unicode Text" can be: by the
   * NULs between the letters. */
  if (from == NULL && length >= 4 && bytes[0] != '\0' && bytes[1] == '\0' && bytes[3] == '\0')
    from = "UTF-16LE";
  else if (from == NULL && length >= 4 && bytes[0] == '\0' && bytes[2] == '\0' && bytes[1] != '\0')
    from = "UTF-16BE";

  /* Otherwise UTF-8 when it reads as UTF-8 -- a NUL aside, which is
   * noise and not a reason to call the file something else -- then the
   * machine's own code page, then Windows-1252, which is what an
   * unmarked file from Windows nearly always is. */
  if (from == NULL)
    {
      const char *p = bytes, *end = bytes + length;
      gboolean utf8 = TRUE;

      while (p < end && utf8)
        {
          const char *stop = NULL;

          if (g_utf8_validate (p, end - p, &stop))
            break;
          if (stop < end && *stop == '\0')
            p = stop + 1;
          else
            utf8 = FALSE;
        }
      if (utf8)
        from = "UTF-8";
      else
        {
          const char *local = NULL;

          if (!g_get_charset (&local) && local != NULL && converts_cleanly (bytes, length, local))
            from = local;
          else
            from = "WINDOWS-1252";
        }
    }

  if (g_ascii_strcasecmp (from, "UTF-8") == 0)
    {
      gsize n = length - skip;
      char *copy = g_malloc (n + 1);

      memcpy (copy, bytes + skip, n);
      copy[n] = '\0';
      for (gsize i = 0; i < n; i++)
        if (copy[i] == '\0')
          copy[i] = ' ';
      text = g_utf8_make_valid (copy, (gssize) n);
      g_free (copy);
    }
  else
    {
      text = convert_forgiving (bytes + skip, length - skip, from);
      if (text == NULL)
        {
          /* An encoding this machine's iconv does not have: the bytes
           * as the forgiving UTF-8 reading gives them. */
          text = o42_csv_decode (bytes + skip, length - skip, "UTF-8", NULL, NULL);
          from = "UTF-8";
        }
    }

  if (found != NULL)
    *found = g_intern_string (from);
  return text;
}

/* ---- Lines and records -------------------------------------------------- */

/* Past the end of the line `p` is on: LF, CR LF, or a lone CR as an old
 * Macintosh ends one. */
static const char *
next_line (const char *p)
{
  while (*p != '\0' && *p != '\n' && *p != '\r')
    p++;
  if (*p == '\r')
    p++;
  if (*p == '\n')
    p++;
  return p;
}

static const char *
skip_lines (const char *text, int n)
{
  const char *p = text;

  for (int i = 0; i < n && *p != '\0'; i++)
    p = next_line (p);
  return p;
}

int
o42_csv_count_lines (const char *text)
{
  const char *p = text;
  int n = 0;

  g_return_val_if_fail (text != NULL, 0);
  while (*p != '\0' && n < G_MAXINT)
    {
      p = next_line (p);
      n++;
    }
  return n;
}

GPtrArray *
o42_csv_lines (const char *text, int max_lines)
{
  GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);
  const char *p = text;

  g_return_val_if_fail (text != NULL, lines);
  while (*p != '\0' && (int) lines->len < max_lines)
    {
      const char *end = p;

      while (*end != '\0' && *end != '\n' && *end != '\r')
        end++;
      g_ptr_array_add (lines, g_strndup (p, end - p));
      p = next_line (p);
    }
  return lines;
}

typedef struct {
  const char          *p;          /* where the next record starts */
  const O42CsvOptions *options;
  guint8               ascii[128]; /* which ASCII characters separate */
  char               **wide;       /* the separators beyond ASCII, in UTF-8 */
  GArray              *breaks;     /* the fixed-width breaks, put in order */
  GString             *field;
} Reader;

static void
reader_init (Reader *r, const char *text, const O42CsvOptions *options)
{
  GPtrArray *wide = g_ptr_array_new ();
  const char *s = options->separators != NULL ? options->separators : "";

  memset (r, 0, sizeof *r);
  r->options = options;
  r->p = skip_lines (text, MAX (options->start_row, 0));
  for (; *s != '\0'; s = g_utf8_next_char (s))
    {
      if ((guchar) *s >= 0x80)
        g_ptr_array_add (wide, g_strndup (s, g_utf8_next_char (s) - s));
      else if (*s != '\n' && *s != '\r' && *s != options->quote)
        r->ascii[(guchar) *s] = 1;
    }
  g_ptr_array_add (wide, NULL);
  r->wide = (char **) g_ptr_array_free (wide, FALSE);

  /* Ascending and past the start, whatever the caller left in them. */
  r->breaks = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; i < options->breaks->len; i++)
    {
      int b = g_array_index (options->breaks, int, i);

      if (b > 0 && (r->breaks->len == 0 || b > g_array_index (r->breaks, int, r->breaks->len - 1)))
        g_array_append_val (r->breaks, b);
    }
  r->field = g_string_new (NULL);
}

static void
reader_clear (Reader *r)
{
  g_strfreev (r->wide);
  g_array_unref (r->breaks);
  g_string_free (r->field, TRUE);
}

/* How many bytes the separator at `s` has, or 0 when there is none. */
static inline gsize
separator_at (const Reader *r, const char *s)
{
  guchar c = (guchar) *s;

  if (c < 0x80)
    return r->ascii[c];
  for (char **w = r->wide; *w != NULL; w++)
    {
      gsize n = strlen (*w);

      if (strncmp (s, *w, n) == 0)
        return n;
    }
  return 0;
}

static inline gboolean
line_end (char c)
{
  return c == '\0' || c == '\n' || c == '\r';
}

/* One record of a delimited file into `fields`: RFC 4180's, with the
 * qualifier and separators the options say.  A qualifier opens a quoted
 * field only at the field's start, as Excel reads it, and a quoted field
 * may run over several lines. */
static gboolean
read_delimited (Reader *r, GPtrArray *fields)
{
  const char *s = r->p;
  char q = r->options->quote;
  gboolean merge = r->options->merge_separators;
  gsize n;

  g_ptr_array_set_size (fields, 0);
  if (*s == '\0')
    return FALSE;

  /* Spaces merged are an indent, not an empty first column. */
  if (merge)
    while ((*s == ' ' || *s == '\t') && (n = separator_at (r, s)) > 0)
      s += n;

  for (;;)
    {
      GString *f = r->field;

      g_string_truncate (f, 0);
      if (q != '\0' && *s == q)
        {
          s++;
          while (*s != '\0')
            {
              const char *start = s;

              if (*s == q)
                {
                  if (s[1] == q)
                    {
                      g_string_append_c (f, q);
                      s += 2;
                      continue;
                    }
                  s++;
                  break;
                }
              while (*s != '\0' && *s != q)
                s++;
              g_string_append_len (f, start, s - start);
            }

          /* Anything between the closing quote and the separator is a
           * malformed file; keeping it is kinder than dropping it. */
          while (!line_end (*s) && separator_at (r, s) == 0)
            g_string_append_c (f, *s++);
        }
      else
        {
          const char *start = s;

          while (!line_end (*s) && separator_at (r, s) == 0)
            s++;
          g_string_append_len (f, start, s - start);
        }
      g_ptr_array_add (fields, g_strndup (f->str, f->len));

      n = separator_at (r, s);
      if (n > 0)
        {
          s += n;
          if (merge)
            {
              while ((n = separator_at (r, s)) > 0)
                s += n;
              if (line_end (*s))
                break;
            }
          continue;
        }
      break;
    }

  if (*s == '\r')
    s++;
  if (*s == '\n')
    s++;
  r->p = s;
  return TRUE;
}

/* One line of a fixed-width file, cut at the breaks, each piece with
 * the spaces that padded it taken off. */
static gboolean
read_fixed (Reader *r, GPtrArray *fields)
{
  const char *s = r->p, *end = s, *at = s;
  glong position = 0;

  g_ptr_array_set_size (fields, 0);
  if (*s == '\0')
    return FALSE;

  while (!line_end (*end))
    end++;
  for (guint i = 0; i <= r->breaks->len; i++)
    {
      glong to = i < r->breaks->len ? g_array_index (r->breaks, int, i) : G_MAXLONG;
      const char *from = at;
      char *piece;

      while (at < end && position < to)
        {
          at = g_utf8_next_char (at);
          position++;
        }
      if (at > end)
        at = end;
      piece = g_strndup (from, at - from);
      g_ptr_array_add (fields, g_strstrip (piece));
    }

  r->p = next_line (end);
  return TRUE;
}

static gboolean
read_record (Reader *r, GPtrArray *fields)
{
  return r->options->fixed_width ? read_fixed (r, fields) : read_delimited (r, fields);
}

GPtrArray *
o42_csv_preview (const char *text, const O42CsvOptions *options, int max_rows)
{
  GPtrArray *rows = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
  GPtrArray *fields = g_ptr_array_new_with_free_func (g_free);
  Reader r;

  g_return_val_if_fail (text != NULL && options != NULL, rows);

  reader_init (&r, text, options);
  while ((int) rows->len < max_rows && read_record (&r, fields))
    {
      char **copy = g_new0 (char *, fields->len + 1);

      for (guint i = 0; i < fields->len; i++)
        copy[i] = g_strdup (g_ptr_array_index (fields, i));
      g_ptr_array_add (rows, copy);
    }
  reader_clear (&r);
  g_ptr_array_unref (fields);
  return rows;
}

/* ---- Guessing ----------------------------------------------------------- */

#define SNIFF_RECORDS 20

/* What a number's shape says of its decimal separator: 1 for a comma
 * (3,5  12,25  1.234,50), -1 for a point (3.5  1,234.50), 0 when it
 * could be either (1,234  1.234) or is no number at all. */
static int
decimal_evidence (const char *field)
{
  const char *p = field;
  int lead = 0, groups = 0, frac = 0;
  char group = '\0', mark = '\0';

  while (*p == ' ')
    p++;
  if (*p == '-' || *p == '+')
    p++;
  while (g_ascii_isdigit (*p))
    p++, lead++;
  if (lead == 0)
    return 0;
  /* Thousands, if the separator comes again, or before the other. */
  while ((*p == '.' || *p == ',') && g_ascii_isdigit (p[1]) && g_ascii_isdigit (p[2]) &&
         g_ascii_isdigit (p[3]) && !g_ascii_isdigit (p[4]) &&
         (p[4] == '.' || p[4] == ',') && (group == '\0' || group == *p))
    {
      group = *p;
      groups++;
      p += 4;
    }
  if (*p == '.' || *p == ',')
    {
      mark = *p++;
      while (g_ascii_isdigit (*p))
        p++, frac++;
    }
  while (*p == ' ' || *p == '%' || *p == '-')
    p++;
  if (*p != '\0' || mark == '\0' || frac == 0 || mark == group)
    return 0;
  /* Three figures after a lone mark could be a thousand either way. */
  if (frac == 3 && groups == 0 && lead <= 3)
    return 0;
  return mark == ',' ? 1 : -1;
}

void
o42_csv_sniff (const char *text, const char *name, O42CsvOptions *options)
{
  /* The separators a file is likely to have, in the order they are
   * preferred when two fit as well: a tab first for a .txt. */
  static const char CANDIDATES[] = { ',', ';', '\t', '|' };
  static const char TABBED[] = { '\t', ',', ';', '|' };
  const char *order = name_is_tabbed (name) ? TABBED : CANDIDATES;
  int counts[4][SNIFF_RECORDS];
  int n_records = 0, best = -1, best_consistent = 0;
  char *folded = name != NULL ? g_ascii_strdown (name, -1) : NULL;
  gboolean csv = folded != NULL && g_str_has_suffix (folded, ".csv");
  gboolean prn = name_is_prn (name);

  g_return_if_fail (text != NULL && options != NULL);
  g_free (folded);

  {
    const char *nl = strchr (text, '\n');

    options->crlf = nl == NULL || (nl > text && nl[-1] == '\r');
  }

  /* How many of each there are on each of the first records, outside
   * quotes. */
  {
    const char *s = text;
    gboolean in_quote = FALSE, any = FALSE;
    int c[4] = { 0, 0, 0, 0 };

    while (n_records < SNIFF_RECORDS)
      {
        char ch = *s;

        if (ch == '\0' || (!in_quote && (ch == '\n' || ch == '\r')))
          {
            if (any)
              {
                for (int k = 0; k < 4; k++)
                  counts[k][n_records] = c[k];
                n_records++;
              }
            memset (c, 0, sizeof c);
            any = FALSE;
            if (ch == '\0')
              break;
            s = next_line (s);
            continue;
          }
        if (ch == '"')
          in_quote = !in_quote;
        else if (!in_quote)
          for (int k = 0; k < 4; k++)
            if (ch == order[k])
              c[k]++;
        if (ch != ' ')
          any = TRUE;
        s++;
      }
  }

  /* The separator is the one that comes the same number of times on the
   * most records, and on half of them at least -- a comma in a quoted
   * name, or a semicolon in a note, comes and goes; the separator does
   * not. */
  for (int k = 0; k < 4; k++)
    {
      int mode = 0, mode_n = 0;

      for (int i = 0; i < n_records; i++)
        {
          int same = 0;

          if (counts[k][i] == 0)
            continue;
          for (int j = 0; j < n_records; j++)
            if (counts[k][j] == counts[k][i])
              same++;
          if (same > mode_n)
            {
              mode = counts[k][i];
              mode_n = same;
            }
        }
      if (mode > 0 && mode_n > best_consistent && mode_n * 2 >= n_records)
        {
          best = k;
          best_consistent = mode_n;
        }
    }

  options->quote = '"';
  options->fixed_width = FALSE;
  {
    char sep[2] = { best >= 0 ? order[best] : order[0], '\0' };
    o42_csv_options_set_separators (options, sep);
  }

  /* Where the columns would be cut if it is fixed width, which is what
   * a file with no separator in sight is taken for, unless its name
   * says it is comma-separated. */
  g_array_set_size (options->breaks, 0);
  {
    GPtrArray *lines = o42_csv_lines (skip_lines (text, MAX (options->start_row, 0)), SNIFF_RECORDS);
    GPtrArray *full = g_ptr_array_new ();

    for (guint i = 0; i < lines->len; i++)
      if (*(char *) g_ptr_array_index (lines, i) != '\0')
        g_ptr_array_add (full, g_ptr_array_index (lines, i));
    o42_guess_fixed_breaks ((const char *const *) full->pdata, full->len, options->breaks);
    g_ptr_array_unref (full);
    g_ptr_array_unref (lines);
  }
  if ((best < 0 || prn) && !csv && options->breaks->len > 0)
    options->fixed_width = TRUE;

  /* A decimal comma is the reason a file has semicolons, and 1.234,50
   * says so outright; a point is what is taken when nothing says. */
  options->decimal = '.';
  options->thousands = ',';
  {
    GPtrArray *rows = o42_csv_preview (text, options, SNIFF_RECORDS);
    int comma = 0, point = 0;

    for (guint i = 0; i < rows->len; i++)
      for (char **f = g_ptr_array_index (rows, i); *f != NULL; f++)
        {
          int says = decimal_evidence (*f);

          comma += says > 0;
          point += says < 0;
        }
    if (comma > point)
      {
        options->decimal = ',';
        options->thousands = '.';
      }
    g_ptr_array_unref (rows);
  }
}

char *
o42_csv_read_file (GFile *file, O42CsvOptions **options, GError **error)
{
  char *contents = NULL, *text, *name;
  gsize length = 0;
  const char *found = NULL;
  gboolean bom = FALSE;

  g_return_val_if_fail (G_IS_FILE (file), NULL);
  g_return_val_if_fail (options != NULL, NULL);

  *options = NULL;
  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return NULL;
  text = o42_csv_decode (contents, length, NULL, &found, &bom);
  g_free (contents);

  *options = o42_csv_options_new ();
  name = g_file_get_basename (file);
  o42_csv_sniff (text, name, *options);
  g_free (name);
  o42_csv_options_set_encoding (*options, found);
  (*options)->bom = bom;
  return text;
}

/* ---- Fields into cells -------------------------------------------------- */

/* A number written with the file's own decimal and thousands separators,
 * or with its minus sign behind it, rewritten the way the entry parser
 * reads one: "1.234,5" with a decimal comma is "1234.5", "12-" is "-12".
 * NULL when the field is not such a number, or needs no rewriting. */
static char *
number_in_file_form (const char *field, const O42CsvOptions *options)
{
  const char *p = field;
  GString *out;
  gboolean digits = FALSE, trailing = FALSE, grouped = FALSE, changed = FALSE;
  char dec = options->decimal != '\0' ? options->decimal : '.';
  char sep = options->thousands;

  while (*p == ' ')
    p++;
  out = g_string_new (NULL);
  while (*p == '(' || *p == '-' || *p == '+' || *p == '$' || *p == ' ')
    g_string_append_c (out, *p++);

  /* The whole part, its groups of three held to. */
  {
    int run = 0;

    for (;;)
      {
        if (g_ascii_isdigit (*p))
          {
            g_string_append_c (out, *p++);
            digits = TRUE;
            run++;
            continue;
          }
        if (sep != '\0' && digits && (*p == sep || (sep == ' ' && strncmp (p, "\302\240", 2) == 0) ||
                                      (sep == ' ' && strncmp (p, "\342\200\257", 3) == 0)))
          {
            const char *q = p + (*p == sep ? 1 : (guchar) p[0] == 0xC2 ? 2 : 3);

            if ((!grouped && run > 3) || (grouped && run != 3) ||
                !(g_ascii_isdigit (q[0]) && g_ascii_isdigit (q[1]) && g_ascii_isdigit (q[2]) &&
                  !g_ascii_isdigit (q[3])))
              goto not_number;
            grouped = TRUE;
            changed = TRUE;
            run = 0;
            p = q;
            continue;
          }
        break;
      }
  }

  if (*p == dec)
    {
      g_string_append_c (out, '.');
      changed = changed || dec != '.';
      p++;
      while (g_ascii_isdigit (*p))
        {
          g_string_append_c (out, *p++);
          digits = TRUE;
        }
    }
  if (!digits)
    goto not_number;
  if ((*p == 'e' || *p == 'E') && (g_ascii_isdigit (p[1]) ||
                                   ((p[1] == '-' || p[1] == '+') && g_ascii_isdigit (p[2]))))
    {
      g_string_append_c (out, *p++);
      while (g_ascii_isdigit (*p) || *p == '-' || *p == '+')
        g_string_append_c (out, *p++);
    }
  if (*p == '%')
    g_string_append_c (out, *p++);
  if (*p == '-' && options->trailing_minus)
    {
      trailing = TRUE;
      p++;
    }
  while (*p == ')' || *p == ' ')
    g_string_append_c (out, *p++);
  if (*p != '\0')
    goto not_number;
  /* The other of the two separators, in a number that had this one,
   * would have been refused above; a point where the decimal is a comma
   * is not a number of this file's. */
  if (!changed && !trailing)
    goto not_number;
  if (trailing)
    g_string_prepend_c (out, '-');
  return g_string_free (out, FALSE);

not_number:
  g_string_free (out, TRUE);
  return NULL;
}

static int
widen_year (int year, int digits)
{
  if (digits > 2)
    return year;
  return year < 30 ? 2000 + year : 1900 + year;
}

static int
month_from_name (const char *s, gsize n)
{
  static const char *const MONTHS[] = {
    "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"
  };

  if (n < 3)
    return 0;
  for (int i = 0; i < 12; i++)
    if (g_ascii_strncasecmp (s, MONTHS[i], 3) == 0)
      return i + 1;
  return 0;
}

/* A date written in the column's order -- three numbers, or a month's
 * name in the month's place, with anything that is not a digit or a
 * letter between them, or the eight digits of 20260827 run together --
 * and perhaps a time after it, rewritten as yyyy-mm-dd for the entry
 * parser to read.  NULL when it is no date in that order. */
static char *
date_in_order (const char *field, O42DateOrder order)
{
  static const char *const PLACES[] = { "mdy", "dmy", "ymd", "myd", "dym", "ydm" };
  const char *places = PLACES[CLAMP ((int) order, 0, 5)];
  const char *p = field;
  int part[3] = { 0, 0, 0 }, width[3] = { 0, 0, 0 };
  int n = 0, y = 0, m = 0, d = 0;

  while (*p == ' ')
    p++;
  while (n < 3 && *p != '\0')
    {
      const char *start = p;

      if (g_ascii_isdigit (*p))
        {
          while (g_ascii_isdigit (*p))
            p++;
          if (p - start > 8)
            return NULL;
          width[n] = (int) (p - start);
          part[n] = (int) g_ascii_strtoll (start, NULL, 10);
        }
      else if (g_ascii_isalpha (*p))
        {
          while (g_ascii_isalpha (*p))
            p++;
          part[n] = month_from_name (start, p - start);
          if (part[n] == 0 || places[n] != 'm')
            return NULL;
          width[n] = 0;
        }
      else
        return NULL;
      n++;
      if (n < 3)
        {
          const char *gap = p;

          while (*p == '/' || *p == '-' || *p == '.' || *p == ' ' || *p == ',')
            p++;
          if (p == gap)
            break;
        }
    }

  /* 20260827, 270826: the parts run together in the column's order. */
  if (n == 1 && (width[0] == 8 || width[0] == 6))
    {
      char digits[9];
      int at = 0;

      g_snprintf (digits, sizeof digits, "%0*d", width[0], part[0]);
      for (int i = 0; i < 3; i++)
        {
          int w = places[i] == 'y' && width[0] == 8 ? 4 : 2;
          char piece[5] = { 0 };

          memcpy (piece, digits + at, w);
          part[i] = atoi (piece);
          width[i] = w;
          at += w;
        }
      n = 3;
    }
  if (n != 3)
    return NULL;

  for (int i = 0; i < 3; i++)
    switch (places[i])
      {
      case 'y': y = widen_year (part[i], width[i]); break;
      case 'm': m = part[i]; break;
      default:  d = part[i]; break;
      }
  if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1900 || y > 9999)
    return NULL;

  while (*p == ' ' || *p == 'T')
    p++;
  return g_strdup_printf ("%04d-%02d-%02d%s%s", y, m, d, *p != '\0' ? " " : "", p);
}

void
o42_csv_set_cell (O42Sheet *sheet, int row, int col, const char *field,
                  const O42CsvColumn *column, const O42CsvOptions *options)
{
  O42SplitType type = column != NULL ? column->type : O42_SPLIT_GENERAL;

  g_return_if_fail (sheet != NULL);

  if (field == NULL || type == O42_SPLIT_SKIP)
    return;
  if (*field == '\0')
    {
      /* Over cells that were there: the file's empty field empties it. */
      if (!o42_sheet_is_empty (sheet, row, col))
        o42_sheet_set_input (sheet, row, col, NULL);
      return;
    }

  if (type == O42_SPLIT_TEXT)
    {
      /* Kept as it is -- 00123 a code, not a hundred and twenty-three. */
      O42Value text = o42_value_text (field);

      o42_sheet_set_value (sheet, row, col, &text);
      o42_value_clear (&text);
      return;
    }

  if (type == O42_SPLIT_DATE)
    {
      char *iso = date_in_order (field, column->order);
      O42Entry entry;

      if (iso != NULL && o42_entry_parse (iso, &entry))
        {
          o42_sheet_set_input (sheet, row, col, iso);
          g_free (iso);
          return;
        }
      g_free (iso);
    }

  /* Most fields of most files need none of this, and a file can have
   * millions of them. */
  if (options != NULL &&
      (options->decimal != '.' || (options->thousands != ',' && options->thousands != '\0') ||
       (options->trailing_minus && field[strlen (field) - 1] == '-')))
    {
      char *number = number_in_file_form (field, options);

      if (number != NULL)
        {
          O42Entry entry;

          if (o42_entry_parse (number, &entry))
            {
              o42_sheet_set_input (sheet, row, col, number);
              g_free (number);
              return;
            }
          g_free (number);
        }
      /* 1,5 in a file whose decimal point is a point stays the text it
       * is; with the decimal a comma, a point in a number is text too. */
      if (options->decimal != '.' && strchr (field, '.') != NULL)
        {
          O42Entry entry;

          if (o42_entry_parse (field, &entry) && entry.format != O42_NUM_DATE &&
              entry.format != O42_NUM_DATETIME && entry.format != O42_NUM_TIME)
            {
              O42Value text = o42_value_text (field);

              o42_sheet_set_value (sheet, row, col, &text);
              o42_value_clear (&text);
              return;
            }
        }
    }

  o42_sheet_set_input (sheet, row, col, field);
}

int
o42_csv_import (O42Sheet *sheet, const char *text, const O42CsvOptions *options,
                int row, int col, gboolean replace, int *columns)
{
  GPtrArray *fields = g_ptr_array_new_with_free_func (g_free);
  O42Range everything = { 0, 0, O42_MAX_ROWS - 1, O42_MAX_COLS - 1 };
  int n = 0, widest = 0;
  Reader r;

  g_return_val_if_fail (sheet != NULL && text != NULL && options != NULL, 0);

  o42_sheet_begin_group (sheet);
  if (replace)
    {
      o42_sheet_clear_range (sheet, &everything);
      o42_sheet_clear_formats (sheet, &everything);
    }

  reader_init (&r, text, options);
  while (row + n < O42_MAX_ROWS && read_record (&r, fields))
    {
      int c = col;

      for (guint i = 0; i < fields->len; i++)
        {
          O42CsvColumn column = o42_csv_options_column (options, (int) i);
          const char *field = g_ptr_array_index (fields, i);

          if (column.type == O42_SPLIT_SKIP)
            continue;
          if (c < O42_MAX_COLS && (!replace || *field != '\0'))
            o42_csv_set_cell (sheet, row + n, c, field, &column, options);
          c++;
        }
      widest = MAX (widest, c - col);
      n++;
    }
  reader_clear (&r);

  /* A text column wears the Text format, as Excel's wizard leaves it, so
   * that typing 00123 into it later keeps the zeros as well. */
  if (n > 0)
    {
      int c = col;

      for (guint i = 0; i < options->columns->len && c < O42_MAX_COLS; i++)
        {
          O42CsvColumn column = g_array_index (options->columns, O42CsvColumn, i);

          if (column.type == O42_SPLIT_SKIP)
            continue;
          if (column.type == O42_SPLIT_TEXT)
            {
              O42Range range = { row, c, row + n - 1, c };
              O42Fmt fmt = { 0 };

              fmt.number = O42_NUM_TEXT;
              o42_sheet_apply_fmt (sheet, &range, O42_FMT_NUMBER, &fmt);
            }
          c++;
        }
    }
  o42_sheet_end_group (sheet);

  g_ptr_array_unref (fields);
  if (columns != NULL)
    *columns = widest;
  return n;
}

gboolean
o42_csv_load_with (O42Sheet *sheet, GFile *file, const O42CsvOptions *options,
                   GError **error)
{
  O42CsvOptions *own = NULL;
  char *text;

  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (options == NULL)
    {
      text = o42_csv_read_file (file, &own, error);
      options = own;
    }
  else
    {
      char *contents = NULL;
      gsize length = 0;

      text = NULL;
      if (g_file_load_contents (file, NULL, &contents, &length, NULL, error))
        text = o42_csv_decode (contents, length, options->encoding, NULL, NULL);
      g_free (contents);
    }
  if (text == NULL)
    return FALSE;
  o42_csv_import (sheet, text, options, 0, 0, TRUE, NULL);

  o42_csv_options_free (own);
  g_free (text);
  return TRUE;
}

gboolean
o42_csv_load (O42Sheet *sheet, GFile *file, GError **error)
{
  return o42_csv_load_with (sheet, file, NULL, error);
}

/* ---- Writing ------------------------------------------------------------ */

static void
append_field (GString *out, const char *text, const char *sep, char quote, gboolean always)
{
  gboolean needs = always;

  if (quote == '\0')
    {
      g_string_append (out, text);
      return;
    }

  if (!needs)
    needs = (*sep != '\0' && strstr (text, sep) != NULL) || strchr (text, quote) != NULL ||
            strpbrk (text, "\n\r") != NULL;

  /* Leading or trailing spaces are quoted too, so they survive a reader
   * that trims. */
  if (*text == ' ' || (*text != '\0' && text[strlen (text) - 1] == ' '))
    needs = TRUE;

  if (!needs)
    {
      g_string_append (out, text);
      return;
    }

  g_string_append_c (out, quote);
  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p == quote)
        g_string_append_c (out, quote);
      g_string_append_c (out, *p);
    }
  g_string_append_c (out, quote);
}

/* The value itself, with no format between it and the file: fifteen
 * figures of a number, and a date as the serial it is. */
static char *
raw_text (const O42Value *v)
{
  switch (v->type)
    {
    case O42_VALUE_NUMBER: return o42_number_to_text (v->as.number, FALSE);
    case O42_VALUE_TEXT:   return g_strdup (v->as.text);
    case O42_VALUE_BOOL:   return g_strdup (v->as.boolean ? "TRUE" : "FALSE");
    case O42_VALUE_ERROR:  return g_strdup (o42_error_name (v->as.error));
    default:               return g_strdup ("");
    }
}

/* Whether the cell shows its number as a date or a time: by the preset,
 * or by a d, m, y, h or s in a format of its own that is not inside
 * quotes or brackets. */
static gboolean
shows_date (O42Sheet *sheet, int row, int col)
{
  const O42Fmt *fmt = o42_sheet_get_fmt (sheet, row, col);
  gboolean quoted = FALSE;
  int bracket = 0;

  if (fmt->number == O42_NUM_DATE || fmt->number == O42_NUM_TIME || fmt->number == O42_NUM_DATETIME)
    return TRUE;
  if (fmt->custom == NULL)
    return FALSE;
  for (const char *p = fmt->custom; *p != '\0'; p++)
    {
      if (*p == '\\' && p[1] != '\0')
        p++;
      else if (*p == '"')
        quoted = !quoted;
      else if (!quoted && *p == '[')
        bracket++;
      else if (!quoted && *p == ']' && bracket > 0)
        bracket--;
      else if (!quoted && bracket == 0 && strchr ("dmyhsDMYHS", *p) != NULL)
        return TRUE;
    }
  return FALSE;
}

/* A number written with the file's own separators: 1,234.50 is
 * 1.234,50 in a file whose decimal is a comma, so that a file read
 * that way goes back the way it came. */
static void
localize_number (char *text, char decimal, char thousands)
{
  char *out = text;

  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p == '.')
        *out++ = decimal;
      else if (*p == ',')
        {
          if (thousands != '\0')
            *out++ = thousands;
        }
      else
        *out++ = *p;
    }
  *out = '\0';
}

/* What the file says for one cell: its formula, what it shows, or its
 * value, as the options ask. */
static char *
cell_text (O42Sheet *sheet, int row, int col, const O42CsvOptions *options, O42ValueType *type)
{
  O42Value v;
  char *text;
  gboolean formula = options->formulas && o42_sheet_has_formula (sheet, row, col);

  o42_sheet_get_value (sheet, row, col, &v);
  if (formula)
    text = o42_sheet_get_input (sheet, row, col);
  else if (options->as_shown)
    text = o42_sheet_get_display (sheet, row, col);
  else
    text = raw_text (&v);
  if (!formula && v.type == O42_VALUE_NUMBER && options->decimal == ',' &&
      !(options->as_shown && shows_date (sheet, row, col)))
    localize_number (text, ',', options->thousands == ',' ? '.' : options->thousands);
  if (type != NULL)
    *type = v.type;
  o42_value_clear (&v);
  return text;
}

gboolean
o42_csv_save_with (O42Sheet *sheet, GFile *file, const O42CsvOptions *options,
                   GError **error)
{
  O42CsvOptions *own = NULL;
  O42Range used;
  GString *out = g_string_new (NULL);
  const char *encoding;
  char sep[8] = ",";
  int *widths = NULL;
  gboolean ok;

  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  o42_csv_lost_characters = FALSE;
  if (options == NULL)
    options = own = o42_csv_options_for_file (file);
  if (options->separators != NULL && *options->separators != '\0')
    {
      const char *next = g_utf8_next_char (options->separators);

      memcpy (sep, options->separators, MIN ((gsize) (next - options->separators), sizeof sep - 1));
      sep[MIN ((gsize) (next - options->separators), sizeof sep - 1)] = '\0';
    }
  encoding = options->encoding != NULL ? options->encoding : "UTF-8";

  o42_sheet_used_range (sheet, &used);

  /* Fixed width: each column as wide as its widest entry and a space,
   * and never narrower than the file it came from had it, so that a
   * report read in and saved goes back with its columns where they
   * were. */
  if (options->fixed_width)
    {
      int kept = 0;

      /* The file's columns that were not brought in have no sheet
       * column to be the width of. */
      widths = g_new0 (int, used.col1 + 1);
      for (guint i = 0; i < options->breaks->len && kept <= used.col1; i++)
        {
          if (o42_csv_options_column (options, (int) i).type == O42_SPLIT_SKIP)
            continue;
          widths[kept++] = g_array_index (options->breaks, int, i) -
                           (i > 0 ? g_array_index (options->breaks, int, i - 1) : 0);
        }
      for (int row = 0; row <= used.row1; row++)
        for (int col = 0; col <= used.col1; col++)
          {
            char *text = cell_text (sheet, row, col, options, NULL);

            widths[col] = MAX (widths[col], (int) g_utf8_strlen (text, -1) + 1);
            g_free (text);
          }
    }

  /* From A1, not from the used range's corner, so that a sheet with its
   * data in C3 comes back in C3. */
  for (int row = 0; row <= used.row1; row++)
    {
      for (int col = 0; col <= used.col1; col++)
        {
          O42ValueType type;
          char *text = cell_text (sheet, row, col, options, &type);

          if (widths != NULL)
            {
              /* Numbers to the right, as they stand in the cells. */
              int pad = widths[col] - (int) g_utf8_strlen (text, -1);

              if (type == O42_VALUE_NUMBER)
                {
                  for (int i = 1; i < pad; i++)
                    g_string_append_c (out, ' ');
                  g_string_append (out, text);
                  g_string_append_c (out, ' ');
                }
              else
                {
                  g_string_append (out, text);
                  for (int i = 0; i < pad; i++)
                    g_string_append_c (out, ' ');
                }
            }
          else
            {
              if (col > 0)
                g_string_append (out, sep);
              append_field (out, text, sep, options->quote,
                            options->quote_all && type == O42_VALUE_TEXT);
            }
          g_free (text);
        }
      if (widths != NULL)
        while (out->len > 0 && out->str[out->len - 1] == ' ')
          g_string_truncate (out, out->len - 1);
      g_string_append (out, options->crlf ? "\r\n" : "\n");
    }
  g_free (widths);

  if (g_ascii_strcasecmp (encoding, "UTF-8") == 0)
    {
      if (options->bom)
        g_string_prepend (out, "\357\273\277");
      ok = g_file_replace_contents (file, out->str, out->len, NULL, FALSE,
                                    G_FILE_CREATE_NONE, NULL, NULL, error);
    }
  else
    {
      gsize written = 0;
      char *bytes = g_convert (out->str, (gssize) out->len, encoding, "UTF-8", NULL, &written, NULL);

      /* Not every character has a place in every code page: a question
       * mark for those, and the window says so. */
      if (bytes == NULL)
        {
          bytes = g_convert_with_fallback (out->str, (gssize) out->len, encoding, "UTF-8", "?",
                                           NULL, &written, error);
          o42_csv_lost_characters = bytes != NULL;
        }
      ok = bytes != NULL;
      if (ok && options->bom && g_ascii_strncasecmp (encoding, "UTF-16", 6) == 0)
        {
          gboolean big = g_ascii_strcasecmp (encoding, "UTF-16BE") == 0;
          char *marked = g_malloc (written + 2);

          marked[0] = big ? '\376' : '\377';
          marked[1] = big ? '\377' : '\376';
          memcpy (marked + 2, bytes, written);
          g_free (bytes);
          bytes = marked;
          written += 2;
        }
      if (ok)
        ok = g_file_replace_contents (file, bytes, written, NULL, FALSE,
                                      G_FILE_CREATE_NONE, NULL, NULL, error);
      g_free (bytes);
    }

  g_string_free (out, TRUE);
  o42_csv_options_free (own);
  return ok;
}

gboolean
o42_csv_save (O42Sheet *sheet, GFile *file, GError **error)
{
  return o42_csv_save_with (sheet, file, NULL, error);
}
