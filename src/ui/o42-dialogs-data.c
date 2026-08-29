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
  static const char *titles[3] = { "Sort by:", "Then by:", "Then by:" };

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
        g_snprintf (label, sizeof label, "Column %s", name);
      gtk_string_list_append (columns, label);
      gtk_string_list_append (columns_none, label);
      g_free (heading);
    }
  gtk_string_list_splice (columns_none, 0, 0, (const char *[]) { "(none)", NULL });

  prompt->dialog = dialog_frame (self, _("Sort"), TRUE, &content, &buttons);

  for (int k = 0; k < 3; k++)
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      GtkWidget *descending;

      gtk_box_append (GTK_BOX (row), gtk_label_new (titles[k]));
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
    char *message = g_strdup_printf ("%d row%s answered the criteria.", n, n == 1 ? "" : "s");
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
    GtkWidget *hint = gtk_label_new ("The criteria range names fields in its first row; each row after it "
                                     "is a set of conditions that must all hold, and any one row is enough. "
                                     "A condition is \">5\", \"<>Japan\", \"*land\" or a value to equal.");
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
  where = g_strdup_printf ("The result goes at %s. Without labels the cells are matched by position.", at);
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
      char *text;
      GtkWidget *label;

      o42_sheet_scenario_cells (prompt->window->sheet, name, &keys, NULL, &comment);
      text = g_strdup_printf ("%s  (%u cells)%s%s", name, keys != NULL ? keys->len : 0,
                              comment != NULL && *comment != '\0' ? " -- " : "",
                              comment != NULL ? comment : "");
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

void
action_scenarios (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  ScenarioPrompt *prompt = g_new0 (ScenarioPrompt, 1);
  GtkWidget *content, *buttons, *scroller, *grid, *show;
  O42Range sel;
  char *a1, *b1, *where;

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
  where = g_strdup_printf ("Add takes the values of %s:%s as they are now.", a1, b1);
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
  char *a1, *b1, *where;

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
  where = g_strdup_printf ("Table over %s:%s", a1, b1);
  gtk_box_append (GTK_BOX (content), gtk_label_new (where));
  g_free (where); g_free (a1); g_free (b1);

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
        g_snprintf (label, sizeof label, "Column %s", name);
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
      char *message = g_strdup_printf ("%d duplicate row%s removed.", removed, removed == 1 ? "" : "s");
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
  gtk_box_append (GTK_BOX (content), table_prompt_checklist (prompt, labels, "Columns that must match", TRUE));
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
  GtkStringList *functions = gtk_string_list_new ((const char *[]) { "Sum", "Count", "Average", "Max", "Min", "Product", NULL });
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
  prompt->function_drop = labelled (grid, 1, _("Use function:"), gtk_drop_down_new (G_LIST_MODEL (functions), NULL));
  gtk_box_append (GTK_BOX (content), grid);
  gtk_box_append (GTK_BOX (content), table_prompt_checklist (prompt, labels, "Add subtotal to", FALSE));
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
  GStrv      fields;
} PivotPrompt;



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

  dest = o42_book_add_sheet (self->book, NULL, -1);
  o42_sheet_add_pivot (dest, &p);
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
  g_ptr_array_add (col_choices, (gpointer) "(none)");
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

/* ---- Data > Validation -------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *kind, *op;
  GtkWidget *value, *value2;
  GtkWidget *message, *blank;
} ValidPrompt;

static const char *VALID_KINDS[] = {
  N_("Any value"), N_("Whole number"), N_("Decimal"), N_("List"), N_("Date"), N_("Time"), N_("Text length"), NULL
};

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

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Error message:")));
  prompt->message = gtk_entry_new ();
  gtk_widget_set_hexpand (prompt->message, TRUE);
  if (existing) gtk_editable_set_text (GTK_EDITABLE (prompt->message), existing->message);
  gtk_box_append (GTK_BOX (row), prompt->message);
  gtk_box_append (GTK_BOX (content), row);

  prompt->blank = gtk_check_button_new_with_mnemonic ( _("Ignore _blank"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->blank), existing ? existing->allow_blank : TRUE);
  gtk_box_append (GTK_BOX (content), prompt->blank);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_valid_ok), prompt);
  dialog_button (buttons, _("_Clear"), G_CALLBACK (on_valid_clear), prompt);
  dialog_button (buttons, _("Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->value);
}

/* ---- Data > Text to Columns -------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *choice[4];
  GtkWidget *other;
} SplitPrompt;

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
  o42_sheet_text_to_columns (self->sheet, &sel, delim);
  o42_grid_refresh (self->grid);
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_text_to_columns (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  SplitPrompt *prompt = g_new0 (SplitPrompt, 1);
  GtkWidget *content, *buttons, *row, *ok;
  static const char *names[4] = { "_Comma", "_Tab", "_Semicolon", "S_pace" };

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Text to Columns"), TRUE, &content, &buttons);
  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Split the selected column at:")));

  for (int i = 0; i < 4; i++)
    {
      prompt->choice[i] = gtk_check_button_new_with_mnemonic (names[i]);
      if (i > 0)
        gtk_check_button_set_group (GTK_CHECK_BUTTON (prompt->choice[i]),
                                    GTK_CHECK_BUTTON (prompt->choice[0]));
      gtk_box_append (GTK_BOX (content), prompt->choice[i]);
    }
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->choice[0]), TRUE);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Other:")));
  prompt->other = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->other), 6);
  gtk_box_append (GTK_BOX (row), prompt->other);
  gtk_box_append (GTK_BOX (content), row);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_split_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}
