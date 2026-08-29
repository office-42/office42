# Notices

office42 is free software under the GPL, version 3 or later. See
[LICENSE](LICENSE). This file says what else is in it, whose names are
used and why.

## No affiliation

office42 is an independent program. It is not affiliated with,
endorsed by, sponsored by or derived from Microsoft, the GNOME
Foundation, The Document Foundation, or anyone else whose work is
named below. No code, artwork, dialog resource, help text or file from
any of those products is in this repository.

## Trademarks

The names below belong to their owners and are used here only to say
what office42 reads, writes or resembles -- which is the one thing a
name cannot be replaced by:

| Name | Owner |
|---|---|
| Microsoft, Excel, Windows, Office Open XML | Microsoft Corporation |
| Gnumeric, GNOME, GTK | The GNOME Foundation |
| LibreOffice, The Document Foundation | The Document Foundation |
| OpenDocument, OASIS | OASIS Open |
| Python | The Python Software Foundation |
| Arial | The Monotype Corporation |
| Times New Roman | The Monotype Corporation |
| Lotus, 1-2-3 | IBM Corporation |
| MATLAB | The MathWorks, Inc. |
| Mathematica | Wolfram Research, Inc. |

"office42", "word42" and "math42" are the names of these programs.
They are not Microsoft Office, Microsoft Word or Mathematica, and are
not meant to be taken for them.

## What is in the repository

- **The code** is written for this project and is GPL-3.0-or-later.
- **The icons** in `data/icons` are drawn for this project, sixteen
  pixels on a side in the sixteen colours a 1993 VGA had, and carry the
  same licence. None of them is traced from, or derived from, another
  program's artwork.
- **The sample books** in `samples/` are written for this project. Their
  numbers are invented.
- **The file formats** office42 reads and writes are documented
  publicly: Office Open XML is ECMA-376 and ISO/IEC 29500, the older
  binary format is Microsoft's published `[MS-XLS]`, OpenDocument is
  ISO/IEC 26300, and `.gnumeric` is Gnumeric's own XML. Reading and
  writing a documented format is what an independent implementation
  does; the specifications themselves are not copied into this
  repository.
- **Font names** such as Arial or Times New Roman are written into
  files and asked of the system's font stack because that is what the
  formats and the documents name. No font file is shipped here.

## What office42 depends on, and under what terms

| Library | Licence |
|---|---|
| GLib, GIO, GObject, GTK 4, Pango, cairo, gdk-pixbuf | LGPL-2.1-or-later |
| Hunspell (optional) | LGPL/GPL/MPL tri-licence |
| poppler-glib (optional) | GPL-2.0-or-later |
| CPython (optional) | Python Software Foundation License |
| SQLite (optional) | public domain |
| libdeflate | MIT |

Being GPL, office42 as a whole is distributed under the GPL; the
libraries keep their own terms.

## If something here is wrong

If you own one of these names and would rather it were used
differently -- or if you believe anything in this repository is not
ours to publish -- open an issue and it will be changed.
