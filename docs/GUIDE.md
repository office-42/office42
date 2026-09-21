<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# The office42 user guide

This is the long form of how to use office42 -- Numbers42 is the same
program under its other name. The README says what the program is and
what is in it; this says how to do the work. It follows the menus, so
if you can find a thing in the window you can find it here.

Everything described below is in the program as it stands. Where
something is not there, or is there in a smaller way than Excel has it,
this guide says so rather than glossing over it.

## Contents

1. [Starting off](#1-starting-off)
2. [The window](#2-the-window)
3. [Moving about and selecting](#3-moving-about-and-selecting)
4. [Putting things in cells](#4-putting-things-in-cells)
5. [Formulas](#5-formulas)
6. [Functions](#6-functions)
7. [Formatting](#7-formatting)
8. [Rows, columns and sheets](#8-rows-columns-and-sheets)
9. [Looking at a large sheet](#9-looking-at-a-large-sheet)
10. [Working with data](#10-working-with-data)
11. [Charts](#11-charts)
12. [Pictures, shapes, notes and links](#12-pictures-shapes-notes-and-links)
13. [Spelling](#13-spelling)
14. [Printing and PDF](#14-printing-and-pdf)
15. [Files](#15-files)
16. [Python and the macro recorder](#16-python-and-the-macro-recorder)
17. [The database](#17-the-database)
18. [Protecting a sheet](#18-protecting-a-sheet)
19. [office42-calc, the terminal front-end](#19-office42-calc-the-terminal-front-end)
20. [Keyboard reference](#20-keyboard-reference)

---

## 1. Starting off

Run `office42` with no arguments for an empty book, or with a file name
to open one:

```sh
office42                       # Book1, one empty sheet
office42 accounts.gnumeric     # open a file
office42 sales.xlsx budget.ods # one window each
```

A book starts with a single sheet called Sheet1. Nothing is written to
disk until you save; the title bar carries an asterisk while there is
work that has not been saved, and closing a modified book asks whether
to save it, throw the work away, or go back.

Two command-line options exist for taking pictures of the program,
which is how its own screenshots and much of its testing are done:

```sh
office42 --screenshot out.png book.gnumeric              # render the window to a PNG
office42 --screenshot out.png --select B7 book.gnumeric  # with B7 selected first
office42 --screenshot out.png --activate freeze-panes book.gnumeric
```

`--activate` runs a menu action by name (the names are the ones in
`data/ui/menus.ui`, without the `win.` in front) and `--select A1,B40`
selects a cell, runs the action, and then goes to a second cell, which
is how a scrolled view gets pictured.

## 2. The window

From the top down:

- **The menu bar** — File, Edit, View, Insert, Format, Tools, Data,
  Window, Help, in the order Excel 97 had them.
- **Two toolbars** — the standard one (new, open, save, print, cut,
  copy, paste, undo, redo, AutoSum, the Function Wizard, sort and the
  ChartWizard) and the formatting one (font name and size, bold, italic,
  underline, the three alignments, currency, percent, comma, and more or
  fewer decimals). View ▸ Toolbars turns either off, and View ▸ Formula
  Bar and View ▸ Status Bar do the same for those; the grid takes the
  room.
- **The Name Box** on the left of the formula bar. It shows where the
  cursor is; type a reference or a defined name into it and press Enter
  to go there.
- **The formula bar** — what is really in the cell, not what is shown.
  The × cancels an edit and the ✓ commits it.
- **The grid**, with column letters across the top and row numbers down
  the side. Click a header to select the whole column or row; drag a
  boundary between headers to resize; double-click a column boundary to
  fit the widest cell in it. A format applied to a whole column or row
  is the column's own: every cell in it wears it, the ones typed later
  included, and the sheet is no bigger for it.
- **The sheet tabs** along the bottom. Click to switch, double-click to
  rename, drag to reorder, right-click for the sheet menu.
- **The status bar**, which shows Ready, the zoom when it is not 100%,
  and the sum and count of the selection when more than one cell is
  selected.

View ▸ Options turns the gridlines and the zero values off and on.

**Ctrl+click** adds to the selection: what was selected stays, and a
new rectangle starts where you pressed. Formatting, clearing and the
sum in the status bar act on all of them; a plain click starts again.

**The right button** opens a menu wherever you press it: over a cell,
cut, copy, paste, paste special, insert and delete cells, clear, format
cells, a note and a hyperlink; over a row or column heading, insert,
delete, clear, the size, hide and unhide; over a chart, a shape or a
control, the dialog that formats it; and over a sheet tab, insert,
delete, rename, move left or right, and the tab colour. Two clicks on a tab rename the
sheet.

## 3. Moving about and selecting

| To do this | Press |
|---|---|
| Move one cell | arrow keys |
| To the edge of the data | Ctrl+arrow |
| To A1 / to the last used cell | Ctrl+Home / Ctrl+End |
| To the start of the row | Home |
| A screen at a time | Page Up, Page Down |
| Extend the selection | Shift with any of the above |
| Select the row / the column | Shift+Space / Ctrl+Space |
| Select everything | Ctrl+A |
| Go to a cell by name | F5, or type into the Name Box |
| Next / previous sheet | Ctrl+Page Down / Ctrl+Page Up |

Dragging with the mouse selects a block; Shift+click extends from the
active cell; Ctrl+click on a hyperlink follows it rather than selecting
the cell.

## 4. Putting things in cells

Type and press Enter. Tab moves right; Enter after a run of Tabs goes
back to the column the row began in, as Excel's does. F2 edits in
place, Escape cancels, Delete clears what is there.

What you type is read for what it is: `42` and `1,234.50` are numbers,
`2026-08-29`, `29/08/2026` and `29 Aug 2026` are dates, `14:30` is a
time, `TRUE` is a truth value, `=A1*2` is a formula, and anything else
is text. An apostrophe in front (`'0042`) keeps the rest as text.

**AutoComplete.** Typing text into a cell offers the first text already
in that column that begins with it, with the rest selected, so carrying
on typing replaces the suggestion and pressing Enter accepts it.

**Filling.** Ctrl+D fills down from the top row of the selection and
Ctrl+R fills right, and Edit ▸ Fill ▸ Up and Left go the other way,
copying the bottom row or the right column over the rest. Edit ▸ Fill ▸
Across Worksheets copies the selection to the same cells on the other
sheets -- each offered and ticked, since office42 has no grouped sheets
-- as all of it, its contents or its formats. Dragging the
small square at the bottom-right corner of the selection continues a
series: numbers step by the difference between the first two, dates by
day, and names of days and months by name.

**Series and Justify.** Edit ▸ Fill ▸ Series is Excel's dialog: each
column of the selection (each row, with Rows chosen) runs on from its
first cell -- Linear adds the step, Growth multiplies by it, Date steps
by day, weekday, month or year, and AutoFill continues whatever the
leading cells hold as the fill handle would. A stop value ends the
series where it would pass it, and with one cell selected the series
runs down from it until it does; Trend fits a line (or, for Growth, an
exponential) through the numbers already there and writes the fit over
them. A month series from 31 January goes 29 February, 31 March, 30
April, as it does in Excel. Edit ▸ Fill ▸ Justify takes the text in the
first column of the selection and lays it out again in rows that fit
the selection's width, a blank row starting a new paragraph, running on
below the selection when the words need more rows.

**Clear.** Edit ▸ Clear takes away All, only the Formats, only the
Contents (which is what Delete does) or only the Notes.

**AutoCorrect.** Tools ▸ AutoCorrect does to typed text what Excel 97
does: TWo INitial CApitals become one, the letter after a full stop
becomes a capital, a day's name typed in lower case gets its capital,
and the words in the replacement list are replaced as whole words --
`(c)` by ©, `...` by …, `teh` by `the`, `i` by `I`. A formula, a
quoted text, a number, a date or a word in capitals is never touched.
Each of the four can be turned off in the dialog, which also edits the
list; Excel keeps the list in its options, office42 keeps it with the
book, so a new book starts with a short list and `.gnumeric` carries
yours.

**Moving and copying.** Ctrl+X, Ctrl+C and Ctrl+V cut, copy and paste,
with references relocated the way Excel relocates them. Edit ▸ Paste
Special pastes only the values, only the formats or only the formulas,
and can turn the block on its side. Edit ▸ Paste as Hyperlink pastes
the copied cells as text, each a link back to the cell it came from,
so Ctrl+click on the copy goes to the original. Dragging the outline of a selection
moves those cells -- formulas inside travel unchanged, and formulas
elsewhere that pointed into the block follow it -- and holding Ctrl
while dragging copies them instead; the pointer says which it will be.

**Undo.** Ctrl+Z and Ctrl+Y (or Ctrl+Shift+Z) step back and forward
through everything, including a deleted sheet, which comes back with
everything on it.

## 5. Formulas

A formula starts with `=`. It may hold numbers, text in quotes, cell
references, ranges, names, operators and function calls:

```
=B4*1.2
=SUM(B4:B6)
=IF(D5>0.2, "good", "flat")
=Sheet2!B3 + 'Another Sheet'!C4
=SUM(Sheet1:Sheet3!B2)          three-dimensional: the same cell on every sheet between
```

**References.** `A1` moves when the formula is copied, `$A$1` does not,
and `A$1` and `$A1` pin one half. **F4** cycles the reference at the
caret round those four, and on a range it does both halves at once.
`A:A` is the whole of column A and `2:2` the whole of row 2, `$A:$C`
and `Sheet2!B:B` as you would expect: a formula over `A:A` reads every
cell the column has, however many rows are added below it, and costs
only the cells that are there. Inserting rows never moves a whole
column, and copying `=SUM(A:A)` one column to the right gives
`=SUM(B:B)`.

**The formula bar.** The bar along the top is not a box that reports
what a cell holds: it is the same edit as the cell, seen from up there.
Type in the cell and the bar follows, coloured references and all; type
in the bar and the cell follows, and the name list, the argument tip
and point mode all work from either. The cross throws the edit away and
the tick finishes it. With nothing being edited it shows what the cell
holds, and typing there starts an edit on that cell.

**The name of a function, offered.** office42 knows some six hundred
functions, and nobody remembers all of them. Two letters into a name --
`=SU` -- the ones that begin that way are listed under the cell with a
line each saying what they do. Up and down walk the list, Tab or Enter
takes the highlighted name and opens its bracket for you, and Escape
takes the list away and leaves what you had typed. Go on typing and the
list narrows; type the whole name and it goes. It appears only where a
function could stand: after the `=`, after an operator, after a bracket
or a comma, so `=A1+SU` offers and `SU` on its own does not.

**What the call wants next.** Knowing the name is not the same as
knowing the arguments: VLOOKUP takes four and nobody remembers which is
which. While the caret stands inside a call's brackets the signature is
shown under the cell with the argument you are on in bold, and it
follows the commas and the nesting -- inside `=SUM(A1,IF(` it is IF's
signature you are shown, not SUM's. Past the last named argument of a
function that takes any number of them, the `...` is what is bold.
Commas inside quotes are text, and are not counted.

**Coloured references.** While a formula is being typed, each
reference in it is written in a colour of its own and the cells it
names are outlined in the same one -- the quickest way to see that a
formula is reading what you meant. Text in quotes and function names
are left alone: `LOG10(` is a function, not a cell.

**Pointing.** Half of writing a formula is not typing it. Type `=SUM(`
and then click the cells you mean: the reference appears where the
caret is, and dragging makes it a range. The arrows do the same --
Shift with them extends the range, Ctrl with them runs to the edge of
the data -- and the cells being pointed at are outlined while you
choose. Pointing begins only where a reference could begin: after the
`=`, after an operator, after a bracket or a comma. After `=A1` an
arrow still leaves the cell, and a click while ordinary text is being
typed still enters it.

**Names.** Insert ▸ Name ▸ Define (Ctrl+F3) gives a cell or a range a
name, which then works anywhere a reference does: `=SUM(Sales)`. The
Name Box lists them. Insert ▸ Name ▸ Create makes names from the labels
along the edges of the selection -- its top row, left column, bottom
row or right column, the dialog guessing the first two from whether the
corner cells are text -- each naming the cells of its column or row
inside; "Unit Cost" becomes `UNIT_COST`. Insert ▸ Name ▸ Apply rewrites
the formulas of the selection (of the whole sheet, when one cell is
selected) so that every reference that is exactly a named rectangle
reads as the name: `=SUM(A2:A9)` becomes `=SUM(Sales)`. Insert ▸ Name ▸
Paste puts a name into the formula being typed, or starts the cell
with `=Name`; its Paste List button writes every name and what it
refers to in two columns from the active cell.

**Array formulas.** Ctrl+Shift+Enter enters a formula over the whole
selection, and the braces appear in the formula bar. Functions that
answer with a rectangle -- `TRANSPOSE`, `MMULT`, `MINVERSE`,
`FREQUENCY`, `LINEST`, `SORT`, `UNIQUE`, `FILTER`, `TEXTSPLIT`,
`MODE.MULT` and the rest -- also spill into the cells beside them when
entered normally, and give `#SPILL!` when something is in the way.

**Errors.** `#DIV/0!`, `#VALUE!`, `#REF!`, `#NAME?`, `#NUM!`, `#N/A`
and `#SPILL!` mean what they do in Excel. `IFERROR` and `IFNA` catch
them.

**Recalculation** happens as you type, following only the cells that
depend on what changed. Functions whose answer depends on more than
their arguments -- `RAND` and its family, `NOW`, `TODAY`, `OFFSET`,
`INDIRECT`, `CELL`, `INFO`, `PY` -- are worked out afresh on every
recalculation.

**How the book calculates.** Tools ▸ Options offers two things Excel
offers:

- **Calculate only when asked (F9).** Nothing is worked out as you
  type; Format ▸ Calculate Now, or F9, works everything out. A formula
  just typed in still shows its answer.
- **Allow a formula to depend on itself.** A circular reference is
  `#CIRCULAR!` unless this is on; with it on, the loop is gone round at
  most so many times, or until nothing moves by more than the amount
  you give. `=A1+B1` in B1, with 1 in A1 and a hundred passes, settles
  at 101.

Both are kept in the file -- `.gnumeric` and `.xlsx` both have a place
for them.

## 6. Functions

525 of them. Insert ▸ Function (Shift+F3) opens the Function Wizard: a
list of every function office42 knows, with what it takes and what it
does, and a filter box. `office42-calc --functions` prints the same
list.

The families, with a few from each:

| Family | Examples |
|---|---|
| Arithmetic | `ABS`, `MOD`, `CEILING`, `FLOOR.PRECISE`, `GCD`, `SUMPRODUCT`, `SUBTOTAL` |
| Trigonometry | `SIN`, `ACOT`, `SEC`, `RADIANS`, `SQRTPI` |
| Statistics | `AVERAGE`, `MEDIAN`, `STDEV`, `PERCENTILE`, `RANK.AVG`, `SKEWP`, `KURTP`, `SSMEDIAN`, `CRONBACH` |
| Distributions | `NORM.DIST`, `T.INV`, `CHISQ.DIST`, `BETADIST`, `POISSON`, `WEIBULL`, `LOGISTIC`, `PARETO` |
| Tests | `TTEST`, `FTEST`, `ZTEST`, `CHITEST`, `ADTEST`, `NORMALTEST` |
| Random | `RAND`, `RANDBETWEEN`, `RANDNORM`, `RANDPOISSON`, `RANDGAMMA`, `RANDDISCRETE` (28 distributions) |
| Matrices | `MMULT`, `MINVERSE`, `MDETERM`, `CHOLESKY`, `EIGEN`, `MPSEUDOINVERSE`, `TRANSPOSE` |
| Lookup | `VLOOKUP`, `INDEX`, `MATCH`, `XLOOKUP`, `CHOOSECOLS`, `TAKE`, `DROP`, `OFFSET`, `INDIRECT` |
| Text | `LEFT`, `MID`, `SUBSTITUTE`, `TEXT`, `TEXTJOIN`, `TEXTBEFORE`, `TEXTSPLIT`, `PROPER`, `UNICHAR` |
| Dates | `DATE`, `EDATE`, `EOMONTH`, `WEEKNUM`, `NETWORKDAYS.INTL`, `WORKDAY.INTL`, `YEARFRAC`, `DATEDIF` |
| The Hebrew calendar | `HDATE`, `HDATE_YEAR`, `HDATE_MONTH`, `HDATE_DAY`, `HDATE_JULIAN`, `HDATE2DATE`, `HDATE2JULIAN`, `DATE2HDATE` |
| Money | `PMT`, `IRR`, `XNPV`, `PRICE`, `YIELD`, `DURATION`, `COUPNUM`, `TBILLEQ`, `AMORLINC` |
| Options | `OPT_BS` and its greeks, `OPT_BAW_AMER`, `OPT_BINOMIAL`, `OPT_JUMP_DIFF`, `OPT_GARMAN_KOHLHAGEN`, the lookbacks, the choosers, the compounds |
| Database | `DSUM`, `DGET`, `DAVERAGE`, `DCOUNT` with a criteria table |
| Engineering | `CONVERT`, `BESSELJ`, `DEC2HEX`, `COMPLEX`, `IMSINH`, `IMARCTAN`, `DELTA`, `ERF` |
| Logic and information | `IF`, `IFS`, `SWITCH`, `LET`, `LAMBDA`, `ISFORMULA`, `CELL`, `INFO`, `SHEET`, `FORMULATEXT` |

Every function Excel 2003 has is here, along with a good deal of
Gnumeric's own and the newer Excel ones that are worth having. The
Hebrew ones count months from Tishri, where the civil year begins, so
that a leap year's Adar I is 6 and its Adar II is 7. Text in
a range is skipped by `SUM` and counted by `SUMA`, as in Gnumeric.

## 7. Formatting

Format ▸ Cells (Ctrl+1) has the tabs Excel 97 had: **Number**,
**Alignment**, **Font**, **Border**, **Patterns** and **Protection**.
Everything applies to the whole selection and is one undo step.

**Number.** The built-in kinds are General, Fixed, Comma, Currency,
Percent, Scientific, Date, Time, Date and Time, and Text, each with a
number of decimals. Under them is the format language, which you can
type into the same box:

| Code | 1234.5 shows as |
|---|---|
| `#,##0.00` | 1,234.50 |
| `"$"#,##0` | $1,235 |
| `0.0%` | (of 0.125) 12.5% |
| `0.00E+00` | 1.23E+03 |
| `#,##0.00;[Red](#,##0.00)` | negatives in red brackets |
| `dddd d mmmm yyyy` | (of a date) Saturday 29 August 2026 |
| `[$-414]dddd d. mmmm yyyy` | (of a date) lørdag 29. august 2026 |
| `h:mm AM/PM`, `[h]:mm` | times, the second past 24 hours |
| `@` | the text as typed |

The language is locale-independent: a file shows the same on every
machine. `[$-409]` and its like say which language writes the month
and day names -- the number is the one Excel uses, and Danish, Dutch,
English, Finnish, French, German, Italian, Norwegian, Polish,
Portuguese, Spanish and Swedish are known. A language that is not
known is written in English, which is what Excel does with one it was
not installed with.

A date format keeps its code in `.xlsx`, `.xls`, `.gnumeric` and
`.ods`. OpenDocument has no place for a format string, so there the
code becomes a `number:date-style` field by field and is read back the
same way.

**Font.** Face, size, bold, italic, underline, strikeout and colour.
Ctrl+B, Ctrl+I and Ctrl+U are the quick ones.

**Rich text.** Select part of the text while editing a cell and press a
font button: that part alone changes. The cell then holds runs, drawn
in the grid, on paper and in PDF, and carried in `.xlsx`, `.ods` and
`.gnumeric`. Typing the cell's text again puts it back to one font.

**Alignment.** Left, centre, right and general (which puts text left
and numbers right), top, middle and bottom, wrapping, an indent, and
rotation from -90 to +90 degrees.

**Border.** Each of the four sides on its own, with a style -- thin,
medium, thick, dashed, dotted, double -- and a colour.

**Patterns.** A background colour and, over it, one of Excel's eighteen
patterns in a colour of its own: five densities of grey, stripes four
ways, and two crosshatches, each light and dark.

**Cell styles.** Format ▸ Style keeps named sets of formatting: the
eleven Excel starts with (Normal, Title, Heading 1 and 2, Good, Bad,
Neutral, Note, Comma, Currency, Percent) and any you define from the
active cell. A cell remembers the style it wears, so redefining the
style restyles every cell that wears it.

**Format ▸ AutoFormat** gives the selection one of five ready-made
looks -- Simple, Classic, Financial, Colourful, Plain rules -- taking
its first row for a heading.

**Format ▸ Format Painter** picks up the look of the active cell; the
next cell or range you click is given it, all of it.

**Conditional formatting.** Format ▸ Conditional Formatting gives a
cell a second look that applies while a condition holds -- between, not
between, equal, greater, less and the rest -- with bold, italic, a
colour and a fill.

## 8. Rows, columns and sheets

Insert ▸ Rows, Columns and Cells make room; Edit ▸ Delete Rows, Delete
Columns and Delete take it away. Formulas that pointed at what moved
follow it.

Format ▸ Column Width, Row Height and Column AutoFit set sizes by
number, or drag the boundary between two headers. Format ▸ Row ▸
AutoFit makes each selected row as tall as its tallest text, wrapped
text laid out at the column's width; Format ▸ Column ▸ Standard Width
sets the width of every column that has not been given one of its own,
and the file keeps it. Format ▸ Row ▸ Hide and Column ▸ Hide take a
band out of sight; Unhide brings it back.

Data ▸ Group Rows and Group Columns make an outline, with the level
buttons above the headers to fold and unfold it; Auto Outline finds the
groups from the SUM rows and columns, Clear Outline takes them away,
Show Detail and Hide Detail work one group, and Settings says whether
the summary rows lie below or above the detail, the summary columns
right or left.

Format ▸ Merge Cells joins the selection into one; the top-left cell's
content is what shows.

Insert ▸ Worksheet adds a sheet before the current one, Format ▸ Sheet
▸ Rename names it, Edit ▸ Delete Sheet removes it -- and Ctrl+Z brings it
back with everything on it. Edit ▸ Move or Copy Sheet puts the sheet
before another one or at the end, or, with Create a copy, makes a copy
there named "Sheet1 (2)" as Excel names it: every cell, format, note,
chart, shape and setting comes along, and formulas on the copy that
named the original by name now name the copy. Click a tab to go to that
sheet. Format ▸
Sheet ▸ Hide takes a sheet out of the tab strip, Unhide lists the hidden
ones and brings one back; a hidden sheet's cells are still there for
formulas. Format ▸ Sheet ▸ Background puts a picture behind the cells,
tiled from the sheet's corner and never printed, and Delete Background
takes it away; `.gnumeric` and `.xlsx` carry it. Each sheet keeps its
own zoom, gridlines, zeros, selection and active cell, and the file
keeps them.

Drag a tab sideways to move the sheet where you let it go. The tab's
right-click menu has **Tab Colour...** and **No Tab Colour**; the colour
shows as a band under the name, and it is kept in `.gnumeric`, `.xlsx`
and `.ods`.

## 9. Looking at a large sheet

- **Zoom.** View ▸ Zoom from 25% to 200%, or View ▸ Zoom ▸ Zoom... for
  a custom percentage or Fit selection, which picks the zoom at which
  the selection just fills the window. Everything scales, charts
  included.
- **Full Screen.** View ▸ Full Screen (F11) gives the whole screen to
  the window; F11 again puts it back.
- **Freeze Panes.** View ▸ Freeze Panes pins the rows above and the
  columns left of the active cell, so the headings stay while the rest
  scrolls. A **split** (View ▸ Split) divides the window instead, and
  its grey bars can be dragged to move the division; drag one back into
  the corner to take it away.
- **Split.** Window ▸ Split divides the view above and left of the
  active cell into two or four panes that scroll on their own: the
  mouse wheel moves whichever pane the pointer is over, the scrollbars
  move the main one. Split again to put it back. The split is a view,
  not part of the file.
- **Windows.** Window ▸ New Window opens a second window on the same
  book, and the bottom of the Window menu lists every open window by
  its title; pick one to bring it to the front. Window ▸ Hide takes
  the current window off the screen without closing its book, as long
  as another window stays, and Window ▸ Unhide brings a hidden one
  back. Arranging windows is the one thing this menu does not do: GTK
  4 gives a program no way to move its own windows.
- **Custom Views.** View ▸ Custom Views keeps a named window state --
  the sheet, the selection, the zoom, and whether the panes were frozen
  or split -- to come back to. They belong to the book and travel in
  `.gnumeric` and `.xlsx`.
- **Page Breaks.** View ▸ Page Breaks draws dashed blue lines where the
  printed pages divide. Dragging one moves it: it becomes a break of
  your own where you drop it, as in Excel.

## 10. Working with data

**Sort.** Data ▸ Sort sorts the selection by up to three keys, each
ascending or descending, with or without a header row.

**Form.** Data ▸ Form is Excel's data form: the list around the active
cell -- the block of filled cells bounded by empty rows and columns,
its first row the headings -- shown one record at a time, a formula's
value as a label rather than a field. New starts an empty record and
writes it to the row under the list, Delete takes the record's row
out, Restore puts the fields back as the record has them, Find Prev
and Find Next move through the records, and Criteria turns the fields
into criteria -- text a field must begin with, or a comparison such as
`>100` or `<>Oslo` -- after which Find Prev and Find Next visit only
the records that meet them. Enter in a field writes the record and
moves to the next; each record written or deleted is one undo step.

**AutoFilter.** Data ▸ Filter ▸ AutoFilter puts a dropdown on each heading.
It lists `(All)` and every distinct value in the column, and a Custom
entry that asks for a test instead: equals, does not equal, is greater
than, is greater than or equal to, is less than, is less than or equal
to, begins with, ends with, contains. Rows that do not match are
hidden, and the row numbers stay as they were so you can see what is
filtered. Data ▸ Filter ▸ Show All puts every column back to `(All)`
at once, the filter itself staying.

**Advanced Filter.** Data ▸ Filter ▸ Advanced Filter takes a criteria table --
headings on top, conditions under them, several rows meaning "or" --
and either hides the rows that do not match or copies the ones that do
somewhere else.

**Text to Columns.** Data ▸ Text to Columns splits the selected
column at a comma, a tab, a semicolon, a space, or another character
you type.

**Validation.** Data ▸ Validation says what a cell may hold: a whole
number, a decimal, a date, a time, a text length -- between, outside,
equal to, greater or less than the bounds you give -- one of a list of
values, or whatever a formula of your own allows. An input message with
a title shows under the cell while it is chosen; a list gets an arrow
in the cell that drops the choices. What happens to an entry the rule
refuses is Excel's three styles: Stop refuses it with the title and
message, Warning asks whether to keep it anyway, Information tells and
keeps it. Data ▸ Circle Invalid Data draws a red ring round every cell
that breaks its rule; Clear Validation Circles takes them away.

**Tables.** Data ▸ Table turns a range into a named table with banded
rows, filter buttons and an optional total row; formulas can name its
columns, `Table1[Sales]`.

**Subtotals.** Data ▸ Subtotals inserts a subtotal at every change of a
chosen column, with an outline to fold them.

**Remove Duplicates.** Data ▸ Remove Duplicates keeps the first of each
repeated row, over the columns you choose.

**Consolidate.** Data ▸ Consolidate adds up several ranges into one,
matching by position or by the labels in the top row and left column.

**Pivot tables.** Data ▸ Pivot Table lays out a cross-tabulation. Give
it the source table, one or two fields down the rows, one or two
across the columns, a field to summarise and a function for it (sum,
count, average, min, max) -- or a calculated field written as a
formula over the column names, `=Sales-Costs` -- and a field to filter
on. Several data fields stand side by side, or down the rows; a date
field groups by years, quarters, months or days, a number field into
buckets of a size from a start, and any field into named groups; the
subtotals and the grand totals can be turned off. It is
written as values, not as Excel's own pivot part, so a file opened in
Excel shows the table but does not offer to refresh it. Data ▸ Refresh
Pivot Table rebuilds it here.

**What-If Table.** Data ▸ What-If Table fills a rectangle with one
formula worked out for a range of inputs -- Excel's Data ▸ Table. Put
the values an input may take down the left column, along the top row,
or both; put the formula in the corner (or, with one variable, in the
top row or the left column beside the values); select the whole
rectangle, and name the cell each set of values goes into. Every
combination is worked out and written in, and the input cells are put
back as they were.

A loan of 200,000 over 30 years, with the rates 3%, 4%, 5% and 6% down
column A and `=PMT(B2/12,B3*12,B1)` in B5, gives the payment at each
rate down column B.

**Goal Seek.** Tools ▸ Goal Seek changes one cell until another reaches
a value.

**Wizards.** Tools ▸ Wizards ▸ Conditional Sum writes a SUMIF for
you: give it a list with its headings, the column to add up, the
column to test and the condition, and it puts the formula where you
say. Tools ▸ Wizards ▸ Lookup writes INDEX and MATCH over a table with
labels along the top and down the left, for the value where a row and
a column meet; the row and column labels can be typed or taken from
cells. Both were add-ins in Excel 97 and are built in here.

**Solver.** Tools ▸ Solver maximises, minimises or hits a target by
changing any number of cells, subject to constraints that may hold a
cell to a whole number or to 0 and 1, by a Nelder-Mead search with
penalties and a branch-and-bound over the whole-number ones; Assume
Non-Negative keeps the cells above zero, and the Answer Report is
written to a sheet of its own.

**What-If tables.** Data ▸ What-If Table fills a row or column (or
both) of inputs through a formula, and the results are Excel's own
`=TABLE(row_input, column_input)` array, worked out again whenever the
inputs or the formula change.

**Euro Conversion.** Tools ▸ Euro Conversion converts a range between
the euro and the currencies that joined it, at the fixed rates, as
values or as EUROCONVERT formulas, with the rounding Excel's tool used.

**Scenarios.** Tools ▸ Scenarios keeps named sets of values for the
same cells and puts any of them back; Summary writes a report sheet
with the changing cells and the result cells under each scenario.

**Protection.** Tools ▸ Protection ▸ Protect Sheet locks the sheet and
asks for a password, which may be left empty; taking the protection off
asks for the same one back. Tools ▸ Protection ▸ Protect Workbook locks
the sheets as they are instead: none can be added, deleted, renamed,
moved, copied, hidden or unhidden until it is taken off, and the cells
stay as editable as their sheets allow; `.gnumeric`, `.xlsx` and `.ods`
carry it. The password is kept as the sixteen-bit hash Excel
invented for this and every spreadsheet since has had to keep -- it
cannot be turned back into the password, and it cannot be relied on
either: a hash that short collides, and anything reading the file can
take the protection off. It guards against a slip of the hand, and the
dialog says so. It travels in `.xlsx`, `.xls` and `.gnumeric`.

**Custom lists.** The fill handle continues the days and the months
already. Tools ▸ Custom Lists takes another run -- the quarters, the
regions, the shifts -- typed with commas between; drag a cell holding
any of its names and the rest follow, round and round. The lists are
kept with the book, so a book that needs them carries them.

**Auditing.** Tools ▸ Auditing ▸ Trace Precedents rings every
rectangle the selected cell's formula reads and draws an arrow from
each into the cell; Trace Dependents does it the other way, from the
cell to every formula on the sheet that reads it. Remove All Arrows
clears them. The arrows are a way of looking and are not kept in the
file.

**Statistical Analysis.** Tools ▸ Statistical Analysis is Gnumeric's
tool of that name and Excel's Analysis ToolPak:

| Tool | What comes out |
|---|---|
| Descriptive Statistics | mean, standard error, median, mode, deviation, variance, kurtosis, skew, range, smallest, largest, sum, count, confidence level |
| Correlation, Covariance | the matrix between the variables |
| Regression | R and R squared, the ANOVA table, and each coefficient with its standard error, t and P-value |
| Histogram | the count in each bin and the cumulative percentage |
| ANOVA: Single Factor | the groups summarised, then SS, df, MS, F and P |
| ANOVA: Two Factor | the input read as a table: the rows summarised, then the columns, then SS, df, MS, F and P for each factor |
| Sampling | a sample of each variable |
| Rank and Percentile | every value with its rank and percentile |
| Moving Average | the average over a chosen number of terms |

Each writes a labelled table of ordinary cells wherever you point it,
in one undo step.

The number box under the ranges serves whichever tool wants one: the
bins of a histogram, the terms of a moving average, the rows to a
sample of a two-factor analysis (leave it at 0 or 1 for a table with
one value per cell, and the interaction between the two factors is
worked out when it is more), or how many values a sample draws.
Sampling draws at random with replacement unless **Sample every nth
value** is ticked, in which case the number is the period.

## 11. Charts

Select the table, including its headings, and press the ChartWizard
button (or Insert ▸ Chart). The wizard asks for the kind -- column,
stacked column, 100% stacked column, bar, line, area, pie, XY scatter,
doughnut, radar, bubble, stock, surface, box and whiskers, histogram,
polar or contour -- for a title, for whether
the first row and
column are headings, for whether the series lie down the columns or
along the rows, and whether the chart goes on this sheet or on a sheet
of its own.

Click a chart and Format ▸ Chart sets:

- the title and the two axis titles;
- the value axis' minimum, maximum and number format;
- whether the legend, the gridlines and the data labels show;
- three dimensions, which draws each bar as a solid and a pie as an
  ellipse on a wall;
- for a pie, a **pie of pie** or **bar of pie**: the last few slices
  are gathered into one slice called Other and shown again, at their
  own scale, in a second pie or a stacked bar beside the first, with
  lines joining the two. The number of slices to move is yours to set;
- a **trendline** through every series: linear, polynomial of order two
  to six, exponential, logarithmic, power, or a moving average over a
  period;
- **error bars**: a fixed amount, a percentage, a multiple of the
  series' standard deviation, or the standard error of its mean;
- the **marker** drawn at each point of a line, scatter or radar
  chart -- a circle, square, diamond, triangle, cross, plus or star, at
  a size of your choosing, or none at all -- or a **picture**: give the
  number of a picture on the sheet and it is drawn at every point, and
  in a column or bar chart it fills the bars themselves;
- the face and size of the chart's text;
- which series to plot against a **second value axis** down the right.

**Stock charts.** Three series are read as high, low and close, and
drawn as Excel draws them: a line from the low to the high with a tick
to the right at the close. Four series are open, high, low and close,
and become candles -- hollow on a day that rose, filled on one that
fell.

**Surface charts.** The table is read as a height field: a value for
every crossing of a category and a series. It is drawn as a mesh seen
from the front, coloured in bands from blue at the bottom through
green and yellow to red at the top, the way a map colours height.

A chart is a picture of some cells: it is redrawn from them every time
it is painted, so it is never out of date. Charts print, export to PDF,
and travel in `.gnumeric`, `.xlsx` and `.ods`.

**Chart sheets.** A chart on a sheet of its own fills the window and
prints one to a page. It plots another sheet's cells, since it has none
of its own.

## 12. Pictures, shapes, notes and links

- **Insert ▸ Picture ▸ From File** floats a picture over the grid,
  anchored to the active cell. Click to select, drag to move, drag a
  handle to resize, Delete to remove.
- **Insert ▸ Picture ▸ From Scanner or Camera** asks the scanner --
  through Windows' own acquire dialog, or SANE's `scanimage` elsewhere
  -- and puts what it gives where a picture from a file would go.
- **Insert ▸ Shape** puts a rectangle, an oval, a line, an arrow, a
  text box or any of Excel's AutoShapes -- rounded rectangle, triangles,
  diamond, pentagon, hexagon, octagon, cross, stars, block arrows,
  callouts, flowchart symbols -- over the grid. Format ▸ Shape sets its
  text, fill, line colour, line width, dash, the heads at either end of
  a line, its rotation and flips. Format ▸ Order brings an object to the
  front or sends it back, a step at a time or all the way. Format ▸
  Picture crops a picture and turns it.
- **Insert ▸ Control** puts a form control on the sheet: a button, a
  check box, an option button, a spinner, a scroll bar, a list box, a
  combo box, a label or a group box. See the next section.
- **Shift+click** adds an object to the selection; a set is dragged,
  deleted, ordered or grouped together. The handle on a stalk above a
  shape turns it (Shift snaps to 15 degrees); Shift on a corner keeps
  the proportions. Insert ▸ Shape ▸ Freeform draws an outline click by
  click, double-click ending it.
- **Format ▸ Shape** has tabs: Colors and Lines (a solid, gradient or
  pattern fill, a shadow, the line, its dash and heads), Size (width,
  height, rotation, flips), Text (font, size, colour, alignment, wrap,
  inset) and Properties -- whether the object moves and sizes with the
  cells under it, only moves, or stays put, as Excel's Format dialog
  asks. Format ▸ Picture crops, turns, and sets brightness and contrast.
- **Format ▸ Group Objects** puts everything anchored in the selection
  into a group: dragging one then moves them all. Ungroup takes them
  apart.
- **Insert ▸ Note** (Shift+F2) attaches a note to a cell, marked with a
  small red triangle and shown as a tooltip. View ▸ Comments shows
  every note on the sheet at once, each in its box beside its cell, and
  again to put them away.
- **Insert ▸ Hyperlink** makes a cell a link, to a place in the book
  (`#Sheet2!A1`) or to the world outside. Ctrl+click follows it.

### Form controls

A control drives one cell. It keeps nothing of its own: what it shows
is whatever that cell says, so a formula and a check box are two views
of one number.

**Format ▸ Control** sets what each needs -- Ctrl+click the control
first, since a plain click works it:

| Control | Its cell | Other settings |
|---|---|---|
| Check box | TRUE or FALSE | caption |
| Option button | its own number, so a set sharing one cell chooses between them | caption |
| Spinner | a number between the bounds | minimum, maximum, increment |
| Scroll bar | the same, and the thumb can be dragged | minimum, maximum, increment, page change |
| List box | the number of the row you picked | input range |
| Combo box | the same, from a dropdown | input range |
| Button | nothing; it runs a script | caption, script name |
| Label, group box | nothing | caption |

So `=INDEX(A1:A3,B2)` beside a list box on `B2` names what was
chosen, and a check box on `B3` makes `=IF(B3,...)` follow it.

A plain click works a control; Ctrl+click takes hold of it, to move it,
resize it by its handles, or open Format ▸ Control. Delete removes the
one you are holding.

Controls are kept in every format that holds a sheet whole:
`.gnumeric`, `.xlsx`, `.xls` and `.ods`. In `.xlsx` they go into the
sheet's legacy drawing the way Excel writes them; in `.xls` they are
Escher shapes with an OBJ record apiece, which is where Excel 97 put
them; in `.ods` they are an ODF form with a `draw:control` for each. A
book saved any of those ways opens with its controls in Excel and in
LibreOffice, and comes back here with its links, its bounds and its
captions.

## 13. Spelling

Tools ▸ Spelling walks the text in the selection, or the whole sheet
when nothing in particular is selected, and stops at every word the
dictionary does not know, offering what it might have been: Change,
Ignore, Ignore All.

It spells with Hunspell and the dictionaries already on the machine,
falling back to English when there is none for the machine's own
language. Built without Hunspell, or run where no dictionary can be
found, the menu item says so and nothing else changes.

## 14. Printing and PDF

File ▸ Page Setup is Excel's dialog, four tabs over the sheet's own
setup, which is kept in the file whatever the format:

- **Page**: portrait or landscape; the paper (Letter, Legal, A3, A4,
  A5, B4, B5 and the rest, A4 unless the locale prints on Letter);
  a scale, or a number of pages wide and tall to fit into; the number
  the first page has.
- **Margins**: the six of them -- left, right, top, bottom, and where
  the header and the footer stand -- in centimetres, or inches when
  the paper is Letter; and whether the cells are centred on the page
  horizontally, vertically, or both.
- **Header/Footer**: in Excel's notation. `&L`, `&C` and `&R` start
  the three parts; `&P` is the page number (`&P+1` the next), `&N` the
  count, `&D` the date, `&T` the time, `&F` the file, `&A` the sheet;
  `&B`, `&I`, `&U`, `&S` turn bold, italic, underline and strikeout on
  and off, `&14` sets a size, `&"Arial,Bold"` a font, `&K00FF00` a
  colour, `&&` is an ampersand. Custom Header and Custom Footer open
  Excel's dialog: the three sections as boxes, and buttons that put the
  font, page number, page count, date, time, file name or sheet name at
  the caret. View ▸ Header and Footer opens this tab.
- **Sheet**: the print area (the used range when there is none) --
  several ranges a comma apart, `A1:C8,E1:F3`, each printed on pages of
  its own; rows to repeat at the top (`$5:$6`) and columns to repeat at
  the left (`$B:$C`) of every page;
  gridlines; row and column headings; black and white; draft quality
  (no fills, gridlines or graphics); how notes print -- not at all, on
  pages of their own after the sheet, or beside their cells; what cell
  errors print as (as shown, blank, `--` or `#N/A`); and the page order,
  down then over (Excel's default) or over then down.

File ▸ Set Print Area limits printing to the selection; Clear Print
Area gives it back. Insert ▸ Page Break puts a break above and left of
the active cell, or only the one when a whole row or column is
selected. View ▸ Page Breaks is Excel's Page Break Preview: what is not
printed is greyed, each print area edged in blue, the pages divided by
dashed lines that can be dragged, and "Page N" written across each.

File ▸ Print Preview shows the pages one at a time, fitted to the
window or at life size (Zoom), with the margins as dotted lines when
asked -- drag one and the margin moves -- and Setup opens Page Setup
from it; Print Preview: Book pages
through every sheet. File ▸ Print goes through the system's print
dialog, which offers the selection as well as the sheet; File ▸ Print
Book does every sheet of the book, one after another, each on its own
paper. Print, preview and PDF lay the page out the same way to the
point.

File ▸ Export as PDF writes the sheet, and Export Book as PDF the whole
book, drawing exactly what the printer would.

File ▸ Import from PDF reads a PDF back in, laying each page's text out
in cells by position -- useful for a statement or a report that arrived
as a PDF. It needs poppler, and says so if the build has none.

## 15. Files

File ▸ New from Template starts an untitled book from one of the
templates that came with the program -- a loan amortization schedule,
an invoice, an expense report, a balance sheet -- or from a `.gnumeric`
of your own in the templates folder.

File ▸ Open and Save As choose the format by the name you give, and
the four files most recently opened or saved wait at the bottom of the
File menu, newest first, to be opened with a click:

| Extension | What it is | What travels |
|---|---|---|
| `.gnumeric` | Gnumeric's own, gzipped XML | everything office42 has, the properties included, and the things no other format holds |
| `.xlsx`, `.xlsm` | Excel 2007 and later | cells, formulas, formats, styles, rich text, merges, notes, links, tables, scenarios, filters, validations, conditional formats, charts, shapes, pictures, print setup, protection, chart sheets, hidden sheets, custom views, scripts, the standard column width, the properties, the workbook protection, sheet backgrounds; an `.xlsm`'s Visual Basic is kept for Excel, not run |
| `.xls` | Excel 97 to 2003, BIFF8 (and the older BIFF5 read) | cells, formulas as Excel's own tokens, formats, rich text, merges, notes, links, validations, conditional formats, filters, print setup, pictures, shapes, charts, form controls, hidden sheets, the view, the properties |
| `.ods`, `.fods` | OpenDocument, LibreOffice Calc's own, zipped or flat | cells, formulas in OpenFormula, formats, rich text, merges, notes, names, validations, frozen panes, print setup, pictures, shapes, charts, form controls, the properties |
| `.html` | a table per sheet | values, fonts, fills, borders, alignments, merges, links |
| `.csv` | comma separated | the values as shown, quoted where they need it |
| `.dif` | VisiCalc's Data Interchange Format | one sheet: the numbers as numbers and everything else as text |
| `.slk` | Multiplan's SYLK, which Excel still offers | one sheet: values, formulas in R1C1, column widths |
| `.tex` | export only | a LaTeX tabular of what the cells show, bold and italic kept |
| `.wk1` | Lotus 1-2-3 release 2 | one sheet: labels and numbers, and what a formula last worked out |
| `.pdf` | export only (import with poppler) | the printed pages |

File ▸ Properties holds what Excel's Summary tab holds -- title,
subject, author, manager, company, category, keywords and comments --
and every format that can carries them. A `.gnumeric` keeps them as
Gnumeric does, in `office:document-meta`; an `.xlsx` in
`docProps/core.xml` and `app.xml`; an `.ods` in `meta.xml`; an `.xls`
in the SummaryInformation and DocumentSummaryInformation streams Excel
keeps them in.

Every one of them but LaTeX and PDF is read as well as written, and
each has been checked both ways against the program it belongs to --
Excel's formats against LibreOffice, `.gnumeric` against Gnumeric's own
documentation.

One wart worth knowing: LibreOffice writes a DIF number with the
machine's decimal separator, where the format and Excel both write a
point. office42 writes the point and reads either, so a DIF from
LibreOffice comes in whole; a DIF from office42 opened in LibreOffice
under a comma locale shows its decimals as text.

## 16. Python and the macro recorder

office42's macro language is Python, embedded in the program.

**The console.** Tools ▸ Python Console gives a prompt with the book
and the sheet already bound:

```python
>>> sheet["A1"].value = 42
>>> sheet["A2"].formula = "=A1*2"
>>> sheet["A1:B3"].values = [[1, 2], [3, 4], [5, 6]]
>>> sheet["A1:B1"].format(bold=True, fill="#ffff99")
>>> sum(sheet["A1:A3"])
9.0
```

`office42.book` is the book, `office42.sheet` the sheet on show,
`book["Name"]` a sheet by name, and a `Range` has `value`, `values`,
`formula`, `text`, `format(...)`, `clear()` and the usual Python
iteration.

**Functions of your own.** A function decorated with
`@office42.function` joins the evaluator and can be called from any
cell:

```python
@office42.function
def MARKUP(cost, rate=0.2):
    return cost * (1 + rate)
```

**Scripts in the book.** Tools ▸ Scripts in this Book keeps Python in
the file itself, so a book carries its own macros. A book that arrives
with scripts shows a bar across the top asking whether to enable them;
nothing runs until you say so.

**`=PY()`.** A cell can call Python directly: `=PY("sum(range(10))")`.

**The macro recorder.** Tools ▸ Record Macro writes down what you do as
the Python that does it again, and stops into a script in the book.
What is recorded is what the Python API can put back -- the text typed
into cells and the formats applied to them, wherever they came from --
so inserting a row is recorded as the cells it moved, which replays to
the same sheet at the cost of a line per cell.

## 17. The database

A book can have one SQLite database: a file beside it, or one carried
inside the book so that it travels with it. SQLite is a whole database
in a single file, with no server to run.

**Data ▸ Database ▸ Connect** opens a `.sqlite`, `.sqlite3` or `.db`
file and remembers where it is. **Embed New Database** makes an empty
one inside the book instead; it is written into the `.gnumeric` file
when you save, and unpacked again when you open it, so the book and
its data are one thing.

**Data ▸ Database ▸ Get Data** lists the tables, takes a query, and
lays the answer out from the active cell, with the column names above
it if you ask. Text arrives as text, numbers as numbers, and NULL as
an empty cell. The sheet remembers the query and where its answer
went, so **Refresh Queries** runs every one of them again and lays the
answers out afresh -- formulas over the answer follow, as any formula
does.

**Data ▸ Database ▸ Send Selection to Table** goes the other way: the
selected rows are added to a table, which is made from the first row's
headings if it is not there yet.

**`SQLVALUE(query, column, row)`** asks the same database a question
from inside a cell and answers with one cell of what it says --
`=SQLVALUE("SELECT SUM(amount) FROM sales")`. The column and the row
say which cell of the answer to take, both counting from one and both
1 by default. A whole table does not fit in a cell, which is what Get
Data is for. It says `#N/A` when the book has no database and
`#VALUE!` when the query is wrong.

Only `.gnumeric` carries the database and the queries; a book saved as
`.xlsx` or `.xls` keeps the values that were laid out and forgets where
they came from. If office42 was built without SQLite, the menu says so
and `SQLVALUE` is not there.

## 18. Protecting a sheet

Format ▸ Cells ▸ Protection marks cells locked (which they all are to
begin with, as in Excel) or hidden. Neither does anything until Tools ▸
Protection ▸ Protect Sheet is on; then a locked cell refuses to be typed into and a
hidden cell's formula does not show in the formula bar.

This guards the window, not the file: a script or the terminal
front-end can still write, as they can in any spreadsheet whose
protection is not a password on the bytes. `.xlsx` and `.gnumeric`
carry the flag.

## 19. office42-calc, the terminal front-end

`office42-calc` drives the same engine with no window, reading commands
from standard input. It exists so the engine can be exercised on its
own -- and it is how nearly everything in office42 is tested.

```sh
printf 'A1 = 10\nB1 = =A1*2\nB1\n' | office42-calc
B1	20	(=A1*2)
```

A line with `=` sets a cell, a bare reference shows one, and `dump`
prints the used range. The commands, by family:

| Family | Commands |
|---|---|
| Cells | `dump`, `copy`, `paste`, `filldown`, `fillright`, `fillup`, `fillleft`, `fillacross`, `series`, `justify`, `autofill`, `moverange`, `merge`, `unmerge`, `merges`, `insertrows`, `deleterows`, `insertcols`, `deletecols`, `insertcells`, `deletecells`, `array` |
| Formats | `format`, `font`, `fontinfo`, `border`, `pattern`, `rich`, `runs`, `indent`, `rotate`, `fmtinfo`, `style`, `styles`, `defstyle`, `styleat`, `autoformat`, `cond`, `conds`, `uncond`, `customlist`, `customlists` |
| Sheets | `sheet`, `rename`, `delsheet`, `copysheet`, `background`, `tabcolour`, `freeze`, `split`, `hiderows`, `unhiderows`, `hidecols`, `unhidecols`, `levels`, `group`, `ungroup`, `autooutline`, `clearoutline`, `detail`, `outlinelevel`, `protect`, `lock`, `hide`, `editable`, `chartsheet` |
| Data | `sort`, `find`, `replace`, `filter`, `advfilter`, `subtotal`, `unsubtotal`, `dedupe`, `consolidate`, `table`, `tables`, `untable`, `pivot`, `refresh`, `validate`, `validations`, `unvalidate`, `goalseek`, `solve`, `scenario`, `scenarios`, `showscenario`, `delscenario`, `summary`, `analyse`, `whatif`, `split`, `splitfixed`, `autofilter` |
| Objects | `chart`, `charts`, `chartset`, `chartinfo`, `shape`, `shapes`, `controlset`, `click`, `picture`, `pictures`, `objgroup`, `objungroup`, `note`, `link`, `links`, `pictureset`, `objects`, `order` |
| Files | `load`, `save`, `pdf`, `pdfbook`, `printarea`, `printscale`, `printsetup`, `printopt`, `pagebreak`, `margin`, `header`, `footer`, `titlerows`, `pageopt`, `titlecols` |
| Python | `py`, `pyfile`, `script`, `scripts`, `runscript`, `delscript`, `record`, `select` |
| Database | `db`, `dbembed`, `dbtables`, `dbcols`, `dbexec`, `sql`, `sqlprint`, `dbput`, `dbrefresh`, `queries` |
| Other | `undo`, `redo`, `name`, `names`, `unname`, `createnames`, `applynames`, `prop`, `props`, `autocorrect`, `correction`, `uncorrect`, `corrections`, `autocorrectopt`, `spell`, `view`, `views`, `shown`, `calcmode`, `iterate`, `recalc`, `evaluate`, `watch`, `watches`, `unwatch`, `check`, `date1904`, `precision`, `fixeddecimals` |

`office42-calc --functions` prints every function with its signature
and a line about what it does; `--help` prints the commands by family
and `--version` the version.

## 20. Keyboard reference

| Key | What it does |
|---|---|
| Ctrl+N, Ctrl+O, Ctrl+S | new, open, save |
| Ctrl+Shift+S | Save As |
| Ctrl+P | print |
| Ctrl+W, Ctrl+Q | close the window, quit |
| Ctrl+Z, Ctrl+Y | undo, redo |
| Ctrl+X, Ctrl+C, Ctrl+V | cut, copy, paste |
| Ctrl+Shift+V | Paste Special |
| Ctrl+A | select all |
| Ctrl+D, Ctrl+R | fill down, fill right |
| Ctrl+B, Ctrl+I, Ctrl+U | bold, italic, underline |
| Ctrl+1 | Format Cells |
| Ctrl+F, Ctrl+H | find, replace |
| F5 | Go To |
| F2 | edit the cell in place |
| F4 | cycle the dollars on the reference at the caret |
| Tab | while the function list is up, take the name it highlights |
| Shift+F2 | insert a note |
| Shift+F3 | the Function Wizard |
| Ctrl+F3 | define a name |
| Ctrl+Page Up / Down | previous / next sheet |
| F1 | this program's short help |
| F9 | work everything out again |
| F11 | full screen |
| Ctrl+Shift+Enter | enter a formula over the whole selection |
| Ctrl+Shift+~ 1 2 3 4 5 6 | General, Comma, Time, Date, Currency, Percent, Scientific formats |
| Alt+F8 | the Macros dialog |
| Ctrl+Shift+letter | run the macro given that letter in Macro Options |
| Escape | cancel the edit |

---

If something here does not match what the program does, the program is
right and this guide is wrong; please report it.
