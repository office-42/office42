/* o42-sheet.c - see o42-sheet.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-sheet.h"

#include "o42-pyquote.h"

#include "o42-book.h"
#include "o42-eval.h"
#include "o42-date.h"
#include "o42-entry.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DEFAULT_COL_WIDTH  80
#define DEFAULT_ROW_HEIGHT 20

/* A named set of cell values: Data > Scenarios. */
typedef struct {
  char      *name;        /* owned */
  char      *comment;     /* owned */
  GArray    *keys;        /* guint64 */
  GPtrArray *values;      /* char *, the inputs to put back */
} Scenario;

static void
scenario_free (gpointer data)
{
  Scenario *s = data;
  g_free (s->name);
  g_free (s->comment);
  g_array_unref (s->keys);
  g_ptr_array_unref (s->values);
  g_free (s);
}

typedef struct {
  O42Value   value;       /* the last computed value */
  char      *input;       /* what the user typed, when it was text or a
                           * formula; numbers keep theirs in `value` */
  O42Node   *ast;         /* the parsed formula, or NULL */
  GArray    *precedents;  /* O42SheetRange read by the formula, or NULL */
  GPtrArray *names;       /* interned upper-case defined names used, or NULL */
  O42FmtIdx  fmt;
  GArray    *runs;        /* O42TextRun, or NULL when the text is plain */
  guint      dirty    : 1;
  guint      visiting : 1;
  guint      spilled  : 1;   /* holds a value spilled from a formula nearby */
  const char *style;         /* interned name of the cell style worn, or NULL */
} O42Cell;

/* What a cell held before a change, so that the change can be taken back. */
typedef struct {
  O42Sheet  *sheet;       /* the sheet the cell is on */
  guint64    key;
  char      *input;       /* owned; NULL when the cell was empty */
  O42FmtIdx  fmt;
  const char *style;      /* the cell style it wore */
  int        array_rows;  /* > 0 when the cell headed an array formula */
  int        array_cols;
} O42Snapshot;

/* What an undo step remembers about something that is not a cell: a
 * column's width, a row's height or hiding, the merges as a whole, a
 * note, a picture or a chart -- the state before the change, so that
 * putting it back is the undo and the state it replaces is the redo. */
typedef enum {
  OBJ_COL_WIDTH, OBJ_ROW_HEIGHT, OBJ_ROW_HIDDEN, OBJ_COL_HIDDEN,
  OBJ_MERGES, OBJ_NOTE, OBJ_LINK, OBJ_NOTES, OBJ_LINKS, OBJ_PICTURE, OBJ_CHART, OBJ_SHAPE,
  OBJ_ROW_LEVEL, OBJ_COL_LEVEL,
  OBJ_SHEET_NAME,       /* the sheet's name */
  OBJ_SHEET_PRESENT     /* whether the sheet is in its book, and where */
} ObjKind;

typedef struct {
  ObjKind     kind;
  O42Sheet   *sheet;
  int         index;      /* column or row; an object's id */
  guint64     key;        /* a note's cell */
  int         number;     /* a width or height */
  gboolean    flag;       /* hidden */
  char       *text;       /* a note's text, NULL for none */
  GArray     *ranges;     /* the merges */
  GHashTable *texts;      /* every note (or link) at once, for a shift */
  O42Picture *picture;    /* a copy, NULL when the picture was not there */
  O42Chart   *chart;
  O42Shape   *shape;      /* a copy, NULL when the shape was not there */
  gboolean    owns_sheet; /* the sheet is out of its book and this snapshot keeps it */
} ObjSnap;

typedef struct {
  GArray *snapshots;      /* O42Snapshot */
  GArray *objects;        /* ObjSnap */
} O42Undo;

struct _O42UndoStack {
  GPtrArray *undo;        /* O42Undo* */
  gsize      pos;
  O42Undo   *pending;
  int        depth;
};

struct _O42Sheet {
  char        *name;
  O42Book     *book;       /* not owned; NULL for a sheet on its own */
  GHashTable  *cells;      /* guint64 key -> O42Cell* */
  GHashTable  *formulas;   /* set of keys of cells that hold a formula */
  GHashTable  *volatiles;  /* those whose formula must be redone after any change:
                            * OFFSET, INDIRECT, RAND, NOW, TODAY... */
  GHashTable  *dependents; /* "SHEET\001band" -> set of formula keys whose
                            * precedents touch that band of 64 rows on
                            * that sheet ("" for this one) */
  guint32      tab_colour;    /* O42_TAB_NO_COLOUR for a plain tab */
  guint16      password;      /* the protection hash, 0 for none */
  gboolean     cycle_seen;    /* a formula asked for itself while evaluating */
  gboolean     recalculating; /* inside o42_sheet_recalculate */
  guint        sizes_stamp;   /* bumped whenever a width or a height moves */
  GArray      *row_stops;     /* SizeStop: the rows that differ from the default */
  GArray      *col_stops;
  guint        row_stops_stamp, col_stops_stamp;
  GHashTable  *col_widths; /* int col -> int width */
  GHashTable  *row_heights;
  GHashTable  *hidden_cols;   /* set of int */
  GHashTable  *hidden_rows;   /* set of int, hidden by hand */
  GHashTable  *row_levels;    /* int -> outline level, absent for 0 */
  GHashTable  *col_levels;
  int          max_row_level, max_col_level;
  GHashTable  *filtered_rows; /* set of int, hidden by the AutoFilter */

  GArray      *merges;        /* O42Range */
  GHashTable  *notes;         /* guint64 key -> char* */
  GHashTable  *links;        /* guint64 key -> char*: hyperlinks */
  GArray      *conditions;    /* O42Condition */
  GArray      *validations;   /* O42Validation */
  GArray      *arrays;        /* O42Range: array formulas, top-left holds the formula */
  GArray      *tables;        /* O42Table, owned */
  GArray      *queries;       /* O42Query: the queries laid out on this sheet */
  GPtrArray   *scenarios;     /* Scenario *, owned */
  GPtrArray   *shapes;        /* O42Shape *, owned */
  guint        next_shape_id;
  GHashTable  *dynamic;       /* keys of the heads of blocks that spilled on their own */
  O42PrintSetup print;        /* File > Page Setup */
  gboolean     protect;       /* Tools > Protection */
  gboolean     chart_sheet;   /* one chart, no cells */
  GArray      *row_breaks;    /* int, sorted: manual page breaks */
  GArray      *col_breaks;
  gboolean     placing_array; /* an array block is being set: no spilling on entry */
  GArray      *pivots;        /* O42Pivot */
  int          frozen_rows, frozen_cols;

  gboolean     has_filter;
  O42Range     filter;
  GHashTable  *filter_choice; /* int col -> char*, the value chosen */
  O42FmtTable *formats;

  O42UndoStack *stack;     /* the book's, or the sheet's own */
  gboolean      owns_stack;

  O42EvalContext eval;

  O42Range     used;          /* the rectangle holding every stored cell */
  gboolean     used_valid;    /* ... when it has been worked out and no cell
                               * on its edge has gone since */
  gboolean     used_any;      /* ... and there is at least one cell */

  GPtrArray   *pictures;         /* O42Picture*, owned */
  guint        next_picture_id;
  GPtrArray   *charts;           /* O42Chart*, owned */

  gboolean     modified;
};

static void sizes_changed (O42Sheet *sheet);

/* ---------------------------------------------------------------------- */
/* Cells                                                                   */
/* ---------------------------------------------------------------------- */

static void
cell_free (gpointer data)
{
  O42Cell *cell = data;

  o42_value_clear (&cell->value);
  g_free (cell->input);
  o42_node_free (cell->ast);
  if (cell->precedents != NULL)
    g_array_free (cell->precedents, TRUE);
  if (cell->names != NULL)
    g_ptr_array_free (cell->names, TRUE);
  if (cell->runs != NULL)
    g_array_unref (cell->runs);
  g_free (cell);
}

/* The text that, typed into a cell, gives it this value.  A number is
 * written with every digit it has, not the ten General shows: this text
 * is what a cell is re-entered from when it is copied, sorted, shifted
 * by an insert, undone or edited, and 3.14159265358979 must survive all
 * of those. */
static char *
value_input_text (const O42Value *value)
{
  if (value->type == O42_VALUE_NUMBER)
    return o42_number_to_text (value->as.number, TRUE);
  return o42_value_display (value);
}

/* The hash table is keyed by a 64-bit value, which does not fit in a
 * pointer on every platform, so the key is stored indirectly. */
static guint
key_hash (gconstpointer p)
{
  guint64 k = *(const guint64 *) p;
  return (guint) (k ^ (k >> 32));
}

static gboolean
key_equal (gconstpointer a, gconstpointer b)
{
  return *(const guint64 *) a == *(const guint64 *) b;
}

/* Keys pack the row above the column, so numeric order is reading order:
 * along each row, then down. */
static int
key_compare_reading_order (gconstpointer a, gconstpointer b)
{
  guint64 x = *(const guint64 *) a, y = *(const guint64 *) b;
  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static O42Cell *
sheet_find (O42Sheet *sheet, int row, int col)
{
  guint64 key = o42_key (row, col);

  return g_hash_table_lookup (sheet->cells, &key);
}

static O42Cell *
sheet_find_key (O42Sheet *sheet, guint64 key)
{
  return g_hash_table_lookup (sheet->cells, &key);
}

static O42Cell *
sheet_ensure (O42Sheet *sheet, int row, int col)
{
  guint64 key = o42_key (row, col);
  O42Cell *cell = g_hash_table_lookup (sheet->cells, &key);
  guint64 *stored;

  if (cell != NULL)
    return cell;

  cell = g_new0 (O42Cell, 1);
  cell->value = o42_value_empty ();
  cell->fmt = o42_fmt_table_default (sheet->formats);

  stored = g_new (guint64, 1);
  *stored = key;
  g_hash_table_insert (sheet->cells, stored, cell);

  /* A new cell can only make the used range larger. */
  if (sheet->used_valid)
    {
      if (!sheet->used_any)
        {
          sheet->used.row0 = sheet->used.row1 = row;
          sheet->used.col0 = sheet->used.col1 = col;
          sheet->used_any = TRUE;
        }
      else
        {
          sheet->used.row0 = MIN (sheet->used.row0, row);
          sheet->used.row1 = MAX (sheet->used.row1, row);
          sheet->used.col0 = MIN (sheet->used.col0, col);
          sheet->used.col1 = MAX (sheet->used.col1, col);
        }
    }

  return cell;
}

/* A cell is going: the used range is only in doubt if it sat on an edge. */
static void
sheet_used_forget (O42Sheet *sheet, int row, int col)
{
  if (sheet->used_valid &&
      (row == sheet->used.row0 || row == sheet->used.row1 ||
       col == sheet->used.col0 || col == sheet->used.col1))
    sheet->used_valid = FALSE;
}

/* A cell that holds nothing and looks like nothing need not be stored. */
static void
sheet_prune (O42Sheet *sheet, int row, int col)
{
  guint64 key = o42_key (row, col);
  O42Cell *cell = g_hash_table_lookup (sheet->cells, &key);

  if (cell == NULL)
    return;

  if (cell->ast == NULL &&
      cell->value.type == O42_VALUE_EMPTY &&
      cell->input == NULL &&
      cell->runs == NULL &&
      cell->fmt == o42_fmt_table_default (sheet->formats))
    {
      g_hash_table_remove (sheet->formulas, &key);
      g_hash_table_remove (sheet->cells, &key);
      sheet_used_forget (sheet, row, col);
    }
}

/* ---------------------------------------------------------------------- */
/* Recalculation                                                           */
/* ---------------------------------------------------------------------- */

static void sheet_get_cell_value (O42EvalContext *ctx, const char *sheet_name,
                                  int row, int col, O42Value *out);
static void set_input_internal (O42Sheet *sheet, int row, int col,
                                const char *text);
static void autofilter_apply (O42Sheet *sheet);
static gboolean ranges_overlap (const O42Range *a, const O42Range *b);

/* The evaluator asking how far a sheet's cells reach, so that A:A is
 * walked as far as there is anything to walk. */
static gboolean
sheet_get_extent (O42EvalContext *ctx, const char *sheet_name, O42Range *used)
{
  O42Sheet *sheet = ctx->user_data;
  O42Sheet *target = sheet;

  if (sheet_name != NULL && g_ascii_strcasecmp (sheet_name, sheet->name) != 0)
    target = (sheet->book != NULL) ? o42_book_find_sheet (sheet->book, sheet_name) : NULL;
  if (target == NULL)
    return FALSE;
  o42_sheet_used_range (target, used);
  return target->used_any;
}

/* The evaluator's way to a defined name: through the book. */
/* The sheets of a 3-D reference: from `first` to `last` in tab order,
 * either way round. */
static int
sheet_sheets_between (O42EvalContext *ctx, const char *first, const char *last,
                      const char ***names)
{
  O42Sheet *sheet = ctx->user_data;
  O42Book *book = sheet->book;
  O42Sheet *a, *b;
  int ia, ib, n = 0;

  *names = NULL;
  if (book == NULL || first == NULL || last == NULL)
    return 0;
  a = o42_book_find_sheet (book, first);
  b = o42_book_find_sheet (book, last);
  if (a == NULL || b == NULL)
    return 0;
  ia = o42_book_sheet_index (book, a);
  ib = o42_book_sheet_index (book, b);
  if (ia > ib) { int t = ia; ia = ib; ib = t; }
  *names = g_new (const char *, ib - ia + 1);
  for (int i = ia; i <= ib; i++)
    (*names)[n++] = g_intern_string (o42_sheet_get_name (o42_book_sheet (book, i)));
  return n;
}

static gboolean
sheet_get_name (O42EvalContext *ctx, const char *name,
                const char **sheet_name, O42Range *range)
{
  O42Sheet *sheet = ctx->user_data;
  O42Sheet *target = NULL;

  /* A structured reference, Table1[Sales], names part of a table. */
  if (strchr (name, '[') != NULL || o42_sheet_find_table (sheet, name) != NULL ||
      (sheet->book != NULL && o42_book_find_table (sheet->book, name) != NULL))
    {
      char *table_name = g_strdup (name);
      char *bracket = strchr (table_name, '[');
      O42Sheet *holder;

      if (bracket != NULL) *bracket = '\0';
      holder = o42_sheet_find_table (sheet, table_name) != NULL ? sheet
             : sheet->book != NULL ? o42_book_find_table (sheet->book, table_name) : NULL;
      g_free (table_name);
      if (holder != NULL && o42_sheet_table_range (sheet, name, ctx->row, range))
        {
          *sheet_name = (holder == sheet) ? NULL : holder->name;
          return TRUE;
        }
      return FALSE;
    }

  if (sheet->book == NULL ||
      !o42_book_lookup_name (sheet->book, name, &target, range))
    return FALSE;

  *sheet_name = (target == sheet) ? NULL : target->name;
  return TRUE;
}

static const O42Range *array_at (O42Sheet *sheet, int row, int col);
static void sheet_invalidate (O42Sheet *sheet, int row, int col);

static void array_register (O42Sheet *sheet, const O42Range *block);
static void array_dissolve_at (O42Sheet *sheet, int row, int col);
static O42Value spill (O42Sheet *sheet, guint64 key, int rows, int cols, O42Value *values);
static void spill_retract (O42Sheet *sheet, guint64 key);

static void
sheet_evaluate (O42Sheet *sheet, guint64 key, O42Cell *cell)
{
  O42Value result;
  const O42Range *array;

  if (cell->ast == NULL)
    {
      /* A cell of an array formula's block takes its value from the
       * formula in the block's top-left cell. */
      array = array_at (sheet, o42_key_row (key), o42_key_col (key));
      if (array != NULL && (array->row0 != o42_key_row (key) || array->col0 != o42_key_col (key)))
        {
          O42Cell *master = sheet_find (sheet, array->row0, array->col0);
          if (master != NULL)
            sheet_evaluate (sheet, o42_key (array->row0, array->col0), master);
        }
      return;
    }
  if (!cell->dirty)
    return;

  /* Calculating by hand: what a cell said last time stands until F9.
   * A cell that has never been worked out is worked out once, so a
   * formula just typed in shows an answer rather than a blank. */
  if (o42_book_manual (sheet->book) && !sheet->recalculating &&
      cell->value.type != O42_VALUE_EMPTY)
    return;

  if (cell->visiting)
    {
      /* Something this cell depends on is asking for this cell.  With
       * iteration turned on that is allowed: the value from the last
       * time round stands for this one, and the loop outside goes round
       * again until nothing moves.  Without it, saying so outright is
       * more useful than Excel's #REF!. */
      if (o42_book_iteration (sheet->book, NULL, NULL))
        {
          sheet->cycle_seen = TRUE;
          cell->dirty = 0;
          return;
        }
      o42_value_clear (&cell->value);
      cell->value = o42_value_error (O42_ERR_CIRCULAR);
      cell->dirty = 0;
      return;
    }

  cell->visiting = 1;
  {
    /* Evaluating pulls on other cells, which evaluate in turn, so the
     * calling cell is saved and put back around each one. */
    int saved_row = sheet->eval.row, saved_col = sheet->eval.col;

    sheet->eval.row = o42_key_row (key);
    sheet->eval.col = o42_key_col (key);
    array = array_at (sheet, o42_key_row (key), o42_key_col (key));
    if (array != NULL && array->row0 == o42_key_row (key) && array->col0 == o42_key_col (key))
      {
        /* The head of an array formula: spread the result over the
         * block, #N/A where the result is smaller than the block. */
        O42Range block = *array;
        int rows, cols;
        O42Value *values;

        o42_eval_array (&sheet->eval, cell->ast, &rows, &cols, &values);
        result = o42_value_copy (&values[0]);
        for (int r = block.row0; r <= block.row1; r++)
          for (int c = block.col0; c <= block.col1; c++)
            {
              int i = r - block.row0, j = c - block.col0;
              if (r == block.row0 && c == block.col0)
                continue;
              {
                O42Cell *member = sheet_ensure (sheet, r, c);
                o42_value_clear (&member->value);
                member->value = (i < rows && j < cols) ? o42_value_copy (&values[i * cols + j])
                                                       : o42_value_error (O42_ERR_NA);
                sheet_invalidate (sheet, r, c);
              }
            }
        for (int k = 0; k < rows * cols; k++)
          o42_value_clear (&values[k]);
        g_free (values);
      }
    else
      {
        /* A plain formula: a one-cell result is the value; a larger
         * one spills into the cells beside and below, if they are free. */
        int rows, cols;
        O42Value *values;

        o42_eval_array (&sheet->eval, cell->ast, &rows, &cols, &values);
        if (rows * cols <= 1)
          {
            result = o42_value_copy (&values[0]);
            spill_retract (sheet, key);
          }
        else
          result = spill (sheet, key, rows, cols, values);
        for (int k = 0; k < rows * cols; k++)
          o42_value_clear (&values[k]);
        g_free (values);
      }
    sheet->eval.row = saved_row;
    sheet->eval.col = saved_col;
  }
  cell->visiting = 0;

  /* The cell may have been dropped while evaluating -- it cannot be, today,
   * but re-finding it costs nothing and would catch that changing. */
  cell = sheet_find_key (sheet, key);
  if (cell == NULL)
    {
      o42_value_clear (&result);
      return;
    }

  o42_value_clear (&cell->value);
  cell->value = result;
  cell->dirty = 0;
}

/* What CELL() asks about a cell's looks, which only the sheet knows:
 * its number format in Excel's letter codes, its column's width, and
 * whether it is locked or aligned. */
static gboolean
sheet_get_cell_info (O42EvalContext *ctx, const char *sheet_name, int row, int col,
                     const char *what, O42Value *out)
{
  O42Sheet *sheet = ctx->user_data;
  const O42Fmt *fmt;

  if (sheet_name != NULL && g_ascii_strcasecmp (sheet_name, sheet->name) != 0)
    {
      O42Sheet *other = (sheet->book != NULL)
                        ? o42_book_find_sheet (sheet->book, sheet_name) : NULL;

      if (other == NULL)
        return FALSE;
      sheet = other;
    }
  if (row < 0 || col < 0 || row >= O42_MAX_ROWS || col >= O42_MAX_COLS)
    return FALSE;
  fmt = o42_sheet_get_fmt (sheet, row, col);

  if (strcmp (what, "format") == 0)
    {
      /* Excel's codes: G for general, F and a digit for fixed, a comma
       * for thousands, C for currency, P for per cent, S for
       * scientific, D for the date and time formats. */
      char code[8];

      switch (fmt->number)
        {
        case O42_NUM_FIXED:      g_snprintf (code, sizeof code, "F%d", fmt->decimals); break;
        case O42_NUM_COMMA:      g_snprintf (code, sizeof code, ",%d", fmt->decimals); break;
        case O42_NUM_CURRENCY:   g_snprintf (code, sizeof code, "C%d", fmt->decimals); break;
        case O42_NUM_PERCENT:    g_snprintf (code, sizeof code, "P%d", fmt->decimals); break;
        case O42_NUM_SCIENTIFIC: g_snprintf (code, sizeof code, "S%d", fmt->decimals); break;
        case O42_NUM_DATE:       g_strlcpy (code, "D4", sizeof code); break;
        case O42_NUM_TIME:       g_strlcpy (code, "D9", sizeof code); break;
        case O42_NUM_DATETIME:   g_strlcpy (code, "D1", sizeof code); break;
        default:                 g_strlcpy (code, "G", sizeof code); break;
        }
      *out = o42_value_text (code);
      return TRUE;
    }
  if (strcmp (what, "width") == 0)
    {
      /* Excel counts a column's width in characters of the standard
       * font; ours are in pixels, at about seven to the character. */
      *out = o42_value_number (floor (o42_sheet_col_width (sheet, col) / 7.0 + 0.5));
      return TRUE;
    }
  if (strcmp (what, "protect") == 0)
    {
      *out = o42_value_number (fmt->locked ? 1 : 0);
      return TRUE;
    }
  if (strcmp (what, "prefix") == 0)
    {
      /* The apostrophe Lotus put in front of text to say how it lines
       * up, which Excel still reports. */
      O42Value v;
      const char *prefix = "";

      o42_sheet_get_value (sheet, row, col, &v);
      if (v.type == O42_VALUE_TEXT)
        prefix = fmt->halign == O42_HALIGN_RIGHT ? "\"" :
                 fmt->halign == O42_HALIGN_CENTRE ? "^" : "'";
      o42_value_clear (&v);
      *out = o42_value_text (prefix);
      return TRUE;
    }
  if (strcmp (what, "sheet") == 0)
    {
      /* SHEET: which sheet this is, counting from one. */
      *out = o42_value_number (sheet->book != NULL
                               ? o42_book_sheet_index (sheet->book, sheet) + 1 : 1);
      return TRUE;
    }
  if (strcmp (what, "sheets") == 0)
    {
      *out = o42_value_number (sheet->book != NULL ? o42_book_n_sheets (sheet->book) : 1);
      return TRUE;
    }
  if (strcmp (what, "formulatext") == 0)
    {
      /* FORMULATEXT wants what was typed, formula and all. */
      char *input = o42_sheet_get_input (sheet, row, col);

      *out = o42_value_text (input != NULL && input[0] == '=' ? input : "");
      g_free (input);
      return TRUE;
    }
  if (strcmp (what, "formula") == 0)
    {
      char *input = o42_sheet_get_input (sheet, row, col);

      *out = o42_value_bool (input != NULL && input[0] == '=');
      g_free (input);
      return TRUE;
    }
  if (strcmp (what, "filename") == 0)
    {
      *out = o42_value_text ("");   /* the book knows its file, the sheet does not */
      return TRUE;
    }
  if (strcmp (what, "color") == 0 || strcmp (what, "parentheses") == 0)
    {
      *out = o42_value_number (0);
      return TRUE;
    }
  return FALSE;
}

static void
sheet_get_cell_value (O42EvalContext *ctx, const char *sheet_name,
                      int row, int col, O42Value *out)
{
  O42Sheet *sheet = ctx->user_data;
  guint64 key = o42_key (row, col);
  O42Cell *cell;

  /* A reference to another sheet goes through the book.  A sheet that is
   * not there -- deleted, or misspelt -- is #REF!. */
  if (sheet_name != NULL && g_ascii_strcasecmp (sheet_name, sheet->name) != 0)
    {
      O42Sheet *other = (sheet->book != NULL)
                        ? o42_book_find_sheet (sheet->book, sheet_name) : NULL;

      if (other == NULL)
        {
          *out = o42_value_error (O42_ERR_REF);
          return;
        }
      sheet_get_cell_value (&other->eval, NULL, row, col, out);
      return;
    }

  if (row < 0 || col < 0 || row >= O42_MAX_ROWS || col >= O42_MAX_COLS)
    {
      *out = o42_value_error (O42_ERR_REF);
      return;
    }

  cell = sheet_find_key (sheet, key);
  if (cell == NULL)
    {
      *out = o42_value_empty ();
      return;
    }

  sheet_evaluate (sheet, key, cell);
  *out = o42_value_copy (&cell->value);
}

/* Marks every formula that reads `row`,`col` as stale, and everything that
 * reads those in turn.
 *
 * This walks the list of formula cells and tests their precedent rectangles,
 * rather than keeping a map from cell to dependants.  It is O(formulas) for
 * each change instead of O(dependants), which for the sheets people actually
 * build is a few thousand rectangle tests -- far too fast to notice, and it
 * avoids a dependency map whose size a single reference to a whole column
 * could blow up. */
static void
sheet_invalidate (O42Sheet *sheet, int row, int col);

/* Does a precedent name the sheet a change happened on?  A precedent with
 * no sheet is on the formula's own. */
static gboolean
precedent_on (const O42SheetRange *p, O42Sheet *own, const char *changed)
{
  const char *name = (p->sheet != NULL) ? p->sheet : own->name;

  return g_ascii_strcasecmp (name, changed) == 0;
}

/* ---- The dependents index ---------------------------------------------- */

#define DEP_BAND 64

/* A precedent taller than this many bands -- a whole column, or most of
 * one -- is indexed under the one band below rather than under every
 * band it crosses, which for A:A would be 16,384 entries a formula. */
#define DEP_TALL_BANDS 256
#define DEP_WHOLE (-1)

static char *
deps_key (const O42SheetRange *p, int band)
{
  char *upper = p->sheet != NULL ? g_ascii_strup (p->sheet, -1) : g_strdup ("");
  char *key = g_strdup_printf ("%s\001%d", upper, band);
  g_free (upper);
  return key;
}

static void
deps_bands (const O42SheetRange *p, int *lo, int *hi)
{
  *lo = MAX (p->range.row0, 0) / DEP_BAND;
  *hi = MIN (p->range.row1, O42_MAX_ROWS - 1) / DEP_BAND;
  if (*hi - *lo >= DEP_TALL_BANDS)
    *lo = *hi = DEP_WHOLE;
}

/* Every band a formula's precedents touch gets the formula's key. */
static void
deps_add (O42Sheet *sheet, guint64 fkey, O42Cell *cell)
{
  if (cell->precedents == NULL)
    return;
  for (guint i = 0; i < cell->precedents->len; i++)
    {
      const O42SheetRange *p = &g_array_index (cell->precedents, O42SheetRange, i);
      int lo, hi;

      deps_bands (p, &lo, &hi);
      for (int band = lo; band <= hi; band++)
        {
          char *key = deps_key (p, band);
          GHashTable *set = g_hash_table_lookup (sheet->dependents, key);
          guint64 *stored;
          if (set == NULL)
            {
              set = g_hash_table_new_full (key_hash, key_equal, g_free, NULL);
              g_hash_table_insert (sheet->dependents, key, set);
            }
          else
            g_free (key);
          stored = g_new (guint64, 1);
          *stored = fkey;
          g_hash_table_add (set, stored);
        }
    }
}

static void
deps_remove (O42Sheet *sheet, guint64 fkey, O42Cell *cell)
{
  if (cell->precedents == NULL)
    return;
  for (guint i = 0; i < cell->precedents->len; i++)
    {
      const O42SheetRange *p = &g_array_index (cell->precedents, O42SheetRange, i);
      int lo, hi;

      deps_bands (p, &lo, &hi);
      for (int band = lo; band <= hi; band++)
        {
          char *key = deps_key (p, band);
          GHashTable *set = g_hash_table_lookup (sheet->dependents, key);
          if (set != NULL)
            {
              g_hash_table_remove (set, &fkey);
              if (g_hash_table_size (set) == 0)
                g_hash_table_remove (sheet->dependents, key);
            }
          g_free (key);
        }
    }
}

/* After cells have moved (rows or columns inserted or deleted) the keys
 * in the index are stale; it is cheaper to build it again. */
static void
deps_rebuild (O42Sheet *sheet)
{
  GHashTableIter iter;
  gpointer key_ptr;

  g_hash_table_remove_all (sheet->dependents);
  g_hash_table_iter_init (&iter, sheet->formulas);
  while (g_hash_table_iter_next (&iter, &key_ptr, NULL))
    {
      guint64 fkey = *(guint64 *) key_ptr;
      O42Cell *cell = sheet_find_key (sheet, fkey);
      if (cell != NULL)
        deps_add (sheet, fkey, cell);
    }
}

/* ---- Auditing: what a cell reads, and what reads it ------------------- */

/* The rectangles a cell's formula reads.  On this sheet only: an arrow
 * to another sheet has nowhere to point.  The array is the caller's to
 * free. */
GArray *
o42_sheet_precedents (O42Sheet *sheet, int row, int col)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (O42Range));
  O42Cell *cell;

  g_return_val_if_fail (sheet != NULL, out);
  cell = sheet_find (sheet, row, col);
  if (cell == NULL || cell->precedents == NULL)
    return out;

  for (guint i = 0; i < cell->precedents->len; i++)
    {
      const O42SheetRange *p = &g_array_index (cell->precedents, O42SheetRange, i);
      O42Range r;

      if (p->sheet != NULL && g_ascii_strcasecmp (p->sheet, sheet->name) != 0)
        continue;
      r = o42_range_normalise (p->range.row0, p->range.col0, p->range.row1, p->range.col1);
      g_array_append_val (out, r);
    }
  return out;
}

/* The cells whose formulas read this one, each as a range of one cell.
 * The dependents index holds them by band, so this looks in the band
 * the cell falls in and keeps whichever of those really reach it. */
GArray *
o42_sheet_dependents (O42Sheet *sheet, int row, int col)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (O42Range));
  O42SheetRange probe;
  char *key;
  GHashTable *set;
  GHashTableIter iter;
  gpointer stored;

  g_return_val_if_fail (sheet != NULL, out);
  memset (&probe, 0, sizeof probe);

  /* A formula that names no sheet is indexed under the empty one, and
   * one over a whole column under the whole band. */
  for (int which = 0; which < 4; which++)
    {
      probe.sheet = (which & 1) ? NULL : sheet->name;
      key = deps_key (&probe, (which & 2) ? DEP_WHOLE : row / DEP_BAND);
      set = g_hash_table_lookup (sheet->dependents, key);
      g_free (key);
      if (set == NULL)
        continue;

      g_hash_table_iter_init (&iter, set);
      while (g_hash_table_iter_next (&iter, &stored, NULL))
        {
          guint64 fkey = *(guint64 *) stored;
          O42Cell *cell = sheet_find_key (sheet, fkey);
          O42Range r;

          if (cell == NULL || cell->precedents == NULL)
            continue;
          for (guint i = 0; i < cell->precedents->len; i++)
            {
              const O42SheetRange *p = &g_array_index (cell->precedents, O42SheetRange, i);
              O42Range n = o42_range_normalise (p->range.row0, p->range.col0,
                                                p->range.row1, p->range.col1);

              if (p->sheet != NULL && g_ascii_strcasecmp (p->sheet, sheet->name) != 0)
                continue;
              if (row < n.row0 || row > n.row1 || col < n.col0 || col > n.col1)
                continue;
              r.row0 = r.row1 = o42_key_row (fkey);
              r.col0 = r.col1 = o42_key_col (fkey);
              g_array_append_val (out, r);
              break;
            }
        }
    }
  return out;
}

/* Whether a formula calls a function whose result depends on more than
 * its precedents say. */
static gboolean
tree_is_volatile (const O42Node *node)
{
  static const char *names[] = { "OFFSET", "INDIRECT", "RAND", "RANDBETWEEN", "RANDARRAY", "NOW", "TODAY", "CELL", "INFO", "PY" };

  /* A 3-D reference reads sheets the dependents index does not follow;
   * recalculating it every time is the honest way to keep it right. */
  if (node != NULL && (node->type == O42_NODE_REF || node->type == O42_NODE_RANGE) && node->sheet_last != NULL)
    return TRUE;

  if (node == NULL)
    return FALSE;
  switch (node->type)
    {
    case O42_NODE_UNARY:
    case O42_NODE_BINARY:
      return tree_is_volatile (node->as.op.a) || tree_is_volatile (node->as.op.b);
    case O42_NODE_CALL:
      /* Every function whose name starts with RAND draws a new number
       * each time, which is the whole point of them. */
      if (g_str_has_prefix (node->as.call.name, "RAND"))
        return TRUE;
      for (guint i = 0; i < G_N_ELEMENTS (names); i++)
        if (strcmp (node->as.call.name, names[i]) == 0)
          return TRUE;
      if (node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          if (tree_is_volatile (g_ptr_array_index (node->as.call.args, i)))
            return TRUE;
      return FALSE;
    default:
      return FALSE;
    }
}

/* Whether a formula could produce an array: it names a range, holds an
 * array constant, or calls a function that returns one. */
static gboolean
tree_has_range (const O42Node *node)
{
  static const char *names[] = { "TRANSPOSE", "MMULT", "MINVERSE", "FREQUENCY", "LINEST", "TREND",
                                 "GROWTH", "LOGEST", "OFFSET", "INDIRECT", "SEQUENCE", "RANDARRAY",
                                 "UNIQUE", "SORT", "SORTBY", "FILTER", "MAP", "BYROW", "BYCOL",
                                 "SCAN", "MAKEARRAY", "LET" };
  if (node == NULL)
    return FALSE;
  switch (node->type)
    {
    case O42_NODE_RANGE:
    case O42_NODE_ARRAY:
      return TRUE;
    case O42_NODE_UNARY:
    case O42_NODE_BINARY:
      return tree_has_range (node->as.op.a) || tree_has_range (node->as.op.b);
    case O42_NODE_CALL:
      for (guint i = 0; i < G_N_ELEMENTS (names); i++)
        if (strcmp (node->as.call.name, names[i]) == 0)
          return TRUE;
      if (node->as.call.args != NULL)
        for (guint i = 0; i < node->as.call.args->len; i++)
          if (tree_has_range (g_ptr_array_index (node->as.call.args, i)))
            return TRUE;
      return FALSE;
    case O42_NODE_APPLY:
      if (tree_has_range (node->as.apply.callee))
        return TRUE;
      if (node->as.apply.args != NULL)
        for (guint i = 0; i < node->as.apply.args->len; i++)
          if (tree_has_range (g_ptr_array_index (node->as.apply.args, i)))
            return TRUE;
      return FALSE;
    default:
      return FALSE;
    }
}

static void
sheet_invalidate_named (O42Sheet *sheet, const char *changed, int row, int col)
{
  GArray *to_visit;
  GHashTable *candidates[4] = { NULL, NULL, NULL, NULL };
  gboolean any_candidate = FALSE;

  if (g_hash_table_size (sheet->formulas) == 0)
    return;

  /* Only the formulas whose precedents reach this band of rows on the
   * changed sheet can be affected: the index holds them under the
   * sheet's name, and under "" when the sheet is this one; the ones
   * over a whole column sit under the whole band. */
  {
    char *upper = g_ascii_strup (changed, -1);
    char *key = g_strdup_printf ("%s\001%d", upper, row / DEP_BAND);
    candidates[0] = g_hash_table_lookup (sheet->dependents, key);
    g_free (key);
    key = g_strdup_printf ("%s\001%d", upper, DEP_WHOLE);
    candidates[2] = g_hash_table_lookup (sheet->dependents, key);
    g_free (key);
    if (g_ascii_strcasecmp (changed, sheet->name) == 0)
      {
        key = g_strdup_printf ("\001%d", row / DEP_BAND);
        candidates[1] = g_hash_table_lookup (sheet->dependents, key);
        g_free (key);
        key = g_strdup_printf ("\001%d", DEP_WHOLE);
        candidates[3] = g_hash_table_lookup (sheet->dependents, key);
        g_free (key);
      }
    g_free (upper);
  }
  for (int which = 0; which < 4; which++)
    any_candidate |= candidates[which] != NULL;
  /* Nothing indexed reads this band and nothing is volatile: no formula
   * can care.  (The volatile ones are checked first: a change in a band
   * no formula names is exactly what INDIRECT reads.) */
  if (!any_candidate && g_hash_table_size (sheet->volatiles) == 0)
    return;

  to_visit = g_array_new (FALSE, FALSE, sizeof (guint64));

  /* Volatile formulas read cells the precedents cannot name: any change
   * on this sheet stales them, as Excel recalculates them every time. */
  (void) changed;   /* any sheet: a 3-D reference reads several, and Excel redoes them all */
  if (g_hash_table_size (sheet->volatiles) > 0)
    {
      GHashTableIter viter;
      gpointer vkey;
      g_hash_table_iter_init (&viter, sheet->volatiles);
      while (g_hash_table_iter_next (&viter, &vkey, NULL))
        {
          guint64 fkey = *(guint64 *) vkey;
          O42Cell *cell = sheet_find_key (sheet, fkey);
          if (cell != NULL && !cell->dirty && fkey != o42_key (row, col))
            {
              cell->dirty = 1;
              g_array_append_val (to_visit, fkey);
            }
        }
    }

  for (int which = 0; which < 4; which++)
    {
      GHashTableIter iter;
      gpointer key_ptr;

      if (candidates[which] == NULL)
        continue;
      g_hash_table_iter_init (&iter, candidates[which]);
      while (g_hash_table_iter_next (&iter, &key_ptr, NULL))
        {
          guint64 fkey = *(guint64 *) key_ptr;
          O42Cell *cell = sheet_find_key (sheet, fkey);

          if (cell == NULL || cell->dirty || cell->precedents == NULL)
            continue;

          for (guint i = 0; i < cell->precedents->len; i++)
            {
              const O42SheetRange *p = &g_array_index (cell->precedents, O42SheetRange, i);

              if (precedent_on (p, sheet, changed) &&
                  o42_range_contains (&p->range, row, col))
                {
                  cell->dirty = 1;
                  g_array_append_val (to_visit, fkey);
                  break;
                }
            }
        }
    }

  /* Recursing only on cells that have just gone from clean to dirty is what
   * makes this terminate even when the sheet contains a cycle.  The cells
   * just staled are on this sheet, so the recursion is a plain
   * invalidation -- which also tells the other sheets. */
  for (guint i = 0; i < to_visit->len; i++)
    {
      guint64 k = g_array_index (to_visit, guint64, i);
      sheet_invalidate (sheet, o42_key_row (k), o42_key_col (k));
    }

  g_array_free (to_visit, TRUE);
}

static void
sheet_invalidate (O42Sheet *sheet, int row, int col)
{
  sheet_invalidate_named (sheet, sheet->name, row, col);

  if (sheet->book != NULL)
    o42_book_cell_changed (sheet->book, sheet, row, col);
}

void
o42_sheet_invalidate_from (O42Sheet *sheet, const char *sheet_name,
                           int row, int col)
{
  g_return_if_fail (sheet != NULL);
  g_return_if_fail (sheet_name != NULL);

  sheet_invalidate_named (sheet, sheet_name, row, col);
}

/* ---------------------------------------------------------------------- */
/* Undo                                                                    */
/* ---------------------------------------------------------------------- */

static void obj_snap_clear (ObjSnap *snap);
static void pivot_clear (O42Pivot *pivot);
static void level_store (O42Sheet *sheet, gboolean rows, int index, int level);
static ObjSnap obj_snap_take (O42Sheet *sheet, ObjKind kind, int index, guint64 key);
static void obj_snap_apply (const ObjSnap *snap);

static void
undo_free (O42Undo *undo)
{
  if (undo == NULL)
    return;

  for (guint i = 0; i < undo->snapshots->len; i++)
    g_free (g_array_index (undo->snapshots, O42Snapshot, i).input);

  g_array_free (undo->snapshots, TRUE);
  for (guint i = 0; i < undo->objects->len; i++)
    obj_snap_clear (&g_array_index (undo->objects, ObjSnap, i));
  g_array_free (undo->objects, TRUE);
  g_free (undo);
}

static O42Undo *
undo_new (void)
{
  O42Undo *undo = g_new0 (O42Undo, 1);

  undo->snapshots = g_array_new (FALSE, FALSE, sizeof (O42Snapshot));
  undo->objects = g_array_new (FALSE, FALSE, sizeof (ObjSnap));
  return undo;
}

static O42Snapshot
snapshot_take (O42Sheet *sheet, guint64 key)
{
  O42Cell *cell = sheet_find_key (sheet, key);
  O42Snapshot snap;

  snap.sheet = sheet;
  snap.key = key;
  snap.fmt = (cell != NULL) ? cell->fmt
                            : o42_fmt_table_default (sheet->formats);
  snap.style = (cell != NULL) ? cell->style : NULL;
  snap.input = NULL;

  if (cell != NULL)
    {
      if (cell->input != NULL)
        snap.input = g_strdup (cell->input);
      else if (cell->value.type != O42_VALUE_EMPTY)
        snap.input = value_input_text (&cell->value);
    }

  snap.array_rows = snap.array_cols = 0;
  {
    const O42Range *a = array_at (sheet, o42_key_row (key), o42_key_col (key));
    if (a != NULL && a->row0 == o42_key_row (key) && a->col0 == o42_key_col (key) &&
        !g_hash_table_contains (sheet->dynamic, &key))
      {
        snap.array_rows = a->row1 - a->row0 + 1;
        snap.array_cols = a->col1 - a->col0 + 1;
      }
  }
  return snap;
}

O42UndoStack *
o42_undo_stack_new (void)
{
  O42UndoStack *stack = g_new0 (O42UndoStack, 1);

  stack->undo = g_ptr_array_new ();
  return stack;
}

static void
undo_stack_clear (O42UndoStack *stack)
{
  for (guint i = 0; i < stack->undo->len; i++)
    undo_free (g_ptr_array_index (stack->undo, i));
  g_ptr_array_set_size (stack->undo, 0);
  stack->pos = 0;
}

void
o42_undo_stack_free (O42UndoStack *stack)
{
  if (stack == NULL)
    return;

  undo_stack_clear (stack);
  g_ptr_array_free (stack->undo, TRUE);
  undo_free (stack->pending);
  g_free (stack);
}

static void
op_begin (O42Sheet *sheet)
{
  O42UndoStack *stack = sheet->stack;

  if (stack->depth++ == 0)
    stack->pending = undo_new ();
}

static void
op_capture (O42Sheet *sheet, int row, int col)
{
  O42UndoStack *stack = sheet->stack;
  O42Snapshot snap;

  if (stack->pending == NULL)
    return;

  snap = snapshot_take (sheet, o42_key (row, col));
  g_array_append_val (stack->pending->snapshots, snap);
}

static void
obj_capture (O42Sheet *sheet, ObjKind kind, int index, guint64 key)
{
  O42UndoStack *stack = sheet->stack;
  ObjSnap snap;

  if (stack->pending == NULL)
    return;
  snap = obj_snap_take (sheet, kind, index, key);
  g_array_append_val (stack->pending->objects, snap);
}

/* Everything an insert or delete of rows or columns moves about. */
static void
obj_capture_all (O42Sheet *sheet)
{
  obj_capture (sheet, OBJ_MERGES, 0, 0);
  for (guint i = 0; i < sheet->shapes->len; i++)
    obj_capture (sheet, OBJ_SHAPE, (int) ((O42Shape *) g_ptr_array_index (sheet->shapes, i))->id, 0);
  if (g_hash_table_size (sheet->notes) > 0) obj_capture (sheet, OBJ_NOTES, 0, 0);
  if (g_hash_table_size (sheet->links) > 0) obj_capture (sheet, OBJ_LINKS, 0, 0);
  for (guint i = 0; i < sheet->pictures->len; i++)
    obj_capture (sheet, OBJ_PICTURE, ((O42Picture *) g_ptr_array_index (sheet->pictures, i))->id, 0);
  for (guint i = 0; i < sheet->charts->len; i++)
    obj_capture (sheet, OBJ_CHART, ((O42Chart *) g_ptr_array_index (sheet->charts, i))->id, 0);
}

static void
op_end (O42Sheet *sheet)
{
  O42UndoStack *stack = sheet->stack;

  if (--stack->depth > 0)
    return;

  if (stack->pending == NULL)
    return;

  if (stack->pending->snapshots->len == 0 && stack->pending->objects->len == 0)
    {
      undo_free (stack->pending);
      stack->pending = NULL;
      return;
    }

  /* Every sheet a snapshot is on has changed. */
  for (guint i = 0; i < stack->pending->snapshots->len; i++)
    g_array_index (stack->pending->snapshots, O42Snapshot, i).sheet->modified = TRUE;

  /* A new edit discards anything that was waiting to be redone. */
  while (stack->undo->len > stack->pos)
    {
      O42Undo *dead = g_ptr_array_index (stack->undo, stack->undo->len - 1);
      g_ptr_array_remove_index (stack->undo, stack->undo->len - 1);
      undo_free (dead);
    }

  g_ptr_array_add (stack->undo, stack->pending);
  stack->pos = stack->undo->len;
  stack->pending = NULL;
}

/* ---------------------------------------------------------------------- */
/* Lifecycle                                                               */
/* ---------------------------------------------------------------------- */

O42Sheet *
o42_sheet_new (const char *name)
{
  O42Sheet *sheet = g_new0 (O42Sheet, 1);

  sheet->name = g_strdup (name != NULL ? name : "Sheet1");
  sheet->cells = g_hash_table_new_full (key_hash, key_equal, g_free, cell_free);
  sheet->formulas = g_hash_table_new_full (key_hash, key_equal, g_free, NULL);
  sheet->volatiles = g_hash_table_new_full (key_hash, key_equal, g_free, NULL);
  sheet->dependents = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify) g_hash_table_unref);
  sheet->col_widths = g_hash_table_new (g_direct_hash, g_direct_equal);
  sheet->row_heights = g_hash_table_new (g_direct_hash, g_direct_equal);
  sheet->hidden_cols = g_hash_table_new (g_direct_hash, g_direct_equal);
  sheet->hidden_rows = g_hash_table_new (g_direct_hash, g_direct_equal);
  sheet->row_levels = g_hash_table_new (g_direct_hash, g_direct_equal);
  sheet->col_levels = g_hash_table_new (g_direct_hash, g_direct_equal);
  sheet->filtered_rows = g_hash_table_new (g_direct_hash, g_direct_equal);
  sheet->filter_choice = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  sheet->merges = g_array_new (FALSE, FALSE, sizeof (O42Range));
  sheet->notes = g_hash_table_new_full (key_hash, key_equal, g_free, g_free);
  sheet->links = g_hash_table_new_full (key_hash, key_equal, g_free, g_free);
  sheet->conditions = g_array_new (FALSE, FALSE, sizeof (O42Condition));
  sheet->validations = g_array_new (FALSE, FALSE, sizeof (O42Validation));
  sheet->arrays = g_array_new (FALSE, FALSE, sizeof (O42Range));
  sheet->tables = g_array_new (FALSE, FALSE, sizeof (O42Table));
  sheet->queries = g_array_new (FALSE, FALSE, sizeof (O42Query));
  sheet->tab_colour = O42_TAB_NO_COLOUR;
  sheet->scenarios = g_ptr_array_new_with_free_func (scenario_free);
  sheet->shapes = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_shape_free);
  sheet->next_shape_id = 1;
  sheet->dynamic = g_hash_table_new_full (key_hash, key_equal, g_free, NULL);
  sheet->print.scale = 100;
  sheet->print.margin = 36;
  sheet->row_breaks = g_array_new (FALSE, FALSE, sizeof (int));
  sheet->col_breaks = g_array_new (FALSE, FALSE, sizeof (int));
  sheet->print.header = g_strdup ("&A");
  sheet->print.footer = g_strdup ("Page &P");
  sheet->print.gridlines = TRUE;
  sheet->pivots = g_array_new (FALSE, FALSE, sizeof (O42Pivot));
  sheet->formats = o42_fmt_table_new ();
  sheet->stack = o42_undo_stack_new ();
  sheet->owns_stack = TRUE;

  sheet->pictures = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_picture_free);
  sheet->next_picture_id = 1;
  sheet->charts = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_chart_free);

  sheet->eval.get_cell = sheet_get_cell_value;
  sheet->eval.get_cell_info = sheet_get_cell_info;
  sheet->eval.get_name = sheet_get_name;
  sheet->eval.sheets_between = sheet_sheets_between;
  sheet->eval.get_extent = sheet_get_extent;
  sheet->eval.user_data = sheet;

  return sheet;
}

void
o42_sheet_free (O42Sheet *sheet)
{
  if (sheet == NULL)
    return;

  if (sheet->owns_stack)
    o42_undo_stack_free (sheet->stack);

  g_hash_table_destroy (sheet->formulas);
  g_hash_table_destroy (sheet->volatiles);
  g_hash_table_destroy (sheet->dependents);
  g_hash_table_destroy (sheet->cells);
  g_clear_pointer (&sheet->row_stops, g_array_unref);
  g_clear_pointer (&sheet->col_stops, g_array_unref);
  g_hash_table_destroy (sheet->col_widths);
  g_hash_table_destroy (sheet->row_heights);
  g_hash_table_destroy (sheet->hidden_cols);
  g_hash_table_destroy (sheet->hidden_rows);
  g_hash_table_destroy (sheet->row_levels);
  g_hash_table_destroy (sheet->col_levels);
  g_hash_table_destroy (sheet->filtered_rows);
  g_hash_table_destroy (sheet->filter_choice);
  g_array_free (sheet->merges, TRUE);
  g_hash_table_destroy (sheet->notes);
  g_hash_table_destroy (sheet->links);
  g_array_free (sheet->conditions, TRUE);
  o42_sheet_clear_validations (sheet, NULL);
  g_array_free (sheet->validations, TRUE);
  for (guint i = 0; i < sheet->tables->len; i++)
    g_free (g_array_index (sheet->tables, O42Table, i).name);
  for (guint i = 0; i < sheet->queries->len; i++)
    g_free (g_array_index (sheet->queries, O42Query, i).sql);
  g_array_free (sheet->queries, TRUE);
  g_array_free (sheet->tables, TRUE);
  g_ptr_array_free (sheet->scenarios, TRUE);
  g_ptr_array_free (sheet->shapes, TRUE);
  g_array_free (sheet->arrays, TRUE);
  g_hash_table_destroy (sheet->dynamic);
  g_free (sheet->print.header);
  g_free (sheet->print.footer);
  g_array_unref (sheet->row_breaks);
  g_array_unref (sheet->col_breaks);
  for (guint i = 0; i < sheet->pivots->len; i++)
    pivot_clear (&g_array_index (sheet->pivots, O42Pivot, i));
  g_array_free (sheet->pivots, TRUE);
  o42_fmt_table_free (sheet->formats);

  g_ptr_array_free (sheet->pictures, TRUE);
  g_ptr_array_free (sheet->charts, TRUE);
  g_free (sheet->name);
  g_free (sheet);
}

const char *
o42_sheet_get_name (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, "");
  return sheet->name;
}

void
o42_sheet_set_name (O42Sheet *sheet, const char *name)
{
  g_return_if_fail (sheet != NULL);

  g_free (sheet->name);
  sheet->name = g_strdup (name != NULL ? name : "Sheet1");
}

O42FmtTable *
o42_sheet_fmt_table (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->formats;
}

void
o42_sheet_set_book (O42Sheet *sheet, O42Book *book)
{
  g_return_if_fail (sheet != NULL);

  sheet->book = book;

  /* Joining a book means sharing its history. */
  if (book != NULL)
    {
      if (sheet->owns_stack)
        o42_undo_stack_free (sheet->stack);
      sheet->stack = o42_book_undo_stack (book);
      sheet->owns_stack = FALSE;
    }
  else if (!sheet->owns_stack)
    {
      sheet->stack = o42_undo_stack_new ();
      sheet->owns_stack = TRUE;
    }
}

O42Book *
o42_sheet_get_book (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->book;
}

/* Rewrites every formula on the sheet through `rewrite`, which returns
 * TRUE if it changed the tree it was given.  Each changed cell is
 * captured for undo, so the rewrite is one step. */
typedef gboolean (*TreeRewrite) (O42Node *tree, gpointer user);

static void
sheet_rewrite_formulas (O42Sheet *sheet, TreeRewrite rewrite, gpointer user)
{
  GArray *keys = g_array_new (FALSE, FALSE, sizeof (guint64));
  GHashTableIter iter;
  gpointer key_ptr;

  g_hash_table_iter_init (&iter, sheet->formulas);
  while (g_hash_table_iter_next (&iter, &key_ptr, NULL))
    g_array_append_val (keys, *(guint64 *) key_ptr);

  op_begin (sheet);

  for (guint i = 0; i < keys->len; i++)
    {
      guint64 k = g_array_index (keys, guint64, i);
      O42Cell *cell = sheet_find_key (sheet, k);
      O42Node *tree;

      if (cell == NULL || cell->ast == NULL)
        continue;

      tree = o42_node_copy (cell->ast);
      if (rewrite (tree, user))
        {
          char *text = o42_node_to_string (tree);
          char *input = g_strconcat ("=", text, NULL);

          op_capture (sheet, o42_key_row (k), o42_key_col (k));
          set_input_internal (sheet, o42_key_row (k), o42_key_col (k), input);
          g_free (input);
          g_free (text);
        }
      o42_node_free (tree);
    }

  op_end (sheet);
  g_array_free (keys, TRUE);
}

typedef struct { const char *own, *target; gboolean rows; int at, count; } ShiftArgs;

static gboolean
shift_tree (O42Node *tree, gpointer user)
{
  const ShiftArgs *a = user;
  return o42_node_shift (tree, a->rows, a->at, a->count, a->own, a->target);
}

void
o42_sheet_shift_references (O42Sheet *sheet, const char *target,
                            gboolean rows, int at, int count)
{
  ShiftArgs a;

  g_return_if_fail (sheet != NULL);

  a.own = sheet->name;
  a.target = target;
  a.rows = rows;
  a.at = at;
  a.count = count;
  sheet_rewrite_formulas (sheet, shift_tree, &a);
}

typedef struct { const char *old_name, *new_name; } RenameArgs;

static gboolean
rename_tree (O42Node *tree, gpointer user)
{
  const RenameArgs *a = user;
  return o42_node_rename_sheet (tree, a->old_name, a->new_name);
}

void
o42_sheet_rename_references (O42Sheet *sheet, const char *old_name,
                             const char *new_name)
{
  RenameArgs a = { old_name, new_name };

  g_return_if_fail (sheet != NULL);
  sheet_rewrite_formulas (sheet, rename_tree, &a);
}

/* ---------------------------------------------------------------------- */
/* Content                                                                 */
/* ---------------------------------------------------------------------- */

/* The rectangles behind the defined names a formula uses count as its
 * precedents too, so that a change inside a named range stales it. */
static void
cell_collect_name_precedents (O42Sheet *sheet, O42Cell *cell)
{
  GPtrArray *names = g_ptr_array_new ();

  o42_node_collect_names (cell->ast, names);
  if (names->len == 0)
    {
      g_ptr_array_free (names, TRUE);
      return;
    }

  cell->names = names;
  if (sheet->book == NULL)
    return;

  for (guint i = 0; i < names->len; i++)
    {
      O42Sheet *target = NULL;
      O42SheetRange p;

      if (o42_book_lookup_name (sheet->book, g_ptr_array_index (names, i), &target, &p.range))
        {
          p.sheet = (target == sheet) ? NULL : g_intern_string (target->name);
          g_array_append_val (cell->precedents, p);
        }
    }
}

void
o42_sheet_name_changed (O42Sheet *sheet, const char *upper)
{
  GHashTableIter iter;
  gpointer key_ptr;
  GArray *keys;

  g_return_if_fail (sheet != NULL);

  keys = g_array_new (FALSE, FALSE, sizeof (guint64));
  g_hash_table_iter_init (&iter, sheet->formulas);
  while (g_hash_table_iter_next (&iter, &key_ptr, NULL))
    g_array_append_val (keys, *(guint64 *) key_ptr);

  for (guint i = 0; i < keys->len; i++)
    {
      guint64 k = g_array_index (keys, guint64, i);
      O42Cell *cell = sheet_find_key (sheet, k);
      gboolean uses = FALSE;

      if (cell == NULL || cell->names == NULL)
        continue;
      for (guint j = 0; j < cell->names->len && !uses; j++)
        uses = (g_ptr_array_index (cell->names, j) == (gpointer) upper);
      if (!uses)
        continue;

      /* Re-entering the same text redoes the precedents and stales it. */
      {
        char *input = g_strdup (cell->input);
        set_input_internal (sheet, o42_key_row (k), o42_key_col (k), input);
        g_free (input);
      }
    }

  g_array_free (keys, TRUE);
}

/* A cell that has just been given a date takes a date format if it had
 * none, which is what Excel does on entry and what makes =TODAY() show a
 * date rather than 46261. */
static void
cell_take_format (O42Sheet *sheet, O42Cell *cell, O42NumberFormat which,
                  int decimals)
{
  O42Fmt fmt = *o42_fmt_table_get (sheet->formats, cell->fmt);

  if (fmt.number != O42_NUM_GENERAL)
    return;

  fmt.number = which;
  fmt.decimals = decimals;
  cell->fmt = o42_fmt_table_intern (sheet->formats, &fmt);
}

/* The functions whose result is a date or a time, so that a formula that
 * is a call to one of them formats itself.  Excel's list is about this. */
static O42NumberFormat
formula_date_format (const O42Node *ast)
{
  static const struct { const char *name; O42NumberFormat fmt; } TABLE[] = {
    { "TODAY",     O42_NUM_DATE },
    { "DATE",      O42_NUM_DATE },
    { "DATEVALUE", O42_NUM_DATE },
    { "EDATE",     O42_NUM_DATE },
    { "EOMONTH",   O42_NUM_DATE },
    { "NOW",       O42_NUM_DATETIME },
    { "TIME",      O42_NUM_TIME },
    { "TIMEVALUE", O42_NUM_TIME },
  };

  if (ast == NULL || ast->type != O42_NODE_CALL)
    return O42_NUM_GENERAL;

  for (guint i = 0; i < G_N_ELEMENTS (TABLE); i++)
    if (strcmp (ast->as.call.name, TABLE[i].name) == 0)
      return TABLE[i].fmt;

  return O42_NUM_GENERAL;
}

/* Strips a cell of its content but not its formatting, which is what
 * Delete does and what setting new content has to do first. */
static void
cell_clear_content (O42Sheet *sheet, guint64 key, O42Cell *cell)
{
  o42_value_clear (&cell->value);
  cell->value = o42_value_empty ();

  g_clear_pointer (&cell->input, g_free);
  g_clear_pointer (&cell->ast, o42_node_free);

  if (cell->precedents != NULL)
    {
      deps_remove (sheet, key, cell);
      g_array_free (cell->precedents, TRUE);
      cell->precedents = NULL;
    }
  if (cell->names != NULL)
    {
      g_ptr_array_free (cell->names, TRUE);
      cell->names = NULL;
    }

  cell->dirty = 0;
  g_hash_table_remove (sheet->formulas, &key);
  g_hash_table_remove (sheet->volatiles, &key);
}

/* ---- Recording a macro ----------------------------------------------- */

/* Everything about a format that the Python API can put back: no
 * borders, protection or pattern, which Range.format does not take. */
#define RECORD_FMT_MASK (O42_FMT_FAMILY | O42_FMT_SIZE | O42_FMT_BOLD |     \
                         O42_FMT_ITALIC | O42_FMT_UNDERLINE |               \
                         O42_FMT_STRIKEOUT | O42_FMT_COLOUR | O42_FMT_FILL |\
                         O42_FMT_HALIGN | O42_FMT_VALIGN | O42_FMT_NUMBER | \
                         O42_FMT_DECIMALS | O42_FMT_WRAP | O42_FMT_INDENT | \
                         O42_FMT_ROTATION)


static void record_format (O42Sheet *sheet, const O42Range *range, O42FmtMask mask,
                           const O42Fmt *fmt);

/* A block emptied wholesale, which is what an insert or delete of rows
 * does before it puts the cells back where they land. */
static void
record_clear (O42Sheet *sheet, const O42Range *range)
{
  char *first, *last, *line;

  if (!o42_book_record_sheet (sheet->book, sheet->name))
    return;
  first = o42_ref_name (range->row0, range->col0);
  last = o42_ref_name (range->row1, range->col1);
  line = g_strdup_printf ("sheet[\"%s:%s\"].clear()", first, last);
  o42_book_record_line (sheet->book, line);
  g_free (line);
  g_free (first);
  g_free (last);
}

/* A cell that has been given a format wholesale -- pasted, filled, or
 * moved by an insert of rows -- rather than through apply_fmt. */
static void
record_cell_format (O42Sheet *sheet, int row, int col, O42FmtIdx idx)
{
  O42Range one = { row, col, row, col };

  if (!o42_book_recording (sheet->book) || idx == o42_fmt_table_default (sheet->formats))
    return;
  record_format (sheet, &one, RECORD_FMT_MASK, o42_fmt_table_get (sheet->formats, idx));
}

/* What the user typed into a cell, as the Python that types it again. */
static void
record_input (O42Sheet *sheet, int row, int col, const char *text)
{
  char *where, *quoted, *line;

  if (!o42_book_record_sheet (sheet->book, sheet->name))
    return;

  where = o42_ref_name (row, col);
  quoted = o42_python_quote (text != NULL ? text : "");
  /* A formula is set as one; anything else is a value, which is what
   * the user typed. */
  line = g_strdup_printf ("sheet[\"%s\"].%s = %s", where,
                          (text != NULL && text[0] == '=') ? "formula" : "value", quoted);
  o42_book_record_line (sheet->book, line);
  g_free (line);
  g_free (quoted);
  g_free (where);
}

/* A format, as the keyword arguments Range.format takes. */
static void
record_format (O42Sheet *sheet, const O42Range *range, O42FmtMask mask, const O42Fmt *fmt)
{
  static const char *const HALIGNS[] = { "general", "left", "centre", "right" };
  static const char *const VALIGNS[] = { "bottom", "middle", "top" };
  static const char *const NUMBERS[] = { "general", "fixed", "comma", "currency",
                                         "percent", "scientific", "text", "date",
                                         "time", "datetime" };
  GString *args = g_string_new (NULL);
  char *a, *b, *line;

  if (!o42_book_record_sheet (sheet->book, sheet->name))
    return;

  #define ARG(condition, ...) G_STMT_START {                     \
      if (condition) {                                           \
        if (args->len > 0) g_string_append (args, ", ");         \
        g_string_append_printf (args, __VA_ARGS__);              \
      } } G_STMT_END

  ARG (mask & O42_FMT_BOLD, "bold=%s", fmt->bold ? "True" : "False");
  ARG (mask & O42_FMT_ITALIC, "italic=%s", fmt->italic ? "True" : "False");
  ARG (mask & O42_FMT_UNDERLINE, "underline=%s", fmt->underline ? "True" : "False");
  ARG (mask & O42_FMT_STRIKEOUT, "strikeout=%s", fmt->strikeout ? "True" : "False");
  ARG (mask & O42_FMT_WRAP, "wrap=%s", fmt->wrap ? "True" : "False");
  ARG (mask & O42_FMT_SIZE, "size=%g", fmt->size / 2.0);
  ARG (mask & O42_FMT_FAMILY, "family=\"%s\"", fmt->family != NULL ? fmt->family : "Arial");
  ARG (mask & O42_FMT_COLOUR, "colour=\"#%06X\"", fmt->colour);
  ARG (mask & O42_FMT_INDENT, "indent=%d", fmt->indent);
  ARG (mask & O42_FMT_ROTATION, "rotation=%d", fmt->rotation);
  ARG (mask & O42_FMT_DECIMALS, "decimals=%d", fmt->decimals);
  if (mask & O42_FMT_FILL)
    {
      if (args->len > 0) g_string_append (args, ", ");
      if (fmt->fill == O42_FILL_NONE)
        g_string_append (args, "fill=None");
      else
        g_string_append_printf (args, "fill=\"#%06X\"", fmt->fill);
    }
  ARG (mask & O42_FMT_HALIGN, "halign=\"%s\"", HALIGNS[CLAMP (fmt->halign, 0, 3)]);
  ARG (mask & O42_FMT_VALIGN, "valign=\"%s\"", VALIGNS[CLAMP (fmt->valign, 0, 2)]);
  if (mask & O42_FMT_NUMBER)
    {
      if (args->len > 0) g_string_append (args, ", ");
      if (fmt->custom != NULL)
        g_string_append_printf (args, "number=\"%s\"", fmt->custom);
      else
        g_string_append_printf (args, "number=\"%s\"", NUMBERS[CLAMP (fmt->number, 0, 9)]);
    }
  #undef ARG

  if (args->len == 0)
    {
      g_string_free (args, TRUE);
      return;
    }

  a = o42_ref_name (range->row0, range->col0);
  b = o42_ref_name (range->row1, range->col1);
  if (range->row0 == range->row1 && range->col0 == range->col1)
    line = g_strdup_printf ("sheet[\"%s\"].format(%s)", a, args->str);
  else
    line = g_strdup_printf ("sheet[\"%s:%s\"].format(%s)", a, b, args->str);
  o42_book_record_line (sheet->book, line);
  g_free (line);
  g_free (a);
  g_free (b);
  g_string_free (args, TRUE);
}

static void
set_input_internal (O42Sheet *sheet, int row, int col, const char *text)
{
  if (o42_book_recording (sheet->book))
    record_input (sheet, row, col, text);
  array_dissolve_at (sheet, row, col);
  {
    O42Cell *was = sheet_find (sheet, row, col);
    if (was != NULL) was->spilled = 0;
  }
  guint64 key = o42_key (row, col);
  O42Cell *cell;

  if (row < 0 || col < 0 || row >= O42_MAX_ROWS || col >= O42_MAX_COLS)
    return;

  cell = sheet_ensure (sheet, row, col);
  cell_clear_content (sheet, key, cell);
  /* New text is text of one piece: whatever the old text was set in
   * has nothing to say about it. */
  g_clear_pointer (&cell->runs, g_array_unref);

  if (text != NULL && *text != '\0')
    {
      if (text[0] == '=')
        {
          guint64 *stored;

          O42NumberFormat date_fmt;

          cell->input = g_strdup (text);
          cell->ast = o42_formula_parse (text + 1);
          cell->precedents = g_array_new (FALSE, FALSE, sizeof (O42SheetRange));
          o42_node_collect_refs (cell->ast, cell->precedents);
          cell_collect_name_precedents (sheet, cell);
          deps_add (sheet, key, cell);
          cell->dirty = 1;

          date_fmt = formula_date_format (cell->ast);
          if (date_fmt != O42_NUM_GENERAL)
            cell_take_format (sheet, cell, date_fmt, 0);

          stored = g_new (guint64, 1);
          *stored = key;
          g_hash_table_add (sheet->formulas, stored);
          if (tree_is_volatile (cell->ast))
            {
              stored = g_new (guint64, 1);
              *stored = key;
              g_hash_table_add (sheet->volatiles, stored);
            }
        }
      else
        {
          /* A leading apostrophe forces text, which is how a spreadsheet has
           * always let you type something that looks like a number and mean
           * it as a label. */
          if (text[0] == '\'')
            {
              cell->value = o42_value_text (text + 1);
              cell->input = g_strdup (text);
            }
          else
            {
              O42Entry entry;

              /* TRUE and FALSE typed into a cell are the values, not
               * the words: it is what Excel does, and it is what a
               * check box writes into the cell it drives. */
              if (g_ascii_strcasecmp (text, "TRUE") == 0)
                cell->value = o42_value_bool (TRUE);
              else if (g_ascii_strcasecmp (text, "FALSE") == 0)
                cell->value = o42_value_bool (FALSE);
              else if (o42_entry_parse (text, &entry))
                {
                  /* 5%, $1,000, 1/2/2026 and 3:45 PM are numbers, and
                   * the way they were typed is the way the cell shows
                   * them from now on, unless it already had a format. */
                  cell->value = o42_value_number (entry.number);
                  if (entry.format != O42_NUM_GENERAL)
                    cell_take_format (sheet, cell, entry.format, entry.decimals);
                }
              else
                cell->value = o42_value_text (text);
            }
        }
    }

  sheet_invalidate (sheet, row, col);
  sheet_prune (sheet, row, col);

  /* A formula over a range may spill; evaluating it now puts the
   * spilled cells in place before anyone reads them. */
  {
    O42Cell *fresh = sheet_find (sheet, row, col);
    if (fresh != NULL && fresh->ast != NULL && tree_has_range (fresh->ast) &&
        !sheet->placing_array && array_at (sheet, row, col) == NULL)
      sheet_evaluate (sheet, o42_key (row, col), fresh);
  }
}

void
o42_sheet_set_input (O42Sheet *sheet, int row, int col, const char *text)
{
  g_return_if_fail (sheet != NULL);

  op_begin (sheet);
  op_capture (sheet, row, col);
  set_input_internal (sheet, row, col, text);
  op_end (sheet);
}

char *
o42_sheet_get_input (O42Sheet *sheet, int row, int col)
{
  O42Cell *cell;

  g_return_val_if_fail (sheet != NULL, g_strdup (""));

  cell = sheet_find (sheet, row, col);
  if (cell == NULL)
    return g_strdup ("");

  if (cell->input != NULL)
    return g_strdup (cell->input);

  return value_input_text (&cell->value);
}

void
o42_sheet_get_value (O42Sheet *sheet, int row, int col, O42Value *out)
{
  g_return_if_fail (sheet != NULL);
  g_return_if_fail (out != NULL);

  sheet_get_cell_value (&sheet->eval, NULL, row, col, out);
}

char *
o42_sheet_get_display (O42Sheet *sheet, int row, int col)
{
  O42Value value;
  const O42Fmt *fmt;
  char *text;

  g_return_val_if_fail (sheet != NULL, g_strdup (""));

  o42_sheet_get_value (sheet, row, col, &value);
  fmt = o42_sheet_get_fmt (sheet, row, col);
  text = o42_fmt_display (fmt, &value);
  o42_value_clear (&value);

  return text;
}

gboolean
o42_sheet_has_formula (O42Sheet *sheet, int row, int col)
{
  O42Cell *cell;

  g_return_val_if_fail (sheet != NULL, FALSE);

  cell = sheet_find (sheet, row, col);
  return cell != NULL && cell->ast != NULL;
}

gboolean
o42_sheet_is_empty (O42Sheet *sheet, int row, int col)
{
  O42Cell *cell;

  g_return_val_if_fail (sheet != NULL, TRUE);

  cell = sheet_find (sheet, row, col);
  return cell == NULL ||
         (cell->ast == NULL && cell->value.type == O42_VALUE_EMPTY);
}

/* The cells of a range, when the range may be enormous.
 *
 * A sheet is a million rows by sixteen thousand columns and holds a few
 * hundred cells, so walking a rectangle that covers the whole of it --
 * as clearing a sheet before loading one does -- is seventeen thousand
 * million steps for nothing.  When the rectangle is bigger than the
 * number of cells there are, the cells are walked instead and the ones
 * outside it passed over.  The answer is the same either way; only the
 * time differs, and it differs by everything. */
typedef struct {
  O42Sheet *sheet;
  O42Range  range;
  GArray   *keys;    /* guint64, the cells inside the range */
} RangeWalk;

static void
walk_collect (O42Sheet *sheet, int row, int col, gpointer user)
{
  RangeWalk *walk = user;

  (void) sheet;
  if (o42_range_contains (&walk->range, row, col))
    {
      guint64 key = o42_key (row, col);

      g_array_append_val (walk->keys, key);
    }
}

/* The keys of every stored cell inside `range`, in no particular order.
 * Free with g_array_unref. */
static GArray *
cells_in_range (O42Sheet *sheet, const O42Range *range)
{
  RangeWalk walk;

  walk.sheet = sheet;
  walk.range = *range;
  walk.keys = g_array_new (FALSE, FALSE, sizeof (guint64));
  o42_sheet_foreach_cell (sheet, walk_collect, &walk);
  return walk.keys;
}

/* TRUE when walking the cells beats walking the rectangle. */
static gboolean
range_is_vast (O42Sheet *sheet, const O42Range *range)
{
  double cells = (double) (range->row1 - range->row0 + 1) *
                 (double) (range->col1 - range->col0 + 1);

  return cells > 100000 && cells > 4.0 * g_hash_table_size (sheet->cells);
}

void
o42_sheet_clear_range (O42Sheet *sheet, const O42Range *range)
{
  g_return_if_fail (sheet != NULL);
  g_return_if_fail (range != NULL);

  op_begin (sheet);

  if (range_is_vast (sheet, range))
    {
      GArray *keys = cells_in_range (sheet, range);

      for (guint i = 0; i < keys->len; i++)
        {
          guint64 key = g_array_index (keys, guint64, i);
          int row = o42_key_row (key), col = o42_key_col (key);

          op_capture (sheet, row, col);
          set_input_internal (sheet, row, col, NULL);
        }
      g_array_unref (keys);
    }
  else
    for (int row = range->row0; row <= range->row1; row++)
      for (int col = range->col0; col <= range->col1; col++)
        {
          if (sheet_find (sheet, row, col) == NULL)
            continue;

          op_capture (sheet, row, col);
          set_input_internal (sheet, row, col, NULL);
        }

  op_end (sheet);
}

void
o42_sheet_clear_formats (O42Sheet *sheet, const O42Range *range)
{
  O42FmtIdx def;

  g_return_if_fail (sheet != NULL);
  g_return_if_fail (range != NULL);

  def = o42_fmt_table_default (sheet->formats);

  op_begin (sheet);

  if (range_is_vast (sheet, range))
    {
      GArray *keys = cells_in_range (sheet, range);

      for (guint i = 0; i < keys->len; i++)
        {
          guint64 key = g_array_index (keys, guint64, i);
          int row = o42_key_row (key), col = o42_key_col (key);
          O42Cell *cell = sheet_find (sheet, row, col);

          if (cell == NULL || cell->fmt == def)
            continue;
          op_capture (sheet, row, col);
          cell->fmt = def;
          sheet_prune (sheet, row, col);
        }
      g_array_unref (keys);
    }
  else
    for (int row = range->row0; row <= range->row1; row++)
      for (int col = range->col0; col <= range->col1; col++)
        {
          O42Cell *cell = sheet_find (sheet, row, col);

          if (cell == NULL || cell->fmt == def)
            continue;

          op_capture (sheet, row, col);
          cell->fmt = def;
          sheet_prune (sheet, row, col);
        }

  op_end (sheet);
}

/* ---------------------------------------------------------------------- */
/* Moving cells about                                                      */
/* ---------------------------------------------------------------------- */

char *
o42_sheet_get_input_relocated (O42Sheet *sheet, int row, int col,
                               int drow, int dcol)
{
  O42Cell *cell;
  O42Node *moved;
  char *text, *result;

  g_return_val_if_fail (sheet != NULL, g_strdup (""));

  cell = sheet_find (sheet, row, col);
  if (cell == NULL || cell->ast == NULL)
    return o42_sheet_get_input (sheet, row, col);

  moved = o42_node_copy (cell->ast);
  if (!o42_node_relocate (moved, drow, dcol))
    {
      o42_node_free (moved);
      return g_strdup (cell->input);
    }

  text = o42_node_to_string (moved);
  result = g_strconcat ("=", text, NULL);
  g_free (text);
  o42_node_free (moved);

  return result;
}

/* One cell's worth of what a copy carries. */
typedef struct {
  char      *input;
  O42FmtIdx  fmt;
} Carried;

static void
sheet_put_carried (O42Sheet *sheet, int row, int col, const Carried *c)
{
  O42Cell *cell;

  op_capture (sheet, row, col);
  set_input_internal (sheet, row, col, c->input);

  if (c->fmt != o42_fmt_table_default (sheet->formats) ||
      sheet_find (sheet, row, col) != NULL)
    {
      cell = sheet_ensure (sheet, row, col);
      cell->fmt = c->fmt;
      record_cell_format (sheet, row, col, c->fmt);
      sheet_prune (sheet, row, col);
    }
}

void
o42_sheet_copy_range (O42Sheet *sheet, const O42Range *source,
                      int row, int col)
{
  o42_sheet_copy_range_special (sheet, source, row, col, O42_PASTE_ALL, FALSE);
}

/* A cell's value as something that can be typed back in: the text of a
 * formula's result, forced with an apostrophe if it would read as a
 * number or a date. */
static char *
cell_value_as_input (O42Sheet *sheet, int row, int col)
{
  O42Value v;
  char *text;

  o42_sheet_get_value (sheet, row, col, &v);
  if (v.type == O42_VALUE_EMPTY)
    { o42_value_clear (&v); return NULL; }

  if (v.type == O42_VALUE_TEXT)
    {
      O42Value probe = o42_value_text (v.as.text);
      double n;
      O42ErrorCode e = O42_ERR_VALUE;

      if (o42_value_to_number (&probe, &n, &e) || o42_date_parse (v.as.text, NULL, NULL, NULL) ||
          v.as.text[0] == '=' || v.as.text[0] == '\'')
        text = g_strconcat ("'", v.as.text, NULL);
      else
        text = g_strdup (v.as.text);
      o42_value_clear (&probe);
    }
  else if (v.type == O42_VALUE_BOOL)
    text = g_strdup (v.as.boolean ? "=TRUE" : "=FALSE");
  else if (v.type == O42_VALUE_ERROR)
    text = g_strconcat ("=", o42_error_name (v.as.error), NULL);
  else
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      text = g_strdup (g_ascii_dtostr (buf, sizeof buf, v.as.number));
    }

  o42_value_clear (&v);
  return text;
}

void
o42_sheet_copy_range_special (O42Sheet *sheet, const O42Range *source,
                              int row, int col, O42PasteMode mode,
                              gboolean transpose)
{
  int rows, cols, out_rows, out_cols;
  int drow, dcol;
  Carried *carried;

  g_return_if_fail (sheet != NULL);
  g_return_if_fail (source != NULL);

  rows = source->row1 - source->row0 + 1;
  cols = source->col1 - source->col0 + 1;
  out_rows = transpose ? cols : rows;
  out_cols = transpose ? rows : cols;
  drow = row - source->row0;
  dcol = col - source->col0;

  if (drow == 0 && dcol == 0 && !transpose && mode == O42_PASTE_ALL)
    return;
  if (row < 0 || col < 0 || row + out_rows > O42_MAX_ROWS || col + out_cols > O42_MAX_COLS)
    return;

  /* Everything is read before anything is written, because the source and
   * the destination may overlap. */
  carried = g_new0 (Carried, (gsize) rows * cols);
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      {
        Carried *k = &carried[r * cols + c];
        int sr = source->row0 + r, sc = source->col0 + c;
        int tr = transpose ? row + c : row + r;
        int tc = transpose ? col + r : col + c;

        switch (mode)
          {
          case O42_PASTE_VALUES:
            k->input = cell_value_as_input (sheet, sr, sc);
            break;
          case O42_PASTE_FORMATS:
            k->input = o42_sheet_get_input (sheet, tr, tc);   /* keeps what is there */
            if (*k->input == '\0') { g_free (k->input); k->input = NULL; }
            break;
          default:
            k->input = o42_sheet_get_input_relocated (sheet, sr, sc, tr - sr, tc - sc);
            break;
          }

        k->fmt = (mode == O42_PASTE_VALUES || mode == O42_PASTE_FORMULAS)
                 ? o42_sheet_get_fmt_idx (sheet, tr, tc)
                 : o42_sheet_get_fmt_idx (sheet, sr, sc);
      }

  op_begin (sheet);
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      {
        int tr = transpose ? row + c : row + r;
        int tc = transpose ? col + r : col + c;
        sheet_put_carried (sheet, tr, tc, &carried[r * cols + c]);
      }
  op_end (sheet);

  for (int i = 0; i < rows * cols; i++)
    g_free (carried[i].input);
  g_free (carried);
}

void
o42_sheet_fill (O42Sheet *sheet, const O42Range *range, gboolean down)
{
  g_return_if_fail (sheet != NULL);
  g_return_if_fail (range != NULL);

  op_begin (sheet);

  if (down)
    {
      for (int row = range->row0 + 1; row <= range->row1; row++)
        for (int col = range->col0; col <= range->col1; col++)
          {
            Carried k;

            k.input = o42_sheet_get_input_relocated (sheet, range->row0, col,
                                                     row - range->row0, 0);
            k.fmt = o42_sheet_get_fmt_idx (sheet, range->row0, col);
            sheet_put_carried (sheet, row, col, &k);
            g_free (k.input);
          }
    }
  else
    {
      for (int col = range->col0 + 1; col <= range->col1; col++)
        for (int row = range->row0; row <= range->row1; row++)
          {
            Carried k;

            k.input = o42_sheet_get_input_relocated (sheet, row, range->col0,
                                                     0, col - range->col0);
            k.fmt = o42_sheet_get_fmt_idx (sheet, row, range->col0);
            sheet_put_carried (sheet, row, col, &k);
            g_free (k.input);
          }
    }

  op_end (sheet);
}

/* ---- Autofill --------------------------------------------------------- */

static const char *const MONTHS[] = {
  "January", "February", "March", "April", "May", "June", "July",
  "August", "September", "October", "November", "December"
};
static const char *const DAYS[] = {
  "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
};

/* Is `text` one of a list of names, or its three-letter form?  Returns the
 * index, with `abbreviated` and `capitalised` describing how it was
 * written so the continuation can match. */
static int
name_in_cycle (const char *text, const char *const *names, int n,
               gboolean *abbreviated)
{
  for (int i = 0; i < n; i++)
    {
      if (g_ascii_strcasecmp (text, names[i]) == 0)
        { *abbreviated = FALSE; return i; }
      if (strlen (text) == 3 && g_ascii_strncasecmp (text, names[i], 3) == 0)
        { *abbreviated = TRUE; return i; }
    }
  return -1;
}

/* Writes a cycle name in the same shape the source used. */
static char *
cycle_name (const char *like, const char *const *names, int index,
            gboolean abbreviated)
{
  char *name = abbreviated ? g_strndup (names[index], 3) : g_strdup (names[index]);

  if (g_ascii_isupper (like[0]) && g_ascii_isupper (like[1]))
    { char *u = g_ascii_strup (name, -1); g_free (name); name = u; }
  else if (g_ascii_islower (like[0]))
    { char *l = g_ascii_strdown (name, -1); g_free (name); name = l; }

  return name;
}

/* The k-th continuation (k = 1, 2, ...) of the run of source inputs along
 * one line, for cell index `n` (source cells are 0 .. count-1, targets
 * count, count+1, ...).  Negative k fills backwards. */
static char *
autofill_value (O42Sheet *sheet, const int *rows, const int *cols, int count,
                int n)
{
  int period_index = ((n % count) + count) % count;
  int step = (n >= count) ? n - (count - 1) : n;   /* distance from the nearer end */
  O42Value first, last, v;
  char *input;

  /* Formulas and anything not a number: repeat the pattern, relocating
   * formulas as a fill would. */
  if (o42_sheet_has_formula (sheet, rows[period_index], cols[period_index]))
    return o42_sheet_get_input_relocated (sheet, rows[period_index], cols[period_index],
                                          rows[n] - rows[period_index],
                                          cols[n] - cols[period_index]);

  o42_sheet_get_value (sheet, rows[0], cols[0], &first);
  o42_sheet_get_value (sheet, rows[count - 1], cols[count - 1], &last);
  o42_sheet_get_value (sheet, rows[period_index], cols[period_index], &v);

  /* Numbers: a progression when there are two or more, a copy of one. */
  if (v.type == O42_VALUE_NUMBER)
    {
      gboolean all_numbers = TRUE;
      double delta = 0;

      for (int i = 0; i < count && all_numbers; i++)
        {
          O42Value w;
          o42_sheet_get_value (sheet, rows[i], cols[i], &w);
          all_numbers = (w.type == O42_VALUE_NUMBER);
          o42_value_clear (&w);
        }

      if (all_numbers && count >= 2)
        {
          delta = (last.as.number - first.as.number) / (count - 1);
          {
            double base = (n >= count) ? last.as.number : first.as.number;
            O42Value r = o42_value_number (base + delta * step);
            input = value_input_text (&r);
          }
        }
      else
        {
          const O42Fmt *fmt = o42_sheet_get_fmt (sheet, rows[period_index], cols[period_index]);
          O42Value r;

          /* A lone date steps by the day; a lone number is copied. */
          if (fmt->number == O42_NUM_DATE || fmt->number == O42_NUM_DATETIME)
            r = o42_value_number (v.as.number + step);
          else
            r = o42_value_copy (&v);
          input = value_input_text (&r);
        }

      o42_value_clear (&first); o42_value_clear (&last); o42_value_clear (&v);
      return input;
    }

  if (v.type == O42_VALUE_TEXT)
    {
      const char *text = v.as.text;
      gboolean abbreviated = FALSE;
      int idx;
      gsize len = strlen (text);
      gsize digits = 0;

      /* Months and days cycle. */
      if ((idx = name_in_cycle (text, MONTHS, 12, &abbreviated)) >= 0)
        {
          input = cycle_name (text, MONTHS, (((idx + n - period_index) % 12) + 12) % 12, abbreviated);
          goto done_text;
        }
      if ((idx = name_in_cycle (text, DAYS, 7, &abbreviated)) >= 0)
        {
          input = cycle_name (text, DAYS, (((idx + n - period_index) % 7) + 7) % 7, abbreviated);
          goto done_text;
        }

      /* A list the book was given: it goes round the same way the days
       * and the months do. */
      if (sheet->book != NULL)
        {
          int at = 0;
          int which = o42_book_custom_list_find (sheet->book, text, &at);

          if (which >= 0)
            {
              GPtrArray *lists = o42_book_custom_lists (sheet->book);
              char **items = g_ptr_array_index (lists, which);
              int count_in = (int) g_strv_length (items);

              input = g_strdup (items[(((at + n - period_index) % count_in) + count_in) % count_in]);
              goto done_text;
            }
        }

      /* "Item1" counts on; the run of digits at the end is the counter. */
      while (digits < len && g_ascii_isdigit (text[len - 1 - digits]))
        digits++;
      if (digits > 0 && digits < len)
        {
          char *prefix = g_strndup (text, len - digits);
          long number = strtol (text + len - digits, NULL, 10);
          long delta = step;

          /* With two or more, the step is theirs. */
          if (count >= 2 && last.type == O42_VALUE_TEXT && first.type == O42_VALUE_TEXT)
            {
              gsize fl = strlen (first.as.text), ll = strlen (last.as.text), fd = 0, ld = 0;
              while (fd < fl && g_ascii_isdigit (first.as.text[fl - 1 - fd])) fd++;
              while (ld < ll && g_ascii_isdigit (last.as.text[ll - 1 - ld])) ld++;
              if (fd > 0 && ld > 0)
                {
                  long a = strtol (first.as.text + fl - fd, NULL, 10);
                  long b = strtol (last.as.text + ll - ld, NULL, 10);
                  long per = (b - a) / (count - 1);
                  number = (n >= count) ? b : a;
                  delta = per * step;
                }
            }

          input = g_strdup_printf ("%s%0*ld", prefix, (int) digits, MAX (number + delta, 0L));
          g_free (prefix);
          goto done_text;
        }

      input = g_strdup (text);
      /* A label that reads as a number or a date would turn into one. */
      {
        O42Value probe = o42_value_text (input);
        double d;
        O42ErrorCode e = O42_ERR_VALUE;
        if (o42_value_to_number (&probe, &d, &e) || o42_date_parse (input, NULL, NULL, NULL))
          {
            char *forced = g_strconcat ("'", input, NULL);
            g_free (input);
            input = forced;
          }
        o42_value_clear (&probe);
      }

    done_text:
      o42_value_clear (&first); o42_value_clear (&last); o42_value_clear (&v);
      return input;
    }

  /* Booleans, errors, blanks: copy the pattern. */
  o42_value_clear (&first); o42_value_clear (&last); o42_value_clear (&v);
  return o42_sheet_get_input (sheet, rows[period_index], cols[period_index]);
}

void
o42_sheet_autofill (O42Sheet *sheet, const O42Range *source,
                    const O42Range *target)
{
  gboolean vertical;
  int lines, count, total, start;
  Carried *carried;

  g_return_if_fail (sheet != NULL);
  g_return_if_fail (source != NULL && target != NULL);

  /* The target reaches past the source either down (or up) or across; the
   * direction with the difference wins. */
  vertical = (target->row0 != source->row0 || target->row1 != source->row1);
  lines = vertical ? source->col1 - source->col0 + 1 : source->row1 - source->row0 + 1;
  count = vertical ? source->row1 - source->row0 + 1 : source->col1 - source->col0 + 1;
  total = vertical ? target->row1 - target->row0 + 1 : target->col1 - target->col0 + 1;
  start = vertical ? source->row0 - target->row0 : source->col0 - target->col0;

  if (total <= count || lines <= 0)
    return;

  carried = g_new0 (Carried, (gsize) lines * total);

  for (int line = 0; line < lines; line++)
    {
      int *rows = g_new (int, total), *cols = g_new (int, total);

      /* Every cell of the line, source and target, in order. */
      for (int i = 0; i < total; i++)
        {
          rows[i] = vertical ? target->row0 + i : source->row0 + line;
          cols[i] = vertical ? source->col0 + line : target->col0 + i;
        }

      for (int i = 0; i < total; i++)
        {
          int n = i - start;
          Carried *k = &carried[line * total + i];

          if (n >= 0 && n < count)
            continue;                              /* the source stays */

          /* autofill_value indexes rows/cols by n relative to the source
           * start, so hand it the arrays offset by `start`. */
          k->input = autofill_value (sheet, rows + start, cols + start, count, n);
          k->fmt = o42_sheet_get_fmt_idx (sheet,
                                          rows[start + (((n % count) + count) % count)],
                                          cols[start + (((n % count) + count) % count)]);
        }

      g_free (rows);
      g_free (cols);
    }

  op_begin (sheet);
  for (int line = 0; line < lines; line++)
    for (int i = 0; i < total; i++)
      {
        int n = i - start;
        Carried *k = &carried[line * total + i];

        if (n >= 0 && n < count)
          continue;
        sheet_put_carried (sheet,
                           vertical ? target->row0 + i : source->row0 + line,
                           vertical ? source->col0 + line : target->col0 + i,
                           k);
      }
  op_end (sheet);

  for (int i = 0; i < lines * total; i++)
    g_free (carried[i].input);
  g_free (carried);
}

void
o42_sheet_edge (O42Sheet *sheet, int *row, int *col, int drow, int dcol)
{
  int r, c;
  gboolean here_filled;

  g_return_if_fail (sheet != NULL);
  g_return_if_fail (row != NULL && col != NULL);

  r = *row + drow;
  c = *col + dcol;
  if (r < 0 || c < 0 || r >= O42_MAX_ROWS || c >= O42_MAX_COLS)
    return;

  here_filled = !o42_sheet_is_empty (sheet, *row, *col);

  if (here_filled && !o42_sheet_is_empty (sheet, r, c))
    {
      /* In a run: go to its end. */
      while (r + drow >= 0 && c + dcol >= 0 &&
             r + drow < O42_MAX_ROWS && c + dcol < O42_MAX_COLS &&
             !o42_sheet_is_empty (sheet, r + drow, c + dcol))
        { r += drow; c += dcol; }
    }
  else
    {
      /* Across the blanks to the next filled cell, or the edge. */
      while (r >= 0 && c >= 0 && r < O42_MAX_ROWS && c < O42_MAX_COLS &&
             o42_sheet_is_empty (sheet, r, c))
        { r += drow; c += dcol; }
      if (r < 0) r = 0;
      if (c < 0) c = 0;
      if (r >= O42_MAX_ROWS) r = O42_MAX_ROWS - 1;
      if (c >= O42_MAX_COLS) c = O42_MAX_COLS - 1;
    }

  *row = r;
  *col = c;
}

/* Inserting or deleting a band of rows (or columns).  Every stored cell is
 * read into a list with the place it will land and the text its formula
 * should then have, the sheet is emptied of them, and they are put back:
 * two passes, so that a cell is never written over one that has not been
 * read yet.  Each cell is captured for undo on the way, which makes one
 * insert one undo step without any machinery of its own. */
typedef struct {
  int        row, col;     /* where it lands; -1 when deleted */
  char      *input;
  O42FmtIdx  fmt;
} Landing;

static void
sheet_shift_band_within (O42Sheet *sheet, gboolean rows, int at, int count,
                         int band_lo, int band_hi);

static void
sheet_shift_band (O42Sheet *sheet, gboolean rows, int at, int count)
{
  sheet_shift_band_within (sheet, rows, at, count, -1, -1);
}

/* With a band, only cells whose other coordinate lies in band_lo..band_hi
 * move: Insert > Cells shifting a block down within its own columns.
 * Without one it is a whole-sheet insert or delete of rows or columns. */
/* A table of texts keyed by cell, with the keys moved as the cells
 * are by an insert or delete of `count` rows (or columns) at `at`. */
static GHashTable *
shift_keyed_texts (GHashTable *table, gboolean rows, int at, int count)
{
  GHashTable *moved = g_hash_table_new_full (key_hash, key_equal, g_free, g_free);
  GHashTableIter iter;
  gpointer key_ptr, value;

  g_hash_table_iter_init (&iter, table);
  while (g_hash_table_iter_next (&iter, &key_ptr, &value))
    {
      guint64 key = *(guint64 *) key_ptr;
      int r = o42_key_row (key), c = o42_key_col (key);
      int *idx = rows ? &r : &c;
      gboolean keep = TRUE;

      if (count > 0)
        { if (*idx >= at) *idx += count; }
      else if (*idx >= at - count)
        *idx += count;
      else if (*idx >= at)
        keep = FALSE;

      if (keep && r < O42_MAX_ROWS && c < O42_MAX_COLS)
        {
          guint64 *stored = g_new (guint64, 1);
          *stored = o42_key (r, c);
          g_hash_table_insert (moved, stored, g_strdup (value));
        }
    }
  g_hash_table_destroy (table);
  return moved;
}

static void
sheet_shift_band_within (O42Sheet *sheet, gboolean rows, int at, int count,
                         int band_lo, int band_hi)
{
  GArray *landings;
  GHashTableIter iter;
  gpointer key_ptr, value;
  int limit = rows ? O42_MAX_ROWS : O42_MAX_COLS;
  gboolean whole = (band_lo < 0);

  if (count == 0 || at < 0 || at >= limit)
    return;
  if (count < 0 && at - count > limit)
    count = at - limit;
  if (count == 0)
    return;

  landings = g_array_new (FALSE, FALSE, sizeof (Landing));

  g_hash_table_iter_init (&iter, sheet->cells);
  while (g_hash_table_iter_next (&iter, &key_ptr, &value))
    {
      guint64 key = *(guint64 *) key_ptr;
      O42Cell *cell = value;
      Landing l;
      int idx = rows ? o42_key_row (key) : o42_key_col (key);
      int moved;

      l.row = o42_key_row (key);
      l.col = o42_key_col (key);
      l.fmt = cell->fmt;

      {
        int other = rows ? l.col : l.row;
        gboolean in_band = whole || (other >= band_lo && other <= band_hi);

        if (!in_band)
          moved = idx;
        else if (count > 0)
          moved = (idx >= at) ? idx + count : idx;
        else if (idx >= at - count)
          moved = idx + count;
        else if (idx >= at)
          moved = -1;
        else
          moved = idx;
      }

      if (moved >= limit)
        moved = -1;

      if (rows) l.row = moved; else l.col = moved;

      if (cell->ast != NULL)
        {
          O42Node *tree = o42_node_copy (cell->ast);
          char *text;

          if (whole)
            o42_node_shift (tree, rows, at, count, sheet->name, sheet->name);
          else
            o42_node_shift_within (tree, rows, at, count, band_lo, band_hi,
                                   sheet->name, sheet->name);
          text = o42_node_to_string (tree);
          l.input = g_strconcat ("=", text, NULL);
          g_free (text);
          o42_node_free (tree);
        }
      else if (cell->input != NULL)
        l.input = g_strdup (cell->input);
      else if (cell->value.type != O42_VALUE_EMPTY)
        l.input = value_input_text (&cell->value);
      else
        l.input = NULL;

      g_array_append_val (landings, l);
    }

  op_begin (sheet);

  /* Capture every cell that is about to change -- the old places and the
   * new -- before touching any of them. */
  {
    GHashTable *seen = g_hash_table_new (g_int64_hash, g_int64_equal);
    GPtrArray *keys = g_ptr_array_new_with_free_func (g_free);

    g_hash_table_iter_init (&iter, sheet->cells);
    while (g_hash_table_iter_next (&iter, &key_ptr, NULL))
      {
        guint64 *k = g_new (guint64, 1);
        *k = *(guint64 *) key_ptr;
        /* Not g_hash_table_add alone: on a key already present it keeps
         * the new pointer and drops the old, and freeing the new one
         * would leave the set pointing at freed memory, which the next
         * allocation may reuse -- then a cell's new place looks seen
         * already and is never captured, and undo leaves it behind. */
        if (!g_hash_table_contains (seen, k))
          {
            g_hash_table_add (seen, k);
            g_ptr_array_add (keys, k);
          }
        else
          g_free (k);
      }
    for (guint i = 0; i < landings->len; i++)
      {
        const Landing *l = &g_array_index (landings, Landing, i);
        guint64 *k;

        if (l->row < 0 || l->col < 0)
          continue;
        k = g_new (guint64, 1);
        *k = o42_key (l->row, l->col);
        /* Not g_hash_table_add alone: on a key already present it keeps
         * the new pointer and drops the old, and freeing the new one
         * would leave the set pointing at freed memory, which the next
         * allocation may reuse -- then a cell's new place looks seen
         * already and is never captured, and undo leaves it behind. */
        if (!g_hash_table_contains (seen, k))
          {
            g_hash_table_add (seen, k);
            g_ptr_array_add (keys, k);
          }
        else
          g_free (k);
      }

    for (guint i = 0; i < keys->len; i++)
      {
        guint64 k = *(guint64 *) g_ptr_array_index (keys, i);
        op_capture (sheet, o42_key_row (k), o42_key_col (k));
      }

    g_hash_table_destroy (seen);
    g_ptr_array_free (keys, TRUE);
  }

  /* Empty the sheet of every stored cell, then put them back where they
   * land.  Clearing by hand rather than through set_input_internal keeps
   * this from invalidating formulas once per cell. */
  if (o42_book_recording (sheet->book))
    {
      /* The macro has to empty what is there before the cells go back,
       * or the ones that moved would leave copies behind. */
      O42Range used;

      o42_sheet_used_range (sheet, &used);
      record_clear (sheet, &used);
    }
  g_hash_table_remove_all (sheet->formulas);
  g_hash_table_remove_all (sheet->cells);
  sheet->used_valid = FALSE;

  for (guint i = 0; i < landings->len; i++)
    {
      Landing *l = &g_array_index (landings, Landing, i);

      if (l->row >= 0 && l->col >= 0)
        {
          set_input_internal (sheet, l->row, l->col, l->input);
          if (l->fmt != o42_fmt_table_default (sheet->formats))
            {
              sheet_ensure (sheet, l->row, l->col)->fmt = l->fmt;
              record_cell_format (sheet, l->row, l->col, l->fmt);
            }
        }
      g_free (l->input);
    }

  /* The other sheets' formulas that point here are rewritten inside the
   * same group, so that one undo puts everything back. */
  if (whole && sheet->book != NULL)
    o42_book_sheet_shifted (sheet->book, sheet, rows, at, count);

  op_end (sheet);
  g_array_free (landings, TRUE);

  if (!whole)
    return;

  /* Notes and hyperlinks move with their cells; one on a deleted cell
   * is gone. */
  sheet->notes = shift_keyed_texts (sheet->notes, rows, at, count);
  sheet->links = shift_keyed_texts (sheet->links, rows, at, count);

  /* Conditional formats' ranges move likewise. */
  for (guint i = 0; i < sheet->conditions->len; )
    {
      O42Condition *c = &g_array_index (sheet->conditions, O42Condition, i);
      int *lo = rows ? &c->range.row0 : &c->range.col0;
      int *hi = rows ? &c->range.row1 : &c->range.col1;

      if (count > 0)
        {
          if (*lo >= at) *lo += count;
          if (*hi >= at) *hi += count;
        }
      else
        {
          if (*lo >= at - count) *lo += count; else if (*lo >= at) *lo = at;
          if (*hi >= at - count) *hi += count; else if (*hi >= at) *hi = at - 1;
        }
      if (*hi < *lo || *hi >= limit)
        g_array_remove_index (sheet->conditions, i);
      else
        i++;
    }

  /* Page breaks move with the rows and columns they sit above. */
  {
    GArray *breaks = rows ? sheet->row_breaks : sheet->col_breaks;
    for (guint i = 0; i < breaks->len; )
      {
        int *b = &g_array_index (breaks, int, i);
        if (count > 0)
          { if (*b >= at) *b += count; }
        else if (*b >= at - count)
          *b += count;
        else if (*b >= at)
          { g_array_remove_index (breaks, i); continue; }
        i++;
      }
  }

  /* A table grows when rows are put inside it and shrinks when they
   * go; outside, it moves. */
  for (guint i = 0; i < sheet->tables->len; )
    {
      O42Table *t = &g_array_index (sheet->tables, O42Table, i);
      int *lo = rows ? &t->range.row0 : &t->range.col0;
      int *hi = rows ? &t->range.row1 : &t->range.col1;

      if (count > 0)
        {
          if (*lo >= at) *lo += count;
          if (*hi >= at) *hi += count;
        }
      else
        {
          if (*lo >= at - count) *lo += count; else if (*lo >= at) *lo = at;
          if (*hi >= at - count) *hi += count; else if (*hi >= at) *hi = at - 1;
        }
      if (*hi < *lo || *hi >= limit)
        {
          g_free (t->name);
          g_array_remove_index (sheet->tables, i);
        }
      else
        i++;
    }

  /* Array formula blocks move whole. */
  for (guint i = 0; i < sheet->arrays->len; )
    {
      O42Range *a = &g_array_index (sheet->arrays, O42Range, i);
      int *lo = rows ? &a->row0 : &a->col0;
      int *hi = rows ? &a->row1 : &a->col1;

      if (count > 0)
        {
          if (*lo >= at) *lo += count;
          if (*hi >= at) *hi += count;
        }
      else
        {
          if (*lo >= at - count) *lo += count; else if (*lo >= at) *lo = at;
          if (*hi >= at - count) *hi += count; else if (*hi >= at) *hi = at - 1;
        }
      if (*hi < *lo || *hi >= limit)
        g_array_remove_index (sheet->arrays, i);
      else
        i++;
    }

  /* And validation rules'. */
  for (guint i = 0; i < sheet->validations->len; )
    {
      O42Validation *v = &g_array_index (sheet->validations, O42Validation, i);
      int *lo = rows ? &v->range.row0 : &v->range.col0;
      int *hi = rows ? &v->range.row1 : &v->range.col1;

      if (count > 0)
        {
          if (*lo >= at) *lo += count;
          if (*hi >= at) *hi += count;
        }
      else
        {
          if (*lo >= at - count) *lo += count; else if (*lo >= at) *lo = at;
          if (*hi >= at - count) *hi += count; else if (*hi >= at) *hi = at - 1;
        }
      if (*hi < *lo || *hi >= limit)
        {
          g_free (v->value); g_free (v->value2); g_free (v->message);
          g_array_remove_index (sheet->validations, i);
        }
      else
        i++;
    }

  /* Merged ranges move as a formula's ranges would, and one wholly
   * deleted is gone. */
  for (guint i = 0; i < sheet->merges->len; )
    {
      O42Range *m = &g_array_index (sheet->merges, O42Range, i);
      int *lo = rows ? &m->row0 : &m->col0;
      int *hi = rows ? &m->row1 : &m->col1;

      if (count > 0)
        {
          if (*lo >= at) *lo += count;
          if (*hi >= at) *hi += count;
        }
      else
        {
          if (*lo >= at - count) *lo += count; else if (*lo >= at) *lo = at;
          if (*hi >= at - count) *hi += count; else if (*hi >= at) *hi = at - 1;
        }

      if (*hi < *lo || *hi >= limit ||
          (m->row0 == m->row1 && m->col0 == m->col1))
        g_array_remove_index (sheet->merges, i);
      else
        i++;
    }

  /* The filter's range moves with the rows; a change of the data under
   * it is re-filtered afterwards. */
  if (sheet->has_filter)
    {
      int *lo = rows ? &sheet->filter.row0 : &sheet->filter.col0;
      int *hi = rows ? &sheet->filter.row1 : &sheet->filter.col1;

      if (count > 0)
        {
          if (*lo >= at) *lo += count;
          if (*hi >= at) *hi += count;
        }
      else
        {
          if (*lo >= at - count) *lo += count; else if (*lo >= at) *lo = at;
          if (*hi >= at - count) *hi += count; else if (*hi >= at) *hi = at - 1;
        }
      if (*hi < *lo)
        {
          sheet->has_filter = FALSE;
          g_hash_table_remove_all (sheet->filter_choice);
        }
    }

  /* Sizes and pictures move with the cells.  Neither is in the undo
   * history yet. */
  {
    GHashTable *tables[3];
    int n_tables = 0;

    sizes_changed (sheet);
    tables[n_tables++] = rows ? sheet->row_heights : sheet->col_widths;
    tables[n_tables++] = rows ? sheet->hidden_rows : sheet->hidden_cols;
    tables[n_tables++] = rows ? sheet->row_levels : sheet->col_levels;

    for (int t = 0; t < n_tables; t++)
      {
        GHashTable *sizes = tables[t];
        GHashTable *moved = g_hash_table_new (g_direct_hash, g_direct_equal);

        g_hash_table_iter_init (&iter, sizes);
        while (g_hash_table_iter_next (&iter, &key_ptr, &value))
          {
            int idx = GPOINTER_TO_INT (key_ptr);
            int to;

            if (count > 0)
              to = (idx >= at) ? idx + count : idx;
            else if (idx >= at - count)
              to = idx + count;
            else if (idx >= at)
              continue;
            else
              to = idx;

            if (to < limit)
              g_hash_table_insert (moved, GINT_TO_POINTER (to), value);
          }

        g_hash_table_remove_all (sizes);
        g_hash_table_iter_init (&iter, moved);
        while (g_hash_table_iter_next (&iter, &key_ptr, &value))
          g_hash_table_insert (sizes, key_ptr, value);
        g_hash_table_destroy (moved);
      }

    autofilter_apply (sheet);
  }

  for (guint i = 0; i < sheet->pictures->len; i++)
    {
      O42Picture *pic = g_ptr_array_index (sheet->pictures, i);
      int *idx = rows ? &pic->row : &pic->col;

      if (count > 0)
        {
          if (*idx >= at) *idx = MIN (*idx + count, limit - 1);
        }
      else if (*idx >= at - count)
        *idx += count;
      else if (*idx >= at)
        *idx = at;
    }

  for (guint i = 0; i < sheet->shapes->len; i++)
    {
      O42Shape *shape = g_ptr_array_index (sheet->shapes, i);
      int *idx = rows ? &shape->row : &shape->col;

      if (count > 0)
        {
          if (*idx >= at) *idx = MIN (*idx + count, limit - 1);
        }
      else if (*idx >= at - count)
        *idx += count;
      else if (*idx >= at)
        *idx = at;
    }

  /* A chart moves with its anchor, and its data range moves as a range
   * in a formula would. */
  for (guint i = 0; i < sheet->charts->len; i++)
    {
      O42Chart *chart = g_ptr_array_index (sheet->charts, i);
      int *idx = rows ? &chart->row : &chart->col;
      int *lo = rows ? &chart->data.row0 : &chart->data.col0;
      int *hi = rows ? &chart->data.row1 : &chart->data.col1;

      if (count > 0)
        {
          if (*idx >= at) *idx = MIN (*idx + count, limit - 1);
          if (*lo >= at) *lo = MIN (*lo + count, limit - 1);
          if (*hi >= at) *hi = MIN (*hi + count, limit - 1);
        }
      else
        {
          if (*idx >= at - count) *idx += count; else if (*idx >= at) *idx = at;
          if (*lo >= at - count) *lo += count; else if (*lo >= at) *lo = at;
          if (*hi >= at - count) *hi += count; else if (*hi >= at) *hi = at - 1;
          if (*hi < *lo) *hi = *lo;
        }
    }

  deps_rebuild (sheet);
}

void
o42_sheet_shift_cells (O42Sheet *sheet, const O42Range *range,
                       gboolean down, gboolean insert)
{
  int count;

  g_return_if_fail (sheet != NULL);
  g_return_if_fail (range != NULL);

  if (down)
    {
      count = range->row1 - range->row0 + 1;
      sheet_shift_band_within (sheet, TRUE, range->row0, insert ? count : -count,
                               range->col0, range->col1);
    }
  else
    {
      count = range->col1 - range->col0 + 1;
      sheet_shift_band_within (sheet, FALSE, range->col0, insert ? count : -count,
                               range->row0, range->row1);
    }
}

void
o42_sheet_insert_rows (O42Sheet *sheet, int at, int count)
{
  g_return_if_fail (sheet != NULL);
  if (count > 0)
    {
      op_begin (sheet);
      obj_capture_all (sheet);
      sheet_shift_band (sheet, TRUE, at, count);
      op_end (sheet);
    }
}

void
o42_sheet_delete_rows (O42Sheet *sheet, int at, int count)
{
  g_return_if_fail (sheet != NULL);
  if (count > 0)
    {
      op_begin (sheet);
      obj_capture_all (sheet);
      sheet_shift_band (sheet, TRUE, at, -count);
      op_end (sheet);
    }
}

void
o42_sheet_insert_cols (O42Sheet *sheet, int at, int count)
{
  g_return_if_fail (sheet != NULL);
  if (count > 0)
    {
      op_begin (sheet);
      obj_capture_all (sheet);
      sheet_shift_band (sheet, FALSE, at, count);
      op_end (sheet);
    }
}

void
o42_sheet_delete_cols (O42Sheet *sheet, int at, int count)
{
  g_return_if_fail (sheet != NULL);
  if (count > 0)
    {
      op_begin (sheet);
      obj_capture_all (sheet);
      sheet_shift_band (sheet, FALSE, at, -count);
      op_end (sheet);
    }
}

/* ---------------------------------------------------------------------- */
/* Sort, find, replace                                                     */
/* ---------------------------------------------------------------------- */

#define SORT_MAX_KEYS 3

typedef struct {
  int       source_row;
  O42Value  key[SORT_MAX_KEYS];
} SortRow;

typedef struct {
  int      n_keys;
  gboolean ascending[SORT_MAX_KEYS];
} SortOrder;

/* Blanks go last whichever way the sort runs, as they do in Excel; among
 * the rest, numbers sort before text before booleans, which is the order
 * the comparison operators already use. */
static int
sort_compare (gconstpointer a, gconstpointer b, gpointer user)
{
  const SortRow *x = a, *y = b;
  const SortOrder *order = user;

  for (int k = 0; k < order->n_keys; k++)
    {
      gboolean xe = (x->key[k].type == O42_VALUE_EMPTY);
      gboolean ye = (y->key[k].type == O42_VALUE_EMPTY);
      int cmp;

      if (xe && ye) continue;
      if (xe) return 1;
      if (ye) return -1;

      cmp = o42_value_compare (&x->key[k], &y->key[k]);
      if (!order->ascending[k])
        cmp = -cmp;
      if (cmp != 0)
        return cmp;
    }

  /* Equal keys keep their order: a stable sort. */
  return x->source_row - y->source_row;
}

void
o42_sheet_sort (O42Sheet *sheet, const O42Range *range, int key,
                gboolean ascending, gboolean has_header)
{
  o42_sheet_sort_keys (sheet, range, &key, &ascending, 1, has_header);
}

void
o42_sheet_sort_keys (O42Sheet *sheet, const O42Range *range,
                     const int *keys, const gboolean *ascending,
                     int n_keys, gboolean has_header)
{
  int first, rows, cols;
  GArray *order;
  SortOrder so;
  Carried *carried;

  g_return_if_fail (sheet != NULL);
  g_return_if_fail (range != NULL);
  g_return_if_fail (keys != NULL && ascending != NULL);

  first = range->row0 + (has_header ? 1 : 0);
  rows = range->row1 - first + 1;
  cols = range->col1 - range->col0 + 1;
  n_keys = CLAMP (n_keys, 0, SORT_MAX_KEYS);
  if (rows < 2 || n_keys == 0)
    return;
  for (int k = 0; k < n_keys; k++)
    if (keys[k] < range->col0 || keys[k] > range->col1)
      return;

  so.n_keys = n_keys;
  for (int k = 0; k < n_keys; k++)
    so.ascending[k] = ascending[k];

  order = g_array_sized_new (FALSE, FALSE, sizeof (SortRow), (guint) rows);
  for (int r = 0; r < rows; r++)
    {
      SortRow sr;

      memset (&sr, 0, sizeof sr);
      sr.source_row = first + r;
      for (int k = 0; k < n_keys; k++)
        o42_sheet_get_value (sheet, first + r, keys[k], &sr.key[k]);
      g_array_append_val (order, sr);
    }
  g_array_sort_with_data (order, sort_compare, &so);

  /* Read every row before writing any, relocating each formula by how far
   * its row moves, then put the rows back in the new order. */
  carried = g_new0 (Carried, (gsize) rows * cols);
  for (int r = 0; r < rows; r++)
    {
      const SortRow *sr = &g_array_index (order, SortRow, r);
      int drow = (first + r) - sr->source_row;

      for (int c = 0; c < cols; c++)
        {
          Carried *k = &carried[r * cols + c];
          k->input = o42_sheet_get_input_relocated (sheet, sr->source_row,
                                                    range->col0 + c, drow, 0);
          k->fmt = o42_sheet_get_fmt_idx (sheet, sr->source_row, range->col0 + c);
        }
    }

  op_begin (sheet);
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      sheet_put_carried (sheet, first + r, range->col0 + c, &carried[r * cols + c]);
  op_end (sheet);

  for (int i = 0; i < rows * cols; i++)
    g_free (carried[i].input);
  g_free (carried);
  for (guint i = 0; i < order->len; i++)
    for (int k = 0; k < SORT_MAX_KEYS; k++)
      o42_value_clear (&g_array_index (order, SortRow, i).key[k]);
  g_array_free (order, TRUE);
}

static gboolean
text_matches (const char *haystack, const char *needle,
              gboolean match_case, gboolean whole)
{
  gboolean hit;

  if (match_case)
    hit = whole ? strcmp (haystack, needle) == 0
                : strstr (haystack, needle) != NULL;
  else
    {
      char *h = g_utf8_casefold (haystack, -1);
      char *n = g_utf8_casefold (needle, -1);

      hit = whole ? strcmp (h, n) == 0 : strstr (h, n) != NULL;
      g_free (h);
      g_free (n);
    }

  return hit;
}

gboolean
o42_sheet_find (O42Sheet *sheet, const char *needle,
                gboolean match_case, gboolean whole_cell,
                int *row, int *col)
{
  GArray *keys;
  GHashTableIter iter;
  gpointer key_ptr;
  guint64 from;
  guint start = 0;

  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (needle != NULL && *needle != '\0', FALSE);
  g_return_val_if_fail (row != NULL && col != NULL, FALSE);

  /* The stored cells in reading order; searching four million addresses
   * would be silly when a hundred of them hold anything. */
  keys = g_array_new (FALSE, FALSE, sizeof (guint64));
  g_hash_table_iter_init (&iter, sheet->cells);
  while (g_hash_table_iter_next (&iter, &key_ptr, NULL))
    g_array_append_val (keys, *(guint64 *) key_ptr);
  g_array_sort (keys, (GCompareFunc) key_compare_reading_order);

  from = o42_key (*row, *col);
  while (start < keys->len && g_array_index (keys, guint64, start) <= from)
    start++;

  for (guint n = 0; n < keys->len; n++)
    {
      guint64 k = g_array_index (keys, guint64, (start + n) % keys->len);
      int r = o42_key_row (k), c = o42_key_col (k);
      char *input, *shown;
      gboolean hit;

      if (o42_sheet_is_empty (sheet, r, c))
        continue;

      input = o42_sheet_get_input (sheet, r, c);
      shown = o42_sheet_get_display (sheet, r, c);
      hit = text_matches (input, needle, match_case, whole_cell) ||
            text_matches (shown, needle, match_case, whole_cell);
      g_free (input);
      g_free (shown);

      if (hit)
        {
          *row = r;
          *col = c;
          g_array_free (keys, TRUE);
          return TRUE;
        }
    }

  g_array_free (keys, TRUE);
  return FALSE;
}

/* Replaces `needle` in `text` however many times it occurs, ignoring case
 * if asked, keeping the case of what is around it. */
static char *
replace_in (const char *text, const char *needle, const char *with,
            gboolean match_case)
{
  GString *out = g_string_new (NULL);
  char *ftext = match_case ? g_strdup (text) : g_utf8_casefold (text, -1);
  char *fneedle = match_case ? g_strdup (needle) : g_utf8_casefold (needle, -1);
  gsize nlen = strlen (fneedle);
  const char *p = text, *fp = ftext;
  gboolean any = FALSE;

  /* Casefolding can change byte lengths, so the folded and original
   * strings are walked in step character by character. */
  while (*p != '\0')
    {
      if (strncmp (fp, fneedle, nlen) == 0)
        {
          const char *end = fp + nlen;
          glong chars = g_utf8_strlen (fp, (gssize) nlen);
          const char *pend = g_utf8_offset_to_pointer (p, chars);

          g_string_append (out, with);
          p = pend;
          fp = end;
          any = TRUE;
          continue;
        }

      {
        const char *next = g_utf8_next_char (p);
        g_string_append_len (out, p, next - p);
        p = next;
        fp = g_utf8_next_char (fp);
      }
    }

  g_free (ftext);
  g_free (fneedle);

  if (!any)
    {
      g_string_free (out, TRUE);
      return NULL;
    }
  return g_string_free (out, FALSE);
}

int
o42_sheet_replace (O42Sheet *sheet, const O42Range *range,
                   const char *needle, const char *replacement,
                   gboolean match_case)
{
  GArray *keys;
  GHashTableIter iter;
  gpointer key_ptr;
  int count = 0;

  g_return_val_if_fail (sheet != NULL, 0);
  g_return_val_if_fail (needle != NULL && *needle != '\0', 0);

  keys = g_array_new (FALSE, FALSE, sizeof (guint64));
  g_hash_table_iter_init (&iter, sheet->cells);
  while (g_hash_table_iter_next (&iter, &key_ptr, NULL))
    g_array_append_val (keys, *(guint64 *) key_ptr);

  op_begin (sheet);

  for (guint i = 0; i < keys->len; i++)
    {
      guint64 k = g_array_index (keys, guint64, i);
      int r = o42_key_row (k), c = o42_key_col (k);
      char *input, *changed;

      if (range != NULL && !o42_range_contains (range, r, c))
        continue;
      if (o42_sheet_is_empty (sheet, r, c))
        continue;

      input = o42_sheet_get_input (sheet, r, c);
      changed = replace_in (input, needle, replacement != NULL ? replacement : "",
                            match_case);
      if (changed != NULL)
        {
          op_capture (sheet, r, c);
          set_input_internal (sheet, r, c, changed);
          count++;
        }
      g_free (changed);
      g_free (input);
    }

  op_end (sheet);
  g_array_free (keys, TRUE);
  return count;
}

/* ---------------------------------------------------------------------- */
/* Formatting                                                              */
/* ---------------------------------------------------------------------- */

O42FmtIdx
o42_sheet_get_fmt_idx (O42Sheet *sheet, int row, int col)
{
  O42Cell *cell;

  g_return_val_if_fail (sheet != NULL, 0);

  cell = sheet_find (sheet, row, col);
  return (cell != NULL) ? cell->fmt : o42_fmt_table_default (sheet->formats);
}

const O42Fmt *
o42_sheet_get_fmt (O42Sheet *sheet, int row, int col)
{
  g_return_val_if_fail (sheet != NULL, NULL);

  return o42_fmt_table_get (sheet->formats,
                            o42_sheet_get_fmt_idx (sheet, row, col));
}

void
o42_sheet_apply_fmt (O42Sheet   *sheet,
                     const O42Range *range,
                     O42FmtMask  mask,
                     const O42Fmt *value)
{
  g_return_if_fail (sheet != NULL);
  g_return_if_fail (range != NULL);

  if (o42_book_recording (sheet->book))
    record_format (sheet, range, mask, value);
  g_return_if_fail (value != NULL);

  op_begin (sheet);

  for (int row = range->row0; row <= range->row1; row++)
    for (int col = range->col0; col <= range->col1; col++)
      {
        O42Cell *cell;
        O42Fmt fmt;
        O42FmtIdx idx;

        if (row < 0 || col < 0 || row >= O42_MAX_ROWS || col >= O42_MAX_COLS)
          continue;

        fmt = *o42_fmt_table_get (sheet->formats,
                                  o42_sheet_get_fmt_idx (sheet, row, col));
        o42_fmt_apply_mask (&fmt, mask, value);
        idx = o42_fmt_table_intern (sheet->formats, &fmt);

        if (idx == o42_sheet_get_fmt_idx (sheet, row, col))
          continue;

        op_capture (sheet, row, col);
        cell = sheet_ensure (sheet, row, col);
        cell->fmt = idx;
      }

  op_end (sheet);
}

/* ---------------------------------------------------------------------- */
/* Geometry                                                                */
/* ---------------------------------------------------------------------- */

int
o42_sheet_col_width (O42Sheet *sheet, int col)
{
  gpointer found;

  g_return_val_if_fail (sheet != NULL, DEFAULT_COL_WIDTH);

  if (g_hash_table_contains (sheet->hidden_cols, GINT_TO_POINTER (col)))
    return 0;

  found = g_hash_table_lookup (sheet->col_widths, GINT_TO_POINTER (col));
  return (found != NULL) ? GPOINTER_TO_INT (found) : DEFAULT_COL_WIDTH;
}

void
o42_sheet_set_col_width (O42Sheet *sheet, int col, int width)
{
  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
  obj_capture (sheet, OBJ_COL_WIDTH, col, 0);
  op_end (sheet);

  g_return_if_fail (sheet != NULL);

  width = CLAMP (width, 8, 2000);
  sizes_changed (sheet);
  g_hash_table_insert (sheet->col_widths, GINT_TO_POINTER (col),
                       GINT_TO_POINTER (width));
  sheet->modified = TRUE;
}

/* ---------------------------------------------------------------------- */
/* Where a row or a column is                                              */
/* ---------------------------------------------------------------------- */

/* A sheet is a million rows deep and nearly every one of them is the
 * default height, so the offsets are worked out from the exceptions
 * alone: the rows with a height of their own, the hidden ones, and the
 * ones an AutoFilter has taken out.  Those are gathered once into a
 * sorted array with a running total beside each, and after that "where
 * does row 900,000 start" is a binary search over that array rather
 * than a walk down 900,000 rows.
 *
 * The array is rebuilt whenever anything that could change a size does,
 * which is what `sizes_stamp` counts. */

typedef struct {
  int    index;
  double size;    /* this one's own height or width */
  double before;  /* how much the ones before it add or take away */
} SizeStop;

static void
sizes_changed (O42Sheet *sheet)
{
  sheet->sizes_stamp++;
}

static int
compare_stops (gconstpointer a, gconstpointer b)
{
  return ((const SizeStop *) a)->index - ((const SizeStop *) b)->index;
}

/* The stops of one axis, rebuilt if anything has moved since last time. */
static GArray *
size_stops (O42Sheet *sheet, gboolean rows)
{
  GArray **cache = rows ? &sheet->row_stops : &sheet->col_stops;
  guint *stamp = rows ? &sheet->row_stops_stamp : &sheet->col_stops_stamp;
  double normal = rows ? DEFAULT_ROW_HEIGHT : DEFAULT_COL_WIDTH;
  GHashTable *sized = rows ? sheet->row_heights : sheet->col_widths;
  GHashTable *hidden = rows ? sheet->hidden_rows : sheet->hidden_cols;
  GHashTable *filtered = rows ? sheet->filtered_rows : NULL;
  GHashTable *seen;
  GHashTableIter iter;
  gpointer key, value;
  double running = 0;

  if (*cache != NULL && *stamp == sheet->sizes_stamp)
    return *cache;

  if (*cache == NULL)
    *cache = g_array_new (FALSE, FALSE, sizeof (SizeStop));
  g_array_set_size (*cache, 0);
  seen = g_hash_table_new (g_direct_hash, g_direct_equal);

  g_hash_table_iter_init (&iter, sized);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_add (seen, key);
  g_hash_table_iter_init (&iter, hidden);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_add (seen, key);
  if (filtered != NULL)
    {
      g_hash_table_iter_init (&iter, filtered);
      while (g_hash_table_iter_next (&iter, &key, &value))
        g_hash_table_add (seen, key);
    }

  g_hash_table_iter_init (&iter, seen);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      SizeStop stop;

      stop.index = GPOINTER_TO_INT (key);
      stop.size = rows ? o42_sheet_row_height (sheet, stop.index)
                       : o42_sheet_col_width (sheet, stop.index);
      stop.before = 0;
      g_array_append_val (*cache, stop);
    }
  g_hash_table_destroy (seen);
  g_array_sort (*cache, compare_stops);

  for (guint i = 0; i < (*cache)->len; i++)
    {
      SizeStop *stop = &g_array_index (*cache, SizeStop, i);

      stop->before = running;
      running += stop->size - normal;
    }
  *stamp = sheet->sizes_stamp;
  return *cache;
}

double
o42_sheet_row_offset (O42Sheet *sheet, int row)
{
  GArray *stops;
  double normal = DEFAULT_ROW_HEIGHT;
  guint low, high;

  g_return_val_if_fail (sheet != NULL, 0);
  row = CLAMP (row, 0, O42_MAX_ROWS);
  stops = size_stops (sheet, TRUE);

  /* The offset is the plain multiple, plus what the stops before it
   * differ by. */
  low = 0;
  high = stops->len;
  while (low < high)
    {
      guint mid = (low + high) / 2;

      if (g_array_index (stops, SizeStop, mid).index < row)
        low = mid + 1;
      else
        high = mid;
    }
  if (low == 0)
    return row * normal;
  {
    const SizeStop *stop = &g_array_index (stops, SizeStop, low - 1);

    return row * normal + stop->before + (stop->size - normal);
  }
}

double
o42_sheet_col_offset (O42Sheet *sheet, int col)
{
  GArray *stops;
  double normal = DEFAULT_COL_WIDTH;
  guint low, high;

  g_return_val_if_fail (sheet != NULL, 0);
  col = CLAMP (col, 0, O42_MAX_COLS);
  stops = size_stops (sheet, FALSE);

  low = 0;
  high = stops->len;
  while (low < high)
    {
      guint mid = (low + high) / 2;

      if (g_array_index (stops, SizeStop, mid).index < col)
        low = mid + 1;
      else
        high = mid;
    }
  if (low == 0)
    return col * normal;
  {
    const SizeStop *stop = &g_array_index (stops, SizeStop, low - 1);

    return col * normal + stop->before + (stop->size - normal);
  }
}

/* The row at an offset: the last one whose top is at or before it.  A
 * hidden row has no height, so several can share a top; the one after
 * them is the one that answers. */
int
o42_sheet_row_at (O42Sheet *sheet, double offset)
{
  int low = 0, high = O42_MAX_ROWS - 1;

  g_return_val_if_fail (sheet != NULL, 0);
  if (offset <= 0)
    return 0;
  while (low < high)
    {
      int mid = low + (high - low + 1) / 2;

      if (o42_sheet_row_offset (sheet, mid) <= offset)
        low = mid;
      else
        high = mid - 1;
    }
  while (low + 1 < O42_MAX_ROWS && o42_sheet_row_height (sheet, low) == 0)
    low++;
  return low;
}

int
o42_sheet_col_at (O42Sheet *sheet, double offset)
{
  int low = 0, high = O42_MAX_COLS - 1;

  g_return_val_if_fail (sheet != NULL, 0);
  if (offset <= 0)
    return 0;
  while (low < high)
    {
      int mid = low + (high - low + 1) / 2;

      if (o42_sheet_col_offset (sheet, mid) <= offset)
        low = mid;
      else
        high = mid - 1;
    }
  while (low + 1 < O42_MAX_COLS && o42_sheet_col_width (sheet, low) == 0)
    low++;
  return low;
}

int
o42_sheet_row_height (O42Sheet *sheet, int row)
{
  gpointer found;

  g_return_val_if_fail (sheet != NULL, DEFAULT_ROW_HEIGHT);

  if (g_hash_table_contains (sheet->hidden_rows, GINT_TO_POINTER (row)) ||
      g_hash_table_contains (sheet->filtered_rows, GINT_TO_POINTER (row)))
    return 0;

  found = g_hash_table_lookup (sheet->row_heights, GINT_TO_POINTER (row));
  return (found != NULL) ? GPOINTER_TO_INT (found) : DEFAULT_ROW_HEIGHT;
}

gboolean
o42_sheet_row_height_set (O42Sheet *sheet, int row)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  return g_hash_table_contains (sheet->row_heights, GINT_TO_POINTER (row));
}

void
o42_sheet_set_row_height (O42Sheet *sheet, int row, int height)
{
  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
  obj_capture (sheet, OBJ_ROW_HEIGHT, row, 0);
  op_end (sheet);

  g_return_if_fail (sheet != NULL);

  height = CLAMP (height, 6, 500);
  sizes_changed (sheet);
  g_hash_table_insert (sheet->row_heights, GINT_TO_POINTER (row),
                       GINT_TO_POINTER (height));
  sheet->modified = TRUE;
}

void
o42_sheet_set_row_hidden (O42Sheet *sheet, int row, gboolean hidden)
{
  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
  obj_capture (sheet, OBJ_ROW_HIDDEN, row, 0);
  op_end (sheet);

  g_return_if_fail (sheet != NULL);
  sizes_changed (sheet);
  if (hidden) g_hash_table_add (sheet->hidden_rows, GINT_TO_POINTER (row));
  else        g_hash_table_remove (sheet->hidden_rows, GINT_TO_POINTER (row));
  sheet->modified = TRUE;
}

gboolean
o42_sheet_row_hidden (O42Sheet *sheet, int row)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  return g_hash_table_contains (sheet->hidden_rows, GINT_TO_POINTER (row)) ||
         g_hash_table_contains (sheet->filtered_rows, GINT_TO_POINTER (row));
}

gboolean
o42_sheet_row_hidden_by_hand (O42Sheet *sheet, int row)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  return g_hash_table_contains (sheet->hidden_rows, GINT_TO_POINTER (row));
}

void
o42_sheet_set_col_hidden (O42Sheet *sheet, int col, gboolean hidden)
{
  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
  obj_capture (sheet, OBJ_COL_HIDDEN, col, 0);
  op_end (sheet);

  g_return_if_fail (sheet != NULL);
  sizes_changed (sheet);
  if (hidden) g_hash_table_add (sheet->hidden_cols, GINT_TO_POINTER (col));
  else        g_hash_table_remove (sheet->hidden_cols, GINT_TO_POINTER (col));
  sheet->modified = TRUE;
}

gboolean
o42_sheet_col_hidden (O42Sheet *sheet, int col)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  return g_hash_table_contains (sheet->hidden_cols, GINT_TO_POINTER (col));
}

/* ---- Conditional formatting -------------------------------------------- */

void
o42_sheet_add_condition (O42Sheet *sheet, const O42Condition *cond)
{
  g_return_if_fail (sheet != NULL);
  g_return_if_fail (cond != NULL);
  g_array_append_val (sheet->conditions, *cond);
  sheet->modified = TRUE;
}

void
o42_sheet_clear_conditions (O42Sheet *sheet, const O42Range *range)
{
  g_return_if_fail (sheet != NULL);

  for (guint i = 0; i < sheet->conditions->len; )
    {
      const O42Condition *c = &g_array_index (sheet->conditions, O42Condition, i);

      if (range == NULL || ranges_overlap (&c->range, range))
        {
          g_array_remove_index (sheet->conditions, i);
          sheet->modified = TRUE;
        }
      else
        i++;
    }
}

GArray *
o42_sheet_conditions (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->conditions;
}

static gboolean
condition_holds (const O42Condition *c, double x)
{
  double lo = MIN (c->value, c->value2), hi = MAX (c->value, c->value2);

  switch (c->op)
    {
    case O42_COND_BETWEEN:       return x >= lo && x <= hi;
    case O42_COND_NOT_BETWEEN:   return x < lo || x > hi;
    case O42_COND_EQUAL:         return x == c->value;
    case O42_COND_NOT_EQUAL:     return x != c->value;
    case O42_COND_GREATER:       return x > c->value;
    case O42_COND_LESS:          return x < c->value;
    case O42_COND_GREATER_EQUAL: return x >= c->value;
    case O42_COND_LESS_EQUAL:    return x <= c->value;
    default:                     return FALSE;
    }
}

gboolean
o42_sheet_conditional_fmt (O42Sheet *sheet, int row, int col, O42Fmt *out)
{
  gboolean any = FALSE;
  O42Value v;
  double x;

  g_return_val_if_fail (sheet != NULL, FALSE);

  if (sheet->conditions->len == 0)
    return FALSE;

  /* Only numbers are judged; the first rule that holds wins, as in Excel,
   * though a later rule's other fields still apply. */
  o42_sheet_get_value (sheet, row, col, &v);
  if (v.type != O42_VALUE_NUMBER)
    {
      o42_value_clear (&v);
      return FALSE;
    }
  x = v.as.number;
  o42_value_clear (&v);

  for (guint i = 0; i < sheet->conditions->len; i++)
    {
      const O42Condition *c = &g_array_index (sheet->conditions, O42Condition, i);

      if (!o42_range_contains (&c->range, row, col) || !condition_holds (c, x))
        continue;
      if (!any)
        *out = *o42_sheet_get_fmt (sheet, row, col);
      o42_fmt_apply_mask (out, c->mask, &c->fmt);
      any = TRUE;
    }

  return any;
}

/* ---- Text to Columns --------------------------------------------------- */

int
o42_sheet_text_to_columns (O42Sheet *sheet, const O42Range *range,
                           const char *delimiter)
{
  int changed = 0;

  g_return_val_if_fail (sheet != NULL, 0);
  g_return_val_if_fail (range != NULL, 0);
  if (delimiter == NULL || *delimiter == '\0')
    return 0;

  op_begin (sheet);

  for (int row = range->row0; row <= range->row1; row++)
    {
      O42Value v;
      char **parts;

      o42_sheet_get_value (sheet, row, range->col0, &v);
      if (v.type != O42_VALUE_TEXT || strstr (v.as.text, delimiter) == NULL)
        {
          o42_value_clear (&v);
          continue;
        }

      parts = g_strsplit (v.as.text, delimiter, -1);
      o42_value_clear (&v);

      for (int i = 0; parts[i] != NULL && range->col0 + i < O42_MAX_COLS; i++)
        {
          char *piece = g_strstrip (parts[i]);

          op_capture (sheet, row, range->col0 + i);
          set_input_internal (sheet, row, range->col0 + i, *piece != '\0' ? piece : NULL);
        }
      g_strfreev (parts);
      changed++;
    }

  op_end (sheet);
  return changed;
}

/* ---- View state -------------------------------------------------------- */

void
o42_sheet_set_frozen (O42Sheet *sheet, int rows, int cols)
{
  g_return_if_fail (sheet != NULL);
  sheet->frozen_rows = CLAMP (rows, 0, O42_MAX_ROWS - 1);
  sheet->frozen_cols = CLAMP (cols, 0, O42_MAX_COLS - 1);
  sheet->modified = TRUE;
}

void
o42_sheet_get_frozen (O42Sheet *sheet, int *rows, int *cols)
{
  g_return_if_fail (sheet != NULL);
  if (rows) *rows = sheet->frozen_rows;
  if (cols) *cols = sheet->frozen_cols;
}

/* ---- Goal Seek --------------------------------------------------------- */

/* The target's value with `x` in the variable cell, or NAN. */
static double
goal_probe (O42Sheet *sheet, int trow, int tcol, int vrow, int vcol, double x)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  O42Value v;
  double y = NAN;

  set_input_internal (sheet, vrow, vcol, g_ascii_dtostr (buf, sizeof buf, x));
  o42_sheet_get_value (sheet, trow, tcol, &v);
  if (v.type == O42_VALUE_NUMBER)
    y = v.as.number;
  o42_value_clear (&v);
  return y;
}

gboolean
o42_sheet_goal_seek (O42Sheet *sheet, int trow, int tcol, double goal,
                     int vrow, int vcol, double *found)
{
  char *original;
  double x0, x1, y0, y1, x = 0, y = NAN;
  gboolean ok = FALSE;
  O42Value start;

  g_return_val_if_fail (sheet != NULL, FALSE);

  if (!o42_sheet_has_formula (sheet, trow, tcol) ||
      o42_sheet_has_formula (sheet, vrow, vcol))
    return FALSE;

  original = o42_sheet_get_input (sheet, vrow, vcol);
  o42_sheet_get_value (sheet, vrow, vcol, &start);
  x0 = (start.type == O42_VALUE_NUMBER) ? start.as.number : 0;
  o42_value_clear (&start);

  op_begin (sheet);
  op_capture (sheet, vrow, vcol);

  /* Secant steps from the present value and a nudge beside it. */
  y0 = goal_probe (sheet, trow, tcol, vrow, vcol, x0) - goal;
  x1 = (x0 == 0) ? 1 : x0 * 1.01;
  y1 = goal_probe (sheet, trow, tcol, vrow, vcol, x1) - goal;

  for (int i = 0; i < 100 && !isnan (y0) && !isnan (y1); i++)
    {
      if (fabs (y1) < 1e-9 * MAX (1.0, fabs (goal)))
        { ok = TRUE; x = x1; y = y1; break; }
      if (y1 == y0)
        break;
      x = x1 - y1 * (x1 - x0) / (y1 - y0);
      if (isnan (x) || isinf (x))
        break;
      x0 = x1; y0 = y1;
      x1 = x;
      y1 = goal_probe (sheet, trow, tcol, vrow, vcol, x1) - goal;
    }

  /* If the secant wandered, bracket and bisect over a widening interval. */
  if (!ok)
    {
      double lo = -1, hi = 1, ylo, yhi;

      for (int i = 0; i < 60 && !ok; i++)
        {
          ylo = goal_probe (sheet, trow, tcol, vrow, vcol, lo) - goal;
          yhi = goal_probe (sheet, trow, tcol, vrow, vcol, hi) - goal;
          if (!isnan (ylo) && !isnan (yhi) && (ylo <= 0) != (yhi <= 0))
            {
              for (int j = 0; j < 200; j++)
                {
                  double mid = (lo + hi) / 2, ymid;
                  ymid = goal_probe (sheet, trow, tcol, vrow, vcol, mid) - goal;
                  if (isnan (ymid)) break;
                  if ((ymid <= 0) == (ylo <= 0)) { lo = mid; ylo = ymid; }
                  else                           { hi = mid; yhi = ymid; }
                  if (fabs (ymid) < 1e-9 * MAX (1.0, fabs (goal)) || hi - lo < 1e-14 * MAX (1.0, fabs (mid)))
                    { ok = TRUE; x = mid; y = ymid; break; }
                }
              break;
            }
          lo *= 10;
          hi *= 10;
        }
    }

  if (ok)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      set_input_internal (sheet, vrow, vcol, g_ascii_dtostr (buf, sizeof buf, x));
      if (found) *found = y + goal;
    }
  else
    set_input_internal (sheet, vrow, vcol, original);

  op_end (sheet);
  g_free (original);
  return ok;
}

/* ---- Notes ------------------------------------------------------------- */

void
o42_sheet_set_note (O42Sheet *sheet, int row, int col, const char *text)
{
  guint64 key = o42_key (row, col);

  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
  obj_capture (sheet, OBJ_NOTE, 0, key);
  op_end (sheet);

  if (text == NULL || *text == '\0')
    g_hash_table_remove (sheet->notes, &key);
  else
    {
      guint64 *stored = g_new (guint64, 1);
      *stored = key;
      g_hash_table_insert (sheet->notes, stored, g_strdup (text));
    }
  sheet->modified = TRUE;
}

const char *
o42_sheet_get_note (O42Sheet *sheet, int row, int col)
{
  guint64 key = o42_key (row, col);

  g_return_val_if_fail (sheet != NULL, NULL);
  return g_hash_table_lookup (sheet->notes, &key);
}

GHashTable *
o42_sheet_notes (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->notes;
}

/* ---- Hyperlinks -------------------------------------------------------- */

void
o42_sheet_set_link (O42Sheet *sheet, int row, int col, const char *target)
{
  guint64 key = o42_key (row, col);

  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
  obj_capture (sheet, OBJ_LINK, 0, key);
  op_end (sheet);

  if (target == NULL || *target == '\0')
    g_hash_table_remove (sheet->links, &key);
  else
    {
      guint64 *stored = g_new (guint64, 1);
      *stored = key;
      g_hash_table_insert (sheet->links, stored, g_strdup (target));
    }
  sheet->modified = TRUE;
}

const char *
o42_sheet_get_link (O42Sheet *sheet, int row, int col)
{
  guint64 key = o42_key (row, col);

  g_return_val_if_fail (sheet != NULL, NULL);
  return g_hash_table_lookup (sheet->links, &key);
}

GHashTable *
o42_sheet_links (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->links;
}

/* ---- Merged cells ------------------------------------------------------ */

static gboolean
ranges_overlap (const O42Range *a, const O42Range *b)
{
  return a->row0 <= b->row1 && b->row0 <= a->row1 &&
         a->col0 <= b->col1 && b->col0 <= a->col1;
}

void
o42_sheet_merge (O42Sheet *sheet, const O42Range *range)
{
  O42Range r;

  g_return_if_fail (sheet != NULL);
  g_return_if_fail (range != NULL);

  r = o42_range_normalise (range->row0, range->col0, range->row1, range->col1);
  if (r.row0 == r.row1 && r.col0 == r.col1)
    return;

  /* A merge swallows any merge it overlaps, and every cell but the
   * top-left is emptied -- one undo step for the emptying and the
   * merge together. */
  op_begin (sheet);
  obj_capture (sheet, OBJ_MERGES, 0, 0);
  for (guint i = 0; i < sheet->merges->len; )
    {
      if (ranges_overlap (&g_array_index (sheet->merges, O42Range, i), &r))
        g_array_remove_index (sheet->merges, i);
      else
        i++;
    }

  for (int row = r.row0; row <= r.row1; row++)
    for (int col = r.col0; col <= r.col1; col++)
      if ((row != r.row0 || col != r.col0) && !o42_sheet_is_empty (sheet, row, col))
        {
          op_capture (sheet, row, col);
          set_input_internal (sheet, row, col, NULL);
        }
  g_array_append_val (sheet->merges, r);
  op_end (sheet);
  sheet->modified = TRUE;
}

void
o42_sheet_unmerge (O42Sheet *sheet, const O42Range *range)
{
  g_return_if_fail (sheet != NULL);
  g_return_if_fail (range != NULL);
  op_begin (sheet);
  obj_capture (sheet, OBJ_MERGES, 0, 0);
  op_end (sheet);

  for (guint i = 0; i < sheet->merges->len; )
    {
      if (ranges_overlap (&g_array_index (sheet->merges, O42Range, i), range))
        {
          g_array_remove_index (sheet->merges, i);
          sheet->modified = TRUE;
        }
      else
        i++;
    }
}

gboolean
o42_sheet_merged_at (O42Sheet *sheet, int row, int col, O42Range *out)
{
  g_return_val_if_fail (sheet != NULL, FALSE);

  for (guint i = 0; i < sheet->merges->len; i++)
    {
      const O42Range *m = &g_array_index (sheet->merges, O42Range, i);

      if (o42_range_contains (m, row, col))
        {
          if (out) *out = *m;
          return TRUE;
        }
    }

  return FALSE;
}

GArray *
o42_sheet_merges (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->merges;
}

/* ---- AutoFilter ------------------------------------------------------- */

/* Hides the rows the choices rule out and shows the rest. */
static void
autofilter_apply (O42Sheet *sheet)
{
  sizes_changed (sheet);
  g_hash_table_remove_all (sheet->filtered_rows);

  if (!sheet->has_filter)
    return;

  for (int row = sheet->filter.row0 + 1; row <= sheet->filter.row1; row++)
    {
      gboolean keep = TRUE;
      GHashTableIter iter;
      gpointer key, value;

      g_hash_table_iter_init (&iter, sheet->filter_choice);
      while (keep && g_hash_table_iter_next (&iter, &key, &value))
        {
          int col = GPOINTER_TO_INT (key);

          if (o42_criterion_is_condition (value))
            {
              /* A custom filter: ">5", "<>Japan", "*land" as COUNTIF
               * reads them, against the cell's value. */
              O42Value v;
              o42_sheet_get_value (sheet, row, col, &v);
              keep = o42_criterion_matches (value, &v);
              o42_value_clear (&v);
            }
          else
            {
              char *shown = o42_sheet_get_display (sheet, row, col);
              keep = (g_strcmp0 (shown, value) == 0);
              g_free (shown);
            }
        }

      if (!keep)
        { sizes_changed (sheet); g_hash_table_add (sheet->filtered_rows, GINT_TO_POINTER (row)); }
    }
}

void
o42_sheet_set_autofilter (O42Sheet *sheet, const O42Range *range)
{
  g_return_if_fail (sheet != NULL);
  g_return_if_fail (range != NULL);

  sheet->has_filter = TRUE;
  sheet->filter = *range;
  g_hash_table_remove_all (sheet->filter_choice);
  autofilter_apply (sheet);
  sheet->modified = TRUE;
}

gboolean
o42_sheet_get_autofilter (O42Sheet *sheet, O42Range *range)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  if (range != NULL && sheet->has_filter)
    *range = sheet->filter;
  return sheet->has_filter;
}

void
o42_sheet_clear_autofilter (O42Sheet *sheet)
{
  g_return_if_fail (sheet != NULL);
  sheet->has_filter = FALSE;
  g_hash_table_remove_all (sheet->filter_choice);
  autofilter_apply (sheet);
  sheet->modified = TRUE;
}

void
o42_sheet_autofilter_choose (O42Sheet *sheet, int col, const char *value)
{
  g_return_if_fail (sheet != NULL);

  if (value == NULL)
    g_hash_table_remove (sheet->filter_choice, GINT_TO_POINTER (col));
  else
    g_hash_table_insert (sheet->filter_choice, GINT_TO_POINTER (col), g_strdup (value));
  autofilter_apply (sheet);
  sheet->modified = TRUE;
}

void
o42_sheet_autofilter_refresh (O42Sheet *sheet)
{
  g_return_if_fail (sheet != NULL);
  autofilter_apply (sheet);
}

const char *
o42_sheet_autofilter_choice (O42Sheet *sheet, int col)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return g_hash_table_lookup (sheet->filter_choice, GINT_TO_POINTER (col));
}

static int
compare_strings (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate (*(const char *const *) a, *(const char *const *) b);
}

char **
o42_sheet_autofilter_values (O42Sheet *sheet, int col)
{
  GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);
  GPtrArray *values = g_ptr_array_new ();

  g_return_val_if_fail (sheet != NULL, NULL);

  if (sheet->has_filter)
    for (int row = sheet->filter.row0 + 1; row <= sheet->filter.row1; row++)
      {
        char *shown = o42_sheet_get_display (sheet, row, col);

        if (*shown != '\0' && !g_hash_table_contains (seen, shown))
          {
            g_hash_table_add (seen, shown);
            g_ptr_array_add (values, shown);
          }
        else
          g_free (shown);
      }

  g_ptr_array_sort (values, compare_strings);
  g_ptr_array_add (values, NULL);
  g_hash_table_destroy (seen);
  return (char **) g_ptr_array_free (values, FALSE);
}

void
o42_sheet_used_range (O42Sheet *sheet, O42Range *out)
{
  GHashTableIter iter;
  gpointer key_ptr;
  gboolean any = FALSE;

  g_return_if_fail (sheet != NULL);
  g_return_if_fail (out != NULL);

  /* Asked for on every repaint and by every formula over a whole
   * column, so the answer is kept and grown as cells come, and only
   * worked out again when a cell on its edge has gone. */
  if (sheet->used_valid)
    {
      *out = sheet->used;
      return;
    }

  out->row0 = out->col0 = 0;
  out->row1 = out->col1 = 0;

  g_hash_table_iter_init (&iter, sheet->cells);
  while (g_hash_table_iter_next (&iter, &key_ptr, NULL))
    {
      guint64 key = *(guint64 *) key_ptr;
      int row = o42_key_row (key);
      int col = o42_key_col (key);

      if (!any)
        {
          out->row0 = out->row1 = row;
          out->col0 = out->col1 = col;
          any = TRUE;
          continue;
        }

      out->row0 = MIN (out->row0, row);
      out->row1 = MAX (out->row1, row);
      out->col0 = MIN (out->col0, col);
      out->col1 = MAX (out->col1, col);
    }

  sheet->used = *out;
  sheet->used_any = any;
  sheet->used_valid = TRUE;
}

void
o42_sheet_foreach_cell (O42Sheet *sheet, O42CellFunc func, gpointer user)
{
  GHashTableIter iter;
  gpointer key_ptr;
  GArray *keys;

  g_return_if_fail (sheet != NULL);
  g_return_if_fail (func != NULL);

  /* The keys are copied out first so that the callback may change the
   * sheet without upsetting the iteration. */
  keys = g_array_new (FALSE, FALSE, sizeof (guint64));
  g_hash_table_iter_init (&iter, sheet->cells);
  while (g_hash_table_iter_next (&iter, &key_ptr, NULL))
    g_array_append_val (keys, *(guint64 *) key_ptr);

  for (guint i = 0; i < keys->len; i++)
    {
      guint64 k = g_array_index (keys, guint64, i);
      func (sheet, o42_key_row (k), o42_key_col (k), user);
    }

  g_array_free (keys, TRUE);
}

gboolean
o42_sheet_is_modified (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  return sheet->modified;
}

void
o42_sheet_set_modified (O42Sheet *sheet, gboolean modified)
{
  g_return_if_fail (sheet != NULL);
  sheet->modified = modified;
}

/* ---------------------------------------------------------------------- */
/* Undo                                                                    */
/* ---------------------------------------------------------------------- */

void
o42_sheet_begin_group (O42Sheet *sheet)
{
  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
}

void
o42_sheet_end_group (O42Sheet *sheet)
{
  g_return_if_fail (sheet != NULL);
  op_end (sheet);
}

gboolean
o42_sheet_can_undo (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  return sheet->stack->pos > 0;
}

gboolean
o42_sheet_can_redo (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  return sheet->stack->pos < sheet->stack->undo->len;
}

/* Restores what a record holds, capturing what was there into a new record
 * as it goes.  The entry that comes back undoes what this one just did, so
 * one routine drives undo and redo alike and a stack entry flips direction
 * each time it is used -- the same shape word42's change records have.
 * Each snapshot knows its sheet, so one record may span several. */
static O42Undo *
undo_apply (O42Undo *record, O42Sheet **target, O42Range *touched)
{
  O42Undo *inverse = undo_new ();
  gboolean any = FALSE;

  /* Newest first, so a cell captured twice in one step ends at its
   * earliest state; the inverse is built in the same order and applied
   * the same way, which brings the step back exactly. */
  for (guint k = record->snapshots->len; k > 0; k--)
    {
      const O42Snapshot *snap =
        &g_array_index (record->snapshots, O42Snapshot, k - 1);
      O42Sheet *sheet = snap->sheet;
      int row = o42_key_row (snap->key);
      int col = o42_key_col (snap->key);
      O42Snapshot before = snapshot_take (sheet, snap->key);
      O42Cell *cell;

      g_array_append_val (inverse->snapshots, before);

      set_input_internal (sheet, row, col, snap->input);
      if (snap->array_rows > 0 && snap->array_cols > 0)
        {
          O42Range block = { row, col, row + snap->array_rows - 1, col + snap->array_cols - 1 };
          array_register (sheet, &block);
        }

      /* Restoring formatting has to come after the content, because setting
       * content can create the cell that the format belongs to. */
      if (snap->fmt != o42_fmt_table_default (sheet->formats) ||
          snap->style != NULL || sheet_find (sheet, row, col) != NULL)
        {
          cell = sheet_ensure (sheet, row, col);
          cell->fmt = snap->fmt;
          cell->style = snap->style;
          sheet_prune (sheet, row, col);
        }

      sheet->modified = TRUE;

      /* The touched rectangle is on the first sheet the record changed. */
      if (!any)
        {
          if (target != NULL) *target = sheet;
          if (touched != NULL)
            {
              touched->row0 = touched->row1 = row;
              touched->col0 = touched->col1 = col;
            }
          any = TRUE;
        }
      else if (touched != NULL && target != NULL && sheet == *target)
        {
          touched->row0 = MIN (touched->row0, row);
          touched->row1 = MAX (touched->row1, row);
          touched->col0 = MIN (touched->col0, col);
          touched->col1 = MAX (touched->col1, col);
        }
    }

  /* Objects go back in reverse, so a step that captured the same thing
   * twice ends at its earliest state. */
  for (guint i = record->objects->len; i > 0; i--)
    {
      ObjSnap *snap = &g_array_index (record->objects, ObjSnap, i - 1);
      ObjSnap before = obj_snap_take (snap->sheet, snap->kind, snap->index, snap->key);
      O42Sheet *shown = snap->sheet;

      obj_snap_apply (snap);
      if (snap->kind == OBJ_SHEET_PRESENT)
        {
          /* A sheet out of its book is kept by the snapshot that would
           * put it back. */
          O42Book *book = snap->sheet->book;
          gboolean attached = book != NULL && o42_book_sheet_index (book, snap->sheet) >= 0;
          before.owns_sheet = !attached;
          snap->owns_sheet = FALSE;
          if (!attached && book != NULL)
            shown = o42_book_sheet (book, 0);
        }
      g_array_prepend_val (inverse->objects, before);
      if (snap->kind == OBJ_SHEET_NAME || snap->kind == OBJ_SHEET_PRESENT)
        {
          if (snap->sheet->book != NULL)
            o42_book_changed (snap->sheet->book, "sheets");
        }
      else
        snap->sheet->modified = TRUE;
      if (!any)
        {
          if (target != NULL) *target = shown;
          if (touched != NULL)
            {
              touched->row0 = 0; touched->col0 = 0;
              touched->row1 = O42_MAX_ROWS - 1; touched->col1 = O42_MAX_COLS - 1;
            }
          any = TRUE;
        }
    }

  return inverse;
}

gboolean
o42_sheet_undo_full (O42Sheet *sheet, O42Sheet **target, O42Range *touched)
{
  O42UndoStack *stack;
  O42Undo *record, *inverse;
  O42Sheet *hit = sheet;

  g_return_val_if_fail (sheet != NULL, FALSE);

  stack = sheet->stack;
  if (stack->pos == 0)
    return FALSE;

  record = g_ptr_array_index (stack->undo, stack->pos - 1);
  inverse = undo_apply (record, &hit, touched);

  g_ptr_array_index (stack->undo, stack->pos - 1) = inverse;
  undo_free (record);
  stack->pos--;

  if (target != NULL)
    *target = hit;
  return TRUE;
}

gboolean
o42_sheet_redo_full (O42Sheet *sheet, O42Sheet **target, O42Range *touched)
{
  O42UndoStack *stack;
  O42Undo *record, *inverse;
  O42Sheet *hit = sheet;

  g_return_val_if_fail (sheet != NULL, FALSE);

  stack = sheet->stack;
  if (stack->pos >= stack->undo->len)
    return FALSE;

  record = g_ptr_array_index (stack->undo, stack->pos);
  inverse = undo_apply (record, &hit, touched);

  g_ptr_array_index (stack->undo, stack->pos) = inverse;
  undo_free (record);
  stack->pos++;

  if (target != NULL)
    *target = hit;
  return TRUE;
}

gboolean
o42_sheet_undo (O42Sheet *sheet, O42Range *touched)
{
  return o42_sheet_undo_full (sheet, NULL, touched);
}

gboolean
o42_sheet_redo (O42Sheet *sheet, O42Range *touched)
{
  return o42_sheet_redo_full (sheet, NULL, touched);
}

void
o42_sheet_clear_undo (O42Sheet *sheet)
{
  g_return_if_fail (sheet != NULL);
  undo_stack_clear (sheet->stack);
}

/* ---------------------------------------------------------------------- */
/* Charts                                                                  */
/* ---------------------------------------------------------------------- */

static void
chart_fetch (gpointer user, int row, int col, O42Value *out)
{
  o42_sheet_get_value (user, row, col, out);
}

/* The sheet a chart's cells are on: another one when it names it,
 * which is how a chart sheet plots a worksheet's numbers. */
static O42Sheet *
chart_source (O42Sheet *sheet, const O42Chart *chart)
{
  O42Sheet *other;

  if (chart == NULL || chart->data_sheet == NULL || chart->data_sheet[0] == '\0' ||
      sheet->book == NULL)
    return sheet;
  other = o42_book_find_sheet (sheet->book, chart->data_sheet);
  return other != NULL ? other : sheet;
}

O42Chart *
o42_sheet_add_chart (O42Sheet *sheet, O42ChartKind kind, const O42Range *data,
                     int row, int col)
{
  O42Chart *chart;

  g_return_val_if_fail (sheet != NULL, NULL);
  g_return_val_if_fail (data != NULL, NULL);

  chart = o42_chart_new (kind, data);
  chart->id = sheet->next_picture_id++;     /* one id space for all objects */
  op_begin (sheet);
  obj_capture (sheet, OBJ_CHART, chart->id, 0);   /* absent: undo removes it */
  op_end (sheet);
  chart->row = CLAMP (row, 0, O42_MAX_ROWS - 1);
  chart->col = CLAMP (col, 0, O42_MAX_COLS - 1);
  o42_chart_guess_labels (chart, chart_fetch, sheet);

  g_ptr_array_add (sheet->charts, chart);
  sheet->modified = TRUE;
  return chart;
}

void
o42_sheet_remove_chart (O42Sheet *sheet, guint id)
{
  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
  obj_capture (sheet, OBJ_CHART, id, 0);
  op_end (sheet);

  for (guint i = 0; i < sheet->charts->len; i++)
    if (((O42Chart *) g_ptr_array_index (sheet->charts, i))->id == id)
      {
        g_ptr_array_remove_index (sheet->charts, i);
        sheet->modified = TRUE;
        return;
      }
}

O42Chart *
o42_sheet_find_chart (O42Sheet *sheet, guint id)
{
  g_return_val_if_fail (sheet != NULL, NULL);

  for (guint i = 0; i < sheet->charts->len; i++)
    if (((O42Chart *) g_ptr_array_index (sheet->charts, i))->id == id)
      return g_ptr_array_index (sheet->charts, i);

  return NULL;
}

GPtrArray *
o42_sheet_charts (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->charts;
}

/* The picture a chart's marker names, decoded, for o42_chart_draw_full. */
static cairo_surface_t *
chart_picture (gpointer user, guint id)
{
  O42Sheet *sheet = user;
  O42Picture *picture = o42_sheet_find_picture (sheet, id);

  return picture != NULL ? o42_picture_surface (picture) : NULL;
}

void
o42_sheet_draw_chart (O42Sheet *sheet, const O42Chart *chart, cairo_t *cr,
                      double width, double height)
{
  g_return_if_fail (sheet != NULL);
  o42_chart_draw_full (chart, cr, width, height, chart_fetch,
                       chart_source (sheet, chart), chart_picture, sheet);
}

/* ---------------------------------------------------------------------- */
/* Pictures                                                                */
/* ---------------------------------------------------------------------- */

O42Picture *
o42_sheet_add_picture (O42Sheet   *sheet,
                       GBytes     *data,
                       const char *format,
                       int         pixel_w,
                       int         pixel_h,
                       int         row,
                       int         col)
{
  O42Picture *picture;

  g_return_val_if_fail (sheet != NULL, NULL);
  g_return_val_if_fail (data != NULL, NULL);

  picture = o42_picture_new (data, format, pixel_w, pixel_h);
  if (picture == NULL)
    return NULL;

  picture->id  = sheet->next_picture_id++;
  op_begin (sheet);
  obj_capture (sheet, OBJ_PICTURE, picture->id, 0);
  op_end (sheet);
  picture->row = CLAMP (row, 0, O42_MAX_ROWS - 1);
  picture->col = CLAMP (col, 0, O42_MAX_COLS - 1);

  g_ptr_array_add (sheet->pictures, picture);
  sheet->modified = TRUE;
  return picture;
}

void
o42_sheet_remove_picture (O42Sheet *sheet, guint id)
{
  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
  obj_capture (sheet, OBJ_PICTURE, id, 0);
  op_end (sheet);

  for (guint i = 0; i < sheet->pictures->len; i++)
    {
      O42Picture *picture = g_ptr_array_index (sheet->pictures, i);

      if (picture->id == id)
        {
          g_ptr_array_remove_index (sheet->pictures, i);
          sheet->modified = TRUE;
          return;
        }
    }
}

O42Picture *
o42_sheet_find_picture (O42Sheet *sheet, guint id)
{
  g_return_val_if_fail (sheet != NULL, NULL);

  for (guint i = 0; i < sheet->pictures->len; i++)
    {
      O42Picture *picture = g_ptr_array_index (sheet->pictures, i);

      if (picture->id == id)
        return picture;
    }

  return NULL;
}

GPtrArray *
o42_sheet_pictures (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->pictures;
}

/* ---------------------------------------------------------------------- */
/* Data > Validation                                                       */
/* ---------------------------------------------------------------------- */

void
o42_sheet_add_validation (O42Sheet *sheet, const O42Validation *v)
{
  O42Validation copy;

  g_return_if_fail (sheet != NULL && v != NULL);
  copy = *v;
  copy.value = g_strdup (v->value ? v->value : "");
  copy.value2 = g_strdup (v->value2 ? v->value2 : "");
  copy.message = g_strdup (v->message ? v->message : "");
  g_array_append_val (sheet->validations, copy);
  sheet->modified = TRUE;
}

void
o42_sheet_clear_validations (O42Sheet *sheet, const O42Range *range)
{
  g_return_if_fail (sheet != NULL);

  for (guint i = 0; i < sheet->validations->len; )
    {
      O42Validation *v = &g_array_index (sheet->validations, O42Validation, i);
      if (range == NULL || ranges_overlap (&v->range, range))
        {
          g_free (v->value); g_free (v->value2); g_free (v->message);
          g_array_remove_index (sheet->validations, i);
          sheet->modified = TRUE;
        }
      else
        i++;
    }
}

GArray *
o42_sheet_validations (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->validations;
}

/* A number as the sheet would read it typed: plain, or a date or time. */
static gboolean
input_number (const char *text, double *n)
{
  O42Value probe;
  O42ErrorCode err = O42_ERR_VALUE;
  gboolean has_date, has_time, ok;

  if (text == NULL || *text == '\0')
    return FALSE;
  probe = o42_value_text (text);
  ok = o42_value_to_number (&probe, n, &err);
  o42_value_clear (&probe);
  if (!ok)
    ok = o42_date_parse (text, n, &has_date, &has_time);
  return ok;
}

static gboolean
validation_allows (O42Sheet *sheet, const O42Validation *v, const char *input)
{
  O42Condition c;
  double x;

  if (input == NULL || *input == '\0')
    return v->allow_blank;
  if (v->kind == O42_VALID_ANY || input[0] == '=')
    return TRUE;   /* formulas are not checked; their value is not known yet */

  if (v->kind == O42_VALID_LIST)
    {
      const char *typed = input[0] == '\'' ? input + 1 : input;
      O42Range r;
      gsize used = 0;
      gboolean found = FALSE;

      if (v->value != NULL && o42_ref_parse (v->value, &r.row0, &r.col0, &used) &&
          v->value[used] == ':' && o42_ref_parse (v->value + used + 1, &r.row1, &r.col1, NULL))
        {
          r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
          for (int row = r.row0; row <= r.row1 && !found; row++)
            for (int col = r.col0; col <= r.col1 && !found; col++)
              {
                char *shown = o42_sheet_get_display (sheet, row, col);
                found = g_ascii_strcasecmp (shown, typed) == 0;
                g_free (shown);
              }
        }
      else if (v->value != NULL)
        {
          char **items = g_strsplit (v->value, ",", -1);
          for (int i = 0; items[i] != NULL && !found; i++)
            found = g_ascii_strcasecmp (g_strstrip (items[i]), typed) == 0;
          g_strfreev (items);
        }
      return found;
    }

  if (v->kind == O42_VALID_LENGTH)
    x = (double) g_utf8_strlen (input, -1);
  else
    {
      if (!input_number (input, &x))
        return FALSE;
      if (v->kind == O42_VALID_WHOLE && x != floor (x))
        return FALSE;
    }

  memset (&c, 0, sizeof c);
  c.op = v->op;
  if (!input_number (v->value, &c.value))
    c.value = 0;
  if (!input_number (v->value2, &c.value2))
    c.value2 = c.value;
  return condition_holds (&c, x);
}

gboolean
o42_sheet_validate (O42Sheet *sheet, int row, int col, const char *input, char **message)
{
  g_return_val_if_fail (sheet != NULL, TRUE);

  for (guint i = 0; i < sheet->validations->len; i++)
    {
      const O42Validation *v = &g_array_index (sheet->validations, O42Validation, i);

      if (!o42_range_contains (&v->range, row, col))
        continue;
      if (!validation_allows (sheet, v, input))
        {
          if (message != NULL)
            *message = g_strdup (v->message != NULL && v->message[0] != '\0'
                                 ? v->message : "The value is not allowed in this cell.");
          return FALSE;
        }
    }
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Array formulas                                                          */
/* ---------------------------------------------------------------------- */

static const O42Range *
array_at (O42Sheet *sheet, int row, int col)
{
  for (guint i = 0; i < sheet->arrays->len; i++)
    {
      const O42Range *a = &g_array_index (sheet->arrays, O42Range, i);
      if (o42_range_contains (a, row, col))
        return a;
    }
  return NULL;
}

static void
array_register (O42Sheet *sheet, const O42Range *block)
{
  g_array_append_val (sheet->arrays, *block);
}

static void
spill_clear_members (O42Sheet *sheet, const O42Range *block, int keep_row, int keep_col)
{
  for (int r = block->row0; r <= block->row1; r++)
    for (int c = block->col0; c <= block->col1; c++)
      {
        O42Cell *m;
        if ((r == block->row0 && c == block->col0) || (r == keep_row && c == keep_col))
          continue;
        m = sheet_find (sheet, r, c);
        if (m != NULL && m->spilled)
          {
            o42_value_clear (&m->value);
            m->value = o42_value_empty ();
            m->spilled = 0;
            sheet_invalidate (sheet, r, c);
            sheet_prune (sheet, r, c);
          }
      }
}

static void
array_dissolve_at (O42Sheet *sheet, int row, int col)
{
  for (guint i = 0; i < sheet->arrays->len; i++)
    if (o42_range_contains (&g_array_index (sheet->arrays, O42Range, i), row, col))
      {
        O42Range block = g_array_index (sheet->arrays, O42Range, i);
        guint64 head = o42_key (block.row0, block.col0);
        g_array_remove_index (sheet->arrays, i);
        if (g_hash_table_contains (sheet->dynamic, &head))
          {
            O42Cell *head_cell = sheet_find (sheet, block.row0, block.col0);
            g_hash_table_remove (sheet->dynamic, &head);
            spill_clear_members (sheet, &block, row, col);
            /* The head must look again: it may be blocked now. */
            if (head_cell != NULL && (block.row0 != row || block.col0 != col))
              {
                head_cell->dirty = 1;
                sheet_invalidate (sheet, block.row0, block.col0);
              }
          }
        return;
      }
}

/* Takes a formula's spill back, when it no longer produces an array. */
static void
spill_retract (O42Sheet *sheet, guint64 key)
{
  if (!g_hash_table_contains (sheet->dynamic, &key))
    return;
  for (guint i = 0; i < sheet->arrays->len; i++)
    {
      const O42Range *a = &g_array_index (sheet->arrays, O42Range, i);
      if (a->row0 == o42_key_row (key) && a->col0 == o42_key_col (key))
        {
          O42Range block = *a;
          g_array_remove_index (sheet->arrays, i);
          g_hash_table_remove (sheet->dynamic, &key);
          spill_clear_members (sheet, &block, -1, -1);
          return;
        }
    }
}

/* Spreads an array result from its formula over the cells beside and
 * below: the head's value, or #SPILL! when a cell in the way holds
 * something of its own. */
static O42Value
spill (O42Sheet *sheet, guint64 key, int rows, int cols, O42Value *values)
{
  int row = o42_key_row (key), col = o42_key_col (key);
  O42Range block = { row, col, row + rows - 1, col + cols - 1 };
  const O42Range *old = NULL;
  gboolean was_dynamic = g_hash_table_contains (sheet->dynamic, &key);

  for (guint i = 0; i < sheet->arrays->len && old == NULL; i++)
    {
      const O42Range *a = &g_array_index (sheet->arrays, O42Range, i);
      if (a->row0 == row && a->col0 == col) old = a;
    }
  if (old != NULL && !was_dynamic)
    {
      /* A fixed array block is spread by its own path; this is not one. */
      return o42_value_copy (&values[0]);
    }

  /* A blocked head is watched like a volatile formula, so it spills
   * as soon as the cells in its way are cleared. */
#define SPILL_BLOCKED()                                       \
  G_STMT_START {                                              \
    guint64 *stored_ = g_new (guint64, 1);                    \
    *stored_ = key;                                           \
    g_hash_table_add (sheet->volatiles, stored_);             \
    spill_retract (sheet, key);                               \
    return o42_value_error (O42_ERR_SPILL);                   \
  } G_STMT_END

  if (block.row1 >= O42_MAX_ROWS || block.col1 >= O42_MAX_COLS)
    SPILL_BLOCKED ();
  for (int r = block.row0; r <= block.row1; r++)
    for (int c = block.col0; c <= block.col1; c++)
      {
        O42Cell *m;
        if (r == row && c == col) continue;
        m = sheet_find (sheet, r, c);
        if (m != NULL && !m->spilled && (m->input != NULL || m->ast != NULL || m->value.type != O42_VALUE_EMPTY))
          SPILL_BLOCKED ();
        if (m != NULL && m->spilled && (old == NULL || !o42_range_contains (old, r, c)))
          SPILL_BLOCKED ();   /* another formula's spill */
      }

  /* Out with the old extent, in with the new. */
  if (old != NULL)
    {
      O42Range previous = *old;
      for (guint i = 0; i < sheet->arrays->len; i++)
        if (g_array_index (sheet->arrays, O42Range, i).row0 == row &&
            g_array_index (sheet->arrays, O42Range, i).col0 == col)
          { g_array_remove_index (sheet->arrays, i); break; }
      for (int r = previous.row0; r <= previous.row1; r++)
        for (int c = previous.col0; c <= previous.col1; c++)
          if (!o42_range_contains (&block, r, c))
            {
              O42Cell *m = sheet_find (sheet, r, c);
              if (m != NULL && m->spilled)
                {
                  o42_value_clear (&m->value);
                  m->value = o42_value_empty ();
                  m->spilled = 0;
                  sheet_invalidate (sheet, r, c);
                  sheet_prune (sheet, r, c);
                }
            }
    }
  g_array_append_val (sheet->arrays, block);
  if (!was_dynamic)
    {
      guint64 *stored = g_new (guint64, 1);
      *stored = key;
      g_hash_table_add (sheet->dynamic, stored);
    }
  {
    O42Cell *head_cell = sheet_find (sheet, row, col);
    if (head_cell != NULL && !tree_is_volatile (head_cell->ast))
      g_hash_table_remove (sheet->volatiles, &key);
  }
  for (int r = block.row0; r <= block.row1; r++)
    for (int c = block.col0; c <= block.col1; c++)
      {
        O42Cell *m;
        if (r == row && c == col) continue;
        m = sheet_ensure (sheet, r, c);
        o42_value_clear (&m->value);
        m->value = o42_value_copy (&values[(r - row) * cols + (c - col)]);
        m->spilled = 1;
        sheet_invalidate (sheet, r, c);
      }
  return o42_value_copy (&values[0]);
}

void
o42_sheet_set_array_formula (O42Sheet *sheet, const O42Range *range, const char *text)
{
  O42Range block;

  g_return_if_fail (sheet != NULL && range != NULL && text != NULL);
  block = o42_range_normalise (range->row0, range->col0, range->row1, range->col1);
  if (block.row1 >= O42_MAX_ROWS || block.col1 >= O42_MAX_COLS)
    return;

  op_begin (sheet);
  for (int r = block.row0; r <= block.row1; r++)
    for (int c = block.col0; c <= block.col1; c++)
      {
        op_capture (sheet, r, c);
        set_input_internal (sheet, r, c, "");
      }
  sheet->placing_array = TRUE;
  set_input_internal (sheet, block.row0, block.col0, text);
  sheet->placing_array = FALSE;
  array_register (sheet, &block);
  for (int r = block.row0; r <= block.row1; r++)
    for (int c = block.col0; c <= block.col1; c++)
      if (r != block.row0 || c != block.col0)
        {
          O42Cell *member = sheet_ensure (sheet, r, c);
          o42_value_clear (&member->value);
          member->value = o42_value_empty ();
        }
  op_end (sheet);
}

gboolean
o42_sheet_array_range (O42Sheet *sheet, int row, int col, O42Range *out)
{
  const O42Range *a;

  g_return_val_if_fail (sheet != NULL, FALSE);
  a = array_at (sheet, row, col);
  if (a == NULL)
    return FALSE;
  if (out != NULL)
    *out = *a;
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Objects in the undo history                                             */
/* ---------------------------------------------------------------------- */

static O42Picture *
picture_copy (const O42Picture *pic)
{
  O42Picture *copy = o42_picture_new (pic->data, pic->format, pic->pixel_w, pic->pixel_h);
  copy->id = pic->id;
  copy->row = pic->row; copy->col = pic->col;
  copy->dx = pic->dx; copy->dy = pic->dy;
  copy->width = pic->width; copy->height = pic->height;
  return copy;
}

static O42Shape *
shape_copy (const O42Shape *shape)
{
  O42Shape *copy = o42_shape_new (shape->kind);

  copy->id = shape->id;
  copy->row = shape->row; copy->col = shape->col;
  copy->dx = shape->dx; copy->dy = shape->dy;
  copy->width = shape->width; copy->height = shape->height;
  g_free (copy->text);
  copy->text = g_strdup (shape->text != NULL ? shape->text : "");
  copy->fill = shape->fill;
  copy->line = shape->line;
  copy->line_width = shape->line_width;
  return copy;
}

static O42Chart *
chart_copy (const O42Chart *chart)
{
  O42Chart *copy = o42_chart_new (chart->kind, &chart->data);
  copy->id = chart->id;
  copy->first_row_labels = chart->first_row_labels;
  copy->first_col_labels = chart->first_col_labels;
  copy->series_in_rows = chart->series_in_rows;
  g_free (copy->title);
  copy->title = g_strdup (chart->title ? chart->title : "");
  g_free (copy->x_title);
  copy->x_title = g_strdup (chart->x_title ? chart->x_title : "");
  g_free (copy->y_title);
  copy->y_title = g_strdup (chart->y_title ? chart->y_title : "");
  copy->legend = chart->legend;
  copy->gridlines = chart->gridlines;
  copy->data_labels = chart->data_labels;
  copy->trend = chart->trend;
  copy->trend_order = chart->trend_order;
  copy->data_sheet = g_strdup (chart->data_sheet != NULL ? chart->data_sheet : "");
  copy->font_family = g_strdup (chart->font_family != NULL ? chart->font_family : "");
  copy->font_size = chart->font_size;
  copy->three_d = chart->three_d;
  copy->err_bars = chart->err_bars;
  copy->err_value = chart->err_value;
  g_free (copy->y_format);
  copy->y_format = g_strdup (chart->y_format ? chart->y_format : "");
  copy->secondary_from = chart->secondary_from;
  copy->has_min = chart->has_min; copy->has_max = chart->has_max;
  copy->min = chart->min; copy->max = chart->max;
  copy->row = chart->row; copy->col = chart->col;
  copy->dx = chart->dx; copy->dy = chart->dy;
  copy->width = chart->width; copy->height = chart->height;
  return copy;
}

static void
obj_snap_clear (ObjSnap *snap)
{
  if (snap->owns_sheet && snap->sheet != NULL)
    o42_sheet_free (snap->sheet);
  g_free (snap->text);
  if (snap->ranges != NULL) g_array_free (snap->ranges, TRUE);
  if (snap->texts != NULL) g_hash_table_unref (snap->texts);
  if (snap->picture != NULL) o42_picture_free (snap->picture);
  if (snap->chart != NULL) o42_chart_free (snap->chart);
  if (snap->shape != NULL) o42_shape_free (snap->shape);
  memset (snap, 0, sizeof *snap);
}

static ObjSnap
obj_snap_take (O42Sheet *sheet, ObjKind kind, int index, guint64 key)
{
  ObjSnap snap;

  memset (&snap, 0, sizeof snap);
  snap.kind = kind;
  snap.sheet = sheet;
  snap.index = index;
  snap.key = key;
  switch (kind)
    {
    case OBJ_COL_WIDTH:  snap.number = o42_sheet_col_width (sheet, index); break;
    case OBJ_ROW_HEIGHT: snap.number = o42_sheet_row_height (sheet, index); break;
    case OBJ_ROW_HIDDEN: snap.flag = o42_sheet_row_hidden_by_hand (sheet, index); break;
    case OBJ_COL_HIDDEN: snap.flag = o42_sheet_col_hidden (sheet, index); break;
    case OBJ_ROW_LEVEL:  snap.number = o42_sheet_row_level (sheet, index); break;
    case OBJ_COL_LEVEL:  snap.number = o42_sheet_col_level (sheet, index); break;
    case OBJ_MERGES:
      snap.ranges = g_array_new (FALSE, FALSE, sizeof (O42Range));
      g_array_append_vals (snap.ranges, sheet->merges->data, sheet->merges->len);
      break;
    case OBJ_NOTE:
      snap.text = g_strdup (g_hash_table_lookup (sheet->notes, &key));
      break;
    case OBJ_LINK:
      snap.text = g_strdup (g_hash_table_lookup (sheet->links, &key));
      break;
    case OBJ_NOTES:
    case OBJ_LINKS:
      {
        GHashTable *from = kind == OBJ_NOTES ? sheet->notes : sheet->links;
        GHashTableIter it;
        gpointer k, v;
        snap.texts = g_hash_table_new_full (key_hash, key_equal, g_free, g_free);
        g_hash_table_iter_init (&it, from);
        while (g_hash_table_iter_next (&it, &k, &v))
          {
            guint64 *stored = g_new (guint64, 1);
            *stored = *(guint64 *) k;
            g_hash_table_insert (snap.texts, stored, g_strdup (v));
          }
      }
      break;
    case OBJ_PICTURE:
      {
        O42Picture *pic = o42_sheet_find_picture (sheet, (guint) index);
        snap.picture = pic != NULL ? picture_copy (pic) : NULL;
      }
      break;
    case OBJ_CHART:
      {
        O42Chart *chart = o42_sheet_find_chart (sheet, (guint) index);
        snap.chart = chart != NULL ? chart_copy (chart) : NULL;
      }
      break;
    case OBJ_SHAPE:
      {
        O42Shape *shape = o42_sheet_find_shape (sheet, (guint) index);
        snap.shape = shape != NULL ? shape_copy (shape) : NULL;
      }
      break;
    case OBJ_SHEET_NAME:
      snap.text = g_strdup (sheet->name);
      break;
    case OBJ_SHEET_PRESENT:
      snap.index = sheet->book != NULL ? o42_book_sheet_index (sheet->book, sheet) : -1;
      snap.flag = snap.index >= 0;
      break;
    }
  return snap;
}

/* Puts the snapshot's state back, touching nothing the undo stack
 * records -- this is the undo stack at work. */
static void
obj_snap_apply (const ObjSnap *snap)
{
  O42Sheet *sheet = snap->sheet;

  switch (snap->kind)
    {
    case OBJ_COL_WIDTH:
      sizes_changed (sheet);
      g_hash_table_insert (sheet->col_widths, GINT_TO_POINTER (snap->index), GINT_TO_POINTER (snap->number));
      break;
    case OBJ_ROW_HEIGHT:
      sizes_changed (sheet);
      g_hash_table_insert (sheet->row_heights, GINT_TO_POINTER (snap->index), GINT_TO_POINTER (snap->number));
      break;
    case OBJ_ROW_HIDDEN:
      sizes_changed (sheet);
      if (snap->flag) g_hash_table_add (sheet->hidden_rows, GINT_TO_POINTER (snap->index));
      else g_hash_table_remove (sheet->hidden_rows, GINT_TO_POINTER (snap->index));
      break;
    case OBJ_COL_HIDDEN:
      sizes_changed (sheet);
      if (snap->flag) g_hash_table_add (sheet->hidden_cols, GINT_TO_POINTER (snap->index));
      else g_hash_table_remove (sheet->hidden_cols, GINT_TO_POINTER (snap->index));
      break;
    case OBJ_ROW_LEVEL:
      level_store (sheet, TRUE, snap->index, snap->number);
      break;
    case OBJ_COL_LEVEL:
      level_store (sheet, FALSE, snap->index, snap->number);
      break;
    case OBJ_MERGES:
      g_array_set_size (sheet->merges, 0);
      g_array_append_vals (sheet->merges, snap->ranges->data, snap->ranges->len);
      break;
    case OBJ_NOTE:
      if (snap->text == NULL || snap->text[0] == '\0')
        g_hash_table_remove (sheet->notes, &snap->key);
      else
        {
          guint64 *stored = g_new (guint64, 1);
          *stored = snap->key;
          g_hash_table_insert (sheet->notes, stored, g_strdup (snap->text));
        }
      break;
    case OBJ_LINK:
      if (snap->text == NULL || snap->text[0] == '\0')
        g_hash_table_remove (sheet->links, &snap->key);
      else
        {
          guint64 *stored = g_new (guint64, 1);
          *stored = snap->key;
          g_hash_table_insert (sheet->links, stored, g_strdup (snap->text));
        }
      break;
    case OBJ_NOTES:
    case OBJ_LINKS:
      {
        GHashTable *to = snap->kind == OBJ_NOTES ? sheet->notes : sheet->links;
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_remove_all (to);
        if (snap->texts != NULL)
          {
            g_hash_table_iter_init (&it, snap->texts);
            while (g_hash_table_iter_next (&it, &k, &v))
              {
                guint64 *stored = g_new (guint64, 1);
                *stored = *(guint64 *) k;
                g_hash_table_insert (to, stored, g_strdup (v));
              }
          }
      }
      break;
    case OBJ_PICTURE:
      {
        O42Picture *pic = o42_sheet_find_picture (sheet, (guint) snap->index);
        if (snap->picture == NULL)
          {
            if (pic != NULL) g_ptr_array_remove (sheet->pictures, pic);
          }
        else if (pic != NULL)
          {
            pic->row = snap->picture->row; pic->col = snap->picture->col;
            pic->dx = snap->picture->dx; pic->dy = snap->picture->dy;
            pic->width = snap->picture->width; pic->height = snap->picture->height;
          }
        else
          g_ptr_array_add (sheet->pictures, picture_copy (snap->picture));
      }
      break;
    case OBJ_CHART:
      {
        O42Chart *chart = o42_sheet_find_chart (sheet, (guint) snap->index);
        if (snap->chart == NULL)
          {
            if (chart != NULL) g_ptr_array_remove (sheet->charts, chart);
          }
        else if (chart != NULL)
          {
            chart->kind = snap->chart->kind;
            chart->data = snap->chart->data;
            chart->first_row_labels = snap->chart->first_row_labels;
            chart->first_col_labels = snap->chart->first_col_labels;
            g_free (chart->title);
            chart->title = g_strdup (snap->chart->title);
            chart->row = snap->chart->row; chart->col = snap->chart->col;
            chart->dx = snap->chart->dx; chart->dy = snap->chart->dy;
            chart->width = snap->chart->width; chart->height = snap->chart->height;
          }
        else
          g_ptr_array_add (sheet->charts, chart_copy (snap->chart));
      }
      break;
    case OBJ_SHAPE:
      {
        O42Shape *have = o42_sheet_find_shape (sheet, (guint) snap->index);

        if (have != NULL)
          g_ptr_array_remove (sheet->shapes, have);
        if (snap->shape != NULL)
          g_ptr_array_add (sheet->shapes, shape_copy (snap->shape));
      }
      break;
    case OBJ_SHEET_NAME:
      if (sheet->book != NULL)
        o42_book_rename_sheet_unrecorded (sheet->book, o42_book_sheet_index (sheet->book, sheet), snap->text);
      break;
    case OBJ_SHEET_PRESENT:
      if (sheet->book != NULL)
        {
          O42Book *book = sheet->book;
          int now = o42_book_sheet_index (book, sheet);

          if (snap->flag && now < 0)
            o42_book_attach_sheet (book, sheet, snap->index);
          else if (!snap->flag && now >= 0)
            o42_book_detach_sheet (book, now);
          /* Formulas elsewhere that name this sheet have to be worked
           * out again: they were #REF! while it was away. */
          for (int i = 0; i < o42_book_n_sheets (book); i++)
            o42_sheet_stale_formulas (o42_book_sheet (book, i));
        }
      break;
    }
}

void
o42_sheet_capture_object (O42Sheet *sheet, guint id)
{
  g_return_if_fail (sheet != NULL);
  if (o42_sheet_find_chart (sheet, id) != NULL)
    obj_capture (sheet, OBJ_CHART, (int) id, 0);
  else if (o42_sheet_find_picture (sheet, id) != NULL)
    obj_capture (sheet, OBJ_PICTURE, (int) id, 0);
  else if (o42_sheet_find_shape (sheet, id) != NULL)
    obj_capture (sheet, OBJ_SHAPE, (int) id, 0);
}

void
o42_sheet_undo_capture_sheet (O42Sheet *sheet, gboolean name)
{
  g_return_if_fail (sheet != NULL);
  obj_capture (sheet, name ? OBJ_SHEET_NAME : OBJ_SHEET_PRESENT, 0, 0);
}

/* ---------------------------------------------------------------------- */
/* Outline groups                                                          */
/* ---------------------------------------------------------------------- */

static void
level_store (O42Sheet *sheet, gboolean rows, int index, int level)
{
  GHashTable *table = rows ? sheet->row_levels : sheet->col_levels;
  GHashTableIter iter;
  gpointer key, value;
  int max = 0;

  level = CLAMP (level, 0, 7);
  if (level == 0)
    g_hash_table_remove (table, GINT_TO_POINTER (index));
  else
    g_hash_table_insert (table, GINT_TO_POINTER (index), GINT_TO_POINTER (level));
  g_hash_table_iter_init (&iter, table);
  while (g_hash_table_iter_next (&iter, &key, &value))
    max = MAX (max, GPOINTER_TO_INT (value));
  if (rows) sheet->max_row_level = max; else sheet->max_col_level = max;
  sheet->modified = TRUE;
}

int
o42_sheet_row_level (O42Sheet *sheet, int row)
{
  g_return_val_if_fail (sheet != NULL, 0);
  return GPOINTER_TO_INT (g_hash_table_lookup (sheet->row_levels, GINT_TO_POINTER (row)));
}

int
o42_sheet_col_level (O42Sheet *sheet, int col)
{
  g_return_val_if_fail (sheet != NULL, 0);
  return GPOINTER_TO_INT (g_hash_table_lookup (sheet->col_levels, GINT_TO_POINTER (col)));
}

void
o42_sheet_set_row_level (O42Sheet *sheet, int row, int level)
{
  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
  obj_capture (sheet, OBJ_ROW_LEVEL, row, 0);
  level_store (sheet, TRUE, row, level);
  op_end (sheet);
}

void
o42_sheet_set_col_level (O42Sheet *sheet, int col, int level)
{
  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
  obj_capture (sheet, OBJ_COL_LEVEL, col, 0);
  level_store (sheet, FALSE, col, level);
  op_end (sheet);
}

int
o42_sheet_max_row_level (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, 0);
  return sheet->max_row_level;
}

int
o42_sheet_max_col_level (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, 0);
  return sheet->max_col_level;
}

void
o42_sheet_group (O42Sheet *sheet, gboolean rows, int lo, int hi, gboolean group)
{
  int limit = rows ? O42_MAX_ROWS : O42_MAX_COLS;

  g_return_if_fail (sheet != NULL);
  if (lo > hi) { int t = lo; lo = hi; hi = t; }
  lo = MAX (lo, 0);
  hi = MIN (hi, limit - 1);
  op_begin (sheet);
  for (int i = lo; i <= hi; i++)
    {
      int level = rows ? o42_sheet_row_level (sheet, i) : o42_sheet_col_level (sheet, i);
      obj_capture (sheet, rows ? OBJ_ROW_LEVEL : OBJ_COL_LEVEL, i, 0);
      level_store (sheet, rows, i, level + (group ? 1 : -1));
    }
  op_end (sheet);
}

/* ---------------------------------------------------------------------- */
/* Pivot tables                                                            */
/* ---------------------------------------------------------------------- */

char *
o42_pivot_fields_to_string (char **fields)
{
  return fields != NULL ? g_strjoinv ("|", fields) : g_strdup ("");
}

char **
o42_pivot_fields_from_string (const char *text)
{
  char **parts;
  GPtrArray *out = g_ptr_array_new ();

  if (text == NULL || text[0] == '\0' || strcmp (text, "-") == 0)
    { g_ptr_array_add (out, NULL); return (char **) g_ptr_array_free (out, FALSE); }
  parts = g_strsplit (text, "|", -1);
  for (int i = 0; parts[i] != NULL; i++)
    if (parts[i][0] != '\0' && strcmp (parts[i], "-") != 0)
      g_ptr_array_add (out, g_strdup (parts[i]));
  g_strfreev (parts);
  g_ptr_array_add (out, NULL);
  return (char **) g_ptr_array_free (out, FALSE);
}

static void
pivot_clear (O42Pivot *pivot)
{
  g_free (pivot->source_sheet);
  g_strfreev (pivot->row_fields);
  g_strfreev (pivot->col_fields);
  g_free (pivot->data_field);
  g_free (pivot->filter_field);
  g_free (pivot->filter_value);
  memset (pivot, 0, sizeof *pivot);
}

static O42Pivot
pivot_copy (const O42Pivot *pivot)
{
  O42Pivot copy = *pivot;
  copy.source_sheet = g_strdup (pivot->source_sheet);
  copy.row_fields = pivot->row_fields ? g_strdupv (pivot->row_fields) : g_new0 (char *, 1);
  copy.col_fields = pivot->col_fields ? g_strdupv (pivot->col_fields) : g_new0 (char *, 1);
  copy.data_field = g_strdup (pivot->data_field ? pivot->data_field : "");
  copy.filter_field = pivot->filter_field != NULL && *pivot->filter_field != '\0' ? g_strdup (pivot->filter_field) : NULL;
  copy.filter_value = g_strdup (pivot->filter_value != NULL ? pivot->filter_value : "");
  return copy;
}

/* A calculated field's value for source row `r`: the expression with
 * each header replaced by that row's cell, evaluated on the source. */
static gboolean
pivot_calc_value (O42Sheet *src, const O42Range *table, const char *expr, int r, double *out)
{
  GString *formula = g_string_new (NULL);
  GPtrArray *heads = g_ptr_array_new_with_free_func (g_free);
  const char *p = expr + 1;   /* past the '=' */
  O42Value v;
  O42ErrorCode e = O42_ERR_VALUE;
  gboolean ok;

  for (int col = table->col0; col <= table->col1; col++)
    g_ptr_array_add (heads, o42_sheet_get_display (src, table->row0, col));

  while (*p != '\0')
    {
      gboolean matched = FALSE;
      if (*p == '"')
        {
          /* A string literal passes through. */
          g_string_append_c (formula, *p++);
          while (*p != '\0' && *p != '"') g_string_append_c (formula, *p++);
          if (*p == '"') g_string_append_c (formula, *p++);
          continue;
        }
      if (g_ascii_isalpha (*p) || *p == '_')
        {
          /* The longest header that starts here and ends at a word
           * boundary wins, so "Unit Price" beats "Unit". */
          gsize best_len = 0;
          int best_col = -1;
          for (guint i = 0; i < heads->len; i++)
            {
              const char *h = g_ptr_array_index (heads, i);
              gsize hl = strlen (h);
              if (hl > 0 && hl > best_len && g_ascii_strncasecmp (p, h, hl) == 0 &&
                  !(g_ascii_isalnum (p[hl]) || p[hl] == '_'))
                { best_len = hl; best_col = table->col0 + (int) i; }
            }
          if (best_col >= 0)
            {
              char *ref = o42_ref_name (r, best_col);
              g_string_append (formula, ref);
              g_free (ref);
              p += best_len;
              matched = TRUE;
            }
        }
      if (!matched)
        g_string_append_c (formula, *p++);
    }
  v = o42_sheet_evaluate_formula (src, formula->str);
  ok = v.type == O42_VALUE_NUMBER && o42_value_to_number (&v, out, &e);
  o42_value_clear (&v);
  g_string_free (formula, TRUE);
  g_ptr_array_unref (heads);
  return ok;
}

/* The column of the source table whose header is `field`, or -1. */
static int
pivot_field_col (O42Sheet *src, const O42Range *table, const char *field)
{
  if (field == NULL) return -1;
  for (int col = table->col0; col <= table->col1; col++)
    {
      char *head = o42_sheet_get_display (src, table->row0, col);
      gboolean hit = g_ascii_strcasecmp (head, field) == 0;
      g_free (head);
      if (hit) return col;
    }
  return -1;
}

typedef struct { double sum, min, max; int count; } Bucket;

/* A key is the values of an axis's fields joined by \002; keys sort
 * field by field, numbers by value and the rest by collation. */
static int
compare_key_parts (const char *x, const char *y)
{
  char *ex, *ey;
  double nx = g_ascii_strtod (x, &ex), ny = g_ascii_strtod (y, &ey);
  if (*x && *y && *ex == '\0' && *ey == '\0')
    return nx < ny ? -1 : nx > ny ? 1 : 0;
  return g_utf8_collate (x, y);
}

static int
compare_keys_collate (gconstpointer a, gconstpointer b)
{
  char **xs = g_strsplit (*(const char *const *) a, "\002", -1);
  char **ys = g_strsplit (*(const char *const *) b, "\002", -1);
  int r = 0;
  for (int i = 0; r == 0 && xs[i] != NULL && ys[i] != NULL; i++)
    r = compare_key_parts (xs[i], ys[i]);
  g_strfreev (xs);
  g_strfreev (ys);
  return r;
}

static void
pivot_put_number (O42Sheet *sheet, int row, int col, double v)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (buf, sizeof buf, "%.15g", v);
  if (g_ascii_strtod (buf, NULL) != v)
    g_ascii_formatd (buf, sizeof buf, "%.17g", v);
  set_input_internal (sheet, row, col, buf);
}

static void
pivot_put_bold (O42Sheet *sheet, int row, int col)
{
  O42Range one = { row, col, row, col };
  O42Fmt f;
  o42_fmt_init_default (&f);
  f.bold = 1;
  o42_sheet_apply_fmt (sheet, &one, O42_FMT_BOLD, &f);
}

static double
bucket_value (const Bucket *b, O42PivotAgg agg)
{
  switch (agg)
    {
    case O42_PIVOT_SUM: return b->sum;
    case O42_PIVOT_COUNT: return b->count;
    case O42_PIVOT_AVERAGE: return b->sum / b->count;
    case O42_PIVOT_MIN: return b->min;
    default: return b->max;
    }
}

static void
bucket_add (Bucket *b, const Bucket *from)
{
  b->sum += from->sum; b->count += from->count;
  b->min = MIN (b->min, from->min); b->max = MAX (b->max, from->max);
}

static void
bucket_init (Bucket *b)
{
  b->sum = 0; b->count = 0; b->min = HUGE_VAL; b->max = -HUGE_VAL;
}

/* The key of a record along one axis. */
static char *
pivot_key (O42Sheet *src, int row, const int *cols, int n)
{
  GString *key = g_string_new (NULL);
  for (int i = 0; i < n; i++)
    {
      char *v = o42_sheet_get_display (src, row, cols[i]);
      if (i > 0) g_string_append_c (key, '\002');
      g_string_append (key, v);
      g_free (v);
    }
  return g_string_free (key, FALSE);
}

static void
key_list_add (GPtrArray *keys, const char *key)
{
  for (guint i = 0; i < keys->len; i++)
    if (strcmp (g_ptr_array_index (keys, i), key) == 0)
      return;
  g_ptr_array_add (keys, g_strdup (key));
}

/* Lays a pivot out from its source, first clearing where it was. */
static void
pivot_layout (O42Sheet *sheet, O42Pivot *p)
{
  O42Sheet *src = sheet;
  int n_row = p->row_fields ? (int) g_strv_length (p->row_fields) : 0;
  int n_col = p->col_fields ? (int) g_strv_length (p->col_fields) : 0;
  int row_cols[8], col_cols[8], data_col;
  GPtrArray *row_keys = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *col_keys = g_ptr_array_new_with_free_func (g_free);
  GHashTable *buckets = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  static const char *agg_names[] = { "Sum", "Count", "Average", "Min", "Max" };
  gboolean ok = TRUE, calculated, has_filter;
  int filter_col = -1;

  if (p->source_sheet != NULL && sheet->book != NULL)
    {
      src = o42_book_find_sheet (sheet->book, p->source_sheet);
      if (src == NULL) src = sheet;
    }
  n_row = MIN (n_row, 8);
  n_col = MIN (n_col, 8);
  for (int i = 0; i < n_row; i++)
    if ((row_cols[i] = pivot_field_col (src, &p->source, p->row_fields[i])) < 0) ok = FALSE;
  for (int i = 0; i < n_col; i++)
    if ((col_cols[i] = pivot_field_col (src, &p->source, p->col_fields[i])) < 0) ok = FALSE;
  calculated = p->data_field != NULL && p->data_field[0] == '=';
  data_col = calculated ? -1 : pivot_field_col (src, &p->source, p->data_field);
  has_filter = p->filter_field != NULL && *p->filter_field != '\0';
  if (has_filter && (filter_col = pivot_field_col (src, &p->source, p->filter_field)) < 0)
    ok = FALSE;

  /* Clear the last layout. */
  if (p->rows > 0 && p->cols > 0)
    {
      O42Range old = { p->row, p->col, MIN (p->row + p->rows - 1, O42_MAX_ROWS - 1),
                       MIN (p->col + p->cols - 1, O42_MAX_COLS - 1) };
      for (int r = old.row0; r <= old.row1; r++)
        for (int c = old.col0; c <= old.col1; c++)
          {
            op_capture (sheet, r, c);
            set_input_internal (sheet, r, c, "");
          }
      o42_sheet_clear_formats (sheet, &old);
    }
  if (!ok || n_row < 1 || (data_col < 0 && !calculated))
    {
      p->rows = p->cols = 0;
      goto out;
    }

  /* Gather keys and buckets, from the rows the filter lets through. */
  for (int r = p->source.row0 + 1; r <= p->source.row1; r++)
    {
      char *rk;
      if (has_filter)
        {
          char *shown = o42_sheet_get_display (src, r, filter_col);
          gboolean keep = g_ascii_strcasecmp (shown, p->filter_value != NULL ? p->filter_value : "") == 0;
          g_free (shown);
          if (!keep) continue;
        }
      rk = pivot_key (src, r, row_cols, n_row);
      char *ck = n_col > 0 ? pivot_key (src, r, col_cols, n_col) : g_strdup ("");
      char *key = g_strdup_printf ("%s\001%s", rk, ck);
      Bucket *b = g_hash_table_lookup (buckets, key);
      O42Value v;
      double x;
      O42ErrorCode e = O42_ERR_VALUE;

      key_list_add (row_keys, rk);
      key_list_add (col_keys, ck);
      if (b == NULL)
        {
          b = g_new0 (Bucket, 1);
          bucket_init (b);
          g_hash_table_insert (buckets, g_strdup (key), b);
        }
      if (calculated)
        {
          double calc;
          v = pivot_calc_value (src, &p->source, p->data_field, r, &calc) ? o42_value_number (calc) : o42_value_empty ();
        }
      else
        o42_sheet_get_value (src, r, data_col, &v);
      if (p->agg == O42_PIVOT_COUNT)
        {
          if (v.type != O42_VALUE_EMPTY) b->count++;
        }
      else if (v.type == O42_VALUE_NUMBER && o42_value_to_number (&v, &x, &e))
        {
          b->sum += x; b->count++;
          b->min = MIN (b->min, x); b->max = MAX (b->max, x);
        }
      o42_value_clear (&v);
      g_free (key);
      g_free (rk);
      g_free (ck);
    }
  g_ptr_array_sort (row_keys, compare_keys_collate);
  g_ptr_array_sort (col_keys, compare_keys_collate);

  /* Lay it out: a title, the column-key header rows, the row keys in
   * their columns, one row per row key, totals. */
  {
    int filter_rows = has_filter ? 2 : 0;         /* "Field: value" and a blank */
    int r0 = p->row + filter_rows, c0 = p->col;
    int ncols = (int) col_keys->len;
    gboolean by_col = n_col > 0;
    int header_rows = by_col ? n_col : 1;
    int data_c0 = c0 + n_row;                     /* first value column */
    int total_c = data_c0 + (by_col ? ncols : 0); /* the row-total column */
    int out_rows = 1 + header_rows + (int) row_keys->len + 1;
    int out_cols = n_row + (by_col ? ncols : 0) + 1;
    char *title = g_strdup_printf ("%s of %s", agg_names[p->agg], calculated ? p->data_field + 1 : p->data_field);
    Bucket *col_totals;

    if (r0 + out_rows > O42_MAX_ROWS || c0 + out_cols > O42_MAX_COLS)
      { g_free (title); p->rows = p->cols = 0; goto out; }
    for (int r = p->row; r < r0 + out_rows; r++)
      for (int c = c0; c < c0 + out_cols; c++)
        op_capture (sheet, r, c);
    if (has_filter)
      {
        char *line = g_strdup_printf ("%s: %s", p->filter_field, p->filter_value != NULL ? p->filter_value : "");
        set_input_internal (sheet, p->row, c0, line);
        pivot_put_bold (sheet, p->row, c0);
        g_free (line);
      }

    set_input_internal (sheet, r0, c0, title);
    pivot_put_bold (sheet, r0, c0);
    g_free (title);

    /* Header rows: the row-field names on the last header row, the
     * column keys field by field above the value columns. */
    for (int i = 0; i < n_row; i++)
      {
        set_input_internal (sheet, r0 + header_rows, c0 + i, p->row_fields[i]);
        pivot_put_bold (sheet, r0 + header_rows, c0 + i);
      }
    if (by_col)
      {
        for (int j = 0; j < ncols; j++)
          {
            char **parts = g_strsplit (g_ptr_array_index (col_keys, j), "\002", -1);
            for (int k = 0; k < n_col && parts[k] != NULL; k++)
              {
                /* A repeated upper-level key is written once. */
                gboolean same = j > 0;
                if (same)
                  {
                    char **prev = g_strsplit (g_ptr_array_index (col_keys, j - 1), "\002", -1);
                    for (int m = 0; m <= k && same; m++)
                      same = prev[m] != NULL && strcmp (prev[m], parts[m]) == 0;
                    g_strfreev (prev);
                  }
                if (!same)
                  {
                    set_input_internal (sheet, r0 + 1 + k, data_c0 + j, parts[k]);
                    pivot_put_bold (sheet, r0 + 1 + k, data_c0 + j);
                  }
              }
            g_strfreev (parts);
          }
        set_input_internal (sheet, r0 + header_rows, total_c, "Grand Total");
      }
    else
      set_input_internal (sheet, r0 + header_rows, total_c, "Total");
    pivot_put_bold (sheet, r0 + header_rows, total_c);

    col_totals = g_new0 (Bucket, ncols + 1);
    for (int j = 0; j <= ncols; j++) bucket_init (&col_totals[j]);

    for (guint i = 0; i < row_keys->len; i++)
      {
        const char *rk = g_ptr_array_index (row_keys, i);
        Bucket row_total;
        int r = r0 + 1 + header_rows + (int) i;
        char **parts = g_strsplit (rk, "\002", -1);

        bucket_init (&row_total);
        for (int k = 0; k < n_row && parts[k] != NULL; k++)
          {
            gboolean same = i > 0;
            if (same)
              {
                char **prev = g_strsplit (g_ptr_array_index (row_keys, i - 1), "\002", -1);
                for (int m = 0; m <= k && same; m++)
                  same = prev[m] != NULL && strcmp (prev[m], parts[m]) == 0;
                g_strfreev (prev);
              }
            if (!same)
              {
                set_input_internal (sheet, r, c0 + k, parts[k]);
                pivot_put_bold (sheet, r, c0 + k);
              }
          }
        g_strfreev (parts);
        for (int j = 0; j < ncols; j++)
          {
            const char *ck = g_ptr_array_index (col_keys, j);
            char *key = g_strdup_printf ("%s\001%s", rk, ck);
            Bucket *b = g_hash_table_lookup (buckets, key);
            g_free (key);
            if (b != NULL && b->count > 0)
              {
                if (by_col)
                  pivot_put_number (sheet, r, data_c0 + j, bucket_value (b, p->agg));
                bucket_add (&row_total, b);
                bucket_add (&col_totals[j], b);
              }
          }
        if (row_total.count > 0)
          pivot_put_number (sheet, r, total_c, bucket_value (&row_total, p->agg));
        bucket_add (&col_totals[ncols], &row_total);
      }
    {
      int r = r0 + 1 + header_rows + (int) row_keys->len;
      set_input_internal (sheet, r, c0, "Grand Total");
      pivot_put_bold (sheet, r, c0);
      for (int j = 0; j <= ncols; j++)
        {
          Bucket *b = &col_totals[j];
          int c = j < ncols ? data_c0 + j : total_c;
          if (!by_col && j < ncols) continue;
          if (b->count > 0)
            {
              pivot_put_number (sheet, r, c, bucket_value (b, p->agg));
              pivot_put_bold (sheet, r, c);
            }
        }
    }
    g_free (col_totals);
    p->rows = out_rows + filter_rows;
    p->cols = out_cols;
  }

out:
  g_ptr_array_unref (row_keys);
  g_ptr_array_unref (col_keys);
  g_hash_table_unref (buckets);
  sheet->modified = TRUE;
}

void
o42_sheet_add_pivot (O42Sheet *sheet, const O42Pivot *pivot)
{
  O42Pivot copy;

  g_return_if_fail (sheet != NULL && pivot != NULL);
  copy = pivot_copy (pivot);
  copy.rows = copy.cols = 0;
  g_array_append_val (sheet->pivots, copy);
  op_begin (sheet);
  pivot_layout (sheet, &g_array_index (sheet->pivots, O42Pivot, sheet->pivots->len - 1));
  op_end (sheet);
}

void
o42_sheet_define_pivot (O42Sheet *sheet, const O42Pivot *pivot)
{
  O42Pivot copy;

  g_return_if_fail (sheet != NULL && pivot != NULL);
  copy = pivot_copy (pivot);
  g_array_append_val (sheet->pivots, copy);
}

void
o42_sheet_refresh_pivots (O42Sheet *sheet)
{
  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
  for (guint i = 0; i < sheet->pivots->len; i++)
    pivot_layout (sheet, &g_array_index (sheet->pivots, O42Pivot, i));
  op_end (sheet);
}

O42Pivot *
o42_sheet_pivot_at (O42Sheet *sheet, int row, int col)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  for (guint i = 0; i < sheet->pivots->len; i++)
    {
      O42Pivot *p = &g_array_index (sheet->pivots, O42Pivot, i);
      if (row >= p->row && col >= p->col && row < p->row + MAX (p->rows, 1) && col < p->col + MAX (p->cols, 1))
        return p;
    }
  return NULL;
}

void
o42_sheet_remove_pivot (O42Sheet *sheet, O42Pivot *pivot)
{
  g_return_if_fail (sheet != NULL);
  for (guint i = 0; i < sheet->pivots->len; i++)
    if (&g_array_index (sheet->pivots, O42Pivot, i) == pivot)
      {
        pivot_clear (pivot);
        g_array_remove_index (sheet->pivots, i);
        sheet->modified = TRUE;
        return;
      }
}

GArray *
o42_sheet_pivots (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->pivots;
}

gboolean
o42_sheet_array_is_dynamic (O42Sheet *sheet, int row, int col)
{
  const O42Range *a;
  guint64 head;

  g_return_val_if_fail (sheet != NULL, FALSE);
  a = array_at (sheet, row, col);
  if (a == NULL)
    return FALSE;
  head = o42_key (a->row0, a->col0);
  return g_hash_table_contains (sheet->dynamic, &head);
}

O42Value
o42_sheet_evaluate_formula (O42Sheet *sheet, const char *text)
{
  O42Node *tree;
  O42Value result;
  int saved_row, saved_col;

  g_return_val_if_fail (sheet != NULL && text != NULL, o42_value_error (O42_ERR_VALUE));
  tree = o42_formula_parse (text[0] == '=' ? text + 1 : text);
  if (tree == NULL)
    return o42_value_error (O42_ERR_NAME);
  saved_row = sheet->eval.row;
  saved_col = sheet->eval.col;
  sheet->eval.row = 0;
  sheet->eval.col = 0;
  result = o42_eval (&sheet->eval, tree);
  sheet->eval.row = saved_row;
  sheet->eval.col = saved_col;
  o42_node_free (tree);
  return result;
}

/* ---------------------------------------------------------------------- */
/* Data > Remove Duplicates, Data > Subtotals                              */
/* ---------------------------------------------------------------------- */

static char *
row_key (O42Sheet *sheet, int row, const int *cols, int n_cols)
{
  GString *key = g_string_new (NULL);
  for (int i = 0; i < n_cols; i++)
    {
      char *shown = o42_sheet_get_display (sheet, row, cols[i]);
      char *folded = g_utf8_casefold (shown, -1);
      g_string_append (key, folded);
      g_string_append_c (key, '\001');
      g_free (folded);
      g_free (shown);
    }
  return g_string_free (key, FALSE);
}

int
o42_sheet_remove_duplicates (O42Sheet *sheet, const O42Range *range,
                             const int *cols, int n_cols, gboolean has_header)
{
  O42Range r;
  GHashTable *seen;
  GArray *doomed;
  int removed = 0;

  g_return_val_if_fail (sheet != NULL && range != NULL && cols != NULL && n_cols > 0, 0);
  r = o42_range_normalise (range->row0, range->col0, range->row1, range->col1);
  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  doomed = g_array_new (FALSE, FALSE, sizeof (int));
  for (int row = r.row0 + (has_header ? 1 : 0); row <= r.row1; row++)
    {
      char *key = row_key (sheet, row, cols, n_cols);
      if (g_hash_table_contains (seen, key))
        {
          g_array_append_val (doomed, row);
          g_free (key);
        }
      else
        g_hash_table_add (seen, key);
    }

  op_begin (sheet);
  /* From the bottom, so the rows still to go keep their numbers. */
  for (int i = (int) doomed->len - 1; i >= 0; i--)
    {
      int row = g_array_index (doomed, int, i);
      O42Range slice = { row, r.col0, row, r.col1 };
      o42_sheet_shift_cells (sheet, &slice, TRUE, FALSE);
      removed++;
    }
  op_end (sheet);
  g_array_unref (doomed);
  g_hash_table_unref (seen);
  return removed;
}

static const char *
subtotal_function_name (int function_num)
{
  switch (function_num)
    {
    case 1: return "Average";
    case 2: return "Count";
    case 3: return "Count";
    case 4: return "Max";
    case 5: return "Min";
    case 6: return "Product";
    default: return "Total";
    }
}

/* Whether a row is one of ours: a SUBTOTAL formula somewhere in it. */
static gboolean
row_is_subtotal (O42Sheet *sheet, int row, const O42Range *range)
{
  for (int col = range->col0; col <= range->col1; col++)
    {
      char *input = o42_sheet_get_input (sheet, row, col);
      gboolean yes = input != NULL && g_str_has_prefix (input, "=SUBTOTAL(");
      g_free (input);
      if (yes)
        return TRUE;
    }
  return FALSE;
}

int
o42_sheet_remove_subtotals (O42Sheet *sheet, const O42Range *range)
{
  O42Range r;
  int removed = 0;

  g_return_val_if_fail (sheet != NULL && range != NULL, 0);
  r = o42_range_normalise (range->row0, range->col0, range->row1, range->col1);
  op_begin (sheet);
  for (int row = r.row1; row >= r.row0; row--)
    if (row_is_subtotal (sheet, row, &r))
      {
        /* The outline goes with the row; then the row. */
        while (o42_sheet_row_level (sheet, row) > 0)
          o42_sheet_set_row_level (sheet, row, o42_sheet_row_level (sheet, row) - 1);
        o42_sheet_delete_rows (sheet, row, 1);
        removed++;
      }
  for (int row = r.row0; row <= r.row1 - removed; row++)
    while (o42_sheet_row_level (sheet, row) > 0)
      o42_sheet_set_row_level (sheet, row, o42_sheet_row_level (sheet, row) - 1);
  op_end (sheet);
  return removed;
}

O42Range
o42_sheet_subtotal (O42Sheet *sheet, const O42Range *range, int group_col,
                    const int *sum_cols, int n_sum, int function_num,
                    gboolean has_header, gboolean replace)
{
  O42Range r;
  GArray *starts;         /* first data row of each group, then one past the last */
  int first, last, added = 0;

  g_return_val_if_fail (sheet != NULL && range != NULL, *range);
  r = o42_range_normalise (range->row0, range->col0, range->row1, range->col1);
  op_begin (sheet);
  if (replace)
    r.row1 -= o42_sheet_remove_subtotals (sheet, &r);
  first = r.row0 + (has_header ? 1 : 0);
  last = r.row1;
  if (first > last)
    {
      op_end (sheet);
      return r;
    }

  starts = g_array_new (FALSE, FALSE, sizeof (int));
  {
    char *previous = NULL;
    for (int row = first; row <= last; row++)
      {
        char *shown = o42_sheet_get_display (sheet, row, group_col);
        char *folded = g_utf8_casefold (shown, -1);
        if (previous == NULL || strcmp (previous, folded) != 0)
          g_array_append_val (starts, row);
        g_free (previous);
        previous = folded;
        g_free (shown);
      }
    g_free (previous);
  }
  {
    int end = last + 1;
    g_array_append_val (starts, end);
  }

  /* From the last group up, so the rows above keep their numbers: a
   * subtotal row after each group, then the grand total at the end. */
  {
    int grand = last + 1;
    o42_sheet_insert_rows (sheet, grand, 1);
    {
      char *label = g_strdup_printf ("Grand %s", subtotal_function_name (function_num));
      o42_sheet_set_input (sheet, grand, group_col, label);
      g_free (label);
    }
    added++;
    for (int g = (int) starts->len - 2; g >= 0; g--)
      {
        int lo = g_array_index (starts, int, g);
        int hi = g_array_index (starts, int, g + 1) - 1;
        int at = hi + 1;
        char *value = o42_sheet_get_display (sheet, lo, group_col);
        char *label = g_strdup_printf ("%s %s", value, subtotal_function_name (function_num));

        o42_sheet_insert_rows (sheet, at, 1);
        added++;
        o42_sheet_set_input (sheet, at, group_col, label);
        for (int i = 0; i < n_sum; i++)
          {
            char *a = o42_ref_name (lo, sum_cols[i]);
            char *b = o42_ref_name (hi, sum_cols[i]);
            char *formula = g_strdup_printf ("=SUBTOTAL(%d,%s:%s)", function_num, a, b);
            o42_sheet_set_input (sheet, at, sum_cols[i], formula);
            g_free (formula); g_free (a); g_free (b);
          }
        {
          O42Fmt bold;
          O42Range cells = { at, r.col0, at, r.col1 };
          o42_fmt_init_default (&bold);
          bold.bold = 1;
          o42_sheet_apply_fmt (sheet, &cells, O42_FMT_BOLD, &bold);
        }
        o42_sheet_group (sheet, TRUE, lo, hi, TRUE);
        g_free (label);
        g_free (value);
      }
    /* The grand total moved down by the subtotal rows put in above it. */
    grand = last + added;
    for (int i = 0; i < n_sum; i++)
      {
        char *a = o42_ref_name (first, sum_cols[i]);
        char *b = o42_ref_name (grand - 1, sum_cols[i]);
        char *formula = g_strdup_printf ("=SUBTOTAL(%d,%s:%s)", function_num, a, b);
        o42_sheet_set_input (sheet, grand, sum_cols[i], formula);
        g_free (formula); g_free (a); g_free (b);
      }
    {
      O42Fmt bold;
      O42Range cells = { grand, r.col0, grand, r.col1 };
      o42_fmt_init_default (&bold);
      bold.bold = 1;
      o42_sheet_apply_fmt (sheet, &cells, O42_FMT_BOLD, &bold);
    }
    o42_sheet_group (sheet, TRUE, first, grand - 1, TRUE);
    r.row1 = grand;
  }
  g_array_unref (starts);
  op_end (sheet);
  return r;
}

/* ---------------------------------------------------------------------- */
/* Printing                                                                */
/* ---------------------------------------------------------------------- */

const O42PrintSetup *
o42_sheet_print_setup (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return &sheet->print;
}

void
o42_sheet_set_print_area (O42Sheet *sheet, const O42Range *area)
{
  g_return_if_fail (sheet != NULL);
  sheet->print.has_area = area != NULL;
  if (area != NULL)
    sheet->print.area = o42_range_normalise (area->row0, area->col0, area->row1, area->col1);
  sheet->modified = TRUE;
}

void
o42_sheet_set_header_footer (O42Sheet *sheet, const char *header, const char *footer)
{
  g_return_if_fail (sheet != NULL);
  if (header != NULL)
    {
      g_free (sheet->print.header);
      sheet->print.header = g_strdup (header);
    }
  if (footer != NULL)
    {
      g_free (sheet->print.footer);
      sheet->print.footer = g_strdup (footer);
    }
  sheet->modified = TRUE;
}

void
o42_sheet_set_print_options (O42Sheet *sheet, gboolean gridlines, gboolean headings, int title_rows)
{
  g_return_if_fail (sheet != NULL);
  sheet->print.gridlines = gridlines;
  sheet->print.headings = headings;
  sheet->print.title_rows = CLAMP (title_rows, 0, O42_MAX_ROWS - 1);
  sheet->modified = TRUE;
}

/* ---------------------------------------------------------------------- */
/* Tables                                                                  */
/* ---------------------------------------------------------------------- */

void
o42_sheet_add_table (O42Sheet *sheet, const char *name, const O42Range *range, gboolean has_headers)
{
  O42Table t;

  g_return_if_fail (sheet != NULL && name != NULL && *name != '\0' && range != NULL);
  o42_sheet_remove_table (sheet, name);
  t.name = g_strdup (name);
  t.range = o42_range_normalise (range->row0, range->col0, range->row1, range->col1);
  t.has_headers = has_headers;
  g_array_append_val (sheet->tables, t);
  sheet->modified = TRUE;
}

gboolean
o42_sheet_remove_table (O42Sheet *sheet, const char *name)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  for (guint i = 0; i < sheet->tables->len; i++)
    {
      O42Table *t = &g_array_index (sheet->tables, O42Table, i);
      if (g_ascii_strcasecmp (t->name, name) == 0)
        {
          g_free (t->name);
          g_array_remove_index (sheet->tables, i);
          sheet->modified = TRUE;
          return TRUE;
        }
    }
  return FALSE;
}

O42Table *
o42_sheet_find_table (O42Sheet *sheet, const char *name)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  for (guint i = 0; name != NULL && i < sheet->tables->len; i++)
    {
      O42Table *t = &g_array_index (sheet->tables, O42Table, i);
      if (g_ascii_strcasecmp (t->name, name) == 0)
        return t;
    }
  return NULL;
}

O42Table *
o42_sheet_table_at (O42Sheet *sheet, int row, int col)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  for (guint i = 0; i < sheet->tables->len; i++)
    {
      O42Table *t = &g_array_index (sheet->tables, O42Table, i);
      if (o42_range_contains (&t->range, row, col))
        return t;
    }
  return NULL;
}

GArray *
o42_sheet_tables (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->tables;
}

/* The column of a table whose header reads `field`, or -1. */
static int
table_field_col (O42Sheet *sheet, const O42Table *t, const char *field)
{
  for (int col = t->range.col0; col <= t->range.col1; col++)
    {
      char *head = o42_sheet_get_display (sheet, t->range.row0, col);
      gboolean hit = g_ascii_strcasecmp (head, field) == 0;
      g_free (head);
      if (hit)
        return col;
    }
  return -1;
}

gboolean
o42_sheet_table_range (O42Sheet *sheet, const char *text, int row, O42Range *out)
{
  const char *bracket;
  char *name, *spec = NULL;
  O42Table *t;
  O42Sheet *holder = sheet;
  gboolean ok = FALSE;
  gboolean this_row = FALSE;
  int first_data;

  g_return_val_if_fail (sheet != NULL && text != NULL && out != NULL, FALSE);
  bracket = strchr (text, '[');
  name = bracket != NULL ? g_strndup (text, (gsize) (bracket - text)) : g_strdup (text);
  t = o42_sheet_find_table (sheet, name);
  if (t == NULL && sheet->book != NULL)
    {
      holder = o42_book_find_table (sheet->book, name);
      t = holder != NULL ? o42_sheet_find_table (holder, name) : NULL;
    }
  if (t == NULL)
    {
      g_free (name);
      return FALSE;
    }
  first_data = t->has_headers ? t->range.row0 + 1 : t->range.row0;

  if (bracket != NULL)
    {
      /* The specifier, with the outer brackets off and the inner ones
       * (Table1[[#Data],[Sales]]) taken as a list. */
      const char *end = text + strlen (text);
      while (end > bracket && end[-1] != ']')
        end--;
      spec = g_strndup (bracket + 1, (gsize) (end - bracket - 2));
    }

  *out = t->range;
  if (spec == NULL || *spec == '\0')
    {
      out->row0 = first_data;
      ok = out->row1 >= out->row0;
    }
  else
    {
      char **parts = g_strsplit (spec, ",", -1);
      const char *field = NULL;
      gboolean headers = FALSE, all = FALSE, data = FALSE;

      for (int i = 0; parts[i] != NULL; i++)
        {
          char *part = g_strstrip (parts[i]);
          gsize len;

          if (*part == '[') part++;
          len = strlen (part);
          if (len > 0 && part[len - 1] == ']') part[len - 1] = '\0';
          if (*part == '@') { this_row = TRUE; part++; }
          if (*part == '\0') continue;
          if (g_ascii_strcasecmp (part, "#All") == 0) all = TRUE;
          else if (g_ascii_strcasecmp (part, "#Headers") == 0) headers = TRUE;
          else if (g_ascii_strcasecmp (part, "#Data") == 0) data = TRUE;
          else if (g_ascii_strcasecmp (part, "#This Row") == 0) this_row = TRUE;
          else if (*part != '#') field = part;
        }

      if (all)
        ok = TRUE;
      else if (headers && t->has_headers)
        { out->row1 = out->row0 = t->range.row0; ok = TRUE; }
      else
        { out->row0 = first_data; ok = out->row1 >= out->row0; }

      if (field != NULL)
        {
          int col = table_field_col (holder, t, field);
          if (col < 0)
            ok = FALSE;
          else
            { out->col0 = out->col1 = col; }
        }
      if (ok && this_row)
        {
          if (row < first_data || row > t->range.row1)
            ok = FALSE;
          else
            { out->row0 = out->row1 = row; }
        }
      (void) data;
      g_strfreev (parts);
    }

  g_free (spec);
  g_free (name);
  return ok;
}

/* ---------------------------------------------------------------------- */
/* Cell styles                                                             */
/* ---------------------------------------------------------------------- */

void
o42_sheet_apply_style (O42Sheet *sheet, const O42Range *range, const char *name)
{
  O42Fmt fmt;
  O42FmtMask mask = 0;
  gboolean known;

  g_return_if_fail (sheet != NULL && range != NULL);
  o42_fmt_init_default (&fmt);
  known = name != NULL && sheet->book != NULL && o42_book_style (sheet->book, name, &fmt, &mask);
  if (name != NULL && !known)
    return;

  op_begin (sheet);
  if (known && mask != 0)
    o42_sheet_apply_fmt (sheet, range, mask, &fmt);
  for (int row = range->row0; row <= range->row1; row++)
    for (int col = range->col0; col <= range->col1; col++)
      {
        O42Cell *cell;

        if (row < 0 || col < 0 || row >= O42_MAX_ROWS || col >= O42_MAX_COLS)
          continue;
        if (name == NULL && sheet_find (sheet, row, col) == NULL)
          continue;
        op_capture (sheet, row, col);
        cell = sheet_ensure (sheet, row, col);
        cell->style = name != NULL ? g_intern_string (name) : NULL;
        sheet_prune (sheet, row, col);
      }
  op_end (sheet);
  sheet->modified = TRUE;
}

const char *
o42_sheet_cell_style (O42Sheet *sheet, int row, int col)
{
  O42Cell *cell;

  g_return_val_if_fail (sheet != NULL, NULL);
  cell = sheet_find (sheet, row, col);
  return cell != NULL ? cell->style : NULL;
}

void
o42_sheet_restyle (O42Sheet *sheet, const char *name)
{
  O42Fmt fmt;
  O42FmtMask mask = 0;
  GHashTableIter iter;
  gpointer key_ptr, value;
  GArray *wearing;

  g_return_if_fail (sheet != NULL);
  if (sheet->book == NULL || !o42_book_style (sheet->book, name, &fmt, &mask) || mask == 0)
    return;

  wearing = g_array_new (FALSE, FALSE, sizeof (guint64));
  g_hash_table_iter_init (&iter, sheet->cells);
  while (g_hash_table_iter_next (&iter, &key_ptr, &value))
    {
      O42Cell *cell = value;
      if (cell->style != NULL && g_ascii_strcasecmp (cell->style, name) == 0)
        g_array_append_val (wearing, *(guint64 *) key_ptr);
    }

  op_begin (sheet);
  for (guint i = 0; i < wearing->len; i++)
    {
      guint64 key = g_array_index (wearing, guint64, i);
      O42Range one = { o42_key_row (key), o42_key_col (key), o42_key_row (key), o42_key_col (key) };
      o42_sheet_apply_fmt (sheet, &one, mask, &fmt);
    }
  op_end (sheet);
  g_array_unref (wearing);
}

/* ---------------------------------------------------------------------- */
/* Scenarios                                                               */
/* ---------------------------------------------------------------------- */

static Scenario *
scenario_find (O42Sheet *sheet, const char *name)
{
  for (guint i = 0; name != NULL && i < sheet->scenarios->len; i++)
    {
      Scenario *s = g_ptr_array_index (sheet->scenarios, i);
      if (g_ascii_strcasecmp (s->name, name) == 0)
        return s;
    }
  return NULL;
}

static Scenario *
scenario_ensure (O42Sheet *sheet, const char *name, const char *comment)
{
  Scenario *s = scenario_find (sheet, name);

  if (s == NULL)
    {
      s = g_new0 (Scenario, 1);
      s->name = g_strdup (name);
      s->comment = g_strdup (comment != NULL ? comment : "");
      s->keys = g_array_new (FALSE, FALSE, sizeof (guint64));
      s->values = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (sheet->scenarios, s);
    }
  else if (comment != NULL && *comment != '\0')
    {
      g_free (s->comment);
      s->comment = g_strdup (comment);
    }
  return s;
}

void
o42_sheet_add_scenario (O42Sheet *sheet, const char *name, const O42Range *cells, const char *comment)
{
  Scenario *s;
  O42Range r;

  g_return_if_fail (sheet != NULL && name != NULL && *name != '\0' && cells != NULL);
  o42_sheet_remove_scenario (sheet, name);
  s = scenario_ensure (sheet, name, comment);
  r = o42_range_normalise (cells->row0, cells->col0, cells->row1, cells->col1);
  for (int row = r.row0; row <= r.row1; row++)
    for (int col = r.col0; col <= r.col1; col++)
      {
        guint64 key = o42_key (row, col);
        g_array_append_val (s->keys, key);
        g_ptr_array_add (s->values, o42_sheet_get_input (sheet, row, col));
      }
  sheet->modified = TRUE;
}

void
o42_sheet_define_scenario (O42Sheet *sheet, const char *name, const char *comment,
                           int row, int col, const char *value)
{
  Scenario *s;
  guint64 key = o42_key (row, col);

  g_return_if_fail (sheet != NULL && name != NULL);
  s = scenario_ensure (sheet, name, comment);
  if (row < 0 || col < 0)
    return;
  g_array_append_val (s->keys, key);
  g_ptr_array_add (s->values, g_strdup (value != NULL ? value : ""));
  sheet->modified = TRUE;
}

gboolean
o42_sheet_show_scenario (O42Sheet *sheet, const char *name)
{
  Scenario *s;

  g_return_val_if_fail (sheet != NULL, FALSE);
  s = scenario_find (sheet, name);
  if (s == NULL)
    return FALSE;
  op_begin (sheet);
  for (guint i = 0; i < s->keys->len && i < s->values->len; i++)
    {
      guint64 key = g_array_index (s->keys, guint64, i);
      op_capture (sheet, o42_key_row (key), o42_key_col (key));
      set_input_internal (sheet, o42_key_row (key), o42_key_col (key),
                          g_ptr_array_index (s->values, i));
    }
  op_end (sheet);
  return TRUE;
}

gboolean
o42_sheet_remove_scenario (O42Sheet *sheet, const char *name)
{
  Scenario *s;

  g_return_val_if_fail (sheet != NULL, FALSE);
  s = scenario_find (sheet, name);
  if (s == NULL)
    return FALSE;
  g_ptr_array_remove (sheet->scenarios, s);
  sheet->modified = TRUE;
  return TRUE;
}

int
o42_sheet_n_scenarios (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, 0);
  return (int) sheet->scenarios->len;
}

const char *
o42_sheet_scenario_name (O42Sheet *sheet, int index)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  if (index < 0 || (guint) index >= sheet->scenarios->len)
    return NULL;
  return ((Scenario *) g_ptr_array_index (sheet->scenarios, index))->name;
}

gboolean
o42_sheet_scenario_cells (O42Sheet *sheet, const char *name, GArray **keys,
                          GPtrArray **values, const char **comment)
{
  Scenario *s;

  g_return_val_if_fail (sheet != NULL, FALSE);
  s = scenario_find (sheet, name);
  if (s == NULL)
    return FALSE;
  if (keys != NULL) *keys = s->keys;
  if (values != NULL) *values = s->values;
  if (comment != NULL) *comment = s->comment;
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Consolidate                                                             */
/* ---------------------------------------------------------------------- */

/* The sheet a source range names, or `sheet` itself. */
static O42Sheet *
source_sheet (O42Sheet *sheet, const O42SheetRange *source)
{
  O42Sheet *other;

  if (source->sheet == NULL || sheet->book == NULL)
    return sheet;
  other = o42_book_find_sheet (sheet->book, source->sheet);
  return other != NULL ? other : sheet;
}

O42Range
o42_sheet_consolidate (O42Sheet *sheet, const O42SheetRange *sources, int n_sources,
                       int row, int col, O42PivotAgg agg,
                       gboolean labels_top, gboolean labels_left)
{
  O42Range out = { row, col, row, col };
  GPtrArray *row_names = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *col_names = g_ptr_array_new_with_free_func (g_free);
  GHashTable *buckets;   /* "r\001c" -> Bucket */
  int rows = 0, cols = 0;

  g_return_val_if_fail (sheet != NULL && sources != NULL && n_sources > 0, out);
  buckets = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  if (!labels_top && !labels_left)
    {
      /* By position: the largest source sets the shape. */
      for (int i = 0; i < n_sources; i++)
        {
          rows = MAX (rows, sources[i].range.row1 - sources[i].range.row0 + 1);
          cols = MAX (cols, sources[i].range.col1 - sources[i].range.col0 + 1);
        }
      for (int i = 0; i < n_sources; i++)
        {
          O42Sheet *src = source_sheet (sheet, &sources[i]);
          for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++)
              {
                int sr = sources[i].range.row0 + r, sc = sources[i].range.col0 + c;
                O42Value v;
                char *key;
                Bucket *b;
                double x;
                O42ErrorCode e = O42_ERR_VALUE;

                if (sr > sources[i].range.row1 || sc > sources[i].range.col1)
                  continue;
                o42_sheet_get_value (src, sr, sc, &v);
                if (v.type != O42_VALUE_NUMBER || !o42_value_to_number (&v, &x, &e))
                  { o42_value_clear (&v); continue; }
                o42_value_clear (&v);
                key = g_strdup_printf ("%d\001%d", r, c);
                b = g_hash_table_lookup (buckets, key);
                if (b == NULL)
                  {
                    b = g_new0 (Bucket, 1);
                    bucket_init (b);
                    g_hash_table_insert (buckets, key, b);
                  }
                else
                  g_free (key);
                b->sum += x; b->count++;
                b->min = MIN (b->min, x); b->max = MAX (b->max, x);
              }
        }
    }
  else
    {
      /* By label: the union of the names each source gives its rows
       * and columns, in the order they first appear. */
      for (int i = 0; i < n_sources; i++)
        {
          O42Sheet *src = source_sheet (sheet, &sources[i]);
          const O42Range *r0 = &sources[i].range;

          for (int r = r0->row0 + (labels_top ? 1 : 0); r <= r0->row1; r++)
            for (int c = r0->col0 + (labels_left ? 1 : 0); c <= r0->col1; c++)
              {
                char *rname = labels_left ? o42_sheet_get_display (src, r, r0->col0)
                                          : g_strdup_printf ("%d", r - r0->row0);
                char *cname = labels_top ? o42_sheet_get_display (src, r0->row0, c)
                                         : g_strdup_printf ("%d", c - r0->col0);
                O42Value v;
                double x;
                O42ErrorCode e = O42_ERR_VALUE;
                char *key;
                Bucket *b;

                key_list_add (row_names, rname);
                key_list_add (col_names, cname);
                o42_sheet_get_value (src, r, c, &v);
                if (v.type == O42_VALUE_NUMBER && o42_value_to_number (&v, &x, &e))
                  {
                    key = g_strdup_printf ("%s\001%s", rname, cname);
                    b = g_hash_table_lookup (buckets, key);
                    if (b == NULL)
                      {
                        b = g_new0 (Bucket, 1);
                        bucket_init (b);
                        g_hash_table_insert (buckets, key, b);
                      }
                    else
                      g_free (key);
                    b->sum += x; b->count++;
                    b->min = MIN (b->min, x); b->max = MAX (b->max, x);
                  }
                o42_value_clear (&v);
                g_free (rname);
                g_free (cname);
              }
        }
      rows = (int) row_names->len;
      cols = (int) col_names->len;
    }

  op_begin (sheet);
  {
    int header = (labels_top || labels_left) ? 1 : 0;

    for (int r = 0; r < rows + header; r++)
      for (int c = 0; c < cols + header; c++)
        {
          int dr = row + r, dc = col + c;

          if (dr >= O42_MAX_ROWS || dc >= O42_MAX_COLS)
            continue;
          op_capture (sheet, dr, dc);
          if (header && r == 0 && c == 0)
            set_input_internal (sheet, dr, dc, "");
          else if (header && r == 0)
            {
              set_input_internal (sheet, dr, dc, g_ptr_array_index (col_names, c - 1));
              pivot_put_bold (sheet, dr, dc);
            }
          else if (header && c == 0)
            {
              set_input_internal (sheet, dr, dc, g_ptr_array_index (row_names, r - 1));
              pivot_put_bold (sheet, dr, dc);
            }
          else
            {
              char *key = header
                ? g_strdup_printf ("%s\001%s", (const char *) g_ptr_array_index (row_names, r - 1),
                                   (const char *) g_ptr_array_index (col_names, c - 1))
                : g_strdup_printf ("%d\001%d", r, c);
              Bucket *b = g_hash_table_lookup (buckets, key);

              g_free (key);
              if (b != NULL && b->count > 0)
                pivot_put_number (sheet, dr, dc, bucket_value (b, agg));
              else
                set_input_internal (sheet, dr, dc, "");
            }
        }
    out.row1 = MIN (row + rows + header - 1, O42_MAX_ROWS - 1);
    out.col1 = MIN (col + cols + header - 1, O42_MAX_COLS - 1);
  }
  op_end (sheet);

  g_hash_table_unref (buckets);
  g_ptr_array_unref (row_names);
  g_ptr_array_unref (col_names);
  sheet->modified = TRUE;
  return out;
}

/* ---------------------------------------------------------------------- */
/* Advanced filter                                                         */
/* ---------------------------------------------------------------------- */

/* Whether one source row answers one row of criteria: every condition
 * in it must hold. */
static gboolean
criteria_row_matches (O42Sheet *sheet, const O42Range *list, int row,
                      const O42Range *criteria, int crow)
{
  for (int c = criteria->col0; c <= criteria->col1; c++)
    {
      char *want = o42_sheet_get_display (sheet, crow, c);
      char *field = o42_sheet_get_display (sheet, criteria->row0, c);
      int col = -1;
      O42Value v;
      gboolean ok;

      if (*want == '\0')
        { g_free (want); g_free (field); continue; }
      for (int i = list->col0; i <= list->col1 && col < 0; i++)
        {
          char *head = o42_sheet_get_display (sheet, list->row0, i);
          if (g_ascii_strcasecmp (head, field) == 0)
            col = i;
          g_free (head);
        }
      g_free (field);
      if (col < 0)
        { g_free (want); return FALSE; }
      o42_sheet_get_value (sheet, row, col, &v);
      ok = o42_criterion_matches (want, &v);
      o42_value_clear (&v);
      g_free (want);
      if (!ok)
        return FALSE;
    }
  return TRUE;
}

int
o42_sheet_advanced_filter (O42Sheet *sheet, const O42Range *list, const O42Range *criteria,
                           int dest_row, int dest_col, gboolean unique)
{
  O42Range l, c;
  GArray *keep;
  GHashTable *seen;
  int matched = 0;

  g_return_val_if_fail (sheet != NULL && list != NULL && criteria != NULL, 0);
  l = o42_range_normalise (list->row0, list->col0, list->row1, list->col1);
  c = o42_range_normalise (criteria->row0, criteria->col0, criteria->row1, criteria->col1);
  keep = g_array_new (FALSE, FALSE, sizeof (int));
  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (int row = l.row0 + 1; row <= l.row1; row++)
    {
      gboolean any = c.row1 <= c.row0;   /* no criteria rows: everything answers */

      for (int crow = c.row0 + 1; crow <= c.row1 && !any; crow++)
        any = criteria_row_matches (sheet, &l, row, &c, crow);
      if (!any)
        continue;
      if (unique)
        {
          GString *key = g_string_new (NULL);
          for (int col = l.col0; col <= l.col1; col++)
            {
              char *shown = o42_sheet_get_display (sheet, row, col);
              char *folded = g_utf8_casefold (shown, -1);
              g_string_append (key, folded);
              g_string_append_c (key, '\001');
              g_free (folded);
              g_free (shown);
            }
          if (g_hash_table_contains (seen, key->str))
            { g_string_free (key, TRUE); continue; }
          g_hash_table_add (seen, g_string_free (key, FALSE));
        }
      g_array_append_val (keep, row);
      matched++;
    }

  op_begin (sheet);
  if (dest_row < 0)
    {
      /* In place: the rows that did not answer are hidden. */
      guint at = 0;
      for (int row = l.row0 + 1; row <= l.row1; row++)
        {
          gboolean keeping = at < keep->len && g_array_index (keep, int, at) == row;
          if (keeping) at++;
          obj_capture (sheet, OBJ_ROW_HIDDEN, row, 0);
          o42_sheet_set_row_hidden (sheet, row, !keeping);
        }
    }
  else
    {
      /* Copied: the header, then the rows that answered. */
      for (int col = l.col0; col <= l.col1; col++)
        {
          char *head = o42_sheet_get_input (sheet, l.row0, col);
          int dc = dest_col + (col - l.col0);
          if (dc < O42_MAX_COLS)
            {
              op_capture (sheet, dest_row, dc);
              set_input_internal (sheet, dest_row, dc, head);
            }
          g_free (head);
        }
      for (guint i = 0; i < keep->len; i++)
        {
          int row = g_array_index (keep, int, i);
          int dr = dest_row + 1 + (int) i;

          if (dr >= O42_MAX_ROWS)
            break;
          for (int col = l.col0; col <= l.col1; col++)
            {
              char *text = o42_sheet_get_input (sheet, row, col);
              int dc = dest_col + (col - l.col0);
              if (dc < O42_MAX_COLS)
                {
                  op_capture (sheet, dr, dc);
                  set_input_internal (sheet, dr, dc, text);
                }
              g_free (text);
            }
        }
    }
  op_end (sheet);

  g_array_unref (keep);
  g_hash_table_unref (seen);
  sheet->modified = TRUE;
  return matched;
}

/* ---------------------------------------------------------------------- */
/* Solver                                                                  */
/* ---------------------------------------------------------------------- */

typedef struct {
  O42Sheet             *sheet;
  int                   target_row, target_col;
  O42SolverGoal         goal;
  double                goal_value;
  const O42Ref         *changing;
  int                   n;
  const O42SolverBound *bounds;
  int                   n_bounds;
  int                   evaluations;
} Solve;

/* Puts the values in the changing cells and reads what the target
 * comes to, with the broken bounds added on as a penalty. */
static double
solve_objective (Solve *s, const double *x)
{
  double target = 0, penalty = 0;
  O42Value v;
  O42ErrorCode e = O42_ERR_VALUE;

  s->evaluations++;
  for (int i = 0; i < s->n; i++)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      set_input_internal (s->sheet, s->changing[i].row, s->changing[i].col,
                          g_ascii_dtostr (buf, sizeof buf, x[i]));
    }
  o42_sheet_get_value (s->sheet, s->target_row, s->target_col, &v);
  if (v.type != O42_VALUE_NUMBER || !o42_value_to_number (&v, &target, &e))
    { o42_value_clear (&v); return 1e300; }
  o42_value_clear (&v);

  for (int i = 0; i < s->n_bounds; i++)
    {
      double got;
      O42Value bv;
      double broken = 0;

      o42_sheet_get_value (s->sheet, s->bounds[i].row, s->bounds[i].col, &bv);
      if (bv.type != O42_VALUE_NUMBER || !o42_value_to_number (&bv, &got, &e))
        { o42_value_clear (&bv); penalty += 1e6; continue; }
      o42_value_clear (&bv);
      switch (s->bounds[i].op)
        {
        case O42_SOLVER_LE: broken = MAX (got - s->bounds[i].value, 0); break;
        case O42_SOLVER_GE: broken = MAX (s->bounds[i].value - got, 0); break;
        default:            broken = fabs (got - s->bounds[i].value); break;
        }
      penalty += 1000 * broken * broken + 1000 * broken;
    }

  switch (s->goal)
    {
    case O42_SOLVER_MAX:   return -target + penalty;
    case O42_SOLVER_MIN:   return target + penalty;
    default:               return fabs (target - s->goal_value) + penalty;
    }
}

gboolean
o42_sheet_solve (O42Sheet *sheet, int target_row, int target_col,
                 O42SolverGoal goal, double goal_value,
                 const O42Ref *changing, int n_changing,
                 const O42SolverBound *bounds, int n_bounds,
                 double *reached)
{
  Solve s;
  int n = n_changing;
  double **simplex;
  double *f, *best, *centroid, *trial, *trial2;
  gboolean ok = FALSE;

  g_return_val_if_fail (sheet != NULL && changing != NULL, FALSE);
  if (n < 1 || n > 16)
    return FALSE;

  memset (&s, 0, sizeof s);
  s.sheet = sheet;
  s.target_row = target_row;
  s.target_col = target_col;
  s.goal = goal;
  s.goal_value = goal_value;
  s.changing = changing;
  s.n = n;
  s.bounds = bounds;
  s.n_bounds = n_bounds;

  simplex = g_new0 (double *, n + 1);
  for (int i = 0; i <= n; i++)
    simplex[i] = g_new0 (double, n);
  f = g_new0 (double, n + 1);
  best = g_new0 (double, n);
  centroid = g_new0 (double, n);
  trial = g_new0 (double, n);
  trial2 = g_new0 (double, n);

  op_begin (sheet);
  for (int i = 0; i < n; i++)
    op_capture (sheet, changing[i].row, changing[i].col);

  /* The starting point is what the cells hold; the other corners are
   * a step away along each axis. */
  for (int i = 0; i < n; i++)
    {
      O42Value v;
      double x = 0;
      O42ErrorCode e = O42_ERR_VALUE;

      o42_sheet_get_value (sheet, changing[i].row, changing[i].col, &v);
      if (v.type == O42_VALUE_NUMBER)
        o42_value_to_number (&v, &x, &e);
      o42_value_clear (&v);
      simplex[0][i] = x;
    }
  for (int i = 1; i <= n; i++)
    {
      memcpy (simplex[i], simplex[0], sizeof (double) * n);
      simplex[i][i - 1] += (fabs (simplex[0][i - 1]) > 1e-9) ? 0.1 * simplex[0][i - 1] : 1.0;
    }
  for (int i = 0; i <= n; i++)
    f[i] = solve_objective (&s, simplex[i]);

  /* Nelder and Mead: reflect the worst corner through the others,
   * stretching or shrinking as that goes well or badly. */
  while (s.evaluations < 4000)
    {
      int hi = 0, lo = 0, next = 0;
      double fr, fe, fc, spread;

      for (int i = 1; i <= n; i++)
        {
          if (f[i] > f[hi]) hi = i;
          if (f[i] < f[lo]) lo = i;
        }
      next = (hi == 0) ? 1 : 0;
      for (int i = 0; i <= n; i++)
        if (i != hi && f[i] > f[next]) next = i;

      spread = fabs (f[hi] - f[lo]);
      if (spread <= 1e-12 * (fabs (f[hi]) + fabs (f[lo]) + 1e-12))
        break;

      for (int j = 0; j < n; j++)
        {
          centroid[j] = 0;
          for (int i = 0; i <= n; i++)
            if (i != hi)
              centroid[j] += simplex[i][j];
          centroid[j] /= n;
        }

      for (int j = 0; j < n; j++)
        trial[j] = centroid[j] + (centroid[j] - simplex[hi][j]);
      fr = solve_objective (&s, trial);

      if (fr < f[lo])
        {
          for (int j = 0; j < n; j++)
            trial2[j] = centroid[j] + 2 * (centroid[j] - simplex[hi][j]);
          fe = solve_objective (&s, trial2);
          if (fe < fr)
            { memcpy (simplex[hi], trial2, sizeof (double) * n); f[hi] = fe; }
          else
            { memcpy (simplex[hi], trial, sizeof (double) * n); f[hi] = fr; }
        }
      else if (fr < f[next])
        { memcpy (simplex[hi], trial, sizeof (double) * n); f[hi] = fr; }
      else
        {
          for (int j = 0; j < n; j++)
            trial2[j] = centroid[j] + 0.5 * (simplex[hi][j] - centroid[j]);
          fc = solve_objective (&s, trial2);
          if (fc < f[hi])
            { memcpy (simplex[hi], trial2, sizeof (double) * n); f[hi] = fc; }
          else
            {
              /* Nothing helped: draw every corner towards the best. */
              for (int i = 0; i <= n; i++)
                if (i != lo)
                  {
                    for (int j = 0; j < n; j++)
                      simplex[i][j] = simplex[lo][j] + 0.5 * (simplex[i][j] - simplex[lo][j]);
                    f[i] = solve_objective (&s, simplex[i]);
                  }
            }
        }
    }

  {
    int lo = 0;
    for (int i = 1; i <= n; i++)
      if (f[i] < f[lo]) lo = i;
    memcpy (best, simplex[lo], sizeof (double) * n);
    ok = f[lo] < 1e299;
    for (int i = 0; i < n; i++)
      {
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        set_input_internal (sheet, changing[i].row, changing[i].col,
                            g_ascii_dtostr (buf, sizeof buf, best[i]));
      }
    if (reached != NULL)
      {
        O42Value v;
        double got = 0;
        O42ErrorCode e = O42_ERR_VALUE;
        o42_sheet_get_value (sheet, target_row, target_col, &v);
        if (v.type == O42_VALUE_NUMBER)
          o42_value_to_number (&v, &got, &e);
        o42_value_clear (&v);
        *reached = got;
      }
  }
  op_end (sheet);

  for (int i = 0; i <= n; i++)
    g_free (simplex[i]);
  g_free (simplex);
  g_free (f);
  g_free (best);
  g_free (centroid);
  g_free (trial);
  g_free (trial2);
  sheet->modified = TRUE;
  return ok;
}

void
o42_sheet_set_print_scale (O42Sheet *sheet, int scale, int fit_wide, int fit_tall)
{
  g_return_if_fail (sheet != NULL);
  sheet->print.scale = CLAMP (scale, 10, 400);
  sheet->print.fit_wide = MAX (fit_wide, 0);
  sheet->print.fit_tall = MAX (fit_tall, 0);
  sheet->modified = TRUE;
}

void
o42_sheet_set_print_margin (O42Sheet *sheet, double points)
{
  g_return_if_fail (sheet != NULL);
  sheet->print.margin = CLAMP (points, 0, 200);
  sheet->modified = TRUE;
}

static int
compare_ints (gconstpointer a, gconstpointer b)
{
  return *(const int *) a - *(const int *) b;
}

void
o42_sheet_toggle_page_break (O42Sheet *sheet, gboolean rows, int at)
{
  GArray *breaks;

  g_return_if_fail (sheet != NULL);
  breaks = rows ? sheet->row_breaks : sheet->col_breaks;
  for (guint i = 0; i < breaks->len; i++)
    if (g_array_index (breaks, int, i) == at)
      {
        g_array_remove_index (breaks, i);
        sheet->modified = TRUE;
        return;
      }
  if (at <= 0)
    return;
  g_array_append_val (breaks, at);
  g_array_sort (breaks, compare_ints);
  sheet->modified = TRUE;
}

gboolean
o42_sheet_page_break (O42Sheet *sheet, gboolean rows, int at)
{
  GArray *breaks;

  g_return_val_if_fail (sheet != NULL, FALSE);
  breaks = rows ? sheet->row_breaks : sheet->col_breaks;
  for (guint i = 0; i < breaks->len; i++)
    if (g_array_index (breaks, int, i) == at)
      return TRUE;
  return FALSE;
}

GArray *
o42_sheet_page_breaks (O42Sheet *sheet, gboolean rows)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return rows ? sheet->row_breaks : sheet->col_breaks;
}

/* ---------------------------------------------------------------------- */
/* Shapes                                                                  */
/* ---------------------------------------------------------------------- */

O42Shape *
o42_sheet_add_shape (O42Sheet *sheet, O42ShapeKind kind, int row, int col)
{
  O42Shape *shape;

  g_return_val_if_fail (sheet != NULL, NULL);
  shape = o42_shape_new (kind);
  shape->id = sheet->next_shape_id++;
  shape->row = CLAMP (row, 0, O42_MAX_ROWS - 1);
  shape->col = CLAMP (col, 0, O42_MAX_COLS - 1);
  /* The snapshot is taken before the shape exists, so undoing takes
   * it away again. */
  op_begin (sheet);
  obj_capture (sheet, OBJ_SHAPE, (int) shape->id, 0);
  op_end (sheet);
  g_ptr_array_add (sheet->shapes, shape);
  sheet->modified = TRUE;
  return shape;
}

void
o42_sheet_remove_shape (O42Sheet *sheet, guint id)
{
  g_return_if_fail (sheet != NULL);
  op_begin (sheet);
  obj_capture (sheet, OBJ_SHAPE, (int) id, 0);
  op_end (sheet);
  for (guint i = 0; i < sheet->shapes->len; i++)
    if (((O42Shape *) g_ptr_array_index (sheet->shapes, i))->id == id)
      {
        g_ptr_array_remove_index (sheet->shapes, i);
        sheet->modified = TRUE;
        return;
      }
}

O42Shape *
o42_sheet_find_shape (O42Sheet *sheet, guint id)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  for (guint i = 0; i < sheet->shapes->len; i++)
    {
      O42Shape *shape = g_ptr_array_index (sheet->shapes, i);
      if (shape->id == id)
        return shape;
    }
  return NULL;
}

GPtrArray *
o42_sheet_shapes (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->shapes;
}

/* Excel's password hash, which every spreadsheet has copied: the bits
 * roll left, each character is exclusive-ored in from the back, and
 * the length and a constant go in at the end. */
guint16
o42_password_hash (const char *password)
{
  guint16 hash = 0;
  gsize length;

  if (password == NULL || *password == 0)
    return 0;
  length = strlen (password);
  for (gsize i = length; i > 0; i--)
    {
      hash = ((hash >> 14) & 0x01) | ((hash << 1) & 0x7FFF);
      hash ^= (guchar) password[i - 1];
    }
  hash = ((hash >> 14) & 0x01) | ((hash << 1) & 0x7FFF);
  hash ^= 0xCE4B;
  hash ^= (guint16) length;
  return hash;
}

void
o42_sheet_set_password (O42Sheet *sheet, const char *password)
{
  g_return_if_fail (sheet != NULL);
  sheet->password = o42_password_hash (password);
  sheet->modified = TRUE;
}

void
o42_sheet_set_password_hash (O42Sheet *sheet, guint16 hash)
{
  g_return_if_fail (sheet != NULL);
  sheet->password = hash;
}

guint16
o42_sheet_password_hash (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, 0);
  return sheet->password;
}

gboolean
o42_sheet_password_matches (O42Sheet *sheet, const char *password)
{
  g_return_val_if_fail (sheet != NULL, TRUE);
  if (sheet->password == 0)
    return TRUE;
  return o42_password_hash (password) == sheet->password;
}

void
o42_sheet_set_tab_colour (O42Sheet *sheet, guint32 colour)
{
  g_return_if_fail (sheet != NULL);
  sheet->tab_colour = colour;
  sheet->modified = TRUE;
}

guint32
o42_sheet_tab_colour (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, O42_TAB_NO_COLOUR);
  return sheet->tab_colour;
}

/* ---------------------------------------------------------------------- */
/* AutoFormat                                                              */
/* ---------------------------------------------------------------------- */

/* Ready-made looks for a table, as Excel's AutoFormat offers: a heading
 * row, banded or plain rows underneath, a rule under the heading and a
 * box round the whole where the look calls for one.  The first row of
 * the range is taken to be its heading. */

typedef struct {
  const char    *name;
  guint32        head_fill;
  guint32        head_colour;
  gboolean       head_bold;
  guint32        band_fill;     /* O42_FILL_NONE for none */
  O42BorderStyle under_head;
  gboolean       outline;
} AutoFormatLook;

static const AutoFormatLook AUTO_FORMATS[] = {
  { "Simple",      0xFFFFFF, 0x000000, TRUE, O42_FILL_NONE, O42_BORDER_THIN,   FALSE },
  { "Classic",     0xC0C0C0, 0x000000, TRUE, 0xF2F2F2,      O42_BORDER_MEDIUM, TRUE },
  { "Financial",   0xFFFFFF, 0x1F497D, TRUE, O42_FILL_NONE, O42_BORDER_DOUBLE, FALSE },
  { "Colourful",   0x1F497D, 0xFFFFFF, TRUE, 0xDCE6F1,      O42_BORDER_THIN,   TRUE },
  { "Plain rules", 0xFFFFFF, 0x000000, TRUE, O42_FILL_NONE, O42_BORDER_THIN,   FALSE }
};

int
o42_auto_format_count (void)
{
  return (int) G_N_ELEMENTS (AUTO_FORMATS);
}

const char *
o42_auto_format_name (int which)
{
  if (which < 0 || which >= (int) G_N_ELEMENTS (AUTO_FORMATS))
    return NULL;
  return AUTO_FORMATS[which].name;
}

gboolean
o42_auto_format_parse (const char *name, int *which)
{
  for (guint i = 0; name != NULL && i < G_N_ELEMENTS (AUTO_FORMATS); i++)
    if (g_ascii_strcasecmp (name, AUTO_FORMATS[i].name) == 0)
      { *which = (int) i; return TRUE; }
  return FALSE;
}

void
o42_sheet_auto_format (O42Sheet *sheet, const O42Range *range, int which)
{
  const AutoFormatLook *look;
  O42Range r;
  O42Fmt fmt;

  g_return_if_fail (sheet != NULL && range != NULL);
  if (which < 0 || which >= (int) G_N_ELEMENTS (AUTO_FORMATS))
    which = 0;
  look = &AUTO_FORMATS[which];
  r = o42_range_normalise (range->row0, range->col0, range->row1, range->col1);

  op_begin (sheet);

  /* Plainly first: a look put on top of another would otherwise be a
   * mixture of the two. */
  o42_fmt_init_default (&fmt);
  o42_sheet_apply_fmt (sheet, &r, O42_FMT_FILL | O42_FMT_BOLD | O42_FMT_COLOUR, &fmt);

  {
    O42Range head = { r.row0, r.col0, r.row0, r.col1 };

    o42_fmt_init_default (&fmt);
    fmt.fill = look->head_fill;
    fmt.colour = look->head_colour;
    fmt.bold = look->head_bold;
    o42_sheet_apply_fmt (sheet, &head, O42_FMT_FILL | O42_FMT_COLOUR | O42_FMT_BOLD, &fmt);

    o42_fmt_init_default (&fmt);
    fmt.border_style[O42_SIDE_BOTTOM] = look->under_head;
    fmt.border_colour[O42_SIDE_BOTTOM] = 0x000000;
    o42_fmt_sync_borders (&fmt);
    o42_sheet_apply_fmt (sheet, &head, O42_FMT_BORDERS, &fmt);
  }

  if (look->band_fill != O42_FILL_NONE)
    for (int row = r.row0 + 2; row <= r.row1; row += 2)
      {
        O42Range band = { row, r.col0, row, r.col1 };

        o42_fmt_init_default (&fmt);
        fmt.fill = look->band_fill;
        o42_sheet_apply_fmt (sheet, &band, O42_FMT_FILL, &fmt);
      }

  if (look->outline)
    {
      /* Only the edges wear a side; every cell would be a grid. */
      O42Range edges[4] = { { r.row0, r.col0, r.row0, r.col1 },
                            { r.row1, r.col0, r.row1, r.col1 },
                            { r.row0, r.col0, r.row1, r.col0 },
                            { r.row0, r.col1, r.row1, r.col1 } };
      int sides[4] = { O42_SIDE_TOP, O42_SIDE_BOTTOM, O42_SIDE_LEFT, O42_SIDE_RIGHT };

      for (int i = 0; i < 4; i++)
        {
          o42_fmt_init_default (&fmt);
          fmt.border_style[sides[i]] = O42_BORDER_THIN;
          fmt.border_colour[sides[i]] = 0x000000;
          o42_fmt_sync_borders (&fmt);
          o42_sheet_apply_fmt (sheet, &edges[i], O42_FMT_BORDERS, &fmt);
        }
    }

  op_end (sheet);
  sheet->modified = TRUE;
}

/* ---------------------------------------------------------------------- */
/* What-If tables                                                          */
/* ---------------------------------------------------------------------- */

/* Excel's Data > Table: a rectangle whose edges hold the values an
 * input may take and whose corner holds -- or points at -- the formula
 * to work out for each of them.
 *
 *   one variable, down:   the values run down the left column and the
 *                         formulas along the top row
 *   one variable, across: the values run along the top row and the
 *                         formulas down the left column
 *   two variables:        values down the left and along the top, and
 *                         one formula in the corner
 *
 * Excel fills the inside with an array formula, TABLE(), that is worked
 * out again whenever anything changes.  office42 writes the numbers,
 * as it does with a pivot table, and Data > Table can be asked again.
 * What it does is honest either way: each value is put into the input
 * cell, everything is worked out, and the answer is copied back. */
gboolean
o42_sheet_data_table (O42Sheet *sheet, const O42Range *range,
                      int row_input_row, int row_input_col,
                      int col_input_row, int col_input_col)
{
  gboolean has_row_input = row_input_row >= 0 && row_input_col >= 0;
  gboolean has_col_input = col_input_row >= 0 && col_input_col >= 0;
  char *row_was = NULL, *col_was = NULL;
  O42Range r;

  g_return_val_if_fail (sheet != NULL && range != NULL, FALSE);
  if (!has_row_input && !has_col_input)
    return FALSE;

  r = o42_range_normalise (range->row0, range->col0, range->row1, range->col1);
  if (r.row1 <= r.row0 || r.col1 <= r.col0)
    return FALSE;

  if (has_row_input)
    row_was = o42_sheet_get_input (sheet, row_input_row, row_input_col);
  if (has_col_input)
    col_was = o42_sheet_get_input (sheet, col_input_row, col_input_col);

  op_begin (sheet);

  for (int row = r.row0 + 1; row <= r.row1; row++)
    for (int col = r.col0 + 1; col <= r.col1; col++)
      {
        int formula_row, formula_col;
        O42Value answer;
        char *text;

        /* Which cell holds the formula for this one, and what goes
         * into the input cells. */
        if (has_row_input && has_col_input)
          {
            formula_row = r.row0;
            formula_col = r.col0;
            text = o42_sheet_get_display (sheet, r.row0, col);
            set_input_internal (sheet, row_input_row, row_input_col, text);
            g_free (text);
            text = o42_sheet_get_display (sheet, row, r.col0);
            set_input_internal (sheet, col_input_row, col_input_col, text);
            g_free (text);
          }
        else if (has_col_input)
          {
            /* The values run down the left column. */
            formula_row = r.row0;
            formula_col = col;
            text = o42_sheet_get_display (sheet, row, r.col0);
            set_input_internal (sheet, col_input_row, col_input_col, text);
            g_free (text);
          }
        else
          {
            /* The values run along the top row. */
            formula_row = row;
            formula_col = r.col0;
            text = o42_sheet_get_display (sheet, r.row0, col);
            set_input_internal (sheet, row_input_row, row_input_col, text);
            g_free (text);
          }

        o42_sheet_get_value (sheet, formula_row, formula_col, &answer);
        text = value_input_text (&answer);
        op_capture (sheet, row, col);
        set_input_internal (sheet, row, col,
                            answer.type == O42_VALUE_TEXT ? NULL : text);
        o42_value_clear (&answer);
        g_free (text);
      }

  /* The input cells are put back as they were, and everything that
   * depended on them worked out again. */
  if (has_row_input)
    set_input_internal (sheet, row_input_row, row_input_col, row_was);
  if (has_col_input)
    set_input_internal (sheet, col_input_row, col_input_col, col_was);
  g_free (row_was);
  g_free (col_was);

  op_end (sheet);
  sheet->modified = TRUE;
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Database queries                                                        */
/* ---------------------------------------------------------------------- */

void
o42_sheet_add_query (O42Sheet *sheet, const char *sql, const O42Range *at,
                     gboolean headings)
{
  O42Query q;

  g_return_if_fail (sheet != NULL && sql != NULL && at != NULL);
  o42_sheet_remove_query (sheet, at->row0, at->col0);
  q.sql = g_strdup (sql);
  q.at = o42_range_normalise (at->row0, at->col0, at->row1, at->col1);
  q.headings = headings;
  g_array_append_val (sheet->queries, q);
  sheet->modified = TRUE;
}

gboolean
o42_sheet_remove_query (O42Sheet *sheet, int row, int col)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  for (guint i = 0; i < sheet->queries->len; i++)
    {
      O42Query *q = &g_array_index (sheet->queries, O42Query, i);

      if (q->at.row0 == row && q->at.col0 == col)
        {
          g_free (q->sql);
          g_array_remove_index (sheet->queries, i);
          sheet->modified = TRUE;
          return TRUE;
        }
    }
  return FALSE;
}

GArray *
o42_sheet_queries (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, NULL);
  return sheet->queries;
}

/* ---------------------------------------------------------------------- */
/* Form controls                                                           */
/* ---------------------------------------------------------------------- */

/* A control keeps nothing of its own: what it shows is whatever its
 * linked cell says, so a formula and a check box are two views of one
 * number.  Clicking writes the cell, and everything that watches the
 * cell follows. */

static gboolean
control_cell (O42Sheet *sheet, const O42Shape *shape, int *row, int *col)
{
  (void) sheet;
  if (shape == NULL || shape->link == NULL || *shape->link == '\0')
    return FALSE;
  return o42_ref_parse (shape->link, row, col, NULL);
}

gboolean
o42_sheet_control_value (O42Sheet *sheet, const O42Shape *shape, double *value)
{
  O42Value v;
  int row, col;
  gboolean got = FALSE;

  g_return_val_if_fail (sheet != NULL && shape != NULL, FALSE);
  if (!control_cell (sheet, shape, &row, &col))
    return FALSE;

  o42_sheet_get_value (sheet, row, col, &v);
  switch (v.type)
    {
    case O42_VALUE_NUMBER:
      *value = v.as.number;
      got = TRUE;
      break;
    case O42_VALUE_BOOL:
      *value = v.as.boolean ? 1 : 0;
      got = TRUE;
      break;
    default:
      break;
    }
  o42_value_clear (&v);
  return got;
}

char **
o42_sheet_control_items (O42Sheet *sheet, const O42Shape *shape)
{
  GPtrArray *items;
  O42Range r;
  gsize len = 0;

  g_return_val_if_fail (sheet != NULL && shape != NULL, NULL);
  if (shape->source == NULL || *shape->source == '\0')
    return NULL;
  if (!o42_ref_parse (shape->source, &r.row0, &r.col0, &len))
    return NULL;
  if (shape->source[len] == ':')
    {
      if (!o42_ref_parse (shape->source + len + 1, &r.row1, &r.col1, NULL))
        return NULL;
    }
  else
    {
      r.row1 = r.row0;
      r.col1 = r.col0;
    }
  r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);

  items = g_ptr_array_new ();
  for (int row = r.row0; row <= r.row1; row++)
    for (int col = r.col0; col <= r.col1; col++)
      {
        char *text = o42_sheet_get_display (sheet, row, col);

        if (text != NULL && *text != '\0')
          g_ptr_array_add (items, text);
        else
          g_free (text);
      }
  g_ptr_array_add (items, NULL);
  return (char **) g_ptr_array_free (items, FALSE);
}

void
o42_sheet_control_set (O42Sheet *sheet, const O42Shape *shape, double value)
{
  int row, col;
  char text[G_ASCII_DTOSTR_BUF_SIZE];

  g_return_if_fail (sheet != NULL && shape != NULL);
  if (!control_cell (sheet, shape, &row, &col))
    return;

  /* A check box writes what Excel writes: TRUE or FALSE. */
  if (shape->kind == O42_SHAPE_CHECKBOX)
    o42_sheet_set_input (sheet, row, col, value != 0 ? "TRUE" : "FALSE");
  else
    {
      g_ascii_dtostr (text, sizeof text, value);
      o42_sheet_set_input (sheet, row, col, text);
    }
}

gboolean
o42_sheet_control_click (O42Sheet *sheet, const O42Shape *shape,
                         double x, double y, double width, double height)
{
  double value = 0, span;
  gboolean has_value;
  O42ControlPart part;
  int item = 0;

  g_return_val_if_fail (sheet != NULL && shape != NULL, FALSE);
  if (!o42_shape_is_control (shape->kind))
    return FALSE;

  has_value = o42_sheet_control_value (sheet, shape, &value);
  part = o42_shape_control_part (shape, width, height, x, y, has_value, value, &item);
  if (part == O42_CONTROL_NONE)
    return FALSE;

  switch (shape->kind)
    {
    case O42_SHAPE_CHECKBOX:
      o42_sheet_control_set (sheet, shape, (has_value && value != 0) ? 0 : 1);
      return TRUE;

    case O42_SHAPE_OPTION:
      /* The set shares one cell, so choosing one un-chooses the rest
       * without anybody having to be told. */
      o42_sheet_control_set (sheet, shape, shape->value != 0 ? shape->value : 1);
      return TRUE;

    case O42_SHAPE_SPINNER:
    case O42_SHAPE_SCROLLBAR:
      {
        double step = shape->step != 0 ? shape->step : 1;
        double page = shape->page != 0 ? shape->page : step;
        double base = has_value ? value : shape->min;
        gboolean near = (part == O42_CONTROL_UP || part == O42_CONTROL_PAGE_UP);
        double by = (part == O42_CONTROL_PAGE_UP || part == O42_CONTROL_PAGE_DOWN)
                    ? page : step;
        double now;

        if (part == O42_CONTROL_THUMB)
          return FALSE;   /* the thumb is dragged, not clicked */
        /* A spinner counts up when its top arrow is pressed; a scroll
         * bar's near arrow goes back towards its start, as a scroll
         * bar should. */
        now = shape->kind == O42_SHAPE_SPINNER ? base + (near ? by : -by)
                                               : base + (near ? -by : by);
        span = shape->max - shape->min;
        o42_sheet_control_set (sheet, shape,
                               span > 0 ? CLAMP (now, shape->min, shape->max) : now);
        return TRUE;
      }

    case O42_SHAPE_LISTBOX:
      {
        char **items = o42_sheet_control_items (sheet, shape);
        int n = items != NULL ? (int) g_strv_length (items) : 0;

        g_strfreev (items);
        if (item < 1 || item > n)
          return FALSE;
        o42_sheet_control_set (sheet, shape, item);
        return TRUE;
      }

    default:
      return FALSE;   /* a button runs its script; the window does that */
    }
}

void
o42_sheet_draw_shape (O42Sheet *sheet, const O42Shape *shape,
                      cairo_t *cr, double width, double height)
{
  double value = 0;
  gboolean has_value;
  char **items;

  g_return_if_fail (sheet != NULL && shape != NULL && cr != NULL);
  if (!o42_shape_is_control (shape->kind))
    {
      o42_shape_draw (shape, cr, width, height);
      return;
    }
  has_value = o42_sheet_control_value (sheet, shape, &value);
  items = o42_sheet_control_items (sheet, shape);
  o42_shape_draw_control (shape, cr, width, height, has_value, value, items);
  g_strfreev (items);
}

/* ---------------------------------------------------------------------- */
/* Protection                                                              */
/* ---------------------------------------------------------------------- */

void
o42_sheet_set_protected (O42Sheet *sheet, gboolean protect)
{
  g_return_if_fail (sheet != NULL);
  sheet->protect = protect;
  sheet->modified = TRUE;
}

gboolean
o42_sheet_protected (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  return sheet->protect;
}

gboolean
o42_sheet_cell_editable (O42Sheet *sheet, int row, int col)
{
  g_return_val_if_fail (sheet != NULL, TRUE);
  if (!sheet->protect)
    return TRUE;
  return !o42_sheet_get_fmt (sheet, row, col)->locked;
}

gboolean
o42_sheet_formula_hidden (O42Sheet *sheet, int row, int col)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  return sheet->protect && o42_sheet_get_fmt (sheet, row, col)->hidden;
}

/* ---------------------------------------------------------------------- */
/* Moving a block                                                          */
/* ---------------------------------------------------------------------- */

void
o42_sheet_move_range (O42Sheet *sheet, const O42Range *from, int to_row, int to_col)
{
  O42Range src;
  int drow, dcol, rows, cols;
  char **inputs;
  O42FmtIdx *formats;

  g_return_if_fail (sheet != NULL && from != NULL);
  src = o42_range_normalise (from->row0, from->col0, from->row1, from->col1);
  drow = to_row - src.row0;
  dcol = to_col - src.col0;
  rows = src.row1 - src.row0 + 1;
  cols = src.col1 - src.col0 + 1;
  if ((drow == 0 && dcol == 0) || to_row < 0 || to_col < 0 ||
      to_row + rows > O42_MAX_ROWS || to_col + cols > O42_MAX_COLS)
    return;

  inputs = g_new0 (char *, (gsize) rows * cols);
  formats = g_new0 (O42FmtIdx, (gsize) rows * cols);
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      {
        inputs[r * cols + c] = o42_sheet_get_input (sheet, src.row0 + r, src.col0 + c);
        formats[r * cols + c] = o42_sheet_get_fmt_idx (sheet, src.row0 + r, src.col0 + c);
      }

  op_begin (sheet);
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      {
        op_capture (sheet, src.row0 + r, src.col0 + c);
        op_capture (sheet, to_row + r, to_col + c);
      }
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      set_input_internal (sheet, src.row0 + r, src.col0 + c, "");
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      {
        O42Cell *cell;

        set_input_internal (sheet, to_row + r, to_col + c, inputs[r * cols + c]);
        if (formats[r * cols + c] != o42_fmt_table_default (sheet->formats))
          {
            cell = sheet_ensure (sheet, to_row + r, to_col + c);
            cell->fmt = formats[r * cols + c];
            record_cell_format (sheet, to_row + r, to_col + c, cell->fmt);
          }
        g_free (inputs[r * cols + c]);
      }

  /* Formulas that pointed into the block follow it, here and on the
   * other sheets. */
  {
    GHashTableIter iter;
    gpointer key_ptr, value;
    GArray *touched = g_array_new (FALSE, FALSE, sizeof (guint64));

    g_hash_table_iter_init (&iter, sheet->cells);
    while (g_hash_table_iter_next (&iter, &key_ptr, &value))
      {
        O42Cell *cell = value;
        guint64 key = *(guint64 *) key_ptr;

        if (cell->ast == NULL)
          continue;
        if (o42_node_move_refs (cell->ast, &src, drow, dcol, sheet->name, sheet->name))
          g_array_append_val (touched, key);
      }
    for (guint i = 0; i < touched->len; i++)
      {
        guint64 key = g_array_index (touched, guint64, i);
        O42Cell *cell = sheet_find_key (sheet, key);
        char *text;

        if (cell == NULL || cell->ast == NULL)
          continue;
        text = o42_node_to_string (cell->ast);
        op_capture (sheet, o42_key_row (key), o42_key_col (key));
        {
          char *formula = g_strconcat ("=", text, NULL);
          set_input_internal (sheet, o42_key_row (key), o42_key_col (key), formula);
          g_free (formula);
        }
        g_free (text);
      }
    g_array_unref (touched);
  }
  if (sheet->book != NULL)
    for (int i = 0; i < o42_book_n_sheets (sheet->book); i++)
      {
        O42Sheet *other = o42_book_sheet (sheet->book, i);
        GHashTableIter it;
        gpointer k, v;
        GArray *touched;

        if (other == sheet)
          continue;
        touched = g_array_new (FALSE, FALSE, sizeof (guint64));
        g_hash_table_iter_init (&it, other->cells);
        while (g_hash_table_iter_next (&it, &k, &v))
          {
            O42Cell *cell = v;
            if (cell->ast != NULL &&
                o42_node_move_refs (cell->ast, &src, drow, dcol, other->name, sheet->name))
              g_array_append_val (touched, *(guint64 *) k);
          }
        for (guint j = 0; j < touched->len; j++)
          {
            guint64 key = g_array_index (touched, guint64, j);
            O42Cell *cell = sheet_find_key (other, key);
            char *text, *formula;

            if (cell == NULL || cell->ast == NULL)
              continue;
            text = o42_node_to_string (cell->ast);
            formula = g_strconcat ("=", text, NULL);
            op_capture (other, o42_key_row (key), o42_key_col (key));
            set_input_internal (other, o42_key_row (key), o42_key_col (key), formula);
            g_free (formula);
            g_free (text);
          }
        g_array_unref (touched);
      }
  op_end (sheet);

  g_free (inputs);
  g_free (formats);
  sheet->modified = TRUE;
}

/* ---------------------------------------------------------------------- */
/* Chart sheets                                                            */
/* ---------------------------------------------------------------------- */

void
o42_sheet_set_chart_sheet (O42Sheet *sheet, gboolean chart_sheet)
{
  g_return_if_fail (sheet != NULL);
  sheet->chart_sheet = chart_sheet;
  sheet->modified = TRUE;
}

gboolean
o42_sheet_is_chart_sheet (O42Sheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  return sheet->chart_sheet;
}

O42Chart *
o42_sheet_the_chart (O42Sheet *sheet)
{
  GPtrArray *charts;

  g_return_val_if_fail (sheet != NULL, NULL);
  charts = o42_sheet_charts (sheet);
  return charts->len > 0 ? g_ptr_array_index (charts, 0) : NULL;
}

/* ---------------------------------------------------------------------- */
/* Rich text                                                               */
/* ---------------------------------------------------------------------- */

void
o42_sheet_set_runs (O42Sheet *sheet, int row, int col,
                    const O42TextRun *runs, int n_runs)
{
  O42Cell *cell;

  g_return_if_fail (sheet != NULL);
  if (row < 0 || col < 0 || row >= O42_MAX_ROWS || col >= O42_MAX_COLS)
    return;

  cell = sheet_find (sheet, row, col);
  if (cell == NULL && (runs == NULL || n_runs <= 0))
    return;
  cell = sheet_ensure (sheet, row, col);
  g_clear_pointer (&cell->runs, g_array_unref);
  if (runs != NULL && n_runs > 0)
    {
      cell->runs = g_array_sized_new (FALSE, FALSE, sizeof (O42TextRun), n_runs);
      g_array_append_vals (cell->runs, runs, n_runs);
    }
  sheet_prune (sheet, row, col);
  sheet->modified = TRUE;
}

const O42TextRun *
o42_sheet_runs (O42Sheet *sheet, int row, int col, int *n_runs)
{
  O42Cell *cell;

  if (n_runs != NULL)
    *n_runs = 0;
  g_return_val_if_fail (sheet != NULL, NULL);
  cell = sheet_find (sheet, row, col);
  if (cell == NULL || cell->runs == NULL || cell->runs->len == 0)
    return NULL;
  if (n_runs != NULL)
    *n_runs = (int) cell->runs->len;
  return &g_array_index (cell->runs, O42TextRun, 0);
}

/* One pass over every formula on the sheet, answering with how far the
 * furthest of them moved. */
static double
recalculate_once (O42Sheet *sheet)
{
  GHashTableIter iter;
  gpointer key_ptr;
  double moved = 0;
  GArray *keys = g_array_new (FALSE, FALSE, sizeof (guint64));

  g_hash_table_iter_init (&iter, sheet->formulas);
  while (g_hash_table_iter_next (&iter, &key_ptr, NULL))
    {
      guint64 key = *(guint64 *) key_ptr;

      g_array_append_val (keys, key);
    }

  for (guint i = 0; i < keys->len; i++)
    {
      guint64 key = g_array_index (keys, guint64, i);
      O42Cell *cell = sheet_find_key (sheet, key);
      double before = 0;
      gboolean had_number;

      if (cell == NULL)
        continue;
      had_number = cell->value.type == O42_VALUE_NUMBER;
      before = had_number ? cell->value.as.number : 0;
      cell->dirty = 1;
      sheet_evaluate (sheet, key, cell);
      cell = sheet_find_key (sheet, key);
      if (cell != NULL && had_number && cell->value.type == O42_VALUE_NUMBER)
        moved = MAX (moved, fabs (cell->value.as.number - before));
      else if (cell != NULL && (cell->value.type == O42_VALUE_NUMBER) != had_number)
        moved = MAX (moved, 1);   /* it changed kind, which is a move */
    }
  g_array_unref (keys);
  return moved;
}

void
o42_sheet_recalculate (O42Sheet *sheet)
{
  int max = 100;
  double tolerance = 0.001;
  gboolean iterate;

  g_return_if_fail (sheet != NULL);
  if (sheet->recalculating)
    return;
  sheet->recalculating = TRUE;
  sheet->cycle_seen = FALSE;

  recalculate_once (sheet);

  /* A formula that depends on itself is worked out again and again
   * until it settles, which is what iteration is for. */
  iterate = o42_book_iteration (sheet->book, &max, &tolerance);
  if (iterate && sheet->cycle_seen)
    for (int pass = 1; pass < max; pass++)
      if (recalculate_once (sheet) <= tolerance)
        break;

  sheet->recalculating = FALSE;
}

void
o42_sheet_stale_formulas (O42Sheet *sheet)
{
  GHashTableIter iter;
  gpointer key_ptr;

  g_return_if_fail (sheet != NULL);
  g_hash_table_iter_init (&iter, sheet->formulas);
  while (g_hash_table_iter_next (&iter, &key_ptr, NULL))
    {
      O42Cell *cell = sheet_find_key (sheet, *(guint64 *) key_ptr);

      if (cell != NULL)
        cell->dirty = 1;
    }
}

/* ---------------------------------------------------------------------- */
/* Grouped objects                                                         */
/* ---------------------------------------------------------------------- */

/* Whether an object anchored at this cell is inside the range. */
static gboolean
anchored_in (const O42Range *range, int row, int col)
{
  return row >= range->row0 && row <= range->row1 &&
         col >= range->col0 && col <= range->col1;
}

guint
o42_sheet_group_objects (O42Sheet *sheet, const O42Range *range)
{
  O42Range r;
  guint highest = 0;
  guint group;
  int members = 0;

  g_return_val_if_fail (sheet != NULL && range != NULL, 0);
  r = o42_range_normalise (range->row0, range->col0, range->row1, range->col1);

  for (guint i = 0; i < sheet->pictures->len; i++)
    highest = MAX (highest, ((O42Picture *) g_ptr_array_index (sheet->pictures, i))->group);
  for (guint i = 0; i < sheet->shapes->len; i++)
    highest = MAX (highest, ((O42Shape *) g_ptr_array_index (sheet->shapes, i))->group);
  for (guint i = 0; i < sheet->charts->len; i++)
    highest = MAX (highest, ((O42Chart *) g_ptr_array_index (sheet->charts, i))->group);
  group = highest + 1;

  for (guint i = 0; i < sheet->pictures->len; i++)
    {
      O42Picture *p = g_ptr_array_index (sheet->pictures, i);

      if (anchored_in (&r, p->row, p->col)) { p->group = group; members++; }
    }
  for (guint i = 0; i < sheet->shapes->len; i++)
    {
      O42Shape *shape = g_ptr_array_index (sheet->shapes, i);

      if (anchored_in (&r, shape->row, shape->col)) { shape->group = group; members++; }
    }
  for (guint i = 0; i < sheet->charts->len; i++)
    {
      O42Chart *chart = g_ptr_array_index (sheet->charts, i);

      if (anchored_in (&r, chart->row, chart->col)) { chart->group = group; members++; }
    }

  if (members < 2)
    {
      /* One object is not a group. */
      o42_sheet_ungroup_objects (sheet, &r);
      return 0;
    }
  sheet->modified = TRUE;
  return group;
}

gboolean
o42_sheet_ungroup_objects (O42Sheet *sheet, const O42Range *range)
{
  O42Range r;
  gboolean any = FALSE;

  g_return_val_if_fail (sheet != NULL && range != NULL, FALSE);
  r = o42_range_normalise (range->row0, range->col0, range->row1, range->col1);

  for (guint i = 0; i < sheet->pictures->len; i++)
    {
      O42Picture *p = g_ptr_array_index (sheet->pictures, i);

      if (anchored_in (&r, p->row, p->col) && p->group != 0) { p->group = 0; any = TRUE; }
    }
  for (guint i = 0; i < sheet->shapes->len; i++)
    {
      O42Shape *shape = g_ptr_array_index (sheet->shapes, i);

      if (anchored_in (&r, shape->row, shape->col) && shape->group != 0)
        { shape->group = 0; any = TRUE; }
    }
  for (guint i = 0; i < sheet->charts->len; i++)
    {
      O42Chart *chart = g_ptr_array_index (sheet->charts, i);

      if (anchored_in (&r, chart->row, chart->col) && chart->group != 0)
        { chart->group = 0; any = TRUE; }
    }
  if (any)
    sheet->modified = TRUE;
  return any;
}

guint
o42_sheet_object_group (O42Sheet *sheet, guint id)
{
  O42Picture *picture;
  O42Shape *shape;
  O42Chart *chart;

  g_return_val_if_fail (sheet != NULL, 0);
  if ((picture = o42_sheet_find_picture (sheet, id)) != NULL)
    return picture->group;
  if ((shape = o42_sheet_find_shape (sheet, id)) != NULL)
    return shape->group;
  if ((chart = o42_sheet_find_chart (sheet, id)) != NULL)
    return chart->group;
  return 0;
}
