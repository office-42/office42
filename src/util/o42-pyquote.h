/* o42-pyquote.h - a piece of text as a Python string literal
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The macro recorder writes Python, and a cell can hold anything a
 * user can type -- quotes, backslashes, newlines.  This puts such a
 * text into a literal that reads back as itself.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* A double-quoted Python literal for `text`; the caller frees it.
 * NULL text comes back as the two characters "". */
char *o42_python_quote (const char *text);

G_END_DECLS
