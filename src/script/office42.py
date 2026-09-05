# office42.py - the Python half of the office42 module
#
# Copyright (C) 2026 The office42 authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Book, Sheet and Range over the dozen calls in _office42.  Rows and
# columns are 0-based in the tuple forms and A1-style in the text
# forms, as they are in the program.  Compiled into the executable as
# a string; edit here, rebuild.

"""Office42 Spreadsheet from Python.

    >>> import office42
    >>> sheet = office42.sheet                # the sheet on show
    >>> sheet["A1"].value = 42
    >>> sheet["A2"].formula = "=A1*2"
    >>> sheet["A2"].value
    84.0
    >>> sheet["A1:B3"].values = [[1, 2], [3, 4], [5, 6]]
    >>> sum(sheet["A1:A3"])
    9.0
    >>> sheet["A1:B1"].format(bold=True, fill="#ffff99")

    >>> @office42.function
    ... def DOUBLE(x):
    ...     return 2 * x
    >>> sheet["C1"].formula = "=DOUBLE(21)"
"""

import io
import sys
import traceback
import functools
from contextlib import redirect_stdout, redirect_stderr

import _office42 as _c

__all__ = ["Book", "Sheet", "Range", "Error", "book", "sheet", "function",
           "evaluate", "functions"]


class Error:
    """A spreadsheet error value: office42.Error("#N/A")."""

    def __init__(self, text="#VALUE!"):
        self.text = text

    def __repr__(self):
        return "Error(%r)" % self.text

    def __str__(self):
        return self.text

    def __eq__(self, other):
        return isinstance(other, Error) and other.text == self.text

    def __hash__(self):
        return hash(self.text)

    def __bool__(self):
        return False


NA = Error("#N/A")
VALUE = Error("#VALUE!")


def _to_input(value):
    """The text to type into a cell for a Python value."""
    if value is None:
        return ""
    if isinstance(value, bool):
        return "TRUE" if value else "FALSE"
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, Error):
        return value.text
    return str(value)


def _shift_is_vertical(shift, vertical, horizontal):
    if shift == vertical:
        return True
    if shift == horizontal:
        return False
    raise ValueError("shift is %r or %r" % (vertical, horizontal))


class Range:
    """A rectangle of cells on a sheet."""

    __slots__ = ("sheet", "row0", "col0", "row1", "col1")

    def __init__(self, sheet, row0, col0, row1=None, col1=None):
        self.sheet = sheet
        self.row0, self.col0 = row0, col0
        self.row1 = row0 if row1 is None else row1
        self.col1 = col0 if col1 is None else col1
        if self.row1 < self.row0:
            self.row0, self.row1 = self.row1, self.row0
        if self.col1 < self.col0:
            self.col0, self.col1 = self.col1, self.col0

    # -- shape ---------------------------------------------------------
    @property
    def rows(self):
        return self.row1 - self.row0 + 1

    @property
    def cols(self):
        return self.col1 - self.col0 + 1

    @property
    def address(self):
        a = _c.ref_name(self.row0, self.col0)
        if self.rows == 1 and self.cols == 1:
            return a
        return a + ":" + _c.ref_name(self.row1, self.col1)

    def __repr__(self):
        return "<Range %s!%s>" % (self.sheet.name, self.address)

    def cell(self, i, j=0):
        """The cell i rows down and j across from the top left."""
        return Range(self.sheet, self.row0 + i, self.col0 + j)

    def row(self, i):
        return Range(self.sheet, self.row0 + i, self.col0, self.row0 + i, self.col1)

    def column(self, j):
        return Range(self.sheet, self.row0, self.col0 + j, self.row1, self.col0 + j)

    def offset(self, rows, cols):
        return Range(self.sheet, self.row0 + rows, self.col0 + cols,
                     self.row1 + rows, self.col1 + cols)

    def resize(self, rows, cols):
        return Range(self.sheet, self.row0, self.col0,
                     self.row0 + rows - 1, self.col0 + cols - 1)

    # -- values --------------------------------------------------------
    @property
    def values(self):
        """A list of rows, each a list of values (None for empty)."""
        i = self.sheet.index
        return [[_c.get_value(i, r, c) for c in range(self.col0, self.col1 + 1)]
                for r in range(self.row0, self.row1 + 1)]

    @values.setter
    def values(self, rows):
        i = self.sheet.index
        _c.begin(i)
        try:
            if not isinstance(rows, (list, tuple)):
                rows = [[rows]]
            elif rows and not isinstance(rows[0], (list, tuple)):
                rows = [list(rows)] if self.rows == 1 and self.cols > 1 else [[v] for v in rows]
            for r, line in enumerate(rows):
                for c, v in enumerate(line):
                    if self.row0 + r <= self.row1 and self.col0 + c <= self.col1 or (self.rows == 1 and self.cols == 1):
                        _c.set_input(i, self.row0 + r, self.col0 + c, _to_input(v))
        finally:
            _c.end(i)

    @property
    def value(self):
        """One value for a single cell, the rows for a larger range."""
        if self.rows == 1 and self.cols == 1:
            return _c.get_value(self.sheet.index, self.row0, self.col0)
        return self.values

    @value.setter
    def value(self, v):
        if isinstance(v, (list, tuple)):
            self.values = v
        else:
            i = self.sheet.index
            _c.begin(i)
            try:
                for r in range(self.row0, self.row1 + 1):
                    for c in range(self.col0, self.col1 + 1):
                        _c.set_input(i, r, c, _to_input(v))
            finally:
                _c.end(i)

    @property
    def text(self):
        """What the cell shows, formatted."""
        i = self.sheet.index
        if self.rows == 1 and self.cols == 1:
            return _c.get_display(i, self.row0, self.col0)
        return [[_c.get_display(i, r, c) for c in range(self.col0, self.col1 + 1)]
                for r in range(self.row0, self.row1 + 1)]

    @property
    def formula(self):
        """What was typed: the formula with its '=', or the value's text."""
        i = self.sheet.index
        if self.rows == 1 and self.cols == 1:
            return _c.get_input(i, self.row0, self.col0)
        return [[_c.get_input(i, r, c) for c in range(self.col0, self.col1 + 1)]
                for r in range(self.row0, self.row1 + 1)]

    @formula.setter
    def formula(self, text):
        i = self.sheet.index
        _c.begin(i)
        try:
            if isinstance(text, (list, tuple)):
                for r, line in enumerate(text):
                    for c, t in enumerate(line):
                        _c.set_input(i, self.row0 + r, self.col0 + c, str(t))
            else:
                for r in range(self.row0, self.row1 + 1):
                    for c in range(self.col0, self.col1 + 1):
                        _c.set_input(i, r, c, str(text))
        finally:
            _c.end(i)

    def formula_from(self, text, origin):
        """Puts a formula in as if copied from `origin` (a cell address
        or Range): its relative references move by the distance.  What
        a macro recorded with relative references writes."""
        row, col = self._cell_of(origin)
        self.formula = _c.relocate_formula(str(text), self.row0 - row, self.col0 - col)

    def clear(self):
        """Empties the cells, keeping their formats."""
        _c.clear_range(self.sheet.index, self.row0, self.col0, self.row1, self.col1, False)

    def clear_formats(self):
        _c.clear_range(self.sheet.index, self.row0, self.col0, self.row1, self.col1, True)

    # -- copying, moving, filling --------------------------------------
    def _cell_of(self, to):
        """The top-left cell a target names: a Range, "D1", or (row, col)."""
        if isinstance(to, Range):
            return to.row0, to.col0
        if isinstance(to, str):
            parsed = _c.ref_parse(to.strip().split(":")[0])
            if parsed is None:
                raise ValueError("not a cell: %r" % to)
            return parsed[0], parsed[1]
        return to[0], to[1]

    def copy(self, to, mode="all", transpose=False):
        """Copies the range so that its corner lands on `to`: everything,
        or only "values", "formats" or "formulas" (Paste Special)."""
        row, col = self._cell_of(to)
        _c.copy_range(self.sheet.index, self.row0, self.col0, self.row1, self.col1,
                      row, col, mode, transpose)

    paste_special = copy

    def cut(self, to):
        """Moves the range to `to`, formulas elsewhere following it."""
        row, col = self._cell_of(to)
        _c.move_range(self.sheet.index, self.row0, self.col0, self.row1, self.col1, row, col)

    def fill_down(self):
        """The first row copied into every other row of the range."""
        _c.fill(self.sheet.index, self.row0, self.col0, self.row1, self.col1, True)

    def fill_right(self):
        _c.fill(self.sheet.index, self.row0, self.col0, self.row1, self.col1, False)

    def autofill(self, target):
        """Continues the range's series over `target`, which contains it."""
        t = target if isinstance(target, Range) else self.sheet.range(target)
        _c.autofill(self.sheet.index, self.row0, self.col0, self.row1, self.col1,
                    t.row0, t.col0, t.row1, t.col1)

    # -- sorting, finding, filtering -----------------------------------
    def sort(self, keys=0, ascending=True, header=False):
        """Sorts the rows by up to three key columns, counted from the
        range's left; `ascending` is one bool or one per key."""
        _c.sort(self.sheet.index, self.row0, self.col0, self.row1, self.col1, keys, ascending, header)

    def replace(self, old, new, match_case=False):
        """Replaces text in the cells' inputs; how many cells changed."""
        return _c.replace(self.sheet.index, self.row0, self.col0, self.row1, self.col1, old, new, match_case)

    def find(self, text, match_case=False, whole_cell=False):
        """The first cell of the range holding the text, or None."""
        row, col = self.row0, self.col0 - 1
        if col < 0:
            row, col = row - 1, _c.ref_parse("XFD1")[1]
        first = None
        while True:
            # The search wraps round the sheet; seeing the first hit again
            # means nothing inside the range matched.
            hit = _c.find(self.sheet.index, text, match_case, whole_cell, row, col)
            if hit is None or hit == first:
                return None
            if first is None:
                first = hit
            row, col = hit
            if self.row0 <= row <= self.row1 and self.col0 <= col <= self.col1:
                return Range(self.sheet, row, col)

    def remove_duplicates(self, cols=None, header=False):
        """Removes rows equal in the given columns (counted from the
        range's left; all of them by default); how many went."""
        return _c.remove_duplicates(self.sheet.index, self.row0, self.col0, self.row1, self.col1, cols, header)

    def autofilter(self):
        """Puts the sheet's AutoFilter on this range, its first row headings."""
        _c.set_autofilter(self.sheet.index, self.row0, self.col0, self.row1, self.col1)

    # -- the window ----------------------------------------------------
    def select(self, active=None):
        """Selects the range in the window, with `active` (a cell address
        or Range inside it, the top-left by default) as the active cell."""
        row, col = (self.row0, self.col0) if active is None else self._cell_of(active)
        _c.select(self.sheet.index, self.row0, self.col0, self.row1, self.col1, row, col)
        return self

    def activate(self):
        """Makes the top-left cell the active cell, keeping the selection
        if it is inside it."""
        try:
            sel = _selection()
        except RuntimeError:
            sel = None
        if (sel is not None and sel.sheet == self.sheet and
                sel.row0 <= self.row0 <= sel.row1 and sel.col0 <= self.col0 <= sel.col1):
            _c.select(self.sheet.index, sel.row0, sel.col0, sel.row1, sel.col1, self.row0, self.col0)
        else:
            self.select()
        return self

    # -- the cells themselves ------------------------------------------
    def merge(self):
        """Makes the range one cell, keeping the top-left cell's content."""
        _c.merge(self.sheet.index, self.row0, self.col0, self.row1, self.col1)
        return self

    def unmerge(self):
        """Takes apart every merge the range touches."""
        _c.unmerge(self.sheet.index, self.row0, self.col0, self.row1, self.col1)
        return self

    @property
    def merged(self):
        """The merged range the top-left cell is in, or None."""
        r = _c.merged_at(self.sheet.index, self.row0, self.col0)
        return None if r is None else Range(self.sheet, *r)

    def insert_cells(self, shift="down"):
        """Insert > Cells: empty cells here, the rest moved down or right."""
        _c.shift_cells(self.sheet.index, self.row0, self.col0, self.row1, self.col1,
                       _shift_is_vertical(shift, "down", "right"), True)

    def delete_cells(self, shift="up"):
        """Edit > Delete: the cells go, and the rest move up or left."""
        _c.shift_cells(self.sheet.index, self.row0, self.col0, self.row1, self.col1,
                       _shift_is_vertical(shift, "up", "left"), False)

    def __iter__(self):
        """The values, row by row."""
        for line in self.values:
            for v in line:
                yield v

    def __len__(self):
        return self.rows * self.cols

    def __getitem__(self, key):
        if isinstance(key, tuple):
            return self.cell(*key)
        return self.cell(key // self.cols, key % self.cols)

    # -- formats -------------------------------------------------------
    def format(self, **properties):
        """Formats the range: bold, italic, underline, strikeout, wrap,
        borders (True, False or a style: 'thin', 'medium', 'thick',
        'double', 'dashed', 'dotted') with border_colour, or one side at
        a time as border_top / border_bottom / border_left /
        border_right and border_top_colour and so on; size (points),
        family, colour, fill (None for none), pattern (Excel's names:
        'solid', 'darkGray', 'mediumGray', 'lightGray', 'gray125',
        'gray0625', 'darkHorizontal' ... 'lightTrellis'; None for none)
        and pattern_colour, halign ('general',
        'left', 'centre', 'right'), valign ('bottom', 'middle', 'top'),
        number ('general', 'fixed', 'comma', 'currency', 'percent',
        'scientific', 'text', 'date', 'time', 'datetime', or a format
        code like '#,##0.00'), decimals, locked and hidden (for a
        protected sheet)."""
        _c.set_format(self.sheet.index, self.row0, self.col0, self.row1, self.col1, **properties)
        return self

    @property
    def style(self):
        """The top-left cell's format, as a dict."""
        return _c.get_format(self.sheet.index, self.row0, self.col0)


class Sheet:
    """One sheet of the book, by index."""

    __slots__ = ("index",)

    def __init__(self, index):
        self.index = index

    @property
    def name(self):
        return _c.sheet_name(self.index)

    @name.setter
    def name(self, text):
        _c.rename_sheet(self.index, text)

    def __repr__(self):
        return "<Sheet %r>" % self.name

    def __eq__(self, other):
        return isinstance(other, Sheet) and other.index == self.index

    def __hash__(self):
        return self.index

    def __getitem__(self, key):
        """sheet["A1"], sheet["A1:C5"], sheet[row, col] (0-based)."""
        if isinstance(key, tuple):
            if len(key) == 2:
                return Range(self, key[0], key[1])
            if len(key) == 4:
                return Range(self, *key)
            raise TypeError("a cell is (row, col), a range (row0, col0, row1, col1)")
        return self.range(key)

    def __setitem__(self, key, value):
        self[key].value = value

    def range(self, address):
        """A range from A1 or A1:C5 text; a whole row or column from 3 or C."""
        text = address.strip()
        if ":" in text:
            a, b = text.split(":", 1)
            first, second = _c.ref_parse(a.strip()), _c.ref_parse(b.strip())
            if first is None or second is None:
                raise ValueError("not a range: %r" % address)
            return Range(self, first[0], first[1], second[0], second[1])
        parsed = _c.ref_parse(text)
        if parsed is None:
            raise ValueError("not a cell: %r" % address)
        return Range(self, parsed[0], parsed[1])

    def cell(self, row, col):
        return Range(self, row, col)

    @property
    def used_range(self):
        """The rectangle with anything in it, or None for an empty sheet."""
        r = _c.used_range(self.index)
        return None if r is None else Range(self, *r)

    @property
    def rows(self):
        """The used rows as lists of values."""
        used = self.used_range
        return [] if used is None else Range(self, 0, 0, used.row1, used.col1).values

    def evaluate(self, formula):
        """The value of a formula on this sheet, without putting it in a cell."""
        return _c.evaluate(formula, self.index)

    # -- rows and columns ----------------------------------------------
    def insert_rows(self, at, count=1):
        """Inserts count empty rows before row `at` (0-based)."""
        _c.insert_rows(self.index, at, count)

    def delete_rows(self, at, count=1):
        _c.delete_rows(self.index, at, count)

    def insert_cols(self, at, count=1):
        """Inserts count empty columns before column `at` (0-based)."""
        _c.insert_cols(self.index, at, count)

    def delete_cols(self, at, count=1):
        _c.delete_cols(self.index, at, count)

    def row_height(self, row, height=None):
        """A row's height in pixels; with `height`, sets it."""
        return _c.row_height(self.index, row) if height is None else _c.row_height(self.index, row, int(height))

    def col_width(self, col, width=None):
        """A column's width in pixels; with `width`, sets it."""
        return _c.col_width(self.index, col) if width is None else _c.col_width(self.index, col, int(width))

    def hide_rows(self, first, last=None):
        _c.set_hidden(self.index, True, first, first if last is None else last, True)

    def unhide_rows(self, first, last=None):
        _c.set_hidden(self.index, True, first, first if last is None else last, False)

    def hide_cols(self, first, last=None):
        _c.set_hidden(self.index, False, first, first if last is None else last, True)

    def unhide_cols(self, first, last=None):
        _c.set_hidden(self.index, False, first, first if last is None else last, False)

    def row_hidden(self, row):
        return _c.hidden(self.index, True, row)

    def col_hidden(self, col):
        return _c.hidden(self.index, False, col)

    def freeze(self, rows=0, cols=0):
        """Freezes so many rows at the top and columns at the left; (0, 0) unfreezes."""
        return _c.frozen(self.index, rows, cols)

    @property
    def frozen(self):
        """(rows, cols) frozen at the top and left."""
        return _c.frozen(self.index)

    # -- finding and filtering -----------------------------------------
    def replace(self, old, new, match_case=False):
        """Replaces text in every cell of the sheet; how many changed."""
        return _c.replace(self.index, -1, -1, -1, -1, old, new, match_case)

    def find(self, text, match_case=False, whole_cell=False, after=None):
        """The next cell holding the text, in reading order from just
        after `after` (a Range or address; the sheet's start by default),
        wrapping round; None if there is none."""
        if after is None:
            row, col = -1, _c.ref_parse("XFD1")[1]
        else:
            r = after if isinstance(after, Range) else self.range(after)
            row, col = r.row0, r.col0
        hit = _c.find(self.index, text, match_case, whole_cell, row, col)
        return None if hit is None else Range(self, *hit)

    @property
    def autofilter(self):
        """The range the AutoFilter is on, or None."""
        r = _c.get_autofilter(self.index)
        return None if r is None else Range(self, *r)

    def clear_autofilter(self):
        _c.set_autofilter(self.index, -1, -1, -1, -1)

    def autofilter_choose(self, col, value):
        """Shows only the rows whose cell in column `col` (an absolute
        index) reads `value`; None for all of them again."""
        _c.autofilter_choose(self.index, col, value)

    def autofilter_choice(self, col):
        return _c.autofilter_choose(self.index, col)


class Book:
    """The book: its sheets, and the one on show."""

    def __repr__(self):
        return "<Book %r>" % [s.name for s in self.sheets]

    @property
    def sheets(self):
        return [Sheet(i) for i in range(_c.n_sheets())]

    def __len__(self):
        return _c.n_sheets()

    def __iter__(self):
        return iter(self.sheets)

    def __getitem__(self, key):
        if isinstance(key, int):
            if key < 0 or key >= _c.n_sheets():
                raise IndexError("no sheet %d" % key)
            return Sheet(key)
        i = _c.sheet_index(key)
        if i < 0:
            raise KeyError(key)
        return Sheet(i)

    @property
    def active(self):
        return Sheet(_c.current())

    def add_sheet(self, name, index=-1):
        return Sheet(_c.add_sheet(name, index))

    def _sheet(self, which):
        return which if isinstance(which, Sheet) else self[which]

    def remove_sheet(self, which):
        _c.remove_sheet(self._sheet(which).index)

    def rename_sheet(self, which, name):
        self._sheet(which).name = name

    def move_sheet(self, which, to):
        """Moves a sheet so that it is the `to`th tab (0-based)."""
        _c.move_sheet(self._sheet(which).index, to)

    @property
    def names(self):
        return [s.name for s in self.sheets]

    # -- scripts kept in the book, and so in its file -----------------
    @property
    def scripts(self):
        """The names of the scripts stored in the book."""
        return _c.scripts()

    def script(self, name):
        """The code of a stored script."""
        return _c.get_script(name)

    def set_script(self, name, code, shortcut=None, description=None):
        """Stores code in the book under a name; saved with the file.
        `shortcut` is a letter for Ctrl+Shift+letter, "" for none."""
        _c.set_script(name, code)
        if shortcut is not None or description is not None:
            key, about = _c.script_info(name)
            _c.script_options(name, key if shortcut is None else shortcut,
                              about if description is None else description)

    def script_info(self, name):
        """A stored script's (shortcut letter or "", description)."""
        return _c.script_info(name)

    def remove_script(self, name):
        return _c.remove_script(name)

    def run_script(self, name):
        """Runs a stored script in the console's namespace."""
        _bind()
        exec(compile(_c.get_script(name), name, "exec"), _namespace)

    # -- the file ------------------------------------------------------
    @property
    def path(self):
        """The file the book was opened from or saved to, or None."""
        return _c.path()

    def save(self):
        """Saves the book to its file; a book without one asks for it."""
        _c.save(None)

    def save_as(self, path):
        """Saves the book to `path`; the extension picks the format."""
        _c.save(str(path))


book = Book()
sheet = None      # bound before each run


class Application:
    """What Excel calls Application: the program around the book."""

    screen_updating = True      # kept for scripts that set it; nothing waits on it
    display_alerts = True

    def __repr__(self):
        return "<Application office42 %s>" % __version__

    @property
    def selection(self):
        return _selection()

    @property
    def active_cell(self):
        return _active_cell()

    @property
    def active_sheet(self):
        return book.active

    def calculate(self):
        """Works out every formula in the book now: F9."""
        _c.calculate()

    calculate_full = calculate

    @property
    def status(self):
        return None

    @status.setter
    def status(self, text):
        """The status bar's text; None or "" for the usual one."""
        _c.status("" if text is None else str(text))

    status_bar = status

    def msgbox(self, text):
        return msgbox(text)

    def inputbox(self, prompt, default=""):
        return inputbox(prompt, default)


app = Application()


def msgbox(text):
    """A message box, waited for."""
    _c.message(str(text))


def inputbox(prompt, default=""):
    """Asks the user for a line of text; None if they cancel."""
    return _c.input(str(prompt), str(default))


def open(path):
    """Opens a file in a window of its own."""
    _c.open(str(path))


def personal_folder():
    """The folder whose .py files run when Python starts, for every book."""
    return _c.personal_folder()


def _selection():
    i, r0, c0, r1, c1, _, _ = _c.selection()
    return Range(Sheet(i), r0, c0, r1, c1)


def _active_cell():
    i, _, _, _, _, row, col = _c.selection()
    return Range(Sheet(i), row, col)


def __getattr__(name):
    """office42.selection and office42.active_cell are asked of the
    window each time, which is why they are not plain names."""
    if name == "selection":
        return _selection()
    if name == "active_cell":
        return _active_cell()
    raise AttributeError("module 'office42' has no attribute %r" % name)


def evaluate(formula):
    """The value of a formula on the sheet on show."""
    return _c.evaluate(formula)


def functions():
    """Every function name a cell formula may use, built in or from a script."""
    return _c.function_names()


# ---- Functions for cells -----------------------------------------------

# A book's scripts define functions for that book; the personal scripts
# define them for every book.  The evaluator knows a name once; which
# function answers it is decided here, by the book on show.
_functions = {}             # name -> fn, the personal ones and PY
_book_functions = {}        # book id -> {name: fn}
_loading_personal = False


def function(f=None, *, name=None, min_args=None, max_args=None, summary=None):
    """Makes a Python function callable from a cell formula.

        @office42.function
        def NPV2(rate, flows): ...

    A range argument arrives as a list of rows; a single cell as a
    value.  The return value becomes the cell's value: a number, text,
    bool, None (empty) or office42.Error."""
    def register(fn):
        import inspect
        fname = (name or fn.__name__).upper()
        if _c.is_builtin(fname):
            raise ValueError("%s is a built-in function" % fname)
        sig = inspect.signature(fn)
        params = list(sig.parameters.values())
        lo = sum(1 for p in params if p.default is p.empty and p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD))
        hi = -1 if any(p.kind == p.VAR_POSITIONAL for p in params) else len(params)
        signature = "%s(%s)" % (fname, ", ".join(p.name for p in params))
        doc = summary or (fn.__doc__ or "").strip().split("\n")[0] or "A function from a script."
        owner = _c.book_id()
        if owner == 0 or _loading_personal:
            _functions[fname] = fn
        else:
            _book_functions.setdefault(owner, {})[fname] = fn
        _c.define(fname, lo if min_args is None else min_args, hi if max_args is None else max_args, signature, doc)
        return fn
    return register(f) if f is not None else register


def _lookup(name):
    """The function answering NAME for the book on show, if any."""
    fn = _book_functions.get(_c.book_id(), {}).get(name)
    return _functions.get(name) if fn is None else fn


def _defined_elsewhere(name, owner):
    return name in _functions or any(name in fns for who, fns in _book_functions.items() if who != owner)


def _call(name, args):
    """Called from C for =NAME(...) in a cell."""
    fn = _lookup(name)
    if fn is None:
        return Error("#NAME?")
    try:
        return fn(*args)
    except Exception:
        _errors.append("%s: %s" % (name, traceback.format_exc().strip().split("\n")[-1]))
        del _errors[:-20]
        return VALUE


_errors = []       # the last few tracebacks from cell functions


def errors():
    """What went wrong in functions called from cells, most recent last."""
    return list(_errors)


def _py_cell(code, *rest):
    """=PY("expression"): the expression's value, as Excel's PY does."""
    if not isinstance(code, str):
        return VALUE
    try:
        return eval(code, _namespace)
    except Exception:
        _errors.append("PY: %s" % traceback.format_exc().strip().split("\n")[-1])
        del _errors[:-20]
        return VALUE


_functions["PY"] = _py_cell
_c.define("PY", 1, 2, "PY(code, return_type)", "Evaluates a Python expression; the sheet is `sheet`.")


# ---- Running code ------------------------------------------------------

_namespace = {}


def _bind():
    global sheet
    sheet = Sheet(_c.current())
    _namespace.setdefault("__name__", "__main__")
    _namespace.setdefault("__builtins__", __builtins__)
    _namespace["office42"] = sys.modules[__name__]
    _namespace["book"] = book
    _namespace["sheet"] = sheet
    _namespace["Error"] = Error
    _namespace["function"] = function


def _run(code, filename="<console>"):
    """Runs code in the console's namespace; (ok, what it printed)."""
    _bind()
    out = io.StringIO()
    ok = True
    with redirect_stdout(out), redirect_stderr(out):
        try:
            try:
                compiled = compile(code, filename, "eval")
            except SyntaxError:
                compiled = None
            if compiled is not None:
                result = eval(compiled, _namespace)
                if result is not None:
                    _namespace["_"] = result
                    print(repr(result))
            else:
                exec(compile(code, filename, "exec"), _namespace)
        except SystemExit:
            pass
        except BaseException:
            ok = False
            lines = traceback.format_exc().splitlines()
            # Leave out the runner's own frame.
            print("\n".join(l for l in lines if "office42.py" not in l))
    return ok, out.getvalue()


def _forget_book(owner):
    """The book is going, or its scripts are being forgotten."""
    for name in _book_functions.pop(owner, {}):
        if not _defined_elsewhere(name, owner):
            _c.undefine(name)


def _reset(owner):
    _forget_book(owner)
    _namespace.clear()
    del _errors[:]


# ---- Personal scripts --------------------------------------------------

personal_scripts = []      # (path, error or None), as loaded at start


def _load_personal(paths):
    """Runs each personal script once, each in a namespace of its own,
    so that what they define with @office42.function is there in every
    book.  A script that fails is noted, not fatal."""
    global _loading_personal
    del personal_scripts[:]
    _loading_personal = True
    try:
        for path in paths:
            try:
                with io.open(path, encoding="utf-8") as f:
                    code = f.read()
                space = {"__name__": "__personal__", "__file__": path,
                         "office42": sys.modules[__name__]}
                exec(compile(code, path, "exec"), space)
                personal_scripts.append((path, None))
            except Exception:
                trace = traceback.format_exc().strip().split("\n")[-1]
                personal_scripts.append((path, trace))
                _errors.append("%s: %s" % (path, trace))
    finally:
        _loading_personal = False
