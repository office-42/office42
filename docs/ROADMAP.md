# Roadmap

The engine, the window and the file formats exist. What is left is
written here in the order it is being done, and
[PARITY.md](PARITY.md) says why -- it is a review of the code and of
the distance to Excel and to Gnumeric, and this list is its last
section spelled out.

## Next

**More of the format language.** The locale codes in `[$-409]`.

**More of `.ods`.** It reads more than it writes.

## After that

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
