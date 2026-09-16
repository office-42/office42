# Python in Office42 Spreadsheet

Office42 Spreadsheet embeds Python the way Gnumeric and LibreOffice do
and Excel 365 does with `=PY()`: one interpreter lives inside the
program, an `office42` module gives it the book, and three doors lead
to it.

- **Tools > Macro > Python Console…** — a console under the grid.
  `book` and `sheet` are already bound; each line runs at once, its
  output and any traceback appear above, Up and Down walk the history,
  and the variables live on until Reset.
- **Tools > Macro > Run Python Script File…** — runs a `.py` file
  against the open book. What it prints goes to the console if one is
  open, otherwise into a message.
- **Tools > Macro > Macros… (Alt+F8)** — the book's macros and the
  personal scripts: Run, Edit, Delete, and Options for a shortcut key
  and a description. **Record Macro** writes what you do as Python;
  see below.
- **`=PY("expression")`** in a cell, as in Excel: the expression's
  value is the cell's value, with `sheet` and `book` in scope.
- **Functions from scripts**: a Python function decorated with
  `@office42.function` becomes a spreadsheet function; `=NAME(...)` in
  any cell calls it, and it is listed in Insert > Function.

Everything a script does to the book is one step of undo.

The terminal front-end has the same: `py CODE` runs a line and
`pyfile PATH` a script, in `office42-calc`; `office42 --py CODE` runs
a line in the window and prints what it printed.

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
book.move_sheet("Data", 0)     # to the first tab
book.copy_sheet("Data", name="Data (2)")   # Edit > Move or Copy Sheet, with a copy
book.properties["title"]; book.set_property("author", "Ann")   # File > Properties
book.path                      # the file, or None
book.save(); book.save_as("q3.xlsx"); book.close()

office42.evaluate("=AVERAGE(A1:B3)")   # a formula without a cell
office42.functions()           # every function name, built in or from scripts
office42.errors()              # tracebacks from functions called by cells
office42.open("other.xlsx")    # in a window of its own
```

A `Range` also has `rows`, `cols`, `address`, `cell(i, j)`, `row(i)`,
`column(j)`, `offset(rows, cols)`, `resize(rows, cols)`, `clear()`
and `clear_formats()`, and the verbs of the Edit and Data menus:

```python
r = sheet["A1:C9"]
r.copy("E1")                         # everything; or .copy("E1", "values"),
                                     # "formats", "formulas", transpose=True
r.cut("E1")                          # formulas elsewhere follow it
r.fill_down(); r.fill_right(); r.fill_up(); r.fill_left()
r.fill_series("linear", step=2, stop=20)   # or "growth", "date" (unit="month"), "autofill"
r.justify()                          # the text in the first column, laid out again
sheet["A1:A2"].autofill("A1:A20")    # continues the series
r.create_names(top=True)             # names from the labels along the edges
r.apply_names(); sheet.apply_names() # named rectangles written as their names
r.sort(keys=[1], ascending=False, header=True)   # keys from the range's left
r.replace("old", "new")              # how many cells changed
r.find("total")                      # the first cell holding it, or None
r.remove_duplicates(cols=[0], header=True)
r.autofilter(); sheet.autofilter_choose(0, "pear"); sheet.clear_autofilter()
r.merge(); r.unmerge(); r.merged
r.insert_cells("down"); r.delete_cells("left")
sheet.insert_rows(2, 3); sheet.delete_cols(0)  # 0-based, then a count
sheet.row_height(0, 40); sheet.col_width(0)    # pixels; get or set
sheet.standard_width = 96                      # Format > Column > Standard Width
sheet.hide_rows(4, 6); sheet.unhide_cols(2)
sheet.freeze(1, 0); sheet.frozen
```

The window is there too, the way Excel's `Selection` and `MsgBox`
are: `office42.selection` and `office42.active_cell` are Ranges,
`r.select()` and `r.activate()` move them, `office42.msgbox(text)`
and `office42.inputbox(prompt)` wait for the user, and
`office42.app` is the Application, with `calculate()`, `status`
(the status bar's text) and `selection`. In `office42-calc` the
selection is what `select A1:B2` last said, a message is printed,
and a question is answered by the next line of input.

Format properties: `bold`, `italic`, `underline`, `strikeout`, `wrap`,
`borders` (`True`, `False` or a style: `thin`, `medium`, `thick`,
`double`, `dashed`, `dotted`) with `border_colour`, or one side at a
time as `border_top` … `border_right` and `border_top_colour` …,
`size` (points), `family`, `colour` (an int `0xRRGGBB` or
`"#rrggbb"`), `fill` (the same, or `None`), `pattern` (Excel's names:
`solid`, `mediumGray`, `darkHorizontal` …, or `None`) with
`pattern_colour`, `halign` (`general`, `left`, `centre`, `right`),
`valign` (`bottom`, `middle`, `top`), `number` (`general`, `fixed`,
`comma`, `currency`, `percent`, `scientific`, `text`, `date`, `time`,
`datetime`, or a format code such as `"#,##0.00"`), `decimals`, and
`locked` and `hidden` for a protected sheet.  Each of these is also a
property of a Range -- `r.bold = True`, `r.number_format = "0.00%"`,
`r.fill = None` -- read from the top-left cell and set over the whole
range; so are `r.note`, `r.hyperlink` (deletable with `del`),
`r.row_height`, `r.column_width` and `r.hidden`, and `r.autofit()`
widens the columns to their text.

Rules on cells: `r.validate("whole", "between", 1, 10, "One to ten")`
(kinds `whole`, `decimal`, `list`, `date`, `time`, `length`; `any`
clears), `r.validation` and `r.clear_validation()`;
`r.add_conditional_format(">", 100, bold=True, fill="#FFCC00")`,
`r.conditional_formats` and `r.clear_conditional_formats()`.

### Charts, shapes and pictures

The objects floating over a sheet are Python objects too, reading and
writing the sheet as they go:

```python
c = sheet.add_chart("pie", "A1:B5", "D2", width=320, height=220, title="Share")
c.kind = "column"; c.legend = False; c.data = sheet["A1:C5"]
s = sheet.add_shape("star5", "F2", 120, 90, text="New!", fill="#FFCC00",
                    line="#000000", line_width=2, rotation=15)
s.dash = "dash"; s.flip_h = True; s.send_to_back()
p = sheet.add_picture("logo.png", "H2", width=200)  # the height follows
p.crop = (0.1, 0, 0, 0); p.rotation = 90
sheet.charts, sheet.shapes, sheet.pictures, sheet.objects   # back to front
c.position, c.size, c.delete()
```

A Shape's `kind` is `rectangle`, `oval`, `line`, `arrow`, `textbox` or
a control (`button`, `checkbox`, `option`, `label`, `spinner`,
`scrollbar`, `listbox`, `groupbox`, `combo`), its `geom` one of the
AutoShape outlines (`roundRect`, `triangle`, `diamond`, `star5`,
`rightArrow`, `wedgeRectCallout` ...); a control has `link`, `source`
and `script`.

### Events

What Excel's `Worksheet_Change` and its kin are.  A script registers a
handler, or simply defines a function by the conventional name, and
the window calls it:

```python
import office42

def on_change(sheet, rng):            # cells the user edited
    rng.offset(0, 1).value = "seen"   # a handler's own changes do not fire it

@office42.on("selection")             # or on_selection_change(sheet, rng)
def moved(sheet, rng):
    office42.app.status = rng.address

def on_before_save(book): ...         # also on_open, on_close
office42.off("selection", moved)
```

Handlers belong to the book whose script registered them, and they
exist once the book's scripts have run (Run Scripts, or a script run
by hand); nothing fires while none is registered.  What a handler
prints goes where a script's output goes.

### Stepping through a script

Tools > Macro > Scripts in this Book runs a script a line at a time:
Step (F8) to the next line, Continue (Shift+F8) to the next breakpoint
(Ctrl+B on a line sets one), Stop to end it.  The line the script
waits on is highlighted, its variables are listed beside the code, and
the grid shows what the script has done so far.

### The application

`office42.app` has `undo()` and `redo()`, `recalculate()`,
`display_alerts` (False keeps `msgbox` quiet), `status`, `selection`,
`active_cell` and `active_sheet`.

### Excel's own macros

An `.xlsm` opens as an `.xlsx` does.  Its Visual Basic project is not
run -- office42 runs Python -- but it is kept as it came, and saving
as `.xlsm` gives it back to Excel untouched; saving as `.xlsx` leaves
it out, as Excel does.  The bar under the toolbar says so.

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

Such a function belongs to the book whose script defined it: another
book's `=NPV2()` is `#NAME?` unless its own scripts, or a personal
script, define one. A file saved with `=NPV2(...)` in it keeps the
formula and its last value; Excel shows `#NAME?` for it until the
script is run again, as it would for any add-in function it does not
have.

### Personal scripts

Every `.py` in your `office42/scripts` folder
(`%LOCALAPPDATA%\office42\scripts` on Windows,
`~/.local/share/office42/scripts` elsewhere) is run when Python
starts, and what it defines with `@office42.function` is there in
every book — Excel's Personal Macro Workbook, as files. They are
yours, so they need no trusting; the Macro dialog lists them after
the book's macros, runs them, and its Folder button opens the folder.
`office42.personal_folder()` names it and `office42.personal_scripts`
says what loaded, with any error.

## `=PY()`

`=PY("sheet['A1'].value * 2")` evaluates the expression in the
console's namespace, so functions and variables defined there are in
scope. It is recomputed on every recalculation, since the evaluator
cannot see which cells the expression reads. Excel's second argument
(the return type) is accepted and ignored.

## Scripts in the file

Tools > Macro > Scripts in this Book… keeps scripts inside the book,
as Excel keeps macros: a list of names, an editor, Save, Run and
Delete. Each may have a shortcut key (Ctrl+Shift and a letter, one
macro to a letter) and a description, set in the Macro dialog's
Options. They are written to `.gnumeric` (a `gnm:o42-Scripts` element
Gnumeric passes over) and `.xlsx` (a part `xl/o42/scripts.xml` Excel
and LibreOffice pass over), key and description with them; `.xls`
does not carry them.

A book that arrives with Python in it never runs any of it on opening:
not its scripts, and not `=PY()` in its cells, which show `#NAME?`
until you say so. A bar under the formula bar says the Python is
there, with Run Scripts, Scripts… and Hide — the counterpart of
Excel's "Enable content". Run Scripts runs the script named
`Auto_Open` if there is one, and otherwise all of them in order, and
works the `=PY()` cells out; from then on a script named `Auto_Close`
runs when the book's last window closes. Running anything from the
console or Tools ▸ Macro ▸ Run Python Script File against the book
says so too. From Python, `book.scripts`, `book.script(name)`,
`book.set_script(name, code, shortcut, description)`,
`book.script_info(name)`, `book.remove_script(name)` and
`book.run_script(name)`; in `office42-calc`, `scripts`,
`script NAME PATH`, `runscript NAME` and `delscript NAME`.

## Recording a macro

Tools > Macro > Record Macro writes what you do as Python until Stop
Recording, and keeps it as Macro1, Macro2 … in the book. What is
written is what the API has one call for: a cell typed into, a
format, an insert of rows, a sort, a paste, a merge, a sheet added or
renamed — one line each, and the selection before a line that acts
on it, as Excel writes `Range("B2").Select`:

```python
# Recorded by office42.
import office42
book = office42.book
sheet = book["Sheet1"]
sheet["B2:C5"].select()
sheet["B2:C5"].sort(keys=[1], ascending=[False], header=True)
sheet["D5"].formula = "=SUM(B2:B4)"
sheet.insert_rows(1, 2)
```

Use Relative References, a check item on the same menu, is Excel's
button of that name: cells are written relative to the active cell —
`office42.active_cell.offset(1, 0)` — and a formula as
`.formula_from("=SUM(B2:B4)", "D5")`, so that its references move
with the macro, and the macro replays wherever it is run. In
`office42-calc`, `record on`, `record on relative`, and
`record off [NAME]`.

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

Running VBA (an `.xlsm`'s project is kept, not run); sandboxing — a script can do what the user can do, so run only
scripts you trust.
