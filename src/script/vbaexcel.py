# vbaexcel.py - Excel's objects for Visual Basic, over the office42 module
#
# Copyright (C) 2026 The office42 authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# What a macro written for Excel finds when it says Range("A1"),
# ActiveSheet or Application.WorksheetFunction: Excel's object model, as
# its reference describes it, made of the calls the office42 module
# already makes on the book.  Rows and columns are counted from 1 here,
# as Visual Basic counts them, and from 0 in the calls underneath.  The
# language itself is vba.py's; this is the host it runs in, and what
# runs a book's macros, its functions for cells and its event
# procedures.  Compiled into the executable as a string; edit here,
# rebuild.

"""Excel's object model for the macros of an office42 book."""

import getpass
import io
import math
import os
import re
import sys
import time
from contextlib import redirect_stdout, redirect_stderr

import _office42 as _c
import office42
import o42vba as vba
from o42vba import (VBAError, EMPTY, NULL, MISSING, VBArray, VBDate, ErrValue,
                    to_str, to_long, to_double, to_bool, to_num, plain_value, is_object)

# What Visual Basic can reach: Excel's own limits, whatever the sheet
# holds beyond them.
MAX_ROWS = 1048576
MAX_COLS = 16384

ERROR_CODES = {"#NULL!": 2000, "#DIV/0!": 2007, "#VALUE!": 2015, "#REF!": 2023, "#NAME?": 2029,
               "#NUM!": 2036, "#N/A": 2042, "#GETTING_DATA": 2043, "#SPILL!": 2045, "#CALC!": 2050}
ERROR_NAMES = {v: k for k, v in ERROR_CODES.items()}

XL = {
    "xlup": -4162, "xldown": -4121, "xltoleft": -4159, "xltoright": -4161,
    "xldisabled": 0, "xlinterrupt": 1, "xlerrorhandler": 2,
    "xlshiftup": -4162, "xlshifttoleft": -4159, "xlshiftdown": -4121, "xlshifttoright": -4161,
    "xlpasteall": -4104, "xlpastevalues": -4163, "xlpasteformats": -4122, "xlpasteformulas": -4123,
    "xlpastecomments": -4144, "xlpastevaluesandnumberformats": 12, "xlpastecolumnwidths": 8,
    "xlpasteallexceptborders": 7, "xlpasteformulasandnumberformats": 11,
    "xlpasteallusingsourcetheme": 13, "xlpastevalidation": 6, "xlpastespecialoperationnone": -4142,
    "xlpastespecialoperationadd": 2, "xlpastespecialoperationsubtract": 3,
    "xlpastespecialoperationmultiply": 4, "xlpastespecialoperationdivide": 5,
    "xlascending": 1, "xldescending": 2, "xlyes": 1, "xlno": 2, "xlguess": 0,
    "xlsortonvalues": 0, "xlsortnormal": 0, "xlsorttextasnumbers": 1, "xltoptobottom": 1,
    "xllefttoright": 2, "xlpinyin": 1, "xlstroke": 2, "xlsortrows": 2, "xlsortcolumns": 1,
    "xlwhole": 1, "xlpart": 2, "xlvalues": -4163, "xlformulas": -4123, "xlcomments": -4144,
    "xlbyrows": 1, "xlbycolumns": 2, "xlnext": 1, "xlprevious": 2,
    "xlcelltypeblanks": 4, "xlcelltypeconstants": 2, "xlcelltypeformulas": -4123,
    "xlcelltypelastcell": 11, "xlcelltypevisible": 12, "xlcelltypecomments": -4144,
    "xlcelltypeallformatconditions": -4172, "xlcelltypeallvalidation": -4174,
    "xlnumbers": 1, "xltextvalues": 2, "xllogical": 4, "xlerrors": 16,
    "xlerrdiv0": 2007, "xlerrna": 2042, "xlerrname": 2029, "xlerrnull": 2000, "xlerrnum": 2036,
    "xlerrref": 2023, "xlerrvalue": 2015,
    "xlcalculationautomatic": -4105, "xlcalculationmanual": -4135, "xlcalculationsemiautomatic": 2,
    "xlsheetvisible": -1, "xlsheethidden": 0, "xlsheetveryhidden": 2,
    "xlleft": -4131, "xlright": -4152, "xlcenter": -4108, "xlgeneral": 1, "xljustify": -4130,
    "xltop": -4160, "xlbottom": -4107, "xlcenteracrossselection": 7, "xldistributed": -4117,
    "xlfill": 5, "xlhalignleft": -4131, "xlhalignright": -4152, "xlhaligncenter": -4108,
    "xlhaligngeneral": 1, "xlvaligntop": -4160, "xlvaligncenter": -4108, "xlvalignbottom": -4107,
    "xlnone": -4142, "xlsolid": 1, "xlautomatic": -4105, "xlgray16": 17, "xlgray8": 18,
    "xlgray25": -4124, "xlgray50": -4125, "xlgray75": -4126, "xlchecker": 9, "xlcrisscross": 16,
    "xlgrid": 15, "xlhorizontal": -4128, "xlvertical": -4166, "xldown_pattern": -4121,
    "xlcontinuous": 1, "xldash": -4115, "xldot": -4118, "xldouble": -4119, "xldashdot": 4,
    "xldashdotdot": 5, "xlslantdashdot": 13, "xllinestylenone": -4142,
    "xlthin": 2, "xlmedium": -4138, "xlthick": 4, "xlhairline": 1,
    "xledgeleft": 7, "xledgetop": 8, "xledgebottom": 9, "xledgeright": 10,
    "xlinsidevertical": 11, "xlinsidehorizontal": 12, "xldiagonaldown": 5, "xldiagonalup": 6,
    "xlunderlinestylenone": -4142, "xlunderlinestylesingle": 2, "xlunderlinestyledouble": -4119,
    "xlunderlinestylesingleaccounting": 4, "xlunderlinestyledoubleaccounting": 5,
    "xla1": 1, "xlr1c1": -4150, "xlabsolute": 1, "xlrelative": 4, "xlabsrowrelcolumn": 2,
    "xlrelrowabscolumn": 3,
    "xlworkbookdefault": 51, "xlopenxmlworkbook": 51, "xlopenxmlworkbookmacroenabled": 52,
    "xlexcel8": 56, "xlexcel12": 50, "xlcsv": 6, "xlcsvutf8": 62, "xlworkbooknormal": -4143,
    "xltypepdf": 0, "xlhtml": 44, "xlopendocumentspreadsheet": 60, "xltext": -4158,
    "xlcurrentplatformtext": -4158, "xlunicodetext": 42, "xltextwindows": 20,
    "xlworksheet": -4167, "xlchart": -4109,
    "xlfilldefault": 0, "xlfillcopy": 1, "xlfillseries": 2, "xlfillformats": 3, "xlfillvalues": 4,
    "xlfilldays": 5, "xlfillweekdays": 6, "xlfillmonths": 7, "xlfillyears": 8,
    "xllineartrend": 9, "xlgrowthtrend": 10,
    "xlvalidateinputonly": 0, "xlvalidatewholenumber": 1, "xlvalidatedecimal": 2,
    "xlvalidatelist": 3, "xlvalidatedate": 4, "xlvalidatetime": 5, "xlvalidatetextlength": 6,
    "xlvalidatecustom": 7, "xlvalidalertstop": 1, "xlvalidalertwarning": 2,
    "xlvalidalertinformation": 3,
    "xlbetween": 1, "xlnotbetween": 2, "xlequal": 3, "xlnotequal": 4, "xlgreater": 5, "xlless": 6,
    "xlgreaterequal": 7, "xllessequal": 8, "xlcellvalue": 1, "xlexpression": 2,
    "xland": 1, "xlor": 2, "xlfiltervalues": 7, "xltop10items": 3,
    "xlcolorindexautomatic": -4105, "xlcolorindexnone": -4142,
    "xlmaximized": -4137, "xlminimized": -4140, "xlnormal": -4143,
    "xlwait": 2, "xldefault": -4143, "xlnorthwestarrow": 1, "xlibeam": 3,
    "xllandscape": 2, "xlportrait": 1, "xlmanual": -4135, "xlcut": 2, "xlcopy": 1,
    "xlrows": 1, "xlcolumns": 2, "xllastcell": 11, "xlvisible": 12,
    "xlrangevaluedefault": 10, "xlrangevaluexmlspreadsheet": 11, "xlrangevaluemsperssistxml": 12,
    "xldecimalseparator": 3, "xlthousandsseparator": 4, "xllistseparator": 5,
    "xlcountrycode": 1, "xlcountrysetting": 2,
    "rgbblack": 0, "rgbwhite": 16777215, "rgbred": 255, "rgbgreen": 32768, "rgbblue": 16711680,
    "rgbyellow": 65535, "rgborange": 42495, "rgbgray": 8421504, "rgbgrey": 8421504,
    "rgbsilver": 12632256, "rgbpurple": 8388736, "rgbnavy": 8388608, "rgbmaroon": 128,
    "rgbolive": 32896, "rgbteal": 8421376, "rgblime": 65280, "rgbaqua": 16776960,
    "rgbfuchsia": 16711935, "rgbdarkgreen": 25600, "rgblightgray": 13882323,
    "msotrue": -1, "msofalse": 0, "msoctrue": 1,
    "xlmsgbox": 0,
}

# Excel's 56 colours, in ColorIndex order, as RGB.
PALETTE = [
    0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF,
    0x800000, 0x008000, 0x000080, 0x808000, 0x800080, 0x008080, 0xC0C0C0, 0x808080,
    0x9999FF, 0x993366, 0xFFFFCC, 0xCCFFFF, 0x660066, 0xFF8080, 0x0066CC, 0xCCCCFF,
    0x000080, 0xFF00FF, 0xFFFF00, 0x00FFFF, 0x800080, 0x800000, 0x008080, 0x0000FF,
    0x00CCFF, 0xCCFFFF, 0xCCFFCC, 0xFFFF99, 0x99CCFF, 0xFF99CC, 0xCC99FF, 0xFFCC99,
    0x3366FF, 0x33CCCC, 0x99CC00, 0xFFCC00, 0xFF9900, 0xFF6600, 0x666699, 0x969696,
    0x003366, 0x339966, 0x003300, 0x333300, 0x993300, 0x993366, 0x333399, 0x333333,
]


def vb_colour(v):
    """A colour as Visual Basic writes one -- red in the low byte -- as
    office42 writes one, 0xRRGGBB."""
    n = to_long(v, "longlong") & 0xFFFFFF
    return ((n & 0xFF) << 16) | (n & 0xFF00) | ((n >> 16) & 0xFF)


def from_colour(rgb):
    if rgb is None:
        return 16777215
    if isinstance(rgb, str):
        rgb = int(rgb.lstrip("#"), 16)
    rgb = int(rgb) & 0xFFFFFF
    return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF)


def colour_index(rgb):
    """The ColorIndex nearest a colour."""
    if rgb is None:
        return -4142
    rgb = int(rgb.lstrip("#"), 16) if isinstance(rgb, str) else int(rgb)
    best, best_d = 1, None
    for i, p in enumerate(PALETTE):
        d = sum((((rgb >> s) & 0xFF) - ((p >> s) & 0xFF)) ** 2 for s in (0, 8, 16))
        if best_d is None or d < best_d:
            best, best_d = i + 1, d
    return best


# ---- Values between the cells and the language ------------------------------------

def from_cell(v, is_date=False):
    """A cell's value as Range.Value gives it."""
    if v is None:
        return EMPTY
    t = type(v)
    if t is float:
        return VBDate(v) if is_date else v
    if t is str or t is bool:
        return v
    if isinstance(v, office42.Error):
        return ErrValue(ERROR_CODES.get(v.text, 2015))
    if t is int:
        return float(v)
    return v


def from_python(v):
    """A value from the office42 module -- a function's argument, an
    evaluated formula -- as a Visual Basic one."""
    if isinstance(v, list):
        return VBArray.from_rows([[from_python(x) for x in row] for row in v])
    return from_cell(v)


def date_input(d):
    dt = VBDate(d).to_datetime()
    f = float(d)
    if f == math.floor(f):
        return "%04d-%02d-%02d" % (dt.year, dt.month, dt.day)
    if 0 <= f < 1:
        return "%02d:%02d:%02d" % (dt.hour, dt.minute, dt.second)
    return "%04d-%02d-%02d %02d:%02d:%02d" % (dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second)


def to_input(v):
    """What typing a value into a cell looks like: what Range.Value = v
    does, which is Excel's way too -- "=A1" is a formula, "12" a number."""
    if is_object(v) and v is not None:
        v = plain_value(v)
    t = type(v)
    if t is str:
        return v
    if t is bool:
        return "TRUE" if v else "FALSE"
    if t is int:
        return str(v)
    if t is float:
        if v != v or v in (float("inf"), float("-inf")):
            raise VBAError(6)
        return repr(v)
    if t is VBDate:
        return date_input(v)
    if v is EMPTY or v is None or v is NULL or v is MISSING:
        return ""
    if isinstance(v, ErrValue):
        return ERROR_NAMES.get(v.code, "#VALUE!")
    if isinstance(v, VBArray):
        return to_input(v.data[0]) if v.data else ""
    return to_str(v)


def to_python(v):
    """A Visual Basic value handed back to a cell by a function."""
    if is_object(v) and v is not None and not isinstance(v, VBArray):
        v = plain_value(v)
    if isinstance(v, VBArray):
        return to_python(v.data[0]) if v.data else office42.Error("#VALUE!")
    t = type(v)
    if t is VBDate:
        return float(v)
    if t is bool or t is str or t is float:
        return v
    if t is int:
        return float(v)
    if v is EMPTY or v is MISSING:
        return 0.0
    if v is NULL:
        return office42.Error("#N/A")
    if isinstance(v, ErrValue):
        return office42.Error(ERROR_NAMES.get(v.code, "#VALUE!"))
    if v is None:
        return office42.Error("#VALUE!")
    return to_str(v)


# ---- Addresses --------------------------------------------------------------------

def col_name(c):
    """Column 0 is A."""
    name = ""
    c += 1
    while c > 0:
        c, r = divmod(c - 1, 26)
        name = chr(65 + r) + name
    return name


def col_index(letters):
    n = 0
    for ch in letters.upper():
        n = n * 26 + ord(ch) - 64
    return n - 1


_CELL_RE = re.compile(r"^\$?([A-Za-z]{1,3})\$?(\d+)$")
_COL_RE = re.compile(r"^\$?([A-Za-z]{1,3})$")
_ROW_RE = re.compile(r"^\$?(\d+)$")


def parse_area(text):
    """A1, A1:B2, A:C, 3:5 or $A$1 as (row0, col0, row1, col1), or None."""
    parts = text.strip().split(":")
    if len(parts) > 2 or not parts[0]:
        return None
    ends = []
    for p in parts:
        p = p.strip()
        m = _CELL_RE.match(p)
        if m:
            c = col_index(m.group(1))
            r = int(m.group(2)) - 1
            if c >= MAX_COLS or r < 0 or r >= MAX_ROWS:
                return None
            ends.append(("cell", r, c))
            continue
        m = _COL_RE.match(p)
        if m and len(parts) == 2:
            ends.append(("col", None, col_index(m.group(1))))
            continue
        m = _ROW_RE.match(p)
        if m and len(parts) == 2:
            ends.append(("row", int(m.group(1)) - 1, None))
            continue
        return None
    if len(ends) == 1:
        _, r, c = ends[0]
        return (r, c, r, c)
    (k1, r1, c1), (k2, r2, c2) = ends
    if k1 == "cell" and k2 == "cell":
        return (min(r1, r2), min(c1, c2), max(r1, r2), max(c1, c2))
    if k1 == "col" and k2 == "col":
        return (0, min(c1, c2), MAX_ROWS - 1, max(c1, c2))
    if k1 == "row" and k2 == "row":
        return (min(r1, r2), 0, max(r1, r2), MAX_COLS - 1)
    return None


def split_sheet(text):
    """'My Sheet'!A1 as ("My Sheet", "A1"); A1 as (None, "A1")."""
    text = text.strip()
    if "!" not in text:
        return None, text
    at = text.rfind("!")
    sheet = text[:at]
    if sheet.startswith("'") and sheet.endswith("'"):
        sheet = sheet[1:-1].replace("''", "'")
    if "[" in sheet and "]" in sheet:
        sheet = sheet[sheet.index("]") + 1:]       # [Book1.xlsx]Sheet1
    return sheet, text[at + 1:]


def cell_address(r, c, row_abs=True, col_abs=True):
    return ("$" if col_abs else "") + col_name(c) + ("$" if row_abs else "") + str(r + 1)


def area_address(r0, c0, r1, c1, row_abs=True, col_abs=True):
    if c0 == 0 and c1 >= MAX_COLS - 1 and not (r0 == 0 and r1 >= MAX_ROWS - 1):
        return "%s%d:%s%d" % ("$" if row_abs else "", r0 + 1, "$" if row_abs else "", r1 + 1)
    if r0 == 0 and r1 >= MAX_ROWS - 1:
        return "%s%s:%s%s" % ("$" if col_abs else "", col_name(c0), "$" if col_abs else "", col_name(c1))
    a = cell_address(r0, c0, row_abs, col_abs)
    if r0 == r1 and c0 == c1:
        return a
    return a + ":" + cell_address(r1, c1, row_abs, col_abs)


def quote_sheet(name):
    return "'" + name.replace("'", "''") + "'"


def _split_strings(formula):
    """A formula's pieces: ("text", "...") for quoted strings, ("code",
    ...) for the rest, which is what references are rewritten in."""
    out = []
    i = 0
    buf = []
    while i < len(formula):
        c = formula[i]
        if c == '"':
            if buf:
                out.append(("code", "".join(buf)))
                buf = []
            j = i + 1
            while j < len(formula):
                if formula[j] == '"':
                    if j + 1 < len(formula) and formula[j + 1] == '"':
                        j += 2
                        continue
                    break
                j += 1
            out.append(("text", formula[i:j + 1]))
            i = j + 1
            continue
        if c == "'":
            # A quoted sheet name is not a reference to rewrite.
            j = formula.find("'", i + 1)
            while j >= 0 and j + 1 < len(formula) and formula[j + 1] == "'":
                j = formula.find("'", j + 2)
            if j < 0:
                j = len(formula) - 1
            buf.append(formula[i:j + 1])
            i = j + 1
            continue
        buf.append(c)
        i += 1
    if buf:
        out.append(("code", "".join(buf)))
    return out


_R1C1_RE = re.compile(r"(?<![A-Za-z0-9_.])([Rr](\[-?\d+\]|\d+)?)?([Cc](\[-?\d+\]|\d+)?)?(?![A-Za-z0-9_(\[])")


def r1c1_to_a1(formula, row, col):
    """A formula in R1C1 references as the cell at (row, col) takes it
    in A1 ones: R[-1]C is the cell above, R2C3 is $C$2."""
    def one(part, here, is_row):
        if part is None:
            return None
        body = part[1:]
        if body == "":
            return (here, False)
        if body.startswith("["):
            return (here + int(body[1:-1]), False)
        return (int(body) - 1, True)

    def replace(m):
        rp, cp = m.group(1), m.group(3)
        if not rp and not cp:
            return m.group(0)
        r = one(rp, row, True) if rp else None
        c = one(cp, col, False) if cp else None
        text = m.string
        before = text[:m.start()].rstrip()
        after = text[m.end():].lstrip()
        if r is not None and c is not None:
            return ("$" if c[1] else "") + col_name(c[0]) + ("$" if r[1] else "") + str(r[0] + 1)
        joined = before.endswith(":") or after.startswith(":")
        if r is not None:
            one_row = ("$" if r[1] else "") + str(r[0] + 1)
            return one_row if joined else one_row + ":" + one_row
        one_col = ("$" if c[1] else "") + col_name(c[0])
        return one_col if joined else one_col + ":" + one_col

    out = []
    for kind, text in _split_strings(formula):
        out.append(text if kind == "text" else _R1C1_RE.sub(replace, text))
    return "".join(out)


_A1_RE = re.compile(r"(?<![A-Za-z0-9_.])(\$?)([A-Za-z]{1,3})(\$?)(\d+)(?![A-Za-z0-9_(])")


def a1_to_r1c1(formula, row, col):
    def replace(m):
        c_abs, letters, r_abs, digits = m.groups()
        c = col_index(letters)
        r = int(digits) - 1
        if c >= MAX_COLS or r >= MAX_ROWS:
            return m.group(0)
        rp = "R%d" % (r + 1) if r_abs else ("R" if r == row else "R[%d]" % (r - row))
        cp = "C%d" % (c + 1) if c_abs else ("C" if c == col else "C[%d]" % (c - col))
        return rp + cp

    out = []
    for kind, text in _split_strings(formula):
        out.append(text if kind == "text" else _A1_RE.sub(replace, text))
    return "".join(out)


def _index_of(sheet_id):
    i = _c.sheet_from_id(sheet_id)
    if i < 0:
        raise VBAError(424, "The sheet this object belongs to has been deleted")
    return i


# ---- The objects -------------------------------------------------------------------

class _XlObject:
    """What every object of the model shares: its host, and the
    Application and Parent every Excel object has."""

    _vba_typename = "Object"

    @property
    def Application(self):
        return self._host.app

    @property
    def Creator(self):
        return 1480803660


class Font(_XlObject):
    _vba_typename = "Font"

    def __init__(self, rng):
        self._host = rng._host
        self._rng = rng

    def _get(self, key):
        return self._rng._style()[key]

    def _set(self, **props):
        self._rng._format(**props)

    @property
    def Bold(self):
        return self._get("bold")

    @Bold.setter
    def Bold(self, v):
        self._set(bold=to_bool(v))

    @property
    def Italic(self):
        return self._get("italic")

    @Italic.setter
    def Italic(self, v):
        self._set(italic=to_bool(v))

    @property
    def Underline(self):
        return 2 if self._get("underline") else -4142

    @Underline.setter
    def Underline(self, v):
        if isinstance(v, bool):
            self._set(underline=v)
        else:
            self._set(underline=to_long(v) not in (-4142, 0))

    @property
    def Strikethrough(self):
        return self._get("strikeout")

    @Strikethrough.setter
    def Strikethrough(self, v):
        self._set(strikeout=to_bool(v))

    @property
    def Size(self):
        return float(self._get("size"))

    @Size.setter
    def Size(self, v):
        self._set(size=to_double(v))

    @property
    def Name(self):
        return self._get("family")

    @Name.setter
    def Name(self, v):
        self._set(family=to_str(v))

    @property
    def Color(self):
        return from_colour(self._get("colour"))

    @Color.setter
    def Color(self, v):
        self._set(colour=vb_colour(v))

    @property
    def ColorIndex(self):
        return colour_index(self._get("colour"))

    @ColorIndex.setter
    def ColorIndex(self, v):
        i = to_long(v)
        self._set(colour=0 if i in (-4105, -4142, 0) else PALETTE[(i - 1) % 56])

    @property
    def FontStyle(self):
        b, i = self.Bold, self.Italic
        return "Bold Italic" if b and i else "Bold" if b else "Italic" if i else "Regular"

    @FontStyle.setter
    def FontStyle(self, v):
        s = to_str(v).lower()
        self._set(bold="bold" in s, italic="italic" in s or "oblique" in s)

    @property
    def Superscript(self):
        return False

    @Superscript.setter
    def Superscript(self, v):
        pass

    @property
    def Subscript(self):
        return False

    @Subscript.setter
    def Subscript(self, v):
        pass

    @property
    def TintAndShade(self):
        return 0

    @TintAndShade.setter
    def TintAndShade(self, v):
        pass

    @property
    def ThemeColor(self):
        return 0

    @ThemeColor.setter
    def ThemeColor(self, v):
        pass

    @property
    def Parent(self):
        return self._rng


_PATTERNS = {1: "solid", -4142: None, 17: "gray125", 18: "gray0625", -4124: "lightGray",
             -4125: "mediumGray", -4126: "darkGray", 9: "darkTrellis", 16: "darkGrid",
             15: "lightGrid", -4128: "darkHorizontal", -4166: "darkVertical"}


class Interior(_XlObject):
    _vba_typename = "Interior"

    def __init__(self, rng):
        self._host = rng._host
        self._rng = rng

    @property
    def Color(self):
        fill = self._rng._style()["fill"]
        return 16777215 if fill is None else from_colour(fill)

    @Color.setter
    def Color(self, v):
        self._rng._format(fill=vb_colour(v))

    @property
    def ColorIndex(self):
        return colour_index(self._rng._style()["fill"])

    @ColorIndex.setter
    def ColorIndex(self, v):
        i = to_long(v)
        if i in (-4142, 0, -4105):
            self._rng._format(fill=None)
        else:
            self._rng._format(fill=PALETTE[(i - 1) % 56])

    @property
    def Pattern(self):
        p = self._rng._style().get("pattern")
        if self._rng._style()["fill"] is None and p in (None, "none", ""):
            return -4142
        for k, v in _PATTERNS.items():
            if v == p:
                return k
        return 1

    @Pattern.setter
    def Pattern(self, v):
        p = _PATTERNS.get(to_long(v), "solid")
        if p is None:
            self._rng._format(fill=None, pattern=None)
        elif p != "solid":
            self._rng._format(pattern=p)

    @property
    def PatternColor(self):
        return from_colour(self._rng._style().get("pattern_colour", 0))

    @PatternColor.setter
    def PatternColor(self, v):
        self._rng._format(pattern_colour=vb_colour(v))

    @property
    def PatternColorIndex(self):
        return -4105

    @PatternColorIndex.setter
    def PatternColorIndex(self, v):
        pass

    @property
    def TintAndShade(self):
        return 0

    @TintAndShade.setter
    def TintAndShade(self, v):
        pass

    @property
    def ThemeColor(self):
        return 0

    @ThemeColor.setter
    def ThemeColor(self, v):
        pass

    @property
    def PatternTintAndShade(self):
        return 0

    @PatternTintAndShade.setter
    def PatternTintAndShade(self, v):
        pass


_LINE_STYLES = {1: "thin", -4115: "dashed", -4118: "dotted", -4119: "double", 4: "dashed",
                5: "dotted", 13: "dashed"}
_WEIGHTS = {1: "thin", 2: "thin", -4138: "medium", 4: "thick"}
_SIDES = {7: ("left",), 8: ("top",), 9: ("bottom",), 10: ("right",)}


class Border(_XlObject):
    _vba_typename = "Border"

    def __init__(self, rng, which):
        self._host = rng._host
        self._rng = rng
        self._which = which         # an xlEdge..., or None for all of them

    def _targets(self):
        """(range, side) pairs this border is made of."""
        r = self._rng
        w = self._which
        if w is None:
            return [(r, s) for s in ("top", "bottom", "left", "right")]
        if w in (7, 8, 9, 10):
            side = _SIDES[w][0]
            if side == "top":
                return [(r.Rows(1), "top")]
            if side == "bottom":
                return [(r.Rows(r.Rows().Count), "bottom")]
            if side == "left":
                return [(r.Columns(1), "left")]
            return [(r.Columns(r.Columns().Count), "right")]
        if w == 12:     # inside horizontal
            if r._r1 == r._r0:
                return []
            return [(r._sub(r._r0, r._c0, r._r1 - 1, r._c1), "bottom")]
        if w == 11:
            if r._c1 == r._c0:
                return []
            return [(r._sub(r._r0, r._c0, r._r1, r._c1 - 1), "right")]
        return []

    def _side_style(self):
        pairs = self._targets()
        if not pairs:
            return "none"
        rng, side = pairs[0]
        return rng._style()["border_" + side]

    @property
    def LineStyle(self):
        s = self._side_style()
        if s in (None, "none"):
            return -4142
        return {"dashed": -4115, "dotted": -4118, "double": -4119}.get(s, 1)

    @LineStyle.setter
    def LineStyle(self, v):
        n = to_long(v)
        style = None if n in (-4142, 0) else _LINE_STYLES.get(n, "thin")
        for rng, side in self._targets():
            rng._format(**{"border_" + side: style if style else "none"})

    @property
    def Weight(self):
        s = self._side_style()
        return {"medium": -4138, "thick": 4}.get(s, 2)

    @Weight.setter
    def Weight(self, v):
        style = _WEIGHTS.get(to_long(v), "thin")
        for rng, side in self._targets():
            rng._format(**{"border_" + side: style})

    @property
    def Color(self):
        pairs = self._targets()
        if not pairs:
            return 0
        rng, side = pairs[0]
        return from_colour(rng._style()["border_%s_colour" % side])

    @Color.setter
    def Color(self, v):
        for rng, side in self._targets():
            rng._format(**{"border_%s_colour" % side: vb_colour(v)})

    @property
    def ColorIndex(self):
        return colour_index(vb_colour(self.Color))

    @ColorIndex.setter
    def ColorIndex(self, v):
        i = to_long(v)
        if i not in (-4105, -4142, 0):
            self.Color = from_colour(PALETTE[(i - 1) % 56])

    @property
    def TintAndShade(self):
        return 0

    @TintAndShade.setter
    def TintAndShade(self, v):
        pass

    @property
    def ThemeColor(self):
        return 0

    @ThemeColor.setter
    def ThemeColor(self, v):
        pass


class Borders(Border):
    _vba_typename = "Borders"

    def __init__(self, rng):
        Border.__init__(self, rng, None)

    def Item(self, Index):
        return Border(self._rng, to_long(Index))

    def _vba_default_get(self, *args):
        if not args:
            raise VBAError(450)
        return self.Item(args[0])

    @property
    def Count(self):
        return 6

    def _vba_iter(self):
        return [Border(self._rng, i) for i in (7, 8, 9, 10, 11, 12)]


class Comment(_XlObject):
    _vba_typename = "Comment"

    def __init__(self, rng):
        self._host = rng._host
        self._rng = rng

    def Text(self, Text=MISSING, Start=MISSING, Overwrite=MISSING):
        i = self._rng._index()
        if Text is not MISSING:
            _c.set_note(i, self._rng._r0, self._rng._c0, to_str(Text))
        return _c.get_note(i, self._rng._r0, self._rng._c0) or ""

    def Delete(self):
        _c.set_note(self._rng._index(), self._rng._r0, self._rng._c0, None)

    @property
    def Parent(self):
        return self._rng

    @property
    def Author(self):
        return getpass.getuser()

    @property
    def Visible(self):
        return False

    @Visible.setter
    def Visible(self, v):
        pass

    @property
    def Shape(self):
        return self


class Hyperlink(_XlObject):
    _vba_typename = "Hyperlink"

    def __init__(self, rng):
        self._host = rng._host
        self._rng = rng

    def _target(self):
        return _c.get_link(self._rng._index(), self._rng._r0, self._rng._c0) or ""

    @property
    def Address(self):
        t = self._target()
        return "" if t.startswith("#") else t

    @Address.setter
    def Address(self, v):
        _c.set_link(self._rng._index(), self._rng._r0, self._rng._c0, to_str(v))

    @property
    def SubAddress(self):
        t = self._target()
        return t[1:] if t.startswith("#") else ""

    @SubAddress.setter
    def SubAddress(self, v):
        _c.set_link(self._rng._index(), self._rng._r0, self._rng._c0, "#" + to_str(v))

    @property
    def TextToDisplay(self):
        return _c.get_display(self._rng._index(), self._rng._r0, self._rng._c0)

    @TextToDisplay.setter
    def TextToDisplay(self, v):
        self._rng.Value = v

    @property
    def Range(self):
        return self._rng

    def Delete(self):
        _c.set_link(self._rng._index(), self._rng._r0, self._rng._c0, None)

    def Follow(self, NewWindow=False, AddHistory=True, ExtraInfo=MISSING, Method=MISSING, HeaderInfo=MISSING):
        raise VBAError(1004, "Following a hyperlink from a macro is not supported")


class Hyperlinks(_XlObject):
    _vba_typename = "Hyperlinks"

    def __init__(self, host, sheet_id, area=None):
        self._host = host
        self._id = sheet_id
        self._area = area

    def _cells(self):
        i = _index_of(self._id)
        if self._area is not None:
            r0, c0, r1, c1 = self._area
        else:
            used = _c.used_range(i)
            if used is None:
                return []
            r0, c0, r1, c1 = used
        out = []
        for r in range(r0, min(r1, r0 + 100000) + 1):
            for c in range(c0, c1 + 1):
                if _c.get_link(i, r, c):
                    out.append(Hyperlink(Range(self._host, self._id, r, c, r, c)))
        return out

    @property
    def Count(self):
        return len(self._cells())

    def Item(self, Index):
        items = self._cells()
        i = to_long(Index)
        if i < 1 or i > len(items):
            raise VBAError(9)
        return items[i - 1]

    def _vba_default_get(self, *args):
        return self.Item(args[0])

    def _vba_iter(self):
        return self._cells()

    def Add(self, Anchor, Address, SubAddress=MISSING, ScreenTip=MISSING, TextToDisplay=MISSING):
        if not isinstance(Anchor, Range):
            raise VBAError(13)
        target = to_str(Address)
        if SubAddress is not MISSING and to_str(SubAddress):
            target = (target + "#" if target else "#") + to_str(SubAddress)
        _c.set_link(Anchor._index(), Anchor._r0, Anchor._c0, target)
        if TextToDisplay is not MISSING:
            Anchor.Value = TextToDisplay
        elif _c.get_input(Anchor._index(), Anchor._r0, Anchor._c0) == "":
            Anchor.Value = to_str(Address) or to_str(SubAddress)
        return Hyperlink(Anchor)

    def Delete(self):
        for h in self._cells():
            h.Delete()


class Validation(_XlObject):
    _vba_typename = "Validation"
    _KINDS = {0: "any", 1: "whole", 2: "decimal", 3: "list", 4: "date", 5: "time", 6: "length",
              7: "custom"}
    _OPS = {1: "between", 2: "not_between", 3: "==", 4: "!=", 5: ">", 6: "<", 7: ">=", 8: "<="}

    def __init__(self, rng):
        self._host = rng._host
        self._rng = rng

    def _rule(self):
        return office42.Sheet(self._rng._index())[self._rng._r0, self._rng._c0].validation

    def Add(self, Type, AlertStyle=1, Operator=1, Formula1=MISSING, Formula2=MISSING):
        kind = self._KINDS.get(to_long(Type), "any")
        op = self._OPS.get(to_long(Operator), "between")
        f1 = "" if Formula1 is MISSING else to_str(Formula1)
        f2 = "" if Formula2 is MISSING else to_str(Formula2)
        r = self._rng
        o = office42.Range(office42.Sheet(r._index()), r._r0, r._c0, r._r1, r._c1)
        if kind == "custom":
            _c.clear_validations(r._index(), r._r0, r._c0, r._r1, r._c1)
            _c.add_validation(r._index(), r._r0, r._c0, r._r1, r._c1, "custom", op, f1, f2, "", True)
            return
        o.validate(kind, op, f1.lstrip("=") if kind == "list" else f1, f2)

    def Delete(self):
        r = self._rng
        _c.clear_validations(r._index(), r._r0, r._c0, r._r1, r._c1)

    def Modify(self, Type=MISSING, AlertStyle=1, Operator=1, Formula1=MISSING, Formula2=MISSING):
        self.Delete()
        self.Add(Type, AlertStyle, Operator, Formula1, Formula2)

    @property
    def Type(self):
        rule = self._rule()
        if rule is None:
            raise VBAError(1004)
        for k, v in self._KINDS.items():
            if v == rule.get("kind"):
                return k
        return 0

    @property
    def Formula1(self):
        rule = self._rule()
        return "" if rule is None else to_str(rule.get("value", ""))

    @property
    def Formula2(self):
        rule = self._rule()
        return "" if rule is None else to_str(rule.get("value2", ""))

    def _flag(self, v=None):
        return True

    InCellDropdown = property(lambda self: True, lambda self, v: None)
    IgnoreBlank = property(lambda self: True, lambda self, v: None)
    ShowInput = property(lambda self: True, lambda self, v: None)
    ShowError = property(lambda self: True, lambda self, v: None)
    InputTitle = property(lambda self: "", lambda self, v: None)
    ErrorTitle = property(lambda self: "", lambda self, v: None)
    InputMessage = property(lambda self: "", lambda self, v: None)
    ErrorMessage = property(lambda self: "", lambda self, v: None)

    def Value(self):
        return True


_CF_OPS = {1: "between", 2: "not_between", 3: "==", 4: "!=", 5: ">", 6: "<", 7: ">=", 8: "<="}


class FormatCondition(_XlObject):
    """A conditional format being built: its look is gathered as the
    macro sets it, and the rule is put on the range again each time."""

    _vba_typename = "FormatCondition"

    def __init__(self, conditions, op, v1, v2):
        self._host = conditions._host
        self._conditions = conditions
        self._op = op
        self._v1 = v1
        self._v2 = v2
        self._format = {}
        self.Font = _CFPart(self, "font")
        self.Interior = _CFPart(self, "interior")
        self.Borders = _CFPart(self, "borders")

    def _apply(self):
        self._conditions._apply_all()

    def Delete(self):
        self._conditions._items.remove(self)
        self._apply()

    def Modify(self, Type, Operator=MISSING, Formula1=MISSING, Formula2=MISSING):
        if Operator is not MISSING:
            self._op = _CF_OPS.get(to_long(Operator), "between")
        if Formula1 is not MISSING:
            self._v1 = Formula1
        if Formula2 is not MISSING:
            self._v2 = Formula2
        self._apply()

    @property
    def StopIfTrue(self):
        return False

    @StopIfTrue.setter
    def StopIfTrue(self, v):
        pass

    def SetFirstPriority(self):
        pass


class _CFPart:
    """The Font or Interior of a FormatCondition."""

    _vba_typename = "Font"

    def __init__(self, cond, kind):
        object.__setattr__(self, "_cond", cond)
        object.__setattr__(self, "_kind", kind)

    def _vba_get(self, name, *args):
        return self._cond._format.get((self._kind, name), EMPTY)

    def _vba_let(self, name, value, *args):
        f = self._cond._format
        f[(self._kind, name)] = value
        self._cond._apply()


class FormatConditions(_XlObject):
    _vba_typename = "FormatConditions"

    def __init__(self, rng):
        self._host = rng._host
        self._rng = rng
        key = (rng._id, rng._r0, rng._c0, rng._r1, rng._c1)
        self._items = rng._host.conditions.setdefault(key, [])

    @property
    def Count(self):
        return len(self._items)

    def Add(self, Type, Operator=MISSING, Formula1=MISSING, Formula2=MISSING, String=MISSING,
            TextOperator=MISSING, DateOperator=MISSING, ScopeType=MISSING):
        if to_long(Type) != 1:
            raise VBAError(1004, "Only conditions on a cell's value are supported (xlCellValue)")
        op = _CF_OPS.get(to_long(Operator) if Operator is not MISSING else 1, "between")
        cond = FormatCondition(self, op, Formula1, Formula2)
        self._items.append(cond)
        return cond

    def Item(self, Index):
        return self._items[to_long(Index) - 1]

    def _vba_default_get(self, *args):
        return self.Item(args[0])

    def _vba_iter(self):
        return list(self._items)

    def Delete(self):
        del self._items[:]
        r = self._rng
        _c.clear_conditions(r._index(), r._r0, r._c0, r._r1, r._c1)

    def _apply_all(self):
        r = self._rng
        i = r._index()
        _c.clear_conditions(i, r._r0, r._c0, r._r1, r._c1)
        for cond in self._items:
            fmt = {}
            for (kind, name), value in cond._format.items():
                if kind == "font" and name == "bold":
                    fmt["bold"] = to_bool(value)
                elif kind == "font" and name == "italic":
                    fmt["italic"] = to_bool(value)
                elif kind == "font" and name == "color":
                    fmt["colour"] = vb_colour(value)
                elif kind == "font" and name == "colorindex" and to_long(value) > 0:
                    fmt["colour"] = PALETTE[(to_long(value) - 1) % 56]
                elif kind == "interior" and name == "color":
                    fmt["fill"] = vb_colour(value)
                elif kind == "interior" and name == "colorindex" and to_long(value) > 0:
                    fmt["fill"] = PALETTE[(to_long(value) - 1) % 56]

            def number(v):
                if v is MISSING:
                    return 0.0
                s = to_str(v)
                if s.startswith("="):
                    got = _c.evaluate(s, i)
                    return float(got) if isinstance(got, (int, float)) else 0.0
                return to_double(v)
            _c.add_condition(i, r._r0, r._c0, r._r1, r._c1, cond._op, number(cond._v1), number(cond._v2), **fmt)


class Range(_XlObject):
    """A rectangle of cells on a sheet, from (row0, col0) to (row1,
    col1), counted from 0."""

    _vba_typename = "Range"
    __slots__ = ("_host", "_id", "_r0", "_c0", "_r1", "_c1")

    def __init__(self, host, sheet_id, r0, c0, r1=None, c1=None):
        self._host = host
        self._id = sheet_id
        r1 = r0 if r1 is None else r1
        c1 = c0 if c1 is None else c1
        if r0 < 0 or c0 < 0 or r1 >= MAX_ROWS or c1 >= MAX_COLS:
            raise VBAError(1004, "The range is off the sheet")
        self._r0, self._c0 = min(r0, r1), min(c0, c1)
        self._r1, self._c1 = max(r0, r1), max(c0, c1)

    def __repr__(self):
        return "<Range %s>" % self.Address()

    def _index(self):
        return _index_of(self._id)

    def _sub(self, r0, c0, r1, c1):
        return Range(self._host, self._id, r0, c0, r1, c1)

    @property
    def _nrows(self):
        return self._r1 - self._r0 + 1

    @property
    def _ncols(self):
        return self._c1 - self._c0 + 1

    def _clamped(self):
        """The part of the range that has anything in it: what a walk
        over a whole column need look at."""
        used = _c.used_range(self._index())
        if used is None:
            return None
        r0, c0 = max(self._r0, used[0]), max(self._c0, used[1])
        r1, c1 = min(self._r1, used[2]), min(self._c1, used[3])
        if r0 > r1 or c0 > c1:
            return None
        return r0, c0, r1, c1

    def _style(self):
        return _c.get_format(self._index(), self._r0, self._c0)

    def _format(self, **props):
        _c.set_format(self._index(), self._r0, self._c0, self._r1, self._c1, **props)

    def _o42(self):
        return office42.Range(office42.Sheet(self._index()), self._r0, self._c0, self._r1, self._c1)

    # -- where --
    @property
    def Parent(self):
        return self._host.worksheet_by_id(self._id)

    Worksheet = Parent

    @property
    def Row(self):
        return self._r0 + 1

    @property
    def Column(self):
        return self._c0 + 1

    @property
    def Count(self):
        return self._nrows * self._ncols

    @property
    def CountLarge(self):
        return self.Count

    def Address(self, RowAbsolute=True, ColumnAbsolute=True, ReferenceStyle=1, External=False,
                RelativeTo=MISSING):
        ra, ca = to_bool(RowAbsolute), to_bool(ColumnAbsolute)
        if to_long(ReferenceStyle) == -4150:
            base = RelativeTo if isinstance(RelativeTo, Range) else None

            def rc(r, c):
                rp = "R%d" % (r + 1) if ra or base is None else ("R" if r == base._r0 else "R[%d]" % (r - base._r0))
                cp = "C%d" % (c + 1) if ca or base is None else ("C" if c == base._c0 else "C[%d]" % (c - base._c0))
                return rp + cp
            text = rc(self._r0, self._c0)
            if self.Count > 1:
                text += ":" + rc(self._r1, self._c1)
        else:
            text = area_address(self._r0, self._c0, self._r1, self._c1, ra, ca)
        if to_bool(External):
            text = "[%s]%s!%s" % (self._host.workbook.Name, quote_sheet(self.Parent.Name), text)
        return text

    AddressLocal = Address

    # -- values --
    @property
    def Value(self):
        i = self._index()
        if self._r0 == self._r1 and self._c0 == self._c1:
            return from_cell(*_c.get_value_date(i, self._r0, self._c0))
        rows = []
        for r in range(self._r0, self._r1 + 1):
            rows.append([from_cell(*_c.get_value_date(i, r, c)) for c in range(self._c0, self._c1 + 1)])
        return VBArray.from_rows(rows)

    @Value.setter
    def Value(self, v):
        i = self._index()
        if isinstance(v, VBArray):
            if len(v.dims) == 2:
                rows = v.rows()
            elif len(v.dims) == 1:
                rows = [list(v.data)]
            else:
                raise VBAError(13)
            nr = len(rows)
            for r in range(self._nrows):
                src = rows[r] if r < nr else (rows[0] if nr == 1 else None)
                for c in range(self._ncols):
                    if src is None or c >= len(src):
                        text = "#N/A"
                    else:
                        text = to_input(src[c])
                    _c.set_input(i, self._r0 + r, self._c0 + c, text)
            return
        text = to_input(v)
        for r in range(self._r0, self._r1 + 1):
            for c in range(self._c0, self._c1 + 1):
                _c.set_input(i, r, c, text)

    @property
    def Value2(self):
        i = self._index()
        if self._r0 == self._r1 and self._c0 == self._c1:
            return from_cell(_c.get_value(i, self._r0, self._c0))
        return VBArray.from_rows([[from_cell(_c.get_value(i, r, c)) for c in range(self._c0, self._c1 + 1)]
                                  for r in range(self._r0, self._r1 + 1)])

    @Value2.setter
    def Value2(self, v):
        self.Value = v

    @property
    def Text(self):
        return _c.get_display(self._index(), self._r0, self._c0)

    @property
    def Formula(self):
        i = self._index()
        if self._r0 == self._r1 and self._c0 == self._c1:
            return _c.get_input(i, self._r0, self._c0)
        return VBArray.from_rows([[_c.get_input(i, r, c) for c in range(self._c0, self._c1 + 1)]
                                  for r in range(self._r0, self._r1 + 1)])

    @Formula.setter
    def Formula(self, v):
        i = self._index()
        if isinstance(v, VBArray):
            self.Value = v
            return
        text = to_input(v)
        if self.Count == 1 or not text.startswith("="):
            for r in range(self._r0, self._r1 + 1):
                for c in range(self._c0, self._c1 + 1):
                    _c.set_input(i, r, c, text)
            return
        # One formula over many cells is written into the first and
        # moved into the rest, as Excel does: its relative references
        # follow.
        for r in range(self._r0, self._r1 + 1):
            for c in range(self._c0, self._c1 + 1):
                _c.set_input(i, r, c, _c.relocate_formula(text, r - self._r0, c - self._c0))

    FormulaLocal = Formula

    @property
    def FormulaR1C1(self):
        i = self._index()
        if self.Count == 1:
            return a1_to_r1c1(_c.get_input(i, self._r0, self._c0), self._r0, self._c0)
        return VBArray.from_rows([[a1_to_r1c1(_c.get_input(i, r, c), r, c) for c in range(self._c0, self._c1 + 1)]
                                  for r in range(self._r0, self._r1 + 1)])

    @FormulaR1C1.setter
    def FormulaR1C1(self, v):
        i = self._index()
        if isinstance(v, VBArray):
            rows = v.rows()
            for r in range(self._nrows):
                for c in range(self._ncols):
                    if r < len(rows) and c < len(rows[r]):
                        _c.set_input(i, self._r0 + r, self._c0 + c,
                                     r1c1_to_a1(to_input(rows[r][c]), self._r0 + r, self._c0 + c))
            return
        text = to_input(v)
        for r in range(self._r0, self._r1 + 1):
            for c in range(self._c0, self._c1 + 1):
                _c.set_input(i, r, c, r1c1_to_a1(text, r, c) if text.startswith("=") else text)

    FormulaR1C1Local = FormulaR1C1

    @property
    def FormulaArray(self):
        return _c.get_input(self._index(), self._r0, self._c0)

    @FormulaArray.setter
    def FormulaArray(self, v):
        text = to_str(v)
        _c.set_array_formula(self._index(), self._r0, self._c0, self._r1, self._c1, text)

    @property
    def HasFormula(self):
        i = self._index()
        area = self._clamped() or (self._r0, self._c0, self._r0, self._c0)
        found = [(_c.get_input(i, r, c) or "").startswith("=")
                 for r in range(area[0], area[2] + 1) for c in range(area[1], area[3] + 1)]
        if all(found):
            return True
        if not any(found):
            return False
        return NULL

    @property
    def HasArray(self):
        return False

    def _vba_default_get(self, *args):
        if args:
            return self.Item(*args)
        return self.Value

    def _vba_default_let(self, value, *args):
        if args:
            self.Item(*args).Value = value
        else:
            self.Value = value

    def _vba_iter(self):
        """Cells, row by row, as For Each goes through them."""
        area = (self._r0, self._c0, self._r1, self._c1)
        if (self._r1 - self._r0) * (self._c1 - self._c0) > 1000000:
            area = self._clamped() or (self._r0, self._c0, self._r0, self._c0)
        return [self._sub(r, c, r, c) for r in range(area[0], area[2] + 1) for c in range(area[1], area[3] + 1)]

    # -- ranges from ranges --
    def Item(self, RowIndex, ColumnIndex=MISSING):
        if ColumnIndex is MISSING:
            n = to_long(RowIndex) - 1
            if isinstance(RowIndex, str):
                return self._host.parse_range(RowIndex, self.Parent)
            r, c = divmod(n, self._ncols)
            return self._sub(self._r0 + r, self._c0 + c, self._r0 + r, self._c0 + c)
        c = col_index(ColumnIndex) if isinstance(ColumnIndex, str) and not ColumnIndex.strip().isdigit() \
            else to_long(ColumnIndex) - 1
        r = to_long(RowIndex) - 1
        return self._sub(self._r0 + r, self._c0 + c, self._r0 + r, self._c0 + c)

    def Cells(self, RowIndex=MISSING, ColumnIndex=MISSING):
        if RowIndex is MISSING and ColumnIndex is MISSING:
            return self
        if RowIndex is MISSING:
            RowIndex = 1
        return self.Item(RowIndex, ColumnIndex)

    def Range(self, Cell1, Cell2=MISSING):
        """A range relative to this one's top left, as Range.Range is."""
        a = self._host.range_of(Cell1, Cell2, self.Parent)
        return self._sub(self._r0 + a._r0, self._c0 + a._c0, self._r0 + a._r1, self._c0 + a._c1)

    def Offset(self, RowOffset=0, ColumnOffset=0):
        dr, dc = to_long(RowOffset), to_long(ColumnOffset)
        return self._sub(self._r0 + dr, self._c0 + dc, self._r1 + dr, self._c1 + dc)

    def Resize(self, RowSize=MISSING, ColumnSize=MISSING):
        nr = self._nrows if RowSize is MISSING else to_long(RowSize)
        nc = self._ncols if ColumnSize is MISSING else to_long(ColumnSize)
        if nr < 1 or nc < 1:
            raise VBAError(1004)
        return self._sub(self._r0, self._c0, self._r0 + nr - 1, self._c0 + nc - 1)

    def Rows(self, RowIndex=MISSING):
        if RowIndex is MISSING:
            return RowsOf(self, True)
        n = to_long(RowIndex) - 1
        return self._sub(self._r0 + n, self._c0, self._r0 + n, self._c1)

    def Columns(self, ColumnIndex=MISSING):
        if ColumnIndex is MISSING:
            return RowsOf(self, False)
        n = col_index(ColumnIndex) if isinstance(ColumnIndex, str) else to_long(ColumnIndex) - 1
        return self._sub(self._r0, self._c0 + n, self._r1, self._c0 + n)

    @property
    def EntireRow(self):
        return self._sub(self._r0, 0, self._r1, MAX_COLS - 1)

    @property
    def EntireColumn(self):
        return self._sub(0, self._c0, MAX_ROWS - 1, self._c1)

    @property
    def Areas(self):
        return Areas(self._host, [self])

    @property
    def MergeArea(self):
        m = _c.merged_at(self._index(), self._r0, self._c0)
        return self if m is None else self._sub(*m)

    @property
    def MergeCells(self):
        return _c.merged_at(self._index(), self._r0, self._c0) is not None

    @MergeCells.setter
    def MergeCells(self, v):
        if to_bool(v):
            self.Merge()
        else:
            self.UnMerge()

    def Merge(self, Across=False):
        if to_bool(Across):
            for r in range(self._r0, self._r1 + 1):
                _c.merge(self._index(), r, self._c0, r, self._c1)
        else:
            _c.merge(self._index(), self._r0, self._c0, self._r1, self._c1)

    def UnMerge(self):
        _c.unmerge(self._index(), self._r0, self._c0, self._r1, self._c1)

    def End(self, Direction):
        """What Ctrl and an arrow key do: to the edge of the block of
        cells with something in them, or across the empty ones to the
        next that has."""
        d = to_long(Direction)
        dr, dc = {-4162: (-1, 0), -4121: (1, 0), -4159: (0, -1), -4161: (0, 1)}.get(d, (None, None))
        if dr is None:
            raise VBAError(5)
        i = self._index()
        used = _c.used_range(i)
        r, c = self._r0, self._c0

        def empty(r, c):
            if used is None or r < used[0] or r > used[2] or c < used[1] or c > used[3]:
                return True
            return _c.get_input(i, r, c) == ""

        def inside(r, c):
            return 0 <= r < MAX_ROWS and 0 <= c < MAX_COLS

        nr, nc = r + dr, c + dc
        if not inside(nr, nc):
            return self._sub(r, c, r, c)
        if empty(r, c) or empty(nr, nc):
            r, c = nr, nc
            # Across the empty cells; past the used range they all are.
            while inside(r, c) and empty(r, c):
                if used is None:
                    r, c = (MAX_ROWS - 1 if dr > 0 else 0 if dr < 0 else r), \
                           (MAX_COLS - 1 if dc > 0 else 0 if dc < 0 else c)
                    return self._sub(r, c, r, c)
                if dr > 0 and r > used[2] or dc > 0 and c > used[3]:
                    r, c = (MAX_ROWS - 1 if dr > 0 else r), (MAX_COLS - 1 if dc > 0 else c)
                    return self._sub(r, c, r, c)
                if dr < 0 and r > used[2]:
                    r = used[2]
                    continue
                if dc < 0 and c > used[3]:
                    c = used[3]
                    continue
                if dr < 0 and r < used[0] or dc < 0 and c < used[1]:
                    r, c = (0 if dr < 0 else r), (0 if dc < 0 else c)
                    return self._sub(r, c, r, c)
                r, c = r + dr, c + dc
            if not inside(r, c):
                r, c = r - dr, c - dc
            return self._sub(r, c, r, c)
        while inside(r + dr, c + dc) and not empty(r + dr, c + dc):
            r, c = r + dr, c + dc
        return self._sub(r, c, r, c)

    @property
    def CurrentRegion(self):
        """The block of filled cells round this one, bounded by empty
        rows and columns: Ctrl+*."""
        i = self._index()
        used = _c.used_range(i)
        if used is None:
            return self
        r0, c0, r1, c1 = self._r0, self._c0, self._r1, self._c1

        def filled_row(r, a, b):
            if r < used[0] or r > used[2]:
                return False
            return any(_c.get_input(i, r, c) != "" for c in range(max(a, used[1]), min(b, used[3]) + 1))

        def filled_col(c, a, b):
            if c < used[1] or c > used[3]:
                return False
            return any(_c.get_input(i, r, c) != "" for r in range(max(a, used[0]), min(b, used[2]) + 1))

        changed = True
        while changed:
            changed = False
            if r0 > 0 and filled_row(r0 - 1, c0 - 1, c1 + 1):
                r0 -= 1
                changed = True
            if r1 < MAX_ROWS - 1 and filled_row(r1 + 1, c0 - 1, c1 + 1):
                r1 += 1
                changed = True
            if c0 > 0 and filled_col(c0 - 1, r0 - 1, r1 + 1):
                c0 -= 1
                changed = True
            if c1 < MAX_COLS - 1 and filled_col(c1 + 1, r0 - 1, r1 + 1):
                c1 += 1
                changed = True
        return self._sub(r0, c0, r1, c1)

    def SpecialCells(self, Type, Value=MISSING):
        t = to_long(Type)
        i = self._index()
        if t == 11:
            used = _c.used_range(i)
            if used is None:
                return self._sub(0, 0, 0, 0)
            return self._sub(used[2], used[3], used[2], used[3])
        area = self._clamped() if self.Count > 1 else (self._r0, self._c0, self._r1, self._c1)
        if area is None:
            raise VBAError(1004, "No cells were found.")
        cells = []
        for r in range(area[0], area[2] + 1):
            hidden_row = _c.hidden(i, True, r)
            for c in range(area[1], area[3] + 1):
                text = _c.get_input(i, r, c)
                if t == 4:
                    hit = text == ""
                elif t == 2:
                    hit = text != "" and not text.startswith("=")
                elif t == -4123:
                    hit = text.startswith("=")
                elif t == 12:
                    hit = not hidden_row and not _c.hidden(i, False, c)
                elif t == -4144:
                    hit = _c.get_note(i, r, c) is not None
                else:
                    raise VBAError(1004, "That kind of special cell is not supported")
                if hit:
                    cells.append((r, c))
        if not cells:
            raise VBAError(1004, "No cells were found.")
        return Areas.from_cells(self._host, self._id, cells).as_range()

    # -- what is done to it --
    def Select(self, Replace=MISSING):
        _c.select(self._index(), self._r0, self._c0, min(self._r1, MAX_ROWS - 1), self._c1,
                  self._r0, self._c0)
        return True

    def Activate(self):
        try:
            sel = self._host.app.Selection
        except VBAError:
            sel = None
        if isinstance(sel, Range) and sel._id == self._id and sel._r0 <= self._r0 <= sel._r1 and \
                sel._c0 <= self._c0 <= sel._c1:
            _c.select(self._index(), sel._r0, sel._c0, sel._r1, sel._c1, self._r0, self._c0)
        else:
            self.Select()
        return True

    def Show(self):
        return True

    def Clear(self):
        i = self._index()
        _c.clear_range(i, self._r0, self._c0, self._r1, self._c1, False)
        _c.clear_range(i, self._r0, self._c0, self._r1, self._c1, True)

    def ClearContents(self):
        _c.clear_range(self._index(), self._r0, self._c0, self._r1, self._c1, False)

    def ClearFormats(self):
        _c.clear_range(self._index(), self._r0, self._c0, self._r1, self._c1, True)

    def ClearComments(self):
        i = self._index()
        area = self._clamped()
        if area is None:
            return
        for r in range(area[0], area[2] + 1):
            for c in range(area[1], area[3] + 1):
                if _c.get_note(i, r, c) is not None:
                    _c.set_note(i, r, c, None)

    ClearNotes = ClearComments

    def ClearHyperlinks(self):
        Hyperlinks(self._host, self._id, (self._r0, self._c0, self._r1, self._c1)).Delete()

    def Copy(self, Destination=MISSING):
        if Destination is MISSING:
            self._host.clipboard = (self, False)
            return True
        dest = Destination if isinstance(Destination, Range) else self._host.range_of(Destination)
        self._copy_to(dest, "all")
        return True

    def _copy_to(self, dest, mode, transpose=False):
        if dest._id != self._id:
            # Across sheets: through the values and formats, cell by cell.
            src_i, dst_i = self._index(), dest._index()
            for r in range(self._nrows):
                for c in range(self._ncols):
                    tr, tc = (c, r) if transpose else (r, c)
                    if mode in ("all", "formulas"):
                        text = _c.get_input(src_i, self._r0 + r, self._c0 + c)
                        if text.startswith("="):
                            text = _c.relocate_formula(text, dest._r0 + tr - self._r0 - r,
                                                       dest._c0 + tc - self._c0 - c)
                        _c.set_input(dst_i, dest._r0 + tr, dest._c0 + tc, text)
                    elif mode == "values":
                        v = _c.get_value(src_i, self._r0 + r, self._c0 + c)
                        _c.set_input(dst_i, dest._r0 + tr, dest._c0 + tc, office42._to_input(v))
                    if mode in ("all", "formats"):
                        style = _c.get_format(src_i, self._r0 + r, self._c0 + c)
                        style.pop("borders", None)
                        style.pop("shrink", None)
                        _c.set_format(dst_i, dest._r0 + tr, dest._c0 + tc, dest._r0 + tr, dest._c0 + tc, **style)
            return
        _c.copy_range(self._index(), self._r0, self._c0, self._r1, self._c1, dest._r0, dest._c0, mode,
                      bool(transpose))

    def Cut(self, Destination=MISSING):
        if Destination is MISSING:
            self._host.clipboard = (self, True)
            return True
        dest = Destination if isinstance(Destination, Range) else self._host.range_of(Destination)
        if dest._id != self._id:
            self._copy_to(dest, "all")
            self.Clear()
            return True
        _c.move_range(self._index(), self._r0, self._c0, self._r1, self._c1, dest._r0, dest._c0)
        return True

    def PasteSpecial(self, Paste=-4104, Operation=-4142, SkipBlanks=False, Transpose=False):
        clip = self._host.clipboard
        if clip is None:
            raise VBAError(1004, "PasteSpecial method of Range class failed: nothing was copied")
        src, cut = clip
        p = to_long(Paste)
        mode = {-4163: "values", -4122: "formats", -4123: "formulas", 12: "values", 11: "formulas"}.get(p, "all")
        if p == 8:          # column widths
            for c in range(src._ncols):
                _c.col_width(self._index(), self._c0 + c, _c.col_width(src._index(), src._c0 + c))
            return True
        src._copy_to(self, mode, to_bool(Transpose))
        if p == 12:
            src._copy_to(self, "formats", to_bool(Transpose))
        return True

    def Insert(self, Shift=MISSING, CopyOrigin=MISSING):
        i = self._index()
        if self._c0 == 0 and self._c1 >= MAX_COLS - 1:
            _c.insert_rows(i, self._r0, self._nrows)
        elif self._r0 == 0 and self._r1 >= MAX_ROWS - 1:
            _c.insert_cols(i, self._c0, self._ncols)
        else:
            down = True if Shift is MISSING else to_long(Shift) == -4121
            _c.shift_cells(i, self._r0, self._c0, self._r1, self._c1, down, True)
        return True

    def Delete(self, Shift=MISSING):
        i = self._index()
        if self._c0 == 0 and self._c1 >= MAX_COLS - 1:
            _c.delete_rows(i, self._r0, self._nrows)
        elif self._r0 == 0 and self._r1 >= MAX_ROWS - 1:
            _c.delete_cols(i, self._c0, self._ncols)
        else:
            if Shift is MISSING:
                up = self._ncols >= self._nrows
            else:
                up = to_long(Shift) == -4162
            _c.shift_cells(i, self._r0, self._c0, self._r1, self._c1, up, False)
        return True

    # -- the look --
    @property
    def Font(self):
        return Font(self)

    @property
    def Interior(self):
        return Interior(self)

    @property
    def Borders(self):
        return Borders(self)

    def BorderAround(self, LineStyle=1, Weight=2, ColorIndex=-4105, Color=MISSING, ThemeColor=MISSING):
        style = _WEIGHTS.get(to_long(Weight), "thin") if to_long(LineStyle) == 1 else \
            _LINE_STYLES.get(to_long(LineStyle), "thin")
        for w in (7, 8, 9, 10):
            b = Border(self, w)
            for rng, side in b._targets():
                rng._format(**{"border_" + side: style})
            if Color is not MISSING:
                b.Color = Color
        return True

    @property
    def NumberFormat(self):
        return _c.number_code(self._index(), self._r0, self._c0)

    @NumberFormat.setter
    def NumberFormat(self, v):
        code = to_str(v)
        self._format(number="general" if code.lower() == "general" else "text" if code == "@" else code)

    NumberFormatLocal = NumberFormat

    @property
    def HorizontalAlignment(self):
        return {"general": 1, "left": -4131, "centre": -4108, "right": -4152}.get(self._style()["halign"], 1)

    @HorizontalAlignment.setter
    def HorizontalAlignment(self, v):
        n = to_long(v)
        self._format(halign={-4131: "left", -4108: "centre", 7: "centre", -4152: "right", 1: "general",
                             -4130: "left", -4117: "centre", 5: "left"}.get(n, "general"))

    @property
    def VerticalAlignment(self):
        return {"top": -4160, "middle": -4108, "bottom": -4107}.get(self._style()["valign"], -4107)

    @VerticalAlignment.setter
    def VerticalAlignment(self, v):
        n = to_long(v)
        self._format(valign={-4160: "top", -4108: "middle", -4107: "bottom"}.get(n, "bottom"))

    @property
    def WrapText(self):
        return self._style()["wrap"]

    @WrapText.setter
    def WrapText(self, v):
        self._format(wrap=to_bool(v))

    @property
    def Locked(self):
        return self._style()["locked"]

    @Locked.setter
    def Locked(self, v):
        self._format(locked=to_bool(v))

    @property
    def FormulaHidden(self):
        return self._style()["hidden"]

    @FormulaHidden.setter
    def FormulaHidden(self, v):
        self._format(hidden=to_bool(v))

    @property
    def ShrinkToFit(self):
        return self._style().get("shrink", False)

    @ShrinkToFit.setter
    def ShrinkToFit(self, v):
        pass

    @property
    def IndentLevel(self):
        return 0

    @IndentLevel.setter
    def IndentLevel(self, v):
        pass

    @property
    def Orientation(self):
        return -4128

    @Orientation.setter
    def Orientation(self, v):
        pass

    @property
    def Style(self):
        return "Normal"

    @Style.setter
    def Style(self, v):
        pass

    # -- rows and columns --
    @property
    def ColumnWidth(self):
        # Excel measures in characters of the standard font: seven
        # pixels each, and five of padding.
        return round((_c.col_width(self._index(), self._c0) - 5) / 7.0, 2)

    @ColumnWidth.setter
    def ColumnWidth(self, v):
        px = max(0, int(round(to_double(v) * 7 + 5)))
        i = self._index()
        for c in range(self._c0, min(self._c1, self._c0 + 16383) + 1):
            _c.col_width(i, c, px)

    @property
    def RowHeight(self):
        return _c.row_height(self._index(), self._r0) * 0.75

    @RowHeight.setter
    def RowHeight(self, v):
        px = max(0, int(round(to_double(v) / 0.75)))
        i = self._index()
        area = (self._r0, self._r1)
        if self._r1 - self._r0 > 100000:
            used = self._clamped()
            area = (used[0], used[2]) if used else (self._r0, self._r0)
        for r in range(area[0], area[1] + 1):
            _c.row_height(i, r, px)

    @property
    def Width(self):
        i = self._index()
        return sum(_c.col_width(i, c) for c in range(self._c0, min(self._c1, self._c0 + 500) + 1)) * 0.75

    @property
    def Height(self):
        i = self._index()
        return sum(_c.row_height(i, r) for r in range(self._r0, min(self._r1, self._r0 + 5000) + 1)) * 0.75

    @property
    def Left(self):
        i = self._index()
        return sum(_c.col_width(i, c) for c in range(0, min(self._c0, 500))) * 0.75

    @property
    def Top(self):
        i = self._index()
        return sum(_c.row_height(i, r) for r in range(0, min(self._r0, 5000))) * 0.75

    @property
    def Hidden(self):
        i = self._index()
        if self._r0 == 0 and self._r1 >= MAX_ROWS - 1:
            return _c.hidden(i, False, self._c0)
        return _c.hidden(i, True, self._r0)

    @Hidden.setter
    def Hidden(self, v):
        i = self._index()
        if self._r0 == 0 and self._r1 >= MAX_ROWS - 1:
            _c.set_hidden(i, False, self._c0, self._c1, to_bool(v))
        else:
            _c.set_hidden(i, True, self._r0, self._r1, to_bool(v))

    def AutoFit(self):
        o = self._o42()
        if not (self._c0 == 0 and self._c1 >= MAX_COLS - 1):
            area = self._clamped()
            if area is not None:
                office42.Range(office42.Sheet(self._index()), 0 if self._r0 == 0 and self._r1 >= MAX_ROWS - 1
                               else area[0], area[1], area[2], area[3]).autofit()
            else:
                o.autofit()
        return True

    # -- data --
    def Sort(self, Key1=MISSING, Order1=1, Key2=MISSING, Type=MISSING, Order2=1, Key3=MISSING, Order3=1,
             Header=2, OrderCustom=MISSING, MatchCase=False, Orientation=1, SortMethod=MISSING,
             DataOption1=MISSING, DataOption2=MISSING, DataOption3=MISSING):
        rng = self
        if self.Count == 1:
            rng = self.CurrentRegion
        keys, asc = [], []
        for key, order in ((Key1, Order1), (Key2, Order2), (Key3, Order3)):
            if key is MISSING or key is None:
                continue
            k = key if isinstance(key, Range) else self._host.range_of(key, self.Parent)
            keys.append(k._c0 - rng._c0)
            asc.append(to_long(order) != 2)
        if not keys:
            keys, asc = [0], [to_long(Order1) != 2]
        header = to_long(Header) == 1
        if to_long(Header) == 0:
            # xlGuess: a first row of text over numbers is a header.
            i = rng._index()
            first = _c.get_value(i, rng._r0, rng._c0 + keys[0])
            second = _c.get_value(i, rng._r0 + 1, rng._c0 + keys[0]) if rng._nrows > 1 else None
            header = isinstance(first, str) and not isinstance(second, str)
        _c.sort(rng._index(), rng._r0, rng._c0, rng._r1, rng._c1, keys, asc, header)
        return True

    def RemoveDuplicates(self, Columns=MISSING, Header=2):
        cols = None
        if Columns is not MISSING:
            if isinstance(Columns, VBArray):
                cols = [to_long(v) - 1 for v in Columns.data]
            else:
                cols = [to_long(Columns) - 1]
        _c.remove_duplicates(self._index(), self._r0, self._c0, self._r1, self._c1, cols, to_long(Header) == 1)

    def AutoFilter(self, Field=MISSING, Criteria1=MISSING, Operator=1, Criteria2=MISSING,
                   VisibleDropDown=True):
        rng = self.CurrentRegion if self.Count == 1 else self
        i = rng._index()
        existing = _c.get_autofilter(i)
        if Field is MISSING:
            if existing is not None:
                _c.set_autofilter(i, -1, -1, -1, -1)
            else:
                _c.set_autofilter(i, rng._r0, rng._c0, rng._r1, rng._c1)
            return True
        if existing is None:
            _c.set_autofilter(i, rng._r0, rng._c0, rng._r1, rng._c1)
            existing = (rng._r0, rng._c0, rng._r1, rng._c1)
        col = existing[1] + to_long(Field) - 1
        if Criteria1 is MISSING:
            _c.autofilter_choose(i, col, None)
        else:
            crit = Criteria1
            if isinstance(crit, VBArray):
                crit = crit.data[0] if crit.data else ""
            text = to_str(crit)
            _c.autofilter_choose(i, col, text[1:] if text.startswith("=") and len(text) > 1
                                 and text[1] not in "<>=" else text)
        return True

    def Find(self, What, After=MISSING, LookIn=MISSING, LookAt=MISSING, SearchOrder=MISSING,
             SearchDirection=1, MatchCase=False, MatchByte=MISSING, SearchFormat=MISSING):
        self._host.last_find = dict(what=What, lookin=LookIn, lookat=LookAt, order=SearchOrder,
                                    case=MatchCase, range=self)
        return self._find(What, After, LookIn, LookAt, SearchOrder, to_long(SearchDirection) == 2,
                          to_bool(MatchCase))

    def _find(self, What, After, LookIn, LookAt, SearchOrder, backwards, match_case):
        i = self._index()
        area = self._clamped() if self.Count > 1 else (self._r0, self._c0, self._r1, self._c1)
        if area is None:
            return None
        whole = LookAt is not MISSING and to_long(LookAt) == 1
        formulas = LookIn is not MISSING and to_long(LookIn) == -4123
        by_cols = SearchOrder is not MISSING and to_long(SearchOrder) == 2
        what = to_str(What)
        pattern = None
        if any(ch in what for ch in "*?~"):
            rx = "".join(".*" if ch == "*" else "." if ch == "?" else re.escape(ch) for ch in what)
            pattern = re.compile(("^" + rx + "$") if whole else rx, 0 if match_case else re.IGNORECASE)
        needle = what if match_case else what.lower()
        r0, c0, r1, c1 = area
        if by_cols:
            cells = [(r, c) for c in range(c0, c1 + 1) for r in range(r0, r1 + 1)]
        else:
            cells = [(r, c) for r in range(r0, r1 + 1) for c in range(c0, c1 + 1)]
        if backwards:
            cells.reverse()
        start = 0
        if isinstance(After, Range):
            key = (After._r0, After._c0)
            if key in cells:
                start = cells.index(key) + 1
        elif cells:
            start = 1
        n = len(cells)
        for k in range(n):
            r, c = cells[(start + k) % n]
            text = _c.get_input(i, r, c) if formulas else _c.get_display(i, r, c)
            if text == "":
                continue
            if pattern is not None:
                hit = pattern.search(text) is not None
            else:
                hay = text if match_case else text.lower()
                hit = hay == needle if whole else needle in hay
            if hit:
                return self._sub(r, c, r, c)
        return None

    def FindNext(self, After=MISSING):
        f = self._host.last_find
        if not f:
            raise VBAError(1004)
        return f["range"]._find(f["what"], After, f["lookin"], f["lookat"], f["order"], False, to_bool(f["case"]))

    def FindPrevious(self, After=MISSING):
        f = self._host.last_find
        if not f:
            raise VBAError(1004)
        return f["range"]._find(f["what"], After, f["lookin"], f["lookat"], f["order"], True, to_bool(f["case"]))

    def Replace(self, What, Replacement, LookAt=2, SearchOrder=MISSING, MatchCase=False, MatchByte=MISSING,
                SearchFormat=MISSING, ReplaceFormat=MISSING):
        what, rep = to_str(What), to_str(Replacement)
        i = self._index()
        area = self._clamped()
        if area is None:
            return True
        if to_long(LookAt) == 1:
            for r in range(area[0], area[2] + 1):
                for c in range(area[1], area[3] + 1):
                    text = _c.get_input(i, r, c)
                    if (text == what) if to_bool(MatchCase) else (text.lower() == what.lower()):
                        _c.set_input(i, r, c, rep)
            return True
        _c.replace(i, area[0], area[1], area[2], area[3], what, rep, to_bool(MatchCase))
        return True

    def FillDown(self):
        _c.fill(self._index(), self._r0, self._c0, self._r1, self._c1, 0)

    def FillRight(self):
        _c.fill(self._index(), self._r0, self._c0, self._r1, self._c1, 1)

    def FillUp(self):
        _c.fill(self._index(), self._r0, self._c0, self._r1, self._c1, 2)

    def FillLeft(self):
        _c.fill(self._index(), self._r0, self._c0, self._r1, self._c1, 3)

    def AutoFill(self, Destination, Type=0):
        dest = Destination if isinstance(Destination, Range) else self._host.range_of(Destination)
        t = to_long(Type)
        i = self._index()
        if t == 1:
            # xlFillCopy: the source repeated.
            for r in range(dest._nrows):
                for c in range(dest._ncols):
                    sr, sc = self._r0 + r % self._nrows, self._c0 + c % self._ncols
                    text = _c.get_input(i, sr, sc)
                    if text.startswith("="):
                        text = _c.relocate_formula(text, dest._r0 + r - sr, dest._c0 + c - sc)
                    _c.set_input(i, dest._r0 + r, dest._c0 + c, text)
            return True
        _c.autofill(i, self._r0, self._c0, self._r1, self._c1, dest._r0, dest._c0, dest._r1, dest._c1)
        return True

    def DataSeries(self, Rowcol=MISSING, Type=-4132, Date=1, Step=1, Stop=MISSING, Trend=False):
        kind = {-4132: "linear", 9: "linear", -4133: "growth", 10: "growth", 1: "date", 4: "autofill"}.get(
            to_long(Type), "linear")
        o = self._o42()
        o.fill_series(kind, step=to_double(Step), stop=None if Stop is MISSING else to_double(Stop),
                      rows=Rowcol is not MISSING and to_long(Rowcol) == 1, trend=to_bool(Trend))

    def Calculate(self):
        _c.calculate()

    def Justify(self):
        self._o42().justify()

    # -- notes, links, names --
    def AddComment(self, Text=MISSING):
        i = self._index()
        if _c.get_note(i, self._r0, self._c0) is not None:
            raise VBAError(1004, "The cell already has a comment")
        _c.set_note(i, self._r0, self._c0, "" if Text is MISSING else to_str(Text))
        return Comment(self)

    AddCommentThreaded = AddComment

    @property
    def Comment(self):
        if _c.get_note(self._index(), self._r0, self._c0) is None:
            return None
        return Comment(self)

    def NoteText(self, Text=MISSING, Start=MISSING, Length=MISSING):
        i = self._index()
        if Text is not MISSING:
            _c.set_note(i, self._r0, self._c0, to_str(Text))
        return _c.get_note(i, self._r0, self._c0) or ""

    @property
    def Hyperlinks(self):
        return Hyperlinks(self._host, self._id, (self._r0, self._c0, self._r1, self._c1))

    @property
    def Name(self):
        i = self._index()
        for n in _c.names():
            info = _c.name_info(n)
            if isinstance(info, tuple) and info == (i, self._r0, self._c0, self._r1, self._c1):
                return NameObj(self._host, n)
        raise VBAError(1004)

    @Name.setter
    def Name(self, v):
        _c.define_name(to_str(v), self._index(), self._r0, self._c0, self._r1, self._c1)

    @property
    def Validation(self):
        return Validation(self)

    @property
    def FormatConditions(self):
        return FormatConditions(self)

    @property
    def Characters(self):
        return self

    def _vba_get(self, name, *args):
        raise VBAError(438, "Object doesn't support this property or method (Range.%s)" % name)

    @property
    def Precedents(self):
        raise VBAError(1004, "No cells were found.")

    @property
    def Dependents(self):
        raise VBAError(1004, "No cells were found.")

    def Group(self):
        return True

    def Ungroup(self):
        return True

    def PrintOut(self, *args, **kwargs):
        raise VBAError(1004, "Printing from a macro is not supported")

    def Speak(self, *args, **kwargs):
        return True

    @property
    def ListObject(self):
        return None

    @property
    def PivotTable(self):
        raise VBAError(1004)


class RowsOf(Range):
    """Range.Rows and Range.Columns without an index: the same cells,
    counted and gone through a row (or a column) at a time."""

    _vba_typename = "Range"
    __slots__ = ("_rows",)

    def __init__(self, rng, rows):
        Range.__init__(self, rng._host, rng._id, rng._r0, rng._c0, rng._r1, rng._c1)
        self._rows = rows

    @property
    def Count(self):
        return self._nrows if self._rows else self._ncols

    def Item(self, RowIndex, ColumnIndex=MISSING):
        if ColumnIndex is not MISSING:
            return Range.Item(self, RowIndex, ColumnIndex)
        n = to_long(RowIndex) - 1 if not isinstance(RowIndex, str) or self._rows else col_index(RowIndex)
        if self._rows:
            return self._sub(self._r0 + n, self._c0, self._r0 + n, self._c1)
        return self._sub(self._r0, self._c0 + n, self._r1, self._c0 + n)

    def _vba_default_get(self, *args):
        if args:
            return self.Item(*args)
        return self.Value

    def _vba_iter(self):
        if self._rows:
            last = self._r1
            if self._r1 - self._r0 > 100000:
                used = self._clamped()
                last = used[2] if used else self._r0
            return [self._sub(r, self._c0, r, self._c1) for r in range(self._r0, last + 1)]
        last = self._c1
        if self._c1 - self._c0 > 1000:
            used = self._clamped()
            last = used[3] if used else self._c0
        return [self._sub(self._r0, c, self._r1, c) for c in range(self._c0, last + 1)]


class Areas(_XlObject):
    """The rectangles of a range that is more than one: what Union and
    SpecialCells give."""

    _vba_typename = "Areas"

    def __init__(self, host, ranges):
        self._host = host
        self._ranges = ranges

    @staticmethod
    def from_cells(host, sheet_id, cells):
        """Cells joined into runs along the rows, and runs of the same
        columns in rows below each other into one rectangle."""
        runs = []
        for r, c in cells:
            if runs and runs[-1][0] == r and runs[-1][2] == c - 1:
                runs[-1][2] = c
            else:
                runs.append([r, c, c])
        rects = []
        for r, a, b in runs:
            for rect in rects:
                if rect[2] == r - 1 and rect[1] == a and rect[3] == b:
                    rect[2] = r
                    break
            else:
                rects.append([r, a, r, b])
        return Areas(host, [Range(host, sheet_id, r0, c0, r1, c1) for r0, c0, r1, c1 in rects])

    def as_range(self):
        if len(self._ranges) == 1:
            return self._ranges[0]
        return MultiRange(self._host, self._ranges)

    @property
    def Count(self):
        return len(self._ranges)

    def Item(self, Index):
        i = to_long(Index)
        if i < 1 or i > len(self._ranges):
            raise VBAError(9)
        return self._ranges[i - 1]

    def _vba_default_get(self, *args):
        return self.Item(args[0])

    def _vba_iter(self):
        return list(self._ranges)


class MultiRange(_XlObject):
    """A range of several rectangles: most of what is asked of it is
    asked of the first, and what is set is set on all of them."""

    _vba_typename = "Range"

    def __init__(self, host, ranges):
        self._host = host
        self._ranges = ranges

    @property
    def Areas(self):
        return Areas(self._host, self._ranges)

    @property
    def Count(self):
        return sum(r.Count for r in self._ranges)

    def Address(self, RowAbsolute=True, ColumnAbsolute=True, ReferenceStyle=1, External=False,
                RelativeTo=MISSING):
        return ",".join(r.Address(RowAbsolute, ColumnAbsolute, ReferenceStyle, External, RelativeTo)
                        for r in self._ranges)

    def _vba_iter(self):
        out = []
        for r in self._ranges:
            out.extend(r._vba_iter())
        return out

    @property
    def EntireRow(self):
        return MultiRange(self._host, [r.EntireRow for r in self._ranges])

    @property
    def EntireColumn(self):
        return MultiRange(self._host, [r.EntireColumn for r in self._ranges])

    def Delete(self, Shift=MISSING):
        # From the bottom right up, so that what goes first does not move
        # what goes next.
        seen = set()
        for r in sorted(self._ranges, key=lambda r: (r._r0, r._c0), reverse=True):
            key = (r._r0, r._c0, r._r1, r._c1)
            if key in seen:
                continue
            seen.add(key)
            r.Delete(Shift)
        return True

    def Select(self, Replace=MISSING):
        return self._ranges[0].Select()

    def _vba_default_get(self, *args):
        return self._ranges[0]._vba_default_get(*args)

    def _vba_default_let(self, value, *args):
        for r in self._ranges:
            r._vba_default_let(value, *args)

    def _vba_get(self, name, *args):
        return self._host.project.member_get(self._ranges[0], name, [vba.ValueArg(a) for a in args], None)

    def _vba_let(self, name, value, *args):
        for r in self._ranges:
            self._host.project.member_let(r, name, [vba.ValueArg(a) for a in args], value, False, None)

    @property
    def Font(self):
        return _Broadcast(self._host, [r.Font for r in self._ranges])

    @property
    def Interior(self):
        return _Broadcast(self._host, [r.Interior for r in self._ranges])

    @property
    def Borders(self):
        return _Broadcast(self._host, [r.Borders for r in self._ranges])

    def ClearContents(self):
        for r in self._ranges:
            r.ClearContents()

    def Clear(self):
        for r in self._ranges:
            r.Clear()


class _Broadcast:
    """The Font (or Interior) of several rectangles at once."""

    def __init__(self, host, targets):
        self._host = host
        self._targets = targets

    def _vba_get(self, name, *args):
        return self._host.project.member_get(self._targets[0], name, [vba.ValueArg(a) for a in args], None)

    def _vba_let(self, name, value, *args):
        for t in self._targets:
            self._host.project.member_let(t, name, [vba.ValueArg(a) for a in args], value, False, None)


class NameObj(_XlObject):
    _vba_typename = "Name"

    def __init__(self, host, name):
        self._host = host
        self._name = name

    @property
    def Name(self):
        return self._name

    @Name.setter
    def Name(self, v):
        refers = self.RefersTo
        _c.delete_name(self._name)
        self._name = to_str(v)
        self._host.define_name(self._name, refers)

    @property
    def RefersTo(self):
        info = _c.name_info(self._name)
        if isinstance(info, tuple):
            i, r0, c0, r1, c1 = info
            return "=" + quote_sheet(_c.sheet_name(i)) + "!" + area_address(r0, c0, r1, c1)
        if isinstance(info, str):
            return info if info.startswith("=") else "=" + info
        raise VBAError(1004)

    @RefersTo.setter
    def RefersTo(self, v):
        self._host.define_name(self._name, to_str(v))

    RefersToLocal = RefersTo

    @property
    def RefersToR1C1(self):
        return a1_to_r1c1(self.RefersTo, 0, 0)

    @property
    def RefersToRange(self):
        info = _c.name_info(self._name)
        if not isinstance(info, tuple):
            raise VBAError(1004)
        i, r0, c0, r1, c1 = info
        return Range(self._host, _c.sheet_id(i), r0, c0, r1, c1)

    @property
    def Value(self):
        return self.RefersTo

    @Value.setter
    def Value(self, v):
        self.RefersTo = v

    def _vba_default_get(self, *args):
        return self.RefersTo

    @property
    def Visible(self):
        return True

    @Visible.setter
    def Visible(self, v):
        pass

    def Delete(self):
        _c.delete_name(self._name)


class Names(_XlObject):
    _vba_typename = "Names"

    def __init__(self, host):
        self._host = host

    @property
    def Count(self):
        return len(_c.names())

    def Item(self, Index=MISSING, IndexLocal=MISSING, RefersTo=MISSING):
        names = _c.names()
        if isinstance(Index, str):
            for n in names:
                if n.lower() == Index.lower():
                    return NameObj(self._host, n)
            raise VBAError(1004, "The name %s is not defined" % Index)
        i = to_long(Index)
        if i < 1 or i > len(names):
            raise VBAError(9)
        return NameObj(self._host, names[i - 1])

    def _vba_default_get(self, *args):
        return self.Item(*args)

    def _vba_iter(self):
        return [NameObj(self._host, n) for n in _c.names()]

    def Add(self, Name=MISSING, RefersTo=MISSING, Visible=True, MacroType=MISSING, ShortcutKey=MISSING,
            Category=MISSING, NameLocal=MISSING, RefersToLocal=MISSING, CategoryLocal=MISSING,
            RefersToR1C1=MISSING, RefersToR1C1Local=MISSING):
        name = to_str(Name if Name is not MISSING else NameLocal)
        if RefersTo is not MISSING:
            refers = RefersTo
        elif RefersToLocal is not MISSING:
            refers = RefersToLocal
        elif RefersToR1C1 is not MISSING:
            refers = r1c1_to_a1(to_str(RefersToR1C1), 0, 0)
        else:
            raise VBAError(449)
        if isinstance(refers, Range):
            _c.define_name(name, refers._index(), refers._r0, refers._c0, refers._r1, refers._c1)
        else:
            self._host.define_name(name, to_str(refers))
        return NameObj(self._host, name)


class Shape(_XlObject):
    """A shape, picture, chart or form control floating over a sheet."""

    _vba_typename = "Shape"

    def __init__(self, host, sheet_id, kind, oid, position):
        self._host = host
        self._id = sheet_id
        self._kind = kind
        self._oid = oid
        self._position = position

    def _props(self):
        return _c.object_get(_index_of(self._id), self._kind, self._oid)

    def _set(self, **props):
        _c.object_set(_index_of(self._id), self._kind, self._oid, props)

    @property
    def Name(self):
        p = self._props()
        kind = p.get("kind", self._kind)
        label = {"button": "Button", "checkbox": "Check Box", "option": "Option Button",
                 "label": "Label", "listbox": "List Box", "combo": "Drop Down", "spinner": "Spinner",
                 "scrollbar": "Scroll Bar", "groupbox": "Group Box", "textbox": "TextBox"}.get(kind)
        if self._kind == "picture":
            label = "Picture"
        elif self._kind == "chart":
            label = "Chart"
        return "%s %d" % (label or "Rectangle", self._oid)

    @property
    def OnAction(self):
        return self._props().get("script") or ""

    @OnAction.setter
    def OnAction(self, v):
        self._set(script=to_str(v))

    @property
    def Width(self):
        return self._props()["width"] * 0.75

    @Width.setter
    def Width(self, v):
        self._set(width=to_double(v) / 0.75)

    @property
    def Height(self):
        return self._props()["height"] * 0.75

    @Height.setter
    def Height(self, v):
        self._set(height=to_double(v) / 0.75)

    @property
    def TopLeftCell(self):
        p = self._props()
        return Range(self._host, self._id, p["row"], p["col"])

    @property
    def Left(self):
        return self.TopLeftCell.Left

    @property
    def Top(self):
        return self.TopLeftCell.Top

    @property
    def Visible(self):
        return True

    @Visible.setter
    def Visible(self, v):
        pass

    @property
    def Type(self):
        return {"chart": 3, "picture": 13}.get(self._kind, 1 if self._props().get("kind") not in (
            "button", "checkbox", "option", "label", "listbox", "combo", "spinner", "scrollbar",
            "groupbox") else 8)

    @property
    def TextFrame(self):
        return _ShapeText(self)

    TextFrame2 = TextFrame

    def Delete(self):
        _c.remove_object(_index_of(self._id), self._kind, self._oid)

    def Select(self, Replace=MISSING):
        return True

    @property
    def ZOrderPosition(self):
        return self._position


class _ShapeText:
    """Shape.TextFrame.Characters.Text, all of which is the shape's text."""

    _vba_typename = "TextFrame"

    def __init__(self, shape):
        self._shape = shape

    def Characters(self, Start=MISSING, Length=MISSING):
        return self

    @property
    def TextRange(self):
        return self

    @property
    def Text(self):
        return self._shape._props().get("text") or ""

    @Text.setter
    def Text(self, v):
        self._shape._set(text=to_str(v))


class Shapes(_XlObject):
    _vba_typename = "Shapes"

    def __init__(self, host, sheet_id, kinds=("shape", "picture", "chart")):
        self._host = host
        self._id = sheet_id
        self._kinds = kinds

    def _all(self):
        i = _index_of(self._id)
        return [Shape(self._host, self._id, t, oid, n + 1)
                for n, (t, oid) in enumerate(_c.objects(i)) if t in self._kinds]

    @property
    def Count(self):
        return len(self._all())

    def Item(self, Index):
        items = self._all()
        if isinstance(Index, str):
            for s in items:
                if s.Name.lower() == Index.lower():
                    return s
            raise VBAError(-2147024809, "The item with the specified name wasn't found.")
        i = to_long(Index)
        if i < 1 or i > len(items):
            raise VBAError(9)
        return items[i - 1]

    def _vba_default_get(self, *args):
        return self.Item(args[0])

    def _vba_iter(self):
        return self._all()

    def SelectAll(self):
        return True

    def AddShape(self, Type, Left, Top, Width, Height):
        ws = self._host.worksheet_by_id(self._id)
        at = ws._cell_at(to_double(Left), to_double(Top))
        oid = _c.add_shape(_index_of(self._id), "rectangle" if to_long(Type) != 9 else "oval", at[0], at[1])
        s = Shape(self._host, self._id, "shape", oid, 0)
        s._set(width=to_double(Width) / 0.75, height=to_double(Height) / 0.75)
        return s

    def AddTextbox(self, Orientation, Left, Top, Width, Height):
        ws = self._host.worksheet_by_id(self._id)
        at = ws._cell_at(to_double(Left), to_double(Top))
        oid = _c.add_shape(_index_of(self._id), "textbox", at[0], at[1])
        s = Shape(self._host, self._id, "shape", oid, 0)
        s._set(width=to_double(Width) / 0.75, height=to_double(Height) / 0.75)
        return s

    def AddPicture(self, Filename, LinkToFile, SaveWithDocument, Left, Top, Width=-1, Height=-1):
        ws = self._host.worksheet_by_id(self._id)
        at = ws._cell_at(to_double(Left), to_double(Top))
        oid = _c.add_picture(_index_of(self._id), to_str(Filename), at[0], at[1])
        s = Shape(self._host, self._id, "picture", oid, 0)
        if to_double(Width) > 0:
            s._set(width=to_double(Width) / 0.75)
        if to_double(Height) > 0:
            s._set(height=to_double(Height) / 0.75)
        return s


class SortFields(_XlObject):
    _vba_typename = "SortFields"

    def __init__(self, sort):
        self._host = sort._host
        self._fields = []

    def Clear(self):
        del self._fields[:]

    def Add(self, Key, SortOn=0, Order=1, CustomOrder=MISSING, DataOption=0):
        self._fields.append((Key, to_long(Order)))
        return self

    Add2 = Add

    @property
    def Count(self):
        return len(self._fields)


class Sort(_XlObject):
    """Worksheet.Sort: what the macro recorder writes for Data > Sort --
    the fields added, the range set, then Apply."""

    _vba_typename = "Sort"

    def __init__(self, host):
        self._host = host
        self.SortFields = SortFields(self)
        self._range = None
        self.Header = 2
        self.MatchCase = False
        self.Orientation = 1
        self.SortMethod = 1

    def SetRange(self, Rng):
        self._range = Rng

    @property
    def Rng(self):
        return self._range

    def Apply(self):
        rng = self._range
        if rng is None or not self.SortFields._fields:
            raise VBAError(1004, "The sort reference is not valid.")
        keys, asc = [], []
        for key, order in self.SortFields._fields[:3]:
            k = key if isinstance(key, Range) else self._host.range_of(key)
            keys.append(k._c0 - rng._c0)
            asc.append(order != 2)
        _c.sort(rng._index(), rng._r0, rng._c0, rng._r1, rng._c1, keys, asc, to_long(self.Header) == 1)


class PageSetup(_XlObject):
    """Kept for the macros that set it: office42 prints from File > Page Setup."""

    _vba_typename = "PageSetup"

    def __init__(self, host):
        self._host = host
        self.__dict__.update(dict(Orientation=1, Zoom=100, FitToPagesWide=1, FitToPagesTall=1,
                                  PrintArea="", PrintTitleRows="", PrintTitleColumns="",
                                  LeftMargin=54.0, RightMargin=54.0, TopMargin=72.0, BottomMargin=72.0,
                                  HeaderMargin=36.0, FooterMargin=36.0, CenterHorizontally=False,
                                  CenterVertically=False, PaperSize=1, PrintGridlines=False,
                                  PrintHeadings=False, LeftHeader="", CenterHeader="", RightHeader="",
                                  LeftFooter="", CenterFooter="", RightFooter="", BlackAndWhite=False,
                                  Draft=False, FirstPageNumber=-4105, Order=1, PrintComments=-4142))


class Worksheet(_XlObject):
    """A sheet of the book, whatever its place: it follows the sheet as
    it moves and is renamed."""

    _vba_typename = "Worksheet"

    def __init__(self, host, sheet_id):
        self._host = host
        self._id = sheet_id
        self._vba_module = None
        self._sort = None
        self._page_setup = None

    def __repr__(self):
        return "<Worksheet %s>" % self.Name

    def _index(self):
        return _index_of(self._id)

    def _cell_at(self, left, top):
        """The cell under a point, in points from the sheet's corner."""
        i = self._index()
        x, c = 0.0, 0
        while c < MAX_COLS - 1 and x + _c.col_width(i, c) * 0.75 <= left:
            x += _c.col_width(i, c) * 0.75
            c += 1
        y, r = 0.0, 0
        while r < MAX_ROWS - 1 and y + _c.row_height(i, r) * 0.75 <= top:
            y += _c.row_height(i, r) * 0.75
            r += 1
        return r, c

    @property
    def Name(self):
        return _c.sheet_name(self._index())

    @Name.setter
    def Name(self, v):
        name = to_str(v)
        if not name or len(name) > 31 or any(ch in name for ch in "\\/?*[]:"):
            raise VBAError(1004, "That name is not valid for a sheet")
        _c.rename_sheet(self._index(), name)

    @property
    def CodeName(self):
        return _c.codename(self._index()) or ""

    @property
    def Index(self):
        return self._index() + 1

    @property
    def Parent(self):
        return self._host.workbook

    @property
    def Type(self):
        return -4167

    @property
    def Visible(self):
        return 0 if _c.sheet_hidden(self._index()) else -1

    @Visible.setter
    def Visible(self, v):
        if isinstance(v, bool):
            hide = not v
        else:
            hide = to_long(v) in (0, 2)
        _c.sheet_hidden(self._index(), hide)

    def Range(self, Cell1, Cell2=MISSING):
        return self._host.range_of(Cell1, Cell2, self)

    def Cells(self, RowIndex=MISSING, ColumnIndex=MISSING):
        whole = Range(self._host, self._id, 0, 0, MAX_ROWS - 1, MAX_COLS - 1)
        if RowIndex is MISSING and ColumnIndex is MISSING:
            return whole
        return whole.Cells(RowIndex, ColumnIndex)

    def Rows(self, RowIndex=MISSING):
        whole = Range(self._host, self._id, 0, 0, MAX_ROWS - 1, MAX_COLS - 1)
        if isinstance(RowIndex, str):
            a = parse_area(RowIndex if ":" in RowIndex else RowIndex + ":" + RowIndex)
            if a is None:
                raise VBAError(13)
            return Range(self._host, self._id, a[0], 0, a[2], MAX_COLS - 1)
        return whole.Rows(RowIndex)

    def Columns(self, ColumnIndex=MISSING):
        whole = Range(self._host, self._id, 0, 0, MAX_ROWS - 1, MAX_COLS - 1)
        if isinstance(ColumnIndex, str) and ":" in ColumnIndex:
            a = parse_area(ColumnIndex)
            if a is None:
                raise VBAError(13)
            return Range(self._host, self._id, 0, a[1], MAX_ROWS - 1, a[3])
        return whole.Columns(ColumnIndex)

    @property
    def UsedRange(self):
        used = _c.used_range(self._index())
        if used is None:
            return Range(self._host, self._id, 0, 0)
        return Range(self._host, self._id, *used)

    def Activate(self):
        _c.activate(self._index())
        return True

    def Select(self, Replace=MISSING):
        _c.activate(self._index())
        return True

    def Delete(self):
        if _c.n_sheets() == 1:
            raise VBAError(1004, "A workbook must contain at least one visible worksheet.")
        _c.remove_sheet(self._index())
        return True

    def _target_place(self, Before, After):
        if isinstance(Before, Worksheet):
            return Before._index()
        if isinstance(After, Worksheet):
            return After._index() + 1
        return -1

    def Copy(self, Before=MISSING, After=MISSING):
        if Before is MISSING and After is MISSING:
            raise VBAError(1004, "Copying a sheet into a new book is not supported; give Before or After")
        to = self._target_place(Before, After)
        i = _c.copy_sheet(self._index(), to, "")
        _c.activate(i)
        return True

    def Move(self, Before=MISSING, After=MISSING):
        if Before is MISSING and After is MISSING:
            raise VBAError(1004, "Moving a sheet into a new book is not supported; give Before or After")
        to = self._target_place(Before, After)
        me = self._index()
        if to > me:
            to -= 1
        _c.move_sheet(me, max(0, min(to, _c.n_sheets() - 1)))
        return True

    def Protect(self, Password=MISSING, DrawingObjects=True, Contents=True, Scenarios=True,
                UserInterfaceOnly=False, AllowFormattingCells=False, AllowFormattingColumns=False,
                AllowFormattingRows=False, AllowInsertingColumns=False, AllowInsertingRows=False,
                AllowInsertingHyperlinks=False, AllowDeletingColumns=False, AllowDeletingRows=False,
                AllowSorting=False, AllowFiltering=False, AllowUsingPivotTables=False):
        _c.protect_sheet(self._index(), True, "" if Password is MISSING else to_str(Password))

    def Unprotect(self, Password=MISSING):
        try:
            _c.protect_sheet(self._index(), False, "" if Password is MISSING else to_str(Password))
        except PermissionError as e:
            raise VBAError(1004, str(e))

    @property
    def ProtectContents(self):
        return _c.protect_sheet(self._index())

    ProtectDrawingObjects = ProtectContents
    ProtectScenarios = ProtectContents

    def Calculate(self):
        _c.calculate()

    @property
    def Next(self):
        i = self._index() + 1
        return self._host.worksheet(i) if i < _c.n_sheets() else None

    @property
    def Previous(self):
        i = self._index() - 1
        return self._host.worksheet(i) if i >= 0 else None

    @property
    def Shapes(self):
        return Shapes(self._host, self._id)

    @property
    def Buttons(self):
        return Shapes(self._host, self._id, ("shape",))

    def ChartObjects(self, Index=MISSING):
        c = Shapes(self._host, self._id, ("chart",))
        return c if Index is MISSING else c.Item(Index)

    @property
    def Hyperlinks(self):
        return Hyperlinks(self._host, self._id)

    @property
    def Comments(self):
        i = self._index()
        used = _c.used_range(i)
        c = vba.Collection()
        if used is not None:
            for r in range(used[0], used[2] + 1):
                for col in range(used[1], used[3] + 1):
                    if _c.get_note(i, r, col) is not None:
                        c.Add(Comment(Range(self._host, self._id, r, col)))
        return c

    @property
    def Names(self):
        return self._host.names

    @property
    def AutoFilterMode(self):
        return _c.get_autofilter(self._index()) is not None

    @AutoFilterMode.setter
    def AutoFilterMode(self, v):
        if not to_bool(v):
            _c.set_autofilter(self._index(), -1, -1, -1, -1)

    @property
    def FilterMode(self):
        i = self._index()
        f = _c.get_autofilter(i)
        return f is not None and any(_c.autofilter_choose(i, c) is not None for c in range(f[1], f[3] + 1))

    @property
    def AutoFilter(self):
        return self if self.AutoFilterMode else None

    def ShowAllData(self):
        i = self._index()
        f = _c.get_autofilter(i)
        if f is None:
            raise VBAError(1004, "ShowAllData method of Worksheet class failed")
        for c in range(f[1], f[3] + 1):
            _c.autofilter_choose(i, c, None)

    @property
    def Sort(self):
        if self._sort is None:
            self._sort = Sort(self._host)
        return self._sort

    @property
    def PageSetup(self):
        if self._page_setup is None:
            self._page_setup = PageSetup(self._host)
        return self._page_setup

    @property
    def StandardWidth(self):
        return round((_c.col_width(self._index(), -1) - 5) / 7.0, 2)

    @StandardWidth.setter
    def StandardWidth(self, v):
        _c.col_width(self._index(), -1, int(round(to_double(v) * 7 + 5)))

    def Paste(self, Destination=MISSING, Link=MISSING):
        clip = self._host.clipboard
        if clip is None:
            raise VBAError(1004, "Paste method of Worksheet class failed")
        dest = Destination if isinstance(Destination, Range) else self._host.app.Selection
        src, cut = clip
        if cut:
            src.Cut(dest)
            self._host.clipboard = None
        else:
            src.Copy(dest)
        return True

    def Evaluate(self, Name):
        return self._host.evaluate(to_str(Name), self)

    @property
    def Tab(self):
        return self

    @property
    def Color(self):
        return 0

    @Color.setter
    def Color(self, v):
        pass

    @property
    def EnableSelection(self):
        return 0

    @EnableSelection.setter
    def EnableSelection(self, v):
        pass

    @property
    def ScrollArea(self):
        return ""

    @ScrollArea.setter
    def ScrollArea(self, v):
        pass

    def PrintOut(self, *args, **kwargs):
        raise VBAError(1004, "Printing from a macro is not supported")

    def PrintPreview(self, EnableChanges=MISSING):
        raise VBAError(1004, "Printing from a macro is not supported")

    @property
    def ListObjects(self):
        return vba.Collection()

    @property
    def PivotTables(self):
        return vba.Collection()

    @property
    def DisplayPageBreaks(self):
        return False

    @DisplayPageBreaks.setter
    def DisplayPageBreaks(self, v):
        pass


class Sheets(_XlObject):
    """Worksheets and Sheets: the book's sheets in tab order, by position
    from 1 or by name."""

    _vba_typename = "Sheets"

    def __init__(self, host):
        self._host = host

    @property
    def Count(self):
        return _c.n_sheets()

    def Item(self, Index):
        if isinstance(Index, VBArray):
            raise VBAError(1004, "Several sheets at once are not supported")
        if isinstance(Index, str):
            i = _c.sheet_index(Index)
            if i < 0:
                for k in range(_c.n_sheets()):
                    if _c.sheet_name(k).lower() == Index.lower():
                        i = k
                        break
            if i < 0:
                raise VBAError(9, "Subscript out of range: there is no sheet named %s" % Index)
            return self._host.worksheet(i)
        i = to_long(Index) - 1
        if i < 0 or i >= _c.n_sheets():
            raise VBAError(9)
        return self._host.worksheet(i)

    def _vba_default_get(self, *args):
        if not args:
            raise VBAError(450)
        return self.Item(args[0])

    def _vba_iter(self):
        return [self._host.worksheet(i) for i in range(_c.n_sheets())]

    def Add(self, Before=MISSING, After=MISSING, Count=1, Type=MISSING):
        n = max(1, to_long(Count))
        if isinstance(Before, Worksheet):
            at = Before._index()
        elif isinstance(After, Worksheet):
            at = After._index() + 1
        else:
            # Excel puts a new sheet before the active one.
            at = _c.current()
        last = None
        for _ in range(n):
            k = 1
            names = {_c.sheet_name(j).lower() for j in range(_c.n_sheets())}
            while ("sheet%d" % k) in names:
                k += 1
            last = _c.add_sheet("Sheet%d" % k, at)
        _c.activate(last)
        return self._host.worksheet(last)

    def Select(self, Replace=MISSING):
        return True

    @property
    def Visible(self):
        return -1

    @Visible.setter
    def Visible(self, v):
        for ws in self._vba_iter():
            ws.Visible = v

    def Copy(self, Before=MISSING, After=MISSING):
        raise VBAError(1004, "Copying all the sheets into a new book is not supported")

    def PrintOut(self, *args, **kwargs):
        raise VBAError(1004, "Printing from a macro is not supported")


class DocumentProperty(_XlObject):
    _vba_typename = "DocumentProperty"

    def __init__(self, host, key):
        self._host = host
        self._key = key

    @property
    def Name(self):
        return self._key.capitalize()

    @property
    def Value(self):
        return _c.properties().get(self._key, "")

    @Value.setter
    def Value(self, v):
        _c.set_property(self._key, to_str(v))

    def _vba_default_get(self, *args):
        return self.Value

    def _vba_default_let(self, value, *args):
        self.Value = value


class DocumentProperties(_XlObject):
    _KEYS = {"title": "title", "subject": "subject", "author": "author", "manager": "manager",
             "company": "company", "category": "category", "keywords": "keywords",
             "comments": "comments"}

    def __init__(self, host):
        self._host = host

    def Item(self, Index):
        key = self._KEYS.get(to_str(Index).lower())
        if key is None:
            raise VBAError(5)
        return DocumentProperty(self._host, key)

    def _vba_default_get(self, *args):
        return self.Item(args[0])


class Workbook(_XlObject):
    _vba_typename = "Workbook"

    def __init__(self, host):
        self._host = host
        self._vba_module = None

    @property
    def Name(self):
        path = _c.path()
        return os.path.basename(path) if path else "Book1"

    @property
    def FullName(self):
        return _c.path() or self.Name

    @property
    def Path(self):
        path = _c.path()
        return os.path.dirname(os.path.abspath(path)) if path else ""

    @property
    def CodeName(self):
        return _c.codename()

    @property
    def Worksheets(self):
        return self._host.sheets

    Sheets = Worksheets

    @property
    def ActiveSheet(self):
        return self._host.worksheet(_c.current())

    @property
    def Names(self):
        return self._host.names

    @property
    def Parent(self):
        return self._host.app

    @property
    def Saved(self):
        return not _c.modified()

    @Saved.setter
    def Saved(self, v):
        _c.modified(not to_bool(v))

    @property
    def ReadOnly(self):
        return False

    @property
    def FileFormat(self):
        name = self.Name.lower()
        return 52 if name.endswith(".xlsm") else 56 if name.endswith(".xls") else 51

    @property
    def HasVBProject(self):
        return True

    def Save(self):
        try:
            _c.save(None)
        except RuntimeError as e:
            raise VBAError(1004, str(e))

    def SaveAs(self, Filename=MISSING, FileFormat=MISSING, *rest, **named):
        if Filename is MISSING:
            raise VBAError(449)
        path = to_str(Filename)
        if FileFormat is not MISSING and not os.path.splitext(path)[1]:
            ext = {51: ".xlsx", 52: ".xlsm", 56: ".xls", 6: ".csv", 62: ".csv", 60: ".ods", 44: ".html"}.get(
                to_long(FileFormat), "")
            path += ext
        try:
            _c.save(path)
        except RuntimeError as e:
            raise VBAError(1004, str(e))

    def SaveCopyAs(self, Filename):
        raise VBAError(1004, "SaveCopyAs is not supported; use SaveAs")

    def Close(self, SaveChanges=MISSING, Filename=MISSING, RouteWorkbook=MISSING):
        if SaveChanges is not MISSING and to_bool(SaveChanges):
            if Filename is not MISSING:
                self.SaveAs(Filename)
            else:
                self.Save()
        elif SaveChanges is not MISSING:
            _c.modified(False)
        try:
            _c.close()
        except RuntimeError:
            pass

    def Activate(self):
        return True

    def Protect(self, Password=MISSING, Structure=True, Windows=False):
        _c.book_protected(True)

    def Unprotect(self, Password=MISSING):
        _c.book_protected(False)

    @property
    def ProtectStructure(self):
        return _c.book_protected()

    @property
    def BuiltinDocumentProperties(self):
        return DocumentProperties(self._host)

    def RefreshAll(self):
        _c.calculate()

    @property
    def Charts(self):
        return vba.Collection()

    @property
    def Windows(self):
        return vba.Collection()

    def PrintOut(self, *args, **kwargs):
        raise VBAError(1004, "Printing from a macro is not supported")

    def ExportAsFixedFormat(self, Type, Filename=MISSING, *rest, **named):
        if Filename is MISSING:
            raise VBAError(449)
        path = to_str(Filename)
        if not path.lower().endswith(".pdf"):
            path += ".pdf"
        _c.save(path)


class Workbooks(_XlObject):
    _vba_typename = "Workbooks"

    def __init__(self, host):
        self._host = host

    @property
    def Count(self):
        return 1

    def Item(self, Index):
        wb = self._host.workbook
        if isinstance(Index, str):
            want = Index.lower()
            name = wb.Name.lower()
            if want != name and os.path.splitext(want)[0] != os.path.splitext(name)[0]:
                raise VBAError(9, "Subscript out of range: a macro reaches only its own book")
            return wb
        if to_long(Index) != 1:
            raise VBAError(9)
        return wb

    def _vba_default_get(self, *args):
        return self.Item(args[0])

    def _vba_iter(self):
        return [self._host.workbook]

    def Open(self, Filename, *rest, **named):
        raise VBAError(1004, "Workbooks.Open is not supported: office42 opens other books in windows of their "
                             "own, and a macro works on the book it is in")

    def Add(self, Template=MISSING):
        raise VBAError(1004, "Workbooks.Add is not supported: a macro works on the book it is in")

    def Close(self):
        self._host.workbook.Close()


class Window(_XlObject):
    _vba_typename = "Window"

    def __init__(self, host):
        self._host = host
        self.__dict__.update(dict(Zoom=100, DisplayGridlines=True, DisplayHeadings=True,
                                  DisplayZeros=True, DisplayWorkbookTabs=True, WindowState=-4137,
                                  DisplayFormulas=False, DisplayHorizontalScrollBar=True,
                                  DisplayVerticalScrollBar=True, ScrollRow=1, ScrollColumn=1,
                                  Visible=True, SplitRow=0, SplitColumn=0))

    @property
    def FreezePanes(self):
        return _c.frozen(_c.current()) != (0, 0)

    @FreezePanes.setter
    def FreezePanes(self, v):
        i = _c.current()
        if not to_bool(v):
            _c.frozen(i, 0, 0)
            return
        try:
            cell = self._host.app.ActiveCell
            _c.frozen(i, cell._r0, cell._c0)
        except VBAError:
            _c.frozen(i, 1, 0)

    @property
    def Caption(self):
        return self._host.workbook.Name

    @property
    def ActiveCell(self):
        return self._host.app.ActiveCell

    @property
    def ActiveSheet(self):
        return self._host.app.ActiveSheet

    @property
    def Selection(self):
        return self._host.app.Selection

    def Activate(self):
        return True

    def SmallScroll(self, Down=0, Up=0, ToRight=0, ToLeft=0):
        return True

    def ScrollWorkbookTabs(self, Sheets=0, Position=0):
        return True


class WorksheetFunction(_XlObject):
    """Application.WorksheetFunction: any of the spreadsheet's functions,
    worked out as a formula over the arguments."""

    _vba_typename = "WorksheetFunction"

    def __init__(self, host, raise_errors=True):
        self._host = host
        self._raise = raise_errors

    def _vba_get(self, name, *args):
        return self._host.worksheet_function(name, args, self._raise)

    def Transpose(self, Arg1):
        return transpose(Arg1)


def transpose(v):
    v = plain_value(v) if isinstance(v, (Range, MultiRange)) else v
    if not isinstance(v, VBArray):
        return v
    if len(v.dims) == 1:
        lb, ub = v.dims[0]
        return VBArray([(1, ub - lb + 1), (1, 1)], data=list(v.data))
    rows = v.rows()
    return VBArray.from_rows([list(col) for col in zip(*rows)] if rows else [])


class Application(_XlObject):
    """What a macro reaches without saying where: the Application's
    members are Visual Basic's global names."""

    _vba_typename = "Application"

    def __init__(self, host):
        self._host = host
        self.__dict__.update(dict(ScreenUpdating=True, DisplayAlerts=True, Interactive=True,
                                  Cursor=-4143, DisplayStatusBar=True, EnableAnimations=True,
                                  AskToUpdateLinks=True, Visible=True, EnableCancelKey=1,
                                  DisplayFormulaBar=True, WindowState=-4137, IgnoreRemoteRequests=False,
                                  CopyObjectsWithCells=True, AutoRecover=None, MoveAfterReturn=True,
                                  MoveAfterReturnDirection=-4121, DisplayFullScreen=False,
                                  EnableAutoComplete=True, ShowWindowsInTaskbar=True,
                                  DisplayCommentIndicator=-1, PrintCommunication=True,
                                  AutomationSecurity=1, FeatureInstall=0, StandardFontSize=11.0,
                                  StandardFont="Calibri", UserControl=True, CalculateBeforeSave=True,
                                  Iteration=False, MaxIterations=100, MaxChange=0.001,
                                  UseSystemSeparators=True, DecimalSeparator=".",
                                  ThousandsSeparator=","))
        self._status = False
        self._window = None

    def __repr__(self):
        return "<Application>"

    # -- the book and where the user is --
    @property
    def ActiveWorkbook(self):
        return self._host.workbook

    ThisWorkbook = ActiveWorkbook

    @property
    def Workbooks(self):
        return self._host.workbooks

    @property
    def ActiveSheet(self):
        return self._host.worksheet(_c.current())

    @property
    def Worksheets(self):
        return self._host.sheets

    Sheets = Worksheets

    @property
    def Names(self):
        return self._host.names

    @property
    def ActiveWindow(self):
        if self._window is None:
            self._window = Window(self._host)
        return self._window

    @property
    def Windows(self):
        c = vba.Collection()
        c.Add(self.ActiveWindow)
        return c

    def _selection(self):
        try:
            return _c.selection()
        except RuntimeError as e:
            raise VBAError(1004, str(e))

    @property
    def Selection(self):
        i, r0, c0, r1, c1, _, _ = self._selection()
        return Range(self._host, _c.sheet_id(i), r0, c0, r1, c1)

    @property
    def ActiveCell(self):
        i, _, _, _, _, row, col = self._selection()
        return Range(self._host, _c.sheet_id(i), row, col)

    def Range(self, Cell1, Cell2=MISSING):
        return self._host.range_of(Cell1, Cell2)

    def Cells(self, RowIndex=MISSING, ColumnIndex=MISSING):
        return self.ActiveSheet.Cells(RowIndex, ColumnIndex)

    def Rows(self, RowIndex=MISSING):
        return self.ActiveSheet.Rows(RowIndex)

    def Columns(self, ColumnIndex=MISSING):
        return self.ActiveSheet.Columns(ColumnIndex)

    # -- the program --
    @property
    def Name(self):
        return "Microsoft Excel"

    @property
    def Version(self):
        return "16.0"

    @property
    def Build(self):
        return 16130

    @property
    def OperatingSystem(self):
        return "office42 on " + sys.platform

    @property
    def UserName(self):
        try:
            return getpass.getuser()
        except Exception:
            return "User"

    @UserName.setter
    def UserName(self, v):
        pass

    @property
    def OrganizationName(self):
        return ""

    @property
    def Path(self):
        return os.path.dirname(os.path.abspath(sys.executable))

    @property
    def DefaultFilePath(self):
        return os.path.expanduser("~")

    @DefaultFilePath.setter
    def DefaultFilePath(self, v):
        pass

    @property
    def PathSeparator(self):
        return os.sep

    @property
    def StartupPath(self):
        return office42.personal_folder()

    def International(self, Index):
        return {3: ".", 4: ",", 5: ",", 1: 1, 2: 1}.get(to_long(Index), "")

    @property
    def Calculation(self):
        return -4135 if _c.manual() else -4105

    @Calculation.setter
    def Calculation(self, v):
        _c.manual(to_long(v) == -4135)

    @property
    def EnableEvents(self):
        return self._host.events_enabled

    @EnableEvents.setter
    def EnableEvents(self, v):
        self._host.events_enabled = to_bool(v)

    @property
    def StatusBar(self):
        return self._status

    @StatusBar.setter
    def StatusBar(self, v):
        if v is False or v is EMPTY:
            self._status = False
            _c.status("")
        else:
            self._status = to_str(v)
            _c.status(self._status)

    @property
    def CutCopyMode(self):
        clip = self._host.clipboard
        return False if clip is None else (2 if clip[1] else 1)

    @CutCopyMode.setter
    def CutCopyMode(self, v):
        if not to_bool(v):
            self._host.clipboard = None

    @property
    def Caller(self):
        if self._host.callers:
            return self._host.callers[-1]
        return ErrValue(2023)

    ThisCell = Caller

    @property
    def WorksheetFunction(self):
        return self._host.wsf

    @property
    def Dialogs(self):
        raise VBAError(1004, "Excel's built-in dialogs are not available to macros in office42")

    @property
    def CommandBars(self):
        raise VBAError(1004, "Command bars are not available to macros in office42")

    @property
    def VBE(self):
        raise VBAError(1004, "The Visual Basic Editor's object model is not available")

    @property
    def FileDialog(self):
        raise VBAError(1004, "FileDialog is not available to macros in office42")

    @property
    def ActivePrinter(self):
        return ""

    @ActivePrinter.setter
    def ActivePrinter(self, v):
        pass

    # -- what it does --
    def Calculate(self):
        _c.calculate()

    CalculateFull = Calculate
    CalculateFullRebuild = Calculate

    def Evaluate(self, Name):
        return self._host.evaluate(to_str(Name) if not isinstance(Name, Range) else Name.Address())

    def Run(self, Macro, *args):
        name = to_str(Macro)
        if "!" in name:
            name = name.split("!", 1)[1]
        return self._host.project.run(name.strip("'"), args)

    def InputBox(self, Prompt, Title=MISSING, Default=MISSING, Left=MISSING, Top=MISSING,
                 HelpFile=MISSING, HelpContextID=MISSING, Type=MISSING):
        default = "" if Default is MISSING else to_str(Default.Address() if isinstance(Default, Range)
                                                         else Default)
        answer = self._host.inputbox(to_str(Prompt), "Input" if Title is MISSING else to_str(Title), default)
        if answer is None:
            return False
        t = 2 if Type is MISSING else to_long(Type)
        if t & 8:
            try:
                return self._host.range_of(answer)
            except VBAError:
                raise VBAError(424, "The reference is not valid")
        if t & 1:
            n = vba.parse_number(answer)
            if n is None:
                got = self._host.evaluate(answer)
                if isinstance(got, (int, float)):
                    return float(got)
                raise VBAError(13, "Number is not valid")
            return n
        if t & 4:
            return to_bool(answer)
        if t == 0:
            return "=" + answer if not answer.startswith("=") else answer
        return answer

    def Intersect(self, Arg1, Arg2, *more):
        result = Arg1
        for other in (Arg2,) + more:
            if other is MISSING or other is None:
                continue
            if result is None:
                return None
            if not isinstance(result, Range) or not isinstance(other, Range):
                raise VBAError(13)
            if result._id != other._id:
                raise VBAError(1004)
            r0, c0 = max(result._r0, other._r0), max(result._c0, other._c0)
            r1, c1 = min(result._r1, other._r1), min(result._c1, other._c1)
            if r0 > r1 or c0 > c1:
                return None
            result = result._sub(r0, c0, r1, c1)
        return result

    def Union(self, Arg1, Arg2, *more):
        ranges = []
        for r in (Arg1, Arg2) + more:
            if r is MISSING or r is None:
                continue
            if isinstance(r, MultiRange):
                ranges.extend(r._ranges)
            elif isinstance(r, Range):
                ranges.append(r)
            else:
                raise VBAError(13)
        if not ranges:
            return None
        first = ranges[0]
        # Rectangles that make one rectangle together are one.
        r0 = min(r._r0 for r in ranges)
        c0 = min(r._c0 for r in ranges)
        r1 = max(r._r1 for r in ranges)
        c1 = max(r._c1 for r in ranges)
        cells = set()
        for r in ranges:
            if (r._r1 - r._r0 + 1) * (r._c1 - r._c0 + 1) > 100000:
                break
            for rr in range(r._r0, r._r1 + 1):
                for cc in range(r._c0, r._c1 + 1):
                    cells.add((rr, cc))
        else:
            if len(cells) == (r1 - r0 + 1) * (c1 - c0 + 1):
                return first._sub(r0, c0, r1, c1)
        return MultiRange(self._host, ranges)

    def Wait(self, Time):
        target = vba.to_date(Time)
        seconds = (float(target) - float(vba.f_now())) * 86400
        time.sleep(max(0.0, min(seconds, 60.0)))
        return True

    def OnTime(self, EarliestTime, Procedure, LatestTime=MISSING, Schedule=True):
        raise VBAError(1004, "Application.OnTime is not supported in office42")

    def OnKey(self, Key, Procedure=MISSING):
        return True

    def Quit(self):
        self._host.workbook.Close()

    def Goto(self, Reference=MISSING, Scroll=False):
        if isinstance(Reference, Range):
            Reference.Parent.Activate()
            Reference.Select()
        elif Reference is not MISSING:
            self._host.range_of(Reference).Select()

    def GetOpenFilename(self, FileFilter=MISSING, FilterIndex=MISSING, Title=MISSING, ButtonText=MISSING,
                        MultiSelect=False):
        answer = self._host.inputbox("File to open:", "Open" if Title is MISSING else to_str(Title), "")
        return False if not answer else answer

    def GetSaveAsFilename(self, InitialFileName=MISSING, FileFilter=MISSING, FilterIndex=MISSING,
                          Title=MISSING, ButtonText=MISSING):
        answer = self._host.inputbox("Save as:", "Save As" if Title is MISSING else to_str(Title),
                                     "" if InitialFileName is MISSING else to_str(InitialFileName))
        return False if not answer else answer

    def ConvertFormula(self, Formula, FromReferenceStyle, ToReferenceStyle=MISSING, ToAbsolute=MISSING,
                       RelativeTo=MISSING):
        text = to_str(Formula)
        base = RelativeTo if isinstance(RelativeTo, Range) else None
        row, col = (base._r0, base._c0) if base is not None else (0, 0)
        frm = to_long(FromReferenceStyle)
        to = frm if ToReferenceStyle is MISSING else to_long(ToReferenceStyle)
        if frm == -4150 and to == 1:
            return r1c1_to_a1(text, row, col)
        if frm == 1 and to == -4150:
            return a1_to_r1c1(text, row, col)
        return text

    def Volatile(self, Volatile=True):
        return True

    def Transpose(self, Arg1):
        return transpose(Arg1)

    def DoEvents(self):
        return 0

    def MailSystem(self):
        return 0

    def _vba_get(self, name, *args):
        # Application.Sum, Application.VLookup: a worksheet function that
        # gives an error value rather than raising one.
        return self._host.worksheet_function(name, args, False)


# ---- The host -----------------------------------------------------------------------

class ExcelHost(vba.Host):
    """The language's view of the book: Excel's objects for its global
    names, and the window for its message boxes."""

    object_types = ("range", "worksheet", "workbook", "application", "font", "interior", "borders",
                    "border", "shape", "shapes", "name", "names", "comment", "hyperlink",
                    "worksheetfunction", "sheets", "workbooks", "window", "chartobject", "chart",
                    "listobject", "validation", "areas", "object")

    def __init__(self):
        self.app = Application(self)
        self.workbook = Workbook(self)
        self.workbooks = Workbooks(self)
        self.sheets = Sheets(self)
        self.names = Names(self)
        self.wsf = WorksheetFunction(self, True)
        self._sheet_objects = {}
        self.clipboard = None
        self.last_find = None
        self.callers = []
        self.events_enabled = True
        self.project = None
        self.conditions = {}

    # -- sheets --
    def worksheet(self, index):
        return self.worksheet_by_id(_c.sheet_id(index))

    def worksheet_by_id(self, sid):
        ws = self._sheet_objects.get(sid)
        if ws is None:
            ws = self._sheet_objects[sid] = Worksheet(self, sid)
        if ws._vba_module is None and self.project is not None:
            ws._vba_module = self.sheet_module(ws)
        return ws

    def sheet_module(self, ws):
        """The document module a sheet's code name names."""
        try:
            index = ws._index()
        except VBAError:
            return None
        code = _c.codename(index)
        project = self.project
        if code:
            m = project.modules.get(code.lower())
            if m is not None and m.kind == "document":
                return m
        # A file that kept no code names: Sheet1's module is the first
        # sheet's, and so on.
        docs = [m for m in project.order if m.kind == "document" and m.lname != _c.codename().lower()]
        name = _c.sheet_name(index).lower()
        for m in docs:
            if m.lname == name:
                return m
        if not code and index < len(docs):
            taken = {(_c.codename(k) or "").lower() for k in range(_c.n_sheets())}
            if docs[index].lname not in taken:
                return docs[index]
        return None

    def find_sheet_for_module(self, module):
        for i in range(_c.n_sheets()):
            ws = self.worksheet(i)
            if ws._vba_module is module:
                return ws
        return None

    # -- names Visual Basic asks the host for --
    def constant(self, lname):
        return XL.get(lname, MISSING)

    def has_global(self, lname):
        return lname in GLOBAL_NAMES

    def document(self, name):
        lname = name.lower()
        if lname == _c.codename().lower() or lname == "thisworkbook":
            wb = self.workbook
            if wb._vba_module is None and self.project is not None:
                wb._vba_module = self.project.modules.get(lname)
            return wb
        m = self.project.modules.get(lname) if self.project is not None else None
        if m is not None:
            ws = self.find_sheet_for_module(m)
            if ws is not None:
                return ws
        raise VBAError(424, "The sheet the module %s belongs to is not in the book" % name)

    def document_has(self, module, lname):
        if module.lname == _c.codename().lower() or module.lname == "thisworkbook":
            return vba.python_member(self.workbook, lname) is not None
        return lname in _WORKSHEET_MEMBERS

    def create_object(self, progid):
        if progid in ("excel.application", "excel.application.16"):
            return self.app
        return None

    def get_object(self, path, progid):
        if progid and progid.lower().startswith("excel.application"):
            return self.app
        raise VBAError(429)

    def typename(self, obj):
        return getattr(obj, "_vba_typename", type(obj).__name__)

    # -- the window --
    def msgbox(self, prompt, buttons, title):
        sets = {0: (["OK"], [1]), 1: (["OK", "Cancel"], [1, 2]),
                2: (["Abort", "Retry", "Ignore"], [3, 4, 5]),
                3: (["Yes", "No", "Cancel"], [6, 7, 2]), 4: (["Yes", "No"], [6, 7]),
                5: (["Retry", "Cancel"], [4, 2])}
        labels, codes = sets.get(buttons & 7, sets[0])
        if not self.app.DisplayAlerts and buttons & 7 == 0:
            pass
        try:
            if len(labels) == 1:
                _c.message(prompt)
                return 1
            answer = _c.ask(prompt, title, labels)
        except RuntimeError:
            print(prompt)
            return codes[0]
        if answer is None or answer < 0:
            # Closed without a button: Cancel where there is one.
            return 2 if 2 in codes else codes[-1]
        return codes[answer] if answer < len(codes) else codes[0]

    def inputbox(self, prompt, title, default):
        try:
            return _c.input(prompt, default)
        except RuntimeError:
            return None

    def format_number(self, code, n):
        if re.search(r"[nN]", re.sub(r'"[^"]*"|\\.', "", code)) and not re.search(r"[0#]", code):
            return None
        try:
            return _c.format_number(code, n)
        except Exception:
            return None

    def declare_call(self, node, values):
        name = (node.alias or node.name).lower().lstrip("_")
        if name in ("sleep",):
            ms = to_double(values[0]) if values else 0
            time.sleep(max(0.0, min(ms / 1000.0, 60.0)))
            return 0
        if name in ("gettickcount", "gettickcount64", "timegettime"):
            return int(time.monotonic() * 1000) & 0x7FFFFFFF
        if name in ("getcurrentprocessid",):
            return os.getpid()
        return vba.Host.declare_call(self, node, values)

    # -- ranges from text --
    def define_name(self, name, refers):
        text = refers[1:] if refers.startswith("=") else refers
        sheet, ref = split_sheet(text)
        area = parse_area(ref) if sheet is not None or parse_area(ref) is not None else None
        if area is not None:
            index = _c.current() if sheet is None else _c.sheet_index(sheet)
            if index < 0:
                raise VBAError(1004, "There is no sheet named %s" % sheet)
            _c.define_name(name, index, *area)
        else:
            _c.define_name(name, text)

    def parse_range(self, text, ws=None):
        """A1, Sheet1!A1:B2, A1:B2,D4, a defined name: as a Range."""
        text = to_str(text).strip()
        if "," in text and not text.startswith("'"):
            parts = [p for p in text.split(",") if p.strip()]
            ranges = [self.parse_range(p, ws) for p in parts]
            return ranges[0] if len(ranges) == 1 else MultiRange(self, ranges)
        sheet, ref = split_sheet(text)
        if sheet is not None:
            index = _c.sheet_index(sheet)
            if index < 0:
                raise VBAError(1004, "Method 'Range' of object '_Global' failed: no sheet named %s" % sheet)
            sid = _c.sheet_id(index)
        else:
            sid = ws._id if ws is not None else _c.sheet_id(_c.current())
        area = parse_area(ref)
        if area is not None:
            return Range(self, sid, *area)
        # Two ends joined by a colon, one of them a name.
        info = _c.name_info(ref)
        if isinstance(info, tuple):
            i, r0, c0, r1, c1 = info
            return Range(self, _c.sheet_id(i), r0, c0, r1, c1)
        if isinstance(info, str):
            got = self.evaluate(info.lstrip("="))
            if isinstance(got, Range):
                return got
        raise VBAError(1004, "Method 'Range' of object '_Global' failed: %s is not a reference" % text)

    def range_of(self, cell1, cell2=MISSING, ws=None):
        """Range(Cell1, Cell2) as Excel takes it: an address, a range, or
        two ends of a rectangle."""
        a = cell1 if isinstance(cell1, (Range, MultiRange)) else self.parse_range(cell1, ws)
        if cell2 is MISSING:
            return a
        b = cell2 if isinstance(cell2, Range) else self.parse_range(cell2, ws)
        if isinstance(a, MultiRange) or isinstance(b, MultiRange):
            raise VBAError(1004)
        return Range(self, a._id, min(a._r0, b._r0), min(a._c0, b._c0), max(a._r1, b._r1), max(a._c1, b._c1))

    def evaluate(self, text, ws=None):
        """Evaluate("...") and [...]: a reference is a Range, a name what
        it names, anything else a formula's value."""
        t = text.strip()
        if t.startswith("="):
            t = t[1:]
        try:
            return self.parse_range(t, ws)
        except VBAError:
            pass
        index = ws._index() if ws is not None else _c.current()
        got = _c.evaluate("=" + t, index)
        return from_python(got)

    # -- worksheet functions --
    def formula_arg(self, v):
        if isinstance(v, Range):
            return quote_sheet(v.Parent.Name) + "!" + v.Address()
        if isinstance(v, MultiRange):
            return "(" + ",".join(self.formula_arg(r) for r in v._ranges) + ")"
        if isinstance(v, VBArray):
            rows = v.rows() if len(v.dims) <= 2 else [list(v.data)]
            return "{" + ";".join(",".join(self.formula_arg(x) for x in row) for row in rows) + "}"
        if v is MISSING:
            return ""
        if is_object(v) and v is not None:
            v = plain_value(v)
        t = type(v)
        if t is str:
            return '"' + v.replace('"', '""') + '"'
        if t is bool:
            return "TRUE" if v else "FALSE"
        if t is int:
            return str(v)
        if t is float or t is VBDate:
            return repr(float(v))
        if v is EMPTY or v is None:
            return "0"
        if isinstance(v, ErrValue):
            return ERROR_NAMES.get(v.code, "#VALUE!")
        return '"' + to_str(v).replace('"', '""') + '"'

    def worksheet_function(self, name, args, raise_errors):
        lname = name.lower()
        if lname == "transpose":
            return transpose(args[0])
        fname = name.upper()
        if not _c.is_builtin(fname) and fname not in _c.function_names():
            raise VBAError(438, "Object doesn't support this property or method (%s)" % name)
        formula = "=%s(%s)" % (fname, ",".join(self.formula_arg(a) for a in args))
        got = from_python(_c.evaluate(formula, _c.current()))
        if isinstance(got, ErrValue) and raise_errors:
            raise VBAError(1004, "Unable to get the %s property of the WorksheetFunction class" % name)
        return got


# What Excel's type library lets a macro say without Application. in
# front: the rest of the Application's members want it.
GLOBAL_NAMES = {"activecell", "activechart", "activeprinter", "activesheet", "activewindow",
                "activeworkbook", "addins", "application", "calculate", "cells", "charts", "columns",
                "commandbars", "evaluate", "intersect", "names", "range", "rows", "run", "selection",
                "sheets", "thisworkbook", "union", "windows", "workbooks", "worksheetfunction",
                "worksheets"}

# The members a sheet's own module can use unqualified: Range means
# Me.Range there, not the active sheet's.
_WORKSHEET_MEMBERS = {n.lower() for n in dir(Worksheet) if not n.startswith("_")}


# ---- The book's project ---------------------------------------------------------------

class BookState:
    """A book's compiled project, and whether its macros are live."""

    def __init__(self):
        self.serial = None
        self.project = None
        self.host = None
        self.enabled = False
        self.functions = set()


_books = {}


def _forget(owner):
    _books.pop(owner, None)


office42._forget_hooks.append(_forget)


def _poll(seconds):
    """vba's poll: the window draws what the macro has done and says
    whether Esc or Ctrl+Break was pressed, which does what
    Application.EnableCancelKey says -- nothing, error 18 for the
    macro's handler, or Excel's question whether to go on."""
    project = vba.current
    app = project.host.app if project is not None and isinstance(project.host, ExcelHost) else None
    draw = app is None or bool(app.__dict__.get("ScreenUpdating", True))
    pressed = _c.poll(seconds, draw)
    if pressed is None:
        vba.set_poll(None)      # no window to listen: office42-calc
        return
    if not pressed or seconds < 0:
        return
    try:
        mode = vba.to_long(app.__dict__.get("EnableCancelKey", 1)) if app is not None else 1
    except VBAError:
        mode = 1
    if mode == 0:
        return
    if mode == 2:
        raise VBAError(18)
    answer = _c.ask("Code execution has been interrupted", "Microsoft Visual Basic", ["Continue", "End"])
    if answer == 1:
        raise vba.EndSignal()


vba.set_poll(_poll)


def state():
    """The project of the book being run against, compiled afresh when a
    module has changed since."""
    owner = _c.book_id()
    st = _books.get(owner)
    if st is None:
        st = _books[owner] = BookState()
    serial = _c.vba_serial()
    if st.project is None or st.serial != serial:
        host = ExcelHost()
        project = vba.Project(host)
        host.project = project
        vba._OBJECT_TYPES.update(host.object_types)
        for name, kind, code in _c.vba_modules():
            project.add_module(name, "class" if kind == "form" else kind, code)
        st.serial = serial
        st.host = host
        st.project = project
        project.compile()
        if st.enabled:
            register(st)
    vba.current = st.project
    return st


def error_text(e, project=None):
    """An error as Excel's box says it, and where it happened."""
    if isinstance(e, vba.VBASyntaxError):
        text = "Compile error in %s, line %d:\n%s" % (e.module or "the project", e.line, e.message)
        source = _source_line(project, e.module, e.line)
        return text + ("\n    " + source if source else "")
    text = "Run-time error '%d':\n%s" % (e.number, e.description)
    if e.where:
        module, proc, line = e.where
        text += "\n(in %s.%s, line %d)" % (module, proc, line)
        source = _source_line(project, module, line)
        if source:
            text += "\n    " + source
    return text


def _source_line(project, module, line):
    if project is None or not module:
        return ""
    m = project.modules.get(module.lower())
    if m is None:
        return ""
    lines = m.text.split("\n")
    return lines[line - 1].strip() if 0 < line <= len(lines) else ""


def _run(fn):
    """Runs fn, gathering what it printed and what Debug.Print wrote,
    and turning an error into its message: (ok, output)."""
    out = io.StringIO()
    ok = True
    project = None
    with redirect_stdout(out), redirect_stderr(out):
        try:
            st = state()
            project = st.project
            fn(st)
        except (vba.VBASyntaxError, VBAError) as e:
            ok = False
            if project is not None:
                out.write(project.take_output())
            print(error_text(e, project))
            project = None
        except vba.StopSignal:
            print("Stopped.")
        except vba.EndSignal:
            pass
        except Exception:
            ok = False
            import traceback
            print("Internal error in office42's Visual Basic:\n" + traceback.format_exc())
    text = (project.take_output() if project is not None else "") + out.getvalue()
    return ok, text


def run_macro(name):
    """Tools > Macro > Macros > Run, a button's macro, a shortcut key."""
    def go(st):
        st.project.run(name)
    return _run(go)


def immediate(line):
    """A line typed into the Immediate window."""
    def go(st):
        st.project.immediate(line)
    return _run(go)


def check():
    """Debug > Compile VBAProject: (True, "") or (False, "module\\tline\\tmessage")
    for the first module that does not compile."""
    try:
        errors = state().project.errors
    except vba.VBASyntaxError as e:
        errors = [e]
    if errors:
        e = errors[0]
        return False, "%s\t%d\t%s" % (e.module or "", e.line, e.message)
    return True, ""


def debug_macro(name, breakpoints, step_first):
    """A macro run a line at a time, or to its breakpoints."""
    stops = set()
    for b in breakpoints:
        module, _, line = b.rpartition(":")
        try:
            stops.add((module.lower(), int(line)))
        except ValueError:
            continue
    mode = {"step": bool(step_first)}

    def trace(fr, line, stopped=False):
        module = fr.proc.module.name
        if mode["step"] or stopped or (module.lower(), line) in stops:
            command = _c.debug_pause(line, locals_text(fr), module)
            if command == 2:
                raise vba.StopSignal()
            mode["step"] = command == 1

    def go(st):
        vba.set_trace(trace)
        try:
            st.project.run(name)
        finally:
            vba.set_trace(None)
    return _run(go)


def locals_text(fr):
    """The frame's variables, one a line, for the stepping pane."""
    lines = []
    proc = fr.proc
    if proc.is_function:
        lines.append("%s = %s" % (proc.name, _shown(fr.v[0])))
    for name, local in sorted(getattr(proc, "local_names", []), key=lambda x: x[0].lower()):
        try:
            if local.static:
                value = proc.statics[local.slot]
            else:
                value = fr.v[local.slot]
                if isinstance(value, (vba.Ref, vba.Cell)):
                    value = value.get()
        except Exception:
            continue
        lines.append("%s = %s" % (name, _shown(value)))
    return "\n".join(lines)


def _shown(v):
    try:
        if isinstance(v, str):
            text = '"%s"' % v
        elif isinstance(v, VBArray):
            text = "Array(%s)" % ", ".join("%s..%s" % d for d in v.dims)
        elif v is None:
            text = "Nothing"
        elif is_object(v):
            text = "<%s>" % vba.typename(v)
        else:
            text = vba.print_text(v).strip() or repr(v)
    except Exception:
        text = "?"
    return text if len(text) <= 70 else text[:67] + "..."


# -- making the macros live --

def enable():
    """The user has let the book's macros run: its functions go to the
    cells, and its event procedures listen."""
    def go(st):
        st.enabled = True
        register(st)
    ok, text = _run(go)
    try:
        _c.calculate()
    except Exception:
        pass
    return ok, text


def public_functions(project):
    for m in project.order:
        if m.kind != "standard" or m.private_module:
            continue
        for group in m.procs.values():
            proc = group.get("function")
            if proc is not None and not proc.private:
                yield proc


def register(st):
    owner = _c.book_id()
    project = st.project
    functions = office42._book_functions.setdefault(owner, {})
    wanted = {}
    for proc in public_functions(project):
        fname = proc.name.upper()
        if _c.is_builtin(fname):
            continue
        wanted[fname] = proc
    for fname in list(st.functions):
        if fname not in wanted:
            functions.pop(fname, None)
            if not office42._defined_elsewhere(fname, owner):
                _c.undefine(fname)
    for fname, proc in wanted.items():
        functions[fname] = _cell_function(proc.module.name, proc.name)
        signature = "%s(%s)" % (fname, ", ".join(p.name for p in proc.params))
        _c.define(fname, proc.min_args(), proc.max_args(), signature,
                  "A function from the book's Visual Basic module %s." % proc.module.name)
    st.functions = set(wanted)
    for event, fn in (("change", _on_change), ("selection", _on_selection), ("before_save", _on_before_save),
                      ("open", _on_open), ("close", _on_close)):
        office42.on(event, fn, name="vba:" + event)


def _cell_function(module, name):
    """What a cell's =NAME(...) calls: the arguments made Visual Basic's,
    ranges as Range objects, and the Function run for the calling cell."""
    def call(args, refs, caller):
        st = state()
        project = st.project
        host = st.host
        proc = project.find_proc(module + "." + name, ("function",))
        if proc is None:
            return office42.Error("#NAME?")
        values = []
        for k, a in enumerate(args):
            ref = refs[k] if refs is not None and k < len(refs) else None
            if ref is not None:
                i, r0, c0, r1, c1 = ref
                values.append(Range(host, _c.sheet_id(i), r0, c0, min(r1, MAX_ROWS - 1), min(c1, MAX_COLS - 1)))
            else:
                values.append(from_python(a))
        if caller is not None:
            i, row, col = caller
            host.callers.append(Range(host, _c.sheet_id(i), row, col))
        try:
            result = project.call(proc, values)
        except VBAError:
            return office42.Error("#VALUE!")
        finally:
            if caller is not None:
                host.callers.pop()
            project.take_output()
        return to_python(result)
    call._wants_refs = True
    call.__name__ = name
    return call


def _call_event(st, module, proc_name, args, me):
    if module is None:
        return
    proc = module.procs.get(proc_name, {}).get("sub")
    if proc is None:
        return
    project = st.project
    try:
        project.call(proc, args, me)
    except (VBAError, vba.VBASyntaxError, vba.StopSignal) as e:
        _debug_printed(project.take_output())
        if not isinstance(e, vba.StopSignal):
            print(error_text(e, project))
        return
    _debug_printed(project.take_output())


def _debug_printed(text):
    """Debug.Print's lines from an event procedure go to the Immediate
    window, not into a box; without a window they are printed."""
    if text and not _c.immediate(text):
        sys.stdout.write(text)


def _book_module(st):
    return st.project.modules.get(_c.codename().lower()) or st.project.modules.get("thisworkbook")


def _on_change(sheet, rng):
    st = state()
    if not st.host.events_enabled:
        return
    ws = st.host.worksheet(sheet.index)
    target = Range(st.host, ws._id, rng.row0, rng.col0, rng.row1, rng.col1)
    _call_event(st, ws._vba_module, "worksheet_change", [target], ws)
    _call_event(st, _book_module(st), "workbook_sheetchange", [ws, target], st.host.document("ThisWorkbook"))


def _on_selection(sheet, rng):
    st = state()
    if not st.host.events_enabled:
        return
    ws = st.host.worksheet(sheet.index)
    target = Range(st.host, ws._id, rng.row0, rng.col0, min(rng.row1, MAX_ROWS - 1), min(rng.col1, MAX_COLS - 1))
    _call_event(st, ws._vba_module, "worksheet_selectionchange", [target], ws)
    _call_event(st, _book_module(st), "workbook_sheetselectionchange", [ws, target],
                st.host.document("ThisWorkbook"))


def _on_before_save(book):
    """Workbook_BeforeSave(SaveAsUI, Cancel): True when it set Cancel."""
    st = state()
    if not st.host.events_enabled:
        return False
    cancel = vba.RefArg(False)
    _call_event(st, _book_module(st), "workbook_beforesave", [False, cancel], st.host.document("ThisWorkbook"))
    return _cancelled(cancel)


def _cancelled(cancel):
    try:
        return vba.to_bool(cancel.cell.value)
    except VBAError:
        return False


def _on_open(book):
    st = state()
    if st.host.events_enabled:
        _call_event(st, _book_module(st), "workbook_open", [], st.host.document("ThisWorkbook"))
    # Auto_Open, from a module of macros: Excel runs it after Workbook_Open.
    proc = st.project.find_proc("Auto_Open", ("sub",))
    if proc is not None and proc.module.kind == "standard":
        _call_event(st, proc.module, "auto_open", [], None)


def _on_close(book):
    """Workbook_BeforeClose(Cancel), then Auto_Close unless it set Cancel;
    True when it did."""
    st = state()
    if st.host.events_enabled:
        cancel = vba.RefArg(False)
        _call_event(st, _book_module(st), "workbook_beforeclose", [cancel], st.host.document("ThisWorkbook"))
        if _cancelled(cancel):
            return True
    proc = st.project.find_proc("Auto_Close", ("sub",))
    if proc is not None and proc.module.kind == "standard":
        _call_event(st, proc.module, "auto_close", [], None)
    return False
