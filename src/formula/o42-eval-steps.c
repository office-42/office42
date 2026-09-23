/* o42-eval-steps.c - see o42-eval-steps.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-eval-steps.h"

#include <string.h>

struct _O42Stepper {
  O42EvalContext *ctx;
  O42Node        *original;   /* what Restart goes back to */
  O42Node        *tree;       /* rewritten as the steps go */
  int             row, col;
};

/* At most this many cells of a range are spelt out as an array constant;
 * a bigger range is left as it is and its function worked out whole. */
#define ARRAY_LIMIT 400

/* Functions that want a reference, not what is in it: their cell and
 * range arguments are not turned into values first, or ROW(A5) would
 * become ROW(3). */
static gboolean
wants_references (const char *name)
{
  static const char *const names[] = {
    "ROW", "COLUMN", "ROWS", "COLUMNS", "OFFSET", "INDEX", "CELL", "ISREF",
    "AREAS", "INDIRECT", "SUMIF", "SUMIFS", "COUNTIF", "COUNTIFS",
    "AVERAGEIF", "AVERAGEIFS", "MINIFS", "MAXIFS", "SUBTOTAL", "AGGREGATE",
    "COUNTBLANK", "SQLVALUE", "GETPIVOTDATA", "HYPERLINK", "FORMULATEXT",
    "ISFORMULA", "SHEET", "SHEETS", "LET", "LAMBDA", "MAP", "REDUCE", "SCAN",
    "BYROW", "BYCOL", "MAKEARRAY", "ISOMITTED", "DGET", "DSUM", "DCOUNT",
    "DCOUNTA", "DAVERAGE", "DMAX", "DMIN", "DPRODUCT", "DSTDEV", "DSTDEVP",
    "DVAR", "DVARP", "RANK", "RANK.EQ", "RANK.AVG", "FREQUENCY", "TABLE",
    NULL
  };
  for (int i = 0; names[i] != NULL; i++)
    if (strcmp (names[i], name) == 0)
      return TRUE;
  return FALSE;
}

/* A call that is worked out whole, its arguments never shown on their
 * own: LET binds names its arguments would not understand alone. */
static gboolean
is_opaque_call (const O42Node *node)
{
  static const char *const names[] = {
    "LET", "LAMBDA", "MAP", "REDUCE", "SCAN", "BYROW", "BYCOL", "MAKEARRAY", NULL
  };
  if (node->type != O42_NODE_CALL)
    return FALSE;
  for (int i = 0; names[i] != NULL; i++)
    if (strcmp (names[i], node->as.call.name) == 0)
      return TRUE;
  return FALSE;
}

static gboolean
is_constant (const O42Node *node)
{
  switch (node->type)
    {
    case O42_NODE_NUMBER: case O42_NODE_STRING: case O42_NODE_BOOL:
    case O42_NODE_ERROR: case O42_NODE_EMPTY: case O42_NODE_ARRAY:
      return TRUE;
    default:
      return FALSE;
    }
}

static int
range_cells (const O42Node *node)
{
  return (node->as.range.row1 - node->as.range.row0 + 1) *
         (node->as.range.col1 - node->as.range.col0 + 1);
}

/* The first node, in the order the machine works them out, that is not
 * yet a value: the deepest leftmost one.  `parent` says what it is an
 * argument of; a reference under a function that wants references, or
 * a range too big to spell out, is left for the call to take whole. */
static O42Node *
find_next (O42Node *node, const O42Node *parent)
{
  O42Node *found;

  if (node == NULL || is_constant (node))
    return NULL;

  switch (node->type)
    {
    case O42_NODE_REF:
      if (parent != NULL && parent->type == O42_NODE_CALL && wants_references (parent->as.call.name))
        return NULL;
      return node;

    case O42_NODE_RANGE:
      if (parent != NULL && parent->type == O42_NODE_CALL && wants_references (parent->as.call.name))
        return NULL;
      if (node->sheet_last != NULL || (node->abs & (O42_WHOLE_COLS | O42_WHOLE_ROWS)))
        return NULL;
      if (parent != NULL && (parent->type == O42_NODE_CALL || parent->type == O42_NODE_APPLY) &&
          range_cells (node) > ARRAY_LIMIT)
        return NULL;
      return node;

    case O42_NODE_NAME:
      return node;

    case O42_NODE_UNARY:
      found = find_next (node->as.op.a, node);
      return found != NULL ? found : node;

    case O42_NODE_BINARY:
      found = find_next (node->as.op.a, node);
      if (found == NULL)
        found = find_next (node->as.op.b, node);
      return found != NULL ? found : node;

    case O42_NODE_CALL:
      if (!is_opaque_call (node) && node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          {
            found = find_next (g_ptr_array_index (node->as.call.args, i), node);
            if (found != NULL)
              return found;
          }
      return node;

    case O42_NODE_APPLY:
      return node;

    default:
      return node;
    }
}

/* The parent of `target` in the tree, for knowing what an empty cell
 * should read as. */
static const O42Node *
find_parent (const O42Node *node, const O42Node *target)
{
  const O42Node *p;

  if (node == NULL)
    return NULL;
  switch (node->type)
    {
    case O42_NODE_UNARY:
    case O42_NODE_BINARY:
      if (node->as.op.a == target || node->as.op.b == target)
        return node;
      p = find_parent (node->as.op.a, target);
      return p != NULL ? p : find_parent (node->as.op.b, target);
    case O42_NODE_CALL:
      if (node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          {
            O42Node *arg = g_ptr_array_index (node->as.call.args, i);
            if (arg == target)
              return node;
            p = find_parent (arg, target);
            if (p != NULL)
              return p;
          }
      return NULL;
    case O42_NODE_APPLY:
      if (node->as.apply.callee == target)
        return node;
      p = find_parent (node->as.apply.callee, target);
      if (p != NULL)
        return p;
      if (node->as.apply.args != NULL)
        for (guint i = 0; i < node->as.apply.args->len; i++)
          {
            O42Node *arg = g_ptr_array_index (node->as.apply.args, i);
            if (arg == target)
              return node;
            p = find_parent (arg, target);
            if (p != NULL)
              return p;
          }
      return NULL;
    default:
      return NULL;
    }
}

/* A node holding a value.  An empty value reads as zero, or as "" under
 * the & operator, which is how the machine takes it. */
static O42Node *
node_from_value (const O42Value *value, const O42Node *parent)
{
  O42Node *node = g_new0 (O42Node, 1);

  switch (value->type)
    {
    case O42_VALUE_NUMBER:
      node->type = O42_NODE_NUMBER;
      node->as.number = value->as.number;
      break;
    case O42_VALUE_TEXT:
      node->type = O42_NODE_STRING;
      node->as.string = g_strdup (value->as.text);
      break;
    case O42_VALUE_BOOL:
      node->type = O42_NODE_BOOL;
      node->as.boolean = value->as.boolean;
      break;
    case O42_VALUE_ERROR:
      node->type = O42_NODE_ERROR;
      node->as.error = value->as.error;
      break;
    default:
      if (parent != NULL && parent->type == O42_NODE_BINARY && parent->as.op.op == O42_OP_CONCAT)
        {
          node->type = O42_NODE_STRING;
          node->as.string = g_strdup ("");
        }
      else
        {
          node->type = O42_NODE_NUMBER;
          node->as.number = 0;
        }
      break;
    }
  return node;
}

/* Puts `with` where `node` is, keeping the pointer the parent holds. */
static void
replace_node (O42Node *node, O42Node *with)
{
  O42Node old = *node;

  *node = *with;
  *with = old;
  o42_node_free (with);
}

static O42Node *
array_from_range (O42Stepper *stepper, const O42Node *range)
{
  O42Node *node = g_new0 (O42Node, 1);

  node->type = O42_NODE_ARRAY;
  node->as.array.rows = range->as.range.row1 - range->as.range.row0 + 1;
  node->as.array.cols = range->as.range.col1 - range->as.range.col0 + 1;
  node->as.array.items = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_node_free);
  for (int r = range->as.range.row0; r <= range->as.range.row1; r++)
    for (int c = range->as.range.col0; c <= range->as.range.col1; c++)
      {
        O42Value v = o42_value_empty ();

        stepper->ctx->get_cell (stepper->ctx, range->sheet, r, c, &v);
        g_ptr_array_add (node->as.array.items, node_from_value (&v, NULL));
        o42_value_clear (&v);
      }
  return node;
}

O42Stepper *
o42_stepper_new (O42EvalContext *ctx, const O42Node *formula, int row, int col)
{
  O42Stepper *stepper;

  g_return_val_if_fail (ctx != NULL && formula != NULL, NULL);
  stepper = g_new0 (O42Stepper, 1);
  stepper->ctx = ctx;
  stepper->original = o42_node_copy (formula);
  stepper->tree = o42_node_copy (formula);
  stepper->row = row;
  stepper->col = col;
  return stepper;
}

void
o42_stepper_free (O42Stepper *stepper)
{
  if (stepper == NULL)
    return;
  o42_node_free (stepper->original);
  o42_node_free (stepper->tree);
  g_free (stepper);
}

void
o42_stepper_restart (O42Stepper *stepper)
{
  g_return_if_fail (stepper != NULL);
  o42_node_free (stepper->tree);
  stepper->tree = o42_node_copy (stepper->original);
}

char *
o42_stepper_text (O42Stepper *stepper, int *start, int *length)
{
  O42Node *next;

  g_return_val_if_fail (stepper != NULL, NULL);
  next = find_next (stepper->tree, NULL);
  return o42_node_to_string_marked (stepper->tree, next, start, length);
}

gboolean
o42_stepper_done (O42Stepper *stepper)
{
  g_return_val_if_fail (stepper != NULL, TRUE);
  return find_next (stepper->tree, NULL) == NULL;
}

gboolean
o42_stepper_next_is_cell (O42Stepper *stepper, const char **sheet, int *row, int *col)
{
  O42Node *next;

  g_return_val_if_fail (stepper != NULL, FALSE);
  next = find_next (stepper->tree, NULL);
  if (next == NULL || next->type != O42_NODE_REF || next->sheet_last != NULL)
    return FALSE;
  if (sheet != NULL) *sheet = next->sheet;
  if (row != NULL) *row = next->as.ref.row;
  if (col != NULL) *col = next->as.ref.col;
  return TRUE;
}

void
o42_stepper_substitute (O42Stepper *stepper, const O42Value *value)
{
  O42Node *next;

  g_return_if_fail (stepper != NULL && value != NULL);
  next = find_next (stepper->tree, NULL);
  if (next == NULL)
    return;
  replace_node (next, node_from_value (value, find_parent (stepper->tree, next)));
}

void
o42_stepper_step (O42Stepper *stepper)
{
  O42Node *next;
  const O42Node *parent;

  g_return_if_fail (stepper != NULL);
  next = find_next (stepper->tree, NULL);
  if (next == NULL)
    return;
  parent = find_parent (stepper->tree, next);

  /* A range handed to a function is spelt out as the values in it, as
   * Excel shows it; anything else is worked out to one value. */
  if (next->type == O42_NODE_RANGE && parent != NULL &&
      (parent->type == O42_NODE_CALL || parent->type == O42_NODE_APPLY))
    {
      replace_node (next, array_from_range (stepper, next));
      return;
    }

  {
    int saved_row = stepper->ctx->row, saved_col = stepper->ctx->col;
    O42Value v;

    stepper->ctx->row = stepper->row;
    stepper->ctx->col = stepper->col;
    v = o42_eval (stepper->ctx, next);
    stepper->ctx->row = saved_row;
    stepper->ctx->col = saved_col;
    replace_node (next, node_from_value (&v, parent));
    o42_value_clear (&v);
  }
}
