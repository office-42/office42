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

    def clear(self):
        self.value = None

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
        borders, size (points), family, colour, fill (None for none),
        halign ('general', 'left', 'centre', 'right'), valign
        ('bottom', 'middle', 'top'), number ('general', 'fixed',
        'comma', 'currency', 'percent', 'scientific', 'text', 'date',
        'time', 'datetime', or a format code like '#,##0.00'),
        decimals."""
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
        saved = _c.current()
        return _c.evaluate(formula)


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

    def remove_sheet(self, which):
        _c.remove_sheet(which.index if isinstance(which, Sheet) else self[which].index)

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

    def set_script(self, name, code):
        """Stores code in the book under a name; saved with the file."""
        _c.set_script(name, code)

    def remove_script(self, name):
        return _c.remove_script(name)

    def run_script(self, name):
        """Runs a stored script in the console's namespace."""
        _bind()
        exec(compile(_c.get_script(name), name, "exec"), _namespace)


book = Book()
sheet = None      # bound before each run


def evaluate(formula):
    """The value of a formula on the sheet on show."""
    return _c.evaluate(formula)


def functions():
    """Every function name a cell formula may use, built in or from a script."""
    return _c.function_names()


# ---- Functions for cells -----------------------------------------------

_functions = {}


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
        _functions[fname] = fn
        _c.define(fname, lo if min_args is None else min_args, hi if max_args is None else max_args, signature, doc)
        return fn
    return register(f) if f is not None else register


def _call(name, args):
    """Called from C for =NAME(...) in a cell."""
    fn = _functions.get(name)
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


def _reset():
    for name in list(_functions):
        if name != "PY":
            _c.undefine(name)
            del _functions[name]
    _namespace.clear()
    del _errors[:]
