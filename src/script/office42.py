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

__all__ = ["Book", "Sheet", "Range", "Chart", "Shape", "Picture", "Error", "book", "sheet", "function", "on", "off",
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

    # Excel's Range.NumberFormat, .Font.Bold and the like, as properties
    # that read the top-left cell and set the whole range.
    def _prop(name):
        def get(self):
            return self.style[name]
        def put(self, value):
            self.format(**{name: value})
        return property(get, put)

    number_format = _prop("number")
    bold = _prop("bold")
    italic = _prop("italic")
    underline = _prop("underline")
    strikeout = _prop("strikeout")
    wrap = _prop("wrap")
    size = _prop("size")
    family = _prop("family")
    colour = _prop("colour")
    fill = _prop("fill")
    halign = _prop("halign")
    valign = _prop("valign")
    decimals = _prop("decimals")
    locked = _prop("locked")
    del _prop

    # -- notes and links -----------------------------------------------
    @property
    def note(self):
        """The top-left cell's note, or None."""
        return _c.get_note(self.sheet.index, self.row0, self.col0)

    @note.setter
    def note(self, text):
        _c.set_note(self.sheet.index, self.row0, self.col0, text)

    @note.deleter
    def note(self):
        _c.set_note(self.sheet.index, self.row0, self.col0, None)

    @property
    def hyperlink(self):
        """The top-left cell's hyperlink -- a URL, or "#Sheet2!A1" -- or None."""
        return _c.get_link(self.sheet.index, self.row0, self.col0)

    @hyperlink.setter
    def hyperlink(self, target):
        _c.set_link(self.sheet.index, self.row0, self.col0, target)

    @hyperlink.deleter
    def hyperlink(self):
        _c.set_link(self.sheet.index, self.row0, self.col0, None)

    # -- rows and columns ----------------------------------------------
    @property
    def row_height(self):
        """The first row's height in pixels; setting it sets every row of the range."""
        return _c.row_height(self.sheet.index, self.row0)

    @row_height.setter
    def row_height(self, pixels):
        for r in range(self.row0, self.row1 + 1):
            _c.row_height(self.sheet.index, r, int(pixels))

    @property
    def column_width(self):
        """The first column's width in pixels; setting it sets every column of the range."""
        return _c.col_width(self.sheet.index, self.col0)

    @column_width.setter
    def column_width(self, pixels):
        for c in range(self.col0, self.col1 + 1):
            _c.col_width(self.sheet.index, c, int(pixels))

    @property
    def hidden(self):
        """Whether the range's rows are hidden (its first row's state)."""
        return _c.hidden(self.sheet.index, True, self.row0)

    @hidden.setter
    def hidden(self, hide):
        _c.set_hidden(self.sheet.index, True, self.row0, self.row1, bool(hide))

    def autofit(self):
        """Widens each column of the range to its longest shown text,
        as a double-click on the column's edge does."""
        i = self.sheet.index
        for c in range(self.col0, self.col1 + 1):
            longest = 0
            for r in range(self.row0, self.row1 + 1):
                longest = max(longest, len(_c.get_display(i, r, c) or ""))
            if longest:
                _c.col_width(i, c, min(7 * longest + 12, 1200))
        return self

    # -- validation and conditional formats ---------------------------
    @property
    def validation(self):
        """The validation rule on the top-left cell, as a dict (kind, op,
        value, value2, message, allow_blank, range), or None."""
        for v in _c.validations(self.sheet.index):
            r0, c0, r1, c1 = v["range"]
            if r0 <= self.row0 <= r1 and c0 <= self.col0 <= c1:
                v["range"] = Range(self.sheet, r0, c0, r1, c1)
                return v
        return None

    def validate(self, kind, op="between", value="", value2="", message="", allow_blank=True):
        """Data > Validation over the range: kind is 'whole', 'decimal',
        'list', 'date', 'time' or 'length' ('any' clears); op is
        'between', 'not_between', '==', '!=', '>', '<', '>=' or '<=';
        for a list, value holds the entries comma-separated or a range
        address.  Replaces the rules the range touched."""
        i = self.sheet.index
        _c.clear_validations(i, self.row0, self.col0, self.row1, self.col1)
        if kind != "any":
            _c.add_validation(i, self.row0, self.col0, self.row1, self.col1, kind, op,
                              str(value), str(value2), message or "", bool(allow_blank))
        return self

    def clear_validation(self):
        _c.clear_validations(self.sheet.index, self.row0, self.col0, self.row1, self.col1)
        return self

    @property
    def conditional_formats(self):
        """The rules touching the range: dicts with range, op, value,
        value2 and format (the dict Range.style gives)."""
        out = []
        for c in _c.conditions(self.sheet.index):
            r0, c0, r1, c1 = c["range"]
            if r0 <= self.row1 and r1 >= self.row0 and c0 <= self.col1 and c1 >= self.col0:
                c["range"] = Range(self.sheet, r0, c0, r1, c1)
                out.append(c)
        return out

    def add_conditional_format(self, op, value, value2=0, **format):
        """Format > Conditional Formatting: when a cell's value stands in
        op ('between', '>', '==', ...) to value (and value2 for between),
        it wears the format given as Range.format takes it."""
        _c.add_condition(self.sheet.index, self.row0, self.col0, self.row1, self.col1,
                         op, float(value), float(value2), **format)
        return self

    def clear_conditional_formats(self):
        _c.clear_conditions(self.sheet.index, self.row0, self.col0, self.row1, self.col1)
        return self


class _Object:
    """A chart, shape or picture floating over a sheet: its properties
    are read and written through the sheet, so they are always what the
    sheet holds."""

    __slots__ = ("sheet", "id")
    _type = ""

    def __init__(self, sheet, id):
        self.sheet = sheet
        self.id = id

    def __repr__(self):
        return "<%s %d on %r>" % (self._type.capitalize(), self.id, self.sheet.name)

    def __eq__(self, other):
        return type(other) is type(self) and other.id == self.id and other.sheet == self.sheet

    def __hash__(self):
        return hash((self._type, self.id))

    def _get(self):
        return _c.object_get(self.sheet.index, self._type, self.id)

    def _set(self, **props):
        _c.object_set(self.sheet.index, self._type, self.id, props)

    def __getattr__(self, name):
        props = self._get()
        if name in props:
            return props[name]
        raise AttributeError("a %s has no %s" % (self._type, name))

    def __setattr__(self, name, value):
        if name in ("sheet", "id") or isinstance(getattr(type(self), name, None), property):
            object.__setattr__(self, name, value)
        else:
            self._set(**{name: value})

    @property
    def position(self):
        """The anchor cell as a Range."""
        p = self._get()
        return Range(self.sheet, p["row"], p["col"])

    @position.setter
    def position(self, where):
        if isinstance(where, str):
            where = self.sheet.range(where)
        self._set(row=where.row0, col=where.col0, dx=0, dy=0)

    @property
    def size(self):
        """(width, height) in pixels."""
        p = self._get()
        return (p["width"], p["height"])

    @size.setter
    def size(self, wh):
        self._set(width=float(wh[0]), height=float(wh[1]))

    def bring_to_front(self):
        _c.reorder_object(self.sheet.index, self._type, self.id, "front")

    def send_to_back(self):
        _c.reorder_object(self.sheet.index, self._type, self.id, "back")

    def bring_forward(self):
        _c.reorder_object(self.sheet.index, self._type, self.id, "forward")

    def send_backward(self):
        _c.reorder_object(self.sheet.index, self._type, self.id, "backward")

    def delete(self):
        _c.remove_object(self.sheet.index, self._type, self.id)


class Chart(_Object):
    """A chart: kind ('column', 'line', 'pie', 'bar', 'area', 'scatter',
    'stacked', 'percent', 'doughnut', 'radar', 'bubble', 'stock',
    'surface', 'box', 'histogram', 'polar', 'contour'), title, x_title,
    y_title, legend, data (a Range), series_in_rows, first_row_labels,
    first_col_labels, data_labels, three_d, gridlines, font_family,
    font_size, y_format, position, size."""
    __slots__ = ()
    _type = "chart"

    @property
    def data(self):
        p = self._get()
        return Range(self.sheet, *p["data"])

    @data.setter
    def data(self, where):
        if isinstance(where, str):
            where = self.sheet.range(where)
        self._set(data=(where.row0, where.col0, where.row1, where.col1))


class Shape(_Object):
    """A drawn shape or a form control: kind, geom (the outline:
    'rect', 'roundRect', 'ellipse', 'triangle', 'diamond', 'star5',
    'rightArrow', ...), text, fill ('#RRGGBB' or None), line, line_width,
    dash, head_start, head_end, rotation, flip_h, flip_v, position, size;
    a control's link, source and script."""
    __slots__ = ()
    _type = "shape"


class Picture(_Object):
    """A picture: format, pixel_w, pixel_h, rotation, flip_h, flip_v,
    crop (left, right, top, bottom as fractions), lock_aspect,
    position, size."""
    __slots__ = ()
    _type = "picture"


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

    # -- charts, shapes and pictures ----------------------------------
    def _anchor(self, at):
        if isinstance(at, str):
            at = self.range(at)
        return at.row0, at.col0

    @property
    def objects(self):
        """Every chart, shape and picture, from the back to the front."""
        kinds = {"chart": Chart, "shape": Shape, "picture": Picture}
        return [kinds[t](self, i) for t, i in _c.objects(self.index)]

    @property
    def charts(self):
        return [o for o in self.objects if isinstance(o, Chart)]

    @property
    def shapes(self):
        return [o for o in self.objects if isinstance(o, Shape)]

    @property
    def pictures(self):
        return [o for o in self.objects if isinstance(o, Picture)]

    def add_chart(self, kind, data, at, width=None, height=None, title=None, **props):
        """Insert > Chart: a chart of `kind` over the cells of `data`
        (a Range or "A1:B5"), anchored at `at` (a Range or "D2")."""
        if isinstance(data, str):
            data = self.range(data)
        row, col = self._anchor(at)
        chart = Chart(self, _c.add_chart(self.index, kind, data.row0, data.col0,
                                         data.row1, data.col1, row, col))
        if width is not None:
            props["width"] = float(width)
        if height is not None:
            props["height"] = float(height)
        if title is not None:
            props["title"] = title
        if props:
            chart._set(**props)
        return chart

    def add_shape(self, kind, at, width=None, height=None, text=None, **props):
        """Insert > Shape: 'rectangle', 'oval', 'line', 'arrow',
        'textbox', an outline such as 'star5' or 'rightArrow', or a
        control ('button', 'checkbox', 'option', 'label', 'spinner',
        'scrollbar', 'listbox', 'groupbox', 'combo')."""
        row, col = self._anchor(at)
        shape = Shape(self, _c.add_shape(self.index, kind, row, col))
        if width is not None:
            props["width"] = float(width)
        if height is not None:
            props["height"] = float(height)
        if text is not None:
            props["text"] = text
        if props:
            shape._set(**props)
        return shape

    def add_picture(self, path, at, width=None, height=None, **props):
        """Insert > Picture > From File, anchored at `at`."""
        row, col = self._anchor(at)
        picture = Picture(self, _c.add_picture(self.index, path, row, col))
        pw, ph = picture.size
        if width is not None and height is None and pw > 0:
            height = float(width) * ph / pw       # in proportion
        if height is not None and width is None and ph > 0:
            width = float(height) * pw / ph
        if width is not None:
            props["width"] = float(width)
        if height is not None:
            props["height"] = float(height)
        if props:
            picture._set(**props)
        return picture

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

    def close(self):
        """Closes the book's window, asking about unsaved work first."""
        _c.close()


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

    def recalculate(self, full=True):
        """Every sheet worked out again; `full` is what Excel's
        CalculateFull is, and the only kind here."""
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

    display_alerts = True   # False keeps msgbox quiet, as Excel's does

    def msgbox(self, text):
        if self.display_alerts:
            return msgbox(text)

    def undo(self):
        """Edit > Undo: one step back on the book's history; whether there was one."""
        return _c.undo(_c.current(), False)

    def redo(self):
        return _c.undo(_c.current(), True)

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


# ---- Events -----------------------------------------------------------
#
# What Excel's Worksheet_Change, Worksheet_SelectionChange,
# Workbook_BeforeSave, Workbook_Open and Workbook_BeforeClose are:
# functions a book's script registers with office42.on(), or names in
# the script that are taken as registered when it runs -- on_change,
# on_selection_change, on_before_save, on_open, on_close.  A handler is
# called with (sheet, range) for the first two and (book) for the rest.
# A change a handler makes does not call the handlers again.

EVENTS = ("change", "selection", "before_save", "open", "close")
_NAMED_HANDLERS = {"on_change": "change", "on_selection_change": "selection",
                   "on_before_save": "before_save", "on_open": "open", "on_close": "close"}
_handlers = {}     # book id -> {event: {name: fn}}
_forgotten = set() # ids of functions off() was given: the convention does not bring them back


def _count_handlers():
    n = sum(len(fns) for events in _handlers.values() for fns in events.values())
    _c.events_count(n)


def on(event, fn=None, *, name=None):
    """Registers fn for an event: office42.on("change", fn), or as a
    decorator @office42.on("change").  A second registration under the
    same name replaces the first, so a script may be run again."""
    if event not in EVENTS:
        raise ValueError("event is one of %s, not %r" % (", ".join(EVENTS), event))

    def register(f):
        key = name or getattr(f, "__name__", None) or repr(f)
        _handlers.setdefault(_c.book_id(), {}).setdefault(event, {})[key] = f
        _count_handlers()
        return f
    return register if fn is None else register(fn)


def off(event, fn_or_name=None):
    """Forgets a handler, or with None every handler of the event."""
    events = _handlers.get(_c.book_id(), {})
    if fn_or_name is None:
        for f in events.pop(event, {}).values():
            _forgotten.add(id(f))
    else:
        fns = events.get(event, {})
        for key in [k for k, f in fns.items() if f is fn_or_name or k == fn_or_name]:
            _forgotten.add(id(fns[key]))
            del fns[key]
    _count_handlers()


def _register_named_handlers():
    """The convention: a function named on_change and the like in what
    just ran is a handler."""
    for name, event in _NAMED_HANDLERS.items():
        f = _namespace.get(name)
        if callable(f) and id(f) not in _forgotten:
            on(event, f, name=name)


def _fire(event, sheet_index, r0, c0, r1, c1):
    """Called from C when the event happens; errors are reported, not raised."""
    _bind()
    fns = list(_handlers.get(_c.book_id(), {}).get(event, {}).values())
    if not fns:
        return ""
    out = io.StringIO()
    with redirect_stdout(out), redirect_stderr(out):
        for f in fns:
            try:
                if event in ("change", "selection"):
                    sh = Sheet(sheet_index)
                    f(sh, Range(sh, r0, c0, r1, c1))
                else:
                    f(book)
            except BaseException:
                lines = traceback.format_exc().splitlines()
                print("\n".join(l for l in lines if "office42.py" not in l))
    return out.getvalue()


def _forget_handlers(owner):
    _handlers.pop(owner, None)
    _count_handlers()


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
                _register_named_handlers()
        except SystemExit:
            pass
        except BaseException:
            ok = False
            lines = traceback.format_exc().splitlines()
            # Leave out the runner's own frame.
            print("\n".join(l for l in lines if "office42.py" not in l))
    return ok, out.getvalue()


# ---- Stepping through a script ------------------------------------------

class DebugStop(BaseException):
    """Raised in the script when the user presses Stop."""


def _locals_text(frame):
    """The frame's variables, one a line, for the debugger's pane."""
    lines = []
    for name, value in sorted(frame.f_locals.items()):
        if name.startswith("__") or type(value).__name__ == "module" or callable(value) and not isinstance(value, type):
            continue
        try:
            shown = repr(value)
        except BaseException:
            shown = "<unrepresentable>"
        if len(shown) > 70:
            shown = shown[:67] + "..."
        lines.append("%s = %s" % (name, shown))
    return "\n".join(lines)


def _debug(code, filename, breakpoints, step_first):
    """Runs code as _run does, pausing -- through _c.debug_pause, which
    waits on the window -- at every line when stepping and at the lines
    in breakpoints otherwise.  debug_pause answers 0 to go on, 1 to step
    to the next line, 2 to stop."""
    import sys
    _bind()
    state = {"step": bool(step_first)}
    stops = set(breakpoints)

    def local_trace(frame, event, arg):
        if event == "line" and frame.f_code.co_filename == filename:
            line = frame.f_lineno
            if state["step"] or line in stops:
                command = _c.debug_pause(line, _locals_text(frame))
                if command == 2:
                    raise DebugStop()
                state["step"] = command == 1
        return local_trace

    def global_trace(frame, event, arg):
        return local_trace if frame.f_code.co_filename == filename else None

    out = io.StringIO()
    ok = True
    with redirect_stdout(out), redirect_stderr(out):
        try:
            compiled = compile(code, filename, "exec")
            sys.settrace(global_trace)
            try:
                exec(compiled, _namespace)
            finally:
                sys.settrace(None)
            _register_named_handlers()
        except DebugStop:
            print("Stopped.")
        except SystemExit:
            pass
        except BaseException:
            ok = False
            lines = traceback.format_exc().splitlines()
            print("\n".join(l for l in lines if "office42.py" not in l))
    return ok, out.getvalue()


def _forget_book(owner):
    """The book is going, or its scripts are being forgotten."""
    _forget_handlers(owner)
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
