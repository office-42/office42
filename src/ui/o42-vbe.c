/* o42-vbe.c - Tools > Macro > Visual Basic Editor
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Excel 97's editor for a book's macros, in the shape it had: the
 * project's modules on the left in its four folders, the code of one of
 * them on the right, and the Immediate window under it, where ?expr
 * prints and Debug.Print writes.  F5 runs the Sub the caret is in, F8
 * steps through it a line at a time, F9 sets a breakpoint.  A module's
 * text is the book's, and goes back to it when another module is
 * chosen, when anything runs and when the editor closes.
 */

#include "o42-window-private.h"

#include "o42-book.h"
#include "o42-python.h"
#include "o42-sheet.h"
#include "o42-vba.h"

#include <glib/gi18n.h>
#include <string.h>

typedef struct {
  O42Window  *window;
  GtkWidget  *dialog;
  GtkWidget  *list;          /* GtkListBox: folders and modules */
  GtkWidget  *name;          /* the module's name, editable for a module or a class */
  GtkWidget  *view;          /* the code */
  GtkWidget  *where;         /* "Ln 3, Col 5" */
  GtkWidget  *immediate;     /* the Immediate window's transcript */
  GtkWidget  *entry;         /* ... and its line */
  GtkWidget  *variables;     /* a paused macro's variables */
  GtkWidget  *run, *step, *go, *stop;
  GtkTextTag *keyword_tag, *comment_tag, *string_tag, *current_tag, *breakpoint_tag, *error_tag;
  char       *module;        /* the module on show, or NULL */
  gboolean    filling;
  gboolean    loading;       /* the view is being filled from the book */
  guint       highlight_id;
  GHashTable *breakpoints;   /* "module:line" */
  GPtrArray  *history;
  int         at;

  /* A macro being stepped. */
  gboolean    running, paused, closed;
  int         command;       /* 0 none yet; 1 go on, 2 step, 3 stop */
} Vbe;

static const char *const KEYWORDS[] = {
  "Alias", "And", "As", "Attribute", "Base", "Binary", "Boolean", "ByRef", "Byte", "ByVal", "Call",
  "Case", "Compare", "Const", "Currency", "Date", "Declare", "Dim", "Do", "Double", "Each", "Else",
  "ElseIf", "Empty", "End", "Enum", "Eqv", "Erase", "Error", "Event", "Exit", "Explicit", "False",
  "For", "Friend", "Function", "Get", "Global", "GoSub", "GoTo", "If", "Imp", "Implements", "In",
  "Integer", "Is", "Let", "Lib", "Like", "Long", "LongLong", "LongPtr", "Loop", "Me", "Mod", "Module",
  "New", "Next", "Not", "Nothing", "Null", "Object", "On", "Option", "Optional", "Or", "ParamArray",
  "Preserve", "Private", "Property", "PtrSafe", "Public", "RaiseEvent", "ReDim", "Rem", "Resume",
  "Return", "Select", "Set", "Single", "Static", "Step", "Stop", "String", "Sub", "Text", "Then",
  "To", "True", "Type", "TypeOf", "Until", "Variant", "Wend", "While", "With", "WithEvents", "Xor",
  NULL
};

static GHashTable *keywords;

static gboolean
is_keyword (const char *word, gsize len)
{
  char *folded;
  gboolean found;

  if (keywords == NULL)
    {
      keywords = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      for (int i = 0; KEYWORDS[i] != NULL; i++)
        g_hash_table_add (keywords, g_ascii_strdown (KEYWORDS[i], -1));
    }
  folded = g_ascii_strdown (word, (gssize) len);
  found = g_hash_table_contains (keywords, folded);
  g_free (folded);
  return found;
}

static O42Book *
vbe_book (Vbe *vbe)
{
  return vbe->window->book;
}

static char *
view_text (Vbe *vbe)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->view));
  GtkTextIter a, b;

  gtk_text_buffer_get_bounds (buffer, &a, &b);
  return gtk_text_buffer_get_text (buffer, &a, &b, FALSE);
}

/* ---- Colours --------------------------------------------------------------- */

/* Keywords blue, comments green and text in quotes dark red, as Visual
 * Basic's editor colours them. */
static gboolean
highlight (gpointer data)
{
  Vbe *vbe = data;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->view));
  GtkTextIter a, b;
  char *text;
  const char *p;
  int offset = 0;

  vbe->highlight_id = 0;
  gtk_text_buffer_get_bounds (buffer, &a, &b);
  gtk_text_buffer_remove_tag (buffer, vbe->keyword_tag, &a, &b);
  gtk_text_buffer_remove_tag (buffer, vbe->comment_tag, &a, &b);
  gtk_text_buffer_remove_tag (buffer, vbe->string_tag, &a, &b);
  text = gtk_text_buffer_get_text (buffer, &a, &b, FALSE);

#define TAG(tag, from, to) \
  G_STMT_START { \
    GtkTextIter s_, e_; \
    gtk_text_buffer_get_iter_at_offset (buffer, &s_, from); \
    gtk_text_buffer_get_iter_at_offset (buffer, &e_, to); \
    gtk_text_buffer_apply_tag (buffer, tag, &s_, &e_); \
  } G_STMT_END

  p = text;
  while (*p != '\0')
    {
      gunichar c = g_utf8_get_char (p);

      if (c == '\'')
        {
          int start = offset;
          while (*p != '\0' && *p != '\n')
            {
              p = g_utf8_next_char (p);
              offset++;
            }
          TAG (vbe->comment_tag, start, offset);
          continue;
        }
      if (c == '"')
        {
          int start = offset;
          p++;
          offset++;
          while (*p != '\0' && *p != '"' && *p != '\n')
            {
              p = g_utf8_next_char (p);
              offset++;
            }
          if (*p == '"')
            {
              p++;
              offset++;
            }
          TAG (vbe->string_tag, start, offset);
          continue;
        }
      if (g_unichar_isalpha (c) || c == '_')
        {
          const char *w = p;
          int start = offset;
          gboolean after_dot = w > text && w[-1] == '.';

          while (*p != '\0' && (g_unichar_isalnum (g_utf8_get_char (p)) || *p == '_'))
            {
              p = g_utf8_next_char (p);
              offset++;
            }
          if (!after_dot && is_keyword (w, (gsize) (p - w)))
            {
              if ((gsize) (p - w) == 3 && g_ascii_strncasecmp (w, "Rem", 3) == 0)
                {
                  while (*p != '\0' && *p != '\n')
                    {
                      p = g_utf8_next_char (p);
                      offset++;
                    }
                  TAG (vbe->comment_tag, start, offset);
                }
              else
                TAG (vbe->keyword_tag, start, offset);
            }
          continue;
        }
      p = g_utf8_next_char (p);
      offset++;
    }
#undef TAG
  g_free (text);
  return G_SOURCE_REMOVE;
}

/* The breakpoints of the module on show tinted, `current` (0 for none)
 * highlighted as the line a macro waits on, `error` as the one an error
 * came from. */
static void
mark_lines (Vbe *vbe, int current, int error)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->view));
  GtkTextIter a, b;
  GHashTableIter it;
  gpointer key;
  char *prefix;

  gtk_text_buffer_get_bounds (buffer, &a, &b);
  gtk_text_buffer_remove_tag (buffer, vbe->current_tag, &a, &b);
  gtk_text_buffer_remove_tag (buffer, vbe->breakpoint_tag, &a, &b);
  gtk_text_buffer_remove_tag (buffer, vbe->error_tag, &a, &b);
  if (vbe->module == NULL)
    return;
  prefix = g_strdup_printf ("%s:", vbe->module);
  g_hash_table_iter_init (&it, vbe->breakpoints);
  while (g_hash_table_iter_next (&it, &key, NULL))
    if (g_str_has_prefix (key, prefix) &&
        gtk_text_buffer_get_iter_at_line (buffer, &a, atoi ((char *) key + strlen (prefix)) - 1))
      {
        b = a;
        if (!gtk_text_iter_ends_line (&b))
          gtk_text_iter_forward_to_line_end (&b);
        gtk_text_buffer_apply_tag (buffer, vbe->breakpoint_tag, &a, &b);
      }
  g_free (prefix);
  for (int k = 0; k < 2; k++)
    {
      int line = k == 0 ? current : error;
      if (line > 0 && gtk_text_buffer_get_iter_at_line (buffer, &a, line - 1))
        {
          b = a;
          if (!gtk_text_iter_ends_line (&b))
            gtk_text_iter_forward_to_line_end (&b);
          gtk_text_buffer_apply_tag (buffer, k == 0 ? vbe->current_tag : vbe->error_tag, &a, &b);
          gtk_text_buffer_place_cursor (buffer, &a);
          gtk_text_view_scroll_to_iter (GTK_TEXT_VIEW (vbe->view), &a, 0.2, FALSE, 0, 0);
        }
    }
}

/* ---- The modules ----------------------------------------------------------- */

/* How many lines of a module's text are its head, which the view leaves
 * out: what a line number in the view is off by from one in the text. */
static int
head_lines (const char *code)
{
  char *body = o42_vba_code_body (code);
  int n = 0;

  for (const char *p = code; p < code + (strlen (code) - strlen (body)); p++)
    n += *p == '\n';
  g_free (body);
  return n;
}

/* The view's text back into the book's module, if it has changed. */
static void
commit (Vbe *vbe)
{
  O42Book *book = vbe_book (vbe);
  int at;
  char *body, *old_body, *code;
  const char *new_name;
  O42VbaKind kind;

  if (vbe->module == NULL || vbe->loading)
    return;
  at = o42_book_vba_module_find (book, vbe->module);
  if (at < 0)
    return;
  kind = o42_book_vba_module_kind (book, at);
  body = view_text (vbe);
  old_body = o42_vba_code_body (o42_book_vba_module_code (book, at));
  new_name = gtk_editable_get_text (GTK_EDITABLE (vbe->name));

  /* A module or a class may be renamed; the book's and the sheets'
   * modules have the names their sheets' code names give them. */
  if (kind != O42_VBA_DOCUMENT && *new_name != '\0' && g_ascii_strcasecmp (new_name, vbe->module) != 0)
    {
      char *old_code = g_strdup (o42_book_vba_module_code (book, at));
      if (o42_book_vba_module_find (book, new_name) >= 0)
        {
          o42_window_show_error (vbe->window, _("Another module already has that name."), NULL);
          gtk_editable_set_text (GTK_EDITABLE (vbe->name), vbe->module);
        }
      else
        {
          code = o42_vba_code_join (book, new_name, kind, old_code, body);
          o42_book_remove_vba_module (book, vbe->module);
          o42_book_set_vba_module (book, new_name, kind, code);
          g_free (code);
          g_free (vbe->module);
          vbe->module = g_strdup (new_name);
        }
      g_free (old_code);
    }
  else if (strcmp (body, old_body) != 0)
    {
      code = o42_vba_code_join (book, vbe->module, kind, o42_book_vba_module_code (book, at), body);
      o42_book_set_vba_module (book, vbe->module, kind, code);
      g_free (code);
    }
  g_free (body);
  g_free (old_body);
}

static const char *
folder_name (O42VbaKind kind)
{
  switch (kind)
    {
    case O42_VBA_DOCUMENT: return _("Microsoft Excel Objects");
    case O42_VBA_FORM:     return _("Forms");
    case O42_VBA_CLASS:    return _("Class Modules");
    case O42_VBA_STANDARD:
    default:               return _("Modules");
    }
}

/* What a module is called in the list: a sheet's module with its tab's
 * name beside it, as "Sheet1 (Data)". */
static char *
module_label (O42Book *book, int index)
{
  const char *name = o42_book_vba_module_name (book, index);

  if (o42_book_vba_module_kind (book, index) == O42_VBA_DOCUMENT)
    for (int i = 0; i < o42_book_n_sheets (book); i++)
      if (g_strcmp0 (o42_sheet_codename (o42_book_sheet (book, i)), name) == 0)
        return g_strdup_printf ("%s (%s)", name, o42_sheet_get_name (o42_book_sheet (book, i)));
  return g_strdup (name);
}

static void
fill_list (Vbe *vbe, const char *select)
{
  O42Book *book = vbe_book (vbe);
  static const O42VbaKind ORDER[] = { O42_VBA_DOCUMENT, O42_VBA_FORM, O42_VBA_STANDARD, O42_VBA_CLASS };
  GtkWidget *child;
  GtkListBoxRow *chosen = NULL;

  vbe->filling = TRUE;
  while ((child = gtk_widget_get_first_child (vbe->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (vbe->list), child);
  {
    GtkWidget *project = gtk_label_new (_("VBAProject"));
    gtk_label_set_xalign (GTK_LABEL (project), 0.0);
    gtk_widget_add_css_class (project, "heading");
    gtk_list_box_append (GTK_LIST_BOX (vbe->list), project);
    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (gtk_widget_get_parent (project)), FALSE);
  }
  for (guint k = 0; k < G_N_ELEMENTS (ORDER); k++)
    {
      gboolean any = FALSE;
      for (int i = 0; i < o42_book_n_vba_modules (book); i++)
        {
          const char *name = o42_book_vba_module_name (book, i);
          GtkWidget *label, *row;
          char *text;

          if (o42_book_vba_module_kind (book, i) != ORDER[k])
            continue;
          if (!any)
            {
              GtkWidget *folder = gtk_label_new (folder_name (ORDER[k]));
              gtk_label_set_xalign (GTK_LABEL (folder), 0.0);
              gtk_widget_set_margin_start (folder, 12);
              gtk_widget_add_css_class (folder, "dim-label");
              gtk_list_box_append (GTK_LIST_BOX (vbe->list), folder);
              gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (gtk_widget_get_parent (folder)), FALSE);
              any = TRUE;
            }
          text = module_label (book, i);
          label = gtk_label_new (text);
          g_free (text);
          gtk_label_set_xalign (GTK_LABEL (label), 0.0);
          gtk_widget_set_margin_start (label, 24);
          gtk_widget_set_margin_end (label, 6);
          gtk_list_box_append (GTK_LIST_BOX (vbe->list), label);
          row = gtk_widget_get_parent (label);
          g_object_set_data_full (G_OBJECT (row), "o42-module", g_strdup (name), g_free);
          if (select != NULL && g_ascii_strcasecmp (select, name) == 0)
            chosen = GTK_LIST_BOX_ROW (row);
        }
    }
  vbe->filling = FALSE;
  if (chosen != NULL)
    gtk_list_box_select_row (GTK_LIST_BOX (vbe->list), chosen);
}

/* Shows a module: its text without its head, its name, its breakpoints. */
static void
show_module (Vbe *vbe, const char *name)
{
  O42Book *book = vbe_book (vbe);
  int at = name != NULL ? o42_book_vba_module_find (book, name) : -1;
  char *body;

  g_free (vbe->module);
  vbe->module = at >= 0 ? g_strdup (o42_book_vba_module_name (book, at)) : NULL;
  body = at >= 0 ? o42_vba_code_body (o42_book_vba_module_code (book, at)) : g_strdup ("");
  vbe->loading = TRUE;
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->view)), body, -1);
  gtk_editable_set_text (GTK_EDITABLE (vbe->name), vbe->module != NULL ? vbe->module : "");
  gtk_editable_set_editable (GTK_EDITABLE (vbe->name),
                             at >= 0 && o42_book_vba_module_kind (book, at) != O42_VBA_DOCUMENT);
  gtk_widget_set_sensitive (vbe->view, at >= 0);
  vbe->loading = FALSE;
  g_free (body);
  highlight (vbe);
  mark_lines (vbe, 0, 0);
}

static void
on_row_selected (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  Vbe *vbe = data;
  const char *name = row != NULL ? g_object_get_data (G_OBJECT (row), "o42-module") : NULL;
  (void) list;

  if (vbe->filling || name == NULL || vbe->running)
    return;
  if (vbe->module != NULL && g_ascii_strcasecmp (vbe->module, name) == 0)
    return;
  commit (vbe);
  show_module (vbe, name);
}

/* Selects a module in the list and shows it, and puts the caret on a
 * line of its text (0 for where it is). */
static void
go_to (Vbe *vbe, const char *module, int text_line)
{
  O42Book *book = vbe_book (vbe);
  int at = o42_book_vba_module_find (book, module);

  if (at < 0)
    return;
  if (vbe->module == NULL || g_ascii_strcasecmp (vbe->module, module) != 0)
    {
      commit (vbe);
      fill_list (vbe, module);
      if (vbe->module == NULL || g_ascii_strcasecmp (vbe->module, module) != 0)
        show_module (vbe, module);
    }
  if (text_line > 0)
    {
      int line = text_line - head_lines (o42_book_vba_module_code (book, at));
      mark_lines (vbe, 0, line);
    }
}

/* ---- The Immediate window ---------------------------------------------------- */

static void
immediate_append (Vbe *vbe, const char *text, const char *tag)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->immediate));
  GtkTextIter end;
  GtkTextMark *mark;

  if (text == NULL || *text == '\0')
    return;
  gtk_text_buffer_get_end_iter (buffer, &end);
  if (tag != NULL)
    gtk_text_buffer_insert_with_tags_by_name (buffer, &end, text, -1, tag, NULL);
  else
    gtk_text_buffer_insert (buffer, &end, text, -1);
  gtk_text_buffer_get_end_iter (buffer, &end);
  mark = gtk_text_buffer_create_mark (buffer, NULL, &end, FALSE);
  gtk_text_view_scroll_mark_onscreen (GTK_TEXT_VIEW (vbe->immediate), mark);
  gtk_text_buffer_delete_mark (buffer, mark);
}

/* A run's output in the Immediate window, and an error's place shown in
 * the code: "(in Module1.Main, line 12)". */
static void
show_result (Vbe *vbe, const char *output, gboolean ok)
{
  immediate_append (vbe, output, ok ? NULL : "error");
  if (!ok && output != NULL)
    {
      const char *in = strstr (output, "(in ");
      const char *compile = strstr (output, "Compile error in ");

      if (in != NULL)
        {
          char module[256] = "";
          char proc[256] = "";
          int line = 0;
          if (sscanf (in, "(in %255[^.].%255[^,], line %d)", module, proc, &line) == 3)
            go_to (vbe, module, line);
        }
      else if (compile != NULL)
        {
          char module[256] = "";
          int line = 0;
          if (sscanf (compile, "Compile error in %255[^,], line %d", module, &line) == 2)
            go_to (vbe, module, line);
        }
    }
}

static void
on_entry_activate (GtkEntry *entry, gpointer data)
{
  Vbe *vbe = data;
  O42Window *self = vbe->window;
  const char *line = gtk_editable_get_text (GTK_EDITABLE (entry));
  char *output = NULL, *echo;
  gboolean ok;

  if (*line == '\0' || vbe->running)
    return;
  commit (vbe);
  echo = g_strdup_printf ("%s\n", line);
  immediate_append (vbe, echo, "input");
  g_free (echo);
  g_ptr_array_add (vbe->history, g_strdup (line));
  vbe->at = -1;
  o42_book_set_scripts_trusted (self->book, TRUE);
  ok = o42_vba_immediate (self->book, self->sheet, line, &output);
  show_result (vbe, output, ok);
  g_free (output);
  gtk_editable_set_text (GTK_EDITABLE (entry), "");
  o42_grid_refresh (self->grid);
  o42_window_sync (self);
}

static gboolean
on_entry_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
              GdkModifierType state, gpointer data)
{
  Vbe *vbe = data;
  int n = (int) vbe->history->len;
  (void) controller; (void) keycode; (void) state;

  if (keyval != GDK_KEY_Up && keyval != GDK_KEY_Down)
    return FALSE;
  if (n == 0)
    return TRUE;
  if (keyval == GDK_KEY_Up)
    vbe->at = vbe->at < 0 ? n - 1 : MAX (vbe->at - 1, 0);
  else
    vbe->at = vbe->at < 0 || vbe->at >= n - 1 ? -1 : vbe->at + 1;
  gtk_editable_set_text (GTK_EDITABLE (vbe->entry),
                         vbe->at < 0 ? "" : g_ptr_array_index (vbe->history, vbe->at));
  gtk_editable_set_position (GTK_EDITABLE (vbe->entry), -1);
  return TRUE;
}

/* ---- Running ------------------------------------------------------------------- */

/* The procedure the caret is in: its name, and whether it is a Sub that
 * takes nothing, which is what a macro is.  NULL between procedures. */
static char *
procedure_at_caret (Vbe *vbe, gboolean *macro)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->view));
  GtkTextIter at;
  char *text = view_text (vbe);
  char **lines = g_strsplit (text, "\n", -1);
  int line;
  char *found = NULL;
  GRegex *head = g_regex_new ("^\\s*(?:(?:Public|Private|Friend|Global)\\s+)?(?:Static\\s+)?"
                              "(Sub|Function|Property\\s+(?:Get|Let|Set))\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*\\(\\s*(\\)?)",
                              G_REGEX_CASELESS, 0, NULL);
  GRegex *tail = g_regex_new ("^\\s*End\\s+(Sub|Function|Property)\\b", G_REGEX_CASELESS, 0, NULL);

  *macro = FALSE;
  gtk_text_buffer_get_iter_at_mark (buffer, &at, gtk_text_buffer_get_insert (buffer));
  line = gtk_text_iter_get_line (&at);
  for (int i = MIN (line, (int) g_strv_length (lines) - 1); i >= 0 && found == NULL; i--)
    {
      GMatchInfo *m = NULL;
      if (i < line && g_regex_match (tail, lines[i], 0, NULL))
        break;
      if (g_regex_match (head, lines[i], 0, &m))
        {
          char *kind = g_match_info_fetch (m, 1);
          char *closing = g_match_info_fetch (m, 3);
          found = g_match_info_fetch (m, 2);
          *macro = g_ascii_strcasecmp (kind, "Sub") == 0 && closing != NULL && *closing == ')';
          g_free (kind);
          g_free (closing);
        }
      g_match_info_free (m);
    }
  g_regex_unref (head);
  g_regex_unref (tail);
  g_strfreev (lines);
  g_free (text);
  return found;
}

static void
set_buttons (Vbe *vbe)
{
  gboolean python = o42_python_available ();

  gtk_widget_set_sensitive (vbe->run, python && (!vbe->running || vbe->paused));
  gtk_widget_set_sensitive (vbe->step, python && (!vbe->running || vbe->paused));
  gtk_widget_set_sensitive (vbe->go, python && vbe->paused);
  gtk_widget_set_sensitive (vbe->stop, vbe->running);
  gtk_widget_set_sensitive (vbe->list, !vbe->running);
  gtk_text_view_set_editable (GTK_TEXT_VIEW (vbe->view), !vbe->running);
}

static void
run_at_caret (Vbe *vbe, gboolean stepping)
{
  O42Window *self = vbe->window;
  gboolean macro;
  char *proc, *qualified, *output = NULL;
  gboolean ok;

  if (vbe->running)
    return;
  commit (vbe);
  proc = procedure_at_caret (vbe, &macro);
  if (proc == NULL || !macro || vbe->module == NULL)
    {
      /* Excel asks which macro, as its Macros dialog does. */
      g_free (proc);
      g_action_group_activate_action (G_ACTION_GROUP (self), "macros", NULL);
      return;
    }
  qualified = g_strdup_printf ("%s.%s", vbe->module, proc);
  o42_book_set_scripts_trusted (self->book, TRUE);
  if (stepping || g_hash_table_size (vbe->breakpoints) > 0)
    {
      GPtrArray *stops = g_ptr_array_new ();
      GHashTableIter it;
      gpointer key;
      O42Book *book = vbe_book (vbe);

      /* The breakpoints are lines of the view; the interpreter counts
       * the module's head too. */
      g_hash_table_iter_init (&it, vbe->breakpoints);
      while (g_hash_table_iter_next (&it, &key, NULL))
        {
          char *colon = strrchr (key, ':');
          char *module = g_strndup (key, colon - (char *) key);
          int at = o42_book_vba_module_find (book, module);
          if (at >= 0)
            g_ptr_array_add (stops, g_strdup_printf ("%s:%d", module,
                                                     atoi (colon + 1) + head_lines (o42_book_vba_module_code (book, at))));
          g_free (module);
        }
      g_ptr_array_add (stops, NULL);
      vbe->running = TRUE;
      vbe->closed = FALSE;
      set_buttons (vbe);
      ok = o42_vba_debug (self->book, self->sheet, qualified, (const char *const *) stops->pdata, stepping, &output);
      g_ptr_array_set_free_func (stops, g_free);
      g_ptr_array_unref (stops);
      if (vbe->closed)
        {
          /* The editor went while the macro ran; it waited for this. */
          g_free (output);
          g_free (qualified);
          g_free (proc);
          g_hash_table_unref (vbe->breakpoints);
          g_ptr_array_unref (vbe->history);
          g_free (vbe->module);
          g_free (vbe);
          return;
        }
      vbe->running = FALSE;
      gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->variables)), "", -1);
      mark_lines (vbe, 0, 0);
      set_buttons (vbe);
    }
  else
    ok = o42_vba_run (self->book, self->sheet, qualified, &output);
  show_result (vbe, output, ok);
  o42_grid_refresh (self->grid);
  o42_window_sync (self);
  g_free (output);
  g_free (qualified);
  g_free (proc);
}

int
o42_vbe_debug_pause (O42Window *self, const char *module, int line, const char *variables)
{
  Vbe *vbe = self->vbe;
  int command;

  if (vbe == NULL || !vbe->running)
    return 0;
  vbe->paused = TRUE;
  vbe->command = 0;
  {
    O42Book *book = vbe_book (vbe);
    int at = o42_book_vba_module_find (book, module);
    if (at >= 0)
      {
        if (vbe->module == NULL || g_ascii_strcasecmp (vbe->module, module) != 0)
          {
            fill_list (vbe, module);
            if (vbe->module == NULL || g_ascii_strcasecmp (vbe->module, module) != 0)
              show_module (vbe, module);
          }
        mark_lines (vbe, line - head_lines (o42_book_vba_module_code (book, at)), 0);
      }
  }
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->variables)),
                            variables != NULL ? variables : "", -1);
  set_buttons (vbe);
  o42_grid_refresh (self->grid);
  gtk_window_present (GTK_WINDOW (vbe->dialog));
  while (vbe->command == 0 && !vbe->closed)
    g_main_context_iteration (NULL, TRUE);
  command = vbe->closed ? 3 : vbe->command;
  vbe->paused = FALSE;
  if (!vbe->closed)
    {
      mark_lines (vbe, 0, 0);
      set_buttons (vbe);
    }
  return command == 1 ? 0 : command == 2 ? 1 : 2;
}

static void
on_run (GtkWidget *w, gpointer data)
{
  Vbe *vbe = data;
  (void) w;
  if (vbe->paused)
    vbe->command = 1;
  else
    run_at_caret (vbe, FALSE);
}

static void
on_step (GtkWidget *w, gpointer data)
{
  Vbe *vbe = data;
  (void) w;
  if (vbe->paused)
    vbe->command = 2;
  else
    run_at_caret (vbe, TRUE);
}

static void
on_go (GtkWidget *w, gpointer data)
{
  Vbe *vbe = data;
  (void) w;
  if (vbe->paused)
    vbe->command = 1;
}

static void
on_stop (GtkWidget *w, gpointer data)
{
  Vbe *vbe = data;
  (void) w;
  if (vbe->paused)
    vbe->command = 3;
}

/* F9: a breakpoint on the caret's line, or off again. */
static void
toggle_breakpoint (Vbe *vbe)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->view));
  GtkTextIter at;
  char *key;

  if (vbe->module == NULL)
    return;
  gtk_text_buffer_get_iter_at_mark (buffer, &at, gtk_text_buffer_get_insert (buffer));
  key = g_strdup_printf ("%s:%d", vbe->module, gtk_text_iter_get_line (&at) + 1);
  if (!g_hash_table_remove (vbe->breakpoints, key))
    g_hash_table_add (vbe->breakpoints, g_strdup (key));
  g_free (key);
  mark_lines (vbe, 0, 0);
}

static void
on_compile (GtkWidget *w, gpointer data)
{
  Vbe *vbe = data;
  char *where = NULL, *message;
  int line = 0;
  (void) w;

  commit (vbe);
  message = o42_vba_check (vbe_book (vbe), &where, &line);
  if (message == NULL)
    immediate_append (vbe, _("The project compiles.\n"), "input");
  else
    {
      char *text = g_strdup_printf ("Compile error in %s, line %d:\n%s\n", where != NULL ? where : "?",
                                    line, message);
      immediate_append (vbe, text, "error");
      if (where != NULL)
        go_to (vbe, where, line);
      g_free (text);
    }
  g_free (message);
  g_free (where);
}

/* ---- Modules made, brought in, sent out and taken away ---------------------------- */

static char *
fresh_name (O42Book *book, const char *stem)
{
  for (int k = 1; ; k++)
    {
      char *name = g_strdup_printf ("%s%d", stem, k);
      if (o42_book_vba_module_find (book, name) < 0)
        return name;
      g_free (name);
    }
}

static void
insert_module (Vbe *vbe, O42VbaKind kind)
{
  O42Book *book = vbe_book (vbe);
  char *name = fresh_name (book, kind == O42_VBA_CLASS ? "Class" : "Module");
  char *code;

  commit (vbe);
  /* A book's first module brings the book's and the sheets' own with
   * it, as every book in Excel has them. */
  if (o42_book_n_vba_modules (book) == 0)
    o42_vba_ensure_documents (book);
  code = o42_vba_code_join (book, name, kind, NULL, "Option Explicit\n\n");
  o42_book_set_vba_module (book, name, kind, code);
  fill_list (vbe, name);
  g_free (code);
  g_free (name);
  gtk_widget_grab_focus (vbe->view);
}

static void
on_insert_module (GtkWidget *w, gpointer data)
{
  (void) w;
  insert_module (data, O42_VBA_STANDARD);
}

static void
on_insert_class (GtkWidget *w, gpointer data)
{
  (void) w;
  insert_module (data, O42_VBA_CLASS);
}

static void
on_import_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Vbe *vbe = data;
  GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, NULL);
  char *contents = NULL, *name = NULL, *code = NULL, *path;
  gsize length = 0;
  O42VbaKind kind;
  O42Book *book = vbe_book (vbe);

  if (file == NULL)
    return;
  path = g_file_get_path (file);
  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, NULL))
    o42_window_show_error (vbe->window, _("office42 could not read that file."), NULL);
  else if (!o42_vba_import (contents, length, path, &name, &kind, &code))
    o42_window_show_error (vbe->window, _("A form (.frm) cannot be imported: office42 does not show forms."), NULL);
  else
    {
      if (o42_book_n_vba_modules (book) == 0)
        o42_vba_ensure_documents (book);
      if (kind != O42_VBA_DOCUMENT && o42_book_vba_module_find (book, name) >= 0)
        {
          /* A module of that name is there: the new one is Name1, as
           * Visual Basic names it. */
          char *renamed = fresh_name (book, name);
          char *body = o42_vba_code_body (code);
          char *joined = o42_vba_code_join (book, renamed, kind, code, body);
          g_free (code);
          g_free (body);
          g_free (name);
          code = joined;
          name = renamed;
        }
      o42_book_set_vba_module (book, name, kind, code);
      fill_list (vbe, name);
    }
  g_free (contents);
  g_free (name);
  g_free (code);
  g_free (path);
  g_object_unref (file);
}

static void
on_import (GtkWidget *w, gpointer data)
{
  Vbe *vbe = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GtkFileFilter *filter = gtk_file_filter_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  (void) w;

  commit (vbe);
  gtk_file_filter_set_name (filter, _("Visual Basic files (*.bas, *.cls)"));
  gtk_file_filter_add_pattern (filter, "*.bas");
  gtk_file_filter_add_pattern (filter, "*.cls");
  gtk_file_filter_add_pattern (filter, "*.BAS");
  gtk_file_filter_add_pattern (filter, "*.CLS");
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_title (dialog, _("Import File"));
  gtk_file_dialog_open (dialog, GTK_WINDOW (vbe->dialog), NULL, on_import_done, vbe);
  g_object_unref (filters);
  g_object_unref (filter);
  g_object_unref (dialog);
}

static void
on_export_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Vbe *vbe = data;
  GFile *file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, NULL);
  O42Book *book = vbe_book (vbe);
  int at;

  if (file == NULL)
    return;
  at = vbe->module != NULL ? o42_book_vba_module_find (book, vbe->module) : -1;
  if (at >= 0)
    {
      gsize length = 0;
      char *text = o42_vba_export (o42_book_vba_module_name (book, at), o42_book_vba_module_kind (book, at),
                                   o42_book_vba_module_code (book, at), &length);
      GError *error = NULL;
      if (!g_file_replace_contents (file, text, length, NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, &error))
        o42_window_show_error (vbe->window, _("office42 could not write that file."), error);
      g_clear_error (&error);
      g_free (text);
    }
  g_object_unref (file);
}

static void
on_export (GtkWidget *w, gpointer data)
{
  Vbe *vbe = data;
  GtkFileDialog *dialog;
  O42Book *book = vbe_book (vbe);
  int at;
  char *suggested;
  (void) w;

  commit (vbe);
  at = vbe->module != NULL ? o42_book_vba_module_find (book, vbe->module) : -1;
  if (at < 0)
    return;
  dialog = gtk_file_dialog_new ();
  suggested = g_strdup_printf ("%s.%s", vbe->module,
                               o42_book_vba_module_kind (book, at) == O42_VBA_STANDARD ? "bas" : "cls");
  gtk_file_dialog_set_initial_name (dialog, suggested);
  gtk_file_dialog_set_title (dialog, _("Export File"));
  gtk_file_dialog_save (dialog, GTK_WINDOW (vbe->dialog), NULL, on_export_done, vbe);
  g_free (suggested);
  g_object_unref (dialog);
}

static void
on_remove_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Vbe *vbe = data;
  int button = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), result, NULL);
  char *name;

  if (button != 0 || vbe->module == NULL)
    return;
  name = g_strdup (vbe->module);
  g_free (vbe->module);
  vbe->module = NULL;
  o42_book_remove_vba_module (vbe_book (vbe), name);
  fill_list (vbe, NULL);
  show_module (vbe, NULL);
  g_free (name);
}

static void
on_remove (GtkWidget *w, gpointer data)
{
  Vbe *vbe = data;
  O42Book *book = vbe_book (vbe);
  int at = vbe->module != NULL ? o42_book_vba_module_find (book, vbe->module) : -1;
  GtkAlertDialog *alert;
  const char *buttons[] = { _("_Remove"), _("_Cancel"), NULL };
  char *question;
  (void) w;

  if (at < 0)
    return;
  if (o42_book_vba_module_kind (book, at) == O42_VBA_DOCUMENT)
    {
      o42_window_show_error (vbe->window, _("The book's and the sheets' own modules go with them, "
                                            "and cannot be removed."), NULL);
      return;
    }
  commit (vbe);
  question = g_strdup_printf (_("Remove %s from the book?"), vbe->module);
  alert = gtk_alert_dialog_new ("%s", question);
  gtk_alert_dialog_set_detail (alert, _("Its code goes with it. Export it first to keep a copy."));
  gtk_alert_dialog_set_buttons (alert, buttons);
  gtk_alert_dialog_set_cancel_button (alert, 1);
  gtk_alert_dialog_choose (alert, GTK_WINDOW (vbe->dialog), NULL, on_remove_done, vbe);
  g_object_unref (alert);
  g_free (question);
}

/* ---- The editor's own keys ---------------------------------------------------------- */

static gboolean
on_view_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
             GdkModifierType state, gpointer data)
{
  Vbe *vbe = data;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->view));
  (void) controller; (void) keycode;

  if (keyval == GDK_KEY_Tab && !(state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) &&
      gtk_text_view_get_editable (GTK_TEXT_VIEW (vbe->view)))
    {
      /* Tab indents by four spaces, as Visual Basic's editor does. */
      gtk_text_buffer_insert_at_cursor (buffer, "    ", -1);
      return TRUE;
    }
  if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && !(state & GDK_CONTROL_MASK) &&
      gtk_text_view_get_editable (GTK_TEXT_VIEW (vbe->view)))
    {
      /* A new line starts where the one above did. */
      GtkTextIter at, start;
      char *before, *indent;
      int n = 0;

      gtk_text_buffer_get_iter_at_mark (buffer, &at, gtk_text_buffer_get_insert (buffer));
      start = at;
      gtk_text_iter_set_line_offset (&start, 0);
      before = gtk_text_buffer_get_text (buffer, &start, &at, FALSE);
      while (before[n] == ' ' || before[n] == '\t')
        n++;
      indent = g_strdup_printf ("\n%.*s", n, before);
      gtk_text_buffer_insert_at_cursor (buffer, indent, -1);
      gtk_text_view_scroll_mark_onscreen (GTK_TEXT_VIEW (vbe->view), gtk_text_buffer_get_insert (buffer));
      g_free (indent);
      g_free (before);
      return TRUE;
    }
  return FALSE;
}

static gboolean
on_dialog_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, gpointer data)
{
  Vbe *vbe = data;
  (void) controller; (void) keycode;

  if (keyval == GDK_KEY_F5 && !(state & GDK_SHIFT_MASK))
    {
      on_run (NULL, vbe);
      return TRUE;
    }
  if (keyval == GDK_KEY_F5 && (state & GDK_SHIFT_MASK))
    {
      on_stop (NULL, vbe);
      return TRUE;
    }
  if (keyval == GDK_KEY_F8)
    {
      on_step (NULL, vbe);
      return TRUE;
    }
  if (keyval == GDK_KEY_F9)
    {
      toggle_breakpoint (vbe);
      return TRUE;
    }
  if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_g || keyval == GDK_KEY_G))
    {
      gtk_widget_grab_focus (vbe->entry);
      return TRUE;
    }
  if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_s || keyval == GDK_KEY_S))
    {
      commit (vbe);
      g_action_group_activate_action (G_ACTION_GROUP (vbe->window), "save", NULL);
      return TRUE;
    }
  if (keyval == GDK_KEY_F11 && (state & GDK_ALT_MASK))
    {
      commit (vbe);
      gtk_window_present (GTK_WINDOW (vbe->window));
      return TRUE;
    }
  return FALSE;
}

static void
on_buffer_changed (GtkTextBuffer *buffer, gpointer data)
{
  Vbe *vbe = data;
  (void) buffer;
  if (vbe->highlight_id == 0)
    vbe->highlight_id = g_timeout_add (150, highlight, vbe);
}

static void
on_mark_set (GtkTextBuffer *buffer, GtkTextIter *location, GtkTextMark *mark, gpointer data)
{
  Vbe *vbe = data;
  GtkTextIter at;
  char *text;
  (void) location;

  if (mark != gtk_text_buffer_get_insert (buffer))
    return;
  gtk_text_buffer_get_iter_at_mark (buffer, &at, mark);
  text = g_strdup_printf (_("Ln %d, Col %d"), gtk_text_iter_get_line (&at) + 1,
                          gtk_text_iter_get_line_offset (&at) + 1);
  gtk_label_set_text (GTK_LABEL (vbe->where), text);
  g_free (text);
}

static void
on_close (GtkWidget *w, gpointer data)
{
  Vbe *vbe = data;
  (void) w;
  commit (vbe);
  gtk_window_destroy (GTK_WINDOW (vbe->dialog));
}

static gboolean
on_close_request (GtkWindow *window, gpointer data)
{
  (void) window;
  commit (data);
  return FALSE;
}

static void
on_destroy (GtkWidget *w, gpointer data)
{
  Vbe *vbe = data;

  (void) w;
  vbe->window->vbe = NULL;
  if (vbe->highlight_id != 0)
    g_source_remove (vbe->highlight_id);
  vbe->highlight_id = 0;
  o42_window_sync (vbe->window);
  if (vbe->running)
    {
      /* A stepped macro is still on its line: it is told to stop, and
       * frees the editor's state when it has. */
      vbe->closed = TRUE;
      return;
    }
  g_hash_table_unref (vbe->breakpoints);
  g_ptr_array_unref (vbe->history);
  g_free (vbe->module);
  g_free (vbe);
}

static GtkWidget *
scrolled (GtkWidget *child)
{
  GtkWidget *s = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (s), child);
  gtk_widget_add_css_class (s, "frame");
  return s;
}

static GtkWidget *
text_pane (void)
{
  GtkWidget *view = gtk_text_view_new ();
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (view), 6);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (view), 4);
  return view;
}

/* The editor, opened on `module` (NULL for the first macro module, or
 * the first there is), with the caret at procedure `proc` if given. */
void
o42_window_vbe (O42Window *self, const char *module, const char *proc)
{
  Vbe *vbe;
  GtkWidget *content, *buttons, *columns, *left, *right, *row, *tools, *panes, *lower, *code_panes;
  GtkTextBuffer *buffer;
  GtkEventController *keys;
  O42Book *book = self->book;
  const char *first = NULL;

  if (self->vbe != NULL)
    {
      vbe = self->vbe;
      gtk_window_present (GTK_WINDOW (vbe->dialog));
    }
  else
    {
      vbe = g_new0 (Vbe, 1);
      vbe->window = self;
      vbe->breakpoints = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      vbe->history = g_ptr_array_new_with_free_func (g_free);
      vbe->at = -1;
      self->vbe = vbe;
      /* Translators: the title of Tools > Macro > Visual Basic Editor. */
      vbe->dialog = o42_dialog_frame (self, _("Visual Basic Editor"), FALSE, &content, &buttons);
      gtk_window_set_resizable (GTK_WINDOW (vbe->dialog), TRUE);
      gtk_window_set_default_size (GTK_WINDOW (vbe->dialog), 980, 660);

      tools = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
      {
        struct { const char *label; const char *tip; GCallback cb; } T[] = {
          { N_("Insert _Module"), N_("A new module for macros and functions"), G_CALLBACK (on_insert_module) },
          { N_("Insert _Class"), N_("A new class module"), G_CALLBACK (on_insert_class) },
          { N_("_Import File..."), N_("Bring in a module exported as .bas or .cls"), G_CALLBACK (on_import) },
          { N_("_Export File..."), N_("Write the module on show to a .bas or .cls file"), G_CALLBACK (on_export) },
          { N_("Re_move"), N_("Take the module on show out of the book"), G_CALLBACK (on_remove) },
          { N_("C_ompile"), N_("Debug > Compile VBAProject: check every module"), G_CALLBACK (on_compile) },
        };
        for (guint i = 0; i < G_N_ELEMENTS (T); i++)
          {
            GtkWidget *b = gtk_button_new_with_mnemonic (_(T[i].label));
            gtk_widget_set_tooltip_text (b, _(T[i].tip));
            g_signal_connect (b, "clicked", T[i].cb, vbe);
            gtk_box_append (GTK_BOX (tools), b);
          }
      }
      gtk_box_append (GTK_BOX (content), tools);

      columns = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
      gtk_widget_set_vexpand (columns, TRUE);
      left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      vbe->list = gtk_list_box_new ();
      gtk_list_box_set_selection_mode (GTK_LIST_BOX (vbe->list), GTK_SELECTION_SINGLE);
      g_signal_connect (vbe->list, "row-selected", G_CALLBACK (on_row_selected), vbe);
      {
        GtkWidget *s = scrolled (vbe->list);
        gtk_widget_set_vexpand (s, TRUE);
        gtk_box_append (GTK_BOX (left), s);
      }
      gtk_widget_set_size_request (left, 200, -1);
      gtk_paned_set_start_child (GTK_PANED (columns), left);
      gtk_paned_set_resize_start_child (GTK_PANED (columns), FALSE);

      right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_box_append (GTK_BOX (row), gtk_label_new (_("Name:")));
      vbe->name = gtk_entry_new ();
      gtk_widget_set_hexpand (vbe->name, TRUE);
      gtk_box_append (GTK_BOX (row), vbe->name);
      vbe->where = gtk_label_new ("");
      gtk_widget_add_css_class (vbe->where, "dim-label");
      gtk_box_append (GTK_BOX (row), vbe->where);
      gtk_box_append (GTK_BOX (right), row);

      vbe->view = text_pane ();
      gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (vbe->view), GTK_WRAP_NONE);
      buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->view));
      vbe->keyword_tag = gtk_text_buffer_create_tag (buffer, "keyword", "foreground", "#00007F", NULL);
      vbe->comment_tag = gtk_text_buffer_create_tag (buffer, "comment", "foreground", "#007F00", NULL);
      vbe->string_tag = gtk_text_buffer_create_tag (buffer, "string", "foreground", "#7F0000", NULL);
      vbe->breakpoint_tag = gtk_text_buffer_create_tag (buffer, "breakpoint", "paragraph-background", "#E8B4B4", NULL);
      vbe->error_tag = gtk_text_buffer_create_tag (buffer, "error", "paragraph-background", "#FFD6D6", NULL);
      vbe->current_tag = gtk_text_buffer_create_tag (buffer, "current", "paragraph-background", "#FFF3A0", NULL);
      g_signal_connect (buffer, "changed", G_CALLBACK (on_buffer_changed), vbe);
      g_signal_connect (buffer, "mark-set", G_CALLBACK (on_mark_set), vbe);
      keys = gtk_event_controller_key_new ();
      g_signal_connect (keys, "key-pressed", G_CALLBACK (on_view_key), vbe);
      gtk_widget_add_controller (vbe->view, keys);

      code_panes = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
      vbe->variables = text_pane ();
      gtk_text_view_set_editable (GTK_TEXT_VIEW (vbe->variables), FALSE);
      {
        GtkWidget *code = scrolled (vbe->view);
        GtkWidget *vars = scrolled (vbe->variables);
        gtk_widget_set_hexpand (code, TRUE);
        gtk_widget_set_size_request (vars, 180, -1);
        gtk_paned_set_start_child (GTK_PANED (code_panes), code);
        gtk_paned_set_end_child (GTK_PANED (code_panes), vars);
        gtk_paned_set_resize_end_child (GTK_PANED (code_panes), FALSE);
        gtk_paned_set_position (GTK_PANED (code_panes), 580);
      }

      lower = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      {
        GtkWidget *heading = gtk_label_new (_("Immediate"));
        GtkTextBuffer *ib;
        GtkEventController *entry_keys = gtk_event_controller_key_new ();

        gtk_label_set_xalign (GTK_LABEL (heading), 0.0);
        gtk_widget_add_css_class (heading, "dim-label");
        gtk_box_append (GTK_BOX (lower), heading);
        vbe->immediate = text_pane ();
        gtk_text_view_set_editable (GTK_TEXT_VIEW (vbe->immediate), FALSE);
        gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (vbe->immediate), GTK_WRAP_WORD_CHAR);
        ib = gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->immediate));
        gtk_text_buffer_create_tag (ib, "input", "foreground", "#1a4d8a", NULL);
        gtk_text_buffer_create_tag (ib, "error", "foreground", "#a01010", NULL);
        {
          GtkWidget *s = scrolled (vbe->immediate);
          gtk_widget_set_vexpand (s, TRUE);
          gtk_box_append (GTK_BOX (lower), s);
        }
        vbe->entry = gtk_entry_new ();
        gtk_entry_set_placeholder_text (GTK_ENTRY (vbe->entry), "?Range(\"A1\").Value");
        g_signal_connect (vbe->entry, "activate", G_CALLBACK (on_entry_activate), vbe);
        g_signal_connect (entry_keys, "key-pressed", G_CALLBACK (on_entry_key), vbe);
        gtk_widget_add_controller (vbe->entry, entry_keys);
        gtk_box_append (GTK_BOX (lower), vbe->entry);
      }
      panes = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
      gtk_paned_set_start_child (GTK_PANED (panes), code_panes);
      gtk_paned_set_end_child (GTK_PANED (panes), lower);
      gtk_paned_set_resize_end_child (GTK_PANED (panes), FALSE);
      gtk_paned_set_position (GTK_PANED (panes), 400);
      gtk_widget_set_vexpand (panes, TRUE);
      gtk_box_append (GTK_BOX (right), panes);
      {
        GtkWidget *hint = gtk_label_new (_("F5 runs the Sub the caret is in, F8 steps through it, F9 sets a "
                                           "breakpoint. Type ?expression in the Immediate window to see its "
                                           "value; Debug.Print writes there."));
        gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
        gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
        gtk_widget_add_css_class (hint, "dim-label");
        gtk_box_append (GTK_BOX (right), hint);
      }
      gtk_paned_set_end_child (GTK_PANED (columns), right);
      gtk_paned_set_position (GTK_PANED (columns), 220);
      gtk_box_append (GTK_BOX (content), columns);

      vbe->run = o42_dialog_button (buttons, _("_Run"), G_CALLBACK (on_run), vbe);
      vbe->step = o42_dialog_button (buttons, _("_Step Into"), G_CALLBACK (on_step), vbe);
      vbe->go = o42_dialog_button (buttons, _("C_ontinue"), G_CALLBACK (on_go), vbe);
      vbe->stop = o42_dialog_button (buttons, _("Rese_t"), G_CALLBACK (on_stop), vbe);
      o42_dialog_button (buttons, _("Close"), G_CALLBACK (on_close), vbe);

      keys = gtk_event_controller_key_new ();
      gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
      g_signal_connect (keys, "key-pressed", G_CALLBACK (on_dialog_key), vbe);
      gtk_widget_add_controller (vbe->dialog, keys);
      g_signal_connect (vbe->dialog, "close-request", G_CALLBACK (on_close_request), vbe);
      g_signal_connect (vbe->dialog, "destroy", G_CALLBACK (on_destroy), vbe);
      set_buttons (vbe);
      gtk_window_present (GTK_WINDOW (vbe->dialog));
      if (!o42_python_available ())
        immediate_append (vbe, _("This build of office42 has no Python in it, and so cannot run "
                                 "Visual Basic: the modules can be read and edited, and are saved.\n"),
                          "error");
    }

  if (module == NULL)
    {
      for (int i = 0; i < o42_book_n_vba_modules (book) && first == NULL; i++)
        if (o42_book_vba_module_kind (book, i) == O42_VBA_STANDARD)
          first = o42_book_vba_module_name (book, i);
      if (first == NULL && o42_book_n_vba_modules (book) > 0)
        first = o42_book_vba_module_name (book, 0);
      module = vbe->module != NULL ? vbe->module : first;
    }
  {
    char *keep = g_strdup (module);
    commit (vbe);
    fill_list (vbe, keep);
    if (keep == NULL || vbe->module == NULL || g_ascii_strcasecmp (vbe->module, keep) != 0)
      show_module (vbe, keep);
    g_free (keep);
  }

  if (proc != NULL && vbe->module != NULL)
    {
      /* The caret on the procedure's first line. */
      char *text = view_text (vbe);
      char **lines = g_strsplit (text, "\n", -1);
      char *pattern = g_strdup_printf ("^\\s*(?:(?:Public|Private|Friend|Global)\\s+)?(?:Static\\s+)?"
                                       "(?:Sub|Function|Property\\s+(?:Get|Let|Set))\\s+%s\\b", proc);
      GRegex *head = g_regex_new (pattern, G_REGEX_CASELESS, 0, NULL);

      for (int i = 0; head != NULL && lines[i] != NULL; i++)
        if (g_regex_match (head, lines[i], 0, NULL))
          {
            GtkTextIter at;
            gtk_text_buffer_get_iter_at_line (gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->view)), &at, i);
            gtk_text_buffer_place_cursor (gtk_text_view_get_buffer (GTK_TEXT_VIEW (vbe->view)), &at);
            gtk_text_view_scroll_to_iter (GTK_TEXT_VIEW (vbe->view), &at, 0.1, TRUE, 0, 0);
            break;
          }
      if (head != NULL)
        g_regex_unref (head);
      g_free (pattern);
      g_strfreev (lines);
      g_free (text);
    }
  gtk_widget_grab_focus (vbe->view);
}

void
action_vbe (GSimpleAction *a, GVariant *p, gpointer data)
{
  (void) a; (void) p;
  o42_window_vbe (data, NULL, NULL);
}

/* The editor's text into the book, before the book is saved or a macro
 * runs from elsewhere. */
void
o42_vbe_commit (O42Window *self)
{
  if (self->vbe != NULL)
    commit (self->vbe);
}

/* What a macro run from the window printed, in the Immediate window if
 * the editor is open. */
void
o42_vbe_show_output (O42Window *self, const char *output, gboolean ok)
{
  if (self->vbe != NULL)
    show_result (self->vbe, output, ok);
}
