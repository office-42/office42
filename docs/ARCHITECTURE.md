# office42 architecture

What the layers are, what each is allowed to know about, and why the engine
is built the way it is. Aimed at someone about to change the code.

## The layers

```
    ui/        O42Window -> O42Grid
      |             GTK 4 widgets, actions, input
      v
    io/        the file formats, PDF, and the SQLite database
      |
      v
    model/     O42Sheet -> O42FmtTable, O42Picture
      |             sparse cells, recalculation, formatting, undo, pictures
      v
    formula/   O42Node, o42_eval, the function library
      |             parsing, evaluation, coercion
      v
    util/      O42Ref, O42Range, O42Value, o42_image
                    addresses, values, coercion rules, picture decoding
```

Dependencies point downwards. `formula/` does not know what a sheet is: it
evaluates against a callback. `model/` and `io/` know nothing about GTK — they
use cairo, Pango and gdk-pixbuf, none of which is GTK. Everything that knows
what a mouse is lives in `ui/`.

This is why `libo42core` builds as its own static library and why
`office42-calc` — a terminal front-end — exists. The engine was checked end
to end before there was a window to type into, and it stays checkable that
way.

## Values

A cell holds one of five things: empty, number, text, boolean, or error.
Errors are values, not failures: `#DIV/0!` propagates through arithmetic and
can be tested for, which is why `IFERROR` can exist at all.

The coercion rules are Excel's, and they are less arbitrary than they look:

- An empty cell is 0 in arithmetic and `""` in text.
- `TRUE` is 1.
- Text is a number only if the *whole* of it parses as one. `"12abc"` is text.
- `""` is not 0. `=""+1` is `#VALUE!`. The distinction between a blank cell
  and a cell holding a blank string is real and users depend on it.
- A number sorts before text, which sorts before a boolean. That ordering is
  what makes `=1<"a"` TRUE.
- Text compares case-insensitively.

Values are passed by value and own their text, so every `O42Value` that has
been assigned to must be cleared exactly once. `o42_value_copy` exists for
when you need two.

## Addresses

Rows and columns are zero-based inside the program and one-based-and-lettered
on screen. The only place the two conventions meet is `o42_ref_name` and
`o42_ref_parse`. Column names are bijective base-26 — after Z comes AA — and
the `- 1` on each round of `o42_col_name` is what makes it bijective rather
than ordinary base-26 with a silent zero digit.

Cells are keyed by one 64-bit integer, row in the high word, so the sparse
store is an ordinary `GHashTable` rather than anything clever.

## The parser

Recursive descent over a hand-written lexer. The grammar, from loosest to
tightest binding:

```
comparison    =  <>  <  >  <=  >=
concat        &
additive      +  -
multiplicative *  /
power         ^            (right-associative: 2^3^2 is 512)
unary         -  +         (binds tighter than ^: -2^2 is 4)
postfix       %            (divides by 100)
primary       number, string, TRUE/FALSE, error literal, (expr), ref, range, call
```

Two things there surprise people and are both Excel's own behaviour: unary
minus binding tighter than exponentiation, and the right associativity of
`^`. Both are called out in comments at the point they are decided.

A name followed by `(` is a call, whatever else it might have been — that is
what keeps `LOG10(` from being read as a cell reference. A name that parses
entirely as a reference is one; anything else is `#NAME?`.

A formula that will not parse becomes an error node rather than a NULL. A bad
formula is a cell showing `#NAME?`, not a special case running through the
model.

## The evaluator

`o42_eval` is a pure function of a tree and an `O42EvalContext`. The context
is one callback: give me the value of row, col. The evaluator never sees a
sheet.

### Operands

Arguments travel as `O42Operand`, which is either a value or a rectangle.
Keeping the distinction is what lets `SUM(A1:A9)` see nine cells while
`SUM(A1)` sees one, without the caller saying which it meant. A bare
reference stays a one-by-one range, so `ROWS(A1)` and `COUNTA(A1)` see a range
rather than a scalar.

A range used where a single value is wanted collapses to that value if it is
one-by-one, and is `#VALUE!` otherwise. Excel would try an implicit
intersection against the calling cell's row or column, which is a cleverness
that mostly confuses people.

### Literals versus ranges

`SUM("x")` is `#VALUE!`. `SUM(A1:A3)` where A2 holds `"x"` is the sum of A1
and A3. A text *literal* passed to an arithmetic function is an error; a text
*cell* inside a range is skipped. A spreadsheet full of labels would be
unusable otherwise, and this is Excel's rule too. `visit_numbers` is the one
place that distinction is implemented.

### The function table

Sorted by name, searched by binary search, case-insensitive. Each entry says
its minimum and maximum argument count, so arity errors are caught before the
implementation runs. To add a function: write `fn_whatever`, add a row to
`FUNCTIONS` *in sorted position*.

Rounding is half-away-from-zero (`ROUND(2.5)` is 3), not C's half-to-even.
`MOD` takes the sign of the divisor (`MOD(-1,3)` is 2), not C's `fmod`.
`VLOOKUP` is exact-match only; Excel's approximate mode wants a sorted table
and returns nonsense when it does not get one.

## The sheet

### Storage

A hash table from packed key to `O42Cell`. A cell holds its last computed
value, the text the user typed (for formulas and forced-text), the parsed
tree, the rectangles the tree reads, a format index, and two flags.

`sheet_prune` drops a cell that holds nothing and looks like nothing, so
clearing a cell leaves no trace.

### Recalculation

Demand-driven. Each cell has a `dirty` flag. `sheet_get_cell_value`
evaluates a dirty formula cell on the way to answering, and the evaluator's
callback lands back in `sheet_get_cell_value` for every reference, so
precedents are evaluated first by construction.

A `visiting` flag catches cycles: a cell asked for while it is being
evaluated yields `#CIRCULAR!`. Excel says `#REF!`; saying so outright is more
use than being compatible about it.

Invalidation is the part worth understanding. When a cell changes,
`sheet_invalidate` walks the set of formula cells and tests each one's
precedent rectangles against the changed address. Every cell that goes from
clean to dirty is queued, and the walk repeats for it. **Recursing only on
cells that have just gone from clean to dirty is what makes this terminate
when the sheet contains a cycle.**

This is O(formulas) per change instead of O(dependants). For the sheets
people actually build that is a few thousand rectangle tests — far too fast
to notice — and it avoids a dependency map whose size a single reference to
a whole column could blow up. If it ever matters, an interval tree over
precedent rectangles is the upgrade, and the interface does not change.

### Undo

A record is a list of snapshots: what a cell's input and format were before
the change. Applying a record restores each snapshot *and captures what was
there into a new record as it goes*, so the entry that comes back undoes what
this one just did. One routine drives undo and redo, and a stack entry flips
direction each time it is used. Same shape as word42's change records.

`o42_sheet_begin_group` / `end_group` collapse everything between them into
one step; clearing a range is one undo, not one per cell.

## Formatting

Interned. `O42FmtTable` compares records by bytes, which is only sound
because every `O42Fmt` is zeroed by `o42_fmt_init_default` before its fields
are set — that defines the padding. **Always build an `O42Fmt` from that
function**, never from an uninitialised stack struct.

Number formatting goes through `g_ascii_formatd`, never the C library's
`printf`, which would write a decimal comma in half the world's locales. A
spreadsheet's `1.5` must be `1.5` everywhere, and its `#,##0` means a comma.

A whole column or row can wear a format of its own. Applying a format to
a range that spans every row goes onto the columns (`col_fmts`, an index
per column), not onto a million cells; a cell with no record of its own
shows its column's format, then its row's, then the sheet's default, and
a cell made in a formatted column starts from that format. `.xlsx` keeps
them as Excel does, `style` on a `<col>` and `s` with `customFormat` on
a `<row>`; `.gnumeric` as a `StyleRegion` over every row of the column,
written before the cells' own so that the cells are made on top of it.

## Pictures

A picture floats above the grid, anchored to a cell with an offset inside
it, so that it moves with the rows and columns around it — the only
arrangement that survives a row being inserted above. `O42Picture` holds the
file's bytes and decodes them to a cairo surface on first draw. The grid
paints pictures after the cells and before the headers, hit-tests them before
cells on a click, and re-anchors one to whatever cell its corner is over when
a drag ends.

## PDF

Export cuts the used range into column bands and row bands that each fit a
landscape Letter page and walks them across then down, Excel's default print
order. Each page is drawn at 96-dpi pixel coordinates under a 0.75 scale.

Import is poppler's text layout: one rectangle per character. Characters
become lines by clustering on their vertical centre; lines become cells where
the gap between characters exceeds one and a half character widths or two
spaces run together; cells become columns by clustering where they start.
Two refinements matter in practice. Adjacent columns that never co-occupy a
line are one column with mixed alignment — a left-aligned heading over
right-aligned numbers — and are folded together. And a gap between lines of
more than one and a half line pitches is an empty row, or several, so a
table with a blank line under its title comes back in the right rows.

## Things to be careful about

- **Clear every `O42Value` you own exactly once.** They own their text.
- **`O42Fmt` must come from `o42_fmt_init_default`.** Interning compares
  padding bytes.
- **Keep `FUNCTIONS` sorted.** The lookup is a binary search.
- **`formula/` must not include `model/`.** The evaluator sees a callback,
  not a sheet. That boundary is the whole design.
