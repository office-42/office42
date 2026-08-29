# Roadmap

The engine, the window and the file formats exist. What is left is
written here in the order it is being done, and
[PARITY.md](PARITY.md) says why -- it is a review of the code and of
the distance to Excel and to Gnumeric, and this list is its last
section spelled out.

## Next

**A grid widget the size of the window.** The sheet is now Excel
2007's: 1,048,576 rows by 16,384 columns, and a `.xlsx` written today
comes in whole. The widget, though, is still nominally as big as the
sheet, and cairo rasterises in 24.8 fixed point -- so the scrollbars
stop at about eight million pixels, which is around row 400,000. A cell
past that is in the sheet and is reached by name. Making the widget the
size of the window and scrolling it ourselves is what lifts the last
of it.

**Sheet tabs that can be dragged.** Right-clicking one moves it left or
right and two clicks rename it; the tab itself does not yet follow the
pointer, and Excel's tab colours are not there.

## After that

**The form controls in Excel's files.** They are in `.gnumeric`; `.xls`
wants them as Escher records with an OBJ record apiece, and `.xlsx` as
`xl/ctrlProps` parts beside the legacy drawing.

**A surface chart in `.xlsx`.** Excel's `surfaceChart` wants a third
axis element that office42 does not write yet, so a surface goes out as
the 3-D columns of the same numbers.

**More of the format language.** The locale codes in `[$-409]`.

**More of `.ods`.** It reads more than it writes.

**Splitting the two big files.** `o42-eval.c` is 14,000 lines and
`o42-window.c` 8,000; both split along obvious seams -- the evaluator
by function family, the window by dialog.

**Translations.** `menus.ui` marks its strings translatable and nothing
reads them: there is no gettext in the build and no `po/`.

## Not planned

**Macros in Visual Basic.** Excel 5 had Excel 4 macros and the first
Visual Basic. office42 runs Python instead, records it, and keeps it in
the file; see [PYTHON.md](PYTHON.md).

**The ribbon.** The shape of this program is Excel 5's: a menu bar and
two toolbars.

**Co-authoring, track changes, sharing.** A spreadsheet on one machine
is what this is.

**Power Query, the modern pivot engine, slicers, sparklines.** A book
can hold a SQLite database instead, and ask it from a cell; see
[DATABASE.md](DATABASE.md).
