/* o42-pyquote.c - a piece of text as a Python string literal
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-pyquote.h"

char *
o42_python_quote (const char *text)
{
  GString *out = g_string_new ("\"");

  for (const char *p = text != NULL ? text : ""; *p != '\0'; p++)
    {
      switch (*p)
        {
        case '"':  g_string_append (out, "\\\""); break;
        case '\\': g_string_append (out, "\\\\"); break;
        case '\n': g_string_append (out, "\\n"); break;
        case '\r': g_string_append (out, "\\r"); break;
        case '\t': g_string_append (out, "\\t"); break;
        default:   g_string_append_c (out, *p); break;
        }
    }
  g_string_append_c (out, '"');
  return g_string_free (out, FALSE);
}
