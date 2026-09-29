# vbaforms.py - Visual Basic's UserForms: read from the project, and run
#
# Copyright (C) 2026 The office42 authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A UserForm is a class module with a designer: a storage of the project
# named after the form, whose "f" stream lays out the form and the sites
# of its controls and whose "o" stream holds each control's own
# properties, one after another [MS-OFORMS].  A Frame or a MultiPage
# keeps its own controls in a storage of its own, "i" and its ID.  The
# first half of this module reads that into Form and ControlDesc; the
# second is what a macro sees at run time -- the form, its controls and
# their events -- over a small table of calls the window answers with
# widgets and office42-calc with lines of text.
#
# Sizes and positions are kept in points, as Visual Basic gives them to
# a macro; the file has them in HIMETRIC, hundredths of a millimetre.

import struct

import _office42 as _c
import o42vba as vba
from o42vba import VBAError, VBArray, MISSING, EMPTY, NULL

HIMETRIC = 72.0 / 2540.0          # points in a HIMETRIC

# The controls the file names by a number rather than a class id
# ([MS-OFORMS] 2.4.5, FormEmbeddedActiveXControlCached).
CACHED_KINDS = {7: "Page", 12: "Image", 14: "Frame", 15: "MorphData", 16: "SpinButton",
                17: "CommandButton", 18: "TabStrip", 21: "Label", 23: "TextBox", 24: "ListBox",
                25: "ComboBox", 26: "CheckBox", 27: "OptionButton", 28: "ToggleButton",
                47: "ScrollBar", 57: "MultiPage"}

# MorphData's DisplayStyle, for a control the class table names.
MORPH_KINDS = {1: "TextBox", 2: "ListBox", 3: "ComboBox", 4: "CheckBox", 5: "OptionButton",
               6: "ToggleButton", 7: "ComboBox"}

# The class ids of the same controls, for a class table ([MS-OFORMS] 2.4.1).
CLSID_KINDS = {
    "8bd21d10-ec42-11ce-9e0d-00aa006002f3": "TextBox",
    "8bd21d20-ec42-11ce-9e0d-00aa006002f3": "ListBox",
    "8bd21d30-ec42-11ce-9e0d-00aa006002f3": "ComboBox",
    "8bd21d40-ec42-11ce-9e0d-00aa006002f3": "CheckBox",
    "8bd21d50-ec42-11ce-9e0d-00aa006002f3": "OptionButton",
    "8bd21d60-ec42-11ce-9e0d-00aa006002f3": "ToggleButton",
    "978c9e23-d4b0-11ce-bf2d-00aa003f40d0": "Label",
    "d7053240-ce69-11cd-a777-00dd01143c57": "CommandButton",
    "6e182020-f460-11ce-9bcd-00aa00608e01": "Frame",
    "4c599241-6926-101b-9992-00000b65c6f9": "Image",
    "79176fb0-b7f2-11ce-97ef-00aa006d2776": "SpinButton",
    "dfd181e0-5e2f-11ce-a449-00aa004a803d": "ScrollBar",
    "46e31370-3f7a-11ce-bed6-00aa00611080": "MultiPage",
    "eae50eb0-4a62-11ce-bed6-00aa00611080": "TabStrip",
}


class FormError(Exception):
    pass


class Reader:
    """Little-endian fields out of a stream, with [MS-OFORMS]'s rule that
    a field in a data block starts at a multiple of its own size, counted
    from the block's start."""

    def __init__(self, data, pos=0, codepage="cp1252"):
        self.data = data
        self.pos = pos
        self.base = pos
        self.codepage = codepage

    def need(self, n):
        if self.pos + n > len(self.data):
            raise FormError("the form's data ends early")

    def raw(self, n):
        self.need(n)
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def align(self, n):
        off = (self.pos - self.base) % n
        if off:
            self.pos += n - off

    def u8(self):
        return self.raw(1)[0]

    def u16(self, aligned=True):
        if aligned:
            self.align(2)
        return struct.unpack("<H", self.raw(2))[0]

    def u32(self, aligned=True):
        if aligned:
            self.align(4)
        return struct.unpack("<I", self.raw(4))[0]

    def i32(self, aligned=True):
        if aligned:
            self.align(4)
        return struct.unpack("<i", self.raw(4))[0]

    def begin_block(self):
        """A data block starts: its fields align from here."""
        self.base = self.pos

    def next_block(self):
        """The data block is padded to four; the extra data block that
        follows aligns from its own start."""
        self.align(4)
        self.base = self.pos

    def text(self, count):
        """A string whose length and compression a count gave: bytes,
        one a character when compressed, else UTF-16; the block pads it
        to four."""
        n = count & 0x7FFFFFFF
        if not n:
            return ""
        self.align(4)
        b = self.raw(n)
        self.align(4)
        if count & 0x80000000:
            return b.decode("latin-1")          # UTF-16 with its high bytes left out
        return b.decode("utf-16-le", "replace")


def bits(mask, i):
    return (mask >> i) & 1


def colour(ole):
    """An OLE_COLOR as a macro sees it: the number itself, a system
    colour's included (&H8000000F&)."""
    return ole


def html(ole):
    """An OLE_COLOR as #rrggbb, a system colour as what Windows 97 drew."""
    ole &= 0xFFFFFFFF
    if ole & 0x80000000:
        return SYSTEM_COLOURS.get(ole & 0xFF, "#c0c0c0")
    r, g, b = ole & 0xFF, (ole >> 8) & 0xFF, (ole >> 16) & 0xFF
    return "#%02x%02x%02x" % (r, g, b)


# Windows' system colours, as the classic scheme had them.
SYSTEM_COLOURS = {0x00: "#c0c0c0", 0x01: "#008080", 0x02: "#000080", 0x03: "#808080",
                  0x04: "#c0c0c0", 0x05: "#ffffff", 0x06: "#000000", 0x07: "#000000",
                  0x08: "#000000", 0x09: "#ffffff", 0x0A: "#c0c0c0", 0x0B: "#c0c0c0",
                  0x0C: "#808080", 0x0D: "#000080", 0x0E: "#ffffff", 0x0F: "#d4d0c8",
                  0x10: "#808080", 0x11: "#808080", 0x12: "#000000", 0x13: "#c0c0c0",
                  0x14: "#ffffff", 0x15: "#404040", 0x16: "#d4d0c8", 0x17: "#000000",
                  0x18: "#ffffe1"}


class ControlDesc:
    """A control as the file lays it out: its site in the form, and what
    its own stream said."""

    def __init__(self):
        self.kind = "Unknown"
        self.name = ""
        self.tag = ""
        self.id = 0
        self.tab_index = -1
        self.visible = True
        self.tab_stop = True
        self.default = False
        self.cancel = False
        self.left = 0.0
        self.top = 0.0
        self.width = 0.0
        self.height = 0.0
        self.tip = ""
        self.control_source = ""
        self.row_source = ""
        self.group_id = 0
        self.props = {}
        self.children = []           # a Frame's or a MultiPage's own
        self.pages = []              # a MultiPage's, each a Form

    def __repr__(self):
        return "<%s %s>" % (self.kind, self.name)


class Form:
    """A form, a Frame's inside or a MultiPage's page: its own properties
    and its controls in order."""

    def __init__(self):
        self.caption = ""
        self.width = 0.0
        self.height = 0.0
        self.back_colour = 0x8000000F
        self.fore_colour = 0x80000012
        self.font = ("Tahoma", 8.0, False, False)
        self.controls = []
        self.start_position = 1

    def walk(self):
        """Every control, those inside frames and pages too."""
        for c in self.controls:
            yield c
            for child in c.children:
                yield from _walk_one(child)
            for page in c.pages:
                yield from page.walk()


def _walk_one(c):
    yield c
    for child in c.children:
        yield from _walk_one(child)
    for page in c.pages:
        yield from page.walk()


def _guid(b):
    a, b1, c = struct.unpack("<IHH", b[:8])
    rest = b[8:16].hex()
    return "%08x-%04x-%04x-%s-%s" % (a, b1, c, rest[:4], rest[4:])


STDFONT = "0be35203-8f91-11ce-9de3-00aa004bb851"
TEXTPROPS_GUID = "afc20920-da4e-11ce-b943-00aa006887b4"
STDPICTURE = "0be35204-8f91-11ce-9de3-00aa004bb851"


def read_picture(r):
    """GuidAndPicture: the picture's bytes, whatever format they are in."""
    guid = _guid(r.raw(16))
    if guid != STDPICTURE:
        raise FormError("not a picture")
    preamble = r.u32(False)
    size = r.u32(False)
    if preamble != 0x0000746C:
        raise FormError("not a picture")
    return r.raw(size)


def read_text_props(r):
    """TextProps: the control's font, as (name, points, bold, italic)."""
    minor, major = r.u8(), r.u8()
    cb = r.u16(False)
    end = r.pos + cb
    mask = r.u32(False)
    r.begin_block()
    name_count = r.u32() if bits(mask, 0) else 0
    effects = r.u32() if bits(mask, 1) else 0
    height = r.u32() if bits(mask, 2) else 160
    if bits(mask, 4):
        r.u8()
    if bits(mask, 5):
        r.u8()
    align = r.u8() if bits(mask, 6) else 1
    weight = r.u16() if bits(mask, 7) else 400
    r.next_block()
    name = r.text(name_count) if name_count else "Tahoma"
    r.pos = end
    return (name, height / 20.0, bool(effects & 1) or weight >= 600, bool(effects & 2)), align


def read_font(r):
    """GuidAndFont: a StdFont or a TextProps."""
    guid = _guid(r.raw(16))
    if guid == STDFONT:
        r.u8()                        # version
        r.raw(2)                      # charset
        flags = r.u8()
        weight = struct.unpack("<H", r.raw(2))[0]
        height = struct.unpack("<I", r.raw(4))[0]
        n = r.u8()
        name = r.raw(n).decode(r.codepage, "replace")
        return (name, height / 10000.0, weight >= 600 or bool(flags & 1), bool(flags & 2))
    if guid == TEXTPROPS_GUID:
        return read_text_props(r)[0]
    raise FormError("an unknown font")


def read_form_control(data, codepage="cp1252"):
    """FormControl, the "f" stream: the form's properties and its sites."""
    r = Reader(data, 0, codepage)
    form = Form()
    minor, major = r.u8(), r.u8()
    if major != 4:
        raise FormError("not a form's stream")
    cb = r.u16(False)
    end = r.pos + cb
    mask = r.u32(False)
    r.begin_block()
    if bits(mask, 1):
        form.back_colour = colour(r.u32())
    if bits(mask, 2):
        form.fore_colour = colour(r.u32())
    if bits(mask, 3):
        r.u32()                       # NextAvailableID
    boolean = r.u32() if bits(mask, 6) else 0x00000004
    if bits(mask, 7):
        r.u8()                        # BorderStyle
    if bits(mask, 8):
        r.u8()                        # MousePointer
    if bits(mask, 9):
        r.u8()                        # ScrollBars
    if bits(mask, 13):
        r.u32()                       # GroupCnt
    if bits(mask, 15):
        r.u16()                       # MouseIcon
    if bits(mask, 16):
        r.u8()                        # Cycle
    if bits(mask, 17):
        r.u8()                        # SpecialEffect
    if bits(mask, 18):
        r.u32()                       # BorderColor
    caption_count = r.u32() if bits(mask, 19) else 0
    if bits(mask, 20):
        r.u16()                       # Font
    if bits(mask, 21):
        r.u16()                       # Picture
    if bits(mask, 22):
        r.u32()                       # Zoom
    if bits(mask, 23):
        r.u8()                        # PictureAlignment
    if bits(mask, 25):
        r.u8()                        # PictureSizeMode
    if bits(mask, 26):
        r.u32()                       # ShapeCookie
    if bits(mask, 27):
        r.u32()                       # DrawBuffer
    r.next_block()
    if bits(mask, 10):
        form.width = r.u32() * HIMETRIC
        form.height = r.u32() * HIMETRIC
    if bits(mask, 11):
        r.u32(), r.u32()              # LogicalSize
    if bits(mask, 12):
        r.u32(), r.u32()              # ScrollPosition
    if caption_count:
        form.caption = r.text(caption_count)
    r.pos = end
    # FormStreamData
    if bits(mask, 15):
        read_picture(r)
    if bits(mask, 20):
        form.font = read_font(r)
    if bits(mask, 21):
        read_picture(r)
    # FormSiteData: the class table, then the sites.
    classes = []
    if not boolean & 0x8000:
        for _ in range(r.u16(False)):
            classes.append(read_site_class(r))
    count = r.u32(False)
    size = r.u32(False)
    sites_end = r.pos + size
    r.begin_block()
    remaining = count
    while remaining > 0:
        r.u8()                        # depth
        type_or_count = r.u8()
        if type_or_count & 0x80:
            r.u8()                    # the type, 1: a control
            remaining -= type_or_count & 0x7F
        else:
            remaining -= 1
    r.align(4)
    for _ in range(count):
        form.controls.append(read_site(r, classes))
    r.pos = sites_end
    return form


def read_site_class(r):
    """SiteClassInfo: a control class the form uses, by its class id."""
    r.u16(False)                      # version
    cb = r.u16(False)
    end = r.pos + cb
    mask = r.u32(False)
    r.begin_block()
    if bits(mask, 0):
        r.u32()                       # ClassTableFlags and VarFlags
    if bits(mask, 1):
        r.u32()                       # CountOfMethods
    if bits(mask, 2):
        r.u32()                       # DispidBind
    if bits(mask, 3):
        r.u16()                       # GetBindIndex
    if bits(mask, 4):
        r.u16()                       # PutBindIndex
    if bits(mask, 5):
        r.u16()                       # BindType
    if bits(mask, 6):
        r.u16()                       # GetValueIndex
    if bits(mask, 7):
        r.u16()                       # PutValueIndex
    if bits(mask, 8):
        r.u16()                       # ValueType
    if bits(mask, 9):
        r.u32()                       # DispidRowset
    if bits(mask, 10):
        r.u16()                       # SetRowset
    r.align(4)
    clsid = _guid(r.raw(16)) if bits(mask, 11) or r.pos + 16 <= end else ""
    r.pos = end
    return CLSID_KINDS.get(clsid, "Unknown")


def read_site(r, classes):
    """OleSiteConcreteControl: a control's name, place and kind."""
    c = ControlDesc()
    r.u16(False)                      # version
    cb = r.u16(False)
    end = r.pos + cb
    mask = r.u32(False)
    r.begin_block()
    name_count = r.u32() if bits(mask, 0) else 0
    tag_count = r.u32() if bits(mask, 1) else 0
    c.id = r.u32() if bits(mask, 2) else 0
    if bits(mask, 3):
        r.u32()                       # HelpContextID
    flags = r.u32() if bits(mask, 4) else 0x33
    c.stream_size = r.u32() if bits(mask, 5) else 0
    c.tab_index = r.u16() if bits(mask, 6) else -1
    cache = r.u16() if bits(mask, 7) else 0x7FFF
    c.group_id = r.u16() if bits(mask, 9) else 0
    tip_count = r.u32() if bits(mask, 11) else 0
    lic_count = r.u32() if bits(mask, 12) else 0
    source_count = r.u32() if bits(mask, 13) else 0
    rows_count = r.u32() if bits(mask, 14) else 0
    r.next_block()
    c.name = r.text(name_count)
    c.tag = r.text(tag_count)
    if bits(mask, 8):
        c.left = r.i32() * HIMETRIC
        c.top = r.i32() * HIMETRIC
    c.tip = r.text(tip_count)
    r.text(lic_count)
    c.control_source = r.text(source_count)
    c.row_source = r.text(rows_count)
    r.pos = end
    c.tab_stop = bool(flags & 0x01)
    c.visible = bool(flags & 0x02)
    c.default = bool(flags & 0x04)
    c.cancel = bool(flags & 0x08)
    c.streamed = bool(flags & 0x10)
    if cache & 0x8000:
        c.kind = classes[cache & 0x7FFF] if (cache & 0x7FFF) < len(classes) else "Unknown"
    elif cache in CACHED_KINDS:
        c.kind = CACHED_KINDS[cache]
    elif cache < len(classes):
        c.kind = classes[cache]
    return c


# ---- A control's own stream ----------------------------------------------------

def _size(r, c):
    c.width = r.u32() * HIMETRIC
    c.height = r.u32() * HIMETRIC


def read_morph(r, c):
    """MorphDataControl: a TextBox, ListBox, ComboBox, CheckBox,
    OptionButton or ToggleButton."""
    p = c.props
    r.u8(), r.u8()
    cb = r.u16(False)
    end = r.pos + cb
    lo, hi = r.u32(False), r.u32(False)
    mask = lo | (hi << 32)
    r.begin_block()
    p["flags"] = r.u32() if bits(mask, 0) else 0x2C80081B
    p["back"] = colour(r.u32() if bits(mask, 1) else 0x80000005)
    p["fore"] = colour(r.u32() if bits(mask, 2) else 0x80000008)
    p["max_length"] = r.u32() if bits(mask, 3) else 0
    p["border_style"] = r.u8() if bits(mask, 4) else 0
    p["scroll_bars"] = r.u8() if bits(mask, 5) else 0
    style = r.u8() if bits(mask, 6) else 1
    if bits(mask, 7):
        r.u8()                        # MousePointer
    p["password_char"] = chr(r.u16()) if bits(mask, 9) else ""
    if p["password_char"] == "\0":
        p["password_char"] = ""
    p["list_width"] = r.u32() * HIMETRIC if bits(mask, 10) else 0
    p["bound_column"] = r.u16() if bits(mask, 11) else 1
    text_column = r.u16() if bits(mask, 12) else 0xFFFF
    p["text_column"] = -1 if text_column == 0xFFFF else text_column
    p["column_count"] = r.u16() if bits(mask, 13) else 1
    p["list_rows"] = r.u16() if bits(mask, 14) else 8
    column_info = r.u16() if bits(mask, 15) else 0
    p["match_entry"] = r.u8() if bits(mask, 16) else 2
    p["list_style"] = r.u8() if bits(mask, 17) else 0
    if bits(mask, 18):
        r.u8()                        # ShowDropButtonWhen
    if bits(mask, 20):
        r.u8()                        # DropButtonStyle
    p["multi_select"] = r.u8() if bits(mask, 21) else 0
    value_count = r.u32() if bits(mask, 22) else 0
    caption_count = r.u32() if bits(mask, 23) else 0
    if bits(mask, 24):
        r.u32()                       # PicturePosition
    if bits(mask, 25):
        r.u32()                       # BorderColor
    p["special_effect"] = r.u32() if bits(mask, 26) else 2
    if bits(mask, 27):
        r.u16()                       # MouseIcon
    if bits(mask, 28):
        r.u16()                       # Picture
    accelerator = r.u16() if bits(mask, 29) else 0
    group_count = r.u32() if bits(mask, 32) else 0
    r.next_block()
    if bits(mask, 8):
        _size(r, c)
    p["value"] = r.text(value_count)
    p["caption"] = r.text(caption_count)
    p["group"] = r.text(group_count)
    p["accelerator"] = chr(accelerator) if accelerator else ""
    r.pos = end
    if bits(mask, 27):
        read_picture(r)
    if bits(mask, 28):
        p["picture"] = read_picture(r)
    p["font"], p["align"] = read_text_props(r)
    # A control the class table named says what it is by its style.
    if c.kind in ("MorphData", "Unknown"):
        c.kind = MORPH_KINDS.get(style, "TextBox")
    p["style"] = style
    p["column_info"] = column_info


def read_command_button(r, c):
    p = c.props
    r.u8(), r.u8()
    cb = r.u16(False)
    end = r.pos + cb
    mask = r.u32(False)
    r.begin_block()
    p["fore"] = colour(r.u32() if bits(mask, 0) else 0x80000012)
    p["back"] = colour(r.u32() if bits(mask, 1) else 0x8000000F)
    p["flags"] = r.u32() if bits(mask, 2) else 0x0000001B
    caption_count = r.u32() if bits(mask, 3) else 0
    if bits(mask, 4):
        r.u32()                       # PicturePosition
    if bits(mask, 6):
        r.u8()                        # MousePointer
    if bits(mask, 7):
        r.u16()                       # Picture
    accelerator = r.u16() if bits(mask, 8) else 0
    if bits(mask, 10):
        r.u16()                       # MouseIcon
    r.next_block()
    p["caption"] = r.text(caption_count)
    if bits(mask, 5):
        _size(r, c)
    p["accelerator"] = chr(accelerator) if accelerator else ""
    p["take_focus"] = not bits(mask, 9)
    r.pos = end
    if bits(mask, 7):
        p["picture"] = read_picture(r)
    if bits(mask, 10):
        read_picture(r)
    p["font"], p["align"] = read_text_props(r)


def read_label(r, c):
    p = c.props
    r.u8(), r.u8()
    cb = r.u16(False)
    end = r.pos + cb
    mask = r.u32(False)
    r.begin_block()
    p["fore"] = colour(r.u32() if bits(mask, 0) else 0x80000012)
    p["back"] = colour(r.u32() if bits(mask, 1) else 0x8000000F)
    p["flags"] = r.u32() if bits(mask, 2) else 0x0080001B
    caption_count = r.u32() if bits(mask, 3) else 0
    if bits(mask, 4):
        r.u32()                       # PicturePosition
    if bits(mask, 6):
        r.u8()                        # MousePointer
    if bits(mask, 7):
        r.u32()                       # BorderColor
    p["border_style"] = r.u16() if bits(mask, 8) else 0
    p["special_effect"] = r.u16() if bits(mask, 9) else 0
    if bits(mask, 10):
        r.u16()                       # Picture
    accelerator = r.u16() if bits(mask, 11) else 0
    if bits(mask, 12):
        r.u16()                       # MouseIcon
    r.next_block()
    p["caption"] = r.text(caption_count)
    if bits(mask, 5):
        _size(r, c)
    p["accelerator"] = chr(accelerator) if accelerator else ""
    r.pos = end
    if bits(mask, 10):
        p["picture"] = read_picture(r)
    if bits(mask, 12):
        read_picture(r)
    p["font"], p["align"] = read_text_props(r)


def read_spin(r, c, scroll):
    """SpinButtonControl, or with `scroll` ScrollBarControl, which has a
    LargeChange and its fields in another order."""
    p = c.props
    r.u8(), r.u8()
    cb = r.u16(False)
    end = r.pos + cb
    mask = r.u32(False)
    r.begin_block()
    p["fore"] = colour(r.u32() if bits(mask, 0) else 0x80000012)
    p["back"] = colour(r.u32() if bits(mask, 1) else 0x8000000F)
    p["flags"] = r.u32() if bits(mask, 2) else 0x1B
    if scroll:
        if bits(mask, 4):
            r.u8()                    # MousePointer
        p["min"] = r.i32() if bits(mask, 5) else 0
        p["max"] = r.i32() if bits(mask, 6) else 32767
        p["value"] = r.i32() if bits(mask, 7) else 0
        if bits(mask, 9):
            r.u32()                   # PrevEnabled
        if bits(mask, 10):
            r.u32()                   # NextEnabled
        p["small_change"] = r.i32() if bits(mask, 11) else 1
        p["large_change"] = r.i32() if bits(mask, 12) else 1
        orientation = r.i32() if bits(mask, 13) else -1
        if bits(mask, 14):
            r.u16()                   # ProportionalThumb
        if bits(mask, 15):
            r.u32()                   # Delay
        if bits(mask, 16):
            r.u16()                   # MouseIcon
        size_bit = 3
    else:
        p["min"] = r.i32() if bits(mask, 5) else 0
        p["max"] = r.i32() if bits(mask, 6) else 100
        p["value"] = r.i32() if bits(mask, 7) else 0
        if bits(mask, 8):
            r.u32()                   # PrevEnabled
        if bits(mask, 9):
            r.u32()                   # NextEnabled
        p["small_change"] = r.i32() if bits(mask, 10) else 1
        orientation = r.i32() if bits(mask, 11) else -1
        if bits(mask, 12):
            r.u32()                   # Delay
        if bits(mask, 13):
            r.u16()                   # MouseIcon
        if bits(mask, 14):
            r.u8()                    # MousePointer
        size_bit = 3
    r.next_block()
    if bits(mask, size_bit):
        _size(r, c)
    p["orientation"] = orientation
    r.pos = end


def read_image(r, c):
    p = c.props
    r.u8(), r.u8()
    cb = r.u16(False)
    end = r.pos + cb
    mask = r.u32(False)
    r.begin_block()
    if bits(mask, 3):
        r.u32()                       # BorderColor
    p["back"] = colour(r.u32() if bits(mask, 4) else 0x8000000F)
    p["border_style"] = r.u8() if bits(mask, 5) else 1
    if bits(mask, 6):
        r.u8()                        # MousePointer
    p["size_mode"] = r.u8() if bits(mask, 7) else 0
    if bits(mask, 8):
        r.u8()                        # SpecialEffect
    if bits(mask, 10):
        r.u16()                       # Picture
    if bits(mask, 11):
        r.u8()                        # PictureAlignment
    p["flags"] = r.u32() if bits(mask, 13) else 0x1B
    if bits(mask, 14):
        r.u16()                       # MouseIcon
    r.next_block()
    if bits(mask, 9):
        _size(r, c)
    r.pos = end
    if bits(mask, 10):
        p["picture"] = read_picture(r)


def read_tab_strip(r, c):
    """TabStripControl: a MultiPage's tabs, whose captions are its pages'."""
    p = c.props
    r.u8(), r.u8()
    cb = r.u16(False)
    end = r.pos + cb
    mask = r.u32(False)
    r.begin_block()
    p["index"] = r.i32() if bits(mask, 0) else 0
    if bits(mask, 1):
        r.u32()                       # BackColor
    if bits(mask, 2):
        r.u32()                       # ForeColor
    if bits(mask, 5):
        r.u32()                       # the size of the captions, in bytes
    if bits(mask, 6):
        r.u8()                        # MousePointer
    for bit in (8, 9, 11, 12, 15, 17, 18, 20, 21):
        if bits(mask, bit):
            r.u32()                   # TabOrientation ... Tags
    tabs = r.u32() if bits(mask, 22) else 0
    if bits(mask, 23):
        r.u32()                       # the accelerators' size
    if bits(mask, 24):
        r.u16()                       # MouseIcon
    r.next_block()
    if bits(mask, 4):
        _size(r, c)
    p["captions"] = [r.text(r.u32()) for _ in range(tabs)] if bits(mask, 5) else []
    r.pos = end


def read_control_stream(data, c, codepage):
    r = Reader(data, 0, codepage)
    kind = c.kind
    if kind in ("TextBox", "ListBox", "ComboBox", "CheckBox", "OptionButton", "ToggleButton",
                "MorphData", "Unknown"):
        read_morph(r, c)
    elif kind == "CommandButton":
        read_command_button(r, c)
    elif kind == "Label":
        read_label(r, c)
    elif kind == "SpinButton":
        read_spin(r, c, False)
    elif kind == "ScrollBar":
        read_spin(r, c, True)
    elif kind == "Image":
        read_image(r, c)
    elif kind == "TabStrip":
        read_tab_strip(r, c)


# ---- The whole form ----------------------------------------------------------------

def read_vbframe(text):
    """The \x03VBFrame stream: the form's caption and client size, in the
    words of an exported .frm."""
    props = {}
    for line in text.replace("\r", "").split("\n"):
        line = line.strip()
        if "=" not in line or line.lower().startswith(("begin", "end", "version")):
            continue
        key, _, value = line.partition("=")
        value = value.strip()
        if "'" in value and not value.startswith('"'):
            value = value.split("'")[0].strip()
        if value.startswith('"'):
            value = value[1:value.rfind('"')] if value.rfind('"') > 0 else value[1:]
            value = value.replace('""', '"')
        props[key.strip().lower()] = value
    return props


def read_form(streams, codepage="cp1252", prefix=""):
    """The form in `streams` -- a dict of a designer storage's streams by
    their paths within it -- or a Frame's inside when `prefix` is its
    storage."""
    form = read_form_control(streams[prefix + "f"], codepage)
    objects = streams.get(prefix + "o", b"")
    at = 0
    for c in form.controls:
        if c.streamed:
            chunk = objects[at:at + c.stream_size]
            at += c.stream_size
            try:
                read_control_stream(chunk, c, codepage)
            except (FormError, struct.error, IndexError):
                c.props.setdefault("unread", True)
        storage = "%si%02d/" % (prefix, c.id)
        if c.kind == "Frame" and storage + "f" in streams:
            inner = read_form(streams, codepage, storage)
            c.children = inner.controls
            c.props["caption"] = inner.caption
            c.props["font"] = inner.font
            c.props["back"] = inner.back_colour
            c.props["fore"] = inner.fore_colour
            if not c.width:
                c.width, c.height = inner.width, inner.height
        elif c.kind == "MultiPage" and storage + "f" in streams:
            read_multipage(streams, codepage, storage, c)
    if not prefix:
        frame = streams.get("\x03VBFrame")
        if frame is not None:
            props = read_vbframe(frame.decode(codepage, "replace"))
            form.caption = props.get("caption", form.caption)
            try:
                form.width = float(props.get("clientwidth", 0)) / 20.0 or form.width
                form.height = float(props.get("clientheight", 0)) / 20.0 or form.height
                form.start_position = int(props.get("startupposition", 1))
            except ValueError:
                pass
    return form


def read_multipage(streams, codepage, storage, c):
    """A MultiPage's storage: its pages are the sites of its own "f",
    each a form in a storage of its own; its "o" holds the tab strip,
    whose captions are the pages'."""
    inner = read_form_control(streams[storage + "f"], codepage)
    tabs = ControlDesc()
    tabs.kind = "TabStrip"
    objects = streams.get(storage + "o", b"")
    captions = []
    try:
        read_control_stream(objects, tabs, codepage)
        captions = tabs.props.get("captions", [])
    except (FormError, struct.error, IndexError):
        pass
    if not c.width:
        c.width, c.height = inner.width, inner.height
    for k, site in enumerate(s for s in inner.controls if s.kind in ("Page", "Unknown")):
        page_storage = "%si%02d/" % (storage, site.id)
        page = read_form(streams, codepage, page_storage) if page_storage + "f" in streams else Form()
        page.name = site.name
        page.caption = captions[k] if k < len(captions) and captions[k] else site.name
        c.pages.append(page)
    c.props["value"] = tabs.props.get("index", 0)


# ---- At run time -------------------------------------------------------------------
#
# A UserForm's instance is a VBObject of its module, whose `ext` is a
# FormInstance: the controls, made from the design when the form loads,
# and what the macro changes of them.  Python holds the state; the host
# only draws it.  A form on show has a number the host gave it, and so
# does each of its controls, and every change is passed on as a
# property's name and its value as text -- "caption", "text", "value",
# "items", "index", "enabled" -- which the window turns into widgets and
# office42-calc into lines.  What the user does comes back through
# _event: a click, a change with the new value, the close box.

_shown = {}                       # the host's form number -> FormInstance


def _bool_text(v):
    return "1" if v else "0"


def _points(v):
    return "%.2f" % v


def _font_text(font):
    return "%s|%s|%d|%d|%d|%d" % (font.Name, _points(font.Size), font.Bold, font.Italic,
                                   font.Underline, font.Strikethrough)


def _cell_text(v):
    """A list's cell as it shows: Null and Empty as nothing."""
    return "" if v is NULL or v is EMPTY else vba.to_str(v)


class ReturnBoolean:
    """MSForms.ReturnBoolean: what DblClick and Exit give their Cancel as."""
    _vba_typename = "ReturnBoolean"

    def __init__(self, value=False):
        self.Value = value

    def _vba_default_get(self):
        return self.Value

    def _vba_default_let(self, value):
        self.Value = vba.to_bool(value)


class NewFont:
    """StdFont, as a form's or a control's Font."""
    _vba_typename = "Font"

    def __init__(self, owner, desc):
        self._owner = owner
        name, size, bold, italic = desc
        self._p = {"Name": name, "Size": size, "Bold": bold, "Italic": italic,
                   "Underline": False, "Strikethrough": False}

    def _get(self, k):
        return self._p[k]

    def _set(self, k, v):
        self._p[k] = v
        self._owner._sync("font", _font_text(self))

    Name = property(lambda s: s._p["Name"], lambda s, v: s._set("Name", vba.to_str(v)))
    Size = property(lambda s: s._p["Size"], lambda s, v: s._set("Size", vba.to_double(v)))
    Bold = property(lambda s: s._p["Bold"], lambda s, v: s._set("Bold", vba.to_bool(v)))
    Italic = property(lambda s: s._p["Italic"], lambda s, v: s._set("Italic", vba.to_bool(v)))
    Underline = property(lambda s: s._p["Underline"], lambda s, v: s._set("Underline", vba.to_bool(v)))
    Strikethrough = property(lambda s: s._p["Strikethrough"],
                             lambda s, v: s._set("Strikethrough", vba.to_bool(v)))

    @property
    def Weight(self):
        return 700 if self._p["Bold"] else 400


class Control:
    """What every control has: its name and place, its colours and font,
    whether it shows and takes input."""

    _vba_typename = "Control"
    _events_on_change = ("change",)

    def __init__(self, form, desc, parent):
        self._form = form
        self._parent = parent
        self._hid = 0
        self._kind = desc.kind
        self._name = desc.name
        self._p = {"left": desc.left, "top": desc.top, "width": desc.width, "height": desc.height,
                   "visible": desc.visible, "enabled": True, "tag": desc.tag, "tip": desc.tip,
                   "tab_index": desc.tab_index, "tab_stop": desc.tab_stop,
                   "back": desc.props.get("back", 0x8000000F), "fore": desc.props.get("fore", 0x80000012)}
        flags = desc.props.get("flags", 0x1B)
        self._p["enabled"] = bool(flags & 0x2)
        self._p["locked"] = bool(flags & 0x4)
        self._flags = flags
        self._font = NewFont(self, desc.props.get("font") or form._font_desc)
        self._desc = desc

    def __repr__(self):
        return "<%s %s>" % (self._kind, self._name)

    # -- the host --
    def _sync(self, prop, value):
        form = self._form
        if self._hid and form._hid:
            _c.form_set(form._hid, self._hid, prop, value)

    def _state(self):
        """Every property the host is told of when the form is first shown."""
        p = self._p
        out = [("left", _points(p["left"])), ("top", _points(p["top"])),
               ("width", _points(p["width"])), ("height", _points(p["height"])),
               ("visible", _bool_text(p["visible"])), ("enabled", _bool_text(p["enabled"])),
               ("back", html(p["back"])), ("fore", html(p["fore"])), ("font", _font_text(self._font))]
        if p["tip"]:
            out.append(("tip", p["tip"]))
        return out

    def _fire(self, event, *args):
        return self._form._fire(self._name.lower() + "_" + event, *args)

    # -- what the host says --
    def _user(self, event, value):
        if event == "click":
            self._fire("click")
        elif event == "dblclick":
            self._fire("dblclick", ReturnBoolean(False))

    # -- properties --
    def _setp(self, key, value, prop=None, text=None):
        self._p[key] = value
        self._sync(prop or key, text if text is not None else str(value))

    Name = property(lambda s: s._name)
    Left = property(lambda s: s._p["left"], lambda s, v: s._setp("left", vba.to_double(v), "left",
                                                                 _points(vba.to_double(v))))
    Top = property(lambda s: s._p["top"], lambda s, v: s._setp("top", vba.to_double(v), "top",
                                                              _points(vba.to_double(v))))
    Width = property(lambda s: s._p["width"], lambda s, v: s._setp("width", vba.to_double(v), "width",
                                                                  _points(vba.to_double(v))))
    Height = property(lambda s: s._p["height"], lambda s, v: s._setp("height", vba.to_double(v), "height",
                                                                    _points(vba.to_double(v))))
    Visible = property(lambda s: s._p["visible"], lambda s, v: s._setp("visible", vba.to_bool(v), "visible",
                                                                      _bool_text(vba.to_bool(v))))
    Enabled = property(lambda s: s._p["enabled"], lambda s, v: s._setp("enabled", vba.to_bool(v), "enabled",
                                                                      _bool_text(vba.to_bool(v))))
    Tag = property(lambda s: s._p["tag"], lambda s, v: s._p.__setitem__("tag", vba.to_str(v)))
    ControlTipText = property(lambda s: s._p["tip"], lambda s, v: s._setp("tip", vba.to_str(v)))
    TabIndex = property(lambda s: s._p["tab_index"], lambda s, v: s._p.__setitem__("tab_index", vba.to_long(v)))
    TabStop = property(lambda s: s._p["tab_stop"], lambda s, v: s._p.__setitem__("tab_stop", vba.to_bool(v)))
    BackColor = property(lambda s: s._p["back"], lambda s, v: s._setp("back", vba.to_long(v), "back",
                                                                     html(vba.to_long(v))))
    ForeColor = property(lambda s: s._p["fore"], lambda s, v: s._setp("fore", vba.to_long(v), "fore",
                                                                     html(vba.to_long(v))))
    Font = property(lambda s: s._font)
    Parent = property(lambda s: s._parent)
    Object = property(lambda s: s)

    def SetFocus(self):
        self._sync("focus", "1")

    def Move(self, Left=MISSING, Top=MISSING, Width=MISSING, Height=MISSING):
        if Left is not MISSING:
            self.Left = Left
        if Top is not MISSING:
            self.Top = Top
        if Width is not MISSING:
            self.Width = Width
        if Height is not MISSING:
            self.Height = Height

    def ZOrder(self, zPosition=0):
        return None


class Label(Control):
    _vba_typename = "Label"

    def __init__(self, form, desc, parent):
        Control.__init__(self, form, desc, parent)
        self._p["caption"] = desc.props.get("caption", "")
        self._p["wrap"] = bool(self._flags & (1 << 23))

    def _state(self):
        return Control._state(self) + [("caption", self._p["caption"]), ("wrap", _bool_text(self._p["wrap"]))]

    Caption = property(lambda s: s._p["caption"], lambda s, v: s._setp("caption", vba.to_str(v)))
    WordWrap = property(lambda s: s._p["wrap"], lambda s, v: s._setp("wrap", vba.to_bool(v), "wrap",
                                                                    _bool_text(vba.to_bool(v))))
    AutoSize = property(lambda s: False, lambda s, v: None)

    def _vba_default_get(self):
        return self._p["caption"]

    def _vba_default_let(self, value):
        self.Caption = value


class CommandButton(Label):
    _vba_typename = "CommandButton"

    def __init__(self, form, desc, parent):
        Label.__init__(self, form, desc, parent)
        self._p["default"] = desc.default
        self._p["cancel"] = desc.cancel

    def _state(self):
        return Control._state(self) + [("caption", self._p["caption"]),
                                       ("default", _bool_text(self._p["default"])),
                                       ("cancel", _bool_text(self._p["cancel"]))]

    Default = property(lambda s: s._p["default"], lambda s, v: s._setp("default", vba.to_bool(v), "default",
                                                                      _bool_text(vba.to_bool(v))))
    Cancel = property(lambda s: s._p["cancel"], lambda s, v: s._setp("cancel", vba.to_bool(v), "cancel",
                                                                    _bool_text(vba.to_bool(v))))
    TakeFocusOnClick = property(lambda s: True, lambda s, v: None)

    def _vba_default_get(self):
        return False


class TextBox(Control):
    _vba_typename = "TextBox"

    def __init__(self, form, desc, parent):
        Control.__init__(self, form, desc, parent)
        p = desc.props
        self._p["text"] = p.get("value", "")
        self._p["multiline"] = bool(self._flags & (1 << 31))
        self._p["wrap"] = bool(self._flags & (1 << 23))
        self._p["max_length"] = p.get("max_length", 0)
        self._p["password"] = p.get("password_char", "")
        self._p["back"] = p.get("back", 0x80000005)
        self._p["fore"] = p.get("fore", 0x80000008)

    def _state(self):
        p = self._p
        return Control._state(self) + [("multiline", _bool_text(p["multiline"])),
                                       ("password", p["password"]), ("maxlength", str(p["max_length"])),
                                       ("locked", _bool_text(p["locked"])), ("text", p["text"])]

    def _set_text(self, text, from_user=False):
        text = vba.to_str(text)
        if self._p["max_length"] and len(text) > self._p["max_length"] and not from_user:
            text = text[:self._p["max_length"]]
        if text == self._p["text"]:
            return
        self._p["text"] = text
        if not from_user:
            self._sync("text", text)
        self._fire("change")

    def _user(self, event, value):
        if event == "change":
            self._set_text(value, True)
        else:
            Control._user(self, event, value)

    Text = property(lambda s: s._p["text"], lambda s, v: s._set_text(v))
    Value = property(lambda s: s._p["text"], lambda s, v: s._set_text(v))
    MultiLine = property(lambda s: s._p["multiline"],
                         lambda s, v: s._setp("multiline", vba.to_bool(v), "multiline", _bool_text(vba.to_bool(v))))
    WordWrap = property(lambda s: s._p["wrap"], lambda s, v: s._p.__setitem__("wrap", vba.to_bool(v)))
    MaxLength = property(lambda s: s._p["max_length"],
                         lambda s, v: s._setp("max_length", vba.to_long(v), "maxlength", str(vba.to_long(v))))
    PasswordChar = property(lambda s: s._p["password"],
                            lambda s, v: s._setp("password", vba.to_str(v)[:1], "password", vba.to_str(v)[:1]))
    Locked = property(lambda s: s._p["locked"],
                      lambda s, v: s._setp("locked", vba.to_bool(v), "locked", _bool_text(vba.to_bool(v))))
    EnterKeyBehavior = property(lambda s: False, lambda s, v: None)

    @property
    def LineCount(self):
        return self._p["text"].count("\n") + 1

    @property
    def TextLength(self):
        return len(self._p["text"])

    SelStart = property(lambda s: len(s._p["text"]), lambda s, v: None)
    SelLength = property(lambda s: 0, lambda s, v: s._sync("select", "all") if vba.to_long(v) else None)
    SelText = property(lambda s: "", lambda s, v: None)

    def _vba_default_get(self):
        return self._p["text"]

    def _vba_default_let(self, value):
        self._set_text(value)


class CheckBox(Control):
    """A CheckBox, an OptionButton or a ToggleButton: a caption and a
    Value of True, False or, with TripleState, Null."""
    _vba_typename = "CheckBox"

    def __init__(self, form, desc, parent):
        Control.__init__(self, form, desc, parent)
        p = desc.props
        v = p.get("value", "")
        self._p["value"] = True if v == "1" else NULL if v == "2" else False
        self._p["caption"] = p.get("caption", "")
        self._p["group"] = p.get("group", "")
        self._p["triple"] = False

    def _state(self):
        v = self._p["value"]
        return Control._state(self) + [("caption", self._p["caption"]), ("group", self._group()),
                                       ("value", "" if v is NULL else _bool_text(v))]

    def _group(self):
        if self._kind != "OptionButton":
            return ""
        # Option buttons go together by GroupName, else by container.
        return self._p["group"] or "parent:%d" % id(self._parent)

    def _set_value(self, value, from_user=False):
        if value is NULL or value is EMPTY and self._p["triple"]:
            value = NULL
        else:
            value = vba.to_bool(value)
        old = self._p["value"]
        if (value is NULL) == (old is NULL) and (value is NULL or value == old):
            return
        self._p["value"] = value
        if not from_user:
            self._sync("value", "" if value is NULL else _bool_text(value))
        # Choosing an option button clears the others of its group.
        if self._kind == "OptionButton" and value is True:
            for other in self._form._walk():
                if other is not self and other._kind == "OptionButton" and other._group() == self._group() \
                        and other._p["value"] is True:
                    other._set_value(False)
        self._fire("change")
        # An option button says Click when it is chosen, the others at
        # every change.
        if self._kind != "OptionButton" or value is True:
            self._fire("click")

    def _user(self, event, value):
        if event == "change":
            self._set_value(value == "1", True)
        elif event != "click":
            Control._user(self, event, value)

    Value = property(lambda s: s._p["value"], lambda s, v: s._set_value(v))
    Caption = property(lambda s: s._p["caption"], lambda s, v: s._setp("caption", vba.to_str(v)))
    GroupName = property(lambda s: s._p["group"], lambda s, v: s._setp("group", vba.to_str(v), "group"))
    TripleState = property(lambda s: s._p["triple"], lambda s, v: s._p.__setitem__("triple", vba.to_bool(v)))

    def _vba_default_get(self):
        return self._p["value"]

    def _vba_default_let(self, value):
        self._set_value(value)


class ListControl(Control):
    """A ListBox or a ComboBox: rows of one or more columns, the one
    chosen, and for a ComboBox the text typed."""
    _vba_typename = "ListBox"

    def __init__(self, form, desc, parent):
        Control.__init__(self, form, desc, parent)
        p = desc.props
        self._rows = []
        self._p["index"] = -1
        self._p["text"] = p.get("value", "") if desc.kind == "ComboBox" else ""
        self._p["columns"] = max(1, p.get("column_count", 1))
        self._p["bound"] = p.get("bound_column", 1)
        self._p["text_column"] = p.get("text_column", -1)
        self._p["multi"] = p.get("multi_select", 0)
        self._p["style"] = 2 if p.get("style", 3) == 7 else 0
        self._p["widths"] = ""
        self._p["back"] = p.get("back", 0x80000005)
        self._p["fore"] = p.get("fore", 0x80000008)
        self._selected = set()
        self._row_source = ""
        if desc.row_source:
            try:
                self.RowSource = desc.row_source
            except VBAError:
                pass

    def _state(self):
        p = self._p
        return Control._state(self) + [("style", str(p["style"])), ("multiselect", str(p["multi"])),
                                       ("columns", str(p["columns"])), ("items", self._items_text()),
                                       ("index", str(p["index"])), ("text", p["text"]),
                                       ("selected", ",".join(str(i) for i in sorted(self._selected)))]

    def _items_text(self):
        return "\n".join("\t".join(_cell_text(c) for c in row) for row in self._rows)

    def _changed_list(self):
        if self._hid and self._form._hid:
            self._sync("items", self._items_text())

    def _row_text(self, i):
        col = self._p["text_column"]
        row = self._rows[i]
        col = 1 if col == -1 else col
        return _cell_text(row[col - 1]) if 0 < col <= len(row) else ""

    def _set_index(self, index, from_user=False):
        index = vba.to_long(index)
        if index < -1 or index >= len(self._rows):
            raise VBAError(380, "Could not set the ListIndex property. Invalid property value.")
        if index == self._p["index"]:
            return
        self._p["index"] = index
        if self._kind == "ComboBox":
            self._p["text"] = self._row_text(index) if index >= 0 else self._p["text"]
        if not from_user:
            self._sync("index", str(index))
        self._fire("change")
        self._fire("click")

    def _user(self, event, value):
        if event == "change" and self._kind == "ComboBox" and value.startswith("text:"):
            text = value[5:]
            if text != self._p["text"]:
                self._p["text"] = text
                match = [i for i in range(len(self._rows)) if self._row_text(i) == text]
                self._p["index"] = match[0] if match else -1
                self._fire("change")
        elif event == "change" and value.startswith("selected:"):
            chosen = {int(x) for x in value[9:].split(",") if x.strip()}
            self._selected = chosen
            self._p["index"] = min(chosen) if chosen else self._p["index"]
            self._fire("change")
        elif event == "change":
            self._set_index(int(value), True)
        else:
            Control._user(self, event, value)

    # -- the list --
    def AddItem(self, pvargItem=MISSING, pvargIndex=MISSING):
        row = [EMPTY if pvargItem is MISSING else pvargItem] + [NULL] * (self._p["columns"] - 1)
        if pvargIndex is MISSING:
            self._rows.append(row)
        else:
            i = vba.to_long(pvargIndex)
            if not 0 <= i <= len(self._rows):
                raise VBAError(-2147024809, "Invalid argument.")
            self._rows.insert(i, row)
            if self._p["index"] >= i:
                self._p["index"] += 1
        self._changed_list()

    def RemoveItem(self, pvargIndex):
        i = vba.to_long(pvargIndex)
        if not 0 <= i < len(self._rows):
            raise VBAError(-2147024809, "Invalid argument.")
        del self._rows[i]
        if self._p["index"] == i:
            self._p["index"] = -1
        elif self._p["index"] > i:
            self._p["index"] -= 1
        self._selected = {k - (k > i) for k in self._selected if k != i}
        self._changed_list()

    def Clear(self):
        if self._row_source:
            raise VBAError(-2147467259, "Unspecified error")
        self._rows = []
        self._p["index"] = -1
        self._selected = set()
        self._changed_list()

    def List(self, pvargIndex=MISSING, pvargColumn=MISSING):
        if pvargIndex is MISSING:
            if not self._rows:
                return NULL
            cols = max(self._p["columns"], max(len(r) for r in self._rows))
            data = []
            # VBArray keeps its elements with the first subscript fastest.
            for c in range(cols):
                for r in self._rows:
                    data.append(r[c] if c < len(r) else NULL)
            return VBArray([(0, len(self._rows) - 1), (0, cols - 1)], data=data)
        i = vba.to_long(pvargIndex)
        c = 0 if pvargColumn is MISSING else vba.to_long(pvargColumn)
        if not 0 <= i < len(self._rows) or c < 0:
            raise VBAError(381, "Could not get the List property. Invalid property array index.")
        row = self._rows[i]
        return row[c] if c < len(row) else NULL

    def let_List(self, value, pvargIndex=MISSING, pvargColumn=MISSING):
        if pvargIndex is MISSING:
            self._rows = []
            if isinstance(value, VBArray):
                if len(value.dims) == 1:
                    (lo, hi), = value.dims
                    self._rows = [[value.get([k])] for k in range(lo, hi + 1)]
                else:
                    (rlo, rhi), (clo, chi) = value.dims[0], value.dims[1]
                    self._rows = [[value.get([r, c]) for c in range(clo, chi + 1)] for r in range(rlo, rhi + 1)]
            self._p["index"] = -1
            self._changed_list()
            return
        i = vba.to_long(pvargIndex)
        c = 0 if pvargColumn is MISSING else vba.to_long(pvargColumn)
        if not 0 <= i < len(self._rows) or c < 0:
            raise VBAError(381, "Could not set the List property. Invalid property array index.")
        row = self._rows[i]
        while len(row) <= c:
            row.append(NULL)
        row[c] = value
        self._changed_list()

    def Column(self, pvargColumn=MISSING, pvargIndex=MISSING):
        if pvargColumn is MISSING:
            return self.List()
        c = vba.to_long(pvargColumn)
        i = self._p["index"] if pvargIndex is MISSING else vba.to_long(pvargIndex)
        if i < 0:
            return NULL
        return self.List(i, c)

    def let_Column(self, value, pvargColumn=MISSING, pvargIndex=MISSING):
        i = self._p["index"] if pvargIndex is MISSING else vba.to_long(pvargIndex)
        self.let_List(value, i, 0 if pvargColumn is MISSING else vba.to_long(pvargColumn))

    def Selected(self, pvargIndex):
        i = vba.to_long(pvargIndex)
        if not 0 <= i < len(self._rows):
            raise VBAError(381, "Could not get the Selected property. Invalid property array index.")
        if self._p["multi"]:
            return i in self._selected
        return i == self._p["index"]

    def let_Selected(self, value, pvargIndex):
        i = vba.to_long(pvargIndex)
        if not 0 <= i < len(self._rows):
            raise VBAError(381, "Could not set the Selected property. Invalid property array index.")
        if self._p["multi"]:
            if vba.to_bool(value):
                self._selected.add(i)
            else:
                self._selected.discard(i)
            self._sync("selected", ",".join(str(k) for k in sorted(self._selected)))
            self._fire("change")
        elif vba.to_bool(value):
            self._set_index(i)
        elif i == self._p["index"]:
            self._set_index(-1)

    ListCount = property(lambda s: len(s._rows))
    ListIndex = property(lambda s: s._p["index"], lambda s, v: s._set_index(v))
    ColumnCount = property(lambda s: s._p["columns"],
                           lambda s, v: s._setp("columns", max(1, vba.to_long(v)), "columns",
                                                str(max(1, vba.to_long(v)))))
    ColumnWidths = property(lambda s: s._p["widths"], lambda s, v: s._p.__setitem__("widths", vba.to_str(v)))
    ColumnHeads = property(lambda s: False, lambda s, v: None)
    BoundColumn = property(lambda s: s._p["bound"], lambda s, v: s._p.__setitem__("bound", vba.to_long(v)))
    TextColumn = property(lambda s: s._p["text_column"],
                          lambda s, v: s._p.__setitem__("text_column", vba.to_long(v)))
    MultiSelect = property(lambda s: s._p["multi"],
                           lambda s, v: s._setp("multi", vba.to_long(v), "multiselect", str(vba.to_long(v))))
    Style = property(lambda s: s._p["style"], lambda s, v: s._setp("style", vba.to_long(v), "style",
                                                                  str(vba.to_long(v))))
    ListRows = property(lambda s: 8, lambda s, v: None)
    MatchRequired = property(lambda s: False, lambda s, v: None)
    MatchEntry = property(lambda s: 1, lambda s, v: None)
    TopIndex = property(lambda s: 0, lambda s, v: None)
    Locked = property(lambda s: s._p["locked"],
                      lambda s, v: s._setp("locked", vba.to_bool(v), "locked", _bool_text(vba.to_bool(v))))

    @property
    def RowSource(self):
        return self._row_source

    @RowSource.setter
    def RowSource(self, value):
        """A range whose cells fill the list, as a worksheet reference."""
        text = vba.to_str(value)
        self._row_source = text
        if not text:
            return
        rng = vba.current.host.parse_range(text) if vba.current is not None else None
        if rng is None:
            raise VBAError(380, "Could not set the RowSource property. Invalid property value.")
        values = rng.Value
        if isinstance(values, VBArray):
            (rlo, rhi), (clo, chi) = values.dims
            self._rows = [[values.get([r, c]) for c in range(clo, chi + 1)] for r in range(rlo, rhi + 1)]
            self._p["columns"] = max(self._p["columns"], chi - clo + 1)
        else:
            self._rows = [[values]]
        self._p["index"] = -1
        self._changed_list()

    @property
    def Text(self):
        if self._kind == "ComboBox":
            return self._p["text"]
        return self._row_text(self._p["index"]) if self._p["index"] >= 0 else ""

    @Text.setter
    def Text(self, value):
        text = vba.to_str(value)
        match = [i for i in range(len(self._rows)) if self._row_text(i) == text]
        if self._kind == "ComboBox":
            if text != self._p["text"]:
                self._p["text"] = text
                self._p["index"] = match[0] if match else -1
                self._sync("text", text)
                self._fire("change")
        elif match:
            self._set_index(match[0])

    @property
    def Value(self):
        i = self._p["index"]
        if self._p["multi"]:
            return NULL
        if i < 0:
            return self._p["text"] if self._kind == "ComboBox" and self._p["text"] else NULL
        bound = self._p["bound"]
        row = self._rows[i]
        if bound == 0:
            return i
        return row[bound - 1] if bound - 1 < len(row) else NULL

    @Value.setter
    def Value(self, value):
        bound = self._p["bound"]
        for i, row in enumerate(self._rows):
            v = i if bound == 0 else (row[bound - 1] if bound - 1 < len(row) else NULL)
            if v is not NULL and vba.to_str(v) == vba.to_str(value):
                self._set_index(i)
                return
        if self._kind == "ComboBox":
            self.Text = value
        else:
            self._set_index(-1)

    def DropDown(self):
        self._sync("dropdown", "1")

    def _vba_default_get(self):
        return self.Value

    def _vba_default_let(self, value):
        self.Value = value


class SpinButton(Control):
    """A SpinButton or a ScrollBar: a Value between Min and Max."""
    _vba_typename = "SpinButton"

    def __init__(self, form, desc, parent):
        Control.__init__(self, form, desc, parent)
        p = desc.props
        for k, default in (("min", 0), ("max", 100), ("value", 0), ("small_change", 1), ("large_change", 1)):
            self._p[k] = p.get(k, default)

    def _state(self):
        p = self._p
        return Control._state(self) + [("min", str(p["min"])), ("max", str(p["max"])),
                                       ("small", str(p["small_change"])), ("large", str(p["large_change"])),
                                       ("value", str(p["value"]))]

    def _set_value(self, value, from_user=False):
        v = vba.to_long(value)
        lo, hi = min(self._p["min"], self._p["max"]), max(self._p["min"], self._p["max"])
        if not lo <= v <= hi:
            if from_user:
                v = max(lo, min(hi, v))
            else:
                raise VBAError(380, "Could not set the Value property. Invalid property value.")
        if v == self._p["value"]:
            return
        old = self._p["value"]
        self._p["value"] = v
        if not from_user:
            self._sync("value", str(v))
        if from_user and self._kind == "SpinButton":
            self._fire("spinup" if v > old else "spindown")
        self._fire("change")

    def _user(self, event, value):
        if event == "change":
            self._set_value(int(value), True)
        else:
            Control._user(self, event, value)

    Value = property(lambda s: s._p["value"], lambda s, v: s._set_value(v))
    Min = property(lambda s: s._p["min"], lambda s, v: s._setp("min", vba.to_long(v)))
    Max = property(lambda s: s._p["max"], lambda s, v: s._setp("max", vba.to_long(v)))
    SmallChange = property(lambda s: s._p["small_change"],
                           lambda s, v: s._setp("small_change", vba.to_long(v), "small"))
    LargeChange = property(lambda s: s._p["large_change"],
                           lambda s, v: s._setp("large_change", vba.to_long(v), "large"))

    def _vba_default_get(self):
        return self._p["value"]

    def _vba_default_let(self, value):
        self._set_value(value)


class Image(Control):
    _vba_typename = "Image"

    def __init__(self, form, desc, parent):
        Control.__init__(self, form, desc, parent)
        self._picture = desc.props.get("picture")

    def _state(self):
        out = Control._state(self)
        if self._picture:
            out.append(("picture", self._picture.hex()))
        return out

    Picture = property(lambda s: s._picture)
    PictureSizeMode = property(lambda s: 0, lambda s, v: None)


class Controls:
    """A form's, a Frame's or a page's Controls: by name, or by number
    from 0; For Each goes through them in their order."""
    _vba_typename = "Controls"

    def __init__(self, owner, items):
        self._owner = owner
        self._items = items

    @property
    def Count(self):
        return len(self._items)

    def Item(self, varg):
        if isinstance(varg, str):
            for c in self._items:
                if c._name.lower() == varg.lower():
                    return c
            raise VBAError(-2147024809, "Could not find the specified object.")
        i = vba.to_long(varg)
        if not 0 <= i < len(self._items):
            raise VBAError(-2147024809, "Invalid argument.")
        return self._items[i]

    def _vba_default_get(self, *args):
        return self.Item(*args)

    def _vba_iter(self):
        return iter(list(self._items))

    def Add(self, bstrProgID, Name=MISSING, Visible=True):
        """Controls.Add "Forms.TextBox.1", "txtNew": a control made at run
        time, at the top left until the macro moves it."""
        kind = vba.to_str(bstrProgID).split(".")
        kind = kind[1] if len(kind) > 1 else kind[0]
        desc = ControlDesc()
        desc.kind = {k.lower(): k for k in _CLASSES}.get(kind.lower(), "TextBox")
        form = self._owner._form if isinstance(self._owner, Control) else self._owner
        desc.name = vba.to_str(Name) if Name is not MISSING else "%s%d" % (desc.kind, len(list(form._walk())) + 1)
        desc.width, desc.height = 72.0, 18.0
        desc.visible = vba.to_bool(Visible)
        c = _make(form, desc, self._owner if isinstance(self._owner, Control) else None)
        self._items.append(c)
        form._by_name[c._name.lower()] = c
        form._add_to_host(c)
        return c

    def Remove(self, vIndex):
        c = self.Item(vIndex)
        self._items.remove(c)
        c._form._by_name.pop(c._name.lower(), None)
        c.Visible = False


class Frame(Control):
    """A Frame, or a MultiPage's page: a caption and controls of its own."""
    _vba_typename = "Frame"

    def __init__(self, form, desc, parent):
        Control.__init__(self, form, desc, parent)
        self._p["caption"] = desc.props.get("caption", "")
        self._children = [_make(form, d, self) for d in desc.children]

    def _state(self):
        return Control._state(self) + [("caption", self._p["caption"])]

    Caption = property(lambda s: s._p["caption"], lambda s, v: s._setp("caption", vba.to_str(v)))

    @property
    def Controls(self):
        return Controls(self, self._children)


class Page(Frame):
    _vba_typename = "Page"

    def __init__(self, form, page, parent, index):
        desc = ControlDesc()
        desc.kind = "Page"
        desc.name = getattr(page, "name", "Page%d" % (index + 1))
        desc.props["caption"] = page.caption
        desc.children = page.controls
        Frame.__init__(self, form, desc, parent)
        self._index = index

    Index = property(lambda s: s._index)

    def _sync(self, prop, value):
        if prop == "caption":
            self._parent._sync("pages", "\n".join(p._p["caption"] for p in self._parent._pages))


class Pages:
    _vba_typename = "Pages"

    def __init__(self, owner):
        self._owner = owner

    @property
    def Count(self):
        return len(self._owner._pages)

    def Item(self, varg):
        pages = self._owner._pages
        if isinstance(varg, str):
            for p in pages:
                if p._name.lower() == varg.lower():
                    return p
            raise VBAError(-2147024809, "Could not find the specified object.")
        i = vba.to_long(varg)
        if not 0 <= i < len(pages):
            raise VBAError(-2147024809, "Invalid argument.")
        return pages[i]

    def _vba_default_get(self, *args):
        return self.Item(*args)

    def _vba_iter(self):
        return iter(list(self._owner._pages))


class MultiPage(Control):
    _vba_typename = "MultiPage"

    def __init__(self, form, desc, parent):
        Control.__init__(self, form, desc, parent)
        self._pages = [Page(form, page, self, i) for i, page in enumerate(desc.pages)]
        self._p["value"] = desc.props.get("value", 0) if self._pages else -1

    def _state(self):
        return Control._state(self) + [("pages", "\n".join(p._p["caption"] for p in self._pages)),
                                       ("value", str(self._p["value"]))]

    def _set_value(self, value, from_user=False):
        v = vba.to_long(value)
        if not -1 <= v < len(self._pages):
            raise VBAError(380, "Could not set the Value property. Invalid property value.")
        if v == self._p["value"]:
            return
        self._p["value"] = v
        if not from_user:
            self._sync("value", str(v))
        self._fire("change")

    def _user(self, event, value):
        if event == "change":
            self._set_value(int(value), True)
        else:
            Control._user(self, event, value)

    Value = property(lambda s: s._p["value"], lambda s, v: s._set_value(v))

    @property
    def Pages(self):
        return Pages(self)

    @property
    def SelectedItem(self):
        v = self._p["value"]
        return self._pages[v] if 0 <= v < len(self._pages) else None

    def _vba_default_get(self):
        return self._p["value"]


_CLASSES = {"Label": Label, "CommandButton": CommandButton, "TextBox": TextBox,
            "CheckBox": CheckBox, "OptionButton": CheckBox, "ToggleButton": CheckBox,
            "ListBox": ListControl, "ComboBox": ListControl, "SpinButton": SpinButton,
            "ScrollBar": SpinButton, "Frame": Frame, "MultiPage": MultiPage, "Image": Image}


def _make(form, desc, parent):
    cls = _CLASSES.get(desc.kind, Control)
    c = cls(form, desc, parent)
    if cls in (CheckBox, ListControl, SpinButton):
        c._vba_typename = desc.kind
    return c


class FormInstance:
    """What a UserForm's instance is beside its module's variables: its
    caption and size, and its controls."""

    _vba_typename = "UserForm"

    def __init__(self, obj, design):
        self._obj = obj
        self._design = design
        self._hid = 0
        self._shown = False
        self._modal = False
        self._unloaded = False
        self._pending = None
        self._font_desc = design.font
        self._p = {"caption": design.caption, "width": design.width + 6, "height": design.height + 24,
                   "left": 0.0, "top": 0.0, "back": design.back_colour, "fore": design.fore_colour,
                   "tag": "", "start": design.start_position, "enabled": True}
        self._font = NewFont(self, design.font)
        self._controls = [_make(self, d, None) for d in design.controls]
        self._by_name = {c._name.lower(): c for c in self._walk()}

    def _walk(self):
        stack = list(reversed(self._controls))
        while stack:
            c = stack.pop()
            yield c
            if isinstance(c, Frame):
                stack.extend(reversed(c._children))
            if isinstance(c, MultiPage):
                for p in reversed(c._pages):
                    stack.extend(reversed(p._children))

    # -- the macro's view --
    def _vba_get(self, lname, *args):
        c = self._by_name.get(lname)
        if c is None:
            raise VBAError(438, "Object doesn't support this property or method (%s)" % lname)
        if args:
            return vba.default_member(c, *args)
        return c

    def _vba_let(self, lname, value, *args):
        c = self._by_name.get(lname)
        if c is None:
            raise VBAError(438, "Object doesn't support this property or method (%s)" % lname)
        c._vba_default_let(value)

    def _sync(self, prop, value):
        if self._hid:
            _c.form_set(self._hid, 0, prop, value)

    def _setp(self, key, value, prop=None, text=None):
        self._p[key] = value
        self._sync(prop or key, text if text is not None else str(value))

    Caption = property(lambda s: s._p["caption"], lambda s, v: s._setp("caption", vba.to_str(v)))
    Width = property(lambda s: s._p["width"], lambda s, v: s._setp("width", vba.to_double(v), "width",
                                                                  _points(vba.to_double(v) - 6)))
    Height = property(lambda s: s._p["height"], lambda s, v: s._setp("height", vba.to_double(v), "height",
                                                                    _points(vba.to_double(v) - 24)))
    Left = property(lambda s: s._p["left"], lambda s, v: s._p.__setitem__("left", vba.to_double(v)))
    Top = property(lambda s: s._p["top"], lambda s, v: s._p.__setitem__("top", vba.to_double(v)))
    InsideWidth = property(lambda s: s._p["width"] - 6)
    InsideHeight = property(lambda s: s._p["height"] - 24)
    BackColor = property(lambda s: s._p["back"], lambda s, v: s._setp("back", vba.to_long(v), "back",
                                                                     html(vba.to_long(v))))
    ForeColor = property(lambda s: s._p["fore"], lambda s, v: s._p.__setitem__("fore", vba.to_long(v)))
    Tag = property(lambda s: s._p["tag"], lambda s, v: s._p.__setitem__("tag", vba.to_str(v)))
    StartUpPosition = property(lambda s: s._p["start"], lambda s, v: s._p.__setitem__("start", vba.to_long(v)))
    Enabled = property(lambda s: s._p["enabled"], lambda s, v: s._setp("enabled", vba.to_bool(v), "enabled",
                                                                      _bool_text(vba.to_bool(v))))
    Font = property(lambda s: s._font)
    Visible = property(lambda s: s._shown)
    Name = property(lambda s: s._obj.cls.name)
    ActiveControl = property(lambda s: None)
    ScrollBars = property(lambda s: 0, lambda s, v: None)
    KeepScrollBarsVisible = property(lambda s: 0, lambda s, v: None)

    @property
    def Controls(self):
        return Controls(self, self._controls)

    # -- showing --
    def _open(self):
        if self._hid:
            return
        try:
            hid = _c.form_open(self._p["caption"], self._p["width"] - 6, self._p["height"] - 24)
        except RuntimeError:
            raise VBAError(1004, "There is no window to show the form %s in" % self.Name)
        self._hid = hid
        _shown[hid] = self
        for key, value in (("back", html(self._p["back"])), ("font", _font_text(self._font)),
                           ("start", str(self._p["start"]))):
            _c.form_set(hid, 0, key, value)
        for c in self._controls:
            self._add_to_host(c)

    def _add_to_host(self, c, parent_id=0):
        if not self._hid:
            return
        if c._parent is not None and parent_id == 0 and not isinstance(c._parent, MultiPage):
            parent_id = c._parent._hid
        c._hid = _c.form_add(self._hid, parent_id, c._kind, c._name)
        for key, value in c._state():
            _c.form_set(self._hid, c._hid, key, value)
        if isinstance(c, Frame):
            for child in c._children:
                self._add_to_host(child, c._hid)
        if isinstance(c, MultiPage):
            for k, page in enumerate(c._pages):
                page._hid = _c.form_add(self._hid, c._hid, "Page", page._name)
                for child in page._children:
                    self._add_to_host(child, page._hid)

    def Show(self, Modal=1):
        if self._unloaded:
            raise VBAError(-2147418105, "Automation error: the form was unloaded")
        modal = vba.to_long(Modal) != 0
        if self._shown:
            if modal and self._modal:
                raise VBAError(400, "Form already displayed; can't show modally")
            return
        self._open()
        self._shown = True
        self._modal = modal
        self._fire("userform_activate")
        _c.form_show(self._hid, modal)
        if modal:
            self._shown = False
            pending, self._pending = self._pending, None
            if pending is not None:
                raise pending

    def Hide(self):
        if self._shown and self._hid:
            _c.form_hide(self._hid)
        self._shown = False

    def Repaint(self):
        return None

    def PrintForm(self):
        return None

    def _unload(self, mode):
        """Unload: QueryClose may keep the form; else the window goes, and
        the instance with it once nothing holds it."""
        cancel = self._fire("userform_queryclose", vba.RefArg(0), mode)
        if cancel:
            return False
        if self._hid:
            _c.form_close(self._hid)
            _shown.pop(self._hid, None)
            self._hid = 0
        self._shown = False
        self._unloaded = True
        m = self._obj.cls
        if m.default is self._obj:
            m.default = None
        return True

    # -- events --
    def _fire(self, lname, *args):
        """Runs the form module's handler if it has one; for QueryClose
        and DblClick, whether it cancelled."""
        obj = self._obj
        group = obj.cls.procs.get(lname)
        proc = group.get("sub") if group else None
        if proc is None:
            return False
        project = obj.cls.project
        values = [a if isinstance(a, vba.ValueArg) else vba.ValueArg(a) for a in args]
        project.invoke(proc, values, None, obj)
        for a in args:
            if isinstance(a, vba.RefArg):
                return vba.to_bool(a.cell.value)
            if isinstance(a, ReturnBoolean):
                return vba.to_bool(a.Value)
        return False


def instance(obj):
    """The host's form_instance: a UserForm's controls, from its design."""
    return FormInstance(obj, obj.cls.form)


def unload(obj):
    ext = getattr(obj, "ext", None)
    if not isinstance(ext, FormInstance):
        raise VBAError(361, "Can't load or unload this object")
    if not ext._unloaded:
        ext._unload(1)


def _event(hid, control, event, value):
    """What the user did, from the host: True when the macro cancelled it
    (QueryClose refusing the close box).  An error, or End, in a handler
    closes a modal form and goes on from its Show."""
    form = _shown.get(hid)
    if form is None:
        return False
    try:
        if control == 0:
            if event == "close":
                return not form._unload(0)
            if event == "click":
                form._fire("userform_click")
            return False
        for c in form._walk():
            if c._hid == control:
                c._user(event, value)
                break
    except (VBAError, vba.VBASyntaxError, vba.EndSignal, vba.StopSignal) as e:
        if form._modal and form._shown:
            form._pending = e
            if form._hid:
                _c.form_hide(form._hid)
            form._shown = False
        else:
            raise
    return False
