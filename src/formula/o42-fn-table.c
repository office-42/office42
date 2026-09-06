/* o42-fn-table.c - TABLE(), the What-If table's own formula
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Excel fills the inside of a Data > Table with {=TABLE(row_input,
 * column_input)}, an array formula that cannot be typed and that shows,
 * in each cell, what the table's formula comes to with that cell's edge
 * values in the input cells.  The formula engine cannot work that out
 * on its own -- it would have to change cells -- so the sheet does, and
 * TABLE() asks the sheet what it worked out for the cell it stands in,
 * through the same door CELL() asks about a cell's format.  The
 * arguments are references so that a change to either input stales the
 * cell.
 */

#include "o42-eval-private.h"

static O42Value
fn_table (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value out;

  (void) args; (void) n;
  if (ctx->get_cell_info == NULL ||
      !ctx->get_cell_info (ctx, NULL, ctx->row, ctx->col, "table", &out))
    return o42_value_error (O42_ERR_NA);
  return out;
}

const O42Function O42_FUNCS_TABLE[] = {
  { "TABLE", 0, 2, fn_table },
  { NULL, 0, 0, NULL }
};

const O42FunctionHelp O42_HELP_TABLE[] = {
  { "TABLE", "TABLE(row_input, column_input)", "What a What-If table's formula comes to for this cell; Data > Table writes it." },
  { NULL, NULL, NULL }
};
