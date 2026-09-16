# A review of the code, and how far it is from Excel 97

Written from the source as it stands, module by module, and then feature
by feature against Excel 97, the version office42 aims to match feature
for feature. Nothing here is taken on trust: every "yes" below was
found in the code or tried in the running program.

- [1. The code](#1-the-code)
- [2. Parity with Excel 97, area by area](#2-parity-with-excel-97-area-by-area)
- [3. The window](#3-the-window)
- [4. Parity with Gnumeric](#4-parity-with-gnumeric)
- [5. Where office42 goes further](#5-where-office42-goes-further)
- [6. Where the next effort pays most](#6-where-the-next-effort-pays-most)

---

## 1. The code

114,223 lines of C11 in six layers, each of which may only see the ones
below it. `util` and `model` and `formula` and `io` never include GTK,
so the engine builds as a library of its own and the window is the only
thing that knows what a mouse is.

| Layer | Module | Lines | What is in it | How it reads |
|---|---|---:|---|---|
| util | `o42-types.c` | 129 | A1 addresses, ranges, the cell key | Small and total; the key packs a row and a column into 64 bits, so the grid can grow without touching it |
| | `o42-numfmt.c` | 1,057 | The number-format language | The biggest thing in `util`, and the one most worth reading: it parses Excel's codes rather than matching them |
| | `o42-date.c` | 241 | Serial dates, the 1900 leap-year bug included | Deliberately bug-compatible, and says so |
| | `o42-image.c` | 156 | Decoding a picture's bytes | A thin cover over gdk-pixbuf |
| | `o42-spell.c` | 348 | Hunspell, when it is there | Optional; without it every word passes |
| model | `o42-sheet.c` | 14,013 | Cells, formats, recalculation, undo, objects, filters, tables, queries, a whole copy of itself | The largest file of all, and the one that would gain most from being split: cells, recalculation and objects are three subjects in one file |
| | `o42-book.c` | 1,884 | Sheets, names, styles, scripts, the properties, the database, the undo stack they share | Clean |
| | `o42-chart.c` | 2,521 | Seventeen kinds of chart, drawn in cairo | One long draw per family; the axis arithmetic is repeated in four of them |
| | `o42-shape.c` | 551 | Shapes and the nine form controls | Fine |
| | `o42-analysis.c` | 928 | The statistical analysis tools | Each writes a labelled table; no drawing |
| formula | `o42-formula.c` | 2,114 | Lexer and parser | Handles array constants, empty arguments, 3-D and structured references |
| | `o42-eval.c` | 12,490 | The machine: operands, array constants, the tree walk, the tables | Was 16,073 and one file; the families live beside it now |
| | `o42-eval-private.h` | 180 | The seam between the machine and the families | What a family may lean on, and nothing else |
| | `o42-fn-*.c` | 7,008 | Twelve families: text, dates, statistics, distributions, finance, options, engineering, random, Hebrew dates, Bessel, info, the data table | Each keeps its own function table and its own help |
| io | `o42-xls.c` | 6,385 | BIFF8 both ways, BIFF5 in | Dense but commented; the record numbers are named |
| | `o42-xlsx.c` + `-draw.c` | 6,472 | Office Open XML, on a zip of our own | The drawing half is separate, which keeps both readable |
| | `o42-ods.c` | 5,951 | OpenDocument | Cells, styles, charts, controls, the metadata, both ways |
| | `o42-gnumeric.c` | 3,990 | The native format | Carries everything the model holds, and office42's own additions in `o42-` attributes Gnumeric passes over |
| | `o42-pdf.c` | 1,412 | Pages out, and pages in through poppler | Shares its layout with the printer |
| | `o42-sql.c` | 619 | The SQLite database | The smallest of the io files that does something whole |
| | `o42-text-formats.c` | 687 | DIF, SYLK and LaTeX | Three formats older than the programs that read them, and the one a paper is set in |
| | `o42-lotus.c` | 247 | Lotus 1-2-3, both ways | The oldest format here, and the smallest reader |
| script | `o42-python.c` | 3,027 | CPython embedded | Optional; the API is `book` and `sheet` objects |
| ui | `o42-window.c` | 7,284 | The window itself: title bar, toolbars, actions, files, printing, tabs | Was 8,825 and held every dialog too |
| | `o42-window-private.h` | 130 | The seam between the window and its dialogs | The frame a dialog is built in, and what a dialog may ask of the window |
| | `o42-dialogs-*.c` | 7,187 | The File and Edit, Data, Tools and Format menus' dialogs, a file apiece | Each is the dialogs of one menu or two and nothing else |
| | `o42-grid.c` | 8,448 | The grid widget: drawing, editing, selection, objects | The heart of the program, and the file most worth keeping small |

**A note on this table.** The row for entering and editing read 100%
while point mode -- building a reference by clicking cells as a formula
is typed -- was missing altogether, and a click mid-formula committed
the entry instead. A table of areas is only as good as the list of
what each area contains, and that one had a hole in it. Point mode, F4
and coloured references are there now, and so is the list of function
names a formula offers while it is typed; the row is honest.

**What the review turned up.** Three things stand out. The first is
size: `o42-eval.c` and `o42-window.c` are each big enough that finding
a function means grepping, and both split along obvious seams. The
second was that the grid walked rows one at a time to answer "which row
is at this y", and was a widget as big as the sheet: the sheet answers
that question by binary search now, and the grid scrolls itself. The
third is that nothing in the program is translated: `menus.ui`
marks its strings translatable and no gettext ever reads them.

**The 1.0 review.** A second pass over the whole tree before 1.0.0,
layer by layer and checked against the running program, found the
kind of thing a table of features cannot: a cell's input text was made
from its ten-digit display, so copying, sorting, undoing or editing a
number quietly lost its last five digits, and so did a formula's own
literals when it was copied or saved to `.xlsx`; `A:A` and `1:1` did
not parse at all, and `=SUM(A:A)` went into `.xlsx` as `SUM(A)`;
formatting a whole column made a cell for every one of its million
rows; a `=PY()` cell in a file ran on opening once Python was up; a
running total of eight thousand rows would overflow the stack on
Linux; and inserting a row anywhere turned every spill on the sheet
into `#SPILL!`. All of it is fixed, with the checks in the commit
messages. What it says about the code is that the model's seams are
sound -- each of these was one mistake in one place -- and that the
next review should again be of behaviour under load rather than of
the list of features, which is complete.

**The 1.1 pass.** Five audits, one per area -- the `.xls` reader and
writer, printing, the drawn shapes, the accounting and analysis tools,
the macro recorder -- and seven hundred formulas run against Excel's
answers, then six streams of work at once.  What they found was not
missing features so much as features that stopped short: Page Setup
had one margin and no orientation, and every format but `.xlsx` lost
it; `.xls` dropped every drawn shape, every hyperlink and every rich
text run on save, and mislabelled every sheet after a chart sheet; a
rounded rectangle from Excel came back a rectangle; a scalar function
given an array said `#VALUE!` where Excel answers with an array; the
Accounting format's `*` fill and `_` padding were parsed and thrown
away; a recorded macro rewrote every cell where Excel writes one line.
All of it is in, each piece checked against LibreOffice or Excel's
documented answers, and the checks are in the commit messages.  Two
things Excel XP has that were not here at all are: Insert > Picture >
From Scanner or Camera, and the Evaluate Formula and Watch Window
auditing tools.

**The second pass**, the day after, went by the same method over
what the first had left: a second battery of fifteen hundred formulas
(IF turned out to work out both its branches, which made a LAMBDA that
called itself twice take forever; empty references gave blank where
Excel gives 0; names could not hold a formula); hidden sheets, the
view state, conditional-format formulas, validation prompts and
charts' legends in `.xls`; text set properly inside shapes, freeforms,
gradients, anchoring that moves and sizes with the cells; pivot
grouping, a live TABLE(), the input message and the three error styles
of a validation; charts and shapes in the Python API, events, a step
debugger, and `.xlsm` files that keep their Visual Basic for Excel;
several print areas, title rows from anywhere, the Custom Header
dialog, Page Break Preview as Excel draws it; shrink to fit, which the
model had no bit for.  Six streams again, each checked against
LibreOffice or Excel's documented answers.

**The third pass** went by the method again, with the batteries
grown to some fifteen hundred formulas, and then outward from the
engine to what a file and a keyboard put in.  In the engine: TRUNC
threw its second argument away; (-8)^(1/3) gave -2 where Excel says
#NUM!; COUNTIF and SUMIF had no tilde, did not count a '5 typed as
text against a criterion of 5, and passed over an error in a cell
they were adding; WORKDAY ignored a holiday given as one date;
YEARFRAC took a year exactly for more than one; RATE lost its
precision at zero.  In the format language, fractional seconds were
never written and a time was never rounded to the unit shown, so
23:59:59.7 under h:mm:ss was not the 0:00:00 it is in Excel.  In the
model, a defined name stayed where it was when rows went in under it;
"ss" equalled "ß"; AutoFill continued 1, 4, 9 with the mean step
rather than the line, took 31 January and 28 February by the day
rather than the month, copied a lone time and ran Q4 on to Q5; 5
typed into a percentage cell was 500%; and =B1 showed a serial where
Excel shows B1's date.  In the files, a custom number code went into
.ods as General and came back as one, and now goes as the styles and
maps LibreOffice writes; an .xlsx from Excel's built-in formats 37 to
40 lost their pads; a comment from openpyxl and a sheet's own names
were dropped on reading.  And on GTK before 4.16 the stylesheet's
variables were refused, so the silver and the bevels were never
drawn: it names its colours the old way now.  Each was checked
through office42-calc, and the files through openpyxl and xlrd
reading what office42 wrote and office42 reading what they wrote.

**The fourth pass** went outward again, to what a file from Excel
or LibreOffice brings and what a keyboard types.  A font or a fill an
.xlsx names by theme colour and tint -- which is nearly every colour
picked from Excel's palette -- or by palette index came in black or
not at all: the theme part is read now, tinted as Excel tints, and
the 64-colour palette is there.  Excel's colour scales were dropped
on reading and could not be made; they are a kind of condition now,
painted from the range's least to its greatest, kept in .xlsx and
.gnumeric, and Excel's contains, begins-with and blanks rules come
in as the formula Excel writes beside them.  An .ods carried no
conditional formats either way; they go in the form LibreOffice
writes and reads, colour scales included.  The database functions
judged a computed criterion once rather than per record, matched
whole words where Excel matches what begins with the text, and
DGET could not return text.  A structured reference with no table
name -- =[@Qty]*[@Price] in a table's column -- said #NAME?, and an
.xlsx is written with the table's name and [#This Row] spelled out,
as Excel keeps them.  A .txt was refused where Excel opens
tab-delimited text, and a UTF-16 .csv came in as noise.  Two smaller
things: the currency presets an en-US Excel names by number came in
as General, and =B1#+0 did not spill.  Data bars and icon sets are
still passed over, and are the one gap left in the row below.  Each
was checked through office42-calc, and the files through openpyxl
and xlrd reading what office42 wrote and office42 reading what they
and xlwt wrote.

**The fifth pass** changed what the program is measured against.
Excel 2003 had been the yardstick for its features; Excel 97 is the
target now, for its shape and its features both -- the version that fixed
the Excel that people know, with its data form, its Series dialog,
its four Name commands and its Properties -- and the pass went down
its menu bar, entry by entry, against ours.  What was missing was
whole commands rather than corners: Edit ▸ Fill had Down and Right
and not Up, Left, Series or Justify; Edit ▸ Clear cleared and could
not clear only the formats or only the notes; a sheet could be moved
one tab at a time and not copied at all; Insert ▸ Name could Define
and not Paste, Create or Apply; there was no Data ▸ Form, no Row ▸
AutoFit, no Standard Width, no Filter ▸ Show All, no Zoom dialog and
no File ▸ Properties.  All of them are in, each one Excel's in its
particulars: a month series from 31 January goes 29 February, 31
March, 30 April; a copied sheet is "Sheet1 (2)" and its formulas that
named the original name the copy; "Unit Cost" makes the name
UNIT_COST; the data form refuses to extend a list into a row that is
not empty; the properties go into an .xlsx's docProps, an .ods's
meta.xml and a .gnumeric's document-meta, where each program keeps
its own.  Two things turned up on the way.  The model had no standard
column width -- the last column's width stood in for it -- so the
.xlsx and .xls readers gave every one of the 16,384 columns a width
of its own, as did o42_book_clear on the way to loading any file, and
a file's own default never showed; a .gnumeric's DefaultSizePts was
passed over.  And with PYTHONUNBUFFERED in the environment, the
embedded interpreter turned off the buffering of the process's own
stdin as it started, and glibc discarded what office42-calc had read
from its pipe: every line after the first "py" was lost.  Both are
fixed.  Each change was checked through office42-calc and the
running program, and the files through openpyxl and office42 reading
what it wrote.

**The sixth pass** finished the menus the fifth had listed as left,
and made Excel 97 the program's whole account of itself, its shape as
well as its target, in the goals, the guide and the comments alike.
Edit ▸ Fill ▸ Across Worksheets; View ▸ Toolbars, Formula Bar and
Status Bar as things to turn off; Tools ▸ Protection ▸ Protect
Workbook, kept in three formats; Tools ▸ AutoCorrect, with Excel's
four rules and a replacement list the book carries; Format ▸ Sheet ▸
Background, tiled behind the cells and kept in .gnumeric and .xlsx;
and the properties in an .xls, written into the two OLE property-set
streams Excel keeps them in and read back from any code page.  The
.xlsx writer had put workbookPr after definedNames, which the schema
forbids; it and the new workbookProtection sit where Excel expects
them.  Checked as before, olefile reading the .xls streams.

**The seventh pass** took the last items a program can take.  The
pie-of-pie and bar-of-pie charts, drawn as Excel draws them -- the
last slices gathered into Other and shown again beside the pie, at
their own scale, with lines joining the two -- and kept in .gnumeric
and .xlsx as ofPieChart.  Window ▸ Hide and Unhide and the list of
open windows at the bottom of the menu, which GTK 4 does allow, where
Arrange it does not.  The four most recent files in the File menu.
View ▸ Comments, which shows every note at once.  Edit ▸ Paste as
Hyperlink, which needs no OLE after all: it pastes text with a link
back to the source cell, which is what Excel's does within a book.
And Tools ▸ Wizards, Conditional Sum and Lookup, the two add-ins Excel
97 shipped, writing SUMIF and INDEX/MATCH.  Checked through the
running program under xvfb, one --activate at a time, and the charts
through office42-calc, out to .gnumeric and .xlsx and back.  Version
1.0.1.

---

## 2. Parity with Excel 97, area by area

Weighted against **Excel 97**, the version this program aims to match
feature for feature. The last column says what is missing, not what
is there.

| Area | Weight | Here | Score | What is missing |
|---|---:|---:|---:|---|
| Entering and editing | 8 | 100% | 8.0 | Edit ▸ Links, which wants an OLE that is not there |
| Selecting and navigating | 5 | 100% | 5.0 | |
| Formulas and functions | 15 | 100% | 15.0 | every function Excel 97 has (and Excel 2003's) -- the natural-language labels Excel 97 let a formula use, and Excel 2007 took away, are not planned |
| Number formats | 6 | 100% | 6.0 | |
| Fonts, borders, colours | 8 | 100% | 8.0 | |
| Styles and conditional formats | 6 | 100% | 6.0 | Excel 97's three conditions per cell and its Style dialog are here; data bars and icon sets, which came later, are not |
| Rows, columns, sheets | 7 | 100% | 7.0 | |
| Data tools | 10 | 99% | 9.9 | the Template Wizard, Web Query and Data Map, which Excel itself dropped |
| Charts | 8 | 100% | 8.0 | the chart wizard's fourth step is a dialog here |
| Objects | 5 | 96% | 4.8 | WordArt, the Clip Gallery, OLE objects |
| File formats | 12 | 99% | 11.9 | Save Workspace; charts in a BIFF5 `.xls` come back as pictures of themselves |
| Printing | 6 | 100% | 6.0 | the Report Manager, which was an add-in |
| Undo | 5 | 100% | 5.0 | |
| Window and dialogs | 6 | 99% | 5.9 | Window ▸ Arrange, which GTK 4 gives a program no way to do |
| Automation | 3 | 100% | 3.0 | Python instead of Visual Basic, by choice; the recorder writes one line per operation, Alt+F8 and Ctrl+Shift+letter run them |
| **Total** | 110 | | **109.5** | |

**Over 99% of Excel 97** (109.5 of a weight of 110), and the same
picture against Excel 2003, whose additions -- list ranges, XML maps,
the research pane -- are either here as tables or not wanted.
Against **Excel 365** the number is nearer 45%: dynamic arrays and
tables are here, but Power Query, the modern pivot engine,
co-authoring, LAMBDA's whole environment, threaded comments,
sparklines, slicers and the ribbon are not.

### The grid's size

| | Rows | Columns |
|---|---:|---:|
| office42 | 1,048,576 | 16,384 |
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

### What Excel 97 has and office42 has not

| | |
|---|---|
| **Passwords** | the sheet and the workbook each take one, kept as the short hash Excel invented; they guard against a slip of the hand and nothing else |
| **Window ▸ Arrange** | GTK 4 took away the calls that move a window, so a program cannot tile or cascade its own; Hide, Unhide and the window list are there |
| **Edit ▸ Links** | it links to OLE objects, which have no home on this desktop |
| **Insert ▸ Object, WordArt, Clip Gallery, Map** | OLE has no home on this desktop; WordArt is a text box with a font; the map was licensed data Excel dropped |
| **File ▸ Save Workspace** | a list of open files and their windows; open them again |
| **Track changes, sharing** | not there, and not planned |
| **The Office Assistant** | no |

---

## 3. The window

Excel 97's shape, and close to it: a menu bar, two toolbars, a formula
bar with a name box, the grid, sheet tabs and a status bar that shows
the sum, the average and the count of what is selected. 172 actions in
all. Judged as an interface rather than as a feature list:

**What is right.** The chrome is the right chrome, and it is drawn
rather than themed, so it looks the same everywhere. The dialogs are
plain GTK boxes with an OK and a Cancel, they remember nothing they
should not, and every one of them can be opened from the command line
with `--activate`, which is how they are checked. The keyboard covers
what a spreadsheet keyboard covers: F2, F5, Shift+F2, Shift+F3, Ctrl+1,
Ctrl+arrows, Shift+Space, Ctrl+Space, Ctrl+PageUp and PageDown.
AutoComplete offers what the column already holds, and two letters into
a function's name the functions that begin that way are offered too,
with the signature of the call the caret is in under the cell. The
formula bar is the same edit as the cell rather than a report of it.
The fill handle continues a series. The status bar aggregates. Charts, pictures,
shapes and controls are selected, dragged and resized with handles.

**What is thin.**

1. **No window arranging.** Window ▸ New Window opens a second view of
   the same book, the Window menu lists, hides and unhides them, and
   nothing tiles or cascades them -- GTK 4 took away the calls that
   would move a window, so this one is not ours to fix.
2. **The longer dialogs would be better as tabs.** Format Chart has
   fifteen rows in one column now; they can at least be pulled bigger.
3. **The toolbars are fixed.** Excel 97 let them be torn off and
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
| Formulas and functions | 18 | 98% | 17.6 | 637 of ~650 functions; Miltersen and Schwartz on commodities, and the odd corner of the rest |
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

1. **Splitting `o42-sheet.c`** (14,013 lines), the biggest file of all:
   cells and their formats, recalculation and the dependency graph,
   and the objects that float over the grid are three subjects in one
   file.  Of Excel 97's menus nothing is left that a program can take:
   the table above is OLE, which this desktop has not, and window
   arranging, which GTK 4 has not.
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
