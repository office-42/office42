/* o42-dialogs-data.c - see o42-window-private.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-window-private.h"

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
#define page_grid o42_page_grid
#define rgba_from_colour o42_rgba_from_colour
#define show_error o42_window_show_error
#define window_sync o42_window_sync
#define window_show_sheet o42_window_show_sheet
#define window_tell_book o42_window_tell_book
#define wizard_bind_item o42_wizard_bind_item
#define wizard_setup_item o42_wizard_setup_item

/* ---- Sort -------------------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *key_drop[3];
  GtkWidget *ascending[3];
  GtkWidget *header;
  O42Range   range;
} SortPrompt;

static void
on_sort_ok (GtkWidget *w, gpointer data)
{
  SortPrompt *prompt = data;
  int keys[3];
  gboolean asc[3];
  int n = 0;

  (void) w;

  /* The first dropdown is a column; the others have "(none)" first. */
  for (int k = 0; k < 3; k++)
    {
      guint index = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->key_drop[k]));

      if (index == GTK_INVALID_LIST_POSITION)
        continue;
      if (k > 0)
        {
          if (index == 0)
            continue;
          index--;
        }
      keys[n] = prompt->range.col0 + (int) index;
      asc[n] = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->ascending[k]));
      n++;
    }

  if (n > 0)
    {
      o42_sheet_sort_keys (prompt->window->sheet, &prompt->range, keys, asc, n,
                           gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->header)));
      o42_grid_refresh (prompt->window->grid);
      window_sync (prompt->window);
    }

  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_sort (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  SortPrompt *prompt = g_new0 (SortPrompt, 1);
  GtkWidget *content, *buttons, *ok;
  GtkStringList *columns = gtk_string_list_new (NULL);
  GtkStringList *columns_none = gtk_string_list_new (NULL);
  O42Range used;
  static const char *const titles[3] = { N_("Sort by:"), N_("Then by:"), N_("Then by:") };

  (void) a; (void) p;

  prompt->window = self;
  o42_grid_get_selection (self->grid, &prompt->range);

  /* A single cell means the block of data around it, as Excel guesses:
   * here, the used range. */
  if (prompt->range.row0 == prompt->range.row1 &&
      prompt->range.col0 == prompt->range.col1)
    {
      o42_sheet_used_range (self->sheet, &used);
      prompt->range = used;
    }

  for (int col = prompt->range.col0; col <= prompt->range.col1; col++)
    {
      char name[8], label[40];
      char *heading = o42_sheet_get_display (self->sheet, prompt->range.row0, col);

      o42_col_name (col, name, sizeof name);
      if (*heading != '\0')
        g_snprintf (label, sizeof label, "%s (%s)", heading, name);
      else
        /* Translators: %s is a column's letter, as in "Column C". */
        g_snprintf (label, sizeof label, _("Column %s"), name);
      gtk_string_list_append (columns, label);
      gtk_string_list_append (columns_none, label);
      g_free (heading);
    }
  gtk_string_list_splice (columns_none, 0, 0, (const char *[]) { _("(none)"), NULL });

  prompt->dialog = dialog_frame (self, _("Sort"), TRUE, &content, &buttons);

  for (int k = 0; k < 3; k++)
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      GtkWidget *descending;

      gtk_box_append (GTK_BOX (row), gtk_label_new (_(titles[k])));
      prompt->key_drop[k] = gtk_drop_down_new (
        G_LIST_MODEL (g_object_ref (k == 0 ? columns : columns_none)), NULL);
      gtk_widget_set_size_request (prompt->key_drop[k], 180, -1);
      gtk_box_append (GTK_BOX (row), prompt->key_drop[k]);

      prompt->ascending[k] = gtk_check_button_new_with_label (_("Ascending"));
      descending = gtk_check_button_new_with_label (_("Descending"));
      gtk_check_button_set_group (GTK_CHECK_BUTTON (descending), GTK_CHECK_BUTTON (prompt->ascending[k]));
      gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->ascending[k]), TRUE);
      gtk_box_append (GTK_BOX (row), prompt->ascending[k]);
      gtk_box_append (GTK_BOX (row), descending);
      gtk_box_append (GTK_BOX (content), row);
    }
  g_object_unref (columns);
  g_object_unref (columns_none);

  prompt->header = gtk_check_button_new_with_mnemonic ( _("My list has a _header row"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->header), TRUE);
  gtk_box_append (GTK_BOX (content), prompt->header);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_sort_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
}


/* A drop-down over a list of strings the program owns: the widget
 * takes the strings as they are, so the catalogue is asked before the
 * list is built.  The strings themselves are marked with N_() where
 * they stand, which is what puts them in the catalogue. */
GtkWidget *
o42_drop_down_of (const char *const *names)
{
  gsize n = 0;
  const char **strings;
  GtkWidget *drop;

  while (names[n] != NULL)
    n++;
  strings = g_new0 (const char *, n + 1);
  for (gsize i = 0; i < n; i++)
    strings[i] = _(names[i]);
  drop = gtk_drop_down_new_from_strings (strings);
  g_free (strings);
  return drop;
}

static const char *PIVOT_AGGS[] = { N_("Sum"), N_("Count"), N_("Average"), N_("Min"), N_("Max"), NULL };

/* ---- Data > Advanced Filter --------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *list, *criteria, *dest, *unique;
} AdvFilterPrompt;

/* "A1:C9" into a range; FALSE if it is not one. */
static gboolean
parse_range_text (const char *text, O42Range *out)
{
  gsize len = 0;

  if (text == NULL || !o42_ref_parse (text, &out->row0, &out->col0, &len))
    return FALSE;
  if (text[len] == '\0')
    { out->row1 = out->row0; out->col1 = out->col0; return TRUE; }
  return text[len] == ':' && o42_ref_parse (text + len + 1, &out->row1, &out->col1, NULL);
}

static void
on_adv_filter_ok (GtkWidget *w, gpointer data)
{
  AdvFilterPrompt *prompt = data;
  O42Range list, criteria, dest;
  const char *dest_text = gtk_editable_get_text (GTK_EDITABLE (prompt->dest));
  int drow = -1, dcol = 0;

  (void) w;
  if (!parse_range_text (gtk_editable_get_text (GTK_EDITABLE (prompt->list)), &list))
    { gtk_widget_grab_focus (prompt->list); return; }
  if (!parse_range_text (gtk_editable_get_text (GTK_EDITABLE (prompt->criteria)), &criteria))
    { gtk_widget_grab_focus (prompt->criteria); return; }
  if (*dest_text != '\0')
    {
      if (!parse_range_text (dest_text, &dest))
        { gtk_widget_grab_focus (prompt->dest); return; }
      drow = dest.row0;
      dcol = dest.col0;
    }

  {
    int n = o42_sheet_advanced_filter (prompt->window->sheet, &list, &criteria, drow, dcol,
                                       gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->unique)));
    char *message = g_strdup_printf (ngettext ("%d row answered the criteria.",
                                               "%d rows answered the criteria.", n), n);
    gtk_label_set_text (GTK_LABEL (prompt->window->status_label), message);
    g_free (message);
  }
  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_advanced_filter (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  AdvFilterPrompt *prompt = g_new0 (AdvFilterPrompt, 1);
  GtkWidget *content, *buttons, *grid, *ok;
  O42Range sel;
  char *a1, *b1, *text;

  (void) a; (void) p;
  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Advanced Filter"), TRUE, &content, &buttons);

  o42_grid_get_selection (self->grid, &sel);
  if (sel.row0 == sel.row1 && sel.col0 == sel.col1)
    o42_sheet_used_range (self->sheet, &sel);
  a1 = o42_ref_name (sel.row0, sel.col0);
  b1 = o42_ref_name (sel.row1, sel.col1);
  text = g_strdup_printf ("%s:%s", a1, b1);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->list = labelled (grid, 0, _("List range:"), gtk_entry_new ());
  prompt->criteria = labelled (grid, 1, _("Criteria range:"), gtk_entry_new ());
  prompt->dest = labelled (grid, 2, _("Copy to:"), gtk_entry_new ());
  gtk_widget_set_size_request (prompt->list, 220, -1);
  gtk_editable_set_text (GTK_EDITABLE (prompt->list), text);
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->criteria), _("E1:F3"));
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->dest), _("empty: filter where it is"));
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->criteria), TRUE);
  gtk_box_append (GTK_BOX (content), grid);
  g_free (text); g_free (a1); g_free (b1);

  prompt->unique = gtk_check_button_new_with_mnemonic ( _("_Unique rows only"));
  gtk_box_append (GTK_BOX (content), prompt->unique);
  {
    /* Translators: the quoted conditions are what may be typed into the
       criteria range; keep the >, <> and * in them. */
    GtkWidget *hint = gtk_label_new (_("The criteria range names fields in its first row; each row after it "
                                       "is a set of conditions that must all hold, and any one row is enough. "
                                       "A condition is \">5\", \"<>Japan\", \"*land\" or a value to equal."));
    gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (hint), 46);
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_box_append (GTK_BOX (content), hint);
  }

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_adv_filter_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->criteria);
}

/* ---- Data > Consolidate ------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *ranges;
  GtkWidget *function;
  GtkWidget *labels;
} ConsolidatePrompt;

static void
on_consolidate_ok (GtkWidget *w, gpointer data)
{
  ConsolidatePrompt *prompt = data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (prompt->ranges));
  char **specs = g_strsplit_set (text, ",;", -1);
  O42SheetRange sources[16];
  int n_sources = 0;
  int row, col;
  guint chosen = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->function));
  gboolean labels = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->labels));

  (void) w;
  for (int i = 0; specs[i] != NULL && n_sources < 16; i++)
    {
      char *spec = g_strstrip (specs[i]);
      char *bang = strrchr (spec, '!');
      gsize len = 0;

      if (*spec == '\0')
        continue;
      memset (&sources[n_sources], 0, sizeof sources[0]);
      if (bang != NULL)
        {
          char *name = g_strndup (spec, (gsize) (bang - spec));
          char *clean = name;
          gsize nlen = strlen (clean);
          if (nlen >= 2 && clean[0] == '\'' && clean[nlen - 1] == '\'')
            { clean[nlen - 1] = '\0'; clean++; }
          sources[n_sources].sheet = g_intern_string (clean);
          g_free (name);
          spec = bang + 1;
        }
      if (o42_ref_parse (spec, &sources[n_sources].range.row0, &sources[n_sources].range.col0, &len) &&
          spec[len] == ':' &&
          o42_ref_parse (spec + len + 1, &sources[n_sources].range.row1, &sources[n_sources].range.col1, NULL))
        n_sources++;
    }
  g_strfreev (specs);

  if (n_sources == 0)
    {
      gtk_widget_grab_focus (prompt->ranges);
      return;
    }
  o42_grid_get_active (prompt->window->grid, &row, &col);
  {
    O42Range made = o42_sheet_consolidate (prompt->window->sheet, sources, n_sources, row, col,
                                           (O42PivotAgg) (chosen == GTK_INVALID_LIST_POSITION ? 0 : chosen),
                                           labels, labels);
    o42_grid_select_range (prompt->window->grid, &made);
  }
  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_consolidate (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  ConsolidatePrompt *prompt = g_new0 (ConsolidatePrompt, 1);
  GtkWidget *content, *buttons, *grid, *ok;
  int row, col;
  char *at, *where;

  (void) a; (void) p;
  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Consolidate"), TRUE, &content, &buttons);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->function = labelled (grid, 0, _("Function:"), drop_down_of (PIVOT_AGGS));
  prompt->ranges = labelled (grid, 1, _("Ranges:"), gtk_entry_new ());
  gtk_widget_set_size_request (prompt->ranges, 320, -1);
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->ranges), _("A1:C4, Sheet2!A1:C4"));
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->ranges), TRUE);
  gtk_box_append (GTK_BOX (content), grid);

  prompt->labels = gtk_check_button_new_with_mnemonic ( _("The top row and left column are _labels"));
  gtk_box_append (GTK_BOX (content), prompt->labels);

  o42_grid_get_active (self->grid, &row, &col);
  at = o42_ref_name (row, col);
  /* Translators: %s is the cell the result starts at, such as B2. */
  where = g_strdup_printf (_("The result goes at %s. Without labels the cells are matched by position."), at);
  {
    GtkWidget *hint = gtk_label_new (where);
    gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (hint), 44);
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_box_append (GTK_BOX (content), hint);
  }
  g_free (where); g_free (at);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_consolidate_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->ranges);
}

/* ---- Data > Scenarios --------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *list;
  GtkWidget *name;
  GtkWidget *comment;
} ScenarioPrompt;

static void
scenario_prompt_fill (ScenarioPrompt *prompt, const char *select)
{
  GtkWidget *child;
  int n = o42_sheet_n_scenarios (prompt->window->sheet);

  while ((child = gtk_widget_get_first_child (prompt->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (prompt->list), child);
  for (int i = 0; i < n; i++)
    {
      const char *name = o42_sheet_scenario_name (prompt->window->sheet, i);
      GArray *keys = NULL;
      const char *comment = NULL;
      char *head, *text;
      guint n_cells;
      GtkWidget *label;

      o42_sheet_scenario_cells (prompt->window->sheet, name, &keys, NULL, &comment);
      n_cells = keys != NULL ? keys->len : 0;
      /* Translators: a scenario in the list: its name, then how many
         cells it sets. */
      head = g_strdup_printf (ngettext ("%s  (%u cell)", "%s  (%u cells)", n_cells), name, n_cells);
      text = comment != NULL && *comment != '\0' ? g_strdup_printf ("%s -- %s", head, comment)
                                                 : g_strdup (head);
      g_free (head);
      label = gtk_label_new (text);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_widget_set_margin_start (label, 6);
      gtk_widget_set_margin_end (label, 6);
      gtk_list_box_append (GTK_LIST_BOX (prompt->list), label);
      if (select != NULL && g_ascii_strcasecmp (name, select) == 0)
        gtk_list_box_select_row (GTK_LIST_BOX (prompt->list),
                                 gtk_list_box_get_row_at_index (GTK_LIST_BOX (prompt->list), i));
      g_free (text);
    }
}

static char *
scenario_selected (ScenarioPrompt *prompt)
{
  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (prompt->list));
  if (row == NULL)
    return NULL;
  return g_strdup (o42_sheet_scenario_name (prompt->window->sheet, gtk_list_box_row_get_index (row)));
}

static void
on_scenario_show (GtkWidget *w, gpointer data)
{
  ScenarioPrompt *prompt = data;
  char *name = scenario_selected (prompt);

  (void) w;
  if (name != NULL && o42_sheet_show_scenario (prompt->window->sheet, name))
    {
      o42_grid_refresh (prompt->window->grid);
      window_sync (prompt->window);
    }
  g_free (name);
}

static void
on_scenario_add (GtkWidget *w, gpointer data)
{
  ScenarioPrompt *prompt = data;
  const char *name = gtk_editable_get_text (GTK_EDITABLE (prompt->name));
  O42Range sel;

  (void) w;
  if (*name == '\0')
    {
      gtk_widget_grab_focus (prompt->name);
      return;
    }
  o42_grid_get_selection (prompt->window->grid, &sel);
  o42_sheet_add_scenario (prompt->window->sheet, name, &sel,
                          gtk_editable_get_text (GTK_EDITABLE (prompt->comment)));
  scenario_prompt_fill (prompt, name);
  window_sync (prompt->window);
}

static void
on_scenario_delete (GtkWidget *w, gpointer data)
{
  ScenarioPrompt *prompt = data;
  char *name = scenario_selected (prompt);

  (void) w;
  if (name != NULL && o42_sheet_remove_scenario (prompt->window->sheet, name))
    scenario_prompt_fill (prompt, NULL);
  g_free (name);
  window_sync (prompt->window);
}

/* Summary asks which cells are the results, then writes the report on
 * a sheet of its own and shows it. */
typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *cells;
} SummaryPrompt;

static void
on_summary_ok (GtkWidget *w, gpointer data)
{
  SummaryPrompt *prompt = data;
  O42Window *self = prompt->window;
  GArray *results = g_array_new (FALSE, FALSE, sizeof (guint64));
  char **parts = g_strsplit_set (gtk_editable_get_text (GTK_EDITABLE (prompt->cells)), ",; ", -1);
  O42Sheet *made;

  (void) w;
  for (int i = 0; parts[i] != NULL; i++)
    {
      O42Range r;
      gsize len = 0;
      const char *part = g_strstrip (parts[i]);

      if (*part == '\0' || !o42_ref_parse (part, &r.row0, &r.col0, &len))
        continue;
      r.row1 = r.row0; r.col1 = r.col0;
      if (part[len] == ':')
        o42_ref_parse (part + len + 1, &r.row1, &r.col1, NULL);
      r = o42_range_normalise (r.row0, r.col0, r.row1, r.col1);
      for (int row = r.row0; row <= r.row1 && row < r.row0 + 100; row++)
        for (int col = r.col0; col <= r.col1 && col < r.col0 + 100; col++)
          {
            guint64 key = o42_key (row, col);
            g_array_append_val (results, key);
          }
    }
  g_strfreev (parts);
  made = o42_sheet_scenario_summary (self->sheet, results);
  g_array_unref (results);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
  if (made != NULL)
    {
      window_show_sheet (self, o42_book_sheet_index (self->book, made));
      window_sync (self);
    }
}

static void
on_scenario_summary (GtkWidget *w, gpointer data)
{
  ScenarioPrompt *scenarios = data;
  O42Window *self = scenarios->window;
  SummaryPrompt *prompt;
  GtkWidget *content, *buttons, *grid, *ok;
  O42Range sel;

  (void) w;
  if (o42_sheet_n_scenarios (self->sheet) == 0)
    return;
  prompt = g_new0 (SummaryPrompt, 1);
  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Scenario Summary"), TRUE, &content, &buttons);
  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->cells = labelled (grid, 0, _("Result cells:"), gtk_entry_new ());
  gtk_widget_set_size_request (prompt->cells, 220, -1);
  gtk_box_append (GTK_BOX (content), grid);
  o42_grid_get_selection (self->grid, &sel);
  {
    char *a1 = o42_ref_name (sel.row0, sel.col0);
    char *b1 = o42_ref_name (sel.row1, sel.col1);
    char *text = (sel.row0 == sel.row1 && sel.col0 == sel.col1) ? g_strdup (a1)
                                                                : g_strdup_printf ("%s:%s", a1, b1);
    gtk_editable_set_text (GTK_EDITABLE (prompt->cells), text);
    g_free (text); g_free (a1); g_free (b1);
  }
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->cells), TRUE);
  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_summary_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

void
action_scenarios (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  ScenarioPrompt *prompt = g_new0 (ScenarioPrompt, 1);
  GtkWidget *content, *buttons, *scroller, *grid, *show;
  O42Range sel;
  char *a1, *b1, *range, *where;

  (void) a; (void) p;
  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Scenarios"), TRUE, &content, &buttons);
  gtk_window_set_default_size (GTK_WINDOW (prompt->dialog), 420, 360);

  prompt->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (prompt->list), GTK_SELECTION_SINGLE);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), prompt->list);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_widget_add_css_class (scroller, "frame");
  gtk_box_append (GTK_BOX (content), scroller);

  o42_grid_get_selection (self->grid, &sel);
  a1 = o42_ref_name (sel.row0, sel.col0);
  b1 = o42_ref_name (sel.row1, sel.col1);
  range = g_strdup_printf ("%s:%s", a1, b1);
  /* Translators: "Add" is the dialog's _Add button; %s is the selected
     range, such as A1:B4. */
  where = g_strdup_printf (_("Add takes the values of %s as they are now."), range);
  g_free (range);
  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->name = labelled (grid, 0, _("Name:"), gtk_entry_new ());
  prompt->comment = labelled (grid, 1, _("Comment:"), gtk_entry_new ());
  gtk_widget_set_hexpand (prompt->name, TRUE);
  gtk_box_append (GTK_BOX (content), grid);
  {
    GtkWidget *hint = gtk_label_new (where);
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_box_append (GTK_BOX (content), hint);
  }
  g_free (where); g_free (a1); g_free (b1);

  scenario_prompt_fill (prompt, NULL);

  show = dialog_button (buttons, _("_Show"), G_CALLBACK (on_scenario_show), prompt);
  dialog_button (buttons, _("_Add"), G_CALLBACK (on_scenario_add), prompt);
  dialog_button (buttons, _("_Delete"), G_CALLBACK (on_scenario_delete), prompt);
  dialog_button (buttons, _("S_ummary..."), G_CALLBACK (on_scenario_summary), prompt);
  dialog_button (buttons, _("_Close"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), show);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Data > Table ------------------------------------------------------ */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *name, *headers, *banded;
  O42Range   range;
} TablePrompt2;

static void
on_table_ok (GtkWidget *w, gpointer data)
{
  TablePrompt2 *prompt = data;
  O42Sheet *sheet = prompt->window->sheet;
  const char *name = gtk_editable_get_text (GTK_EDITABLE (prompt->name));
  gboolean headers = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->headers));
  gboolean banded = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->banded));

  (void) w;
  if (*name == '\0')
    {
      gtk_widget_grab_focus (prompt->name);
      return;
    }

  o42_sheet_begin_group (sheet);
  o42_sheet_add_table (sheet, name, &prompt->range, headers);
  if (banded)
    {
      /* What "format as table" means: a heading row in bold on a
       * shaded band, and every other row lightly shaded. */
      O42Fmt fmt;
      O42Range row = prompt->range;

      if (headers)
        {
          o42_fmt_init_default (&fmt);
          fmt.bold = 1;
          fmt.fill = 0xDCE6F1;
          row.row1 = row.row0;
          o42_sheet_apply_fmt (sheet, &row, O42_FMT_BOLD | O42_FMT_FILL, &fmt);
        }
      for (int r = prompt->range.row0 + (headers ? 1 : 0); r <= prompt->range.row1; r++)
        {
          int index = r - prompt->range.row0 - (headers ? 1 : 0);
          O42Range one = { r, prompt->range.col0, r, prompt->range.col1 };

          o42_fmt_init_default (&fmt);
          fmt.fill = (index % 2 == 0) ? 0xFFFFFF : 0xF2F6FB;
          o42_sheet_apply_fmt (sheet, &one, O42_FMT_FILL, &fmt);
        }
    }
  o42_sheet_end_group (sheet);
  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_table (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  TablePrompt2 *prompt = g_new0 (TablePrompt2, 1);
  GtkWidget *content, *buttons, *ok, *row;
  O42Table *existing;
  char *suggestion;
  char *a1, *b1, *range, *where;

  (void) a; (void) p;
  prompt->window = self;
  o42_grid_get_selection (self->grid, &prompt->range);
  if (prompt->range.row0 == prompt->range.row1 && prompt->range.col0 == prompt->range.col1)
    o42_sheet_used_range (self->sheet, &prompt->range);

  existing = o42_sheet_table_at (self->sheet, prompt->range.row0, prompt->range.col0);
  suggestion = existing != NULL ? g_strdup (existing->name)
                                : g_strdup_printf ("Table%d", o42_sheet_tables (self->sheet)->len + 1);
  if (existing != NULL)
    prompt->range = existing->range;

  prompt->dialog = dialog_frame (self, _("Table"), TRUE, &content, &buttons);
  a1 = o42_ref_name (prompt->range.row0, prompt->range.col0);
  b1 = o42_ref_name (prompt->range.row1, prompt->range.col1);
  range = g_strdup_printf ("%s:%s", a1, b1);
  /* Translators: %s is the table's range, such as A1:D20. */
  where = g_strdup_printf (_("Table over %s"), range);
  gtk_box_append (GTK_BOX (content), gtk_label_new (where));
  g_free (where); g_free (range); g_free (a1); g_free (b1);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Name:")));
  prompt->name = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (prompt->name), suggestion);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->name), TRUE);
  gtk_widget_set_hexpand (prompt->name, TRUE);
  gtk_box_append (GTK_BOX (row), prompt->name);
  gtk_box_append (GTK_BOX (content), row);
  g_free (suggestion);

  prompt->headers = gtk_check_button_new_with_mnemonic ( _("My table has _headers"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->headers), existing == NULL || existing->has_headers);
  gtk_box_append (GTK_BOX (content), prompt->headers);
  prompt->banded = gtk_check_button_new_with_mnemonic ( _("_Shade the heading row and every other row"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->banded), existing == NULL);
  gtk_box_append (GTK_BOX (content), prompt->banded);

  {
    GtkWidget *hint = gtk_label_new (_("Formulas may then name its parts: Table1[Sales], Table1[#Headers], Table1[@Sales]."));
    gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (hint), 46);
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_box_append (GTK_BOX (content), hint);
  }

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_table_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Data > Subtotals, Data > Remove Duplicates ------------------------ */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *group_drop;      /* subtotals: "At each change in" */
  GtkWidget *function_drop;   /* subtotals: "Use function" */
  GtkWidget *header;
  GtkWidget *replace;         /* subtotals only */
  GPtrArray *checks;          /* one check button per column */
  O42Range   range;
} TablePrompt;

/* The selection, or the used range around a single cell, with the
 * column headings as labels. */
static void
table_prompt_columns (O42Window *self, TablePrompt *prompt, GtkStringList *labels)
{
  O42Range used;

  o42_grid_get_selection (self->grid, &prompt->range);
  if (prompt->range.row0 == prompt->range.row1 && prompt->range.col0 == prompt->range.col1)
    {
      o42_sheet_used_range (self->sheet, &used);
      prompt->range = used;
    }
  for (int col = prompt->range.col0; col <= prompt->range.col1; col++)
    {
      char name[8], label[40];
      char *heading = o42_sheet_get_display (self->sheet, prompt->range.row0, col);

      o42_col_name (col, name, sizeof name);
      if (*heading != '\0')
        g_snprintf (label, sizeof label, "%s (%s)", heading, name);
      else
        /* Translators: %s is a column's letter, as in "Column C". */
        g_snprintf (label, sizeof label, _("Column %s"), name);
      gtk_string_list_append (labels, label);
      g_free (heading);
    }
}

static int
table_prompt_checked (TablePrompt *prompt, int *cols)
{
  int n = 0;
  for (guint i = 0; i < prompt->checks->len; i++)
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (g_ptr_array_index (prompt->checks, i))))
      cols[n++] = prompt->range.col0 + (int) i;
  return n;
}

static void
on_table_prompt_destroy (GtkWidget *w, gpointer data)
{
  TablePrompt *prompt = data;
  (void) w;
  g_ptr_array_unref (prompt->checks);
  g_free (prompt);
}

static GtkWidget *
table_prompt_checklist (TablePrompt *prompt, GtkStringList *labels, const char *title, gboolean all)
{
  GtkWidget *frame = gtk_frame_new (title);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroller), 180);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroller), TRUE);
  for (guint i = 0; i < g_list_model_get_n_items (G_LIST_MODEL (labels)); i++)
    {
      GtkWidget *check = gtk_check_button_new_with_label (gtk_string_list_get_string (labels, i));
      gtk_check_button_set_active (GTK_CHECK_BUTTON (check), all);
      gtk_box_append (GTK_BOX (box), check);
      g_ptr_array_add (prompt->checks, check);
    }
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box);
  gtk_frame_set_child (GTK_FRAME (frame), scroller);
  return frame;
}

static void
on_remove_duplicates_ok (GtkWidget *w, gpointer data)
{
  TablePrompt *prompt = data;
  int cols[O42_MAX_COLS];
  int n = table_prompt_checked (prompt, cols);
  (void) w;

  if (n > 0)
    {
      int removed = o42_sheet_remove_duplicates (prompt->window->sheet, &prompt->range, cols, n,
                                                 gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->header)));
      char *message = g_strdup_printf (ngettext ("%d duplicate row removed.",
                                                 "%d duplicate rows removed.", removed), removed);
      o42_grid_refresh (prompt->window->grid);
      window_sync (prompt->window);
      gtk_label_set_text (GTK_LABEL (prompt->window->status_label), message);
      g_free (message);
    }
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_remove_duplicates (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  TablePrompt *prompt = g_new0 (TablePrompt, 1);
  GtkStringList *labels = gtk_string_list_new (NULL);
  GtkWidget *content, *buttons, *ok;

  (void) a; (void) p;
  prompt->window = self;
  prompt->checks = g_ptr_array_new ();
  table_prompt_columns (self, prompt, labels);
  prompt->dialog = dialog_frame (self, _("Remove Duplicates"), TRUE, &content, &buttons);
  gtk_box_append (GTK_BOX (content), table_prompt_checklist (prompt, labels, _("Columns that must match"), TRUE));
  prompt->header = gtk_check_button_new_with_mnemonic ( _("My list has a _header row"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->header), TRUE);
  gtk_box_append (GTK_BOX (content), prompt->header);
  g_object_unref (labels);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_remove_duplicates_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_table_prompt_destroy), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* The drop-down's choices, and the SUBTOTAL function number each stands
 * for: the choice is read back by its place in the list. */
static const char *const SUBTOTAL_NAMES[] = {
  N_("Sum"), N_("Count"), N_("Average"), N_("Max"), N_("Min"), N_("Product"), NULL
};
static const int SUBTOTAL_FUNCTIONS[] = { 9, 3, 1, 4, 5, 6 };

static void
on_subtotals_ok (GtkWidget *w, gpointer data)
{
  TablePrompt *prompt = data;
  int cols[O42_MAX_COLS];
  int n = table_prompt_checked (prompt, cols);
  guint g = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->group_drop));
  guint f = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->function_drop));
  (void) w;

  if (n > 0 && g != GTK_INVALID_LIST_POSITION)
    {
      O42Range out = o42_sheet_subtotal (prompt->window->sheet, &prompt->range, prompt->range.col0 + (int) g, cols, n,
                                         f < G_N_ELEMENTS (SUBTOTAL_FUNCTIONS) ? SUBTOTAL_FUNCTIONS[f] : 9,
                                         gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->header)),
                                         gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->replace)));
      o42_grid_select_range (prompt->window->grid, &out);
      o42_grid_refresh (prompt->window->grid);
      window_sync (prompt->window);
    }
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_subtotals_remove_all (GtkWidget *w, gpointer data)
{
  TablePrompt *prompt = data;
  (void) w;
  o42_sheet_remove_subtotals (prompt->window->sheet, &prompt->range);
  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_subtotals (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  TablePrompt *prompt = g_new0 (TablePrompt, 1);
  GtkStringList *labels = gtk_string_list_new (NULL);
  GtkWidget *content, *buttons, *ok, *grid;

  (void) a; (void) p;
  prompt->window = self;
  prompt->checks = g_ptr_array_new ();
  table_prompt_columns (self, prompt, labels);
  prompt->dialog = dialog_frame (self, _("Subtotals"), TRUE, &content, &buttons);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->group_drop = labelled (grid, 0, _("At each change in:"), gtk_drop_down_new (G_LIST_MODEL (g_object_ref (labels)), NULL));
  prompt->function_drop = labelled (grid, 1, _("Use function:"), drop_down_of (SUBTOTAL_NAMES));
  gtk_box_append (GTK_BOX (content), grid);
  gtk_box_append (GTK_BOX (content), table_prompt_checklist (prompt, labels, _("Add subtotal to"), FALSE));
  if (prompt->checks->len > 1)
    gtk_check_button_set_active (GTK_CHECK_BUTTON (g_ptr_array_index (prompt->checks, prompt->checks->len - 1)), TRUE);
  prompt->header = gtk_check_button_new_with_mnemonic ( _("My list has a _header row"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->header), TRUE);
  gtk_box_append (GTK_BOX (content), prompt->header);
  prompt->replace = gtk_check_button_new_with_mnemonic ( _("_Replace current subtotals"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->replace), TRUE);
  gtk_box_append (GTK_BOX (content), prompt->replace);
  g_object_unref (labels);

  dialog_button (buttons, _("Remove _All"), G_CALLBACK (on_subtotals_remove_all), prompt);
  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_subtotals_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_table_prompt_destroy), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Data > Pivot Table ---------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *source, *row_field, *row_field2, *col_field, *col_field2, *data_field, *agg;
  GtkWidget *calc, *filter_field, *filter_value;
  GtkWidget *agg2, *data_field2, *data_on_rows;      /* a second data field */
  GtkWidget *group_field, *group_kind, *bucket_start, *bucket_size, *manual_groups;
  GtkWidget *subtotals, *grand_rows, *grand_cols;
  GStrv      fields;
} PivotPrompt;

/* How a field may be grouped, in the drop-down's order; the specs are
 * what O42Pivot.groups spells.  The choice is read back by its place. */
static const char *const PIVOT_GROUP_KINDS[] = {
  N_("(not grouped)"), N_("Years"), N_("Years and quarters"), N_("Years, quarters and months"),
  N_("Quarters"), N_("Months"), N_("Days"), N_("Number buckets"), N_("Named groups"), NULL
};
static const char *const PIVOT_GROUP_SPECS[] = { "", "y", "y,q", "y,q,m", "q", "m", "d", "n", "g" };



static void
on_pivot_ok (GtkWidget *w, gpointer data)
{
  PivotPrompt *prompt = data;
  O42Window *self = prompt->window;
  O42Pivot p;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (prompt->source));
  gsize len = 0;
  guint ri = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->row_field));
  guint ri2 = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->row_field2));
  guint ci = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->col_field));
  guint ci2 = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->col_field2));
  guint di = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->data_field));
  guint n = g_strv_length (prompt->fields);
  O42Sheet *dest;
  GPtrArray *rows = g_ptr_array_new (), *cols = g_ptr_array_new ();
  char *calc_text = NULL;

  (void) w;
  memset (&p, 0, sizeof p);
  if (!(o42_ref_parse (text, &p.source.row0, &p.source.col0, &len) && text[len] == ':' &&
        o42_ref_parse (text + len + 1, &p.source.row1, &p.source.col1, NULL)) || n == 0)
    return;
  p.source = o42_range_normalise (p.source.row0, p.source.col0, p.source.row1, p.source.col1);
  p.source_sheet = (char *) o42_sheet_get_name (self->sheet);
  g_ptr_array_add (rows, prompt->fields[MIN (ri, n - 1)]);
  if (ri2 > 0) g_ptr_array_add (rows, prompt->fields[MIN (ri2 - 1, n - 1)]);
  g_ptr_array_add (rows, NULL);
  if (ci > 0) g_ptr_array_add (cols, prompt->fields[MIN (ci - 1, n - 1)]);
  if (ci2 > 0) g_ptr_array_add (cols, prompt->fields[MIN (ci2 - 1, n - 1)]);
  g_ptr_array_add (cols, NULL);
  p.row_fields = (char **) rows->pdata;
  p.col_fields = (char **) cols->pdata;
  p.data_field = prompt->fields[MIN (di, n - 1)];
  p.agg = (O42PivotAgg) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->agg));
  {
    /* A calculated field takes the place of the data field; a filter
     * keeps only the rows whose field shows the value. */
    const char *calc = gtk_editable_get_text (GTK_EDITABLE (prompt->calc));
    guint fi = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->filter_field));
    calc_text = *calc != '\0' ? g_strconcat (calc[0] == '=' ? "" : "=", calc, NULL) : NULL;
    if (calc_text != NULL)
      p.data_field = calc_text;
    if (fi > 0 && fi != GTK_INVALID_LIST_POSITION)
      {
        p.filter_field = prompt->fields[MIN (fi - 1, n - 1)];
        p.filter_value = (char *) gtk_editable_get_text (GTK_EDITABLE (prompt->filter_value));
      }
  }
  {
    /* The further parts: a second data field, a grouping, the totals. */
    guint di2 = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->data_field2));
    guint gf = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->group_field));
    guint gk = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->group_kind));
    GString *groups = g_string_new (NULL);

    if (di2 > 0 && di2 != GTK_INVALID_LIST_POSITION)
      {
        char *spec = g_strdup_printf ("%s:%s", PIVOT_AGGS[gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->agg2))],
                                      prompt->fields[MIN (di2 - 1, n - 1)]);
        p.data_fields = g_new0 (char *, 2);
        p.data_fields[0] = spec;
      }
    p.data_on_rows = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->data_on_rows));
    if (gf > 0 && gf != GTK_INVALID_LIST_POSITION && gk > 0 && gk < G_N_ELEMENTS (PIVOT_GROUP_SPECS))
      {
        const char *field = prompt->fields[MIN (gf - 1, n - 1)];
        if (strcmp (PIVOT_GROUP_SPECS[gk], "n") == 0)
          {
            char a[G_ASCII_DTOSTR_BUF_SIZE], b[G_ASCII_DTOSTR_BUF_SIZE];
            g_string_append_printf (groups, "%s=n,%s,%s", field,
                                    g_ascii_formatd (a, sizeof a, "%g", gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->bucket_start))),
                                    g_ascii_formatd (b, sizeof b, "%g", gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->bucket_size))));
          }
        else if (strcmp (PIVOT_GROUP_SPECS[gk], "g") == 0)
          g_string_append_printf (groups, "%s=g,%s", field, gtk_editable_get_text (GTK_EDITABLE (prompt->manual_groups)));
        else
          g_string_append_printf (groups, "%s=%s", field, PIVOT_GROUP_SPECS[gk]);
      }
    p.groups = g_string_free (groups, FALSE);
    p.subtotals = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->subtotals));
    p.no_grand_rows = !gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->grand_rows));
    p.no_grand_cols = !gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->grand_cols));
  }

  dest = o42_book_add_sheet (self->book, NULL, -1);
  o42_sheet_add_pivot (dest, &p);
  g_strfreev (p.data_fields);
  g_free (p.groups);
  g_free (calc_text);
  g_ptr_array_free (rows, TRUE);
  g_ptr_array_free (cols, TRUE);
  window_tell_book (self, "sheets");
  window_show_sheet (self, o42_book_sheet_index (self->book, dest));
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_pivot_destroy (gpointer data)
{
  PivotPrompt *prompt = data;
  g_strfreev (prompt->fields);
  g_free (prompt);
}

void
action_pivot (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  PivotPrompt *prompt = g_new0 (PivotPrompt, 1);
  GtkWidget *content, *buttons, *row, *ok;
  O42Range sel;
  GPtrArray *fields = g_ptr_array_new ();
  GPtrArray *col_choices = g_ptr_array_new ();
  char *a1, *b1, *text;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Pivot Table"), TRUE, &content, &buttons);

  /* The selection is the source; its top row names the fields. */
  o42_grid_get_selection (self->grid, &sel);
  if (sel.row0 == sel.row1 && sel.col0 == sel.col1)
    o42_sheet_used_range (self->sheet, &sel);
  for (int col = sel.col0; col <= sel.col1; col++)
    {
      char *head = o42_sheet_get_display (self->sheet, sel.row0, col);
      if (head[0] == '\0') { g_free (head); head = o42_ref_name (sel.row0, col); }
      g_ptr_array_add (fields, head);
    }
  g_ptr_array_add (fields, NULL);
  prompt->fields = (GStrv) g_ptr_array_free (fields, FALSE);
  g_ptr_array_add (col_choices, (gpointer) _("(none)"));
  for (guint i = 0; prompt->fields[i] != NULL; i++)
    g_ptr_array_add (col_choices, prompt->fields[i]);
  g_ptr_array_add (col_choices, NULL);

  a1 = o42_ref_name (sel.row0, sel.col0);
  b1 = o42_ref_name (sel.row1, sel.col1);
  text = g_strdup_printf ("%s:%s", a1, b1);
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Source table:")));
  prompt->source = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (prompt->source), text);
  gtk_box_append (GTK_BOX (row), prompt->source);
  gtk_box_append (GTK_BOX (content), row);
  g_free (text); g_free (a1); g_free (b1);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Rows:")));
  prompt->row_field = gtk_drop_down_new_from_strings ((const char * const *) prompt->fields);
  gtk_box_append (GTK_BOX (row), prompt->row_field);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("then")));
  prompt->row_field2 = gtk_drop_down_new_from_strings ((const char * const *) col_choices->pdata);
  gtk_box_append (GTK_BOX (row), prompt->row_field2);
  gtk_box_append (GTK_BOX (content), row);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Columns:")));
  prompt->col_field = gtk_drop_down_new_from_strings ((const char * const *) col_choices->pdata);
  gtk_box_append (GTK_BOX (row), prompt->col_field);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("then")));
  prompt->col_field2 = gtk_drop_down_new_from_strings ((const char * const *) col_choices->pdata);
  gtk_box_append (GTK_BOX (row), prompt->col_field2);
  gtk_box_append (GTK_BOX (content), row);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Values:")));
  prompt->agg = drop_down_of (PIVOT_AGGS);
  gtk_box_append (GTK_BOX (row), prompt->agg);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("of")));
  prompt->data_field = gtk_drop_down_new_from_strings ((const char * const *) prompt->fields);
  if (g_strv_length (prompt->fields) > 1)
    gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->data_field), g_strv_length (prompt->fields) - 1);
  gtk_box_append (GTK_BOX (row), prompt->data_field);
  gtk_box_append (GTK_BOX (content), row);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("and:")));
  prompt->agg2 = drop_down_of (PIVOT_AGGS);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->agg2), 1);
  gtk_box_append (GTK_BOX (row), prompt->agg2);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("of")));
  prompt->data_field2 = gtk_drop_down_new_from_strings ((const char * const *) col_choices->pdata);
  gtk_box_append (GTK_BOX (row), prompt->data_field2);
  prompt->data_on_rows = gtk_check_button_new_with_mnemonic (_("Data fields _down the rows"));
  gtk_box_append (GTK_BOX (row), prompt->data_on_rows);
  gtk_box_append (GTK_BOX (content), row);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("or calculated field:")));
  prompt->calc = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->calc), _("=Sales-Costs"));
  gtk_widget_set_hexpand (prompt->calc, TRUE);
  gtk_box_append (GTK_BOX (row), prompt->calc);
  gtk_box_append (GTK_BOX (content), row);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Filter:")));
  prompt->filter_field = gtk_drop_down_new_from_strings ((const char * const *) col_choices->pdata);
  gtk_box_append (GTK_BOX (row), prompt->filter_field);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("=")));
  prompt->filter_value = gtk_entry_new ();
  gtk_widget_set_hexpand (prompt->filter_value, TRUE);
  gtk_box_append (GTK_BOX (row), prompt->filter_value);
  gtk_box_append (GTK_BOX (content), row);
  /* Grouping: one field, by dates, buckets or named groups. */
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Group:")));
  prompt->group_field = gtk_drop_down_new_from_strings ((const char * const *) col_choices->pdata);
  gtk_box_append (GTK_BOX (row), prompt->group_field);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("by")));
  prompt->group_kind = drop_down_of (PIVOT_GROUP_KINDS);
  gtk_box_append (GTK_BOX (row), prompt->group_kind);
  gtk_box_append (GTK_BOX (content), row);
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Buckets from")));
  prompt->bucket_start = gtk_spin_button_new_with_range (-1e9, 1e9, 1);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->bucket_start), 0);
  gtk_box_append (GTK_BOX (row), prompt->bucket_start);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("of")));
  prompt->bucket_size = gtk_spin_button_new_with_range (0.01, 1e9, 1);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->bucket_size), 100);
  gtk_box_append (GTK_BOX (row), prompt->bucket_size);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("or groups:")));
  prompt->manual_groups = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->manual_groups), _("Coast=East|West,Inland=Central"));
  gtk_widget_set_hexpand (prompt->manual_groups, TRUE);
  gtk_box_append (GTK_BOX (row), prompt->manual_groups);
  gtk_box_append (GTK_BOX (content), row);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  prompt->subtotals = gtk_check_button_new_with_mnemonic (_("_Subtotals"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->subtotals), TRUE);
  prompt->grand_rows = gtk_check_button_new_with_mnemonic (_("Grand total for _rows"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->grand_rows), TRUE);
  prompt->grand_cols = gtk_check_button_new_with_mnemonic (_("Grand total for _columns"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->grand_cols), TRUE);
  gtk_box_append (GTK_BOX (row), prompt->subtotals);
  gtk_box_append (GTK_BOX (row), prompt->grand_rows);
  gtk_box_append (GTK_BOX (row), prompt->grand_cols);
  gtk_box_append (GTK_BOX (content), row);

  gtk_box_append (GTK_BOX (content), gtk_label_new (_("The table is laid out on a new sheet; Data > Refresh Pivot Table lays it out again.")));
  g_ptr_array_free (col_choices, TRUE);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_pivot_ok), prompt);
  dialog_button (buttons, _("Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (on_pivot_destroy), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

void
action_refresh_pivot (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  (void) a; (void) p;
  o42_sheet_refresh_pivots (self->sheet);
  o42_grid_refresh (self->grid);
  window_sync (self);
}

/* ---- Data > Group and Outline -------------------------------------------- */

static void
outline_action (O42Window *self, gboolean rows, gboolean group)
{
  O42Range sel;

  if (o42_grid_is_editing (self->grid))
    o42_grid_commit_edit (self->grid);
  o42_grid_get_selection (self->grid, &sel);
  if (rows)
    o42_sheet_group (self->sheet, TRUE, sel.row0, sel.row1, group);
  else
    o42_sheet_group (self->sheet, FALSE, sel.col0, sel.col1, group);
  o42_grid_refresh (self->grid);
  window_sync (self);
}

void action_group_rows (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; outline_action (d, TRUE, TRUE); }
void action_group_cols (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; outline_action (d, FALSE, TRUE); }
void action_ungroup_rows (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; outline_action (d, TRUE, FALSE); }
void action_ungroup_cols (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; outline_action (d, FALSE, FALSE); }

void
action_auto_outline (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  int made;

  (void) a; (void) p;
  if (o42_grid_is_editing (self->grid))
    o42_grid_commit_edit (self->grid);
  made = o42_sheet_auto_outline (self->sheet);
  o42_grid_refresh (self->grid);
  window_sync (self);
  if (made == 0)
    gtk_label_set_text (GTK_LABEL (self->status_label),
                        _("No formula sums up the rows above it or the columns to its left."));
}

/* Data > Group and Outline > Settings: which side the summaries are on. */
typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *above, *left, *auto_outline;
} OutlineSettingsPrompt;

static void
on_outline_settings_ok (GtkWidget *w, gpointer data)
{
  OutlineSettingsPrompt *prompt = data;
  O42Window *self = prompt->window;

  (void) w;
  o42_sheet_set_outline_settings (self->sheet,
                                  gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->above)),
                                  gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->left)));
  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->auto_outline)))
    o42_sheet_auto_outline (self->sheet);
  o42_grid_refresh (self->grid);
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_outline_settings (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  OutlineSettingsPrompt *prompt = g_new0 (OutlineSettingsPrompt, 1);
  GtkWidget *content, *buttons, *ok, *label;

  (void) a; (void) p;
  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Outline Settings"), TRUE, &content, &buttons);
  label = gtk_label_new (_("Direction"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_add_css_class (label, "heading");
  gtk_box_append (GTK_BOX (content), label);
  prompt->above = gtk_check_button_new_with_mnemonic (_("Summary rows _above detail"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->above), o42_sheet_summary_above (self->sheet));
  gtk_box_append (GTK_BOX (content), prompt->above);
  prompt->left = gtk_check_button_new_with_mnemonic (_("Summary columns to _left of detail"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->left), o42_sheet_summary_left (self->sheet));
  gtk_box_append (GTK_BOX (content), prompt->left);
  prompt->auto_outline = gtk_check_button_new_with_mnemonic (_("_Create the outline now (Auto Outline)"));
  gtk_box_append (GTK_BOX (content), prompt->auto_outline);
  label = gtk_label_new (_("Auto Outline, Show Detail, Hide Detail and the fold boxes follow the direction."));
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_add_css_class (label, "dim-label");
  gtk_box_append (GTK_BOX (content), label);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_outline_settings_ok), prompt);
  dialog_button (buttons, _("Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

void
action_clear_outline (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  (void) a; (void) p;
  o42_sheet_clear_outline (self->sheet);
  o42_grid_refresh (self->grid);
  window_sync (self);
}

/* Show Detail and Hide Detail work on the active cell's row, or its
 * column when the row is in no group and its column is. */
static void
detail_action (O42Window *self, gboolean show)
{
  int row, col;
  gboolean done;

  if (o42_grid_is_editing (self->grid))
    o42_grid_commit_edit (self->grid);
  o42_grid_get_active (self->grid, &row, &col);
  done = o42_sheet_outline_detail (self->sheet, TRUE, row, show);
  if (!done)
    done = o42_sheet_outline_detail (self->sheet, FALSE, col, show);
  if (!done)
    gtk_label_set_text (GTK_LABEL (self->status_label),
                        _("The active cell is in no group and below none."));
  o42_grid_refresh (self->grid);
  window_sync (self);
}

void action_show_detail (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; detail_action (d, TRUE); }
void action_hide_detail (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; detail_action (d, FALSE); }

/* ---- Data > Validation -------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *kind, *op;
  GtkWidget *value, *value2;
  GtkWidget *message, *blank;
  GtkWidget *dropdown, *prompt_title, *prompt_text, *style, *title, *show_error;
} ValidPrompt;

static const char *VALID_KINDS[] = {
  N_("Any value"), N_("Whole number"), N_("Decimal"), N_("List"), N_("Date"), N_("Time"), N_("Text length"), NULL
};
static const char *VALID_STYLES[] = { N_("Stop"), N_("Warning"), N_("Information"), NULL };

static void
on_valid_ok (GtkWidget *w, gpointer data)
{
  ValidPrompt *prompt = data;
  O42Window *self = prompt->window;
  O42Validation v;

  (void) w;
  memset (&v, 0, sizeof v);
  o42_grid_get_selection (self->grid, &v.range);
  v.kind = (O42ValidKind) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->kind));
  v.op = (O42CondOp) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->op));
  v.value = (char *) gtk_editable_get_text (GTK_EDITABLE (prompt->value));
  v.value2 = (char *) gtk_editable_get_text (GTK_EDITABLE (prompt->value2));
  v.message = (char *) gtk_editable_get_text (GTK_EDITABLE (prompt->message));
  v.allow_blank = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->blank));
  v.no_dropdown = !gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->dropdown));
  v.prompt_title = (char *) gtk_editable_get_text (GTK_EDITABLE (prompt->prompt_title));
  v.prompt = (char *) gtk_editable_get_text (GTK_EDITABLE (prompt->prompt_text));
  v.style = (O42ValidStyle) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->style));
  v.title = (char *) gtk_editable_get_text (GTK_EDITABLE (prompt->title));
  v.no_error = !gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->show_error));

  o42_sheet_clear_validations (self->sheet, &v.range);
  if (v.kind != O42_VALID_ANY)
    o42_sheet_add_validation (self->sheet, &v);
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_valid_clear (GtkWidget *w, gpointer data)
{
  ValidPrompt *prompt = data;
  O42Range sel;

  (void) w;
  o42_grid_get_selection (prompt->window->grid, &sel);
  o42_sheet_clear_validations (prompt->window->sheet, &sel);
  window_sync (prompt->window);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_validation (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  ValidPrompt *prompt = g_new0 (ValidPrompt, 1);
  GtkWidget *content, *buttons, *row, *ok;
  O42Range sel;
  const O42Validation *existing = NULL;
  GArray *rules;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Data Validation"), TRUE, &content, &buttons);

  /* A rule already on the active cell fills the dialog in. */
  o42_grid_get_selection (self->grid, &sel);
  rules = o42_sheet_validations (self->sheet);
  for (guint i = 0; i < rules->len; i++)
    if (o42_range_contains (&g_array_index (rules, O42Validation, i).range, sel.row0, sel.col0))
      existing = &g_array_index (rules, O42Validation, i);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Allow:")));
  prompt->kind = drop_down_of (VALID_KINDS);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->kind), existing ? existing->kind : O42_VALID_WHOLE);
  gtk_box_append (GTK_BOX (row), prompt->kind);
  gtk_box_append (GTK_BOX (content), row);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Data:")));
  prompt->op = drop_down_of (O42_COND_NAMES);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->op), existing ? existing->op : O42_COND_BETWEEN);
  gtk_box_append (GTK_BOX (row), prompt->op);
  prompt->value = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->value), 10);
  if (existing) gtk_editable_set_text (GTK_EDITABLE (prompt->value), existing->value);
  gtk_box_append (GTK_BOX (row), prompt->value);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("and")));
  prompt->value2 = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->value2), 10);
  if (existing) gtk_editable_set_text (GTK_EDITABLE (prompt->value2), existing->value2);
  gtk_box_append (GTK_BOX (row), prompt->value2);
  gtk_box_append (GTK_BOX (content), row);
  gtk_box_append (GTK_BOX (content),
                  gtk_label_new (_("For a list, put the entries in the first box, comma-separated, or a range holding them.")));

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  prompt->blank = gtk_check_button_new_with_mnemonic ( _("Ignore _blank"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->blank), existing ? existing->allow_blank : TRUE);
  gtk_box_append (GTK_BOX (row), prompt->blank);
  prompt->dropdown = gtk_check_button_new_with_mnemonic ( _("In-cell _dropdown"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->dropdown), existing ? !existing->no_dropdown : TRUE);
  gtk_box_append (GTK_BOX (row), prompt->dropdown);
  gtk_box_append (GTK_BOX (content), row);

  /* Input Message: shown under the cell while it is chosen. */
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Input message:")));
  prompt->prompt_title = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->prompt_title), _("Title"));
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->prompt_title), 12);
  if (existing && existing->prompt_title) gtk_editable_set_text (GTK_EDITABLE (prompt->prompt_title), existing->prompt_title);
  gtk_box_append (GTK_BOX (row), prompt->prompt_title);
  prompt->prompt_text = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->prompt_text), _("Shown when the cell is selected"));
  gtk_widget_set_hexpand (prompt->prompt_text, TRUE);
  if (existing && existing->prompt) gtk_editable_set_text (GTK_EDITABLE (prompt->prompt_text), existing->prompt);
  gtk_box_append (GTK_BOX (row), prompt->prompt_text);
  gtk_box_append (GTK_BOX (content), row);

  /* Error Alert: the style, a title and the message. */
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Error alert:")));
  prompt->style = drop_down_of (VALID_STYLES);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->style), existing ? existing->style : O42_VALID_STOP);
  gtk_box_append (GTK_BOX (row), prompt->style);
  prompt->title = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->title), _("Title"));
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->title), 12);
  if (existing && existing->title) gtk_editable_set_text (GTK_EDITABLE (prompt->title), existing->title);
  gtk_box_append (GTK_BOX (row), prompt->title);
  prompt->message = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->message), _("Error message"));
  gtk_widget_set_hexpand (prompt->message, TRUE);
  if (existing) gtk_editable_set_text (GTK_EDITABLE (prompt->message), existing->message);
  gtk_box_append (GTK_BOX (row), prompt->message);
  gtk_box_append (GTK_BOX (content), row);
  prompt->show_error = gtk_check_button_new_with_mnemonic ( _("Show error alert after invalid data is _entered"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->show_error), existing ? !existing->no_error : TRUE);
  gtk_box_append (GTK_BOX (content), prompt->show_error);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_valid_ok), prompt);
  dialog_button (buttons, _("_Clear"), G_CALLBACK (on_valid_clear), prompt);
  dialog_button (buttons, _("Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->value);
}

/* Data > Validation > Circle Invalid Data, and Clear Validation Circles. */
void
action_circle_invalid (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  (void) a; (void) p;
  o42_grid_set_circle_invalid (self->grid, TRUE);
}

void
action_clear_circles (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  (void) a; (void) p;
  o42_grid_set_circle_invalid (self->grid, FALSE);
}

/* ---- Data > Text to Columns -------------------------------------------- */

/* Two pages, as Excel's wizard has: split at a delimiter, or at fixed
 * character positions shown on a ruler over the first rows, where a
 * click puts a break, a drag moves one and a double-click takes it
 * away.  A click on a column of the preview chooses it for the data
 * format drop-down. */
#define PREVIEW_ROWS 8

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *notebook;
  GtkWidget *choice[4];
  GtkWidget *other;
  GtkWidget *preview;
  GtkWidget *type_drop;
  GArray    *breaks;        /* int, ascending character positions */
  GArray    *types;         /* O42SplitType, one per column */
  GPtrArray *lines;         /* the first rows' text */
  int        selected;      /* the column the drop-down is about */
  int        dragging;      /* index of the break being dragged, or -1 */
  double     char_w, line_h;
  gboolean   updating;
} SplitPrompt;

#define RULER_H 18.0
#define PREVIEW_PAD 6.0

static int
split_column_at (SplitPrompt *prompt, int position)
{
  int col = 0;

  for (guint i = 0; i < prompt->breaks->len; i++)
    if (g_array_index (prompt->breaks, int, i) <= position)
      col = i + 1;
  return col;
}

static void
split_sync_type_drop (SplitPrompt *prompt)
{
  O42SplitType type = O42_SPLIT_GENERAL;

  if (prompt->selected >= 0 && (guint) prompt->selected < prompt->types->len)
    type = g_array_index (prompt->types, O42SplitType, prompt->selected);
  prompt->updating = TRUE;
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->type_drop), type);
  prompt->updating = FALSE;
}

static void
split_preview_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  SplitPrompt *prompt = data;
  PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (area), NULL);
  PangoFontDescription *desc = pango_font_description_from_string ("Monospace 10");
  int tw, th;
  double x0 = PREVIEW_PAD;

  pango_layout_set_font_description (layout, desc);
  pango_layout_set_text (layout, "0", -1);
  pango_layout_get_pixel_size (layout, &tw, &th);
  prompt->char_w = tw;
  prompt->line_h = th + 2;

  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_paint (cr);

  /* The chosen column, shaded. */
  if (prompt->selected >= 0)
    {
      int from = prompt->selected == 0 ? 0 : g_array_index (prompt->breaks, int, prompt->selected - 1);
      double to_x = (guint) prompt->selected < prompt->breaks->len
                    ? x0 + g_array_index (prompt->breaks, int, prompt->selected) * tw : width;

      cairo_set_source_rgb (cr, 0.85, 0.88, 0.95);
      cairo_rectangle (cr, x0 + from * tw, RULER_H, to_x - x0 - from * tw, height - RULER_H);
      cairo_fill (cr);
    }

  /* The ruler: a tick every character, a longer one every five, the
   * number every ten. */
  cairo_set_source_rgb (cr, 0.3, 0.3, 0.3);
  cairo_set_line_width (cr, 1);
  for (int c = 0; x0 + c * tw < width; c++)
    {
      double x = floor (x0 + c * tw) + 0.5;
      double len = c % 10 == 0 ? 6 : c % 5 == 0 ? 4 : 2;

      cairo_move_to (cr, x, RULER_H - 1);
      cairo_line_to (cr, x, RULER_H - 1 - len);
      if (c % 10 == 0 && c > 0)
        {
          char num[8];
          PangoFontDescription *small = pango_font_description_from_string ("Sans 7");
          int nw, nh;

          g_snprintf (num, sizeof num, "%d", c);
          pango_layout_set_font_description (layout, small);
          pango_layout_set_text (layout, num, -1);
          pango_layout_get_pixel_size (layout, &nw, &nh);
          cairo_move_to (cr, x - nw / 2.0, 0);
          pango_cairo_show_layout (cr, layout);
          pango_layout_set_font_description (layout, desc);
          pango_font_description_free (small);
        }
    }
  cairo_stroke (cr);

  /* The rows. */
  cairo_set_source_rgb (cr, 0, 0, 0);
  for (guint i = 0; i < prompt->lines->len; i++)
    {
      pango_layout_set_text (layout, g_ptr_array_index (prompt->lines, i), -1);
      cairo_move_to (cr, x0, RULER_H + 2 + i * prompt->line_h);
      pango_cairo_show_layout (cr, layout);
    }

  /* The breaks, with an arrowhead on the ruler. */
  for (guint i = 0; i < prompt->breaks->len; i++)
    {
      double x = floor (x0 + g_array_index (prompt->breaks, int, i) * tw) + 0.5;

      cairo_set_source_rgb (cr, 0.1, 0.1, 0.6);
      cairo_move_to (cr, x, RULER_H - 8);
      cairo_line_to (cr, x, height);
      cairo_stroke (cr);
      cairo_move_to (cr, x - 4, RULER_H - 12);
      cairo_line_to (cr, x + 4, RULER_H - 12);
      cairo_line_to (cr, x, RULER_H - 6);
      cairo_close_path (cr);
      cairo_fill (cr);
    }

  pango_font_description_free (desc);
  g_object_unref (layout);
}

static int
split_break_near (SplitPrompt *prompt, double x)
{
  for (guint i = 0; i < prompt->breaks->len; i++)
    if (fabs (x - (PREVIEW_PAD + g_array_index (prompt->breaks, int, i) * prompt->char_w)) <= 4)
      return (int) i;
  return -1;
}

static int
split_position_at (SplitPrompt *prompt, double x)
{
  return prompt->char_w > 0 ? MAX (0, (int) ((x - PREVIEW_PAD) / prompt->char_w + 0.5)) : 0;
}

static void
split_insert_break (SplitPrompt *prompt, int position)
{
  guint at = 0;
  O42SplitType general = O42_SPLIT_GENERAL;

  if (position <= 0)
    return;
  for (at = 0; at < prompt->breaks->len; at++)
    {
      int b = g_array_index (prompt->breaks, int, at);
      if (b == position)
        return;
      if (b > position)
        break;
    }
  g_array_insert_val (prompt->breaks, at, position);
  /* The column that was cut becomes two of the same kind. */
  g_array_insert_val (prompt->types, at + 1, general);
  if (at + 1 < prompt->types->len)
    g_array_index (prompt->types, O42SplitType, at + 1) = g_array_index (prompt->types, O42SplitType, at);
}

static void
split_remove_break (SplitPrompt *prompt, int index)
{
  g_array_remove_index (prompt->breaks, index);
  if ((guint) index + 1 < prompt->types->len)
    g_array_remove_index (prompt->types, index + 1);
  if ((guint) prompt->selected >= prompt->types->len)
    prompt->selected = (int) prompt->types->len - 1;
}

static void
on_split_pressed (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  SplitPrompt *prompt = data;
  int near = split_break_near (prompt, x);

  (void) gesture;
  if (n_press == 2 && near >= 0)
    {
      split_remove_break (prompt, near);
      prompt->dragging = -1;
    }
  else if (n_press == 1 && near >= 0)
    prompt->dragging = near;
  else if (n_press == 1)
    {
      int position = split_position_at (prompt, x);

      if (y < RULER_H)
        split_insert_break (prompt, position);
      else
        prompt->selected = split_column_at (prompt, position);
      prompt->dragging = -1;
    }
  split_sync_type_drop (prompt);
  gtk_widget_queue_draw (prompt->preview);
}

static void
on_split_released (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  SplitPrompt *prompt = data;

  (void) gesture; (void) n_press; (void) x; (void) y;
  prompt->dragging = -1;
  /* Keep the breaks in order after a drag past a neighbour. */
  for (guint i = 1; i < prompt->breaks->len; i++)
    if (g_array_index (prompt->breaks, int, i) <= g_array_index (prompt->breaks, int, i - 1))
      {
        split_remove_break (prompt, (int) i);
        i--;
      }
  gtk_widget_queue_draw (prompt->preview);
}

static void
on_split_motion (GtkEventControllerMotion *motion, double x, double y, gpointer data)
{
  SplitPrompt *prompt = data;

  (void) motion; (void) y;
  if (prompt->dragging < 0 || (guint) prompt->dragging >= prompt->breaks->len)
    return;
  g_array_index (prompt->breaks, int, prompt->dragging) = MAX (1, split_position_at (prompt, x));
  gtk_widget_queue_draw (prompt->preview);
}

static void
on_split_type_changed (GObject *drop, GParamSpec *pspec, gpointer data)
{
  SplitPrompt *prompt = data;

  (void) pspec;
  if (prompt->updating || prompt->selected < 0 || (guint) prompt->selected >= prompt->types->len)
    return;
  g_array_index (prompt->types, O42SplitType, prompt->selected) =
    (O42SplitType) gtk_drop_down_get_selected (GTK_DROP_DOWN (drop));
}

static void
on_split_ok (GtkWidget *w, gpointer data)
{
  SplitPrompt *prompt = data;
  O42Window *self = prompt->window;
  static const char *delims[4] = { ",", "\t", ";", " " };
  const char *delim = ",";
  O42Range sel;

  (void) w;

  for (int i = 0; i < 4; i++)
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->choice[i])))
      delim = delims[i];
  if (*gtk_editable_get_text (GTK_EDITABLE (prompt->other)) != '\0')
    delim = gtk_editable_get_text (GTK_EDITABLE (prompt->other));

  if (o42_grid_is_editing (self->grid))
    o42_grid_commit_edit (self->grid);
  o42_grid_get_selection (self->grid, &sel);
  if (gtk_notebook_get_current_page (GTK_NOTEBOOK (prompt->notebook)) == 1)
    o42_sheet_text_to_columns_fixed (self->sheet, &sel, (const int *) prompt->breaks->data,
                                     (int) prompt->breaks->len,
                                     (const O42SplitType *) prompt->types->data);
  else
    o42_sheet_text_to_columns (self->sheet, &sel, delim);
  o42_grid_refresh (self->grid);
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_split_destroy (GtkWidget *w, gpointer data)
{
  SplitPrompt *prompt = data;

  (void) w;
  g_array_unref (prompt->breaks);
  g_array_unref (prompt->types);
  g_ptr_array_unref (prompt->lines);
  g_free (prompt);
}

void
action_text_to_columns (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  SplitPrompt *prompt = g_new0 (SplitPrompt, 1);
  GtkWidget *content, *buttons, *row, *ok, *page, *hint;
  static const char *const names[4] = { N_("_Comma"), N_("_Tab"), N_("_Semicolon"), N_("S_pace") };
  static const char *const TYPES[] = { N_("General"), N_("Text"), N_("Date"), N_("Do not import"), NULL };
  O42Range sel;

  (void) a; (void) p;

  prompt->window = self;
  prompt->breaks = g_array_new (FALSE, FALSE, sizeof (int));
  prompt->types = g_array_new (FALSE, FALSE, sizeof (O42SplitType));
  prompt->lines = g_ptr_array_new_with_free_func (g_free);
  prompt->dragging = -1;
  prompt->dialog = dialog_frame (self, _("Text to Columns"), TRUE, &content, &buttons);

  prompt->notebook = gtk_notebook_new ();
  gtk_box_append (GTK_BOX (content), prompt->notebook);

  /* Delimited. */
  page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top (page, 8);
  gtk_widget_set_margin_bottom (page, 8);
  gtk_widget_set_margin_start (page, 8);
  gtk_widget_set_margin_end (page, 8);
  gtk_box_append (GTK_BOX (page), gtk_label_new (_("Split the selected column at:")));
  for (int i = 0; i < 4; i++)
    {
      prompt->choice[i] = gtk_check_button_new_with_mnemonic (_(names[i]));
      if (i > 0)
        gtk_check_button_set_group (GTK_CHECK_BUTTON (prompt->choice[i]),
                                    GTK_CHECK_BUTTON (prompt->choice[0]));
      gtk_box_append (GTK_BOX (page), prompt->choice[i]);
    }
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->choice[0]), TRUE);
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Other:")));
  prompt->other = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->other), 6);
  gtk_box_append (GTK_BOX (row), prompt->other);
  gtk_box_append (GTK_BOX (page), row);
  gtk_notebook_append_page (GTK_NOTEBOOK (prompt->notebook), page, gtk_label_new (_("Delimited")));

  /* Fixed width: the first rows under a ruler. */
  o42_grid_get_selection (self->grid, &sel);
  o42_sheet_guess_fixed_breaks (self->sheet, &sel, prompt->breaks);
  g_array_set_size (prompt->types, prompt->breaks->len + 1);
  for (int r = sel.row0; r <= sel.row1 && r < sel.row0 + PREVIEW_ROWS; r++)
    {
      O42Value v;

      o42_sheet_get_value (self->sheet, r, sel.col0, &v);
      g_ptr_array_add (prompt->lines, v.type == O42_VALUE_TEXT ? g_strdup (v.as.text) : g_strdup (""));
      o42_value_clear (&v);
    }

  page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top (page, 8);
  gtk_widget_set_margin_bottom (page, 8);
  gtk_widget_set_margin_start (page, 8);
  gtk_widget_set_margin_end (page, 8);
  hint = gtk_label_new (_("Click the ruler to put in a break, drag one to move it, "
                          "double-click one to take it away.  Click a column to choose its format."));
  gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_label_set_max_width_chars (GTK_LABEL (hint), 56);
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_box_append (GTK_BOX (page), hint);

  prompt->preview = gtk_drawing_area_new ();
  gtk_widget_set_size_request (prompt->preview, 480, (int) (RULER_H + 4 + PREVIEW_ROWS * 19));
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (prompt->preview), split_preview_draw, prompt, NULL);
  {
    GtkGesture *click = gtk_gesture_click_new ();
    GtkEventController *motion = gtk_event_controller_motion_new ();
    GtkWidget *frame = gtk_frame_new (NULL);

    g_signal_connect (click, "pressed", G_CALLBACK (on_split_pressed), prompt);
    g_signal_connect (click, "released", G_CALLBACK (on_split_released), prompt);
    gtk_widget_add_controller (prompt->preview, GTK_EVENT_CONTROLLER (click));
    g_signal_connect (motion, "motion", G_CALLBACK (on_split_motion), prompt);
    gtk_widget_add_controller (prompt->preview, motion);
    gtk_frame_set_child (GTK_FRAME (frame), prompt->preview);
    gtk_box_append (GTK_BOX (page), frame);
  }
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Column data format:")));
  prompt->type_drop = drop_down_of (TYPES);
  g_signal_connect (prompt->type_drop, "notify::selected", G_CALLBACK (on_split_type_changed), prompt);
  gtk_box_append (GTK_BOX (row), prompt->type_drop);
  gtk_box_append (GTK_BOX (page), row);
  gtk_notebook_append_page (GTK_NOTEBOOK (prompt->notebook), page, gtk_label_new (_("Fixed width")));
  prompt->selected = 0;
  split_sync_type_drop (prompt);

  /* Text that lines up in columns with no delimiter in sight opens on
   * the fixed-width page, as Excel's wizard guesses. */
  if (prompt->breaks->len > 0 && prompt->lines->len > 0 &&
      strpbrk (g_ptr_array_index (prompt->lines, 0), ",\t;") == NULL)
    gtk_notebook_set_current_page (GTK_NOTEBOOK (prompt->notebook), 1);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_split_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_split_destroy), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Data > Form -------------------------------------------------------- */

/* Excel 97's data form: the list around the active cell, one record at
 * a time, with New, Delete, Restore, Find Prev, Find Next and Criteria.
 * The first row of the list is its headings, one field apiece. */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  O42Range   list;          /* headings on the first row */
  int        record;        /* the row on show, or -1 for a new record */
  gboolean   criteria;      /* the entries hold criteria rather than a record */
  GPtrArray *entries;       /* GtkWidget *, one per column */
  GtkWidget *counter;
  GtkWidget *new_btn, *delete_btn, *criteria_btn;
  char     **criteria_text; /* the criteria in force, one per column, or NULL */
} FormPrompt;

/* The block of filled cells around a cell, bounded by empty rows and
 * columns: Excel's CurrentRegion. */
static void
current_region (O42Sheet *sheet, int row, int col, O42Range *out)
{
  O42Range r = { row, col, row, col };
  gboolean grew = TRUE;

  while (grew)
    {
      grew = FALSE;
      /* A line just outside each edge, the corners included, with
       * anything in it pulls the edge out. */
      if (r.row0 > 0)
        for (int c = MAX (r.col0 - 1, 0); c <= MIN (r.col1 + 1, O42_MAX_COLS - 1); c++)
          if (!o42_sheet_is_empty (sheet, r.row0 - 1, c)) { r.row0--; grew = TRUE; break; }
      if (r.row1 < O42_MAX_ROWS - 1)
        for (int c = MAX (r.col0 - 1, 0); c <= MIN (r.col1 + 1, O42_MAX_COLS - 1); c++)
          if (!o42_sheet_is_empty (sheet, r.row1 + 1, c)) { r.row1++; grew = TRUE; break; }
      if (r.col0 > 0)
        for (int rr = MAX (r.row0 - 1, 0); rr <= MIN (r.row1 + 1, O42_MAX_ROWS - 1); rr++)
          if (!o42_sheet_is_empty (sheet, rr, r.col0 - 1)) { r.col0--; grew = TRUE; break; }
      if (r.col1 < O42_MAX_COLS - 1)
        for (int rr = MAX (r.row0 - 1, 0); rr <= MIN (r.row1 + 1, O42_MAX_ROWS - 1); rr++)
          if (!o42_sheet_is_empty (sheet, rr, r.col1 + 1)) { r.col1++; grew = TRUE; break; }
    }
  *out = r;
}

static int
form_n_records (FormPrompt *prompt)
{
  return prompt->list.row1 - prompt->list.row0;
}

static void
form_set_counter (FormPrompt *prompt)
{
  char *text;

  if (prompt->criteria)
    text = g_strdup (_("Criteria"));
  else if (prompt->record < 0)
    text = g_strdup (_("New Record"));
  else
    /* Translators: the data form's counter: the record on show and how
       many records the list has, as in "3 of 12". */
    text = g_strdup_printf (_("%d of %d"), prompt->record - prompt->list.row0, form_n_records (prompt));
  gtk_label_set_text (GTK_LABEL (prompt->counter), text);
  g_free (text);
}

/* The entries show a record: what was typed into each cell, and a
 * formula's value, which is not for editing. */
static void
form_show_record (FormPrompt *prompt, int row)
{
  O42Sheet *sheet = prompt->window->sheet;

  prompt->record = row;
  prompt->criteria = FALSE;
  for (guint i = 0; i < prompt->entries->len; i++)
    {
      GtkWidget *entry = g_ptr_array_index (prompt->entries, i);
      int col = prompt->list.col0 + (int) i;

      if (row < 0)
        {
          gtk_editable_set_text (GTK_EDITABLE (entry), "");
          gtk_widget_set_sensitive (entry, TRUE);
        }
      else if (o42_sheet_has_formula (sheet, row, col))
        {
          char *shown = o42_sheet_get_display (sheet, row, col);
          gtk_editable_set_text (GTK_EDITABLE (entry), shown != NULL ? shown : "");
          gtk_widget_set_sensitive (entry, FALSE);
          g_free (shown);
        }
      else
        {
          char *input = o42_sheet_get_input (sheet, row, col);
          gtk_editable_set_text (GTK_EDITABLE (entry), input != NULL ? input : "");
          gtk_widget_set_sensitive (entry, TRUE);
          g_free (input);
        }
    }
  gtk_button_set_label (GTK_BUTTON (prompt->new_btn), _("_New"));
  gtk_button_set_label (GTK_BUTTON (prompt->criteria_btn), _("C_riteria"));
  gtk_widget_set_sensitive (prompt->delete_btn, row >= 0);
  form_set_counter (prompt);
  if (prompt->entries->len > 0)
    gtk_widget_grab_focus (g_ptr_array_index (prompt->entries, 0));
}

/* Writes the entries back: into the record's row, or a new row under
 * the list.  FALSE when a new record has nowhere to go. */
static gboolean
form_commit (FormPrompt *prompt)
{
  O42Sheet *sheet = prompt->window->sheet;
  int row = prompt->record;
  gboolean any = FALSE, changed = FALSE;

  if (prompt->criteria)
    return TRUE;

  for (guint i = 0; i < prompt->entries->len && !any; i++)
    any = *gtk_editable_get_text (GTK_EDITABLE (g_ptr_array_index (prompt->entries, i))) != '\0';
  if (row < 0)
    {
      if (!any)
        return TRUE;
      row = prompt->list.row1 + 1;
      if (row >= O42_MAX_ROWS)
        return FALSE;
      for (int col = prompt->list.col0; col <= prompt->list.col1; col++)
        if (!o42_sheet_is_empty (sheet, row, col))
          {
            o42_window_show_error (prompt->window, _("Cannot extend the list: the row below it is not empty."), NULL);
            return FALSE;
          }
    }

  o42_sheet_begin_group (sheet);
  for (guint i = 0; i < prompt->entries->len; i++)
    {
      GtkWidget *entry = g_ptr_array_index (prompt->entries, i);
      int col = prompt->list.col0 + (int) i;
      const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
      char *had;

      if (!gtk_widget_get_sensitive (entry))
        continue;   /* a formula's value, shown and not touched */
      had = o42_sheet_get_input (sheet, row, col);
      if (g_strcmp0 (had != NULL ? had : "", text) != 0)
        {
          char *typed = o42_sheet_typed_input (sheet, row, col, text);

          o42_sheet_set_input (sheet, row, col, typed != NULL ? typed : text);
          g_free (typed);
          changed = TRUE;
        }
      g_free (had);
    }
  o42_sheet_end_group (sheet);

  if (prompt->record < 0)
    {
      prompt->list.row1 = row;
      prompt->record = row;
    }
  if (changed)
    {
      o42_grid_refresh (prompt->window->grid);
      o42_window_sync (prompt->window);
    }
  return TRUE;
}

/* Whether the record on `row` meets the criteria: text that begins as
 * the criterion does, or a comparison -- >100, <=5, <>x, =y. */
static gboolean
form_matches (FormPrompt *prompt, int row)
{
  O42Sheet *sheet = prompt->window->sheet;

  if (prompt->criteria_text == NULL)
    return TRUE;
  for (guint i = 0; i < prompt->entries->len; i++)
    {
      const char *crit = prompt->criteria_text[i];
      int col = prompt->list.col0 + (int) i;
      char *shown;
      gboolean ok;

      if (crit == NULL || *crit == '\0')
        continue;
      shown = o42_sheet_get_display (sheet, row, col);
      if (crit[0] == '<' || crit[0] == '>' || crit[0] == '=')
        {
          const char *op = crit;
          const char *rhs = crit + ((crit[1] == '=' || crit[1] == '>') ? 2 : 1);
          char *end = NULL;
          double a = g_ascii_strtod (shown != NULL ? shown : "", &end);
          gboolean a_num = end != NULL && end != shown && *end == '\0';
          double b = g_ascii_strtod (rhs, &end);
          gboolean b_num = end != NULL && end != rhs && *end == '\0';
          int cmp = (a_num && b_num) ? (a < b ? -1 : a > b ? 1 : 0)
                                     : g_utf8_collate (shown != NULL ? shown : "", rhs);

          if (g_str_has_prefix (op, "<>"))      ok = cmp != 0;
          else if (g_str_has_prefix (op, "<=")) ok = cmp <= 0;
          else if (g_str_has_prefix (op, ">=")) ok = cmp >= 0;
          else if (op[0] == '<')                ok = cmp < 0;
          else if (op[0] == '>')                ok = cmp > 0;
          else                                  ok = cmp == 0;
        }
      else
        {
          char *a = g_utf8_casefold (shown != NULL ? shown : "", -1);
          char *b = g_utf8_casefold (crit, -1);
          ok = g_str_has_prefix (a, b);
          g_free (a);
          g_free (b);
        }
      g_free (shown);
      if (!ok)
        return FALSE;
    }
  return TRUE;
}

/* Leaving the criteria: what was typed is kept in force. */
static void
form_take_criteria (FormPrompt *prompt)
{
  gboolean any = FALSE;

  g_strfreev (prompt->criteria_text);
  prompt->criteria_text = g_new0 (char *, prompt->entries->len + 1);
  for (guint i = 0; i < prompt->entries->len; i++)
    {
      prompt->criteria_text[i] = g_strdup (gtk_editable_get_text (GTK_EDITABLE (g_ptr_array_index (prompt->entries, i))));
      any = any || *prompt->criteria_text[i] != '\0';
    }
  if (!any)
    g_clear_pointer (&prompt->criteria_text, g_strfreev);
}

static void
form_step (FormPrompt *prompt, int direction)
{
  int from;

  if (prompt->criteria)
    {
      form_take_criteria (prompt);
      from = prompt->record < 0 ? prompt->list.row1 + 1 : prompt->record;
    }
  else
    {
      if (!form_commit (prompt))
        return;
      from = prompt->record < 0 ? prompt->list.row1 + 1 : prompt->record;
    }
  for (int row = from + direction; row > prompt->list.row0 && row <= prompt->list.row1; row += direction)
    if (form_matches (prompt, row))
      {
        form_show_record (prompt, row);
        return;
      }
  /* Nothing further that way: stay, but as a record. */
  if (prompt->criteria)
    form_show_record (prompt, prompt->record < 0 && form_n_records (prompt) > 0 ? prompt->list.row0 + 1 : prompt->record);
}

static void
on_form_prev (GtkWidget *w, gpointer data) { (void) w; form_step (data, -1); }
static void
on_form_next (GtkWidget *w, gpointer data) { (void) w; form_step (data, 1); }
static void
on_form_activate (GtkEntry *entry, gpointer data) { (void) entry; form_step (data, 1); }

static void
on_form_new (GtkWidget *w, gpointer data)
{
  FormPrompt *prompt = data;

  (void) w;
  if (prompt->criteria)
    {
      /* Clear, in the criteria. */
      for (guint i = 0; i < prompt->entries->len; i++)
        gtk_editable_set_text (GTK_EDITABLE (g_ptr_array_index (prompt->entries, i)), "");
      return;
    }
  if (!form_commit (prompt))
    return;
  form_show_record (prompt, -1);
}

static void
on_form_delete (GtkWidget *w, gpointer data)
{
  FormPrompt *prompt = data;
  O42Sheet *sheet = prompt->window->sheet;
  int row = prompt->record;

  (void) w;
  if (prompt->criteria || row < 0)
    return;
  o42_sheet_delete_rows (sheet, row, 1);
  prompt->list.row1--;
  o42_grid_refresh (prompt->window->grid);
  o42_window_sync (prompt->window);
  if (form_n_records (prompt) <= 0)
    form_show_record (prompt, -1);
  else
    form_show_record (prompt, MIN (row, prompt->list.row1));
}

static void
on_form_restore (GtkWidget *w, gpointer data)
{
  FormPrompt *prompt = data;

  (void) w;
  if (!prompt->criteria)
    form_show_record (prompt, prompt->record);
}

static void
on_form_criteria (GtkWidget *w, gpointer data)
{
  FormPrompt *prompt = data;

  (void) w;
  if (prompt->criteria)
    {
      /* Back to the form, the criteria kept. */
      form_take_criteria (prompt);
      form_show_record (prompt, prompt->record < 0 && form_n_records (prompt) > 0
                                ? prompt->list.row0 + 1 : prompt->record);
      return;
    }
  if (!form_commit (prompt))
    return;
  prompt->criteria = TRUE;
  for (guint i = 0; i < prompt->entries->len; i++)
    {
      GtkWidget *entry = g_ptr_array_index (prompt->entries, i);

      gtk_widget_set_sensitive (entry, TRUE);
      gtk_editable_set_text (GTK_EDITABLE (entry),
                             prompt->criteria_text != NULL && prompt->criteria_text[i] != NULL
                             ? prompt->criteria_text[i] : "");
    }
  gtk_button_set_label (GTK_BUTTON (prompt->new_btn), _("Cl_ear"));
  gtk_button_set_label (GTK_BUTTON (prompt->criteria_btn), _("_Form"));
  gtk_widget_set_sensitive (prompt->delete_btn, FALSE);
  form_set_counter (prompt);
  if (prompt->entries->len > 0)
    gtk_widget_grab_focus (g_ptr_array_index (prompt->entries, 0));
}

static void
on_form_close (GtkWidget *w, gpointer data)
{
  FormPrompt *prompt = data;

  (void) w;
  if (!prompt->criteria && !form_commit (prompt))
    return;
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
form_prompt_free (gpointer data)
{
  FormPrompt *prompt = data;

  g_ptr_array_unref (prompt->entries);
  g_strfreev (prompt->criteria_text);
  g_free (prompt);
}

void
action_data_form (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  FormPrompt *prompt;
  GtkWidget *content, *buttons, *columns, *grid, *side;
  O42Range list;
  int row, col;

  (void) a; (void) p;

  if (o42_grid_is_editing (self->grid))
    o42_grid_commit_edit (self->grid);
  o42_grid_get_active (self->grid, &row, &col);
  current_region (self->sheet, row, col, &list);
  if (list.row1 == list.row0)
    {
      show_error (self, _("No list was found around the active cell. A list has a row of headings with its records below."), NULL);
      return;
    }

  prompt = g_new0 (FormPrompt, 1);
  prompt->window = self;
  prompt->list = list;
  prompt->entries = g_ptr_array_new ();
  prompt->dialog = dialog_frame (self, o42_sheet_get_name (self->sheet), FALSE, &content, &buttons);

  columns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 4);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  for (int c = list.col0; c <= list.col1 && c - list.col0 < 32; c++)
    {
      char *heading = o42_sheet_get_display (self->sheet, list.row0, c);
      GtkWidget *entry = gtk_entry_new ();
      char letters[8], *label;

      /* A column without a heading goes by its letter. */
      o42_col_name (c, letters, sizeof letters);
      /* Translators: a field's label in the data form: the column's
         heading, or its letter, then a colon. */
      label = g_strdup_printf (_("%s:"), heading != NULL && *heading != '\0' ? heading : letters);

      gtk_editable_set_width_chars (GTK_EDITABLE (entry), 24);
      labelled (grid, c - list.col0, label, entry);
      g_signal_connect (entry, "activate", G_CALLBACK (on_form_activate), prompt);
      g_ptr_array_add (prompt->entries, entry);
      g_free (label);
      g_free (heading);
    }
  gtk_box_append (GTK_BOX (columns), grid);

  /* Excel's column of buttons down the right, the counter above them. */
  side = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  prompt->counter = gtk_label_new ("");
  gtk_widget_set_halign (prompt->counter, GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (side), prompt->counter);
  prompt->new_btn = dialog_button (side, _("_New"), G_CALLBACK (on_form_new), prompt);
  prompt->delete_btn = dialog_button (side, _("_Delete"), G_CALLBACK (on_form_delete), prompt);
  dialog_button (side, _("Res_tore"), G_CALLBACK (on_form_restore), prompt);
  dialog_button (side, _("Find _Prev"), G_CALLBACK (on_form_prev), prompt);
  dialog_button (side, _("Find Ne_xt"), G_CALLBACK (on_form_next), prompt);
  prompt->criteria_btn = dialog_button (side, _("C_riteria"), G_CALLBACK (on_form_criteria), prompt);
  dialog_button (side, _("Close"), G_CALLBACK (on_form_close), prompt);
  gtk_box_append (GTK_BOX (columns), side);
  gtk_box_append (GTK_BOX (content), columns);
  gtk_widget_set_visible (buttons, FALSE);

  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (form_prompt_free), prompt);

  /* The record the active cell is on, or the first. */
  form_show_record (prompt, row > list.row0 ? row : list.row0 + 1);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}
