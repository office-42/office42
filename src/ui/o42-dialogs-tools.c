/* o42-dialogs-tools.c - see o42-window-private.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-window-private.h"

#include "o42-analysis.h"
#include "o42-book.h"
#include "o42-eval.h"
#include "o42-formula.h"
#include "o42-python.h"
#include "o42-spell.h"
#include "o42-types.h"

#include <glib/gi18n.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The names these dialogs have always called them by. */
#define colour_from_rgba o42_colour_from_rgba
#define dialog_button o42_dialog_button
#define dialog_frame o42_dialog_frame
#define drop_down_of o42_drop_down_of
#define labelled o42_labelled
#define on_dialog_close_clicked o42_dialog_close_clicked
#define on_dialog_destroy_refocus o42_dialog_destroy_refocus
#define window_show_sheet o42_window_show_sheet
#define scripts_bar_hide o42_scripts_bar_hide
#define window_run_script o42_window_run_script
#define page_grid o42_page_grid
#define rgba_from_colour o42_rgba_from_colour
#define show_error o42_window_show_error
#define window_sync o42_window_sync
#define window_tell_book o42_window_tell_book
#define wizard_bind_item o42_wizard_bind_item
#define wizard_setup_item o42_wizard_setup_item

/* ---- Tools > Goal Seek ------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *target, *value, *variable, *status;
} GoalPrompt;

static void
on_goal_ok (GtkWidget *w, gpointer data)
{
  GoalPrompt *prompt = data;
  O42Window *self = prompt->window;
  int trow, tcol, vrow, vcol;
  double goal, found = 0;
  const char *tt = gtk_editable_get_text (GTK_EDITABLE (prompt->target));
  const char *vt = gtk_editable_get_text (GTK_EDITABLE (prompt->variable));
  char *end = NULL;

  (void) w;

  goal = g_ascii_strtod (gtk_editable_get_text (GTK_EDITABLE (prompt->value)), &end);
  if (!o42_ref_parse (tt, &trow, &tcol, NULL) || !o42_ref_parse (vt, &vrow, &vcol, NULL) ||
      end == gtk_editable_get_text (GTK_EDITABLE (prompt->value)))
    {
      gtk_label_set_text (GTK_LABEL (prompt->status), _("Give a formula cell, a number, and a cell to change."));
      return;
    }

  if (o42_sheet_goal_seek (self->sheet, trow, tcol, goal, vrow, vcol, &found))
    {
      char *v = o42_sheet_get_display (self->sheet, vrow, vcol);
      char *msg = g_strdup_printf ("Found a solution: %s = %s.", vt, v);
      gtk_label_set_text (GTK_LABEL (prompt->status), msg);
      g_free (msg);
      g_free (v);
    }
  else
    gtk_label_set_text (GTK_LABEL (prompt->status), _("Goal Seek may not have found a solution."));

  o42_grid_refresh (self->grid);
  window_sync (self);
}

void
action_goal_seek (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GoalPrompt *prompt = g_new0 (GoalPrompt, 1);
  GtkWidget *content, *buttons, *grid, *ok;
  int row, col;
  char *name;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Goal Seek"), FALSE, &content, &buttons);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->target = labelled (grid, 0, _("Set cell:"), gtk_entry_new ());
  prompt->value = labelled (grid, 1, _("To value:"), gtk_entry_new ());
  prompt->variable = labelled (grid, 2, _("By changing cell:"), gtk_entry_new ());
  gtk_box_append (GTK_BOX (content), grid);

  o42_grid_get_active (self->grid, &row, &col);
  name = o42_ref_name (row, col);
  gtk_editable_set_text (GTK_EDITABLE (prompt->target), name);
  g_free (name);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->variable), TRUE);

  prompt->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (prompt->status), 0.0);
  gtk_label_set_wrap (GTK_LABEL (prompt->status), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (prompt->status), 40);
  gtk_box_append (GTK_BOX (content), prompt->status);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_goal_ok), prompt);
  dialog_button (buttons, _("Close"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->value);
}


/* ---- Tools > Python Console ------------------------------------------- */

/* A transcript and a line to type into, as the interpreter's own
 * console; the namespace lives on between lines and between openings
 * of the window.  Up and Down walk the history. */
typedef struct {
  O42Window  *window;
  GtkWidget  *dialog;
  GtkWidget  *view;
  GtkWidget  *entry;
  GPtrArray  *history;
  int         at;             /* history index while walking, or -1 */
} PyConsole;

static void
console_append (PyConsole *console, const char *text, const char *tag)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (console->view));
  GtkTextIter end;
  GtkTextMark *mark;

  gtk_text_buffer_get_end_iter (buffer, &end);
  if (tag != NULL)
    gtk_text_buffer_insert_with_tags_by_name (buffer, &end, text, -1, tag, NULL);
  else
    gtk_text_buffer_insert (buffer, &end, text, -1);
  gtk_text_buffer_get_end_iter (buffer, &end);
  mark = gtk_text_buffer_create_mark (buffer, NULL, &end, FALSE);
  gtk_text_view_scroll_mark_onscreen (GTK_TEXT_VIEW (console->view), mark);
  gtk_text_buffer_delete_mark (buffer, mark);
}

/* Runs code from the console; the window repaints through the book's
 * change notice, which the runner sends when cells changed. */
static void
console_run (PyConsole *console, const char *code, const char *filename)
{
  O42Window *self = console->window;
  char *output = NULL;
  gboolean ok = o42_python_run (self->book, self->sheet, code, filename, &output);

  if (output != NULL && *output != '\0')
    console_append (console, output, ok ? NULL : "error");
  g_free (output);
  o42_grid_refresh (self->grid);
  window_sync (self);
}

static void
on_console_activate (GtkEntry *entry, gpointer data)
{
  PyConsole *console = data;
  const char *line = gtk_editable_get_text (GTK_EDITABLE (entry));
  char *prompt;

  if (*line == '\0')
    return;
  prompt = g_strdup_printf (">>> %s\n", line);
  console_append (console, prompt, "input");
  g_free (prompt);
  g_ptr_array_add (console->history, g_strdup (line));
  console->at = -1;
  console_run (console, line, "<console>");
  gtk_editable_set_text (GTK_EDITABLE (entry), "");
}

static gboolean
on_console_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
                GdkModifierType state, gpointer data)
{
  PyConsole *console = data;
  int n = (int) console->history->len;
  (void) controller; (void) keycode; (void) state;

  if (keyval != GDK_KEY_Up && keyval != GDK_KEY_Down)
    return FALSE;
  if (n == 0)
    return TRUE;
  if (keyval == GDK_KEY_Up)
    console->at = console->at < 0 ? n - 1 : MAX (console->at - 1, 0);
  else
    console->at = console->at < 0 || console->at >= n - 1 ? -1 : console->at + 1;
  gtk_editable_set_text (GTK_EDITABLE (console->entry),
                         console->at < 0 ? "" : g_ptr_array_index (console->history, console->at));
  gtk_editable_set_position (GTK_EDITABLE (console->entry), -1);
  return TRUE;
}

static void
on_console_destroy (GtkWidget *w, gpointer data)
{
  PyConsole *console = data;
  (void) w;
  console->window->python_console = NULL;
  g_ptr_array_unref (console->history);
  g_free (console);
}

static void
on_console_reset (GtkWidget *w, gpointer data)
{
  PyConsole *console = data;
  (void) w;
  o42_python_reset ();
  console_append (console, "-- variables and script functions forgotten --\n", "error");
}

void
action_python_console (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  PyConsole *console;
  GtkWidget *content, *buttons, *scroller, *reset;
  GtkTextBuffer *buffer;
  GtkEventController *keys;
  char *banner;

  (void) a; (void) p;
  if (self->python_console != NULL)
    {
      gtk_window_present (GTK_WINDOW (((PyConsole *) self->python_console)->dialog));
      return;
    }

  console = g_new0 (PyConsole, 1);
  console->window = self;
  console->history = g_ptr_array_new_with_free_func (g_free);
  console->at = -1;
  console->dialog = dialog_frame (self, _("Python Console"), FALSE, &content, &buttons);
  gtk_window_set_resizable (GTK_WINDOW (console->dialog), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (console->dialog), 640, 400);
  self->python_console = console;

  console->view = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (console->view), FALSE);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (console->view), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (console->view), TRUE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (console->view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (console->view), 6);
  buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (console->view));
  gtk_text_buffer_create_tag (buffer, "input", "foreground", "#1a4d8a", NULL);
  gtk_text_buffer_create_tag (buffer, "error", "foreground", "#a01010", NULL);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), console->view);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_widget_set_hexpand (scroller, TRUE);
  gtk_box_append (GTK_BOX (content), scroller);

  console->entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (console->entry), _("sheet[\"A1\"].value = 42"));
  g_signal_connect (console->entry, "activate", G_CALLBACK (on_console_activate), console);
  keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_console_key), console);
  gtk_widget_add_controller (console->entry, keys);
  gtk_box_append (GTK_BOX (content), console->entry);

  reset = dialog_button (buttons, _("_Reset"), G_CALLBACK (on_console_reset), console);
  gtk_widget_set_tooltip_text (reset, _("Forget the console's variables and the functions scripts defined"));
  dialog_button (buttons, _("Close"), G_CALLBACK (on_dialog_close_clicked), console->dialog);
  g_signal_connect (console->dialog, "destroy", G_CALLBACK (on_console_destroy), console);

  banner = g_strdup_printf ("Python %s in Office42 Spreadsheet %s\n"
                            "`book` and `sheet` are bound; `import office42` for the rest; help(office42) tells.\n",
                            o42_python_version () != NULL ? o42_python_version () : "?", O42_VERSION);
  console_append (console, banner, NULL);
  g_free (banner);

  gtk_window_present (GTK_WINDOW (console->dialog));
  gtk_widget_grab_focus (console->entry);
}

static void
on_python_script_response (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Window *self = data;
  GError *error = NULL;
  GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      char *output = NULL;
      gboolean ok = o42_python_run_file (self->book, self->sheet, file, &output);

      o42_grid_refresh (self->grid);
      window_sync (self);
      if (self->python_console != NULL)
        {
          PyConsole *console = self->python_console;
          char *name = g_file_get_basename (file);
          char *line = g_strdup_printf (">>> # %s\n", name);
          console_append (console, line, "input");
          if (output != NULL && *output != '\0')
            console_append (console, output, ok ? NULL : "error");
          g_free (line);
          g_free (name);
        }
      else if (!ok || (output != NULL && *output != '\0'))
        {
          GtkAlertDialog *alert = gtk_alert_dialog_new ("%s", ok ? "The script said:" : "The script failed.");
          gtk_alert_dialog_set_detail (alert, output != NULL ? output : "");
          gtk_alert_dialog_show (alert, GTK_WINDOW (self));
          g_object_unref (alert);
        }
      g_free (output);
      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, "office42 could not open that file.", error);
  g_clear_error (&error);
}

void
action_python_run (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GtkFileFilter *scripts = gtk_file_filter_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  (void) a; (void) p;
  gtk_file_filter_set_name (scripts, _("Python scripts (*.py)"));
  gtk_file_filter_add_pattern (scripts, "*.py");
  g_list_store_append (filters, scripts);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_title (dialog, _("Run Python Script"));
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_python_script_response, self);
  g_object_unref (filters);
  g_object_unref (scripts);
  g_object_unref (dialog);
}

/* ---- Tools > Scripts in this Book -------------------------------------- */

void
o42_scripts_bar_hide (O42Window *self)
{
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->scripts_bar), FALSE);
}

/* Runs code the book holds, telling the console if there is one and
 * a message otherwise; TRUE if it got through. */
gboolean
o42_window_run_script (O42Window *self, const char *name, const char *code)
{
  char *output = NULL;
  gboolean ok = o42_python_run (self->book, self->sheet, code, name, &output);

  o42_grid_refresh (self->grid);
  window_sync (self);
  if (self->python_console != NULL)
    {
      PyConsole *console = self->python_console;
      char *line = g_strdup_printf (">>> # %s\n", name);
      console_append (console, line, "input");
      if (output != NULL && *output != '\0')
        console_append (console, output, ok ? NULL : "error");
      g_free (line);
    }
  else if (!ok || (output != NULL && *output != '\0'))
    {
      GtkAlertDialog *alert = gtk_alert_dialog_new ("%s", ok ? "The script said:" : "The script failed.");
      gtk_alert_dialog_set_detail (alert, output != NULL ? output : "");
      gtk_alert_dialog_show (alert, GTK_WINDOW (self));
      g_object_unref (alert);
    }
  g_free (output);
  return ok;
}

void
action_scripts_run_all (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  int n = o42_book_n_scripts (self->book);
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);

  (void) a; (void) p;
  /* This is the user saying the book's Python may run: the scripts
   * now, and the =PY() cells, which are worked out again. */
  o42_book_set_scripts_trusted (self->book, TRUE);
  /* The names first: a script may add or remove scripts. */
  for (int i = 0; i < n; i++)
    g_ptr_array_add (names, g_strdup (o42_book_script_name (self->book, i)));
  for (guint i = 0; i < names->len; i++)
    {
      const char *code = o42_book_script_code (self->book, g_ptr_array_index (names, i));
      if (code != NULL && !window_run_script (self, g_ptr_array_index (names, i), code))
        break;
    }
  g_ptr_array_unref (names);
  for (int i = 0; i < o42_book_n_sheets (self->book); i++)
    o42_sheet_touch_volatiles (o42_book_sheet (self->book, i));
  o42_window_tell_book (self, "cells");
  scripts_bar_hide (self);
}

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *list;      /* GtkListBox of names */
  GtkWidget *name;      /* GtkEntry */
  GtkWidget *view;      /* GtkTextView with the code */
  gboolean   filling;
} ScriptsPrompt;

static char *
scripts_view_text (ScriptsPrompt *prompt)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->view));
  GtkTextIter a, b;
  gtk_text_buffer_get_bounds (buffer, &a, &b);
  return gtk_text_buffer_get_text (buffer, &a, &b, FALSE);
}

static void
scripts_fill_list (ScriptsPrompt *prompt, const char *select)
{
  GtkWidget *child;
  int n = o42_book_n_scripts (prompt->window->book);

  prompt->filling = TRUE;
  while ((child = gtk_widget_get_first_child (prompt->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (prompt->list), child);
  for (int i = 0; i < n; i++)
    {
      const char *sname = o42_book_script_name (prompt->window->book, i);
      GtkWidget *label = gtk_label_new (sname);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_widget_set_margin_start (label, 6);
      gtk_widget_set_margin_end (label, 6);
      gtk_list_box_append (GTK_LIST_BOX (prompt->list), label);
      if (select != NULL && strcmp (sname, select) == 0)
        gtk_list_box_select_row (GTK_LIST_BOX (prompt->list),
                                 gtk_list_box_get_row_at_index (GTK_LIST_BOX (prompt->list), i));
    }
  prompt->filling = FALSE;
}

static void
on_scripts_row_selected (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  ScriptsPrompt *prompt = data;
  const char *sname, *code;
  (void) list;

  if (prompt->filling || row == NULL)
    return;
  sname = o42_book_script_name (prompt->window->book, gtk_list_box_row_get_index (row));
  code = sname != NULL ? o42_book_script_code (prompt->window->book, sname) : NULL;
  gtk_editable_set_text (GTK_EDITABLE (prompt->name), sname != NULL ? sname : "");
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->view)), code != NULL ? code : "", -1);
}

static gboolean
scripts_save (ScriptsPrompt *prompt)
{
  const char *sname = gtk_editable_get_text (GTK_EDITABLE (prompt->name));
  char *code;

  if (*sname == '\0')
    {
      gtk_widget_grab_focus (prompt->name);
      return FALSE;
    }
  code = scripts_view_text (prompt);
  o42_book_set_script (prompt->window->book, sname, code);
  g_free (code);
  scripts_fill_list (prompt, sname);
  window_sync (prompt->window);
  return TRUE;
}

static void
on_scripts_save (GtkWidget *w, gpointer data)
{
  (void) w;
  scripts_save (data);
}

static void
on_scripts_run (GtkWidget *w, gpointer data)
{
  ScriptsPrompt *prompt = data;
  char *code;
  (void) w;
  if (!scripts_save (prompt))
    return;
  code = scripts_view_text (prompt);
  window_run_script (prompt->window, gtk_editable_get_text (GTK_EDITABLE (prompt->name)), code);
  g_free (code);
}

static void
on_scripts_new (GtkWidget *w, gpointer data)
{
  ScriptsPrompt *prompt = data;
  char *sname = g_strdup_printf ("Script%d", o42_book_n_scripts (prompt->window->book) + 1);
  (void) w;
  gtk_list_box_unselect_all (GTK_LIST_BOX (prompt->list));
  gtk_editable_set_text (GTK_EDITABLE (prompt->name), sname);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->view)),
                            "import office42\n\n", -1);
  gtk_widget_grab_focus (prompt->view);
  g_free (sname);
}

static void
on_scripts_delete (GtkWidget *w, gpointer data)
{
  ScriptsPrompt *prompt = data;
  (void) w;
  if (o42_book_remove_script (prompt->window->book, gtk_editable_get_text (GTK_EDITABLE (prompt->name))))
    {
      gtk_editable_set_text (GTK_EDITABLE (prompt->name), "");
      gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->view)), "", -1);
      scripts_fill_list (prompt, NULL);
      window_sync (prompt->window);
    }
}

void
action_scripts (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  ScriptsPrompt *prompt = g_new0 (ScriptsPrompt, 1);
  GtkWidget *content, *buttons, *columns, *left, *scroller, *row, *new_button, *run;

  (void) a; (void) p;
  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Scripts in this Book"), FALSE, &content, &buttons);
  gtk_window_set_resizable (GTK_WINDOW (prompt->dialog), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (prompt->dialog), 720, 460);

  columns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_vexpand (columns, TRUE);
  left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_size_request (left, 180, -1);
  prompt->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (prompt->list), GTK_SELECTION_SINGLE);
  g_signal_connect (prompt->list, "row-selected", G_CALLBACK (on_scripts_row_selected), prompt);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), prompt->list);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_widget_add_css_class (scroller, "frame");
  gtk_box_append (GTK_BOX (left), scroller);
  new_button = gtk_button_new_with_mnemonic (_("_New Script"));
  g_signal_connect (new_button, "clicked", G_CALLBACK (on_scripts_new), prompt);
  gtk_box_append (GTK_BOX (left), new_button);
  gtk_box_append (GTK_BOX (columns), left);

  {
    GtkWidget *right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *code_scroller = gtk_scrolled_window_new ();
    row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append (GTK_BOX (row), gtk_label_new (_("Name:")));
    prompt->name = gtk_entry_new ();
    gtk_widget_set_hexpand (prompt->name, TRUE);
    gtk_box_append (GTK_BOX (row), prompt->name);
    gtk_box_append (GTK_BOX (right), row);
    prompt->view = gtk_text_view_new ();
    gtk_text_view_set_monospace (GTK_TEXT_VIEW (prompt->view), TRUE);
    gtk_text_view_set_left_margin (GTK_TEXT_VIEW (prompt->view), 6);
    gtk_text_view_set_top_margin (GTK_TEXT_VIEW (prompt->view), 4);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (code_scroller), prompt->view);
    gtk_widget_set_vexpand (code_scroller, TRUE);
    gtk_widget_set_hexpand (code_scroller, TRUE);
    gtk_widget_add_css_class (code_scroller, "frame");
    gtk_box_append (GTK_BOX (right), code_scroller);
    gtk_box_append (GTK_BOX (columns), right);
  }
  gtk_box_append (GTK_BOX (content), columns);

  dialog_button (buttons, _("_Save"), G_CALLBACK (on_scripts_save), prompt);
  run = dialog_button (buttons, _("_Run"), G_CALLBACK (on_scripts_run), prompt);
  gtk_widget_set_sensitive (run, o42_python_available ());
  dialog_button (buttons, _("_Delete"), G_CALLBACK (on_scripts_delete), prompt);
  dialog_button (buttons, _("Close"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  scripts_fill_list (prompt, o42_book_script_name (self->book, 0));
  if (o42_book_n_scripts (self->book) > 0)
    on_scripts_row_selected (GTK_LIST_BOX (prompt->list),
                             gtk_list_box_get_row_at_index (GTK_LIST_BOX (prompt->list), 0), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Tools > Spelling -------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *word_label, *where_label, *replacement, *suggestions;
  O42Spell  *spell;
  O42Range   range;
  int        row, col;     /* where the walk has got to */
  gsize      from;         /* the byte offset in that cell to look on from */
  char      *word;         /* the misspelling in hand */
  gsize      offset, length;
} SpellPrompt;

/* The first word at or after `from` that the dictionary does not know. */
typedef struct {
  O42Spell *spell;
  gsize     from;
  char     *word;
  gsize     offset, length;
} SpellScan;

static void
spell_scan_word (const char *word, gsize offset, gsize length, gpointer user)
{
  SpellScan *scan = user;

  if (scan->word != NULL || offset < scan->from)
    return;
  if (o42_spell_check (scan->spell, word))
    return;
  scan->word = g_strdup (word);
  scan->offset = offset;
  scan->length = length;
}

/* Walks on until something is misspelt, or the range runs out. */
static gboolean
spell_find_next (SpellPrompt *prompt)
{
  O42Sheet *sheet = prompt->window->sheet;

  g_clear_pointer (&prompt->word, g_free);
  for (; prompt->row <= prompt->range.row1; prompt->row++, prompt->col = prompt->range.col0)
    for (; prompt->col <= prompt->range.col1; prompt->col++)
      {
        O42Value value;
        char *shown;
        SpellScan scan = { prompt->spell, prompt->from, NULL, 0, 0 };

        o42_sheet_get_value (sheet, prompt->row, prompt->col, &value);
        if (value.type != O42_VALUE_TEXT)
          {
            o42_value_clear (&value);
            prompt->from = 0;
            continue;
          }
        o42_value_clear (&value);

        shown = o42_sheet_get_display (sheet, prompt->row, prompt->col);
        o42_spell_words (shown, spell_scan_word, &scan);
        g_free (shown);
        if (scan.word != NULL)
          {
            prompt->word = scan.word;
            prompt->offset = scan.offset;
            prompt->length = scan.length;
            return TRUE;
          }
        prompt->from = 0;
      }
  return FALSE;
}

static void
spell_show (SpellPrompt *prompt)
{
  char **suggestions;
  char *where;

  if (!spell_find_next (prompt))
    {
      gtk_label_set_text (GTK_LABEL (prompt->word_label), _("The spelling is checked."));
      gtk_label_set_text (GTK_LABEL (prompt->where_label), "");
      gtk_editable_set_text (GTK_EDITABLE (prompt->replacement), "");
      gtk_widget_set_sensitive (prompt->replacement, FALSE);
      return;
    }

  where = o42_ref_name (prompt->row, prompt->col);
  gtk_label_set_text (GTK_LABEL (prompt->word_label), prompt->word);
  gtk_label_set_text (GTK_LABEL (prompt->where_label), where);
  g_free (where);

  suggestions = o42_spell_suggest (prompt->spell, prompt->word);
  gtk_editable_set_text (GTK_EDITABLE (prompt->replacement),
                         (suggestions != NULL && suggestions[0] != NULL) ? suggestions[0]
                                                                         : prompt->word);
  gtk_widget_set_sensitive (prompt->replacement, TRUE);
  {
    GString *rest = g_string_new (NULL);

    for (int i = 1; suggestions != NULL && suggestions[i] != NULL && i < 6; i++)
      g_string_append_printf (rest, "%s%s", rest->len > 0 ? ", " : "", suggestions[i]);
    gtk_label_set_text (GTK_LABEL (prompt->suggestions), rest->str);
    g_string_free (rest, TRUE);
  }
  g_strfreev (suggestions);
}

static void
on_spell_change (GtkWidget *w, gpointer data)
{
  SpellPrompt *prompt = data;
  const char *replacement = gtk_editable_get_text (GTK_EDITABLE (prompt->replacement));
  char *shown;
  char *changed;

  (void) w;
  if (prompt->word == NULL || prompt->window->sheet == NULL)
    return;

  shown = o42_sheet_get_display (prompt->window->sheet, prompt->row, prompt->col);
  changed = g_strdup_printf ("%.*s%s%s", (int) prompt->offset, shown, replacement,
                             shown + prompt->offset + prompt->length);
  o42_sheet_set_input (prompt->window->sheet, prompt->row, prompt->col, changed);
  prompt->from = prompt->offset + strlen (replacement);
  g_free (changed);
  g_free (shown);

  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
  spell_show (prompt);
}

static void
on_spell_ignore (GtkWidget *w, gpointer data)
{
  SpellPrompt *prompt = data;

  (void) w;
  prompt->from = prompt->offset + prompt->length;
  spell_show (prompt);
}

static void
on_spell_ignore_all (GtkWidget *w, gpointer data)
{
  SpellPrompt *prompt = data;

  (void) w;
  if (prompt->word != NULL)
    o42_spell_ignore (prompt->spell, prompt->word);
  prompt->from = prompt->offset + prompt->length;
  spell_show (prompt);
}

static void
on_spell_destroy (GtkWidget *w, gpointer data)
{
  SpellPrompt *prompt = data;

  (void) w;
  o42_spell_free (prompt->spell);
  g_free (prompt->word);
  g_free (prompt);
}

void
action_spelling (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  SpellPrompt *prompt;
  GtkWidget *content, *buttons, *grid;
  O42Spell *spell;

  (void) a; (void) p;

  spell = o42_spell_new (NULL);
  if (spell == NULL)
    {
      gtk_label_set_text (GTK_LABEL (self->status_label), o42_spell_available () ? _("No dictionary was found for this language.") : _("This build has no spelling checker."));
      return;
    }

  prompt = g_new0 (SpellPrompt, 1);
  prompt->window = self;
  prompt->spell = spell;
  o42_grid_get_selection (self->grid, &prompt->range);
  if (prompt->range.row0 == prompt->range.row1 && prompt->range.col0 == prompt->range.col1)
    o42_sheet_used_range (self->sheet, &prompt->range);   /* the whole sheet */
  prompt->row = prompt->range.row0;
  prompt->col = prompt->range.col0;

  prompt->dialog = dialog_frame (self, _("Spelling"), TRUE, &content, &buttons);
  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  gtk_box_append (GTK_BOX (content), grid);

  prompt->word_label = labelled (grid, 0, _("Not in dictionary:"), gtk_label_new (""));
  prompt->where_label = labelled (grid, 1, _("In cell:"), gtk_label_new (""));
  prompt->replacement = labelled (grid, 2, _("Change to:"), gtk_entry_new ());
  prompt->suggestions = labelled (grid, 3, _("Or:"), gtk_label_new (""));
  gtk_label_set_wrap (GTK_LABEL (prompt->suggestions), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (prompt->suggestions), 32);

  dialog_button (buttons, _("_Change"), G_CALLBACK (on_spell_change), prompt);
  dialog_button (buttons, _("_Ignore"), G_CALLBACK (on_spell_ignore), prompt);
  dialog_button (buttons, _("Ignore _All"), G_CALLBACK (on_spell_ignore_all), prompt);
  dialog_button (buttons, _("_Close"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_spell_destroy), prompt);

  spell_show (prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* View > Page Breaks: show where the printed pages divide. */
void
action_page_breaks (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  gboolean now = !o42_grid_page_breaks_shown (self->grid);

  (void) a; (void) p;
  o42_grid_show_page_breaks (self->grid, now);
  window_sync (self);
  gtk_label_set_text (GTK_LABEL (self->status_label), now ? _("The dashed lines are where the pages divide.") : _("Ready"));
}

/* ---- View > Custom Views ----------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *list;        /* the names, one row each */
  GtkWidget *name;        /* what to call a new one */
} ViewsPrompt;

static void
views_fill (ViewsPrompt *prompt)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (prompt->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (prompt->list), child);
  for (int i = 0; i < o42_book_n_views (prompt->window->book); i++)
    {
      const O42BookView *view = o42_book_view (prompt->window->book, i);
      char *text = g_strdup_printf ("%s  (%s)", view->name, view->sheet);
      GtkWidget *label = gtk_label_new (text);

      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      g_object_set_data_full (G_OBJECT (label), "view-name", g_strdup (view->name), g_free);
      gtk_list_box_append (GTK_LIST_BOX (prompt->list), label);
      g_free (text);
    }
}

/* The name on the row the user has picked, or NULL. */
static const char *
views_selected (ViewsPrompt *prompt)
{
  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (prompt->list));
  GtkWidget *child = row != NULL ? gtk_list_box_row_get_child (row) : NULL;

  return child != NULL ? g_object_get_data (G_OBJECT (child), "view-name") : NULL;
}

static void
on_views_add (GtkWidget *w, gpointer data)
{
  ViewsPrompt *prompt = data;
  O42Window *self = prompt->window;
  const char *name = gtk_editable_get_text (GTK_EDITABLE (prompt->name));
  O42BookView view;

  (void) w;
  if (name == NULL || *name == '\0')
    return;

  memset (&view, 0, sizeof view);
  view.name = (char *) name;
  view.sheet = (char *) o42_sheet_get_name (self->sheet);
  o42_grid_get_selection (self->grid, &view.selection);
  o42_grid_get_active (self->grid, &view.active_row, &view.active_col);
  view.zoom = o42_grid_get_zoom (self->grid);
  o42_sheet_get_frozen (self->sheet, &view.frozen_rows, &view.frozen_cols);
  view.split = o42_grid_is_split (self->grid);
  o42_book_set_view (self->book, &view);

  gtk_editable_set_text (GTK_EDITABLE (prompt->name), "");
  views_fill (prompt);
  window_sync (self);
}

static void
on_views_show (GtkWidget *w, gpointer data)
{
  ViewsPrompt *prompt = data;
  O42Window *self = prompt->window;
  const char *name = views_selected (prompt);
  const O42BookView *view = name != NULL ? o42_book_find_view (self->book, name) : NULL;
  int index;

  (void) w;
  if (view == NULL)
    return;

  index = o42_book_sheet_index (self->book, o42_book_find_sheet (self->book, view->sheet));
  if (index >= 0)
    window_show_sheet (self, index);
  o42_grid_set_zoom (self->grid, view->zoom > 0 ? view->zoom : 1.0);
  o42_grid_select_range (self->grid, &view->selection);
  o42_window_select_cell (self, view->active_row, view->active_col);
  window_sync (self);
}

static void
on_views_delete (GtkWidget *w, gpointer data)
{
  ViewsPrompt *prompt = data;
  const char *name = views_selected (prompt);

  (void) w;
  if (name != NULL && o42_book_remove_view (prompt->window->book, name))
    views_fill (prompt);
}

void
action_custom_views (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  ViewsPrompt *prompt = g_new0 (ViewsPrompt, 1);
  GtkWidget *content, *buttons, *scroller, *row;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Custom Views"), TRUE, &content, &buttons);

  prompt->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (prompt->list), GTK_SELECTION_SINGLE);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), prompt->list);
  gtk_widget_set_size_request (scroller, 300, 160);
  gtk_box_append (GTK_BOX (content), scroller);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Name:")));
  prompt->name = gtk_entry_new ();
  gtk_widget_set_hexpand (prompt->name, TRUE);
  gtk_box_append (GTK_BOX (row), prompt->name);
  gtk_box_append (GTK_BOX (content), row);

  views_fill (prompt);

  dialog_button (buttons, _("_Add"), G_CALLBACK (on_views_add), prompt);
  dialog_button (buttons, _("_Show"), G_CALLBACK (on_views_show), prompt);
  dialog_button (buttons, _("_Delete"), G_CALLBACK (on_views_delete), prompt);
  dialog_button (buttons, _("_Close"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Tools > Statistical Analysis -------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *tool;
  GtkWidget *input, *output;
  GtkWidget *labels, *by_rows;
  GtkWidget *number;      /* bins, terms, rows to a sample, or how many to draw */
  GtkWidget *periodic;    /* sampling: every nth rather than at random */
} AnalysisPrompt;

static void
on_analysis_ok (GtkWidget *w, gpointer data)
{
  AnalysisPrompt *prompt = data;
  O42Window *self = prompt->window;
  O42AnalysisOptions options;
  const char *input = gtk_editable_get_text (GTK_EDITABLE (prompt->input));
  const char *output = gtk_editable_get_text (GTK_EDITABLE (prompt->output));
  gsize len = 0;
  guint which = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->tool));
  gboolean ok = FALSE;

  (void) w;
  memset (&options, 0, sizeof options);
  options.confidence = 0.95;
  options.labels = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->labels));
  options.layout = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->by_rows))
                   ? O42_ANALYSIS_ROWS : O42_ANALYSIS_COLUMNS;
  options.bins = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->number));
  options.interval = options.bins;
  options.per_sample = options.bins;
  options.sample_size = options.bins;
  options.periodic = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->periodic));

  if (!o42_ref_parse (input, &options.input.row0, &options.input.col0, &len) ||
      input[len] != ':' ||
      !o42_ref_parse (input + len + 1, &options.input.row1, &options.input.col1, NULL) ||
      !o42_ref_parse (output, &options.out_row, &options.out_col, NULL))
    {
      gtk_label_set_text (GTK_LABEL (self->status_label), _("Give a range like A1:C10 and a cell to put the results in."));
      return;
    }

  switch (which)
    {
    case 0: ok = o42_analysis_descriptive (self->sheet, &options); break;
    case 1: ok = o42_analysis_correlation (self->sheet, &options); break;
    case 2: ok = o42_analysis_covariance (self->sheet, &options); break;
    case 3: ok = o42_analysis_regression (self->sheet, &options); break;
    case 4: ok = o42_analysis_histogram (self->sheet, &options); break;
    case 5: ok = o42_analysis_anova (self->sheet, &options); break;
    case 6: ok = o42_analysis_anova2 (self->sheet, &options); break;
    case 7: ok = o42_analysis_sampling (self->sheet, &options); break;
    case 8: ok = o42_analysis_rank (self->sheet, &options); break;
    default: ok = o42_analysis_moving (self->sheet, &options); break;
    }

  o42_grid_refresh (self->grid);
  window_sync (self);
  if (!ok)
    gtk_label_set_text (GTK_LABEL (self->status_label), _("There is not enough data in that range for this tool."));
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

/* ---- Tools > Custom Lists ---------------------------------------------- */


typedef struct {
  O42Window     *window;
  GtkWidget     *dialog;
  GtkWidget     *list;
  GtkStringList *entries;
  GtkWidget     *items;      /* the entry a new list is typed into */
} CustomListPrompt;

static void
custom_list_fill (CustomListPrompt *prompt)
{
  GPtrArray *lists = o42_book_custom_lists (prompt->window->book);

  gtk_string_list_splice (prompt->entries, 0,
                          g_list_model_get_n_items (G_LIST_MODEL (prompt->entries)), NULL);
  for (guint i = 0; i < lists->len; i++)
    {
      char *joined = g_strjoinv (", ", g_ptr_array_index (lists, i));

      gtk_string_list_append (prompt->entries, joined);
      g_free (joined);
    }
}

static void
on_custom_list_add (GtkWidget *w, gpointer data)
{
  CustomListPrompt *prompt = data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (prompt->items));
  char **parts = g_strsplit (text, ",", -1);

  (void) w;
  for (int i = 0; parts[i] != NULL; i++)
    {
      char *trimmed = g_strdup (g_strstrip (parts[i]));

      g_free (parts[i]);
      parts[i] = trimmed;
    }
  o42_book_add_custom_list (prompt->window->book, parts);
  custom_list_fill (prompt);
  gtk_editable_set_text (GTK_EDITABLE (prompt->items), "");
}

static void
on_custom_list_remove (GtkWidget *w, gpointer data)
{
  CustomListPrompt *prompt = data;
  GtkSelectionModel *model = gtk_list_view_get_model (GTK_LIST_VIEW (prompt->list));
  guint at = gtk_single_selection_get_selected (GTK_SINGLE_SELECTION (model));

  (void) w;
  if (at != GTK_INVALID_LIST_POSITION)
    {
      o42_book_remove_custom_list (prompt->window->book, at);
      custom_list_fill (prompt);
    }
}

void
action_custom_lists (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  CustomListPrompt *prompt = g_new0 (CustomListPrompt, 1);
  GtkWidget *content, *buttons, *scrolled, *grid;
  GtkListItemFactory *factory;
  GtkSingleSelection *selection;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Custom Lists"), TRUE, &content, &buttons);

  prompt->entries = gtk_string_list_new (NULL);
  custom_list_fill (prompt);
  factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (wizard_setup_item), prompt);
  g_signal_connect (factory, "bind", G_CALLBACK (wizard_bind_item), prompt);
  selection = gtk_single_selection_new (G_LIST_MODEL (prompt->entries));
  gtk_single_selection_set_autoselect (selection, FALSE);
  prompt->list = gtk_list_view_new (GTK_SELECTION_MODEL (selection), factory);

  scrolled = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), prompt->list);
  gtk_widget_set_size_request (scrolled, 320, 140);
  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Lists the fill handle continues:")));
  gtk_box_append (GTK_BOX (content), scrolled);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->items = labelled (grid, 0, _("New list:"), gtk_entry_new ());
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->items), _("Spring, Summer, Autumn, Winter"));
  gtk_box_append (GTK_BOX (content), grid);
  gtk_box_append (GTK_BOX (content),
                  gtk_label_new (_("The days and the months are known already.")));

  dialog_button (buttons, _("_Add"), G_CALLBACK (on_custom_list_add), prompt);
  dialog_button (buttons, _("_Remove"), G_CALLBACK (on_custom_list_remove), prompt);
  dialog_button (buttons, _("_Close"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  g_object_set_data_full (G_OBJECT (prompt->dialog), "o42-prompt", prompt, g_free);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Tools > Auditing -------------------------------------------------- */

void
action_trace_precedents (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  (void) a; (void) p;
  o42_grid_trace (self->grid, TRUE);
  if (!o42_grid_has_arrows (self->grid))
    gtk_label_set_text (GTK_LABEL (self->status_label),
                        _("That cell holds no formula, so it reads nothing."));
}

void
action_trace_dependents (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  (void) a; (void) p;
  o42_grid_trace (self->grid, FALSE);
  if (!o42_grid_has_arrows (self->grid))
    gtk_label_set_text (GTK_LABEL (self->status_label),
                        _("No formula on this sheet reads that cell."));
}

void
action_clear_arrows (GSimpleAction *a, GVariant *p, gpointer data)
{
  (void) a; (void) p;
  o42_grid_clear_arrows (O42_WINDOW (data)->grid);
}

void
action_analysis (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  AnalysisPrompt *prompt = g_new0 (AnalysisPrompt, 1);
  GtkWidget *content, *buttons, *grid, *ok;
  static const char *const TOOLS[] = {
    N_("Descriptive Statistics"), N_("Correlation"), N_("Covariance"), N_("Regression"),
    N_("Histogram"), N_("ANOVA: Single Factor"), N_("ANOVA: Two Factor"),
    N_("Sampling"), N_("Rank and Percentile"), N_("Moving Average"), NULL
  };
  O42Range selection;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Statistical Analysis"), TRUE, &content, &buttons);
  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  gtk_box_append (GTK_BOX (content), grid);

  prompt->tool = labelled (grid, 0, _("Tool:"), drop_down_of (TOOLS));
  prompt->input = labelled (grid, 1, _("Input range:"), gtk_entry_new ());
  prompt->output = labelled (grid, 2, _("Output at:"), gtk_entry_new ());
  /* One number serves every tool that wants one: the bins of a
   * histogram, the terms of a moving average, the rows to a sample of
   * a two-factor analysis, or how many values a sample draws. */
  prompt->number = labelled (grid, 3, _("Bins, terms or sample:"),
                             gtk_spin_button_new_with_range (0, 1000, 1));

  o42_grid_get_selection (self->grid, &selection);
  {
    char *first = o42_ref_name (selection.row0, selection.col0);
    char *last = o42_ref_name (selection.row1, selection.col1);
    char *range = g_strdup_printf ("%s:%s", first, last);
    char *at = o42_ref_name (selection.row0, MIN (selection.col1 + 2, O42_MAX_COLS - 1));

    gtk_editable_set_text (GTK_EDITABLE (prompt->input), range);
    gtk_editable_set_text (GTK_EDITABLE (prompt->output), at);
    g_free (first);
    g_free (last);
    g_free (range);
    g_free (at);
  }

  prompt->labels = gtk_check_button_new_with_mnemonic ( _("First row holds the _names"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->labels), TRUE);
  gtk_box_append (GTK_BOX (content), prompt->labels);
  prompt->by_rows = gtk_check_button_new_with_mnemonic ( _("Variables lie along the _rows"));
  gtk_box_append (GTK_BOX (content), prompt->by_rows);
  prompt->periodic = gtk_check_button_new_with_mnemonic ( _("Sample every nth value, not at rando_m"));
  gtk_box_append (GTK_BOX (content), prompt->periodic);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_analysis_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Format > Group ----------------------------------------------------- */

void
action_group_objects (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  (void) a; (void) p;
  o42_grid_group_objects (self->grid, TRUE);
  window_sync (self);
  gtk_label_set_text (GTK_LABEL (self->status_label), _("The objects in the selection now move together."));
}

void
action_ungroup_objects (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  (void) a; (void) p;
  o42_grid_group_objects (self->grid, FALSE);
  window_sync (self);
  gtk_label_set_text (GTK_LABEL (self->status_label), _("The objects are apart again."));
}

/* ---- Tools > Record Macro ---------------------------------------------- */

/* Excel records a macro by writing down what you do; office42 writes
 * the Python that does it again.  Recording stops into a script in the
 * book, where Tools > Scripts can run or edit it. */
void
action_record_macro (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  (void) a; (void) p;

  if (!o42_book_recording (self->book))
    {
      o42_book_record_start (self->book);
      window_sync (self);
      gtk_label_set_text (GTK_LABEL (self->status_label), _("Recording. Tools > Record Macro again to stop."));
      return;
    }
  else
    {
      char *script = o42_book_record_stop (self->book);
      char *name = NULL;
      char *said;

      for (int i = 1; name == NULL; i++)
        {
          char *candidate = g_strdup_printf ("Macro%d", i);

          if (o42_book_script_code (self->book, candidate) == NULL)
            name = candidate;
          else
            g_free (candidate);
        }
      o42_book_set_script (self->book, name, script != NULL ? script : "");
      said = g_strdup_printf ("Recorded %s: Tools > Scripts runs it.", name);
      o42_book_set_modified (self->book, TRUE);
      window_sync (self);
      gtk_label_set_text (GTK_LABEL (self->status_label), said);
      g_free (said);
      g_free (name);
      g_free (script);
      return;
    }
  window_sync (self);
}

/* ---- Tools > Protection ------------------------------------------------ */

/* Format > Protect Sheet asks for a password when it is putting the
 * protection on, and for the same one when taking it off. */
typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *entry;
  GtkWidget *status;
  gboolean   locking;
} ProtectPrompt;

static void
on_protect_ok (GtkWidget *w, gpointer data)
{
  ProtectPrompt *prompt = data;
  O42Window *self = prompt->window;
  const char *typed = gtk_editable_get_text (GTK_EDITABLE (prompt->entry));

  (void) w;
  if (prompt->locking)
    {
      o42_sheet_set_password (self->sheet, typed);
      o42_sheet_set_protected (self->sheet, TRUE);
      gtk_label_set_text (GTK_LABEL (self->status_label),
                          _("The sheet is protected: locked cells cannot be typed into."));
    }
  else
    {
      if (!o42_sheet_password_matches (self->sheet, typed))
        {
          gtk_label_set_text (GTK_LABEL (prompt->status), _("That is not the password."));
          return;
        }
      o42_sheet_set_protected (self->sheet, FALSE);
      o42_sheet_set_password (self->sheet, NULL);
      gtk_label_set_text (GTK_LABEL (self->status_label),
                          _("The sheet is no longer protected."));
    }
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_protect (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  gboolean now = !o42_sheet_protected (self->sheet);
  ProtectPrompt *prompt;
  GtkWidget *content, *buttons, *grid, *ok;

  (void) a; (void) p;

  /* Taking protection off a sheet that never had a password needs no
   * dialog at all. */
  if (!now && o42_sheet_password_hash (self->sheet) == 0)
    {
      o42_sheet_set_protected (self->sheet, FALSE);
      window_sync (self);
      gtk_label_set_text (GTK_LABEL (self->status_label),
                          _("The sheet is no longer protected."));
      return;
    }

  prompt = g_new0 (ProtectPrompt, 1);
  prompt->window = self;
  prompt->locking = now;
  prompt->dialog = dialog_frame (self, now ? _("Protect Sheet") : _("Unprotect Sheet"),
                                 TRUE, &content, &buttons);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->entry = labelled (grid, 0, _("Password:"), gtk_entry_new ());
  gtk_entry_set_visibility (GTK_ENTRY (prompt->entry), FALSE);
  if (now)
    gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->entry), _("leave empty for none"));
  gtk_box_append (GTK_BOX (content), grid);

  if (now)
    {
      GtkWidget *hint = gtk_label_new (
        _("A spreadsheet's password keeps a hand off the keys, not a "
          "reader out of the file: it is kept as a short hash that anything "
          "reading the file can ignore."));

      gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
      gtk_label_set_max_width_chars (GTK_LABEL (hint), 44);
      gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
      gtk_box_append (GTK_BOX (content), hint);
    }

  prompt->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (prompt->status), 0.0);
  gtk_box_append (GTK_BOX (content), prompt->status);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_protect_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  g_object_set_data_full (G_OBJECT (prompt->dialog), "o42-prompt", prompt, g_free);
  gtk_widget_grab_focus (prompt->entry);
  (void) ok;
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Tools > Solver ---------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *target, *goal, *value, *changing, *bounds, *status;
} SolverPrompt;

static const char *SOLVER_GOALS[] = { N_("Max"), N_("Min"), N_("Value of"), NULL };

static void
on_solver_solve (GtkWidget *w, gpointer data)
{
  SolverPrompt *prompt = data;
  O42Ref changing[16];
  O42SolverBound bounds[16];
  int n_changing = 0, n_bounds = 0;
  int trow, tcol;
  guint goal = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->goal));
  double reached = 0;
  char **cells;
  char *lines;
  GtkTextIter a, b;

  (void) w;
  if (!o42_ref_parse (gtk_editable_get_text (GTK_EDITABLE (prompt->target)), &trow, &tcol, NULL))
    {
      gtk_label_set_text (GTK_LABEL (prompt->status), _("That is not a cell to aim at."));
      return;
    }

  cells = g_strsplit_set (gtk_editable_get_text (GTK_EDITABLE (prompt->changing)), ",; ", -1);
  for (int i = 0; cells[i] != NULL && n_changing < 16; i++)
    {
      char *cell = g_strstrip (cells[i]);
      gsize len = 0;
      int row, col, row1, col1;

      if (*cell == '\0')
        continue;
      if (o42_ref_parse (cell, &row, &col, &len) && cell[len] == ':' &&
          o42_ref_parse (cell + len + 1, &row1, &col1, NULL))
        {
          /* A range of changing cells, cell by cell. */
          O42Range r = o42_range_normalise (row, col, row1, col1);
          for (int rr = r.row0; rr <= r.row1 && n_changing < 16; rr++)
            for (int cc = r.col0; cc <= r.col1 && n_changing < 16; cc++)
              { changing[n_changing].row = rr; changing[n_changing].col = cc; n_changing++; }
        }
      else if (o42_ref_parse (cell, &row, &col, NULL))
        { changing[n_changing].row = row; changing[n_changing].col = col; n_changing++; }
    }
  g_strfreev (cells);
  if (n_changing == 0)
    {
      gtk_label_set_text (GTK_LABEL (prompt->status), _("Name at least one cell to change."));
      return;
    }

  gtk_text_buffer_get_bounds (gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->bounds)), &a, &b);
  lines = gtk_text_buffer_get_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->bounds)), &a, &b, FALSE);
  {
    char **each = g_strsplit (lines, "\n", -1);
    for (int i = 0; each[i] != NULL && n_bounds < 16; i++)
      {
        char *line = g_strstrip (each[i]);
        const char *op = strstr (line, "<=");
        O42SolverOp which = O42_SOLVER_LE;
        char *cell;

        if (*line == '\0')
          continue;
        if (op == NULL) { op = strstr (line, ">="); which = O42_SOLVER_GE; }
        if (op == NULL) { op = strchr (line, '='); which = O42_SOLVER_EQ; }
        if (op == NULL)
          continue;
        cell = g_strstrip (g_strndup (line, (gsize) (op - line)));
        if (o42_ref_parse (cell, &bounds[n_bounds].row, &bounds[n_bounds].col, NULL))
          {
            bounds[n_bounds].op = which;
            bounds[n_bounds].value = g_strtod (op + (which == O42_SOLVER_EQ ? 1 : 2), NULL);
            n_bounds++;
          }
        g_free (cell);
      }
    g_strfreev (each);
  }
  g_free (lines);

  {
    gboolean ok = o42_sheet_solve (prompt->window->sheet, trow, tcol,
                                   goal == 1 ? O42_SOLVER_MIN : goal == 2 ? O42_SOLVER_VALUE : O42_SOLVER_MAX,
                                   g_strtod (gtk_editable_get_text (GTK_EDITABLE (prompt->value)), NULL),
                                   changing, n_changing, bounds, n_bounds, &reached);
    char *message = g_strdup_printf (ok ? "The target reached %g." : "The search gave up at %g.", reached);
    gtk_label_set_text (GTK_LABEL (prompt->status), message);
    g_free (message);
  }
  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
}

void
action_solver (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  SolverPrompt *prompt = g_new0 (SolverPrompt, 1);
  GtkWidget *content, *buttons, *grid, *scrolled, *solve;
  int row, col;
  char *name;

  (void) a; (void) p;
  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Solver"), FALSE, &content, &buttons);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->target = labelled (grid, 0, _("Target cell:"), gtk_entry_new ());
  prompt->goal = labelled (grid, 1, _("Make it:"), drop_down_of (SOLVER_GOALS));
  prompt->value = labelled (grid, 2, _("Value:"), gtk_entry_new ());
  prompt->changing = labelled (grid, 3, _("By changing:"), gtk_entry_new ());
  gtk_widget_set_size_request (prompt->target, 200, -1);
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->changing), _("A1, B1  or  A1:B1"));
  gtk_box_append (GTK_BOX (content), grid);

  o42_grid_get_active (self->grid, &row, &col);
  name = o42_ref_name (row, col);
  gtk_editable_set_text (GTK_EDITABLE (prompt->target), name);
  g_free (name);

  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Keeping these in bounds, one to a line:")));
  prompt->bounds = gtk_text_view_new ();
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (prompt->bounds), TRUE);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (prompt->bounds), 4);
  scrolled = gtk_scrolled_window_new ();
  gtk_widget_set_size_request (scrolled, 320, 90);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), prompt->bounds);
  gtk_widget_add_css_class (scrolled, "frame");
  gtk_box_append (GTK_BOX (content), scrolled);
  {
    GtkWidget *hint = gtk_label_new ("D1<=10, A1>=0, B2=5. The search is a downhill simplex with the "
                                     "broken bounds counted against it: it finds a good answer, not "
                                     "always the best one.");
    gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (hint), 46);
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_box_append (GTK_BOX (content), hint);
  }
  prompt->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (prompt->status), 0.0);
  gtk_box_append (GTK_BOX (content), prompt->status);

  solve = dialog_button (buttons, _("_Solve"), G_CALLBACK (on_solver_solve), prompt);
  dialog_button (buttons, _("_Close"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), solve);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->changing);
}
