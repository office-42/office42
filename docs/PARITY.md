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
| model | `o42-sheet.c` | 7,774 | Cells, formats, recalculation, undo, objects, filters, tables, queries | The largest file after the evaluator, and the one that would gain most from being split: cells, recalculation and objects are three subjects in one file |
| | `o42-book.c` | 980 | Sheets, names, styles, scripts, the database, the undo stack they share | Clean |
| | `o42-chart.c` | 2,086 | Thirteen kinds of chart, drawn in cairo | One long draw per family; the axis arithmetic is repeated in four of them |
| | `o42-shape.c` | 551 | Shapes and the nine form controls | Fine |
| | `o42-analysis.c` | 928 | The statistical analysis tools | Each writes a labelled table; no drawing |
| formula | `o42-formula.c` | 1,662 | Lexer and parser | Handles array constants, empty arguments, 3-D and structured references |
| | `o42-eval.c` | 14,355 | 561 functions and the array evaluator | Far too big for one file. The function table is sorted and binary-searched, which is right, but the file should be several: text, dates, money, statistics, engineering, the array frame |
| io | `o42-xls.c` | 3,943 | BIFF8 both ways, BIFF5 in | Dense but commented; the record numbers are named |
| | `o42-xlsx.c` + `-draw.c` | 4,478 | Office Open XML, on a zip of our own | The drawing half is separate, which keeps both readable |
| | `o42-ods.c` | 2,451 | OpenDocument | Reads more than it writes |
| | `o42-gnumeric.c` | 3,022 | The native format | Carries everything the model holds, and office42's own additions in `o42-` attributes Gnumeric passes over |
| | `o42-pdf.c` | 1,412 | Pages out, and pages in through poppler | Shares its layout with the printer |
| | `o42-sql.c` | 619 | The SQLite database | The newest file, and the smallest of the io ones that does something whole |
| script | `o42-python.c` | 962 | CPython embedded | Optional; the API is `book` and `sheet` objects |
| ui | `o42-window.c` | 8,004 | 106 actions, every dialog, printing | Too big by the same argument as the evaluator: the dialogs could be a file of their own |
| | `o42-grid.c` | 4,520 | The grid widget: drawing, editing, selection, objects | The heart of the program, and the file most worth keeping small |

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
| Formulas and functions | 15 | 100% | 15.0 | every Excel 2003 function is here (561 in all) |
| Number formats | 6 | 98% | 5.9 | the locale codes in `[$-409]` |
| Fonts, borders, colours | 8 | 100% | 8.0 | |
| Styles and conditional formats | 6 | 100% | 6.0 | |
| Rows, columns, sheets | 7 | 98% | 6.9 | dragging a sheet tab, and tab colours |
| Data tools | 10 | 98% | 9.8 | Excel's own pivot parts, and its live TABLE() -- ours writes the numbers |
| Charts | 8 | 100% | 8.0 | |
| Objects | 5 | 99% | 5.0 | the form controls are not in `.xls`; `.xlsx` carries them |
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
| **Trace precedents** | the dependency graph is there and nothing draws it |
| **Custom lists** | the fill handle continues numbers, days and months, but the list cannot be added to |
| **Translations** | English only |
| **Passwords** | protection is a flag, not a secret |
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

1. **Sheet tabs cannot be dragged.** Right-clicking one offers move
   left and move right, and two clicks rename it, but the tab itself
   does not follow the pointer, and there is no tab colour.
3. **No window arranging.** Window ▸ New Window opens a second view of
   the same book, and nothing tiles or cascades them -- GTK 4 took away
   the calls that would move a window, so this one is not ours to
   fix.
4. **The longer dialogs would be better as tabs.** Format Chart has
   fifteen rows in one column now; they can at least be pulled bigger.
5. **The toolbars are fixed.** Excel 5 let them be torn off and
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
| Formulas and functions | 18 | 86% | 15.5 | 561 of ~650 functions; the Hebrew-calendar dates and the exotic options (Black and Scholes is here) |
| Formatting | 10 | 97% | 9.7 | as above |
| Rows, columns, sheets | 8 | 97% | 7.8 | |
| Data tools | 12 | 92% | 11.0 | sampling and the two-factor ANOVAs |
| Charts | 8 | 90% | 7.2 | Gnumeric's chart engine (GOffice) has many more plot types and styling |
| Objects | 4 | 99% | 4.0 | |
| File formats | 14 | 78% | 10.9 | LaTeX, DIF, SYLK, Lotus, Applix, Quattro; `.gnumeric` itself is nearly complete |
| Printing | 6 | 98% | 5.9 | as above |
| Undo | 5 | 100% | 5.0 | |
| Window and dialogs | 5 | 92% | 4.6 | right-click menus |
| **Total** | 100 | | **91.4** | |

**About 91% of Gnumeric.**

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

1. **The form controls in `.xls`.** `.gnumeric` and `.xlsx` carry
   them; BIFF wants them as Escher records with an OBJ apiece, which
   is the last format that cannot hold a book whole.
2. **Sheet tabs that can be dragged**, and given a colour.
4. **Splitting the two big files**, `o42-eval.c` (14,000 lines) and
   `o42-window.c` (8,000): both split along obvious seams.
5. **Translations.** `menus.ui` marks its strings translatable and
   nothing reads them; there is no gettext in the build and no `po/`.
