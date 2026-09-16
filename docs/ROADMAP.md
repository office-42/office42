# Roadmap

The engine, the window and the file formats exist. What is left is
written here in the order it is being done, and
[PARITY.md](PARITY.md) says why -- it is a review of the code and of
the distance to Excel and to Gnumeric, and this list is its last
section spelled out.

## Next

**The last of Excel 97's menus.** Excel 97 is the target for feature
parity, and after the fifth pass what its menu bar has that this one
has not is short: Edit ▸ Fill ▸ Across Worksheets and Paste as
Hyperlink; View ▸ Toolbars, Formula Bar and Status Bar as things to
turn off; Format ▸ Sheet ▸ Background; Tools ▸ AutoCorrect and
Protection ▸ Protect Workbook; Insert ▸ Object and WordArt; the
pie-of-pie chart; the document properties in an `.xls`, whose
SummaryInformation stream is a property set of its own.  Each is a
day or less, and they are being done in that order.

**Splitting `o42-sheet.c`.** It is 14,013 lines, the biggest file of
all: cells and their formats, recalculation and the dependency graph,
and the objects that float over the grid are three subjects in one
file.

**Splitting `o42-grid.c`.** 8,448 lines: drawing, editing, selection
and dragging objects.

## Not planned

**Quattro Pro and Applix files.** The last two formats Gnumeric reads.
There is no file of either here and no program on this machine that
writes one, so a reader built from the description alone would go out
unverified; Lotus 1-2-3 could be done because LibreOffice reads it.

**Miltersen and Schwartz on commodity options.** It prices against a
three-factor model, and there is nothing here to check it against.

**Macros in Visual Basic.** Excel 97 had Visual Basic and its editor. office42 runs Python instead, records it, and keeps it in
the file; see [PYTHON.md](PYTHON.md).

**The ribbon.** The shape of this program is Excel 97's: a menu bar and
two toolbars.

**Co-authoring, track changes, sharing.** A spreadsheet on one machine
is what this is.

**Power Query, the modern pivot engine, slicers, sparklines.** A book
can hold a SQLite database instead, and ask it from a cell; see
[DATABASE.md](DATABASE.md).

**Excel 97's Office Assistant, Data Map, Web Query and Template
Wizard.** The first was a paperclip; the map needed data Microsoft
licensed and later dropped; a web query is a database query here; and
a template is a `.gnumeric` in the templates folder.

**Natural-language labels in formulas** -- `=Sales Total` meaning the
cell under "Sales" and beside "Total" -- which Excel 97 introduced and
Excel 2007 removed.  Insert ▸ Name ▸ Create makes the same names
explicitly, and they stay where a formula can see them.
