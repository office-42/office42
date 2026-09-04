/* o42-eval.c - see o42-eval.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-eval-private.h"

#include "o42-date.h"
#include "o42-hdate.h"
#include "o42-numfmt.h"

#include <math.h>
#include <string.h>

/* The three the families borrow are exported under their long names;
 * this file goes on calling them what it always did. */
#define operand_value o42_operand_value
#define normal_cdf o42_normal_cdf
#define normal_pdf o42_normal_pdf
#define collect_numbers o42_collect_numbers
#define collect_pairs o42_collect_pairs
#define is_holiday o42_is_holiday
#define visit_numbers o42_visit_numbers
#define accum_clear o42_accum_clear
#define accum_init o42_accum_init
#define accumulate o42_accumulate
#define round_half_away o42_round_half_away
#define complex_parse o42_complex_parse
#define complex_format o42_complex_format
#define chi_cdf o42_chi_cdf
#define beta_i o42_beta_i
#define invert_cdf o42_invert_cdf
#define t_cdf o42_t_cdf
#define f_cdf o42_f_cdf
#define gamma_p o42_gamma_p
#define compare_doubles o42_compare_doubles
#define optional_bool o42_optional_bool
#define moments o42_moments
#define normal_inverse o42_normal_inverse

static O42Value eval_node (O42EvalContext *ctx, const O42Node *node);

/* ---------------------------------------------------------------------- */
/* Operands                                                                */
/* ---------------------------------------------------------------------- */

/* ---------------------------------------------------------------------- */
/* Array constants                                                         */
/* ---------------------------------------------------------------------- */

/* {1,2;3,4} in a formula behaves as a range would: SUM adds it up, INDEX
 * picks from it, MATCH searches it.  Rather than teach every function a
 * third kind of operand, an array constant is stored for the duration
 * of the formula's evaluation and handed out as a range on a sheet
 * whose name begins with \001; a get_cell wrapper serves those cells
 * from the store and forwards everything else to the real context. */
typedef struct {
  int       rows, cols;
  O42Value *cells;
} ArrayConst;

typedef struct {
  O42EvalContext *original;
  GPtrArray      *arrays;     /* ArrayConst* */
} ArrayFrame;

static GPtrArray *array_frames = NULL;   /* ArrayFrame*, innermost last */

static void
array_const_free (ArrayConst *a)
{
  for (int i = 0; i < a->rows * a->cols; i++)
    o42_value_clear (&a->cells[i]);
  g_free (a->cells);
  g_free (a);
}

static void
array_get_cell (O42EvalContext *ctx, const char *sheet, int row, int col, O42Value *out)
{
  ArrayFrame *frame = array_frames && array_frames->len > 0
                      ? g_ptr_array_index (array_frames, array_frames->len - 1) : NULL;

  if (sheet != NULL && sheet[0] == '\001' && frame != NULL)
    {
      guint idx = (guint) atoi (sheet + 1);
      if (idx < frame->arrays->len)
        {
          ArrayConst *a = g_ptr_array_index (frame->arrays, idx);
          if (row >= 0 && row < a->rows && col >= 0 && col < a->cols)
            {
              *out = o42_value_copy (&a->cells[row * a->cols + col]);
              return;
            }
        }
      *out = o42_value_empty ();
      return;
    }
  if (frame != NULL)
    frame->original->get_cell (frame->original, sheet, row, col, out);
  else
    *out = o42_value_error (O42_ERR_REF);
  (void) ctx;
}

static gboolean
tree_has_array (const O42Node *node)
{
  if (node == NULL) return FALSE;
  switch (node->type)
    {
    case O42_NODE_ARRAY: return TRUE;
    case O42_NODE_UNARY:
    case O42_NODE_BINARY: return tree_has_array (node->as.op.a) || tree_has_array (node->as.op.b);
    case O42_NODE_CALL:
      if (node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          if (tree_has_array (g_ptr_array_index (node->as.call.args, i)))
            return TRUE;
      return FALSE;
    default: return FALSE;
    }
}

/* Registers an array in the current frame and hands it out as a range. */
static O42Operand
array_operand (ArrayConst *a)
{
  ArrayFrame *frame = array_frames && array_frames->len > 0
                      ? g_ptr_array_index (array_frames, array_frames->len - 1) : NULL;
  O42Operand op;
  char *name;

  memset (&op, 0, sizeof op);
  if (frame == NULL)
    {
      array_const_free (a);
      op.value = o42_value_error (O42_ERR_VALUE);
      return op;
    }
  g_ptr_array_add (frame->arrays, a);
  name = g_strdup_printf ("\001%u", frame->arrays->len - 1);
  op.is_range = TRUE;
  op.sheet = g_intern_string (name);
  op.range.row0 = op.range.col0 = 0;
  op.range.row1 = a->rows - 1;
  op.range.col1 = a->cols - 1;
  g_free (name);
  return op;
}

/* The most cells an array a formula makes up may hold.  Excel's own
 * limit is the grid; ours is what fits in memory with room to spare,
 * since =SEQUENCE(1048576,16384) would ask for 270 GB and take the
 * program down rather than answer #NUM!. */
#define ARRAY_CELLS_MAX (10 * 1000 * 1000)

static ArrayConst *
array_const_new (int rows, int cols)
{
  ArrayConst *a = g_new0 (ArrayConst, 1);
  a->rows = rows;
  a->cols = cols;
  a->cells = g_new0 (O42Value, (gsize) rows * cols);
  return a;
}

/* A:A names a million rows and 1:1 sixteen thousand columns, nearly
 * all of them empty.  The range a formula walks is cut down to the
 * sheet's stored cells, which is what every function that reads the
 * range cell by cell needs; a range with nothing in it keeps its first
 * cell so that it is still a range. */
static void
operand_clip_whole (O42EvalContext *ctx, O42Operand *op, const O42Node *node)
{
  O42Range used;

  if (!(node->abs & (O42_WHOLE_COLS | O42_WHOLE_ROWS)) || ctx->get_extent == NULL)
    return;
  if (!ctx->get_extent (ctx, node->sheet, &used))
    used.row0 = used.col0 = used.row1 = used.col1 = 0;
  if (node->abs & O42_WHOLE_COLS)
    op->range.row1 = MAX (used.row1, op->range.row0);
  if (node->abs & O42_WHOLE_ROWS)
    op->range.col1 = MAX (used.col1, op->range.col0);
}

/* An operand's shape: a value is one by one. */
static void
operand_dims (const O42Operand *op, int *rows, int *cols)
{
  if (op->is_range)
    {
      *rows = op->range.row1 - op->range.row0 + 1;
      *cols = op->range.col1 - op->range.col0 + 1;
    }
  else
    *rows = *cols = 1;
}

/* Cell (i, j) of an operand, with a one-row or one-column operand
 * stretched along the missing axis, as Excel broadcasts. */
static O42Value
operand_cell (O42EvalContext *ctx, const O42Operand *op, int i, int j)
{
  O42Value v;
  int rows, cols;

  if (!op->is_range)
    return o42_value_copy (&op->value);
  operand_dims (op, &rows, &cols);
  if (rows == 1) i = 0;
  if (cols == 1) j = 0;
  if (i >= rows || j >= cols)
    return o42_value_error (O42_ERR_NA);
  ctx->get_cell (ctx, op->sheet, op->range.row0 + i, op->range.col0 + j, &v);
  return v;
}

static gboolean
operand_is_multi (const O42Operand *op)
{
  int rows, cols;
  operand_dims (op, &rows, &cols);
  return rows > 1 || cols > 1;
}

static O42Value binary_values (O42Op op, O42Value a, O42Value b);

/* a OP b over every cell, with the shapes broadcast against each other. */
static O42Operand
broadcast_binary (O42EvalContext *ctx, O42Op op, const O42Operand *oa, const O42Operand *ob)
{
  int ra, ca, rb, cb, rows, cols;
  ArrayConst *out;

  operand_dims (oa, &ra, &ca);
  operand_dims (ob, &rb, &cb);
  rows = MAX (ra, rb);
  cols = MAX (ca, cb);
  out = array_const_new (rows, cols);
  for (int i = 0; i < rows; i++)
    for (int j = 0; j < cols; j++)
      out->cells[i * cols + j] = binary_values (op, operand_cell (ctx, oa, i, j), operand_cell (ctx, ob, i, j));
  return array_operand (out);
}

static void
operand_clear (O42Operand *op)
{
  if (!op->is_range)
    o42_value_clear (&op->value);
}

/* A range used where a single value is wanted.  A one-by-one range is that
 * value; anything larger is an error rather than a guess.  Excel would try an
 * implicit intersection against the calling cell's row or column, which is a
 * cleverness that mostly confuses people. */
O42Value
o42_operand_value (O42EvalContext *ctx, const O42Operand *op)
{
  O42Value v;

  if (!op->is_range)
    return o42_value_copy (&op->value);

  if (op->range.row0 == op->range.row1 && op->range.col0 == op->range.col1)
    {
      ctx->get_cell (ctx, op->sheet, op->range.row0, op->range.col0, &v);
      return v;
    }

  return o42_value_error (O42_ERR_VALUE);
}

/* ---------------------------------------------------------------------- */
/* Walking numbers out of arguments                                        */
/* ---------------------------------------------------------------------- */

/* Excel treats a number written into a formula differently from a number
 * sitting in a referenced cell, and the difference matters: SUM("x") is an
 * error, but SUM over a range containing "x" quietly ignores it.  A
 * spreadsheet full of labels would be unusable otherwise. */

gboolean
o42_visit_numbers (O42EvalContext *ctx,
               O42Operand     *args,
               int             n_args,
               NumberVisitor   visit,
               gpointer        user,
               O42ErrorCode   *error)
{
  for (int i = 0; i < n_args; i++)
    {
      if (args[i].is_range)
        {
          const O42Range *r = &args[i].range;

          for (int row = r->row0; row <= r->row1; row++)
            for (int col = r->col0; col <= r->col1; col++)
              {
                O42Value v;
                gboolean stop = FALSE;

                ctx->get_cell (ctx, args[i].sheet, row, col, &v);

                if (v.type == O42_VALUE_ERROR)
                  {
                    *error = v.as.error;
                    o42_value_clear (&v);
                    return FALSE;
                  }

                /* Text and blanks in a range are skipped, not coerced. */
                if (v.type == O42_VALUE_NUMBER)
                  stop = !visit (v.as.number, user);
                else if (v.type == O42_VALUE_BOOL)
                  stop = FALSE;      /* booleans in ranges are skipped too */

                o42_value_clear (&v);

                if (stop)
                  return TRUE;
              }
        }
      else
        {
          double n;

          if (!o42_value_to_number (&args[i].value, &n, error))
            return FALSE;

          if (!visit (n, user))
            return TRUE;
        }
    }

  return TRUE;
}

/* Counts every non-blank cell, whatever it holds: what COUNTA does. */
static int
count_non_blank (O42EvalContext *ctx, O42Operand *args, int n_args)
{
  int count = 0;

  for (int i = 0; i < n_args; i++)
    {
      if (args[i].is_range)
        {
          const O42Range *r = &args[i].range;

          for (int row = r->row0; row <= r->row1; row++)
            for (int col = r->col0; col <= r->col1; col++)
              {
                O42Value v;

                ctx->get_cell (ctx, args[i].sheet, row, col, &v);
                if (v.type != O42_VALUE_EMPTY)
                  count++;
                o42_value_clear (&v);
              }
        }
      else if (args[i].value.type != O42_VALUE_EMPTY)
        {
          count++;
        }
    }

  return count;
}

/* ---------------------------------------------------------------------- */
/* Accumulators used by the aggregate functions                            */
/* ---------------------------------------------------------------------- */

gboolean
o42_accumulate (double n, gpointer user)
{
  Accum *a = user;

  a->sum += n;
  a->product *= n;
  if (a->count == 0 || n < a->min) a->min = n;
  if (a->count == 0 || n > a->max) a->max = n;
  a->count++;

  if (a->values != NULL)
    g_array_append_val (a->values, n);

  return TRUE;
}

void
o42_accum_init (Accum *a, gboolean keep_values)
{
  memset (a, 0, sizeof *a);
  a->product = 1.0;
  a->values = keep_values ? g_array_new (FALSE, FALSE, sizeof (double)) : NULL;
}

void
o42_accum_clear (Accum *a)
{
  if (a->values != NULL)
    g_array_free (a->values, TRUE);
}

/* ---------------------------------------------------------------------- */
/* The function library                                                    */
/* ---------------------------------------------------------------------- */

typedef O42Value (*O42FuncImpl) (O42EvalContext *ctx,
                                 O42Operand     *args,
                                 int             n_args);



/* ---- Arithmetic aggregates ------------------------------------------- */

static O42Value
fn_sum (O42EvalContext *ctx, O42Operand *args, int n)
{
  Accum a;
  O42ErrorCode err = O42_ERR_VALUE;
  O42Value result;

  accum_init (&a, FALSE);
  if (!visit_numbers (ctx, args, n, accumulate, &a, &err))
    { accum_clear (&a); return o42_value_error (err); }

  result = o42_value_number (a.sum);
  accum_clear (&a);
  return result;
}

static O42Value
fn_product (O42EvalContext *ctx, O42Operand *args, int n)
{
  Accum a;
  O42ErrorCode err = O42_ERR_VALUE;
  O42Value result;

  accum_init (&a, FALSE);
  if (!visit_numbers (ctx, args, n, accumulate, &a, &err))
    { accum_clear (&a); return o42_value_error (err); }

  result = o42_value_number (a.count > 0 ? a.product : 0.0);
  accum_clear (&a);
  return result;
}

static O42Value
fn_average (O42EvalContext *ctx, O42Operand *args, int n)
{
  Accum a;
  O42ErrorCode err = O42_ERR_VALUE;
  O42Value result;

  accum_init (&a, FALSE);
  if (!visit_numbers (ctx, args, n, accumulate, &a, &err))
    { accum_clear (&a); return o42_value_error (err); }

  /* The average of nothing is not zero. */
  result = (a.count == 0) ? o42_value_error (O42_ERR_DIV0)
                          : o42_value_number (a.sum / a.count);
  accum_clear (&a);
  return result;
}

static O42Value
fn_min (O42EvalContext *ctx, O42Operand *args, int n)
{
  Accum a;
  O42ErrorCode err = O42_ERR_VALUE;
  O42Value result;

  accum_init (&a, FALSE);
  if (!visit_numbers (ctx, args, n, accumulate, &a, &err))
    { accum_clear (&a); return o42_value_error (err); }

  result = o42_value_number (a.count > 0 ? a.min : 0.0);
  accum_clear (&a);
  return result;
}

static O42Value
fn_max (O42EvalContext *ctx, O42Operand *args, int n)
{
  Accum a;
  O42ErrorCode err = O42_ERR_VALUE;
  O42Value result;

  accum_init (&a, FALSE);
  if (!visit_numbers (ctx, args, n, accumulate, &a, &err))
    { accum_clear (&a); return o42_value_error (err); }

  result = o42_value_number (a.count > 0 ? a.max : 0.0);
  accum_clear (&a);
  return result;
}

static O42Value
fn_count (O42EvalContext *ctx, O42Operand *args, int n)
{
  int count = 0;

  /* COUNT asks how many are numbers and never complains about the rest:
   * an error in the range is not a number, and neither is "abc" as an
   * argument.  Only what looks like a number counts: a number, a date,
   * or a numeric text and a boolean given directly. */
  for (int i = 0; i < n; i++)
    {
      if (args[i].is_range)
        {
          const O42Range *r = &args[i].range;

          for (int row = r->row0; row <= r->row1; row++)
            for (int col = r->col0; col <= r->col1; col++)
              {
                O42Value v;

                ctx->get_cell (ctx, args[i].sheet, row, col, &v);
                if (v.type == O42_VALUE_NUMBER)
                  count++;
                o42_value_clear (&v);
              }
        }
      else
        {
          double number;
          O42ErrorCode e = O42_ERR_VALUE;

          if (args[i].value.type != O42_VALUE_ERROR &&
              o42_value_to_number (&args[i].value, &number, &e))
            count++;
        }
    }

  return o42_value_number (count);
}

static O42Value
fn_counta (O42EvalContext *ctx, O42Operand *args, int n)
{
  return o42_value_number (count_non_blank (ctx, args, n));
}

int
o42_compare_doubles (gconstpointer a, gconstpointer b)
{
  double x = *(const double *) a;
  double y = *(const double *) b;

  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static O42Value
fn_median (O42EvalContext *ctx, O42Operand *args, int n)
{
  Accum a;
  O42ErrorCode err = O42_ERR_VALUE;
  O42Value result;

  accum_init (&a, TRUE);
  if (!visit_numbers (ctx, args, n, accumulate, &a, &err))
    { accum_clear (&a); return o42_value_error (err); }

  if (a.count == 0)
    { accum_clear (&a); return o42_value_error (O42_ERR_NUM); }

  g_array_sort (a.values, compare_doubles);

  if (a.count % 2 == 1)
    result = o42_value_number (g_array_index (a.values, double, a.count / 2));
  else
    result = o42_value_number (
      (g_array_index (a.values, double, a.count / 2 - 1) +
       g_array_index (a.values, double, a.count / 2)) / 2.0);

  accum_clear (&a);
  return result;
}

/* ---- Logic ------------------------------------------------------------ */

static O42Value
fn_if (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value cond = operand_value (ctx, &args[0]);
  gboolean truth = FALSE;
  O42ErrorCode err = O42_ERR_VALUE;

  if (!o42_value_to_bool (&cond, &truth, &err))
    {
      o42_value_clear (&cond);
      return o42_value_error (err);
    }
  o42_value_clear (&cond);

  if (truth)
    return operand_value (ctx, &args[1]);

  if (n >= 3)
    return operand_value (ctx, &args[2]);

  return o42_value_bool (FALSE);
}

static O42Value
fn_and_or (O42EvalContext *ctx, O42Operand *args, int n, gboolean want_or)
{
  gboolean result = !want_or;
  int seen = 0;

  for (int i = 0; i < n; i++)
    {
      if (args[i].is_range)
        {
          const O42Range *r = &args[i].range;

          for (int row = r->row0; row <= r->row1; row++)
            for (int col = r->col0; col <= r->col1; col++)
              {
                O42Value v;
                gboolean b = FALSE;
                O42ErrorCode err = O42_ERR_VALUE;

                ctx->get_cell (ctx, args[i].sheet, row, col, &v);

                if (v.type == O42_VALUE_EMPTY)
                  { o42_value_clear (&v); continue; }

                if (!o42_value_to_bool (&v, &b, &err))
                  { o42_value_clear (&v); return o42_value_error (err); }

                o42_value_clear (&v);
                seen++;
                result = want_or ? (result || b) : (result && b);
              }
        }
      else
        {
          gboolean b = FALSE;
          O42ErrorCode err = O42_ERR_VALUE;

          if (!o42_value_to_bool (&args[i].value, &b, &err))
            return o42_value_error (err);

          seen++;
          result = want_or ? (result || b) : (result && b);
        }
    }

  if (seen == 0)
    return o42_value_error (O42_ERR_VALUE);

  return o42_value_bool (result);
}

static O42Value fn_and (O42EvalContext *c, O42Operand *a, int n) { return fn_and_or (c, a, n, FALSE); }
static O42Value fn_or  (O42EvalContext *c, O42Operand *a, int n) { return fn_and_or (c, a, n, TRUE); }

static O42Value
fn_not (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v = operand_value (ctx, &args[0]);
  gboolean b = FALSE;
  O42ErrorCode err = O42_ERR_VALUE;

  (void) n;

  if (!o42_value_to_bool (&v, &b, &err))
    { o42_value_clear (&v); return o42_value_error (err); }

  o42_value_clear (&v);
  return o42_value_bool (!b);
}

static O42Value fn_true  (O42EvalContext *c, O42Operand *a, int n) { (void)c;(void)a;(void)n; return o42_value_bool (TRUE); }
static O42Value fn_false (O42EvalContext *c, O42Operand *a, int n) { (void)c;(void)a;(void)n; return o42_value_bool (FALSE); }

static O42Value
fn_iferror (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v = operand_value (ctx, &args[0]);

  (void) n;

  if (v.type != O42_VALUE_ERROR)
    return v;

  o42_value_clear (&v);
  return operand_value (ctx, &args[1]);
}

/* ---- Tests ------------------------------------------------------------ */

static O42Value
fn_is (O42EvalContext *ctx, O42Operand *args, O42ValueType want, gboolean any_error)
{
  O42Value v = operand_value (ctx, &args[0]);
  gboolean result;

  if (any_error)
    result = (v.type == O42_VALUE_ERROR);
  else
    result = (v.type == want);

  o42_value_clear (&v);
  return o42_value_bool (result);
}

static O42Value fn_isblank  (O42EvalContext *c, O42Operand *a, int n) { (void)n; return fn_is (c, a, O42_VALUE_EMPTY, FALSE); }
static O42Value fn_isnumber (O42EvalContext *c, O42Operand *a, int n) { (void)n; return fn_is (c, a, O42_VALUE_NUMBER, FALSE); }
static O42Value fn_istext   (O42EvalContext *c, O42Operand *a, int n) { (void)n; return fn_is (c, a, O42_VALUE_TEXT, FALSE); }
static O42Value fn_islogical(O42EvalContext *c, O42Operand *a, int n) { (void)n; return fn_is (c, a, O42_VALUE_BOOL, FALSE); }
static O42Value fn_iserror  (O42EvalContext *c, O42Operand *a, int n) { (void)n; return fn_is (c, a, O42_VALUE_ERROR, TRUE); }

static O42Value
fn_na (O42EvalContext *ctx, O42Operand *args, int n)
{
  (void) ctx; (void) args; (void) n;
  return o42_value_error (O42_ERR_NA);
}

/* ---- Numbers ---------------------------------------------------------- */

#define UNARY_MATH(fname, expr)                                          \
  static O42Value                                                        \
  fname (O42EvalContext *ctx, O42Operand *args, int n)                   \
  {                                                                      \
    double x;                                                            \
    (void) n;                                                            \
    ARG_NUMBER (0, x);                                                   \
    return o42_value_number (expr);                                      \
  }

UNARY_MATH (fn_abs,   fabs (x))
UNARY_MATH (fn_int,   floor (x))
UNARY_MATH (fn_trunc, trunc (x))
UNARY_MATH (fn_exp,   exp (x))
UNARY_MATH (fn_sin,   sin (x))
UNARY_MATH (fn_cos,   cos (x))
UNARY_MATH (fn_tan,   tan (x))

static O42Value
fn_sign (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  return o42_value_number (x > 0 ? 1 : x < 0 ? -1 : 0);
}

static O42Value
fn_sqrt (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  if (x < 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (sqrt (x));
}

static O42Value
fn_ln (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  if (x <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (log (x));
}

static O42Value
fn_log10 (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  if (x <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (log10 (x));
}

static O42Value
fn_log (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, base = 10.0;

  ARG_NUMBER (0, x);
  if (n >= 2)
    ARG_NUMBER (1, base);

  if (x <= 0 || base <= 0 || base == 1.0)
    return o42_value_error (O42_ERR_NUM);

  return o42_value_number (log (x) / log (base));
}

static O42Value
fn_power (O42EvalContext *ctx, O42Operand *args, int n)
{
  double base, exponent, result;

  (void) n;
  ARG_NUMBER (0, base);
  ARG_NUMBER (1, exponent);

  result = pow (base, exponent);
  if (isnan (result) || isinf (result))
    return o42_value_error (O42_ERR_NUM);

  return o42_value_number (result);
}

static O42Value
fn_mod (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a, b, r;

  (void) n;
  ARG_NUMBER (0, a);
  ARG_NUMBER (1, b);

  if (b == 0.0)
    return o42_value_error (O42_ERR_DIV0);

  /* A spreadsheet's MOD takes the sign of the divisor, so MOD(-1,3) is 2
   * and not -1 the way C's fmod would have it. */
  r = fmod (a, b);
  if (r != 0.0 && ((r < 0) != (b < 0)))
    r += b;

  return o42_value_number (r);
}

double
o42_round_half_away (double x, int digits)
{
  double scale = pow (10.0, digits);
  double scaled = x * scale;

  if (!isfinite (scaled))
    return x;

  /* 1.005 is 1.00499999999999989 as a double, and rounding that binary
   * value gives 1.00 where every spreadsheet says 1.01: Excel decides at
   * the fifteenth significant digit, where the number reads 1.005
   * exactly.  A relative nudge of 1e-14 before the half is added is that
   * decision; it moves nothing that is not already within a rounding
   * error of the halfway point, and it is left out once the scaled
   * number is so large that the nudge itself would cross an integer. */
  if (fabs (scaled) < 1e13)
    scaled += copysign (fabs (scaled) * 1e-14, scaled);

  /* Half away from zero, which is what a spreadsheet does and what people
   * expect; C's rint rounds half to even. */
  return (scaled >= 0 ? floor (scaled + 0.5) : ceil (scaled - 0.5)) / scale;
}

static O42Value
fn_round (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, digits = 0;

  ARG_NUMBER (0, x);
  if (n >= 2)
    ARG_NUMBER (1, digits);

  return o42_value_number (round_half_away (x, (int) digits));
}

static O42Value
fn_rounddown (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, digits = 0, scale;

  ARG_NUMBER (0, x);
  if (n >= 2)
    ARG_NUMBER (1, digits);

  scale = pow (10.0, (int) digits);
  return o42_value_number (trunc (x * scale) / scale);
}

static O42Value
fn_roundup (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, digits = 0, scale, scaled;

  ARG_NUMBER (0, x);
  if (n >= 2)
    ARG_NUMBER (1, digits);

  scale = pow (10.0, (int) digits);
  scaled = x * scale;
  return o42_value_number ((scaled >= 0 ? ceil (scaled) : floor (scaled)) / scale);
}

static O42Value
fn_pi (O42EvalContext *ctx, O42Operand *args, int n)
{
  (void) ctx; (void) args; (void) n;
  return o42_value_number (G_PI);
}

/* ---- Criteria --------------------------------------------------------- */

/* What COUNTIF and SUMIF take as their second argument: a value to equal,
 * or text such as ">5", "<>x" or "a*" with an operator in front and
 * wildcards allowed.  Parsed once per call, not once per cell. */
typedef enum { CRIT_EQ, CRIT_NE, CRIT_LT, CRIT_GT, CRIT_LE, CRIT_GE } CritOp;

typedef struct {
  CritOp        op;
  O42Value      value;
  GPatternSpec *pattern;    /* when the value is text with * or ? in it */
} Criterion;

static void
criterion_init (Criterion *c, const O42Value *from)
{
  c->op = CRIT_EQ;
  c->pattern = NULL;

  if (from->type != O42_VALUE_TEXT)
    {
      c->value = o42_value_copy (from);
      return;
    }

  {
    const char *s = from->as.text;
    O42Value probe;
    double n;
    O42ErrorCode err = O42_ERR_VALUE;

    if (g_str_has_prefix (s, "<>"))      { c->op = CRIT_NE; s += 2; }
    else if (g_str_has_prefix (s, "<=")) { c->op = CRIT_LE; s += 2; }
    else if (g_str_has_prefix (s, ">=")) { c->op = CRIT_GE; s += 2; }
    else if (*s == '<')                  { c->op = CRIT_LT; s += 1; }
    else if (*s == '>')                  { c->op = CRIT_GT; s += 1; }
    else if (*s == '=')                  { c->op = CRIT_EQ; s += 1; }

    probe = o42_value_text (s);
    if (o42_value_to_number (&probe, &n, &err))
      c->value = o42_value_number (n);
    else if (g_ascii_strcasecmp (s, "TRUE") == 0)
      c->value = o42_value_bool (TRUE);
    else if (g_ascii_strcasecmp (s, "FALSE") == 0)
      c->value = o42_value_bool (FALSE);
    else
      {
        c->value = o42_value_text (s);
        if (strchr (s, '*') != NULL || strchr (s, '?') != NULL)
          {
            char *folded = g_utf8_casefold (s, -1);
            c->pattern = g_pattern_spec_new (folded);
            g_free (folded);
          }
      }
    o42_value_clear (&probe);
  }
}

static void
criterion_clear (Criterion *c)
{
  o42_value_clear (&c->value);
  if (c->pattern != NULL)
    g_pattern_spec_free (c->pattern);
}

static gboolean
criterion_match (const Criterion *c, const O42Value *v)
{
  int cmp;

  if (c->pattern != NULL)
    {
      gboolean hit = FALSE;

      if (v->type == O42_VALUE_TEXT)
        {
          char *folded = g_utf8_casefold (v->as.text, -1);
          hit = g_pattern_spec_match_string (c->pattern, folded);
          g_free (folded);
        }
      return (c->op == CRIT_NE) ? !hit : hit;
    }

  /* "<>" matches everything that is not equal, blanks and text included;
   * the ordered comparisons only make sense between values of one kind. */
  if (c->op == CRIT_NE)
    {
      /* "<>" alone means "not blank". */
      if (c->value.type == O42_VALUE_TEXT && c->value.as.text[0] == 0)
        return v->type != O42_VALUE_EMPTY;
      return !(v->type == c->value.type && o42_value_compare (v, &c->value) == 0);
    }

  if (v->type != c->value.type)
    {
      if (v->type == O42_VALUE_EMPTY && c->value.type == O42_VALUE_TEXT &&
          c->value.as.text[0] == 0)
        return c->op == CRIT_EQ;
      return FALSE;
    }

  cmp = o42_value_compare (v, &c->value);

  switch (c->op)
    {
    case CRIT_EQ: return cmp == 0;
    case CRIT_LT: return cmp < 0;
    case CRIT_GT: return cmp > 0;
    case CRIT_LE: return cmp <= 0;
    case CRIT_GE: return cmp >= 0;
    default:      return FALSE;
    }
}

static O42Value
fn_countif (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value criterion_value;
  Criterion crit;
  int count = 0;

  (void) n;

  if (!args[0].is_range)
    return o42_value_error (O42_ERR_VALUE);

  criterion_value = operand_value (ctx, &args[1]);
  criterion_init (&crit, &criterion_value);
  o42_value_clear (&criterion_value);

  for (int row = args[0].range.row0; row <= args[0].range.row1; row++)
    for (int col = args[0].range.col0; col <= args[0].range.col1; col++)
      {
        O42Value v;

        ctx->get_cell (ctx, args[0].sheet, row, col, &v);
        if (criterion_match (&crit, &v))
          count++;
        o42_value_clear (&v);
      }

  criterion_clear (&crit);
  return o42_value_number (count);
}

/* SUMIF and AVERAGEIF share everything but the last line. */
static O42Value
fn_sumif_averageif (O42EvalContext *ctx, O42Operand *args, int n, gboolean average)
{
  O42Value criterion_value;
  Criterion crit;
  const O42Range *test, *sum;
  double total = 0.0;
  int count = 0;

  if (!args[0].is_range)
    return o42_value_error (O42_ERR_VALUE);
  if (n >= 3 && !args[2].is_range)
    return o42_value_error (O42_ERR_VALUE);

  test = &args[0].range;
  sum = (n >= 3) ? &args[2].range : test;
  criterion_value = operand_value (ctx, &args[1]);
  criterion_init (&crit, &criterion_value);
  o42_value_clear (&criterion_value);

  for (int row = test->row0; row <= test->row1; row++)
    for (int col = test->col0; col <= test->col1; col++)
      {
        O42Value v;
        gboolean matched;

        ctx->get_cell (ctx, args[0].sheet, row, col, &v);
        matched = criterion_match (&crit, &v);
        o42_value_clear (&v);

        if (!matched)
          continue;

        /* The cell to add is the one in the same position of the summing
         * range, offset from its corner. */
        {
          int srow = sum->row0 + (row - test->row0);
          int scol = sum->col0 + (col - test->col0);
          O42Value sv;

          ctx->get_cell (ctx, (n >= 3) ? args[2].sheet : args[0].sheet, srow, scol, &sv);
          if (sv.type == O42_VALUE_NUMBER)
            {
              total += sv.as.number;
              count++;
            }
          o42_value_clear (&sv);
        }
      }

  criterion_clear (&crit);

  if (average)
    return (count == 0) ? o42_value_error (O42_ERR_DIV0)
                        : o42_value_number (total / count);
  return o42_value_number (total);
}

static O42Value fn_sumif     (O42EvalContext *c, O42Operand *a, int n) { return fn_sumif_averageif (c, a, n, FALSE); }
static O42Value fn_averageif (O42EvalContext *c, O42Operand *a, int n) { return fn_sumif_averageif (c, a, n, TRUE); }

static O42Value
fn_countblank (O42EvalContext *ctx, O42Operand *args, int n)
{
  int count = 0;

  (void) n;

  if (!args[0].is_range)
    return o42_value_error (O42_ERR_VALUE);

  for (int row = args[0].range.row0; row <= args[0].range.row1; row++)
    for (int col = args[0].range.col0; col <= args[0].range.col1; col++)
      {
        O42Value v;

        ctx->get_cell (ctx, args[0].sheet, row, col, &v);
        if (v.type == O42_VALUE_EMPTY ||
            (v.type == O42_VALUE_TEXT && *v.as.text == '\0'))
          count++;
        o42_value_clear (&v);
      }

  return o42_value_number (count);
}

/* ---- Lookup ----------------------------------------------------------- */

/* The value at position i along a vector: down a column for a vertical
 * one, across a row otherwise. */
static void
vector_get (O42EvalContext *ctx, const O42Operand *op, gboolean vertical,
            int i, O42Value *out)
{
  const O42Range *r = &op->range;

  if (vertical)
    ctx->get_cell (ctx, op->sheet, r->row0 + i, r->col0, out);
  else
    ctx->get_cell (ctx, op->sheet, r->row0, r->col0 + i, out);
}

static int
vector_length (const O42Range *r, gboolean vertical)
{
  return vertical ? r->row1 - r->row0 + 1 : r->col1 - r->col0 + 1;
}

/* MATCH's three modes, shared by LOOKUP and the approximate forms of
 * VLOOKUP and HLOOKUP.  Type 1 wants the vector ascending and finds the
 * last value not above the needle; -1 wants it descending and finds the
 * last value not below; 0 is an exact match, with wildcards for text.
 * Returns the index, or -1. */
static int
match_in_vector (O42EvalContext *ctx, const O42Value *needle,
                 const O42Operand *op, gboolean vertical, int type)
{
  const O42Range *r = &op->range;
  int len = vector_length (r, vertical);
  int best = -1;

  if (type == 0)
    {
      Criterion crit;

      criterion_init (&crit, needle);
      for (int i = 0; i < len; i++)
        {
          O42Value v;
          gboolean hit;

          vector_get (ctx, op, vertical, i, &v);
          hit = criterion_match (&crit, &v);
          o42_value_clear (&v);
          if (hit) { best = i; break; }
        }
      criterion_clear (&crit);
      return best;
    }

  /* The last value not past the needle, walking until one is.  Excel
   * binary-searches, which on an unsorted table gives an answer that looks
   * plausible and is wrong; a walk gives the same answer on a sorted one
   * and a predictable one otherwise.  Values of another type than the
   * needle are skipped. */
  for (int i = 0; i < len; i++)
    {
      O42Value v;
      int cmp;

      vector_get (ctx, op, vertical, i, &v);
      if (v.type != needle->type)
        {
          o42_value_clear (&v);
          continue;
        }
      cmp = o42_value_compare (&v, needle);
      o42_value_clear (&v);
      if (type < 0)
        cmp = -cmp;
      if (cmp > 0)
        break;
      best = i;
    }

  return best;
}

static O42Value
fn_match (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value needle;
  double type = 1;
  gboolean vertical;
  int found;

  if (!args[1].is_range)
    return o42_value_error (O42_ERR_VALUE);

  if (n >= 3)
    ARG_NUMBER (2, type);

  needle = operand_value (ctx, &args[0]);
  if (needle.type == O42_VALUE_ERROR)
    return needle;

  vertical = (args[1].range.col0 == args[1].range.col1);
  found = match_in_vector (ctx, &needle, &args[1], vertical,
                           type > 0 ? 1 : type < 0 ? -1 : 0);
  o42_value_clear (&needle);

  return (found < 0) ? o42_value_error (O42_ERR_NA)
                     : o42_value_number (found + 1);
}

static O42Value
fn_lookup (O42EvalContext *ctx, O42Operand *args, int n, gboolean vertical)
{
  O42Value needle;
  const O42Range *table;
  double index;
  gboolean approximate = TRUE;
  int best;
  O42Value result;

  if (!args[1].is_range)
    return o42_value_error (O42_ERR_VALUE);

  ARG_NUMBER (2, index);
  if (n >= 4)
    {
      O42Value v = operand_value (ctx, &args[3]);
      O42ErrorCode err = O42_ERR_VALUE;
      gboolean ok = o42_value_to_bool (&v, &approximate, &err);
      o42_value_clear (&v);
      if (!ok) return o42_value_error (err);
    }

  if (index < 1)
    return o42_value_error (O42_ERR_VALUE);

  needle = operand_value (ctx, &args[0]);
  if (needle.type == O42_VALUE_ERROR)
    return needle;

  table = &args[1].range;
  best = match_in_vector (ctx, &needle, &args[1], vertical, approximate ? 1 : 0);
  o42_value_clear (&needle);

  if (best < 0)
    return o42_value_error (O42_ERR_NA);

  if (vertical)
    {
      int col = table->col0 + (int) index - 1;
      if (col > table->col1)
        return o42_value_error (O42_ERR_REF);
      ctx->get_cell (ctx, args[1].sheet, table->row0 + best, col, &result);
    }
  else
    {
      int row = table->row0 + (int) index - 1;
      if (row > table->row1)
        return o42_value_error (O42_ERR_REF);
      ctx->get_cell (ctx, args[1].sheet, row, table->col0 + best, &result);
    }

  return result;
}

static O42Value fn_vlookup (O42EvalContext *c, O42Operand *a, int n) { return fn_lookup (c, a, n, TRUE); }
static O42Value fn_hlookup (O42EvalContext *c, O42Operand *a, int n) { return fn_lookup (c, a, n, FALSE); }

/* LOOKUP's vector form: the value in the result vector at the position
 * the needle is found in the lookup vector. */
static O42Value
fn_lookup_vector (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value needle, result;
  const O42Range *from, *to;
  gboolean vertical;
  int found;

  if (!args[1].is_range || (n >= 3 && !args[2].is_range))
    return o42_value_error (O42_ERR_VALUE);

  from = &args[1].range;
  to = (n >= 3) ? &args[2].range : from;
  vertical = (from->col0 == from->col1);

  needle = operand_value (ctx, &args[0]);
  if (needle.type == O42_VALUE_ERROR)
    return needle;

  found = match_in_vector (ctx, &needle, &args[1], vertical, 1);
  o42_value_clear (&needle);

  if (found < 0)
    return o42_value_error (O42_ERR_NA);

  /* The array form, one range with two columns: the result comes from the
   * last column. */
  if (n < 3 && from->col0 != from->col1 && from->row0 != from->row1)
    {
      ctx->get_cell (ctx, args[1].sheet, from->row0 + found, from->col1, &result);
      return result;
    }

  if (found >= vector_length (to, to->col0 == to->col1))
    return o42_value_error (O42_ERR_NA);

  vector_get (ctx, (n >= 3) ? &args[2] : &args[1], to->col0 == to->col1, found, &result);
  return result;
}

static O42Value
fn_index (O42EvalContext *ctx, O42Operand *args, int n)
{
  const O42Range *r;
  double row = 1, col = 1;
  O42Value result;

  if (!args[0].is_range)
    return o42_value_error (O42_ERR_VALUE);

  r = &args[0].range;
  if (n >= 2) ARG_NUMBER (1, row);
  if (n >= 3) ARG_NUMBER (2, col);

  /* INDEX(A1:F1, 3) on a single row means the third column. */
  if (n == 2 && r->row0 == r->row1 && r->col0 != r->col1)
    {
      col = row;
      row = 1;
    }

  if (row < 1 || col < 1 || row > O42_MAX_ROWS || col > O42_MAX_COLS ||
      r->row0 + (int) row - 1 > r->row1 || r->col0 + (int) col - 1 > r->col1)
    return o42_value_error (O42_ERR_REF);

  ctx->get_cell (ctx, args[0].sheet, r->row0 + (int) row - 1, r->col0 + (int) col - 1, &result);
  return result;
}

static O42Value
fn_choose (O42EvalContext *ctx, O42Operand *args, int n)
{
  double index;

  ARG_NUMBER (0, index);

  /* Compared as a double: cast first and 3e9 wraps to a negative index. */
  if (index < 1 || index >= n)
    return o42_value_error (O42_ERR_VALUE);

  return operand_value (ctx, &args[(int) index]);
}

static O42Value
fn_row_column (O42EvalContext *ctx, O42Operand *args, int n, gboolean row)
{
  if (n == 0)
    return o42_value_number ((row ? ctx->row : ctx->col) + 1);

  if (!args[0].is_range)
    return o42_value_error (O42_ERR_VALUE);

  return o42_value_number ((row ? args[0].range.row0 : args[0].range.col0) + 1);
}

static O42Value fn_row    (O42EvalContext *c, O42Operand *a, int n) { return fn_row_column (c, a, n, TRUE); }
static O42Value fn_column (O42EvalContext *c, O42Operand *a, int n) { return fn_row_column (c, a, n, FALSE); }

/* ---- More tests ------------------------------------------------------- */

static O42Value
fn_isna (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v = operand_value (ctx, &args[0]);
  gboolean result = (v.type == O42_VALUE_ERROR && v.as.error == O42_ERR_NA);
  (void) n;
  o42_value_clear (&v);
  return o42_value_bool (result);
}

static O42Value
fn_iserr (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v = operand_value (ctx, &args[0]);
  gboolean result = (v.type == O42_VALUE_ERROR && v.as.error != O42_ERR_NA);
  (void) n;
  o42_value_clear (&v);
  return o42_value_bool (result);
}

static O42Value
fn_isnontext (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v = operand_value (ctx, &args[0]);
  gboolean result = (v.type != O42_VALUE_TEXT);
  (void) n;
  o42_value_clear (&v);
  return o42_value_bool (result);
}

static O42Value
fn_iseven_odd (O42EvalContext *ctx, O42Operand *args, int n, gboolean even)
{
  double x;
  gboolean is_even;
  (void) n;
  ARG_NUMBER (0, x);
  is_even = fmod (trunc (fabs (x)), 2.0) == 0.0;
  return o42_value_bool (even ? is_even : !is_even);
}

static O42Value fn_iseven (O42EvalContext *c, O42Operand *a, int n) { return fn_iseven_odd (c, a, n, TRUE); }
static O42Value fn_isodd  (O42EvalContext *c, O42Operand *a, int n) { return fn_iseven_odd (c, a, n, FALSE); }

static O42Value
fn_type (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v;
  int code;

  (void) n;

  if (args[0].is_range &&
      (args[0].range.row0 != args[0].range.row1 ||
       args[0].range.col0 != args[0].range.col1))
    return o42_value_number (64);

  v = operand_value (ctx, &args[0]);
  switch (v.type)
    {
    case O42_VALUE_TEXT:  code = 2;  break;
    case O42_VALUE_BOOL:  code = 4;  break;
    case O42_VALUE_ERROR: code = 16; break;
    default:              code = 1;  break;
    }
  o42_value_clear (&v);
  return o42_value_number (code);
}

static O42Value
fn_error_type (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v = operand_value (ctx, &args[0]);
  int code;

  (void) n;

  if (v.type != O42_VALUE_ERROR)
    { o42_value_clear (&v); return o42_value_error (O42_ERR_NA); }

  switch (v.as.error)
    {
    case O42_ERR_NULL:  code = 1; break;
    case O42_ERR_DIV0:  code = 2; break;
    case O42_ERR_VALUE: code = 3; break;
    case O42_ERR_REF:
    case O42_ERR_CIRCULAR: code = 4; break;
    case O42_ERR_NAME:  code = 5; break;
    case O42_ERR_NUM:   code = 6; break;
    default:            code = 7; break;
    }
  return o42_value_number (code);
}

static O42Value
fn_n (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v = operand_value (ctx, &args[0]);
  O42Value result;

  (void) n;

  switch (v.type)
    {
    case O42_VALUE_NUMBER: result = o42_value_number (v.as.number); break;
    case O42_VALUE_BOOL:   result = o42_value_number (v.as.boolean ? 1 : 0); break;
    case O42_VALUE_ERROR:  result = o42_value_copy (&v); break;
    default:               result = o42_value_number (0); break;
    }
  o42_value_clear (&v);
  return result;
}

static O42Value
fn_t (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v = operand_value (ctx, &args[0]);
  (void) n;
  if (v.type == O42_VALUE_TEXT || v.type == O42_VALUE_ERROR)
    return v;
  o42_value_clear (&v);
  return o42_value_text ("");
}

/* ---- More numbers ----------------------------------------------------- */

UNARY_MATH (fn_sinh,  sinh (x))
UNARY_MATH (fn_cosh,  cosh (x))
UNARY_MATH (fn_tanh,  tanh (x))
UNARY_MATH (fn_atan,  atan (x))
UNARY_MATH (fn_degrees, x * 180.0 / G_PI)
UNARY_MATH (fn_radians, x * G_PI / 180.0)

static O42Value
fn_asin_acos (O42EvalContext *ctx, O42Operand *args, int n, gboolean sine)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  if (x < -1 || x > 1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (sine ? asin (x) : acos (x));
}

static O42Value fn_asin (O42EvalContext *c, O42Operand *a, int n) { return fn_asin_acos (c, a, n, TRUE); }
static O42Value fn_acos (O42EvalContext *c, O42Operand *a, int n) { return fn_asin_acos (c, a, n, FALSE); }

static O42Value
fn_atan2 (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, y;
  (void) n;
  /* Excel's argument order is x then y, the reverse of C's. */
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, y);
  if (x == 0 && y == 0)
    return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (atan2 (y, x));
}

static O42Value
fn_even_odd (O42EvalContext *ctx, O42Operand *args, int n, gboolean even)
{
  double x, r;
  (void) n;
  ARG_NUMBER (0, x);

  /* Away from zero, to the next even (or odd) integer. */
  r = ceil (fabs (x));
  if (even)
    {
      if (fmod (r, 2.0) != 0.0) r += 1;
    }
  else
    {
      if (fmod (r, 2.0) == 0.0) r += 1;
    }
  return o42_value_number (x < 0 ? -r : r);
}

static O42Value fn_even (O42EvalContext *c, O42Operand *a, int n) { return fn_even_odd (c, a, n, TRUE); }
static O42Value fn_odd  (O42EvalContext *c, O42Operand *a, int n) { return fn_even_odd (c, a, n, FALSE); }

static O42Value
fn_ceiling_floor (O42EvalContext *ctx, O42Operand *args, int n, gboolean up)
{
  double x, sig = 1, q;

  ARG_NUMBER (0, x);
  if (n >= 2)
    ARG_NUMBER (1, sig);

  if (sig == 0)
    return o42_value_number (0);
  if ((x > 0 && sig < 0))
    return o42_value_error (O42_ERR_NUM);

  q = x / sig;
  /* Rounding the quotient to a few ulps first keeps CEILING(0.3, 0.1)
   * from being 0.4, which is the answer the raw division gives. */
  q = round_half_away (q, 10);
  return o42_value_number ((up ? ceil (q) : floor (q)) * sig);
}

static O42Value fn_ceiling (O42EvalContext *c, O42Operand *a, int n) { return fn_ceiling_floor (c, a, n, TRUE); }
static O42Value fn_floor   (O42EvalContext *c, O42Operand *a, int n) { return fn_ceiling_floor (c, a, n, FALSE); }

static O42Value
fn_mround (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, m;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, m);
  if (m == 0)
    return o42_value_number (0);
  if ((x < 0) != (m < 0))
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (round_half_away (x / m, 0) * m);
}

static O42Value
fn_quotient (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a, b;
  (void) n;
  ARG_NUMBER (0, a);
  ARG_NUMBER (1, b);
  if (b == 0)
    return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (trunc (a / b));
}

static O42Value
fn_fact (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, r = 1;
  (void) n;
  ARG_NUMBER (0, x);
  if (x < 0 || x > 170)
    return o42_value_error (O42_ERR_NUM);
  for (int i = 2; i <= (int) x; i++)
    r *= i;
  return o42_value_number (r);
}

/* Binomial coefficients by the multiplicative formula, which stays exact
 * in a double far longer than the ratio of factorials does. */
static double
binomial (double n, double k)
{
  double r = 1;

  if (k > n - k)
    k = n - k;
  for (int i = 1; i <= (int) k; i++)
    r = r * (n - k + i) / i;
  return round_half_away (r, 0);
}

static O42Value
fn_combin (O42EvalContext *ctx, O42Operand *args, int n)
{
  double total, chosen;
  (void) n;
  ARG_NUMBER (0, total);
  ARG_NUMBER (1, chosen);
  total = trunc (total); chosen = trunc (chosen);
  if (total < 0 || chosen < 0 || chosen > total)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (binomial (total, chosen));
}

static O42Value
fn_permut (O42EvalContext *ctx, O42Operand *args, int n)
{
  double total, chosen, r = 1;
  (void) n;
  ARG_NUMBER (0, total);
  ARG_NUMBER (1, chosen);
  total = trunc (total); chosen = trunc (chosen);
  if (total < 0 || chosen < 0 || chosen > total)
    return o42_value_error (O42_ERR_NUM);
  for (int i = 0; i < (int) chosen; i++)
    r *= total - i;
  return o42_value_number (r);
}

static double
gcd_of (double a, double b)
{
  a = fabs (trunc (a));
  b = fabs (trunc (b));
  while (b != 0)
    {
      double t = fmod (a, b);
      a = b;
      b = t;
    }
  return a;
}

static O42Value
fn_gcd_lcm (O42EvalContext *ctx, O42Operand *args, int n, gboolean want_gcd)
{
  Accum a;
  O42ErrorCode err = O42_ERR_VALUE;
  double r;

  accum_init (&a, TRUE);
  if (!visit_numbers (ctx, args, n, accumulate, &a, &err))
    { accum_clear (&a); return o42_value_error (err); }

  r = want_gcd ? 0 : 1;
  for (guint i = 0; i < a.values->len; i++)
    {
      double v = g_array_index (a.values, double, i);

      if (v < 0)
        { accum_clear (&a); return o42_value_error (O42_ERR_NUM); }
      if (want_gcd)
        r = gcd_of (r, v);
      else
        {
          double g = gcd_of (r, v);
          r = (g == 0) ? 0 : r / g * trunc (v);
        }
    }

  accum_clear (&a);
  return o42_value_number (r);
}

static O42Value fn_gcd (O42EvalContext *c, O42Operand *a, int n) { return fn_gcd_lcm (c, a, n, TRUE); }
static O42Value fn_lcm (O42EvalContext *c, O42Operand *a, int n) { return fn_gcd_lcm (c, a, n, FALSE); }

static gboolean
accumulate_squares (double n, gpointer user)
{
  double *sum = user;
  *sum += n * n;
  return TRUE;
}

static O42Value
fn_sumsq (O42EvalContext *ctx, O42Operand *args, int n)
{
  double sum = 0;
  O42ErrorCode err = O42_ERR_VALUE;

  if (!visit_numbers (ctx, args, n, accumulate_squares, &sum, &err))
    return o42_value_error (err);
  return o42_value_number (sum);
}

static O42Value
fn_sumproduct (O42EvalContext *ctx, O42Operand *args, int n)
{
  int rows, cols;
  double total = 0;

  for (int i = 0; i < n; i++)
    if (!args[i].is_range)
      return o42_value_error (O42_ERR_VALUE);

  rows = args[0].range.row1 - args[0].range.row0 + 1;
  cols = args[0].range.col1 - args[0].range.col0 + 1;

  for (int i = 1; i < n; i++)
    if (args[i].range.row1 - args[i].range.row0 + 1 != rows ||
        args[i].range.col1 - args[i].range.col0 + 1 != cols)
      return o42_value_error (O42_ERR_VALUE);

  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      {
        double product = 1;

        for (int i = 0; i < n; i++)
          {
            O42Value v;

            ctx->get_cell (ctx, args[i].sheet, args[i].range.row0 + r, args[i].range.col0 + c, &v);
            if (v.type == O42_VALUE_ERROR)
              {
                O42ErrorCode e = v.as.error;
                o42_value_clear (&v);
                return o42_value_error (e);
              }
            /* Anything that is not a number counts as zero, as in Excel. */
            product *= (v.type == O42_VALUE_NUMBER) ? v.as.number : 0.0;
            o42_value_clear (&v);
          }
        total += product;
      }

  return o42_value_number (total);
}

static O42Value
fn_rand (O42EvalContext *ctx, O42Operand *args, int n)
{
  (void) ctx; (void) args; (void) n;
  return o42_value_number (g_random_double ());
}

static O42Value
fn_randbetween (O42EvalContext *ctx, O42Operand *args, int n)
{
  double lo, hi;
  (void) n;
  ARG_NUMBER (0, lo);
  ARG_NUMBER (1, hi);
  lo = ceil (lo); hi = floor (hi);
  if (hi < lo)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (lo + floor (g_random_double () * (hi - lo + 1)));
}

static O42Value
fn_sqrtpi (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  if (x < 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (sqrt (x * G_PI));
}

/* ---- Money ------------------------------------------------------------ */

/* The annuity functions all come from one identity relating present
 * value, future value, payment, rate and term; each solves it for one of
 * the five.  `type` 1 means payments at the start of each period. */
static double
annuity_fv (double rate, double nper, double pmt, double pv, int type)
{
  if (rate == 0)
    return -(pv + pmt * nper);
  {
    double f = pow (1 + rate, nper);
    return -(pv * f + pmt * (1 + rate * type) * (f - 1) / rate);
  }
}

static double
annuity_pmt (double rate, double nper, double pv, double fv, int type)
{
  if (rate == 0)
    return -(pv + fv) / nper;
  {
    double f = pow (1 + rate, nper);
    return -(pv * f + fv) * rate / ((1 + rate * type) * (f - 1));
  }
}

static double
annuity_pv (double rate, double nper, double pmt, double fv, int type)
{
  if (rate == 0)
    return -(fv + pmt * nper);
  {
    double f = pow (1 + rate, nper);
    return -(fv + pmt * (1 + rate * type) * (f - 1) / rate) / f;
  }
}

/* Reads the optional trailing arguments the annuity functions share. */
#define ANNUITY_OPTIONAL(fv_index, type_index)                            \
  G_STMT_START {                                                          \
    if (n > (fv_index))   ARG_NUMBER ((fv_index), fv);                    \
    if (n > (type_index)) { double t_; ARG_NUMBER ((type_index), t_);     \
                            type = (t_ != 0) ? 1 : 0; }                   \
  } G_STMT_END

static O42Value
fn_pmt (O42EvalContext *ctx, O42Operand *args, int n)
{
  double rate, nper, pv, fv = 0;
  int type = 0;
  ARG_NUMBER (0, rate); ARG_NUMBER (1, nper); ARG_NUMBER (2, pv);
  ANNUITY_OPTIONAL (3, 4);
  if (nper == 0) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (annuity_pmt (rate, nper, pv, fv, type));
}

static O42Value
fn_fv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double rate, nper, pmt, pv = 0;
  int type = 0;
  ARG_NUMBER (0, rate); ARG_NUMBER (1, nper); ARG_NUMBER (2, pmt);
  if (n > 3) ARG_NUMBER (3, pv);
  if (n > 4) { double t_; ARG_NUMBER (4, t_); type = (t_ != 0) ? 1 : 0; }
  return o42_value_number (annuity_fv (rate, nper, pmt, pv, type));
}

static O42Value
fn_pv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double rate, nper, pmt, fv = 0;
  int type = 0;
  ARG_NUMBER (0, rate); ARG_NUMBER (1, nper); ARG_NUMBER (2, pmt);
  ANNUITY_OPTIONAL (3, 4);
  return o42_value_number (annuity_pv (rate, nper, pmt, fv, type));
}

static O42Value
fn_nper (O42EvalContext *ctx, O42Operand *args, int n)
{
  double rate, pmt, pv, fv = 0;
  int type = 0;
  ARG_NUMBER (0, rate); ARG_NUMBER (1, pmt); ARG_NUMBER (2, pv);
  ANNUITY_OPTIONAL (3, 4);

  if (rate == 0)
    {
      if (pmt == 0) return o42_value_error (O42_ERR_NUM);
      return o42_value_number (-(pv + fv) / pmt);
    }
  {
    double a = pmt * (1 + rate * type) - fv * rate;
    double b = pv * rate + pmt * (1 + rate * type);
    if (a / b <= 0) return o42_value_error (O42_ERR_NUM);
    return o42_value_number (log (a / b) / log (1 + rate));
  }
}

/* RATE has no closed form; Newton's method on the annuity identity from a
 * guess, as Excel does, with a bisection fallback if Newton wanders. */
static O42Value
fn_rate (O42EvalContext *ctx, O42Operand *args, int n)
{
  double nper, pmt, pv, fv = 0, guess = 0.1;
  int type = 0;
  double r;

  ARG_NUMBER (0, nper); ARG_NUMBER (1, pmt); ARG_NUMBER (2, pv);
  ANNUITY_OPTIONAL (3, 4);
  if (n > 5) ARG_NUMBER (5, guess);

  r = guess;
  for (int i = 0; i < 100; i++)
    {
      double f, df, next;
      double h = 1e-6;

      f = annuity_fv (r, nper, pmt, pv, type) - fv;
      df = (annuity_fv (r + h, nper, pmt, pv, type) - annuity_fv (r - h, nper, pmt, pv, type)) / (2 * h);
      if (df == 0 || isnan (df))
        break;
      next = r - f / df;
      if (fabs (next - r) < 1e-12)
        return o42_value_number (next);
      r = next;
      if (r <= -1)
        r = -0.99;
    }

  return o42_value_error (O42_ERR_NUM);
}

static O42Value
fn_ipmt_ppmt (O42EvalContext *ctx, O42Operand *args, int n, gboolean interest)
{
  double rate, per, nper, pv, fv = 0, pmt, ipmt;
  int type = 0;

  ARG_NUMBER (0, rate); ARG_NUMBER (1, per); ARG_NUMBER (2, nper); ARG_NUMBER (3, pv);
  ANNUITY_OPTIONAL (4, 5);

  if (per < 1 || per > nper || nper == 0)
    return o42_value_error (O42_ERR_NUM);

  pmt = annuity_pmt (rate, nper, pv, fv, type);

  /* The interest in a period is the rate on what was owed at its start,
   * which is the balance after the payments before it -- one fewer when
   * payments fall at the start of a period, and none at all in the first
   * such period. */
  if (type == 1 && per == 1)
    ipmt = 0;
  else if (type == 1)
    ipmt = (annuity_fv (rate, per - 2, pmt, pv, 1) - pmt) * rate;
  else
    ipmt = annuity_fv (rate, per - 1, pmt, pv, 0) * rate;

  return o42_value_number (interest ? ipmt : pmt - ipmt);
}

static O42Value fn_ipmt (O42EvalContext *c, O42Operand *a, int n) { return fn_ipmt_ppmt (c, a, n, TRUE); }
static O42Value fn_ppmt (O42EvalContext *c, O42Operand *a, int n) { return fn_ipmt_ppmt (c, a, n, FALSE); }

static O42Value
fn_npv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double rate, total = 0;
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;

  ARG_NUMBER (0, rate);
  if (!collect_numbers (ctx, args + 1, n - 1, &values, &err))
    return o42_value_error (err);

  /* The first cash flow is at the end of the first period, not now: that
   * is what makes NPV differ from a plain present value and trips up
   * everyone once. */
  for (guint i = 0; i < values->len; i++)
    total += g_array_index (values, double, i) / pow (1 + rate, i + 1);

  g_array_free (values, TRUE);
  return o42_value_number (total);
}

static double
npv_at (const GArray *values, double rate)
{
  double total = 0;
  for (guint i = 0; i < values->len; i++)
    total += g_array_index (values, double, i) / pow (1 + rate, i);
  return total;
}

static O42Value
fn_irr (O42EvalContext *ctx, O42Operand *args, int n)
{
  double guess = 0.1, r;
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;

  if (n >= 2)
    ARG_NUMBER (1, guess);
  if (!collect_numbers (ctx, args, 1, &values, &err))
    return o42_value_error (err);

  r = guess;
  for (int i = 0; i < 200; i++)
    {
      double f = npv_at (values, r);
      double h = 1e-7;
      double df = (npv_at (values, r + h) - npv_at (values, r - h)) / (2 * h);
      double next;

      if (df == 0 || isnan (df))
        break;
      next = r - f / df;
      if (next <= -1)
        next = (r - 1) / 2;
      if (fabs (next - r) < 1e-12)
        {
          g_array_free (values, TRUE);
          return o42_value_number (next);
        }
      r = next;
    }

  g_array_free (values, TRUE);
  return o42_value_error (O42_ERR_NUM);
}

static O42Value
fn_mirr (O42EvalContext *ctx, O42Operand *args, int n)
{
  double finance, reinvest, neg = 0, pos = 0;
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  guint count;

  (void) n;
  ARG_NUMBER (1, finance);
  ARG_NUMBER (2, reinvest);
  if (!collect_numbers (ctx, args, 1, &values, &err))
    return o42_value_error (err);

  count = values->len;
  for (guint i = 0; i < count; i++)
    {
      double v = g_array_index (values, double, i);
      if (v < 0) neg += v / pow (1 + finance, i);
      else       pos += v * pow (1 + reinvest, count - 1 - i);
    }
  g_array_free (values, TRUE);

  if (neg == 0 || pos == 0 || count < 2)
    return o42_value_error (O42_ERR_DIV0);

  return o42_value_number (pow (-pos / neg, 1.0 / (count - 1)) - 1);
}

static O42Value
fn_sln (O42EvalContext *ctx, O42Operand *args, int n)
{
  double cost, salvage, life;
  (void) n;
  ARG_NUMBER (0, cost); ARG_NUMBER (1, salvage); ARG_NUMBER (2, life);
  if (life == 0) return o42_value_error (O42_ERR_DIV0);
  return o42_value_number ((cost - salvage) / life);
}

static O42Value
fn_syd (O42EvalContext *ctx, O42Operand *args, int n)
{
  double cost, salvage, life, per;
  (void) n;
  ARG_NUMBER (0, cost); ARG_NUMBER (1, salvage); ARG_NUMBER (2, life); ARG_NUMBER (3, per);
  if (life <= 0 || per < 1 || per > life) return o42_value_error (O42_ERR_NUM);
  return o42_value_number ((cost - salvage) * (life - per + 1) * 2 / (life * (life + 1)));
}

static O42Value
fn_ddb (O42EvalContext *ctx, O42Operand *args, int n)
{
  double cost, salvage, life, per, factor = 2, book, dep = 0;

  ARG_NUMBER (0, cost); ARG_NUMBER (1, salvage); ARG_NUMBER (2, life); ARG_NUMBER (3, per);
  if (n >= 5) ARG_NUMBER (4, factor);
  if (life <= 0 || per < 1 || per > life || cost < 0) return o42_value_error (O42_ERR_NUM);

  book = cost;
  for (int i = 1; i <= (int) per; i++)
    {
      dep = book * factor / life;
      if (book - dep < salvage)
        dep = MAX (book - salvage, 0);
      book -= dep;
    }
  return o42_value_number (dep);
}

/* ---- Several criteria at once ------------------------------------------ */

/* SUMIFS, COUNTIFS, AVERAGEIFS, MAXIFS, MINIFS: the pairs of (range,
 * criteria) after the value range must all hold for a cell to count. */
typedef enum { IFS_SUM, IFS_COUNT, IFS_AVERAGE, IFS_MAX, IFS_MIN } IfsKind;

static O42Value
fn_ifs (O42EvalContext *ctx, O42Operand *args, int n, IfsKind kind)
{
  int first = (kind == IFS_COUNT) ? 0 : 1;
  int pairs = (n - first) / 2;
  const O42Range *shape;
  Criterion *crits;
  double total = 0, best = 0;
  int count = 0;
  int rows, cols;

  if ((n - first) % 2 != 0 || pairs < 1)
    return o42_value_error (O42_ERR_VALUE);
  if (kind != IFS_COUNT && !args[0].is_range)
    return o42_value_error (O42_ERR_VALUE);

  shape = &args[first].range;
  rows = shape->row1 - shape->row0 + 1;
  cols = shape->col1 - shape->col0 + 1;

  for (int i = 0; i < pairs; i++)
    {
      const O42Operand *r = &args[first + 2 * i];
      if (!r->is_range ||
          r->range.row1 - r->range.row0 + 1 != rows ||
          r->range.col1 - r->range.col0 + 1 != cols)
        return o42_value_error (O42_ERR_VALUE);
    }
  if (kind != IFS_COUNT &&
      (args[0].range.row1 - args[0].range.row0 + 1 != rows ||
       args[0].range.col1 - args[0].range.col0 + 1 != cols))
    return o42_value_error (O42_ERR_VALUE);

  crits = g_new0 (Criterion, (gsize) pairs);
  for (int i = 0; i < pairs; i++)
    {
      O42Value cv = operand_value (ctx, &args[first + 2 * i + 1]);
      criterion_init (&crits[i], &cv);
      o42_value_clear (&cv);
    }

  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      {
        gboolean all = TRUE;

        for (int i = 0; i < pairs && all; i++)
          {
            const O42Operand *range = &args[first + 2 * i];
            O42Value v;

            ctx->get_cell (ctx, range->sheet, range->range.row0 + r,
                           range->range.col0 + c, &v);
            all = criterion_match (&crits[i], &v);
            o42_value_clear (&v);
          }

        if (!all)
          continue;

        if (kind == IFS_COUNT)
          count++;
        else
          {
            O42Value v;

            ctx->get_cell (ctx, args[0].sheet, args[0].range.row0 + r,
                           args[0].range.col0 + c, &v);
            if (v.type == O42_VALUE_NUMBER)
              {
                if (count == 0 || (kind == IFS_MAX ? v.as.number > best : v.as.number < best))
                  best = v.as.number;
                total += v.as.number;
                count++;
              }
            o42_value_clear (&v);
          }
      }

  for (int i = 0; i < pairs; i++)
    criterion_clear (&crits[i]);
  g_free (crits);

  switch (kind)
    {
    case IFS_COUNT:   return o42_value_number (count);
    case IFS_SUM:     return o42_value_number (total);
    case IFS_AVERAGE: return (count == 0) ? o42_value_error (O42_ERR_DIV0)
                                          : o42_value_number (total / count);
    default:          return o42_value_number (count == 0 ? 0 : best);
    }
}

static O42Value fn_sumifs     (O42EvalContext *c, O42Operand *a, int n) { return fn_ifs (c, a, n, IFS_SUM); }
static O42Value fn_countifs   (O42EvalContext *c, O42Operand *a, int n) { return fn_ifs (c, a, n, IFS_COUNT); }
static O42Value fn_averageifs (O42EvalContext *c, O42Operand *a, int n) { return fn_ifs (c, a, n, IFS_AVERAGE); }
static O42Value fn_maxifs     (O42EvalContext *c, O42Operand *a, int n) { return fn_ifs (c, a, n, IFS_MAX); }
static O42Value fn_minifs     (O42EvalContext *c, O42Operand *a, int n) { return fn_ifs (c, a, n, IFS_MIN); }

/* ---- Database functions ----------------------------------------------- */

/* DSUM(database, field, criteria) and its family: the database is a table
 * with headings in its first row, the field a heading or a column number,
 * and the criteria another little table of headings over conditions --
 * conditions on one row must all hold, any row may match.  The oldest
 * query language a spreadsheet has, from Lotus. */
typedef enum { DB_SUM, DB_COUNT, DB_COUNTA, DB_AVERAGE, DB_MAX, DB_MIN, DB_GET,
               DB_PRODUCT, DB_STDEV, DB_VAR, DB_STDEVP, DB_VARP } DbKind;

static int
db_find_field (O42EvalContext *ctx, const O42Operand *db, const O42Value *field)
{
  int cols = db->range.col1 - db->range.col0 + 1;

  if (field->type == O42_VALUE_NUMBER)
    {
      int i = (int) field->as.number;
      return (i >= 1 && i <= cols) ? i - 1 : -1;
    }

  if (field->type != O42_VALUE_TEXT)
    return -1;

  for (int c = 0; c < cols; c++)
    {
      O42Value h;
      gboolean hit;

      ctx->get_cell (ctx, db->sheet, db->range.row0, db->range.col0 + c, &h);
      hit = (h.type == O42_VALUE_TEXT && g_ascii_strcasecmp (h.as.text, field->as.text) == 0);
      o42_value_clear (&h);
      if (hit)
        return c;
    }

  return -1;
}

static O42Value
fn_db (O42EvalContext *ctx, O42Operand *args, int n, DbKind kind)
{
  const O42Operand *db = &args[0], *crit = &args[2];
  O42Value field;
  int col, crit_cols, crit_rows;
  int *crit_field;
  Criterion *crits;
  GArray *values;
  O42Value result;
  int counta = 0;

  (void) n;

  if (!db->is_range || !crit->is_range)
    return o42_value_error (O42_ERR_VALUE);

  field = operand_value (ctx, &args[1]);
  col = db_find_field (ctx, db, &field);
  o42_value_clear (&field);
  if (col < 0 && kind != DB_COUNT)
    return o42_value_error (O42_ERR_VALUE);

  /* Each criteria column is a condition on a database column. */
  crit_cols = crit->range.col1 - crit->range.col0 + 1;
  crit_rows = crit->range.row1 - crit->range.row0;       /* conditions below the heading */
  crit_field = g_new (int, (gsize) crit_cols);
  crits = g_new0 (Criterion, (gsize) crit_cols * MAX (crit_rows, 0));

  for (int c = 0; c < crit_cols; c++)
    {
      O42Value h;
      ctx->get_cell (ctx, crit->sheet, crit->range.row0, crit->range.col0 + c, &h);
      crit_field[c] = db_find_field (ctx, db, &h);
      o42_value_clear (&h);

      for (int r = 0; r < crit_rows; r++)
        {
          O42Value cv;
          ctx->get_cell (ctx, crit->sheet, crit->range.row0 + 1 + r, crit->range.col0 + c, &cv);
          criterion_init (&crits[r * crit_cols + c], &cv);
          o42_value_clear (&cv);
        }
    }

  values = g_array_new (FALSE, FALSE, sizeof (double));

  for (int row = db->range.row0 + 1; row <= db->range.row1; row++)
    {
      gboolean any_row = (crit_rows == 0);

      for (int r = 0; r < crit_rows && !any_row; r++)
        {
          gboolean all = TRUE;

          for (int c = 0; c < crit_cols && all; c++)
            {
              O42Value v;

              if (crit_field[c] < 0 || crits[r * crit_cols + c].value.type == O42_VALUE_EMPTY)
                continue;      /* an empty condition is no condition */

              ctx->get_cell (ctx, db->sheet, row, db->range.col0 + crit_field[c], &v);
              all = criterion_match (&crits[r * crit_cols + c], &v);
              o42_value_clear (&v);
            }
          any_row = all;
        }

      if (!any_row)
        continue;

      if (col >= 0)
        {
          O42Value v;

          ctx->get_cell (ctx, db->sheet, row, db->range.col0 + col, &v);
          if (v.type == O42_VALUE_NUMBER)
            g_array_append_val (values, v.as.number);
          if (v.type != O42_VALUE_EMPTY)
            counta++;
          o42_value_clear (&v);
        }
      else
        counta++;
    }

  for (int i = 0; i < crit_cols * crit_rows; i++)
    criterion_clear (&crits[i]);
  g_free (crits);
  g_free (crit_field);

  {
    guint count = values->len;
    double sum = 0, product = 1, best = 0, mean, ssd;

    for (guint i = 0; i < count; i++)
      {
        double v = g_array_index (values, double, i);
        sum += v;
        product *= v;
        if (i == 0 || (kind == DB_MAX ? v > best : v < best))
          best = v;
      }

    switch (kind)
      {
      case DB_SUM:     result = o42_value_number (sum); break;
      case DB_PRODUCT: result = o42_value_number (count ? product : 0); break;
      case DB_COUNT:   result = o42_value_number (count); break;
      case DB_COUNTA:  result = o42_value_number (counta); break;
      case DB_MAX:
      case DB_MIN:     result = o42_value_number (count ? best : 0); break;
      case DB_AVERAGE: result = count ? o42_value_number (sum / count)
                                      : o42_value_error (O42_ERR_DIV0); break;
      case DB_GET:
        result = (count == 1) ? o42_value_number (g_array_index (values, double, 0))
               : (count == 0) ? o42_value_error (O42_ERR_VALUE)
                              : o42_value_error (O42_ERR_NUM);
        break;
      case DB_STDEVP:
      case DB_VARP:
        if (count < 1)
          result = o42_value_error (O42_ERR_DIV0);
        else
          {
            moments (values, &mean, &ssd);
            result = o42_value_number (kind == DB_VARP ? ssd / count : sqrt (ssd / count));
          }
        break;
      case DB_STDEV:
      case DB_VAR:
      default:
        if (count < 2)
          result = o42_value_error (O42_ERR_DIV0);
        else
          {
            moments (values, &mean, &ssd);
            result = o42_value_number (kind == DB_VAR ? ssd / (count - 1)
                                                      : sqrt (ssd / (count - 1)));
          }
        break;
      }
  }

  g_array_free (values, TRUE);
  return result;
}

static O42Value fn_dsum     (O42EvalContext *c, O42Operand *a, int n) { return fn_db (c, a, n, DB_SUM); }
static O42Value fn_dcount   (O42EvalContext *c, O42Operand *a, int n) { return fn_db (c, a, n, DB_COUNT); }
static O42Value fn_dcounta  (O42EvalContext *c, O42Operand *a, int n) { return fn_db (c, a, n, DB_COUNTA); }
static O42Value fn_daverage (O42EvalContext *c, O42Operand *a, int n) { return fn_db (c, a, n, DB_AVERAGE); }
static O42Value fn_dmax     (O42EvalContext *c, O42Operand *a, int n) { return fn_db (c, a, n, DB_MAX); }
static O42Value fn_dmin     (O42EvalContext *c, O42Operand *a, int n) { return fn_db (c, a, n, DB_MIN); }
static O42Value fn_dget     (O42EvalContext *c, O42Operand *a, int n) { return fn_db (c, a, n, DB_GET); }
static O42Value fn_dproduct (O42EvalContext *c, O42Operand *a, int n) { return fn_db (c, a, n, DB_PRODUCT); }
static O42Value fn_dstdev   (O42EvalContext *c, O42Operand *a, int n) { return fn_db (c, a, n, DB_STDEV); }
static O42Value fn_dvar     (O42EvalContext *c, O42Operand *a, int n) { return fn_db (c, a, n, DB_VAR); }
static O42Value fn_dstdevp  (O42EvalContext *c, O42Operand *a, int n) { return fn_db (c, a, n, DB_STDEVP); }
static O42Value fn_dvarp    (O42EvalContext *c, O42Operand *a, int n) { return fn_db (c, a, n, DB_VARP); }

/* ---- Text and dates that Excel added later ---------------------------- */

/* The weekend a WORKDAY.INTL or NETWORKDAYS.INTL is told to keep: a
 * number from Excel's table, or a string of seven ones and noughts
 * starting on Monday. */
static gboolean
weekend_days (O42EvalContext *ctx, const O42Operand *operand, gboolean weekend[8])
{
  O42Value value;
  int which = 1;

  for (int i = 0; i < 8; i++)
    weekend[i] = FALSE;
  weekend[6] = weekend[7] = TRUE;      /* Saturday and Sunday by default */
  if (operand == NULL)
    return TRUE;

  value = operand_value (ctx, operand);
  if (value.type == O42_VALUE_TEXT)
    {
      const char *pattern = value.as.text;
      gboolean ok = strlen (pattern) == 7;

      for (int i = 0; ok && i < 7; i++)
        {
          if (pattern[i] != '0' && pattern[i] != '1')
            ok = FALSE;
          else
            weekend[i + 1] = pattern[i] == '1';
        }
      o42_value_clear (&value);
      /* A week with no working day in it has no next working day, and
       * looking for one would never stop. */
      if (ok && weekend[1] && weekend[2] && weekend[3] && weekend[4] &&
          weekend[5] && weekend[6] && weekend[7])
        ok = FALSE;
      return ok;
    }
  {
    double number = 0;
    O42ErrorCode e = O42_ERR_VALUE;
    gboolean ok = o42_value_to_number (&value, &number, &e);

    o42_value_clear (&value);
    if (!ok)
      return FALSE;
    which = (int) number;
  }

  for (int i = 1; i <= 7; i++)
    weekend[i] = FALSE;
  /* 1 to 7 are the two-day weekends from Saturday and Sunday round;
   * 11 to 17 are the one-day weekends from Sunday round. */
  if (which >= 1 && which <= 7)
    {
      int first = ((which + 4) % 7) + 1;   /* 1 -> Saturday */
      int second = (first % 7) + 1;

      weekend[first] = weekend[second] = TRUE;
      return TRUE;
    }
  if (which >= 11 && which <= 17)
    {
      int day = ((which - 11 + 6) % 7) + 1;   /* 11 -> Sunday */

      weekend[day] = TRUE;
      return TRUE;
    }
  return FALSE;
}

static O42Value
fn_networkdays_intl (O42EvalContext *ctx, O42Operand *args, int n)
{
  double start, end;
  gboolean weekend[8];
  int count = 0, sign = 1;

  ARG_NUMBER (0, start);
  ARG_NUMBER (1, end);
  if (!weekend_days (ctx, n >= 3 ? &args[2] : NULL, weekend))
    return o42_value_error (O42_ERR_NUM);
  start = floor (start);
  end = floor (end);
  if (end < start) { double t = start; start = end; end = t; sign = -1; }

  for (double d = start; d <= end; d += 1)
    if (!weekend[o42_date_weekday (d)] && !is_holiday (ctx, n >= 4 ? &args[3] : NULL, d))
      count++;
  return o42_value_number (sign * count);
}

static O42Value
fn_workday_intl (O42EvalContext *ctx, O42Operand *args, int n)
{
  double start, days, d;
  gboolean weekend[8];
  int step;

  ARG_NUMBER (0, start);
  ARG_NUMBER (1, days);
  if (!weekend_days (ctx, n >= 3 ? &args[2] : NULL, weekend))
    return o42_value_error (O42_ERR_NUM);

  d = floor (start);
  step = (days < 0) ? -1 : 1;
  days = fabs (trunc (days));
  while (days > 0)
    {
      d += step;
      if (!weekend[o42_date_weekday (d)] && !is_holiday (ctx, n >= 4 ? &args[3] : NULL, d))
        days--;
    }
  return o42_value_number (d);
}

/* FORMULATEXT: what was typed into a cell, formula and all, which only
 * the caller can say. */
static O42Value
fn_formulatext (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value out;

  (void) n;
  if (!args[0].is_range)
    return o42_value_error (O42_ERR_NA);
  if (ctx->get_cell_info != NULL &&
      ctx->get_cell_info (ctx, args[0].sheet, args[0].range.row0, args[0].range.col0,
                          "formulatext", &out))
    {
      if (out.type == O42_VALUE_TEXT && out.as.text[0] != '\0')
        return out;
      o42_value_clear (&out);
    }
  return o42_value_error (O42_ERR_NA);
}

/* TEXTBEFORE and TEXTAFTER: the part of a text on one side of the
 * delimiter, counting from the start or, for a negative instance, from
 * the end. */
static O42Value
text_around (O42EvalContext *ctx, O42Operand *args, int n, gboolean before)
{
  char *text = NULL, *needle = NULL;
  double instance = 1;
  const char *found = NULL;
  char *answer;

  ARG_TEXT (0, text);
  ARG_TEXT (1, needle);
  if (n >= 3)
    {
      O42Value v = operand_value (ctx, &args[2]);
      O42ErrorCode e = O42_ERR_VALUE;
      gboolean ok = o42_value_to_number (&v, &instance, &e);

      o42_value_clear (&v);
      if (!ok)
        { g_free (text); g_free (needle); return o42_value_error (e); }
    }
  if (*needle == '\0' || instance == 0)
    { g_free (text); g_free (needle); return o42_value_error (O42_ERR_VALUE); }

  if (instance > 0)
    {
      const char *p = text;

      for (int i = 0; i < (int) instance; i++)
        {
          p = strstr (found == NULL ? p : found + strlen (needle), needle);
          if (p == NULL)
            break;
          found = p;
        }
    }
  else
    {
      /* From the end: walk every match and keep the one asked for. */
      GPtrArray *hits = g_ptr_array_new ();
      const char *p = text;

      while ((p = strstr (p, needle)) != NULL)
        {
          g_ptr_array_add (hits, (gpointer) p);
          p += strlen (needle);
        }
      {
        int which = (int) hits->len + (int) instance;

        if (which >= 0 && which < (int) hits->len)
          found = g_ptr_array_index (hits, which);
      }
      g_ptr_array_free (hits, TRUE);
    }

  if (found == NULL)
    { g_free (text); g_free (needle); return o42_value_error (O42_ERR_NA); }

  if (before)
    answer = g_strndup (text, (gsize) (found - text));
  else
    answer = g_strdup (found + strlen (needle));
  g_free (text);
  g_free (needle);
  return o42_value_take (answer);
}

static O42Value fn_textbefore (O42EvalContext *c, O42Operand *a, int n) { return text_around (c, a, n, TRUE); }
static O42Value fn_textafter  (O42EvalContext *c, O42Operand *a, int n) { return text_around (c, a, n, FALSE); }

/* VALUETOTEXT and ARRAYTOTEXT: a value as text, plainly or with the
 * quotes and braces that say what it was. */
static char *
value_as_text (const O42Value *value, gboolean strict)
{
  switch (value->type)
    {
    case O42_VALUE_TEXT:
      return strict ? g_strdup_printf ("\"%s\"", value->as.text) : g_strdup (value->as.text);
    case O42_VALUE_BOOL:
      return g_strdup (value->as.boolean ? "TRUE" : "FALSE");
    case O42_VALUE_ERROR:
      return g_strdup (o42_error_name (value->as.error));
    case O42_VALUE_EMPTY:
      return g_strdup ("");
    default:
      return o42_value_display (value);
    }
}

static O42Value
fn_valuetotext (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value value = operand_value (ctx, &args[0]);
  double format = 0;
  char *text;

  if (n >= 2)
    {
      O42Value v = operand_value (ctx, &args[1]);
      O42ErrorCode e = O42_ERR_VALUE;

      o42_value_to_number (&v, &format, &e);
      o42_value_clear (&v);
    }
  text = value_as_text (&value, format != 0);
  o42_value_clear (&value);
  return o42_value_take (text);
}

static O42Value
fn_arraytotext (O42EvalContext *ctx, O42Operand *args, int n)
{
  double format = 0;
  GString *out;
  int rows = 0, cols = 0;

  if (n >= 2)
    {
      O42Value v = operand_value (ctx, &args[1]);
      O42ErrorCode e = O42_ERR_VALUE;

      o42_value_to_number (&v, &format, &e);
      o42_value_clear (&v);
    }

  if (!args[0].is_range)
    {
      O42Value value = operand_value (ctx, &args[0]);
      char *text = value_as_text (&value, format != 0);

      o42_value_clear (&value);
      if (format != 0)
        {
          char *braced = g_strdup_printf ("{%s}", text);

          g_free (text);
          text = braced;
        }
      return o42_value_take (text);
    }

  rows = args[0].range.row1 - args[0].range.row0 + 1;
  cols = args[0].range.col1 - args[0].range.col0 + 1;
  out = g_string_new (format != 0 ? "{" : "");
  for (int r = 0; r < rows; r++)
    {
      if (r > 0)
        g_string_append (out, format != 0 ? ";" : ", ");
      for (int c = 0; c < cols; c++)
        {
          O42Value value;
          char *text;

          ctx->get_cell (ctx, args[0].sheet, args[0].range.row0 + r,
                         args[0].range.col0 + c, &value);
          text = value_as_text (&value, format != 0);
          o42_value_clear (&value);
          if (c > 0)
            g_string_append (out, format != 0 ? "," : ", ");
          g_string_append (out, text);
          g_free (text);
        }
    }
  if (format != 0)
    g_string_append_c (out, '}');
  return o42_value_take (g_string_free (out, FALSE));
}

/* GETENV: what the machine's environment says, which Gnumeric offers
 * and nothing else does. */
static O42Value
fn_getenv (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *name = NULL;
  const char *value;

  (void) n;
  ARG_TEXT (0, name);
  value = g_getenv (name);
  g_free (name);
  return value != NULL ? o42_value_text (value) : o42_value_error (O42_ERR_NA);
}

/* ---- Two tests for normality, and the Fourier transform -------------- */

/* ---- Two tests for normality, and the Fourier transform -------------- */

/* Anderson and Darling's test: how far a sample is from the normal
 * distribution its own mean and deviation describe.  The answer is the
 * probability of seeing a sample at least this far off. */
static O42Value
fn_adtest (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double mean = 0, sd = 0, a2 = 0, adjusted, p;
  guint count;

  if (!collect_numbers (ctx, args, n, &values, &err))
    return o42_value_error (err);
  count = values->len;
  if (count < 8)
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NUM); }

  g_array_sort (values, compare_doubles);
  for (guint i = 0; i < count; i++)
    mean += g_array_index (values, double, i);
  mean /= count;
  for (guint i = 0; i < count; i++)
    {
      double d = g_array_index (values, double, i) - mean;

      sd += d * d;
    }
  sd = sqrt (sd / (count - 1));
  if (sd <= 0)
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_DIV0); }

  for (guint i = 0; i < count; i++)
    {
      double low = normal_cdf ((g_array_index (values, double, i) - mean) / sd);
      double high = normal_cdf ((g_array_index (values, double, count - 1 - i) - mean) / sd);

      low = CLAMP (low, 1e-15, 1 - 1e-15);
      high = CLAMP (high, 1e-15, 1 - 1e-15);
      a2 += (2.0 * (i + 1) - 1) * (log (low) + log (1 - high));
    }
  a2 = -(double) count - a2 / count;
  g_array_free (values, TRUE);

  /* The statistic corrected for the size of the sample, and the
   * probability that goes with it -- Stephens's approximation. */
  adjusted = a2 * (1 + 0.75 / count + 2.25 / ((double) count * count));
  if (adjusted < 0.2)
    p = 1 - exp (-13.436 + 101.14 * adjusted - 223.73 * adjusted * adjusted);
  else if (adjusted < 0.34)
    p = 1 - exp (-8.318 + 42.796 * adjusted - 59.938 * adjusted * adjusted);
  else if (adjusted < 0.6)
    p = exp (0.9177 - 4.279 * adjusted - 1.38 * adjusted * adjusted);
  else
    p = exp (1.2937 - 5.709 * adjusted + 0.0186 * adjusted * adjusted);
  return o42_value_number (CLAMP (p, 0, 1));
}

/* D'Agostino and Pearson's test: the skew and the kurtosis of a sample
 * turned into a chi-squared with two degrees of freedom. */
static O42Value
fn_normaltest (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double mean = 0, m2 = 0, m3 = 0, m4 = 0;
  double skew, kurtosis, zs, zk, k2;
  double count;

  if (!collect_numbers (ctx, args, n, &values, &err))
    return o42_value_error (err);
  count = values->len;
  if (count < 8)
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NUM); }

  for (guint i = 0; i < values->len; i++)
    mean += g_array_index (values, double, i);
  mean /= count;
  for (guint i = 0; i < values->len; i++)
    {
      double d = g_array_index (values, double, i) - mean;

      m2 += d * d;
      m3 += d * d * d;
      m4 += d * d * d * d;
    }
  g_array_free (values, TRUE);
  m2 /= count;
  m3 /= count;
  m4 /= count;
  if (m2 <= 0)
    return o42_value_error (O42_ERR_DIV0);
  skew = m3 / pow (m2, 1.5);
  kurtosis = m4 / (m2 * m2);

  /* The skew, made normal by Anscombe and Glynn's transformation. */
  {
    double y = skew * sqrt ((count + 1) * (count + 3) / (6 * (count - 2)));
    double beta2 = 3 * (count * count + 27 * count - 70) * (count + 1) * (count + 3) /
                   ((count - 2) * (count + 5) * (count + 7) * (count + 9));
    double w2 = -1 + sqrt (2 * (beta2 - 1));
    double delta = 1 / sqrt (log (sqrt (w2)));
    double alpha = sqrt (2 / (w2 - 1));

    zs = delta * log (y / alpha + sqrt ((y / alpha) * (y / alpha) + 1));
  }
  /* And the kurtosis. */
  {
    double e = 3 * (count - 1) / (count + 1);
    double variance = 24 * count * (count - 2) * (count - 3) /
                      ((count + 1) * (count + 1) * (count + 3) * (count + 5));
    double x = (kurtosis - e) / sqrt (variance);
    double beta1 = 6 * (count * count - 5 * count + 2) / ((count + 7) * (count + 9)) *
                   sqrt (6 * (count + 3) * (count + 5) / (count * (count - 2) * (count - 3)));
    double a = 6 + 8 / beta1 * (2 / beta1 + sqrt (1 + 4 / (beta1 * beta1)));
    double top = 1 - 2 / (9 * a);
    double bottom = 1 + x * sqrt (2 / (a - 4));

    zk = (top - pow ((1 - 2 / a) / bottom, 1.0 / 3.0)) / sqrt (2 / (9 * a));
  }
  k2 = zs * zs + zk * zk;
  return o42_value_number (1 - chi_cdf (k2, 2, 0));
}

/* ---- Gnumeric's number theory ------------------------------------------ */

/* All of these want a whole number that is not too big to take apart by
 * trial division; a million million is where the patience runs out. */
#define NT_LIMIT 1e12

static gboolean
nt_argument (double value, gint64 *out)
{
  if (!(value >= 1) || value > NT_LIMIT || value != floor (value))
    return FALSE;
  *out = (gint64) value;
  return TRUE;
}

static gboolean
nt_is_prime (gint64 n)
{
  if (n < 2)
    return FALSE;
  if (n % 2 == 0)
    return n == 2;
  for (gint64 d = 3; d * d <= n; d += 2)
    if (n % d == 0)
      return FALSE;
  return TRUE;
}

static O42Value
fn_isprime (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  gint64 k;

  (void) n;
  ARG_NUMBER (0, x);
  if (!nt_argument (x, &k))
    return o42_value_error (O42_ERR_VALUE);
  return o42_value_bool (nt_is_prime (k));
}

static O42Value
fn_ithprime (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  gint64 want, found = 0, at = 1;

  (void) n;
  ARG_NUMBER (0, x);
  if (!nt_argument (x, &want) || want > 1000000)
    return o42_value_error (O42_ERR_VALUE);
  while (found < want)
    {
      at++;
      if (nt_is_prime (at))
        found++;
    }
  return o42_value_number ((double) at);
}

static O42Value
fn_pfactor (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  gint64 k;

  (void) n;
  ARG_NUMBER (0, x);
  if (!nt_argument (x, &k) || k < 2)
    return o42_value_error (O42_ERR_VALUE);
  if (k % 2 == 0)
    return o42_value_number (2);
  for (gint64 d = 3; d * d <= k; d += 2)
    if (k % d == 0)
      return o42_value_number ((double) d);
  return o42_value_number ((double) k);   /* it is its own smallest factor */
}

/* n taken apart: each distinct prime and how many times it goes in. */
static int
nt_factor (gint64 n, gint64 *primes, int *powers)
{
  int count = 0;

  for (gint64 d = 2; d * d <= n; d += (d == 2 ? 1 : 2))
    if (n % d == 0)
      {
        primes[count] = d;
        powers[count] = 0;
        while (n % d == 0)
          { n /= d; powers[count]++; }
        count++;
      }
  if (n > 1)
    { primes[count] = n; powers[count] = 1; count++; }
  return count;
}

/* The seven that are answered from the factorisation. */
static O42Value
nt_answer (double x, char what)
{
  gint64 k, primes[64];
  int powers[64], count;
  double answer = 1;

  if (!nt_argument (x, &k))
    return o42_value_error (O42_ERR_VALUE);
  if (what == 'p' && k >= 1)
    { /* NT_PI counts the primes up to n, which is not a factorisation */
      gint64 found = 0;

      if (k > 10000000)
        return o42_value_error (O42_ERR_NUM);
      for (gint64 i = 2; i <= k; i++)
        if (nt_is_prime (i))
          found++;
      return o42_value_number ((double) found);
    }
  count = nt_factor (k, primes, powers);

  switch (what)
    {
    case 'd':   /* NT_D: how many divisors */
      for (int i = 0; i < count; i++)
        answer *= powers[i] + 1;
      break;
    case 's':   /* NT_SIGMA: what they add up to */
      for (int i = 0; i < count; i++)
        {
          double term = 1, power = 1;

          for (int j = 0; j < powers[i]; j++)
            { power *= (double) primes[i]; term += power; }
          answer *= term;
        }
      break;
    case 'f':   /* NT_PHI: how many below it are coprime to it */
      for (int i = 0; i < count; i++)
        {
          double power = 1;

          for (int j = 0; j + 1 < powers[i]; j++)
            power *= (double) primes[i];
          answer *= power * (primes[i] - 1);
        }
      break;
    case 'm':   /* NT_MU: Moebius */
      for (int i = 0; i < count; i++)
        if (powers[i] > 1)
          return o42_value_number (0);
      answer = (count % 2 == 0) ? 1 : -1;
      break;
    case 'o':   /* NT_OMEGA: how many distinct primes */
      answer = count;
      break;
    case 'r':   /* NT_RADICAL: their product */
      for (int i = 0; i < count; i++)
        answer *= (double) primes[i];
      break;
    default:
      return o42_value_error (O42_ERR_VALUE);
    }
  return o42_value_number (answer);
}

#define NT_FUNCTION(name, letter)                                          \
static O42Value                                                            \
name (O42EvalContext *ctx, O42Operand *args, int n)                        \
{                                                                          \
  double x;                                                                \
                                                                           \
  (void) n;                                                                \
  ARG_NUMBER (0, x);                                                       \
  return nt_answer (x, letter);                                            \
}

NT_FUNCTION (fn_nt_d, 'd')
NT_FUNCTION (fn_nt_sigma, 's')
NT_FUNCTION (fn_nt_phi, 'f')
NT_FUNCTION (fn_nt_mu, 'm')
NT_FUNCTION (fn_nt_omega, 'o')
NT_FUNCTION (fn_nt_radical, 'r')
NT_FUNCTION (fn_nt_pi, 'p')

/* ---- Two more of Gnumeric's distributions ------------------------------ */

/* Cauchy's -- the physicists' Lorentz, or Breit and Wigner -- which has
 * no mean and no variance, only a scale. */
static O42Value
fn_cauchy (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, a;
  gboolean cumulative = FALSE;

  ARG_NUMBER (0, x);
  ARG_NUMBER (1, a);
  if (n >= 3)
    {
      O42Value v = operand_value (ctx, &args[2]);
      O42ErrorCode err = O42_ERR_VALUE;
      gboolean ok = o42_value_to_bool (&v, &cumulative, &err);

      o42_value_clear (&v);
      if (!ok) return o42_value_error (err);
    }
  if (a <= 0)
    return o42_value_error (O42_ERR_NUM);
  if (cumulative)
    return o42_value_number (0.5 + atan (x / a) / G_PI);
  return o42_value_number (1 / (G_PI * a * (1 + (x / a) * (x / a))));
}

/* Landau's, the energy a particle loses crossing thin matter.  It has
 * no closed form; what there is is the integral
 *
 *     p(x) = (1/pi) INT[0, inf] e^(-t ln t - x t) sin(pi t) dt
 *
 * which is what this works out.  The e^(-t ln t) kills the integrand
 * fast -- by t = 20 it is e^(-60) -- and the sine turns over every two,
 * so Simpson over a fine grid to t = 30 is plenty. */
static O42Value
fn_landau (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, total = 0;
  const double top = 30.0;
  const int steps = 60000;   /* an even number: Simpson goes in pairs */
  const double h = top / steps;

  (void) n;
  ARG_NUMBER (0, x);

  for (int i = 0; i <= steps; i++)
    {
      double t = i * h;
      double weight = (i == 0 || i == steps) ? 1 : (i % 2 ? 4 : 2);
      double f;

      if (t == 0)
        f = 0;   /* sin(pi t) is zero there, and t ln t with it */
      else
        {
          double e = -t * log (t) - x * t;

          f = e < -700 ? 0 : exp (e) * sin (G_PI * t);
        }
      total += weight * f;
    }
  total *= h / 3;
  return o42_value_number (total / G_PI);
}

/* ---- Gnumeric's dates -------------------------------------------------- */

/* The Julian day number of a serial date.  Excel's serial 1 is the 1st
 * of January 1900, whose Julian day number is 2415021, but the calendar
 * has that phantom 29th of February 1900 in it, so everything from the
 * 1st of March on is one day out and has to be pulled back. */
static O42Value
fn_date2julian (O42EvalContext *ctx, O42Operand *args, int n)
{
  double serial;

  (void) n;
  ARG_NUMBER (0, serial);
  serial = floor (serial);
  if (serial < 1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (serial + (serial >= 60 ? 2415019 : 2415020));
}

/* Unix time is seconds from the start of 1970, which is serial 25569. */
static O42Value
fn_date2unix (O42EvalContext *ctx, O42Operand *args, int n)
{
  double serial;

  (void) n;
  ARG_NUMBER (0, serial);
  return o42_value_number ((serial - 25569) * 86400);
}

static O42Value
fn_unix2date (O42EvalContext *ctx, O42Operand *args, int n)
{
  double seconds;

  (void) n;
  ARG_NUMBER (0, seconds);
  return o42_value_number (25569 + seconds / 86400);
}

/* The year an ISO week belongs to, which is the year of its Thursday --
 * so the 1st of January can fall in the year before. */
static O42Value
fn_isoyear (O42EvalContext *ctx, O42Operand *args, int n)
{
  double serial, thursday;
  int weekday, y, m, d;

  (void) n;
  ARG_NUMBER (0, serial);
  serial = floor (serial);
  weekday = o42_date_weekday (serial);
  thursday = serial + (4 - weekday);
  if (!o42_date_from_serial (thursday, &y, &m, &d))
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (y);
}

/* Easter, by the anonymous Gregorian computus as Meeus writes it, and
 * the four feasts that are counted from it. */
static O42Value
easter_offset (O42EvalContext *ctx, O42Operand *args, int n, int days)
{
  double year_arg;
  int year, a, b, c, d, e, f, g, h, i, k, l, month, day;

  (void) n;
  ARG_NUMBER (0, year_arg);
  year = (int) year_arg;
  if (year < 1900 || year > 9999)
    return o42_value_error (O42_ERR_NUM);

  a = year % 19;
  b = year / 100;
  c = year % 100;
  d = b / 4;
  e = b % 4;
  f = (b + 8) / 25;
  g = (b - f + 1) / 3;
  h = (19 * a + b - d - g + 15) % 30;
  i = c / 4;
  k = c % 4;
  l = (32 + 2 * e + 2 * i - h - k) % 7;
  {
    int m = (a + 11 * h + 22 * l) / 451;

    month = (h + l - 7 * m + 114) / 31;
    day = ((h + l - 7 * m + 114) % 31) + 1;
  }
  return o42_value_number (o42_date_serial (year, month, day) + days);
}

#define EASTER_FUNCTION(name, days)                                        \
static O42Value                                                            \
name (O42EvalContext *ctx, O42Operand *args, int n)                        \
{                                                                          \
  return easter_offset (ctx, args, n, days);                               \
}

EASTER_FUNCTION (fn_eastersunday, 0)
EASTER_FUNCTION (fn_goodfriday, -2)
EASTER_FUNCTION (fn_ashwednesday, -46)
EASTER_FUNCTION (fn_ascensionthursday, 39)
EASTER_FUNCTION (fn_pentecostsunday, 49)

/* ---- More of Gnumeric's statistics ------------------------------------ */

/* ---- More of Gnumeric's statistics ------------------------------------ */

/* GEOMDIST(k, p, cumulative): how many failures before the first
 * success, counting from nothing. */
static O42Value
fn_geomdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double k, p;
  gboolean cumulative = FALSE;

  ARG_NUMBER (0, k);
  ARG_NUMBER (1, p);
  if (n >= 3)
    {
      O42Value v = operand_value (ctx, &args[2]);
      O42ErrorCode e = O42_ERR_VALUE;
      gboolean ok = o42_value_to_bool (&v, &cumulative, &e);

      o42_value_clear (&v);
      if (!ok)
        return o42_value_error (e);
    }
  if (p <= 0 || p > 1 || k < 0)
    return o42_value_error (O42_ERR_NUM);
  k = floor (k);
  if (cumulative)
    return o42_value_number (1 - pow (1 - p, k + 1));
  return o42_value_number (p * pow (1 - p, k));
}

/* EXPPOWDIST(x, a, b): the density of the exponential power
 * distribution, which is the normal at b = 2 and Laplace's at b = 1. */
static O42Value
fn_exppowdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, a, b;

  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, a);
  ARG_NUMBER (2, b);
  if (a <= 0 || b <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (exp (-pow (fabs (x / a), b)) /
                           (2 * a * exp (lgamma (1 + 1 / b))));
}

/* SNORM.DIST.RANGE(x1, x2): the chance that a standard normal falls
 * between the two. */
static O42Value
fn_snorm_dist_range (O42EvalContext *ctx, O42Operand *args, int n)
{
  double lower, upper;

  (void) n;
  ARG_NUMBER (0, lower);
  ARG_NUMBER (1, upper);
  if (upper < lower)
    { double t = lower; lower = upper; upper = t; }
  return o42_value_number (normal_cdf (upper) - normal_cdf (lower));
}

/* OWENT(h, a): Owen's T, the volume under a bivariate normal over a
 * wedge, by Simpson's rule over the integral that defines it. */
static O42Value
fn_owent (O42EvalContext *ctx, O42Operand *args, int n)
{
  double h, a, total = 0, step;
  const int steps = 2000;   /* even, as Simpson's rule wants */

  (void) n;
  ARG_NUMBER (0, h);
  ARG_NUMBER (1, a);
  if (a == 0)
    return o42_value_number (0);
  step = a / steps;
  for (int i = 0; i <= steps; i++)
    {
      double x = i * step;
      double f = exp (-h * h * (1 + x * x) / 2) / (1 + x * x);
      double weight = (i == 0 || i == steps) ? 1 : (i % 2 == 1 ? 4 : 2);

      total += weight * f;
    }
  return o42_value_number (total * step / 3 / (2 * G_PI));
}

/* SUMA: the sum, counting text as nothing and TRUE as one, which is
 * what tells it from SUM. */
static O42Value
fn_suma (O42EvalContext *ctx, O42Operand *args, int n)
{
  double total = 0;

  for (int i = 0; i < n; i++)
    {
      int rows = 0, cols = 0;

      if (!args[i].is_range)
        {
          O42Value v = operand_value (ctx, &args[i]);
          double number = 0;
          O42ErrorCode e = O42_ERR_VALUE;

          if (v.type == O42_VALUE_ERROR)
            { O42Value copy = v; return copy; }
          if (v.type == O42_VALUE_BOOL)
            number = v.as.boolean ? 1 : 0;
          else if (v.type != O42_VALUE_TEXT && o42_value_to_number (&v, &number, &e))
            ;
          else
            number = 0;
          total += number;
          o42_value_clear (&v);
          continue;
        }

      rows = args[i].range.row1 - args[i].range.row0 + 1;
      cols = args[i].range.col1 - args[i].range.col0 + 1;
      for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
          {
            O42Value v;
            double number = 0;
            O42ErrorCode e = O42_ERR_VALUE;

            ctx->get_cell (ctx, args[i].sheet, args[i].range.row0 + r,
                           args[i].range.col0 + c, &v);
            if (v.type == O42_VALUE_ERROR)
              { O42Value copy = v; return copy; }
            if (v.type == O42_VALUE_BOOL)
              number = v.as.boolean ? 1 : 0;
            else if (v.type == O42_VALUE_NUMBER)
              number = v.as.number;
            else if (v.type != O42_VALUE_TEXT && v.type != O42_VALUE_EMPTY &&
                     o42_value_to_number (&v, &number, &e))
              ;
            else
              number = 0;
            total += number;
            o42_value_clear (&v);
          }
    }
  return o42_value_number (total);
}

/* CHISQDIST and CHISQINV: Gnumeric's names for the chi-squared
 * distribution measured from the left, and for its inverse. */
static O42Value
fn_chisqdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, df;
  gboolean cumulative = TRUE;

  ARG_NUMBER (0, x);
  ARG_NUMBER (1, df);
  if (n >= 3)
    {
      O42Value v = operand_value (ctx, &args[2]);
      O42ErrorCode e = O42_ERR_VALUE;
      gboolean ok = o42_value_to_bool (&v, &cumulative, &e);

      o42_value_clear (&v);
      if (!ok)
        return o42_value_error (e);
    }
  if (x < 0 || df < 1)
    return o42_value_error (O42_ERR_NUM);
  df = floor (df);
  if (cumulative)
    return o42_value_number (chi_cdf (x, df, 0));
  if (x == 0)
    return o42_value_number (df == 2 ? 0.5 : df < 2 ? HUGE_VAL : 0);
  return o42_value_number (exp ((df / 2 - 1) * log (x) - x / 2 -
                                (df / 2) * log (2) - lgamma (df / 2)));
}

static O42Value
fn_chisqinv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p, df, low = 0, high = 1e6;

  (void) n;
  ARG_NUMBER (0, p);
  ARG_NUMBER (1, df);
  if (p < 0 || p > 1 || df < 1)
    return o42_value_error (O42_ERR_NUM);
  df = floor (df);
  for (int i = 0; i < 200; i++)
    {
      double mid = (low + high) / 2;

      if (chi_cdf (mid, df, 0) < p)
        low = mid;
      else
        high = mid;
    }
  return o42_value_number ((low + high) / 2);
}

/* PERCENTRANK.EXC: the rank of a value among the others, counting the
 * ends out rather than in. */
static O42Value
fn_percentrank_exc (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double x, sig = 3, rank = 0, scale;
  guint count;

  ARG_NUMBER (1, x);
  if (n >= 3)
    ARG_NUMBER (2, sig);
  if (sig < 1)
    return o42_value_error (O42_ERR_NUM);
  if (!collect_numbers (ctx, args, 1, &values, &err))
    return o42_value_error (err);
  count = values->len;
  g_array_sort (values, compare_doubles);
  if (count == 0 || x < g_array_index (values, double, 0) ||
      x > g_array_index (values, double, count - 1))
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NA); }

  for (guint i = 0; i < count; i++)
    {
      double lo = g_array_index (values, double, i);

      if (x == lo)
        { rank = (i + 1.0) / (count + 1.0); break; }
      if (i + 1 < count)
        {
          double hi = g_array_index (values, double, i + 1);

          if (x > lo && x < hi)
            { rank = (i + 1 + (x - lo) / (hi - lo)) / (count + 1.0); break; }
        }
    }
  g_array_free (values, TRUE);
  if (rank <= 0 || rank >= 1)
    return o42_value_error (O42_ERR_NA);
  scale = pow (10, floor (sig));
  return o42_value_number (floor (rank * scale + 1e-9) / scale);
}

/* ---- Odds and ends ----------------------------------------------------- */

static O42Value
fn_xor (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v = fn_and_or (ctx, args, n, TRUE);   /* for the error handling */
  int trues = 0;

  if (v.type == O42_VALUE_ERROR)
    return v;
  o42_value_clear (&v);

  for (int i = 0; i < n; i++)
    {
      if (args[i].is_range)
        {
          const O42Range *r = &args[i].range;
          for (int row = r->row0; row <= r->row1; row++)
            for (int col = r->col0; col <= r->col1; col++)
              {
                O42Value w;
                gboolean b = FALSE;
                O42ErrorCode e = O42_ERR_VALUE;
                ctx->get_cell (ctx, args[i].sheet, row, col, &w);
                if (w.type != O42_VALUE_EMPTY && o42_value_to_bool (&w, &b, &e) && b)
                  trues++;
                o42_value_clear (&w);
              }
        }
      else
        {
          gboolean b = FALSE;
          O42ErrorCode e = O42_ERR_VALUE;
          if (o42_value_to_bool (&args[i].value, &b, &e) && b)
            trues++;
        }
    }

  return o42_value_bool (trues % 2 == 1);
}

static O42Value
fn_ifna (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v = operand_value (ctx, &args[0]);
  (void) n;
  if (v.type == O42_VALUE_ERROR && v.as.error == O42_ERR_NA)
    {
      o42_value_clear (&v);
      return operand_value (ctx, &args[1]);
    }
  return v;
}

/* IFS(test1, value1, test2, value2, ...): the first value whose test holds. */
static O42Value
fn_ifs_logic (O42EvalContext *ctx, O42Operand *args, int n)
{
  for (int i = 0; i + 1 < n; i += 2)
    {
      O42Value t = operand_value (ctx, &args[i]);
      gboolean b = FALSE;
      O42ErrorCode err = O42_ERR_VALUE;
      gboolean ok = o42_value_to_bool (&t, &b, &err);
      o42_value_clear (&t);
      if (!ok)
        return o42_value_error (err);
      if (b)
        return operand_value (ctx, &args[i + 1]);
    }
  return o42_value_error (O42_ERR_NA);
}

/* SWITCH(value, case1, result1, ..., default). */
static O42Value
fn_switch (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value v = operand_value (ctx, &args[0]);
  int i;

  if (v.type == O42_VALUE_ERROR)
    return v;

  for (i = 1; i + 1 < n; i += 2)
    {
      O42Value c = operand_value (ctx, &args[i]);
      gboolean hit = (c.type == v.type && o42_value_compare (&c, &v) == 0);
      o42_value_clear (&c);
      if (hit)
        {
          o42_value_clear (&v);
          return operand_value (ctx, &args[i + 1]);
        }
    }
  o42_value_clear (&v);

  if (i < n)
    return operand_value (ctx, &args[i]);
  return o42_value_error (O42_ERR_NA);
}

/* TEXTJOIN(delimiter, ignore_empty, text1, ...). */
static O42Value
fn_textjoin (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *delim = NULL;
  gboolean skip_empty = TRUE;
  GString *out = g_string_new (NULL);
  gboolean first = TRUE;

  ARG_TEXT (0, delim);
  if (!optional_bool (ctx, args, n, 1, TRUE, &skip_empty))
    { g_free (delim); g_string_free (out, TRUE); return o42_value_error (O42_ERR_VALUE); }

  for (int i = 2; i < n; i++)
    {
      if (args[i].is_range)
        {
          const O42Range *r = &args[i].range;
          for (int row = r->row0; row <= r->row1; row++)
            for (int col = r->col0; col <= r->col1; col++)
              {
                O42Value v;
                char *text;
                ctx->get_cell (ctx, args[i].sheet, row, col, &v);
                text = o42_value_to_text (&v);
                o42_value_clear (&v);
                if (!(skip_empty && *text == '\0'))
                  {
                    if (!first) g_string_append (out, delim);
                    g_string_append (out, text);
                    first = FALSE;
                  }
                g_free (text);
              }
        }
      else
        {
          char *text = o42_value_to_text (&args[i].value);
          if (!(skip_empty && *text == '\0'))
            {
              if (!first) g_string_append (out, delim);
              g_string_append (out, text);
              first = FALSE;
            }
          g_free (text);
        }
    }

  g_free (delim);
  return o42_value_take (g_string_free (out, FALSE));
}

static O42Value
fn_roman (O42EvalContext *ctx, O42Operand *args, int n)
{
  static const struct { int value; const char *numeral; } TABLE[] = {
    { 1000, "M" }, { 900, "CM" }, { 500, "D" }, { 400, "CD" }, { 100, "C" },
    { 90, "XC" }, { 50, "L" }, { 40, "XL" }, { 10, "X" }, { 9, "IX" },
    { 5, "V" }, { 4, "IV" }, { 1, "I" }
  };
  double x;
  int v;
  GString *out;

  (void) n;
  ARG_NUMBER (0, x);
  v = (int) floor (x);
  if (v < 0 || v > 3999)
    return o42_value_error (O42_ERR_VALUE);

  out = g_string_new (NULL);
  for (guint i = 0; i < G_N_ELEMENTS (TABLE); i++)
    while (v >= TABLE[i].value)
      {
        g_string_append (out, TABLE[i].numeral);
        v -= TABLE[i].value;
      }
  return o42_value_take (g_string_free (out, FALSE));
}

static O42Value
fn_sumx (O42EvalContext *ctx, O42Operand *args, int n, int mode)
{
  GArray *xs, *ys;
  O42ErrorCode err = O42_ERR_VALUE;
  double total = 0;

  (void) n;
  if (!collect_pairs (ctx, &args[0], &args[1], &xs, &ys, &err))
    return o42_value_error (err);

  for (guint i = 0; i < xs->len; i++)
    {
      double x = g_array_index (xs, double, i), y = g_array_index (ys, double, i);
      switch (mode)
        {
        case 0:  total += x * x - y * y; break;
        case 1:  total += x * x + y * y; break;
        default: total += (x - y) * (x - y); break;
        }
    }
  g_array_free (xs, TRUE);
  g_array_free (ys, TRUE);
  return o42_value_number (total);
}

static O42Value fn_sumx2my2 (O42EvalContext *c, O42Operand *a, int n) { return fn_sumx (c, a, n, 0); }
static O42Value fn_sumx2py2 (O42EvalContext *c, O42Operand *a, int n) { return fn_sumx (c, a, n, 1); }
static O42Value fn_sumxmy2  (O42EvalContext *c, O42Operand *a, int n) { return fn_sumx (c, a, n, 2); }

/* ---------------------------------------------------------------------- */

/* ---------------------------------------------------------------------- */
/* Batch three: the functions Excel's files use that were still missing    */
/* ---------------------------------------------------------------------- */

static const O42Function *find_function (const char *name);
static O42Operand eval_operand (O42EvalContext *ctx, const O42Node *node);

/* ---- hyperbolic inverses ---- */

static O42Value
fn_asinh (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  return o42_value_number (asinh (x));
}

static O42Value
fn_acosh (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  if (x < 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (acosh (x));
}

static O42Value
fn_atanh (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  if (x <= -1 || x >= 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (atanh (x));
}

/* ---- the A family: text counts as 0, booleans as 1 and 0 ---- */

static gboolean
collect_numbers_a (O42EvalContext *ctx, O42Operand *args, int n,
                   GArray **values, O42ErrorCode *err)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (double));

  for (int i = 0; i < n; i++)
    {
      if (args[i].is_range)
        {
          const O42Range *r = &args[i].range;
          for (int row = r->row0; row <= r->row1; row++)
            for (int col = r->col0; col <= r->col1; col++)
              {
                O42Value v;
                double d = 0;
                ctx->get_cell (ctx, args[i].sheet, row, col, &v);
                if (v.type == O42_VALUE_ERROR)
                  { *err = v.as.error; o42_value_clear (&v); g_array_free (out, TRUE); return FALSE; }
                if (v.type == O42_VALUE_EMPTY) { o42_value_clear (&v); continue; }
                if (v.type == O42_VALUE_NUMBER) d = v.as.number;
                else if (v.type == O42_VALUE_BOOL) d = v.as.boolean ? 1 : 0;
                o42_value_clear (&v);
                g_array_append_val (out, d);
              }
        }
      else
        {
          double d = 0;
          O42ErrorCode e = O42_ERR_VALUE;
          if (args[i].value.type == O42_VALUE_ERROR)
            { *err = args[i].value.as.error; g_array_free (out, TRUE); return FALSE; }
          if (!o42_value_to_number (&args[i].value, &d, &e))
            d = 0;
          g_array_append_val (out, d);
        }
    }
  *values = out;
  return TRUE;
}

typedef enum { A_AVERAGE, A_MAX, A_MIN, A_VAR, A_VARP, A_STDEV, A_STDEVP } AKind;

static O42Value
fn_a_family (O42EvalContext *ctx, O42Operand *args, int n, AKind kind)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double r = 0, mean, ssd;
  guint count;

  if (!collect_numbers_a (ctx, args, n, &values, &err))
    return o42_value_error (err);
  count = values->len;
  if (count == 0)
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_DIV0); }

  switch (kind)
    {
    case A_AVERAGE:
      for (guint i = 0; i < count; i++) r += g_array_index (values, double, i);
      r /= count;
      break;
    case A_MAX:
    case A_MIN:
      r = g_array_index (values, double, 0);
      for (guint i = 1; i < count; i++)
        {
          double v = g_array_index (values, double, i);
          if (kind == A_MAX ? v > r : v < r) r = v;
        }
      break;
    default:
      moments (values, &mean, &ssd);
      if ((kind == A_VAR || kind == A_STDEV) && count < 2)
        { g_array_free (values, TRUE); return o42_value_error (O42_ERR_DIV0); }
      r = (kind == A_VAR || kind == A_STDEV) ? ssd / (count - 1) : ssd / count;
      if (kind == A_STDEV || kind == A_STDEVP) r = sqrt (r);
      break;
    }
  g_array_free (values, TRUE);
  return o42_value_number (r);
}

static O42Value fn_averagea (O42EvalContext *c, O42Operand *a, int n) { return fn_a_family (c, a, n, A_AVERAGE); }
static O42Value fn_maxa     (O42EvalContext *c, O42Operand *a, int n) { return fn_a_family (c, a, n, A_MAX); }
static O42Value fn_mina     (O42EvalContext *c, O42Operand *a, int n) { return fn_a_family (c, a, n, A_MIN); }
static O42Value fn_vara     (O42EvalContext *c, O42Operand *a, int n) { return fn_a_family (c, a, n, A_VAR); }
static O42Value fn_varpa    (O42EvalContext *c, O42Operand *a, int n) { return fn_a_family (c, a, n, A_VARP); }
static O42Value fn_stdeva   (O42EvalContext *c, O42Operand *a, int n) { return fn_a_family (c, a, n, A_STDEV); }
static O42Value fn_stdevpa  (O42EvalContext *c, O42Operand *a, int n) { return fn_a_family (c, a, n, A_STDEVP); }

/* ---- SUBTOTAL: a function number picks the aggregate ---- */

static O42Value
fn_subtotal (O42EvalContext *ctx, O42Operand *args, int n)
{
  static const char *names[] = { "AVERAGE", "COUNT", "COUNTA", "MAX", "MIN", "PRODUCT",
                                 "STDEV", "STDEVP", "SUM", "VAR", "VARP" };
  double which;
  const O42Function *fn;

  ARG_NUMBER (0, which);
  if (which >= 101) which -= 100;   /* 101-111 skip hidden rows; the same aggregates here */
  if (which < 1 || which > 11)
    return o42_value_error (O42_ERR_VALUE);
  fn = find_function (names[(int) which - 1]);
  if (fn == NULL)
    return o42_value_error (O42_ERR_NAME);
  return fn->fn (ctx, args + 1, n - 1);
}

/* ---- XLOOKUP ---- */

/* The cells of a one-dimensional range, in order. */
static int
xl_vector_length (const O42Range *r)
{
  return (r->col0 == r->col1) ? r->row1 - r->row0 + 1 : r->col1 - r->col0 + 1;
}

static void
xl_vector_cell (O42EvalContext *ctx, const O42Operand *op, int i, O42Value *out)
{
  const O42Range *r = &op->range;
  if (r->col0 == r->col1)
    ctx->get_cell (ctx, op->sheet, r->row0 + i, r->col0, out);
  else
    ctx->get_cell (ctx, op->sheet, r->row0, r->col0 + i, out);
}

static O42Value
fn_xlookup (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value needle, result;
  double mode = 0;
  int length, best = -1;
  GPatternSpec *pattern = NULL;

  if (!args[1].is_range || !args[2].is_range)
    return o42_value_error (O42_ERR_VALUE);
  if (n >= 5) ARG_NUMBER (4, mode);
  needle = operand_value (ctx, &args[0]);
  if (needle.type == O42_VALUE_ERROR)
    return needle;
  length = xl_vector_length (&args[1].range);

  if (mode == 2 && needle.type == O42_VALUE_TEXT)
    {
      char *folded = g_utf8_casefold (needle.as.text, -1);
      pattern = g_pattern_spec_new (folded);
      g_free (folded);
    }

  for (int i = 0; i < length; i++)
    {
      O42Value v;
      int cmp;
      xl_vector_cell (ctx, &args[1], i, &v);
      if (pattern != NULL)
        {
          gboolean hit = FALSE;
          if (v.type == O42_VALUE_TEXT)
            {
              char *folded = g_utf8_casefold (v.as.text, -1);
              hit = g_pattern_spec_match_string (pattern, folded);
              g_free (folded);
            }
          o42_value_clear (&v);
          if (hit) { best = i; break; }
          continue;
        }
      cmp = o42_value_compare (&v, &needle);
      if (cmp == 0)
        { o42_value_clear (&v); best = i; break; }
      if ((mode == -1 && cmp < 0) || (mode == 1 && cmp > 0))
        {
          /* The nearest value on the wanted side of the needle. */
          if (best < 0)
            best = i;
          else
            {
              O42Value b;
              xl_vector_cell (ctx, &args[1], best, &b);
              if (mode == -1 ? o42_value_compare (&v, &b) > 0 : o42_value_compare (&v, &b) < 0)
                best = i;
              o42_value_clear (&b);
            }
        }
      o42_value_clear (&v);
    }
  if (pattern != NULL)
    g_pattern_spec_free (pattern);
  o42_value_clear (&needle);

  if (best < 0)
    {
      if (n >= 4)
        return operand_value (ctx, &args[3]);
      return o42_value_error (O42_ERR_NA);
    }
  /* The return array runs the same way, or the other way if that is
   * the only way its length fits. */
  {
    const O42Range *r = &args[2].range;
    int rows = r->row1 - r->row0 + 1, cols = r->col1 - r->col0 + 1;
    gboolean vertical = args[1].range.col0 == args[1].range.col1;
    if ((vertical && rows >= length) || (!vertical && cols < length))
      ctx->get_cell (ctx, args[2].sheet, r->row0 + best, r->col0, &result);
    else
      ctx->get_cell (ctx, args[2].sheet, r->row0, r->col0 + best, &result);
  }
  return result;
}

/* ---- references as text and as values ---- */

/* LET is handled before any function is called; this is what the
 * table needs to know it. */
static O42Value
fn_let_stub (O42EvalContext *ctx, O42Operand *args, int n)
{
  (void) ctx; (void) args; (void) n;
  return o42_value_error (O42_ERR_VALUE);
}

static O42Value
fn_xmatch (O42EvalContext *ctx, O42Operand *args, int n)
{
  /* XLOOKUP over a sequence of positions gives the position. */
  O42Operand three[6];
  int length;
  ArrayConst *positions;
  O42Value r;

  if (!args[1].is_range)
    return o42_value_error (O42_ERR_VALUE);
  length = xl_vector_length (&args[1].range);
  positions = array_const_new (length, 1);
  for (int i = 0; i < length; i++) positions->cells[i] = o42_value_number (i + 1);
  three[0] = args[0];
  three[1] = args[1];
  three[2] = array_operand (positions);
  memset (&three[3], 0, sizeof three[3]);
  three[3].value = o42_value_error (O42_ERR_NA);
  if (n >= 3) three[4] = args[2]; else { memset (&three[4], 0, sizeof three[4]); three[4].value = o42_value_number (0); }
  r = fn_xlookup (ctx, three, 5);
  o42_value_clear (&three[3].value);
  if (n < 3) o42_value_clear (&three[4].value);
  return r;
}

static O42Value
fn_address (O42EvalContext *ctx, O42Operand *args, int n)
{
  double row, col, abs = 1;
  char *ref, *text;

  ARG_NUMBER (0, row);
  ARG_NUMBER (1, col);
  if (n >= 3) ARG_NUMBER (2, abs);
  if (row < 1 || col < 1 || row > O42_MAX_ROWS || col > O42_MAX_COLS || abs < 1 || abs > 4)
    return o42_value_error (O42_ERR_VALUE);
  ref = o42_ref_name_full ((int) row - 1, (int) col - 1, abs == 1 || abs == 2, abs == 1 || abs == 3);
  if (n >= 5)
    {
      char *sheet, *quoted;
      ARG_TEXT (4, sheet);
      quoted = o42_sheet_name_quote (sheet);
      text = g_strdup_printf ("%s!%s", quoted, ref);
      g_free (quoted);
      g_free (sheet);
      g_free (ref);
    }
  else
    text = ref;
  return o42_value_take (text);
}

static O42Value
fn_isref (O42EvalContext *ctx, O42Operand *args, int n)
{
  (void) ctx; (void) n;
  return o42_value_bool (args[0].is_range);
}

static O42Value
fn_areas (O42EvalContext *ctx, O42Operand *args, int n)
{
  (void) ctx; (void) n;
  return args[0].is_range ? o42_value_number (1) : o42_value_error (O42_ERR_VALUE);
}

static O42Value
fn_hyperlink (O42EvalContext *ctx, O42Operand *args, int n)
{
  return operand_value (ctx, &args[n >= 2 ? 1 : 0]);
}

/* OFFSET and INDIRECT give back a reference, which no other function
 * does; the evaluator builds their result as an operand (see
 * eval_range_call) so that SUM(OFFSET(A1,0,0,3,1)) sees a range.  These
 * bodies are reached only when the arguments are wrong. */
static O42Value
fn_offset (O42EvalContext *ctx, O42Operand *args, int n)
{
  (void) ctx; (void) args; (void) n;
  return o42_value_error (O42_ERR_VALUE);
}

static O42Value
fn_indirect (O42EvalContext *ctx, O42Operand *args, int n)
{
  (void) ctx; (void) args; (void) n;
  return o42_value_error (O42_ERR_REF);
}

/* LAMBDA's parameters and body, and the depth guard for recursion. */
static O42Operand apply_lambda (O42EvalContext *ctx, const O42Node *lambda,
                                O42Operand *args, int n_args);
static int lambda_depth = 0;

/* LET's names while its calculation is evaluated; innermost last. */
typedef struct {
  const char *name;
  O42Operand  operand;
} LetBinding;

static GPtrArray *let_scope = NULL;

/* Binds a lambda's parameters to `args` and evaluates its body.  The
 * parameters are the call's arguments but the last, which is the body;
 * a missing argument is an empty value, which ISOMITTED sees. */
static O42Operand
apply_lambda (O42EvalContext *ctx, const O42Node *lambda, O42Operand *args, int n_args)
{
  int n_params = lambda != NULL && lambda->as.call.args != NULL ? (int) lambda->as.call.args->len - 1 : -1;
  O42Operand result;
  int pushed = 0;

  memset (&result, 0, sizeof result);
  if (n_params < 0 || lambda_depth > 64)
    {
      result.value = o42_value_error (n_params < 0 ? O42_ERR_VALUE : O42_ERR_NUM);
      return result;
    }
  if (let_scope == NULL)
    let_scope = g_ptr_array_new ();
  for (int i = 0; i < n_params; i++)
    {
      const O42Node *p = g_ptr_array_index (lambda->as.call.args, i);
      LetBinding *b;

      if (p == NULL || p->type != O42_NODE_NAME)
        break;
      b = g_new0 (LetBinding, 1);
      b->name = p->as.name;
      if (i < n_args)
        {
          b->operand = args[i];
          b->operand.value = o42_value_copy (&args[i].value);
        }
      else
        b->operand.value = o42_value_empty ();
      g_ptr_array_add (let_scope, b);
      pushed++;
    }
  lambda_depth++;
  result = eval_operand (ctx, g_ptr_array_index (lambda->as.call.args, n_params));
  lambda_depth--;
  while (pushed-- > 0)
    {
      LetBinding *dead = g_ptr_array_index (let_scope, let_scope->len - 1);
      g_ptr_array_remove_index (let_scope, let_scope->len - 1);
      operand_clear (&dead->operand);
      g_free (dead);
    }
  return result;
}

/* The lambda an argument comes to, or NULL. */
static const O42Node *
lambda_arg (O42EvalContext *ctx, const O42Node *node, int index, O42Operand *hold)
{
  *hold = eval_operand (ctx, g_ptr_array_index (node->as.call.args, index));
  return hold->lambda;
}

/* Scalar arguments of the array functions. */
static gboolean
eval_number_arg (O42EvalContext *ctx, const O42Node *node, int index, double *out)
{
  O42Value v = eval_node (ctx, g_ptr_array_index (node->as.call.args, index));
  O42ErrorCode e = O42_ERR_VALUE;
  gboolean ok = v.type == O42_VALUE_EMPTY ? TRUE : o42_value_to_number (&v, out, &e);
  o42_value_clear (&v);
  return ok;
}

static gboolean
eval_bool_arg (O42EvalContext *ctx, const O42Node *node, int index, gboolean *out)
{
  O42Value v = eval_node (ctx, g_ptr_array_index (node->as.call.args, index));
  O42ErrorCode e = O42_ERR_VALUE;
  gboolean ok = v.type == O42_VALUE_EMPTY ? TRUE : o42_value_to_bool (&v, out, &e);
  o42_value_clear (&v);
  return ok;
}

/* A row (or column) of an operand as one text, for comparing rows. */
static char *
operand_line_key (O42EvalContext *ctx, const O42Operand *op, int line, gboolean by_col)
{
  GString *key = g_string_new (NULL);
  int rows, cols, n;
  operand_dims (op, &rows, &cols);
  n = by_col ? rows : cols;
  for (int j = 0; j < n; j++)
    {
      O42Value v = by_col ? operand_cell (ctx, op, j, line) : operand_cell (ctx, op, line, j);
      char *text = o42_value_to_text (&v);
      char *folded = g_utf8_casefold (text, -1);
      g_string_append (key, folded);
      g_string_append_c (key, '\001');
      g_free (folded);
      g_free (text);
      o42_value_clear (&v);
    }
  return g_string_free (key, FALSE);
}

/* ---- Gnumeric's time series analysis ---------------------------------- */


/* A sequence for these functions is a vector: n by 1 or 1 by n, and
 * nothing else.  The values come back in an array the caller frees. */
static gboolean
series_vector (O42EvalContext *ctx, O42Operand *operand, double **out, int *count)
{
  int rows, cols, n;
  double *values;

  operand_dims (operand, &rows, &cols);
  if (rows != 1 && cols != 1)
    return FALSE;
  n = rows * cols;
  if (n < 1)
    return FALSE;

  values = g_new0 (double, n);
  for (int i = 0; i < n; i++)
    {
      O42Value v = operand_cell (ctx, operand, rows == 1 ? 0 : i, rows == 1 ? i : 0);
      O42ErrorCode e = O42_ERR_VALUE;

      if (!o42_value_to_number (&v, &values[i], &e))
        {
          o42_value_clear (&v);
          g_free (values);
          return FALSE;
        }
      o42_value_clear (&v);
    }
  *out = values;
  *count = n;
  return TRUE;
}

/* The same, for a sequence that may hold complex numbers written as
 * text -- what the inverse transform is given.  `im` may come back all
 * zeroes, which is what a column of ordinary numbers means. */
static gboolean
series_complex_vector (O42EvalContext *ctx, O42Operand *operand,
                       double **re_out, double **im_out, int *count)
{
  int rows, cols, n;
  double *re, *im;

  operand_dims (operand, &rows, &cols);
  if (rows != 1 && cols != 1)
    return FALSE;
  n = rows * cols;
  if (n < 1)
    return FALSE;

  re = g_new0 (double, n);
  im = g_new0 (double, n);
  for (int i = 0; i < n; i++)
    {
      O42Value v = operand_cell (ctx, operand, rows == 1 ? 0 : i, rows == 1 ? i : 0);
      O42ErrorCode e = O42_ERR_VALUE;
      gboolean ok;

      if (v.type == O42_VALUE_TEXT)
        {
          char suffix = 'i';

          ok = complex_parse (v.as.text, &re[i], &im[i], &suffix);
        }
      else
        ok = o42_value_to_number (&v, &re[i], &e);
      o42_value_clear (&v);
      if (!ok)
        {
          g_free (re);
          g_free (im);
          return FALSE;
        }
    }
  *re_out = re;
  *im_out = im;
  *count = n;
  return TRUE;
}

/* The discrete Fourier transform, worked out as it is written: n^2
 * multiplications and no cleverness.  A sheet's sequences are short,
 * and the plain sum is the one that can be read against the formula. */
static void
series_dft (const double *in, const double *in_im, int n, gboolean inverse,
            double *re, double *im)
{
  double sign = inverse ? 1.0 : -1.0;

  for (int k = 0; k < n; k++)
    {
      double sum_re = 0, sum_im = 0;

      for (int j = 0; j < n; j++)
        {
          /* The turn is kept as the fraction of a circle it is, so that
           * a quarter turn is exactly zero and one, and a long sequence
           * does not drift.  Without that, the transform of four real
           * numbers comes back with imaginary parts of 1e-16. */
          long long index = ((long long) j * k) % n;
          double angle = sign * 2 * G_PI * (double) index / n;
          double c, si;

          if (4 * index % n == 0)
            {
              int quarter = (int) (4 * index / n);   /* 0, 1, 2 or 3 */

              c = quarter == 0 ? 1 : quarter == 2 ? -1 : 0;
              si = quarter == 1 ? sign : quarter == 3 ? -sign : 0;
            }
          else
            {
              c = cos (angle);
              si = sin (angle);
            }
          sum_re += in[j] * c - (in_im != NULL ? in_im[j] * si : 0);
          sum_im += in[j] * si + (in_im != NULL ? in_im[j] * c : 0);
        }
      re[k] = inverse ? sum_re / n : sum_re;
      im[k] = inverse ? sum_im / n : sum_im;
    }
}

/* The second derivatives of the natural cubic spline through the
 * points, by the usual tridiagonal sweep. */
static double *
spline_second_derivatives (const double *x, const double *y, int n)
{
  double *m = g_new0 (double, n);
  double *c = g_new0 (double, n);
  double *d = g_new0 (double, n);

  if (n < 3)
    {
      g_free (c);
      g_free (d);
      return m;   /* two points are a straight line: no curvature */
    }
  for (int i = 1; i < n - 1; i++)
    {
      double h0 = x[i] - x[i - 1], h1 = x[i + 1] - x[i];
      double diagonal = 2 * (h0 + h1) - h0 * c[i - 1];

      c[i] = h1 / diagonal;
      d[i] = (6 * ((y[i + 1] - y[i]) / h1 - (y[i] - y[i - 1]) / h0) - h0 * d[i - 1]) / diagonal;
    }
  for (int i = n - 2; i >= 1; i--)
    m[i] = d[i] - c[i] * m[i + 1];
  g_free (c);
  g_free (d);
  return m;
}

/* The interpolant at one point.  Outside the data it holds the value of
 * the nearest end, which is what a staircase does and what keeps a
 * target just past the last abscissa from running away. */
static double
series_interpolate_at (const double *x, const double *y, const double *m2,
                       int n, int method, double at)
{
  int i;

  if (at <= x[0])
    return y[0];
  if (at >= x[n - 1])
    return y[n - 1];
  for (i = 0; i < n - 1; i++)
    if (at < x[i + 1])
      break;
  if (i >= n - 1)
    i = n - 2;

  if (method == 2 || method == 3)
    return y[i];   /* the staircase holds the value it started at */
  if ((method == 4 || method == 5) && m2 != NULL)
    {
      double h = x[i + 1] - x[i];
      double a = (x[i + 1] - at) / h, b = (at - x[i]) / h;

      return a * y[i] + b * y[i + 1] +
             ((a * a * a - a) * m2[i] + (b * b * b - b) * m2[i + 1]) * (h * h) / 6;
    }
  {
    double h = x[i + 1] - x[i];

    return y[i] + (y[i + 1] - y[i]) * (at - x[i]) / h;
  }
}

/* The mean of the interpolant over an interval.  It is integrated piece
 * by piece, cut at every abscissa inside the interval, with Simpson's
 * rule -- which is exact for the cubics and for everything simpler. */
static double
series_interpolate_mean (const double *x, const double *y, const double *m2,
                         int n, int method, double from, double to)
{
  double total = 0, start = from;

  if (to <= from)
    return series_interpolate_at (x, y, m2, n, method, from);

  for (int i = 0; i <= n && start < to; i++)
    {
      double stop = to;

      for (int k = 0; k < n; k++)
        if (x[k] > start && x[k] < stop)
          stop = x[k];
      {
        double middle = (start + stop) / 2;
        double fa = series_interpolate_at (x, y, m2, n, method, start);
        double fm = series_interpolate_at (x, y, m2, n, method, middle);
        double fb = series_interpolate_at (x, y, m2, n, method,
                                           stop - (stop - start) * 1e-12);

        total += (stop - start) * (fa + 4 * fm + fb) / 6;
      }
      start = stop;
    }
  return total / (to - from);
}

/* One of the four windows a periodogram may be seen through. */
static double
series_window (int filter, int j, int n)
{
  double half = (n - 1) / 2.0;

  switch (filter)
    {
    case 1:   /* Bartlett: a triangle */
      return 1 - fabs ((j - half) / half);
    case 2:   /* Hahn: a raised cosine */
      return 0.5 * (1 - cos (2 * G_PI * j / (n - 1)));
    case 3:   /* Welch: a parabola */
      return 1 - ((j - half) / half) * ((j - half) / half);
    default:
      return 1;
    }
}

/* One straight-line fit for LOGFIT: y on ln(sign (x - c)), given c.
 * FALSE when that offset puts any point at or past the singularity. */
static gboolean
logfit_line (const double *x, const double *y, int n, double sign, double c,
             double *a, double *b, double *ssr)
{
  double sum_z = 0, sum_y = 0, sum_zz = 0, sum_zy = 0, denominator;
  double *z = g_new0 (double, n);

  for (int i = 0; i < n; i++)
    {
      double inside = sign * (x[i] - c);

      if (!(inside > 0))
        { g_free (z); return FALSE; }
      z[i] = log (inside);
      sum_z += z[i];
      sum_y += y[i];
    }
  for (int i = 0; i < n; i++)
    {
      sum_zz += (z[i] - sum_z / n) * (z[i] - sum_z / n);
      sum_zy += (z[i] - sum_z / n) * (y[i] - sum_y / n);
    }
  denominator = sum_zz;
  if (!(denominator > 0))
    { g_free (z); return FALSE; }
  *b = sum_zy / denominator;
  *a = sum_y / n - *b * (sum_z / n);
  *ssr = 0;
  for (int i = 0; i < n; i++)
    {
      double residual = y[i] - (*a + *b * z[i]);

      *ssr += residual * residual;
    }
  g_free (z);
  return isfinite (*a) && isfinite (*b) && isfinite (*ssr);
}

/* TRUE if `node` is a call to a reference-returning function; then
 * `out` holds its result, a range or an error value. */
static gboolean
eval_range_call (O42EvalContext *ctx, const O42Node *node, O42Operand *out)
{
  int n_args;

  if (node->type != O42_NODE_CALL)
    return FALSE;
  n_args = node->as.call.args ? (int) node->as.call.args->len : 0;
  memset (out, 0, sizeof *out);

  if (strcmp (node->as.call.name, "OFFSET") == 0)
    {
      O42Operand base;
      double rows, cols, height, width;
      O42Value v;
      O42ErrorCode e = O42_ERR_VALUE;
      O42Range r;

      if (n_args < 3 || n_args > 5)
        { out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
      base = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      if (!base.is_range)
        { operand_clear (&base); out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
      v = eval_node (ctx, g_ptr_array_index (node->as.call.args, 1));
      if (!o42_value_to_number (&v, &rows, &e)) { o42_value_clear (&v); out->value = o42_value_error (e); return TRUE; }
      o42_value_clear (&v);
      v = eval_node (ctx, g_ptr_array_index (node->as.call.args, 2));
      if (!o42_value_to_number (&v, &cols, &e)) { o42_value_clear (&v); out->value = o42_value_error (e); return TRUE; }
      o42_value_clear (&v);
      height = base.range.row1 - base.range.row0 + 1;
      width = base.range.col1 - base.range.col0 + 1;
      if (n_args >= 4)
        {
          v = eval_node (ctx, g_ptr_array_index (node->as.call.args, 3));
          if (v.type != O42_VALUE_EMPTY && !o42_value_to_number (&v, &height, &e))
            { o42_value_clear (&v); out->value = o42_value_error (e); return TRUE; }
          o42_value_clear (&v);
        }
      if (n_args >= 5)
        {
          v = eval_node (ctx, g_ptr_array_index (node->as.call.args, 4));
          if (v.type != O42_VALUE_EMPTY && !o42_value_to_number (&v, &width, &e))
            { o42_value_clear (&v); out->value = o42_value_error (e); return TRUE; }
          o42_value_clear (&v);
        }
      /* Checked as doubles before the casts: a height of 3e9 cast to
       * int wraps negative and would pass as a range. */
      if (height < 1 || width < 1 || height > O42_MAX_ROWS || width > O42_MAX_COLS ||
          fabs (rows) > O42_MAX_ROWS || fabs (cols) > O42_MAX_COLS)
        { operand_clear (&base); out->value = o42_value_error (O42_ERR_REF); return TRUE; }
      r.row0 = base.range.row0 + (int) rows;
      r.col0 = base.range.col0 + (int) cols;
      r.row1 = r.row0 + (int) height - 1;
      r.col1 = r.col0 + (int) width - 1;
      if (r.row0 < 0 || r.col0 < 0 ||
          r.row1 >= O42_MAX_ROWS || r.col1 >= O42_MAX_COLS)
        { operand_clear (&base); out->value = o42_value_error (O42_ERR_REF); return TRUE; }
      out->is_range = TRUE;
      out->sheet = base.sheet;
      out->range = r;
      operand_clear (&base);
      return TRUE;
    }

  if (strcmp (node->as.call.name, "IF") == 0 && (n_args == 2 || n_args == 3))
    {
      /* IF over a range of conditions picks cell by cell, which is what
       * SUM(IF(A1:A9>5,1,0)) counts on. */
      O42Operand cond = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      O42Operand yes, no;
      int rows, cols;
      ArrayConst *a;

      if (!operand_is_multi (&cond))
        { operand_clear (&cond); return FALSE; }
      yes = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 1));
      memset (&no, 0, sizeof no);
      if (n_args == 3)
        no = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 2));
      else
        no.value = o42_value_bool (FALSE);
      operand_dims (&cond, &rows, &cols);
      a = array_const_new (rows, cols);
      for (int i = 0; i < rows; i++)
        for (int j = 0; j < cols; j++)
          {
            O42Value c = operand_cell (ctx, &cond, i, j);
            gboolean truth = FALSE;
            O42ErrorCode e = O42_ERR_VALUE;
            if (c.type == O42_VALUE_ERROR)
              a->cells[i * cols + j] = o42_value_copy (&c);
            else if (!o42_value_to_bool (&c, &truth, &e))
              a->cells[i * cols + j] = o42_value_error (e);
            else
              a->cells[i * cols + j] = operand_cell (ctx, truth ? &yes : &no, i, j);
            o42_value_clear (&c);
          }
      operand_clear (&cond);
      operand_clear (&yes);
      operand_clear (&no);
      *out = array_operand (a);
      return TRUE;
    }

  if (strcmp (node->as.call.name, "LAMBDA") == 0 && n_args >= 1)
    {
      /* Not evaluated here: the node itself is the value, and calling
       * it binds its parameters and evaluates its last argument. */
      memset (out, 0, sizeof *out);
      out->lambda = node;
      return TRUE;
    }

  if (strcmp (node->as.call.name, "ISOMITTED") == 0 && n_args == 1)
    {
      /* An argument left out, or a lambda parameter that was: such a
       * parameter is bound to an empty value. */
      const O42Node *arg = g_ptr_array_index (node->as.call.args, 0);
      gboolean omitted = arg == NULL || arg->type == O42_NODE_EMPTY;

      if (!omitted && arg->type == O42_NODE_NAME && let_scope != NULL)
        for (guint i = let_scope->len; i > 0 && !omitted; i--)
          {
            const LetBinding *b = g_ptr_array_index (let_scope, i - 1);
            if (g_ascii_strcasecmp (b->name, arg->as.name) == 0)
              omitted = !b->operand.is_range && b->operand.lambda == NULL &&
                        b->operand.value.type == O42_VALUE_EMPTY;
          }
      out->value = o42_value_bool (omitted);
      return TRUE;
    }

  if (strcmp (node->as.call.name, "LET") == 0 && n_args >= 3 && n_args % 2 == 1)
    {
      /* LET(name1, value1, ..., calculation): each name is bound to
       * its value, seen by the values after it and the calculation. */
      int pushed = 0;
      O42Operand result;

      if (let_scope == NULL)
        let_scope = g_ptr_array_new ();
      for (int i = 0; i + 1 < n_args; i += 2)
        {
          const O42Node *name_node = g_ptr_array_index (node->as.call.args, i);
          LetBinding *b;
          if (name_node == NULL || name_node->type != O42_NODE_NAME)
            {
              while (pushed-- > 0)
                {
                  LetBinding *dead = g_ptr_array_index (let_scope, let_scope->len - 1);
                  g_ptr_array_remove_index (let_scope, let_scope->len - 1);
                  operand_clear (&dead->operand);
                  g_free (dead);
                }
              out->value = o42_value_error (O42_ERR_VALUE);
              return TRUE;
            }
          b = g_new0 (LetBinding, 1);
          b->name = name_node->as.name;
          b->operand = eval_operand (ctx, g_ptr_array_index (node->as.call.args, i + 1));
          g_ptr_array_add (let_scope, b);
          pushed++;
        }
      result = eval_operand (ctx, g_ptr_array_index (node->as.call.args, n_args - 1));
      while (pushed-- > 0)
        {
          LetBinding *dead = g_ptr_array_index (let_scope, let_scope->len - 1);
          g_ptr_array_remove_index (let_scope, let_scope->len - 1);
          operand_clear (&dead->operand);
          g_free (dead);
        }
      *out = result;
      return TRUE;
    }

  if ((strcmp (node->as.call.name, "MAP") == 0 && n_args >= 2) ||
      (strcmp (node->as.call.name, "BYROW") == 0 && n_args == 2) ||
      (strcmp (node->as.call.name, "BYCOL") == 0 && n_args == 2) ||
      (strcmp (node->as.call.name, "REDUCE") == 0 && n_args == 3) ||
      (strcmp (node->as.call.name, "SCAN") == 0 && n_args == 3) ||
      (strcmp (node->as.call.name, "MAKEARRAY") == 0 && n_args == 3))
    {
      const char *what = node->as.call.name;
      O42Operand hold, first;
      const O42Node *fn = lambda_arg (ctx, node, n_args - 1, &hold);
      ArrayConst *a = NULL;

      memset (&first, 0, sizeof first);
      if (fn == NULL)
        {
          operand_clear (&hold);
          out->value = o42_value_error (O42_ERR_VALUE);
          return TRUE;
        }

      if (strcmp (what, "MAKEARRAY") == 0)
        {
          double rows = 0, cols = 0;
          if (!eval_number_arg (ctx, node, 0, &rows) || !eval_number_arg (ctx, node, 1, &cols) ||
              rows < 1 || cols < 1 || rows > O42_MAX_ROWS || cols > O42_MAX_COLS)
            { operand_clear (&hold); out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
          a = array_const_new ((int) rows, (int) cols);
          for (int i = 0; i < (int) rows; i++)
            for (int j = 0; j < (int) cols; j++)
              {
                O42Operand two[2], r;
                memset (two, 0, sizeof two);
                two[0].value = o42_value_number (i + 1);
                two[1].value = o42_value_number (j + 1);
                r = apply_lambda (ctx, fn, two, 2);
                a->cells[i * (int) cols + j] = operand_value (ctx, &r);
                operand_clear (&r);
                o42_value_clear (&two[0].value);
                o42_value_clear (&two[1].value);
              }
        }
      else if (strcmp (what, "MAP") == 0)
        {
          /* One lambda argument per array, elementwise over the first
           * array's shape. */
          int n_arrays = n_args - 1;
          O42Operand arrays[8];
          int rows = 1, cols = 1;

          n_arrays = MIN (n_arrays, 8);
          for (int k = 0; k < n_arrays; k++)
            arrays[k] = eval_operand (ctx, g_ptr_array_index (node->as.call.args, k));
          operand_dims (&arrays[0], &rows, &cols);
          a = array_const_new (rows, cols);
          for (int i = 0; i < rows; i++)
            for (int j = 0; j < cols; j++)
              {
                O42Operand each[8], r;
                memset (each, 0, sizeof each);
                for (int k = 0; k < n_arrays; k++)
                  each[k].value = operand_cell (ctx, &arrays[k], i, j);
                r = apply_lambda (ctx, fn, each, n_arrays);
                a->cells[i * cols + j] = operand_value (ctx, &r);
                operand_clear (&r);
                for (int k = 0; k < n_arrays; k++)
                  o42_value_clear (&each[k].value);
              }
          for (int k = 0; k < n_arrays; k++)
            operand_clear (&arrays[k]);
        }
      else if (strcmp (what, "BYROW") == 0 || strcmp (what, "BYCOL") == 0)
        {
          gboolean by_row = what[2] == 'R';
          int rows, cols, n_lines, n_across;

          first = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
          operand_dims (&first, &rows, &cols);
          n_lines = by_row ? rows : cols;
          n_across = by_row ? cols : rows;
          a = by_row ? array_const_new (n_lines, 1) : array_const_new (1, n_lines);
          for (int i = 0; i < n_lines; i++)
            {
              ArrayConst *line = by_row ? array_const_new (1, n_across) : array_const_new (n_across, 1);
              O42Operand one, r;

              for (int j = 0; j < n_across; j++)
                line->cells[j] = by_row ? operand_cell (ctx, &first, i, j) : operand_cell (ctx, &first, j, i);
              one = array_operand (line);
              r = apply_lambda (ctx, fn, &one, 1);
              a->cells[i] = operand_value (ctx, &r);
              operand_clear (&r);
              operand_clear (&one);
            }
          operand_clear (&first);
        }
      else
        {
          /* REDUCE and SCAN: an accumulator over every cell in reading order. */
          gboolean scan = what[0] == 'S';
          O42Operand acc, values;
          int rows, cols;

          memset (&acc, 0, sizeof acc);
          acc = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
          values = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 1));
          operand_dims (&values, &rows, &cols);
          if (scan)
            a = array_const_new (rows, cols);
          for (int i = 0; i < rows; i++)
            for (int j = 0; j < cols; j++)
              {
                O42Operand two[2], r;
                memset (two, 0, sizeof two);
                two[0] = acc;
                two[0].value = o42_value_copy (&acc.value);
                two[1].value = operand_cell (ctx, &values, i, j);
                r = apply_lambda (ctx, fn, two, 2);
                o42_value_clear (&two[0].value);
                o42_value_clear (&two[1].value);
                operand_clear (&acc);
                memset (&acc, 0, sizeof acc);
                acc.value = operand_value (ctx, &r);
                operand_clear (&r);
                if (scan)
                  a->cells[i * cols + j] = o42_value_copy (&acc.value);
              }
          operand_clear (&values);
          if (!scan)
            {
              *out = acc;
              operand_clear (&hold);
              return TRUE;
            }
          operand_clear (&acc);
        }

      operand_clear (&hold);
      *out = array_operand (a);
      return TRUE;
    }

  if (strcmp (node->as.call.name, "SEQUENCE") == 0 && n_args >= 1 && n_args <= 4)
    {
      double rows = 1, cols = 1, start = 1, step = 1;
      ArrayConst *a;
      if (!eval_number_arg (ctx, node, 0, &rows) || (n_args >= 2 && !eval_number_arg (ctx, node, 1, &cols)) ||
          (n_args >= 3 && !eval_number_arg (ctx, node, 2, &start)) || (n_args >= 4 && !eval_number_arg (ctx, node, 3, &step)))
        { out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
      rows = floor (rows); cols = floor (cols);
      if (rows < 1 || cols < 1 || rows > O42_MAX_ROWS || cols > O42_MAX_COLS)
        { out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
      if (rows * cols > ARRAY_CELLS_MAX)
        { out->value = o42_value_error (O42_ERR_NUM); return TRUE; }
      a = array_const_new ((int) rows, (int) cols);
      for (int i = 0; i < (int) rows * (int) cols; i++)
        a->cells[i] = o42_value_number (start + i * step);
      *out = array_operand (a);
      return TRUE;
    }

  if (strcmp (node->as.call.name, "RANDARRAY") == 0 && n_args <= 5)
    {
      double rows = 1, cols = 1, lo = 0, hi = 1;
      gboolean whole = FALSE;
      ArrayConst *a;
      if ((n_args >= 1 && !eval_number_arg (ctx, node, 0, &rows)) || (n_args >= 2 && !eval_number_arg (ctx, node, 1, &cols)) ||
          (n_args >= 3 && !eval_number_arg (ctx, node, 2, &lo)) || (n_args >= 4 && !eval_number_arg (ctx, node, 3, &hi)))
        { out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
      if (n_args >= 5)
        {
          O42Value v = eval_node (ctx, g_ptr_array_index (node->as.call.args, 4));
          O42ErrorCode e = O42_ERR_VALUE;
          if (!o42_value_to_bool (&v, &whole, &e)) whole = FALSE;
          o42_value_clear (&v);
        }
      rows = floor (rows); cols = floor (cols);
      if (rows < 1 || cols < 1 || rows > O42_MAX_ROWS || cols > O42_MAX_COLS || hi < lo)
        { out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
      if (rows * cols > ARRAY_CELLS_MAX)
        { out->value = o42_value_error (O42_ERR_NUM); return TRUE; }
      a = array_const_new ((int) rows, (int) cols);
      for (int i = 0; i < (int) rows * (int) cols; i++)
        {
          double r = g_random_double_range (lo, hi);
          a->cells[i] = o42_value_number (whole ? floor (g_random_double_range (lo, hi + 1)) : r);
        }
      *out = array_operand (a);
      return TRUE;
    }

  if (strcmp (node->as.call.name, "UNIQUE") == 0 && n_args >= 1 && n_args <= 3)
    {
      /* Distinct rows (or columns), in first-seen order; with
       * exactly_once, only those that occur once. */
      O42Operand src = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      gboolean by_col = FALSE, once = FALSE;
      int rows, cols, n_items, n_keep = 0;
      GPtrArray *keys;
      GArray *counts, *first;
      ArrayConst *a;

      if (n_args >= 2) eval_bool_arg (ctx, node, 1, &by_col);
      if (n_args >= 3) eval_bool_arg (ctx, node, 2, &once);
      operand_dims (&src, &rows, &cols);
      n_items = by_col ? cols : rows;
      keys = g_ptr_array_new_with_free_func (g_free);
      counts = g_array_new (FALSE, FALSE, sizeof (int));
      first = g_array_new (FALSE, FALSE, sizeof (int));
      for (int i = 0; i < n_items; i++)
        {
          char *key = operand_line_key (ctx, &src, i, by_col);
          gboolean seen = FALSE;
          for (guint k = 0; k < keys->len && !seen; k++)
            if (strcmp (g_ptr_array_index (keys, k), key) == 0)
              { g_array_index (counts, int, k)++; seen = TRUE; }
          if (!seen)
            {
              int one = 1;
              g_ptr_array_add (keys, key);
              g_array_append_val (counts, one);
              g_array_append_val (first, i);
            }
          else
            g_free (key);
        }
      for (guint k = 0; k < keys->len; k++)
        if (!once || g_array_index (counts, int, k) == 1) n_keep++;
      if (n_keep == 0)
        { out->value = o42_value_error (O42_ERR_VALUE); }
      else
        {
          int n_across = by_col ? rows : cols, at = 0;
          a = by_col ? array_const_new (rows, n_keep) : array_const_new (n_keep, cols);
          for (guint k = 0; k < keys->len; k++)
            {
              int line;
              if (once && g_array_index (counts, int, k) != 1) continue;
              line = g_array_index (first, int, k);
              for (int j = 0; j < n_across; j++)
                {
                  O42Value v = by_col ? operand_cell (ctx, &src, j, line) : operand_cell (ctx, &src, line, j);
                  if (by_col) a->cells[j * n_keep + at] = v; else a->cells[at * cols + j] = v;
                }
              at++;
            }
          *out = array_operand (a);
        }
      g_ptr_array_unref (keys);
      g_array_unref (counts);
      g_array_unref (first);
      operand_clear (&src);
      return TRUE;
    }

  if ((strcmp (node->as.call.name, "SORT") == 0 && n_args >= 1 && n_args <= 4) ||
      (strcmp (node->as.call.name, "SORTBY") == 0 && n_args >= 2 && n_args <= 4))
    {
      gboolean sortby = node->as.call.name[4] == 'B';
      O42Operand src = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      O42Operand by;
      double index = 1, order = 1;
      gboolean by_col = FALSE;
      int rows, cols, n_items, n_across;
      GArray *idx;
      ArrayConst *a;

      memset (&by, 0, sizeof by);
      if (sortby)
        {
          by = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 1));
          if (n_args >= 3 && !eval_number_arg (ctx, node, 2, &order)) order = 1;
        }
      else
        {
          if (n_args >= 2 && !eval_number_arg (ctx, node, 1, &index)) index = 1;
          if (n_args >= 3 && !eval_number_arg (ctx, node, 2, &order)) order = 1;
          if (n_args >= 4) eval_bool_arg (ctx, node, 3, &by_col);
        }
      operand_dims (&src, &rows, &cols);
      n_items = by_col ? cols : rows;
      n_across = by_col ? rows : cols;
      if (index < 1 || index > n_across)
        { operand_clear (&src); operand_clear (&by); out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
      idx = g_array_new (FALSE, FALSE, sizeof (int));
      for (int i = 0; i < n_items; i++) g_array_append_val (idx, i);
      {
        /* Insertion sort on the key column: stable, and the sets here
         * are small. */
        for (int i = 1; i < n_items; i++)
          for (int j = i; j > 0; j--)
            {
              int p1 = g_array_index (idx, int, j - 1), p2 = g_array_index (idx, int, j);
              O42Value k1, k2;
              int cmp;
              if (sortby)
                { k1 = operand_cell (ctx, &by, by_col ? 0 : p1, by_col ? p1 : 0); k2 = operand_cell (ctx, &by, by_col ? 0 : p2, by_col ? p2 : 0); }
              else
                { k1 = by_col ? operand_cell (ctx, &src, (int) index - 1, p1) : operand_cell (ctx, &src, p1, (int) index - 1);
                  k2 = by_col ? operand_cell (ctx, &src, (int) index - 1, p2) : operand_cell (ctx, &src, p2, (int) index - 1); }
              cmp = o42_value_compare (&k1, &k2);
              o42_value_clear (&k1);
              o42_value_clear (&k2);
              if (order < 0) cmp = -cmp;
              if (cmp <= 0) break;
              g_array_index (idx, int, j) = p1;
              g_array_index (idx, int, j - 1) = p2;
            }
      }
      a = array_const_new (rows, cols);
      for (int i = 0; i < n_items; i++)
        {
          int from = g_array_index (idx, int, i);
          for (int j = 0; j < n_across; j++)
            {
              if (by_col) a->cells[j * cols + i] = operand_cell (ctx, &src, j, from);
              else a->cells[i * cols + j] = operand_cell (ctx, &src, from, j);
            }
        }
      g_array_unref (idx);
      operand_clear (&src);
      operand_clear (&by);
      *out = array_operand (a);
      return TRUE;
    }

  if (strcmp (node->as.call.name, "FILTER") == 0 && n_args >= 2 && n_args <= 3)
    {
      O42Operand src = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      O42Operand inc = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 1));
      int rows, cols, ir, ic, n_keep = 0;
      gboolean by_col;
      GArray *keep = g_array_new (FALSE, FALSE, sizeof (int));
      ArrayConst *a;

      operand_dims (&src, &rows, &cols);
      operand_dims (&inc, &ir, &ic);
      by_col = ir == 1 && ic == cols && cols > 1 && ir != rows;
      {
        int n_items = by_col ? cols : rows;
        for (int i = 0; i < n_items; i++)
          {
            O42Value v = by_col ? operand_cell (ctx, &inc, 0, i) : operand_cell (ctx, &inc, i, 0);
            gboolean truth = FALSE;
            O42ErrorCode e = O42_ERR_VALUE;
            if (v.type != O42_VALUE_ERROR && o42_value_to_bool (&v, &truth, &e) && truth)
              { g_array_append_val (keep, i); n_keep++; }
            o42_value_clear (&v);
          }
      }
      if (n_keep == 0)
        {
          if (n_args >= 3)
            {
              O42Operand alt = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 2));
              *out = alt;
            }
          else
            out->value = o42_value_error (O42_ERR_VALUE);
        }
      else
        {
          a = by_col ? array_const_new (rows, n_keep) : array_const_new (n_keep, cols);
          for (int i = 0; i < n_keep; i++)
            {
              int from = g_array_index (keep, int, i);
              if (by_col)
                for (int j = 0; j < rows; j++) a->cells[j * n_keep + i] = operand_cell (ctx, &src, j, from);
              else
                for (int j = 0; j < cols; j++) a->cells[i * cols + j] = operand_cell (ctx, &src, from, j);
            }
          *out = array_operand (a);
        }
      g_array_unref (keep);
      operand_clear (&src);
      operand_clear (&inc);
      return TRUE;
    }

  /* CHOOSECOLS and CHOOSEROWS: the columns or rows named, in the order
   * named, counting from the far end for a negative number. */
  if ((strcmp (node->as.call.name, "CHOOSECOLS") == 0 ||
       strcmp (node->as.call.name, "CHOOSEROWS") == 0) && n_args >= 2)
    {
      gboolean by_col = node->as.call.name[6] == 'C';
      O42Operand src = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      int rows, cols;
      GArray *wanted = g_array_new (FALSE, FALSE, sizeof (int));
      ArrayConst *a;

      operand_dims (&src, &rows, &cols);
      for (int i = 1; i < n_args; i++)
        {
          O42Operand pick = eval_operand (ctx, g_ptr_array_index (node->as.call.args, i));
          int pick_rows, pick_cols;

          operand_dims (&pick, &pick_rows, &pick_cols);
          for (int r = 0; r < pick_rows; r++)
            for (int c = 0; c < pick_cols; c++)
              {
                O42Value v = operand_cell (ctx, &pick, r, c);
                double number = 0;
                O42ErrorCode e = O42_ERR_VALUE;

                if (o42_value_to_number (&v, &number, &e))
                  {
                    int which = (int) number;
                    int limit = by_col ? cols : rows;

                    if (which < 0)
                      which = limit + which + 1;
                    if (which >= 1 && which <= limit)
                      { which--; g_array_append_val (wanted, which); }
                  }
                o42_value_clear (&v);
              }
          operand_clear (&pick);
        }

      if (wanted->len == 0)
        out->value = o42_value_error (O42_ERR_VALUE);
      else
        {
          a = by_col ? array_const_new (rows, (int) wanted->len)
                     : array_const_new ((int) wanted->len, cols);
          for (guint k = 0; k < wanted->len; k++)
            {
              int which = g_array_index (wanted, int, k);

              if (by_col)
                for (int r = 0; r < rows; r++)
                  a->cells[r * wanted->len + k] = operand_cell (ctx, &src, r, which);
              else
                for (int c = 0; c < cols; c++)
                  a->cells[k * cols + c] = operand_cell (ctx, &src, which, c);
            }
          *out = array_operand (a);
        }
      g_array_free (wanted, TRUE);
      operand_clear (&src);
      return TRUE;
    }

  /* TAKE and DROP: so many rows and columns from one end or the other. */
  if ((strcmp (node->as.call.name, "TAKE") == 0 ||
       strcmp (node->as.call.name, "DROP") == 0) && n_args >= 2 && n_args <= 3)
    {
      gboolean taking = node->as.call.name[0] == 'T';
      O42Operand src = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      int rows, cols;
      double want_rows = 0, want_cols = 0;
      gboolean has_rows = FALSE, has_cols = FALSE;
      int row0, row1, col0, col1;
      ArrayConst *a;

      operand_dims (&src, &rows, &cols);
      {
        O42Value v = o42_eval (ctx, g_ptr_array_index (node->as.call.args, 1));
        O42ErrorCode e = O42_ERR_VALUE;

        has_rows = v.type != O42_VALUE_EMPTY && o42_value_to_number (&v, &want_rows, &e);
        o42_value_clear (&v);
      }
      if (n_args >= 3)
        {
          O42Value v = o42_eval (ctx, g_ptr_array_index (node->as.call.args, 2));
          O42ErrorCode e = O42_ERR_VALUE;

          has_cols = v.type != O42_VALUE_EMPTY && o42_value_to_number (&v, &want_cols, &e);
          o42_value_clear (&v);
        }

      row0 = 0; row1 = rows - 1; col0 = 0; col1 = cols - 1;
      if (has_rows)
        {
          int k = (int) fabs (want_rows);

          k = MIN (k, rows);
          if (taking)
            { if (want_rows >= 0) row1 = k - 1; else row0 = rows - k; }
          else
            { if (want_rows >= 0) row0 = k; else row1 = rows - 1 - k; }
        }
      if (has_cols)
        {
          int k = (int) fabs (want_cols);

          k = MIN (k, cols);
          if (taking)
            { if (want_cols >= 0) col1 = k - 1; else col0 = cols - k; }
          else
            { if (want_cols >= 0) col0 = k; else col1 = cols - 1 - k; }
        }

      if (row1 < row0 || col1 < col0)
        out->value = o42_value_error (O42_ERR_VALUE);
      else
        {
          a = array_const_new (row1 - row0 + 1, col1 - col0 + 1);
          for (int r = row0; r <= row1; r++)
            for (int c = col0; c <= col1; c++)
              a->cells[(r - row0) * (col1 - col0 + 1) + (c - col0)] =
                operand_cell (ctx, &src, r, c);
          *out = array_operand (a);
        }
      operand_clear (&src);
      return TRUE;
    }

  /* TEXTSPLIT: a text cut into a rectangle at its delimiters. */
  if (strcmp (node->as.call.name, "TEXTSPLIT") == 0 && n_args >= 2 && n_args <= 3)
    {
      O42Value text_value = o42_eval (ctx, g_ptr_array_index (node->as.call.args, 0));
      O42Value across_value = o42_eval (ctx, g_ptr_array_index (node->as.call.args, 1));
      O42Value down_value = n_args >= 3
                            ? o42_eval (ctx, g_ptr_array_index (node->as.call.args, 2))
                            : o42_value_empty ();
      char *text = o42_value_display (&text_value);
      char *across = o42_value_display (&across_value);
      char *down = down_value.type != O42_VALUE_EMPTY ? o42_value_display (&down_value) : NULL;
      char **lines;
      int n_lines, widest = 0;
      ArrayConst *a;

      o42_value_clear (&text_value);
      o42_value_clear (&across_value);
      o42_value_clear (&down_value);

      lines = (down != NULL && *down != '\0') ? g_strsplit (text, down, -1)
                                              : g_strsplit (text, "\n", -1);
      n_lines = (int) g_strv_length (lines);
      {
        char ***cut = g_new0 (char **, n_lines);

        for (int i = 0; i < n_lines; i++)
          {
            cut[i] = (*across != '\0') ? g_strsplit (lines[i], across, -1)
                                       : g_strsplit (lines[i], "\t", -1);
            widest = MAX (widest, (int) g_strv_length (cut[i]));
          }
        a = array_const_new (MAX (n_lines, 1), MAX (widest, 1));
        for (int i = 0; i < n_lines; i++)
          for (int j = 0; j < widest; j++)
            a->cells[i * MAX (widest, 1) + j] =
              (cut[i][j] != NULL) ? o42_value_text (cut[i][j]) : o42_value_error (O42_ERR_NA);
        for (int i = 0; i < n_lines; i++)
          g_strfreev (cut[i]);
        g_free (cut);
      }
      g_strfreev (lines);
      g_free (text);
      g_free (across);
      g_free (down);
      *out = array_operand (a);
      return TRUE;
    }

  /* MODE.MULT: every value that turns up as often as the commonest. */
  if (strcmp (node->as.call.name, "MODE.MULT") == 0 && n_args >= 1)
    {
      GArray *values = g_array_new (FALSE, FALSE, sizeof (double));
      GArray *modes = g_array_new (FALSE, FALSE, sizeof (double));
      guint best = 1;

      for (int i = 0; i < n_args; i++)
        {
          O42Operand src = eval_operand (ctx, g_ptr_array_index (node->as.call.args, i));
          int rows, cols;

          operand_dims (&src, &rows, &cols);
          for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++)
              {
                O42Value v = operand_cell (ctx, &src, r, c);

                if (v.type == O42_VALUE_NUMBER)
                  g_array_append_val (values, v.as.number);
                o42_value_clear (&v);
              }
          operand_clear (&src);
        }

      for (guint i = 0; i < values->len; i++)
        {
          double x = g_array_index (values, double, i);
          guint count = 0;

          for (guint j = 0; j < values->len; j++)
            if (g_array_index (values, double, j) == x)
              count++;
          if (count > best)
            {
              best = count;
              g_array_set_size (modes, 0);
            }
          if (count == best)
            {
              gboolean seen = FALSE;

              for (guint k = 0; k < modes->len; k++)
                if (g_array_index (modes, double, k) == x)
                  seen = TRUE;
              if (!seen)
                g_array_append_val (modes, x);
            }
        }

      if (best < 2 || modes->len == 0)
        out->value = o42_value_error (O42_ERR_NA);
      else
        {
          ArrayConst *a = array_const_new ((int) modes->len, 1);

          for (guint k = 0; k < modes->len; k++)
            a->cells[k] = o42_value_number (g_array_index (modes, double, k));
          *out = array_operand (a);
        }
      g_array_free (values, TRUE);
      g_array_free (modes, TRUE);
      return TRUE;
    }

  if (strcmp (node->as.call.name, "TRANSPOSE") == 0 && n_args == 1)
    {
      O42Operand src = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      int rows, cols;
      ArrayConst *a;
      operand_dims (&src, &rows, &cols);
      a = array_const_new (cols, rows);
      for (int i = 0; i < rows; i++)
        for (int j = 0; j < cols; j++)
          a->cells[j * rows + i] = operand_cell (ctx, &src, i, j);
      operand_clear (&src);
      *out = array_operand (a);
      return TRUE;
    }

  if (strcmp (node->as.call.name, "MMULT") == 0 && n_args == 2)
    {
      O42Operand x = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      O42Operand y = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 1));
      int rx, cx, ry, cy;
      ArrayConst *a;
      operand_dims (&x, &rx, &cx);
      operand_dims (&y, &ry, &cy);
      if (cx != ry)
        { operand_clear (&x); operand_clear (&y); out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
      a = array_const_new (rx, cy);
      for (int i = 0; i < rx; i++)
        for (int j = 0; j < cy; j++)
          {
            double sum = 0;
            gboolean bad = FALSE;
            for (int k = 0; k < cx && !bad; k++)
              {
                O42Value p = operand_cell (ctx, &x, i, k), q = operand_cell (ctx, &y, k, j);
                double u, v;
                O42ErrorCode e = O42_ERR_VALUE;
                if (o42_value_to_number (&p, &u, &e) && o42_value_to_number (&q, &v, &e))
                  sum += u * v;
                else
                  bad = TRUE;
                o42_value_clear (&p);
                o42_value_clear (&q);
              }
            a->cells[i * cy + j] = bad ? o42_value_error (O42_ERR_VALUE) : o42_value_number (sum);
          }
      operand_clear (&x);
      operand_clear (&y);
      *out = array_operand (a);
      return TRUE;
    }

  /* FOURIER, Gnumeric's: the transform of a sequence, as one column of
   * complex numbers or two columns of real and imaginary parts. */
  if (strcmp (node->as.call.name, "FOURIER") == 0 && n_args >= 1 && n_args <= 3)
    {
      O42Operand src = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      gboolean inverse = FALSE, separate = FALSE;
      double *values = NULL, *values_im = NULL, *re = NULL, *im = NULL;
      int n = 0;
      ArrayConst *a;

      if (n_args >= 2) eval_bool_arg (ctx, node, 1, &inverse);
      if (n_args >= 3) eval_bool_arg (ctx, node, 2, &separate);
      /* The sequence may be complex -- written as text, the way every
       * other IM function writes one -- which is what the inverse
       * transform of a transform is. */
      if (!series_complex_vector (ctx, &src, &values, &values_im, &n))
        {
          operand_clear (&src);
          out->value = o42_value_error (O42_ERR_VALUE);
          return TRUE;
        }
      operand_clear (&src);

      re = g_new0 (double, n);
      im = g_new0 (double, n);
      series_dft (values, values_im, n, inverse, re, im);
      g_free (values);
      g_free (values_im);

      a = array_const_new (n, separate ? 2 : 1);
      for (int k = 0; k < n; k++)
        {
          if (separate)
            {
              a->cells[k * 2] = o42_value_number (re[k]);
              a->cells[k * 2 + 1] = o42_value_number (im[k]);
            }
          else
            a->cells[k] = o42_value_take (complex_format (re[k], im[k], 'i'));
        }
      g_free (re);
      g_free (im);
      *out = array_operand (a);
      return TRUE;
    }

  /* HPFILTER, Gnumeric's: Hodrick and Prescott's filter, which splits a
   * series into the trend that a smoothness penalty allows and what is
   * left over.  The trend solves (I + lambda K'K) t = y with K the
   * second difference, a symmetric band of five, so it is solved by a
   * banded Cholesky and not by inverting anything. */
  if (strcmp (node->as.call.name, "HPFILTER") == 0 && n_args >= 1 && n_args <= 2)
    {
      O42Operand src = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      double lambda = 1600;
      double *y = NULL, *d0 = NULL, *d1 = NULL, *d2 = NULL, *t = NULL;
      int n = 0;
      ArrayConst *a;

      if (n_args >= 2)
        {
          O42Value v = o42_eval (ctx, g_ptr_array_index (node->as.call.args, 1));
          O42ErrorCode e = O42_ERR_VALUE;

          if (v.type != O42_VALUE_EMPTY && !o42_value_to_number (&v, &lambda, &e))
            {
              o42_value_clear (&v);
              operand_clear (&src);
              out->value = o42_value_error (O42_ERR_VALUE);
              return TRUE;
            }
          o42_value_clear (&v);
        }
      if (!series_vector (ctx, &src, &y, &n) || n < 6 || lambda < 0)
        {
          operand_clear (&src);
          g_free (y);
          out->value = o42_value_error (n > 0 ? O42_ERR_NUM : O42_ERR_VALUE);
          return TRUE;
        }
      operand_clear (&src);

      /* The three bands of I + lambda K'K, built row by row from the
       * second differences (1, -2, 1). */
      d0 = g_new0 (double, n);
      d1 = g_new0 (double, n);
      d2 = g_new0 (double, n);
      for (int i = 0; i < n; i++)
        d0[i] = 1;
      for (int j = 0; j + 2 < n; j++)
        {
          const int at[3] = { j, j + 1, j + 2 };
          const double k[3] = { 1, -2, 1 };

          for (int p = 0; p < 3; p++)
            for (int q = 0; q < 3; q++)
              {
                int r = at[p], c = at[q];

                if (c == r)          d0[r] += lambda * k[p] * k[q];
                else if (c == r + 1) d1[r] += lambda * k[p] * k[q];
                else if (c == r + 2) d2[r] += lambda * k[p] * k[q];
              }
        }

      /* Cholesky of a band of two: L L' with the same bands. */
      {
        double *l0 = g_new0 (double, n), *l1 = g_new0 (double, n), *l2 = g_new0 (double, n);
        gboolean bad = FALSE;

        for (int i = 0; i < n && !bad; i++)
          {
            double sum = d0[i];

            if (i >= 1) sum -= l1[i - 1] * l1[i - 1];
            if (i >= 2) sum -= l2[i - 2] * l2[i - 2];
            if (sum <= 0)
              { bad = TRUE; break; }
            l0[i] = sqrt (sum);
            if (i + 1 < n)
              {
                double s = d1[i];

                if (i >= 1) s -= l2[i - 1] * l1[i - 1];
                l1[i] = s / l0[i];
              }
            if (i + 2 < n)
              l2[i] = d2[i] / l0[i];
          }

        t = g_new0 (double, n);
        if (!bad)
          {
            /* Forward, then back. */
            for (int i = 0; i < n; i++)
              {
                double sum = y[i];

                if (i >= 1) sum -= l1[i - 1] * t[i - 1];
                if (i >= 2) sum -= l2[i - 2] * t[i - 2];
                t[i] = sum / l0[i];
              }
            for (int i = n - 1; i >= 0; i--)
              {
                double sum = t[i];

                if (i + 1 < n) sum -= l1[i] * t[i + 1];
                if (i + 2 < n) sum -= l2[i] * t[i + 2];
                t[i] = sum / l0[i];
              }
          }
        g_free (l0); g_free (l1); g_free (l2);
        g_free (d0); g_free (d1); g_free (d2);
        if (bad)
          {
            g_free (y); g_free (t);
            out->value = o42_value_error (O42_ERR_NUM);
            return TRUE;
          }
      }

      a = array_const_new (n, 2);
      for (int i = 0; i < n; i++)
        {
          a->cells[i * 2] = o42_value_number (t[i]);
          a->cells[i * 2 + 1] = o42_value_number (y[i] - t[i]);
        }
      g_free (y);
      g_free (t);
      *out = array_operand (a);
      return TRUE;
    }

  /* INTERPOLATION, Gnumeric's: the ordinates read off at the targets,
   * by a line, a staircase or a natural cubic spline -- and, in the
   * odd-numbered methods, averaged over each interval between targets,
   * which is one answer fewer than there are targets. */
  if (strcmp (node->as.call.name, "INTERPOLATION") == 0 && n_args >= 3 && n_args <= 4)
    {
      O42Operand xs = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      O42Operand ys = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 1));
      O42Operand ts = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 2));
      double *x = NULL, *y = NULL, *targets = NULL, *m2 = NULL;
      int nx = 0, ny = 0, nt = 0, method = 0, answers;
      gboolean averaging;
      ArrayConst *a;

      if (n_args >= 4)
        {
          O42Value v = o42_eval (ctx, g_ptr_array_index (node->as.call.args, 3));
          double number = 0;
          O42ErrorCode e = O42_ERR_VALUE;

          if (v.type != O42_VALUE_EMPTY && o42_value_to_number (&v, &number, &e))
            method = (int) number;
          o42_value_clear (&v);
        }
      if (!series_vector (ctx, &xs, &x, &nx) ||
          !series_vector (ctx, &ys, &y, &ny) ||
          !series_vector (ctx, &ts, &targets, &nt) ||
          nx != ny || nx < 2 || method < 0 || method > 5)
        {
          operand_clear (&xs); operand_clear (&ys); operand_clear (&ts);
          g_free (x); g_free (y); g_free (targets);
          out->value = o42_value_error (O42_ERR_VALUE);
          return TRUE;
        }
      operand_clear (&xs); operand_clear (&ys); operand_clear (&ts);

      averaging = (method % 2) == 1;
      answers = averaging ? nt - 1 : nt;
      if (answers < 1)
        {
          g_free (x); g_free (y); g_free (targets);
          out->value = o42_value_error (O42_ERR_VALUE);
          return TRUE;
        }
      if (method >= 4)
        m2 = spline_second_derivatives (x, y, nx);

      a = array_const_new (answers, 1);
      for (int i = 0; i < answers; i++)
        a->cells[i] = o42_value_number (
          averaging ? series_interpolate_mean (x, y, m2, nx, method, targets[i], targets[i + 1])
                    : series_interpolate_at (x, y, m2, nx, method, targets[i]));
      g_free (x); g_free (y); g_free (targets); g_free (m2);
      *out = array_operand (a);
      return TRUE;
    }

  /* PERIODOGRAM, Gnumeric's: how much of each frequency is in a series.
   * The data may be put through one of four windows first, and may be
   * given with abscissae of its own, in which case it is interpolated
   * onto a regular grid before it is transformed. */
  if (strcmp (node->as.call.name, "PERIODOGRAM") == 0 && n_args >= 1 && n_args <= 5)
    {
      O42Operand ys = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      double *y = NULL, *x = NULL, *re = NULL, *im = NULL, *work = NULL;
      int n = 0, nx = 0, filter = 0, method = 0, number = 0;
      ArrayConst *a;

      if (!series_vector (ctx, &ys, &y, &n))
        {
          operand_clear (&ys);
          out->value = o42_value_error (O42_ERR_VALUE);
          return TRUE;
        }
      operand_clear (&ys);

      if (n_args >= 2)
        {
          O42Value v = o42_eval (ctx, g_ptr_array_index (node->as.call.args, 1));
          double number_arg = 0;
          O42ErrorCode e = O42_ERR_VALUE;

          if (v.type != O42_VALUE_EMPTY && o42_value_to_number (&v, &number_arg, &e))
            filter = (int) number_arg;
          o42_value_clear (&v);
        }
      if (n_args >= 4)
        {
          O42Value v = o42_eval (ctx, g_ptr_array_index (node->as.call.args, 3));
          double number_arg = 0;
          O42ErrorCode e = O42_ERR_VALUE;

          if (v.type != O42_VALUE_EMPTY && o42_value_to_number (&v, &number_arg, &e))
            method = (int) number_arg;
          o42_value_clear (&v);
        }
      if (n_args >= 5)
        {
          O42Value v = o42_eval (ctx, g_ptr_array_index (node->as.call.args, 4));
          double number_arg = 0;
          O42ErrorCode e = O42_ERR_VALUE;

          if (v.type != O42_VALUE_EMPTY && o42_value_to_number (&v, &number_arg, &e))
            number = (int) number_arg;
          o42_value_clear (&v);
        }
      if (filter < 0 || filter > 3 || method < 0 || method > 5)
        {
          g_free (y);
          out->value = o42_value_error (O42_ERR_VALUE);
          return TRUE;
        }

      /* With abscissae, the series is read off a regular grid first. */
      if (n_args >= 3)
        {
          O42Operand xs = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 2));

          if (xs.is_range || xs.value.type != O42_VALUE_EMPTY)
            {
              if (!series_vector (ctx, &xs, &x, &nx) || nx != n)
                {
                  operand_clear (&xs);
                  g_free (x); g_free (y);
                  out->value = o42_value_error (O42_ERR_VALUE);
                  return TRUE;
                }
            }
          operand_clear (&xs);
        }
      if (x != NULL)
        {
          double *m2 = method >= 4 ? spline_second_derivatives (x, y, n) : NULL;
          int wanted = number > 1 ? number : n;
          double step = (x[n - 1] - x[0]) / MAX (wanted - 1, 1);

          work = g_new0 (double, wanted);
          for (int i = 0; i < wanted; i++)
            work[i] = series_interpolate_at (x, y, m2, n, method, x[0] + i * step);
          g_free (m2);
          g_free (y);
          y = work;
          n = wanted;
        }

      for (int j = 0; j < n; j++)
        y[j] *= series_window (filter, j, n);

      re = g_new0 (double, n);
      im = g_new0 (double, n);
      series_dft (y, NULL, n, FALSE, re, im);

      a = array_const_new (n, 1);
      for (int k = 0; k < n; k++)
        a->cells[k] = o42_value_number ((re[k] * re[k] + im[k] * im[k]) / n);
      g_free (x); g_free (y); g_free (re); g_free (im);
      *out = array_operand (a);
      return TRUE;
    }

  /* LOGFIT, Gnumeric's: the least-squares fit of y = a + b ln(sign (x -
   * c)), answering with one row of five -- sign, a, b, c and the sum of
   * the squared residuals.  With c and the sign settled the fit is an
   * ordinary line in ln(sign (x - c)), so what is searched for is c:
   * the offset must keep every sign (x - c) above zero, which puts c
   * below the smallest x when the sign is +1 and above the largest when
   * it is -1.  The search is a scan of the distance from that end, on a
   * logarithmic scale, and then a golden section between the two points
   * either side of the best. */
  if (strcmp (node->as.call.name, "LOGFIT") == 0 && n_args == 2)
    {
      O42Operand ys = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      O42Operand xs = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 1));
      int yr, yc, xr, xc, n;
      double *x = NULL, *y = NULL;
      double best_ssr = 0, best_a = 0, best_b = 0, best_c = 0, best_sign = 0;
      gboolean bad = FALSE, any = FALSE;
      ArrayConst *a;

      operand_dims (&ys, &yr, &yc);
      operand_dims (&xs, &xr, &xc);
      n = yr * yc;
      if (n < 3 || xr * xc != n)
        {
          operand_clear (&ys); operand_clear (&xs);
          out->value = o42_value_error (O42_ERR_VALUE);
          return TRUE;
        }

      x = g_new0 (double, n);
      y = g_new0 (double, n);
      for (int i = 0; i < n && !bad; i++)
        {
          O42Value vy = operand_cell (ctx, &ys, yr == 1 ? 0 : i, yr == 1 ? i : 0);
          O42Value vx = operand_cell (ctx, &xs, xr == 1 ? 0 : i, xr == 1 ? i : 0);
          O42ErrorCode e = O42_ERR_VALUE;

          if (!o42_value_to_number (&vy, &y[i], &e) ||
              !o42_value_to_number (&vx, &x[i], &e))
            bad = TRUE;
          o42_value_clear (&vy);
          o42_value_clear (&vx);
        }
      operand_clear (&ys);
      operand_clear (&xs);
      if (bad)
        {
          g_free (x); g_free (y);
          out->value = o42_value_error (O42_ERR_VALUE);
          return TRUE;
        }

      {
        double lo = x[0], hi = x[0], span;

        for (int i = 1; i < n; i++)
          { lo = MIN (lo, x[i]); hi = MAX (hi, x[i]); }
        span = hi - lo;
        if (span <= 0)
          {
            g_free (x); g_free (y);
            out->value = o42_value_error (O42_ERR_NUM);
            return TRUE;
          }

        for (int which = 0; which < 2; which++)
          {
            double sign = which == 0 ? 1.0 : -1.0;
            double edge = which == 0 ? lo : hi;
            double t_best = 0, ssr_best = 0;
            gboolean got = FALSE;

            /* The distance from the edge, as span * e^t. */
            for (int step = -60; step <= 40; step++)
              {
                double t = step / 2.0;
                double c = which == 0 ? edge - span * exp (t) : edge + span * exp (t);
                double aa, bb, ssr;

                if (!logfit_line (x, y, n, sign, c, &aa, &bb, &ssr))
                  continue;
                if (!got || ssr < ssr_best)
                  { got = TRUE; ssr_best = ssr; t_best = t; }
              }
            if (!got)
              continue;

            /* Golden section between the neighbours of the best step. */
            {
              double left = t_best - 0.5, right = t_best + 0.5;
              const double phi = 0.6180339887498949;

              for (int it = 0; it < 200; it++)
                {
                  double t1 = right - phi * (right - left);
                  double t2 = left + phi * (right - left);
                  double c1 = which == 0 ? edge - span * exp (t1) : edge + span * exp (t1);
                  double c2 = which == 0 ? edge - span * exp (t2) : edge + span * exp (t2);
                  double a1, b1, s1, a2, b2, s2;
                  gboolean ok1 = logfit_line (x, y, n, sign, c1, &a1, &b1, &s1);
                  gboolean ok2 = logfit_line (x, y, n, sign, c2, &a2, &b2, &s2);

                  if (!ok1 && !ok2)
                    break;
                  if (!ok2 || (ok1 && s1 < s2))
                    right = t2;
                  else
                    left = t1;
                }
              t_best = (left + right) / 2;
            }

            {
              double c = which == 0 ? edge - span * exp (t_best) : edge + span * exp (t_best);
              double aa, bb, ssr;

              if (logfit_line (x, y, n, sign, c, &aa, &bb, &ssr) &&
                  (!any || ssr < best_ssr))
                {
                  any = TRUE;
                  best_ssr = ssr; best_a = aa; best_b = bb;
                  best_c = c; best_sign = sign;
                }
            }
          }
      }
      g_free (x);
      g_free (y);
      if (!any)
        { out->value = o42_value_error (O42_ERR_NUM); return TRUE; }

      a = array_const_new (1, 5);
      a->cells[0] = o42_value_number (best_sign);
      a->cells[1] = o42_value_number (best_a);
      a->cells[2] = o42_value_number (best_b);
      a->cells[3] = o42_value_number (best_c);
      a->cells[4] = o42_value_number (best_ssr);
      *out = array_operand (a);
      return TRUE;
    }

  /* LEVERAGE, Gnumeric's: the diagonal of A (A'A)^-1 A', as a column.
   * It says how much each row of a design matrix pulls on the fit. */
  if (strcmp (node->as.call.name, "LEVERAGE") == 0 && n_args == 1)
    {
      O42Operand src = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      int rows, cols;
      double *A = NULL, *M = NULL, *inv = NULL;
      gboolean bad = FALSE, singular = FALSE;
      ArrayConst *a;

      operand_dims (&src, &rows, &cols);
      if (rows < 1 || cols < 1 || cols > rows)
        {
          operand_clear (&src);
          out->value = o42_value_error (O42_ERR_VALUE);
          return TRUE;
        }

      A = g_new0 (double, (gsize) rows * cols);
      for (int r = 0; r < rows && !bad; r++)
        for (int c = 0; c < cols && !bad; c++)
          {
            O42Value v = operand_cell (ctx, &src, r, c);
            O42ErrorCode e = O42_ERR_VALUE;

            if (!o42_value_to_number (&v, &A[r * cols + c], &e))
              bad = TRUE;
            o42_value_clear (&v);
          }
      operand_clear (&src);
      if (bad)
        {
          g_free (A);
          out->value = o42_value_error (O42_ERR_VALUE);
          return TRUE;
        }

      /* M = A'A, inverted in place beside the identity. */
      M = g_new0 (double, (gsize) cols * cols);
      inv = g_new0 (double, (gsize) cols * cols);
      for (int i = 0; i < cols; i++)
        {
          inv[i * cols + i] = 1;
          for (int j = 0; j < cols; j++)
            for (int r = 0; r < rows; r++)
              M[i * cols + j] += A[r * cols + i] * A[r * cols + j];
        }
      for (int col = 0; col < cols && !singular; col++)
        {
          int pivot = col;

          for (int r = col + 1; r < cols; r++)
            if (fabs (M[r * cols + col]) > fabs (M[pivot * cols + col]))
              pivot = r;
          if (fabs (M[pivot * cols + col]) < 1e-12)
            { singular = TRUE; break; }
          if (pivot != col)
            for (int j = 0; j < cols; j++)
              {
                double t = M[col * cols + j];
                M[col * cols + j] = M[pivot * cols + j];
                M[pivot * cols + j] = t;
                t = inv[col * cols + j];
                inv[col * cols + j] = inv[pivot * cols + j];
                inv[pivot * cols + j] = t;
              }
          {
            double d = M[col * cols + col];

            for (int j = 0; j < cols; j++)
              { M[col * cols + j] /= d; inv[col * cols + j] /= d; }
          }
          for (int r = 0; r < cols; r++)
            if (r != col && M[r * cols + col] != 0)
              {
                double m = M[r * cols + col];

                for (int j = 0; j < cols; j++)
                  {
                    M[r * cols + j] -= m * M[col * cols + j];
                    inv[r * cols + j] -= m * inv[col * cols + j];
                  }
              }
        }
      if (singular)
        {
          g_free (A); g_free (M); g_free (inv);
          out->value = o42_value_error (O42_ERR_NUM);
          return TRUE;
        }

      a = array_const_new (rows, 1);
      for (int r = 0; r < rows; r++)
        {
          double h = 0;

          for (int i = 0; i < cols; i++)
            for (int j = 0; j < cols; j++)
              h += A[r * cols + i] * inv[i * cols + j] * A[r * cols + j];
          a->cells[r] = o42_value_number (h);
        }
      g_free (A); g_free (M); g_free (inv);
      *out = array_operand (a);
      return TRUE;
    }

  /* Gnumeric's three other matrix functions.  CHOLESKY factors a
   * symmetric positive-definite matrix into a lower triangle times its
   * own transpose; MPSEUDOINVERSE is Moore and Penrose's inverse, which
   * a matrix has whether it is square or not; EIGEN gives the
   * eigenvalues of a symmetric matrix and the vectors that go with
   * them. */
  if ((strcmp (node->as.call.name, "CHOLESKY") == 0 ||
       strcmp (node->as.call.name, "MPSEUDOINVERSE") == 0 ||
       strcmp (node->as.call.name, "EIGEN") == 0) && n_args >= 1)
    {
      O42Operand src = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      int rows, cols;
      double *m;
      ArrayConst *a;
      gboolean bad = FALSE;

      operand_dims (&src, &rows, &cols);
      if (rows < 1 || cols < 1)
        { operand_clear (&src); out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }

      m = g_new0 (double, (gsize) rows * cols);
      for (int i = 0; i < rows; i++)
        for (int j = 0; j < cols; j++)
          {
            O42Value v = operand_cell (ctx, &src, i, j);
            O42ErrorCode e = O42_ERR_VALUE;

            if (!o42_value_to_number (&v, &m[i * cols + j], &e))
              bad = TRUE;
            o42_value_clear (&v);
          }
      operand_clear (&src);
      if (bad)
        { g_free (m); out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }

      if (strcmp (node->as.call.name, "CHOLESKY") == 0)
        {
          double *l;

          if (rows != cols)
            { g_free (m); out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
          l = g_new0 (double, (gsize) rows * rows);
          for (int i = 0; i < rows && !bad; i++)
            for (int j = 0; j <= i; j++)
              {
                double sum = m[i * rows + j];

                for (int k = 0; k < j; k++)
                  sum -= l[i * rows + k] * l[j * rows + k];
                if (i == j)
                  {
                    if (sum <= 0) { bad = TRUE; break; }
                    l[i * rows + j] = sqrt (sum);
                  }
                else
                  l[i * rows + j] = sum / l[j * rows + j];
              }
          if (bad)
            { g_free (l); g_free (m); out->value = o42_value_error (O42_ERR_NUM); return TRUE; }
          a = array_const_new (rows, rows);
          for (int i = 0; i < rows; i++)
            for (int j = 0; j < rows; j++)
              a->cells[i * rows + j] = o42_value_number (l[i * rows + j]);
          g_free (l);
          g_free (m);
          *out = array_operand (a);
          return TRUE;
        }

      if (strcmp (node->as.call.name, "EIGEN") == 0)
        {
          /* Jacobi's rotations: the matrix must be symmetric, and each
           * rotation kills the largest off-diagonal pair until none is
           * left worth killing. */
          double *v;
          int *order;

          if (rows != cols)
            { g_free (m); out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
          for (int i = 0; i < rows; i++)
            for (int j = 0; j < i; j++)
              if (fabs (m[i * rows + j] - m[j * rows + i]) > 1e-9 * (1 + fabs (m[i * rows + j])))
                bad = TRUE;
          if (bad)
            { g_free (m); out->value = o42_value_error (O42_ERR_NUM); return TRUE; }

          v = g_new0 (double, (gsize) rows * rows);
          for (int i = 0; i < rows; i++)
            v[i * rows + i] = 1;

          for (int sweep = 0; sweep < 100; sweep++)
            {
              double off = 0;

              for (int i = 0; i < rows; i++)
                for (int j = i + 1; j < rows; j++)
                  off += m[i * rows + j] * m[i * rows + j];
              if (off < 1e-30)
                break;
              for (int p = 0; p < rows - 1; p++)
                for (int q = p + 1; q < rows; q++)
                  {
                    double apq = m[p * rows + q];
                    double theta, t, c, s;

                    if (fabs (apq) < 1e-300)
                      continue;
                    theta = (m[q * rows + q] - m[p * rows + p]) / (2 * apq);
                    t = (theta >= 0 ? 1 : -1) / (fabs (theta) + sqrt (theta * theta + 1));
                    c = 1 / sqrt (t * t + 1);
                    s = t * c;
                    for (int k = 0; k < rows; k++)
                      {
                        double akp = m[k * rows + p], akq = m[k * rows + q];

                        m[k * rows + p] = c * akp - s * akq;
                        m[k * rows + q] = s * akp + c * akq;
                      }
                    for (int k = 0; k < rows; k++)
                      {
                        double apk = m[p * rows + k], aqk = m[q * rows + k];

                        m[p * rows + k] = c * apk - s * aqk;
                        m[q * rows + k] = s * apk + c * aqk;
                      }
                    for (int k = 0; k < rows; k++)
                      {
                        double vkp = v[k * rows + p], vkq = v[k * rows + q];

                        v[k * rows + p] = c * vkp - s * vkq;
                        v[k * rows + q] = s * vkp + c * vkq;
                      }
                  }
            }

          /* Smallest eigenvalue first, which is the order Gnumeric
           * answers in. */
          order = g_new (int, rows);
          for (int i = 0; i < rows; i++)
            order[i] = i;
          for (int i = 0; i < rows; i++)
            for (int j = i + 1; j < rows; j++)
              if (m[order[j] * rows + order[j]] < m[order[i] * rows + order[i]])
                { int t = order[i]; order[i] = order[j]; order[j] = t; }

          /* The eigenvalues on the first row, and under each its
           * vector. */
          a = array_const_new (rows + 1, rows);
          for (int j = 0; j < rows; j++)
            {
              int c = order[j];

              a->cells[j] = o42_value_number (m[c * rows + c]);
              for (int i = 0; i < rows; i++)
                a->cells[(i + 1) * rows + j] = o42_value_number (v[i * rows + c]);
            }
          g_free (order);
          g_free (v);
          g_free (m);
          *out = array_operand (a);
          return TRUE;
        }

      /* MPSEUDOINVERSE, by the normal equations: (A'A)^-1 A' when the
       * columns are independent, and A'(AA')^-1 when the rows are.
       * Gnumeric's takes a tolerance it ignores for a well-conditioned
       * matrix, and so does this. */
      {
        int n = rows >= cols ? cols : rows;   /* the side of the square to invert */
        double *sq = g_new0 (double, (gsize) n * n);
        double *inv = g_new0 (double, (gsize) n * n * 2);
        double *result;
        gboolean singular = FALSE;

        for (int i = 0; i < n; i++)
          for (int j = 0; j < n; j++)
            {
              double sum = 0;

              if (rows >= cols)
                for (int k = 0; k < rows; k++)
                  sum += m[k * cols + i] * m[k * cols + j];      /* A'A */
              else
                for (int k = 0; k < cols; k++)
                  sum += m[i * cols + k] * m[j * cols + k];      /* AA' */
              sq[i * n + j] = sum;
            }

        for (int i = 0; i < n; i++)
          {
            for (int j = 0; j < n; j++)
              inv[i * 2 * n + j] = sq[i * n + j];
            inv[i * 2 * n + n + i] = 1;
          }
        for (int k = 0; k < n && !singular; k++)
          {
            int pivot = k;

            for (int i = k + 1; i < n; i++)
              if (fabs (inv[i * 2 * n + k]) > fabs (inv[pivot * 2 * n + k]))
                pivot = i;
            if (fabs (inv[pivot * 2 * n + k]) < 1e-12)
              { singular = TRUE; break; }
            if (pivot != k)
              for (int j = 0; j < 2 * n; j++)
                { double t = inv[k * 2 * n + j]; inv[k * 2 * n + j] = inv[pivot * 2 * n + j]; inv[pivot * 2 * n + j] = t; }
            {
              double d = inv[k * 2 * n + k];

              for (int j = 0; j < 2 * n; j++)
                inv[k * 2 * n + j] /= d;
            }
            for (int i = 0; i < n; i++)
              if (i != k)
                {
                  double f = inv[i * 2 * n + k];

                  for (int j = 0; j < 2 * n; j++)
                    inv[i * 2 * n + j] -= f * inv[k * 2 * n + j];
                }
          }
        if (singular)
          {
            g_free (sq); g_free (inv); g_free (m);
            out->value = o42_value_error (O42_ERR_NUM);
            return TRUE;
          }

        /* The pseudo-inverse is the other way up from the matrix. */
        result = g_new0 (double, (gsize) cols * rows);
        for (int i = 0; i < cols; i++)
          for (int j = 0; j < rows; j++)
            {
              double sum = 0;

              if (rows >= cols)
                for (int k = 0; k < n; k++)
                  sum += inv[i * 2 * n + n + k] * m[j * cols + k];
              else
                for (int k = 0; k < n; k++)
                  sum += m[k * cols + i] * inv[k * 2 * n + n + j];
              result[i * rows + j] = sum;
            }

        a = array_const_new (cols, rows);
        for (int i = 0; i < cols; i++)
          for (int j = 0; j < rows; j++)
            a->cells[i * rows + j] = o42_value_number (result[i * rows + j]);
        g_free (result); g_free (sq); g_free (inv); g_free (m);
        *out = array_operand (a);
        return TRUE;
      }
    }

  if (strcmp (node->as.call.name, "MINVERSE") == 0 && n_args == 1)
    {
      O42Operand src = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      int rows, cols;
      double *m;
      ArrayConst *a;
      gboolean singular = FALSE;

      operand_dims (&src, &rows, &cols);
      if (rows != cols)
        { operand_clear (&src); out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
      /* Gauss-Jordan on [M | I]. */
      m = g_new0 (double, (gsize) rows * rows * 2);
      for (int i = 0; i < rows; i++)
        {
          for (int j = 0; j < rows; j++)
            {
              O42Value v = operand_cell (ctx, &src, i, j);
              O42ErrorCode e = O42_ERR_VALUE;
              if (!o42_value_to_number (&v, &m[i * 2 * rows + j], &e)) singular = TRUE;
              o42_value_clear (&v);
            }
          m[i * 2 * rows + rows + i] = 1;
        }
      for (int k = 0; k < rows && !singular; k++)
        {
          int pivot = k;
          for (int i = k + 1; i < rows; i++)
            if (fabs (m[i * 2 * rows + k]) > fabs (m[pivot * 2 * rows + k])) pivot = i;
          if (fabs (m[pivot * 2 * rows + k]) < 1e-300) { singular = TRUE; break; }
          if (pivot != k)
            for (int j = 0; j < 2 * rows; j++)
              { double t = m[k * 2 * rows + j]; m[k * 2 * rows + j] = m[pivot * 2 * rows + j]; m[pivot * 2 * rows + j] = t; }
          {
            double d = m[k * 2 * rows + k];
            for (int j = 0; j < 2 * rows; j++) m[k * 2 * rows + j] /= d;
          }
          for (int i = 0; i < rows; i++)
            if (i != k)
              {
                double f = m[i * 2 * rows + k];
                for (int j = 0; j < 2 * rows; j++) m[i * 2 * rows + j] -= f * m[k * 2 * rows + j];
              }
        }
      operand_clear (&src);
      if (singular)
        { g_free (m); out->value = o42_value_error (O42_ERR_NUM); return TRUE; }
      a = array_const_new (rows, rows);
      for (int i = 0; i < rows; i++)
        for (int j = 0; j < rows; j++)
          a->cells[i * rows + j] = o42_value_number (m[i * 2 * rows + rows + j]);
      g_free (m);
      *out = array_operand (a);
      return TRUE;
    }

  if (strcmp (node->as.call.name, "FREQUENCY") == 0 && n_args == 2)
    {
      O42Operand data = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      O42Operand bins = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 1));
      int dr, dc, br, bc, nbins;
      ArrayConst *a;

      operand_dims (&data, &dr, &dc);
      operand_dims (&bins, &br, &bc);
      nbins = br * bc;
      a = array_const_new (nbins + 1, 1);
      for (int i = 0; i <= nbins; i++) a->cells[i] = o42_value_number (0);
      for (int i = 0; i < dr; i++)
        for (int j = 0; j < dc; j++)
          {
            O42Value v = operand_cell (ctx, &data, i, j);
            int slot = nbins;
            if (v.type == O42_VALUE_NUMBER)
              {
                for (int k = 0; k < nbins; k++)
                  {
                    O42Value b = operand_cell (ctx, &bins, k / bc, k % bc);
                    gboolean below = b.type == O42_VALUE_NUMBER && v.as.number <= b.as.number;
                    o42_value_clear (&b);
                    if (below) { slot = k; break; }
                  }
                a->cells[slot].as.number += 1;
              }
            o42_value_clear (&v);
          }
      operand_clear (&data);
      operand_clear (&bins);
      *out = array_operand (a);
      return TRUE;
    }

  if ((strcmp (node->as.call.name, "TREND") == 0 || strcmp (node->as.call.name, "GROWTH") == 0) &&
      n_args >= 1 && n_args <= 4)
    {
      /* A straight line through (x, y), or through (x, ln y) for the
       * exponential fit, then read off at the new xs. */
      gboolean logfit = strcmp (node->as.call.name, "TREND") != 0;
      gboolean logest = strcmp (node->as.call.name, "LOGEST") == 0;
      O42Operand ys = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      O42Operand xs, news;
      gboolean have_x = n_args >= 2, have_new = n_args >= 3 && !logest, bad = FALSE, through_origin = FALSE;
      int rows, cols, n, nrows, ncols;
      double sx = 0, sy = 0, sxx = 0, sxy = 0, slope, intercept;
      ArrayConst *a;

      memset (&xs, 0, sizeof xs);
      memset (&news, 0, sizeof news);
      if (have_x)
        {
          xs = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 1));
          if (!xs.is_range && xs.value.type == O42_VALUE_EMPTY) have_x = FALSE;
        }
      if (have_new)
        {
          news = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 2));
          if (!news.is_range && news.value.type == O42_VALUE_EMPTY) have_new = FALSE;
        }
      if (n_args >= (logest ? 3 : 4))
        {
          O42Value c = eval_node (ctx, g_ptr_array_index (node->as.call.args, logest ? 2 : 3));
          gboolean keep = TRUE;
          O42ErrorCode e = O42_ERR_VALUE;
          if (c.type != O42_VALUE_EMPTY && o42_value_to_bool (&c, &keep, &e))
            through_origin = !keep;
          o42_value_clear (&c);
        }
      operand_dims (&ys, &rows, &cols);
      n = rows * cols;
      for (int k = 0; k < n; k++)
        {
          O42Value vy = operand_cell (ctx, &ys, k / cols, k % cols);
          double x = k + 1, y;
          O42ErrorCode e = O42_ERR_VALUE;
          if (have_x)
            {
              O42Value vx = operand_cell (ctx, &xs, k / cols, k % cols);
              if (!o42_value_to_number (&vx, &x, &e)) bad = TRUE;
              o42_value_clear (&vx);
            }
          if (!o42_value_to_number (&vy, &y, &e)) bad = TRUE;
          o42_value_clear (&vy);
          if (logfit)
            {
              if (y <= 0) bad = TRUE; else y = log (y);
            }
          sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
      if (bad || n < 1 || (!through_origin && (n < 2 || n * sxx - sx * sx == 0)) || (through_origin && sxx == 0))
        {
          operand_clear (&ys); operand_clear (&xs); operand_clear (&news);
          out->value = o42_value_error (bad ? O42_ERR_VALUE : O42_ERR_DIV0);
          return TRUE;
        }
      if (through_origin)
        { slope = sxy / sxx; intercept = 0; }
      else
        {
          slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
          intercept = (sy - slope * sx) / n;
        }
      if (logest)
        {
          a = array_const_new (1, 2);
          a->cells[0] = o42_value_number (exp (slope));
          a->cells[1] = o42_value_number (exp (intercept));
        }
      else
        {
          const O42Operand *at = have_new ? &news : have_x ? &xs : NULL;
          if (at != NULL) operand_dims (at, &nrows, &ncols);
          else { nrows = rows; ncols = cols; }
          a = array_const_new (nrows, ncols);
          for (int i = 0; i < nrows; i++)
            for (int j = 0; j < ncols; j++)
              {
                double x = i * ncols + j + 1, fit;
                if (at != NULL)
                  {
                    O42Value vx = operand_cell (ctx, at, i, j);
                    O42ErrorCode e = O42_ERR_VALUE;
                    if (!o42_value_to_number (&vx, &x, &e))
                      { o42_value_clear (&vx); a->cells[i * ncols + j] = o42_value_error (O42_ERR_VALUE); continue; }
                    o42_value_clear (&vx);
                  }
                fit = intercept + slope * x;
                a->cells[i * ncols + j] = o42_value_number (logfit ? exp (fit) : fit);
              }
        }
      operand_clear (&ys); operand_clear (&xs); operand_clear (&news);
      *out = array_operand (a);
      return TRUE;
    }

  if ((strcmp (node->as.call.name, "LINEST") == 0 ||
       strcmp (node->as.call.name, "LOGEST") == 0 ||
       strcmp (node->as.call.name, "LOGREG") == 0) &&
      n_args >= 1 && n_args <= 4)
    {
      /* Least squares of y on any number of x variables, by the normal
       * equations; with stats, Excel's five rows: coefficients, their
       * standard errors, r2 and the standard error of y, F and the
       * degrees of freedom, and the regression and residual sums of
       * squares.  LOGEST fits ln y and reports e to the coefficients. */
      gboolean logest = strcmp (node->as.call.name, "LOGEST") == 0;
      gboolean logreg = strcmp (node->as.call.name, "LOGREG") == 0;
      O42Operand ys = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 0));
      O42Operand xs;
      gboolean have_x = FALSE, with_const = TRUE, stats = FALSE, bad = FALSE, singular = FALSE;
      int yr, yc, n, p = 1, k, xr = 0, xc = 0;
      gboolean y_is_row;
      double *X = NULL, *y = NULL, *A = NULL, *inv = NULL, *c = NULL, *beta = NULL;
      double ss_res = 0, ss_tot = 0, ss_reg, ybar = 0, se_y = 0, r2, f;
      int df;
      ArrayConst *a;

      memset (&xs, 0, sizeof xs);
      if (n_args >= 2)
        {
          xs = eval_operand (ctx, g_ptr_array_index (node->as.call.args, 1));
          have_x = xs.is_range || xs.value.type != O42_VALUE_EMPTY;
        }
      if (n_args >= 3) eval_bool_arg (ctx, node, 2, &with_const);
      if (n_args >= 4) eval_bool_arg (ctx, node, 3, &stats);

      operand_dims (&ys, &yr, &yc);
      n = yr * yc;
      y_is_row = yr == 1 && yc > 1;
      if (have_x)
        {
          operand_dims (&xs, &xr, &xc);
          if (xr == yr && xc == yc)
            p = 1;
          else if (y_is_row && xc == n)
            p = xr;
          else if (!y_is_row && xr == n)
            p = xc;
          else
            bad = TRUE;
        }
      k = p + (with_const ? 1 : 0);
      if (bad || n < 1 || k > n)
        {
          operand_clear (&ys); operand_clear (&xs);
          out->value = o42_value_error (bad ? O42_ERR_REF : O42_ERR_VALUE);
          return TRUE;
        }

      X = g_new0 (double, (gsize) n * k);
      y = g_new0 (double, n);
      for (int i = 0; i < n && !bad; i++)
        {
          O42Value vy = operand_cell (ctx, &ys, y_is_row ? 0 : i, y_is_row ? i : 0);
          O42ErrorCode e = O42_ERR_VALUE;
          if (!o42_value_to_number (&vy, &y[i], &e)) bad = TRUE;
          o42_value_clear (&vy);
          if (logest && !bad)
            {
              if (y[i] <= 0) bad = TRUE; else y[i] = log (y[i]);
            }
          for (int j = 0; j < p && !bad; j++)
            {
              double x = i + 1;
              if (have_x)
                {
                  O42Value vx;
                  if (xr == yr && xc == yc)
                    vx = operand_cell (ctx, &xs, y_is_row ? 0 : i, y_is_row ? i : 0);
                  else if (y_is_row)
                    vx = operand_cell (ctx, &xs, j, i);
                  else
                    vx = operand_cell (ctx, &xs, i, j);
                  if (!o42_value_to_number (&vx, &x, &e)) bad = TRUE;
                  o42_value_clear (&vx);
                }
              /* LOGREG is Gnumeric's: the same fit after z = ln(x), so
               * the line is y = m ln(x) + b. */
              if (logreg && !bad)
                {
                  if (x <= 0) bad = TRUE; else x = log (x);
                }
              X[i * k + j] = x;
            }
          if (with_const)
            X[i * k + p] = 1;
        }
      operand_clear (&ys);
      operand_clear (&xs);
      if (bad)
        {
          g_free (X); g_free (y);
          out->value = o42_value_error (O42_ERR_VALUE);
          return TRUE;
        }

      /* A = X'X, c = X'y, inv = A^-1 by Gauss-Jordan, beta = inv c. */
      A = g_new0 (double, (gsize) k * k);
      inv = g_new0 (double, (gsize) k * k);
      c = g_new0 (double, k);
      beta = g_new0 (double, k);
      for (int i = 0; i < k; i++)
        {
          inv[i * k + i] = 1;
          for (int j = 0; j < k; j++)
            for (int r = 0; r < n; r++)
              A[i * k + j] += X[r * k + i] * X[r * k + j];
          for (int r = 0; r < n; r++)
            c[i] += X[r * k + i] * y[r];
        }
      for (int col = 0; col < k && !singular; col++)
        {
          int pivot = col;
          for (int r = col + 1; r < k; r++)
            if (fabs (A[r * k + col]) > fabs (A[pivot * k + col])) pivot = r;
          if (fabs (A[pivot * k + col]) < 1e-12)
            { singular = TRUE; break; }
          if (pivot != col)
            for (int j = 0; j < k; j++)
              {
                double t = A[col * k + j]; A[col * k + j] = A[pivot * k + j]; A[pivot * k + j] = t;
                t = inv[col * k + j]; inv[col * k + j] = inv[pivot * k + j]; inv[pivot * k + j] = t;
              }
          {
            double d = A[col * k + col];
            for (int j = 0; j < k; j++) { A[col * k + j] /= d; inv[col * k + j] /= d; }
          }
          for (int r = 0; r < k; r++)
            if (r != col && A[r * k + col] != 0)
              {
                double m = A[r * k + col];
                for (int j = 0; j < k; j++)
                  { A[r * k + j] -= m * A[col * k + j]; inv[r * k + j] -= m * inv[col * k + j]; }
              }
        }
      if (singular)
        {
          g_free (X); g_free (y); g_free (A); g_free (inv); g_free (c); g_free (beta);
          out->value = o42_value_error (O42_ERR_NUM);
          return TRUE;
        }
      for (int i = 0; i < k; i++)
        for (int j = 0; j < k; j++)
          beta[i] += inv[i * k + j] * c[j];

      for (int r = 0; r < n; r++) ybar += y[r];
      ybar /= n;
      for (int r = 0; r < n; r++)
        {
          double fit = 0;
          for (int j = 0; j < k; j++) fit += X[r * k + j] * beta[j];
          ss_res += (y[r] - fit) * (y[r] - fit);
          ss_tot += with_const ? (y[r] - ybar) * (y[r] - ybar) : y[r] * y[r];
        }
      ss_reg = MAX (ss_tot - ss_res, 0);
      df = n - k;
      se_y = df > 0 ? sqrt (ss_res / df) : 0;
      r2 = ss_tot > 0 ? ss_reg / ss_tot : 1;
      f = (df > 0 && ss_res > 0) ? (ss_reg / p) / (ss_res / df) : 0;

      a = array_const_new (stats ? 5 : 1, p + 1);
      for (int j = 0; j < p; j++)
        {
          double m = beta[p - 1 - j];
          a->cells[j] = o42_value_number (logest ? exp (m) : m);
        }
      a->cells[p] = o42_value_number (with_const ? (logest ? exp (beta[p]) : beta[p]) : (logest ? 1 : 0));
      if (stats)
        {
          int w = p + 1;
          for (int j = 0; j < p; j++)
            a->cells[w + j] = df > 0 ? o42_value_number (sqrt (se_y * se_y * inv[(p - 1 - j) * k + (p - 1 - j)]))
                                     : o42_value_error (O42_ERR_NA);
          a->cells[w + p] = (with_const && df > 0) ? o42_value_number (sqrt (se_y * se_y * inv[p * k + p]))
                                                   : o42_value_error (O42_ERR_NA);
          for (int j = 0; j < w; j++)
            {
              a->cells[2 * w + j] = j == 0 ? o42_value_number (r2) : j == 1 ? (df > 0 ? o42_value_number (se_y) : o42_value_error (O42_ERR_NA)) : o42_value_error (O42_ERR_NA);
              a->cells[3 * w + j] = j == 0 ? (df > 0 && ss_res > 0 ? o42_value_number (f) : o42_value_error (O42_ERR_NUM)) : j == 1 ? o42_value_number (df) : o42_value_error (O42_ERR_NA);
              a->cells[4 * w + j] = j == 0 ? o42_value_number (ss_reg) : j == 1 ? o42_value_number (ss_res) : o42_value_error (O42_ERR_NA);
            }
        }
      g_free (X); g_free (y); g_free (A); g_free (inv); g_free (c); g_free (beta);
      *out = array_operand (a);
      return TRUE;
    }

  if (strcmp (node->as.call.name, "INDIRECT") == 0)
    {
      O42Value v;
      O42Node *tree;

      if (n_args < 1 || n_args > 2)
        { out->value = o42_value_error (O42_ERR_VALUE); return TRUE; }
      v = eval_node (ctx, g_ptr_array_index (node->as.call.args, 0));
      if (v.type != O42_VALUE_TEXT)
        { o42_value_clear (&v); out->value = o42_value_error (O42_ERR_REF); return TRUE; }
      tree = o42_formula_parse (v.as.text);
      o42_value_clear (&v);
      if (tree->type == O42_NODE_REF)
        {
          out->is_range = TRUE;
          out->sheet = tree->sheet;
          out->range.row0 = out->range.row1 = tree->as.ref.row;
          out->range.col0 = out->range.col1 = tree->as.ref.col;
        }
      else if (tree->type == O42_NODE_RANGE)
        {
          out->is_range = TRUE;
          out->sheet = tree->sheet;
          out->range = tree->as.range;
          operand_clip_whole (ctx, out, tree);
        }
      else if (tree->type == O42_NODE_NAME && ctx->get_name != NULL)
        {
          const char *sheet = NULL;
          O42Range range;
          if (ctx->get_name (ctx, tree->as.name, &sheet, &range))
            { out->is_range = TRUE; out->sheet = sheet; out->range = range; }
          else
            out->value = o42_value_error (O42_ERR_REF);
        }
      else
        out->value = o42_value_error (O42_ERR_REF);
      o42_node_free (tree);
      return TRUE;
    }

  return FALSE;
}

/* ---- more distributions and tests ---- */

static double
lchoose (double n, double k)
{
  return lgamma (n + 1) - lgamma (k + 1) - lgamma (n - k + 1);
}

static O42Value
fn_fisher (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  if (x <= -1 || x >= 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (0.5 * log ((1 + x) / (1 - x)));
}

static O42Value
fn_fisherinv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double y;
  (void) n;
  ARG_NUMBER (0, y);
  return o42_value_number ((exp (2 * y) - 1) / (exp (2 * y) + 1));
}

static O42Value
fn_loginv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p, mean, sd;
  (void) n;
  ARG_NUMBER (0, p);
  ARG_NUMBER (1, mean);
  ARG_NUMBER (2, sd);
  if (p <= 0 || p >= 1 || sd <= 0) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (exp (mean + sd * normal_inverse (p)));
}

static O42Value
fn_hypgeomdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, draws, successes, population;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, draws);
  ARG_NUMBER (2, successes);
  ARG_NUMBER (3, population);
  x = floor (x); draws = floor (draws); successes = floor (successes); population = floor (population);
  if (x < 0 || x > draws || x > successes || draws - x > population - successes ||
      draws > population || successes > population || population <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (exp (lchoose (successes, x) + lchoose (population - successes, draws - x)
                                - lchoose (population, draws)));
}

static O42Value
fn_negbinomdist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double f, s, p;
  (void) n;
  ARG_NUMBER (0, f);
  ARG_NUMBER (1, s);
  ARG_NUMBER (2, p);
  f = floor (f); s = floor (s);
  if (f < 0 || s < 1 || p <= 0 || p >= 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (exp (lchoose (f + s - 1, s - 1)) * pow (p, s) * pow (1 - p, f));
}

static O42Value
fn_critbinom (O42EvalContext *ctx, O42Operand *args, int n)
{
  double trials, p, alpha, sum = 0;
  (void) n;
  ARG_NUMBER (0, trials);
  ARG_NUMBER (1, p);
  ARG_NUMBER (2, alpha);
  trials = floor (trials);
  if (trials < 0 || p < 0 || p > 1 || alpha <= 0 || alpha >= 1) return o42_value_error (O42_ERR_NUM);
  for (int k = 0; k <= (int) trials; k++)
    {
      sum += exp (lchoose (trials, k)) * pow (p, k) * pow (1 - p, trials - k);
      if (sum >= alpha)
        return o42_value_number (k);
    }
  return o42_value_number (trials);
}

static O42Value
fn_steyx (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *xs, *ys;
  O42ErrorCode err = O42_ERR_VALUE;
  double mx = 0, my = 0, sxx = 0, syy = 0, sxy = 0;
  guint count;
  (void) n;

  if (!collect_pairs (ctx, &args[1], &args[0], &xs, &ys, &err))
    return o42_value_error (err);
  count = xs->len;
  if (count < 3)
    { g_array_free (xs, TRUE); g_array_free (ys, TRUE); return o42_value_error (O42_ERR_DIV0); }
  for (guint i = 0; i < count; i++)
    { mx += g_array_index (xs, double, i); my += g_array_index (ys, double, i); }
  mx /= count; my /= count;
  for (guint i = 0; i < count; i++)
    {
      double dx = g_array_index (xs, double, i) - mx, dy = g_array_index (ys, double, i) - my;
      sxx += dx * dx; syy += dy * dy; sxy += dx * dy;
    }
  g_array_free (xs, TRUE);
  g_array_free (ys, TRUE);
  if (sxx == 0) return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (sqrt ((syy - sxy * sxy / sxx) / (count - 2)));
}

static O42Value
fn_percentrank (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double x, sig = 3, rank = 0, scale;
  guint count;

  ARG_NUMBER (1, x);
  if (n >= 3) ARG_NUMBER (2, sig);
  if (sig < 1) return o42_value_error (O42_ERR_NUM);
  if (!collect_numbers (ctx, args, 1, &values, &err))
    return o42_value_error (err);
  count = values->len;
  g_array_sort (values, compare_doubles);
  if (count == 0 || x < g_array_index (values, double, 0) || x > g_array_index (values, double, count - 1))
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NA); }
  if (count == 1)
    rank = 1;
  else
    for (guint i = 0; i + 1 < count; i++)
      {
        double lo = g_array_index (values, double, i), hi = g_array_index (values, double, i + 1);
        if (x == lo) { rank = (double) i / (count - 1); break; }
        if (x == hi) { rank = (double) (i + 1) / (count - 1); break; }
        if (x > lo && x < hi)
          { rank = (i + (x - lo) / (hi - lo)) / (count - 1); break; }
      }
  g_array_free (values, TRUE);
  scale = pow (10, floor (sig));
  return o42_value_number (floor (rank * scale + 1e-9) / scale);
}

static O42Value
fn_chitest (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *actual, *expected;
  O42ErrorCode err = O42_ERR_VALUE;
  double chi = 0, df;
  int rows, cols;
  (void) n;

  if (!args[0].is_range)
    return o42_value_error (O42_ERR_VALUE);
  if (!collect_pairs (ctx, &args[0], &args[1], &actual, &expected, &err))
    return o42_value_error (err);
  for (guint i = 0; i < actual->len; i++)
    {
      double a = g_array_index (actual, double, i), e = g_array_index (expected, double, i);
      if (e == 0) { g_array_free (actual, TRUE); g_array_free (expected, TRUE); return o42_value_error (O42_ERR_DIV0); }
      chi += (a - e) * (a - e) / e;
    }
  g_array_free (actual, TRUE);
  g_array_free (expected, TRUE);
  rows = args[0].range.row1 - args[0].range.row0 + 1;
  cols = args[0].range.col1 - args[0].range.col0 + 1;
  df = (rows > 1 && cols > 1) ? (double) (rows - 1) * (cols - 1) : MAX (rows, cols) - 1;
  if (df < 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (1 - gamma_p (df / 2, chi / 2));
}

/* Mean, sample variance and count of one argument's numbers. */
static gboolean
sample_stats (O42EvalContext *ctx, O42Operand *arg, double *mean, double *var, guint *count, O42ErrorCode *err)
{
  GArray *values;
  double ssd;
  if (!collect_numbers (ctx, arg, 1, &values, err))
    return FALSE;
  *count = values->len;
  moments (values, mean, &ssd);
  *var = *count > 1 ? ssd / (*count - 1) : 0;
  g_array_free (values, TRUE);
  return TRUE;
}

static O42Value
fn_ftest (O42EvalContext *ctx, O42Operand *args, int n)
{
  double m1, m2, v1, v2, f, p;
  guint n1, n2;
  O42ErrorCode err = O42_ERR_VALUE;
  (void) n;

  if (!sample_stats (ctx, &args[0], &m1, &v1, &n1, &err) ||
      !sample_stats (ctx, &args[1], &m2, &v2, &n2, &err))
    return o42_value_error (err);
  if (n1 < 2 || n2 < 2 || v1 == 0 || v2 == 0)
    return o42_value_error (O42_ERR_DIV0);
  f = v1 / v2;
  p = 1 - f_cdf (f, n1 - 1, n2 - 1);
  return o42_value_number (2 * MIN (p, 1 - p));
}

static O42Value
fn_ttest (O42EvalContext *ctx, O42Operand *args, int n)
{
  double tails, type, t, df;
  (void) n;

  ARG_NUMBER (2, tails);
  ARG_NUMBER (3, type);
  if ((tails != 1 && tails != 2) || type < 1 || type > 3)
    return o42_value_error (O42_ERR_NUM);

  if (type == 1)
    {
      GArray *xs, *ys;
      O42ErrorCode err = O42_ERR_VALUE;
      double sum = 0, ss = 0, mean;
      guint count;
      if (!collect_pairs (ctx, &args[0], &args[1], &xs, &ys, &err))
        return o42_value_error (err);
      count = xs->len;
      if (count < 2) { g_array_free (xs, TRUE); g_array_free (ys, TRUE); return o42_value_error (O42_ERR_DIV0); }
      for (guint i = 0; i < count; i++)
        sum += g_array_index (xs, double, i) - g_array_index (ys, double, i);
      mean = sum / count;
      for (guint i = 0; i < count; i++)
        {
          double d = g_array_index (xs, double, i) - g_array_index (ys, double, i) - mean;
          ss += d * d;
        }
      g_array_free (xs, TRUE);
      g_array_free (ys, TRUE);
      if (ss == 0) return o42_value_error (O42_ERR_DIV0);
      t = mean / sqrt (ss / (count - 1) / count);
      df = count - 1;
    }
  else
    {
      double m1, m2, v1, v2;
      guint n1, n2;
      O42ErrorCode err = O42_ERR_VALUE;
      if (!sample_stats (ctx, &args[0], &m1, &v1, &n1, &err) ||
          !sample_stats (ctx, &args[1], &m2, &v2, &n2, &err))
        return o42_value_error (err);
      if (n1 < 2 || n2 < 2) return o42_value_error (O42_ERR_DIV0);
      if (type == 2)
        {
          double sp = ((n1 - 1) * v1 + (n2 - 1) * v2) / (n1 + n2 - 2);
          if (sp == 0) return o42_value_error (O42_ERR_DIV0);
          t = (m1 - m2) / sqrt (sp * (1.0 / n1 + 1.0 / n2));
          df = n1 + n2 - 2;
        }
      else
        {
          double a = v1 / n1, b = v2 / n2;
          if (a + b == 0) return o42_value_error (O42_ERR_DIV0);
          t = (m1 - m2) / sqrt (a + b);
          df = (a + b) * (a + b) / (a * a / (n1 - 1) + b * b / (n2 - 1));
        }
    }
  return o42_value_number (tails * (1 - t_cdf (fabs (t), df, 0)));
}

static O42Value
fn_ztest (O42EvalContext *ctx, O42Operand *args, int n)
{
  double mean, var, x, sigma;
  guint count;
  O42ErrorCode err = O42_ERR_VALUE;

  ARG_NUMBER (1, x);
  if (!sample_stats (ctx, &args[0], &mean, &var, &count, &err))
    return o42_value_error (err);
  if (n >= 3) ARG_NUMBER (2, sigma);
  else sigma = sqrt (var);
  if (count < 1 || sigma <= 0) return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (1 - normal_cdf ((mean - x) / (sigma / sqrt (count))));
}

static O42Value
fn_prob (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *xs, *ps;
  O42ErrorCode err = O42_ERR_VALUE;
  double lower, upper, total = 0, result = 0;

  ARG_NUMBER (2, lower);
  upper = lower;
  if (n >= 4) ARG_NUMBER (3, upper);
  if (!collect_pairs (ctx, &args[0], &args[1], &xs, &ps, &err))
    return o42_value_error (err);
  for (guint i = 0; i < xs->len; i++)
    {
      double x = g_array_index (xs, double, i), p = g_array_index (ps, double, i);
      if (p < 0 || p > 1) { g_array_free (xs, TRUE); g_array_free (ps, TRUE); return o42_value_error (O42_ERR_NUM); }
      total += p;
      if (x >= lower && x <= upper) result += p;
    }
  g_array_free (xs, TRUE);
  g_array_free (ps, TRUE);
  if (fabs (total - 1) > 1e-9) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (result);
}

/* ---- matrices ---- */

static O42Value
fn_mdeterm (O42EvalContext *ctx, O42Operand *args, int n)
{
  const O42Range *r;
  int size;
  double *m, det = 1;
  (void) n;

  if (!args[0].is_range)
    return args[0].value.type == O42_VALUE_ERROR ? o42_value_copy (&args[0].value)
                                                  : o42_value_error (O42_ERR_VALUE);
  r = &args[0].range;
  size = r->row1 - r->row0 + 1;
  if (size != r->col1 - r->col0 + 1) return o42_value_error (O42_ERR_VALUE);
  m = g_new (double, (gsize) size * size);
  for (int i = 0; i < size; i++)
    for (int j = 0; j < size; j++)
      {
        O42Value v;
        O42ErrorCode e = O42_ERR_VALUE;
        ctx->get_cell (ctx, args[0].sheet, r->row0 + i, r->col0 + j, &v);
        if (!o42_value_to_number (&v, &m[i * size + j], &e))
          { o42_value_clear (&v); g_free (m); return o42_value_error (e); }
        o42_value_clear (&v);
      }
  /* Gaussian elimination with partial pivoting. */
  for (int k = 0; k < size; k++)
    {
      int pivot = k;
      for (int i = k + 1; i < size; i++)
        if (fabs (m[i * size + k]) > fabs (m[pivot * size + k])) pivot = i;
      if (m[pivot * size + k] == 0) { g_free (m); return o42_value_number (0); }
      if (pivot != k)
        {
          for (int j = 0; j < size; j++)
            { double t = m[k * size + j]; m[k * size + j] = m[pivot * size + j]; m[pivot * size + j] = t; }
          det = -det;
        }
      det *= m[k * size + k];
      for (int i = k + 1; i < size; i++)
        {
          double f = m[i * size + k] / m[k * size + k];
          for (int j = k; j < size; j++)
            m[i * size + j] -= f * m[k * size + j];
        }
    }
  g_free (m);
  return o42_value_number (det);
}

/* ---- more depreciation ---- */

static O42Value
fn_ispmt (O42EvalContext *ctx, O42Operand *args, int n)
{
  double rate, per, nper, pv;
  (void) n;
  ARG_NUMBER (0, rate);
  ARG_NUMBER (1, per);
  ARG_NUMBER (2, nper);
  ARG_NUMBER (3, pv);
  if (nper == 0) return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (pv * rate * (per / nper - 1));
}

static O42Value
fn_db_depreciation (O42EvalContext *ctx, O42Operand *args, int n)
{
  double cost, salvage, life, period, month = 12, rate, total = 0, d = 0;

  ARG_NUMBER (0, cost);
  ARG_NUMBER (1, salvage);
  ARG_NUMBER (2, life);
  ARG_NUMBER (3, period);
  if (n >= 5) ARG_NUMBER (4, month);
  if (cost <= 0 || salvage < 0 || life <= 0 || period < 1 || month < 1 || month > 12 || period > life + 1)
    return o42_value_error (O42_ERR_NUM);
  rate = floor ((1 - pow (salvage / cost, 1 / life)) * 1000 + 0.5) / 1000;
  for (int p = 1; p <= (int) period; p++)
    {
      if (p == 1) d = cost * rate * month / 12;
      else if (p == (int) life + 1) d = (cost - total) * rate * (12 - month) / 12;
      else d = (cost - total) * rate;
      total += d;
    }
  return o42_value_number (d);
}

static O42Value
fn_vdb (O42EvalContext *ctx, O42Operand *args, int n)
{
  double cost, salvage, life, start, end, factor = 2, value, total = 0;
  gboolean no_switch = FALSE;

  ARG_NUMBER (0, cost);
  ARG_NUMBER (1, salvage);
  ARG_NUMBER (2, life);
  ARG_NUMBER (3, start);
  ARG_NUMBER (4, end);
  if (n >= 6) ARG_NUMBER (5, factor);
  if (n >= 7)
    {
      O42Value v = operand_value (ctx, &args[6]);
      O42ErrorCode e = O42_ERR_VALUE;
      if (!o42_value_to_bool (&v, &no_switch, &e)) { o42_value_clear (&v); return o42_value_error (e); }
      o42_value_clear (&v);
    }
  if (cost < 0 || salvage < 0 || life <= 0 || start < 0 || end < start || end > life || factor < 0)
    return o42_value_error (O42_ERR_NUM);
  value = cost;
  for (int p = 1; p <= (int) ceil (end); p++)
    {
      double ddb = MIN (value * factor / life, value - salvage);
      double sln = (value - salvage) / (life - p + 1);
      double d = (!no_switch && sln > ddb) ? sln : ddb;
      double from = MAX (start, p - 1.0), to = MIN (end, (double) p);
      if (d < 0) d = 0;
      if (to > from) total += d * (to - from);
      value -= d;
    }
  return o42_value_number (total);
}

/* ---------------------------------------------------------------------- */
/* Batch four: Excel's newer names for the distributions, and engineering  */
/* ---------------------------------------------------------------------- */

/* ---- distributions under their 2010 names, where the shape differs ---- */

static double
t_pdf (double t, double df)
{
  return exp (lgamma ((df + 1) / 2) - lgamma (df / 2)) / sqrt (df * G_PI) * pow (1 + t * t / df, -(df + 1) / 2);
}

static O42Value
fn_t_dist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, df;
  gboolean cumulative;
  O42ErrorCode e = O42_ERR_VALUE;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, df);
  {
    O42Value v = operand_value (ctx, &args[2]);
    if (!o42_value_to_bool (&v, &cumulative, &e)) { o42_value_clear (&v); return o42_value_error (e); }
    o42_value_clear (&v);
  }
  if (df < 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (cumulative ? t_cdf (x, floor (df), 0) : t_pdf (x, floor (df)));
}

static O42Value
fn_t_dist_rt (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, df;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, df);
  if (df < 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (1 - t_cdf (x, floor (df), 0));
}

static O42Value
fn_t_dist_2t (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, df;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, df);
  if (x < 0 || df < 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (2 * (1 - t_cdf (x, floor (df), 0)));
}

static O42Value
fn_t_inv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p, df;
  (void) n;
  ARG_NUMBER (0, p);
  ARG_NUMBER (1, df);
  if (p <= 0 || p >= 1 || df < 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (invert_cdf (t_cdf, p, floor (df), 0, -1e6, 1e6));
}

static O42Value
fn_chisq_dist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, df;
  gboolean cumulative;
  O42ErrorCode e = O42_ERR_VALUE;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, df);
  {
    O42Value v = operand_value (ctx, &args[2]);
    if (!o42_value_to_bool (&v, &cumulative, &e)) { o42_value_clear (&v); return o42_value_error (e); }
    o42_value_clear (&v);
  }
  if (x < 0 || df < 1) return o42_value_error (O42_ERR_NUM);
  df = floor (df);
  if (cumulative)
    return o42_value_number (chi_cdf (x, df, 0));
  if (x == 0)
    return o42_value_number (df == 2 ? 0.5 : df < 2 ? HUGE_VAL : 0);
  return o42_value_number (exp ((df / 2 - 1) * log (x) - x / 2 - (df / 2) * log (2) - lgamma (df / 2)));
}

static O42Value
fn_chisq_inv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p, df;
  (void) n;
  ARG_NUMBER (0, p);
  ARG_NUMBER (1, df);
  if (p < 0 || p >= 1 || df < 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (invert_cdf (chi_cdf, p, floor (df), 0, 0, 1e6));
}

static O42Value
fn_f_dist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, d1, d2;
  gboolean cumulative;
  O42ErrorCode e = O42_ERR_VALUE;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, d1);
  ARG_NUMBER (2, d2);
  {
    O42Value v = operand_value (ctx, &args[3]);
    if (!o42_value_to_bool (&v, &cumulative, &e)) { o42_value_clear (&v); return o42_value_error (e); }
    o42_value_clear (&v);
  }
  if (x < 0 || d1 < 1 || d2 < 1) return o42_value_error (O42_ERR_NUM);
  d1 = floor (d1); d2 = floor (d2);
  if (cumulative)
    return o42_value_number (f_cdf (x, d1, d2));
  if (x == 0)
    return o42_value_number (d1 == 2 ? 1 : d1 < 2 ? HUGE_VAL : 0);
  return o42_value_number (exp (lgamma ((d1 + d2) / 2) - lgamma (d1 / 2) - lgamma (d2 / 2)
                                + (d1 / 2) * log (d1 / d2) + (d1 / 2 - 1) * log (x)
                                - ((d1 + d2) / 2) * log (1 + d1 * x / d2)));
}

static O42Value
fn_f_inv (O42EvalContext *ctx, O42Operand *args, int n)
{
  double p, d1, d2;
  (void) n;
  ARG_NUMBER (0, p);
  ARG_NUMBER (1, d1);
  ARG_NUMBER (2, d2);
  if (p < 0 || p >= 1 || d1 < 1 || d2 < 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (invert_cdf (f_cdf, p, floor (d1), floor (d2), 0, 1e6));
}

static O42Value
fn_lognorm_dist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, mean, sd;
  gboolean cumulative;
  O42ErrorCode e = O42_ERR_VALUE;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, mean);
  ARG_NUMBER (2, sd);
  {
    O42Value v = operand_value (ctx, &args[3]);
    if (!o42_value_to_bool (&v, &cumulative, &e)) { o42_value_clear (&v); return o42_value_error (e); }
    o42_value_clear (&v);
  }
  if (x <= 0 || sd <= 0) return o42_value_error (O42_ERR_NUM);
  if (cumulative)
    return o42_value_number (normal_cdf ((log (x) - mean) / sd));
  {
    double z = (log (x) - mean) / sd;
    return o42_value_number (exp (-z * z / 2) / (x * sd * sqrt (2 * G_PI)));
  }
}

static O42Value
fn_norm_s_dist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double z;
  gboolean cumulative = TRUE;
  O42ErrorCode e = O42_ERR_VALUE;
  ARG_NUMBER (0, z);
  if (n >= 2)
    {
      O42Value v = operand_value (ctx, &args[1]);
      if (!o42_value_to_bool (&v, &cumulative, &e)) { o42_value_clear (&v); return o42_value_error (e); }
      o42_value_clear (&v);
    }
  return o42_value_number (cumulative ? normal_cdf (z) : exp (-z * z / 2) / sqrt (2 * G_PI));
}

static O42Value
fn_beta_dist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, a, b, lo = 0, hi = 1;
  gboolean cumulative;
  O42ErrorCode e = O42_ERR_VALUE;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, a);
  ARG_NUMBER (2, b);
  {
    O42Value v = operand_value (ctx, &args[3]);
    if (!o42_value_to_bool (&v, &cumulative, &e)) { o42_value_clear (&v); return o42_value_error (e); }
    o42_value_clear (&v);
  }
  if (n >= 5) ARG_NUMBER (4, lo);
  if (n >= 6) ARG_NUMBER (5, hi);
  if (a <= 0 || b <= 0 || x < lo || x > hi || lo == hi) return o42_value_error (O42_ERR_NUM);
  x = (x - lo) / (hi - lo);
  if (cumulative)
    return o42_value_number (beta_i (a, b, x));
  return o42_value_number (exp ((a - 1) * log (x) + (b - 1) * log (1 - x)
                                + lgamma (a + b) - lgamma (a) - lgamma (b)) / (hi - lo));
}

static O42Value
fn_hypgeom_dist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, draws, successes, population;
  gboolean cumulative;
  O42ErrorCode e = O42_ERR_VALUE;
  double sum = 0;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, draws);
  ARG_NUMBER (2, successes);
  ARG_NUMBER (3, population);
  {
    O42Value v = operand_value (ctx, &args[4]);
    if (!o42_value_to_bool (&v, &cumulative, &e)) { o42_value_clear (&v); return o42_value_error (e); }
    o42_value_clear (&v);
  }
  x = floor (x); draws = floor (draws); successes = floor (successes); population = floor (population);
  if (x < 0 || draws > population || successes > population || population <= 0)
    return o42_value_error (O42_ERR_NUM);
  for (double k = cumulative ? 0 : x; k <= x; k++)
    if (k <= draws && k <= successes && draws - k <= population - successes)
      sum += exp (lchoose (successes, k) + lchoose (population - successes, draws - k) - lchoose (population, draws));
  return o42_value_number (sum);
}

static O42Value
fn_negbinom_dist (O42EvalContext *ctx, O42Operand *args, int n)
{
  double f, s, p, sum = 0;
  gboolean cumulative;
  O42ErrorCode e = O42_ERR_VALUE;
  (void) n;
  ARG_NUMBER (0, f);
  ARG_NUMBER (1, s);
  ARG_NUMBER (2, p);
  {
    O42Value v = operand_value (ctx, &args[3]);
    if (!o42_value_to_bool (&v, &cumulative, &e)) { o42_value_clear (&v); return o42_value_error (e); }
    o42_value_clear (&v);
  }
  f = floor (f); s = floor (s);
  if (f < 0 || s < 1 || p <= 0 || p >= 1) return o42_value_error (O42_ERR_NUM);
  for (double k = cumulative ? 0 : f; k <= f; k++)
    sum += exp (lchoose (k + s - 1, s - 1)) * pow (p, s) * pow (1 - p, k);
  return o42_value_number (sum);
}

static O42Value
fn_covariance_s (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *xs, *ys;
  O42ErrorCode err = O42_ERR_VALUE;
  double mx = 0, my = 0, sxy = 0;
  guint count;
  (void) n;
  if (!collect_pairs (ctx, &args[0], &args[1], &xs, &ys, &err))
    return o42_value_error (err);
  count = xs->len;
  if (count < 2) { g_array_free (xs, TRUE); g_array_free (ys, TRUE); return o42_value_error (O42_ERR_DIV0); }
  for (guint i = 0; i < count; i++) { mx += g_array_index (xs, double, i); my += g_array_index (ys, double, i); }
  mx /= count; my /= count;
  for (guint i = 0; i < count; i++)
    sxy += (g_array_index (xs, double, i) - mx) * (g_array_index (ys, double, i) - my);
  g_array_free (xs, TRUE);
  g_array_free (ys, TRUE);
  return o42_value_number (sxy / (count - 1));
}

/* CEILING.MATH and FLOOR.MATH take a third "mode" argument this
 * evaluator does not need for positive numbers; the first two go to
 * the old functions. */
static O42Value fn_ceiling_math (O42EvalContext *c, O42Operand *a, int n) { return fn_ceiling (c, a, MIN (n, 2)); }
static O42Value fn_floor_math   (O42EvalContext *c, O42Operand *a, int n) { return fn_floor (c, a, MIN (n, 2)); }

static O42Value
fn_numbervalue (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *text, *cleaned;
  char *decimal = NULL, *group = NULL;
  double v;
  char *end_ptr = NULL;
  ARG_TEXT (0, text);
  if (n >= 2) ARG_TEXT (1, decimal);
  if (n >= 3) ARG_TEXT (2, group);
  /* Spaces and the group separator are dropped; the decimal separator
   * becomes a point. */
  cleaned = g_new (char, strlen (text) + 1);
  {
    char *q = cleaned;
    char dec = decimal && decimal[0] ? decimal[0] : '.';
    char grp = group && group[0] ? group[0] : ',';
    for (const char *p = text; *p; p++)
      {
        if (*p == ' ' || *p == grp) continue;
        *q++ = (*p == dec) ? '.' : *p;
      }
    *q = '\0';
  }
  g_free (text);
  g_free (decimal);
  g_free (group);
  v = g_ascii_strtod (cleaned, &end_ptr);
  if (end_ptr == cleaned || (*end_ptr != '\0' && strcmp (end_ptr, "%") != 0))
    { g_free (cleaned); return o42_value_error (O42_ERR_VALUE); }
  if (*end_ptr == '%') v /= 100;
  g_free (cleaned);
  return o42_value_number (v);
}

static O42Value
fn_unichar (O42EvalContext *ctx, O42Operand *args, int n)
{
  double code;
  char buf[8];
  (void) n;
  ARG_NUMBER (0, code);
  if (code < 1 || code > 0x10FFFF || !g_unichar_validate ((gunichar) code))
    return o42_value_error (O42_ERR_VALUE);
  buf[g_unichar_to_utf8 ((gunichar) code, buf)] = '\0';
  return o42_value_text (buf);
}

static O42Value
fn_unicode (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *text;
  gunichar c;
  (void) n;
  ARG_TEXT (0, text);
  if (text[0] == '\0') { g_free (text); return o42_value_error (O42_ERR_VALUE); }
  c = g_utf8_get_char (text);
  g_free (text);
  return o42_value_number (c);
}

/* ---- More of what Gnumeric offers ------------------------------------ */

/* The complex functions Excel leaves out and Gnumeric has: the
 * hyperbolic three, their reciprocals, the inverse trigonometric
 * three, and the two that need no trigonometry at all. */
static O42Value
fn_imsinh (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  return o42_value_take (complex_format (sinh (re) * cos (im), cosh (re) * sin (im), sfx));
}

static O42Value
fn_imcosh (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  return o42_value_take (complex_format (cosh (re) * cos (im), sinh (re) * sin (im), sfx));
}

/* One complex number divided by another, which the reciprocals need. */
static void
complex_divide (double ar, double ai, double br, double bi, double *re, double *im)
{
  double d = br * br + bi * bi;

  if (d == 0)
    { *re = *im = NAN; return; }
  *re = (ar * br + ai * bi) / d;
  *im = (ai * br - ar * bi) / d;
}

static O42Value
complex_ratio (O42EvalContext *ctx, O42Operand *args, int which)
{
  double re, im, ar, ai, br, bi, qr, qi; char sfx;

  ARG_COMPLEX (0, re, im, sfx);
  /* sin, cos, tan, and their reciprocals, from the two that are known. */
  switch (which)
    {
    case 0:  /* IMTAN = sin / cos */
      ar = sin (re) * cosh (im); ai = cos (re) * sinh (im);
      br = cos (re) * cosh (im); bi = -sin (re) * sinh (im);
      break;
    case 1:  /* IMSEC = 1 / cos */
      ar = 1; ai = 0;
      br = cos (re) * cosh (im); bi = -sin (re) * sinh (im);
      break;
    case 2:  /* IMCSC = 1 / sin */
      ar = 1; ai = 0;
      br = sin (re) * cosh (im); bi = cos (re) * sinh (im);
      break;
    case 3:  /* IMCOT = cos / sin */
      ar = cos (re) * cosh (im); ai = -sin (re) * sinh (im);
      br = sin (re) * cosh (im); bi = cos (re) * sinh (im);
      break;
    case 4:  /* IMTANH = sinh / cosh */
      ar = sinh (re) * cos (im); ai = cosh (re) * sin (im);
      br = cosh (re) * cos (im); bi = sinh (re) * sin (im);
      break;
    case 5:  /* IMSECH = 1 / cosh */
      ar = 1; ai = 0;
      br = cosh (re) * cos (im); bi = sinh (re) * sin (im);
      break;
    case 6:  /* IMCSCH = 1 / sinh */
      ar = 1; ai = 0;
      br = sinh (re) * cos (im); bi = cosh (re) * sin (im);
      break;
    default: /* IMINV = 1 / z */
      ar = 1; ai = 0; br = re; bi = im;
      break;
    }
  complex_divide (ar, ai, br, bi, &qr, &qi);
  if (isnan (qr) || isnan (qi))
    return o42_value_error (O42_ERR_DIV0);
  return o42_value_take (complex_format (qr, qi, sfx));
}

static O42Value fn_imtan  (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_ratio (c, a, 0); }
static O42Value fn_imsec  (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_ratio (c, a, 1); }
static O42Value fn_imcsc  (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_ratio (c, a, 2); }
static O42Value fn_imcot  (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_ratio (c, a, 3); }
static O42Value fn_imtanh (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_ratio (c, a, 4); }
static O42Value fn_imsech (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_ratio (c, a, 5); }
static O42Value fn_imcsch (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_ratio (c, a, 6); }
static O42Value fn_iminv  (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_ratio (c, a, 7); }

static O42Value
fn_imneg (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im; char sfx;
  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  return o42_value_take (complex_format (-re, -im, sfx));
}

/* The inverse trigonometric three, by way of the logarithm:
 * arcsin z = -i ln (iz + sqrt (1 - z^2)) and its companions. */
static void
complex_sqrt (double re, double im, double *outr, double *outi)
{
  double r = hypot (re, im);

  *outr = sqrt (MAX ((r + re) / 2, 0));
  *outi = (im >= 0 ? 1 : -1) * sqrt (MAX ((r - re) / 2, 0));
}

static void
complex_log (double re, double im, double *outr, double *outi)
{
  *outr = log (hypot (re, im));
  *outi = atan2 (im, re);
}

static O42Value
complex_inverse_trig (O42EvalContext *ctx, O42Operand *args, int which)
{
  double re, im; char sfx;
  double sr, si, lr, li, qr, qi;

  ARG_COMPLEX (0, re, im, sfx);

  if (which == 2)
    {
      /* arctan z = (i/2) ln ((i + z) / (i - z)) */
      complex_divide (re, 1 + im, -re, 1 - im, &qr, &qi);
      complex_log (qr, qi, &lr, &li);
      return o42_value_take (complex_format (-li / 2, lr / 2, sfx));
    }

  /* 1 - z^2, then its square root. */
  complex_sqrt (1 - (re * re - im * im), -(2 * re * im), &sr, &si);
  if (which == 0)
    {
      /* arcsin z = -i ln (iz + sqrt (1 - z^2)) */
      complex_log (sr - im, si + re, &lr, &li);
      return o42_value_take (complex_format (li, -lr, sfx));
    }
  /* arccos z = -i ln (z + i sqrt (1 - z^2)) */
  complex_log (re - si, im + sr, &lr, &li);
  return o42_value_take (complex_format (li, -lr, sfx));
}

static O42Value fn_imarcsin (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_inverse_trig (c, a, 0); }
static O42Value fn_imarccos (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_inverse_trig (c, a, 1); }
static O42Value fn_imarctan (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_inverse_trig (c, a, 2); }

/* The inverse hyperbolic functions of a complex number, and the six
 * that are one of those applied to 1/z.  Gnumeric has the lot; each is
 * a logarithm once the algebra is done. */
static void
c_arcsinh (double re, double im, double *outr, double *outi)
{
  double sr, si, lr, li;

  /* arcsinh z = ln (z + sqrt (z^2 + 1)) */
  complex_sqrt (re * re - im * im + 1, 2 * re * im, &sr, &si);
  complex_log (re + sr, im + si, &lr, &li);
  *outr = lr; *outi = li;
}

static void
c_arccosh (double re, double im, double *outr, double *outi)
{
  double sr, si, lr, li;

  /* arccosh z = ln (z + sqrt (z^2 - 1)) */
  complex_sqrt (re * re - im * im - 1, 2 * re * im, &sr, &si);
  complex_log (re + sr, im + si, &lr, &li);
  *outr = lr; *outi = li;
}

static void
c_arctanh (double re, double im, double *outr, double *outi)
{
  double qr, qi, lr, li;

  /* arctanh z = ln ((1 + z) / (1 - z)) / 2 */
  complex_divide (1 + re, im, 1 - re, -im, &qr, &qi);
  complex_log (qr, qi, &lr, &li);
  *outr = lr / 2; *outi = li / 2;
}

static void
c_arcsin (double re, double im, double *outr, double *outi)
{
  double sr, si, lr, li;

  complex_sqrt (1 - (re * re - im * im), -(2 * re * im), &sr, &si);
  complex_log (sr - im, si + re, &lr, &li);
  *outr = li; *outi = -lr;
}

static void
c_arccos (double re, double im, double *outr, double *outi)
{
  double sr, si, lr, li;

  complex_sqrt (1 - (re * re - im * im), -(2 * re * im), &sr, &si);
  complex_log (re - si, im + sr, &lr, &li);
  *outr = li; *outi = -lr;
}

static void
c_arctan (double re, double im, double *outr, double *outi)
{
  double qr, qi, lr, li;

  complex_divide (re, 1 + im, -re, 1 - im, &qr, &qi);
  complex_log (qr, qi, &lr, &li);
  *outr = -li / 2; *outi = lr / 2;
}

/* `which` names the function; a `reciprocal` one is the same applied
 * to 1/z, which is what arcsec, arccsc and arccot are. */
static O42Value
complex_inverse (O42EvalContext *ctx, O42Operand *args, int which, gboolean reciprocal)
{
  double re, im, outr = 0, outi = 0; char sfx;

  ARG_COMPLEX (0, re, im, sfx);
  if (reciprocal)
    {
      double qr, qi;

      complex_divide (1, 0, re, im, &qr, &qi);
      if (isnan (qr) || isnan (qi))
        return o42_value_error (O42_ERR_DIV0);
      re = qr; im = qi;
    }
  switch (which)
    {
    case 0: c_arcsin (re, im, &outr, &outi); break;
    case 1: c_arccos (re, im, &outr, &outi); break;
    case 2: c_arctan (re, im, &outr, &outi); break;
    case 3: c_arcsinh (re, im, &outr, &outi); break;
    case 4: c_arccosh (re, im, &outr, &outi); break;
    default: c_arctanh (re, im, &outr, &outi); break;
    }
  if (isnan (outr) || isnan (outi) || isinf (outr) || isinf (outi))
    return o42_value_error (O42_ERR_NUM);
  return o42_value_take (complex_format (outr, outi, sfx));
}

static O42Value fn_imarcsinh (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_inverse (c, a, 3, FALSE); }
static O42Value fn_imarccosh (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_inverse (c, a, 4, FALSE); }
static O42Value fn_imarctanh (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_inverse (c, a, 5, FALSE); }
static O42Value fn_imarcsec  (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_inverse (c, a, 1, TRUE); }
static O42Value fn_imarccsc  (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_inverse (c, a, 0, TRUE); }
static O42Value fn_imarccot  (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_inverse (c, a, 2, TRUE); }
static O42Value fn_imarcsech (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_inverse (c, a, 4, TRUE); }
static O42Value fn_imarccsch (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_inverse (c, a, 3, TRUE); }
static O42Value fn_imarccoth (O42EvalContext *c, O42Operand *a, int n) { (void) n; return complex_inverse (c, a, 5, TRUE); }

/* IMCOTH = cosh / sinh, the one reciprocal complex_ratio has not got. */
static O42Value
fn_imcoth (O42EvalContext *ctx, O42Operand *args, int n)
{
  double re, im, qr, qi; char sfx;

  (void) n;
  ARG_COMPLEX (0, re, im, sfx);
  complex_divide (cosh (re) * cos (im), sinh (re) * sin (im),
                  sinh (re) * cos (im), cosh (re) * sin (im), &qr, &qi);
  if (isnan (qr) || isnan (qi))
    return o42_value_error (O42_ERR_DIV0);
  return o42_value_take (complex_format (qr, qi, sfx));
}

/* FLOOR.PRECISE and CEILING.PRECISE round towards minus and plus
 * infinity whatever the sign, which is what tells them from FLOOR and
 * CEILING. */
static O42Value
fn_floor_precise (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, step = 1;

  ARG_NUMBER (0, x);
  if (n >= 2)
    ARG_NUMBER (1, step);
  step = fabs (step);
  if (step == 0)
    return o42_value_number (0);
  return o42_value_number (floor (x / step) * step);
}

static O42Value
fn_ceiling_precise (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, step = 1;

  ARG_NUMBER (0, x);
  if (n >= 2)
    ARG_NUMBER (1, step);
  step = fabs (step);
  if (step == 0)
    return o42_value_number (0);
  return o42_value_number (ceil (x / step) * step);
}

/* SHEET and SHEETS, which the caller answers since the engine has no
 * book of its own. */
static O42Value
fn_sheet (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value out;

  if (ctx->get_cell_info != NULL &&
      ctx->get_cell_info (ctx, n >= 1 && args[0].is_range ? args[0].sheet : NULL,
                          ctx->row, ctx->col, "sheet", &out))
    return out;
  return o42_value_error (O42_ERR_NA);
}

static O42Value
fn_sheets (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value out;

  (void) args; (void) n;
  if (ctx->get_cell_info != NULL &&
      ctx->get_cell_info (ctx, NULL, ctx->row, ctx->col, "sheets", &out))
    return out;
  return o42_value_error (O42_ERR_NA);
}

/* ISFORMULA: whether the cell holds one, which again only the caller
 * can say. */
static O42Value
fn_isformula (O42EvalContext *ctx, O42Operand *args, int n)
{
  O42Value out;

  (void) n;
  if (!args[0].is_range)
    return o42_value_bool (FALSE);
  if (ctx->get_cell_info != NULL &&
      ctx->get_cell_info (ctx, args[0].sheet, args[0].range.row0, args[0].range.col0,
                          "formula", &out))
    return out;
  return o42_value_error (O42_ERR_NA);
}

/* The population skew and kurtosis, which divide by n rather than by
 * n - 1: Gnumeric's SKEWP and KURTP. */
static O42Value
population_moment (O42EvalContext *ctx, O42Operand *args, int n, gboolean kurtosis)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double sum = 0, mean, variance = 0, moment = 0;
  guint count;

  if (!collect_numbers (ctx, args, n, &values, &err))
    return o42_value_error (err);
  count = values->len;
  if (count < (kurtosis ? 2u : 2u))
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_DIV0); }

  for (guint i = 0; i < count; i++)
    sum += g_array_index (values, double, i);
  mean = sum / count;
  for (guint i = 0; i < count; i++)
    {
      double d = g_array_index (values, double, i) - mean;

      variance += d * d;
      moment += kurtosis ? d * d * d * d : d * d * d;
    }
  variance /= count;
  moment /= count;
  g_array_free (values, TRUE);
  if (variance <= 0)
    return o42_value_error (O42_ERR_DIV0);
  if (kurtosis)
    return o42_value_number (moment / (variance * variance) - 3);
  return o42_value_number (moment / pow (variance, 1.5));
}

static O42Value fn_skewp (O42EvalContext *c, O42Operand *a, int n) { return population_moment (c, a, n, FALSE); }
static O42Value fn_kurtp (O42EvalContext *c, O42Operand *a, int n) { return population_moment (c, a, n, TRUE); }

/* The distributions Gnumeric has and Excel does not.  Each takes its
  * parameters, a value, and whether the answer is the density or the
  * probability up to that point. */
static O42Value
fn_logistic (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, a;
  double e;

  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, a);
  if (a <= 0)
    return o42_value_error (O42_ERR_NUM);
  e = exp (-x / a);
  return o42_value_number (e / (a * (1 + e) * (1 + e)));
}

static O42Value
fn_pareto (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, a, b;

  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, a);
  ARG_NUMBER (2, b);
  if (a <= 0 || b <= 0 || x < b)
    return o42_value_number (0);
  return o42_value_number (a * pow (b, a) / pow (x, a + 1));
}

static O42Value
fn_rayleigh (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, sigma;

  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, sigma);
  if (sigma <= 0)
    return o42_value_error (O42_ERR_NUM);
  if (x < 0)
    return o42_value_number (0);
  return o42_value_number (x / (sigma * sigma) * exp (-x * x / (2 * sigma * sigma)));
}

static O42Value
fn_laplace (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, a;

  (void) n;
  ARG_NUMBER (0, x);
  ARG_NUMBER (1, a);
  if (a <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (exp (-fabs (x / a)) / (2 * a));
}

/* SSMEDIAN: the median of grouped data, where every value stands for a
 * bin of the given width and the answer is interpolated inside the bin
 * the middle falls in. */
static O42Value
fn_ssmedian (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double interval = 1, median;
  guint count;

  if (!collect_numbers (ctx, args, 1, &values, &err))
    return o42_value_error (err);
  if (n >= 2)
    {
      O42Value v = operand_value (ctx, &args[1]);
      double width;
      gboolean ok = o42_value_to_number (&v, &width, &err);

      o42_value_clear (&v);
      if (!ok)
        { g_array_free (values, TRUE); return o42_value_error (err); }
      interval = width;
    }
  count = values->len;
  if (count == 0 || interval <= 0)
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NUM); }

  g_array_sort (values, compare_doubles);
  median = (count % 2 == 1) ? g_array_index (values, double, count / 2)
                            : (g_array_index (values, double, count / 2 - 1) +
                               g_array_index (values, double, count / 2)) / 2;
  /* How many are below the bin the median sits in, and how many are in
   * it: the interpolation Gnumeric documents. */
  {
    double lower = median - interval / 2;
    guint below = 0, inside = 0;

    for (guint i = 0; i < count; i++)
      {
        double v = g_array_index (values, double, i);

        if (v < lower) below++;
        else if (v < lower + interval) inside++;
      }
    g_array_free (values, TRUE);
    if (inside == 0)
      return o42_value_number (median);
    return o42_value_number (lower + (count / 2.0 - below) * interval / inside);
  }
}

/* CRONBACH: the alpha that says how far a set of columns measures one
 * and the same thing. */
static O42Value
fn_cronbach (O42EvalContext *ctx, O42Operand *args, int n)
{
  double sum_of_variances = 0;
  double total_variance = 0;
  int rows = 0, k = n;
  GArray *totals;

  if (n < 2)
    return o42_value_error (O42_ERR_NUM);

  /* Each argument is one item; the rows are the subjects. */
  {
    O42Range r = args[0].range;

    rows = args[0].is_range ? (r.row1 - r.row0 + 1) : 1;
  }
  if (rows < 2)
    return o42_value_error (O42_ERR_NUM);
  totals = g_array_sized_new (FALSE, TRUE, sizeof (double), rows);
  g_array_set_size (totals, rows);

  for (int item = 0; item < k; item++)
    {
      GArray *values;
      O42ErrorCode err = O42_ERR_VALUE;
      double sum = 0, mean, variance = 0;

      if (!collect_numbers (ctx, &args[item], 1, &values, &err))
        { g_array_free (totals, TRUE); return o42_value_error (err); }
      if ((int) values->len != rows)
        { g_array_free (values, TRUE); g_array_free (totals, TRUE); return o42_value_error (O42_ERR_NUM); }
      for (guint i = 0; i < values->len; i++)
        {
          double v = g_array_index (values, double, i);

          sum += v;
          g_array_index (totals, double, i) += v;
        }
      mean = sum / values->len;
      for (guint i = 0; i < values->len; i++)
        {
          double d = g_array_index (values, double, i) - mean;

          variance += d * d;
        }
      sum_of_variances += variance / (values->len - 1);
      g_array_free (values, TRUE);
    }

  {
    double sum = 0, mean;

    for (int i = 0; i < rows; i++)
      sum += g_array_index (totals, double, i);
    mean = sum / rows;
    for (int i = 0; i < rows; i++)
      {
        double d = g_array_index (totals, double, i) - mean;

        total_variance += d * d;
      }
    total_variance /= (rows - 1);
  }
  g_array_free (totals, TRUE);
  if (total_variance <= 0)
    return o42_value_error (O42_ERR_DIV0);
  return o42_value_number (k / (double) (k - 1) * (1 - sum_of_variances / total_variance));
}

/* ---- More of what Gnumeric offers: the small ones --------------------- */

/* exp(x) - 1, worked out so that a small x keeps its digits: the
 * subtraction would take them away. */
static O42Value
fn_expm1 (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;

  (void) n;
  ARG_NUMBER (0, x);
  return o42_value_number (expm1 (x));
}

/* log(1 + x), the same care the other way about. */
static O42Value
fn_ln1p (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;

  (void) n;
  ARG_NUMBER (0, x);
  if (x <= -1)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (log1p (x));
}

/* The beta function, and its logarithm, which is what a large
 * argument needs. */
static O42Value
fn_beta (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a, b;

  (void) n;
  ARG_NUMBER (0, a);
  ARG_NUMBER (1, b);
  if (a <= 0 || b <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (exp (lgamma (a) + lgamma (b) - lgamma (a + b)));
}

static O42Value
fn_betaln (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a, b;

  (void) n;
  ARG_NUMBER (0, a);
  ARG_NUMBER (1, b);
  if (a <= 0 || b <= 0)
    return o42_value_error (O42_ERR_NUM);
  return o42_value_number (lgamma (a) + lgamma (b) - lgamma (a + b));
}

/* CEIL and FLOOR without a step: Gnumeric's plain pair. */
static O42Value
fn_ceil (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;

  (void) n;
  ARG_NUMBER (0, x);
  return o42_value_number (ceil (x));
}

/* An error value by name, which is how a formula makes one on purpose. */
static O42Value
fn_error (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *text;
  O42ErrorCode code = O42_ERR_NA;

  (void) n;
  ARG_TEXT (0, text);
  if (g_ascii_strcasecmp (text, "#DIV/0!") == 0)      code = O42_ERR_DIV0;
  else if (g_ascii_strcasecmp (text, "#VALUE!") == 0) code = O42_ERR_VALUE;
  else if (g_ascii_strcasecmp (text, "#REF!") == 0)   code = O42_ERR_REF;
  else if (g_ascii_strcasecmp (text, "#NAME?") == 0)  code = O42_ERR_NAME;
  else if (g_ascii_strcasecmp (text, "#NUM!") == 0)   code = O42_ERR_NUM;
  else if (g_ascii_strcasecmp (text, "#NULL!") == 0)  code = O42_ERR_NULL;
  g_free (text);
  return o42_value_error (code);
}

/* ---- CONVERT ---- */

typedef struct { const char *unit; int group; double factor; gboolean prefixable; } Unit;

/* Factors to a base unit of each group; temperature is handled apart. */
static const Unit UNITS[] = {
  { "g", 1, 1, TRUE }, { "sg", 1, 14593.9029372064, FALSE }, { "lbm", 1, 453.59237, FALSE },
  { "u", 1, 1.66053886282828e-24, TRUE }, { "ozm", 1, 28.349523125, FALSE }, { "stone", 1, 6350.29318, FALSE },
  { "ton", 1, 907184.74, FALSE }, { "uk_ton", 1, 1016046.9088, FALSE },
  { "m", 2, 1, TRUE }, { "mi", 2, 1609.344, FALSE }, { "Nmi", 2, 1852, FALSE }, { "in", 2, 0.0254, FALSE },
  { "ft", 2, 0.3048, FALSE }, { "yd", 2, 0.9144, FALSE }, { "ang", 2, 1e-10, TRUE }, { "ell", 2, 1.143, FALSE },
  { "ly", 2, 9.46073047258e15, FALSE }, { "parsec", 2, 3.08567758128e16, FALSE }, { "pc", 2, 3.08567758128e16, FALSE },
  { "sec", 3, 1, TRUE }, { "s", 3, 1, TRUE }, { "min", 3, 60, FALSE }, { "mn", 3, 60, FALSE }, { "hr", 3, 3600, FALSE },
  { "day", 3, 86400, FALSE }, { "d", 3, 86400, FALSE }, { "yr", 3, 31557600, FALSE },
  { "Pa", 4, 1, TRUE }, { "p", 4, 1, TRUE }, { "atm", 4, 101325, TRUE }, { "at", 4, 101325, TRUE },
  { "mmHg", 4, 133.322, TRUE }, { "psi", 4, 6894.757293168, FALSE }, { "Torr", 4, 133.322368421053, FALSE },
  { "N", 5, 1, TRUE }, { "dyn", 5, 1e-5, TRUE }, { "dy", 5, 1e-5, TRUE }, { "lbf", 5, 4.4482216152605, FALSE }, { "pond", 5, 0.00980665, TRUE },
  { "J", 6, 1, TRUE }, { "e", 6, 1e-7, TRUE }, { "c", 6, 4.184, TRUE }, { "cal", 6, 4.1868, TRUE }, { "eV", 6, 1.602176462e-19, TRUE },
  { "ev", 6, 1.602176462e-19, TRUE }, { "HPh", 6, 2684519.5376962, FALSE }, { "hh", 6, 2684519.5376962, FALSE },
  { "Wh", 6, 3600, TRUE }, { "wh", 6, 3600, TRUE }, { "flb", 6, 1.3558179483314, FALSE }, { "BTU", 6, 1055.05585262, FALSE }, { "btu", 6, 1055.05585262, FALSE },
  { "W", 7, 1, TRUE }, { "w", 7, 1, TRUE }, { "HP", 7, 745.69987158227, FALSE }, { "h", 7, 745.69987158227, FALSE }, { "PS", 7, 735.49875, FALSE },
  { "T", 8, 1, TRUE }, { "ga", 8, 1e-4, TRUE },
  { "l", 9, 1, TRUE }, { "L", 9, 1, TRUE }, { "lt", 9, 1, TRUE }, { "tsp", 9, 0.00492892159375, FALSE }, { "tspm", 9, 0.005, FALSE },
  { "tbs", 9, 0.01478676478125, FALSE }, { "oz", 9, 0.0295735295625, FALSE }, { "cup", 9, 0.2365882365, FALSE },
  { "pt", 9, 0.473176473, FALSE }, { "us_pt", 9, 0.473176473, FALSE }, { "uk_pt", 9, 0.56826125, FALSE },
  { "qt", 9, 0.946352946, FALSE }, { "uk_qt", 9, 1.1365225, FALSE }, { "gal", 9, 3.785411784, FALSE }, { "uk_gal", 9, 4.54609, FALSE },
  { "m3", 9, 1000, TRUE }, { "in3", 9, 0.016387064, FALSE }, { "ft3", 9, 28.316846592, FALSE }, { "yd3", 9, 764.554857984, FALSE },
  { "barrel", 9, 158.987294928, FALSE }, { "bushel", 9, 35.23907016688, FALSE }, { "MTON", 9, 1132.67386368, FALSE },
  { "m2", 10, 1, TRUE }, { "ha", 10, 10000, TRUE }, { "ar", 10, 100, TRUE }, { "us_acre", 10, 4046.8564224, FALSE }, { "uk_acre", 10, 4046.8564224, FALSE },
  { "in2", 10, 0.00064516, FALSE }, { "ft2", 10, 0.09290304, FALSE }, { "yd2", 10, 0.83612736, FALSE }, { "mi2", 10, 2589988.110336, FALSE },
  { "m/s", 11, 1, TRUE }, { "m/sec", 11, 1, TRUE }, { "m/h", 11, 1.0 / 3600, TRUE }, { "m/hr", 11, 1.0 / 3600, TRUE },
  { "mph", 11, 0.44704, FALSE }, { "kn", 11, 0.514444444444444, FALSE }, { "admkn", 11, 0.514773333333333, FALSE },
  { "bit", 12, 1, TRUE }, { "byte", 12, 8, TRUE },
  { "C", 13, 0, FALSE }, { "cel", 13, 0, FALSE }, { "F", 13, 0, FALSE }, { "fah", 13, 0, FALSE }, { "K", 13, 0, TRUE }, { "kel", 13, 0, TRUE },
};

static const struct { const char *prefix; double factor; } PREFIXES[] = {
  { "Y", 1e24 }, { "Z", 1e21 }, { "E", 1e18 }, { "P", 1e15 }, { "T", 1e12 }, { "G", 1e9 }, { "M", 1e6 },
  { "k", 1e3 }, { "h", 1e2 }, { "da", 1e1 }, { "e", 1e1 }, { "d", 1e-1 }, { "c", 1e-2 }, { "m", 1e-3 },
  { "u", 1e-6 }, { "n", 1e-9 }, { "p", 1e-12 }, { "f", 1e-15 }, { "a", 1e-18 }, { "z", 1e-21 }, { "y", 1e-24 },
  { "ki", 1024.0 }, { "Mi", 1048576.0 }, { "Gi", 1073741824.0 }, { "Ti", 1099511627776.0 },
};

static const Unit *
unit_lookup (const char *text, double *factor)
{
  for (guint i = 0; i < G_N_ELEMENTS (UNITS); i++)
    if (strcmp (UNITS[i].unit, text) == 0)
      { *factor = 1; return &UNITS[i]; }
  for (guint k = 0; k < G_N_ELEMENTS (PREFIXES); k++)
    {
      gsize plen = strlen (PREFIXES[k].prefix);
      if (strncmp (text, PREFIXES[k].prefix, plen) == 0)
        for (guint i = 0; i < G_N_ELEMENTS (UNITS); i++)
          if (UNITS[i].prefixable && strcmp (UNITS[i].unit, text + plen) == 0)
            { *factor = PREFIXES[k].factor; return &UNITS[i]; }
    }
  return NULL;
}

static O42Value
fn_convert (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, ff, tf;
  char *from, *to;
  const Unit *uf, *ut;
  (void) n;
  ARG_NUMBER (0, x);
  ARG_TEXT (1, from);
  ARG_TEXT (2, to);
  uf = unit_lookup (from, &ff);
  ut = unit_lookup (to, &tf);
  g_free (from);
  g_free (to);
  if (uf == NULL || ut == NULL || uf->group != ut->group)
    return o42_value_error (O42_ERR_NA);
  if (uf->group == 13)
    {
      /* Temperature, through kelvin. */
      double k;
      if (uf->unit[0] == 'C' || uf->unit[0] == 'c') k = x + 273.15;
      else if (uf->unit[0] == 'F' || uf->unit[0] == 'f') k = (x - 32) * 5 / 9 + 273.15;
      else k = x * ff;
      if (ut->unit[0] == 'C' || ut->unit[0] == 'c') return o42_value_number (k - 273.15);
      if (ut->unit[0] == 'F' || ut->unit[0] == 'f') return o42_value_number ((k - 273.15) * 9 / 5 + 32);
      return o42_value_number (k / tf);
    }
  return o42_value_number (x * uf->factor * ff / (ut->factor * tf));
}

/* ---------------------------------------------------------------------- */
/* Batch five: the rest of the statistics, Bessel, and the math and        */
/* finance extras Excel and Gnumeric both have                             */
/* ---------------------------------------------------------------------- */

/* ---- statistics ---- */

static O42Value
fn_percentile_exc (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double k, pos, frac;
  guint lo, count;
  double result;
  (void) n;

  ARG_NUMBER (1, k);
  if (!collect_numbers (ctx, args, 1, &values, &err))
    return o42_value_error (err);
  count = values->len;
  pos = k * (count + 1);
  if (count == 0 || k <= 0 || k >= 1 || pos < 1 || pos > count)
    { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NUM); }
  g_array_sort (values, compare_doubles);
  lo = (guint) floor (pos);
  frac = pos - lo;
  if (lo >= count)
    result = g_array_index (values, double, count - 1);
  else
    result = g_array_index (values, double, lo - 1) * (1 - frac) + g_array_index (values, double, lo) * frac;
  g_array_free (values, TRUE);
  return o42_value_number (result);
}

static O42Value
fn_quartile_exc (O42EvalContext *ctx, O42Operand *args, int n)
{
  double q;
  O42Operand quarter;
  O42Value r;
  (void) n;
  ARG_NUMBER (1, q);
  q = trunc (q);
  if (q < 1 || q > 3) return o42_value_error (O42_ERR_NUM);
  memset (&quarter, 0, sizeof quarter);
  quarter.value = o42_value_number (q / 4);
  {
    O42Operand two[2] = { args[0], quarter };
    r = fn_percentile_exc (ctx, two, 2);
  }
  return r;
}

static O42Value
fn_rank_avg (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double x, order = 0, sum = 0;
  guint before = 0, ties = 0;

  ARG_NUMBER (0, x);
  if (n >= 3) ARG_NUMBER (2, order);
  if (!collect_numbers (ctx, args + 1, 1, &values, &err))
    return o42_value_error (err);
  for (guint i = 0; i < values->len; i++)
    {
      double v = g_array_index (values, double, i);
      if (v == x) ties++;
      else if (order == 0 ? v > x : v < x) before++;
    }
  g_array_free (values, TRUE);
  if (ties == 0) return o42_value_error (O42_ERR_NA);
  for (guint k = 0; k < ties; k++) sum += before + 1 + k;
  return o42_value_number (sum / ties);
}

static O42Value
fn_gamma (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  if (x == floor (x) && x <= 0) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (tgamma (x));
}

static O42Value
fn_gauss (O42EvalContext *ctx, O42Operand *args, int n)
{
  double z;
  (void) n;
  ARG_NUMBER (0, z);
  return o42_value_number (normal_cdf (z) - 0.5);
}

static O42Value
fn_phi (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x;
  (void) n;
  ARG_NUMBER (0, x);
  return o42_value_number (exp (-x * x / 2) / sqrt (2 * G_PI));
}

static O42Value
fn_skew_p (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double mean, ssd, m3 = 0, var;
  guint count;

  if (!collect_numbers (ctx, args, n, &values, &err))
    return o42_value_error (err);
  count = values->len;
  if (count < 3) { g_array_free (values, TRUE); return o42_value_error (O42_ERR_DIV0); }
  moments (values, &mean, &ssd);
  var = ssd / count;
  if (var == 0) { g_array_free (values, TRUE); return o42_value_error (O42_ERR_DIV0); }
  for (guint i = 0; i < count; i++)
    {
      double d = g_array_index (values, double, i) - mean;
      m3 += d * d * d;
    }
  g_array_free (values, TRUE);
  return o42_value_number ((m3 / count) / pow (var, 1.5));
}

static O42Value
fn_confidence_t (O42EvalContext *ctx, O42Operand *args, int n)
{
  double alpha, sd, size;
  (void) n;
  ARG_NUMBER (0, alpha);
  ARG_NUMBER (1, sd);
  ARG_NUMBER (2, size);
  size = floor (size);
  if (alpha <= 0 || alpha >= 1 || sd <= 0 || size < 2) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (invert_cdf (t_cdf, 1 - alpha / 2, size - 1, 0, 0, 1e6) * sd / sqrt (size));
}

static O42Value
fn_binom_dist_range (O42EvalContext *ctx, O42Operand *args, int n)
{
  double trials, p, s1, s2, sum = 0;
  ARG_NUMBER (0, trials);
  ARG_NUMBER (1, p);
  ARG_NUMBER (2, s1);
  s2 = s1;
  if (n >= 4) ARG_NUMBER (3, s2);
  trials = floor (trials); s1 = floor (s1); s2 = floor (s2);
  if (trials < 0 || p < 0 || p > 1 || s1 < 0 || s2 < s1 || s2 > trials) return o42_value_error (O42_ERR_NUM);
  for (double k = s1; k <= s2; k++)
    sum += exp (lchoose (trials, k)) * pow (p, k) * pow (1 - p, trials - k);
  return o42_value_number (sum);
}

static O42Value
fn_permutationa (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a, k;
  (void) n;
  ARG_NUMBER (0, a);
  ARG_NUMBER (1, k);
  a = floor (a); k = floor (k);
  if (a < 0 || k < 0) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (pow (a, k));
}

static O42Value
fn_combina (O42EvalContext *ctx, O42Operand *args, int n)
{
  double a, k;
  (void) n;
  ARG_NUMBER (0, a);
  ARG_NUMBER (1, k);
  a = floor (a); k = floor (k);
  if (a < 0 || k < 0 || (a == 0 && k > 0)) return o42_value_error (O42_ERR_NUM);
  if (k == 0) return o42_value_number (1);
  return o42_value_number (floor (exp (lchoose (a + k - 1, k)) + 0.5));
}

/* ---- math extras ---- */

static O42Value
fn_multinomial (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values;
  O42ErrorCode err = O42_ERR_VALUE;
  double total = 0, lg = 0;

  if (!collect_numbers (ctx, args, n, &values, &err))
    return o42_value_error (err);
  for (guint i = 0; i < values->len; i++)
    {
      double v = floor (g_array_index (values, double, i));
      if (v < 0) { g_array_free (values, TRUE); return o42_value_error (O42_ERR_NUM); }
      total += v;
      lg -= lgamma (v + 1);
    }
  g_array_free (values, TRUE);
  return o42_value_number (floor (exp (lgamma (total + 1) + lg) + 0.5));
}

static O42Value
fn_seriessum (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *coeffs;
  O42ErrorCode err = O42_ERR_VALUE;
  double x, start, step, sum = 0;
  (void) n;

  ARG_NUMBER (0, x);
  ARG_NUMBER (1, start);
  ARG_NUMBER (2, step);
  if (!collect_numbers (ctx, args + 3, 1, &coeffs, &err))
    return o42_value_error (err);
  for (guint i = 0; i < coeffs->len; i++)
    sum += g_array_index (coeffs, double, i) * pow (x, start + i * step);
  g_array_free (coeffs, TRUE);
  return o42_value_number (sum);
}

static O42Value
fn_factdouble (O42EvalContext *ctx, O42Operand *args, int n)
{
  double x, r = 1;
  (void) n;
  ARG_NUMBER (0, x);
  x = floor (x);
  if (x < -1 || x > 300) return o42_value_error (O42_ERR_NUM);
  for (double k = x; k > 1; k -= 2) r *= k;
  return o42_value_number (r);
}

static O42Value
fn_arabic (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *text, *p;
  int total = 0, prev = 0;
  gboolean negative = FALSE;
  (void) n;
  ARG_TEXT (0, text);
  p = g_strstrip (text);
  if (*p == '-') { negative = TRUE; p++; }
  for (gsize i = strlen (p); i > 0; i--)
    {
      int v;
      switch (g_ascii_toupper (p[i - 1]))
        {
        case 'I': v = 1; break; case 'V': v = 5; break; case 'X': v = 10; break;
        case 'L': v = 50; break; case 'C': v = 100; break; case 'D': v = 500; break;
        case 'M': v = 1000; break;
        default: g_free (text); return o42_value_error (O42_ERR_VALUE);
        }
      if (v < prev) total -= v; else { total += v; prev = v; }
    }
  g_free (text);
  return o42_value_number (negative ? -total : total);
}

static O42Value
fn_base (O42EvalContext *ctx, O42Operand *args, int n)
{
  double number, radix, min_len = 0;
  const char *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  char buf[80];
  int len = 0;
  guint64 v;

  ARG_NUMBER (0, number);
  ARG_NUMBER (1, radix);
  if (n >= 3) ARG_NUMBER (2, min_len);
  number = floor (number); radix = floor (radix); min_len = floor (min_len);
  if (number < 0 || radix < 2 || radix > 36 || min_len < 0 || min_len > 255)
    return o42_value_error (O42_ERR_NUM);
  v = (guint64) number;
  do { buf[len++] = digits[v % (guint64) radix]; v /= (guint64) radix; } while (v > 0 && len < 70);
  while (len < min_len && len < 70) buf[len++] = '0';
  {
    char *out = g_new (char, len + 1);
    for (int i = 0; i < len; i++) out[i] = buf[len - 1 - i];
    out[len] = '\0';
    return o42_value_take (out);
  }
}

static O42Value
fn_decimal (O42EvalContext *ctx, O42Operand *args, int n)
{
  char *text;
  double radix, v = 0;
  (void) n;
  ARG_TEXT (0, text);
  ARG_NUMBER (1, radix);
  radix = floor (radix);
  if (radix < 2 || radix > 36) { g_free (text); return o42_value_error (O42_ERR_NUM); }
  for (const char *p = g_strstrip (text); *p; p++)
    {
      int d = g_ascii_isdigit (*p) ? *p - '0' : g_ascii_isalpha (*p) ? g_ascii_toupper (*p) - 'A' + 10 : 99;
      if (d >= radix) { g_free (text); return o42_value_error (O42_ERR_NUM); }
      v = v * radix + d;
    }
  g_free (text);
  return o42_value_number (v);
}

static O42Value
fn_trig_extra (O42EvalContext *ctx, O42Operand *args, int n, int which)
{
  double x, r;
  (void) n;
  ARG_NUMBER (0, x);
  switch (which)
    {
    case 0: if (x == 0) return o42_value_error (O42_ERR_DIV0); r = 1 / tan (x); break;
    case 1: if (x == 0) return o42_value_error (O42_ERR_DIV0); r = 1 / tanh (x); break;
    case 2: if (x == 0) return o42_value_error (O42_ERR_DIV0); r = 1 / sin (x); break;
    case 3: if (x == 0) return o42_value_error (O42_ERR_DIV0); r = 1 / sinh (x); break;
    case 4: r = 1 / cos (x); break;
    case 5: r = 1 / cosh (x); break;
    case 6: r = G_PI / 2 - atan (x); break;
    default:
      if (fabs (x) <= 1) return o42_value_error (O42_ERR_NUM);
      r = 0.5 * log ((x + 1) / (x - 1));
      break;
    }
  if (isnan (r) || isinf (r)) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (r);
}

static O42Value fn_cot   (O42EvalContext *c, O42Operand *a, int n) { return fn_trig_extra (c, a, n, 0); }
static O42Value fn_coth  (O42EvalContext *c, O42Operand *a, int n) { return fn_trig_extra (c, a, n, 1); }
static O42Value fn_csc   (O42EvalContext *c, O42Operand *a, int n) { return fn_trig_extra (c, a, n, 2); }
static O42Value fn_csch  (O42EvalContext *c, O42Operand *a, int n) { return fn_trig_extra (c, a, n, 3); }
static O42Value fn_sec   (O42EvalContext *c, O42Operand *a, int n) { return fn_trig_extra (c, a, n, 4); }
static O42Value fn_sech  (O42EvalContext *c, O42Operand *a, int n) { return fn_trig_extra (c, a, n, 5); }
static O42Value fn_acot  (O42EvalContext *c, O42Operand *a, int n) { return fn_trig_extra (c, a, n, 6); }
static O42Value fn_acoth (O42EvalContext *c, O42Operand *a, int n) { return fn_trig_extra (c, a, n, 7); }

/* ---- finance extras ---- */

static O42Value
fn_effect (O42EvalContext *ctx, O42Operand *args, int n)
{
  double rate, periods;
  (void) n;
  ARG_NUMBER (0, rate);
  ARG_NUMBER (1, periods);
  periods = floor (periods);
  if (rate <= 0 || periods < 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (pow (1 + rate / periods, periods) - 1);
}

static O42Value
fn_nominal (O42EvalContext *ctx, O42Operand *args, int n)
{
  double rate, periods;
  (void) n;
  ARG_NUMBER (0, rate);
  ARG_NUMBER (1, periods);
  periods = floor (periods);
  if (rate <= 0 || periods < 1) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (periods * (pow (1 + rate, 1 / periods) - 1));
}

static O42Value
fn_rri (O42EvalContext *ctx, O42Operand *args, int n)
{
  double nper, pv, fv;
  (void) n;
  ARG_NUMBER (0, nper);
  ARG_NUMBER (1, pv);
  ARG_NUMBER (2, fv);
  if (nper <= 0 || pv == 0 || fv / pv < 0) return o42_value_error (O42_ERR_NUM);
  return o42_value_number (pow (fv / pv, 1 / nper) - 1);
}

static O42Value
fn_pduration (O42EvalContext *ctx, O42Operand *args, int n)
{
  double rate, pv, fv;
  (void) n;
  ARG_NUMBER (0, rate);
  ARG_NUMBER (1, pv);
  ARG_NUMBER (2, fv);
  if (rate <= 0 || pv <= 0 || fv <= 0) return o42_value_error (O42_ERR_NUM);
  return o42_value_number ((log (fv) - log (pv)) / log (1 + rate));
}

/* CUMIPMT and CUMPRINC walk the schedule of an even-payment loan. */
static O42Value
fn_cumulative (O42EvalContext *ctx, O42Operand *args, int n, gboolean interest)
{
  double rate, nper, pv, start, end, type, pmt, balance, total = 0;
  (void) n;
  ARG_NUMBER (0, rate);
  ARG_NUMBER (1, nper);
  ARG_NUMBER (2, pv);
  ARG_NUMBER (3, start);
  ARG_NUMBER (4, end);
  ARG_NUMBER (5, type);
  if (rate <= 0 || nper <= 0 || pv <= 0 || start < 1 || end < start || end > nper || (type != 0 && type != 1))
    return o42_value_error (O42_ERR_NUM);
  pmt = -pv * rate / (1 - pow (1 + rate, -nper));
  if (type == 1) pmt /= 1 + rate;
  balance = pv;
  for (int k = 1; k <= (int) end; k++)
    {
      double ipmt, ppmt;
      if (type == 1 && k == 1)
        ipmt = 0;
      else
        ipmt = -balance * rate;
      ppmt = pmt - ipmt;
      if (k >= (int) start)
        total += interest ? ipmt : ppmt;
      balance += ppmt;
    }
  return o42_value_number (total);
}

static O42Value fn_cumipmt  (O42EvalContext *c, O42Operand *a, int n) { return fn_cumulative (c, a, n, TRUE); }
static O42Value fn_cumprinc (O42EvalContext *c, O42Operand *a, int n) { return fn_cumulative (c, a, n, FALSE); }

static double
xnpv_at (GArray *values, GArray *dates, double rate)
{
  double d0 = g_array_index (dates, double, 0), sum = 0;
  for (guint i = 0; i < values->len; i++)
    sum += g_array_index (values, double, i) / pow (1 + rate, (g_array_index (dates, double, i) - d0) / 365);
  return sum;
}

static O42Value
fn_xnpv (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values, *dates;
  O42ErrorCode err = O42_ERR_VALUE;
  double rate, result;
  (void) n;
  ARG_NUMBER (0, rate);
  if (!collect_pairs (ctx, &args[1], &args[2], &values, &dates, &err))
    return o42_value_error (err);
  if (values->len == 0 || rate <= -1)
    { g_array_free (values, TRUE); g_array_free (dates, TRUE); return o42_value_error (O42_ERR_NUM); }
  result = xnpv_at (values, dates, rate);
  g_array_free (values, TRUE);
  g_array_free (dates, TRUE);
  return o42_value_number (result);
}

static O42Value
fn_xirr (O42EvalContext *ctx, O42Operand *args, int n)
{
  GArray *values, *dates;
  O42ErrorCode err = O42_ERR_VALUE;
  double guess = 0.1, r;
  gboolean found = FALSE;

  if (n >= 3) ARG_NUMBER (2, guess);
  if (!collect_pairs (ctx, &args[0], &args[1], &values, &dates, &err))
    return o42_value_error (err);
  if (values->len < 2)
    { g_array_free (values, TRUE); g_array_free (dates, TRUE); return o42_value_error (O42_ERR_NUM); }
  r = guess;
  for (int iter = 0; iter < 100; iter++)
    {
      double f = xnpv_at (values, dates, r);
      double h = 1e-6, df = (xnpv_at (values, dates, r + h) - f) / h;
      double next;
      if (df == 0) break;
      next = r - f / df;
      if (next <= -1) next = (r - 1) / 2;
      if (fabs (next - r) < 1e-10) { r = next; found = TRUE; break; }
      r = next;
    }
  g_array_free (values, TRUE);
  g_array_free (dates, TRUE);
  return found ? o42_value_number (r) : o42_value_error (O42_ERR_NUM);
}

static O42Value
fn_dollarde (O42EvalContext *ctx, O42Operand *args, int n)
{
  double d, fraction, whole, part;
  int digits;
  (void) n;
  ARG_NUMBER (0, d);
  ARG_NUMBER (1, fraction);
  fraction = floor (fraction);
  if (fraction < 1) return o42_value_error (O42_ERR_NUM);
  digits = (int) ceil (log10 (fraction));
  whole = trunc (d);
  part = (d - whole) * pow (10, digits);
  return o42_value_number (whole + part / fraction);
}

static O42Value
fn_dollarfr (O42EvalContext *ctx, O42Operand *args, int n)
{
  double d, fraction, whole, part;
  int digits;
  (void) n;
  ARG_NUMBER (0, d);
  ARG_NUMBER (1, fraction);
  fraction = floor (fraction);
  if (fraction < 1) return o42_value_error (O42_ERR_NUM);
  digits = (int) ceil (log10 (fraction));
  whole = trunc (d);
  part = (d - whole) * fraction / pow (10, digits);
  return o42_value_number (whole + part);
}

static const O42Function FUNCTIONS[] = {
  { "ABS", 1, 1, fn_abs },
  { "ACOS", 1, 1, fn_acos },
  { "ACOSH", 1, 1, fn_acosh },
  { "ACOT", 1, 1, fn_acot },
  { "ACOTH", 1, 1, fn_acoth },
  { "ADDRESS", 2, 5, fn_address },
  { "ADTEST", 1, -1, fn_adtest },
  { "AND", 1, -1, fn_and },
  { "ARABIC", 1, 1, fn_arabic },
  { "AREAS", 1, 1, fn_areas },
  { "ARRAYTOTEXT", 1, 2, fn_arraytotext },
  { "ASCENSIONTHURSDAY", 1, 1, fn_ascensionthursday },
  { "ASHWEDNESDAY", 1, 1, fn_ashwednesday },
  { "ASIN", 1, 1, fn_asin },
  { "ASINH", 1, 1, fn_asinh },
  { "ATAN", 1, 1, fn_atan },
  { "ATAN2", 2, 2, fn_atan2 },
  { "ATANH", 1, 1, fn_atanh },
  { "AVERAGE", 1, -1, fn_average },
  { "AVERAGEA", 1, -1, fn_averagea },
  { "AVERAGEIF", 2, 3, fn_averageif },
  { "AVERAGEIFS", 3, -1, fn_averageifs },
  { "BASE", 2, 3, fn_base },
  { "BETA", 2, 2, fn_beta },
  { "BETA.DIST", 4, 6, fn_beta_dist },
  { "BETALN", 2, 2, fn_betaln },
  { "BINOM.DIST.RANGE", 3, 4, fn_binom_dist_range },
  { "BINOM.INV", 3, 3, fn_critbinom },
  { "BYCOL", 2, 2, fn_let_stub },
  { "BYROW", 2, 2, fn_let_stub },
  { "CAUCHY", 2, 3, fn_cauchy },
  { "CEIL", 1, 1, fn_ceil },
  { "CEILING", 1, 2, fn_ceiling },
  { "CEILING.MATH", 1, 3, fn_ceiling_math },
  { "CEILING.PRECISE", 1, 2, fn_ceiling_precise },
  { "CHISQ.DIST", 3, 3, fn_chisq_dist },
  { "CHISQ.INV", 2, 2, fn_chisq_inv },
  { "CHISQ.TEST", 2, 2, fn_chitest },
  { "CHISQDIST", 2, 3, fn_chisqdist },
  { "CHISQINV", 2, 2, fn_chisqinv },
  { "CHITEST", 2, 2, fn_chitest },
  { "CHOOSE", 2, -1, fn_choose },
  { "COLUMN", 0, 1, fn_column },
  { "COMBIN", 2, 2, fn_combin },
  { "COMBINA", 2, 2, fn_combina },
  { "CONFIDENCE.T", 3, 3, fn_confidence_t },
  { "CONVERT", 3, 3, fn_convert },
  { "COS", 1, 1, fn_cos },
  { "COSH", 1, 1, fn_cosh },
  { "COT", 1, 1, fn_cot },
  { "COTH", 1, 1, fn_coth },
  { "COUNT", 1, -1, fn_count },
  { "COUNTA", 1, -1, fn_counta },
  { "COUNTBLANK", 1, 1, fn_countblank },
  { "COUNTIF", 2, 2, fn_countif },
  { "COUNTIFS", 2, -1, fn_countifs },
  { "COVARIANCE.S", 2, 2, fn_covariance_s },
  { "CRITBINOM", 3, 3, fn_critbinom },
  { "CRONBACH", 2, -1, fn_cronbach },
  { "CSC", 1, 1, fn_csc },
  { "CSCH", 1, 1, fn_csch },
  { "CUMIPMT", 6, 6, fn_cumipmt },
  { "CUMPRINC", 6, 6, fn_cumprinc },
  { "DATE2JULIAN", 1, 1, fn_date2julian },
  { "DATE2UNIX", 1, 1, fn_date2unix },
  { "DAVERAGE", 3, 3, fn_daverage },
  { "DB", 4, 5, fn_db_depreciation },
  { "DCOUNT", 3, 3, fn_dcount },
  { "DCOUNTA", 3, 3, fn_dcounta },
  { "DDB", 4, 5, fn_ddb },
  { "DECIMAL", 2, 2, fn_decimal },
  { "DEGREES", 1, 1, fn_degrees },
  { "DGET", 3, 3, fn_dget },
  { "DMAX", 3, 3, fn_dmax },
  { "DMIN", 3, 3, fn_dmin },
  { "DOLLARDE", 2, 2, fn_dollarde },
  { "DOLLARFR", 2, 2, fn_dollarfr },
  { "DPRODUCT", 3, 3, fn_dproduct },
  { "DSTDEV", 3, 3, fn_dstdev },
  { "DSTDEVP", 3, 3, fn_dstdevp },
  { "DSUM", 3, 3, fn_dsum },
  { "DVAR", 3, 3, fn_dvar },
  { "DVARP", 3, 3, fn_dvarp },
  { "EASTERSUNDAY", 1, 1, fn_eastersunday },
  { "EFFECT", 2, 2, fn_effect },
  { "ERROR", 1, 1, fn_error },
  { "ERROR.TYPE", 1, 1, fn_error_type },
  { "EVEN", 1, 1, fn_even },
  { "EXP", 1, 1, fn_exp },
  { "EXPM1", 1, 1, fn_expm1 },
  { "EXPPOWDIST", 3, 3, fn_exppowdist },
  { "F.DIST", 4, 4, fn_f_dist },
  { "F.INV", 3, 3, fn_f_inv },
  { "F.TEST", 2, 2, fn_ftest },
  { "FACT", 1, 1, fn_fact },
  { "FACTDOUBLE", 1, 1, fn_factdouble },
  { "FALSE", 0, 0, fn_false },
  { "FILTER", 2, 3, fn_offset },
  { "FISHER", 1, 1, fn_fisher },
  { "FISHERINV", 1, 1, fn_fisherinv },
  { "FLOOR", 1, 2, fn_floor },
  { "FLOOR.MATH", 1, 3, fn_floor_math },
  { "FLOOR.PRECISE", 1, 2, fn_floor_precise },
  { "FORMULATEXT", 1, 1, fn_formulatext },
  { "FTEST", 2, 2, fn_ftest },
  { "FV", 3, 5, fn_fv },
  { "GAMMA", 1, 1, fn_gamma },
  { "GAUSS", 1, 1, fn_gauss },
  { "GCD", 1, -1, fn_gcd },
  { "GEOMDIST", 2, 3, fn_geomdist },
  { "GETENV", 1, 1, fn_getenv },
  { "GOODFRIDAY", 1, 1, fn_goodfriday },
  { "GROWTH", 1, 4, fn_offset },
  { "HLOOKUP", 3, 4, fn_hlookup },
  { "HYPERLINK", 1, 2, fn_hyperlink },
  { "HYPGEOM.DIST", 5, 5, fn_hypgeom_dist },
  { "HYPGEOMDIST", 4, 4, fn_hypgeomdist },
  { "IF", 2, 3, fn_if },
  { "IFERROR", 2, 2, fn_iferror },
  { "IFNA", 2, 2, fn_ifna },
  { "IFS", 2, -1, fn_ifs_logic },
  { "IMARCCOS", 1, 1, fn_imarccos },
  { "IMARCCOSH", 1, 1, fn_imarccosh },
  { "IMARCCOT", 1, 1, fn_imarccot },
  { "IMARCCOTH", 1, 1, fn_imarccoth },
  { "IMARCCSC", 1, 1, fn_imarccsc },
  { "IMARCCSCH", 1, 1, fn_imarccsch },
  { "IMARCSEC", 1, 1, fn_imarcsec },
  { "IMARCSECH", 1, 1, fn_imarcsech },
  { "IMARCSIN", 1, 1, fn_imarcsin },
  { "IMARCSINH", 1, 1, fn_imarcsinh },
  { "IMARCTAN", 1, 1, fn_imarctan },
  { "IMARCTANH", 1, 1, fn_imarctanh },
  { "IMCOSH", 1, 1, fn_imcosh },
  { "IMCOT", 1, 1, fn_imcot },
  { "IMCOTH", 1, 1, fn_imcoth },
  { "IMCSC", 1, 1, fn_imcsc },
  { "IMCSCH", 1, 1, fn_imcsch },
  { "IMINV", 1, 1, fn_iminv },
  { "IMNEG", 1, 1, fn_imneg },
  { "IMSEC", 1, 1, fn_imsec },
  { "IMSECH", 1, 1, fn_imsech },
  { "IMSINH", 1, 1, fn_imsinh },
  { "IMTAN", 1, 1, fn_imtan },
  { "IMTANH", 1, 1, fn_imtanh },
  { "INDEX", 2, 3, fn_index },
  { "INDIRECT", 1, 2, fn_indirect },
  { "INT", 1, 1, fn_int },
  { "IPMT", 4, 6, fn_ipmt },
  { "IRR", 1, 2, fn_irr },
  { "ISBLANK", 1, 1, fn_isblank },
  { "ISERR", 1, 1, fn_iserr },
  { "ISERROR", 1, 1, fn_iserror },
  { "ISEVEN", 1, 1, fn_iseven },
  { "ISFORMULA", 1, 1, fn_isformula },
  { "ISLOGICAL", 1, 1, fn_islogical },
  { "ISNA", 1, 1, fn_isna },
  { "ISNONTEXT", 1, 1, fn_isnontext },
  { "ISNUMBER", 1, 1, fn_isnumber },
  { "ISO.CEILING", 1, 2, fn_ceiling },
  { "ISODD", 1, 1, fn_isodd },
  { "ISOMITTED", 1, 1, fn_let_stub },
  { "ISOYEAR", 1, 1, fn_isoyear },
  { "ISPMT", 4, 4, fn_ispmt },
  { "ISPRIME", 1, 1, fn_isprime },
  { "ISREF", 1, 1, fn_isref },
  { "ISTEXT", 1, 1, fn_istext },
  { "ITHPRIME", 1, 1, fn_ithprime },
  { "KURTP", 1, -1, fn_kurtp },
  { "LAMBDA", 1, -1, fn_let_stub },
  { "LANDAU", 1, 1, fn_landau },
  { "LAPLACE", 2, 2, fn_laplace },
  { "LCM", 1, -1, fn_lcm },
  { "LET", 3, -1, fn_let_stub },
  { "LN", 1, 1, fn_ln },
  { "LN1P", 1, 1, fn_ln1p },
  { "LOG", 1, 2, fn_log },
  { "LOG10", 1, 1, fn_log10 },
  { "LOGEST", 1, 4, fn_offset },
  { "LOGINV", 3, 3, fn_loginv },
  { "LOGISTIC", 2, 2, fn_logistic },
  { "LOGNORM.DIST", 4, 4, fn_lognorm_dist },
  { "LOGNORM.INV", 3, 3, fn_loginv },
  { "LOOKUP", 2, 3, fn_lookup_vector },
  { "MAKEARRAY", 3, 3, fn_let_stub },
  { "MAP", 2, -1, fn_let_stub },
  { "MATCH", 2, 3, fn_match },
  { "MAX", 1, -1, fn_max },
  { "MAXA", 1, -1, fn_maxa },
  { "MAXIFS", 3, -1, fn_maxifs },
  { "MDETERM", 1, 1, fn_mdeterm },
  { "MEDIAN", 1, -1, fn_median },
  { "MIN", 1, -1, fn_min },
  { "MINA", 1, -1, fn_mina },
  { "MINIFS", 3, -1, fn_minifs },
  { "MIRR", 3, 3, fn_mirr },
  { "MOD", 2, 2, fn_mod },
  { "MROUND", 2, 2, fn_mround },
  { "MULTINOMIAL", 1, -1, fn_multinomial },
  { "N", 1, 1, fn_n },
  { "NA", 0, 0, fn_na },
  { "NEGBINOM.DIST", 4, 4, fn_negbinom_dist },
  { "NEGBINOMDIST", 3, 3, fn_negbinomdist },
  { "NETWORKDAYS.INTL", 2, 4, fn_networkdays_intl },
  { "NOMINAL", 2, 2, fn_nominal },
  { "NORM.S.DIST", 1, 2, fn_norm_s_dist },
  { "NORMALTEST", 1, -1, fn_normaltest },
  { "NOT", 1, 1, fn_not },
  { "NPER", 3, 5, fn_nper },
  { "NPV", 2, -1, fn_npv },
  { "NT_D", 1, 1, fn_nt_d },
  { "NT_MU", 1, 1, fn_nt_mu },
  { "NT_OMEGA", 1, 1, fn_nt_omega },
  { "NT_PHI", 1, 1, fn_nt_phi },
  { "NT_PI", 1, 1, fn_nt_pi },
  { "NT_RADICAL", 1, 1, fn_nt_radical },
  { "NT_SIGMA", 1, 1, fn_nt_sigma },
  { "NUMBERVALUE", 1, 3, fn_numbervalue },
  { "ODD", 1, 1, fn_odd },
  { "OFFSET", 3, 5, fn_offset },
  { "OR", 1, -1, fn_or },
  { "OWENT", 2, 2, fn_owent },
  { "PARETO", 3, 3, fn_pareto },
  { "PDURATION", 3, 3, fn_pduration },
  { "PENTECOSTSUNDAY", 1, 1, fn_pentecostsunday },
  { "PERCENTILE.EXC", 2, 2, fn_percentile_exc },
  { "PERCENTRANK", 2, 3, fn_percentrank },
  { "PERCENTRANK.EXC", 2, 3, fn_percentrank_exc },
  { "PERCENTRANK.INC", 2, 3, fn_percentrank },
  { "PERMUT", 2, 2, fn_permut },
  { "PERMUTATIONA", 2, 2, fn_permutationa },
  { "PFACTOR", 1, 1, fn_pfactor },
  { "PHI", 1, 1, fn_phi },
  { "PI", 0, 0, fn_pi },
  { "PMT", 3, 5, fn_pmt },
  { "POWER", 2, 2, fn_power },
  { "PPMT", 4, 6, fn_ppmt },
  { "PROB", 3, 4, fn_prob },
  { "PRODUCT", 1, -1, fn_product },
  { "PV", 3, 5, fn_pv },
  { "QUARTILE.EXC", 2, 2, fn_quartile_exc },
  { "QUOTIENT", 2, 2, fn_quotient },
  { "RADIANS", 1, 1, fn_radians },
  { "RAND", 0, 0, fn_rand },
  { "RANDARRAY", 0, 5, fn_offset },
  { "RANDBETWEEN", 2, 2, fn_randbetween },
  { "RANK.AVG", 2, 3, fn_rank_avg },
  { "RATE", 3, 6, fn_rate },
  { "RAYLEIGH", 2, 2, fn_rayleigh },
  { "REDUCE", 3, 3, fn_let_stub },
  { "ROMAN", 1, 2, fn_roman },
  { "ROUND", 1, 2, fn_round },
  { "ROUNDDOWN", 1, 2, fn_rounddown },
  { "ROUNDUP", 1, 2, fn_roundup },
  { "ROW", 0, 1, fn_row },
  { "RRI", 3, 3, fn_rri },
  { "SCAN", 3, 3, fn_let_stub },
  { "SEC", 1, 1, fn_sec },
  { "SECH", 1, 1, fn_sech },
  { "SEQUENCE", 1, 4, fn_offset },
  { "SERIESSUM", 4, 4, fn_seriessum },
  { "SHEET", 0, 1, fn_sheet },
  { "SHEETS", 0, 1, fn_sheets },
  { "SIGN", 1, 1, fn_sign },
  { "SIN", 1, 1, fn_sin },
  { "SINH", 1, 1, fn_sinh },
  { "SKEW.P", 1, -1, fn_skew_p },
  { "SKEWP", 1, -1, fn_skewp },
  { "SLN", 3, 3, fn_sln },
  { "SNORM.DIST.RANGE", 2, 2, fn_snorm_dist_range },
  { "SORT", 1, 4, fn_offset },
  { "SORTBY", 2, 4, fn_offset },
  { "SQRT", 1, 1, fn_sqrt },
  { "SQRTPI", 1, 1, fn_sqrtpi },
  { "SSMEDIAN", 1, 2, fn_ssmedian },
  { "STDEVA", 1, -1, fn_stdeva },
  { "STDEVPA", 1, -1, fn_stdevpa },
  { "STEYX", 2, 2, fn_steyx },
  { "SUBTOTAL", 2, -1, fn_subtotal },
  { "SUM", 1, -1, fn_sum },
  { "SUMA", 1, -1, fn_suma },
  { "SUMIF", 2, 3, fn_sumif },
  { "SUMIFS", 3, -1, fn_sumifs },
  { "SUMPRODUCT", 1, -1, fn_sumproduct },
  { "SUMSQ", 1, -1, fn_sumsq },
  { "SUMX2MY2", 2, 2, fn_sumx2my2 },
  { "SUMX2PY2", 2, 2, fn_sumx2py2 },
  { "SUMXMY2", 2, 2, fn_sumxmy2 },
  { "SWITCH", 3, -1, fn_switch },
  { "SYD", 4, 4, fn_syd },
  { "T", 1, 1, fn_t },
  { "T.DIST", 3, 3, fn_t_dist },
  { "T.DIST.2T", 2, 2, fn_t_dist_2t },
  { "T.DIST.RT", 2, 2, fn_t_dist_rt },
  { "T.INV", 2, 2, fn_t_inv },
  { "T.TEST", 4, 4, fn_ttest },
  { "TAN", 1, 1, fn_tan },
  { "TANH", 1, 1, fn_tanh },
  { "TEXTAFTER", 2, 3, fn_textafter },
  { "TEXTBEFORE", 2, 3, fn_textbefore },
  { "TEXTJOIN", 3, -1, fn_textjoin },
  { "TREND", 1, 4, fn_offset },
  { "TRUE", 0, 0, fn_true },
  { "TRUNC", 1, 2, fn_trunc },
  { "TTEST", 4, 4, fn_ttest },
  { "TYPE", 1, 1, fn_type },
  { "UNICHAR", 1, 1, fn_unichar },
  { "UNICODE", 1, 1, fn_unicode },
  { "UNIQUE", 1, 3, fn_offset },
  { "UNIX2DATE", 1, 1, fn_unix2date },
  { "VALUETOTEXT", 1, 2, fn_valuetotext },
  { "VARA", 1, -1, fn_vara },
  { "VARPA", 1, -1, fn_varpa },
  { "VDB", 5, 7, fn_vdb },
  { "VLOOKUP", 3, 4, fn_vlookup },
  { "WORKDAY.INTL", 2, 4, fn_workday_intl },
  { "XIRR", 2, 3, fn_xirr },
  { "XLOOKUP", 3, 6, fn_xlookup },
  { "XMATCH", 2, 4, fn_xmatch },
  { "XNPV", 3, 3, fn_xnpv },
  { "XOR", 1, -1, fn_xor },
  { "Z.TEST", 2, 3, fn_ztest },
  { "ZTEST", 2, 3, fn_ztest },
};

/* The families that live in files of their own, each table ended by a
 * NULL name. */
static const O42Function *const FAMILY_FUNCS[] = {
  O42_FUNCS_STATISTICS,
  O42_FUNCS_DATES,
  O42_FUNCS_TEXT,
  O42_FUNCS_HDATE,
  O42_FUNCS_ENGINEERING,
  O42_FUNCS_DISTRIBUTIONS,
  O42_FUNCS_FINANCE,
  O42_FUNCS_INFO,
  O42_FUNCS_RANDOM,
  O42_FUNCS_BESSEL,
  O42_FUNCS_OPTIONS
};

static const O42FunctionHelp *const FAMILY_HELP[] = {
  O42_HELP_STATISTICS,
  O42_HELP_DATES,
  O42_HELP_TEXT,
  O42_HELP_HDATE,
  O42_HELP_ENGINEERING,
  O42_HELP_DISTRIBUTIONS,
  O42_HELP_FINANCE,
  O42_HELP_INFO,
  O42_HELP_RANDOM,
  O42_HELP_BESSEL,
  O42_HELP_OPTIONS
};

static int
compare_functions (gconstpointer a, gconstpointer b)
{
  return g_ascii_strcasecmp (((const O42Function *) a)->name,
                             ((const O42Function *) b)->name);
}

/* Every function there is, in one sorted array: this file's table and
 * the families', gathered the first time anything asks.  Sorting here
 * is what lets a family keep its own table in whatever order suits it. */
static const GArray *
all_functions (void)
{
  static GArray *gathered;

  if (gathered == NULL)
    {
      gathered = g_array_new (FALSE, FALSE, sizeof (O42Function));
      g_array_append_vals (gathered, FUNCTIONS, G_N_ELEMENTS (FUNCTIONS));
      for (guint i = 0; i < G_N_ELEMENTS (FAMILY_FUNCS); i++)
        for (const O42Function *f = FAMILY_FUNCS[i]; f->name != NULL; f++)
          g_array_append_val (gathered, *f);
      g_array_sort (gathered, compare_functions);
    }
  return gathered;
}

static const O42Function *
find_function (const char *name)
{
  const GArray *table = all_functions ();
  int lo = 0;
  int hi = (int) table->len - 1;

  while (lo <= hi)
    {
      int mid = (lo + hi) / 2;
      const O42Function *at = &g_array_index (table, O42Function, mid);
      int cmp = g_ascii_strcasecmp (name, at->name);

      if (cmp == 0) return at;
      if (cmp < 0)  hi = mid - 1;
      else          lo = mid + 1;
    }

  return NULL;
}

/* ---- Functions defined from outside ----------------------------------- */

typedef struct {
  char                *name;        /* upper case */
  int                  min_args, max_args;
  char                *signature;
  char                *summary;
  O42ExternalFunction  impl;
  gpointer             user;
} External;

static GHashTable *externals = NULL;   /* upper-case name -> External */
static GPtrArray  *all_names = NULL;   /* built-in and external, sorted; NULL when stale */

static void
external_free (gpointer data)
{
  External *e = data;
  g_free (e->name);
  g_free (e->signature);
  g_free (e->summary);
  g_free (e);
}

static External *
find_external (const char *name)
{
  char *upper;
  External *e;

  if (externals == NULL || name == NULL)
    return NULL;
  upper = g_ascii_strup (name, -1);
  e = g_hash_table_lookup (externals, upper);
  g_free (upper);
  return e;
}

void
o42_function_register_external (const char *name, int min_args, int max_args,
                                const char *signature, const char *summary,
                                O42ExternalFunction impl, gpointer user)
{
  External *e;

  g_return_if_fail (name != NULL && *name != '\0' && impl != NULL);
  if (externals == NULL)
    externals = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, external_free);
  e = g_new0 (External, 1);
  e->name = g_ascii_strup (name, -1);
  e->min_args = MAX (min_args, 0);
  e->max_args = max_args;
  e->signature = g_strdup (signature != NULL ? signature : e->name);
  e->summary = g_strdup (summary != NULL ? summary : "");
  e->impl = impl;
  e->user = user;
  g_hash_table_replace (externals, e->name, e);
  g_clear_pointer (&all_names, g_ptr_array_unref);
}

gboolean
o42_function_unregister_external (const char *name)
{
  External *e = find_external (name);
  if (e == NULL)
    return FALSE;
  g_hash_table_remove (externals, e->name);
  g_clear_pointer (&all_names, g_ptr_array_unref);
  return TRUE;
}

gboolean
o42_function_is_external (const char *name)
{
  return find_external (name) != NULL;
}

void
o42_operand_cell (O42EvalContext *ctx, const O42Operand *operand, int i, int j, O42Value *out)
{
  *out = operand_cell (ctx, operand, i, j);
}

void
o42_operand_dims (const O42Operand *operand, int *rows, int *cols)
{
  operand_dims (operand, rows, cols);
}

/* The functions that answer with a whole rectangle and are worked out
 * in o42_eval_array rather than through the table above.  They are
 * named here so that the Function Wizard lists them, so that a
 * misspelt name can still be told from a real one, and so that
 * anything asking what office42 knows gets the whole answer. */
static const struct {
  const char *name;
  const char *signature;
  const char *summary;
} ARRAY_FUNCTIONS[] = {
  { "FOURIER", "FOURIER(sequence, inverse, separate)", "Gnumeric's: the Fourier transform of a sequence, or its inverse." },
  { "FREQUENCY", "FREQUENCY(data, bins)", "How many values fall in each bin." },
  { "HPFILTER", "HPFILTER(sequence, lambda)", "Gnumeric's: Hodrick and Prescott's trend and what is left over." },
  { "INTERPOLATION", "INTERPOLATION(abscissae, ordinates, targets, method)", "Gnumeric's: the ordinates read off at the targets, six ways." },
  { "LEVERAGE", "LEVERAGE(array)", "Gnumeric's: how much each row of a design matrix pulls on the fit." },
  { "LINEST", "LINEST(known_y, known_x, const, stats)", "The least-squares line, with its statistics." },
  { "LOGFIT", "LOGFIT(known_ys, known_xs)", "Gnumeric's fit of y = a + b ln(sign (x - c)): sign, a, b, c and the residuals." },
  { "LOGREG", "LOGREG(known_ys, known_xs, const, stats)", "Gnumeric's: the least-squares fit of y = m ln(x) + b, with its statistics." },
  { "MINVERSE", "MINVERSE(array)", "The inverse of a square matrix." },
  { "MMULT", "MMULT(array1, array2)", "The product of two matrices." },
  { "PERIODOGRAM", "PERIODOGRAM(ordinates, filter, abscissae, method, number)", "Gnumeric's: how much of each frequency is in a series." },
  { "TRANSPOSE", "TRANSPOSE(array)", "A rectangle turned on its side." },
  { "CHOOSECOLS", "CHOOSECOLS(array, col1, col2, ...)", "The columns named, in the order named." },
  { "CHOOSEROWS", "CHOOSEROWS(array, row1, row2, ...)", "The rows named, in the order named." },
  { "TAKE", "TAKE(array, rows, cols)", "So many rows and columns from the near end, or the far one." },
  { "DROP", "DROP(array, rows, cols)", "The rectangle with so many rows and columns left off." },
  { "TEXTSPLIT", "TEXTSPLIT(text, across, down)", "A text cut into a rectangle at its delimiters." },
  { "MODE.MULT", "MODE.MULT(number1, number2, ...)", "Every value that turns up as often as the commonest." },
  { "CHOLESKY", "CHOLESKY(matrix)", "Gnumeric's: the lower triangle whose product with its transpose is the matrix." },
  { "EIGEN", "EIGEN(matrix)", "Gnumeric's: the eigenvalues of a symmetric matrix, each with its vector under it." },
  { "MPSEUDOINVERSE", "MPSEUDOINVERSE(matrix, tolerance)", "Gnumeric's: Moore and Penrose's inverse, which any matrix has." }
};

gboolean
o42_function_exists (const char *name)
{
  if (name == NULL)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (ARRAY_FUNCTIONS); i++)
    if (g_ascii_strcasecmp (name, ARRAY_FUNCTIONS[i].name) == 0)
      return TRUE;
  return find_function (name) != NULL || find_external (name) != NULL;
}

static int
compare_names (gconstpointer a, gconstpointer b)
{
  return g_ascii_strcasecmp (*(const char * const *) a, *(const char * const *) b);
}


const char * const *
o42_function_names (guint *n_names)
{
  if (all_names == NULL)
    {
      all_names = g_ptr_array_new ();
      {
        const GArray *table = all_functions ();

        for (guint i = 0; i < table->len; i++)
          g_ptr_array_add (all_names,
                           (gpointer) g_array_index (table, O42Function, i).name);
      }
      for (guint i = 0; i < G_N_ELEMENTS (ARRAY_FUNCTIONS); i++)
        g_ptr_array_add (all_names, (gpointer) ARRAY_FUNCTIONS[i].name);
      if (externals != NULL)
        {
          GHashTableIter it;
          gpointer key, value;
          g_hash_table_iter_init (&it, externals);
          while (g_hash_table_iter_next (&it, &key, &value))
            if (find_function (key) == NULL)
              g_ptr_array_add (all_names, ((External *) value)->name);
        }
      g_ptr_array_sort (all_names, compare_names);
    }

  if (n_names)
    *n_names = all_names->len;

  return (const char * const *) all_names->pdata;
}

/* What each function takes and does, for the Function Wizard.  Sorted by
 * name, as the function table is, and searched the same way. */
static const struct {
  const char *name;
  const char *signature;
  const char *summary;
} FUNCTION_HELP[] = {
  { "ABS", "ABS(number)", "The absolute value of a number." },
  { "ACOS", "ACOS(number)", "The arccosine, in radians." },
  { "ACOSH", "ACOSH(number)", "The inverse hyperbolic cosine." },
  { "ACOT", "ACOT(number)", "The arccotangent, in radians." },
  { "ACOTH", "ACOTH(number)", "The inverse hyperbolic cotangent." },
  { "ADDRESS", "ADDRESS(row, column, abs_num, a1, sheet)", "A cell reference as text." },
  { "ADTEST", "ADTEST(array)", "Anderson and Darling's test: the chance a sample this far from normal." },
  { "AND", "AND(logical1, logical2, ...)", "TRUE if every argument is TRUE." },
  { "ARABIC", "ARABIC(text)", "A Roman numeral as a number." },
  { "AREAS", "AREAS(reference)", "The number of areas in a reference: one." },
  { "ARRAYTOTEXT", "ARRAYTOTEXT(array, format)", "A rectangle as text, plainly or in braces." },
  { "ASCENSIONTHURSDAY", "ASCENSIONTHURSDAY(year)", "Ascension Thursday, thirty-nine days after Easter." },
  { "ASHWEDNESDAY", "ASHWEDNESDAY(year)", "Ash Wednesday, forty-six days before Easter." },
  { "ASIN", "ASIN(number)", "The arcsine, in radians." },
  { "ASINH", "ASINH(number)", "The inverse hyperbolic sine." },
  { "ATAN", "ATAN(number)", "The arctangent, in radians." },
  { "ATAN2", "ATAN2(x, y)", "The arctangent of y/x, in radians, in the right quadrant." },
  { "ATANH", "ATANH(number)", "The inverse hyperbolic tangent." },
  { "AVERAGE", "AVERAGE(number1, number2, ...)", "The arithmetic mean; text and blanks in a range are skipped." },
  { "AVERAGEA", "AVERAGEA(value1, value2, ...)", "The average, counting text as 0 and TRUE as 1." },
  { "AVERAGEIF", "AVERAGEIF(range, criteria, average_range)", "The mean of the cells that meet a condition." },
  { "AVERAGEIFS", "AVERAGEIFS(average_range, range1, criteria1, ...)", "The mean of the cells meeting every condition." },
  { "BASE", "BASE(number, radix, min_length)", "A number as text in another base." },
  { "BETA", "BETA(a, b)", "The beta function." },
  { "BETA.DIST", "BETA.DIST(x, alpha, beta, cumulative, A, B)", "The beta distribution, density or cumulative." },
  { "BETALN", "BETALN(a, b)", "The logarithm of the beta function." },
  { "BINOM.DIST.RANGE", "BINOM.DIST.RANGE(trials, probability_s, number_s, number_s2)", "The probability of a range of successes." },
  { "BINOM.INV", "BINOM.INV(trials, probability_s, alpha)", "The smallest value whose cumulative binomial distribution reaches alpha." },
  { "BYCOL", "BYCOL(array, lambda)", "Each column of an array through a function." },
  { "BYROW", "BYROW(array, lambda)", "Each row of an array through a function." },
  { "CAUCHY", "CAUCHY(x, a, cumulative)", "The Cauchy, Lorentz or Breit-Wigner distribution." },
  { "CEIL", "CEIL(number)", "Rounded up to the next whole number." },
  { "CEILING", "CEILING(number, significance)", "Rounds up, away from zero, to a multiple of significance." },
  { "CEILING.MATH", "CEILING.MATH(number, significance, mode)", "Rounds up to a multiple of the significance." },
  { "CEILING.PRECISE", "CEILING.PRECISE(number, significance)", "Rounded up towards plus infinity, whatever the sign." },
  { "CHISQ.DIST", "CHISQ.DIST(x, deg_freedom, cumulative)", "The chi-squared distribution, density or cumulative." },
  { "CHISQ.INV", "CHISQ.INV(probability, deg_freedom)", "The inverse of the left-tailed chi-squared distribution." },
  { "CHISQ.TEST", "CHISQ.TEST(actual_range, expected_range)", "The chi-squared test for independence." },
  { "CHISQDIST", "CHISQDIST(x, dof, cumulative)", "The chi-squared distribution, as Gnumeric names it." },
  { "CHISQINV", "CHISQINV(p, dof)", "The value of chi-squared with the given probability below it." },
  { "CHITEST", "CHITEST(actual_range, expected_range)", "The chi-squared test for independence." },
  { "CHOOSE", "CHOOSE(index, value1, value2, ...)", "The value at a given position in the list." },
  { "COLUMN", "COLUMN(reference)", "The column number of a reference, or of this cell." },
  { "COMBIN", "COMBIN(n, k)", "How many ways k things can be chosen from n." },
  { "COMBINA", "COMBINA(number, number_chosen)", "Combinations with repetition." },
  { "CONFIDENCE.T", "CONFIDENCE.T(alpha, standard_dev, size)", "Half the width of a confidence interval, by Student's t." },
  { "CONVERT", "CONVERT(number, from_unit, to_unit)", "A measurement in other units: CONVERT(1,\"mi\",\"km\")." },
  { "COS", "COS(number)", "The cosine of an angle in radians." },
  { "COSH", "COSH(number)", "The hyperbolic cosine." },
  { "COT", "COT(number)", "The cotangent." },
  { "COTH", "COTH(number)", "The hyperbolic cotangent." },
  { "COUNT", "COUNT(value1, value2, ...)", "How many of the arguments are numbers." },
  { "COUNTA", "COUNTA(value1, value2, ...)", "How many of the arguments are not blank." },
  { "COUNTBLANK", "COUNTBLANK(range)", "How many cells in a range are blank." },
  { "COUNTIF", "COUNTIF(range, criteria)", "How many cells meet a condition such as \">5\" or \"a*\"." },
  { "COUNTIFS", "COUNTIFS(range1, criteria1, range2, criteria2, ...)", "How many cells meet every condition." },
  { "COVARIANCE.S", "COVARIANCE.S(array1, array2)", "The sample covariance." },
  { "CRITBINOM", "CRITBINOM(trials, probability, alpha)", "The smallest value whose cumulative binomial distribution reaches alpha." },
  { "CRONBACH", "CRONBACH(item1, item2, ...)", "Cronbach's alpha: how far the items measure one thing." },
  { "CSC", "CSC(number)", "The cosecant." },
  { "CSCH", "CSCH(number)", "The hyperbolic cosecant." },
  { "CUMIPMT", "CUMIPMT(rate, nper, pv, start_period, end_period, type)", "Interest paid between two periods of a loan." },
  { "CUMPRINC", "CUMPRINC(rate, nper, pv, start_period, end_period, type)", "Principal paid between two periods of a loan." },
  { "DATE2JULIAN", "DATE2JULIAN(date)", "The Julian day number of a date." },
  { "DATE2UNIX", "DATE2UNIX(date)", "A date as Unix time: seconds since the start of 1970." },
  { "DAVERAGE", "DAVERAGE(database, field, criteria)", "The mean of a field in the records that match the criteria." },
  { "DB", "DB(cost, salvage, life, period, month)", "Fixed-declining-balance depreciation." },
  { "DCOUNT", "DCOUNT(database, field, criteria)", "How many records match the criteria and hold a number in the field." },
  { "DCOUNTA", "DCOUNTA(database, field, criteria)", "How many records match the criteria and are not blank in the field." },
  { "DDB", "DDB(cost, salvage, life, period, factor)", "Depreciation by the double-declining balance method." },
  { "DECIMAL", "DECIMAL(text, radix)", "Text in another base as a number." },
  { "DEGREES", "DEGREES(angle)", "Radians to degrees." },
  { "DGET", "DGET(database, field, criteria)", "The field of the one record that matches the criteria." },
  { "DMAX", "DMAX(database, field, criteria)", "The largest value of a field in the matching records." },
  { "DMIN", "DMIN(database, field, criteria)", "The smallest value of a field in the matching records." },
  { "DOLLARDE", "DOLLARDE(fractional_dollar, fraction)", "A price quoted as a fraction, as a decimal." },
  { "DOLLARFR", "DOLLARFR(decimal_dollar, fraction)", "A decimal price as a fraction quote." },
  { "DPRODUCT", "DPRODUCT(database, field, criteria)", "The product of a field in the matching records." },
  { "DSTDEV", "DSTDEV(database, field, criteria)", "The sample standard deviation of a field in the matching records." },
  { "DSTDEVP", "DSTDEVP(database, field, criteria)", "Population standard deviation of matching records." },
  { "DSUM", "DSUM(database, field, criteria)", "The sum of a field in the records that match the criteria." },
  { "DVAR", "DVAR(database, field, criteria)", "The sample variance of a field in the matching records." },
  { "DVARP", "DVARP(database, field, criteria)", "Population variance of matching records." },
  { "EASTERSUNDAY", "EASTERSUNDAY(year)", "Easter Sunday, by the Gregorian computus." },
  { "EFFECT", "EFFECT(nominal_rate, npery)", "The effective annual interest rate." },
  { "ERROR", "ERROR(text)", "The error value that text names." },
  { "ERROR.TYPE", "ERROR.TYPE(error)", "A number for each kind of error value." },
  { "EVEN", "EVEN(number)", "Rounds away from zero to an even integer." },
  { "EXP", "EXP(number)", "e raised to a power." },
  { "EXPM1", "EXPM1(x)", "exp(x) - 1, keeping the digits a small x would lose." },
  { "EXPPOWDIST", "EXPPOWDIST(x, a, b)", "The density of the exponential power distribution." },
  { "F.DIST", "F.DIST(x, deg_freedom1, deg_freedom2, cumulative)", "The F distribution, density or cumulative." },
  { "F.INV", "F.INV(probability, deg_freedom1, deg_freedom2)", "The inverse of the left-tailed F distribution." },
  { "F.TEST", "F.TEST(array1, array2)", "The two-tailed F-test probability." },
  { "FACT", "FACT(number)", "The factorial." },
  { "FACTDOUBLE", "FACTDOUBLE(number)", "The double factorial." },
  { "FALSE", "FALSE()", "The logical value FALSE." },
  { "FILTER", "FILTER(array, include, if_empty)", "The rows (or columns) of an array where a condition holds." },
  { "FISHER", "FISHER(x)", "The Fisher transformation." },
  { "FISHERINV", "FISHERINV(y)", "The inverse of the Fisher transformation." },
  { "FLOOR", "FLOOR(number, significance)", "Rounds down, toward zero, to a multiple of significance." },
  { "FLOOR.MATH", "FLOOR.MATH(number, significance, mode)", "Rounds down to a multiple of the significance." },
  { "FLOOR.PRECISE", "FLOOR.PRECISE(number, significance)", "Rounded down towards minus infinity, whatever the sign." },
  { "FORMULATEXT", "FORMULATEXT(reference)", "What was typed into a cell, formula and all." },
  { "FTEST", "FTEST(array1, array2)", "The two-tailed F-test probability that the variances differ." },
  { "FV", "FV(rate, nper, pmt, pv, type)", "The future value of regular payments." },
  { "GAMMA", "GAMMA(number)", "The gamma function." },
  { "GAUSS", "GAUSS(z)", "The probability between the mean and z standard deviations." },
  { "GCD", "GCD(number1, number2, ...)", "The greatest common divisor." },
  { "GEOMDIST", "GEOMDIST(k, p, cumulative)", "How many failures before the first success." },
  { "GETENV", "GETENV(name)", "A value from the machine's environment." },
  { "GOODFRIDAY", "GOODFRIDAY(year)", "Good Friday, two days before Easter." },
  { "GROWTH", "GROWTH(known_ys, known_xs, new_xs, const)", "Values on the exponential curve fitted to the points." },
  { "HLOOKUP", "HLOOKUP(value, table, row_index, approximate)", "Finds a value in the top row and returns from that column." },
  { "HYPERLINK", "HYPERLINK(link, friendly_name)", "The text to show for a link." },
  { "HYPGEOM.DIST", "HYPGEOM.DIST(sample_s, number_sample, population_s, number_pop, cumulative)", "The hypergeometric distribution." },
  { "HYPGEOMDIST", "HYPGEOMDIST(successes, draws, population_successes, population)", "The hypergeometric distribution." },
  { "IF", "IF(condition, if_true, if_false)", "One value if a condition holds, another if not." },
  { "IFERROR", "IFERROR(value, if_error)", "A value, or something else if it is an error." },
  { "IFNA", "IFNA(value, if_na)", "A value, or something else if it is #N/A." },
  { "IFS", "IFS(test1, value1, test2, value2, ...)", "The first value whose test holds." },
  { "IMARCCOS", "IMARCCOS(complex)", "The inverse cosine of a complex number." },
  { "IMARCCOSH", "IMARCCOSH(complex)", "The inverse hyperbolic cosine of a complex number." },
  { "IMARCCOT", "IMARCCOT(complex)", "The inverse cotangent of a complex number." },
  { "IMARCCOTH", "IMARCCOTH(complex)", "The inverse hyperbolic cotangent of a complex number." },
  { "IMARCCSC", "IMARCCSC(complex)", "The inverse cosecant of a complex number." },
  { "IMARCCSCH", "IMARCCSCH(complex)", "The inverse hyperbolic cosecant of a complex number." },
  { "IMARCSEC", "IMARCSEC(complex)", "The inverse secant of a complex number." },
  { "IMARCSECH", "IMARCSECH(complex)", "The inverse hyperbolic secant of a complex number." },
  { "IMARCSIN", "IMARCSIN(complex)", "The inverse sine of a complex number." },
  { "IMARCSINH", "IMARCSINH(complex)", "The inverse hyperbolic sine of a complex number." },
  { "IMARCTAN", "IMARCTAN(complex)", "The inverse tangent of a complex number." },
  { "IMARCTANH", "IMARCTANH(complex)", "The inverse hyperbolic tangent of a complex number." },
  { "IMCOSH", "IMCOSH(complex)", "The hyperbolic cosine of a complex number." },
  { "IMCOT", "IMCOT(complex)", "The cotangent of a complex number." },
  { "IMCOTH", "IMCOTH(complex)", "The hyperbolic cotangent of a complex number." },
  { "IMCSC", "IMCSC(complex)", "The cosecant of a complex number." },
  { "IMCSCH", "IMCSCH(complex)", "The hyperbolic cosecant of a complex number." },
  { "IMINV", "IMINV(complex)", "One divided by a complex number." },
  { "IMNEG", "IMNEG(complex)", "A complex number with its sign turned round." },
  { "IMSEC", "IMSEC(complex)", "The secant of a complex number." },
  { "IMSECH", "IMSECH(complex)", "The hyperbolic secant of a complex number." },
  { "IMSINH", "IMSINH(complex)", "The hyperbolic sine of a complex number." },
  { "IMTAN", "IMTAN(complex)", "The tangent of a complex number." },
  { "IMTANH", "IMTANH(complex)", "The hyperbolic tangent of a complex number." },
  { "INDEX", "INDEX(range, row, column)", "The cell at a row and column of a range." },
  { "INDIRECT", "INDIRECT(text)", "The reference the text names." },
  { "INT", "INT(number)", "Rounds down to the nearest integer." },
  { "IPMT", "IPMT(rate, per, nper, pv, fv, type)", "The interest part of one payment." },
  { "IRR", "IRR(values, guess)", "The internal rate of return of a series of cash flows." },
  { "ISBLANK", "ISBLANK(value)", "TRUE for an empty cell." },
  { "ISERR", "ISERR(value)", "TRUE for any error but #N/A." },
  { "ISERROR", "ISERROR(value)", "TRUE for any error." },
  { "ISEVEN", "ISEVEN(number)", "TRUE for an even number." },
  { "ISFORMULA", "ISFORMULA(reference)", "Whether the cell holds a formula." },
  { "ISLOGICAL", "ISLOGICAL(value)", "TRUE for TRUE or FALSE." },
  { "ISNA", "ISNA(value)", "TRUE for #N/A." },
  { "ISNONTEXT", "ISNONTEXT(value)", "TRUE for anything that is not text." },
  { "ISNUMBER", "ISNUMBER(value)", "TRUE for a number." },
  { "ISO.CEILING", "ISO.CEILING(number, significance)", "Rounds up to a multiple of the significance." },
  { "ISODD", "ISODD(number)", "TRUE for an odd number." },
  { "ISOMITTED", "ISOMITTED(argument)", "Whether a LAMBDA argument was left out." },
  { "ISOYEAR", "ISOYEAR(date)", "The year the ISO 8601 week of a date belongs to." },
  { "ISPMT", "ISPMT(rate, period, nper, pv)", "Interest paid in a period of an even-principal loan." },
  { "ISPRIME", "ISPRIME(n)", "TRUE if the number is prime." },
  { "ISREF", "ISREF(value)", "TRUE if the value is a reference." },
  { "ISTEXT", "ISTEXT(value)", "TRUE for text." },
  { "ITHPRIME", "ITHPRIME(i)", "The ith prime number." },
  { "KURTP", "KURTP(number1, number2, ...)", "The kurtosis of a whole population." },
  { "LAMBDA", "LAMBDA(name1, ..., calculation)", "A function of its own, called at once or through LET." },
  { "LANDAU", "LANDAU(x)", "The Landau distribution, by its integral." },
  { "LAPLACE", "LAPLACE(x, a)", "The density of Laplace's distribution." },
  { "LCM", "LCM(number1, number2, ...)", "The least common multiple." },
  { "LET", "LET(name1, value1, ..., calculation)", "Names values for use in a calculation." },
  { "LN", "LN(number)", "The natural logarithm." },
  { "LN1P", "LN1P(x)", "log(1 + x), keeping the digits a small x would lose." },
  { "LOG", "LOG(number, base)", "The logarithm to a base, 10 if none is given." },
  { "LOG10", "LOG10(number)", "The logarithm to base 10." },
  { "LOGEST", "LOGEST(known_ys, known_xs, const, stats)", "The exponential curve through the points: m and b of y = b*m^x." },
  { "LOGINV", "LOGINV(probability, mean, standard_dev)", "The inverse lognormal distribution." },
  { "LOGISTIC", "LOGISTIC(x, a)", "The density of the logistic distribution." },
  { "LOGNORM.DIST", "LOGNORM.DIST(x, mean, standard_dev, cumulative)", "The lognormal distribution." },
  { "LOGNORM.INV", "LOGNORM.INV(probability, mean, standard_dev)", "The inverse of the cumulative lognormal distribution." },
  { "LOOKUP", "LOOKUP(value, lookup_vector, result_vector)", "Finds a value in one vector and returns from another." },
  { "MAKEARRAY", "MAKEARRAY(rows, columns, lambda)", "An array from a function of the row and column." },
  { "MAP", "MAP(array, ..., lambda)", "Every element of an array through a function." },
  { "MATCH", "MATCH(value, range, type)", "The position of a value in a range." },
  { "MAX", "MAX(number1, number2, ...)", "The largest number." },
  { "MAXA", "MAXA(value1, value2, ...)", "The largest value, counting text as 0 and TRUE as 1." },
  { "MAXIFS", "MAXIFS(max_range, range1, criteria1, ...)", "The largest value among the cells meeting every condition." },
  { "MDETERM", "MDETERM(array)", "The determinant of a square matrix." },
  { "MEDIAN", "MEDIAN(number1, number2, ...)", "The middle value." },
  { "MIN", "MIN(number1, number2, ...)", "The smallest number." },
  { "MINA", "MINA(value1, value2, ...)", "The smallest value, counting text as 0 and TRUE as 1." },
  { "MINIFS", "MINIFS(min_range, range1, criteria1, ...)", "The smallest value among the cells meeting every condition." },
  { "MIRR", "MIRR(values, finance_rate, reinvest_rate)", "The modified internal rate of return." },
  { "MOD", "MOD(number, divisor)", "The remainder, with the sign of the divisor." },
  { "MROUND", "MROUND(number, multiple)", "Rounds to the nearest multiple." },
  { "MULTINOMIAL", "MULTINOMIAL(number1, number2, ...)", "The multinomial of a set of numbers." },
  { "N", "N(value)", "A value as a number; text becomes 0." },
  { "NA", "NA()", "The error value #N/A." },
  { "NEGBINOM.DIST", "NEGBINOM.DIST(number_f, number_s, probability_s, cumulative)", "The negative binomial distribution." },
  { "NEGBINOMDIST", "NEGBINOMDIST(failures, successes, probability)", "The negative binomial distribution." },
  { "NETWORKDAYS.INTL", "NETWORKDAYS.INTL(start, end, weekend, holidays)", "Working days between two dates, with the weekend named." },
  { "NOMINAL", "NOMINAL(effect_rate, npery)", "The nominal annual interest rate." },
  { "NORM.S.DIST", "NORM.S.DIST(z, cumulative)", "The standard normal distribution." },
  { "NORMALTEST", "NORMALTEST(array)", "D'Agostino and Pearson's test for normality, from the skew and kurtosis." },
  { "NOT", "NOT(logical)", "The opposite of a logical value." },
  { "NPER", "NPER(rate, pmt, pv, fv, type)", "How many payments an investment takes." },
  { "NPV", "NPV(rate, value1, value2, ...)", "The net present value of a series of cash flows." },
  { "NT_D", "NT_D(n)", "How many divisors a number has." },
  { "NT_MU", "NT_MU(n)", "The Moebius mu function of a number." },
  { "NT_OMEGA", "NT_OMEGA(n)", "How many distinct primes go into a number." },
  { "NT_PHI", "NT_PHI(n)", "Euler's totient: how many below n are coprime to it." },
  { "NT_PI", "NT_PI(n)", "How many primes there are up to n." },
  { "NT_RADICAL", "NT_RADICAL(n)", "The product of the distinct primes of a number." },
  { "NT_SIGMA", "NT_SIGMA(n)", "What the divisors of a number add up to." },
  { "NUMBERVALUE", "NUMBERVALUE(text, decimal_separator, group_separator)", "Text as a number, ignoring spaces." },
  { "ODD", "ODD(number)", "Rounds away from zero to an odd integer." },
  { "OFFSET", "OFFSET(reference, rows, cols, height, width)", "A reference moved and resized from another." },
  { "OR", "OR(logical1, logical2, ...)", "TRUE if any argument is TRUE." },
  { "OWENT", "OWENT(h, a)", "Owen's T function." },
  { "PARETO", "PARETO(x, a, b)", "The density of Pareto's distribution." },
  { "PDURATION", "PDURATION(rate, pv, fv)", "Periods for an investment to reach a value." },
  { "PENTECOSTSUNDAY", "PENTECOSTSUNDAY(year)", "Pentecost, forty-nine days after Easter." },
  { "PERCENTILE.EXC", "PERCENTILE.EXC(array, k)", "The k-th percentile, exclusive." },
  { "PERCENTRANK", "PERCENTRANK(array, x, significance)", "The rank of a value as a fraction of the set." },
  { "PERCENTRANK.EXC", "PERCENTRANK.EXC(array, x, significance)", "The rank of a value, with the ends counted out." },
  { "PERCENTRANK.INC", "PERCENTRANK.INC(array, x, significance)", "The rank of a value as a fraction of the set, inclusive." },
  { "PERMUT", "PERMUT(n, k)", "How many ordered ways k things can be chosen from n." },
  { "PERMUTATIONA", "PERMUTATIONA(number, number_chosen)", "Permutations with repetition." },
  { "PFACTOR", "PFACTOR(n)", "The smallest prime that goes into a number." },
  { "PHI", "PHI(x)", "The standard normal density." },
  { "PI", "PI()", "3.14159265358979." },
  { "PMT", "PMT(rate, nper, pv, fv, type)", "The payment on a loan." },
  { "POWER", "POWER(number, power)", "A number raised to a power." },
  { "PPMT", "PPMT(rate, per, nper, pv, fv, type)", "The principal part of one payment." },
  { "PROB", "PROB(x_range, prob_range, lower, upper)", "The probability that a value lies between two limits." },
  { "PRODUCT", "PRODUCT(number1, number2, ...)", "Multiplies its arguments." },
  { "PV", "PV(rate, nper, pmt, fv, type)", "The present value of regular payments." },
  { "QUARTILE.EXC", "QUARTILE.EXC(array, quart)", "A quartile, exclusive." },
  { "QUOTIENT", "QUOTIENT(numerator, denominator)", "The integer part of a division." },
  { "RADIANS", "RADIANS(angle)", "Degrees to radians." },
  { "RAND", "RAND()", "A random number between 0 and 1." },
  { "RANDARRAY", "RANDARRAY(rows, columns, min, max, whole_number)", "An array of random numbers." },
  { "RANDBETWEEN", "RANDBETWEEN(bottom, top)", "A random integer in a range." },
  { "RANK.AVG", "RANK.AVG(number, ref, order)", "The rank of a number, averaged over ties." },
  { "RATE", "RATE(nper, pmt, pv, fv, type, guess)", "The interest rate per period of an annuity." },
  { "RAYLEIGH", "RAYLEIGH(x, sigma)", "The density of Rayleigh's distribution." },
  { "REDUCE", "REDUCE(initial, array, lambda)", "An array folded into one value." },
  { "ROMAN", "ROMAN(number, form)", "A number as a Roman numeral." },
  { "ROUND", "ROUND(number, digits)", "Rounds to a number of digits, halves away from zero." },
  { "ROUNDDOWN", "ROUNDDOWN(number, digits)", "Rounds toward zero." },
  { "ROUNDUP", "ROUNDUP(number, digits)", "Rounds away from zero." },
  { "ROW", "ROW(reference)", "The row number of a reference, or of this cell." },
  { "RRI", "RRI(nper, pv, fv)", "The rate that grows pv to fv over nper periods." },
  { "SCAN", "SCAN(initial, array, lambda)", "The running results of folding an array." },
  { "SEC", "SEC(number)", "The secant." },
  { "SECH", "SECH(number)", "The hyperbolic secant." },
  { "SEQUENCE", "SEQUENCE(rows, columns, start, step)", "An array of numbers in sequence." },
  { "SERIESSUM", "SERIESSUM(x, n, m, coefficients)", "A power series." },
  { "SHEET", "SHEET(reference)", "The number of the sheet, counting from one." },
  { "SHEETS", "SHEETS(reference)", "How many sheets the book has." },
  { "SIGN", "SIGN(number)", "1, 0 or -1 by the sign of a number." },
  { "SIN", "SIN(number)", "The sine of an angle in radians." },
  { "SINH", "SINH(number)", "The hyperbolic sine." },
  { "SKEW.P", "SKEW.P(number1, number2, ...)", "The skewness of a population." },
  { "SKEWP", "SKEWP(number1, number2, ...)", "The skew of a whole population." },
  { "SLN", "SLN(cost, salvage, life)", "Straight-line depreciation for one period." },
  { "SNORM.DIST.RANGE", "SNORM.DIST.RANGE(x1, x2)", "The chance a standard normal falls between the two." },
  { "SORT", "SORT(array, sort_index, sort_order, by_col)", "An array sorted by one of its columns." },
  { "SORTBY", "SORTBY(array, by_array, sort_order)", "An array sorted by another." },
  { "SQRT", "SQRT(number)", "The square root." },
  { "SQRTPI", "SQRTPI(number)", "The square root of a number times pi." },
  { "SSMEDIAN", "SSMEDIAN(array, interval)", "The median of grouped data, interpolated inside its bin." },
  { "STDEVA", "STDEVA(value1, value2, ...)", "Sample standard deviation, counting text as 0 and TRUE as 1." },
  { "STDEVPA", "STDEVPA(value1, value2, ...)", "Population standard deviation, counting text as 0 and TRUE as 1." },
  { "STEYX", "STEYX(known_ys, known_xs)", "The standard error of the predicted y for each x." },
  { "SUBTOTAL", "SUBTOTAL(function_num, ref1, ...)", "An aggregate chosen by number: 1 AVERAGE, 2 COUNT, 3 COUNTA, 4 MAX, 5 MIN, 6 PRODUCT, 7 STDEV, 8 STDEVP, 9 SUM, 10 VAR, 11 VARP." },
  { "SUM", "SUM(number1, number2, ...)", "Adds its arguments; text in a range is skipped." },
  { "SUMA", "SUMA(value1, value2, ...)", "The sum, counting text as 0 and TRUE as 1." },
  { "SUMIF", "SUMIF(range, criteria, sum_range)", "Adds the cells that meet a condition." },
  { "SUMIFS", "SUMIFS(sum_range, range1, criteria1, ...)", "Adds the cells meeting every condition." },
  { "SUMPRODUCT", "SUMPRODUCT(array1, array2, ...)", "Multiplies ranges cell by cell and adds the products." },
  { "SUMSQ", "SUMSQ(number1, number2, ...)", "The sum of the squares." },
  { "SUMX2MY2", "SUMX2MY2(array_x, array_y)", "The sum of the differences of squares." },
  { "SUMX2PY2", "SUMX2PY2(array_x, array_y)", "The sum of the sums of squares." },
  { "SUMXMY2", "SUMXMY2(array_x, array_y)", "The sum of the squares of differences." },
  { "SWITCH", "SWITCH(value, case1, result1, ..., default)", "The result matching a value, or the default." },
  { "SYD", "SYD(cost, salvage, life, period)", "Sum-of-years'-digits depreciation for one period." },
  { "T", "T(value)", "A value if it is text, otherwise empty text." },
  { "T.DIST", "T.DIST(x, deg_freedom, cumulative)", "Student's t distribution, density or cumulative." },
  { "T.DIST.2T", "T.DIST.2T(x, deg_freedom)", "Both tails of Student's t distribution." },
  { "T.DIST.RT", "T.DIST.RT(x, deg_freedom)", "The right tail of Student's t distribution." },
  { "T.INV", "T.INV(probability, deg_freedom)", "The inverse of the left-tailed t distribution." },
  { "T.TEST", "T.TEST(array1, array2, tails, type)", "Student's t-test probability." },
  { "TAN", "TAN(number)", "The tangent of an angle in radians." },
  { "TANH", "TANH(number)", "The hyperbolic tangent." },
  { "TEXTAFTER", "TEXTAFTER(text, delimiter, instance)", "The part of a text after the delimiter." },
  { "TEXTBEFORE", "TEXTBEFORE(text, delimiter, instance)", "The part of a text before the delimiter." },
  { "TEXTJOIN", "TEXTJOIN(delimiter, ignore_empty, text1, ...)", "Joins texts with a delimiter between them." },
  { "TREND", "TREND(known_ys, known_xs, new_xs, const)", "Values on the line fitted to the points." },
  { "TRUE", "TRUE()", "The logical value TRUE." },
  { "TRUNC", "TRUNC(number, digits)", "Cuts a number off at a number of digits." },
  { "TTEST", "TTEST(array1, array2, tails, type)", "Student's t-test probability: type 1 paired, 2 equal variance, 3 unequal." },
  { "TYPE", "TYPE(value)", "A number for the kind of value: 1, 2, 4, 16 or 64." },
  { "UNICHAR", "UNICHAR(number)", "The character with a Unicode code point." },
  { "UNICODE", "UNICODE(text)", "The code point of the first character." },
  { "UNIQUE", "UNIQUE(array, by_col, exactly_once)", "The distinct rows (or columns) of an array." },
  { "UNIX2DATE", "UNIX2DATE(t)", "Unix time as a date and time." },
  { "VALUETOTEXT", "VALUETOTEXT(value, format)", "A value as text, plainly or with its quotes." },
  { "VARA", "VARA(value1, value2, ...)", "Sample variance, counting text as 0 and TRUE as 1." },
  { "VARPA", "VARPA(value1, value2, ...)", "Population variance, counting text as 0 and TRUE as 1." },
  { "VDB", "VDB(cost, salvage, life, start, end, factor, no_switch)", "Declining-balance depreciation over part of a life." },
  { "VLOOKUP", "VLOOKUP(value, table, col_index, approximate)", "Finds a value in the first column and returns from that row." },
  { "WORKDAY.INTL", "WORKDAY.INTL(start, days, weekend, holidays)", "The date so many working days on, with the weekend named." },
  { "XIRR", "XIRR(values, dates, guess)", "The internal rate of return of dated cash flows." },
  { "XLOOKUP", "XLOOKUP(lookup, lookup_array, return_array, if_not_found, match_mode)", "Finds a value in one vector and returns the matching one from another." },
  { "XMATCH", "XMATCH(lookup_value, lookup_array, match_mode)", "The position of a value in a vector." },
  { "XNPV", "XNPV(rate, values, dates)", "The net present value of dated cash flows." },
  { "XOR", "XOR(logical1, logical2, ...)", "TRUE if an odd number of arguments are TRUE." },
  { "Z.TEST", "Z.TEST(array, x, sigma)", "The one-tailed probability of a z-test." },
  { "ZTEST", "ZTEST(array, x, sigma)", "The one-tailed probability of a z-test." },
};

gboolean
o42_function_help (const char *name, const char **signature, const char **summary)
{
  int lo = 0;
  int hi = (int) G_N_ELEMENTS (FUNCTION_HELP) - 1;

  if (name == NULL)
    return FALSE;

  for (guint i = 0; i < G_N_ELEMENTS (ARRAY_FUNCTIONS); i++)
    if (g_ascii_strcasecmp (name, ARRAY_FUNCTIONS[i].name) == 0)
      {
        if (signature) *signature = ARRAY_FUNCTIONS[i].signature;
        if (summary)   *summary = ARRAY_FUNCTIONS[i].summary;
        return TRUE;
      }

  /* The families keep their own help beside their own functions. */
  for (guint i = 0; i < G_N_ELEMENTS (FAMILY_HELP); i++)
    for (const O42FunctionHelp *h = FAMILY_HELP[i]; h->name != NULL; h++)
      if (g_ascii_strcasecmp (name, h->name) == 0)
        {
          if (signature) *signature = h->signature;
          if (summary)   *summary = h->summary;
          return TRUE;
        }

  while (lo <= hi)
    {
      int mid = (lo + hi) / 2;
      int cmp = g_ascii_strcasecmp (name, FUNCTION_HELP[mid].name);

      if (cmp == 0)
        {
          if (signature) *signature = FUNCTION_HELP[mid].signature;
          if (summary)   *summary = FUNCTION_HELP[mid].summary;
          return TRUE;
        }
      if (cmp < 0) hi = mid - 1;
      else         lo = mid + 1;
    }

  {
    const External *e = find_external (name);
    if (e != NULL)
      {
        if (signature) *signature = e->signature;
        if (summary)   *summary = e->summary;
        return TRUE;
      }
  }
  return FALSE;
}

/* ---------------------------------------------------------------------- */
/* Evaluation                                                              */
/* ---------------------------------------------------------------------- */

static O42Operand eval_operand (O42EvalContext *ctx, const O42Node *node);

/* One operator applied to two values, which it takes over. */
static O42Value
binary_values (O42Op op, O42Value a, O42Value b)
{
  O42Value result;
  double x = 0, y = 0;
  O42ErrorCode err = O42_ERR_VALUE;

  /* An error on either side is the answer, whatever the operator. */
  if (a.type == O42_VALUE_ERROR) { result = o42_value_copy (&a); goto done; }
  if (b.type == O42_VALUE_ERROR) { result = o42_value_copy (&b); goto done; }

  switch (op)
    {
    case O42_OP_CONCAT:
      {
        char *sa = o42_value_to_text (&a);
        char *sb = o42_value_to_text (&b);

        result = o42_value_take (g_strconcat (sa, sb, NULL));
        g_free (sa);
        g_free (sb);
        goto done;
      }

    case O42_OP_EQ: case O42_OP_NE: case O42_OP_LT:
    case O42_OP_GT: case O42_OP_LE: case O42_OP_GE:
      {
        int cmp = o42_value_compare (&a, &b);
        gboolean truth;

        switch (op)
          {
          case O42_OP_EQ: truth = (cmp == 0); break;
          case O42_OP_NE: truth = (cmp != 0); break;
          case O42_OP_LT: truth = (cmp < 0);  break;
          case O42_OP_GT: truth = (cmp > 0);  break;
          case O42_OP_LE: truth = (cmp <= 0); break;
          default:        truth = (cmp >= 0); break;
          }

        result = o42_value_bool (truth);
        goto done;
      }

    default:
      break;
    }

  if (!o42_value_to_number (&a, &x, &err) ||
      !o42_value_to_number (&b, &y, &err))
    {
      result = o42_value_error (err);
      goto done;
    }

  switch (op)
    {
    case O42_OP_ADD: result = o42_value_number (x + y); break;
    case O42_OP_SUB: result = o42_value_number (x - y); break;
    case O42_OP_MUL: result = o42_value_number (x * y); break;

    case O42_OP_DIV:
      result = (y == 0.0) ? o42_value_error (O42_ERR_DIV0)
                          : o42_value_number (x / y);
      break;

    case O42_OP_POW:
      {
        double p = pow (x, y);
        result = (isnan (p) || isinf (p)) ? o42_value_error (O42_ERR_NUM)
                                          : o42_value_number (p);
        break;
      }

    default:
      result = o42_value_error (O42_ERR_VALUE);
      break;
    }

done:
  o42_value_clear (&a);
  o42_value_clear (&b);
  return result;
}

static O42Value
eval_binary (O42EvalContext *ctx, const O42Node *node)
{
  O42Operand oa = eval_operand (ctx, node->as.op.a);
  O42Operand ob = eval_operand (ctx, node->as.op.b);
  O42Value a = operand_value (ctx, &oa);
  O42Value b = operand_value (ctx, &ob);

  operand_clear (&oa);
  operand_clear (&ob);
  return binary_values (node->as.op.op, a, b);
}

static O42Value
eval_unary (O42EvalContext *ctx, const O42Node *node)
{
  O42Operand oa = eval_operand (ctx, node->as.op.a);
  O42Value a = operand_value (ctx, &oa);
  O42Value result;
  double x = 0;
  O42ErrorCode err = O42_ERR_VALUE;

  operand_clear (&oa);

  if (a.type == O42_VALUE_ERROR)
    return a;

  if (!o42_value_to_number (&a, &x, &err))
    {
      o42_value_clear (&a);
      return o42_value_error (err);
    }

  switch (node->as.op.op)
    {
    case O42_OP_NEG:     result = o42_value_number (-x);      break;
    case O42_OP_PERCENT: result = o42_value_number (x / 100); break;
    case O42_OP_POS:
    default:             result = o42_value_number (x);       break;
    }

  o42_value_clear (&a);
  return result;
}

static O42Value
eval_call (O42EvalContext *ctx, const O42Node *node)
{
  const O42Function *fn = find_function (node->as.call.name);
  const External *ext = fn == NULL ? find_external (node->as.call.name) : NULL;
  int n_args = node->as.call.args ? (int) node->as.call.args->len : 0;
  int min_args = fn != NULL ? fn->min_args : ext != NULL ? ext->min_args : 0;
  int max_args = fn != NULL ? fn->max_args : ext != NULL ? ext->max_args : -1;
  O42Operand *operands;
  O42Value result;

  if (fn == NULL && ext == NULL)
    {
      /* A name bound to a lambda, called: LET(f, LAMBDA(x, x*2), f(3)). */
      if (let_scope != NULL)
        for (guint i = let_scope->len; i > 0; i--)
          {
            const LetBinding *b = g_ptr_array_index (let_scope, i - 1);
            if (b->operand.lambda != NULL && g_ascii_strcasecmp (b->name, node->as.call.name) == 0)
              {
                O42Operand *given = n_args > 0 ? g_new0 (O42Operand, n_args) : NULL;
                O42Operand r;
                O42Value v;
                for (int k = 0; k < n_args; k++)
                  given[k] = eval_operand (ctx, g_ptr_array_index (node->as.call.args, k));
                r = apply_lambda (ctx, b->operand.lambda, given, n_args);
                v = operand_value (ctx, &r);
                operand_clear (&r);
                for (int k = 0; k < n_args; k++)
                  operand_clear (&given[k]);
                g_free (given);
                return v;
              }
          }
      return o42_value_error (O42_ERR_NAME);
    }

  if (n_args < min_args || (max_args >= 0 && n_args > max_args))
    return o42_value_error (O42_ERR_VALUE);

  operands = (n_args > 0) ? g_new0 (O42Operand, n_args) : NULL;

  for (int i = 0; i < n_args; i++)
    operands[i] = eval_operand (ctx,
                                g_ptr_array_index (node->as.call.args, i));

  {
    /* A 3-D range, Sheet1:Sheet3!A1:B2, becomes one range per sheet, so
     * SUM and its kind add them up without knowing. */
    gboolean any_3d = FALSE;
    for (int i = 0; i < n_args; i++)
      if (operands[i].is_range && operands[i].sheet_last != NULL)
        any_3d = TRUE;
    if (any_3d)
      {
        GArray *spread = g_array_new (FALSE, TRUE, sizeof (O42Operand));
        for (int i = 0; i < n_args; i++)
          {
            const char **names = NULL;
            int n = 0;
            if (operands[i].is_range && operands[i].sheet_last != NULL && ctx->sheets_between != NULL)
              n = ctx->sheets_between (ctx, operands[i].sheet, operands[i].sheet_last, &names);
            if (n > 0)
              {
                for (int k = 0; k < n; k++)
                  {
                    O42Operand one = operands[i];
                    one.sheet = names[k];
                    one.sheet_last = NULL;
                    one.value = o42_value_empty ();
                    g_array_append_val (spread, one);
                  }
                operand_clear (&operands[i]);
              }
            else if (operands[i].is_range && operands[i].sheet_last != NULL)
              {
                O42Operand bad;
                memset (&bad, 0, sizeof bad);
                bad.value = o42_value_error (O42_ERR_REF);
                operand_clear (&operands[i]);
                g_array_append_val (spread, bad);
              }
            else
              g_array_append_val (spread, operands[i]);
            g_free (names);
          }
        g_free (operands);
        n_args = (int) spread->len;
        operands = (O42Operand *) g_array_free (spread, FALSE);
        if (max_args >= 0 && n_args > max_args)
          {
            for (int i = 0; i < n_args; i++)
              operand_clear (&operands[i]);
            g_free (operands);
            return o42_value_error (O42_ERR_VALUE);
          }
      }
  }

  result = fn != NULL ? fn->fn (ctx, operands, n_args)
                      : ext->impl (ctx, ext->name, operands, n_args, ext->user);

  for (int i = 0; i < n_args; i++)
    operand_clear (&operands[i]);
  g_free (operands);

  return result;
}

static O42Operand
eval_operand (O42EvalContext *ctx, const O42Node *node)
{
  O42Operand op;

  memset (&op, 0, sizeof op);

  if (node == NULL)
    {
      op.value = o42_value_error (O42_ERR_VALUE);
      return op;
    }

  switch (node->type)
    {
    case O42_NODE_REF:
      /* A bare reference stays a one-by-one range, so that ROWS(A1) and
       * COUNTA(A1) see a range rather than a scalar. */
      op.is_range = TRUE;
      op.sheet = node->sheet;
      op.sheet_last = node->sheet_last;
      op.range.row0 = op.range.row1 = node->as.ref.row;
      op.range.col0 = op.range.col1 = node->as.ref.col;
      return op;

    case O42_NODE_RANGE:
      op.is_range = TRUE;
      op.sheet = node->sheet;
      op.sheet_last = node->sheet_last;
      op.range = node->as.range;
      operand_clip_whole (ctx, &op, node);
      return op;

    case O42_NODE_APPLY:
      {
        /* Calling what an expression came to: a lambda, or nothing. */
        O42Operand callee = eval_operand (ctx, node->as.apply.callee);
        int n = node->as.apply.args != NULL ? (int) node->as.apply.args->len : 0;
        O42Operand *given = n > 0 ? g_new0 (O42Operand, n) : NULL;

        if (callee.lambda == NULL)
          {
            operand_clear (&callee);
            g_free (given);
            op.value = o42_value_error (O42_ERR_VALUE);
            return op;
          }
        for (int k = 0; k < n; k++)
          given[k] = eval_operand (ctx, g_ptr_array_index (node->as.apply.args, k));
        op = apply_lambda (ctx, callee.lambda, given, n);
        for (int k = 0; k < n; k++)
          operand_clear (&given[k]);
        g_free (given);
        operand_clear (&callee);
        return op;
      }

    case O42_NODE_NAME:
      {
        const char *sheet = NULL;
        O42Range range;

        if (let_scope != NULL)
          for (guint i = let_scope->len; i > 0; i--)
            {
              const LetBinding *b = g_ptr_array_index (let_scope, i - 1);
              if (g_ascii_strcasecmp (b->name, node->as.name) == 0)
                {
                  op = b->operand;
                  op.value = o42_value_copy (&b->operand.value);
                  return op;
                }
            }
        if (ctx->get_name != NULL && ctx->get_name (ctx, node->as.name, &sheet, &range))
          {
            op.is_range = TRUE;
            op.sheet = sheet;
            op.range = range;
            return op;
          }
        op.value = o42_value_error (O42_ERR_NAME);
        return op;
      }

    case O42_NODE_CALL:
      if (eval_range_call (ctx, node, &op))
        return op;
      op.value = eval_node (ctx, node);
      return op;

    case O42_NODE_ARRAY:
      {
        ArrayConst *a;

        if (node->as.array.rows < 1 || node->as.array.cols < 1)
          {
            op.value = o42_value_error (O42_ERR_VALUE);
            return op;
          }
        a = array_const_new (node->as.array.rows, node->as.array.cols);
        for (int i = 0; i < a->rows * a->cols; i++)
          a->cells[i] = (guint) i < node->as.array.items->len
                        ? eval_node (ctx, g_ptr_array_index (node->as.array.items, i))
                        : o42_value_empty ();
        return array_operand (a);
      }

    case O42_NODE_BINARY:
      {
        /* A range on either side of an operator, where a function
         * wants an operand, works cell by cell: SUM(A1:A3*2). */
        O42Operand oa = eval_operand (ctx, node->as.op.a);
        O42Operand ob = eval_operand (ctx, node->as.op.b);
        if (operand_is_multi (&oa) || operand_is_multi (&ob))
          op = broadcast_binary (ctx, node->as.op.op, &oa, &ob);
        else
          op.value = binary_values (node->as.op.op, operand_value (ctx, &oa), operand_value (ctx, &ob));
        operand_clear (&oa);
        operand_clear (&ob);
        return op;
      }

    case O42_NODE_UNARY:
      {
        O42Operand oa = eval_operand (ctx, node->as.op.a);
        if (operand_is_multi (&oa) && node->as.op.op != O42_OP_POS)
          {
            O42Operand scalar;
            memset (&scalar, 0, sizeof scalar);
            scalar.value = o42_value_number (node->as.op.op == O42_OP_NEG ? 0 : 100);
            op = node->as.op.op == O42_OP_NEG ? broadcast_binary (ctx, O42_OP_SUB, &scalar, &oa)
                                              : broadcast_binary (ctx, O42_OP_DIV, &oa, &scalar);
            operand_clear (&scalar);
          }
        else if (operand_is_multi (&oa))
          op = oa, oa.is_range = FALSE, oa.value = o42_value_empty ();
        else
          op.value = eval_node (ctx, node);
        operand_clear (&oa);
        return op;
      }

    default:
      op.value = eval_node (ctx, node);
      return op;
    }
}

static O42Value
eval_node (O42EvalContext *ctx, const O42Node *node)
{
  if (node == NULL)
    return o42_value_error (O42_ERR_VALUE);

  switch (node->type)
    {
    case O42_NODE_NUMBER: return o42_value_number (node->as.number);
    case O42_NODE_STRING: return o42_value_text (node->as.string);
    case O42_NODE_BOOL:   return o42_value_bool (node->as.boolean);
    case O42_NODE_ERROR:  return o42_value_error (node->as.error);
    case O42_NODE_EMPTY:  return o42_value_empty ();
    case O42_NODE_ARRAY:
      {
        O42Operand op = eval_operand (ctx, node);
        O42Value v;
        if (!op.is_range)
          return op.value;
        ctx->get_cell (ctx, op.sheet, op.range.row0, op.range.col0, &v);
        return v;
      }

    case O42_NODE_REF:
      {
        O42Value v;
        ctx->get_cell (ctx, node->sheet, node->as.ref.row, node->as.ref.col, &v);
        return v;
      }

    case O42_NODE_RANGE:
      {
        O42Operand op;
        memset (&op, 0, sizeof op);
        op.is_range = TRUE;
        op.sheet = node->sheet;
        op.range = node->as.range;
        operand_clip_whole (ctx, &op, node);
        return operand_value (ctx, &op);
      }

    case O42_NODE_NAME:
    case O42_NODE_APPLY:
      {
        O42Operand op = eval_operand (ctx, node);
        O42Value v = operand_value (ctx, &op);
        operand_clear (&op);
        return v;
      }

    case O42_NODE_UNARY:  return eval_unary (ctx, node);
    case O42_NODE_BINARY: return eval_binary (ctx, node);
    case O42_NODE_CALL:
      {
        O42Operand op;
        if (eval_range_call (ctx, node, &op))
          {
            O42Value v = operand_value (ctx, &op);
            operand_clear (&op);
            return v;
          }
        return eval_call (ctx, node);
      }

    default:
      return o42_value_error (O42_ERR_VALUE);
    }
}

O42Value
o42_eval (O42EvalContext *ctx, const O42Node *node)
{
  O42EvalContext wrapper;
  ArrayFrame frame;
  O42Value result;

  /* Every evaluation runs under a context that serves array constants
   * and array results as ranges. */
  (void) tree_has_array;
  wrapper = *ctx;
  wrapper.get_cell = array_get_cell;
  frame.original = ctx;
  frame.arrays = g_ptr_array_new_with_free_func ((GDestroyNotify) array_const_free);
  if (array_frames == NULL)
    array_frames = g_ptr_array_new ();
  g_ptr_array_add (array_frames, &frame);

  result = eval_node (&wrapper, node);

  g_ptr_array_remove_index (array_frames, array_frames->len - 1);
  g_ptr_array_unref (frame.arrays);
  return result;
}

gboolean
o42_eval_array (O42EvalContext *ctx, const O42Node *node,
                int *rows, int *cols, O42Value **values)
{
  O42EvalContext wrapper;
  ArrayFrame frame;
  O42Operand op;

  wrapper = *ctx;
  wrapper.get_cell = array_get_cell;
  frame.original = ctx;
  frame.arrays = g_ptr_array_new_with_free_func ((GDestroyNotify) array_const_free);
  if (array_frames == NULL)
    array_frames = g_ptr_array_new ();
  g_ptr_array_add (array_frames, &frame);

  op = eval_operand (&wrapper, node);
  operand_dims (&op, rows, cols);
  *values = g_new0 (O42Value, (gsize) *rows * *cols);
  for (int i = 0; i < *rows; i++)
    for (int j = 0; j < *cols; j++)
      (*values)[i * *cols + j] = operand_cell (&wrapper, &op, i, j);
  operand_clear (&op);

  g_ptr_array_remove_index (array_frames, array_frames->len - 1);
  g_ptr_array_unref (frame.arrays);
  return TRUE;
}

/* The functions Excel added after 2007, which its files carry with an
 * _xlfn. prefix so older Excels show #NAME? rather than mis-evaluate. */
gboolean
o42_function_is_future (const char *name)
{
  static const char *plain[] = {
    "AGGREGATE", "CONCAT", "DAYS", "IFNA", "IFS", "ISOWEEKNUM", "MAXIFS", "MINIFS",
    "NUMBERVALUE", "SWITCH", "TEXTJOIN", "UNICHAR", "UNICODE", "XLOOKUP", "XMATCH",
    "BITAND", "BITOR", "BITXOR", "BITLSHIFT", "BITRSHIFT", "FORMULATEXT", "SHEET", "SHEETS",
    "ISFORMULA", "SKEW.P", "GAUSS", "PHI", "RRI", "PDURATION", "ARABIC", "BASE", "DECIMAL",
    "COMBINA", "PERMUTATIONA", "ACOT", "ACOTH", "COT", "COTH", "CSC", "CSCH", "SEC", "SECH",
    "MUNIT", "ENCODEURL", "FILTERXML", "WEBSERVICE", "IMCOSH", "IMCOT", "IMCSC", "IMCSCH",
    "IMSEC", "IMSECH", "IMSINH", "IMTAN", "SORT", "SORTBY", "UNIQUE", "SEQUENCE", "RANDARRAY",
    "FILTER", "LET", "LAMBDA", "TEXTSPLIT", "VSTACK", "HSTACK", "TAKE", "DROP",
    "MAP", "BYROW", "BYCOL", "REDUCE", "SCAN", "MAKEARRAY", "ISOMITTED"
  };
  if (strchr (name, '.') != NULL && strcmp (name, "ERROR.TYPE") != 0)
    return TRUE;
  for (guint i = 0; i < G_N_ELEMENTS (plain); i++)
    if (strcmp (plain[i], name) == 0)
      return TRUE;
  return FALSE;
}

gboolean
o42_criterion_matches (const char *criterion, const O42Value *value)
{
  O42Value text = o42_value_text (criterion);
  Criterion c;
  gboolean hit;

  criterion_init (&c, &text);
  hit = criterion_match (&c, value);
  criterion_clear (&c);
  o42_value_clear (&text);
  return hit;
}

gboolean
o42_criterion_is_condition (const char *text)
{
  if (text == NULL) return FALSE;
  if (text[0] == '<' || text[0] == '>' || text[0] == '=') return TRUE;
  return strchr (text, '*') != NULL || strchr (text, '?') != NULL;
}
