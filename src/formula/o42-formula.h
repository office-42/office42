/* o42-formula.h - parsing a formula into a tree
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A formula is parsed once, when it is entered, and the tree is kept.  That
 * matters more in a spreadsheet than it looks: a cell is re-evaluated every
 * time something it depends on changes, and re-parsing the text each time
 * would make recalculation cost the length of the formula rather than the
 * size of the tree.
 */

#pragma once

#include "o42-value.h"

G_BEGIN_DECLS

typedef enum {
  O42_NODE_NUMBER,
  O42_NODE_STRING,
  O42_NODE_BOOL,
  O42_NODE_ERROR,
  O42_NODE_REF,
  O42_NODE_RANGE,
  O42_NODE_UNARY,
  O42_NODE_BINARY,
  O42_NODE_CALL,
  O42_NODE_NAME,           /* a defined name, resolved when evaluated */
  O42_NODE_EMPTY,          /* an argument left out: IF(A1,,5) */
  O42_NODE_ARRAY,          /* a constant array: {1,2;3,4} */
  O42_NODE_APPLY           /* calling what an expression came to:
                            * LAMBDA(x,x+1)(4), or f(4) with f a LET name */
} O42NodeType;

typedef enum {
  O42_OP_ADD, O42_OP_SUB, O42_OP_MUL, O42_OP_DIV, O42_OP_POW,
  O42_OP_CONCAT,
  O42_OP_EQ, O42_OP_NE, O42_OP_LT, O42_OP_GT, O42_OP_LE, O42_OP_GE,
  O42_OP_NEG, O42_OP_POS, O42_OP_PERCENT
} O42Op;

typedef struct _O42Node O42Node;

/* Which corners of a reference are absolute.  A ref uses the first two; a
 * range uses all four, ROW0/COL0 for its top-left corner. */
enum {
  O42_ABS_ROW0 = 1 << 0,
  O42_ABS_COL0 = 1 << 1,
  O42_ABS_ROW1 = 1 << 2,
  O42_ABS_COL1 = 1 << 3
};

/* A rectangle on a named sheet, or on the formula's own sheet when the
 * name is NULL.  What a formula's precedents are made of. */
typedef struct {
  const char *sheet;       /* interned, or NULL for the formula's own sheet */
  O42Range    range;
} O42SheetRange;

struct _O42Node {
  O42NodeType type;
  guint8      abs;         /* O42_ABS_* flags, for refs and ranges */
  const char *sheet;       /* interned sheet name for a ref or range on
                            * another sheet; NULL for the formula's own */
  const char *sheet_last;  /* the last sheet of a 3-D reference,
                            * Sheet1:Sheet3!A1, spanning the sheets from
                            * `sheet` to this one in tab order; else NULL */
  union {
    double        number;
    char         *string;    /* owned */
    gboolean      boolean;
    O42ErrorCode  error;
    O42Ref        ref;
    O42Range      range;
    struct {
      O42Op    op;
      O42Node *a;
      O42Node *b;            /* NULL for a unary operator */
    } op;
    struct {
      char      *name;       /* owned, upper case */
      GPtrArray *args;       /* O42Node*, owned */
    } call;
    char         *name;      /* owned, as typed, for O42_NODE_NAME */
    struct {
      int        rows;
      int        cols;
      GPtrArray *items;      /* O42Node* constants, row by row; owned */
    } array;
    struct {
      O42Node   *callee;     /* owned */
      GPtrArray *args;       /* O42Node*, owned */
    } apply;
  } as;
};

/* Parses the text after the leading "=".  Never returns NULL: a formula that
 * will not parse becomes an error node, so that a bad formula is a cell
 * showing #NAME? rather than a special case running through the model. */
O42Node *o42_formula_parse (const char *text);

void     o42_node_free (O42Node *node);
O42Node *o42_node_copy (const O42Node *node);

/* Moves the relative references in a tree by a number of rows and columns:
 * what happens to a formula when it is copied to another cell.  A reference
 * that would fall off the sheet becomes #REF!.  Returns TRUE if anything
 * changed. */
gboolean o42_node_relocate (O42Node *node, int drow, int dcol);

/* Adjusts every reference into sheet `target` for rows (or columns)
 * inserted or deleted there at `at`: `count` rows inserted when positive,
 * -`count` rows deleted when negative.  `own` is the name of the sheet the
 * formula lives on, so that an unqualified reference counts as one into
 * it.  A reference to a deleted cell becomes #REF!; a range that straddles
 * the band shrinks.  Returns TRUE if anything changed. */
gboolean o42_node_shift (O42Node *node, gboolean rows, int at, int count,
                         const char *own, const char *target);

/* The same for cells shifted within a band of columns (for a row shift)
 * or rows (for a column shift): only references whose other coordinate
 * lies in `band_lo`..`band_hi` move, and a range that straddles the
 * band's edge is left alone, since there is no rectangle that describes
 * where its cells went. */
/* A block of cells was moved: references wholly inside it follow,
 * whether they are relative or absolute, as Excel's cut and paste
 * moves them.  A range that only half overlaps is left alone, since
 * there is no honest answer.  TRUE if anything changed. */
gboolean o42_node_move_refs (O42Node *node, const O42Range *from, int drow, int dcol,
                             const char *own, const char *target);

gboolean o42_node_shift_within (O42Node *node, gboolean rows, int at, int count,
                                int band_lo, int band_hi,
                                const char *own, const char *target);

/* Points references at a sheet's new name.  Returns TRUE if any did. */
gboolean o42_node_rename_sheet (O42Node *node, const char *old_name,
                                const char *new_name);

/* Every cell and rectangle the tree reads, as O42SheetRange.  This is what
 * the sheet uses to decide which formulas a change has invalidated. */
void     o42_node_collect_refs (const O42Node *node, GArray *ranges);

/* Every defined name the tree uses, appended to `names` as interned
 * upper-case strings.  For finding the formulas a name change touches. */
void     o42_node_collect_names (const O42Node *node, GPtrArray *names);

/* A sheet name as a formula writes it: bare, or in single quotes when it
 * holds anything but letters, digits and underscores.  Caller frees. */
char    *o42_sheet_name_quote (const char *name);

/* Rebuilds the source text from the tree.  Used to show a formula in the
 * formula bar with its references in canonical form. */
char    *o42_node_to_string (const O42Node *node);

/* Prefixes every call to a function `is_future` says yes to with
 * `prefix`, in place: how .xlsx spells the newer functions. */
void     o42_node_prefix_functions (O42Node *node, gboolean (*is_future) (const char *),
                                    const char *prefix);

G_END_DECLS
