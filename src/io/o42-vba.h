/* o42-vba.h - a Visual Basic project, read from its file and written back
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Excel keeps a book's macros as a compound file of their own [MS-OVBA]:
 * an .xlsm's xl/vbaProject.bin, or the _VBA_PROJECT_CUR storage of an
 * .xls.  Each module's text is there, compressed, after the compiled
 * form Excel ran last; a "dir" stream says where and under what name,
 * and a PROJECT stream what kind of module each is.  This reads the
 * text into the book's modules and builds a project again from them:
 * one with the text only, which Excel compiles when it opens it, as the
 * format provides for.  Nothing here runs anything.
 */

#pragma once

#include "o42-book.h"
#include <gio/gio.h>

G_BEGIN_DECLS

/* Reads the project in `file` -- the whole compound file when `storage`
 * is "" or NULL, or the storage of that name inside it -- into the
 * book's modules, replacing any it had, and marks them unedited.  FALSE
 * when there is no project there or it cannot be read. */
gboolean o42_vba_read (O42Book *book, GBytes *file, const char *storage, GError **error);

/* A vbaProject.bin holding the book's modules.  `original`, the project
 * the book came with (may be NULL), gives its settings, references,
 * password and forms, which are carried over; without it the project
 * is a new one. */
GBytes  *o42_vba_build (O42Book *book, GBytes *original);

/* The streams of the same project, their paths under `prefix` ("" for
 * the top): what an .xls holds under _VBA_PROJECT_CUR.  The names and
 * the contents (GBytes) are appended to the two arrays. */
void     o42_vba_build_streams (O42Book *book, GBytes *original, const char *prefix,
                                GPtrArray *names, GPtrArray *contents);

/* A book that is getting its first module is given what Excel gives
 * every book: a module of its own (ThisWorkbook) and one for each sheet,
 * with the code names that tie them together. */
void     o42_vba_ensure_documents (O42Book *book);

/* A form's designer storage in the project the book came with: its
 * streams ("f", "o", "\003VBFrame", "i05/f" and the rest) by their paths
 * inside it, as GBytes, and the project's code page, which the form's
 * byte strings are in.  NULL when the book has no such storage. */
GHashTable *o42_vba_form_streams (O42Book *book, const char *form, guint *codepage);

/* ---- A module's text ----------------------------------------------------- */

/* The Attribute lines a module's text begins with -- its name, and for
 * a class whether it can be made -- are the program's to keep, as are
 * those under a procedure that give its description and shortcut key;
 * an editor shows the rest.  The caller frees both. */
char    *o42_vba_code_body (const char *code);
/* The whole text again: `body` under the head `code` had (NULL for
 * none), its name made `name`, and its procedures' Attribute lines put
 * back under them; or under a new head for a module of `kind`. */
char    *o42_vba_code_join (O42Book *book, const char *name, O42VbaKind kind,
                            const char *code, const char *body);

/* A line of the text (from 1) as a line of what o42_vba_code_body shows,
 * a hidden one as the line before it; and back. */
int      o42_vba_view_line (const char *code, int line);
int      o42_vba_text_line (const char *code, int view_line);

/* A module from the text of an exported file (.bas, .cls): its name,
 * from its VB_Name or else from the file's, its kind and its text as a
 * module keeps it.  FALSE for a form (.frm), which needs its .frx. */
gboolean o42_vba_import (const char *text, gsize length, const char *filename,
                         char **name, O42VbaKind *kind, char **code);
/* What File > Export File writes for a module: CR LF lines, and for a
 * class the VERSION block Visual Basic reads back. */
char    *o42_vba_export (const char *name, O42VbaKind kind, const char *code, gsize *length);

/* ---- Macros --------------------------------------------------------------- */

/* A macro is a public Sub that takes nothing: what Tools > Macro >
 * Macros lists and a button can run.  `shortcut` is the letter Excel's
 * Macro Options gave it (an upper-case one wants Shift too), or 0. */
typedef struct {
  char *module;
  char *name;
  char  shortcut;
  char *description;     /* "" for none */
} O42VbaMacro;

/* Whether any module has code in it, and not only the Attribute and
 * Option lines every one begins with: whether there is anything to run. */
gboolean   o42_vba_has_code (O42Book *book);

/* The book's macros, module by module; free with g_ptr_array_unref. */
GPtrArray *o42_vba_macros (O42Book *book);

/* Tools > Macro > Macros > Delete: the Sub goes from its module, with
 * the Attribute lines that belong to it.  FALSE if it is not there. */
gboolean   o42_vba_remove_procedure (O42Book *book, const char *module, const char *name);

/* Macro Options: the Attribute lines Excel keeps a macro's shortcut and
 * description in, written afresh ('\0' and "" for none). */
gboolean   o42_vba_set_macro_options (O42Book *book, const char *module, const char *name,
                                      char shortcut, const char *description);

G_END_DECLS
