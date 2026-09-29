# vbaxml.py - Microsoft XML for Visual Basic: the DOM, XPath and XMLHTTP
#
# Copyright (C) 2026 The office42 authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# What a macro gets from CreateObject("MSXML2.DOMDocument") or a
# reference to Microsoft XML: a document to load, walk, change and
# save, with selectNodes and selectSingleNode taking XPath; and
# XMLHTTP, which fetches a URL, the other half of most macros that read
# XML (or JSON) from a server.  The tree underneath is Python's
# minidom; each node a macro holds is a wrapper the document keeps, so
# that the same node is always the same object and Is says so.
#
# XPath is XPath 1.0, all of it but variables and the namespace axis.
# Version 6.0 of the document matches a name with no prefix only in no
# namespace, as XPath says; the older ones, whose default was Microsoft's
# XSL Patterns, match it by the name as written, which is what the
# macros written for them expect.

import math
import re
import urllib.error
import urllib.parse
import urllib.request
import xml.dom
import xml.dom.minidom as minidom
import xml.parsers.expat

import o42vba as vba
from o42vba import VBAError, VBArray, MISSING, EMPTY, NULL

ELEMENT, ATTRIBUTE, TEXT, CDATA, ENTITY_REF, ENTITY, PI, COMMENT, DOCUMENT, DOCTYPE, FRAGMENT = range(1, 12)

_TYPE_STRINGS = {ELEMENT: "element", ATTRIBUTE: "attribute", TEXT: "text", CDATA: "cdatasection",
                 PI: "processinginstruction", COMMENT: "comment", DOCUMENT: "document",
                 DOCTYPE: "documenttype", FRAGMENT: "documentfragment"}

_TYPE_NAMES = {ELEMENT: "IXMLDOMElement", ATTRIBUTE: "IXMLDOMAttribute", TEXT: "IXMLDOMText",
               CDATA: "IXMLDOMCDATASection", PI: "IXMLDOMProcessingInstruction",
               COMMENT: "IXMLDOMComment", DOCTYPE: "IXMLDOMDocumentType",
               FRAGMENT: "IXMLDOMDocumentFragment"}


def _string_value(n):
    """XPath's string-value of a minidom node."""
    t = n.nodeType
    if t in (ATTRIBUTE,):
        return n.value
    if t in (TEXT, CDATA, COMMENT, PI):
        return n.data
    out = []

    def walk(m):
        for c in m.childNodes:
            if c.nodeType in (TEXT, CDATA):
                out.append(c.data)
            elif c.nodeType == ELEMENT:
                walk(c)
    walk(n)
    return "".join(out)


class ParseError:
    _vba_typename = "IXMLDOMParseError"

    def __init__(self, code=0, reason="", line=0, linepos=0, src="", url=""):
        self.errorCode = code
        self.reason = reason
        self.line = line
        self.linepos = linepos
        self.srcText = src
        self.url = url
        self.filepos = 0


class NodeList:
    """A node list: childNodes (live, read from the parent each time),
    or what selectNodes and getElementsByTagName found."""

    _vba_typename = "IXMLDOMNodeList"

    def __init__(self, doc, nodes=None, parent=None):
        self._doc = doc
        self._nodes = nodes
        self._parent = parent
        self._next = 0

    def _list(self):
        if self._parent is not None:
            return list(self._parent.childNodes)
        return self._nodes

    @property
    def length(self):
        return len(self._list())

    def item(self, index):
        nodes = self._list()
        i = vba.to_long(index)
        return self._doc._wrap(nodes[i]) if 0 <= i < len(nodes) else None

    def nextNode(self):
        nodes = self._list()
        if self._next >= len(nodes):
            return None
        self._next += 1
        return self._doc._wrap(nodes[self._next - 1])

    def reset(self):
        self._next = 0

    def _vba_default_get(self, *args):
        if not args:
            raise VBAError(450)
        return self.item(args[0])

    def _vba_iter(self):
        return [self._doc._wrap(n) for n in self._list()]


class Selection(NodeList):
    _vba_typename = "IXMLDOMSelection"

    def __init__(self, doc, nodes, expr):
        NodeList.__init__(self, doc, nodes)
        self.expr = expr

    def removeAll(self):
        for n in self._nodes:
            if n.nodeType == ATTRIBUTE:
                if n.ownerElement is not None:
                    n.ownerElement.removeAttributeNode(n)
            elif n.parentNode is not None:
                n.parentNode.removeChild(n)

    def peekNode(self):
        return self._doc._wrap(self._nodes[self._next]) if self._next < len(self._nodes) else None


class NamedNodeMap:
    """An element's attributes."""

    _vba_typename = "IXMLDOMNamedNodeMap"

    def __init__(self, doc, element):
        self._doc = doc
        self._el = element
        self._next = 0

    def _list(self):
        a = self._el.attributes
        return [a.item(i) for i in range(a.length)]

    @property
    def length(self):
        return len(self._list())

    def item(self, index):
        nodes = self._list()
        i = vba.to_long(index)
        return self._doc._wrap(nodes[i]) if 0 <= i < len(nodes) else None

    def getNamedItem(self, name):
        n = self._el.getAttributeNode(vba.to_str(name))
        return self._doc._wrap(n) if n is not None else None

    def setNamedItem(self, newItem):
        self._el.setAttributeNode(newItem._n)
        return newItem

    def removeNamedItem(self, name):
        n = self._el.getAttributeNode(vba.to_str(name))
        if n is None:
            return None
        self._el.removeAttributeNode(n)
        return self._doc._wrap(n)

    def nextNode(self):
        nodes = self._list()
        if self._next >= len(nodes):
            return None
        self._next += 1
        return self._doc._wrap(nodes[self._next - 1])

    def reset(self):
        self._next = 0

    def _vba_default_get(self, *args):
        return self.item(args[0])

    def _vba_iter(self):
        return [self._doc._wrap(n) for n in self._list()]


class Node:
    """IXMLDOMNode, and what each kind of node adds to it: an element's
    attributes, a text's data, an attribute's value."""

    def __init__(self, doc, node):
        self._doc = doc
        self._n = node

    @property
    def _vba_typename(self):
        return _TYPE_NAMES.get(self._n.nodeType, "IXMLDOMNode")

    # -- what it is --
    @property
    def nodeType(self):
        return self._n.nodeType

    @property
    def nodeTypeString(self):
        return _TYPE_STRINGS.get(self._n.nodeType, "")

    @property
    def nodeName(self):
        t = self._n.nodeType
        return {TEXT: "#text", CDATA: "#cdata-section", COMMENT: "#comment",
                DOCUMENT: "#document", FRAGMENT: "#document-fragment"}.get(t) or self._n.nodeName

    @property
    def baseName(self):
        t = self._n.nodeType
        if t in (ELEMENT, ATTRIBUTE):
            return self._n.localName or self._n.nodeName.split(":")[-1]
        return self.nodeName if t == PI else ""

    @property
    def prefix(self):
        return getattr(self._n, "prefix", None) or ""

    @property
    def namespaceURI(self):
        return getattr(self._n, "namespaceURI", None) or ""

    @property
    def tagName(self):
        return self._n.tagName

    @property
    def name(self):
        return self._n.nodeName

    @property
    def target(self):
        return self._n.target

    # -- its value and its text --
    @property
    def nodeValue(self):
        t = self._n.nodeType
        if t == ATTRIBUTE:
            return self._n.value
        if t in (TEXT, CDATA, COMMENT, PI):
            return self._n.data
        return NULL

    @nodeValue.setter
    def nodeValue(self, v):
        t = self._n.nodeType
        if t == ATTRIBUTE:
            self._n.value = vba.to_str(v)
        elif t in (TEXT, CDATA, COMMENT, PI):
            self._n.data = vba.to_str(v)
        else:
            raise VBAError(-2147467259, "This operation cannot be performed on a node of this type.")

    value = nodeValue
    data = nodeValue

    @property
    def length(self):
        return len(self._n.data)

    def substringData(self, offset, count):
        o = vba.to_long(offset)
        return self._n.data[o:o + vba.to_long(count)]

    def appendData(self, data):
        self._n.data += vba.to_str(data)

    def insertData(self, offset, data):
        o = vba.to_long(offset)
        self._n.data = self._n.data[:o] + vba.to_str(data) + self._n.data[o:]

    def deleteData(self, offset, count):
        o = vba.to_long(offset)
        self._n.data = self._n.data[:o] + self._n.data[o + vba.to_long(count):]

    def replaceData(self, offset, count, data):
        o = vba.to_long(offset)
        self._n.data = self._n.data[:o] + vba.to_str(data) + self._n.data[o + vba.to_long(count):]

    @property
    def text(self):
        s = _string_value(self._n)
        # Without preserveWhiteSpace, MSXML gives the words without the
        # space around them.
        return s if self._doc.preserveWhiteSpace else s.strip()

    @text.setter
    def text(self, v):
        n = self._n
        s = vba.to_str(v)
        if n.nodeType in (ELEMENT, FRAGMENT):
            for c in list(n.childNodes):
                n.removeChild(c)
            if s:
                n.appendChild(self._doc._dom.createTextNode(s))
        elif n.nodeType == ATTRIBUTE:
            n.value = s
        elif n.nodeType in (TEXT, CDATA, COMMENT, PI):
            n.data = s
        else:
            raise VBAError(-2147467259, "This operation cannot be performed on a node of this type.")

    @property
    def nodeTypedValue(self):
        return self.text

    @nodeTypedValue.setter
    def nodeTypedValue(self, v):
        self.text = v

    @property
    def dataType(self):
        return NULL

    @property
    def specified(self):
        return True

    @property
    def parsed(self):
        return True

    @property
    def xml(self):
        n = self._n
        if n.nodeType == ELEMENT and n.namespaceURI and n.parentNode is not None:
            # An element taken out of its document says which namespace
            # its prefix is, as MSXML writes it.
            attr = "xmlns:" + n.prefix if n.prefix else "xmlns"
            if not n.hasAttribute(attr):
                n = n.cloneNode(True)
                n.setAttribute(attr, self._n.namespaceURI)
        return _serialize(n)

    # -- where it is --
    @property
    def parentNode(self):
        p = self._n.parentNode if self._n.nodeType != ATTRIBUTE else None
        return self._doc._wrap(p) if p is not None else None

    @property
    def childNodes(self):
        return NodeList(self._doc, parent=self._n)

    @property
    def firstChild(self):
        return self._doc._wrap(self._n.firstChild) if self._n.firstChild is not None else None

    @property
    def lastChild(self):
        return self._doc._wrap(self._n.lastChild) if self._n.lastChild is not None else None

    @property
    def nextSibling(self):
        n = self._n.nextSibling if self._n.nodeType != ATTRIBUTE else None
        return self._doc._wrap(n) if n is not None else None

    @property
    def previousSibling(self):
        n = self._n.previousSibling if self._n.nodeType != ATTRIBUTE else None
        return self._doc._wrap(n) if n is not None else None

    @property
    def ownerDocument(self):
        return self._doc

    @property
    def attributes(self):
        return NamedNodeMap(self._doc, self._n) if self._n.nodeType == ELEMENT else None

    def hasChildNodes(self):
        return bool(self._n.childNodes)

    # -- changing the tree --
    def _dom_node(self, v):
        if not isinstance(v, Node):
            raise VBAError(13)
        return v._n

    def appendChild(self, newChild):
        n = self._dom_node(newChild)
        try:
            self._n.appendChild(n)
        except xml.dom.DOMException as e:
            raise VBAError(-2147467259, str(e) or "This operation cannot be performed.")
        return newChild

    def insertBefore(self, newChild, refChild=MISSING):
        n = self._dom_node(newChild)
        ref = None if refChild is MISSING or refChild is None or refChild is EMPTY else self._dom_node(refChild)
        self._n.insertBefore(n, ref)
        return newChild

    def removeChild(self, childNode):
        n = self._dom_node(childNode)
        if n.parentNode is not self._n:
            raise VBAError(-2147024809, "The parameter node is not a child of this node.")
        self._n.removeChild(n)
        return childNode

    def replaceChild(self, newChild, oldChild):
        self._n.replaceChild(self._dom_node(newChild), self._dom_node(oldChild))
        return oldChild

    def cloneNode(self, deep):
        return self._doc._wrap(self._n.cloneNode(vba.to_bool(deep)))

    def normalize(self):
        self._n.normalize()

    # -- an element's --
    def getAttribute(self, name):
        n = self._n.getAttributeNode(vba.to_str(name)) if self._n.nodeType == ELEMENT else None
        return n.value if n is not None else NULL

    def setAttribute(self, name, value):
        self._n.setAttribute(vba.to_str(name), vba.to_str(value))

    def removeAttribute(self, name):
        if self._n.hasAttribute(vba.to_str(name)):
            self._n.removeAttribute(vba.to_str(name))

    def getAttributeNode(self, name):
        n = self._n.getAttributeNode(vba.to_str(name))
        return self._doc._wrap(n) if n is not None else None

    def setAttributeNode(self, DOMAttribute):
        old = self._n.setAttributeNode(DOMAttribute._n)
        return self._doc._wrap(old) if old is not None else None

    def removeAttributeNode(self, DOMAttribute):
        self._n.removeAttributeNode(DOMAttribute._n)
        return DOMAttribute

    def getElementsByTagName(self, tagName):
        name = vba.to_str(tagName)
        found = []

        def walk(m):
            for c in m.childNodes:
                if c.nodeType == ELEMENT:
                    if name == "*" or c.nodeName == name:
                        found.append(c)
                    walk(c)
        walk(self._n)
        return NodeList(self._doc, found)

    # -- XPath --
    def selectNodes(self, queryString):
        q = vba.to_str(queryString)
        return Selection(self._doc, self._doc._select(q, self._n), q)

    def selectSingleNode(self, queryString):
        found = self._doc._select(vba.to_str(queryString), self._n, first=True)
        return self._doc._wrap(found[0]) if found else None

    def transformNode(self, stylesheet):
        raise VBAError(-2147467259, "office42 does not run XSL transformations.")

    def splitText(self, offset):
        return self._doc._wrap(self._n.splitText(vba.to_long(offset)))


def _serialize(n):
    """The node's markup as MSXML writes it: a document's top-level
    nodes each on a line of its own, ended CR LF; an empty element
    closed in itself."""
    t = n.nodeType
    if t == DOCUMENT:
        return "".join(_serialize(c) + "\r\n" for c in n.childNodes)
    if t == FRAGMENT:
        return "".join(_serialize(c) for c in n.childNodes)
    if t == ELEMENT:
        a = n.attributes
        attrs = "".join(" " + _serialize(a.item(i)) for i in range(a.length))
        inner = "".join(_serialize(c) for c in n.childNodes)
        return "<%s%s>%s</%s>" % (n.nodeName, attrs, inner, n.nodeName) if inner else "<%s%s/>" % (n.nodeName, attrs)
    if t == ATTRIBUTE:
        return '%s="%s"' % (n.nodeName, n.value.replace("&", "&amp;").replace("<", "&lt;").replace('"', "&quot;"))
    if t == TEXT:
        return n.data.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    if t == CDATA:
        return "<![CDATA[%s]]>" % n.data
    if t == COMMENT:
        return "<!--%s-->" % n.data
    if t == PI:
        return "<?%s %s?>" % (n.target, n.data) if n.data else "<?%s?>" % n.target
    return n.toxml()


class DOMDocument(Node):
    """The document: load and loadXML, the root, the factories, save."""

    def __init__(self, version=3):
        self._version = version
        self._cache = {}
        self._dom = minidom.Document()
        Node.__init__(self, self, self._dom)
        self.async_ = True
        self.validateOnParse = True
        self.resolveExternals = False
        self.preserveWhiteSpace = False
        self.url = ""
        self.parseError = ParseError()
        self._namespaces = {}
        self._xpath = version >= 6

    _vba_aliases = {"async": "async_"}

    @property
    def _vba_typename(self):
        return {6: "DOMDocument60", 4: "DOMDocument40"}.get(self._version, "DOMDocument")

    @property
    def readyState(self):
        return 4

    def _wrap(self, n):
        if n is None:
            return None
        if n.nodeType == DOCUMENT:
            return self
        got = self._cache.get(id(n))
        if got is None or got[0] is not n:
            got = (n, Node(self, n))
            self._cache[id(n)] = got
        return got[1]

    def _take(self, dom):
        self._dom = dom
        self._n = dom
        self._cache = {}

    # -- loading --
    def loadXML(self, bstrXML):
        return self._parse(vba.to_str(bstrXML), "")

    def load(self, xmlSource):
        if isinstance(xmlSource, DOMDocument):
            return self._parse(xmlSource.xml, "")
        src = vba.to_str(xmlSource)
        self.url = src
        try:
            if re.match(r"(?i)https?://", src):
                status, reason, headers, data = _fetch("GET", src, {}, None, 60)
                if status >= 400:
                    raise urllib.error.URLError(reason)
            else:
                path = src
                if src.lower().startswith("file:"):
                    path = urllib.request.url2pathname(urllib.parse.urlparse(src).path)
                with open(path, "rb") as f:
                    data = f.read()
        except (OSError, urllib.error.URLError, ValueError):
            self._take(minidom.Document())
            self.parseError = ParseError(-2146697210, "The system cannot locate the object specified.\r\n", url=src)
            return False
        return self._parse(data, src)

    def _parse(self, data, url):
        text = data if isinstance(data, str) else None
        raw = data.encode("utf-8") if isinstance(data, str) else data
        # A string handed to loadXML is text already: an encoding its
        # declaration names does not apply to it.
        if text is not None:
            raw = re.sub(rb"^(\s*<\?xml[^>]*?)\s+encoding\s*=\s*(['\"])[^'\"]*\2", rb"\1", raw)
        try:
            dom = minidom.parseString(raw)
        except xml.parsers.expat.ExpatError as e:
            self._take(minidom.Document())
            lines = (text if text is not None else raw.decode("utf-8", "replace")).split("\n")
            src = lines[e.lineno - 1] if 0 < e.lineno <= len(lines) else ""
            self.parseError = ParseError(-1072896659, xml.parsers.expat.ErrorString(e.code).capitalize() + ".\r\n",
                                         e.lineno, e.offset + 1, src.rstrip("\r"), url)
            return False
        if not self.preserveWhiteSpace:
            _strip_space(dom)
        # The declaration is a node of the document in MSXML: the
        # first of its childNodes, and written again with it.
        m = re.match(rb"\s*<\?xml\s+(.*?)\?>", raw, re.S)
        if m:
            decl = m.group(1).decode("utf-8", "replace").strip()
            dom.insertBefore(dom.createProcessingInstruction("xml", decl), dom.firstChild)
        self._take(dom)
        self.parseError = ParseError(url=url)
        return True

    def save(self, destination):
        if isinstance(destination, DOMDocument):
            destination.loadXML(self.xml)
            return
        path = vba.to_str(destination)
        encoding = "utf-8"
        first = self._dom.firstChild
        if first is not None and first.nodeType == PI and first.target == "xml":
            m = re.search(r"encoding\s*=\s*['\"]([^'\"]+)", first.data)
            if m:
                encoding = m.group(1)
        try:
            data = self.xml.encode(encoding, "xmlcharrefreplace")
        except LookupError:
            data = self.xml.encode("utf-8")
        try:
            with open(path, "wb") as f:
                f.write(data)
        except OSError:
            raise VBAError(-2147024893, "The system cannot find the path specified.")

    def abort(self):
        pass

    def validate(self):
        return ParseError()

    # -- the root --
    @property
    def documentElement(self):
        return self._wrap(self._dom.documentElement)

    @documentElement.setter
    def documentElement(self, v):
        old = self._dom.documentElement
        if old is not None:
            self._dom.replaceChild(v._n, old)
        else:
            self._dom.appendChild(v._n)

    @property
    def doctype(self):
        return self._wrap(self._dom.doctype)

    @property
    def xml(self):
        return _serialize(self._dom)

    # -- the factories --
    def createElement(self, tagName):
        return self._wrap(self._dom.createElement(vba.to_str(tagName)))

    def createTextNode(self, data):
        return self._wrap(self._dom.createTextNode(vba.to_str(data)))

    def createAttribute(self, name):
        return self._wrap(self._dom.createAttribute(vba.to_str(name)))

    def createCDATASection(self, data):
        return self._wrap(self._dom.createCDATASection(vba.to_str(data)))

    def createComment(self, data):
        return self._wrap(self._dom.createComment(vba.to_str(data)))

    def createProcessingInstruction(self, target, data):
        return self._wrap(self._dom.createProcessingInstruction(vba.to_str(target), vba.to_str(data)))

    def createDocumentFragment(self):
        return self._wrap(self._dom.createDocumentFragment())

    def createNode(self, Type, name, namespaceURI):
        t = vba.to_long(Type) if not isinstance(Type, str) else \
            {v: k for k, v in _TYPE_STRINGS.items()}.get(Type.lower(), 0)
        name = vba.to_str(name)
        ns = vba.to_str(namespaceURI) or None
        if t == ELEMENT:
            n = self._dom.createElementNS(ns, name) if ns else self._dom.createElement(name)
            if ns:
                prefix = name.split(":")[0] if ":" in name else None
                n.setAttribute("xmlns:" + prefix if prefix else "xmlns", ns)
            return self._wrap(n)
        if t == ATTRIBUTE:
            return self._wrap(self._dom.createAttributeNS(ns, name) if ns else self._dom.createAttribute(name))
        if t == TEXT:
            return self.createTextNode("")
        if t == CDATA:
            return self.createCDATASection("")
        if t == COMMENT:
            return self.createComment("")
        if t == PI:
            return self.createProcessingInstruction(name, "")
        if t == FRAGMENT:
            return self.createDocumentFragment()
        raise VBAError(5)

    # -- what XPath is asked in --
    def setProperty(self, name, value):
        n = vba.to_str(name).lower()
        if n == "selectionlanguage":
            self._xpath = vba.to_str(value).lower() == "xpath"
        elif n == "selectionnamespaces":
            self._namespaces = dict((p or "", u) for p, u in re.findall(
                r"xmlns(?::([\w.-]+))?\s*=\s*['\"]([^'\"]*)['\"]", vba.to_str(value)))
        # ProhibitDTD, AllowDocumentFunction, ServerHTTPRequest and the
        # rest change nothing office42 does.

    def getProperty(self, name):
        n = vba.to_str(name).lower()
        if n == "selectionlanguage":
            return "XPath" if self._xpath else "XSLPattern"
        if n == "selectionnamespaces":
            return " ".join("xmlns%s='%s'" % (":" + p if p else "", u) for p, u in self._namespaces.items())
        return EMPTY

    def _select(self, query, context, first=False):
        try:
            found = XPath(query, self._namespaces, self._xpath).select(context)
        except XPathError as e:
            raise VBAError(-2147467259, "Expression error: %s\r\n\r\n%s" % (e, query))
        return found[:1] if first else found


def _strip_space(n):
    """What MSXML does to a document loaded without preserveWhiteSpace:
    text that is only white space goes, but not under xml:space."""
    for c in list(n.childNodes):
        if c.nodeType == TEXT and not c.data.strip():
            n.removeChild(c)
        elif c.nodeType == ELEMENT and c.getAttribute("xml:space") != "preserve":
            _strip_space(c)


# ---- XPath ------------------------------------------------------------

class XPathError(Exception):
    pass


_TOKEN = re.compile(r"""\s*(?:
    (?P<num>\d+(?:\.\d*)?|\.\d+) |
    (?P<str>"[^"]*"|'[^']*') |
    (?P<op>//|::|\.\.|!=|<=|>=|[/()\[\]@,|=<>+\-*.$]) |
    (?P<name>[A-Za-z_À-￿][\w.\-·À-￿]*(?::(?:\*|[A-Za-z_À-￿][\w.\-·À-￿]*))?)
    )""", re.X)

_AXES = ("ancestor", "ancestor-or-self", "attribute", "child", "descendant", "descendant-or-self",
         "following", "following-sibling", "namespace", "parent", "preceding", "preceding-sibling", "self")
_NODE_TYPES = ("node", "text", "comment", "processing-instruction")


def _tokens(text):
    out = []
    pos = 0
    text = text.rstrip()
    while pos < len(text):
        m = _TOKEN.match(text, pos)
        if m is None or m.end() == pos:
            raise XPathError("unexpected character: %r" % text[pos:pos + 10].strip())
        pos = m.end()
        kind = m.lastgroup
        v = m.group(kind)
        if kind == "str":
            v = v[1:-1]
        out.append((kind, v))
    # XPath's rules for "*" and a name that could be an operator: they
    # are operators when a value stands before them.
    fixed = []
    for i, (kind, v) in enumerate(out):
        if i > 0 and (kind == "name" and v in ("and", "or", "div", "mod") or (kind, v) == ("op", "*")):
            pk, pv = fixed[-1]
            if not (pk == "op" and pv in ("@", "::", "(", "[", ",", "/", "//", "|", "+", "-", "=", "!=",
                                          "<", "<=", ">", ">=")) and not pk == "oper":
                fixed.append(("oper", v))
                continue
        fixed.append((kind, v))
    return fixed


class XPath:
    """An expression compiled to a tree of tuples, evaluated against a
    minidom node."""

    def __init__(self, text, namespaces, strict):
        self.namespaces = namespaces
        self.strict = strict
        self.toks = _tokens(text)
        self.i = 0
        self.tree = self.parse_or()
        if self.i != len(self.toks):
            raise XPathError("unexpected %s" % self.toks[self.i][1])

    # -- parsing --
    def peek(self, k=0):
        return self.toks[self.i + k] if self.i + k < len(self.toks) else (None, None)

    def take(self):
        t = self.peek()
        self.i += 1
        return t

    def want(self, v):
        t = self.take()
        if t[1] != v:
            raise XPathError("expected %s" % v)

    def binary(self, sub, ops):
        left = sub()
        while self.peek()[0] in ("oper", "op") and self.peek()[1] in ops:
            op = self.take()[1]
            left = ("bin", op, left, sub())
        return left

    def parse_or(self):
        return self.binary(self.parse_and, ("or",))

    def parse_and(self):
        return self.binary(self.parse_eq, ("and",))

    def parse_eq(self):
        return self.binary(self.parse_rel, ("=", "!="))

    def parse_rel(self):
        return self.binary(self.parse_add, ("<", "<=", ">", ">="))

    def parse_add(self):
        return self.binary(self.parse_mul, ("+", "-"))

    def parse_mul(self):
        return self.binary(self.parse_unary, ("*", "div", "mod"))

    def parse_unary(self):
        if self.peek() == ("op", "-"):
            self.take()
            return ("neg", self.parse_unary())
        return self.binary(self.parse_path, ("|",))

    def parse_path(self):
        kind, v = self.peek()
        if kind in ("num", "str") or v in ("(", "$") or (
                kind == "name" and self.peek(1)[1] == "(" and v not in _NODE_TYPES):
            e = self.parse_primary()
            preds = self.parse_predicates()
            if preds:
                e = ("filter", e, preds)
            if self.peek()[1] in ("/", "//"):
                return ("path", e, self.parse_relative(self.take()[1] == "//"))
            return e
        if v == "/":
            self.take()
            k2, v2 = self.peek()
            if k2 == "name" or v2 in (".", "..", "@", "*") or k2 == "oper" and v2 == "*":
                return ("root", self.parse_relative(False))
            return ("root", [])
        if v == "//":
            self.take()
            return ("root", self.parse_relative(True))
        return ("rel", self.parse_relative(False))

    def parse_relative(self, deep):
        steps = []
        if deep:
            steps.append(("descendant-or-self", ("type", "node"), []))
        steps.append(self.parse_step())
        while self.peek()[1] in ("/", "//"):
            if self.take()[1] == "//":
                steps.append(("descendant-or-self", ("type", "node"), []))
            steps.append(self.parse_step())
        return steps

    def parse_step(self):
        kind, v = self.peek()
        if v == ".":
            self.take()
            return ("self", ("type", "node"), [])
        if v == "..":
            self.take()
            return ("parent", ("type", "node"), [])
        axis = "child"
        if v == "@":
            self.take()
            axis = "attribute"
        elif kind == "name" and self.peek(1)[1] == "::":
            if v not in _AXES:
                raise XPathError("no axis %s" % v)
            axis = v
            self.take()
            self.take()
        kind, v = self.take()
        if kind == "name" and v in _NODE_TYPES and self.peek()[1] == "(":
            self.take()
            arg = None
            if self.peek()[0] == "str":
                arg = self.take()[1]
            self.want(")")
            test = ("type", v, arg)
        elif v == "*" and kind in ("op", "oper"):
            test = ("any",)
        elif kind == "name":
            test = ("name", v)
        else:
            raise XPathError("expected a name after %s" % (axis + "::"))
        return (axis, test, self.parse_predicates())

    def parse_predicates(self):
        preds = []
        while self.peek()[1] == "[":
            self.take()
            preds.append(self.parse_or())
            self.want("]")
        return preds

    def parse_primary(self):
        kind, v = self.take()
        if kind == "num":
            return ("lit", float(v))
        if kind == "str":
            return ("lit", v)
        if v == "(":
            e = self.parse_or()
            self.want(")")
            return e
        if v == "$":
            raise XPathError("variables are not supported")
        self.want("(")
        args = []
        if self.peek()[1] != ")":
            args.append(self.parse_or())
            while self.peek()[1] == ",":
                self.take()
                args.append(self.parse_or())
        self.want(")")
        if v not in _FUNCTIONS:
            raise XPathError("unknown function %s()" % v)
        return ("call", v, args)

    # -- evaluating --
    def select(self, context):
        v = self.eval(self.tree, context, 1, 1)
        if not isinstance(v, list):
            raise XPathError("Expression must evaluate to a node-set")
        return v

    def order(self, nodes):
        if len(nodes) < 2:
            return nodes
        doc = nodes[0].ownerDocument or nodes[0]
        index = {}
        count = 0

        def walk(n):
            nonlocal count
            index[id(n)] = count
            count += 1
            if n.nodeType == ELEMENT:
                a = n.attributes
                for i in range(a.length):
                    index[id(a.item(i))] = count
                    count += 1
            for c in n.childNodes:
                walk(c)
        walk(doc)
        seen = set()
        out = []
        for n in sorted(nodes, key=lambda n: index.get(id(n), -1)):
            if id(n) not in seen:
                seen.add(id(n))
                out.append(n)
        return out

    def eval(self, e, node, pos, size):
        op = e[0]
        if op == "lit":
            return e[1]
        if op == "root":
            root = node
            while True:
                up = root.ownerElement if root.nodeType == ATTRIBUTE else root.parentNode
                if up is None:
                    break
                root = up
            return self.steps([root], e[1])
        if op == "rel":
            return self.steps([node], e[1])
        if op == "path":
            base = self.eval(e[1], node, pos, size)
            if not isinstance(base, list):
                raise XPathError("a path must start from nodes")
            return self.steps(base, e[2])
        if op == "filter":
            base = self.eval(e[1], node, pos, size)
            if not isinstance(base, list):
                raise XPathError("only nodes can be filtered")
            for p in e[2]:
                base = self.predicate(base, p)
            return base
        if op == "neg":
            return -_number(self.eval(e[1], node, pos, size))
        if op == "call":
            return _FUNCTIONS[e[1]](self, node, pos, size, e[2])
        if op == "bin":
            o = e[1]
            if o == "or":
                return _boolean(self.eval(e[2], node, pos, size)) or _boolean(self.eval(e[3], node, pos, size))
            if o == "and":
                return _boolean(self.eval(e[2], node, pos, size)) and _boolean(self.eval(e[3], node, pos, size))
            a = self.eval(e[2], node, pos, size)
            b = self.eval(e[3], node, pos, size)
            if o == "|":
                if not isinstance(a, list) or not isinstance(b, list):
                    raise XPathError("| joins nodes only")
                return self.order(a + b)
            if o in ("=", "!=", "<", "<=", ">", ">="):
                return _compare(o, a, b)
            x, y = _number(a), _number(b)
            if o == "+":
                return x + y
            if o == "-":
                return x - y
            if o == "*":
                return x * y
            if o == "div":
                return x / y if y else (math.nan if x == 0 or x != x else math.copysign(math.inf, x) * math.copysign(1, y))
            if o == "mod":
                return math.fmod(x, y) if y else math.nan
        raise XPathError("cannot evaluate %s" % op)

    def steps(self, nodes, steps):
        for axis, test, preds in steps:
            out = []
            for n in nodes:
                found = [m for m in _axis(axis, n) if self.matches(test, m, axis)]
                # The axis gives its nodes nearest first, which is the
                # order a predicate counts them in.
                for p in preds:
                    found = self.predicate(found, p)
                out.extend(found)
            nodes = self.order(out) if len(nodes) > 1 or axis not in ("child", "attribute", "self") else out
        return nodes

    def predicate(self, nodes, p):
        size = len(nodes)
        out = []
        for i, n in enumerate(nodes):
            position = i + 1
            v = self.eval(p, n, position, size)
            if isinstance(v, float):
                if v == position:
                    out.append(n)
            elif _boolean(v):
                out.append(n)
        return out

    def matches(self, test, n, axis):
        kind = test[0]
        principal = ATTRIBUTE if axis == "attribute" else ELEMENT
        if kind == "type":
            t = test[1]
            if t == "node":
                return True
            if t == "text":
                return n.nodeType in (TEXT, CDATA)
            if t == "comment":
                return n.nodeType == COMMENT
            return n.nodeType == PI and (test[2] is None or n.target == test[2])
        if n.nodeType != principal:
            return False
        if kind == "any":
            return not (principal == ATTRIBUTE and n.nodeName.startswith("xmlns"))
        name = test[1]
        if ":" in name:
            prefix, local = name.split(":", 1)
            uri = self.namespaces.get(prefix)
            if uri is None:
                # A prefix nobody declared: the name as the file wrote it.
                return n.nodeName == name
            return (n.namespaceURI or "") == uri and (local == "*" or (n.localName or n.nodeName) == local)
        if self.strict:
            return (n.localName or n.nodeName) == name and not n.namespaceURI or \
                (principal == ATTRIBUTE and n.nodeName == name)
        return n.nodeName == name


def _axis(axis, n):
    if axis == "child":
        return list(n.childNodes) if n.nodeType != ATTRIBUTE else []
    if axis == "attribute":
        if n.nodeType != ELEMENT:
            return []
        a = n.attributes
        return [a.item(i) for i in range(a.length)]
    if axis == "self":
        return [n]
    if axis == "parent":
        p = n.ownerElement if n.nodeType == ATTRIBUTE else n.parentNode
        return [p] if p is not None else []
    if axis in ("ancestor", "ancestor-or-self"):
        out = [n] if axis == "ancestor-or-self" else []
        p = n.ownerElement if n.nodeType == ATTRIBUTE else n.parentNode
        while p is not None:
            out.append(p)
            p = p.parentNode
        return out
    if axis in ("descendant", "descendant-or-self"):
        out = [n] if axis == "descendant-or-self" else []

        def walk(m):
            for c in m.childNodes:
                out.append(c)
                walk(c)
        if n.nodeType != ATTRIBUTE:
            walk(n)
        return out
    if axis == "following-sibling":
        out = []
        s = n.nextSibling if n.nodeType != ATTRIBUTE else None
        while s is not None:
            out.append(s)
            s = s.nextSibling
        return out
    if axis == "preceding-sibling":
        out = []
        s = n.previousSibling if n.nodeType != ATTRIBUTE else None
        while s is not None:
            out.append(s)
            s = s.previousSibling
        return out
    if axis in ("following", "preceding"):
        start = n.ownerElement if n.nodeType == ATTRIBUTE else n
        out = []
        m = start
        while m is not None:
            s = m.nextSibling if axis == "following" else m.previousSibling
            while s is not None:
                if axis == "following":
                    out.append(s)
                    out.extend(_axis("descendant", s))
                else:
                    out.extend(reversed(_axis("descendant", s)))
                    out.append(s)
                s = s.nextSibling if axis == "following" else s.previousSibling
            m = m.parentNode
        return out
    return []


def _string(v):
    if isinstance(v, list):
        return _string_value(v[0]) if v else ""
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        if v != v:
            return "NaN"
        if v in (math.inf, -math.inf):
            return "Infinity" if v > 0 else "-Infinity"
        return str(int(v)) if v == int(v) else repr(v)
    return v


def _number(v):
    if isinstance(v, bool):
        return 1.0 if v else 0.0
    if isinstance(v, float):
        return v
    s = _string(v).strip()
    try:
        if re.fullmatch(r"-?(\d+(\.\d*)?|\.\d+)", s):
            return float(s)
    except ValueError:
        pass
    return math.nan


def _boolean(v):
    if isinstance(v, list):
        return bool(v)
    if isinstance(v, float) and not isinstance(v, bool):
        return v != 0 and v == v
    if isinstance(v, str):
        return v != ""
    return bool(v)


def _compare(op, a, b):
    """XPath 1.0's comparisons, a node-set true if any of its nodes
    makes it so."""
    if isinstance(a, list) or isinstance(b, list):
        if isinstance(a, list) and isinstance(b, list):
            bs = [_string_value(n) for n in b]
            return any(_compare(op, _string_value(x), y) for x in a for y in bs)
        if isinstance(a, list):
            if isinstance(b, bool):
                return _compare(op, _boolean(a), b)
            conv = _number if isinstance(b, float) else (lambda s: s)
            return any(_compare(op, conv(_string_value(x)), b) for x in a)
        if isinstance(a, bool):
            return _compare(op, a, _boolean(b))
        conv = _number if isinstance(a, float) else (lambda s: s)
        return any(_compare(op, a, conv(_string_value(y))) for y in b)
    if op in ("=", "!="):
        if isinstance(a, bool) or isinstance(b, bool):
            r = _boolean(a) == _boolean(b)
        elif isinstance(a, float) or isinstance(b, float):
            r = _number(a) == _number(b)
        else:
            r = _string(a) == _string(b)
        return r if op == "=" else not r
    x, y = _number(a), _number(b)
    return {"<": x < y, "<=": x <= y, ">": x > y, ">=": x >= y}[op]


def _f_nodes(xp, node, pos, size, args, i=0):
    if len(args) <= i:
        return [node]
    v = xp.eval(args[i], node, pos, size)
    if not isinstance(v, list):
        raise XPathError("the function wants nodes")
    return v


def _arg(xp, node, pos, size, args, i):
    return xp.eval(args[i], node, pos, size) if i < len(args) else [node]


def _local_name(n):
    if n.nodeType in (ELEMENT, ATTRIBUTE):
        return n.localName or n.nodeName.split(":")[-1]
    return n.target if n.nodeType == PI else ""


def _f_substring(xp, node, pos, size, args):
    s = _string(_arg(xp, node, pos, size, args, 0))
    start = _number(_arg(xp, node, pos, size, args, 1))
    if len(args) > 2:
        end = start + _number(_arg(xp, node, pos, size, args, 2))
    else:
        end = math.inf
    if start != start or end != end:
        return ""
    lo = _round(start)
    hi = _round(end) if end != math.inf else math.inf
    return "".join(ch for i, ch in enumerate(s, 1) if lo <= i < hi)


def _round(x):
    return math.floor(x + 0.5) if x == x and x not in (math.inf, -math.inf) else x


def _f_translate(xp, node, pos, size, args):
    s = _string(_arg(xp, node, pos, size, args, 0))
    a = _string(_arg(xp, node, pos, size, args, 1))
    b = _string(_arg(xp, node, pos, size, args, 2))
    table = {}
    for i, ch in enumerate(a):
        if ch not in table:
            table[ch] = b[i] if i < len(b) else None
    return "".join(table.get(ch, ch) or "" for ch in s)


_FUNCTIONS = {
    "last": lambda xp, n, p, s, a: float(s),
    "position": lambda xp, n, p, s, a: float(p),
    "count": lambda xp, n, p, s, a: float(len(_f_nodes(xp, n, p, s, a))),
    "local-name": lambda xp, n, p, s, a: (lambda v: _local_name(v[0]) if v else "")(_f_nodes(xp, n, p, s, a)),
    "name": lambda xp, n, p, s, a: (lambda v: (v[0].nodeName if v[0].nodeType in (ELEMENT, ATTRIBUTE)
                                               else _local_name(v[0])) if v else "")(_f_nodes(xp, n, p, s, a)),
    "namespace-uri": lambda xp, n, p, s, a: (lambda v: (getattr(v[0], "namespaceURI", None) or "")
                                             if v else "")(_f_nodes(xp, n, p, s, a)),
    "string": lambda xp, n, p, s, a: _string(_arg(xp, n, p, s, a, 0)),
    "concat": lambda xp, n, p, s, a: "".join(_string(xp.eval(x, n, p, s)) for x in a),
    "starts-with": lambda xp, n, p, s, a: _string(_arg(xp, n, p, s, a, 0)).startswith(_string(_arg(xp, n, p, s, a, 1))),
    "ends-with": lambda xp, n, p, s, a: _string(_arg(xp, n, p, s, a, 0)).endswith(_string(_arg(xp, n, p, s, a, 1))),
    "contains": lambda xp, n, p, s, a: _string(_arg(xp, n, p, s, a, 1)) in _string(_arg(xp, n, p, s, a, 0)),
    "substring-before": lambda xp, n, p, s, a: (lambda x, y: x[:x.find(y)] if y in x else "")(
        _string(_arg(xp, n, p, s, a, 0)), _string(_arg(xp, n, p, s, a, 1))),
    "substring-after": lambda xp, n, p, s, a: (lambda x, y: x[x.find(y) + len(y):] if y in x else "")(
        _string(_arg(xp, n, p, s, a, 0)), _string(_arg(xp, n, p, s, a, 1))),
    "substring": _f_substring,
    "string-length": lambda xp, n, p, s, a: float(len(_string(_arg(xp, n, p, s, a, 0)))),
    "normalize-space": lambda xp, n, p, s, a: " ".join(_string(_arg(xp, n, p, s, a, 0)).split()),
    "translate": _f_translate,
    "not": lambda xp, n, p, s, a: not _boolean(_arg(xp, n, p, s, a, 0)),
    "true": lambda xp, n, p, s, a: True,
    "false": lambda xp, n, p, s, a: False,
    "boolean": lambda xp, n, p, s, a: _boolean(_arg(xp, n, p, s, a, 0)),
    "number": lambda xp, n, p, s, a: _number(_arg(xp, n, p, s, a, 0)),
    "sum": lambda xp, n, p, s, a: float(sum(_number(_string_value(m)) for m in _f_nodes(xp, n, p, s, a))),
    "floor": lambda xp, n, p, s, a: float(math.floor(_number(_arg(xp, n, p, s, a, 0)))),
    "ceiling": lambda xp, n, p, s, a: float(math.ceil(_number(_arg(xp, n, p, s, a, 0)))),
    "round": lambda xp, n, p, s, a: float(_round(_number(_arg(xp, n, p, s, a, 0)))),
    "lang": lambda xp, n, p, s, a: False,
}


# ---- XMLHTTP -------------------------------------------------------------

def _fetch(method, url, headers, body, timeout):
    """One request: (status, reason, headers as [(name, value)], body
    bytes).  A status that is an error comes back as one; only a
    request that never got an answer raises."""
    req = urllib.request.Request(url, data=body, method=method.upper())
    for k, v in headers.items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.reason, list(r.headers.items()), r.read()
    except urllib.error.HTTPError as e:
        try:
            data = e.read()
        except OSError:
            data = b""
        return e.code, e.reason or "", list(e.headers.items()) if e.headers else [], data


def _decode(data, headers):
    ctype = next((v for k, v in headers if k.lower() == "content-type"), "")
    m = re.search(r"charset\s*=\s*\"?([\w.:-]+)", ctype, re.I)
    if data.startswith(b"\xef\xbb\xbf"):
        return data[3:].decode("utf-8", "replace")
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16", "replace")
    if m:
        try:
            return data.decode(m.group(1), "replace")
        except LookupError:
            pass
    return data.decode("utf-8", "replace")


class XMLHTTP:
    """MSXML2.XMLHTTP, ServerXMLHTTP and WinHttp.WinHttpRequest: open,
    setRequestHeader, send, and the answer.  send waits for it, even
    when open asked for it to come later: readyState is 4 by the time a
    macro looks."""

    def __init__(self, typename="IXMLHTTPRequest", winhttp=False):
        self._vba_typename = typename
        self._winhttp = winhttp
        self._method = "GET"
        self._url = None
        self._headers = {}
        self._timeout = 60.0
        self._answer = None
        self.readyState = 0
        self.onreadystatechange = None

    def open(self, bstrMethod, bstrUrl, varAsync=MISSING, bstrUser=MISSING, bstrPassword=MISSING):
        self._method = vba.to_str(bstrMethod)
        self._url = vba.to_str(bstrUrl)
        self._headers = {}
        self._answer = None
        self.readyState = 1
        user = "" if bstrUser is MISSING else vba.to_str(bstrUser)
        if user:
            import base64
            pw = "" if bstrPassword is MISSING else vba.to_str(bstrPassword)
            self._headers["Authorization"] = "Basic " + base64.b64encode(("%s:%s" % (user, pw)).encode()).decode()

    def setRequestHeader(self, bstrHeader, bstrValue):
        if self.readyState != 1:
            raise VBAError(-2147483638, "The data necessary to complete this operation is not yet available.")
        self._headers[vba.to_str(bstrHeader)] = vba.to_str(bstrValue)

    def setTimeouts(self, resolveTimeout, connectTimeout, sendTimeout, receiveTimeout):
        ms = max(vba.to_long(connectTimeout), vba.to_long(receiveTimeout))
        self._timeout = ms / 1000.0 if ms > 0 else None

    def SetProxy(self, *args):
        pass

    def SetCredentials(self, *args):
        pass

    def SetAutoLogonPolicy(self, *args):
        pass

    def SetClientCertificate(self, *args):
        pass

    def setOption(self, *args):
        pass

    def getOption(self, *args):
        return EMPTY

    def waitForResponse(self, timeoutInSeconds=MISSING):
        return True

    def abort(self):
        self._answer = None
        self.readyState = 0

    def send(self, varBody=MISSING):
        if self._url is None:
            raise VBAError(-2147483638, "The data necessary to complete this operation is not yet available.")
        body = None
        headers = dict(self._headers)
        if varBody is not MISSING and varBody is not EMPTY and varBody is not NULL:
            if isinstance(varBody, VBArray):
                body = bytes(vba.to_long(b) & 0xFF for b in varBody.data)
            elif isinstance(varBody, DOMDocument):
                body = varBody.xml.encode("utf-8")
                if not any(k.lower() == "content-type" for k in headers):
                    headers["Content-Type"] = "text/xml;charset=UTF-8"
            else:
                body = vba.to_str(varBody).encode("utf-8")
                if not any(k.lower() == "content-type" for k in headers):
                    headers["Content-Type"] = "text/plain;charset=UTF-8"
        try:
            self._answer = _fetch(self._method, self._url, headers, body, self._timeout)
        except ValueError:
            raise VBAError(-2147012890 if self._winhttp else -2147024809,
                           "The URL is invalid" if self._winhttp else "The parameter is incorrect.")
        except (urllib.error.URLError, OSError) as e:
            reason = getattr(e, "reason", e)
            if self._winhttp:
                raise VBAError(-2147012889, "The server name or address could not be resolved (%s)" % reason)
            raise VBAError(-2146697211, "The system cannot locate the resource specified. (%s)" % reason)
        self.readyState = 4

    def _got(self):
        if self._answer is None:
            raise VBAError(-2147483638, "The data necessary to complete this operation is not yet available.")
        return self._answer

    @property
    def status(self):
        return self._got()[0]

    @property
    def statusText(self):
        return self._got()[1]

    @property
    def responseText(self):
        a = self._got()
        return _decode(a[3], a[2])

    @property
    def responseBody(self):
        data = self._got()[3]
        return VBArray([(0, len(data) - 1)], "byte", data=list(data))

    @property
    def responseStream(self):
        return self.responseBody

    @property
    def responseXML(self):
        doc = DOMDocument()
        a = self._got()
        ctype = next((v for k, v in a[2] if k.lower() == "content-type"), "")
        if "xml" in ctype.lower() or a[3].lstrip().startswith(b"<"):
            doc._parse(a[3], self._url)
        return doc

    def getResponseHeader(self, bstrHeader):
        name = vba.to_str(bstrHeader).lower()
        found = [v for k, v in self._got()[2] if k.lower() == name]
        if not found:
            if self._winhttp:
                raise VBAError(-2147012746, "The requested header was not found")
            return ""
        return ", ".join(found)

    def getAllResponseHeaders(self):
        return "".join("%s: %s\r\n" % kv for kv in self._got()[2])


# ---- what New and CreateObject make ------------------------------------

def _dom(version):
    return lambda: DOMDocument(version)


def _http(typename, winhttp=False):
    return lambda: XMLHTTP(typename, winhttp)


_CLASSES = {
    "microsoft.xmldom": _dom(3), "msxml.domdocument": _dom(3), "msxml2.domdocument": _dom(3),
    "msxml2.domdocument.3.0": _dom(3), "msxml2.domdocument.4.0": _dom(4),
    "msxml2.domdocument.5.0": _dom(4), "msxml2.domdocument.6.0": _dom(6),
    "msxml2.freethreadeddomdocument": _dom(3), "msxml2.freethreadeddomdocument.6.0": _dom(6),
    "domdocument": _dom(3), "domdocument26": _dom(3), "domdocument30": _dom(3), "domdocument40": _dom(4),
    "domdocument60": _dom(6), "freethreadeddomdocument": _dom(3), "freethreadeddomdocument60": _dom(6),
    "microsoft.xmlhttp": _http("IXMLHTTPRequest"), "msxml2.xmlhttp": _http("IXMLHTTPRequest"),
    "msxml2.xmlhttp.3.0": _http("IXMLHTTPRequest"), "msxml2.xmlhttp.6.0": _http("IXMLHTTPRequest"),
    "msxml2.serverxmlhttp": _http("IServerXMLHTTPRequest2"),
    "msxml2.serverxmlhttp.6.0": _http("IServerXMLHTTPRequest2"),
    "xmlhttp": _http("XMLHTTP"), "xmlhttp30": _http("XMLHTTP30"), "xmlhttp60": _http("XMLHTTP60"),
    "serverxmlhttp": _http("ServerXMLHTTP"), "serverxmlhttp60": _http("ServerXMLHTTP60"),
    "winhttp.winhttprequest.5.1": _http("IWinHttpRequest", True),
    "winhttprequest": _http("WinHttpRequest", True),
}

vba.LIBRARY_CLASSES.update(_CLASSES)
