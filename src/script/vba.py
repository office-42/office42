# vba.py - Visual Basic for Applications, read and run
#
# Copyright (C) 2026 The office42 authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The language of Excel's macros, as its reference describes it: a
# module's text is read into statements, the statements are turned into
# Python closures once, and the closures run against whatever objects
# the host gives them -- the book, in office42; see vbaexcel.py.  Nothing
# here knows of a spreadsheet: the host answers the names the language
# does not have (Range, ActiveSheet, MsgBox's window), and the language
# is the same with none.  Compiled into the executable as a string, as
# office42.py is; edit here, rebuild.

"""Visual Basic for Applications: modules read, compiled to closures, run."""

import datetime
import inspect
import io
import math
import os
import random
import re
import time
import weakref

__all__ = ["Project", "VBAError", "VBASyntaxError", "EMPTY", "NULL", "MISSING"]


# ---- Errors ----------------------------------------------------------------

ERROR_TEXT = {
    3: "Return without GoSub", 5: "Invalid procedure call or argument", 6: "Overflow",
    7: "Out of memory", 9: "Subscript out of range",
    10: "This array is fixed or temporarily locked", 11: "Division by zero",
    13: "Type mismatch", 14: "Out of string space", 16: "Expression too complex",
    17: "Can't perform requested operation", 18: "User interrupt occurred",
    20: "Resume without error", 28: "Out of stack space",
    35: "Sub or Function not defined", 48: "Error in loading DLL", 51: "Internal error",
    52: "Bad file name or number", 53: "File not found", 54: "Bad file mode",
    55: "File already open", 57: "Device I/O error", 58: "File already exists",
    61: "Disk full", 62: "Input past end of file", 67: "Too many files",
    68: "Device unavailable", 70: "Permission denied", 71: "Disk not ready",
    74: "Can't rename with different drive", 75: "Path/File access error",
    76: "Path not found", 91: "Object variable or With block variable not set",
    92: "For loop not initialized", 93: "Invalid pattern string", 94: "Invalid use of Null",
    424: "Object required", 429: "ActiveX component can't create object",
    438: "Object doesn't support this property or method", 440: "Automation error",
    445: "Object doesn't support this action", 446: "Object doesn't support named arguments",
    448: "Named argument not found", 449: "Argument not optional",
    450: "Wrong number of arguments or invalid property assignment",
    451: "Property let procedure not defined and property get procedure did not return an object",
    453: "Specified DLL function not found",
    457: "This key is already associated with an element of this collection",
    1004: "Application-defined or object-defined error",
}


class VBAError(Exception):
    """A run-time error, as Err sees it: a number and a description.
    Where it happened is filled in as it passes out of a statement."""

    def __init__(self, number, description=None, source=""):
        self.number = int(number)
        self.description = description if description is not None else \
            ERROR_TEXT.get(self.number, "Application-defined or object-defined error")
        self.source = source
        self.where = None       # (module, procedure, line) of the statement
        Exception.__init__(self, self.description)

    def __str__(self):
        return "Run-time error '%d': %s" % (self.number, self.description)


class VBASyntaxError(Exception):
    """What the compiler says of a module: a message and a line."""

    def __init__(self, message, line=0, module=""):
        self.message = message
        self.line = line
        self.module = module
        Exception.__init__(self, message)

    def __str__(self):
        where = self.module + ", " if self.module else ""
        return "Compile error: %s (%sline %d)" % (self.message, where, self.line)


def error_from_python(exc):
    """A Python exception from the library or the host, as a VBA error."""
    if isinstance(exc, VBAError):
        return exc
    if isinstance(exc, ZeroDivisionError):
        return VBAError(11)
    if isinstance(exc, OverflowError):
        return VBAError(6)
    if isinstance(exc, (IndexError, KeyError)):
        return VBAError(9)
    if isinstance(exc, (TypeError,)):
        return VBAError(13)
    if isinstance(exc, FileNotFoundError):
        return VBAError(53)
    if isinstance(exc, PermissionError):
        return VBAError(70)
    if isinstance(exc, OSError):
        return VBAError(75, "Path/File access error: %s" % (exc.strerror or exc))
    if isinstance(exc, RecursionError):
        return VBAError(28)
    return VBAError(1004, str(exc) or ERROR_TEXT[1004])


# Control flow that leaves a statement: not errors, and not caught as
# errors by anything in between.
class _Signal(BaseException):
    pass


class ExitProc(_Signal):
    pass


class ExitLoop(_Signal):
    def __init__(self, kind):
        self.kind = kind


class GotoSignal(_Signal):
    def __init__(self, label):
        self.label = label


class ResumeSignal(_Signal):
    def __init__(self, kind, label=None):
        self.kind = kind            # "next", "retry" or "label"
        self.label = label


class ReturnSignal(_Signal):
    pass


class EndSignal(_Signal):
    """The End statement: everything stops, and the project forgets its
    variables."""


class StopSignal(_Signal):
    """Stop, or the user stopping a stepped macro."""


# ---- Values ------------------------------------------------------------------

class _Empty:
    """A Variant that was never given anything: "" as text, 0 as a number."""
    __slots__ = ()

    def __repr__(self):
        return "Empty"

    def __str__(self):
        return ""

    def __bool__(self):
        return False

    def __eq__(self, other):
        return other is self

    def __hash__(self):
        return 0


class _Null:
    __slots__ = ()

    def __repr__(self):
        return "Null"

    def __str__(self):
        return "Null"

    def __bool__(self):
        return False


class _Missing:
    """An Optional Variant argument that was left out: IsMissing."""
    __slots__ = ()

    def __repr__(self):
        return "Missing"

    def __bool__(self):
        return False


EMPTY = _Empty()
NULL = _Null()
MISSING = _Missing()

_EPOCH = datetime.datetime(1899, 12, 30)


class VBDate(float):
    """A Date: days since 30 December 1899, the time the fraction, as
    Excel counts them.  A float, so that it compares and adds as one."""

    __slots__ = ()

    @staticmethod
    def from_datetime(dt):
        days = (dt.date() - _EPOCH.date()).days
        frac = (dt.hour * 3600 + dt.minute * 60 + dt.second + dt.microsecond / 1e6) / 86400.0
        # Before the epoch the day counts back and the time still forward.
        return VBDate(days - frac if days < 0 else days + frac)

    def to_datetime(self):
        d = float(self)
        days = int(d)
        seconds = int(round(abs(d - days) * 86400))
        return _EPOCH + datetime.timedelta(days=days, seconds=seconds)

    def __repr__(self):
        return "#%s#" % date_text(self)

    def __str__(self):
        return date_text(self)


def date_text(d):
    """A Date as Visual Basic shows one: the date, the time, or both."""
    dt = VBDate(d).to_datetime()
    whole = float(d) == math.floor(float(d))
    if float(d) >= 0 and float(d) < 1 and not whole:
        return time_text(dt)
    text = "%d/%d/%04d" % (dt.month, dt.day, dt.year)
    if not whole:
        text += " " + time_text(dt)
    return text


def time_text(dt):
    hour = dt.hour % 12 or 12
    return "%d:%02d:%02d %s" % (hour, dt.minute, dt.second, "AM" if dt.hour < 12 else "PM")


class ErrValue:
    """CVErr(n): an error as a value, which is what a cell's #N/A is."""

    __slots__ = ("code",)

    def __init__(self, code):
        self.code = int(code)

    def __repr__(self):
        return "Error %d" % self.code

    __str__ = __repr__

    def __eq__(self, other):
        return isinstance(other, ErrValue) and other.code == self.code

    def __hash__(self):
        return hash(self.code)


class Ref:
    """A variable passed ByRef: the procedure called reads and writes
    the caller's."""

    __slots__ = ("c", "k")

    def __init__(self, container, key):
        self.c = container
        self.k = key

    def get(self):
        return self.c[self.k]

    def set(self, value):
        self.c[self.k] = value


class Cell:
    """A value with nowhere of its own to live: what an expression
    passed ByRef is given."""

    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value

    def get(self):
        return self.value

    def set(self, value):
        self.value = value


class VBArray:
    """An array of one or more dimensions, each with its bounds.  The
    elements lie with the first subscript changing fastest, as Visual
    Basic lays them out, so that ReDim Preserve may grow the last."""

    __slots__ = ("dims", "data", "typ", "fixed")

    def __init__(self, dims, typ="variant", fixed=False, data=None, init=None):
        self.dims = [(int(lb), int(ub)) for lb, ub in dims]
        self.typ = typ
        self.fixed = fixed
        if data is not None:
            self.data = data
        else:
            n = 1
            for lb, ub in self.dims:
                if ub < lb - 1:
                    raise VBAError(9)
                n *= ub - lb + 1
            if init is not None:
                self.data = [init() for _ in range(n)]
            else:
                self.data = [default_value(typ)] * n

    def __repr__(self):
        return "Array(%s)" % ", ".join(repr(v) for v in self.data[:20])

    @property
    def allocated(self):
        return bool(self.dims)

    def index(self, subs):
        if len(subs) != len(self.dims):
            raise VBAError(9)
        at = 0
        stride = 1
        for s, (lb, ub) in zip(subs, self.dims):
            s = to_long(s)
            if s < lb or s > ub:
                raise VBAError(9)
            at += (s - lb) * stride
            stride *= ub - lb + 1
        return at

    def get(self, subs):
        return self.data[self.index(subs)]

    def set(self, subs, value):
        self.data[self.index(subs)] = coerce(value, self.typ) if self.typ != "variant" else value

    def copy(self):
        data = [v.copy() if isinstance(v, (VBArray, UDTValue)) else v for v in self.data]
        return VBArray(self.dims, self.typ, self.fixed, data)

    def rows(self):
        """A two-dimensional array as a list of rows; one dimension as one row."""
        if len(self.dims) == 1:
            return [list(self.data)]
        if len(self.dims) != 2:
            raise VBAError(13)
        (r0, r1), (c0, c1) = self.dims
        n = r1 - r0 + 1
        return [[self.data[r + c * n] for c in range(c1 - c0 + 1)] for r in range(n)]

    @staticmethod
    def from_rows(rows, base=1):
        """A list of rows as a two-dimensional array counted from `base`."""
        nr = len(rows)
        nc = max((len(r) for r in rows), default=0)
        data = [EMPTY] * (nr * nc)
        for i, row in enumerate(rows):
            for j, v in enumerate(row):
                data[i + j * nr] = v
        return VBArray([(base, base + nr - 1), (base, base + nc - 1)], data=data)

    @staticmethod
    def from_list(items, base=0):
        items = list(items)
        return VBArray([(base, base + len(items) - 1)], data=items)


class UDTValue:
    """A value of a user-defined Type: its fields by name."""

    __slots__ = ("typedef", "fields")

    def __init__(self, typedef, fields=None):
        self.typedef = typedef
        if fields is None:
            fields = {}
            for fname, (ftype, fdims) in typedef.fields.items():
                fields[fname] = typedef.project.new_value(ftype, fdims)
        self.fields = fields

    def copy(self):
        return UDTValue(self.typedef, {k: (v.copy() if isinstance(v, (VBArray, UDTValue)) else v)
                                       for k, v in self.fields.items()})

    def __repr__(self):
        return "<%s>" % self.typedef.name


# ---- Types and conversions -----------------------------------------------------

NUMERIC_TYPES = ("byte", "integer", "long", "longlong", "longptr", "single", "double",
                 "currency", "decimal")
_INT_RANGE = {"byte": (0, 255), "integer": (-32768, 32767),
              "long": (-2147483648, 2147483647), "longptr": (-2 ** 63, 2 ** 63 - 1),
              "longlong": (-2 ** 63, 2 ** 63 - 1)}
_OBJECT_TYPES = set()   # the host adds its class names: range, worksheet ...


def default_value(typ):
    if typ in ("variant", None):
        return EMPTY
    if typ == "string":
        return ""
    if typ == "boolean":
        return False
    if typ == "date":
        return VBDate(0)
    if typ in ("single", "double", "currency", "decimal"):
        return 0.0
    if typ in _INT_RANGE:
        return 0
    if isinstance(typ, str) and typ.startswith("string*"):
        return " " * int(typ[7:])
    return None     # an object: Nothing


def is_object(v):
    return v is None or not isinstance(v, (int, float, str, _Empty, _Null, _Missing, VBArray,
                                            ErrValue, UDTValue))


def bankers_round(x):
    """Rounds half to even, as Visual Basic's CInt and Round do."""
    return int(round(x))


_NUM_RE = re.compile(r"^\s*([+-]?)\s*(\d+\.?\d*(?:[eEdD][+-]?\d+)?|\.\d+(?:[eEdD][+-]?\d+)?)\s*$")
_HEX_RE = re.compile(r"^\s*&[hH]([0-9a-fA-F]+)&?\s*$")
_OCT_RE = re.compile(r"^\s*&[oO]?([0-7]+)&?\s*$")


def parse_number(s):
    """The number a text stands for, or None: "1.5", " -2 ", "1e3",
    "&HFF", "1,000.5" and "$12" all are, as Visual Basic reads them."""
    m = _NUM_RE.match(s)
    if m:
        text = m.group(2).replace("d", "e").replace("D", "e")
        v = float(text)
        return -v if m.group(1) == "-" else v
    m = _HEX_RE.match(s)
    if m:
        return float(int(m.group(1), 16))
    t = s.strip()
    if t.startswith("&O") or t.startswith("&o"):
        m = _OCT_RE.match(t)
        if m:
            return float(int(m.group(1), 8))
    # Thousands separators, a currency sign, a percent, brackets for a
    # negative: what CDbl and IsNumeric accept.
    t = t.replace(",", "").replace("$", "")
    neg = False
    if t.startswith("(") and t.endswith(")"):
        t, neg = t[1:-1], True
    pct = t.endswith("%")
    if pct:
        t = t[:-1]
    m = _NUM_RE.match(t)
    if m and t:
        v = float(m.group(2).replace("d", "e").replace("D", "e"))
        if m.group(1) == "-":
            v = -v
        if pct:
            v /= 100.0
        return -v if neg else v
    return None


def to_num(v):
    """A value as a number for arithmetic: True is -1, Empty 0, a
    numeric text its number; anything else a type mismatch."""
    t = type(v)
    if t is int or t is float or t is VBDate:
        return v
    if t is bool:
        return -1 if v else 0
    if v is EMPTY:
        return 0
    if t is str:
        n = parse_number(v)
        if n is None:
            if v.strip().lower() in ("true", "false"):
                return -1 if v.strip().lower() == "true" else 0
            d = parse_date(v)
            if d is not None:
                return float(d)
            raise VBAError(13)
        return int(n) if n == int(n) and abs(n) < 2 ** 53 and "." not in v and "e" not in v.lower() else n
    if v is NULL:
        raise VBAError(94)
    if isinstance(v, ErrValue):
        raise VBAError(13)
    if v is None:
        raise VBAError(91)
    return to_num(default_member(v))


def to_double(v):
    return float(to_num(v))


def to_long(v, typ="long"):
    n = to_num(v)
    if type(n) is not int:
        if n != n:
            raise VBAError(6)
        n = bankers_round(n)
    lo, hi = _INT_RANGE.get(typ, _INT_RANGE["long"])
    if n < lo or n > hi:
        raise VBAError(6)
    return n


def number_text(n):
    """A number as Str and & write one: fifteen significant figures, no
    trailing zeroes, an exponent for the very large and very small."""
    if type(n) is int:
        return str(n)
    if n != n or n in (float("inf"), float("-inf")):
        raise VBAError(6)
    if n == int(n) and abs(n) < 1e15:
        return str(int(n))
    a = abs(n)
    if 1e-15 <= a < 1e15:
        text = "%.15g" % n
        if "e" in text:
            text = ("%.15f" % n).rstrip("0").rstrip(".")
        if text.startswith("0."):
            text = text[0:]
        elif text.startswith("-0."):
            text = "-" + text[1:]
        return text
    text = "%.14E" % n
    mant, exp = text.split("E")
    mant = mant.rstrip("0").rstrip(".")
    return "%sE%s%02d" % (mant, "-" if int(exp) < 0 else "+", abs(int(exp)))


def to_str(v):
    t = type(v)
    if t is str:
        return v
    if t is bool:
        return "True" if v else "False"
    if t is int:
        return str(v)
    if t is float:
        return number_text(v)
    if t is VBDate:
        return date_text(v)
    if v is EMPTY:
        return ""
    if v is NULL:
        raise VBAError(94)
    if isinstance(v, ErrValue):
        return "Error %d" % v.code
    if isinstance(v, VBArray) and v.typ == "byte" and len(v.dims) == 1:
        # A String is its UTF-16 bytes, two to a character, and a Byte
        # array made a String is read that way.
        return bytes(v.data).decode("utf-16-le", "replace")
    if isinstance(v, VBArray) or isinstance(v, UDTValue):
        raise VBAError(13)
    if v is None:
        raise VBAError(91)
    return to_str(default_member(v))


def byte_array(data):
    """A Byte array from 0 of these bytes."""
    return VBArray([(0, len(data) - 1)], "byte", data=list(data))


def to_bool(v):
    t = type(v)
    if t is bool:
        return v
    if t is int or t is float or t is VBDate:
        return v != 0
    if v is EMPTY:
        return False
    if t is str:
        s = v.strip().lower()
        if s == "true":
            return True
        if s == "false":
            return False
        n = parse_number(v)
        if n is None:
            raise VBAError(13)
        return n != 0
    if v is NULL:
        raise VBAError(94)
    if v is None:
        raise VBAError(91)
    if isinstance(v, (VBArray, ErrValue, UDTValue)):
        raise VBAError(13)
    return to_bool(default_member(v))


_MONTHS = ["january", "february", "march", "april", "may", "june", "july", "august",
           "september", "october", "november", "december"]
_DATE_RES = [
    (re.compile(r"^(\d{4})[-/.](\d{1,2})[-/.](\d{1,2})$"), "ymd"),
    (re.compile(r"^(\d{1,2})[-/.](\d{1,2})[-/.](\d{2,4})$"), "mdy"),
    (re.compile(r"^(\d{1,2})[-/ ]([A-Za-z]{3,9})[-/ ,]+(\d{2,4})$"), "dMy"),
    (re.compile(r"^([A-Za-z]{3,9})[-/ ](\d{1,2}),?[-/ ]+(\d{2,4})$"), "Mdy"),
    (re.compile(r"^(\d{1,2})[-/](\d{1,2})$"), "md"),
]
_TIME_RE = re.compile(r"^(\d{1,2}):(\d{1,2})(?::(\d{1,2}(?:\.\d+)?))?\s*([AaPp][Mm]?)?$")


def _month(name):
    name = name.lower()
    for i, m in enumerate(_MONTHS):
        if m.startswith(name[:3]) and m.startswith(name):
            return i + 1
    return None


def _year(y):
    y = int(y)
    if y < 100:
        y += 2000 if y < 30 else 1900
    return y


def parse_date(s):
    """The Date a text says, as CDate and IsDate read it, or None."""
    s = s.strip()
    if not s:
        return None
    date_part, time_part = s, None
    m = re.match(r"^(.*?)\s+(\d{1,2}:\d{1,2}(?::\d{1,2}(?:\.\d+)?)?\s*(?:[AaPp][Mm]?)?)$", s)
    if m:
        date_part, time_part = m.group(1), m.group(2)
    elif _TIME_RE.match(s):
        date_part, time_part = "", s
    days = 0.0
    if date_part:
        found = None
        for rx, order in _DATE_RES:
            dm = rx.match(date_part)
            if not dm:
                continue
            try:
                if order == "ymd":
                    y, mo, d = int(dm.group(1)), int(dm.group(2)), int(dm.group(3))
                elif order == "mdy":
                    mo, d, y = int(dm.group(1)), int(dm.group(2)), _year(dm.group(3))
                    if mo > 12 and d <= 12:
                        mo, d = d, mo
                elif order == "dMy":
                    d, mo, y = int(dm.group(1)), _month(dm.group(2)), _year(dm.group(3))
                elif order == "Mdy":
                    mo, d, y = _month(dm.group(1)), int(dm.group(2)), _year(dm.group(3))
                else:
                    mo, d, y = int(dm.group(1)), int(dm.group(2)), datetime.date.today().year
                if mo is None:
                    continue
                found = datetime.datetime(y, mo, d)
            except ValueError:
                continue
            break
        if found is None:
            return None
        days = float((found - _EPOCH).days)
    if time_part:
        tm = _TIME_RE.match(time_part.strip())
        if not tm:
            return None
        h, mi = int(tm.group(1)), int(tm.group(2))
        sec = float(tm.group(3)) if tm.group(3) else 0.0
        ampm = (tm.group(4) or "").lower()
        if ampm.startswith("p") and h < 12:
            h += 12
        elif ampm.startswith("a") and h == 12:
            h = 0
        if h > 23 or mi > 59 or sec >= 60:
            return None
        frac = (h * 3600 + mi * 60 + sec) / 86400.0
        days = days + frac if days >= 0 else days - frac
    return VBDate(days)


def to_date(v):
    t = type(v)
    if t is VBDate:
        return v
    if t is str:
        d = parse_date(v)
        if d is None:
            n = parse_number(v)
            if n is None:
                raise VBAError(13)
            return VBDate(n)
        return d
    if t is int or t is float or t is bool:
        return VBDate(to_num(v))
    if v is EMPTY:
        return VBDate(0)
    if v is NULL:
        raise VBAError(94)
    if v is None:
        raise VBAError(91)
    if isinstance(v, (VBArray, ErrValue, UDTValue)):
        raise VBAError(13)
    return to_date(default_member(v))


def coerce(v, typ):
    """A value made fit for a variable of a declared type, as assigning
    to one does: rounded to an Integer, written as a String, and so on."""
    if typ == "variant" or typ is None:
        if isinstance(v, (VBArray, UDTValue)):
            return v.copy()
        return v
    if typ == "string":
        return to_str(v)
    if typ in ("double", "single", "currency", "decimal"):
        if isinstance(v, (int, float)) and type(v) is not bool:
            n = float(v)
        else:
            n = to_double(v)
        if typ == "currency":
            n = round(n, 4)
        return n
    if typ in _INT_RANGE:
        return to_long(v, typ)
    if typ == "boolean":
        return to_bool(v)
    if typ == "date":
        return to_date(v)
    if typ.startswith("string*"):
        n = int(typ[7:])
        s = to_str(v)
        return (s + " " * n)[:n]
    if isinstance(v, (VBArray, UDTValue)):
        return v.copy()
    return v


# The objects of the host and of class modules say what their default
# member is; the language asks through this.
def default_member(obj, *args):
    f = getattr(obj, "_vba_default_get", None)
    if f is None:
        raise VBAError(438)
    return f(*args)


def default_let(obj, value, *args):
    f = getattr(obj, "_vba_default_let", None)
    if f is None:
        raise VBAError(438)
    f(value, *args)


def plain_value(v):
    """What an object stands for when a value is wanted: its default member."""
    if is_object(v) and v is not None and not isinstance(v, VBArray):
        return default_member(v)
    return v


# ---- Operators -------------------------------------------------------------------

def _num_pair(a, b):
    return to_num(a), to_num(b)


def _numeric_result(x, a, b):
    """Integer arithmetic stays whole; a Date plus a number is a Date."""
    if type(x) is float and x == int(x) and type(a) is int and type(b) is int:
        return int(x)
    return x


def op_add(a, b):
    ta, tb = type(a), type(b)
    if ta is int and tb is int:
        return a + b
    if (ta is float or ta is int) and (tb is float or tb is int):
        return a + b
    if ta is str and tb is str:
        return a + b
    if a is NULL or b is NULL:
        return NULL
    if ta is str and b is EMPTY:
        return a
    if tb is str and a is EMPTY:
        return b
    if ta is VBDate or tb is VBDate:
        x, y = _num_pair(a, b)
        return VBDate(float(x) + float(y))
    x, y = _num_pair(a, b)
    return x + y


def op_sub(a, b):
    ta, tb = type(a), type(b)
    if (ta is int or ta is float) and (tb is int or tb is float):
        return a - b
    if a is NULL or b is NULL:
        return NULL
    x, y = _num_pair(a, b)
    if ta is VBDate and tb is not VBDate:
        return VBDate(float(x) - float(y))
    return x - y


def op_mul(a, b):
    ta, tb = type(a), type(b)
    if (ta is int or ta is float) and (tb is int or tb is float):
        return a * b
    if a is NULL or b is NULL:
        return NULL
    x, y = _num_pair(a, b)
    return x * y


def op_div(a, b):
    if a is NULL or b is NULL:
        return NULL
    x, y = _num_pair(a, b)
    if y == 0:
        if x == 0:
            raise VBAError(6)
        raise VBAError(11)
    return float(x) / float(y)


def op_intdiv(a, b):
    if a is NULL or b is NULL:
        return NULL
    x, y = to_long(a, "longlong"), to_long(b, "longlong")
    if y == 0:
        raise VBAError(11)
    q = abs(x) // abs(y)
    return q if (x >= 0) == (y >= 0) else -q


def op_mod(a, b):
    if a is NULL or b is NULL:
        return NULL
    x, y = to_long(a, "longlong"), to_long(b, "longlong")
    if y == 0:
        raise VBAError(11)
    r = abs(x) % abs(y)
    return r if x >= 0 else -r


def op_pow(a, b):
    if a is NULL or b is NULL:
        return NULL
    x, y = _num_pair(a, b)
    if x == 0 and y < 0:
        raise VBAError(5)
    if x < 0 and y != int(y):
        raise VBAError(5)
    try:
        r = float(x) ** float(y)
    except OverflowError:
        raise VBAError(6)
    if type(r) is complex:
        raise VBAError(5)
    return r


def op_neg(a):
    t = type(a)
    if t is int or t is float:
        return -a
    if a is NULL:
        return NULL
    if t is VBDate:
        return VBDate(-float(a))
    return -to_num(a)


def op_concat(a, b):
    sa = "" if a is NULL else (a if type(a) is str else to_str(a))
    sb = "" if b is NULL else (b if type(b) is str else to_str(b))
    return sa + sb


def _is_numeric_value(v):
    t = type(v)
    return t is int or t is float or t is bool or t is VBDate


def compare(a, b, text=False):
    """-1, 0 or 1, or None when Null is compared; how =, < and the rest,
    and Select Case, compare two values."""
    if a is NULL or b is NULL:
        return None
    if is_object(a) and a is not None:
        a = default_member(a)
    if is_object(b) and b is not None:
        b = default_member(b)
    ta, tb = type(a), type(b)
    if ta is str and tb is str:
        if text:
            a, b = a.lower(), b.lower()
        return (a > b) - (a < b)
    if a is EMPTY and tb is str:
        a, ta = "", str
    elif b is EMPTY and ta is str:
        b, tb = "", str
    if ta is str and tb is str:
        return (a > b) - (a < b)
    if ta is str or tb is str:
        # A number against a text: numbers when the text is one, and
        # otherwise the number is the lesser, as a Variant compares.
        s, n = (a, b) if ta is str else (b, a)
        num = parse_number(s)
        if num is None:
            d = parse_date(s) if type(n) is VBDate else None
            if d is None:
                r = -1
                return r if ta is not str else -r
            num = float(d)
        x = to_num(n)
        y = num
        if ta is str:
            x, y = y, x
        return (x > y) - (x < y)
    if isinstance(a, ErrValue) or isinstance(b, ErrValue):
        raise VBAError(13)
    if a is None or b is None:
        raise VBAError(91)
    x, y = to_num(a), to_num(b)
    return (x > y) - (x < y)


def _cmp_result(r, test):
    if r is None:
        return NULL
    return test(r)


def op_eq(a, b, text=False):
    ta, tb = type(a), type(b)
    if ta is tb and (ta is int or ta is float or (ta is str and not text)):
        return a == b
    return _cmp_result(compare(a, b, text), lambda r: r == 0)


def op_ne(a, b, text=False):
    ta, tb = type(a), type(b)
    if ta is tb and (ta is int or ta is float or (ta is str and not text)):
        return a != b
    return _cmp_result(compare(a, b, text), lambda r: r != 0)


def op_lt(a, b, text=False):
    ta, tb = type(a), type(b)
    if ta is tb and (ta is int or ta is float):
        return a < b
    return _cmp_result(compare(a, b, text), lambda r: r < 0)


def op_le(a, b, text=False):
    ta, tb = type(a), type(b)
    if ta is tb and (ta is int or ta is float):
        return a <= b
    return _cmp_result(compare(a, b, text), lambda r: r <= 0)


def op_gt(a, b, text=False):
    ta, tb = type(a), type(b)
    if ta is tb and (ta is int or ta is float):
        return a > b
    return _cmp_result(compare(a, b, text), lambda r: r > 0)


def op_ge(a, b, text=False):
    ta, tb = type(a), type(b)
    if ta is tb and (ta is int or ta is float):
        return a >= b
    return _cmp_result(compare(a, b, text), lambda r: r >= 0)


def _logic_operands(a, b):
    if type(a) is bool and type(b) is bool:
        return a, b, True
    return to_long(a, "longlong"), to_long(b, "longlong"), False


def op_and(a, b):
    if a is NULL or b is NULL:
        # Null And False is False; anything else with Null is Null.
        other = b if a is NULL else a
        if other is not NULL and not to_bool(other):
            return False
        return NULL
    x, y, logical = _logic_operands(a, b)
    return (x and y) if logical else x & y


def op_or(a, b):
    if a is NULL or b is NULL:
        other = b if a is NULL else a
        if other is not NULL and to_bool(other):
            return True
        return NULL
    x, y, logical = _logic_operands(a, b)
    return (x or y) if logical else x | y


def op_xor(a, b):
    if a is NULL or b is NULL:
        return NULL
    x, y, logical = _logic_operands(a, b)
    return (x != y) if logical else x ^ y


def op_eqv(a, b):
    if a is NULL or b is NULL:
        return NULL
    x, y, logical = _logic_operands(a, b)
    return (x == y) if logical else ~(x ^ y)


def op_imp(a, b):
    if a is NULL or b is NULL:
        return NULL
    x, y, logical = _logic_operands(a, b)
    return ((not x) or y) if logical else (~x) | y


def op_not(a):
    if a is NULL:
        return NULL
    if type(a) is bool:
        return not a
    return ~to_long(a, "longlong")


def like_regex(pattern, text_compare=False):
    """A Like pattern as a regular expression: ? * # [a-z] [!a-z]."""
    out = []
    i = 0
    while i < len(pattern):
        c = pattern[i]
        if c == "?":
            out.append(".")
        elif c == "*":
            out.append(".*")
        elif c == "#":
            out.append("[0-9]")
        elif c == "[":
            j = pattern.find("]", i + 1)
            if j < 0:
                raise VBAError(93)
            body = pattern[i + 1:j]
            if body.startswith("!"):
                out.append("[^" + re.escape(body[1:]).replace("\\-", "-") + "]")
            elif body == "":
                pass
            else:
                out.append("[" + re.escape(body).replace("\\-", "-") + "]")
            i = j
        else:
            out.append(re.escape(c))
        i += 1
    flags = re.DOTALL | (re.IGNORECASE if text_compare else 0)
    return re.compile("^" + "".join(out) + "$", flags)


_like_cache = {}


def op_like(a, b, text=False):
    if a is NULL or b is NULL:
        return NULL
    s, p = to_str(a), to_str(b)
    key = (p, text)
    rx = _like_cache.get(key)
    if rx is None:
        rx = _like_cache[key] = like_regex(p, text)
    return rx.match(s) is not None


def op_is(a, b):
    if not is_object(a) or not is_object(b):
        raise VBAError(424)
    return a is b


def truth(v):
    """What If makes of a value: Null is not true."""
    if type(v) is bool:
        return v
    if v is NULL:
        return False
    return to_bool(v)


# ---- Reading the text ---------------------------------------------------------

class Token:
    __slots__ = ("kind", "val", "line", "suffix")

    def __init__(self, kind, val, line, suffix=""):
        self.kind = kind        # id num str date op eval nl eof
        self.val = val
        self.line = line
        self.suffix = suffix    # an identifier's type character: $ % & ! # @

    def __repr__(self):
        return "%s:%r" % (self.kind, self.val)


_SUFFIX_TYPES = {"%": "integer", "&": "long", "!": "single", "#": "double",
                 "@": "currency", "$": "string", "^": "longlong"}
_ID_START = re.compile(r"[A-Za-z_À-￿]")
_ID_CHARS = re.compile(r"[A-Za-z0-9_À-￿]*")
_NUMBER = re.compile(r"(\d+\.?\d*|\.\d+)([eEdD][+-]?\d+)?")
_DATE_LITERAL = re.compile(r"#([0-9A-Za-z/:\-., ]+)#")


def _is_id_char(c):
    return c.isalnum() or c == "_" or ord(c) >= 0xC0


class Lexer:
    """Turns a module's text into logical lines of tokens: continuations
    joined, comments dropped, #If sections that do not apply left out."""

    def __init__(self, text, module=""):
        self.text = text
        self.module = module

    def error(self, message, line):
        raise VBASyntaxError(message, line, self.module)

    def lines(self):
        """Logical lines: (first physical line number, tokens)."""
        physical = self.text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
        out = []
        i = 0
        while i < len(physical):
            start = i + 1
            tokens = []
            while True:
                more, comment_goes_on = self.scan(physical[i], i + 1, tokens)
                i += 1
                if comment_goes_on:
                    # A comment ending in " _" swallows the next line too.
                    while i < len(physical) and physical[i - 1].rstrip().endswith(" _"):
                        i += 1
                    break
                if not more or i >= len(physical):
                    break
            out.append((start, tokens))
        return out

    def scan(self, s, line, tokens):
        """Tokens of one physical line, appended; (continues, comment
        continues)."""
        i = 0
        n = len(s)
        at_statement_start = not tokens or (tokens[-1].kind == "op" and tokens[-1].val == ":")
        while i < n:
            c = s[i]
            if c in " \t ":
                i += 1
                continue
            if c == "'":
                return False, s.rstrip().endswith(" _")
            if c == '"':
                j = i + 1
                buf = []
                while True:
                    if j >= n:
                        self.error("Expected: end of string", line)
                    if s[j] == '"':
                        if j + 1 < n and s[j + 1] == '"':
                            buf.append('"')
                            j += 2
                            continue
                        break
                    buf.append(s[j])
                    j += 1
                tokens.append(Token("str", "".join(buf), line))
                i = j + 1
                at_statement_start = False
                continue
            if c == "_" and (i + 1 >= n or s[i + 1:].strip() == "") and (i == 0 or s[i - 1] in " \t"):
                return True, False          # a line continuation
            if _ID_START.match(c):
                j = i + 1
                while j < n and _is_id_char(s[j]):
                    j += 1
                word = s[i:j]
                suffix = ""
                if j < n and s[j] in "$%!#@&":
                    after = s[j + 1] if j + 1 < n else ""
                    if s[j] == "!" and after and (_ID_START.match(after) or after == "["):
                        pass                # rs!Field: a bang, not a Single
                    elif s[j] == "&" and after and (after not in " \t),=:;" ):
                        pass                # a&b: concatenation
                    elif s[j] == "#" and after and (after.isdigit() or after == "/"):
                        pass
                    else:
                        suffix = s[j]
                        j += 1
                if at_statement_start and word.lower() == "rem" and (j >= n or not _is_id_char(s[j])):
                    return False, s.rstrip().endswith(" _")
                tokens.append(Token("id", word, line, suffix))
                i = j
                at_statement_start = False
                continue
            if c.isdigit() or (c == "." and i + 1 < n and s[i + 1].isdigit() and
                               (not tokens or (tokens[-1].kind == "op" and tokens[-1].val != ")"))):
                m = _NUMBER.match(s, i)
                text = m.group(0)
                j = m.end()
                exp = m.group(2)
                is_float = "." in text or exp is not None
                value = float(text.replace("d", "e").replace("D", "e")) if is_float else int(text)
                if j < n and s[j] in "%&!#@^":
                    t = s[j]
                    if t in "!#@":
                        value = float(value)
                    j += 1
                tokens.append(Token("num", value, line))
                i = j
                at_statement_start = False
                continue
            if c == "&" and i + 1 < n and s[i + 1] in "hHoO":
                m = re.match(r"&[hH]([0-9A-Fa-f]+)(&?)|&[oO]([0-7]+)(&?)", s[i:])
                if m:
                    if m.group(1) is not None:
                        v = int(m.group(1), 16)
                        long_ = m.group(2) == "&"
                    else:
                        v = int(m.group(3), 8)
                        long_ = m.group(4) == "&"
                    # &HFFFF is -1: a literal that fits sixteen bits is an
                    # Integer, and one that fits 32 a Long.
                    if not long_ and v <= 0xFFFF:
                        if v >= 0x8000:
                            v -= 0x10000
                    elif v <= 0xFFFFFFFF and v >= 0x80000000:
                        v -= 0x100000000
                    tokens.append(Token("num", v, line))
                    i += m.end()
                    at_statement_start = False
                    continue
            if c == "#":
                m = _DATE_LITERAL.match(s, i)
                prev = tokens[-1] if tokens else None
                prev_ok = prev is None or prev.kind == "op" or (prev.kind == "id" and prev.val.lower() in
                          ("then", "else", "to", "step", "and", "or", "not", "is", "case", "return", "mod"))
                if m and prev_ok and (parse_date(m.group(1)) is not None):
                    tokens.append(Token("date", parse_date(m.group(1)), line))
                    i = m.end()
                    at_statement_start = False
                    continue
                if (prev is None and i + 1 < n and s[i + 1].isalpha()):
                    # #If, #Else, #End If, #Const: a directive.
                    word = re.match(r"#\s*([A-Za-z]+)", s[i:])
                    tokens.append(Token("directive", word.group(1).lower(), line))
                    i += word.end()
                    continue
                tokens.append(Token("op", "#", line))
                i += 1
                continue
            if c == "[":
                j = s.find("]", i + 1)
                if j < 0:
                    self.error("Expected: ]", line)
                tokens.append(Token("eval", s[i + 1:j], line))
                i = j + 1
                at_statement_start = False
                continue
            two = s[i:i + 2]
            if two in ("<>", "<=", ">=", ":=", "=<", "=>", "><"):
                two = {"=<": "<=", "=>": ">=", "><": "<>"}.get(two, two)
                tokens.append(Token("op", two, line))
                i += 2
                continue
            if c in "+-*/\\^&=<>(),.:;!":
                tokens.append(Token("op", c, line))
                i += 1
                at_statement_start = c == ":"
                continue
            self.error("Invalid character %r" % c, line)
        return False, False


# The names #If knows without a #Const: this is 64-bit Visual Basic 7
# on something that is not a Macintosh.
_DIRECTIVE_CONSTS = {"vba6": True, "vba7": True, "win16": False, "win32": True,
                     "win64": True, "mac": False}


def _eval_directive(tokens, consts, module, line):
    parser = Parser([t for t in tokens] + [Token("nl", None, line), Token("eof", None, line)], module)
    expr = parser.parse_expr()
    return const_eval(expr, lambda name: consts.get(name, _DIRECTIVE_CONSTS.get(name, EMPTY)))


def preprocess(lines, module=""):
    """Drops what #If leaves out: returns the logical lines that apply."""
    out = []
    stack = []          # (this branch applies, some branch was taken, outer applies)
    consts = {}
    for start, tokens in lines:
        if tokens and tokens[0].kind == "directive":
            word = tokens[0].val
            rest = tokens[1:]
            applies = all(s[0] for s in stack)
            if word == "if":
                cond = rest[:-1] if rest and rest[-1].kind == "id" and rest[-1].val.lower() == "then" else rest
                ok = applies and truth(_eval_directive(cond, consts, module, start))
                stack.append([ok, ok, applies])
            elif word == "elseif":
                if not stack:
                    raise VBASyntaxError("#ElseIf without #If", start, module)
                top = stack[-1]
                cond = rest[:-1] if rest and rest[-1].kind == "id" and rest[-1].val.lower() == "then" else rest
                ok = top[2] and not top[1] and truth(_eval_directive(cond, consts, module, start))
                top[0] = ok
                top[1] = top[1] or ok
            elif word == "else":
                if not stack:
                    raise VBASyntaxError("#Else without #If", start, module)
                top = stack[-1]
                top[0] = top[2] and not top[1]
                top[1] = True
            elif word in ("end", "endif"):
                if not stack:
                    raise VBASyntaxError("#End If without #If", start, module)
                stack.pop()
            elif word == "const":
                if applies and len(rest) >= 3 and rest[0].kind == "id":
                    consts[rest[0].val.lower()] = _eval_directive(rest[2:], consts, module, start)
            continue
        if all(s[0] for s in stack):
            out.append((start, tokens))
    return out


# ---- The statement tree ----------------------------------------------------------

class Node:
    __slots__ = ()


class Lit(Node):
    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value


class NameE(Node):
    __slots__ = ("name", "suffix", "line")

    def __init__(self, name, suffix="", line=0):
        self.name = name
        self.suffix = suffix
        self.line = line


class MeE(Node):
    __slots__ = ()


class WithE(Node):
    __slots__ = ()


class MemberE(Node):
    __slots__ = ("obj", "name")

    def __init__(self, obj, name):
        self.obj = obj
        self.name = name


class BangE(Node):
    __slots__ = ("obj", "name")

    def __init__(self, obj, name):
        self.obj = obj
        self.name = name


class ArgE(Node):
    __slots__ = ("name", "expr", "byval")

    def __init__(self, name, expr, byval=False):
        self.name = name        # a named argument's name, lower-cased, or None
        self.expr = expr        # None when the argument is left out
        self.byval = byval


class CallE(Node):
    __slots__ = ("target", "args")

    def __init__(self, target, args):
        self.target = target
        self.args = args


class ParenE(Node):
    __slots__ = ("expr",)

    def __init__(self, expr):
        self.expr = expr


class BinE(Node):
    __slots__ = ("op", "a", "b")

    def __init__(self, op, a, b):
        self.op = op
        self.a = a
        self.b = b


class UnE(Node):
    __slots__ = ("op", "x")

    def __init__(self, op, x):
        self.op = op
        self.x = x


class NewE(Node):
    __slots__ = ("typename",)

    def __init__(self, typename):
        self.typename = typename


class TypeOfE(Node):
    __slots__ = ("expr", "typename")

    def __init__(self, expr, typename):
        self.expr = expr
        self.typename = typename


class EvalE(Node):
    __slots__ = ("text",)

    def __init__(self, text):
        self.text = text


class Stmt:
    __slots__ = ("line", "kind", "a")

    def __init__(self, kind, line, **a):
        self.kind = kind
        self.line = line
        self.a = a

    def __getattr__(self, name):
        try:
            return self.a[name]
        except KeyError:
            raise AttributeError(name)

    def __repr__(self):
        return "<%s line %d>" % (self.kind, self.line)


class VarDecl:
    __slots__ = ("name", "typ", "dims", "dynamic", "new", "line", "with_events")

    def __init__(self, name, typ, dims, dynamic, new, line, with_events=False):
        self.name = name
        self.typ = typ          # lower-cased type name, "variant" if none
        self.dims = dims        # [(lb expr or None, ub expr)] for a fixed array
        self.dynamic = dynamic  # declared with ()
        self.new = new          # As New
        self.line = line
        self.with_events = with_events


class Param:
    __slots__ = ("name", "byval", "optional", "paramarray", "typ", "default", "is_array")

    def __init__(self, name, byval, optional, paramarray, typ, default, is_array):
        self.name = name
        self.byval = byval
        self.optional = optional
        self.paramarray = paramarray
        self.typ = typ
        self.default = default
        self.is_array = is_array


class ProcDef:
    def __init__(self, kind, name, params, ret_type, body, private, static, line, end_line):
        self.kind = kind            # sub function get let set
        self.name = name
        self.params = params
        self.ret_type = ret_type
        self.ret_array = False
        self.body = body
        self.private = private
        self.static = static
        self.line = line
        self.end_line = end_line


class TypeDefNode:
    def __init__(self, name, fields, private):
        self.name = name
        self.fields = fields        # [(name, typ, dims)]
        self.private = private


class EnumDefNode:
    def __init__(self, name, members, private):
        self.name = name
        self.members = members      # [(name, expr or None)]
        self.private = private


class DeclareNode:
    def __init__(self, name, kind, lib, alias, params, ret_type, private):
        self.name = name
        self.kind = kind
        self.lib = lib
        self.alias = alias
        self.params = params
        self.ret_type = ret_type
        self.private = private


class ModuleAST:
    def __init__(self):
        self.options = {"explicit": False, "base": 0, "compare": "binary", "private": False}
        self.decls = []             # Stmt dim / const at module level, with scope
        self.procs = []
        self.types = []
        self.enums = []
        self.declares = []
        self.events = []
        self.attributes = {}
        self.implements = []        # the interfaces a class names in Implements


# Words that end a block, and so are never a label or a procedure call.
_RESERVED = {
    "and", "as", "byref", "byval", "call", "case", "const", "declare", "dim", "do", "each",
    "else", "elseif", "end", "enum", "eqv", "event", "exit", "false", "for", "friend",
    "function", "global", "gosub", "goto", "if", "imp", "implements", "in", "is", "let",
    "like", "loop", "me", "mod", "new", "next", "not", "nothing", "on", "option",
    "optional", "or", "paramarray", "private", "property", "public", "redim", "resume",
    "return", "select", "set", "static", "step", "stop", "sub", "then", "to", "true",
    "type", "typeof", "until", "wend", "while", "with", "withevents", "xor",
}


# A block the procedure ended inside, by the last word that would close it.
# The type libraries a macro may name before their members, as it names
# VBA before its own: Excel.Range, Office.MsoTriState, MSForms.TextBox.
_LIBRARIES = {"excel", "office", "msforms", "scripting", "stdole", "vbide", "vbscript_regexp_55",
              "msxml2", "winhttp"}

# The classes of the other libraries a macro may reference -- Microsoft
# XML's, which o42xml puts here when it loads -- by ProgID for
# CreateObject and by class name for New: a factory for each.
LIBRARY_CLASSES = {}

_UNCLOSED = {("end", "if"): "Block If without End If", ("endif",): "Block If without End If",
             ("end", "with"): "With without End With", ("next",): "For without Next",
             ("loop",): "Do without Loop", ("end", "while"): "While without Wend",
             ("end", "select"): "Select Case without End Select"}


_STRAY = ((("end", "if"), "End If without block If"), (("endif",), "End If without block If"),
          (("end", "with"), "End With without With"), (("next",), "Next without For"),
          (("loop",), "Loop without Do"), (("wend",), "Wend without While"),
          (("end", "select"), "End Select without Select Case"))


class Parser:
    def __init__(self, tokens, module=""):
        self.t = tokens
        self.i = 0
        self.module = module
        self.attributes = []        # (procedure, attribute, value) inside procedures

    # -- tokens --
    def peek(self, k=0):
        return self.t[min(self.i + k, len(self.t) - 1)]

    def next(self):
        tok = self.t[self.i]
        if self.i < len(self.t) - 1:
            self.i += 1
        return tok

    def error(self, message, tok=None):
        tok = tok or self.peek()
        raise VBASyntaxError(message, tok.line, self.module)

    def is_op(self, val, k=0):
        tok = self.peek(k)
        return tok.kind == "op" and tok.val == val

    def is_kw(self, word, k=0):
        tok = self.peek(k)
        return tok.kind == "id" and tok.val.lower() == word

    def accept_op(self, val):
        if self.is_op(val):
            self.next()
            return True
        return False

    def accept_kw(self, word):
        if self.is_kw(word):
            self.next()
            return True
        return False

    def expect_op(self, val):
        if not self.accept_op(val):
            self.error("Expected: %s" % val)

    def expect_kw(self, word):
        if not self.accept_kw(word):
            self.error("Expected: %s" % word.capitalize())

    def ident(self, what="identifier"):
        tok = self.peek()
        if tok.kind == "id" or tok.kind == "eval":
            self.next()
            return tok.val
        self.error("Expected: %s" % what)

    def at_end(self):
        """At the end of a statement: a new line, a colon, or Else on a
        single-line If."""
        tok = self.peek()
        return tok.kind in ("nl", "eof") or (tok.kind == "op" and tok.val == ":")

    def end_statement(self):
        tok = self.peek()
        if tok.kind == "nl":
            self.next()
            return
        if tok.kind == "op" and tok.val == ":":
            self.next()
            return
        if tok.kind == "eof":
            return
        if tok.kind == "id" and tok.val.lower() == "else" and getattr(self, "single_if", 0):
            return
        self.error("Expected: end of statement")

    def skip_newlines(self):
        while self.peek().kind == "nl" or self.is_op(":"):
            self.next()

    # -- types --
    def parse_type(self):
        """After As: a type name, lower-cased; String * n is "string*n"."""
        new = self.accept_kw("new")
        name = self.ident("type")
        while self.accept_op("."):
            name = self.ident("type")        # Excel.Range is Range
        typ = name.lower()
        if typ == "string" and self.accept_op("*"):
            tok = self.next()
            if tok.kind == "num":
                typ = "string*%d" % int(tok.val)
            else:
                typ = "string*255"
        return typ, new

    # -- expressions --
    def parse_expr(self):
        return self.parse_imp()

    def _binary(self, sub, ops, kw=True):
        a = sub()
        while True:
            tok = self.peek()
            op = None
            if kw and tok.kind == "id" and tok.val.lower() in ops:
                op = tok.val.lower()
            elif not kw and tok.kind == "op" and tok.val in ops:
                op = tok.val
            if op is None:
                return a
            self.next()
            a = BinE(op, a, sub())

    def parse_imp(self):
        return self._binary(self.parse_eqv, ("imp",))

    def parse_eqv(self):
        return self._binary(self.parse_xor, ("eqv",))

    def parse_xor(self):
        return self._binary(self.parse_or, ("xor",))

    def parse_or(self):
        return self._binary(self.parse_and, ("or",))

    def parse_and(self):
        return self._binary(self.parse_not, ("and",))

    def parse_not(self):
        if self.accept_kw("not"):
            return UnE("not", self.parse_not())
        return self.parse_cmp()

    def parse_cmp(self):
        a = self.parse_concat()
        while True:
            tok = self.peek()
            if tok.kind == "op" and tok.val in ("=", "<>", "<", ">", "<=", ">="):
                op = tok.val
            elif tok.kind == "id" and tok.val.lower() in ("like", "is"):
                op = tok.val.lower()
            else:
                return a
            self.next()
            a = BinE(op, a, self.parse_concat())

    def parse_concat(self):
        return self._binary(self.parse_add, ("&",), kw=False)

    def parse_add(self):
        return self._binary(self.parse_mod, ("+", "-"), kw=False)

    def parse_mod(self):
        return self._binary(self.parse_intdiv, ("mod",))

    def parse_intdiv(self):
        return self._binary(self.parse_mul, ("\\",), kw=False)

    def parse_mul(self):
        return self._binary(self.parse_neg, ("*", "/"), kw=False)

    def parse_neg(self):
        if self.accept_op("-"):
            return UnE("-", self.parse_neg())
        if self.accept_op("+"):
            return self.parse_neg()
        return self.parse_pow()

    def parse_pow(self):
        a = self.parse_postfix()
        while self.accept_op("^"):
            if self.accept_op("-"):
                b = UnE("-", self.parse_postfix())
            else:
                self.accept_op("+")
                b = self.parse_postfix()
            a = BinE("^", a, b)
        return a

    def parse_args(self, closing):
        """Arguments up to `closing` (")" or the end of the statement):
        named ones as name:=value, left-out ones as nothing."""
        args = []
        if closing == ")" and self.accept_op(")"):
            return args
        if closing is None and self.at_end():
            return args
        while True:
            if self.is_op(","):
                args.append(ArgE(None, None))
                self.next()
                continue
            if closing == ")" and self.is_op(")"):
                args.append(ArgE(None, None))
                break
            if closing is None and self.at_end():
                args.append(ArgE(None, None))
                break
            name = None
            byval = False
            if self.peek().kind == "id" and self.is_op(":=", 1):
                name = self.next().val.lower()
                self.next()
            elif self.is_kw("byval") and self.peek(1).kind in ("id", "num", "str"):
                self.next()
                byval = True
            args.append(ArgE(name, self.parse_expr(), byval))
            if not self.accept_op(","):
                break
        if closing == ")":
            self.expect_op(")")
        return args

    def parse_primary(self):
        tok = self.peek()
        if tok.kind == "num" or tok.kind == "str" or tok.kind == "date":
            self.next()
            return Lit(tok.val)
        if tok.kind == "eval":
            self.next()
            return EvalE(tok.val)
        if tok.kind == "op":
            if tok.val == "(":
                self.next()
                e = self.parse_expr()
                self.expect_op(")")
                return ParenE(e)
            if tok.val == "#":
                # Input(5, #1), EOF(#1): a file's number.
                self.next()
                return self.parse_primary()
            if tok.val == "." or tok.val == "!":
                self.next()
                name = self.ident()
                return MemberE(WithE(), name) if tok.val == "." else BangE(WithE(), name)
            self.error("Expected: expression")
        if tok.kind == "id":
            low = tok.val.lower()
            if low == "true":
                self.next()
                return Lit(True)
            if low == "false":
                self.next()
                return Lit(False)
            if low == "nothing":
                self.next()
                return Lit(None)
            if low == "empty":
                self.next()
                return Lit(EMPTY)
            if low == "null":
                self.next()
                return Lit(NULL)
            if low == "me":
                self.next()
                return MeE()
            if low == "new":
                self.next()
                name = self.ident("class")
                while self.accept_op("."):
                    name = name + "." + self.ident("class")
                return NewE(name)
            if low == "typeof":
                self.next()
                e = self.parse_postfix()
                self.expect_kw("is")
                name = self.ident("type")
                while self.accept_op("."):
                    name = self.ident("type")
                return TypeOfE(e, name.lower())
            if low == "addressof":
                self.next()
                return Lit(self.ident())
            if low in _RESERVED and low not in ("me",):
                self.error("Expected: expression")
            self.next()
            return NameE(tok.val, tok.suffix, tok.line)
        self.error("Expected: expression")

    def parse_postfix(self, track=None):
        e = self.parse_primary()
        while True:
            tok = self.peek()
            if tok.kind != "op":
                return e
            if tok.val == ".":
                self.next()
                name_tok = self.peek()
                if name_tok.kind not in ("id", "eval"):
                    self.error("Expected: identifier")
                self.next()
                e = MemberE(e, name_tok.val)
                if track is not None:
                    track[0] = None
            elif tok.val == "!":
                self.next()
                e = BangE(e, self.ident())
                if track is not None:
                    track[0] = None
            elif tok.val == "(":
                if track is not None:
                    track[0] = self.i
                self.next()
                e = CallE(e, self.parse_args(")"))
            else:
                return e

    # -- statements --
    def parse_statement_list_line(self):
        """Statements up to the end of the line: a single-line If's."""
        body = []
        while True:
            tok = self.peek()
            if tok.kind in ("nl", "eof"):
                return body
            if tok.kind == "id" and tok.val.lower() == "else":
                return body
            if self.accept_op(":"):
                continue
            body.extend(self.parse_statement(inline=True))

    def parse_block(self, enders):
        """Statements until one of `enders` -- each a tuple of words,
        "end if" as ("end", "if") -- begins a statement."""
        body = []
        while True:
            self.skip_newlines()
            tok = self.peek()
            for words in enders:
                if all(self.is_kw(w, k) for k, w in enumerate(words)):
                    return body, words
            # The procedure ends with the block still open: what Visual
            # Basic says of it.
            if tok.kind == "eof" or (self.is_kw("end") and any(self.is_kw(w, 1) for w in
                                                              ("sub", "function", "property"))):
                self.error(_UNCLOSED.get(enders[-1], "Expected: %s" % " ".join(enders[0]).title()))
            # Another block's end, with this one still open.
            for words, message in _STRAY:
                if all(self.is_kw(w, k) for k, w in enumerate(words)):
                    self.error(message)
            body.extend(self.parse_statement())

    def parse_statement(self, inline=False):
        """One statement, or a label and the statement after it."""
        tok = self.peek()
        line = tok.line
        # A label: Name: at the start of a line, or a line number.
        if not inline:
            if tok.kind == "id" and self.is_op(":", 1) and tok.val.lower() not in _RESERVED:
                self.next()
                self.next()
                return [Stmt("label", line, name=tok.val.lower())]
            if tok.kind == "num" and isinstance(tok.val, int):
                self.next()
                self.accept_op(":")
                return [Stmt("label", line, name=str(tok.val))]
        st = self._statement()
        if st is None:
            return []
        if inline:
            # A single-line If's statement: the line's end is the If's.
            tok = self.peek()
            if tok.kind == "op" and tok.val == ":":
                self.next()
            elif tok.kind not in ("nl", "eof") and not (tok.kind == "id" and tok.val.lower() == "else"):
                self.error("Expected: end of statement")
        else:
            self.end_statement()
        return [st]

    def _statement(self):
        tok = self.peek()
        line = tok.line
        if tok.kind != "id":
            if tok.kind == "op" and tok.val in (".", "!"):
                return self.parse_expr_statement(line)
            if tok.kind == "eval":
                return self.parse_expr_statement(line)
            self.error("Expected: statement")
        w = tok.val.lower()

        if w in ("dim", "static") and not (w == "static" and (self.is_kw("sub", 1) or self.is_kw("function", 1))):
            self.next()
            return Stmt("dim", line, decls=self.parse_var_decls(), scope=w)
        if w == "const":
            self.next()
            return Stmt("const", line, decls=self.parse_const_decls())
        if w == "redim":
            self.next()
            preserve = self.accept_kw("preserve")
            items = []
            while True:
                name_tok = self.peek()
                target = NameE(self.ident("variable"), name_tok.suffix, name_tok.line)
                while self.accept_op("."):
                    target = MemberE(target, self.ident())
                dims = self.parse_bounds()
                typ = None
                if self.accept_kw("as"):
                    typ, _ = self.parse_type()
                items.append((target, dims, typ))
                if not self.accept_op(","):
                    break
            return Stmt("redim", line, preserve=preserve, items=items)
        if w == "set":
            self.next()
            target = self.parse_postfix()
            self.expect_op("=")
            return Stmt("assign", line, target=target, expr=self.parse_expr(), set=True)
        if w == "let":
            self.next()
            target = self.parse_postfix()
            self.expect_op("=")
            return Stmt("assign", line, target=target, expr=self.parse_expr(), set=False)
        if w == "lset" or w == "rset":
            self.next()
            target = self.parse_postfix()
            self.expect_op("=")
            return Stmt(w, line, target=target, expr=self.parse_expr())
        if w == "call":
            self.next()
            e = self.parse_postfix()
            return Stmt("call", line, expr=e)
        if w == "if":
            return self.parse_if()
        if w == "select":
            return self.parse_select()
        if w == "for":
            return self.parse_for()
        if w == "do":
            return self.parse_do()
        if w == "while":
            self.next()
            cond = self.parse_expr()
            self.end_statement()
            body, _ = self.parse_block([("wend",), ("end", "while")])
            self.next()
            if self.is_kw("while"):
                self.next()
            return Stmt("while", line, cond=cond, body=body)
        if w == "with":
            self.next()
            e = self.parse_expr()
            self.end_statement()
            body, _ = self.parse_block([("end", "with")])
            self.next()
            self.next()
            return Stmt("with", line, expr=e, body=body)
        if w == "exit":
            self.next()
            kind = self.ident("Do, For, Function, Property or Sub").lower()
            if kind not in ("do", "for", "function", "property", "sub"):
                self.error("Expected: Do or For or Sub or Function or Property")
            return Stmt("exit", line, what=kind)
        if w == "goto":
            self.next()
            return Stmt("goto", line, label=self.label_name())
        if w == "gosub":
            self.next()
            return Stmt("gosub", line, label=self.label_name())
        if w == "return":
            self.next()
            return Stmt("return", line)
        if w == "on":
            return self.parse_on()
        if w == "resume":
            self.next()
            if self.at_end() or self.is_kw("else"):
                return Stmt("resume", line, how="retry", label=None)
            if self.accept_kw("next"):
                return Stmt("resume", line, how="next", label=None)
            lab = self.label_name()
            if lab == "0":
                return Stmt("resume", line, how="retry", label=None)
            return Stmt("resume", line, how="label", label=lab)
        if w == "end":
            self.next()
            if self.at_end() or self.is_kw("else"):
                return Stmt("end", line)
            self.error("Expected: end of statement", tok)
        if w == "stop":
            self.next()
            return Stmt("stop", line)
        if w == "debug" and self.is_op(".", 1) and self.is_kw("print", 2):
            self.next()
            self.next()
            self.next()
            return Stmt("print", line, file=None, items=self.parse_print_items())
        if w == "print" and self.is_op("#", 1):
            self.next()
            self.next()
            f = self.parse_expr()
            items = []
            if self.accept_op(","):
                items = self.parse_print_items()
            return Stmt("print", line, file=f, items=items)
        if w == "write" and self.is_op("#", 1):
            self.next()
            self.next()
            f = self.parse_expr()
            items = []
            if self.accept_op(","):
                while not self.at_end():
                    items.append(self.parse_expr())
                    if not (self.accept_op(",") or self.accept_op(";")):
                        break
            return Stmt("write", line, file=f, items=items)
        if w == "open":
            return self.parse_open()
        if w == "close":
            self.next()
            nums = []
            while not self.at_end():
                self.accept_op("#")
                nums.append(self.parse_expr())
                if not self.accept_op(","):
                    break
            return Stmt("close", line, files=nums)
        if w == "line" and self.is_kw("input", 1):
            self.next()
            self.next()
            self.expect_op("#")
            f = self.parse_expr()
            self.expect_op(",")
            return Stmt("lineinput", line, file=f, target=self.parse_postfix())
        if w == "input" and self.is_op("#", 1):
            self.next()
            self.next()
            f = self.parse_expr()
            targets = []
            while self.accept_op(","):
                targets.append(self.parse_postfix())
            return Stmt("input", line, file=f, targets=targets)
        if w in ("get", "put") and self.is_op("#", 1):
            self.next()
            self.next()
            f = self.parse_expr()
            self.expect_op(",")
            rec = None
            if not self.is_op(","):
                rec = self.parse_expr()
            self.expect_op(",")
            return Stmt(w, line, file=f, rec=rec, target=self.parse_postfix())
        if w == "seek" and self.is_op("#", 1):
            self.next()
            self.next()
            f = self.parse_expr()
            self.expect_op(",")
            return Stmt("seek", line, file=f, pos=self.parse_expr())
        if w == "name" and not self.is_op("=", 1) and not self.is_op(".", 1) and not self.is_op("(", 1):
            self.next()
            old = self.parse_expr()
            self.expect_kw("as")
            return Stmt("name", line, old=old, new=self.parse_expr())
        if w == "erase":
            self.next()
            targets = [self.parse_postfix()]
            while self.accept_op(","):
                targets.append(self.parse_postfix())
            return Stmt("erase", line, targets=targets)
        if w == "error" and not self.is_op("=", 1) and not self.is_op(".", 1) and not self.at_end_after(1):
            self.next()
            return Stmt("error", line, expr=self.parse_expr())
        if w == "raiseevent":
            self.next()
            name = self.ident()
            args = []
            if self.accept_op("("):
                args = self.parse_args(")")
            return Stmt("raiseevent", line, name=name, args=args)
        if w in ("mid", "mid$", "midb") and self.is_op("(", 1):
            # Mid(s, 2, 3) = "abc" replaces characters in place.
            save = self.i
            e = self.parse_postfix()
            if self.accept_op("="):
                if not isinstance(e, CallE) or len(e.args) < 2:
                    self.error("Expected: variable")
                return Stmt("midassign", line, target=e.args[0].expr, start=e.args[1].expr,
                            length=e.args[2].expr if len(e.args) > 2 else None, expr=self.parse_expr())
            self.i = save
        if w in ("else", "elseif", "loop", "next", "wend", "case") or \
                (w == "end" and not self.at_end_after(1)):
            self.error("%s without block" % tok.val.capitalize())
        if w in ("sub", "function", "property", "private", "public", "global", "friend",
                 "declare", "type", "enum", "option", "implements", "event"):
            self.error("Invalid inside procedure")
        if w == "attribute":
            # Attribute Item.VB_UserMemId = 0: the class's default
            # member; -4, what For Each asks for.
            self.next()
            parts = [self.ident()]
            while self.accept_op("."):
                parts.append(self.ident())
            value = None
            if self.accept_op("="):
                if self.accept_op("-"):
                    value = -self.next().val
                else:
                    value = self.next().val
            if len(parts) >= 2:
                self.attributes.append((parts[0].lower(), ".".join(parts[1:]).lower(), value))
            while not self.at_end():
                self.next()
            return None
        if w in ("load", "unload") and not self.is_op("(", 1) and not self.is_op("=", 1):
            self.next()
            return Stmt(w, line, expr=self.parse_expr())
        return self.parse_expr_statement(line)

    def at_end_after(self, k):
        tok = self.peek(k)
        return tok.kind in ("nl", "eof") or (tok.kind == "op" and tok.val == ":")

    def label_name(self):
        tok = self.next()
        if tok.kind == "num":
            return str(int(tok.val))
        if tok.kind == "id":
            return tok.val.lower()
        self.error("Expected: line number or label", tok)

    def parse_bounds(self):
        """(1 To 5, 10) after a name: [(lb or None, ub)], [] for ()."""
        self.expect_op("(")
        dims = []
        if self.accept_op(")"):
            return dims
        while True:
            a = self.parse_expr()
            if self.accept_kw("to"):
                dims.append((a, self.parse_expr()))
            else:
                dims.append((None, a))
            if not self.accept_op(","):
                break
        self.expect_op(")")
        return dims

    def parse_var_decls(self):
        decls = []
        while True:
            line = self.peek().line
            with_events = self.accept_kw("withevents")
            tok = self.peek()
            name = self.ident("variable")
            typ = _SUFFIX_TYPES.get(tok.suffix, "variant")
            dims = None
            dynamic = False
            if self.is_op("("):
                dims = self.parse_bounds()
                if not dims:
                    dynamic = True
                    dims = None
            new = False
            if self.accept_kw("as"):
                typ, new = self.parse_type()
            decls.append(VarDecl(name, typ, dims, dynamic, new, line, with_events))
            if not self.accept_op(","):
                return decls

    def parse_const_decls(self):
        decls = []
        while True:
            tok = self.peek()
            name = self.ident("constant")
            typ = _SUFFIX_TYPES.get(tok.suffix)
            if self.accept_kw("as"):
                typ, _ = self.parse_type()
            self.expect_op("=")
            decls.append((name, typ, self.parse_expr()))
            if not self.accept_op(","):
                return decls

    def parse_print_items(self):
        """Debug.Print a; b, c: the values and what follows each."""
        items = []
        while not self.at_end() and not self.is_kw("else"):
            if self.is_op(";") or self.is_op(","):
                items.append((None, self.next().val))
                continue
            if (self.is_kw("spc") or self.is_kw("tab")) and self.is_op("(", 1):
                which = self.next().val.lower()
                self.next()
                arg = None if self.is_op(")") else self.parse_expr()
                self.expect_op(")")
                items.append(((which, arg), None))
            else:
                items.append((self.parse_expr(), None))
            if self.is_op(";") or self.is_op(","):
                items[-1] = (items[-1][0], self.next().val)
        return items

    def parse_open(self):
        line = self.next().line
        path = self.parse_expr()
        mode = "random"
        if self.accept_kw("for"):
            mode = self.ident("Input, Output, Append, Binary or Random").lower()
        if self.accept_kw("access"):
            while self.peek().kind == "id" and self.peek().val.lower() in ("read", "write"):
                self.next()
        while self.peek().kind == "id" and self.peek().val.lower() in ("shared", "lock", "read", "write"):
            self.next()
        self.expect_kw("as")
        self.accept_op("#")
        num = self.parse_expr()
        reclen = None
        if self.accept_kw("len"):
            self.expect_op("=")
            reclen = self.parse_expr()
        return Stmt("open", line, path=path, mode=mode, file=num, reclen=reclen)

    def parse_on(self):
        line = self.next().line
        self.accept_kw("local")
        if self.accept_kw("error"):
            if self.accept_kw("resume"):
                self.expect_kw("next")
                return Stmt("onerror", line, how="next", label=None)
            self.expect_kw("goto")
            if self.accept_op("-"):
                self.next()
                return Stmt("onerror", line, how="reset", label=None)
            lab = self.label_name()
            if lab == "0":
                return Stmt("onerror", line, how="off", label=None)
            return Stmt("onerror", line, how="goto", label=lab)
        e = self.parse_expr()
        kind = "goto" if self.accept_kw("goto") else ("gosub" if self.accept_kw("gosub") else None)
        if kind is None:
            self.error("Expected: GoTo or GoSub")
        labels = [self.label_name()]
        while self.accept_op(","):
            labels.append(self.label_name())
        return Stmt("ongoto", line, expr=e, labels=labels, gosub=kind == "gosub")

    def parse_if(self):
        line = self.next().line
        cond = self.parse_expr()
        self.expect_kw("then")
        if not self.at_end() or (self.is_op(":") and not self.at_end_after(1)):
            # A single-line If: statements after Then, maybe Else and more.
            self.single_if = getattr(self, "single_if", 0) + 1
            try:
                if self.peek().kind == "num":
                    then_body = [Stmt("goto", line, label=str(int(self.next().val)))]
                else:
                    then_body = self.parse_statement_list_line()
                else_body = []
                if self.accept_kw("else"):
                    if self.peek().kind == "num":
                        else_body = [Stmt("goto", line, label=str(int(self.next().val)))]
                    else:
                        else_body = self.parse_statement_list_line()
            finally:
                self.single_if -= 1
            return Stmt("if", line, branches=[(cond, then_body)], orelse=else_body, single=True)
        self.end_statement()
        branches = []
        orelse = []
        body, ender = self.parse_block([("elseif",), ("else",), ("end", "if"), ("endif",)])
        branches.append((cond, body))
        while True:
            if ender == ("elseif",):
                self.next()
                c = self.parse_expr()
                self.expect_kw("then")
                self.end_statement()
                body, ender = self.parse_block([("elseif",), ("else",), ("end", "if"), ("endif",)])
                branches.append((c, body))
            elif ender == ("else",):
                self.next()
                if self.is_kw("if"):
                    self.error("Expected: end of statement")
                self.end_statement()
                orelse, ender = self.parse_block([("end", "if"), ("endif",)])
            else:
                for _ in ender:
                    self.next()
                return Stmt("if", line, branches=branches, orelse=orelse, single=False)

    def parse_select(self):
        line = self.next().line
        self.expect_kw("case")
        test = self.parse_expr()
        self.end_statement()
        cases = []
        orelse = None
        self.skip_newlines()
        while True:
            self.skip_newlines()
            if self.is_kw("end") and self.is_kw("select", 1):
                self.next()
                self.next()
                return Stmt("select", line, test=test, cases=cases, orelse=orelse or [])
            if not self.accept_kw("case"):
                self.error("Expected: Case")
            if self.accept_kw("else"):
                self.accept_op(":")
                body, _ = self.parse_block([("case",), ("end", "select")])
                orelse = body
                continue
            conds = []
            while True:
                if self.accept_kw("is"):
                    op = self.next()
                    if op.kind != "op" or op.val not in ("=", "<>", "<", ">", "<=", ">="):
                        self.error("Expected: comparison operator", op)
                    conds.append(("is", op.val, self.parse_expr()))
                else:
                    a = self.parse_expr()
                    if self.accept_kw("to"):
                        conds.append(("range", a, self.parse_expr()))
                    else:
                        conds.append(("eq", a))
                if not self.accept_op(","):
                    break
            if not self.at_end():
                self.error("Expected: end of statement")
            body, _ = self.parse_block([("case",), ("end", "select")])
            cases.append((conds, body))

    def parse_for(self):
        line = self.next().line
        if self.accept_kw("each"):
            var = self.parse_postfix()
            self.expect_kw("in")
            coll = self.parse_expr()
            self.end_statement()
            body = self.parse_loop_body()
            return Stmt("foreach", line, var=var, coll=coll, body=body)
        var = self.parse_postfix()
        self.expect_op("=")
        start = self.parse_expr()
        self.expect_kw("to")
        end = self.parse_expr()
        step = None
        if self.accept_kw("step"):
            step = self.parse_expr()
        self.end_statement()
        body = self.parse_loop_body()
        return Stmt("for", line, var=var, start=start, end=end, step=step, body=body)

    def parse_loop_body(self):
        body, _ = self.parse_block([("next",)])
        self.next()
        if not self.at_end():
            self.parse_postfix()        # Next i: the name is not checked
            if self.is_op(","):
                # Next j, i ends two loops: the outer one sees "Next i".
                comma = self.next()
                self.t[self.i:self.i] = [Token("op", ":", comma.line), Token("id", "Next", comma.line)]
                return body
        return body

    def parse_do(self):
        line = self.next().line
        pre = None
        if self.is_kw("while") or self.is_kw("until"):
            kind = self.next().val.lower()
            pre = (kind, self.parse_expr())
        self.end_statement()
        body, _ = self.parse_block([("loop",)])
        self.next()
        post = None
        if self.is_kw("while") or self.is_kw("until"):
            kind = self.next().val.lower()
            post = (kind, self.parse_expr())
        return Stmt("do", line, pre=pre, post=post, body=body)

    def parse_expr_statement(self, line):
        """An assignment, or a call with or without its arguments in
        brackets: x = 1, MsgBox "Hi", Foo (a), b, ws.Range("A1").Select."""
        track = [None]
        e = self.parse_postfix(track)
        if self.accept_op("="):
            return Stmt("assign", line, target=e, expr=self.parse_expr(), set=False)
        if self.at_end() or self.is_kw("else"):
            return Stmt("call", line, expr=e)
        if track[0] is not None and isinstance(e, CallE):
            # Foo (a) + 1, b: the brackets began the first argument.
            self.i = track[0]
            callee = e.target
        else:
            callee = e
        args = self.parse_args(None)
        return Stmt("call", line, expr=CallE(callee, args), bare=True)

    # -- the module --
    def parse_params(self):
        params = []
        if not self.accept_op("("):
            return params
        if self.accept_op(")"):
            return params
        while True:
            optional = self.accept_kw("optional")
            byval = False
            if self.accept_kw("byval"):
                byval = True
            elif self.accept_kw("byref"):
                byval = False
            paramarray = self.accept_kw("paramarray")
            tok = self.peek()
            name = self.ident("parameter")
            typ = _SUFFIX_TYPES.get(tok.suffix, "variant")
            is_array = False
            if self.accept_op("("):
                self.expect_op(")")
                is_array = True
            if self.accept_kw("as"):
                typ, _ = self.parse_type()
            default = None
            if self.accept_op("="):
                default = self.parse_expr()
            params.append(Param(name, byval, optional, paramarray, typ, default, is_array))
            if not self.accept_op(","):
                break
        self.expect_op(")")
        return params

    def parse_module(self):
        m = ModuleAST()
        while True:
            self.skip_newlines()
            tok = self.peek()
            if tok.kind == "eof":
                for proc, attr, value in self.attributes:
                    m.attributes[proc + "." + attr] = value
                return m
            self.parse_module_item(m)

    def parse_module_item(self, m):
        tok = self.peek()
        line = tok.line
        if tok.kind != "id":
            self.error("Invalid outside procedure")
        w = tok.val.lower()
        if w == "attribute":
            self.next()
            name = self.ident()
            while self.accept_op("."):
                name += "." + self.ident()
            if self.accept_op("="):
                vals = [self.parse_expr()]
                while self.accept_op(","):
                    vals.append(self.parse_expr())
                if isinstance(vals[0], Lit):
                    m.attributes[name.lower()] = vals[0].value
            self.end_statement()
            return
        if w == "option":
            self.next()
            what = self.ident().lower()
            if what == "explicit":
                m.options["explicit"] = True
            elif what == "base":
                tok = self.next()
                m.options["base"] = int(tok.val) if tok.kind == "num" else 0
            elif what == "compare":
                m.options["compare"] = self.ident().lower()
            elif what == "private":
                self.expect_kw("module")
                m.options["private"] = True
            self.end_statement()
            return
        if w.startswith("def") and w in ("defbool", "defbyte", "defint", "deflng", "deflnglng",
                                          "deflngptr", "defcur", "defsng", "defdbl", "defdec",
                                          "defdate", "defstr", "defobj", "defvar"):
            while not self.at_end():
                self.next()
            self.end_statement()
            return
        if w == "implements":
            self.next()
            name = self.ident()
            while self.accept_op("."):
                name = self.ident()
            m.implements.append(name.lower())
            self.end_statement()
            return
        private = False
        scope = None
        if w in ("public", "private", "global", "friend"):
            scope = w
            private = w == "private"
            self.next()
            tok = self.peek()
            w = tok.val.lower() if tok.kind == "id" else ""
        static = False
        if w == "static":
            static = True
            self.next()
            tok = self.peek()
            w = tok.val.lower() if tok.kind == "id" else ""
        if w in ("sub", "function", "property"):
            m.procs.append(self.parse_proc(private, static))
            return
        if w == "declare":
            self.next()
            self.accept_kw("ptrsafe")
            kind = self.ident().lower()
            name = self.ident()
            self.expect_kw("lib")
            lib = self.next().val
            alias = None
            if self.accept_kw("alias"):
                alias = self.next().val
            params = self.parse_params()
            ret = "variant"
            if self.accept_kw("as"):
                ret, _ = self.parse_type()
            m.declares.append(DeclareNode(name, kind, lib, alias, params, ret, private))
            self.end_statement()
            return
        if w == "type":
            self.next()
            name = self.ident()
            self.end_statement()
            fields = []
            while True:
                self.skip_newlines()
                if self.is_kw("end") and self.is_kw("type", 1):
                    self.next()
                    self.next()
                    break
                fname_tok = self.peek()
                fname = self.ident("field")
                dims = None
                if self.is_op("("):
                    dims = self.parse_bounds()
                ftype = _SUFFIX_TYPES.get(fname_tok.suffix, "variant")
                if self.accept_kw("as"):
                    ftype, _ = self.parse_type()
                fields.append((fname, ftype, dims))
                self.end_statement()
            m.types.append(TypeDefNode(name, fields, private))
            self.end_statement()
            return
        if w == "enum":
            self.next()
            name = self.ident()
            self.end_statement()
            members = []
            while True:
                self.skip_newlines()
                if self.is_kw("end") and self.is_kw("enum", 1):
                    self.next()
                    self.next()
                    break
                mname = self.ident("enum member")
                value = None
                if self.accept_op("="):
                    value = self.parse_expr()
                members.append((mname, value))
                self.end_statement()
            m.enums.append(EnumDefNode(name, members, private))
            self.end_statement()
            return
        if w == "event":
            self.next()
            name = self.ident()
            self.parse_params()
            m.events.append(name)
            self.end_statement()
            return
        if w == "const":
            self.next()
            m.decls.append(Stmt("const", line, decls=self.parse_const_decls(), private=private or scope is None))
            self.end_statement()
            return
        if w == "dim" or (scope is not None and w not in ("sub", "function", "property")):
            if w == "dim":
                self.next()
            m.decls.append(Stmt("dim", line, decls=self.parse_var_decls(),
                                private=private or scope in (None, "dim")))
            self.end_statement()
            return
        self.error("Invalid outside procedure")

    def parse_proc(self, private, static):
        tok = self.next()
        line = tok.line
        kind = tok.val.lower()
        if kind == "property":
            kind = self.ident("Get, Let or Set").lower()
            if kind not in ("get", "let", "set"):
                self.error("Expected: Get or Let or Set")
        name_tok = self.peek()
        name = self.ident("procedure name")
        params = self.parse_params()
        ret = _SUFFIX_TYPES.get(name_tok.suffix, "variant")
        ret_array = False
        if self.accept_kw("as"):
            ret, _ = self.parse_type()
            if self.accept_op("("):
                self.expect_op(")")
                ret_array = True
        self.end_statement()
        ender = {"sub": "sub", "function": "function"}.get(kind, "property")
        body, _ = self.parse_block([("end", ender)])
        end_line = self.peek().line
        self.next()
        self.next()
        self.end_statement()
        p = ProcDef(kind, name, params, ret, body, private, static, line, end_line)
        p.ret_array = ret_array
        return p


_PROC_HEAD = re.compile(r"^\s*(?:(public|private|friend|global)\s+)?(?:static\s+)?"
                        r"(sub|function|property\s+(?:get|let|set))\s+([a-z_][a-z0-9_]*)", re.I)


def salvage_module(text):
    """What can be told of a module that does not parse: the names of its
    procedures, so that calls to them compile elsewhere and fail, when
    made, with the module's error."""
    m = ModuleAST()
    seen = set()
    for i, line in enumerate(text.split("\n"), 1):
        head = _PROC_HEAD.match(line)
        if head is None:
            continue
        kind = head.group(2).lower().split()[-1]
        key = (head.group(3).lower(), kind)
        if key in seen:
            continue
        seen.add(key)
        m.procs.append(ProcDef(kind, head.group(3), [], "variant", [],
                               (head.group(1) or "").lower() == "private", False, i, i))
    return m


def parse_module(text, name=""):
    lines = preprocess(Lexer(text, name).lines(), name)
    tokens = []
    for start, toks in lines:
        if not toks:
            continue
        tokens.extend(toks)
        tokens.append(Token("nl", None, toks[-1].line))
    tokens.append(Token("eof", None, lines[-1][0] if lines else 1))
    return Parser(tokens, name).parse_module()


def const_eval(e, lookup):
    """A constant expression's value: what Const and #If are given."""
    if isinstance(e, Lit):
        return e.value
    if isinstance(e, ParenE):
        return const_eval(e.expr, lookup)
    if isinstance(e, NameE):
        v = lookup(e.name.lower())
        if v is MISSING:
            raise VBASyntaxError("Constant expression required", e.line)
        return v
    if isinstance(e, UnE):
        x = const_eval(e.x, lookup)
        return op_neg(x) if e.op == "-" else op_not(x)
    if isinstance(e, BinE):
        a, b = const_eval(e.a, lookup), const_eval(e.b, lookup)
        return BINARY_OPS[e.op](a, b)
    if isinstance(e, CallE) and isinstance(e.target, NameE):
        fn = LIBRARY.get(e.target.name.lower())
        if fn is not None:
            return fn(*[const_eval(a.expr, lookup) for a in e.args])
    if isinstance(e, MemberE):
        # Enum.Member
        v = lookup(e.name.lower())
        if v is not MISSING:
            return v
    raise VBASyntaxError("Constant expression required", 0)


BINARY_OPS = {
    "+": op_add, "-": op_sub, "*": op_mul, "/": op_div, "\\": op_intdiv, "mod": op_mod,
    "^": op_pow, "&": op_concat, "=": op_eq, "<>": op_ne, "<": op_lt, "<=": op_le,
    ">": op_gt, ">=": op_ge, "like": op_like, "is": op_is, "and": op_and, "or": op_or,
    "xor": op_xor, "eqv": op_eqv, "imp": op_imp,
}
_TEXT_OPS = {"=", "<>", "<", "<=", ">", ">=", "like"}


# ---- Running -------------------------------------------------------------------------

# While a macro is being stepped: called with (frame, line) before each
# statement.  None otherwise, and then it costs one look at a global.
_trace = None


def set_trace(fn):
    global _trace
    _trace = fn


# While a macro runs in a window: called a few times a second with how
# long it has been running, from the start of every block of statements
# -- a loop's every turn, every procedure called -- so that a loop that
# never ends can still be stopped with Esc; and with -1 when it is over,
# if it was called at all.  None otherwise, and then it costs one look at
# a global.
_poll = None
_poll_count = 0
_poll_next = 0.0
_polled = False
_run_depth = 0
_run_started = 0.0


def set_poll(fn):
    global _poll
    _poll = fn


def _check_poll():
    global _poll_next, _polled
    now = time.monotonic()
    if now < _poll_next:
        return
    _poll_next = now + 0.1
    _polled = True
    _poll(now - _run_started)


def _run_begin():
    global _run_depth, _run_started, _poll_next, _polled
    if _run_depth == 0:
        _run_started = time.monotonic()
        _poll_next = _run_started + 0.1
        _polled = False
    _run_depth += 1


def _run_end():
    global _run_depth, _polled
    _run_depth -= 1
    if _run_depth == 0 and _polled:
        _polled = False
        _poll(-1.0)


class Frame:
    """A procedure being run: its variables, the object it runs for, the
    With objects open, and what it does with an error."""

    __slots__ = ("v", "me", "w", "on_error", "in_handler", "proc", "err_line")

    def __init__(self, proc, me):
        self.proc = proc
        self.me = me
        self.v = [None] * proc.nslots
        self.w = []
        self.on_error = None        # None, "next" or a label
        self.in_handler = False
        self.err_line = 0


class ParamRT:
    __slots__ = ("name", "lname", "slot", "byval", "optional", "paramarray", "typ",
                 "default", "is_array")


class Proc:
    """A compiled Sub, Function or Property procedure."""

    def __init__(self, module, node):
        self.module = module
        self.node = node
        self.name = node.name
        self.lname = node.name.lower()
        self.kind = node.kind
        self.private = node.private
        self.params = []
        self.nslots = 0
        self.init = []              # (slot, factory) run as the frame is made
        self.body = None            # the top-level block
        self.statics = None         # Static variables' values
        self.ret_type = node.ret_type
        self.compiled = False
        self.line = node.line
        self.end_line = node.end_line

    def __repr__(self):
        return "<%s %s.%s>" % (self.kind.capitalize(), self.module.name, self.name)

    @property
    def is_function(self):
        return self.kind in ("function", "get")

    def min_args(self):
        return sum(1 for p in self.params if not p.optional and not p.paramarray)

    def max_args(self):
        return -1 if any(p.paramarray for p in self.params) else len(self.params)


class ArgC:
    """A compiled argument: its value, and where it lives for ByRef."""

    __slots__ = ("name", "missing", "getv", "getref", "paren")

    def __init__(self, name, missing, getv, getref, paren):
        self.name = name
        self.missing = missing
        self.getv = getv
        self.getref = getref
        self.paren = paren


class ValueArg:
    """An argument that is already a value: what the host and Run pass."""

    __slots__ = ("value", "name", "missing", "paren")

    def __init__(self, value, name=None):
        self.value = value
        self.name = name
        self.missing = value is MISSING
        self.paren = False

    def getv(self, fr):
        return self.value

    def getref(self, fr):
        return None


class RefArg(ValueArg):
    """An argument the host passes ByRef and reads back afterwards: the
    Cancel of Workbook_BeforeSave."""

    __slots__ = ("cell",)

    def __init__(self, value, name=None):
        ValueArg.__init__(self, value, name)
        self.cell = Cell(value)

    def getv(self, fr):
        return self.cell.value

    def getref(self, fr):
        return self.cell


class TypeDef:
    def __init__(self, project, name, fields):
        self.project = project
        self.name = name
        self.fields = fields        # {lname: (typ, dims as [(lb, ub)] or None)}
        self.display = {}


class VBObject:
    """An instance of a class module: its own copy of the module's
    variables, and the module's procedures as its members."""

    __slots__ = ("cls", "v", "gen", "ext", "__weakref__")

    def __init__(self, cls):
        self.cls = cls
        self.ext = None             # what the host adds: a UserForm's controls
        self.gen = cls.project.generation
        self.v = [None] * len(cls.var_list)
        for slot, factory in cls.var_init:
            self.v[slot] = factory()
        if cls.with_events:
            cls.instances.add(self)

    def __del__(self):
        # Class_Terminate, as the last reference goes, which is when
        # Visual Basic runs it too -- but not for what End, a reset or a
        # project compiled afresh lets go of, which it drops without.
        cls = self.cls
        project = cls.project
        if current is not project or self.gen != project.generation:
            return
        group = cls.procs.get("userform_terminate" if cls.form is not None else "class_terminate")
        proc = group.get("sub") if group else None
        if proc is None:
            return
        try:
            project.invoke(proc, [], None, self)
        except BaseException:
            pass

    def __repr__(self):
        return "<%s>" % self.cls.name

    def _vba_default_get(self, *args):
        name = self.cls.default_member
        if name is None:
            raise VBAError(438)
        return self.cls.project.member_get(self, name, [ValueArg(a) for a in args], None)

    def _vba_default_let(self, value, *args):
        name = self.cls.default_member
        if name is None:
            raise VBAError(438)
        self.cls.project.member_let(self, name, [ValueArg(a) for a in args], value, False, None)

    def _vba_iter(self):
        name = self.cls.enum_member
        if name is None:
            raise VBAError(438)
        return vba_iter(self.cls.project.member_get(self, name, [], None))


class Collection:
    """VBA.Collection: items in order, found by position from 1 or by a
    key that ignores case."""

    def __init__(self):
        self.items = []
        self.keys = []

    def _index(self, index):
        if isinstance(index, str):
            k = index.lower()
            for i, key in enumerate(self.keys):
                if key == k:
                    return i
            raise VBAError(5)
        i = to_long(index) - 1
        if i < 0 or i >= len(self.items):
            raise VBAError(9)
        return i

    def Add(self, Item, Key=MISSING, Before=MISSING, After=MISSING):
        k = None
        if Key is not MISSING:
            k = to_str(Key).lower()
            if k in self.keys:
                raise VBAError(457)
        if Before is not MISSING:
            at = self._index(Before)
        elif After is not MISSING:
            at = self._index(After) + 1
        else:
            at = len(self.items)
        self.items.insert(at, Item)
        self.keys.insert(at, k)

    def Item(self, Index):
        return self.items[self._index(Index)]

    def Remove(self, Index):
        i = self._index(Index)
        del self.items[i]
        del self.keys[i]

    @property
    def Count(self):
        return len(self.items)

    def _vba_default_get(self, *args):
        if not args:
            raise VBAError(450)
        return self.Item(args[0])

    def _vba_iter(self):
        return list(self.items)


class ErrObject:
    """Err: what the last error was."""

    def __init__(self):
        self.Clear()

    def Clear(self):
        self.Number = 0
        self.Description = ""
        self.Source = ""
        self.HelpFile = ""
        self.HelpContext = 0
        self.LastDllError = 0

    def Raise(self, Number, Source=MISSING, Description=MISSING, HelpFile=MISSING, HelpContext=MISSING):
        n = to_long(Number)
        e = VBAError(n, to_str(Description) if Description is not MISSING else None,
                     to_str(Source) if Source is not MISSING else "")
        raise e

    def set(self, e):
        self.Number = e.number
        self.Description = e.description
        self.Source = e.source or "VBAProject"

    def _vba_default_get(self, *args):
        return self.Number


class DebugObject:
    def __init__(self, project):
        self._project = project

    def Print(self, *values):
        self._project.write_debug("".join(print_text(v) for v in values) + "\n")

    def Assert(self, Condition):
        if not truth(Condition):
            raise StopSignal()


def print_text(v):
    """A value as Print writes it: a number with a place for its sign
    before it and a space after."""
    t = type(v)
    if t is int or t is float:
        text = number_text(v) if t is float else str(v)
        return (" " if v >= 0 else "") + text + " "
    if t is bool:
        return "True" if v else "False"
    if v is NULL:
        return "Null"
    if isinstance(v, ErrValue):
        return "Error %d" % v.code
    if v is EMPTY or v is MISSING:
        return ""
    return to_str(v)


def vba_iter(obj):
    """What For Each goes through."""
    if isinstance(obj, VBArray):
        return list(obj.data)
    if obj is None:
        raise VBAError(91)
    f = getattr(obj, "_vba_iter", None)
    if f is not None:
        return f()
    if isinstance(obj, (list, tuple)):
        return list(obj)
    if isinstance(obj, (int, float, str)) or obj is EMPTY:
        raise VBAError(451 if isinstance(obj, str) else 13)
    raise VBAError(438)


# -- Calling Python: the library and the host -------------------------------------

_signatures = {}


def _python_params(fn):
    key = getattr(fn, "__func__", fn)
    sig = _signatures.get(key)
    if sig is None:
        try:
            params = list(inspect.signature(fn).parameters.values())
        except (TypeError, ValueError):
            params = []
        names = [p.name.lower() for p in params if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)]
        pynames = [p.name for p in params if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)]
        varargs = any(p.kind == p.VAR_POSITIONAL for p in params)
        sig = _signatures[key] = (names, pynames, varargs)
    return sig


def call_python(fn, values, named):
    """Calls a library or host function with Visual Basic's arguments:
    left-out ones take the function's defaults, named ones go by name."""
    if not named and MISSING not in values:
        return fn(*values)
    names, pynames, varargs = _python_params(fn)
    kwargs = {}
    pos = []
    for i, v in enumerate(values):
        if v is MISSING:
            if i < len(names):
                continue
            pos.append(v)
            continue
        if i < len(names) and (kwargs or any(x is MISSING for x in values[:i])):
            kwargs[pynames[i]] = v
        else:
            pos.append(v)
    for k, v in named.items():
        if k in names:
            kwargs[pynames[names.index(k)]] = v
        else:
            raise VBAError(448)
    return fn(*pos, **kwargs)


_member_names = {}


def python_member(obj, lname):
    """The attribute of a Python object that a Visual Basic member name
    means, whatever its case: ("attr" name, or None)."""
    cls = type(obj)
    table = _member_names.get(cls)
    if table is None:
        table = {}
        for name in dir(cls):
            if not name.startswith("_"):
                table.setdefault(name.lower(), name)
        extra = getattr(cls, "_vba_aliases", None)
        if extra:
            for k, v in extra.items():
                table[k.lower()] = v
        _member_names[cls] = table
    attr = table.get(lname)
    if attr is None:
        # Plain attributes an object set on itself: Err.Number, a
        # RegExp's Pattern.
        for name in getattr(obj, "__dict__", ()):
            if name.lower() == lname and not name.startswith("_"):
                return name
    return attr


def _is_property(obj, attr):
    return isinstance(getattr(type(obj), attr, None), property)


class Scope:
    """What a name means while a procedure is compiled."""

    def __init__(self, module, proc=None):
        self.module = module
        self.project = module.project
        self.proc = proc
        self.locals = {}
        self.with_depth = 0
        self.nslots = 1 if proc is not None else 0
        self.init = []
        self.labels_seen = set()


class Local:
    __slots__ = ("slot", "typ", "param", "is_array", "static", "decl_object")

    def __init__(self, slot, typ, param=None, is_array=False, static=False):
        self.slot = slot
        self.typ = typ
        self.param = param
        self.is_array = is_array
        self.static = static
        self.decl_object = False


class ModuleRT:
    """A module read and compiled: its variables, constants and procedures."""

    def __init__(self, project, name, kind, text):
        self.project = project
        self.name = name
        self.lname = name.lower()
        self.kind = kind            # standard class document form
        self.text = text
        self.ast = None
        self.v = []
        self.var_list = []          # (lname, typ, decl)
        self.vars = {}              # lname -> (slot, typ, is_array, private)
        self.var_init = []          # (slot, factory)
        self.consts = {}            # lname -> value
        self.const_private = {}
        self.procs = {}             # lname -> {kind: Proc}
        self.enums = {}             # lname -> {member: value}
        self.enum_members = {}      # lname -> value
        self.types = {}             # lname -> TypeDef
        self.declares = {}
        self.text_compare = False
        self.base = 0
        self.explicit = False
        self.private_module = False
        self.default_member = None
        self.enum_member = None
        # Dim WithEvents x As SomeClass: (slot, class, variable), and for a
        # class the instances living now, among which RaiseEvent looks
        # for the ones that hold the object raising it.
        self.with_events = []
        self.instances = weakref.WeakSet()
        self.implements = []
        self.error = None           # VBASyntaxError when the module does not compile
        self.predeclared = False    # Attribute VB_PredeclaredId = True
        self.form = None            # a UserForm's design, which the host reads
        self.form_names = frozenset()   # the names its code may use unqualified: its controls
        self.default = None         # the instance the module's name stands for, once made

    def __repr__(self):
        return "<Module %s>" % self.name

    def reset(self):
        """The module's variables as they start: what End and an edit do."""
        self.v[:] = [None] * len(self.var_list)
        for slot, factory in self.var_init:
            self.v[slot] = factory()
        for group in self.procs.values():
            for p in group.values():
                p.statics = None


class Project:
    """A book's modules, compiled together: what runs a macro, a function
    for a cell, or a line typed into the Immediate window."""

    def __init__(self, host=None):
        self.host = host if host is not None else Host()
        self.modules = {}           # lname -> ModuleRT
        self.order = []
        self.event_holders = []
        self.errors = []            # the modules' compile errors, in order
        self.generation = 0         # one more at every End: see VBObject.__del__
        self.err = ErrObject()
        self.debug = DebugObject(self)
        self.files = {}
        self.output = []            # what Debug.Print wrote, not yet collected
        self.debug_line = ""
        self.depth = 0
        self.immediate_scope = None
        self.immediate_frame = None
        self.rnd_last = 0.5
        self.rnd = random.Random()
        self.compiled = False
        self.settings = {}          # SaveSetting's registry, for this session
        self.text_compare_now = False

    # -- modules --
    def add_module(self, name, kind, text):
        m = ModuleRT(self, name, kind, text)
        self.modules[m.lname] = m
        self.order.append(m)
        self.compiled = False
        return m

    def module(self, name):
        return self.modules.get(name.lower())

    def compile(self):
        """Reads and compiles every module.  One that will not compile
        keeps its error in .error, and `errors` has them all in order:
        the rest of the project runs, and calling into that module
        raises the error -- Excel's Compile On Demand, which lets a book
        with one broken module still run the macros in the others."""
        self.errors = []
        for m in self.order:
            m.error = None
            try:
                m.ast = parse_module(m.text, m.name)
            except VBASyntaxError as e:
                self.broken(m, e)
                m.ast = salvage_module(m.text)
            self.guarded(m, self.declare, m)
        for m in self.order:
            self.guarded(m, self.declare_consts, m)
        for m in self.order:
            m.with_events = []
            self.guarded(m, self.declare_vars, m)
        self.event_holders = [m for m in self.order if m.with_events]
        for m in self.order:
            if m.error is not None:
                continue
            for group in m.procs.values():
                for p in group.values():
                    if m.error is None:
                        self.guarded(m, self.compile_proc, p)
        self.compiled = True

    def broken(self, m, e):
        if m.error is None:
            m.error = e
            self.errors.append(e)

    def guarded(self, m, fn, arg):
        try:
            fn(arg)
        except VBASyntaxError as e:
            self.broken(m, e)

    def declare(self, m):
        a = m.ast
        m.implements = a.implements
        m.text_compare = a.options["compare"] == "text"
        m.base = a.options["base"]
        m.explicit = a.options["explicit"]
        m.private_module = a.options["private"]
        for t in a.types:
            fields = {}
            display = {}
            for fname, ftype, fdims in t.fields:
                fields[fname.lower()] = (ftype, fdims)
                display[fname.lower()] = fname
            td = TypeDef(self, t.name, fields)
            td.display = display
            m.types[t.name.lower()] = td
        for e in a.enums:
            members = {}
            nxt = 0
            for mname, expr in e.members:
                if expr is not None:
                    nxt = to_long(const_eval(expr, lambda n, m=m, members=members:
                                             members.get(n, self.const_lookup(m, n))), "longlong")
                members[mname.lower()] = nxt
                nxt += 1
            m.enums[e.name.lower()] = members
            m.enum_members.update(members)
        for d in a.declares:
            m.declares[d.name.lower()] = d
        for node in a.procs:
            group = m.procs.setdefault(node.name.lower(), {})
            kind = node.kind
            if kind in group:
                raise VBASyntaxError("Ambiguous name detected: %s" % node.name, node.line, m.name)
            if kind in ("sub", "function") and group:
                raise VBASyntaxError("Ambiguous name detected: %s" % node.name, node.line, m.name)
            group[kind] = Proc(m, node)
        m.predeclared = a.attributes.get("vb_predeclaredid") is True
        for key, value in a.attributes.items():
            if key.endswith(".vb_usermemid"):
                if value == 0:
                    m.default_member = key.split(".")[0]
                elif value == -4:
                    m.enum_member = key.split(".")[0]

    def const_lookup(self, m, name):
        """A constant's value while constants are worked out, or MISSING."""
        if name in m.consts:
            return m.consts[name]
        if name in m.enum_members:
            return m.enum_members[name]
        for other in self.order:
            if other is m:
                continue
            if name in other.consts and not other.const_private.get(name):
                return other.consts[name]
            if name in other.enum_members:
                return other.enum_members[name]
        v = CONSTANTS.get(name, MISSING)
        if v is MISSING:
            v = self.host.constant(name)
        return v

    def declare_consts(self, m):
        pending = []
        for st in m.ast.decls:
            if st.kind == "const":
                for name, typ, expr in st.decls:
                    pending.append((name, typ, expr, st.private, st.line))
        # Constants may name each other in any order; go round until
        # nothing more comes out.
        for _ in range(len(pending) + 1):
            left = []
            for name, typ, expr, private, line in pending:
                try:
                    v = const_eval(expr, lambda n: self.const_lookup(m, n))
                except VBASyntaxError:
                    left.append((name, typ, expr, private, line))
                    continue
                m.consts[name.lower()] = coerce(v, typ) if typ else v
                m.const_private[name.lower()] = private
            if not left or len(left) == len(pending):
                pending = left
                break
            pending = left
        if pending:
            raise VBASyntaxError("Constant expression required", pending[0][4], m.name)

    def declare_vars(self, m):
        for st in m.ast.decls:
            if st.kind != "dim":
                continue
            for d in st.decls:
                slot = len(m.var_list)
                m.var_list.append((d.name.lower(), d.typ, d))
                m.vars[d.name.lower()] = (slot, d.typ, d.dims is not None or d.dynamic, st.private)
                m.var_init.append((slot, self.factory_for(m, d.typ, d.dims, d.dynamic, d.new, d.line)))
                if d.with_events:
                    m.with_events.append((slot, d.typ.lower().rpartition(".")[2], d.name.lower()))
        m.v = [None] * len(m.var_list)
        if m.kind != "class":
            for slot, factory in m.var_init:
                m.v[slot] = factory()

    # -- values of declared types --
    def find_type(self, m, lname):
        if lname in m.types:
            return m.types[lname]
        for other in self.order:
            if lname in other.types:
                return other.types[lname]
        return None

    def class_module(self, lname):
        m = self.modules.get(lname)
        return m if m is not None and m.kind == "class" else None

    def new_value(self, typ, dims=None):
        """A fresh value of a declared type, for a Type's field."""
        if dims:
            bounds = [(to_long(const_eval(lb, lambda n: self.const_lookup(self.order[0], n)))
                       if lb is not None else 0,
                       to_long(const_eval(ub, lambda n: self.const_lookup(self.order[0], n))))
                      for lb, ub in dims]
            return self.new_array(bounds, typ)
        td = self.find_type(self.order[0], typ) if self.order else None
        if td is not None:
            return UDTValue(td)
        return default_value(typ)

    def new_array(self, bounds, typ, fixed=False, m=None):
        td = self.find_type(m or (self.order[0] if self.order else None), typ) if typ else None
        if td is not None:
            return VBArray(bounds, typ, fixed, init=lambda: UDTValue(td))
        return VBArray(bounds, typ if typ in NUMERIC_TYPES or typ in ("string", "boolean", "date", "variant")
                       or (typ or "").startswith("string*") else "object", fixed)

    def factory_for(self, m, typ, dims, dynamic, new, line):
        """What makes a variable's first value when its scope begins."""
        if dynamic:
            return lambda: VBArray([], typ)
        if dims is not None:
            def fixed_array():
                bounds = []
                for lb, ub in dims:
                    lo = to_long(const_eval(lb, lambda n: self.const_lookup(m, n))) if lb is not None else m.base
                    hi = to_long(const_eval(ub, lambda n: self.const_lookup(m, n)))
                    bounds.append((lo, hi))
                return self.new_array(bounds, typ, True, m)
            fixed_array()           # a bound that is not constant is an error now
            return fixed_array
        td = self.find_type(m, typ)
        if td is not None:
            return lambda: UDTValue(td)
        if new:
            return lambda: self.create_new(typ, line)
        v = default_value(typ)
        return lambda: v

    def create_new(self, typename, line=0):
        cls = self.class_module(typename.lower())
        if cls is not None:
            obj = VBObject(cls)
            if cls.form is not None:
                obj.ext = self.host.form_instance(obj)
            init = cls.procs.get("userform_initialize" if cls.form is not None else "class_initialize",
                                 {}).get("sub")
            if init is not None:
                self.invoke(init, [], None, obj)
            return obj
        lname = typename.lower()
        if lname in ("collection", "vba.collection"):
            return Collection()
        # What a reference to the Scripting Runtime or to VBScript's
        # regular expressions gives New.
        short = lname.rsplit(".", 1)[-1]
        if short == "dictionary":
            return Dictionary()
        if short == "filesystemobject":
            return FileSystemObject()
        if short == "regexp":
            return RegExp()
        make = LIBRARY_CLASSES.get(lname) or LIBRARY_CLASSES.get(short)
        if make is not None:
            return make()
        obj = self.host.new_object(lname)
        if obj is None:
            raise VBAError(429, "User-defined type not defined: %s" % typename)
        return obj

    def is_object_type(self, typ):
        if typ in (None, "variant", "string", "boolean", "date") or typ in NUMERIC_TYPES:
            return False
        if typ.startswith("string*"):
            return False
        for m in self.order:
            if typ in m.types:
                return False
        return True

    # -- output --
    def write_debug(self, text):
        self.output.append(text)
        self.host.debug_output(text)

    def take_output(self):
        text = "".join(self.output)
        del self.output[:]
        return text

    # -- running --
    def reset(self):
        """What End does: every module's variables begin again, and the
        open files close."""
        self.generation += 1
        for m in self.order:
            m.reset()
        for f in list(self.files.values()):
            try:
                f.close()
            except Exception:
                pass
        self.files.clear()

    def find_proc(self, name, kinds=("sub", "function")):
        """A procedure by its name, or Module.Name."""
        lname = name.lower()
        if "!" in lname:
            lname = lname.split("!", 1)[1]
        if "." in lname:
            mod, pname = lname.rsplit(".", 1)
            m = self.modules.get(mod.strip("'"))
            if m is None:
                return None
            group = m.procs.get(pname, {})
            for k in kinds:
                if k in group:
                    return group[k]
            return None
        for m in self.order:
            if m.kind != "standard":
                continue
            group = m.procs.get(lname, {})
            for k in kinds:
                if k in group:
                    return group[k]
        for m in self.order:
            group = m.procs.get(lname, {})
            for k in kinds:
                if k in group:
                    return group[k]
        return None

    def default_instance(self, m):
        """What a UserForm's name, or a class's with VB_PredeclaredId,
        stands for: an instance made the first time it is used, and made
        again after Unload."""
        if m.default is None:
            m.default = self.create_new(m.name)
        return m.default

    def me_for(self, proc):
        if proc.module.kind == "document":
            return self.host.document(proc.module.name)
        return None

    def raise_event(self, obj, event, args, caller):
        """RaiseEvent: every variable declared WithEvents that holds `obj`
        now has its handler, variable_Event, run with the arguments --
        ByRef ones given back, as a Cancel is."""
        cls = obj.cls.lname
        for m in self.event_holders:
            for slot, typ, var in m.with_events:
                if typ != cls:
                    continue
                group = m.procs.get(var + "_" + event)
                handler = group.get("sub") if group else None
                if handler is None:
                    continue
                if m.kind == "class":
                    holders = [(h, h.v) for h in list(m.instances)]
                else:
                    holders = [(self.me_for(handler), m.v)]
                for me, store in holders:
                    if store[slot] is obj:
                        self.invoke(handler, args, caller, me)

    def run(self, name, args=()):
        """Runs a macro -- or any procedure -- by name, with values for
        its arguments; what a Function returns."""
        if not self.compiled:
            self.compile()
        proc = self.find_proc(name)
        if proc is None:
            raise VBAError(35, "Cannot run the macro '%s'. The macro may not be available in this "
                               "workbook or all macros may be disabled." % name)
        return self.call(proc, list(args))

    def call(self, proc, values, me=None):
        global current
        current = self
        _run_begin()
        try:
            return self.invoke(proc, [v if isinstance(v, ValueArg) else ValueArg(v) for v in values], None,
                               me if me is not None else self.me_for(proc))
        except EndSignal:
            self.reset()
            return EMPTY
        finally:
            _run_end()
            self.flush_debug()

    def flush_debug(self):
        if self.debug_line:
            self.write_debug("\n")
            self.debug_line = ""

    def invoke(self, proc, args, caller, me, let_value=None, has_let=False):
        """Runs a procedure with compiled (or value) arguments."""
        if proc.module.error is not None:
            raise proc.module.error
        if not proc.compiled:
            self.compile_proc(proc)
        fr = Frame(proc, me)
        v = fr.v
        for slot, factory in proc.init:
            v[slot] = factory()
        if proc.statics is not None:
            pass
        params = proc.params
        npos = 0
        named = None
        for a in args:
            if a.name is not None:
                if named is None:
                    named = {}
                named[a.name] = a
            else:
                npos += 1
        np = len(params) - (1 if has_let else 0)
        if npos > np and not (np and params[np - 1].paramarray):
            raise VBAError(450)
        pi = 0
        for idx in range(np):
            p = params[idx]
            if p.paramarray:
                rest = [a.getv(caller) for a in args[idx:] if a.name is None]
                v[p.slot] = VBArray([(0, len(rest) - 1)], data=rest)
                break
            a = args[idx] if idx < len(args) and args[idx].name is None else None
            if a is None and named is not None:
                a = named.pop(p.lname, None)
            if a is None or a.missing:
                if not p.optional:
                    raise VBAError(449)
                val = p.default
                v[p.slot] = val if p.byval else Cell(val)
                continue
            if p.byval or a.paren:
                val = a.getv(caller)
                if p.typ != "variant":
                    val = coerce(val, p.typ)
                elif isinstance(val, (VBArray, UDTValue)):
                    val = val.copy()
                if p.byval:
                    v[p.slot] = val
                else:
                    v[p.slot] = Cell(val)
            else:
                getref = a.getref
                ref = getref(caller) if getref is not None else None
                if ref is None:
                    val = a.getv(caller)
                    if p.typ != "variant" and not p.is_array:
                        val = coerce(val, p.typ)
                    ref = Cell(val)
                v[p.slot] = ref
            pi += 1
        if named:
            raise VBAError(448)
        if has_let:
            p = params[-1]
            val = let_value
            if p.typ != "variant" and not is_object(val):
                val = coerce(val, p.typ)
            v[p.slot] = val if p.byval else Cell(val)
        if self.depth > 900:
            raise VBAError(28)
        self.depth += 1
        try:
            proc.body(fr)
        except ExitProc:
            pass
        except GotoSignal as g:
            raise VBAError(1004, "Label not defined: %s" % g.label)
        except ResumeSignal:
            raise VBAError(20)
        except ReturnSignal:
            raise VBAError(3)
        finally:
            self.depth -= 1
        if proc.is_function:
            return v[0]
        return EMPTY

    # -- members of objects --
    def interface_member(self, cls, lname):
        """A member the class has as Interface_Member, for an interface it
        Implements: what a call through a variable of the interface's
        type reaches.  The interpreter does not keep a variable's type
        with its object, so any such member serves."""
        for iface in cls.implements:
            group = cls.procs.get(iface + "_" + lname)
            if group:
                return group
        return None

    def member_get(self, obj, lname, args, caller):
        """obj.Name or obj.Name(args), as a value."""
        if obj is None:
            raise VBAError(91)
        if isinstance(obj, VBObject):
            cls = obj.cls
            group = cls.procs.get(lname)
            if group:
                proc = group.get("get") or group.get("function") or group.get("sub")
                if proc is not None:
                    if proc.private and caller is not None and caller.proc.module is not cls:
                        raise VBAError(438)
                    return self.invoke(proc, args, caller, obj)
            var = cls.vars.get(lname)
            if var is not None and not (var[3] and (caller is None or caller.proc.module is not cls)):
                value = obj.v[var[0]]
                return self.index_value(value, args, caller) if args else value
            group = self.interface_member(cls, lname)
            if group:
                proc = group.get("get") or group.get("function") or group.get("sub")
                if proc is not None:
                    return self.invoke(proc, args, caller, obj)
            if obj.ext is not None:
                return self.member_get(obj.ext, lname, args, caller)
            raise VBAError(438)
        if isinstance(obj, UDTValue):
            if lname not in obj.fields:
                raise VBAError(438)
            value = obj.fields[lname]
            return self.index_value(value, args, caller) if args else value
        m = getattr(obj, "_vba_module", None)
        if m is not None:
            # A sheet's or the book's own module: its procedures first.
            group = m.procs.get(lname)
            if group:
                proc = group.get("get") or group.get("function") or group.get("sub")
                if proc is not None:
                    return self.invoke(proc, args, caller, obj)
            var = m.vars.get(lname)
            if var is not None and not var[3]:
                value = m.v[var[0]]
                return self.index_value(value, args, caller) if args else value
        return self.python_get(obj, lname, args, caller)

    def python_get(self, obj, lname, args, caller):
        attr = python_member(obj, lname)
        if attr is None:
            getter = getattr(obj, "_vba_get", None)
            if getter is not None:
                return getter(lname, *[a.getv(caller) for a in args])
            raise VBAError(438, "Object doesn't support this property or method (%s)" % lname)
        if _is_property(obj, attr):
            value = getattr(obj, attr)
            return self.index_value(value, args, caller) if args else value
        fn = getattr(obj, attr)
        if not callable(fn):
            return self.index_value(fn, args, caller) if args else fn
        values, named = self.arg_values(args, caller)
        return call_python(fn, values, named)

    def arg_values(self, args, caller):
        values = []
        named = {}
        for a in args:
            if a.name is not None:
                named[a.name] = a.getv(caller)
            elif a.missing:
                values.append(MISSING)
            else:
                values.append(a.getv(caller))
        return values, named

    def index_value(self, value, args, caller):
        """value(args): an array's element, or an object's default member."""
        subs = [a.getv(caller) for a in args]
        if isinstance(value, VBArray):
            return value.get(subs)
        if is_object(value) and value is not None:
            return default_member(value, *subs)
        if value is None:
            raise VBAError(91)
        raise VBAError(13)

    def member_let(self, obj, lname, args, value, is_set, caller):
        """obj.Name = value, or obj.Name(args) = value."""
        if obj is None:
            raise VBAError(91)
        if isinstance(obj, VBObject):
            cls = obj.cls
            group = cls.procs.get(lname)
            if group:
                proc = group.get("set" if is_set else "let") or group.get("let") or group.get("set")
                if proc is not None:
                    self.invoke(proc, args, caller, obj, value, True)
                    return
            var = cls.vars.get(lname)
            if var is not None:
                slot, typ = var[0], var[1]
                if args:
                    self.index_let(obj.v[slot], args, value, caller)
                else:
                    obj.v[slot] = value if is_set else coerce(plain_value(value) if not self.is_object_type(typ)
                                                              else value, typ)
                return
            group = self.interface_member(cls, lname)
            if group:
                proc = group.get("set" if is_set else "let") or group.get("let") or group.get("set")
                if proc is not None:
                    self.invoke(proc, args, caller, obj, value, True)
                    return
            if obj.ext is not None:
                self.member_let(obj.ext, lname, args, value, is_set, caller)
                return
            raise VBAError(438)
        if isinstance(obj, UDTValue):
            if lname not in obj.fields:
                raise VBAError(438)
            if args:
                self.index_let(obj.fields[lname], args, value, caller)
            else:
                typ = obj.typedef.fields[lname][0]
                obj.fields[lname] = value if is_set else coerce(value, typ)
            return
        m = getattr(obj, "_vba_module", None)
        if m is not None:
            group = m.procs.get(lname)
            if group:
                proc = group.get("set" if is_set else "let") or group.get("let")
                if proc is not None:
                    self.invoke(proc, args, caller, obj, value, True)
                    return
            var = m.vars.get(lname)
            if var is not None:
                m.v[var[0]] = value if is_set else coerce(value, var[1])
                return
        attr = python_member(obj, lname)
        if attr is None:
            setter = getattr(obj, "_vba_let", None)
            if setter is not None:
                setter(lname, value, *[a.getv(caller) for a in args])
                return
            raise VBAError(438, "Object doesn't support this property or method (%s)" % lname)
        if args:
            letter = python_member(obj, "let_" + lname)
            if letter is not None:
                values, named = self.arg_values(args, caller)
                call_python(getattr(obj, letter), [value] + values, named)
                return
            target = self.python_get(obj, lname, args, caller)
            if isinstance(target, VBArray):
                raise VBAError(438)
            default_let(target, value)
            return
        if _is_property(obj, attr):
            if getattr(type(obj), attr).fset is None:
                raise VBAError(438, "The %s property cannot be set" % attr)
            setattr(obj, attr, value)
            return
        if attr in getattr(obj, "__dict__", ()):
            setattr(obj, attr, value)
            return
        letter = python_member(obj, "let_" + lname)
        if letter is not None:
            getattr(obj, letter)(value)
            return
        # A method that returns an object: its default member is set.
        default_let(self.python_get(obj, lname, [], caller), value)

    def index_let(self, target, args, value, caller):
        subs = [a.getv(caller) for a in args]
        if isinstance(target, VBArray):
            target.set(subs, value)
            return
        if is_object(target) and target is not None:
            default_let(target, value, *subs)
            return
        raise VBAError(13)

    # -- the Immediate window --
    def immediate(self, line):
        """Runs a line typed into the Immediate window: ?expr prints its
        value, anything else is statements.  Variables made here last."""
        global current
        current = self
        if not self.compiled:
            self.compile()
        text = line.strip()
        if text.startswith("?"):
            text = "Debug.Print " + text[1:]
        elif text.lower().startswith("print "):
            text = "Debug.Print " + text[6:]
        if self.immediate_scope is None:
            host_module = ModuleRT(self, "Immediate", "standard", "")
            host_module.v = []
            self.immediate_module = host_module
            node = ProcDef("sub", "Immediate", [], "variant", [], False, False, 1, 1)
            self.immediate_proc = Proc(host_module, node)
            self.immediate_scope = Scope(host_module, self.immediate_proc)
            self.immediate_frame = None
        tokens = []
        for start, toks in Lexer(text, "Immediate").lines():
            tokens.extend(toks)
            tokens.append(Token("nl", None, start))
        tokens.append(Token("eof", None, 1))
        parser = Parser(tokens, "Immediate")
        stmts = []
        while True:
            parser.skip_newlines()
            if parser.peek().kind == "eof":
                break
            stmts.extend(parser.parse_statement())
        sc = self.immediate_scope
        comp = Compiler(self, sc)
        # Dim and Const declare for the lines typed after them too; a name
        # declared again is the variable it already was.
        for st in comp.walk(stmts):
            if st.kind == "dim":
                for d in st.decls:
                    if d.name.lower() not in sc.locals:
                        comp.add_local(d, False)
            elif st.kind == "const":
                for name, typ, expr in st.decls:
                    v = const_eval(expr, comp.const_value)
                    sc.locals[name.lower()] = ("const", coerce(v, typ) if typ else v)
        body = comp.block(stmts)
        proc = self.immediate_proc
        proc.nslots = sc.nslots
        # The frame lives on, growing as lines add variables.
        fr = self.immediate_frame
        if fr is None:
            fr = self.immediate_frame = Frame(proc, None)
            made = 0
        else:
            made = len(fr.v)
            fr.v.extend([EMPTY] * (sc.nslots - made))
        # The variables this line made start as their types say.
        for slot, factory in sc.init:
            if slot >= made:
                fr.v[slot] = factory()
        _run_begin()
        try:
            body(fr)
        except ExitProc:
            pass
        except EndSignal:
            self.reset()
        finally:
            _run_end()
            self.flush_debug()

    # -- procedures, compiled --
    def compile_proc(self, proc):
        if proc.compiled:
            return
        m = proc.module
        node = proc.node
        sc = Scope(m, proc)
        comp = Compiler(self, sc)
        # Slot 0 is a Function's value; the parameters follow.
        if proc.is_function:
            rt = node.ret_type
            if node.ret_array:
                sc.init.append((0, lambda rt=rt: VBArray([], rt)))
            else:
                sc.init.append((0, self.factory_for(m, rt, None, False, False, node.line)))
        params = []
        for p in node.params:
            prt = ParamRT()
            prt.name = p.name
            prt.lname = p.name.lower()
            prt.slot = sc.nslots
            sc.nslots += 1
            prt.byval = p.byval or p.paramarray
            prt.optional = p.optional or p.paramarray
            prt.paramarray = p.paramarray
            prt.typ = p.typ if not p.is_array else "variant"
            prt.is_array = p.is_array
            if p.default is not None:
                prt.default = coerce(const_eval(p.default, lambda n: self.const_lookup(m, n)), p.typ) \
                    if p.typ != "variant" else const_eval(p.default, lambda n: self.const_lookup(m, n))
            elif p.optional and p.typ == "variant":
                prt.default = MISSING
            else:
                prt.default = default_value(p.typ)
            local = Local(prt.slot, prt.typ, prt, p.is_array)
            local.decl_object = self.is_object_type(p.typ)
            sc.locals[prt.lname] = local
            params.append(prt)
        proc.params = params
        comp.declare_locals(node.body, node.static)
        proc.body = comp.block(node.body, top=True)
        proc.local_names = [(name, b) for name, b in sc.locals.items() if isinstance(b, Local)]
        proc.nslots = sc.nslots
        proc.init = sc.init
        proc.compiled = True


class Host:
    """What the language asks of the program it runs in.  This one has
    no book: MsgBox prints, and there are no objects of its own."""

    object_types = ()

    def form_instance(self, obj):
        """What a UserForm's instance has beside its module: its controls.
        None where there are no forms."""
        return None

    def unload(self, obj):
        raise VBAError(361, "Can't load or unload this object")

    def constant(self, lname):
        return MISSING

    def has_global(self, lname):
        return False

    def global_get(self, lname, args):
        raise VBAError(35, "Sub or Function not defined: %s" % lname)

    def global_let(self, lname, value, args):
        raise VBAError(438)

    def document(self, name):
        return None

    def document_has(self, module, lname):
        return False

    def new_object(self, lname):
        return None

    def create_object(self, progid):
        return None

    def get_object(self, path, progid):
        raise VBAError(429)

    def msgbox(self, prompt, buttons, title):
        print(prompt)
        return 1

    def inputbox(self, prompt, title, default):
        return default

    def debug_output(self, text):
        pass

    def typename(self, obj):
        return type(obj).__name__

    def evaluate(self, text):
        raise VBAError(1004)

    def do_events(self):
        pass

    def format_number(self, code, n):
        """A number in a format code, or None to have the language's own
        formatter do it."""
        return None

    def declare_call(self, node, values):
        """A procedure a Declare statement names in a library: Windows
        DLLs are not here, so only the host's stand-ins answer."""
        raise VBAError(453, "Specified DLL function not found: %s in %s" % (node.name, node.lib))

    app = None


RESUME_NEXT = "\x00next"
RESUME_RETRY = "\x00retry"


def _on_error(fr, ex, line):
    """An error reached a statement of a procedure: go on past it,
    run the procedure's handler, or let it go to the caller."""
    if isinstance(ex, VBASyntaxError):
        raise ex                    # a module that did not compile: no handler hears of it
    e = error_from_python(ex)
    if e.where is None:
        e.where = (fr.proc.module.name, fr.proc.name, line)
    mode = fr.on_error
    if mode is None or fr.in_handler:
        raise e
    project = fr.proc.module.project
    project.err.set(e)
    fr.err_line = line
    if mode == "next":
        return RESUME_NEXT
    fr.in_handler = True
    try:
        fr.proc.body(fr, fr.proc.labels[mode])
    except ResumeSignal as r:
        fr.in_handler = False
        project.err.Clear()
        if r.kind == "next":
            return RESUME_NEXT
        if r.kind == "retry":
            return RESUME_RETRY
        return r.label
    raise ExitProc()


def make_block(stmts, lines, labels, top=False):
    """Statements run in order, with what GoTo, an error and a trace
    ask of the order."""
    n = len(stmts)

    def run(fr, start=0):
        global _poll_count
        if _poll is not None:
            _poll_count += 1
            if not _poll_count & 31:
                _check_poll()
        i = start
        while i < n:
            try:
                if _trace is not None:
                    _trace(fr, lines[i])
                stmts[i](fr)
                i += 1
            except GotoSignal as g:
                j = labels.get(g.label)
                if j is None:
                    raise
                if j <= i and _poll is not None:
                    _check_poll()
                i = j
            except Exception as ex:
                r = _on_error(fr, ex, lines[i])
                if r is RESUME_NEXT:
                    i += 1
                elif r is RESUME_RETRY:
                    pass
                else:
                    j = labels.get(r)
                    if j is None:
                        raise GotoSignal(r)
                    i = j
    return run


def _copy_preserved(old, new):
    """ReDim Preserve: the elements both arrays have keep their values."""
    if not old.dims:
        return
    if len(old.dims) != len(new.dims):
        raise VBAError(9)
    for d in range(len(old.dims) - 1):
        if old.dims[d] != new.dims[d]:
            raise VBAError(9)
    # With the last subscript slowest, the common elements are a prefix.
    n = min(len(old.data), len(new.data))
    new.data[:n] = old.data[:n]


class Compiler:
    """Turns a procedure's statements into closures, deciding what each
    name means as it goes."""

    def __init__(self, project, scope):
        self.project = project
        self.sc = scope
        self.module = scope.module
        self.text = scope.module.text_compare
        self.line = 0

    def error(self, message, line=0):
        raise VBASyntaxError(message, line or self.line, self.module.name)

    # -- declarations --
    def declare_locals(self, body, static_all):
        """Every Dim, Static and Const of the procedure, wherever it is:
        Visual Basic makes them all as the procedure begins."""
        for st in self.walk(body):
            if st.kind == "dim":
                static = static_all or st.scope == "static"
                for d in st.decls:
                    self.add_local(d, static)
            elif st.kind == "const":
                for name, typ, expr in st.decls:
                    v = const_eval(expr, self.const_value)
                    self.sc.locals[name.lower()] = ("const", coerce(v, typ) if typ else v)
            elif st.kind == "redim":
                for target, dims, typ in st.items:
                    if isinstance(target, NameE) and self.resolve(target.name.lower()) is None:
                        d = VarDecl(target.name, typ or _SUFFIX_TYPES.get(target.suffix, "variant"),
                                    None, True, False, st.line)
                        self.add_local(d, static_all)

    def walk(self, body):
        for st in body:
            yield st
            k = st.kind
            if k == "if":
                for _, b in st.branches:
                    yield from self.walk(b)
                yield from self.walk(st.orelse)
            elif k == "select":
                for _, b in st.cases:
                    yield from self.walk(b)
                yield from self.walk(st.orelse)
            elif k in ("for", "foreach", "do", "while", "with"):
                yield from self.walk(st.body)

    def const_value(self, lname):
        b = self.sc.locals.get(lname)
        if isinstance(b, tuple) and b[0] == "const":
            return b[1]
        return self.project.const_lookup(self.module, lname)

    def add_local(self, d, static):
        lname = d.name.lower()
        if lname in self.sc.locals:
            existing = self.sc.locals[lname]
            if isinstance(existing, Local) and existing.param is None:
                self.error("Duplicate declaration in current scope: %s" % d.name, d.line)
            if isinstance(existing, Local):
                self.error("Duplicate declaration in current scope: %s" % d.name, d.line)
        factory = self.project.factory_for(self.module, d.typ, d.dims, d.dynamic, d.new, d.line)
        is_array = d.dims is not None or d.dynamic
        if static:
            proc = self.sc.proc
            if proc.statics is None:
                proc.statics = []
            slot = len(proc.statics)
            proc.statics.append(factory())
            local = Local(slot, d.typ, None, is_array, True)
        else:
            slot = self.sc.nslots
            self.sc.nslots += 1
            self.sc.init.append((slot, factory))
            local = Local(slot, d.typ, None, is_array)
        local.decl_object = self.project.is_object_type(d.typ) and not is_array and \
            self.project.find_type(self.module, d.typ) is None
        self.sc.locals[lname] = local
        return local

    def implicit(self, name, suffix, line):
        if self.module.explicit and self.sc.proc is not None and self.sc.proc.name != "Immediate":
            self.error("Variable not defined: %s" % name, line)
        d = VarDecl(name, _SUFFIX_TYPES.get(suffix, "variant"), None, False, False, line)
        return self.add_local(d, False)

    # -- names --
    def resolve(self, lname):
        """What a name means here, as a tuple, or None."""
        sc = self.sc
        b = sc.locals.get(lname)
        if b is not None:
            return b if isinstance(b, tuple) else ("local", b)
        proc = sc.proc
        if proc is not None and proc.is_function and lname == proc.lname:
            return ("retval",)
        m = self.module
        found = self.resolve_in_module(m, lname, True)
        if found is not None:
            return found
        if lname in m.form_names:
            return ("docmember", lname)       # a UserForm's control, a member of Me
        project = self.project
        for other in project.order:
            if other is m or other.kind != "standard":
                continue
            found = self.resolve_in_module(other, lname, False)
            if found is not None:
                return found
        other = project.modules.get(lname)
        if other is not None:
            if other.kind == "document":
                return ("doc", other)
            if other.kind == "standard":
                return ("modref", other)
            if other.kind == "class":
                if other.form is not None or other.predeclared:
                    return ("predeclared", other)
                return ("classname", other)
        if m.kind == "document" and project.host.document_has(m, lname):
            return ("docmember", lname)
        if lname == "err":
            return ("const", project.err)
        if lname == "debug":
            return ("const", project.debug)
        if lname == "vba":
            return ("vba",)
        fn = LIBRARY.get(lname)
        if fn is not None:
            return ("lib", fn)
        if lname in CONSTANTS:
            return ("const", CONSTANTS[lname])
        v = project.host.constant(lname)
        if v is not MISSING:
            return ("const", v)
        if project.host.has_global(lname):
            return ("host", lname)
        return None

    def resolve_in_module(self, m, lname, inside):
        var = m.vars.get(lname)
        if var is not None and (inside or not var[3]) and m.kind != "class":
            return ("modvar", m, var[0], var[1], var[2])
        if var is not None and inside and m.kind == "class":
            return ("memvar", var[0], var[1], var[2])
        if lname in m.consts and (inside or not m.const_private.get(lname)):
            return ("const", m.consts[lname])
        if lname in m.enum_members:
            return ("const", m.enum_members[lname])
        group = m.procs.get(lname)
        if group:
            p = next(iter(group.values()))
            if inside or (not p.private and not m.private_module) or m.kind == "standard" and not p.private:
                return ("proc", m, group)
        if lname in m.enums:
            return ("enum", m.enums[lname])
        d = m.declares.get(lname)
        if d is not None and (inside or not d.private):
            return ("declare", d)
        return None

    def me_getter(self, target_module):
        """The object a procedure of `target_module` runs for, from here."""
        if target_module.kind == "class":
            if target_module is self.module:
                return lambda fr: fr.me
            return None
        if target_module.kind == "document":
            if target_module is self.module:
                return lambda fr: fr.me
            host = self.project.host
            name = target_module.name
            return lambda fr: host.document(name)
        return lambda fr: None

    # -- expressions --
    def expr(self, e):
        t = type(e)
        project = self.project
        if t is Lit:
            v = e.value
            return lambda fr: v
        if t is NameE:
            return self.name_get(e)
        if t is ParenE:
            return self.expr(e.expr)
        if t is BinE:
            af = self.expr(e.a)
            bf = self.expr(e.b)
            op = BINARY_OPS[e.op]
            if e.op in _TEXT_OPS and self.text:
                return lambda fr: op(af(fr), bf(fr), True)
            if e.op == "&":
                def concat(fr):
                    a = af(fr)
                    b = bf(fr)
                    if type(a) is str and type(b) is str:
                        return a + b
                    return op_concat(a, b)
                return concat
            if e.op == "is":
                return lambda fr: af(fr) is bf(fr) if is_object(af(fr)) else op_is(af(fr), bf(fr))
            return lambda fr: op(af(fr), bf(fr))
        if t is UnE:
            xf = self.expr(e.x)
            if e.op == "-":
                return lambda fr: op_neg(xf(fr))
            return lambda fr: op_not(xf(fr))
        if t is MeE:
            return lambda fr: fr.me
        if t is WithE:
            if self.sc.with_depth == 0:
                self.error("Invalid or unqualified reference")
            return lambda fr: fr.w[-1]
        if t is MemberE:
            u = self.unqualified(e)
            if u is not e:
                return self.expr(u)
            return self.member_get(e, [])
        if t is CallE:
            return self.call_get(e)
        if t is BangE:
            of = self.expr(e.obj)
            key = e.name
            return lambda fr: default_member(of(fr), key)
        if t is NewE:
            name = e.typename
            return lambda fr: project.create_new(name)
        if t is TypeOfE:
            of = self.expr(e.expr)
            typename = e.typename
            return lambda fr: type_of(project, of(fr), typename)
        if t is EvalE:
            text = e.text
            return lambda fr: project.host.evaluate(text)
        self.error("Expected: expression")

    def name_get(self, e):
        lname = e.name.lower()
        b = self.resolve(lname)
        project = self.project
        if b is None:
            b = ("local", self.implicit(e.name, e.suffix, e.line))
        kind = b[0]
        if kind == "local":
            local = b[1]
            s = local.slot
            if local.static:
                store = self.sc.proc.statics
                return lambda fr: store[s]
            if local.param is not None and not local.param.byval:
                return lambda fr: fr.v[s].get()
            return lambda fr: fr.v[s]
        if kind == "retval":
            return lambda fr: fr.v[0]
        if kind == "modvar":
            mv = b[1].v
            s = b[2]
            return lambda fr: mv[s]
        if kind == "memvar":
            s = b[1]
            return lambda fr: fr.me.v[s]
        if kind == "const":
            v = b[1]
            return lambda fr: v
        if kind == "proc":
            return self.proc_call(b[1], b[2], [], getter=True)
        if kind == "lib":
            fn = b[1]
            return lambda fr: fn()
        if kind == "doc":
            host = project.host
            name = b[1].name
            return lambda fr: host.document(name)
        if kind == "docmember":
            return lambda fr: project.member_get(fr.me, lname, [], fr)
        if kind == "host":
            return lambda fr: project.member_get(project.host.app, lname, [], fr)
        if kind == "declare":
            node = b[1]
            return lambda fr: project.host.declare_call(node, [])
        if kind == "predeclared":
            m = b[1]
            return lambda fr: project.default_instance(m)
        if kind in ("modref", "enum", "classname", "vba"):
            self.error("Expected: variable or procedure, not module", e.line)
        self.error("Unknown name: %s" % e.name, e.line)

    def args(self, args):
        return [self.arg(a) for a in args]

    def arg(self, a):
        if a.expr is None:
            return ArgC(a.name, True, None, None, False)
        paren = isinstance(a.expr, ParenE) or a.byval
        getv = self.expr(a.expr)
        getref = None if paren else self.ref(a.expr)
        return ArgC(a.name, False, getv, getref, paren)

    def ref(self, e):
        """Where a variable passed ByRef lives, or None for a value."""
        e = self.unqualified(e)
        if isinstance(e, NameE):
            b = self.resolve(e.name.lower())
            if b is None:
                b = ("local", self.implicit(e.name, e.suffix, e.line))
            kind = b[0]
            if kind == "local":
                local = b[1]
                s = local.slot
                if local.static:
                    store = self.sc.proc.statics
                    return lambda fr: Ref(store, s)
                if local.param is not None and not local.param.byval:
                    return lambda fr: fr.v[s]
                return lambda fr: Ref(fr.v, s)
            if kind == "retval":
                return lambda fr: Ref(fr.v, 0)
            if kind == "modvar":
                mv = b[1].v
                s = b[2]
                return lambda fr: Ref(mv, s)
            if kind == "memvar":
                s = b[1]
                return lambda fr: Ref(fr.me.v, s)
            return None
        if isinstance(e, CallE) and isinstance(e.target, NameE):
            b = self.resolve(e.target.name.lower())
            if b is not None and b[0] in ("local", "modvar", "memvar"):
                getarr = self.name_get(e.target)
                subs = [self.expr(a.expr) for a in e.args if a.expr is not None]

                def element(fr):
                    arr = getarr(fr)
                    if isinstance(arr, VBArray):
                        return Ref(arr.data, arr.index([f(fr) for f in subs]))
                    return None
                return element
        if isinstance(e, MemberE):
            if self.member_target(e) is not None:
                return None
            of = self.expr(e.obj) if not isinstance(e.obj, WithE) else (lambda fr: fr.w[-1])
            lname = e.name.lower()

            def field(fr):
                obj = of(fr)
                if isinstance(obj, UDTValue) and lname in obj.fields:
                    return Ref(obj.fields, lname)
                return None
            return field
        return None

    def proc_call(self, m, group, argcs, getter=False, statement=False):
        """A call of a procedure in the project: a Function's value, or a
        Sub run."""
        project = self.project
        proc = group.get("function") or group.get("get") or group.get("sub")
        if proc is None:
            proc = group.get("let") or group.get("set")
        me = self.me_getter(m)
        if me is None:
            self.error("Invalid use of a class's procedure without its object")
        return lambda fr: project.invoke(proc, argcs, fr, me(fr))

    def unqualified(self, e):
        """Excel.Range, Excel.xlUp, MSForms.ReturnBoolean: a type library's
        name before one of its members is the member, unless the project
        has something of that name itself."""
        while isinstance(e, MemberE) and isinstance(e.obj, NameE) and \
                e.obj.name.lower() in _LIBRARIES and self.resolve(e.obj.name.lower()) is None:
            e = NameE(e.name, "", e.obj.line)
        return e

    def member_target(self, e):
        """For obj.Name: whether obj is a module, an enum or VBA, which
        are known now; else None."""
        if isinstance(e.obj, NameE):
            b = self.resolve(e.obj.name.lower())
            if b is not None:
                if b[0] == "modref":
                    return ("module", b[1])
                if b[0] == "enum":
                    return ("enum", b[1])
                if b[0] == "vba":
                    return ("vba", None)
        return None

    def member_get(self, e, args):
        project = self.project
        lname = e.name.lower()
        known = self.member_target(e)
        argcs = self.args(args) if isinstance(args, list) and args and isinstance(args[0], ArgE) else args
        if known is not None:
            kind, what = known
            if kind == "module":
                b = self.resolve_in_module(what, lname, what is self.module)
                if b is None:
                    self.error("Method or data member not found: %s" % e.name)
                if b[0] == "proc":
                    return self.proc_call(what, b[2], argcs)
                if b[0] == "modvar":
                    mv, s = b[1].v, b[2]
                    if argcs:
                        return lambda fr: project.index_value(mv[s], argcs, fr)
                    return lambda fr: mv[s]
                if b[0] == "const":
                    v = b[1]
                    return lambda fr: v
                self.error("Method or data member not found: %s" % e.name)
            if kind == "enum":
                if lname not in what:
                    self.error("Method or data member not found: %s" % e.name)
                v = what[lname]
                return lambda fr: v
            if kind == "vba":
                fn = LIBRARY.get(lname)
                if fn is None:
                    if lname in CONSTANTS and not argcs:
                        v = CONSTANTS[lname]
                        return lambda fr: v
                    if lname in ("err", "debug"):
                        obj = project.err if lname == "err" else project.debug
                        return lambda fr: obj
                    self.error("Method or data member not found: %s" % e.name)
                return self.lib_call(fn, argcs)
        if isinstance(e.obj, WithE):
            if self.sc.with_depth == 0:
                self.error("Invalid or unqualified reference")
            return lambda fr: project.member_get(fr.w[-1], lname, argcs, fr)
        of = self.expr(e.obj)
        return lambda fr: project.member_get(of(fr), lname, argcs, fr)

    def lib_call(self, fn, argcs):
        if not argcs:
            return lambda fr: fn()
        if fn not in _TAKES_OBJECTS:
            # The library's arguments are Variants: an object given to
            # one is its default value, so IsEmpty(ActiveCell) asks of
            # the cell's value.
            argcs = [ArgC(a.name, a.missing, _valued(a.getv), None, a.paren) if not a.missing else a
                     for a in argcs]
        if all(a.name is None and not a.missing for a in argcs):
            getters = [a.getv for a in argcs]
            if len(getters) == 1:
                g0 = getters[0]
                return lambda fr: fn(g0(fr))
            if len(getters) == 2:
                g0, g1 = getters
                return lambda fr: fn(g0(fr), g1(fr))
            return lambda fr: fn(*[g(fr) for g in getters])
        project = self.project

        def call(fr):
            values, named = project.arg_values(argcs, fr)
            return call_python(fn, values, named)
        return call

    def call_get(self, e):
        """name(args) or obj.Name(args) as a value."""
        project = self.project
        target = self.unqualified(e.target)
        if target is not e.target:
            return self.call_get(CallE(target, e.args))
        argcs = self.args(e.args)
        if isinstance(target, NameE):
            lname = target.name.lower()
            b = self.resolve(lname)
            if b is None:
                name = target.name
                if lname in ("mid", "midb") or lname in LIBRARY:
                    pass

                def undefined(fr):
                    raise VBAError(35, "Sub or Function not defined: %s" % name)
                return undefined
            kind = b[0]
            if kind in ("local", "modvar", "memvar"):
                getv = self.name_get(target)
                if len(argcs) == 1 and argcs[0].name is None and not argcs[0].missing:
                    g0 = argcs[0].getv

                    def one(fr):
                        v = getv(fr)
                        if type(v) is VBArray:
                            return v.data[v.index((g0(fr),))]
                        return project.index_value(v, argcs, fr)
                    return one
                return lambda fr: project.index_value(getv(fr), argcs, fr)
            if kind == "retval":
                proc = self.sc.proc
                me = self.me_getter(proc.module)
                return lambda fr: project.invoke(proc, argcs, fr, me(fr))
            if kind == "proc":
                return self.proc_call(b[1], b[2], argcs)
            if kind == "lib":
                return self.lib_call(b[1], argcs)
            if kind == "host":
                return lambda fr: project.member_get(project.host.app, lname, argcs, fr)
            if kind == "docmember":
                return lambda fr: project.member_get(fr.me, lname, argcs, fr)
            if kind == "declare":
                node = b[1]
                return lambda fr: project.host.declare_call(node, [a.getv(fr) if not a.missing else MISSING
                                                                   for a in argcs])
            if kind == "const":
                v = b[1]
                return lambda fr: project.index_value(v, argcs, fr)
            if kind == "doc":
                getv = self.name_get(target)
                return lambda fr: project.index_value(getv(fr), argcs, fr)
            self.error("Expected: procedure or array", target.line)
        if isinstance(target, MemberE):
            return self.member_get(target, argcs)
        tf = self.expr(target)
        return lambda fr: project.index_value(tf(fr), argcs, fr)

    # -- assignment --
    def store(self, e, is_set, line):
        """What sets a target: a closure (frame, value)."""
        project = self.project
        e = self.unqualified(e)
        if isinstance(e, NameE):
            lname = e.name.lower()
            b = self.resolve(lname)
            if b is None:
                b = ("local", self.implicit(e.name, e.suffix, e.line))
            kind = b[0]
            if kind == "local":
                local = b[1]
                return self.store_slot(local, is_set)
            if kind == "retval":
                typ = self.sc.proc.ret_type
                obj_type = project.is_object_type(typ)

                def set_ret(fr, value):
                    if is_set:
                        fr.v[0] = check_object(value)
                    elif obj_type and is_object(fr.v[0]) and fr.v[0] is not None:
                        default_let(fr.v[0], plain_value(value))
                    else:
                        fr.v[0] = coerce(plain_value(value), typ)
                return set_ret
            if kind == "modvar":
                mv, s, typ = b[1].v, b[2], b[3]
                return self.store_list(lambda fr: mv, s, typ, is_set)
            if kind == "memvar":
                s, typ = b[1], b[2]
                return self.store_list(lambda fr: fr.me.v, s, typ, is_set)
            if kind == "proc":
                group = b[2]
                proc = group.get("set" if is_set else "let") or group.get("let") or group.get("set")
                if proc is None:
                    self.error("Assignment to a procedure: %s" % e.name, line)
                me = self.me_getter(b[1])
                return lambda fr, value: project.invoke(proc, [], fr, me(fr), value, True)
            if kind == "host":
                return lambda fr, value: project.member_let(project.host.app, lname, [], value, is_set, fr)
            if kind == "docmember":
                return lambda fr, value: project.member_let(fr.me, lname, [], value, is_set, fr)
            if kind == "const":
                self.error("Assignment to constant not permitted", line)
            self.error("Expected: variable", line)
        if isinstance(e, MemberE):
            lname = e.name.lower()
            known = self.member_target(e)
            if known is not None and known[0] == "module":
                b = self.resolve_in_module(known[1], lname, known[1] is self.module)
                if b is not None and b[0] == "modvar":
                    mv, s, typ = b[1].v, b[2], b[3]
                    return self.store_list(lambda fr: mv, s, typ, is_set)
                self.error("Expected: variable", line)
            if isinstance(e.obj, WithE):
                if self.sc.with_depth == 0:
                    self.error("Invalid or unqualified reference", line)
                return lambda fr, value: project.member_let(fr.w[-1], lname, [], value, is_set, fr)
            of = self.expr(e.obj)
            return lambda fr, value: project.member_let(of(fr), lname, [], value, is_set, fr)
        if isinstance(e, CallE):
            argcs = self.args(e.args)
            target = self.unqualified(e.target)
            if isinstance(target, NameE):
                lname = target.name.lower()
                b = self.resolve(lname)
                if b is not None:
                    kind = b[0]
                    if kind in ("local", "modvar", "memvar"):
                        getv = self.name_get(target)
                        return lambda fr, value: project.index_let(getv(fr), argcs, value, fr)
                    if kind == "proc":
                        group = b[2]
                        proc = group.get("set" if is_set else "let") or group.get("let")
                        if proc is not None:
                            me = self.me_getter(b[1])
                            return lambda fr, value: project.invoke(proc, argcs, fr, me(fr), value, True)
                        getter = self.proc_call(b[1], group, argcs)
                        return lambda fr, value: default_let(getter(fr), value)
                    if kind == "host":
                        return lambda fr, value: project.member_let(project.host.app, lname, argcs,
                                                                    value, is_set, fr)
                    if kind == "docmember":
                        return lambda fr, value: project.member_let(fr.me, lname, argcs, value, is_set, fr)
                self.error("Expected: array or procedure", line)
            if isinstance(target, MemberE):
                lname = target.name.lower()
                if isinstance(target.obj, WithE):
                    return lambda fr, value: project.member_let(fr.w[-1], lname, argcs, value, is_set, fr)
                of = self.expr(target.obj)
                return lambda fr, value: project.member_let(of(fr), lname, argcs, value, is_set, fr)
            tf = self.expr(target)
            return lambda fr, value: project.index_let(tf(fr), argcs, value, fr)
        if isinstance(e, BangE):
            of = self.expr(e.obj)
            key = e.name
            return lambda fr, value: default_let(of(fr), value, key)
        self.error("Expected: variable", line)

    def store_raw(self, e, line):
        """A target set as it is, with no coercion: what ReDim and Erase
        put an array in with."""
        if isinstance(e, NameE):
            b = self.resolve(e.name.lower())
            if b is None:
                b = ("local", self.implicit(e.name, e.suffix, e.line))
            kind = b[0]
            if kind == "local":
                local = b[1]
                s = local.slot
                if local.static:
                    store = self.sc.proc.statics

                    def set_static(fr, value):
                        store[s] = value
                    return set_static
                if local.param is not None and not local.param.byval:
                    return lambda fr, value: fr.v[s].set(value)

                def set_local(fr, value):
                    fr.v[s] = value
                return set_local
            if kind == "modvar":
                mv, s = b[1].v, b[2]

                def set_mod(fr, value):
                    mv[s] = value
                return set_mod
            if kind == "memvar":
                s = b[1]

                def set_member(fr, value):
                    fr.me.v[s] = value
                return set_member
            if kind == "retval":
                def set_ret(fr, value):
                    fr.v[0] = value
                return set_ret
        return self.store(e, True, line)

    def store_slot(self, local, is_set):
        s = local.slot
        typ = local.typ
        if local.is_array and not is_set:
            def set_array(fr, value, store=None):
                if typ == "byte" and isinstance(value, str):
                    value = byte_array(value.encode("utf-16-le"))
                if not isinstance(value, VBArray):
                    raise VBAError(13)
                value = value.copy()
                if local.static:
                    self.sc.proc.statics[s] = value
                elif local.param is not None and not local.param.byval:
                    fr.v[s].set(value)
                else:
                    fr.v[s] = value
            return set_array
        if local.static:
            store = self.sc.proc.statics
            return self.store_list(lambda fr: store, s, typ, is_set, local.decl_object)
        if local.param is not None and not local.param.byval:
            obj_type = local.decl_object

            def set_param(fr, value):
                r = fr.v[s]
                if is_set:
                    r.set(check_object(value))
                elif obj_type and is_object(r.get()) and r.get() is not None:
                    default_let(r.get(), plain_value(value))
                else:
                    r.set(coerce(plain_value(value), typ))
            return set_param
        if not is_set and not local.decl_object:
            fast = _fast_local_store(s, typ)
            if fast is not None:
                return fast
        return self.store_list(lambda fr: fr.v, s, typ, is_set, local.decl_object)

    def store_list(self, container, s, typ, is_set, obj_type=None):
        if obj_type is None:
            obj_type = self.project.is_object_type(typ) and self.project.find_type(self.module, typ) is None
        if is_set:
            def set_obj(fr, value):
                container(fr)[s] = check_object(value)
            return set_obj
        if obj_type:
            def let_obj(fr, value):
                c = container(fr)
                if is_object(c[s]) and c[s] is not None:
                    default_let(c[s], plain_value(value))
                else:
                    raise VBAError(91)
            return let_obj
        if typ == "variant":
            def let_variant(fr, value):
                t = type(value)
                if t is int or t is float or t is str or t is bool:
                    container(fr)[s] = value
                else:
                    container(fr)[s] = coerce(plain_value(value), "variant")
            return let_variant

        def let_typed(fr, value):
            container(fr)[s] = coerce(plain_value(value), typ)
        return let_typed

    # -- statements --
    def block(self, stmts, top=False):
        compiled = []
        lines = []
        labels = {}
        for st in stmts:
            if st.kind == "label":
                labels[st.name] = len(compiled)
                continue
            f = self.stmt(st)
            if f is None:
                continue
            compiled.append(f)
            lines.append(st.line)
        runner = make_block(compiled, lines, labels, top)
        if top and self.sc.proc is not None:
            self.sc.proc.labels = labels
            for st in self.walk(stmts):
                if st.kind == "onerror" and st.how == "goto" and st.label not in labels:
                    self.error("Label not defined: %s" % st.label, st.line)
        return runner

    def body(self, stmts):
        return self.block(stmts)

    def stmt(self, st):
        k = st.kind
        self.line = st.line
        method = getattr(self, "st_" + k, None)
        if method is None:
            self.error("Unsupported statement: %s" % k, st.line)
        return method(st)

    def st_dim(self, st):
        return None

    def st_const(self, st):
        return None

    def st_assign(self, st):
        valf = self.expr(st.expr)
        store = self.store(st.target, st.set, st.line)
        return lambda fr: store(fr, valf(fr))

    def st_call(self, st):
        e = st.expr
        project = self.project
        if isinstance(e, NameE):
            lname = e.name.lower()
            b = self.resolve(lname)
            if b is None:
                name = e.name

                def undefined(fr):
                    raise VBAError(35, "Sub or Function not defined: %s" % name)
                return undefined
            if b[0] in ("local", "modvar", "memvar", "const", "retval"):
                self.error("Expected: procedure, not variable", st.line)
            return self.name_get(e)
        if isinstance(e, (CallE, MemberE)):
            return self.expr(e)
        if isinstance(e, (EvalE, WithE, MeE)):
            return self.expr(e)
        self.error("Expected: procedure", st.line)

    def st_if(self, st):
        branches = [(self.expr(c), self.block(b)) for c, b in st.branches]
        orelse = self.block(st.orelse) if st.orelse else None
        if len(branches) == 1:
            cond, body = branches[0]

            def if1(fr):
                c = cond(fr)
                if (c if type(c) is bool else truth(c)):
                    body(fr)
                elif orelse is not None:
                    orelse(fr)
            return if1

        def if_n(fr):
            for cond, body in branches:
                c = cond(fr)
                if c if type(c) is bool else truth(c):
                    body(fr)
                    return
            if orelse is not None:
                orelse(fr)
        return if_n

    def st_select(self, st):
        test = self.expr(st.test)
        text = self.text
        cases = []
        for conds, body in st.cases:
            matchers = []
            for c in conds:
                if c[0] == "eq":
                    matchers.append(("eq", self.expr(c[1])))
                elif c[0] == "range":
                    matchers.append(("range", self.expr(c[1]), self.expr(c[2])))
                else:
                    matchers.append(("is", BINARY_OPS[c[1]], self.expr(c[2])))
            cases.append((matchers, self.block(body)))
        orelse = self.block(st.orelse)

        def select(fr):
            v = test(fr)
            for matchers, body in cases:
                for m in matchers:
                    if m[0] == "eq":
                        hit = truth(op_eq(v, m[1](fr), text))
                    elif m[0] == "range":
                        hit = truth(op_ge(v, m[1](fr), text)) and truth(op_le(v, m[2](fr), text))
                    else:
                        op = m[1]
                        hit = truth(op(v, m[2](fr), text) if op in (op_eq, op_ne, op_lt, op_le, op_gt, op_ge)
                                    else op(v, m[2](fr)))
                    if hit:
                        body(fr)
                        return
            orelse(fr)
        return select

    def st_for(self, st):
        getv = self.expr(st.var)
        setv = self.store(st.var, False, st.line)
        start = self.expr(st.start)
        end = self.expr(st.end)
        step = self.expr(st.step) if st.step is not None else None
        body = self.block(st.body)

        def for_loop(fr):
            a = to_num(start(fr))
            b = to_num(end(fr))
            s = to_num(step(fr)) if step is not None else 1
            setv(fr, a)
            try:
                if s >= 0:
                    while True:
                        v = getv(fr)
                        if (v if type(v) is int or type(v) is float else to_num(v)) > b:
                            break
                        body(fr)
                        v = getv(fr)
                        setv(fr, (v if type(v) is int or type(v) is float else to_num(v)) + s)
                else:
                    while True:
                        v = getv(fr)
                        if (v if type(v) is int or type(v) is float else to_num(v)) < b:
                            break
                        body(fr)
                        v = getv(fr)
                        setv(fr, (v if type(v) is int or type(v) is float else to_num(v)) + s)
            except ExitLoop as x:
                if x.kind != "for":
                    raise
        return for_loop

    def st_foreach(self, st):
        setv = self.store(st.var, True, st.line)
        setv_let = self.store(st.var, False, st.line)
        coll = self.expr(st.coll)
        body = self.block(st.body)

        def for_each(fr):
            items = vba_iter(coll(fr))
            try:
                for item in items:
                    if is_object(item):
                        setv(fr, item)
                    else:
                        setv_let(fr, item)
                    body(fr)
            except ExitLoop as x:
                if x.kind != "for":
                    raise
        return for_each

    def st_do(self, st):
        body = self.block(st.body)
        pre = (st.pre[0], self.expr(st.pre[1])) if st.pre else None
        post = (st.post[0], self.expr(st.post[1])) if st.post else None

        def do_loop(fr):
            try:
                while True:
                    if pre is not None:
                        c = truth(pre[1](fr))
                        if (pre[0] == "while") != c:
                            return
                    body(fr)
                    if post is not None:
                        c = truth(post[1](fr))
                        if (post[0] == "while") != c:
                            return
            except ExitLoop as x:
                if x.kind != "do":
                    raise
        return do_loop

    def st_while(self, st):
        cond = self.expr(st.cond)
        body = self.block(st.body)

        def while_loop(fr):
            while truth(cond(fr)):
                body(fr)
        return while_loop

    def st_with(self, st):
        of = self.expr(st.expr)
        self.sc.with_depth += 1
        try:
            body = self.block(st.body)
        finally:
            self.sc.with_depth -= 1

        def with_block(fr):
            obj = of(fr)
            if obj is None:
                raise VBAError(91)
            fr.w.append(obj)
            try:
                body(fr)
            finally:
                fr.w.pop()
        return with_block

    def st_exit(self, st):
        what = st.what
        err = self.project.err
        if what in ("for", "do"):
            def exit_loop(fr):
                raise ExitLoop(what)
            return exit_loop

        def exit_proc(fr):
            if fr.in_handler:
                err.Clear()
            raise ExitProc()
        return exit_proc

    def st_goto(self, st):
        label = st.label

        def goto(fr):
            raise GotoSignal(label)
        return goto

    def st_ongoto(self, st):
        e = self.expr(st.expr)
        labels = st.labels
        gosub = st.gosub

        def on_goto(fr):
            i = to_long(e(fr))
            if 1 <= i <= len(labels):
                if gosub:
                    self.run_gosub(fr, labels[i - 1])
                else:
                    raise GotoSignal(labels[i - 1])
        return on_goto

    def run_gosub(self, fr, label):
        at = fr.proc.labels.get(label)
        if at is None:
            raise VBAError(1004, "Label not defined: %s" % label)
        try:
            fr.proc.body(fr, at)
        except ReturnSignal:
            return
        raise ExitProc()

    def st_gosub(self, st):
        label = st.label
        return lambda fr: self.run_gosub(fr, label)

    def st_return(self, st):
        def ret(fr):
            raise ReturnSignal()
        return ret

    def st_onerror(self, st):
        how = st.how
        label = st.label
        err = self.project.err

        def on_error(fr):
            err.Clear()
            if how == "next":
                fr.on_error = "next"
            elif how == "goto":
                fr.on_error = label
            elif how == "off":
                fr.on_error = None
            else:
                fr.in_handler = False
        return on_error

    def st_resume(self, st):
        how = st.how
        label = st.label

        def resume(fr):
            if not fr.in_handler:
                raise VBAError(20)
            raise ResumeSignal(how, label)
        return resume

    def st_load(self, st):
        # Naming a UserForm loads it, running UserForm_Initialize.
        e = self.expr(st.expr)

        def load(fr):
            e(fr)
        return load

    def st_unload(self, st):
        e = self.expr(st.expr)
        project = self.project

        def unload(fr):
            project.host.unload(e(fr))
        return unload

    def st_end(self, st):
        def end(fr):
            raise EndSignal()
        return end

    def st_stop(self, st):
        line = st.line
        project = self.project

        def stop(fr):
            if _trace is not None:
                _trace(fr, line, True)
                return
            raise StopSignal()
        return stop

    def st_error(self, st):
        e = self.expr(st.expr)

        def error(fr):
            raise VBAError(to_long(e(fr)))
        return error

    def st_raiseevent(self, st):
        args = self.args(st.args)
        event = st.name.lower()
        project = self.project

        def raise_event(fr):
            if isinstance(fr.me, VBObject):
                project.raise_event(fr.me, event, args, fr)
        return raise_event

    def st_redim(self, st):
        project = self.project
        items = []
        for target, dims, typ in st.items:
            elem_type = typ
            if isinstance(target, NameE):
                b = self.resolve(target.name.lower())
                if b is not None and b[0] == "local":
                    elem_type = elem_type or b[1].typ
                    if b[1].typ == "variant" and typ:
                        b[1].typ = "variant"
                elif b is not None and b[0] in ("modvar", "memvar"):
                    elem_type = elem_type or (b[3] if b[0] == "modvar" else b[2])
            elem_type = elem_type or "variant"
            bounds = [(self.expr(lb) if lb is not None else None, self.expr(ub)) for lb, ub in dims]
            getv = self.expr(target)
            setv = self.store_raw(target, st.line)
            items.append((getv, setv, bounds, elem_type))
        preserve = st.preserve
        base = self.module.base
        m = self.module

        def redim(fr):
            for getv, setv, bounds, elem_type in items:
                b = [(to_long(lb(fr)) if lb is not None else base, to_long(ub(fr))) for lb, ub in bounds]
                new = project.new_array(b, elem_type, False, m)
                if preserve:
                    old = getv(fr)
                    if isinstance(old, VBArray):
                        _copy_preserved(old, new)
                setv(fr, new)
        return redim

    def st_erase(self, st):
        items = [(self.expr(t), self.store_raw(t, st.line)) for t in st.targets]

        def erase(fr):
            for getv, setv in items:
                arr = getv(fr)
                if not isinstance(arr, VBArray):
                    raise VBAError(13)
                if arr.fixed:
                    fresh = default_value(arr.typ)
                    arr.data = [(v.__class__(v.typedef) if isinstance(v, UDTValue) else fresh)
                                for v in arr.data]
                else:
                    setv(fr, VBArray([], arr.typ))
        return erase

    def st_midassign(self, st):
        getv = self.expr(st.target)
        setv = self.store(st.target, False, st.line)
        start = self.expr(st.start)
        length = self.expr(st.length) if st.length is not None else None
        value = self.expr(st.expr)

        def mid_assign(fr):
            s = to_str(getv(fr))
            at = to_long(start(fr)) - 1
            if at < 0 or at > len(s):
                raise VBAError(5)
            r = to_str(value(fr))
            n = len(r) if length is None else min(to_long(length(fr)), len(r))
            n = min(n, len(s) - at)
            setv(fr, s[:at] + r[:n] + s[at + n:])
        return mid_assign

    def st_lset(self, st):
        return self._set_aligned(st, True)

    def st_rset(self, st):
        return self._set_aligned(st, False)

    def _set_aligned(self, st, left):
        getv = self.expr(st.target)
        setv = self.store(st.target, False, st.line)
        value = self.expr(st.expr)

        def aligned(fr):
            old = to_str(getv(fr))
            new = to_str(value(fr))[:len(old)]
            setv(fr, new.ljust(len(old)) if left else new.rjust(len(old)))
        return aligned

    def st_print(self, st):
        items = []
        for item, sep in st.items:
            if isinstance(item, tuple):
                items.append(((item[0], self.expr(item[1]) if item[1] is not None else None), sep))
            elif item is None:
                items.append((None, sep))
            else:
                items.append((self.expr(item), sep))
        filef = self.expr(st.file) if st.file is not None else None
        project = self.project

        def print_stmt(fr):
            if filef is None:
                line = project.debug_line
            else:
                handle = project.file(filef(fr))
                line = handle.pending
            ends_line = True
            for item, sep in items:
                if isinstance(item, tuple):
                    kind, arg = item
                    n = to_long(arg(fr)) if arg is not None else 0
                    if kind == "spc":
                        line += " " * max(n, 0)
                    else:
                        col = len(line.rsplit("\n", 1)[-1])
                        if n > col + 1:
                            line += " " * (n - col - 1)
                elif item is not None:
                    line += print_text(item(fr))
                ends_line = sep is None
                if sep == ",":
                    col = len(line.rsplit("\n", 1)[-1])
                    line += " " * (14 - col % 14)
            if not items:
                ends_line = True
            if filef is None:
                if ends_line:
                    project.write_debug(line + "\n")
                    project.debug_line = ""
                else:
                    project.debug_line = line
            else:
                if ends_line:
                    handle.write(line + "\r\n")
                    handle.pending = ""
                else:
                    handle.pending = line
        return print_stmt

    def st_write(self, st):
        filef = self.expr(st.file)
        items = [self.expr(i) for i in st.items]
        project = self.project

        def write_stmt(fr):
            handle = project.file(filef(fr))
            handle.write(",".join(write_text(i(fr)) for i in items) + "\r\n")
        return write_stmt

    def st_open(self, st):
        path = self.expr(st.path)
        num = self.expr(st.file)
        mode = st.mode
        project = self.project
        return lambda fr: project.open_file(to_str(path(fr)), mode, to_long(num(fr)))

    def st_close(self, st):
        nums = [self.expr(n) for n in st.files]
        project = self.project

        def close(fr):
            if not nums:
                for n in list(project.files):
                    project.close_file(n)
            for n in nums:
                project.close_file(to_long(n(fr)))
        return close

    def st_lineinput(self, st):
        filef = self.expr(st.file)
        setv = self.store(st.target, False, st.line)
        project = self.project
        return lambda fr: setv(fr, project.file(filef(fr)).read_line())

    def st_input(self, st):
        filef = self.expr(st.file)
        setters = [self.store(t, False, st.line) for t in st.targets]
        project = self.project

        def input_stmt(fr):
            handle = project.file(filef(fr))
            for setv in setters:
                setv(fr, handle.read_field())
        return input_stmt

    def st_get(self, st):
        return self._binary_io(st)

    def st_put(self, st):
        return self._binary_io(st)

    def st_seek(self, st):
        return self._binary_io(st)

    def _binary_io(self, st):
        def unsupported(fr):
            raise VBAError(54, "Get, Put and Seek on binary and random files are not supported")
        return unsupported

    def st_name(self, st):
        old = self.expr(st.old)
        new = self.expr(st.new)

        def rename(fr):
            os.rename(to_str(old(fr)), to_str(new(fr)))
        return rename


def _fast_local_store(s, typ):
    """Let for a local of a common type, with the usual case first: an
    Integer given an int in range, a Double a float, a String a str."""
    if typ in _INT_RANGE:
        lo, hi = _INT_RANGE[typ]

        def let_int(fr, value):
            if type(value) is int and lo <= value <= hi:
                fr.v[s] = value
            else:
                fr.v[s] = coerce(plain_value(value), typ)
        return let_int
    if typ == "double" or typ == "single":
        def let_float(fr, value):
            t = type(value)
            if t is float:
                fr.v[s] = value
            elif t is int:
                fr.v[s] = float(value)
            else:
                fr.v[s] = coerce(plain_value(value), typ)
        return let_float
    if typ == "string":
        def let_str(fr, value):
            if type(value) is str:
                fr.v[s] = value
            else:
                fr.v[s] = to_str(plain_value(value))
        return let_str
    if typ == "variant":
        def let_variant(fr, value):
            t = type(value)
            if t is int or t is float or t is str or t is bool:
                fr.v[s] = value
            else:
                fr.v[s] = coerce(plain_value(value), "variant")
        return let_variant
    return None


def _valued(getv):
    def get(fr):
        v = getv(fr)
        t = type(v)
        if t is int or t is float or t is str or t is bool or v is EMPTY:
            return v
        if is_object(v) and v is not None and getattr(v, "_vba_default_get", None) is not None:
            try:
                return default_member(v)
            except VBAError:
                return v
        return v
    return get


def check_object(value):
    if value is None or is_object(value):
        return value
    raise VBAError(424)


def type_of(project, value, typename):
    if value is None:
        return False
    typename = typename.rpartition(".")[2]      # Excel.Range, MSForms.TextBox
    if isinstance(value, VBObject):
        return value.cls.lname == typename or typename == "object" or typename in value.cls.implements
    if typename == "object":
        return is_object(value)
    return project.host.typename(value).lower() == typename or \
        type(value).__name__.lower() == typename


def write_text(v):
    """A value as Write # puts it in a file: text in quotes, dates and
    truth in hashes, numbers bare."""
    if isinstance(v, str):
        return '"%s"' % v
    if type(v) is bool:
        return "#TRUE#" if v else "#FALSE#"
    if type(v) is VBDate:
        dt = v.to_datetime()
        if float(v) == int(v):
            return "#%04d-%02d-%02d#" % (dt.year, dt.month, dt.day)
        return "#%04d-%02d-%02d %02d:%02d:%02d#" % (dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second)
    if v is NULL:
        return "#NULL#"
    if v is EMPTY:
        return ""
    if isinstance(v, ErrValue):
        return "#ERROR %d#" % v.code
    return to_str(v)


# ---- Files ------------------------------------------------------------------------

class VBFile:
    """A file opened with Open: read whole for Input, written as it goes
    for Output and Append."""

    def __init__(self, path, mode):
        self.path = path
        self.mode = mode
        self.pending = ""
        self.text = None
        self.pos = 0
        self.f = None
        if mode == "input":
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except FileNotFoundError:
                raise VBAError(53)
            try:
                self.text = data.decode("utf-8")
            except UnicodeDecodeError:
                self.text = data.decode("cp1252", "replace")
            if self.text.startswith("\ufeff"):
                self.text = self.text[1:]
        elif mode in ("output", "append"):
            self.f = open(path, "w" if mode == "output" else "a", encoding="utf-8", newline="")
        elif mode in ("binary", "random"):
            if not os.path.exists(path):
                open(path, "wb").close()
            self.f = open(path, "r+b")
        else:
            raise VBAError(54)

    def write(self, text):
        if self.f is None or self.mode not in ("output", "append"):
            raise VBAError(54)
        self.f.write(text)

    def eof(self):
        if self.text is not None:
            return self.pos >= len(self.text)
        if self.f is not None and self.mode in ("binary", "random"):
            here = self.f.tell()
            self.f.seek(0, 2)
            end = self.f.tell()
            self.f.seek(here)
            return here >= end
        return True

    def read_line(self):
        if self.text is None:
            raise VBAError(54)
        if self.pos >= len(self.text):
            raise VBAError(62)
        t = self.text
        i = self.pos
        j = i
        while j < len(t) and t[j] not in "\r\n":
            j += 1
        line = t[i:j]
        if j < len(t) and t[j] == "\r":
            j += 1
        if j < len(t) and t[j] == "\n":
            j += 1
        self.pos = j
        return line

    def read_chars(self, n):
        if self.text is None:
            raise VBAError(54)
        if self.pos + n > len(self.text):
            raise VBAError(62)
        s = self.text[self.pos:self.pos + n]
        self.pos += n
        return s

    def read_field(self):
        """One field for Input #: a quoted text, or up to the next comma
        or the end of the line, read as a number when it is one."""
        if self.text is None:
            raise VBAError(54)
        t = self.text
        i = self.pos
        while i < len(t) and t[i] in " \t":
            i += 1
        if i >= len(t):
            raise VBAError(62)
        if t[i] == '"':
            j = t.find('"', i + 1)
            if j < 0:
                j = len(t)
            value = t[i + 1:j]
            i = j + 1
            quoted = True
        else:
            j = i
            while j < len(t) and t[j] not in ",\r\n":
                j += 1
            value = t[i:j].strip()
            i = j
            quoted = False
        while i < len(t) and t[i] in " \t":
            i += 1
        if i < len(t) and t[i] == ",":
            i += 1
        elif i < len(t) and t[i] in "\r\n":
            if t[i] == "\r":
                i += 1
            if i < len(t) and t[i] == "\n":
                i += 1
        self.pos = i
        if quoted:
            return value
        if value.upper() in ("#TRUE#", "#FALSE#"):
            return value.upper() == "#TRUE#"
        if value.startswith("#") and value.endswith("#"):
            d = parse_date(value[1:-1])
            if d is not None:
                return d
        n = parse_number(value)
        if n is not None:
            return int(n) if n == int(n) and "." not in value else n
        return value

    def size(self):
        if self.text is not None:
            return len(self.text.encode("utf-8"))
        self.f.flush()
        return os.path.getsize(self.path)

    def close(self):
        if self.f is not None:
            if self.pending:
                self.f.write(self.pending)
            self.f.close()
            self.f = None


def _project_files(project):
    return project.files


def _open_file(self, path, mode, num):
    if num in self.files:
        raise VBAError(55)
    if num < 1 or num > 511:
        raise VBAError(52)
    self.files[num] = VBFile(path, mode)


def _file(self, num):
    n = to_long(num)
    f = self.files.get(n)
    if f is None:
        raise VBAError(52)
    return f


def _close_file(self, num):
    f = self.files.pop(num, None)
    if f is not None:
        f.close()


Project.open_file = _open_file
Project.file = _file
Project.close_file = _close_file


# ---- The library ---------------------------------------------------------------------

# The project whose code is running, for the functions that need it:
# Err, files, MsgBox's host.  Set by the host around each run.
current = None


def _null_or(v, fn):
    return NULL if v is NULL else fn(v)


def _compare_text(compare):
    return compare is not MISSING and to_long(compare) == 1


def f_len(expression):
    if expression is NULL:
        return NULL
    if isinstance(expression, UDTValue):
        return len(expression.fields) * 8
    return len(to_str(expression))


def f_left(string, length):
    if string is NULL:
        return NULL
    n = to_long(length)
    if n < 0:
        raise VBAError(5)
    return to_str(string)[:n]


def f_right(string, length):
    if string is NULL:
        return NULL
    n = to_long(length)
    if n < 0:
        raise VBAError(5)
    s = to_str(string)
    return s[len(s) - n:] if n else ""


def f_mid(string, start, length=MISSING):
    if string is NULL:
        return NULL
    s = to_str(string)
    a = to_long(start)
    if a < 1:
        raise VBAError(5)
    if length is MISSING:
        return s[a - 1:]
    n = to_long(length)
    if n < 0:
        raise VBAError(5)
    return s[a - 1:a - 1 + n]


def f_instr(*args):
    args = [a for a in args]
    while args and args[-1] is MISSING:
        args.pop()
    start = 1
    compare = MISSING
    if len(args) >= 3 and not isinstance(args[0], str) and args[0] is not NULL:
        start = to_long(args[0])
        args = args[1:]
    if len(args) < 2:
        raise VBAError(449)
    s1, s2 = args[0], args[1]
    if len(args) > 2:
        compare = args[2]
    if s1 is NULL or s2 is NULL:
        return NULL
    if start < 1:
        raise VBAError(5)
    a, b = to_str(s1), to_str(s2)
    if _compare_text(compare) or (compare is MISSING and current is not None and current.text_compare_now):
        a, b = a.lower(), b.lower()
    if b == "":
        return start if start <= len(a) else 0
    return a.find(b, start - 1) + 1


def f_instrrev(stringcheck, stringmatch, start=-1, compare=MISSING):
    if stringcheck is NULL or stringmatch is NULL:
        return NULL
    a, b = to_str(stringcheck), to_str(stringmatch)
    if _compare_text(compare):
        a, b = a.lower(), b.lower()
    st = to_long(start)
    if st == -1:
        st = len(a)
    if st < 1 and st != -1:
        raise VBAError(5)
    if b == "":
        return st
    return a.rfind(b, 0, st) + 1


def f_replace(expression, find, replace, start=1, count=-1, compare=MISSING):
    s = to_str(expression)
    f = to_str(find)
    r = to_str(replace)
    st = to_long(start)
    n = to_long(count)
    if st < 1:
        raise VBAError(5)
    s = s[st - 1:]
    if f == "":
        return s
    if _compare_text(compare):
        out = re.sub(re.escape(f), lambda m: r, s, count=0 if n < 0 else n, flags=re.IGNORECASE)
        return out
    return s.replace(f, r) if n < 0 else s.replace(f, r, n)


def f_ucase(string):
    return _null_or(string, lambda v: to_str(v).upper())


def f_lcase(string):
    return _null_or(string, lambda v: to_str(v).lower())


def f_trim(string):
    return _null_or(string, lambda v: to_str(v).strip(" "))


def f_ltrim(string):
    return _null_or(string, lambda v: to_str(v).lstrip(" "))


def f_rtrim(string):
    return _null_or(string, lambda v: to_str(v).rstrip(" "))


def f_space(number):
    return " " * max(to_long(number), 0)


def f_string(number, character):
    n = to_long(number)
    if n < 0:
        raise VBAError(5)
    if isinstance(character, str):
        if not character:
            raise VBAError(5)
        c = character[0]
    else:
        c = chr(to_long(character) % 256)
    return c * n


def f_strreverse(expression):
    return to_str(expression)[::-1]


def f_strcomp(string1, string2, compare=MISSING):
    if string1 is NULL or string2 is NULL:
        return NULL
    a, b = to_str(string1), to_str(string2)
    if _compare_text(compare):
        a, b = a.lower(), b.lower()
    return (a > b) - (a < b)


def f_strconv(string, conversion, lcid=MISSING):
    c = to_long(conversion)
    # vbUnicode reads bytes in the ANSI code page; vbFromUnicode writes
    # them, and gives the Byte array a macro assigns the result to.
    if c & 64:
        if isinstance(string, VBArray):
            data = bytes(to_long(b) & 0xFF for b in string.data)
        else:
            data = to_str(string).encode("utf-16-le")
        return data.decode("cp1252", "replace")
    if c & 128:
        return byte_array(to_str(string).encode("cp1252", "replace"))
    s = to_str(string)
    if c & 1 and not c & 2:
        return s.upper()
    if c & 2 and not c & 1:
        return s.lower()
    if c & 3 == 3:
        out = []
        prev = " "
        for ch in s.lower():
            out.append(ch.upper() if not prev.isalnum() and prev != "'" else ch)
            prev = ch
        return "".join(out)
    return s


def f_split(expression, delimiter=" ", limit=-1, compare=MISSING):
    s = to_str(expression)
    if s == "":
        return VBArray([(0, -1)], "string", data=[])
    d = to_str(delimiter)
    n = to_long(limit)
    if d == "":
        parts = [s]
    elif _compare_text(compare):
        parts = re.split(re.escape(d), s, maxsplit=0 if n < 0 else max(n - 1, 0), flags=re.IGNORECASE)
    else:
        parts = s.split(d) if n < 0 else s.split(d, max(n - 1, 0))
    return VBArray([(0, len(parts) - 1)], "string", data=parts)


def f_join(sourcearray, delimiter=" "):
    if not isinstance(sourcearray, VBArray):
        raise VBAError(13)
    return to_str(delimiter).join(to_str(v) for v in sourcearray.data)


def f_filter(sourcearray, match, include=True, compare=MISSING):
    if not isinstance(sourcearray, VBArray):
        raise VBAError(13)
    m = to_str(match)
    text = _compare_text(compare)
    out = []
    for v in sourcearray.data:
        s = to_str(v)
        hit = (m.lower() in s.lower()) if text else (m in s)
        if hit == to_bool(include):
            out.append(s)
    return VBArray([(0, len(out) - 1)], "string", data=out)


def f_asc(string):
    s = to_str(string)
    if not s:
        raise VBAError(5)
    c = ord(s[0])
    if c < 256:
        return c
    try:
        b = s[0].encode("cp1252")
        return b[0]
    except UnicodeEncodeError:
        return 63


def f_ascw(string):
    s = to_str(string)
    if not s:
        raise VBAError(5)
    return ord(s[0])


def f_chr(charcode):
    n = to_long(charcode)
    if n < 0 or n > 255:
        if -32768 <= n <= 65535:
            return chr(n & 0xFFFF)
        raise VBAError(5)
    if 128 <= n < 160:
        try:
            return bytes([n]).decode("cp1252")
        except UnicodeDecodeError:
            return chr(n)
    return chr(n)


def f_chrw(charcode):
    n = to_long(charcode)
    return chr(n & 0xFFFF)


def f_hex(number):
    if number is NULL:
        return NULL
    n = to_long(number, "longlong")
    if n < 0:
        n &= 0xFFFFFFFF if n >= -2 ** 31 else 0xFFFFFFFFFFFFFFFF
    return "%X" % n


def f_oct(number):
    if number is NULL:
        return NULL
    n = to_long(number, "longlong")
    if n < 0:
        n &= 0xFFFFFFFF
    return "%o" % n


def f_val(string):
    """Val: the number the text begins with; spaces are skipped, and it
    stops at the first thing that is not part of a number."""
    s = to_str(string)
    t = "".join(s.split(" ")).replace("\t", "").replace("\n", "")
    m = re.match(r"&[hH]([0-9a-fA-F]+)", t)
    if m:
        v = int(m.group(1), 16)
        if v <= 0xFFFF and v >= 0x8000 and len(m.group(1)) <= 4:
            v -= 0x10000
        return float(v)
    m = re.match(r"&[oO]([0-7]+)", t)
    if m:
        return float(int(m.group(1), 8))
    m = re.match(r"[+-]?(\d+\.?\d*|\.\d+)([eEdD][+-]?\d+)?", t)
    if not m:
        return 0.0
    return float(m.group(0).replace("d", "e").replace("D", "e"))


def f_str(number):
    if number is NULL:
        return NULL
    n = to_num(number)
    text = number_text(n) if type(n) is float else str(n)
    return text if n < 0 else " " + text


def f_abs(number):
    return _null_or(number, lambda v: abs(to_num(v)))


def f_atn(number):
    return math.atan(to_double(number))


def f_cos(number):
    return math.cos(to_double(number))


def f_sin(number):
    return math.sin(to_double(number))


def f_tan(number):
    return math.tan(to_double(number))


def f_exp(number):
    try:
        return math.exp(to_double(number))
    except OverflowError:
        raise VBAError(6)


def f_log(number):
    x = to_double(number)
    if x <= 0:
        raise VBAError(5)
    return math.log(x)


def f_sqr(number):
    x = to_double(number)
    if x < 0:
        raise VBAError(5)
    return math.sqrt(x)


def f_sgn(number):
    x = to_num(number)
    return (x > 0) - (x < 0)


def f_int(number):
    if number is NULL:
        return NULL
    x = to_num(number)
    if type(x) is int:
        return x
    r = math.floor(x)
    return VBDate(r) if type(number) is VBDate else float(r)


def f_fix(number):
    if number is NULL:
        return NULL
    x = to_num(number)
    if type(x) is int:
        return x
    return float(math.trunc(x))


def f_round(expression, numdecimalsafterdecimal=0):
    import decimal
    x = to_num(expression)
    n = to_long(numdecimalsafterdecimal)
    if n < 0:
        raise VBAError(5)
    if type(x) is int:
        return x
    d = decimal.Decimal(repr(float(x))).quantize(decimal.Decimal(1).scaleb(-n), rounding=decimal.ROUND_HALF_EVEN)
    return float(d)


def f_rnd(number=MISSING):
    p = current
    if p is None:
        return random.random()
    if number is not MISSING:
        n = to_double(number)
        if n == 0:
            return p.rnd_last
        if n < 0:
            p.rnd.seed(n)
    p.rnd_last = p.rnd.random()
    return p.rnd_last


def f_randomize(number=MISSING):
    if current is not None:
        current.rnd.seed(None if number is MISSING else to_double(number))


def f_cbool(expression):
    return to_bool(expression)


def f_cbyte(expression):
    return to_long(expression, "byte")


def f_ccur(expression):
    return round(to_double(expression), 4)


def f_cdate(date):
    return to_date(date)


def f_cdbl(expression):
    return to_double(expression)


def f_cint(expression):
    return to_long(expression, "integer")


def f_clng(expression):
    return to_long(expression, "long")


def f_clnglng(expression):
    return to_long(expression, "longlong")


def f_csng(expression):
    x = to_double(expression)
    if abs(x) > 3.402823e38:
        raise VBAError(6)
    return float("%.7g" % x)


def f_cstr(expression):
    if isinstance(expression, ErrValue):
        return "Error %d" % expression.code
    return to_str(expression)


def f_cvar(expression):
    return expression


def f_cverr(errornumber):
    return ErrValue(to_long(errornumber))


def f_isarray(varname):
    return isinstance(varname, VBArray)


def f_isdate(expression):
    if type(expression) is VBDate:
        return True
    if isinstance(expression, str):
        return parse_date(expression) is not None
    return False


def f_isempty(expression):
    return expression is EMPTY


def f_iserror(expression):
    return isinstance(expression, ErrValue)


def f_ismissing(argname=MISSING):
    return argname is MISSING


def f_isnull(expression):
    return expression is NULL


def f_isnumeric(expression):
    t = type(expression)
    if t is int or t is float or t is bool or expression is EMPTY:
        return True
    if t is VBDate:
        return False
    if t is str:
        return parse_number(expression) is not None
    return False


def f_isobject(identifier):
    return is_object(identifier)


def f_typename(varname):
    return typename(varname)


def typename(v):
    t = type(v)
    if t is bool:
        return "Boolean"
    if t is int:
        return "Integer" if -32768 <= v <= 32767 else "Long"
    if t is float:
        return "Double"
    if t is str:
        return "String"
    if t is VBDate:
        return "Date"
    if v is EMPTY:
        return "Empty"
    if v is NULL:
        return "Null"
    if v is None:
        return "Nothing"
    if v is MISSING:
        return "Error"
    if isinstance(v, ErrValue):
        return "Error"
    if isinstance(v, VBArray):
        return {"variant": "Variant", "string": "String", "long": "Long", "integer": "Integer",
                "double": "Double", "boolean": "Boolean", "date": "Date", "single": "Single",
                "byte": "Byte", "currency": "Currency", "object": "Object"}.get(v.typ, "Variant") + "()"
    if isinstance(v, VBObject):
        return v.cls.name
    if isinstance(v, UDTValue):
        return v.typedef.name
    if isinstance(v, Collection):
        return "Collection"
    name = getattr(v, "_vba_typename", None)
    if name:
        return name
    if current is not None:
        return current.host.typename(v)
    return type(v).__name__


def f_vartype(varname):
    v = varname
    t = type(v)
    if isinstance(v, VBArray):
        base = {"variant": 12, "string": 8, "long": 3, "integer": 2, "double": 5, "boolean": 11,
                "date": 7, "single": 4, "byte": 17, "currency": 6, "object": 9}.get(v.typ, 12)
        return 8192 + base
    if t is bool:
        return 11
    if t is int:
        return 2 if -32768 <= v <= 32767 else 3
    if t is float:
        return 5
    if t is str:
        return 8
    if t is VBDate:
        return 7
    if v is EMPTY:
        return 0
    if v is NULL:
        return 1
    if isinstance(v, ErrValue) or v is MISSING:
        return 10
    if isinstance(v, UDTValue):
        return 36
    return 9


def f_ubound(arrayname, dimension=1):
    if not isinstance(arrayname, VBArray):
        raise VBAError(13)
    d = to_long(dimension)
    if not arrayname.dims or d < 1 or d > len(arrayname.dims):
        raise VBAError(9)
    return arrayname.dims[d - 1][1]


def f_lbound(arrayname, dimension=1):
    if not isinstance(arrayname, VBArray):
        raise VBAError(13)
    d = to_long(dimension)
    if not arrayname.dims or d < 1 or d > len(arrayname.dims):
        raise VBAError(9)
    return arrayname.dims[d - 1][0]


def f_array(*arglist):
    return VBArray([(0, len(arglist) - 1)], data=list(arglist))


def f_iif(expression, truepart, falsepart):
    return truepart if truth(expression) else falsepart


def f_choose(index, *choice):
    i = to_long(index)
    if 1 <= i <= len(choice):
        return choice[i - 1]
    return NULL


def f_switch(*pairs):
    for i in range(0, len(pairs) - 1, 2):
        if truth(pairs[i]):
            return pairs[i + 1]
    return NULL


def f_rgb(red, green, blue):
    def part(x):
        return max(0, min(255, to_long(x)))
    return part(red) + part(green) * 256 + part(blue) * 65536


_QB = [0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
       0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF]


def f_qbcolor(color):
    return _QB[to_long(color) % 16]


# -- dates --
def _dt(v):
    return to_date(v).to_datetime()


def f_now():
    return VBDate.from_datetime(datetime.datetime.now().replace(microsecond=0))


def f_date():
    return VBDate(float(math.floor(f_now())))


def f_time():
    n = f_now()
    return VBDate(float(n) - math.floor(n))


def f_timer():
    now = datetime.datetime.now()
    return now.hour * 3600 + now.minute * 60 + now.second + now.microsecond / 1e6


def f_year(date):
    return _null_or(date, lambda v: _dt(v).year)


def f_month(date):
    return _null_or(date, lambda v: _dt(v).month)


def f_day(date):
    return _null_or(date, lambda v: _dt(v).day)


def f_hour(time):
    return _null_or(time, lambda v: _dt(v).hour)


def f_minute(time):
    return _null_or(time, lambda v: _dt(v).minute)


def f_second(time):
    return _null_or(time, lambda v: _dt(v).second)


def _weekday(dt, first):
    # Sunday is 1 when the week starts on Sunday.
    first = first or 1
    iso = dt.isoweekday() % 7 + 1       # Sunday 1 ... Saturday 7
    return (iso - first) % 7 + 1


def f_weekday(date, firstdayofweek=1):
    return _weekday(_dt(date), to_long(firstdayofweek))


_DAYS = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
_MONTH_NAMES = ["January", "February", "March", "April", "May", "June", "July", "August",
                "September", "October", "November", "December"]


def f_weekdayname(weekday, abbreviate=False, firstdayofweek=1):
    i = to_long(weekday)
    if i < 1 or i > 7:
        raise VBAError(5)
    first = to_long(firstdayofweek) or 1
    name = _DAYS[(i - 1 + first - 1) % 7]
    return name[:3] if to_bool(abbreviate) else name


def f_monthname(month, abbreviate=False):
    i = to_long(month)
    if i < 1 or i > 12:
        raise VBAError(5)
    name = _MONTH_NAMES[i - 1]
    return name[:3] if to_bool(abbreviate) else name


def _serial(y, m, d):
    y = to_long(y)
    m = to_long(m)
    d = to_long(d)
    if 0 <= y < 30:
        y += 2000
    elif 30 <= y < 100:
        y += 1900
    y += (m - 1) // 12
    m = (m - 1) % 12 + 1
    base = datetime.datetime(y, m, 1)
    return float((base - _EPOCH).days + d - 1)


def f_dateserial(year, month, day):
    try:
        return VBDate(_serial(year, month, day))
    except (ValueError, OverflowError):
        raise VBAError(5)


def f_timeserial(hour, minute, second):
    secs = to_long(hour) * 3600 + to_long(minute) * 60 + to_long(second)
    return VBDate(secs / 86400.0)


def f_datevalue(date):
    d = to_date(date)
    return VBDate(float(math.floor(d)))


def f_timevalue(time):
    d = to_date(time)
    return VBDate(float(d) - math.floor(d))


def _add_months(dt, months):
    y = dt.year + (dt.month - 1 + months) // 12
    m = (dt.month - 1 + months) % 12 + 1
    last = (datetime.datetime(y + (m // 12), m % 12 + 1, 1) - datetime.timedelta(days=1)).day
    return dt.replace(year=y, month=m, day=min(dt.day, last))


def f_dateadd(interval, number, date):
    iv = to_str(interval).lower()
    n = to_double(number)
    dt = _dt(date)
    if iv == "yyyy":
        dt = _add_months(dt, int(n) * 12)
    elif iv == "q":
        dt = _add_months(dt, int(n) * 3)
    elif iv == "m":
        dt = _add_months(dt, int(n))
    elif iv in ("y", "d", "w"):
        dt = dt + datetime.timedelta(days=int(n))
    elif iv == "ww":
        dt = dt + datetime.timedelta(weeks=int(n))
    elif iv == "h":
        dt = dt + datetime.timedelta(hours=int(n))
    elif iv == "n":
        dt = dt + datetime.timedelta(minutes=int(n))
    elif iv == "s":
        dt = dt + datetime.timedelta(seconds=int(n))
    else:
        raise VBAError(5)
    return VBDate.from_datetime(dt)


def f_datediff(interval, date1, date2, firstdayofweek=1, firstweekofyear=1):
    iv = to_str(interval).lower()
    a, b = _dt(date1), _dt(date2)
    if iv == "yyyy":
        return b.year - a.year
    if iv == "q":
        return (b.year - a.year) * 4 + (b.month - 1) // 3 - (a.month - 1) // 3
    if iv == "m":
        return (b.year - a.year) * 12 + b.month - a.month
    if iv in ("y", "d"):
        return (b.date() - a.date()).days
    if iv == "w":
        return int((b.date() - a.date()).days / 7)
    if iv == "ww":
        first = to_long(firstdayofweek) or 1

        def week_start(dt):
            return dt.date() - datetime.timedelta(days=_weekday(dt, first) - 1)
        return (week_start(b) - week_start(a)).days // 7
    if iv == "h":
        return int((b.replace(minute=0, second=0) - a.replace(minute=0, second=0)).total_seconds() // 3600)
    if iv == "n":
        return int((b.replace(second=0) - a.replace(second=0)).total_seconds() // 60)
    if iv == "s":
        return int((b - a).total_seconds())
    raise VBAError(5)


def f_datepart(interval, date, firstdayofweek=1, firstweekofyear=1):
    iv = to_str(interval).lower()
    dt = _dt(date)
    if iv == "yyyy":
        return dt.year
    if iv == "q":
        return (dt.month - 1) // 3 + 1
    if iv == "m":
        return dt.month
    if iv == "y":
        return dt.timetuple().tm_yday
    if iv == "d":
        return dt.day
    if iv == "w":
        return _weekday(dt, to_long(firstdayofweek))
    if iv == "ww":
        first = to_long(firstdayofweek) or 1
        jan1 = dt.replace(month=1, day=1)
        offset = _weekday(jan1, first) - 1
        return (dt.timetuple().tm_yday + offset - 1) // 7 + 1
    if iv == "h":
        return dt.hour
    if iv == "n":
        return dt.minute
    if iv == "s":
        return dt.second
    raise VBAError(5)


# -- Format --

_NAMED_FORMATS = {
    "general number": None, "currency": "$#,##0.00;($#,##0.00)", "fixed": "0.00",
    "standard": "#,##0.00", "percent": "0.00%", "scientific": "0.00E+00",
    "long date": "dddd, mmmm d, yyyy", "medium date": "d-mmm-yy", "short date": "m/d/yyyy",
    "long time": "h:mm:ss AM/PM", "medium time": "h:mm AM/PM", "short time": "hh:mm",
    "general date": None,
}


def _format_sections(fmt):
    """The ; sections of a format, quotes and escapes respected."""
    out, cur, i = [], [], 0
    while i < len(fmt):
        c = fmt[i]
        if c == '"':
            j = fmt.find('"', i + 1)
            j = len(fmt) if j < 0 else j
            cur.append(fmt[i:j + 1])
            i = j + 1
            continue
        if c == "\\" and i + 1 < len(fmt):
            cur.append(fmt[i:i + 2])
            i += 2
            continue
        if c == ";":
            out.append("".join(cur))
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    out.append("".join(cur))
    return out


_DATE_TOKEN = re.compile(r'"[^"]*"|\\.|dddddd|ddddd|dddd|ddd|dd|d|ww|w|mmmm|mmm|mm|m|q|yyyy|yy|y|'
                         r'hh|h|nn|n|ss|s|ttttt|AM/PM|am/pm|A/P|a/p|AMPM|c|.', re.IGNORECASE)


def format_date(value, fmt):
    """A date in one of Visual Basic's date formats."""
    d = to_date(value)
    dt = d.to_datetime()
    tokens = _DATE_TOKEN.findall(fmt)
    twelve = any(t.lower() in ("am/pm", "a/p", "ampm") for t in tokens)
    out = []
    for i, tok in enumerate(tokens):
        low = tok.lower()
        if tok.startswith('"'):
            out.append(tok[1:-1])
        elif tok.startswith("\\"):
            out.append(tok[1:])
        elif low == "dddddd":
            out.append(format_date(d, "dddd, mmmm d, yyyy"))
        elif low == "ddddd":
            out.append(format_date(d, "m/d/yyyy"))
        elif low == "dddd":
            out.append(_DAYS[dt.isoweekday() % 7])
        elif low == "ddd":
            out.append(_DAYS[dt.isoweekday() % 7][:3])
        elif low == "dd":
            out.append("%02d" % dt.day)
        elif low == "d":
            out.append(str(dt.day))
        elif low == "ww":
            out.append(str(f_datepart("ww", d)))
        elif low == "w":
            out.append(str(_weekday(dt, 1)))
        elif low in ("mmmm", "mmm", "mm", "m"):
            # After an hour, or before a second, m is the minute.
            prev = next((t.lower() for t in reversed(tokens[:i]) if t.strip() and not t.startswith('"')
                         and t not in (":", " ", "/", "-", ".", ",")), "")
            nxt = next((t.lower() for t in tokens[i + 1:] if t.strip() and not t.startswith('"')
                        and t not in (":", " ", "/", "-", ".", ",")), "")
            if low in ("mm", "m") and (prev in ("h", "hh") or nxt in ("s", "ss")):
                out.append("%02d" % dt.minute if low == "mm" else str(dt.minute))
            elif low == "mmmm":
                out.append(_MONTH_NAMES[dt.month - 1])
            elif low == "mmm":
                out.append(_MONTH_NAMES[dt.month - 1][:3])
            elif low == "mm":
                out.append("%02d" % dt.month)
            else:
                out.append(str(dt.month))
        elif low == "q":
            out.append(str((dt.month - 1) // 3 + 1))
        elif low == "yyyy":
            out.append("%04d" % dt.year)
        elif low == "yy":
            out.append("%02d" % (dt.year % 100))
        elif low == "y":
            out.append(str(dt.timetuple().tm_yday))
        elif low in ("hh", "h"):
            h = dt.hour
            if twelve:
                h = h % 12 or 12
            out.append("%02d" % h if low == "hh" else str(h))
        elif low == "nn":
            out.append("%02d" % dt.minute)
        elif low == "n":
            out.append(str(dt.minute))
        elif low == "ss":
            out.append("%02d" % dt.second)
        elif low == "s":
            out.append(str(dt.second))
        elif low == "ttttt":
            out.append(time_text(dt))
        elif low == "am/pm":
            text = "AM" if dt.hour < 12 else "PM"
            out.append(text if tok[0] == "A" else text.lower())
        elif low == "ampm":
            out.append("AM" if dt.hour < 12 else "PM")
        elif low == "a/p":
            text = "A" if dt.hour < 12 else "P"
            out.append(text if tok[0] in "AP" else text.lower())
        elif low == "c":
            out.append(date_text(d))
        else:
            out.append(tok)
    return "".join(out)


def _looks_like_date_format(fmt):
    stripped = re.sub(r'"[^"]*"|\\.', "", fmt)
    return re.search(r"[dyhnsqw]|m{3,}|am/pm|a/p", stripped, re.IGNORECASE) is not None and \
        not re.search(r"[0#]", stripped)


def format_number(n, fmt):
    """A number in a format of 0, #, commas, a point, %, E+ and text:
    what Format does without a program to ask."""
    sections = _format_sections(fmt)
    if n < 0 and len(sections) > 1 and sections[1] != "":
        sec, n, neg = sections[1], -n, False
    elif n == 0 and len(sections) > 2 and sections[2] != "":
        sec, neg = sections[2], False
    else:
        sec, neg = sections[0], n < 0
        n = abs(n)
    # Pull the literal text apart from the digits' pattern.
    parts = []
    i = 0
    while i < len(sec):
        c = sec[i]
        if c == '"':
            j = sec.find('"', i + 1)
            j = len(sec) if j < 0 else j
            parts.append(("lit", sec[i + 1:j]))
            i = j + 1
        elif c == "\\" and i + 1 < len(sec):
            parts.append(("lit", sec[i + 1]))
            i += 2
        elif c in "0#.,%":
            j = i
            while j < len(sec) and sec[j] in "0#.,%":
                j += 1
            k = j
            if k < len(sec) and sec[k] in "eE" and k + 1 < len(sec) and sec[k + 1] in "+-":
                k += 2
                while k < len(sec) and sec[k] in "0#":
                    k += 1
            parts.append(("num", sec[i:k]))
            i = k
        else:
            parts.append(("lit", c))
            i += 1
    pattern = "".join(p[1] for p in parts if p[0] == "num")
    if not pattern:
        text = "".join(p[1] for p in parts)
        return ("-" if neg else "") + text
    percent = pattern.count("%")
    n = n * (100 ** percent)
    pattern = pattern.replace("%", "")
    exp_part = None
    m = re.search(r"[eE][+-]", pattern)
    if m:
        exp_part = pattern[m.start():]
        pattern = pattern[:m.start()]
    # Trailing commas scale by a thousand each.
    while pattern.endswith(","):
        n /= 1000.0
        pattern = pattern[:-1]
    int_pat, _, dec_pat = pattern.partition(".")
    grouping = "," in int_pat
    int_pat = int_pat.replace(",", "")
    decimals = len(dec_pat)
    min_int = int_pat.count("0")
    if exp_part is not None:
        if n == 0:
            e = 0
        else:
            e = int(math.floor(math.log10(n)))
            e -= max(len(int_pat), 1) - 1
        mant = n / (10 ** e) if n else 0.0
        mant = round(mant, decimals)
        if mant >= 10 ** max(len(int_pat), 1):
            mant /= 10
            e += 1
        digits = ("%." + str(decimals) + "f") % mant
        exp_digits = exp_part.count("0") or 1
        sign = "-" if e < 0 else ("+" if "+" in exp_part else "")
        body = digits + exp_part[0] + sign + str(abs(e)).zfill(exp_digits)
    else:
        text = ("%." + str(decimals) + "f") % round(n + 0.0, decimals)
        ip, _, dp = text.partition(".")
        if dec_pat:
            # Optional decimals (#) drop their trailing zeroes.
            keep = len(dec_pat.rstrip("#"))
            while len(dp) > keep and dp.endswith("0"):
                dp = dp[:-1]
        if ip == "0" and min_int == 0:
            ip = ""
        ip = ip.zfill(min_int)
        if grouping and ip:
            ip = "{:,}".format(int(ip))
        body = ip + ("." + dp if dec_pat else "")
        if dec_pat and not dp and "." in pattern:
            body = ip + "."
    out = []
    placed = False
    for kind, text in parts:
        if kind == "lit":
            out.append(text)
        elif not placed:
            out.append(body + "%" * percent)
            placed = True
    result = "".join(out)
    return ("-" + result) if neg and n != 0 else result


def f_format(expression, format=MISSING, firstdayofweek=1, firstweekofyear=1):
    v = expression
    if v is NULL:
        return NULL
    if v is EMPTY:
        return ""
    fmt = "" if format is MISSING else to_str(format)
    low = fmt.lower()
    if low in ("yes/no", "true/false", "on/off"):
        b = to_bool(v)
        words = low.split("/")
        return (words[0] if b else words[1]).capitalize() if low != "on/off" else ("On" if b else "Off")
    if low in _NAMED_FORMATS:
        code = _NAMED_FORMATS[low]
        if code is None:
            if low == "general date":
                return date_text(to_date(v))
            return to_str(to_num(v)) if not isinstance(v, str) or parse_number(v) is not None else v
        fmt = code
    if fmt == "":
        if type(v) is VBDate:
            return date_text(v)
        return to_str(v)
    if isinstance(v, str) and parse_number(v) is None and parse_date(v) is None:
        return _format_text(v, fmt)
    if type(v) is VBDate or _looks_like_date_format(fmt):
        return format_date(v, fmt)
    if isinstance(v, str) and parse_number(v) is None:
        return format_date(v, fmt)
    n = to_double(v)
    if current is not None:
        text = current.host.format_number(fmt, n)
        if text is not None:
            return text
    return format_number(n, fmt)


def _format_text(s, fmt):
    """@ and & stand for the text's characters; < and > change its case."""
    if "<" in fmt:
        s = s.lower()
    if ">" in fmt:
        s = s.upper()
    places = [c for c in fmt if c in "@&"]
    if not places:
        return s
    out = []
    chars = list(s)
    n = len(places)
    if len(chars) < n:
        pad = n - len(chars)
        filled = [None] * pad + chars
    else:
        filled = chars[:len(chars) - n + 1] and [("".join(chars[:len(chars) - n + 1]))] + chars[len(chars) - n + 1:]
    k = 0
    for c in fmt:
        if c in "@&":
            ch = filled[k]
            k += 1
            out.append(ch if ch is not None else (" " if c == "@" else ""))
        elif c not in "<>!":
            out.append(c)
    return "".join(out)


def f_formatnumber(expression, numdigitsafterdecimal=-1, includeleadingdigit=-2, useparensfornegativenumbers=-2,
                   groupdigits=-2):
    d = to_long(numdigitsafterdecimal)
    if d < 0:
        d = 2
    group = to_long(groupdigits)
    code = ("#,##0" if group != 0 else "0") + ("." + "0" * d if d else "")
    if to_long(useparensfornegativenumbers) == -1:
        code = code + ";(" + code + ")"
    text = format_number(to_double(expression), code)
    if to_long(includeleadingdigit) == 0:
        text = text.replace("0.", ".", 1) if text.startswith("0.") or text.startswith("-0.") else text
    return text


def f_formatcurrency(expression, numdigitsafterdecimal=-1, includeleadingdigit=-2,
                     useparensfornegativenumbers=-2, groupdigits=-2):
    d = to_long(numdigitsafterdecimal)
    if d < 0:
        d = 2
    code = "$#,##0" + ("." + "0" * d if d else "")
    return format_number(to_double(expression), code + ";(" + code + ")")


def f_formatpercent(expression, numdigitsafterdecimal=-1, includeleadingdigit=-2,
                    useparensfornegativenumbers=-2, groupdigits=-2):
    d = to_long(numdigitsafterdecimal)
    if d < 0:
        d = 2
    return format_number(to_double(expression), "0" + ("." + "0" * d if d else "") + "%")


def f_formatdatetime(date, namedformat=0):
    n = to_long(namedformat)
    d = to_date(date)
    return {0: lambda: date_text(d), 1: lambda: format_date(d, "dddd, mmmm d, yyyy"),
            2: lambda: format_date(d, "m/d/yyyy"), 3: lambda: format_date(d, "h:mm:ss AM/PM"),
            4: lambda: format_date(d, "hh:mm")}.get(n, lambda: date_text(d))()


# -- interaction --
def f_msgbox(prompt, buttons=0, title=MISSING, helpfile=MISSING, context=MISSING):
    text = to_str(plain_value(prompt)) if not isinstance(prompt, VBArray) else ""
    t = "Microsoft Excel" if title is MISSING else to_str(title)
    return current.host.msgbox(text, to_long(buttons), t)


def f_inputbox(prompt, title=MISSING, default="", xpos=MISSING, ypos=MISSING, helpfile=MISSING,
               context=MISSING):
    answer = current.host.inputbox(to_str(prompt), "Microsoft Excel" if title is MISSING else to_str(title),
                                   to_str(default))
    return "" if answer is None else answer


def f_beep():
    return EMPTY


def f_doevents():
    if current is not None:
        current.host.do_events()
    return 0


def f_environ(expression):
    if isinstance(expression, str):
        return os.environ.get(expression, "")
    i = to_long(expression)
    items = sorted(os.environ.items())
    if 1 <= i <= len(items):
        return "%s=%s" % items[i - 1]
    return ""


def f_command():
    return ""


def f_shell(pathname, windowstyle=MISSING):
    raise VBAError(5, "Shell is not available: office42 does not start programs from a macro")


def f_sendkeys(string, wait=MISSING):
    raise VBAError(5, "SendKeys is not available in office42")


def f_appactivate(title, wait=MISSING):
    raise VBAError(5, "AppActivate is not available in office42")


def f_error(errornumber=MISSING):
    if errornumber is MISSING:
        return current.err.Description if current is not None else ""
    return ERROR_TEXT.get(to_long(errornumber), "Application-defined or object-defined error")


def f_callbyname(object, procname, calltype, *args):
    p = current
    kind = to_long(calltype)
    name = to_str(procname).lower()
    vargs = [ValueArg(a) for a in args]
    if kind == 4 or kind == 8:      # vbLet, vbSet
        if not args:
            raise VBAError(449)
        p.member_let(object, name, [ValueArg(a) for a in args[:-1]], args[-1], kind == 8, None)
        return EMPTY
    return p.member_get(object, name, vargs, None)


def f_createobject(classname, servername=MISSING):
    progid = to_str(classname).lower()
    if progid == "scripting.dictionary":
        return Dictionary()
    if progid == "scripting.filesystemobject":
        return FileSystemObject()
    if progid == "vbscript.regexp":
        return RegExp()
    if progid in ("collection", "vba.collection"):
        return Collection()
    if progid in LIBRARY_CLASSES:
        return LIBRARY_CLASSES[progid]()
    obj = current.host.create_object(progid) if current is not None else None
    if obj is None:
        raise VBAError(429, "ActiveX component can't create object: %s" % classname)
    return obj


def f_getobject(pathname=MISSING, classname=MISSING):
    return current.host.get_object(None if pathname is MISSING else to_str(pathname),
                                   None if classname is MISSING else to_str(classname))


def f_objptr(obj):
    return id(obj)


def f_savesetting(appname, section, key, setting):
    current.settings[(to_str(appname).lower(), to_str(section).lower(), to_str(key).lower())] = to_str(setting)


def f_getsetting(appname, section, key, default=""):
    return current.settings.get((to_str(appname).lower(), to_str(section).lower(), to_str(key).lower()),
                                to_str(default))


def f_deletesetting(appname, section=MISSING, key=MISSING):
    a = to_str(appname).lower()
    for k in list(current.settings):
        if k[0] == a and (section is MISSING or k[1] == to_str(section).lower()) and \
                (key is MISSING or k[2] == to_str(key).lower()):
            del current.settings[k]


# -- files --
def f_freefile(rangenumber=0):
    n = 1 if to_long(rangenumber) == 0 else 256
    while n in current.files:
        n += 1
    return n


def f_eof(filenumber):
    return current.file(filenumber).eof()


def f_lof(filenumber):
    return current.file(filenumber).size()


def f_loc(filenumber):
    f = current.file(filenumber)
    return f.pos if f.text is not None else 0


def f_input(number, filenumber):
    return current.file(filenumber).read_chars(to_long(number))


def f_filelen(pathname):
    try:
        return os.path.getsize(to_str(pathname))
    except OSError:
        raise VBAError(53)


def f_filedatetime(pathname):
    try:
        t = os.path.getmtime(to_str(pathname))
    except OSError:
        raise VBAError(53)
    return VBDate.from_datetime(datetime.datetime.fromtimestamp(int(t)))


_dir_state = {"items": [], "pos": 0}


def f_dir(pathname=MISSING, attributes=0):
    """Dir: the first file matching a pattern, then with no argument the
    next, and "" at the end."""
    import fnmatch
    if pathname is MISSING:
        st = _dir_state
        if st["pos"] >= len(st["items"]):
            return ""
        st["pos"] += 1
        return st["items"][st["pos"] - 1]
    path = to_str(pathname)
    attrs = to_long(attributes)
    if path == "":
        path = "*"
    folder, pattern = os.path.split(path)
    if os.path.isdir(path) and not any(c in path for c in "*?"):
        folder, pattern = path, "*"
        if attrs & 16 == 0:
            _dir_state["items"], _dir_state["pos"] = [], 0
            return ""
    folder = folder or "."
    try:
        names = sorted(os.listdir(folder))
    except OSError:
        names = []
    items = []
    for name in names:
        full = os.path.join(folder, name)
        is_dir = os.path.isdir(full)
        if is_dir and not attrs & 16:
            continue
        if fnmatch.fnmatch(name.lower(), pattern.lower()) or (pattern == "*.*" and True):
            items.append(name)
    _dir_state["items"], _dir_state["pos"] = items, 0
    return f_dir()


def f_kill(pathname):
    import glob
    path = to_str(pathname)
    matches = glob.glob(path) if any(c in path for c in "*?") else [path]
    if not matches or not all(os.path.exists(m) for m in matches):
        raise VBAError(53)
    for m in matches:
        os.remove(m)


def f_mkdir(path):
    os.mkdir(to_str(path))


def f_rmdir(path):
    os.rmdir(to_str(path))


def f_chdir(path):
    os.chdir(to_str(path))


def f_chdrive(drive):
    return EMPTY


def f_curdir(drive=MISSING):
    return os.getcwd()


def f_filecopy(source, destination):
    import shutil
    shutil.copyfile(to_str(source), to_str(destination))


def f_getattr(pathname):
    p = to_str(pathname)
    if not os.path.exists(p):
        raise VBAError(53)
    return 16 if os.path.isdir(p) else 32


def f_setattr(pathname, attributes):
    return EMPTY


# ---- Objects the language makes itself --------------------------------------------

class Dictionary:
    """Scripting.Dictionary: keys to items, the keys of any type, text
    ones ignoring case when CompareMode is vbTextCompare."""

    _vba_typename = "Dictionary"

    def __init__(self):
        self._d = {}
        self._compare = 0

    def _key(self, key):
        if isinstance(key, str):
            return ("s", key.lower() if self._compare == 1 else key)
        if type(key) is bool:
            return ("n", -1 if key else 0)
        if isinstance(key, (int, float)):
            return ("n", float(key))
        if key is EMPTY:
            return ("e", None)
        return ("o", id(key))

    @property
    def CompareMode(self):
        return self._compare

    @CompareMode.setter
    def CompareMode(self, value):
        if self._d:
            raise VBAError(5)
        self._compare = to_long(value)

    @property
    def Count(self):
        return len(self._d)

    def Add(self, Key, Item):
        k = self._key(Key)
        if k in self._d:
            raise VBAError(457)
        self._d[k] = [Key, Item]

    def Exists(self, Key):
        return self._key(Key) in self._d

    def Item(self, Key):
        k = self._key(Key)
        if k not in self._d:
            # Asking for a key that is not there adds it, empty.
            self._d[k] = [Key, EMPTY]
        return self._d[k][1]

    def let_Item(self, value, Key):
        k = self._key(Key)
        if k in self._d:
            self._d[k][1] = value
        else:
            self._d[k] = [Key, value]

    def let_Key(self, value, Key):
        k = self._key(Key)
        if k not in self._d:
            raise VBAError(5)
        item = self._d.pop(k)[1]
        self._d[self._key(value)] = [value, item]

    def Keys(self):
        keys = [e[0] for e in self._d.values()]
        return VBArray([(0, len(keys) - 1)], data=keys)

    def Items(self):
        items = [e[1] for e in self._d.values()]
        return VBArray([(0, len(items) - 1)], data=items)

    def Remove(self, Key):
        k = self._key(Key)
        if k not in self._d:
            raise VBAError(5)
        del self._d[k]

    def RemoveAll(self):
        self._d.clear()

    def _vba_default_get(self, *args):
        if not args:
            raise VBAError(450)
        return self.Item(args[0])

    def _vba_default_let(self, value, *args):
        if not args:
            raise VBAError(450)
        self.let_Item(value, args[0])

    def _vba_iter(self):
        return [e[0] for e in self._d.values()]


class TextStream:
    _vba_typename = "TextStream"

    def __init__(self, path, mode, create=False):
        self._mode = mode
        self._path = path
        if mode == 1:
            self._file = VBFile(path, "input")
        else:
            if not create and not os.path.exists(path) and mode == 8:
                pass
            self._file = VBFile(path, "output" if mode == 2 else "append")

    @property
    def AtEndOfStream(self):
        return self._file.eof()

    @property
    def AtEndOfLine(self):
        f = self._file
        return f.eof() or f.text[f.pos] in "\r\n"

    def ReadLine(self):
        return self._file.read_line()

    def ReadAll(self):
        f = self._file
        text = f.text[f.pos:]
        f.pos = len(f.text)
        return text

    def Read(self, Characters):
        return self._file.read_chars(min(to_long(Characters), len(self._file.text) - self._file.pos))

    def SkipLine(self):
        self._file.read_line()

    def Write(self, Text):
        self._file.write(to_str(Text))

    def WriteLine(self, Text=""):
        self._file.write(to_str(Text) + "\r\n")

    def WriteBlankLines(self, Lines):
        self._file.write("\r\n" * to_long(Lines))

    def Close(self):
        self._file.close()


class FSFile:
    _vba_typename = "File"

    def __init__(self, path):
        self._path = os.path.abspath(path)

    @property
    def Name(self):
        return os.path.basename(self._path)

    @property
    def Path(self):
        return self._path

    @property
    def Size(self):
        return os.path.getsize(self._path)

    @property
    def DateLastModified(self):
        return VBDate.from_datetime(datetime.datetime.fromtimestamp(int(os.path.getmtime(self._path))))

    @property
    def ParentFolder(self):
        return FSFolder(os.path.dirname(self._path))

    def _vba_default_get(self, *args):
        return self._path


class FSFolder(FSFile):
    _vba_typename = "Folder"

    @property
    def Files(self):
        items = [FSFile(os.path.join(self._path, n)) for n in sorted(os.listdir(self._path))
                 if os.path.isfile(os.path.join(self._path, n))]
        c = Collection()
        for f in items:
            c.Add(f, f.Name)
        return c

    @property
    def SubFolders(self):
        c = Collection()
        for n in sorted(os.listdir(self._path)):
            if os.path.isdir(os.path.join(self._path, n)):
                c.Add(FSFolder(os.path.join(self._path, n)), n)
        return c


class FileSystemObject:
    """Scripting.FileSystemObject, as much of it as macros use."""

    _vba_typename = "FileSystemObject"

    def FileExists(self, FileSpec):
        return os.path.isfile(to_str(FileSpec))

    def FolderExists(self, FolderSpec):
        return os.path.isdir(to_str(FolderSpec))

    def GetFileName(self, Path):
        return os.path.basename(to_str(Path))

    def GetBaseName(self, Path):
        return os.path.splitext(os.path.basename(to_str(Path)))[0]

    def GetExtensionName(self, Path):
        return os.path.splitext(to_str(Path))[1].lstrip(".")

    def GetParentFolderName(self, Path):
        return os.path.dirname(to_str(Path))

    def GetAbsolutePathName(self, Path):
        return os.path.abspath(to_str(Path))

    def BuildPath(self, Path, Name):
        return os.path.join(to_str(Path), to_str(Name))

    def CreateFolder(self, Path):
        os.mkdir(to_str(Path))
        return FSFolder(to_str(Path))

    def DeleteFile(self, FileSpec, Force=False):
        f_kill(FileSpec)

    def DeleteFolder(self, FolderSpec, Force=False):
        import shutil
        shutil.rmtree(to_str(FolderSpec))

    def CopyFile(self, Source, Destination, OverWriteFiles=True):
        import shutil
        dst = to_str(Destination)
        if os.path.exists(dst) and not os.path.isdir(dst) and not to_bool(OverWriteFiles):
            raise VBAError(58)
        shutil.copy(to_str(Source), dst)

    def MoveFile(self, Source, Destination):
        import shutil
        shutil.move(to_str(Source), to_str(Destination))

    def GetFile(self, FilePath):
        p = to_str(FilePath)
        if not os.path.isfile(p):
            raise VBAError(53)
        return FSFile(p)

    def GetFolder(self, FolderPath):
        p = to_str(FolderPath)
        if not os.path.isdir(p):
            raise VBAError(76)
        return FSFolder(p)

    def OpenTextFile(self, FileName, IOMode=1, Create=False, Format=0):
        return TextStream(to_str(FileName), to_long(IOMode), to_bool(Create))

    def CreateTextFile(self, FileName, Overwrite=True, Unicode=False):
        p = to_str(FileName)
        if os.path.exists(p) and not to_bool(Overwrite):
            raise VBAError(58)
        return TextStream(p, 2, True)

    def GetTempName(self):
        return "rad%05X.tmp" % random.randint(0, 0xFFFFF)


class RegExp:
    """VBScript.RegExp over Python's re: Pattern, Global, IgnoreCase and
    MultiLine; Test, Execute and Replace."""

    _vba_typename = "IRegExp2"

    def __init__(self):
        self.Pattern = ""
        self.Global = False
        self.IgnoreCase = False
        self.MultiLine = False

    def _rx(self):
        flags = (re.IGNORECASE if to_bool(self.IgnoreCase) else 0) | \
                (re.MULTILINE if to_bool(self.MultiLine) else 0)
        try:
            return re.compile(to_str(self.Pattern), flags)
        except re.error:
            raise VBAError(5017, "Syntax error in regular expression")

    def Test(self, SourceString):
        return self._rx().search(to_str(SourceString)) is not None

    def Execute(self, SourceString):
        s = to_str(SourceString)
        rx = self._rx()
        found = rx.finditer(s) if to_bool(self.Global) else ([rx.search(s)] if rx.search(s) else [])
        return MatchCollection([Match(m) for m in found])

    def Replace(self, SourceString, ReplaceString):
        s = to_str(SourceString)
        rep = re.sub(r"\$(\d+)", r"\\g<\1>", to_str(ReplaceString).replace("\\", "\\\\")).replace("$&", r"\g<0>")
        return self._rx().sub(rep, s, count=0 if to_bool(self.Global) else 1)


class Match:
    _vba_typename = "IMatch2"

    def __init__(self, m):
        self._m = m

    @property
    def Value(self):
        return self._m.group(0)

    @property
    def FirstIndex(self):
        return self._m.start()

    @property
    def Length(self):
        return len(self._m.group(0))

    @property
    def SubMatches(self):
        groups = [g if g is not None else EMPTY for g in self._m.groups()]
        return MatchCollection(groups, base=0)

    def _vba_default_get(self, *args):
        return self.Value


class MatchCollection:
    _vba_typename = "IMatchCollection2"

    def __init__(self, items, base=0):
        self._items = list(items)

    @property
    def Count(self):
        return len(self._items)

    def Item(self, Index):
        i = to_long(Index)
        if i < 0 or i >= len(self._items):
            raise VBAError(5)
        return self._items[i]

    def _vba_default_get(self, *args):
        return self.Item(args[0])

    def _vba_iter(self):
        return list(self._items)


# The functions that take an object as it is, rather than its value.
_TAKES_OBJECTS = {f_isobject, f_typename, f_iif, f_array, f_choose, f_switch, f_callbyname,
                  f_objptr, f_ismissing, f_vartype, f_isarray, f_createobject, f_getobject}

LIBRARY = {}
for _name, _fn in list(globals().items()):
    if _name.startswith("f_") and callable(_fn):
        LIBRARY[_name[2:]] = _fn
LIBRARY.update({
    "leftb": f_left, "rightb": f_right, "midb": f_mid, "lenb": f_len, "instrb": f_instr,
    "ascb": f_asc, "chrb": f_chr, "cvdate": f_cdate, "clngptr": f_clnglng, "cdec": f_cdbl,
    "strptr": f_objptr, "varptr": f_objptr,
})
del _name, _fn


CONSTANTS = {
    "vbcrlf": "\r\n", "vbcr": "\r", "vblf": "\n", "vbnewline": "\r\n", "vbtab": "\t",
    "vbnullstring": "", "vbnullchar": "\0", "vbback": "\b", "vbformfeed": "\f",
    "vbverticaltab": "\v", "vbobjecterror": -2147221504,
    "vbokonly": 0, "vbokcancel": 1, "vbabortretryignore": 2, "vbyesnocancel": 3, "vbyesno": 4,
    "vbretrycancel": 5, "vbcritical": 16, "vbquestion": 32, "vbexclamation": 48,
    "vbinformation": 64, "vbdefaultbutton1": 0, "vbdefaultbutton2": 256,
    "vbdefaultbutton3": 512, "vbdefaultbutton4": 768, "vbapplicationmodal": 0,
    "vbsystemmodal": 4096, "vbmsgboxhelpbutton": 16384, "vbmsgboxsetforeground": 65536,
    "vbmsgboxright": 524288, "vbmsgboxrtlreading": 1048576,
    "vbok": 1, "vbcancel": 2, "vbabort": 3, "vbretry": 4, "vbignore": 5, "vbyes": 6, "vbno": 7,
    "vbbinarycompare": 0, "vbtextcompare": 1, "vbdatabasecompare": 2,
    "vbusesystemdayofweek": 0, "vbsunday": 1, "vbmonday": 2, "vbtuesday": 3, "vbwednesday": 4,
    "vbthursday": 5, "vbfriday": 6, "vbsaturday": 7, "vbusesystem": 0, "vbfirstjan1": 1,
    "vbfirstfourdays": 2, "vbfirstfullweek": 3,
    "vbgeneraldate": 0, "vblongdate": 1, "vbshortdate": 2, "vblongtime": 3, "vbshorttime": 4,
    "vbempty": 0, "vbnull": 1, "vbinteger": 2, "vblong": 3, "vbsingle": 4, "vbdouble": 5,
    "vbcurrency": 6, "vbdate": 7, "vbstring": 8, "vbobject": 9, "vberror": 10, "vbboolean": 11,
    "vbvariant": 12, "vbdataobject": 13, "vbdecimal": 14, "vbbyte": 17, "vblonglong": 20,
    "vbuserdefinedtype": 36, "vbarray": 8192,
    "vbuppercase": 1, "vblowercase": 2, "vbpropercase": 3, "vbwide": 4, "vbnarrow": 8,
    "vbunicode": 64, "vbfromunicode": 128,
    "vbblack": 0x0, "vbred": 0xFF, "vbgreen": 0xFF00, "vbyellow": 0xFFFF, "vbblue": 0xFF0000,
    "vbmagenta": 0xFF00FF, "vbcyan": 0xFFFF00, "vbwhite": 0xFFFFFF,
    "vbnormal": 0, "vbreadonly": 1, "vbhidden": 2, "vbsystem": 4, "vbvolume": 8,
    "vbdirectory": 16, "vbarchive": 32,
    "vbhide": 0, "vbnormalfocus": 1, "vbminimizedfocus": 2, "vbmaximizedfocus": 3,
    "vbnormalnofocus": 4, "vbminimizednofocus": 6,
    "vbget": 2, "vblet": 4, "vbmethod": 1, "vbset": 8,
    "vbtrue": -1, "vbfalse": 0, "vbusedefault": -2,
    "forreading": 1, "forwriting": 2, "forappending": 8,
    "tristatetrue": -1, "tristatefalse": 0, "tristateusedefault": -2,
}
