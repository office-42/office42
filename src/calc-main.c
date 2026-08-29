/* calc-main.c - drive the office42 engine from a terminal
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Reads lines of the form
 *
 *     A1 = 10
 *     B1 = =A1*2
 *     B1
 *
 * from standard input.  A line with "=" sets a cell, a bare reference shows
 * one, and "dump" prints the used range.  "copy A1:B2 C1", "filldown A1:A9",
 * "fillright A1:F1", "insertrows 3 2", "deleterows 3", "insertcols 2" and
 * "deletecols 2" move cells about, with row and column numbers as the
 * headers show them.  "save FILE" and "load FILE" write and read a
 * .gnumeric file, or a .csv one if the name ends that way.  "sort A1:C9 B
 * desc header", "find TEXT" and "replace OLD -> NEW" do what they say;
 * "pdf FILE" exports.  "sheet NAME" switches to (or makes) a sheet, "rename
 * NAME" renames the current one and "delsheet" removes it; formulas reach
 * across with Sheet2!A1.
 *
 * It exists so the engine can be exercised without a window, and so the
 * window is the only thing the window has to get right.
 */

#include "o42-sheet.h"
#include "o42-analysis.h"
#include "o42-image.h"
#include "o42-pattern.h"
#include "o42-spell.h"
#include "o42-book.h"
#include "o42-eval.h"
#include "o42-csv.h"
#include "o42-text-formats.h"
#include "o42-gnumeric.h"
#include "o42-xlsx.h"
#include "o42-xls.h"
#include "o42-ods.h"
#include "o42-html.h"
#include "o42-pdf.h"
#include "o42-sql.h"
#include "o42-python.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
dump (O42Sheet *sheet)
{
  O42Range used;
  int shown_rows = 0, last_col;
  /* A used range can now be a million rows deep and sixteen thousand
   * wide; what a terminal wants is the corner of it. */
  const int MAX_ROWS_SHOWN = 200, MAX_COLS_SHOWN = 40;

  o42_sheet_used_range (sheet, &used);
  last_col = MIN (used.col1, used.col0 + MAX_COLS_SHOWN - 1);

  printf ("      ");
  for (int col = used.col0; col <= last_col; col++)
    {
      char name[8];
      o42_col_name (col, name, sizeof name);
      printf ("%-14s", name);
    }
  printf ("\n");

  for (int row = used.row0; row <= used.row1 && shown_rows < MAX_ROWS_SHOWN; row++)
    {
      if (o42_sheet_row_hidden (sheet, row))
        continue;
      printf ("%-6d", row + 1);
      for (int col = used.col0; col <= last_col; col++)
        {
          char *text = o42_sheet_get_display (sheet, row, col);
          printf ("%-14s", text);
          g_free (text);
        }
      printf ("\n");
      shown_rows++;
    }
  if (used.row1 - used.row0 + 1 > MAX_ROWS_SHOWN || last_col < used.col1)
    printf ("... %d rows by %d columns in all%s\n",
            used.row1 - used.row0 + 1, used.col1 - used.col0 + 1,
            shown_rows >= MAX_ROWS_SHOWN ? "; ask for a cell or a range by name" : "");
}

/* One word of a cell, on its way through the spelling check. */
typedef struct {
  O42Spell *spell;
  O42Sheet *sheet;
  int       row, col;
} SpellWalk;

static void
spell_word (const char *word, gsize offset, gsize length, gpointer user)
{
  SpellWalk *walk = user;
  char **suggestions;
  char *where;

  (void) offset; (void) length;
  if (o42_spell_check (walk->spell, word))
    return;

  where = o42_ref_name (walk->row, walk->col);
  suggestions = o42_spell_suggest (walk->spell, word);
  printf ("%s: %s", where, word);
  if (suggestions != NULL)
    {
      printf (" ->");
      for (int i = 0; suggestions[i] != NULL && i < 3; i++)
        printf ("%s %s", i > 0 ? "," : "", suggestions[i]);
    }
  printf ("\n");
  g_strfreev (suggestions);
  g_free (where);
}

int
main (int argc, char *argv[])
{
  O42Book *book = o42_book_new ();
  O42Sheet *sheet = o42_book_sheet (book, 0);
  O42Db *db = NULL;
  char line[4096];

  /* SQLVALUE() asks the book's database; it answers #N/A while there
   * is none. */
  o42_db_register_function (book);

  (void) argc; (void) argv;

  if (argc > 1 && strcmp (argv[1], "--functions") == 0)
    {
      guint n = 0;
      const char * const *names = o42_function_names (&n);

      for (guint i = 0; i < n; i++)
        {
          const char *signature = NULL, *summary = NULL;

          if (o42_function_help (names[i], &signature, &summary))
            printf ("%-40s %s\n", signature, summary);
          else
            printf ("%s\n", names[i]);
        }

      o42_book_free (book);
      return 0;
    }

  while (fgets (line, sizeof line, stdin) != NULL)
    {
      char *text = g_strstrip (line);
      char *eq;
      int row, col;
      gsize used;

      if (*text == '\0' || *text == '#')
        continue;

      if (strcmp (text, "dump") == 0)
        {
          dump (sheet);
          continue;
        }

      /* "sheet NAME" switches to a sheet, making it if it is new;
       * "rename NAME" renames the current one; "delsheet" removes it. */
      if (g_str_has_prefix (text, "sheet "))
        {
          O42Sheet *found = o42_book_find_sheet (book, text + 6);

          if (found == NULL)
            found = o42_book_add_sheet (book, text + 6, -1);
          sheet = found;
          continue;
        }

      /* "tabcolour RRGGBB" paints the tab, "tabcolour none" clears it,
       * "tabcolour" alone says what it is. */
      if (g_str_has_prefix (text, "tabcolour") || g_str_has_prefix (text, "tabcolor"))
        {
          const char *arg = strchr (text, ' ');

          while (arg != NULL && *arg == ' ')
            arg++;
          if (arg == NULL || *arg == 0)
            {
              guint32 c = o42_sheet_tab_colour (sheet);

              if (c == O42_TAB_NO_COLOUR)
                printf ("no tab colour\n");
              else
                printf ("%06X\n", c & 0xFFFFFF);
            }
          else if (g_ascii_strcasecmp (arg, "none") == 0)
            o42_sheet_set_tab_colour (sheet, O42_TAB_NO_COLOUR);
          else
            o42_sheet_set_tab_colour (sheet, (guint32) g_ascii_strtoull (arg, NULL, 16) & 0xFFFFFF);
          continue;
        }

      if (g_str_has_prefix (text, "rename "))
        {
          if (!o42_book_rename_sheet (book, o42_book_sheet_index (book, sheet), text + 7))
            fprintf (stderr, "cannot rename to %s\n", text + 7);
          continue;
        }

      if (strcmp (text, "delsheet") == 0)
        {
          if (o42_book_remove_sheet (book, o42_book_sheet_index (book, sheet)))
            sheet = o42_book_sheet (book, 0);
          else
            fprintf (stderr, "cannot delete the only sheet\n");
          continue;
        }

      if (g_str_has_prefix (text, "load ") || g_str_has_prefix (text, "save "))
        {
          const char *path = text + 5;
          GFile *file = g_file_new_for_path (path);
          GError *error = NULL;
          gboolean csv = g_str_has_suffix (path, ".csv");
          gboolean xlsx = g_str_has_suffix (path, ".xlsx");
          gboolean xls = g_str_has_suffix (path, ".xls");
          gboolean ods = g_str_has_suffix (path, ".ods");
          gboolean html = g_str_has_suffix (path, ".html") || g_str_has_suffix (path, ".htm");
          gboolean dif = g_str_has_suffix (path, ".dif");
          gboolean sylk = g_str_has_suffix (path, ".slk") || g_str_has_suffix (path, ".sylk");
          gboolean latex = g_str_has_suffix (path, ".tex");
          gboolean ok;

          if (text[0] == 'l')
            ok = csv ? o42_csv_load (sheet, file, &error)
               : xlsx ? o42_xlsx_load (book, file, &error)
               : xls ? o42_xls_load (book, file, &error)
               : ods ? o42_ods_load (book, file, &error)
               : html ? o42_html_load (sheet, file, &error)
               : dif ? o42_dif_load (sheet, file, &error)
               : sylk ? o42_sylk_load (sheet, file, &error)
                     : o42_gnumeric_load (book, file, &error);
          else
            ok = csv ? o42_csv_save (sheet, file, &error)
               : xlsx ? o42_xlsx_save (book, file, &error)
               : xls ? o42_xls_save (book, file, &error)
               : ods ? o42_ods_save (book, file, &error)
               : html ? o42_html_save (book, file, &error)
               : dif ? o42_dif_save (sheet, file, &error)
               : sylk ? o42_sylk_save (sheet, file, &error)
               : latex ? o42_latex_save (sheet, file, &error)
                     : o42_gnumeric_save (book, file, &error);

          if (ok && xls && text[0] == 115 && o42_xls_dropped_cells > 0)
            fprintf (stderr, "%s: %d cells outside the 65,536 rows by 256 "
                             "columns Excel 97 holds were not written\n",
                     path, o42_xls_dropped_cells);

          if (ok && !csv && !html && !dif && !sylk && text[0] == 'l')
            sheet = o42_book_sheet (book, 0);

          if (!ok)
            {
              fprintf (stderr, "%s: %s\n", path,
                       error ? error->message : "failed");
              g_clear_error (&error);
            }
          g_object_unref (file);
          continue;
        }

      if (g_str_has_prefix (text, "autofill "))
        {
          /* autofill A1:A2 A1:A10 */
          char **words = g_strsplit (text, " ", -1);
          O42Range src, dst;
          gsize l1 = 0, l2 = 0;

          if (g_strv_length (words) >= 3 &&
              o42_ref_parse (words[1], &src.row0, &src.col0, &l1) && words[1][l1] == ':' &&
              o42_ref_parse (words[1] + l1 + 1, &src.row1, &src.col1, NULL) &&
              o42_ref_parse (words[2], &dst.row0, &dst.col0, &l2) && words[2][l2] == ':' &&
              o42_ref_parse (words[2] + l2 + 1, &dst.row1, &dst.col1, NULL))
            {
              src = o42_range_normalise (src.row0, src.col0, src.row1, src.col1);
              dst = o42_range_normalise (dst.row0, dst.col0, dst.row1, dst.col1);
              o42_sheet_autofill (sheet, &src, &dst);
            }
          else
            fprintf (stderr, "usage: autofill A1:A2 A1:A10\n");

          g_strfreev (words);
          continue;
        }

      /* format A1:B2 CODE gives cells a format string. */
      /* font A1:B2 bold,italic,size=14,colour=CC0000,fill=FFFF99;
       * fontinfo A1 prints what a cell wears. */
      if (g_str_has_prefix (text, "font ") || g_str_has_prefix (text, "fontinfo "))
        {
          gboolean query = text[4] == 'i';
          char **words = g_strsplit (text + (query ? 9 : 5), " ", 2);
          O42Range r;
          gsize len = 0;

          if (g_strv_length (words) >= (query ? 1u : 2u) &&
              o42_ref_parse (words[0], &r.row0, &r.col0, &len) &&
              (words[0][len] == '\0' ||
               (words[0][len] == ':' && o42_ref_parse (words[0] + len + 1, &r.row1, &r.col1, NULL))))
            {
              if (words[0][len] == '\0') { r.row1 = r.row0; r.col1 = r.col0; }
              r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);

              if (query)
                {
                  const O42Fmt *f = o42_sheet_get_fmt (sheet, r.row0, r.col0);
                  char *number = o42_fmt_format_string (f);

                  printf ("%s %g%s%s%s%s colour %06X fill ", f->family, f->size / 2.0,
                          f->bold ? " bold" : "", f->italic ? " italic" : "",
                          f->underline ? " underline" : "", f->strikeout ? " strike" : "",
                          f->colour);
                  if (f->fill == O42_FILL_NONE)
                    printf ("none");
                  else
                    printf ("%06X", f->fill);
                  printf (" number %s halign %d valign %d\n", number, f->halign, f->valign);
                  g_free (number);
                }
              else
                {
                  O42Fmt want = *o42_sheet_get_fmt (sheet, r.row0, r.col0);
                  O42FmtMask mask = 0;
                  char **attrs = g_strsplit (words[1], ",", -1);

                  for (int i = 0; attrs[i] != NULL; i++)
                    {
                      const char *a = attrs[i];

                      if (strcmp (a, "bold") == 0) { want.bold = 1; mask |= O42_FMT_BOLD; }
                      else if (strcmp (a, "nobold") == 0) { want.bold = 0; mask |= O42_FMT_BOLD; }
                      else if (strcmp (a, "italic") == 0) { want.italic = 1; mask |= O42_FMT_ITALIC; }
                      else if (strcmp (a, "underline") == 0) { want.underline = 1; mask |= O42_FMT_UNDERLINE; }
                      else if (strcmp (a, "strike") == 0) { want.strikeout = 1; mask |= O42_FMT_STRIKEOUT; }
                      else if (strcmp (a, "wrap") == 0) { want.wrap = 1; mask |= O42_FMT_WRAP; }
                      else if (g_str_has_prefix (a, "size=")) { want.size = (guint8) (g_ascii_strtod (a + 5, NULL) * 2); mask |= O42_FMT_SIZE; }
                      else if (g_str_has_prefix (a, "family=")) { want.family = g_intern_string (a + 7); mask |= O42_FMT_FAMILY; }
                      else if (g_str_has_prefix (a, "colour=")) { want.colour = (guint32) g_ascii_strtoull (a + 7, NULL, 16); mask |= O42_FMT_COLOUR; }
                      else if (strcmp (a, "fill=none") == 0) { want.fill = O42_FILL_NONE; mask |= O42_FMT_FILL; }
                      else if (g_str_has_prefix (a, "fill=")) { want.fill = (guint32) g_ascii_strtoull (a + 5, NULL, 16); mask |= O42_FMT_FILL; }
                      else if (g_str_has_prefix (a, "halign=")) { want.halign = (O42HAlign) atoi (a + 7); mask |= O42_FMT_HALIGN; }
                      else if (g_str_has_prefix (a, "valign=")) { want.valign = (O42VAlign) atoi (a + 7); mask |= O42_FMT_VALIGN; }
                      else fprintf (stderr, "font: unknown attribute %s\n", a);
                    }
                  if (mask != 0)
                    o42_sheet_apply_fmt (sheet, &r, mask, &want);
                  g_strfreev (attrs);
                }
            }
          else
            fprintf (stderr, "usage: font A1:B2 bold,size=14,colour=RRGGBB; fontinfo A1\n");
          g_strfreev (words);
          continue;
        }

      if (g_str_has_prefix (text, "format "))
        {
          char *rest = text + 7;
          char *space = strchr (rest, ' ');
          O42Range r;
          gsize len = 0;

          if (space != NULL)
            {
              *space = '\0';
              if (o42_ref_parse (rest, &r.row0, &r.col0, &len) &&
                  (rest[len] == '\0' ||
                   (rest[len] == ':' && o42_ref_parse (rest + len + 1, &r.row1, &r.col1, NULL))))
                {
                  O42Fmt want;

                  if (rest[len] == '\0') { r.row1 = r.row0; r.col1 = r.col0; }
                  r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
                  o42_fmt_init_default (&want);
                  if (!o42_number_format_parse (space + 1, &want.number, &want.decimals) ||
                      g_ascii_strcasecmp (space + 1, "General") != 0)
                    {
                      char *ours = o42_number_format_to_string (want.number, want.decimals);
                      if (g_ascii_strcasecmp (ours, space + 1) != 0)
                        {
                          want.number = O42_NUM_GENERAL;
                          want.custom = g_intern_string (space + 1);
                        }
                      g_free (ours);
                    }
                  o42_sheet_apply_fmt (sheet, &r, O42_FMT_NUMBER | O42_FMT_DECIMALS, &want);
                  continue;
                }
            }
          fprintf (stderr, "usage: format A1:B2 CODE\n");
          continue;
        }

      /* solve TARGET max|min|VALUE A1,B1 [A2<=10] [B2>=0] ... */
      if (g_str_has_prefix (text, "solve "))
        {
          char **words = g_strsplit (text + 6, " ", -1);
          int n = (int) g_strv_length (words);
          int trow, tcol;
          O42SolverGoal goal = O42_SOLVER_MAX;
          double goal_value = 0;
          O42Ref changing[16];
          int n_changing = 0;
          O42SolverBound bounds[16];
          int n_bounds = 0;

          if (n >= 3 && o42_ref_parse (words[0], &trow, &tcol, NULL))
            {
              char **cells;

              if (strcmp (words[1], "max") == 0) goal = O42_SOLVER_MAX;
              else if (strcmp (words[1], "min") == 0) goal = O42_SOLVER_MIN;
              else { goal = O42_SOLVER_VALUE; goal_value = g_ascii_strtod (words[1], NULL); }

              cells = g_strsplit (words[2], ",", -1);
              for (int i = 0; cells[i] != NULL && n_changing < 16; i++)
                if (o42_ref_parse (cells[i], &changing[n_changing].row, &changing[n_changing].col, NULL))
                  n_changing++;
              g_strfreev (cells);

              for (int i = 3; i < n && n_bounds < 16; i++)
                {
                  const char *op = strstr (words[i], "<=");
                  O42SolverOp which = O42_SOLVER_LE;
                  char *cell;

                  if (op == NULL) { op = strstr (words[i], ">="); which = O42_SOLVER_GE; }
                  if (op == NULL) { op = strchr (words[i], '='); which = O42_SOLVER_EQ; }
                  if (op == NULL) continue;
                  cell = g_strndup (words[i], (gsize) (op - words[i]));
                  if (o42_ref_parse (cell, &bounds[n_bounds].row, &bounds[n_bounds].col, NULL))
                    {
                      bounds[n_bounds].op = which;
                      bounds[n_bounds].value = g_ascii_strtod (op + (which == O42_SOLVER_EQ ? 1 : 2), NULL);
                      n_bounds++;
                    }
                  g_free (cell);
                }

              if (n_changing > 0)
                {
                  double reached = 0;
                  gboolean ok = o42_sheet_solve (sheet, trow, tcol, goal, goal_value,
                                                 changing, n_changing, bounds, n_bounds, &reached);
                  printf ("%s %g\n", ok ? "reached" : "gave up at", reached);
                }
              else
                fprintf (stderr, "usage: solve TARGET max|min|VALUE A1,B1 [A2<=10]...\n");
            }
          else
            fprintf (stderr, "usage: solve TARGET max|min|VALUE A1,B1 [A2<=10]...\n");
          g_strfreev (words);
          continue;
        }

      /* advfilter A1:C9 E1:F3 [DEST] [unique] */
      if (g_str_has_prefix (text, "advfilter "))
        {
          char **words = g_strsplit (text + 10, " ", -1);
          int n = (int) g_strv_length (words);
          O42Range list, criteria;
          gsize l1 = 0, l2 = 0;
          int drow = -1, dcol = 0;
          gboolean unique = FALSE;

          if (n >= 2 &&
              o42_ref_parse (words[0], &list.row0, &list.col0, &l1) && words[0][l1] == ':' &&
              o42_ref_parse (words[0] + l1 + 1, &list.row1, &list.col1, NULL) &&
              o42_ref_parse (words[1], &criteria.row0, &criteria.col0, &l2) && words[1][l2] == ':' &&
              o42_ref_parse (words[1] + l2 + 1, &criteria.row1, &criteria.col1, NULL))
            {
              for (int i = 2; i < n; i++)
                {
                  if (strcmp (words[i], "unique") == 0) unique = TRUE;
                  else o42_ref_parse (words[i], &drow, &dcol, NULL);
                }
              printf ("%d rows\n", o42_sheet_advanced_filter (sheet, &list, &criteria, drow, dcol, unique));
            }
          else
            fprintf (stderr, "usage: advfilter A1:C9 E1:F3 [DEST] [unique]\n");
          g_strfreev (words);
          continue;
        }

      /* consolidate DEST sum|count|average|min|max [labels] RANGE... */
      if (g_str_has_prefix (text, "consolidate "))
        {
          char **words = g_strsplit (text + 12, " ", -1);
          int n = (int) g_strv_length (words);
          int drow, dcol;
          static const char *aggs[] = { "sum", "count", "average", "min", "max" };
          O42PivotAgg agg = O42_PIVOT_SUM;
          gboolean labels = FALSE;
          O42SheetRange sources[16];
          int n_sources = 0;

          if (n >= 3 && o42_ref_parse (words[0], &drow, &dcol, NULL))
            {
              for (guint i = 0; i < G_N_ELEMENTS (aggs); i++)
                if (strcmp (words[1], aggs[i]) == 0) agg = (O42PivotAgg) i;
              for (int i = 2; i < n && n_sources < 16; i++)
                {
                  char *spec = words[i];
                  char *bang = strchr (spec, '!');
                  gsize len = 0;

                  if (strcmp (spec, "labels") == 0) { labels = TRUE; continue; }
                  memset (&sources[n_sources], 0, sizeof sources[0]);
                  if (bang != NULL)
                    {
                      *bang = '\0';
                      sources[n_sources].sheet = g_intern_string (spec);
                      spec = bang + 1;
                    }
                  if (o42_ref_parse (spec, &sources[n_sources].range.row0, &sources[n_sources].range.col0, &len) &&
                      spec[len] == ':' &&
                      o42_ref_parse (spec + len + 1, &sources[n_sources].range.row1, &sources[n_sources].range.col1, NULL))
                    n_sources++;
                }
              if (n_sources > 0)
                {
                  O42Range made = o42_sheet_consolidate (sheet, sources, n_sources, drow, dcol, agg, labels, labels);
                  char *a = o42_ref_name (made.row0, made.col0), *b = o42_ref_name (made.row1, made.col1);
                  printf ("%s:%s\n", a, b);
                  g_free (a); g_free (b);
                }
              else
                fprintf (stderr, "usage: consolidate A1 sum [labels] A1:B3 Sheet2!A1:B3\n");
            }
          else
            fprintf (stderr, "usage: consolidate A1 sum [labels] A1:B3 Sheet2!A1:B3\n");
          g_strfreev (words);
          continue;
        }

      /* scenario NAME A1:B2 [COMMENT] saves the cells' values; showscenario
       * NAME puts them back; scenarios lists them; delscenario NAME. */
      if (g_str_has_prefix (text, "scenario ") || g_str_has_prefix (text, "showscenario ") ||
          g_str_has_prefix (text, "delscenario ") || strcmp (text, "scenarios") == 0)
        {
          if (strcmp (text, "scenarios") == 0)
            {
              for (int i = 0; i < o42_sheet_n_scenarios (sheet); i++)
                {
                  const char *name = o42_sheet_scenario_name (sheet, i);
                  GArray *keys = NULL;
                  const char *comment = NULL;
                  o42_sheet_scenario_cells (sheet, name, &keys, NULL, &comment);
                  printf ("%s: %u cells%s%s\n", name, keys != NULL ? keys->len : 0,
                          comment != NULL && *comment ? ", " : "", comment != NULL ? comment : "");
                }
            }
          else if (g_str_has_prefix (text, "showscenario "))
            {
              if (!o42_sheet_show_scenario (sheet, text + 13))
                fprintf (stderr, "no such scenario\n");
            }
          else if (g_str_has_prefix (text, "delscenario "))
            {
              if (!o42_sheet_remove_scenario (sheet, text + 12))
                fprintf (stderr, "no such scenario\n");
            }
          else
            {
              char **words = g_strsplit (text + 9, " ", 3);
              O42Range r;
              gsize len = 0;

              if (g_strv_length (words) >= 2 &&
                  o42_ref_parse (words[1], &r.row0, &r.col0, &len) &&
                  (words[1][len] == '\0' ||
                   (words[1][len] == ':' && o42_ref_parse (words[1] + len + 1, &r.row1, &r.col1, NULL))))
                {
                  if (words[1][len] == '\0') { r.row1 = r.row0; r.col1 = r.col0; }
                  o42_sheet_add_scenario (sheet, words[0], &r, g_strv_length (words) >= 3 ? words[2] : "");
                }
              else
                fprintf (stderr, "usage: scenario NAME A1:B2 [COMMENT]\n");
              g_strfreev (words);
            }
          continue;
        }

      /* style NAME A1:B2 applies a cell style; styles lists them;
       * defstyle NAME A1 defines one from a cell's format. */
      if (g_str_has_prefix (text, "style ") || strcmp (text, "styles") == 0 ||
          g_str_has_prefix (text, "defstyle "))
        {
          if (strcmp (text, "styles") == 0)
            {
              for (int i = 0; i < o42_book_n_styles (book); i++)
                printf ("%s\n", o42_book_style_name (book, i));
            }
          else if (text[0] == 'd')
            {
              char **words = g_strsplit (text + 9, " ", 2);
              int srow, scol;

              if (g_strv_length (words) >= 2 && o42_ref_parse (words[1], &srow, &scol, NULL))
                o42_book_set_style (book, words[0], o42_sheet_get_fmt (sheet, srow, scol), O42_FMT_ALL);
              else
                fprintf (stderr, "usage: defstyle NAME A1\n");
              g_strfreev (words);
            }
          else
            {
              char **words = g_strsplit (text + 6, " ", 2);
              O42Range r;
              gsize len = 0;

              if (g_strv_length (words) >= 2 &&
                  o42_ref_parse (words[1], &r.row0, &r.col0, &len) &&
                  (words[1][len] == '\0' ||
                   (words[1][len] == ':' && o42_ref_parse (words[1] + len + 1, &r.row1, &r.col1, NULL))))
                {
                  if (words[1][len] == '\0') { r.row1 = r.row0; r.col1 = r.col0; }
                  r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
                  o42_sheet_apply_style (sheet, &r, strcmp (words[0], "none") == 0 ? NULL : words[0]);
                }
              else
                fprintf (stderr, "usage: style NAME A1:B2\n");
              g_strfreev (words);
            }
          continue;
        }
      if (g_str_has_prefix (text, "styleat "))
        {
          int srow, scol;
          if (o42_ref_parse (text + 8, &srow, &scol, NULL))
            {
              const char *name = o42_sheet_cell_style (sheet, srow, scol);
              printf ("%s\n", name != NULL ? name : "(none)");
            }
          continue;
        }

      /* moverange A1:B2 D5 */
      if (g_str_has_prefix (text, "moverange "))
        {
          char **words = g_strsplit (text + 10, " ", -1);
          O42Range r;
          gsize len = 0;
          int drow, dcol;

          if (g_strv_length (words) >= 2 &&
              o42_ref_parse (words[0], &r.row0, &r.col0, &len) && words[0][len] == ':' &&
              o42_ref_parse (words[0] + len + 1, &r.row1, &r.col1, NULL) &&
              o42_ref_parse (words[1], &drow, &dcol, NULL))
            o42_sheet_move_range (sheet, &r, drow, dcol);
          else
            fprintf (stderr, "usage: moverange A1:B2 D5\n");
          g_strfreev (words);
          continue;
        }

      /* chartsheet on|off */
      if (g_str_has_prefix (text, "chartsheet "))
        {
          o42_sheet_set_chart_sheet (sheet, strcmp (text + 11, "on") == 0);
          continue;
        }

      /* view add NAME; view show NAME; view del NAME; views */
      if (g_str_has_prefix (text, "view ") || strcmp (text, "views") == 0)
        {
          if (strcmp (text, "views") == 0)
            {
              for (int i = 0; i < o42_book_n_views (book); i++)
                {
                  const O42BookView *v = o42_book_view (book, i);
                  char *first = o42_ref_name (v->selection.row0, v->selection.col0);
                  char *last = o42_ref_name (v->selection.row1, v->selection.col1);

                  printf ("%s: sheet %s selection %s:%s zoom %g frozen %d,%d split %s\n",
                          v->name, v->sheet, first, last, v->zoom,
                          v->frozen_rows, v->frozen_cols, v->split ? "on" : "off");
                  g_free (first);
                  g_free (last);
                }
            }
          else if (g_str_has_prefix (text + 5, "add ") && text[9] != '\0')
            {
              O42BookView view;

              memset (&view, 0, sizeof view);
              view.name = (char *) (text + 9);
              view.sheet = (char *) o42_sheet_get_name (sheet);
              view.zoom = 1.0;
              o42_book_set_view (book, &view);
            }
          else if (g_str_has_prefix (text + 5, "show ") && text[10] != '\0')
            {
              const O42BookView *v = o42_book_find_view (book, text + 10);
              O42Sheet *wanted = v != NULL ? o42_book_find_sheet (book, v->sheet) : NULL;

              if (wanted != NULL)
                sheet = wanted;
              else if (v == NULL)
                fprintf (stderr, "no such view\n");
            }
          else if (g_str_has_prefix (text + 5, "del ") && text[9] != '\0')
            {
              if (!o42_book_remove_view (book, text + 9))
                fprintf (stderr, "no such view\n");
            }
          else
            fprintf (stderr, "usage: view add|show|del NAME; views\n");
          continue;
        }

      /* record on; record off [NAME] -- writes the macro, or keeps it
       * in the book under a name */
      if (g_str_has_prefix (text, "record "))
        {
          const char *what = text + 7;

          if (g_str_has_prefix (what, "on"))
            {
              o42_book_record_start (book);
              printf ("recording\n");
            }
          else
            {
              char *script = o42_book_record_stop (book);

              if (script == NULL)
                printf ("not recording\n");
              else if (g_str_has_prefix (what, "off ") && what[4] != '\0')
                {
                  o42_book_set_script (book, what + 4, script);
                  printf ("recorded %s, %d lines\n", what + 4,
                          (int) g_strv_length (g_strsplit (script, "\n", -1)) - 1);
                }
              else
                fputs (script, stdout);
              g_free (script);
            }
          continue;
        }

      /* analyse TOOL A1:C10 E1 [labels] [rows] [N] */
      if (g_str_has_prefix (text, "analyse ") || g_str_has_prefix (text, "analyze "))
        {
          char **words = g_strsplit (text + 8, " ", -1);
          O42AnalysisOptions options;
          gsize len = 0;
          int n_words = (int) g_strv_length (words);

          memset (&options, 0, sizeof options);
          options.confidence = 0.95;
          options.layout = O42_ANALYSIS_COLUMNS;

          if (n_words >= 3 &&
              o42_ref_parse (words[1], &options.input.row0, &options.input.col0, &len) &&
              words[1][len] == ':' &&
              o42_ref_parse (words[1] + len + 1, &options.input.row1, &options.input.col1, NULL) &&
              o42_ref_parse (words[2], &options.out_row, &options.out_col, NULL))
            {
              gboolean ok;

              for (int i = 3; i < n_words; i++)
                {
                  if (strcmp (words[i], "labels") == 0) options.labels = TRUE;
                  else if (strcmp (words[i], "rows") == 0) options.layout = O42_ANALYSIS_ROWS;
                  else if (g_str_has_prefix (words[i], "bins=")) options.bins = atoi (words[i] + 5);
                  else if (g_str_has_prefix (words[i], "terms=")) options.interval = atoi (words[i] + 6);
                  else if (g_str_has_prefix (words[i], "per=")) options.per_sample = atoi (words[i] + 4);
                  else if (g_str_has_prefix (words[i], "size=")) options.sample_size = atoi (words[i] + 5);
                  else if (strcmp (words[i], "periodic") == 0) options.periodic = TRUE;
                  else if (g_str_has_prefix (words[i], "confidence="))
                    options.confidence = g_ascii_strtod (words[i] + 11, NULL);
                }

              if (strcmp (words[0], "descriptive") == 0)
                ok = o42_analysis_descriptive (sheet, &options);
              else if (strcmp (words[0], "correlation") == 0)
                ok = o42_analysis_correlation (sheet, &options);
              else if (strcmp (words[0], "covariance") == 0)
                ok = o42_analysis_covariance (sheet, &options);
              else if (strcmp (words[0], "regression") == 0)
                ok = o42_analysis_regression (sheet, &options);
              else if (strcmp (words[0], "histogram") == 0)
                ok = o42_analysis_histogram (sheet, &options);
              else if (strcmp (words[0], "anova") == 0)
                ok = o42_analysis_anova (sheet, &options);
              else if (strcmp (words[0], "anova2") == 0)
                ok = o42_analysis_anova2 (sheet, &options);
              else if (strcmp (words[0], "sampling") == 0)
                ok = o42_analysis_sampling (sheet, &options);
              else if (strcmp (words[0], "rank") == 0)
                ok = o42_analysis_rank (sheet, &options);
              else if (strcmp (words[0], "moving") == 0)
                ok = o42_analysis_moving (sheet, &options);
              else
                {
                  fprintf (stderr, "analyse: descriptive, correlation, covariance, regression,"
                                   " histogram, anova, anova2, sampling, rank or moving\n");
                  ok = TRUE;
                }
              if (!ok)
                fprintf (stderr, "analyse: not enough data for that\n");
            }
          else
            fprintf (stderr, "usage: analyse TOOL A1:C10 E1 [labels] [rows] [bins=N] [terms=N]\n");
          g_strfreev (words);
          continue;
        }

      /* spell A1:C9 */
      if (g_str_has_prefix (text, "spell ") || strcmp (text, "spell") == 0)
        {
          O42Range r;
          gsize len = 0;
          O42Spell *spell = o42_spell_new (NULL);

          if (spell == NULL)
            {
              printf ("no dictionary\n");
              continue;
            }
          printf ("dictionary %s\n", o42_spell_language (spell));
          if (strcmp (text, "spell") == 0 ||
              !(o42_ref_parse (text + 6, &r.row0, &r.col0, &len) && text[6 + len] == ':' &&
                o42_ref_parse (text + 6 + len + 1, &r.row1, &r.col1, NULL)))
            o42_sheet_used_range (sheet, &r);
          else
            r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);

          for (int sr = r.row0; sr <= r.row1; sr++)
            for (int sc = r.col0; sc <= r.col1; sc++)
              {
                O42Value v;
                char *shown;

                o42_sheet_get_value (sheet, sr, sc, &v);
                if (v.type != O42_VALUE_TEXT)
                  {
                    o42_value_clear (&v);
                    continue;
                  }
                o42_value_clear (&v);
                shown = o42_sheet_get_display (sheet, sr, sc);
                {
                  SpellWalk walk = { spell, sheet, sr, sc };
                  o42_spell_words (shown, spell_word, &walk);
                }
                g_free (shown);
              }
          o42_spell_free (spell);
          continue;
        }

      /* rich A1 START END ATTR[,ATTR...]; runs A1 */
      if (g_str_has_prefix (text, "rich ") || g_str_has_prefix (text, "runs "))
        {
          char **words = g_strsplit (text + 5, " ", 4);
          int rrow, rcol;

          if (text[0] == 'r' && text[1] == 'u')
            {
              if (o42_ref_parse (words[0], &rrow, &rcol, NULL))
                {
                  int n_runs = 0;
                  const O42TextRun *runs = o42_sheet_runs (sheet, rrow, rcol, &n_runs);

                  for (int i = 0; i < n_runs; i++)
                    printf ("run %d at %d: %s %d%s%s%s%s %06X\n", i, runs[i].start,
                            runs[i].fmt.family, runs[i].fmt.size / 2,
                            runs[i].fmt.bold ? " bold" : "",
                            runs[i].fmt.italic ? " italic" : "",
                            runs[i].fmt.underline ? " underline" : "",
                            runs[i].fmt.strikeout ? " strike" : "",
                            runs[i].fmt.colour);
                  if (n_runs == 0)
                    printf ("plain\n");
                }
            }
          else if (g_strv_length (words) >= 4 && o42_ref_parse (words[0], &rrow, &rcol, NULL))
            {
              /* One run of the given look between the two offsets, the
               * cell's own format on either side of it. */
              O42TextRun runs[3];
              int start = atoi (words[1]);
              int end = atoi (words[2]);
              char **attrs = g_strsplit (words[3], ",", -1);
              char *shown = o42_sheet_get_display (sheet, rrow, rcol);
              int length = (int) strlen (shown);
              int n = 0;

              runs[0].start = 0;
              runs[0].fmt = *o42_sheet_get_fmt (sheet, rrow, rcol);
              runs[1].start = CLAMP (start, 0, length);
              runs[1].fmt = runs[0].fmt;
              for (int i = 0; attrs[i] != NULL; i++)
                {
                  const char *a = attrs[i];

                  if (strcmp (a, "bold") == 0) runs[1].fmt.bold = 1;
                  else if (strcmp (a, "italic") == 0) runs[1].fmt.italic = 1;
                  else if (strcmp (a, "underline") == 0) runs[1].fmt.underline = 1;
                  else if (strcmp (a, "strike") == 0) runs[1].fmt.strikeout = 1;
                  else if (g_str_has_prefix (a, "size=")) runs[1].fmt.size = (guint8) (atoi (a + 5) * 2);
                  else if (g_str_has_prefix (a, "colour=")) runs[1].fmt.colour = (guint32) g_ascii_strtoull (a + 7, NULL, 16);
                  else if (g_str_has_prefix (a, "font=")) runs[1].fmt.family = g_intern_string (a + 5);
                  else fprintf (stderr, "rich: unknown attribute %s\n", a);
                }
              runs[2].start = CLAMP (end, 0, length);
              runs[2].fmt = runs[0].fmt;
              n = (runs[2].start > runs[1].start && runs[2].start < length) ? 3 : 2;
              o42_sheet_set_runs (sheet, rrow, rcol, runs, n);
              g_strfreev (attrs);
              g_free (shown);
            }
          else
            fprintf (stderr, "usage: rich A1 START END bold,italic,size=12,colour=FF0000\n");
          g_strfreev (words);
          continue;
        }

      /* pattern A1:B2 NAME [RRGGBB] */
      if (g_str_has_prefix (text, "pattern "))
        {
          char **words = g_strsplit (text + 8, " ", 4);
          O42Range r;
          gsize len = 0;
          O42Pattern pattern;

          if (g_strv_length (words) >= 2 &&
              o42_ref_parse (words[0], &r.row0, &r.col0, &len) &&
              (words[0][len] == '\0' ||
               (words[0][len] == ':' && o42_ref_parse (words[0] + len + 1, &r.row1, &r.col1, NULL))) &&
              o42_pattern_parse (words[1], &pattern))
            {
              O42Fmt want = *o42_sheet_get_fmt (sheet, r.row0, r.col0);

              if (words[0][len] == '\0') { r.row1 = r.row0; r.col1 = r.col0; }
              r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
              want.pattern = (guint8) pattern;
              if (g_strv_length (words) >= 3)
                want.pattern_colour = (guint32) g_ascii_strtoull (words[2], NULL, 16);
              if (g_strv_length (words) >= 4)
                want.fill = (guint32) g_ascii_strtoull (words[3], NULL, 16);
              o42_sheet_apply_fmt (sheet, &r, O42_FMT_PATTERN | O42_FMT_FILL, &want);
            }
          else
            fprintf (stderr, "usage: pattern A1:B2 NAME [PATTERN-RRGGBB [SHADING-RRGGBB]]\n");
          g_strfreev (words);
          continue;
        }

      /* protect on|off; lock A1:B2 on|off; hide A1:B2 on|off */
      if (g_str_has_prefix (text, "protect ") || g_str_has_prefix (text, "lock ") ||
          g_str_has_prefix (text, "hide "))
        {
          char **words = g_strsplit (text, " ", -1);
          int n = (int) g_strv_length (words);

          if (words[0][0] == 'p' && n >= 2)
            o42_sheet_set_protected (sheet, strcmp (words[1], "on") == 0);
          else if (n >= 3)
            {
              O42Range r;
              gsize len = 0;
              if (o42_ref_parse (words[1], &r.row0, &r.col0, &len) &&
                  (words[1][len] == '\0' ||
                   (words[1][len] == ':' && o42_ref_parse (words[1] + len + 1, &r.row1, &r.col1, NULL))))
                {
                  O42Fmt want = *o42_sheet_get_fmt (sheet, r.row0, r.col0);
                  if (words[1][len] == '\0') { r.row1 = r.row0; r.col1 = r.col0; }
                  r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
                  if (words[0][0] == 'l') want.locked = strcmp (words[2], "on") == 0;
                  else want.hidden = strcmp (words[2], "on") == 0;
                  o42_sheet_apply_fmt (sheet, &r, O42_FMT_PROTECTION, &want);
                }
            }
          else
            fprintf (stderr, "usage: protect on|off; lock A1:B2 on|off; hide A1:B2 on|off\n");
          g_strfreev (words);
          continue;
        }
      if (g_str_has_prefix (text, "editable "))
        {
          int erow, ecol;
          if (o42_ref_parse (text + 9, &erow, &ecol, NULL))
            printf ("%s %s\n", o42_sheet_cell_editable (sheet, erow, ecol) ? "editable" : "locked",
                    o42_sheet_formula_hidden (sheet, erow, ecol) ? "hidden" : "shown");
          continue;
        }

      /* objgroup A1:D10; objungroup A1:D10 -- "group" is taken by the
       * outline grouping of rows and columns. */
      if (g_str_has_prefix (text, "objgroup ") || g_str_has_prefix (text, "objungroup "))
        {
          gboolean grouping = g_str_has_prefix (text, "objgroup ");
          const char *where = text + (grouping ? 9 : 11);
          O42Range r;
          gsize len = 0;

          if (o42_ref_parse (where, &r.row0, &r.col0, &len) && where[len] == ':' &&
              o42_ref_parse (where + len + 1, &r.row1, &r.col1, NULL))
            {
              if (grouping)
                {
                  guint id = o42_sheet_group_objects (sheet, &r);

                  if (id == 0)
                    fprintf (stderr, "nothing there to group\n");
                  else
                    printf ("group %u\n", id);
                }
              else if (!o42_sheet_ungroup_objects (sheet, &r))
                fprintf (stderr, "nothing there was grouped\n");
            }
          else
            fprintf (stderr, "usage: objgroup A1:D10; objungroup A1:D10\n");
          continue;
        }

      /* picture PATH A1; pictures */
      if (g_str_has_prefix (text, "picture ") || strcmp (text, "pictures") == 0)
        {
          if (strcmp (text, "pictures") == 0)
            {
              GPtrArray *list = o42_sheet_pictures (sheet);

              for (guint i = 0; i < list->len; i++)
                {
                  const O42Picture *pic = g_ptr_array_index (list, i);
                  char *at = o42_ref_name (pic->row, pic->col);

                  printf ("picture %u: %s at %s %gx%g\n", pic->id,
                          pic->format != NULL ? pic->format : "?", at, pic->width, pic->height);
                  g_free (at);
                }
            }
          else
            {
              char **words = g_strsplit (text + 8, " ", 2);
              int prow, pcol;
              char *data = NULL;
              gsize length = 0;

              if (g_strv_length (words) >= 2 && o42_ref_parse (words[1], &prow, &pcol, NULL) &&
                  g_file_get_contents (words[0], &data, &length, NULL))
                {
                  GBytes *bytes = g_bytes_new_take (data, length);
                  int pw = 0, ph = 0;
                  const char *format = NULL;

                  data = NULL;
                  if (o42_image_probe (bytes, &pw, &ph, &format))
                    o42_sheet_add_picture (sheet, bytes, format, pw, ph, prow, pcol);
                  else
                    fprintf (stderr, "not a picture office42 knows\n");
                  g_bytes_unref (bytes);
                }
              else
                fprintf (stderr, "usage: picture PATH A1\n");
              g_free (data);
              g_strfreev (words);
            }
          continue;
        }

      /* shape rectangle|oval|line|arrow|textbox A1 [TEXT]; shapes */
      if (g_str_has_prefix (text, "shape ") || strcmp (text, "shapes") == 0)
        {
          if (strcmp (text, "shapes") == 0)
            {
              GPtrArray *shapes = o42_sheet_shapes (sheet);
              for (guint i = 0; i < shapes->len; i++)
                {
                  const O42Shape *sh = g_ptr_array_index (shapes, i);
                  char *at = o42_ref_name (sh->row, sh->col);
                  printf ("shape %u: %s at %s %gx%g group %u fill ", sh->id,
                          o42_shape_kind_name (sh->kind), at, sh->width, sh->height,
                          sh->group);
                  if (sh->fill == O42_FILL_NONE)
                    printf ("none");
                  else
                    printf ("%06X", sh->fill);
                  printf (" line %06X/%g \"%s\"", sh->line, sh->line_width,
                          sh->text != NULL ? sh->text : "");
                  if (o42_shape_is_control (sh->kind))
                    {
                      double v = 0;

                      printf (" link %s source %s script %s %g..%g by %g page %g",
                              sh->link != NULL ? sh->link : "-",
                              sh->source != NULL ? sh->source : "-",
                              sh->script != NULL ? sh->script : "-",
                              sh->min, sh->max, sh->step, sh->page);
                      if (o42_sheet_control_value (sheet, sh, &v))
                        printf (" reads %g", v);
                    }
                  printf ("\n");
                  g_free (at);
                }
            }
          else
            {
              char **words = g_strsplit (text + 6, " ", 3);
              O42ShapeKind kind;
              int srow, scol;

              if (g_strv_length (words) >= 2 && o42_shape_kind_parse (words[0], &kind) &&
                  o42_ref_parse (words[1], &srow, &scol, NULL))
                {
                  O42Shape *sh = o42_sheet_add_shape (sheet, kind, srow, scol);
                  if (sh != NULL && g_strv_length (words) >= 3)
                    { g_free (sh->text); sh->text = g_strdup (words[2]); }
                }
              else
                fprintf (stderr, "usage: shape rectangle|oval|line|arrow|textbox|"
                                 "button|checkbox|option|spinner|scrollbar|listbox|"
                                 "combo|label|groupbox A1 [TEXT]\n");
              g_strfreev (words);
            }
          continue;
        }

      /* db [FILE]; dbembed; dbtables; dbcols TABLE; dbexec STATEMENT;
       * sql A1 QUERY; sqlprint QUERY; dbput TABLE A1:C9; dbrefresh;
       * queries */
      if (g_str_has_prefix (text, "db") || g_str_has_prefix (text, "sql") ||
          strcmp (text, "queries") == 0)
        {
          GError *error = NULL;

          if (strcmp (text, "db") == 0)
            {
              gboolean embedded = FALSE;
              const char *path = o42_book_database (book, &embedded);

              printf ("%s%s\n", path != NULL ? path : "no database",
                      path != NULL && embedded ? " (inside the book)" : "");
              continue;
            }

          if (g_str_has_prefix (text, "db ") || strcmp (text, "dbembed") == 0)
            {
              char *path = NULL;
              gboolean embedded = text[2] != 32;

              if (embedded)
                {
                  int fd = g_file_open_tmp ("office42-XXXXXX.sqlite", &path, NULL);

                  if (fd >= 0)
                    g_close (fd, NULL);
                }
              else
                path = g_strdup (text + 3);

              o42_db_close (db);
              db = path != NULL ? o42_db_open (path, &error) : NULL;
              if (db == NULL)
                fprintf (stderr, "%s\n", error != NULL ? error->message : "no database");
              else
                o42_book_set_database (book, path, embedded);
              g_clear_error (&error);
              g_free (path);
              continue;
            }

          /* Everything below needs a database open; the book may name
           * one from the file it was loaded from. */
          if (db == NULL)
            {
              GError *why = NULL;

              db = o42_db_for_book (book, &why);
              if (db == NULL)
                {
                  fprintf (stderr, "%s",
                           o42_db_available () ? "" : "no SQLite in this build: ");
                  g_clear_error (&why);
                  fprintf (stderr, "no database: say db FILE or dbembed first\n");
                  continue;
                }
            }

          if (strcmp (text, "dbtables") == 0)
            {
              char **tables = o42_db_tables (db);

              for (int i = 0; tables != NULL && tables[i] != NULL; i++)
                printf ("%s\n", tables[i]);
              g_strfreev (tables);
            }
          else if (g_str_has_prefix (text, "dbcols "))
            {
              char **cols = o42_db_columns (db, text + 7);

              for (int i = 0; cols != NULL && cols[i] != NULL; i++)
                printf ("%s\n", cols[i]);
              g_strfreev (cols);
            }
          else if (g_str_has_prefix (text, "dbexec "))
            {
              int changed = 0;

              if (!o42_db_exec (db, text + 7, &changed, &error))
                fprintf (stderr, "%s\n", error->message);
              else
                printf ("%d row%s\n", changed, changed == 1 ? "" : "s");
              g_clear_error (&error);
            }
          else if (g_str_has_prefix (text, "sqlprint "))
            {
              O42DbResult *result = o42_db_query (db, text + 9, &error);

              if (result == NULL)
                fprintf (stderr, "%s\n", error->message);
              else
                {
                  for (int c = 0; c < result->cols; c++)
                    printf ("%s%s", c > 0 ? "\t" : "", result->names[c]);
                  printf ("\n");
                  for (int r = 0; r < result->rows; r++)
                    {
                      for (int c = 0; c < result->cols; c++)
                        {
                          char *shown = o42_value_display (&result->cells[r * result->cols + c]);

                          printf ("%s%s", c > 0 ? "\t" : "", shown);
                          g_free (shown);
                        }
                      printf ("\n");
                    }
                  o42_db_result_free (result);
                }
              g_clear_error (&error);
            }
          else if (g_str_has_prefix (text, "sql "))
            {
              char **words = g_strsplit (text + 4, " ", 2);
              int qrow, qcol;
              O42Range filled;

              if (g_strv_length (words) >= 2 &&
                  o42_ref_parse (words[0], &qrow, &qcol, NULL))
                {
                  if (!o42_db_query_into (db, sheet, words[1], qrow, qcol, TRUE, &filled, &error))
                    fprintf (stderr, "%s\n", error->message);
                  else
                    {
                      char *a = o42_ref_name (filled.row0, filled.col0);
                      char *b = o42_ref_name (filled.row1, filled.col1);

                      printf ("%s:%s\n", a, b);
                      g_free (a);
                      g_free (b);
                    }
                }
              else
                fprintf (stderr, "usage: sql A1 SELECT ...\n");
              g_clear_error (&error);
              g_strfreev (words);
            }
          else if (g_str_has_prefix (text, "dbput "))
            {
              char **words = g_strsplit (text + 6, " ", 3);
              O42Range r;
              gsize len = 0;

              if (g_strv_length (words) >= 2 &&
                  o42_ref_parse (words[1], &r.row0, &r.col0, &len) && words[1][len] == 58 &&
                  o42_ref_parse (words[1] + len + 1, &r.row1, &r.col1, NULL))
                {
                  gboolean headings = g_strv_length (words) < 3 ||
                                      strcmp (words[2], "noheadings") != 0;

                  if (!o42_db_put_range (db, sheet, &r, words[0], headings, &error))
                    fprintf (stderr, "%s\n", error->message);
                }
              else
                fprintf (stderr, "usage: dbput TABLE A1:C9 [noheadings]\n");
              g_clear_error (&error);
              g_strfreev (words);
            }
          else if (strcmp (text, "dbrefresh") == 0)
            {
              int done = o42_db_refresh (db, sheet, &error);

              if (done < 0)
                fprintf (stderr, "%s\n", error->message);
              else
                printf ("%d quer%s\n", done, done == 1 ? "y" : "ies");
              g_clear_error (&error);
            }
          else if (strcmp (text, "queries") == 0)
            {
              GArray *queries = o42_sheet_queries (sheet);

              for (guint i = 0; i < queries->len; i++)
                {
                  const O42Query *q = &g_array_index (queries, O42Query, i);
                  char *a = o42_ref_name (q->at.row0, q->at.col0);
                  char *b = o42_ref_name (q->at.row1, q->at.col1);

                  printf ("%s:%s %s\n", a, b, q->sql);
                  g_free (a);
                  g_free (b);
                }
            }
          else
            fprintf (stderr, "no such command\n");
          continue;
        }

      /* autoformat A1:D9 NAME */
      if (g_str_has_prefix (text, "autoformat "))
        {
          char **words = g_strsplit (text + 11, " ", 2);
          O42Range r;
          gsize len = 0;
          int which = 0;

          if (g_strv_length (words) >= 2 &&
              o42_ref_parse (words[0], &r.row0, &r.col0, &len) && words[0][len] == 58 &&
              o42_ref_parse (words[0] + len + 1, &r.row1, &r.col1, NULL) &&
              o42_auto_format_parse (words[1], &which))
            o42_sheet_auto_format (sheet, &r, which);
          else
            {
              fprintf (stderr, "usage: autoformat A1:D9 NAME, where NAME is one of:");
              for (int i = 0; i < o42_auto_format_count (); i++)
                fprintf (stderr, " %s", o42_auto_format_name (i));
              fprintf (stderr, "\n");
            }
          g_strfreev (words);
          continue;
        }

      /* whatif A1:D5 [row=B1] [col=B2] */
      if (g_str_has_prefix (text, "whatif "))
        {
          char **words = g_strsplit (text + 7, " ", -1);
          O42Range r;
          gsize len = 0;
          int rr = -1, rc = -1, cr = -1, cc = -1;

          if (g_strv_length (words) >= 2 &&
              o42_ref_parse (words[0], &r.row0, &r.col0, &len) && words[0][len] == 58 &&
              o42_ref_parse (words[0] + len + 1, &r.row1, &r.col1, NULL))
            {
              for (int i = 1; words[i] != NULL; i++)
                {
                  if (g_str_has_prefix (words[i], "row="))
                    o42_ref_parse (words[i] + 4, &rr, &rc, NULL);
                  else if (g_str_has_prefix (words[i], "col="))
                    o42_ref_parse (words[i] + 4, &cr, &cc, NULL);
                }
              if (!o42_sheet_data_table (sheet, &r, rr, rc, cr, cc))
                fprintf (stderr, "whatif: needs a row= or a col= input cell, and "
                                 "a rectangle bigger than one cell\n");
            }
          else
            fprintf (stderr, "usage: whatif A1:D5 [row=B1] [col=B2]\n");
          g_strfreev (words);
          continue;
        }

      /* calcmode auto|manual; iterate off|on [MAX [TOLERANCE]]; recalc */
      if (g_str_has_prefix (text, "calcmode") || g_str_has_prefix (text, "iterate") ||
          strcmp (text, "recalc") == 0)
        {
          char **words = g_strsplit (text, " ", -1);
          int n = (int) g_strv_length (words);

          if (strcmp (text, "recalc") == 0)
            {
              for (int i = 0; i < o42_book_n_sheets (book); i++)
                o42_sheet_recalculate (o42_book_sheet (book, i));
            }
          else if (g_str_has_prefix (text, "calcmode"))
            {
              if (n >= 2)
                o42_book_set_manual (book, strcmp (words[1], "manual") == 0);
              printf ("%s\n", o42_book_manual (book) ? "manual" : "automatic");
            }
          else
            {
              int max = 100;
              double tolerance = 0.001;
              gboolean on = o42_book_iteration (book, &max, &tolerance);

              /* "iterate" on its own says how things stand; with a word
               * after it, it sets them. */
              if (n >= 2)
                {
                  on = strcmp (words[1], "off") != 0;
                  if (n >= 3) max = atoi (words[2]);
                  if (n >= 4) tolerance = g_ascii_strtod (words[3], NULL);
                  o42_book_set_iteration (book, on, max, tolerance);
                }
              printf ("iteration %s, at most %d passes, tolerance %g\n",
                      on ? "on" : "off", max, tolerance);
            }
          g_strfreev (words);
          continue;
        }

      /* controlset ID FIELD VALUE, where FIELD is link, source, script,
       * text, value, min, max, step, page, width or height; click ID X Y */
      if (g_str_has_prefix (text, "controlset ") || g_str_has_prefix (text, "click "))
        {
          char **words = g_strsplit (text, " ", 4);
          int n = (int) g_strv_length (words);
          O42Shape *sh = n >= 2 ? o42_sheet_find_shape (sheet, (guint) atoi (words[1])) : NULL;

          if (sh == NULL)
            fprintf (stderr, "no such shape\n");
          else if (text[0] == 99 && text[1] == 108)   /* "click" */
            {
              if (n >= 4)
                {
                  if (!o42_sheet_control_click (sheet, sh, g_ascii_strtod (words[2], NULL),
                                                g_ascii_strtod (words[3], NULL),
                                                sh->width, sh->height))
                    printf ("nothing happened\n");
                }
              else
                fprintf (stderr, "usage: click ID X Y\n");
            }
          else if (n >= 4)
            {
              double number = g_ascii_strtod (words[3], NULL);

              if (strcmp (words[2], "link") == 0)
                { g_free (sh->link); sh->link = g_strdup (words[3]); }
              else if (strcmp (words[2], "source") == 0)
                { g_free (sh->source); sh->source = g_strdup (words[3]); }
              else if (strcmp (words[2], "script") == 0)
                { g_free (sh->script); sh->script = g_strdup (words[3]); }
              else if (strcmp (words[2], "text") == 0)
                { g_free (sh->text); sh->text = g_strdup (words[3]); }
              else if (strcmp (words[2], "value") == 0)
                sh->value = number;
              else if (strcmp (words[2], "min") == 0)
                sh->min = number;
              else if (strcmp (words[2], "max") == 0)
                sh->max = number;
              else if (strcmp (words[2], "step") == 0)
                sh->step = number;
              else if (strcmp (words[2], "page") == 0)
                sh->page = number;
              else if (strcmp (words[2], "width") == 0)
                sh->width = number;
              else if (strcmp (words[2], "height") == 0)
                sh->height = number;
              else
                fprintf (stderr, "no such field\n");
            }
          else
            fprintf (stderr, "usage: controlset ID link|source|script|text|value|"
                             "min|max|step|page|width|height VALUE\n");
          g_strfreev (words);
          continue;
        }

      /* table NAME A1:C5 [noheaders]; tables; untable NAME */
      if (g_str_has_prefix (text, "table ") || g_str_has_prefix (text, "untable ") ||
          strcmp (text, "tables") == 0)
        {
          if (strcmp (text, "tables") == 0)
            {
              GArray *tables = o42_sheet_tables (sheet);
              for (guint i = 0; i < tables->len; i++)
                {
                  const O42Table *t = &g_array_index (tables, O42Table, i);
                  char *a = o42_ref_name (t->range.row0, t->range.col0);
                  char *b = o42_ref_name (t->range.row1, t->range.col1);
                  printf ("%s %s:%s headers %d\n", t->name, a, b, t->has_headers);
                  g_free (a); g_free (b);
                }
            }
          else if (text[0] == 'u')
            {
              if (!o42_sheet_remove_table (sheet, text + 8))
                fprintf (stderr, "no such table\n");
            }
          else
            {
              char **words = g_strsplit (text + 6, " ", -1);
              O42Range r;
              gsize len = 0;

              if (g_strv_length (words) >= 2 &&
                  o42_ref_parse (words[1], &r.row0, &r.col0, &len) && words[1][len] == ':' &&
                  o42_ref_parse (words[1] + len + 1, &r.row1, &r.col1, NULL))
                o42_sheet_add_table (sheet, words[0], &r,
                                     g_strv_length (words) < 3 || strcmp (words[2], "noheaders") != 0);
              else
                fprintf (stderr, "usage: table NAME A1:C5 [noheaders]\n");
              g_strfreev (words);
            }
          continue;
        }

      /* border A1:B2 top|bottom|left|right|all|none STYLE [#rrggbb]; indent A1:B2 N; rotate A1:B2 DEG */
      if (g_str_has_prefix (text, "border ") || g_str_has_prefix (text, "indent ") || g_str_has_prefix (text, "rotate "))
        {
          char **words = g_strsplit (text, " ", -1);
          int n = (int) g_strv_length (words);
          O42Range r;
          gsize len = 0;

          if (n >= 3 && o42_ref_parse (words[1], &r.row0, &r.col0, &len) &&
              (words[1][len] == '\0' || (words[1][len] == ':' && o42_ref_parse (words[1] + len + 1, &r.row1, &r.col1, NULL))))
            {
              O42Fmt want = *o42_sheet_get_fmt (sheet, r.row0, r.col0);
              if (words[1][len] == '\0') { r.row1 = r.row0; r.col1 = r.col0; }
              r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
              if (words[0][0] == 'i')
                {
                  want.indent = (guint8) CLAMP (atoi (words[2]), 0, 15);
                  o42_sheet_apply_fmt (sheet, &r, O42_FMT_INDENT, &want);
                }
              else if (words[0][0] == 'r')
                {
                  want.rotation = (gint16) CLAMP (atoi (words[2]), -90, 90);
                  o42_sheet_apply_fmt (sheet, &r, O42_FMT_ROTATION, &want);
                }
              else if (n >= 4 || strcmp (words[2], "none") == 0)
                {
                  O42BorderStyle style = O42_BORDER_NONE;
                  guint32 colour = 0;
                  gboolean all = strcmp (words[2], "all") == 0 || strcmp (words[2], "none") == 0;
                  int side = strcmp (words[2], "top") == 0 ? O42_SIDE_TOP : strcmp (words[2], "bottom") == 0 ? O42_SIDE_BOTTOM
                           : strcmp (words[2], "left") == 0 ? O42_SIDE_LEFT : O42_SIDE_RIGHT;
                  if (n >= 4) o42_border_style_parse (words[3], &style);
                  if (n >= 5 && words[4][0] == '#') colour = (guint32) g_ascii_strtoull (words[4] + 1, NULL, 16);
                  for (int i = 0; i < 4; i++)
                    if (all || i == side)
                      { want.border_style[i] = style; want.border_colour[i] = colour; }
                  o42_fmt_sync_borders (&want);
                  o42_sheet_apply_fmt (sheet, &r, O42_FMT_BORDERS, &want);
                }
              else
                fprintf (stderr, "usage: border A1:B2 top|bottom|left|right|all|none STYLE [#rrggbb]\n");
            }
          else
            fprintf (stderr, "usage: border A1:B2 SIDE STYLE [#rrggbb]; indent A1:B2 N; rotate A1:B2 DEG\n");
          g_strfreev (words);
          continue;
        }
      if (g_str_has_prefix (text, "fmtinfo "))
        {
          int frow, fcol;
          if (o42_ref_parse (text + 8, &frow, &fcol, NULL))
            {
              const O42Fmt *f = o42_sheet_get_fmt (sheet, frow, fcol);
              printf ("borders %s/%06x %s/%06x %s/%06x %s/%06x indent %d rotation %d\n",
                      o42_border_style_name (f->border_style[0]), f->border_colour[0],
                      o42_border_style_name (f->border_style[1]), f->border_colour[1],
                      o42_border_style_name (f->border_style[2]), f->border_colour[2],
                      o42_border_style_name (f->border_style[3]), f->border_colour[3],
                      f->indent, f->rotation);
              printf ("pattern %s/%06X on ", o42_pattern_name ((O42Pattern) f->pattern),
                      f->pattern_colour);
              if (f->fill == O42_FILL_NONE)
                printf ("none\n");
              else
                printf ("%06X\n", f->fill);
            }
          continue;
        }

      /* chart KIND A1:B5 [TITLE] puts a chart below and right of the range. */
      if (g_str_has_prefix (text, "chart "))
        {
          char **words = g_strsplit (text, " ", 4);
          O42ChartKind kind;
          O42Range r;
          gsize len = 0;
          const char *where = g_strv_length (words) >= 3 ? words[2] : "";
          char *from_sheet = NULL;
          const char *bang = strchr (where, '!');

          /* "chart column Sheet1!A1:B5" plots another sheet's cells,
           * which is the only thing a chart sheet can do. */
          if (bang != NULL)
            {
              from_sheet = g_strndup (where, bang - where);
              where = bang + 1;
            }
          if (g_strv_length (words) >= 3 && o42_chart_kind_parse (words[1], &kind) &&
              o42_ref_parse (where, &r.row0, &r.col0, &len) && where[len] == ':' &&
              o42_ref_parse (where + len + 1, &r.row1, &r.col1, NULL))
            {
              O42Chart *chart;

              r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
              chart = o42_sheet_add_chart (sheet, kind, &r, r.row1 + 2, r.col1 + 2);
              if (chart != NULL && from_sheet != NULL)
                {
                  g_free (chart->data_sheet);
                  chart->data_sheet = g_strdup (from_sheet);
                }
              if (chart != NULL && words[3] != NULL)
                {
                  const char *title = words[3];

                  /* Words in front of the title override what the
                   * range looked like: "headings" and "stubs" say the top
                   * row and the left column are headings even when they
                   * hold numbers, such as years, and "rows" lays the
                   * series along the rows instead of down the columns. */
                  for (;;)
                    {
                      if (g_str_has_prefix (title, "headings"))
                        { chart->first_row_labels = TRUE; title += 8; }
                      else if (g_str_has_prefix (title, "stubs"))
                        { chart->first_col_labels = TRUE; title += 5; }
                      else if (g_str_has_prefix (title, "rows"))
                        { chart->series_in_rows = TRUE; title += 4; }
                      else
                        break;
                      while (*title == ' ') title++;
                    }
                  g_free (chart->title);
                  chart->title = g_strdup (title);
                }
              if (chart != NULL)
                printf ("chart %u: %s of %s, series in %s, labels row %d col %d\n",
                        chart->id, o42_chart_kind_name (chart->kind), words[2],
                        chart->series_in_rows ? "rows" : "columns",
                        chart->first_row_labels, chart->first_col_labels);
            }
          else
            fprintf (stderr, "usage: chart KIND [SHEET!]A1:B5 [headings] [stubs] [rows] [TITLE]\n");
          g_free (from_sheet);

          g_strfreev (words);
          continue;
        }

      /* chartset ID title|xtitle|ytitle|legend|gridlines|min|max VALUE */
      if (g_str_has_prefix (text, "chartset "))
        {
          char **words = g_strsplit (text + 9, " ", 3);
          O42Chart *chart = g_strv_length (words) >= 2 ? o42_sheet_find_chart (sheet, (guint) atoi (words[0])) : NULL;
          const char *value = g_strv_length (words) >= 3 ? words[2] : "";

          if (chart == NULL)
            fprintf (stderr, "usage: chartset ID title|xtitle|ytitle|legend|gridlines|labels|trend|trendorder|"
                     "errbars|errvalue|font|fontsize|yformat|secondary|min|max|"
                     "at|size|marker|markersize|markerpicture VALUE\n");
          else if (strcmp (words[1], "title") == 0) { g_free (chart->title); chart->title = g_strdup (value); }
          else if (strcmp (words[1], "xtitle") == 0) { g_free (chart->x_title); chart->x_title = g_strdup (value); }
          else if (strcmp (words[1], "ytitle") == 0) { g_free (chart->y_title); chart->y_title = g_strdup (value); }
          else if (strcmp (words[1], "legend") == 0) chart->legend = strcmp (value, "on") == 0;
          else if (strcmp (words[1], "gridlines") == 0) chart->gridlines = strcmp (value, "on") == 0;
          else if (strcmp (words[1], "labels") == 0) chart->data_labels = strcmp (value, "on") == 0;
          else if (strcmp (words[1], "trend") == 0)
            {
              /* trend=off|on|linear|poly|exp|log|power|average */
              if (strcmp (value, "on") == 0) chart->trend = O42_TREND_LINEAR;
              else if (strcmp (value, "off") == 0) chart->trend = O42_TREND_NONE;
              else if (!o42_trend_kind_parse (value, &chart->trend))
                fprintf (stderr, "trend: none, linear, poly, exp, log, power or average\n");
            }
          else if (strcmp (words[1], "3d") == 0) chart->three_d = strcmp (value, "on") == 0;
          else if (strcmp (words[1], "trendorder") == 0) chart->trend_order = CLAMP (atoi (value), 2, 6);
          else if (strcmp (words[1], "errbars") == 0)
            {
              if (!o42_errbar_kind_parse (value, &chart->err_bars))
                fprintf (stderr, "errbars: none, fixed, percent, stddev or stderr\n");
            }
          else if (strcmp (words[1], "errvalue") == 0) chart->err_value = g_ascii_strtod (value, NULL);
          else if (strcmp (words[1], "font") == 0)
            { g_free (chart->font_family); chart->font_family = g_strdup (value); }
          else if (strcmp (words[1], "fontsize") == 0) chart->font_size = g_ascii_strtod (value, NULL);
          else if (strcmp (words[1], "yformat") == 0) { g_free (chart->y_format); chart->y_format = g_strdup (value); }
          else if (strcmp (words[1], "secondary") == 0) chart->secondary_from = atoi (value);
          else if (strcmp (words[1], "min") == 0) { chart->has_min = *value != '\0'; chart->min = g_ascii_strtod (value, NULL); }
          else if (strcmp (words[1], "max") == 0) { chart->has_max = *value != '\0'; chart->max = g_ascii_strtod (value, NULL); }
          else if (strcmp (words[1], "marker") == 0)
            {
              if (!o42_marker_kind_parse (value, &chart->marker))
                fprintf (stderr, "marker: auto, none, circle, square, diamond, "
                                 "triangle, cross, plus, star or picture\n");
            }
          else if (strcmp (words[1], "markersize") == 0)
            chart->marker_size = g_ascii_strtod (value, NULL);
          else if (strcmp (words[1], "markerpicture") == 0)
            chart->marker_picture = (guint) atoi (value);
          else if (strcmp (words[1], "at") == 0)
            {
              int crow, ccol;

              if (o42_ref_parse (value, &crow, &ccol, NULL))
                { chart->row = crow; chart->col = ccol; chart->dx = chart->dy = 0; }
              else
                fprintf (stderr, "at: a cell, as in A10\n");
            }
          else if (strcmp (words[1], "size") == 0)
            {
              char **parts = g_strsplit (value, " ", 2);

              if (g_strv_length (parts) == 2)
                {
                  chart->width = MAX (g_ascii_strtod (parts[0], NULL), 40);
                  chart->height = MAX (g_ascii_strtod (parts[1], NULL), 40);
                }
              else
                fprintf (stderr, "size: a width and a height in pixels\n");
              g_strfreev (parts);
            }
          g_strfreev (words);
          continue;
        }
      if (strcmp (text, "chartinfo") == 0)
        {
          GPtrArray *charts = o42_sheet_charts (sheet);
          for (guint i = 0; i < charts->len; i++)
            {
              const O42Chart *c = g_ptr_array_index (charts, i);
              printf ("chart %u: title \"%s\" x \"%s\" y \"%s\" legend %s gridlines %s labels %s trend %s/%d errbars %s/%g 3d %s font \"%s\"/%g yformat \"%s\" secondary %d min %s%g max %s%g\n",
                      c->id, c->title, c->x_title, c->y_title, c->legend ? "on" : "off", c->gridlines ? "on" : "off",
                      c->data_labels ? "on" : "off", o42_trend_kind_name (c->trend), c->trend_order,
                      o42_errbar_kind_name (c->err_bars), c->err_value,
                      c->three_d ? "on" : "off",
                      c->font_family != NULL ? c->font_family : "", c->font_size,
                      c->y_format ? c->y_format : "", c->secondary_from,
                      c->has_min ? "" : "auto ", c->min, c->has_max ? "" : "auto ", c->max);
            }
          continue;
        }

      /* insertcells A1:B2 down|right, deletecells A1:B2 up|left */
      if (g_str_has_prefix (text, "insertcells ") || g_str_has_prefix (text, "deletecells "))
        {
          char **words = g_strsplit (text, " ", -1);
          O42Range r;
          gsize len = 0;

          if (g_strv_length (words) >= 3 &&
              o42_ref_parse (words[1], &r.row0, &r.col0, &len) && words[1][len] == ':' &&
              o42_ref_parse (words[1] + len + 1, &r.row1, &r.col1, NULL))
            {
              gboolean vertical = (strcmp (words[2], "down") == 0 || strcmp (words[2], "up") == 0);

              r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
              o42_sheet_shift_cells (sheet, &r, vertical, words[0][0] == 'i');
            }
          else
            fprintf (stderr, "usage: insertcells A1:B2 down|right / deletecells A1:B2 up|left\n");

          g_strfreev (words);
          continue;
        }

      /* hiderows 3 2, unhiderows 3, hidecols 2, unhidecols 2 */
      if (g_str_has_prefix (text, "hiderows ") || g_str_has_prefix (text, "unhiderows ") ||
          g_str_has_prefix (text, "hidecols ") || g_str_has_prefix (text, "unhidecols "))
        {
          char **words = g_strsplit (text, " ", -1);
          int n = (int) g_strv_length (words);
          int at = (n >= 2) ? atoi (words[1]) - 1 : -1;
          int count = (n >= 3) ? atoi (words[2]) : 1;
          gboolean hide = (words[0][0] == 'h');
          gboolean rows = g_str_has_suffix (words[0], "rows");

          for (int i = at; i < at + count && at >= 0; i++)
            {
              if (rows) o42_sheet_set_row_hidden (sheet, i, hide);
              else      o42_sheet_set_col_hidden (sheet, i, hide);
            }
          g_strfreev (words);
          continue;
        }

      /* autofilter A1:C5 turns the filter on; filter B Japan chooses;
       * filter B all clears the choice. */
      if (g_str_has_prefix (text, "autofilter "))
        {
          O42Range r;
          gsize len = 0;

          if (o42_ref_parse (text + 11, &r.row0, &r.col0, &len) && text[11 + len] == ':' &&
              o42_ref_parse (text + 12 + len, &r.row1, &r.col1, NULL))
            o42_sheet_set_autofilter (sheet, &r);
          else
            fprintf (stderr, "usage: autofilter A1:C5\n");
          continue;
        }

      if (g_str_has_prefix (text, "filter "))
        {
          char **words = g_strsplit (text + 7, " ", 2);
          char *ref = g_strconcat (words[0], "1", NULL);
          int frow, fcol;

          if (o42_ref_parse (ref, &frow, &fcol, NULL) && words[1] != NULL)
            o42_sheet_autofilter_choose (sheet, fcol,
                                         strcmp (words[1], "all") == 0 ? NULL : words[1]);
          else
            fprintf (stderr, "usage: filter B VALUE|all\n");
          g_free (ref);
          g_strfreev (words);
          continue;
        }

      /* name TOTAL A1:A5 defines a name on the current sheet; unname TOTAL
       * removes it; names lists them. */
      if (g_str_has_prefix (text, "name "))
        {
          char **words = g_strsplit (text + 5, " ", -1);
          O42Range r;
          gsize len = 0;

          if (g_strv_length (words) >= 2 &&
              o42_ref_parse (words[1], &r.row0, &r.col0, &len) &&
              (words[1][len] == '\0' ||
               (words[1][len] == ':' && o42_ref_parse (words[1] + len + 1, &r.row1, &r.col1, NULL))))
            {
              if (words[1][len] == '\0') { r.row1 = r.row0; r.col1 = r.col0; }
              r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
              if (!o42_book_define_name (book, words[0], sheet, &r))
                fprintf (stderr, "cannot use %s as a name\n", words[0]);
            }
          else
            fprintf (stderr, "usage: name TOTAL A1:A5\n");
          g_strfreev (words);
          continue;
        }

      if (g_str_has_prefix (text, "unname "))
        {
          if (!o42_book_undefine_name (book, text + 7))
            fprintf (stderr, "no such name: %s\n", text + 7);
          continue;
        }

      if (strcmp (text, "names") == 0)
        {
          GList *names = o42_book_names (book);
          for (GList *l = names; l != NULL; l = l->next)
            {
              O42Sheet *target; O42Range r;
              if (o42_book_lookup_name (book, l->data, &target, &r))
                {
                  char *a = o42_ref_name (r.row0, r.col0), *b = o42_ref_name (r.row1, r.col1);
                  printf ("%s = %s!%s:%s\n", (char *) l->data, o42_sheet_get_name (target), a, b);
                  g_free (a); g_free (b);
                }
            }
          g_list_free (names);
          continue;
        }

      /* merge A1:C1 / unmerge A1:C1 */
      if (g_str_has_prefix (text, "merge ") || g_str_has_prefix (text, "unmerge "))
        {
          const char *arg = strchr (text, ' ') + 1;
          O42Range r;
          gsize len = 0;

          if (o42_ref_parse (arg, &r.row0, &r.col0, &len) && arg[len] == ':' &&
              o42_ref_parse (arg + len + 1, &r.row1, &r.col1, NULL))
            {
              r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
              if (text[0] == 'm') o42_sheet_merge (sheet, &r);
              else                o42_sheet_unmerge (sheet, &r);
            }
          else
            fprintf (stderr, "usage: merge A1:C1\n");
          continue;
        }

      if (strcmp (text, "charts") == 0)
        {
          /* The names the model knows, rather than a copy that goes
           * stale when a chart kind is added. */
          GPtrArray *charts = o42_sheet_charts (sheet);
          for (guint i = 0; i < charts->len; i++)
            {
              const O42Chart *c = g_ptr_array_index (charts, i);
              char *a = o42_ref_name (c->data.row0, c->data.col0);
              char *b = o42_ref_name (c->data.row1, c->data.col1);
              printf ("chart %u: %s of %s:%s, series in %s, labels row %d col %d, \"%s\", at %d,%d %gx%g\n",
                      c->id, o42_chart_kind_name (c->kind), a, b,
                      c->series_in_rows ? "rows" : "columns",
                      c->first_row_labels, c->first_col_labels,
                      c->title ? c->title : "", c->row, c->col, c->width, c->height);
              g_free (a);
              g_free (b);
            }
          continue;
        }

      if (strcmp (text, "merges") == 0)
        {
          GArray *merges = o42_sheet_merges (sheet);
          for (guint i = 0; i < merges->len; i++)
            {
              const O42Range *m = &g_array_index (merges, O42Range, i);
              char *a = o42_ref_name (m->row0, m->col0), *b = o42_ref_name (m->row1, m->col1);
              printf ("%s:%s\n", a, b);
              g_free (a); g_free (b);
            }
          continue;
        }

      /* note A1 TEXT sets a note; note A1 alone shows it. */
      /* link A1 TARGET, link A1 clear, links */
      if (g_str_has_prefix (text, "link "))
        {
          int lrow, lcol;
          gsize len = 0;
          if (o42_ref_parse (text + 5, &lrow, &lcol, &len) && text[5 + len] == ' ')
            o42_sheet_set_link (sheet, lrow, lcol, strcmp (text + 6 + len, "clear") == 0 ? NULL : text + 6 + len);
          else
            fprintf (stderr, "usage: link A1 TARGET|clear\n");
          continue;
        }
      if (strcmp (text, "links") == 0)
        {
          GHashTableIter it;
          gpointer k, v;
          g_hash_table_iter_init (&it, o42_sheet_links (sheet));
          while (g_hash_table_iter_next (&it, &k, &v))
            {
              char *name = o42_ref_name (o42_key_row (*(guint64 *) k), o42_key_col (*(guint64 *) k));
              printf ("%s -> %s\n", name, (const char *) v);
              g_free (name);
            }
          continue;
        }

      if (g_str_has_prefix (text, "note "))
        {
          int nrow, ncol;
          gsize len = 0;

          if (o42_ref_parse (text + 5, &nrow, &ncol, &len))
            {
              const char *rest = text + 5 + len;
              while (*rest == ' ') rest++;
              if (*rest != '\0')
                o42_sheet_set_note (sheet, nrow, ncol, rest);
              else
                printf ("%s\n", o42_sheet_get_note (sheet, nrow, ncol) ?
                        o42_sheet_get_note (sheet, nrow, ncol) : "(no note)");
            }
          continue;
        }

      /* goalseek B1 100 A1: make B1 come to 100 by changing A1. */
      if (g_str_has_prefix (text, "goalseek "))
        {
          char **words = g_strsplit (text + 9, " ", -1);
          int trow, tcol, vrow, vcol;
          double found = 0;

          if (g_strv_length (words) >= 3 &&
              o42_ref_parse (words[0], &trow, &tcol, NULL) &&
              o42_ref_parse (words[2], &vrow, &vcol, NULL))
            {
              if (o42_sheet_goal_seek (sheet, trow, tcol, g_ascii_strtod (words[1], NULL),
                                       vrow, vcol, &found))
                {
                  char *v = o42_sheet_get_display (sheet, vrow, vcol);
                  printf ("%s = %s gives %g\n", words[2], v, found);
                  g_free (v);
                }
              else
                printf ("no solution found\n");
            }
          else
            fprintf (stderr, "usage: goalseek B1 100 A1\n");
          g_strfreev (words);
          continue;
        }

      /* paste A1:B2 C1 values|formats|formulas|all [transpose] */
      if (g_str_has_prefix (text, "paste "))
        {
          char **words = g_strsplit (text + 6, " ", -1);
          int n = (int) g_strv_length (words);
          O42Range r;
          int trow, tcol;
          gsize len = 0;

          if (n >= 3 && o42_ref_parse (words[0], &r.row0, &r.col0, &len) && words[0][len] == ':' &&
              o42_ref_parse (words[0] + len + 1, &r.row1, &r.col1, NULL) &&
              o42_ref_parse (words[1], &trow, &tcol, NULL))
            {
              O42PasteMode mode = O42_PASTE_ALL;
              gboolean transpose = FALSE;

              if (strcmp (words[2], "values") == 0)   mode = O42_PASTE_VALUES;
              if (strcmp (words[2], "formats") == 0)  mode = O42_PASTE_FORMATS;
              if (strcmp (words[2], "formulas") == 0) mode = O42_PASTE_FORMULAS;
              for (int i = 3; i < n; i++)
                if (strcmp (words[i], "transpose") == 0) transpose = TRUE;
              r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
              o42_sheet_copy_range_special (sheet, &r, trow, tcol, mode, transpose);
            }
          else
            fprintf (stderr, "usage: paste A1:B2 C1 values|formats|formulas|all [transpose]\n");
          g_strfreev (words);
          continue;
        }

      if (g_str_has_prefix (text, "freeze "))
        {
          int fr = 0, fc = 0;
          if (sscanf (text + 7, "%d %d", &fr, &fc) >= 1)
            o42_sheet_set_frozen (sheet, fr, fc);
          continue;
        }

      /* split A1:A9 , */
      if (g_str_has_prefix (text, "split "))
        {
          char **words = g_strsplit (text + 6, " ", 2);
          O42Range r;
          gsize len = 0;

          if (words[0] != NULL && words[1] != NULL &&
              o42_ref_parse (words[0], &r.row0, &r.col0, &len) &&
              (words[0][len] == '\0' ||
               (words[0][len] == ':' && o42_ref_parse (words[0] + len + 1, &r.row1, &r.col1, NULL))))
            {
              if (words[0][len] == '\0') { r.row1 = r.row0; r.col1 = r.col0; }
              printf ("%d rows split\n", o42_sheet_text_to_columns (sheet, &r, words[1]));
            }
          else
            fprintf (stderr, "usage: split A1:A9 ,\n");
          g_strfreev (words);
          continue;
        }

      /* pivot A1:C9 ROWFIELD COLFIELD|- DATAFIELD sum|count|average|min|max
       * lays a pivot table out on a new sheet; "refresh" lays the current
       * sheet's pivots out again. */
      if (g_str_has_prefix (text, "pivot "))
        {
          char **words = g_strsplit (text + 6, " ", -1);
          O42Pivot p;
          gsize len = 0;

          memset (&p, 0, sizeof p);
          if (g_strv_length (words) >= 5 &&
              o42_ref_parse (words[0], &p.source.row0, &p.source.col0, &len) && words[0][len] == ':' &&
              o42_ref_parse (words[0] + len + 1, &p.source.row1, &p.source.col1, NULL))
            {
              static const char *aggs[] = { "sum", "count", "average", "min", "max" };
              O42Sheet *dest = o42_book_add_sheet (book, NULL, -1);

              p.source = o42_range_normalise (p.source.row0, p.source.col0, p.source.row1, p.source.col1);
              p.source_sheet = (char *) o42_sheet_get_name (sheet);
              p.row_fields = o42_pivot_fields_from_string (words[1]);
              p.col_fields = o42_pivot_fields_from_string (words[2]);
              p.data_field = words[3];
              for (guint i = 0; i < G_N_ELEMENTS (aggs); i++)
                if (strcmp (words[4], aggs[i]) == 0) p.agg = (O42PivotAgg) i;
              if (g_strv_length (words) >= 6 && strchr (words[5], '=') != NULL)
                {
                  /* A page filter, Field=Value. */
                  char *at = strchr (words[5], '=');
                  *at = '\0';
                  p.filter_field = words[5];
                  p.filter_value = at + 1;
                }
              o42_sheet_add_pivot (dest, &p);
              g_strfreev (p.row_fields);
              g_strfreev (p.col_fields);
              sheet = dest;
              printf ("pivot on %s\n", o42_sheet_get_name (dest));
            }
          else
            fprintf (stderr, "usage: pivot A1:C9 Region|Year Quarter|- Sales|=Sales-Costs sum [Field=Value]\n");
          g_strfreev (words);
          continue;
        }

      if (strcmp (text, "refresh") == 0)
        {
          o42_sheet_refresh_pivots (sheet);
          continue;
        }

      /* group rows 2 5 / group cols 2 3 / ungroup rows 2 5; levels */
      if (g_str_has_prefix (text, "group ") || g_str_has_prefix (text, "ungroup "))
        {
          gboolean grp = text[0] == 'g';
          char **words = g_strsplit (text + (grp ? 6 : 8), " ", -1);
          if (g_strv_length (words) >= 3)
            {
              gboolean rows = words[0][0] == 'r';
              int lo = atoi (words[1]) - 1, hi = atoi (words[2]) - 1;
              if (!rows)
                {
                  int r0, c0, r1, c1;
                  char *a = g_strconcat (words[1], "1", NULL), *b = g_strconcat (words[2], "1", NULL);
                  if (o42_ref_parse (a, &r0, &c0, NULL) && o42_ref_parse (b, &r1, &c1, NULL))
                    { lo = c0; hi = c1; }
                  g_free (a); g_free (b);
                }
              o42_sheet_group (sheet, rows, lo, hi, grp);
            }
          else
            fprintf (stderr, "usage: group rows 2 5 | group cols B D\n");
          g_strfreev (words);
          continue;
        }

      if (strcmp (text, "levels") == 0)
        {
          O42Range span;
          o42_sheet_used_range (sheet, &span);
          for (int rr = 0; rr <= span.row1; rr++)
            if (o42_sheet_row_level (sheet, rr) > 0)
              printf ("row %d level %d%s\n", rr + 1, o42_sheet_row_level (sheet, rr),
                      o42_sheet_row_hidden_by_hand (sheet, rr) ? " hidden" : "");
          for (int cc = 0; cc <= span.col1; cc++)
            if (o42_sheet_col_level (sheet, cc) > 0)
              {
                char name[8];
                o42_col_name (cc, name, sizeof name);
                printf ("col %s level %d%s\n", name, o42_sheet_col_level (sheet, cc),
                        o42_sheet_col_hidden (sheet, cc) ? " hidden" : "");
              }
          continue;
        }

      /* array A1:A3 =TRANSPOSE(B1:D1): an array formula over the block */
      /* scripts lists the book's scripts; script NAME PATH stores a
       * file's text under NAME; runscript NAME runs one; delscript NAME. */
      if (strcmp (text, "scripts") == 0)
        {
          for (int i = 0; i < o42_book_n_scripts (book); i++)
            printf ("%s\n", o42_book_script_name (book, i));
          continue;
        }
      if (g_str_has_prefix (text, "script "))
        {
          char **words = g_strsplit (text + 7, " ", 2);
          char *code = NULL;
          if (g_strv_length (words) == 2 && g_file_get_contents (words[1], &code, NULL, NULL))
            o42_book_set_script (book, words[0], code);
          else
            fprintf (stderr, "usage: script NAME PATH\n");
          g_free (code);
          g_strfreev (words);
          continue;
        }
      if (g_str_has_prefix (text, "runscript "))
        {
          const char *code = o42_book_script_code (book, text + 10);
          char *output = NULL;
          gboolean ok = code != NULL && o42_python_run (book, sheet, code, text + 10, &output);
          fputs (output != NULL ? output : (code == NULL ? "no such script\n" : ""), ok ? stdout : stderr);
          g_free (output);
          continue;
        }
      if (g_str_has_prefix (text, "delscript "))
        {
          if (!o42_book_remove_script (book, text + 10))
            fprintf (stderr, "no such script\n");
          continue;
        }

      /* py CODE runs a line of Python with the sheet bound; pyfile PATH
       * runs a script.  What it printed comes out here. */
      if (g_str_has_prefix (text, "py ") || g_str_has_prefix (text, "pyfile "))
        {
          char *output = NULL;
          gboolean ok;

          if (text[2] == ' ')
            ok = o42_python_run (book, sheet, text + 3, "<calc>", &output);
          else
            {
              GFile *file = g_file_new_for_path (text + 7);
              ok = o42_python_run_file (book, sheet, file, &output);
              g_object_unref (file);
            }
          fputs (output != NULL ? output : "", ok ? stdout : stderr);
          fflush (stdout);
          g_free (output);
          continue;
        }

      if (g_str_has_prefix (text, "array "))
        {
          O42Range r;
          gsize len = 0;
          const char *rest = text + 6;
          if (o42_ref_parse (rest, &r.row0, &r.col0, &len) && rest[len] == ':' &&
              o42_ref_parse (rest + len + 1, &r.row1, &r.col1, &used) && rest[len + 1 + used] == ' ')
            o42_sheet_set_array_formula (sheet, &r, rest + len + 2 + used);
          else
            fprintf (stderr, "usage: array A1:A3 =TRANSPOSE(B1:D1)\n");
          continue;
        }

      /* validate A1:A9 whole between 1 10 [MESSAGE...]; validate A1:A9 list a,b,c;
       * unvalidate A1:A9; validations */
      if (g_str_has_prefix (text, "validate "))
        {
          char **words = g_strsplit (text + 9, " ", -1);
          int n = (int) g_strv_length (words);
          O42Validation v;
          gsize len = 0;
          static const char *kinds[] = { "any", "whole", "decimal", "list", "date", "time", "length" };
          static const char *ops[] = { "between", "!between", "=", "<>", ">", "<", ">=", "<=" };

          memset (&v, 0, sizeof v);
          v.allow_blank = TRUE;
          if (n >= 3 && o42_ref_parse (words[0], &v.range.row0, &v.range.col0, &len) &&
              words[0][len] == ':' &&
              o42_ref_parse (words[0] + len + 1, &v.range.row1, &v.range.col1, NULL))
            {
              int next = 2;
              GString *msg = g_string_new (NULL);

              v.range = o42_range_normalise (v.range.row0, v.range.col0, v.range.row1, v.range.col1);
              for (guint i = 0; i < G_N_ELEMENTS (kinds); i++)
                if (strcmp (words[1], kinds[i]) == 0) v.kind = (O42ValidKind) i;
              if (v.kind == O42_VALID_LIST)
                v.value = words[2], next = 3;
              else
                {
                  for (guint i = 0; i < G_N_ELEMENTS (ops); i++)
                    if (strcmp (words[2], ops[i]) == 0) v.op = (O42CondOp) i;
                  v.value = n > 3 ? words[3] : (char *) "";
                  next = 4;
                  if ((v.op == O42_COND_BETWEEN || v.op == O42_COND_NOT_BETWEEN) && n > 4)
                    v.value2 = words[4], next = 5;
                }
              for (int i = next; i < n; i++)
                {
                  if (msg->len > 0) g_string_append_c (msg, ' ');
                  g_string_append (msg, words[i]);
                }
              v.message = msg->str;
              o42_sheet_add_validation (sheet, &v);
              g_string_free (msg, TRUE);
            }
          else
            fprintf (stderr, "usage: validate A1:A9 whole between 1 10 [message]\n");
          g_strfreev (words);
          continue;
        }

      if (g_str_has_prefix (text, "unvalidate "))
        {
          O42Range r;
          gsize len = 0;
          if (o42_ref_parse (text + 11, &r.row0, &r.col0, &len) && text[11 + len] == ':' &&
              o42_ref_parse (text + 12 + len, &r.row1, &r.col1, NULL))
            {
              r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
              o42_sheet_clear_validations (sheet, &r);
            }
          continue;
        }

      if (strcmp (text, "validations") == 0)
        {
          GArray *rules = o42_sheet_validations (sheet);
          for (guint i = 0; i < rules->len; i++)
            {
              const O42Validation *v = &g_array_index (rules, O42Validation, i);
              char *a = o42_ref_name (v->range.row0, v->range.col0);
              char *b = o42_ref_name (v->range.row1, v->range.col1);
              printf ("%s:%s kind %d op %d value \"%s\" value2 \"%s\" blank %d message \"%s\"\n",
                      a, b, (int) v->kind, (int) v->op, v->value, v->value2, v->allow_blank, v->message);
              g_free (a);
              g_free (b);
            }
          continue;
        }

      /* cond A1:A9 > 5 [bold] [italic] [red] [fill]; uncond A1:A9 */
      if (g_str_has_prefix (text, "cond "))
        {
          char **words = g_strsplit (text + 5, " ", -1);
          int n = (int) g_strv_length (words);
          O42Condition c;
          gsize len = 0;

          memset (&c, 0, sizeof c);
          if (n >= 3 && o42_ref_parse (words[0], &c.range.row0, &c.range.col0, &len) &&
              words[0][len] == ':' &&
              o42_ref_parse (words[0] + len + 1, &c.range.row1, &c.range.col1, NULL))
            {
              const char *ops[] = { "between", "!between", "=", "<>", ">", "<", ">=", "<=" };

              c.range = o42_range_normalise (c.range.row0, c.range.col0, c.range.row1, c.range.col1);
              c.op = O42_COND_GREATER;
              for (int i = 0; i < 8; i++)
                if (strcmp (words[1], ops[i]) == 0) c.op = (O42CondOp) i;
              c.value = g_ascii_strtod (words[2], NULL);
              o42_fmt_init_default (&c.fmt);
              for (int i = 3; i < n; i++)
                {
                  if (strcmp (words[i], "bold") == 0)   { c.fmt.bold = 1; c.mask |= O42_FMT_BOLD; }
                  if (strcmp (words[i], "italic") == 0) { c.fmt.italic = 1; c.mask |= O42_FMT_ITALIC; }
                  if (strcmp (words[i], "red") == 0)    { c.fmt.colour = 0xC00000; c.mask |= O42_FMT_COLOUR; }
                  if (strcmp (words[i], "fill") == 0)   { c.fmt.fill = 0xFFFF99; c.mask |= O42_FMT_FILL; }
                  if (g_ascii_isdigit (words[i][0]) && i == 3 && (c.op == O42_COND_BETWEEN || c.op == O42_COND_NOT_BETWEEN))
                    c.value2 = g_ascii_strtod (words[i], NULL);
                }
              o42_sheet_add_condition (sheet, &c);
            }
          else
            fprintf (stderr, "usage: cond A1:A9 > 5 bold red fill\n");
          g_strfreev (words);
          continue;
        }

      if (g_str_has_prefix (text, "uncond "))
        {
          O42Range r;
          gsize len = 0;

          if (o42_ref_parse (text + 7, &r.row0, &r.col0, &len) && text[7 + len] == ':' &&
              o42_ref_parse (text + 8 + len, &r.row1, &r.col1, NULL))
            o42_sheet_clear_conditions (sheet, &r);
          continue;
        }

      if (strcmp (text, "conds") == 0)
        {
          GArray *conds = o42_sheet_conditions (sheet);
          for (guint i = 0; i < conds->len; i++)
            {
              const O42Condition *c = &g_array_index (conds, O42Condition, i);
              char *a = o42_ref_name (c->range.row0, c->range.col0), *b = o42_ref_name (c->range.row1, c->range.col1);
              printf ("%s:%s op %d value %g mask %u bold %d colour %06X fill %08X\n", a, b, (int) c->op,
                      c->value, (unsigned) c->mask, c->fmt.bold, c->fmt.colour, c->fmt.fill);
              g_free (a); g_free (b);
            }
          continue;
        }

      /* shows the format a cell is drawn with: its own plus its conditions */
      if (g_str_has_prefix (text, "shown "))
        {
          int srow, scol;
          if (o42_ref_parse (text + 6, &srow, &scol, NULL))
            {
              O42Fmt f;
              if (!o42_sheet_conditional_fmt (sheet, srow, scol, &f))
                f = *o42_sheet_get_fmt (sheet, srow, scol);
              printf ("%s bold %d italic %d colour %06X fill %08X\n", text + 6, f.bold, f.italic, f.colour, f.fill);
            }
          continue;
        }

      /* pdfbook FILE writes every sheet into one PDF. */
      if (g_str_has_prefix (text, "pdfbook "))
        {
          GFile *file = g_file_new_for_path (text + 8);
          GError *error = NULL;

          if (!o42_pdf_export_book (book, file, &error))
            {
              fprintf (stderr, "%s\n", error->message);
              g_error_free (error);
            }
          g_object_unref (file);
          continue;
        }

      if (g_str_has_prefix (text, "pdf "))
        {
          GFile *file = g_file_new_for_path (text + 4);
          GError *error = NULL;

          if (!o42_pdf_export (sheet, file, &error))
            {
              fprintf (stderr, "%s: %s\n", text + 4,
                       error ? error->message : "failed");
              g_clear_error (&error);
            }
          g_object_unref (file);
          continue;
        }

      /* dedupe A1:C9 A,B [header]; subtotal A1:C9 A C,D [sum|average|count|max|min|product] [header] [replace];
       * unsubtotal A1:C9 */
      if (g_str_has_prefix (text, "dedupe ") || g_str_has_prefix (text, "subtotal ") || g_str_has_prefix (text, "unsubtotal "))
        {
          char **words = g_strsplit (text, " ", -1);
          int n = (int) g_strv_length (words);
          O42Range r;
          gsize len = 0;

          if (n >= 2 && o42_ref_parse (words[1], &r.row0, &r.col0, &len) && words[1][len] == ':' &&
              o42_ref_parse (words[1] + len + 1, &r.row1, &r.col1, NULL))
            {
              r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
              if (words[0][0] == 'u')
                printf ("%d rows removed\n", o42_sheet_remove_subtotals (sheet, &r));
              else if (n >= 3)
                {
                  int cols[64], n_cols = 0, krow, kcol, group = -1;
                  gboolean header = FALSE, replace = FALSE;
                  int function_num = 9;
                  char **names;
                  int first_list = words[0][0] == 'd' ? 2 : 3;

                  if (words[0][0] == 's')
                    {
                      char *ref = g_strconcat (words[2], "1", NULL);
                      if (o42_ref_parse (ref, &krow, &kcol, NULL)) group = kcol;
                      g_free (ref);
                    }
                  names = n > first_list ? g_strsplit (words[first_list], ",", -1) : NULL;
                  for (int i = 0; names != NULL && names[i] != NULL && n_cols < 64; i++)
                    {
                      char *ref = g_strconcat (names[i], "1", NULL);
                      if (o42_ref_parse (ref, &krow, &kcol, NULL)) cols[n_cols++] = kcol;
                      g_free (ref);
                    }
                  g_strfreev (names);
                  for (int i = first_list + 1; i < n; i++)
                    {
                      if (strcmp (words[i], "header") == 0) header = TRUE;
                      else if (strcmp (words[i], "replace") == 0) replace = TRUE;
                      else if (strcmp (words[i], "sum") == 0) function_num = 9;
                      else if (strcmp (words[i], "average") == 0) function_num = 1;
                      else if (strcmp (words[i], "count") == 0) function_num = 3;
                      else if (strcmp (words[i], "max") == 0) function_num = 4;
                      else if (strcmp (words[i], "min") == 0) function_num = 5;
                      else if (strcmp (words[i], "product") == 0) function_num = 6;
                    }
                  if (words[0][0] == 'd')
                    printf ("%d rows removed\n", o42_sheet_remove_duplicates (sheet, &r, cols, n_cols, header));
                  else if (group >= 0)
                    {
                      O42Range out = o42_sheet_subtotal (sheet, &r, group, cols, n_cols, function_num, header, replace);
                      char *a = o42_ref_name (out.row0, out.col0), *b = o42_ref_name (out.row1, out.col1);
                      printf ("%s:%s\n", a, b);
                      g_free (a); g_free (b);
                    }
                }
            }
          else
            fprintf (stderr, "usage: dedupe A1:C9 A,B [header]; subtotal A1:C9 A C,D [sum] [header] [replace]; unsubtotal A1:C9\n");
          g_strfreev (words);
          continue;
        }

      /* printarea A1:C9|clear; header TEXT; footer TEXT; printopt gridlines|headings on|off;
       * titlerows N; pdf PATH */
      if (g_str_has_prefix (text, "printarea "))
        {
          O42Range r;
          gsize len = 0;
          if (strcmp (text + 10, "clear") == 0)
            o42_sheet_set_print_area (sheet, NULL);
          else if (o42_ref_parse (text + 10, &r.row0, &r.col0, &len) && text[10 + len] == ':' &&
                   o42_ref_parse (text + 11 + len, &r.row1, &r.col1, NULL))
            o42_sheet_set_print_area (sheet, &r);
          else
            fprintf (stderr, "usage: printarea A1:C9|clear\n");
          continue;
        }
      if (g_str_has_prefix (text, "header ") || g_str_has_prefix (text, "footer "))
        {
          o42_sheet_set_header_footer (sheet, text[0] == 'h' ? text + 7 : NULL, text[0] == 'f' ? text + 7 : NULL);
          continue;
        }
      if (g_str_has_prefix (text, "printopt ") || g_str_has_prefix (text, "titlerows "))
        {
          const O42PrintSetup *ps = o42_sheet_print_setup (sheet);
          gboolean gridlines = ps->gridlines, headings = ps->headings;
          int titles = ps->title_rows;
          if (text[0] == 't')
            titles = atoi (text + 10);
          else if (g_str_has_prefix (text + 9, "gridlines "))
            gridlines = strcmp (text + 19, "on") == 0;
          else if (g_str_has_prefix (text + 9, "headings "))
            headings = strcmp (text + 18, "on") == 0;
          else
            fprintf (stderr, "usage: printopt gridlines|headings on|off\n");
          o42_sheet_set_print_options (sheet, gridlines, headings, titles);
          continue;
        }
      /* printscale 80 | printscale fit 1 0; pagebreak row|col N; margin PT */
      if (g_str_has_prefix (text, "printscale ") || g_str_has_prefix (text, "pagebreak ") ||
          g_str_has_prefix (text, "margin "))
        {
          char **words = g_strsplit (text, " ", -1);
          int n = (int) g_strv_length (words);

          if (words[0][0] == 'm' && n >= 2)
            o42_sheet_set_print_margin (sheet, g_ascii_strtod (words[1], NULL));
          else if (words[0][0] == 'p' && words[0][1] == 'r' && n >= 2)
            {
              if (strcmp (words[1], "fit") == 0 && n >= 4)
                o42_sheet_set_print_scale (sheet, 100, atoi (words[2]), atoi (words[3]));
              else
                o42_sheet_set_print_scale (sheet, atoi (words[1]), 0, 0);
            }
          else if (n >= 3)
            o42_sheet_toggle_page_break (sheet, words[1][0] == 'r', atoi (words[2]) - 1);
          else
            fprintf (stderr, "usage: printscale 80 | printscale fit 1 0; pagebreak row 12; margin 36\n");
          g_strfreev (words);
          continue;
        }

      if (strcmp (text, "printsetup") == 0)
        {
          const O42PrintSetup *ps = o42_sheet_print_setup (sheet);
          if (ps->has_area)
            {
              char *x = o42_ref_name (ps->area.row0, ps->area.col0), *y = o42_ref_name (ps->area.row1, ps->area.col1);
              printf ("area %s:%s\n", x, y);
              g_free (x); g_free (y);
            }
          printf ("header \"%s\"\nfooter \"%s\"\ngridlines %s\nheadings %s\ntitlerows %d\n",
                  ps->header ? ps->header : "", ps->footer ? ps->footer : "",
                  ps->gridlines ? "on" : "off", ps->headings ? "on" : "off", ps->title_rows);
          printf ("scale %d fit %dx%d margin %g\n", ps->scale, ps->fit_wide, ps->fit_tall, ps->margin);
          {
            GArray *rb = o42_sheet_page_breaks (sheet, TRUE), *cb = o42_sheet_page_breaks (sheet, FALSE);
            printf ("breaks rows");
            for (guint i = 0; i < rb->len; i++) printf (" %d", g_array_index (rb, int, i) + 1);
            printf (" cols");
            for (guint i = 0; i < cb->len; i++) printf (" %d", g_array_index (cb, int, i) + 1);
            printf ("\n");
          }
          continue;
        }
      if (g_str_has_prefix (text, "pdf "))
        {
          GFile *file = g_file_new_for_path (text + 4);
          GError *error = NULL;
          if (!o42_pdf_export (sheet, file, &error))
            {
              fprintf (stderr, "%s\n", error ? error->message : "failed");
              g_clear_error (&error);
            }
          g_object_unref (file);
          continue;
        }

      if (g_str_has_prefix (text, "sort "))
        {
          /* sort A1:C9 B [desc] [header] */
          char **words = g_strsplit (text, " ", -1);
          int n = (int) g_strv_length (words);
          O42Range r;
          gsize len = 0;
          int key = -1;

          if (n >= 3 && o42_ref_parse (words[1], &r.row0, &r.col0, &len) &&
              words[1][len] == ':' &&
              o42_ref_parse (words[1] + len + 1, &r.row1, &r.col1, NULL))
            {
              gboolean header = FALSE;
              int keys[3], nk = 0;
              gboolean asc[3] = { TRUE, TRUE, TRUE };

              r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
              (void) key;
              for (int i = 2; i < n; i++)
                {
                  int krow, kcol;
                  char *keyref = g_strconcat (words[i], "1", NULL);

                  if (strcmp (words[i], "desc") == 0) { if (nk > 0) asc[nk - 1] = FALSE; }
                  else if (strcmp (words[i], "header") == 0) header = TRUE;
                  else if (strcmp (words[i], "then") == 0) { /* the next word is a key */ }
                  else if (nk < 3 && o42_ref_parse (keyref, &krow, &kcol, NULL))
                    keys[nk++] = kcol;
                  g_free (keyref);
                }
              o42_sheet_sort_keys (sheet, &r, keys, asc, nk, header);
            }
          else
            fprintf (stderr, "usage: sort A1:C9 B [desc] [header]\n");

          g_strfreev (words);
          continue;
        }

      if (g_str_has_prefix (text, "find "))
        {
          int frow = -1, fcol = -1;

          if (o42_sheet_find (sheet, text + 5, FALSE, FALSE, &frow, &fcol))
            {
              char *name = o42_ref_name (frow, fcol);
              printf ("found at %s\n", name);
              g_free (name);
            }
          else
            printf ("not found\n");
          continue;
        }

      if (g_str_has_prefix (text, "replace "))
        {
          /* replace OLD -> NEW */
          char **parts = g_strsplit (text + 8, " -> ", 2);

          if (parts[0] != NULL && parts[1] != NULL)
            printf ("%d cells changed\n",
                    o42_sheet_replace (sheet, NULL, parts[0], parts[1], FALSE));
          else
            fprintf (stderr, "usage: replace OLD -> NEW\n");

          g_strfreev (parts);
          continue;
        }

      if (strcmp (text, "undo") == 0)
        {
          o42_sheet_undo (sheet, NULL);
          continue;
        }

      if (strcmp (text, "redo") == 0)
        {
          o42_sheet_redo (sheet, NULL);
          continue;
        }

      if (g_str_has_prefix (text, "copy ") ||
          g_str_has_prefix (text, "filldown ") ||
          g_str_has_prefix (text, "fillright "))
        {
          char **words = g_strsplit (text, " ", -1);
          int n = (int) g_strv_length (words);
          O42Range r;
          int trow, tcol;
          gsize len = 0;

          /* The source is "A1" or "A1:B2". */
          if (n >= 2 && o42_ref_parse (words[1], &r.row0, &r.col0, &len) &&
              (words[1][len] == '\0' ||
               (words[1][len] == ':' &&
                o42_ref_parse (words[1] + len + 1, &r.row1, &r.col1, NULL))))
            {
              if (words[1][len] == '\0')
                {
                  r.row1 = r.row0;
                  r.col1 = r.col0;
                }
              r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);

              if (words[0][0] == 'f')
                o42_sheet_fill (sheet, &r, strcmp (words[0], "filldown") == 0);
              else if (n >= 3 && o42_ref_parse (words[2], &trow, &tcol, NULL))
                o42_sheet_copy_range (sheet, &r, trow, tcol);
              else
                fprintf (stderr, "usage: copy A1:B2 C1\n");
            }
          else
            fprintf (stderr, "usage: %s A1:B2 [C1]\n", words[0]);

          g_strfreev (words);
          continue;
        }

      if (g_str_has_prefix (text, "insertrows ") ||
          g_str_has_prefix (text, "deleterows ") ||
          g_str_has_prefix (text, "insertcols ") ||
          g_str_has_prefix (text, "deletecols "))
        {
          char **words = g_strsplit (text, " ", -1);
          int n = (int) g_strv_length (words);
          int at = (n >= 2) ? atoi (words[1]) - 1 : -1;
          int count = (n >= 3) ? atoi (words[2]) : 1;
          gboolean rows = g_str_has_suffix (words[0], "rows");

          if (words[0][0] == 'i' && rows)  o42_sheet_insert_rows (sheet, at, count);
          if (words[0][0] == 'd' && rows)  o42_sheet_delete_rows (sheet, at, count);
          if (words[0][0] == 'i' && !rows) o42_sheet_insert_cols (sheet, at, count);
          if (words[0][0] == 'd' && !rows) o42_sheet_delete_cols (sheet, at, count);

          g_strfreev (words);
          continue;
        }

      if (!o42_ref_parse (text, &row, &col, &used))
        {
          fprintf (stderr, "not a cell reference: %s\n", text);
          continue;
        }

      eq = text + used;
      while (*eq == ' ')
        eq++;

      if (*eq == '=')
        {
          eq++;
          while (*eq == ' ')
            eq++;
          {
            char *message = NULL;
            if (!o42_sheet_validate (sheet, row, col, eq, &message))
              {
                fprintf (stderr, "%s: not allowed: %s\n", text, message);
                g_free (message);
                continue;
              }
          }
          o42_sheet_set_input (sheet, row, col, eq);
        }
      else if (*eq == '\0')
        {
          char *input = o42_sheet_get_input (sheet, row, col);
          char *shown = o42_sheet_get_display (sheet, row, col);

          if (o42_sheet_has_formula (sheet, row, col))
            printf ("%s\t%s\t(%s)\n", text, shown, input);
          else
            printf ("%s\t%s\n", text, shown);

          g_free (input);
          g_free (shown);
        }
      else
        {
          fprintf (stderr, "expected '=' after %s\n", text);
        }
    }

  o42_book_free (book);
  return 0;
}
