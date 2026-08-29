/* o42-richtext.h - a cell's text set in more than one font
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Turning a cell's runs into the Pango attributes that draw them, so
 * that the grid, the printer and the PDF all show the same thing.
 */

#pragma once

#include <pango/pango.h>

#include "o42-fmt.h"
#include "o42-sheet.h"

G_BEGIN_DECLS

/* The attributes for `text` under `base`, with each run laid over it.
 * NULL when there are no runs.  The caller unrefs what comes back. */
PangoAttrList *o42_runs_attributes (const O42TextRun *runs, int n_runs,
                                    const O42Fmt *base, const char *text);

G_END_DECLS
