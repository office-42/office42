/* o42-dialogs-edit.c - see o42-window-private.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Edit and Insert menus' dialogs: Fill > Series, Move or Copy Sheet,
 * and Name > Paste, Create and Apply.
 */

#include "o42-window-private.h"

#include "o42-types.h"

#include <glib/gi18n.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The names these dialogs have always called them by. */
#define dialog_button o42_dialog_button
#define dialog_frame o42_dialog_frame
#define labelled o42_labelled
#define on_dialog_close_clicked o42_dialog_close_clicked
#define on_dialog_destroy_refocus o42_dialog_destroy_refocus
#define show_error o42_window_show_error
#define window_sync o42_window_sync
#define window_show_sheet o42_window_show_sheet
#define window_tell_book o42_window_tell_book
#define wizard_bind_item o42_wizard_bind_item
#define wizard_setup_item o42_wizard_setup_item

/* A row of radio buttons under a heading, as Excel 97's Series dialog
 * groups them.  The buttons come back in `buttons`. */
static GtkWidget *
radio_group (const char *heading, const char *const *names, int n, GtkWidget **buttons)
{
  GtkWidget *frame = gtk_frame_new (heading);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  gtk_widget_set_margin_top (box, 4);
  gtk_widget_set_margin_bottom (box, 4);
  gtk_widget_set_margin_start (box, 8);
  gtk_widget_set_margin_end (box, 8);
  for (int i = 0; i < n; i++)
    {
      buttons[i] = gtk_check_button_new_with_mnemonic (_(names[i]));
      if (i > 0)
        gtk_check_button_set_group (GTK_CHECK_BUTTON (buttons[i]), GTK_CHECK_BUTTON (buttons[0]));
      gtk_box_append (GTK_BOX (box), buttons[i]);
    }
  gtk_check_button_set_active (GTK_CHECK_BUTTON (buttons[0]), TRUE);
  gtk_frame_set_child (GTK_FRAME (frame), box);
  return frame;
}

static int
radio_chosen (GtkWidget **buttons, int n)
{
  for (int i = 0; i < n; i++)
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (buttons[i])))
      return i;
  return 0;
}

/* ---- Fill > Series ------------------------------------------------------ */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *in[2];        /* rows, columns */
  GtkWidget *type[4];      /* linear, growth, date, autofill */
  GtkWidget *unit[4];      /* day, weekday, month, year */
  GtkWidget *unit_frame;
  GtkWidget *trend;
  GtkWidget *step_entry;
  GtkWidget *stop_entry;
} SeriesPrompt;

static void
on_series_type_toggled (GtkCheckButton *button, gpointer data)
{
  SeriesPrompt *prompt = data;
  gboolean date = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->type[2]));

  (void) button;
  gtk_widget_set_sensitive (prompt->unit_frame, date);
  gtk_widget_set_sensitive (prompt->trend, !date &&
                            !gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->type[3])));
}

static void
on_series_ok (GtkWidget *w, gpointer data)
{
  SeriesPrompt *prompt = data;
  O42Series series;
  const char *stop = gtk_editable_get_text (GTK_EDITABLE (prompt->stop_entry));
  const char *step = gtk_editable_get_text (GTK_EDITABLE (prompt->step_entry));
  char *end = NULL;

  (void) w;

  series.in_rows = radio_chosen (prompt->in, 2) == 0;
  series.type = (O42SeriesType) radio_chosen (prompt->type, 4);
  series.unit = (O42SeriesUnit) radio_chosen (prompt->unit, 4);
  series.trend = gtk_widget_get_sensitive (prompt->trend) &&
                 gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->trend));
  series.step = g_strtod (step, &end);
  if (end == step || (end != NULL && *g_strstrip (end) != '\0'))
    {
      show_error (prompt->window, _("The step value must be a number."), NULL);
      return;
    }
  series.has_stop = (*stop != '\0');
  series.stop = series.has_stop ? g_strtod (stop, &end) : 0;
  if (series.has_stop && (end == stop || (end != NULL && *g_strstrip (end) != '\0')))
    {
      show_error (prompt->window, _("The stop value must be a number."), NULL);
      return;
    }

  o42_grid_fill_series (prompt->window->grid, &series);
  window_sync (prompt->window);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_fill_series (GSimpleAction *a, GVariant *p, gpointer data)
{
  static const char *const IN[]   = { N_("_Rows"), N_("_Columns") };
  static const char *const TYPE[] = { N_("_Linear"), N_("_Growth"), N_("_Date"), N_("_AutoFill") };
  static const char *const UNIT[] = { N_("Da_y"), N_("_Weekday"), N_("_Month"), N_("_Year") };
  O42Window *self = data;
  SeriesPrompt *prompt = g_new0 (SeriesPrompt, 1);
  GtkWidget *content, *buttons, *row, *grid, *ok;
  O42Range sel;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Series"), TRUE, &content, &buttons);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_append (GTK_BOX (row), radio_group (_("Series in"), IN, 2, prompt->in));
  gtk_box_append (GTK_BOX (row), radio_group (_("Type"), TYPE, 4, prompt->type));
  prompt->unit_frame = radio_group (_("Date unit"), UNIT, 4, prompt->unit);
  gtk_box_append (GTK_BOX (row), prompt->unit_frame);
  gtk_box_append (GTK_BOX (content), row);

  /* Excel's guess: a selection wider than it is tall runs in rows. */
  o42_grid_get_selection (self->grid, &sel);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->in[sel.col1 - sel.col0 > sel.row1 - sel.row0 ? 0 : 1]), TRUE);

  prompt->trend = gtk_check_button_new_with_mnemonic (_("_Trend"));
  gtk_box_append (GTK_BOX (content), prompt->trend);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->step_entry = labelled (grid, 0, _("Step value:"), gtk_entry_new ());
  gtk_editable_set_text (GTK_EDITABLE (prompt->step_entry), "1");
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->step_entry), 10);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->step_entry), TRUE);
  prompt->stop_entry = labelled (grid, 1, _("Stop value:"), gtk_entry_new ());
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->stop_entry), 10);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->stop_entry), TRUE);
  gtk_box_append (GTK_BOX (content), grid);

  for (int i = 0; i < 4; i++)
    g_signal_connect (prompt->type[i], "toggled", G_CALLBACK (on_series_type_toggled), prompt);
  on_series_type_toggled (NULL, prompt);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_series_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->step_entry);
  gtk_editable_select_region (GTK_EDITABLE (prompt->step_entry), 0, -1);
}

/* ---- Move or Copy Sheet ------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *before;       /* a drop-down: each sheet, then "(move to end)" */
  GtkWidget *make_copy;
} MoveCopyPrompt;

static void
on_move_copy_ok (GtkWidget *w, gpointer data)
{
  MoveCopyPrompt *prompt = data;
  O42Window *self = prompt->window;
  int from = o42_book_sheet_index (self->book, self->sheet);
  int n = o42_book_n_sheets (self->book);
  int before = (int) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->before));
  gboolean copy = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->make_copy));

  (void) w;
  if (from < 0 || before < 0)
    return;

  if (copy)
    {
      O42Sheet *made = o42_book_copy_sheet (self->book, from, before >= n ? -1 : before, NULL);

      if (made != NULL)
        window_show_sheet (self, o42_book_sheet_index (self->book, made));
    }
  else
    {
      /* Before the sheet chosen -- which, once this one is taken out
       * from in front of it, is one place nearer the front. */
      int to = before > from ? before - 1 : before;

      if (to != from)
        {
          o42_book_move_sheet (self->book, from, to);
          window_show_sheet (self, to);
        }
    }
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_move_copy_sheet (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  MoveCopyPrompt *prompt = g_new0 (MoveCopyPrompt, 1);
  GtkWidget *content, *buttons, *ok;
  GtkStringList *names = gtk_string_list_new (NULL);
  int n = o42_book_n_sheets (self->book);
  int index = o42_book_sheet_index (self->book, self->sheet);

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Move or Copy"), TRUE, &content, &buttons);

  for (int i = 0; i < n; i++)
    gtk_string_list_append (names, o42_sheet_get_name (o42_book_sheet (self->book, i)));
  gtk_string_list_append (names, _("(move to end)"));

  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Before sheet:")));
  prompt->before = gtk_drop_down_new (G_LIST_MODEL (names), NULL);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->before), (guint) MIN (index + 1, n));
  gtk_box_append (GTK_BOX (content), prompt->before);

  prompt->make_copy = gtk_check_button_new_with_mnemonic (_("Create a _copy"));
  gtk_box_append (GTK_BOX (content), prompt->make_copy);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_move_copy_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Insert > Name ------------------------------------------------------ */

/* A list of the book's names in a scrolled view, for Paste and Apply. */
static GtkWidget *
names_list (O42Window *self, GtkStringList **names, GtkSelectionModel **selection, gboolean multiple)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  GtkWidget *scrolled, *list;
  GList *all = o42_book_names (self->book);

  *names = gtk_string_list_new (NULL);
  for (GList *l = all; l != NULL; l = l->next)
    gtk_string_list_append (*names, l->data);
  g_list_free (all);

  g_signal_connect (factory, "setup", G_CALLBACK (wizard_setup_item), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (wizard_bind_item), NULL);
  if (multiple)
    *selection = GTK_SELECTION_MODEL (gtk_multi_selection_new (G_LIST_MODEL (*names)));
  else
    {
      GtkSingleSelection *single = gtk_single_selection_new (G_LIST_MODEL (*names));
      gtk_single_selection_set_autoselect (single, TRUE);
      *selection = GTK_SELECTION_MODEL (single);
    }
  list = gtk_list_view_new (*selection, factory);
  scrolled = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (scrolled, 300, 160);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), list);
  return scrolled;
}

/* The name a formula refers to, as the Define Name dialog writes it. */
static char *
name_refers_to (O42Window *self, const char *name)
{
  O42Sheet *target;
  O42Range r;

  if (o42_book_lookup_name (self->book, name, &target, &r))
    {
      char *sheet = o42_sheet_name_quote (o42_sheet_get_name (target));
      char *a = o42_ref_name_full (r.row0, r.col0, TRUE, TRUE);
      char *b = o42_ref_name_full (r.row1, r.col1, TRUE, TRUE);
      char *text = (r.row0 == r.row1 && r.col0 == r.col1)
                 ? g_strdup_printf ("=%s!%s", sheet, a)
                 : g_strdup_printf ("=%s!%s:%s", sheet, a, b);

      g_free (sheet); g_free (a); g_free (b);
      return text;
    }
  if (o42_book_lookup_name_formula (self->book, name) != NULL)
    return g_strconcat ("=", o42_book_lookup_name_formula (self->book, name), NULL);
  return g_strdup ("");
}

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkStringList *names;
  GtkSelectionModel *selection;
} PasteNamePrompt;

static void
on_paste_name_ok (GtkWidget *w, gpointer data)
{
  PasteNamePrompt *prompt = data;
  O42Window *self = prompt->window;
  guint pos = gtk_single_selection_get_selected (GTK_SINGLE_SELECTION (prompt->selection));
  const char *name;

  (void) w;
  if (pos == GTK_INVALID_LIST_POSITION)
    return;
  name = gtk_string_list_get_string (prompt->names, pos);

  if (o42_grid_is_editing (self->grid))
    {
      /* Into the formula being typed, where the caret is: the formula
       * bar is the same edit as the cell. */
      GtkEditable *entry = GTK_EDITABLE (self->formula_entry);
      int at = gtk_editable_get_position (entry);

      gtk_editable_insert_text (entry, name, -1, &at);
      gtk_editable_set_position (entry, at);
    }
  else
    {
      char *text = g_strconcat ("=", name, NULL);
      o42_grid_begin_edit (self->grid, text);
      g_free (text);
    }
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

/* Excel's Paste List: every name and what it refers to, in two columns
 * from the active cell. */
static void
on_paste_name_list (GtkWidget *w, gpointer data)
{
  PasteNamePrompt *prompt = data;
  O42Window *self = prompt->window;
  guint n = g_list_model_get_n_items (G_LIST_MODEL (prompt->names));
  int row, col;

  (void) w;
  if (o42_grid_is_editing (self->grid))
    o42_grid_commit_edit (self->grid);
  o42_grid_get_active (self->grid, &row, &col);
  o42_sheet_begin_group (self->sheet);
  for (guint i = 0; i < n && row + (int) i < O42_MAX_ROWS; i++)
    {
      const char *name = gtk_string_list_get_string (prompt->names, i);
      char *refers = name_refers_to (self, name);

      o42_sheet_set_input (self->sheet, row + (int) i, col, name);
      /* As text: the list is for reading, not for working out. */
      if (col + 1 < O42_MAX_COLS)
        {
          char *quoted = g_strconcat ("'", refers, NULL);
          o42_sheet_set_input (self->sheet, row + (int) i, col + 1, quoted);
          g_free (quoted);
        }
      g_free (refers);
    }
  o42_sheet_end_group (self->sheet);
  o42_grid_refresh (self->grid);
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_paste_name (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  PasteNamePrompt *prompt = g_new0 (PasteNamePrompt, 1);
  GtkWidget *content, *buttons, *ok;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Paste Name"), TRUE, &content, &buttons);
  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Paste name:")));
  gtk_box_append (GTK_BOX (content), names_list (self, &prompt->names, &prompt->selection, FALSE));

  dialog_button (buttons, _("Paste _List"), G_CALLBACK (on_paste_name_list), prompt);
  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_paste_name_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *edge[4];      /* top row, left column, bottom row, right column */
  O42Range   range;
} CreateNamesPrompt;

static void
on_create_names_ok (GtkWidget *w, gpointer data)
{
  CreateNamesPrompt *prompt = data;
  O42Window *self = prompt->window;
  gboolean on[4];

  (void) w;
  for (int i = 0; i < 4; i++)
    on[i] = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->edge[i]));
  o42_book_create_names (self->book, self->sheet, &prompt->range, on[0], on[1], on[2], on[3]);
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_create_names (GSimpleAction *a, GVariant *p, gpointer data)
{
  static const char *const EDGES[] = { N_("_Top row"), N_("_Left column"), N_("_Bottom row"), N_("_Right column") };
  O42Window *self = data;
  CreateNamesPrompt *prompt = g_new0 (CreateNamesPrompt, 1);
  GtkWidget *content, *buttons, *ok;
  O42Value v;

  (void) a; (void) p;

  prompt->window = self;
  o42_grid_get_selection (self->grid, &prompt->range);
  prompt->dialog = dialog_frame (self, _("Create Names"), TRUE, &content, &buttons);
  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Create names in")));
  for (int i = 0; i < 4; i++)
    {
      prompt->edge[i] = gtk_check_button_new_with_mnemonic (_(EDGES[i]));
      gtk_box_append (GTK_BOX (content), prompt->edge[i]);
    }

  /* Excel's guess: the top row when its first cell is text, the left
   * column when the cell under it is. */
  o42_sheet_get_value (self->sheet, prompt->range.row0, prompt->range.col0, &v);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->edge[0]), v.type == O42_VALUE_TEXT);
  o42_value_clear (&v);
  if (prompt->range.row1 > prompt->range.row0)
    {
      o42_sheet_get_value (self->sheet, prompt->range.row0 + 1, prompt->range.col0, &v);
      gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->edge[1]), v.type == O42_VALUE_TEXT);
      o42_value_clear (&v);
    }

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_create_names_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkStringList *names;
  GtkSelectionModel *selection;
} ApplyNamesPrompt;

static void
on_apply_names_ok (GtkWidget *w, gpointer data)
{
  ApplyNamesPrompt *prompt = data;
  O42Window *self = prompt->window;
  guint n = g_list_model_get_n_items (G_LIST_MODEL (prompt->names));
  GPtrArray *chosen = g_ptr_array_new_with_free_func (g_free);
  O42Range sel;

  (void) w;
  for (guint i = 0; i < n; i++)
    if (gtk_selection_model_is_selected (prompt->selection, i))
      g_ptr_array_add (chosen, g_strdup (gtk_string_list_get_string (prompt->names, i)));
  g_ptr_array_add (chosen, NULL);

  if (o42_grid_is_editing (self->grid))
    o42_grid_commit_edit (self->grid);
  /* One cell selected means the whole sheet, as Excel takes it. */
  o42_grid_get_selection (self->grid, &sel);
  o42_sheet_apply_names (self->sheet,
                         (sel.row0 == sel.row1 && sel.col0 == sel.col1) ? NULL : &sel,
                         (const char *const *) chosen->pdata);
  g_ptr_array_unref (chosen);
  o42_grid_refresh (self->grid);
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_apply_names (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  ApplyNamesPrompt *prompt = g_new0 (ApplyNamesPrompt, 1);
  GtkWidget *content, *buttons, *ok;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Apply Names"), TRUE, &content, &buttons);
  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Apply names:")));
  gtk_box_append (GTK_BOX (content), names_list (self, &prompt->names, &prompt->selection, TRUE));
  gtk_selection_model_select_all (prompt->selection);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_apply_names_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}
