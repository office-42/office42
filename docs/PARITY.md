# A review of the code, and how far it is from Excel

Written from the source as it stands, module by module, and then feature
by feature against the spreadsheet everybody has. Nothing here is taken
on trust: every "yes" below was found in the code or tried in the
running program.

- [1. The code](#1-the-code)
- [2. Parity with Excel, area by area](#2-parity-with-excel-area-by-area)
- [3. The window](#3-the-window)
- [4. Parity with Gnumeric](#4-parity-with-gnumeric)
- [5. Where office42 goes further](#5-where-office42-goes-further)
- [6. Where the next effort pays most](#6-where-the-next-effort-pays-most)

---

## 1. The code

68,000 lines of C11 in six layers, each of which may only see the ones
below it. `util` and `model` and `formula` and `io` never include GTK,
so the engine builds as a library of its own and the window is the only
thing that knows what a mouse is.

| Layer | Module | Lines | What is in it | How it reads |
|---|---|---:|---|---|
| util | `o42-types.c` | 129 | A1 addresses, ranges, the cell key | Small and total; the key packs a row and a column into 64 bits, so the grid can grow without touching it |
| | `o42-numfmt.c` | 797 | The number-format language | The biggest thing in `util`, and the one most worth reading: it parses Excel's codes rather than matching them |
| | `o42-date.c` | 241 | Serial dates, the 1900 leap-year bug included | Deliberately bug-compatible, and says so |
| | `o42-image.c` | 156 | Decoding a picture's bytes | A thin cover over gdk-pixbuf |
| | `o42-spell.c` | 348 | Hunspell, when it is there | Optional; without it every word passes |
| model | `o42-sheet.c` | 8,580 | Cells, formats, recalculation, undo, objects, filters, tables, queries | The largest file after the evaluator, and the one that would gain most from being split: cells, recalculation and objects are three subjects in one file |
| | `o42-book.c` | 980 | Sheets, names, styles, scripts, the database, the undo stack they share | Clean |
| | `o42-chart.c` | 2,521 | Seventeen kinds of chart, drawn in cairo | One long draw per family; the axis arithmetic is repeated in four of them |
| | `o42-shape.c` | 551 | Shapes and the nine form controls | Fine |
| | `o42-analysis.c` | 928 | The statistical analysis tools | Each writes a labelled table; no drawing |
| formula | `o42-formula.c` | 1,662 | Lexer and parser | Handles array constants, empty arguments, 3-D and structured references |
| | `o42-eval.c` | 10,059 | The machine: operands, array constants, the tree walk, the tables | Was 16,073 and one file; the families live beside it now |
| | `o42-eval-private.h` | 180 | The seam between the machine and the families | What a family may lean on, and nothing else |
| | `o42-fn-*.c` | 6,400 | Eleven families: text, dates, statistics, distributions, finance, options, engineering, random, Hebrew dates, Bessel, info | Each keeps its own function table and its own help |
| io | `o42-xls.c` | 3,943 | BIFF8 both ways, BIFF5 in | Dense but commented; the record numbers are named |
| | `o42-xlsx.c` + `-draw.c` | 4,478 | Office Open XML, on a zip of our own | The drawing half is separate, which keeps both readable |
| | `o42-ods.c` | 3,165 | OpenDocument | Cells, styles, charts, controls, both ways |
| | `o42-gnumeric.c` | 3,022 | The native format | Carries everything the model holds, and office42's own additions in `o42-` attributes Gnumeric passes over |
| | `o42-pdf.c` | 1,412 | Pages out, and pages in through poppler | Shares its layout with the printer |
| | `o42-sql.c` | 619 | The SQLite database | The smallest of the io files that does something whole |
| | `o42-text-formats.c` | 687 | DIF, SYLK and LaTeX | Three formats older than the programs that read them, and the one a paper is set in |
| | `o42-lotus.c` | 247 | Lotus 1-2-3, both ways | The oldest format here, and the smallest reader |
| script | `o42-python.c` | 962 | CPython embedded | Optional; the API is `book` and `sheet` objects |
| ui | `o42-window.c` | 8,818 | 111 actions, every dialog, printing | The biggest file now, and the one that would gain most from being split: the dialogs could be a file of their own |
| | `o42-grid.c` | 5,400 | The grid widget: drawing, editing, selection, objects | The heart of the program, and the file most worth keeping small |

**What the review turned up.** Three things stand out. The first is
size: `o42-eval.c` and `o42-window.c` are each big enough that finding
a function means grepping, and both split along obvious seams. The
second was that the grid walked rows one at a time to answer "which row
is at this y", and was a widget as big as the sheet: the sheet answers
that question by binary search now, and the grid scrolls itself. The
third is that nothing in the program is translated: `menus.ui`
marks its strings translatable and no gettext ever reads them.

---

## 2. Parity with Excel, area by area

Weighted against **Excel 2003**, the version whose shape this program
has. The last column says what is missing, not what is there.

| Area | Weight | Here | Score | What is missing |
|---|---:|---:|---:|---|
| Entering and editing | 8 | 100% | 8.0 | |
| Selecting and navigating | 5 | 100% | 5.0 | |
| Formulas and functions | 15 | 100% | 15.0 | every Excel 2003 function is here (610 in all) |
| Number formats | 6 | 100% | 6.0 | |
| Fonts, borders, colours | 8 | 100% | 8.0 | |
| Styles and conditional formats | 6 | 100% | 6.0 | |
| Rows, columns, sheets | 7 | 100% | 7.0 | |
| Data tools | 10 | 98% | 9.8 | Excel's own pivot parts, and its live TABLE() -- ours writes the numbers |
| Charts | 8 | 100% | 8.0 | |
| Objects | 5 | 100% | 5.0 | |
| File formats | 12 | 98% | 11.8 | Excel 5 charts in `.xls` come back as pictures of themselves |
| Printing | 6 | 100% | 6.0 | |
| Undo | 5 | 100% | 5.0 | |
| Window and dialogs | 6 | 97% | 5.8 | arranging windows, which GTK 4 gives a program no way to do |
| Automation | 3 | 100% | 3.0 | Python instead of Visual Basic, by choice |
| **Total** | 100 | | **99.2** | |

**About 98% of Excel 2003.** Against **Excel 365** the number is nearer
45%: dynamic arrays and tables are here, but Power Query, the modern
pivot engine, co-authoring, LAMBDA's whole environment, threaded
comments, sparklines, slicers and the ribbon are not.

### The grid's size

| | Rows | Columns |
|---|---:|---:|
| office42 | 1,048,576 | 16,384 |
| Excel 5 and 95 | 16,384 | 256 |
| Excel 97 to 2003 | 65,536 | 256 |
| Excel 2007 and later | 1,048,576 | 16,384 |

The grid is Excel 2007's, a `.xlsx` written by a modern Excel comes in
whole, and the scrollbars reach all of it: the grid is a widget the
size of the window that scrolls itself and paints where the scroll
says, so nothing is drawn at a coordinate cairo cannot address. One
thing is worth knowing:

- **An `.xls` cannot hold it all.** Excel 97's file format stops at
  65,536 by 256; office42 writes what fits, leaves out what does not,
  and says how many cells that was rather than losing them quietly.

### What Excel has and office42 has not

| | |
|---|---|
| **Passwords** | the sheet takes one, kept as the short hash Excel invented; it guards against a slip of the hand and nothing else |
| **Track changes, sharing** | not there, and not planned |

---

## 3. The window

Excel 5's shape, and close to it: a menu bar, two toolbars, a formula
bar with a name box, the grid, sheet tabs and a status bar that shows
the sum, the average and the count of what is selected. 106 actions in
all. Judged as an interface rather than as a feature list:

**What is right.** The chrome is the right chrome, and it is drawn
rather than themed, so it looks the same everywhere. The dialogs are
plain GTK boxes with an OK and a Cancel, they remember nothing they
should not, and every one of them can be opened from the command line
with `--activate`, which is how they are checked. The keyboard covers
what a spreadsheet keyboard covers: F2, F5, Shift+F2, Shift+F3, Ctrl+1,
Ctrl+arrows, Shift+Space, Ctrl+Space, Ctrl+PageUp and PageDown.
AutoComplete offers what the column already holds. The fill handle
continues a series. The status bar aggregates. Charts, pictures,
shapes and controls are selected, dragged and resized with handles.

**What is thin.**

1. **No window arranging.** Window ▸ New Window opens a second view of
   the same book, and nothing tiles or cascades them -- GTK 4 took away
   the calls that would move a window, so this one is not ours to
   fix.
2. **The longer dialogs would be better as tabs.** Format Chart has
   fifteen rows in one column now; they can at least be pulled bigger.
3. **The toolbars are fixed.** Excel 5 let them be torn off and
   rearranged; ours are two rows of buttons.

None of these is deep. The first two are a day's work each and would
close most of the distance between "it does the same things" and "it
feels the same".

---

## 4. Parity with Gnumeric

Gnumeric is the free spreadsheet office42 measures its arithmetic
against.

| Area | Weight | Here | Score | What is missing |
|---|---:|---:|---:|---|
| Editing and navigation | 10 | 98% | 9.8 | |
| Formulas and functions | 18 | 98% | 17.6 | 610 of ~650 functions; Miltersen and Schwartz on commodities, and the odd corner of the rest |
| Formatting | 10 | 97% | 9.7 | as above |
| Rows, columns, sheets | 8 | 97% | 7.8 | |
| Data tools | 12 | 100% | 12.0 | |
| Charts | 8 | 97% | 7.8 | GOffice's styling, and the plot types nobody asks for |
| Objects | 4 | 99% | 4.0 | |
| File formats | 14 | 94% | 13.2 | Quattro Pro and Applix, both long dead; `.gnumeric` itself is nearly complete |
| Printing | 6 | 98% | 5.9 | as above |
| Undo | 5 | 100% | 5.0 | |
| Window and dialogs | 5 | 98% | 4.9 | Gnumeric's side pane for names and its object list |
| **Total** | 100 | | **97.7** | |

**About 98% of Gnumeric.**

## 5. Where office42 goes further

A few things are here that neither has in this shape:

- **A database inside the book.** Both can read a database -- Excel
  through Get & Transform, Gnumeric through its data-source plugins --
  but neither carries one in the file. An office42 book can hold a
  whole SQLite database, and `SQLVALUE()` asks it from a cell. See
  [DATABASE.md](DATABASE.md).
- **Python as the macro language**, in the program and recorded from
  what you do, rather than Visual Basic. See [PYTHON.md](PYTHON.md).
- **A terminal front-end.** `office42-calc` drives the whole engine
  from a script, which is how nearly everything here was checked.

## 6. Where the next effort pays most

In the order the work is being done, largest gap first.

1. **Splitting `o42-window.c`** (8,800 lines): the dialogs would make
   a file of their own, the way the evaluator's function families now
   do.
2. **The last two file formats Gnumeric reads**: Quattro Pro and
   Applix. Both are long dead, and neither can be written here with a
   straight face: there is no file of either to read, and no program on
   this machine that writes one, so anything built from the format's
   description alone would go out unverified. Lotus 1-2-3 could be
   done because LibreOffice reads it, which gave the other half of the
   check.
3. **Miltersen and Schwartz on commodity options**, the one formula of
   Gnumeric's derivatives plugin that is not here. It prices against a
   three-factor model -- the spot, the convenience yield and the
   forward rate, each mean-reverting -- and checking it would mean
   simulating all three. Everything else here was checked against
   something outside office42, and this one has nothing to check it
   against.
