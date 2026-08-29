# Roadmap

The engine, the window and the file formats exist. What is left is
written here in the order it is being done, and
[PARITY.md](PARITY.md) says why -- it is a review of the code and of
the distance to Excel and to Gnumeric, and this list is its last
section spelled out.

## Next

**Splitting `o42-sheet.c`.** It is 8,580 lines and the biggest file
now that the evaluator and the window have come apart: cells and their
formats, recalculation and the dependency graph, and the objects that
float over the grid are three subjects in one file.

**Splitting `o42-grid.c`.** 5,400 lines: drawing, editing, selection
and dragging objects.

## Not planned

**Quattro Pro and Applix files.** The last two formats Gnumeric reads.
There is no file of either here and no program on this machine that
writes one, so a reader built from the description alone would go out
unverified; Lotus 1-2-3 could be done because LibreOffice reads it.

**Miltersen and Schwartz on commodity options.** It prices against a
three-factor model, and there is nothing here to check it against.

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
