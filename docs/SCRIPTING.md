# Scripting: which language, and how

A review of the languages that could give Office42 Spreadsheet what VBA
gives Excel — automation, user-defined functions and small programs over
the sheet — and a recommendation. Written 2026-08-28, before any of it
is built.

## What Excel offers, for comparison

- **VBA** (since 1993): in-process, an object model (`Workbook`,
  `Worksheet`, `Range`), macros stored in the file, a macro recorder,
  a "macros are disabled" bar on opening. Still the bulk of the world's
  spreadsheet automation.
- **Office Scripts** (2020): TypeScript, runs in the browser/cloud
  against a similar object model; Excel 365 only.
- **Python in Excel** (2023): `=PY("...")` in a cell, pandas and
  matplotlib, runs in Microsoft's cloud, results come back as values or
  images. Excel 365 only.

Gnumeric has Python (and Perl) plugins — functions written in Python
appear in the function list; LibreOffice has Basic, Python, JavaScript
and BeanShell, with Python the one people actually use.

## The candidates

| | Python (CPython) | JavaScript (QuickJS/Duktape) | Visual Basic | Lua |
|---|---|---|---|---|
| Licence | PSF (GPL-compatible) | MIT | none: would have to be written | MIT |
| Runtime on disk | ~15 MB (embeddable build) | ~1 MB | — | ~300 kB |
| Embeds from C | Python/C API, mature | C API, small and clean | — | C API, the simplest there is |
| Ecosystem the user gets | numpy, pandas, matplotlib, everything | npm, mostly unusable off-line | none | small |
| Who already knows it | most people who compute | web developers | Excel power users | game modders |
| Runs existing Excel macros | no | no | not without Excel's whole object model | no |
| Sandbox | none worth relying on | good (memory and time limits, no I/O unless given) | — | good |
| Precedent in spreadsheets | Excel 365, LibreOffice, Gnumeric | Excel 365 (Office Scripts) | Excel, LibreOffice Basic | none |

**Visual Basic** is the language Excel users know, but there is no open
source implementation to embed; writing one would be a project the size
of this one, and it would still not run real Excel macros, because
those are written against Excel's object model of some 200 classes.
LibreOffice spent twenty years on that and its VBA compatibility is
still partial. Not worth it.

**JavaScript** is the cheapest to embed and the safest to run: QuickJS
is one C library, ES2023 complete, and a script can be given a memory
cap, a time limit and nothing but the sheet. It is the right answer for
a program that must run untrusted scripts from files. But nobody
opens a spreadsheet to write JavaScript; the people who script
spreadsheets want pandas.

**Lua** is the smallest and simplest to embed and would fit the spirit
of a small C codebase perfectly, but it brings no libraries and few
users.

**Python** costs the most to ship (15 MB is more than the rest of the
program together) and cannot be sandboxed in-process, but it is what
the users want, what Excel itself chose for its newest scripting, and
what both open source spreadsheets settled on. The model and formula
layers here have no GTK in them and are plain C structs with a few
dozen entry points, which makes the binding small.

## Recommendation: Python, embedded, optional

*Done the same day: points 1–4 below are in; see docs/PYTHON.md.*

1. **Embed CPython in-process** through the stable ABI (`Py_LIMITED_API`,
   `python3.dll`), as Gnumeric's plugin does, behind a Meson option
   `-Dpython=enabled` so the core stays buildable without it. On
   Windows, ship the official embeddable distribution beside the
   executable; on Linux, link the system libpython.
2. **An `office42` module** written in C over the model: `book`,
   `sheets[]`, `sheet["A1"]`, `sheet["A1:C5"]`, `.value`, `.formula`,
   `.format`, `range.values` as nested lists, with numpy/pandas
   conversion done in a small pure-Python layer on top. Every setter
   goes through `o42_sheet_set_input` and friends, so scripts are
   undoable as one step, the same as a paste.
3. **Three doors in the window:**
   - Tools > Python Console: a REPL docked under the grid, `sheet` and
     `book` pre-bound. This is where most value is, and it is the
     smallest piece.
   - Tools > Run Script…: run a `.py` file against the open book.
   - `=PY("expr")` and user-defined functions: a decorated Python
     function `@office42.function` shows up in the FUNCTIONS list and
     in Insert Function, and is written to files as `_xlfn.` names
     would be, so a book that uses one still opens elsewhere with the
     last computed value.
4. **Scripts in the file**: a `scripts` element in `.gnumeric` (an
   extension, ignored by Gnumeric) and a custom part
   `xl/o42/scripts/*.py` in `.xlsx` (ignored by Excel). Never run on
   open; a bar like Excel's "Enable content" offers to run them.
5. **Not in scope**: VBA import, a macro recorder, sandboxing. Say so
   in the manual: a script can do what the user can do.

Order of work: the C module and the console first (usable in a day,
and it exercises the binding), then user-defined functions, then
scripts stored in files.
