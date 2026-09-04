# Python in Office42 Spreadsheet

Office42 Spreadsheet embeds Python the way Gnumeric and LibreOffice do
and Excel 365 does with `=PY()`: one interpreter lives inside the
program, an `office42` module gives it the book, and three doors lead
to it.

- **Tools > Python Console…** — a console under the grid. `book` and
  `sheet` are already bound; each line runs at once, its output and
  any traceback appear above, Up and Down walk the history, and the
  variables live on until Reset.
- **Tools > Run Python Script…** — runs a `.py` file against the open
  book. What it prints goes to the console if one is open, otherwise
  into a message.
- **`=PY("expression")`** in a cell, as in Excel: the expression's
  value is the cell's value, with `sheet` and `book` in scope.
- **Functions from scripts**: a Python function decorated with
  `@office42.function` becomes a spreadsheet function; `=NAME(...)` in
  any cell calls it, and it is listed in Insert > Function.

Everything a script does to the book is one step of undo.

The terminal front-end has the same: `py CODE` runs a line and
`pyfile PATH` a script, in `office42-calc`.

## The office42 module

```python
import office42
book  = office42.book          # the open book
sheet = office42.sheet         # the sheet on show

sheet["A1"].value = 42         # a number, text, bool, None (empty) or office42.Error
sheet["A2"].formula = "=A1*2"  # what one would type
sheet["A2"].value              # 84.0 — formulas are evaluated on demand
sheet["A2"].text               # "84" — as shown, with the cell's format
sheet["A1:B3"].values = [[1, 2], [3, 4], [5, 6]]
sheet["A1:B3"].values          # a list of rows
sum(sheet["A1:A3"])            # ranges iterate over their values
sheet[0, 0]                    # (row, col), 0-based, is A1
sheet["A1:B1"].format(bold=True, fill="#ffff99", number="fixed", decimals=2)
sheet["A1"].style              # a dict of the format
sheet.used_range               # the rectangle with anything in it, or None
sheet.name = "Sales"

book.names                     # ['Sales']
book["Sales"], book[0]         # sheets by name or index
book.add_sheet("Data")
book.remove_sheet("Data")

office42.evaluate("=AVERAGE(A1:B3)")   # a formula without a cell
office42.functions()           # every function name, built in or from scripts
office42.errors()              # tracebacks from functions called by cells
```

A `Range` also has `rows`, `cols`, `address`, `cell(i, j)`, `row(i)`,
`column(j)`, `offset(rows, cols)`, `resize(rows, cols)` and `clear()`.

Format properties: `bold`, `italic`, `underline`, `strikeout`, `wrap`,
`borders`, `size` (points), `family`, `colour` (an int `0xRRGGBB` or
`"#rrggbb"`), `fill` (the same, or `None`), `halign` (`general`,
`left`, `centre`, `right`), `valign` (`bottom`, `middle`, `top`),
`number` (`general`, `fixed`, `comma`, `currency`, `percent`,
`scientific`, `text`, `date`, `time`, `datetime`, or a format code such
as `"#,##0.00"`) and `decimals`.

## Functions from scripts

```python
import office42

@office42.function
def NPV2(rate, flows):
    """Net present value, the flows starting now rather than in a year."""
    return sum(f / (1 + rate) ** i for i, row in enumerate(flows) for f in row)
```

Then `=NPV2(0.1, A1:A5)` works in a cell. A single cell arrives as its
value, a range as a list of rows; the return value becomes the cell's
value — a number, text, bool, `None` for empty, or `office42.Error("#N/A")`.
The function's name is upper-cased; a docstring's first line becomes
its description in Insert > Function. A function that raises shows
`#VALUE!` in the cell and its traceback in `office42.errors()`.
Built-in names cannot be redefined.

Such a function exists while the program runs. A file saved with
`=NPV2(...)` in it keeps the formula and its last value; Excel shows
`#NAME?` for it until the script is run again, as it would for any
add-in function it does not have.

## `=PY()`

`=PY("sheet['A1'].value * 2")` evaluates the expression in the
console's namespace, so functions and variables defined there are in
scope. It is recomputed on every recalculation, since the evaluator
cannot see which cells the expression reads. Excel's second argument
(the return type) is accepted and ignored.

## Scripts in the file

Tools > Scripts in this Book… keeps scripts inside the book, as Excel
keeps macros: a list of names, an editor, Save, Run and Delete. They
are written to `.gnumeric` (a `gnm:o42-Scripts` element Gnumeric
passes over) and `.xlsx` (a part `xl/o42/scripts.xml` Excel and
LibreOffice pass over); `.xls` does not carry them.

A book that arrives with Python in it never runs any of it on opening:
not its scripts, and not `=PY()` in its cells, which show `#NAME?`
until you say so. A bar under the formula bar says the Python is
there, with Run Scripts (all of them, in order, and the `=PY()` cells
worked out), Scripts… and Hide — the counterpart of Excel's "Enable
content". Running anything from the console or Tools ▸ Run Python
Script against the book says so too. From Python, `book.scripts`, `book.script(name)`,
`book.set_script(name, code)`, `book.remove_script(name)` and
`book.run_script(name)`; in `office42-calc`, `scripts`,
`script NAME PATH`, `runscript NAME` and `delscript NAME`.

## Building with Python

Python is a build option: `meson setup builddir -Dpython=enabled`
(the default `auto` takes it when `python3-embed` is found). Without
it the Tools items are greyed out and `=PY()` is `#NAME?`. On Windows
the interpreter finds its standard library beside its DLL; on Linux
the system `libpython` is used, and whatever is installed for it
(numpy, pandas) imports as usual.

The layer is `src/script/`: `o42-python.c` is the C half of the
module and `office42.py` the Python half, compiled into the executable
as a string by `embed.py`. It sees the model and the evaluator, never
GTK.

## Not there

VBA; sandboxing — a script can do what the user can do, so run only
scripts you trust.
