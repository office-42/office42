/* o42-numfmt.c - see o42-numfmt.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-numfmt.h"

#include "o42-date.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The shape of a format code, with everything that is not the number
 * taken out: the bracketed sections ([Red], [$kr-414], [>100]), the
 * quoted text, the backslash escapes, and every section after the
 * first, which is the one that shows a positive number.  What is left
 * is lower case and safe to look at a character at a time -- before
 * this, the "e" in "[RED]" made every coloured format scientific and
 * the "$" in "[$kr-414]" was the only reason that one was not.
 *
 * `money` comes back TRUE when a [$...] section named a currency. */
static char *
format_skeleton (const char *text, gboolean *money)
{
  GString *out = g_string_new (NULL);

  *money = FALSE;
  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p == '[')
        {
          const char *close = strchr (p, ']');

          if (close == NULL)
            break;
          if (p[1] == '$')
            *money = TRUE;
          p = close;
          continue;
        }
      if (*p == '"')
        {
          const char *close = strchr (p + 1, '"');

          if (close == NULL)
            break;
          p = close;
          continue;
        }
      if (*p == '\\')
        {
          /* An escaped character is shown as itself -- \$ is a dollar
           * sign, and the sign is what says the format is money -- so
           * it stays in the shape. */
          if (p[1] != '\0')
            g_string_append_c (out, g_ascii_tolower (*++p));
          continue;
        }
      if (*p == ';')
        break;   /* the sections after the first say what a negative,
                  * a zero and a text look like; the shape is the same */
      g_string_append_c (out, g_ascii_tolower (*p));
    }
  return g_string_free (out, FALSE);
}

/* Rounding a number for showing, the way every spreadsheet does it.
 *
 * Two things separate this from printf.  The first is the direction: a
 * half goes away from zero, so 2.5 at no decimals is 3 and printf's own
 * round-half-to-even would make it 2.  The second is what is being
 * rounded: the double nearest 2.675 is 2.67499999999999982, and a
 * spreadsheet still shows 2.68 at two decimals, because it decides at
 * the fifteenth significant digit -- where that number reads 2.675
 * exactly.  Nudging by a relative 1e-14 before the half is added is
 * that decision, and it moves nothing that is not already within a
 * rounding error of the halfway point.
 */
static double
show_round (double n, int places)
{
  double scale = pow (10, CLAMP (places, 0, 30));
  double scaled = n * scale;
  double nudged = scaled + copysign (fabs (scaled) * 1e-14, scaled);

  if (!isfinite (scaled) || fabs (n) >= 1e15)
    return n;   /* past fifteen digits the scaling would lose more than
                 * the rounding could gain */
  return copysign (floor (fabs (nudged) + 0.5), n) / scale;
}

/* Groups the integer part in threes.  Done by hand rather than with the
 * locale's thousands separator because a spreadsheet's #,##0 means a comma,
 * and a file that shows commas on one machine and full stops on another is
 * a file that cannot be shared. */
static void
append_grouped (GString *out, const char *digits)
{
  gsize len = strlen (digits);
  gsize first = len % 3;

  if (first == 0)
    first = 3;

  g_string_append_len (out, digits, first);

  for (gsize i = first; i < len; i += 3)
    {
      g_string_append_c (out, ',');
      g_string_append_len (out, digits + i, 3);
    }
}

static char *
format_general (double n)
{
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];

  /* Whole numbers print without a decimal point, which is what General does
   * and what stops a column of counts reading as 1.0, 2.0, 3.0. */
  if (n == floor (n) && fabs (n) < 1e15)
    return g_strdup_printf ("%.0f", n);

  /* Otherwise up to ten significant figures, with the trailing zeroes that
   * %g leaves behind trimmed off.  It has to be the g_ascii_ variant: the C
   * library's own printf would write a decimal comma in half the world's
   * locales, and a spreadsheet's 1.5 must be 1.5 everywhere. */
  g_ascii_formatd (buffer, sizeof buffer, "%.10g", n);

  return g_strdup (buffer);
}

char *
o42_number_format (double n, O42NumberFormat format, int decimals)
{
  GString *out;
  gboolean negative;
  char buffer[64];
  char *point;

  if (isnan (n) || isinf (n))
    return g_strdup ("#NUM!");

  if (format == O42_NUM_GENERAL || format == O42_NUM_TEXT)
    return format_general (n);

  if (format == O42_NUM_DATE || format == O42_NUM_TIME ||
      format == O42_NUM_DATETIME)
    return o42_date_format (n, format);

  if (format == O42_NUM_PERCENT)
    n *= 100.0;

  decimals = CLAMP (decimals, 0, 20);

  /* g_ascii_formatd takes a fixed format string, so the precision is baked
   * into one first. */
  if (format == O42_NUM_SCIENTIFIC)
    {
      char spec[16];

      g_snprintf (spec, sizeof spec, "%%.%dE", decimals);
      g_ascii_formatd (buffer, sizeof buffer, spec, n);
      return g_strdup (buffer);
    }

  negative = (n < 0);
  if (negative)
    n = -n;

  {
    char spec[16];

    g_snprintf (spec, sizeof spec, "%%.%df", decimals);
    g_ascii_formatd (buffer, sizeof buffer, spec, show_round (n, decimals));
  }

  /* A value that rounds to nothing is not negative: -0.001 at two
   * decimals is 0.00, not -0.00. */
  if (negative && strspn (buffer, "0.") == strlen (buffer))
    negative = FALSE;

  out = g_string_new (NULL);

  if (negative)
    g_string_append_c (out, '-');

  if (format == O42_NUM_CURRENCY)
    g_string_append_c (out, '$');

  point = strchr (buffer, '.');
  if (point != NULL)
    *point = '\0';

  if (format == O42_NUM_COMMA || format == O42_NUM_CURRENCY)
    append_grouped (out, buffer);
  else
    g_string_append (out, buffer);

  if (point != NULL)
    {
      g_string_append_c (out, '.');
      g_string_append (out, point + 1);
    }

  if (format == O42_NUM_PERCENT)
    g_string_append_c (out, '%');

  return g_string_free (out, FALSE);
}

char *
o42_number_format_to_string (O42NumberFormat format, int decimals)
{
  GString *out;

  switch (format)
    {
    case O42_NUM_TEXT:     return g_strdup ("@");
    case O42_NUM_DATE:     return g_strdup ("yyyy-mm-dd");
    case O42_NUM_TIME:     return g_strdup ("hh:mm:ss");
    case O42_NUM_DATETIME: return g_strdup ("yyyy-mm-dd hh:mm");
    case O42_NUM_GENERAL:  return g_strdup ("General");
    default: break;
    }

  out = g_string_new (NULL);
  decimals = CLAMP (decimals, 0, 20);

  if (format == O42_NUM_CURRENCY)
    g_string_append (out, "$#,##0");
  else if (format == O42_NUM_COMMA)
    g_string_append (out, "#,##0");
  else
    g_string_append_c (out, '0');

  if (decimals > 0)
    {
      g_string_append_c (out, '.');
      for (int i = 0; i < decimals; i++)
        g_string_append_c (out, '0');
    }

  if (format == O42_NUM_PERCENT)
    g_string_append_c (out, '%');
  else if (format == O42_NUM_SCIENTIFIC)
    g_string_append (out, "E+00");

  return g_string_free (out, FALSE);
}

gboolean
o42_number_format_parse (const char *text, O42NumberFormat *format, int *decimals)
{
  char *lower;
  const char *point;
  gboolean has_date, has_time, money = FALSE;
  O42NumberFormat f;
  int d = 0;

  if (text == NULL)
    return FALSE;

  lower = format_skeleton (text, &money);
  has_date = strstr (lower, "yy") != NULL || strstr (lower, "dd") != NULL ||
             strstr (lower, "mmm") != NULL;
  has_time = strstr (lower, "hh") != NULL || strstr (lower, "ss") != NULL ||
             strstr (lower, "h:") != NULL;

  if (strcmp (lower, "general") == 0 || *lower == '\0') f = O42_NUM_GENERAL;
  else if (strcmp (lower, "@") == 0)          f = O42_NUM_TEXT;
  else if (has_date && has_time)              f = O42_NUM_DATETIME;
  else if (has_date)                          f = O42_NUM_DATE;
  else if (has_time)                          f = O42_NUM_TIME;
  else if (strstr (lower, "e+") != NULL ||
           strstr (lower, "e-") != NULL)      f = O42_NUM_SCIENTIFIC;
  else if (strchr (lower, '%') != NULL)       f = O42_NUM_PERCENT;
  else if (money || strchr (lower, '$') != NULL) f = O42_NUM_CURRENCY;
  else if (strchr (lower, ',') != NULL)       f = O42_NUM_COMMA;
  else if (strchr (lower, '0') != NULL || strchr (lower, '#') != NULL)
    f = O42_NUM_FIXED;
  else
    {
      g_free (lower);
      return FALSE;
    }

  point = strchr (lower, '.');
  if (point != NULL)
    for (const char *p = point + 1; *p == '0' || *p == '#'; p++)
      d++;

  g_free (lower);

  if (format)   *format = f;
  if (decimals) *decimals = d;
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Format strings                                                          */
/* ---------------------------------------------------------------------- */

/* Excel's format language, as much of it as a sheet from Excel or Gnumeric
 * is likely to carry: up to four sections split by ";" for positive,
 * negative, zero and text; "0", "#" and "?" digit places with "," for
 * grouping and scaling and "." for the point; "%"; "E+00"; the date and
 * time codes; "@" for text; text in quotes or after a backslash; "[Red]"
 * and its kin for colour; "_x" for a space and "*x" for a fill, the fill
 * being ignored.  Fractions ("# ?/?") are not done. */

typedef struct {
  const char *start;
  const char *end;
} Section;

/* Splits a format into its sections, ";" outside quotes. */
static int
split_sections (const char *format, Section *sections, int max)
{
  int n = 0;
  const char *p = format;
  const char *start = format;
  gboolean quoted = FALSE;

  for (; ; p++)
    {
      if (*p == '"')
        quoted = !quoted;
      else if (*p == '\\' && p[1] != '\0')
        p++;
      else if ((*p == ';' && !quoted) || *p == '\0')
        {
          if (n < max)
            {
              sections[n].start = start;
              sections[n].end = p;
              n++;
            }
          if (*p == '\0')
            break;
          start = p + 1;
        }
    }

  return n;
}

/* Is there a date or time code in the section, outside quotes and
 * brackets? */
static gboolean
section_is_date (const Section *s)
{
  gboolean quoted = FALSE, bracket = FALSE;

  for (const char *p = s->start; p < s->end; p++)
    {
      if (*p == '"') { quoted = !quoted; continue; }
      if (quoted) continue;
      if (*p == '\\') { p++; continue; }
      if (*p == '[') { bracket = TRUE; continue; }
      if (*p == ']') { bracket = FALSE; continue; }
      if (bracket) continue;
      if (strchr ("yYdDhHsS", *p) != NULL)
        return TRUE;
      if ((*p == 'm' || *p == 'M') )
        return TRUE;
      if (g_ascii_strncasecmp (p, "AM/PM", 5) == 0 || g_ascii_strncasecmp (p, "A/P", 3) == 0)
        return TRUE;
    }

  return FALSE;
}

static const char *const LONG_MONTHS[] = {
  "January", "February", "March", "April", "May", "June", "July",
  "August", "September", "October", "November", "December"
};
static const char *const LONG_DAYS[] = {
  "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
};

/* The month and day names of the locales an Excel format can ask for
 * with [$-409] and its like.  The number in the brackets is a Windows
 * LCID; its low ten bits are the language, which is what decides the
 * names -- 0x409 and 0x809 are both English.  A locale that is not
 * here is written in English, which is what Excel does with a language
 * it was not installed with. */
typedef struct {
  guint             lang;
  const char *const months[12];
  const char *const days[7];      /* Monday first, as o42_date_weekday counts */
  const char *const dated[12];    /* the form a month takes beside a day */
} DateNames;

static const DateNames DATE_NAMES[] = {
  { 0x07,   /* German */
    { "Januar", "Februar", "M\303\244rz", "April", "Mai", "Juni", "Juli",
      "August", "September", "Oktober", "November", "Dezember" },
    { "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag", "Sonntag" },
    { NULL } },
  { 0x0A,   /* Spanish */
    { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio",
      "agosto", "septiembre", "octubre", "noviembre", "diciembre" },
    { "lunes", "martes", "mi\303\251rcoles", "jueves", "viernes", "s\303\241bado", "domingo" },
    { NULL } },
  { 0x0B,   /* Finnish */
    { "tammikuu", "helmikuu", "maaliskuu", "huhtikuu", "toukokuu", "kes\303\244kuu",
      "hein\303\244kuu", "elokuu", "syyskuu", "lokakuu", "marraskuu", "joulukuu" },
    { "maanantai", "tiistai", "keskiviikko", "torstai", "perjantai", "lauantai", "sunnuntai" },
    { NULL } },
  { 0x0C,   /* French */
    { "janvier", "f\303\251vrier", "mars", "avril", "mai", "juin", "juillet",
      "ao\303\273t", "septembre", "octobre", "novembre", "d\303\251cembre" },
    { "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi", "dimanche" },
    { NULL } },
  { 0x10,   /* Italian */
    { "gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio",
      "agosto", "settembre", "ottobre", "novembre", "dicembre" },
    { "luned\303\254", "marted\303\254", "mercoled\303\254", "gioved\303\254",
      "venerd\303\254", "sabato", "domenica" },
    { NULL } },
  { 0x13,   /* Dutch */
    { "januari", "februari", "maart", "april", "mei", "juni", "juli",
      "augustus", "september", "oktober", "november", "december" },
    { "maandag", "dinsdag", "woensdag", "donderdag", "vrijdag", "zaterdag", "zondag" },
    { NULL } },
  { 0x14,   /* Norwegian */
    { "januar", "februar", "mars", "april", "mai", "juni", "juli",
      "august", "september", "oktober", "november", "desember" },
    { "mandag", "tirsdag", "onsdag", "torsdag", "fredag", "l\303\270rdag", "s\303\270ndag" },
    { NULL } },
  { 0x06,   /* Danish */
    { "januar", "februar", "marts", "april", "maj", "juni", "juli",
      "august", "september", "oktober", "november", "december" },
    { "mandag", "tirsdag", "onsdag", "torsdag", "fredag", "l\303\270rdag", "s\303\270ndag" },
    { NULL } },
  { 0x1D,   /* Swedish */
    { "januari", "februari", "mars", "april", "maj", "juni", "juli",
      "augusti", "september", "oktober", "november", "december" },
    { "m\303\245ndag", "tisdag", "onsdag", "torsdag", "fredag", "l\303\266rdag", "s\303\266ndag" },
    { NULL } },
  { 0x16,   /* Portuguese */
    { "janeiro", "fevereiro", "mar\303\247o", "abril", "maio", "junho", "julho",
      "agosto", "setembro", "outubro", "novembro", "dezembro" },
    { "segunda-feira", "ter\303\247a-feira", "quarta-feira", "quinta-feira",
      "sexta-feira", "s\303\241bado", "domingo" },
    { NULL } },
  { 0x15,   /* Polish */
    { "stycze\305\204", "luty", "marzec", "kwiecie\305\204", "maj", "czerwiec",
      "lipiec", "sierpie\305\204", "wrzesie\305\204", "pa\305\272dziernik",
      "listopad", "grudzie\305\204" },
    { "poniedzia\305\202ek", "wtorek", "\305\233roda", "czwartek", "pi\304\205tek",
      "sobota", "niedziela" },
    /* Polish puts the month in the genitive when a day stands beside
     * it, and Excel and LibreOffice both write it that way. */
    { "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca", "lipca",
      "sierpnia", "wrze\305\233nia", "pa\305\272dziernika", "listopada", "grudnia" } }
};

/* The locale a section asks for: the hexadecimal number after the dash
 * inside [$...-409], reduced to its language.  Zero for a section that
 * names no locale, and so for English. */
static guint
section_language (const Section *s)
{
  for (const char *p = s->start; p + 2 < s->end; p++)
    {
      const char *close, *dash;

      if (*p != '[' || p[1] != '$')
        continue;
      close = memchr (p, ']', (gsize) (s->end - p));
      if (close == NULL)
        break;
      dash = memchr (p, '-', (gsize) (close - p));
      if (dash != NULL)
        return (guint) g_ascii_strtoull (dash + 1, NULL, 16) & 0x3FF;
      p = close;
    }
  return 0;
}

/* Whether a section writes the day of the month as a number, which is
 * what decides the form a month name takes in some languages. */
static gboolean
section_has_day (const Section *s)
{
  gboolean quoted = FALSE, bracket = FALSE;

  for (const char *p = s->start; p < s->end; p++)
    {
      if (*p == '"') { quoted = !quoted; continue; }
      if (quoted) continue;
      if (*p == '[') { bracket = TRUE; continue; }
      if (*p == ']') { bracket = FALSE; continue; }
      if (bracket) continue;
      if (*p == 'd' || *p == 'D')
        {
          int run = 0;

          while (p + run < s->end && (p[run] == 'd' || p[run] == 'D'))
            run++;
          if (run <= 2)
            return TRUE;
          p += run - 1;
        }
    }
  return FALSE;
}

static const DateNames *
date_names (guint lang)
{
  for (gsize i = 0; i < G_N_ELEMENTS (DATE_NAMES); i++)
    if (DATE_NAMES[i].lang == lang)
      return &DATE_NAMES[i];
  return NULL;
}

/* The first three characters of a name, which is how every locale here
 * shortens one.  Characters, not bytes: "M\303\244rz" is three of them. */
static void
append_short (GString *out, const char *name)
{
  const char *end = g_utf8_offset_to_pointer (name, MIN (3, g_utf8_strlen (name, -1)));

  g_string_append_len (out, name, end - name);
}

/* Writes a date or time section.  "m" is a month unless it follows an
 * hour code or precedes a second code, in which case it is minutes -- the
 * rule Excel uses and everyone else copied. */
static void
format_date_section (GString *out, const Section *s, double n)
{
  int y = 0, mo = 1, d = 1, h, mi, sec;
  gboolean ampm = FALSE;
  const char *p;
  gboolean last_was_hour = FALSE;
  const DateNames *names = date_names (section_language (s));
  const char *const *months = names != NULL ? names->months : LONG_MONTHS;
  const char *const *days = names != NULL ? names->days : LONG_DAYS;

  if (names != NULL && names->dated[0] != NULL && section_has_day (s))
    months = names->dated;

  o42_date_from_serial (n, &y, &mo, &d);
  o42_time_from_serial (n, &h, &mi, &sec);

  /* Twelve-hour clock if AM/PM appears anywhere in the section. */
  for (p = s->start; p < s->end; p++)
    if (g_ascii_strncasecmp (p, "AM/PM", 5) == 0 || g_ascii_strncasecmp (p, "A/P", 3) == 0)
      ampm = TRUE;

  for (p = s->start; p < s->end; )
    {
      char c = *p;
      int run = 0;

      if (c == '"')
        {
          p++;
          while (p < s->end && *p != '"')
            g_string_append_c (out, *p++);
          if (p < s->end) p++;
          continue;
        }
      if (c == '\\')
        {
          if (p + 1 < s->end) g_string_append_c (out, p[1]);
          p += 2;
          continue;
        }
      if (c == '[')
        {
          /* [h], [m], [s]: elapsed time; colours are skipped. */
          const char *close = memchr (p, ']', (gsize) (s->end - p));
          if (close == NULL) { p++; continue; }
          if (close - p == 2 && (p[1] == 'h' || p[1] == 'H'))
            g_string_append_printf (out, "%d", (int) floor (n * 24));
          else if (close - p == 2 && (p[1] == 'm' || p[1] == 'M'))
            g_string_append_printf (out, "%d", (int) floor (n * 24 * 60));
          else if (close - p == 2 && (p[1] == 's' || p[1] == 'S'))
            g_string_append_printf (out, "%d", (int) floor (n * 24 * 3600));
          last_was_hour = (close - p == 2 && (p[1] == 'h' || p[1] == 'H'));
          p = close + 1;
          continue;
        }
      if (g_ascii_strncasecmp (p, "AM/PM", 5) == 0)
        {
          g_string_append (out, h < 12 ? "AM" : "PM");
          p += 5;
          continue;
        }
      if (g_ascii_strncasecmp (p, "A/P", 3) == 0)
        {
          g_string_append (out, h < 12 ? "A" : "P");
          p += 3;
          continue;
        }

      while (p + run < s->end && g_ascii_tolower (p[run]) == g_ascii_tolower (c))
        run++;

      switch (g_ascii_tolower (c))
        {
        case 'y':
          if (run >= 4) g_string_append_printf (out, "%04d", y);
          else          g_string_append_printf (out, "%02d", y % 100);
          last_was_hour = FALSE;
          break;

        case 'm':
          {
            /* Minutes when an hour came just before or seconds come next. */
            const char *after = p + run;
            gboolean minutes = last_was_hour;

            while (after < s->end && (*after == ':' || *after == ' ' || *after == '.'))
              after++;
            if (after < s->end && (*after == 's' || *after == 'S'))
              minutes = TRUE;

            if (minutes)
              g_string_append_printf (out, run >= 2 ? "%02d" : "%d", mi);
            else if (run >= 4)
              g_string_append (out, months[mo - 1]);
            else if (run == 3)
              append_short (out, months[mo - 1]);
            else
              g_string_append_printf (out, run >= 2 ? "%02d" : "%d", mo);
            last_was_hour = FALSE;
            break;
          }

        case 'd':
          if (run >= 4)      g_string_append (out, days[o42_date_weekday (n) - 1]);
          else if (run == 3) append_short (out, days[o42_date_weekday (n) - 1]);
          else               g_string_append_printf (out, run >= 2 ? "%02d" : "%d", d);
          last_was_hour = FALSE;
          break;

        case 'h':
          {
            int hh = ampm ? ((h % 12 == 0) ? 12 : h % 12) : h;
            g_string_append_printf (out, run >= 2 ? "%02d" : "%d", hh);
            last_was_hour = TRUE;
            break;
          }

        case 's':
          g_string_append_printf (out, run >= 2 ? "%02d" : "%d", sec);
          last_was_hour = FALSE;
          break;

        case '_':
          g_string_append_c (out, ' ');
          run = 2;
          break;

        case '*':
          run = 2;
          break;

        default:
          g_string_append_len (out, p, run);
          if (c != ':' && c != ' ')
            last_was_hour = FALSE;
          break;
        }

      p += run;
    }
}

/* Writes a number section: the digit places on either side of the point,
 * with grouping, scaling, percent and an exponent as the section asks. */
static void
format_number_section (GString *out, const Section *s, double n)
{
  int int_places = 0, dec_places = 0;
  gboolean grouping = FALSE, percent = FALSE, exponent = FALSE, exp_plus = FALSE;
  int scale_commas = 0;
  gboolean seen_point = FALSE, seen_digit = FALSE, int_zero_place = FALSE;
  const char *p;
  char digits[64];
  char *point;
  const char *int_digits, *dec_digits;
  int int_len, exp10 = 0;
  int int_used = 0, dec_used = 0;

  /* First pass: what the section asks for. */
  for (p = s->start; p < s->end; p++)
    {
      if (*p == '"') { p++; while (p < s->end && *p != '"') p++; continue; }
      if (*p == '\\') { p++; continue; }
      if (*p == '[') { while (p < s->end && *p != ']') p++; continue; }
      if (*p == '_' || *p == '*') { p++; continue; }

      if (*p == '0' || *p == '#' || *p == '?')
        {
          if (exponent) { /* exponent places are counted where they are written */ }
          else if (seen_point) dec_places++;
          else
            {
              int_places++;
              if (*p == '0')
                int_zero_place = TRUE;
            }
          seen_digit = TRUE;
          scale_commas = 0;
        }
      else if (*p == '.' && !exponent)
        seen_point = TRUE;
      else if (*p == ',' && seen_digit && !seen_point && !exponent)
        {
          /* A comma between digits groups; commas after the last digit
           * before the point each divide by a thousand. */
          const char *q = p + 1;
          while (q < s->end && *q == ',') q++;
          if (q < s->end && (*q == '0' || *q == '#' || *q == '?'))
            grouping = TRUE;
          else
            scale_commas++;
          p = q - 1;
        }
      else if (*p == '%')
        percent = TRUE;
      else if ((*p == 'E' || *p == 'e') && p + 1 < s->end && (p[1] == '+' || p[1] == '-'))
        {
          exponent = TRUE;
          exp_plus = (p[1] == '+');
          p++;
        }
    }

  if (percent)
    n *= 100;
  for (int i = 0; i < scale_commas; i++)
    n /= 1000;

  if (exponent)
    {
      /* Mantissa scaled so that int_places digits stand before the
       * point, as in Excel's 0.00E+00 versus ##0.0E+0. */
      if (n != 0)
        {
          exp10 = (int) floor (log10 (fabs (n)));
          exp10 -= MAX (int_places, 1) - 1;
          if (int_places > 1)
            exp10 = (int) floor ((double) exp10 / int_places) * int_places;
          n /= pow (10, exp10);
        }
    }

  /* Half away from zero, as a spreadsheet rounds; printf's own rounding
   * would make 2.5 into 2. */
  {
    char spec[16];

    n = show_round (n, dec_places);
    g_snprintf (spec, sizeof spec, "%%.%df", CLAMP (dec_places, 0, 30));
    g_ascii_formatd (digits, sizeof digits, spec, n);
  }
  point = strchr (digits, '.');
  if (point != NULL)
    *point++ = '\0';
  int_digits = digits;
  dec_digits = (point != NULL) ? point : "";
  int_len = (int) strlen (int_digits);
  if (int_len == 1 && int_digits[0] == '0' && !int_zero_place)
    int_len = 0;      /* "#.00" shows .50, not 0.50 */

  /* Second pass: write it. */
  for (p = s->start; p < s->end; )
    {
      char c = *p;

      if (c == '"')
        {
          p++;
          while (p < s->end && *p != '"') g_string_append_c (out, *p++);
          if (p < s->end) p++;
          continue;
        }
      if (c == '\\')
        {
          if (p + 1 < s->end) g_string_append_c (out, p[1]);
          p += 2;
          continue;
        }
      if (c == '[')
        {
          const char *close = memchr (p, ']', (gsize) (s->end - p));
          if (close == NULL) { p++; continue; }
          /* [$USD-409] and [$$-409] carry a currency symbol. */
          if (p[1] == '$')
            {
              const char *dash = memchr (p, '-', (gsize) (close - p));
              g_string_append_len (out, p + 2, (dash != NULL ? dash : close) - (p + 2));
            }
          p = close + 1;
          continue;
        }
      if (c == '_') { g_string_append_c (out, ' '); p += 2; continue; }
      if (c == '*') { p += 2; continue; }

      if (c == '0' || c == '#' || c == '?')
        {
          if (int_used < int_places)
            {
              /* The first integer place takes every digit not covered by
               * the places after it. */
              int remaining_places = int_places - int_used;
              int take = int_len - (remaining_places - 1);

              if (int_used == 0)
                {
                  if (take > 0)
                    {
                      for (int i = 0; i < take; i++)
                        {
                          g_string_append_c (out, int_digits[i]);
                          if (grouping && (int_len - 1 - i) % 3 == 0 && i < int_len - 1)
                            g_string_append_c (out, ',');
                        }
                    }
                  else if (c == '0')
                    g_string_append_c (out, '0');
                  else if (c == '?')
                    g_string_append_c (out, ' ');
                }
              else
                {
                  int idx = int_len - remaining_places;
                  if (idx >= 0)
                    {
                      g_string_append_c (out, int_digits[idx]);
                      if (grouping && (int_len - 1 - idx) % 3 == 0 && idx < int_len - 1)
                        g_string_append_c (out, ',');
                    }
                  else if (c == '0')
                    g_string_append_c (out, '0');
                  else if (c == '?')
                    g_string_append_c (out, ' ');
                }
              int_used++;
            }
          else if (dec_used < dec_places)
            {
              char digit = dec_digits[dec_used];
              gboolean rest_zero = TRUE;

              for (const char *q = dec_digits + dec_used; *q != '\0'; q++)
                if (*q != '0') rest_zero = FALSE;

              if (c == '0' || !rest_zero)
                g_string_append_c (out, digit);
              else if (c == '?')
                g_string_append_c (out, ' ');
              dec_used++;
            }
          p++;
          continue;
        }

      if (c == '.' && !exponent)
        {
          /* No point when there are no decimals to follow. */
          if (dec_places > 0)
            g_string_append_c (out, '.');
          p++;
          continue;
        }
      if (c == ',')
        { p++; continue; }
      if ((c == 'E' || c == 'e') && p + 1 < s->end && (p[1] == '+' || p[1] == '-'))
        {
          const char *q = p + 2;
          int places = 0;

          while (q < s->end && (*q == '0' || *q == '#' || *q == '?'))
            { places++; q++; }

          g_string_append_c (out, c);
          if (exp10 < 0) g_string_append_c (out, '-');
          else if (exp_plus) g_string_append_c (out, '+');
          g_string_append_printf (out, "%0*d", places, abs (exp10));
          p = q;
          continue;
        }

      g_string_append_c (out, c);
      p++;
    }
}

char *
o42_format_string (const char *format, double n, const char *text)
{
  Section sections[4];
  int count;
  const Section *use;
  GString *out;
  gboolean negative;

  g_return_val_if_fail (format != NULL, g_strdup (""));

  count = split_sections (format, sections, 4);
  if (count == 0)
    return g_strdup ("");

  /* Text goes to the fourth section, or to a lone section with "@" in
   * it, or is shown as itself. */
  if (text != NULL)
    {
      if (count >= 4)
        use = &sections[3];
      else if (count == 1 && memchr (sections[0].start, '@',
                                     (gsize) (sections[0].end - sections[0].start)) != NULL)
        use = &sections[0];
      else
        return g_strdup (text);

      out = g_string_new (NULL);
      for (const char *p = use->start; p < use->end; p++)
        {
          if (*p == '@') g_string_append (out, text);
          else if (*p == '"') { p++; while (p < use->end && *p != '"') g_string_append_c (out, *p++); }
          else if (*p == '\\' && p + 1 < use->end) g_string_append_c (out, *++p);
          else if (*p == '[') { while (p < use->end && *p != ']') p++; }
          else g_string_append_c (out, *p);
        }
      return g_string_free (out, FALSE);
    }

  if (isnan (n) || isinf (n))
    return g_strdup ("#NUM!");

  negative = (n < 0);
  if (count >= 2 && negative)
    {
      use = &sections[1];
      n = -n;              /* the negative section supplies its own sign */
    }
  else if (count >= 3 && n == 0)
    use = &sections[2];
  else
    use = &sections[0];

  /* "General" in a section is the General display. */
  if (use->end - use->start == 7 && g_ascii_strncasecmp (use->start, "General", 7) == 0)
    return o42_number_format (negative && count >= 2 ? -n : n, O42_NUM_GENERAL, 0);

  /* Fractions ("# ?/?") are not done; General is more honest than a
   * wrong picture. */
  if (!section_is_date (use))
    {
      gboolean quoted = FALSE;
      for (const char *p = use->start; p < use->end; p++)
        {
          if (*p == '"') quoted = !quoted;
          else if (*p == '/' && !quoted)
            return o42_number_format (negative && count >= 2 ? -n : n, O42_NUM_GENERAL, 0);
        }
    }

  out = g_string_new (NULL);

  if (section_is_date (use))
    format_date_section (out, use, n);
  else
    {
      format_number_section (out, use, fabs (n));

      /* A single section supplies no sign of its own; the minus goes in
       * front -- unless what was written rounds to nothing, since -0.00
       * is not a number anyone wants to see. */
      if (negative && count < 2 && strpbrk (out->str, "123456789") != NULL)
        g_string_prepend_c (out, '-');
    }

  return g_string_free (out, FALSE);
}

/* Which colour a format asks for on this value, as [Red] and the others
 * put it, or FALSE if none. */
gboolean
o42_format_string_colour (const char *format, double n, guint32 *colour)
{
  static const struct { const char *name; guint32 rgb; } COLOURS[] = {
    { "Black", 0x000000 }, { "Blue", 0x0000FF }, { "Cyan", 0x00FFFF },
    { "Green", 0x00FF00 }, { "Magenta", 0xFF00FF }, { "Red", 0xFF0000 },
    { "White", 0xFFFFFF }, { "Yellow", 0xFFFF00 },
  };
  Section sections[4];
  int count;
  const Section *use;

  if (format == NULL)
    return FALSE;

  count = split_sections (format, sections, 4);
  if (count == 0)
    return FALSE;

  if (count >= 2 && n < 0)      use = &sections[1];
  else if (count >= 3 && n == 0) use = &sections[2];
  else                           use = &sections[0];

  for (const char *p = use->start; p < use->end; p++)
    {
      if (*p != '[')
        continue;
      for (guint i = 0; i < G_N_ELEMENTS (COLOURS); i++)
        {
          gsize len = strlen (COLOURS[i].name);
          if (g_ascii_strncasecmp (p + 1, COLOURS[i].name, len) == 0 && p[1 + len] == ']')
            {
              *colour = COLOURS[i].rgb;
              return TRUE;
            }
        }
    }

  return FALSE;
}
