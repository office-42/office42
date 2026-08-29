/* o42-fn-info.c - see o42-eval-private.h
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

/* ---- What a cell is, and what the program is ------------------------- */

/* CELL(info_type, [reference]): what Excel will say about a cell.  The
 * questions about where the cell is and what is in it are answered
 * here; the ones about how it looks are put to the caller, which is
 * the only one that knows. */
static O42Value
fn_cell (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *what = NULL;
  int row, col;
  const char *sheet = NULL;
  O42Value result;

  ARG_TEXT (0, what);
  if (n >= 2 && args[1].is_range)
    {
      row = args[1].range.row0;
      col = args[1].range.col0;
      sheet = args[1].sheet;
    }
  else
    {
      row = ctx->row;
      col = ctx->col;
    }

  for (char *p = what; *p != '\0'; p++)
    *p = g_ascii_tolower (*p);

  if (strcmp (what, "address") == 0)
    {
      char *ref = o42_ref_name_full (row, col, TRUE, TRUE);
      char *text = ref;

      if (sheet != NULL)
        {
          char *quoted = o42_sheet_name_quote (sheet);

          text = g_strdup_printf ("%s!%s", quoted, ref);
          g_free (quoted);
          g_free (ref);
        }
      g_free (what);
      return o42_value_take (text);
    }
  if (strcmp (what, "row") == 0)
    { g_free (what); return o42_value_number (row + 1); }
  if (strcmp (what, "col") == 0)
    { g_free (what); return o42_value_number (col + 1); }
  if (strcmp (what, "coord") == 0)
    { g_free (what); return o42_value_number (col + 1); }
  if (strcmp (what, "contents") == 0 || strcmp (what, "type") == 0)
    {
      O42Value v;

      ctx->get_cell (ctx, sheet, row, col, &v);
      if (strcmp (what, "contents") == 0)
        { g_free (what); return v; }
      /* "b" for blank, "l" for a label, "v" for anything else. */
      {
        const char *kind = "v";

        if (v.type == O42_VALUE_EMPTY) kind = "b";
        else if (v.type == O42_VALUE_TEXT) kind = "l";
        o42_value_clear (&v);
        g_free (what);
        return o42_value_text (kind);
      }
    }

  /* The rest is the caller's to answer. */
  if (ctx->get_cell_info != NULL &&
      ctx->get_cell_info (ctx, sheet, row, col, what, &result))
    {
      g_free (what);
      return result;
    }
  g_free (what);
  return o42_value_error (O42_ERR_NA);
}

/* INFO(type): what Excel says about the machine it is running on. */
static O42Value
fn_info (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *what = NULL;

  (void) ctx; (void) n;
  ARG_TEXT (0, what);
  for (char *p = what; *p != '\0'; p++)
    *p = g_ascii_tolower (*p);

  if (strcmp (what, "directory") == 0)
    {
      char *dir = g_get_current_dir ();

      g_free (what);
      return o42_value_take (dir);
    }
  if (strcmp (what, "numfile") == 0 || strcmp (what, "origin") == 0)
    {
      gboolean numfile = strcmp (what, "numfile") == 0;

      g_free (what);
      return numfile ? o42_value_number (1) : o42_value_text ("$A:$A$1");
    }
  if (strcmp (what, "osversion") == 0)
    {
      g_free (what);
#if defined (G_OS_WIN32)
      return o42_value_text ("Windows (32-bit) NT 10.00");
#elif defined (__APPLE__)
      return o42_value_text ("Macintosh");
#else
      return o42_value_text ("Linux");
#endif
    }
  if (strcmp (what, "recalc") == 0)
    { g_free (what); return o42_value_text ("Automatic"); }
  if (strcmp (what, "release") == 0)
    { g_free (what); return o42_value_text ("office42 " O42_VERSION); }
  if (strcmp (what, "system") == 0)
    {
      g_free (what);
#if defined (__APPLE__)
      return o42_value_text ("mac");
#else
      return o42_value_text ("pcdos");
#endif
    }
  g_free (what);
  return o42_value_error (O42_ERR_VALUE);
}

const O42Function O42_FUNCS_INFO[] = {
  { "CELL", 1, 2, fn_cell },
  { "INFO", 1, 1, fn_info },
  { NULL, 0, 0, NULL }
};

const O42FunctionHelp O42_HELP_INFO[] = {
  { "CELL", "CELL(info_type, reference)", "Something about a cell: address, row, col, contents, type, format, width, protect, prefix." },
  { "INFO", "INFO(type)", "Something about the program: directory, osversion, recalc, release, system." },
  { NULL, NULL, NULL }
};
