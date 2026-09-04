<img src="docs/images/logo.svg" alt="Office42 Spreadsheet" width="520">

# Office42 Spreadsheet

**Office42 Spreadsheet** (the binary is `office42`, and that is one word:
Office42) is a spreadsheet in the shape of Excel 5 and at parity with
Gnumeric, written from scratch in C on GTK 4, Pango and Cairo. It is the
sister project of [word42](https://github.com/office-42/word42), built on
the same principles: a small, honest codebase that does one thing well.
The site is [office42.net](https://office42.net/).

[![Linux](https://github.com/office-42/office42/actions/workflows/linux.yml/badge.svg)](https://github.com/office-42/office42/actions/workflows/linux.yml)
[![macOS](https://github.com/office-42/office42/actions/workflows/macos.yml/badge.svg)](https://github.com/office-42/office42/actions/workflows/macos.yml)
[![Windows](https://github.com/office-42/office42/actions/workflows/windows.yml/badge.svg)](https://github.com/office-42/office42/actions/workflows/windows.yml)

![Office42 Spreadsheet showing a regional sales sheet: a title set in two fonts in one cell, a styled heading row, currency and percent formats, a total row with SUM and INDEX/MATCH formulas, conditional formats on the growth column, a note, a text box, frozen panes, a three-dimensional column chart, a pie chart with labels, and three sheet tabs](docs/images/screenshot.png)

[docs/GUIDE.md](docs/GUIDE.md) is the user guide: every
menu, every dialog, the number-format codes, the keyboard and the terminal
front-end. [docs/PARITY.md](docs/PARITY.md) is a review of the code and an
honest estimate of how far it is from Excel and from Gnumeric.

## Goals

office42 aims at two programs:

- **[Gnumeric](https://en.wikipedia.org/wiki/Gnumeric)**, for what a free
  spreadsheet should be able to do and for how to do it right. Gnumeric was
  built by people who cared about numerical correctness more than about
  features, and that is the standard to hold to.
- **Microsoft Excel 5** (1993), for what it should look like and how it
  should behave: a grid, a formula bar, a name box, two toolbars and sheet
  tabs along the bottom. No ribbon.

## What it does

**The window.** A grid of 1,048,576 rows by 16,384 columns, with Excel's
keys: Enter steps down, Tab steps right and Tab-Tab-Enter returns to the
column the row began in. The formula bar is the same edit as the cell, with
every reference in its own colour; two letters into a function name the
matching functions are listed, and inside a call the signature is shown
with the argument you are on in bold. Point mode writes a reference by
clicking a cell, and F4 cycles its dollars. The fill handle continues a
series, dragging a selection moves it and every formula that pointed at it,
and AutoComplete offers the text already in the column. Zoom, freeze panes,
split panes, custom views, outline groups, a second window on the same
book, and undo of everything, deleting a sheet included.

**Formatting.** Excel 5's tabbed Format Cells dialog: number formats in
Excel's own code language (`#,##0.00;[Red](#,##0.00)`, `dddd d mmmm yyyy`,
`[h]:mm`), fonts, rich text with several fonts in one cell, borders of
every style and colour, pattern fills, indent, rotation, merged cells,
named cell styles that restyle every cell wearing them, and conditional
formats. Formats are interned, so a thousand bold cells share one record.

**Formulas.** A parser with Excel's precedence, `A1` and `$A$1`
references, ranges, whole rows and columns, array constants, 3-D references
across sheets, structured references into tables, names, `LET` and
`LAMBDA`. Array formulas with Ctrl+Shift+Enter and dynamic arrays that
spill. Recalculation is demand-driven with dirty flags, and circular
references are reported rather than followed. **610 functions**: every one
Excel 2003 has, the statistics computed in two passes so they are accurate,
the distributions on regularised incomplete gamma and beta functions, the
whole bond arithmetic on the five day-count bases, the `.INTL` dates, the
text-splitting and array-cutting functions of Excel 365, and Gnumeric's own
besides: the complex functions Excel leaves out, twenty-eight random
distributions, number theory, `FOURIER`, `HPFILTER`, `INTERPOLATION`,
`PERIODOGRAM`, the movable feasts and Black and Scholes with its Greeks.

**Charts and objects.** Column, bar, line, area, pie, doughnut, radar,
bubble, stock, surface, box and whiskers, histogram, polar, contour and XY
scatter charts, in two or three dimensions, with trendlines, error bars, a
second value axis and chart sheets, redrawn from the cells every time they
are painted. Pictures, shapes, text boxes, grouped objects, notes,
hyperlinks and the nine form controls of Excel's Forms toolbar, each bound
to a cell.

**Data.** Sort, Find and Replace, AutoFilter and Advanced Filter, Text to
Columns, Remove Duplicates, Subtotals, Consolidate, Scenarios, Data
Validation, Tables, Pivot Tables with calculated fields and page filters,
Goal Seek, a Solver and the statistical analysis tools: descriptive
statistics, correlation, covariance, regression, histogram, ANOVA, rank
and percentile, moving average. Each is one undo step.

**Files.** Gnumeric's `.gnumeric`, which carries everything; Excel's
`.xlsx` and `.xls` (BIFF8 out, BIFF5 and BIFF8 in), with formulas, styles,
charts, shapes, tables, names and notes, opening in Excel, LibreOffice and
Gnumeric; OpenDocument `.ods`, checked against LibreOffice both ways; and
HTML, CSV, DIF, SYLK, LaTeX and Lotus 1-2-3. The zip and OLE2 containers
are a few hundred lines each, so no library was added. Printing, print
preview, page setup with headers and footers, and export and import of PDF.

**Automation.** Python instead of Visual Basic: a console with `book` and
`sheet` bound, scripts kept in the file, a macro recorder that writes
Python, `=PY("expression")` in a cell, and functions defined in Python that
appear in the Function Wizard. A book that arrives with scripts offers to
run them and never runs them by itself. A SQLite database can sit beside
the book or inside it, queried from Data ▸ Get Data or from a cell with
`=SQLVALUE(...)`. See [docs/PYTHON.md](docs/PYTHON.md) and
[docs/DATABASE.md](docs/DATABASE.md).

**Spelling** with Hunspell, and **the terminal.** `office42-calc` drives the
whole engine from stdin, which is how it is checked:

```
$ ./builddir/src/office42-calc
A1 = 10
A2 = 20
B1 = =SUM(A1:A2)*2
B1
B1      60      (=SUM(A1:A2)*2)
dump
      A             B
1     10            60
2     20
```

Not planned: the ribbon, Visual Basic, co-authoring, Power Query and the
Quattro Pro and Applix formats. [docs/ROADMAP.md](docs/ROADMAP.md) says
why.

## Building

office42 needs a C11 compiler, Meson, Ninja, and GTK 4.10 or newer with
Pango, Cairo and gdk-pixbuf. Importing PDF needs poppler-glib; without it
office42 still builds and still exports PDF. Python (`python3-embed`),
spelling (Hunspell) and the database (SQLite) are used if they are there
and can be turned off with `-Dpython=disabled`, `-Dspell=disabled` and
`-Dsqlite=disabled`. The toolbar icons are SVG, so gdk-pixbuf wants its SVG
loader at run time (`librsvg2-common` on Debian and Ubuntu).

```sh
meson setup builddir
meson compile -C builddir
./builddir/src/office42            # the window
./builddir/src/office42-calc       # the engine, from a terminal
```

Per-platform dependency lists are the same as word42's; see its
[docs/BUILD.md](https://github.com/office-42/word42/blob/main/docs/BUILD.md).
On Windows, `build-aux/bundle-windows.sh` gathers a tree that runs
without MSYS2 and `build-aux/pack-msix.sh` makes the Microsoft Store
package from it; [docs/WINDOWS-STORE.md](docs/WINDOWS-STORE.md) has the
procedure.

`office42 --screenshot out.png book.gnumeric` renders the window to a PNG
without a compositor or a person at the keyboard; `--activate ACTION`,
`--select B4`, `--type '=SU'` and `--keys down,tab,enter` open a dialog,
move the selection, type and press keys first. It is how the pictures here
are made and how a change to the grid is checked from a script.

## How it works

The sheet is sparse: cells live in a hash table keyed by a packed row and
column, and an empty cell costs nothing. Formulas are parsed once and kept
as trees. Each formula cell records the rectangles it reads; when a cell
changes the sheet marks stale every formula whose rectangles contain it,
and theirs in turn, and nothing is recomputed until something asks. The
evaluator does not know what a sheet is: it reads values through a
callback, which is why the engine builds as a library of its own and is
driven from a terminal. Cell formats are interned, as word42's character
formats are. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) goes into detail.

```
src/util/      cell addresses, values, number formats, dates
src/model/     the sheet, the book, charts, shapes, undo
src/formula/   the parser, the evaluator, the function library
src/io/        .gnumeric, .xlsx, .xls, .ods, CSV and the rest, PDF, SQLite
src/script/    Python: the office42 module, the interpreter
src/ui/        the grid, the window, the dialogs
src/main.c        the window's entry point
src/calc-main.c   the terminal front-end
data/          menus, icons, stylesheet
docs/          the guide, the architecture, the parity review
```

The model and the formula engine never include GTK.

## Prior art

office42 studies three projects and copies none of their code:
[Gnumeric](https://en.wikipedia.org/wiki/Gnumeric) for what a spreadsheet
engine should get right; [AbiWord](https://github.com/AbiWord/abiword), by
way of word42, for interned formatting and symmetric undo records; and
[northstar-browser](https://github.com/nordstjernen-web/northstar-browser)
for how to ship a large C/GTK 4 program with Meson and CI on three
platforms.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).

office42 is an independent program. It is not affiliated with, endorsed by
or derived from Microsoft, the GNOME Foundation or The Document Foundation,
and no code, artwork or resource of theirs is in it. Microsoft, Excel and
Office Open XML are trademarks of Microsoft Corporation; Gnumeric and GTK
belong to the GNOME Foundation; the other names used here belong to their
owners, and are used only to say what office42 reads, writes or resembles.
[NOTICE.md](NOTICE.md) lists them and gives the terms of every library
office42 links against.
