/* o42-lotus.h - Lotus 1-2-3 worksheets
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The format that made the spreadsheet a thing people bought a
 * computer for, and the one Gnumeric still reads.  A .wk1 is a string
 * of records -- a two-byte opcode, a two-byte length, then that many
 * bytes -- and the cells are four of them: BLANK, INTEGER, NUMBER and
 * LABEL, each carrying a format byte and a column and a row, with
 * FORMULA carrying the value it last worked out as well.
 *
 * Reading takes all five.  A formula's own tokens are Lotus's own
 * stack machine and are not read: the value beside them is what the
 * file says the cell holds, and that is what the cell gets.  Writing
 * puts out the values, as writing a CSV does, since a formula means
 * nothing to a program that has not got office42's functions.
 */

#pragma once

#include <gio/gio.h>

#include "o42-sheet.h"

G_BEGIN_DECLS

gboolean o42_lotus_save (O42Sheet *sheet, GFile *file, GError **error);
gboolean o42_lotus_load (O42Sheet *sheet, GFile *file, GError **error);

G_END_DECLS
