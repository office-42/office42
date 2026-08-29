/* o42-window.c - see o42-window.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Excel 5's window, top to bottom: a navy title bar, the menu bar, the
 * Standard and Formatting toolbars, the name box beside the formula bar,
 * the grid, the sheet tabs, and a status bar.  The chrome is the same
 * Windows 3.1 silver word42 wears, because the two programs shipped side by
 * side and looked it.
 */

#include "o42-window-private.h"

/* The helpers the dialog files share are exported under their
 * long names; this file goes on calling them what it always did. */
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
#define scripts_bar_hide o42_scripts_bar_hide
#define window_run_script o42_window_run_script
#define window_tell_book o42_window_tell_book
#define wizard_bind_item o42_wizard_bind_item
#define wizard_setup_item o42_wizard_setup_item
#include "o42-analysis.h"
#include "o42-pattern.h"
#include "o42-spell.h"

#include "o42-grid.h"
#include "o42-image.h"
#include "o42-pdf.h"
#include "o42-sql.h"
#include "o42-csv.h"
#include "o42-text-formats.h"
#include "o42-lotus.h"
#include "o42-gnumeric.h"
#include "o42-xlsx.h"
#include "o42-xls.h"
#include "o42-ods.h"
#include "o42-html.h"
#include "o42-book.h"
#include "o42-eval.h"
#include "o42-formula.h"
#include "o42-python.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <string.h>
#include <locale.h>
#ifdef G_OS_WIN32
#include <windows.h>
#endif

/* A modal dialog stops both itself and its parent from producing render
 * nodes on Windows, so when a picture is being taken they are not modal. */
static gboolean dialogs_modal = TRUE;

void
o42_window_set_dialogs_modal (gboolean modal)
{
  dialogs_modal = modal;
}

/* The sizes Excel 5's Formatting toolbar offered. */
static const int FONT_SIZES[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22,
                                  24, 26, 28, 36, 48, 72 };


G_DEFINE_FINAL_TYPE (O42Window, o42_window, GTK_TYPE_APPLICATION_WINDOW)

static void window_rebuild_tabs (O42Window *self);
static void action_new_window (GSimpleAction *a, GVariant *p, gpointer data);

/* ---------------------------------------------------------------------- */
/* Title bar, drawn by office42 itself                                     */
/* ---------------------------------------------------------------------- */

static void
on_titlebar_minimise (GtkButton *b, gpointer data)
{
  (void) b;
  gtk_window_minimize (GTK_WINDOW (data));
}

static void
on_titlebar_maximise (GtkButton *b, gpointer data)
{
  GtkWindow *w = data;
  (void) b;
  if (gtk_window_is_maximized (w)) gtk_window_unmaximize (w);
  else gtk_window_maximize (w);
}

static void
on_titlebar_close (GtkButton *b, gpointer data)
{
  (void) b;
  gtk_window_close (GTK_WINDOW (data));
}

static void
draw_caption_glyph (GtkDrawingArea *area, cairo_t *cr,
                    int width, int height, gpointer data)
{
  const char *which = data;
  double cx = width / 2.0, cy = height / 2.0;

  (void) area;
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_set_line_width (cr, 1.0);

  if (g_strcmp0 (which, "minimise") == 0)
    {
      cairo_rectangle (cr, cx - 3, cy + 2, 7, 2);
      cairo_fill (cr);
    }
  else if (g_strcmp0 (which, "maximise") == 0)
    {
      cairo_rectangle (cr, cx - 4.5, cy - 4.5, 9, 9);
      cairo_stroke (cr);
      cairo_rectangle (cr, cx - 4.5, cy - 4.5, 9, 2);
      cairo_fill (cr);
    }
  else
    {
      cairo_move_to (cr, cx - 3.5, cy - 3.5); cairo_line_to (cr, cx + 3.5, cy + 3.5);
      cairo_move_to (cr, cx + 3.5, cy - 3.5); cairo_line_to (cr, cx - 3.5, cy + 3.5);
      cairo_set_line_width (cr, 1.4);
      cairo_stroke (cr);
    }
}

static GtkWidget *
caption_button (const char *glyph, const char *tip, GCallback cb, gpointer data)
{
  GtkWidget *button = gtk_button_new ();
  GtkWidget *area = gtk_drawing_area_new ();

  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (area), 14);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (area), 12);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), draw_caption_glyph,
                                  (gpointer) glyph, NULL);
  gtk_button_set_child (GTK_BUTTON (button), area);
  gtk_widget_set_tooltip_text (button, tip);
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_widget_set_focusable (button, FALSE);
  g_signal_connect (button, "clicked", cb, data);

  return button;
}

static GtkWidget *
build_titlebar (O42Window *self)
{
  GtkWidget *handle = gtk_window_handle_new ();
  GtkWidget *centre = gtk_center_box_new ();
  GtkWidget *right = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);

  gtk_widget_add_css_class (centre, "o42-titlebar");

  self->title_label = gtk_label_new ("office42 " O42_VERSION " - Book1");
  gtk_widget_add_css_class (self->title_label, "o42-titlebar-label");
  gtk_center_box_set_center_widget (GTK_CENTER_BOX (centre), self->title_label);

  gtk_box_append (GTK_BOX (right), caption_button ("minimise", "Minimize",
                  G_CALLBACK (on_titlebar_minimise), self));
  gtk_box_append (GTK_BOX (right), caption_button ("maximise", "Maximize",
                  G_CALLBACK (on_titlebar_maximise), self));
  gtk_box_append (GTK_BOX (right), caption_button ("close", "Close",
                  G_CALLBACK (on_titlebar_close), self));
  gtk_center_box_set_end_widget (GTK_CENTER_BOX (centre), right);

  gtk_window_handle_set_child (GTK_WINDOW_HANDLE (handle), centre);
  return handle;
}

/* ---------------------------------------------------------------------- */
/* Actions                                                                 */
/* ---------------------------------------------------------------------- */

#define GRID_ACTION(name, call)                                          \
  static void                                                            \
  action_##name (GSimpleAction *a, GVariant *p, gpointer data)           \
  {                                                                      \
    (void) a; (void) p;                                                  \
    call (O42_WINDOW (data)->grid);                                      \
  }

GRID_ACTION (cut,        o42_grid_cut)
GRID_ACTION (copy,       o42_grid_copy)
GRID_ACTION (paste,      o42_grid_paste)
GRID_ACTION (select_all, o42_grid_select_all)
GRID_ACTION (clear,      o42_grid_delete_selection)
GRID_ACTION (insert_rows,    o42_grid_insert_rows)
GRID_ACTION (insert_columns, o42_grid_insert_columns)
GRID_ACTION (delete_rows,    o42_grid_delete_rows)
GRID_ACTION (delete_columns, o42_grid_delete_columns)

#undef GRID_ACTION

/* Undo and redo may put back a change on another sheet, which is then the
 * sheet to show. */
static void
window_undo_redo (O42Window *self, gboolean undo)
{
  O42Sheet *target = self->sheet;
  O42Range touched;
  gboolean done;

  if (o42_grid_is_editing (self->grid))
    o42_grid_cancel_edit (self->grid);

  done = undo ? o42_sheet_undo_full (self->sheet, &target, &touched)
              : o42_sheet_redo_full (self->sheet, &target, &touched);
  if (!done)
    return;

  if (target != self->sheet)
    window_show_sheet (self, o42_book_sheet_index (self->book, target));

  o42_grid_set_active (self->grid, touched.row0, touched.col0);
  o42_grid_refresh (self->grid);
  window_sync (self);
}

static void action_undo (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; window_undo_redo (d, TRUE); }
static void action_redo (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; window_undo_redo (d, FALSE); }

static void action_fill_down  (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_fill (O42_WINDOW (d)->grid, TRUE);  }
static void action_fill_right (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_fill (O42_WINDOW (d)->grid, FALSE); }

static void
toggle_fmt (O42Window *self, O42FmtMask mask)
{
  const O42Fmt *now = o42_grid_active_fmt (self->grid);
  O42Fmt want;

  if (now == NULL)
    return;

  o42_fmt_init_default (&want);

  switch (mask)
    {
    case O42_FMT_BOLD:      want.bold = !now->bold;           break;
    case O42_FMT_ITALIC:    want.italic = !now->italic;       break;
    case O42_FMT_UNDERLINE: want.underline = !now->underline; break;
    default: return;
    }

  o42_grid_apply_fmt (self->grid, mask, &want);
}

static void action_bold      (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; toggle_fmt (d, O42_FMT_BOLD); }
static void action_italic    (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; toggle_fmt (d, O42_FMT_ITALIC); }
static void action_underline (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; toggle_fmt (d, O42_FMT_UNDERLINE); }

static void
action_align (GSimpleAction *a, GVariant *param, gpointer data)
{
  O42Window *self = data;
  const char *which = g_variant_get_string (param, NULL);
  O42Fmt want;

  (void) a;
  o42_fmt_init_default (&want);

  if (g_strcmp0 (which, "left") == 0)        want.halign = O42_HALIGN_LEFT;
  else if (g_strcmp0 (which, "center") == 0) want.halign = O42_HALIGN_CENTRE;
  else if (g_strcmp0 (which, "right") == 0)  want.halign = O42_HALIGN_RIGHT;
  else                                        want.halign = O42_HALIGN_GENERAL;

  o42_grid_apply_fmt (self->grid, O42_FMT_HALIGN, &want);
}

static void
action_number (GSimpleAction *a, GVariant *param, gpointer data)
{
  O42Window *self = data;
  const char *which = g_variant_get_string (param, NULL);
  O42Fmt want;
  O42FmtMask mask = O42_FMT_NUMBER;

  (void) a;
  o42_fmt_init_default (&want);

  if (g_strcmp0 (which, "currency") == 0)        { want.number = O42_NUM_CURRENCY;   want.decimals = 2; mask |= O42_FMT_DECIMALS; }
  else if (g_strcmp0 (which, "percent") == 0)    { want.number = O42_NUM_PERCENT;    want.decimals = 0; mask |= O42_FMT_DECIMALS; }
  else if (g_strcmp0 (which, "comma") == 0)      { want.number = O42_NUM_COMMA;      want.decimals = 2; mask |= O42_FMT_DECIMALS; }
  else if (g_strcmp0 (which, "fixed") == 0)      { want.number = O42_NUM_FIXED;      want.decimals = 2; mask |= O42_FMT_DECIMALS; }
  else if (g_strcmp0 (which, "scientific") == 0) { want.number = O42_NUM_SCIENTIFIC; want.decimals = 2; mask |= O42_FMT_DECIMALS; }
  else if (g_strcmp0 (which, "date") == 0)       want.number = O42_NUM_DATE;
  else if (g_strcmp0 (which, "time") == 0)       want.number = O42_NUM_TIME;
  else if (g_strcmp0 (which, "datetime") == 0)   want.number = O42_NUM_DATETIME;
  else                                            want.number = O42_NUM_GENERAL;

  o42_grid_apply_fmt (self->grid, mask, &want);
}

/* The two decimal buttons.  A General-formatted cell becomes Fixed at the
 * first press, which is what Excel does too. */
static void
action_decimals (GSimpleAction *a, GVariant *param, gpointer data)
{
  O42Window *self = data;
  const O42Fmt *now = o42_grid_active_fmt (self->grid);
  O42Fmt want;
  int delta = (int) g_variant_get_int32 (param);

  (void) a;
  if (now == NULL)
    return;

  o42_fmt_init_default (&want);
  want.number = (now->number == O42_NUM_GENERAL || now->number == O42_NUM_TEXT)
                  ? O42_NUM_FIXED : now->number;
  want.decimals = CLAMP (now->decimals + delta, 0, 15);

  o42_grid_apply_fmt (self->grid, O42_FMT_NUMBER | O42_FMT_DECIMALS, &want);
}

/* AutoSum: puts =SUM() of the numbers above the active cell into it, which
 * is what the Σ button has done since it first appeared in Excel 4. */
static void
action_autosum (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  int row, col, top;
  char *from, *to, *formula;

  (void) a; (void) p;

  o42_grid_get_active (self->grid, &row, &col);

  /* Walk up while there are numbers. */
  top = row;
  while (top > 0)
    {
      O42Value v;
      gboolean numeric;

      o42_sheet_get_value (self->sheet, top - 1, col, &v);
      numeric = (v.type == O42_VALUE_NUMBER);
      o42_value_clear (&v);

      if (!numeric)
        break;
      top--;
    }

  if (top == row)
    {
      o42_grid_begin_edit (self->grid, "=SUM(");
      return;
    }

  from = o42_ref_name (top, col);
  to = o42_ref_name (row - 1, col);
  formula = g_strdup_printf ("=SUM(%s:%s)", from, to);
  o42_grid_set_active_input (self->grid, formula);

  g_free (formula);
  g_free (from);
  g_free (to);
}

void
o42_window_show_error (O42Window *self, const char *heading, GError *error)
{
  GtkAlertDialog *dialog = gtk_alert_dialog_new ("%s", heading);

  if (error != NULL)
    gtk_alert_dialog_set_detail (dialog, error->message);

  gtk_alert_dialog_show (dialog, GTK_WINDOW (self));
  g_object_unref (dialog);
}

static GtkFileFilter *
pattern_filter (const char *name, const char *pattern)
{
  GtkFileFilter *filter = gtk_file_filter_new ();

  gtk_file_filter_set_name (filter, name);
  gtk_file_filter_add_pattern (filter, pattern);
  return filter;
}

/* ---- Pictures --------------------------------------------------------- */


/* Every format gdk-pixbuf has a loader for, by file extension.  This is
 * what gtk_file_filter_add_pixbuf_formats() did before GTK deprecated it. */
static void
add_picture_patterns (GtkFileFilter *filter)
{
  GSList *formats = gdk_pixbuf_get_formats ();

  for (GSList *l = formats; l != NULL; l = l->next)
    {
      char **extensions = gdk_pixbuf_format_get_extensions (l->data);

      for (guint i = 0; extensions != NULL && extensions[i] != NULL; i++)
        {
          char *pattern = g_strdup_printf ("*.%s", extensions[i]);
          gtk_file_filter_add_pattern (filter, pattern);
          g_free (pattern);
        }

      g_strfreev (extensions);
    }

  g_slist_free (formats);
}

static void
on_picture_response (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      int width = 0, height = 0;
      const char *format = NULL;
      GBytes *bytes = o42_image_load_file (file, &width, &height, &format,
                                           &error);

      if (bytes != NULL)
        {
          o42_grid_insert_picture (self->grid, bytes, format, width, height);
          g_bytes_unref (bytes);
        }
      else
        show_error (self, "office42 could not insert that picture.", error);

      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, "office42 could not open that picture.", error);

  g_clear_error (&error);
  gtk_widget_grab_focus (GTK_WIDGET (self->grid));
}

static void
action_insert_picture (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  GtkFileFilter *pictures = gtk_file_filter_new ();

  (void) a; (void) p;

  gtk_file_filter_set_name (pictures, _("Pictures"));
  add_picture_patterns (pictures);
  g_list_store_append (filters, pictures);

  gtk_file_dialog_set_title (dialog, _("Insert Picture"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL,
                        on_picture_response, self);

  g_object_unref (filters);
  g_object_unref (dialog);
}

/* ---- PDF -------------------------------------------------------------- */

static void
on_export_pdf_response (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      if (!o42_pdf_export (self->sheet, file, &error))
        show_error (self, "office42 could not export the PDF.", error);
      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, "office42 could not export the PDF.", error);

  g_clear_error (&error);
}

static void
action_export_pdf (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  (void) a; (void) p;

  g_list_store_append (filters, pattern_filter ("PDF Documents (*.pdf)", "*.pdf"));
  gtk_file_dialog_set_title (dialog, _("Export as PDF"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_initial_name (dialog, "Book1.pdf");
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL,
                        on_export_pdf_response, self);

  g_object_unref (filters);
  g_object_unref (dialog);
}

static void
on_import_pdf_response (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      if (o42_pdf_import (self->sheet, file, &error))
        o42_grid_refresh (self->grid);
      else
        show_error (self, "office42 could not import that PDF.", error);
      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, "office42 could not open that PDF.", error);

  g_clear_error (&error);
  window_sync (self);
  gtk_widget_grab_focus (GTK_WIDGET (self->grid));
}

/* File > Export Book as PDF: every sheet, one after another. */
static void
on_export_book_pdf_response (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Window *self = data;
  GFile *file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, NULL);
  GError *error = NULL;

  if (file == NULL)
    return;
  if (!o42_pdf_export_book (self->book, file, &error))
    show_error (self, "office42 could not write the PDF.", error);
  g_clear_error (&error);
  g_object_unref (file);
}

static void
action_export_book_pdf (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  (void) a; (void) p;

  g_list_store_append (filters, pattern_filter ("PDF Documents (*.pdf)", "*.pdf"));
  gtk_file_dialog_set_title (dialog, _("Export Book as PDF"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_initial_name (dialog, "Book1.pdf");
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL,
                        on_export_book_pdf_response, self);

  g_object_unref (filters);
  g_object_unref (dialog);
}

static void
action_import_pdf (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  (void) a; (void) p;

  g_list_store_append (filters, pattern_filter ("PDF Documents (*.pdf)", "*.pdf"));
  gtk_file_dialog_set_title (dialog, _("Import from PDF"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL,
                        on_import_pdf_response, self);

  g_object_unref (filters);
  g_object_unref (dialog);
}

/* ---- Column Width and Row Height ------------------------------------- */

/* One small dialog serves both: a label, an entry, OK and Cancel, in the
 * shape Excel 5's had.  The value is in pixels at the grid's 96 dpi. */
typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *entry;
  gboolean   columns;
} SizePrompt;

static void
on_size_prompt_ok (GtkWidget *w, gpointer data)
{
  SizePrompt *prompt = data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (prompt->entry));
  char *end = NULL;
  double value = g_ascii_strtod (text, &end);

  (void) w;

  if (end != text && value > 0)
    {
      if (prompt->columns)
        o42_grid_set_column_width (prompt->window->grid, (int) (value + 0.5));
      else
        o42_grid_set_row_height (prompt->window->grid, (int) (value + 0.5));
    }

  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_size_prompt_cancel (GtkWidget *w, gpointer data)
{
  SizePrompt *prompt = data;
  (void) w;
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_size_prompt_destroy (GtkWidget *w, gpointer data)
{
  SizePrompt *prompt = data;
  (void) w;
  gtk_widget_grab_focus (GTK_WIDGET (prompt->window->grid));
  g_free (prompt);
}

static void
size_prompt (O42Window *self, gboolean columns)
{
  SizePrompt *prompt = g_new0 (SizePrompt, 1);
  GtkWidget *box, *row, *label, *buttons, *ok, *cancel;
  int row_i, col_i, current;
  char initial[16];

  prompt->window = self;
  prompt->columns = columns;

  o42_grid_get_active (self->grid, &row_i, &col_i);
  current = columns ? o42_sheet_col_width (self->sheet, col_i)
                    : o42_sheet_row_height (self->sheet, row_i);
  g_snprintf (initial, sizeof initial, "%d", current);

  prompt->dialog = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (prompt->dialog), columns ? _("Column Width") : _("Row Height"));
  gtk_window_set_transient_for (GTK_WINDOW (prompt->dialog), GTK_WINDOW (self));
  gtk_window_set_modal (GTK_WINDOW (prompt->dialog), dialogs_modal);
  gtk_window_set_resizable (GTK_WINDOW (prompt->dialog), FALSE);
  gtk_widget_add_css_class (prompt->dialog, "o42");

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);
  gtk_window_set_child (GTK_WINDOW (prompt->dialog), box);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  label = gtk_label_new (columns ? "Column Width (pixels):" : "Row Height (pixels):");
  prompt->entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (prompt->entry), initial);
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->entry), 8);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->entry), TRUE);
  gtk_box_append (GTK_BOX (row), label);
  gtk_box_append (GTK_BOX (row), prompt->entry);
  gtk_box_append (GTK_BOX (box), row);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  ok = gtk_button_new_with_mnemonic (_("_OK"));
  cancel = gtk_button_new_with_mnemonic (_("_Cancel"));
  gtk_widget_set_size_request (ok, 80, -1);
  gtk_widget_set_size_request (cancel, 80, -1);
  gtk_box_append (GTK_BOX (buttons), ok);
  gtk_box_append (GTK_BOX (buttons), cancel);
  gtk_box_append (GTK_BOX (box), buttons);

  g_signal_connect (ok, "clicked", G_CALLBACK (on_size_prompt_ok), prompt);
  g_signal_connect (cancel, "clicked", G_CALLBACK (on_size_prompt_cancel), prompt);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_size_prompt_destroy), prompt);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->entry);
  gtk_editable_select_region (GTK_EDITABLE (prompt->entry), 0, -1);
}

static void action_merge_cells   (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_merge_cells (O42_WINDOW (d)->grid, TRUE); }
static void action_unmerge_cells (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_merge_cells (O42_WINDOW (d)->grid, FALSE); }
static void action_hide_rows      (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_hide_rows (O42_WINDOW (d)->grid, TRUE); }
static void action_unhide_rows    (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_hide_rows (O42_WINDOW (d)->grid, FALSE); }
static void action_hide_columns   (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_hide_columns (O42_WINDOW (d)->grid, TRUE); }
static void action_unhide_columns (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_hide_columns (O42_WINDOW (d)->grid, FALSE); }
static void action_filter         (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_toggle_autofilter (O42_WINDOW (d)->grid); }
static void action_column_width (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; size_prompt (d, TRUE); }
static void action_row_height   (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; size_prompt (d, FALSE); }
static void action_autofit      (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_autofit_columns (O42_WINDOW (d)->grid); }

/* ---- Small dialogs: the frame they share ------------------------------ */

/* A transient window with a content box and an OK/Cancel row (or whatever
 * buttons the caller adds): the shape every Excel 5 dialog had. */
GtkWidget *
o42_dialog_frame (O42Window *self, const char *title, gboolean modal,
              GtkWidget **content, GtkWidget **buttons)
{
  GtkWidget *dialog = gtk_window_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);

  gtk_window_set_title (GTK_WINDOW (dialog), title);
  gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (self));
  gtk_window_set_modal (GTK_WINDOW (dialog), modal && dialogs_modal);
  /* Resizable: the Function Wizard, the scripts and the console are
   * all better for being pulled bigger, and a dialog that refuses is
   * an annoyance with nothing to say for it. */
  gtk_window_set_resizable (GTK_WINDOW (dialog), TRUE);
  gtk_widget_add_css_class (dialog, "o42");

  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);
  gtk_window_set_child (GTK_WINDOW (dialog), box);

  *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_append (GTK_BOX (box), *content);

  *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (*buttons, GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (box), *buttons);

  return dialog;
}

GtkWidget *
o42_dialog_button (GtkWidget *buttons, const char *label, GCallback cb, gpointer data)
{
  GtkWidget *button = gtk_button_new_with_mnemonic (label);

  gtk_widget_set_size_request (button, 84, -1);
  gtk_box_append (GTK_BOX (buttons), button);
  g_signal_connect (button, "clicked", cb, data);
  return button;
}

void
o42_dialog_close_clicked (GtkWidget *w, gpointer dialog)
{
  (void) w;
  gtk_window_destroy (GTK_WINDOW (dialog));
}

void
o42_dialog_destroy_refocus (GtkWidget *w, gpointer grid)
{
  (void) w;
  gtk_widget_grab_focus (GTK_WIDGET (grid));
}

/* ---- Paste Special ------------------------------------------------------ */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *mode[4];
  GtkWidget *transpose;
} PastePrompt;

static void
on_paste_special_ok (GtkWidget *w, gpointer data)
{
  PastePrompt *prompt = data;
  O42PasteMode mode = O42_PASTE_ALL;

  (void) w;
  for (int i = 0; i < 4; i++)
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->mode[i])))
      mode = (O42PasteMode) i;

  o42_grid_paste_special (prompt->window->grid, mode,
                          gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->transpose)));
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
action_paste_special (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  PastePrompt *prompt;
  GtkWidget *content, *buttons, *ok;
  static const char *names[4] = { "_All", "_Values", "_Formats", "F_ormulas" };

  (void) a; (void) p;

  if (!o42_grid_has_own_copy (self->grid))
    {
      show_error (self, "Copy some cells first; Paste Special works on office42's own copy.", NULL);
      return;
    }

  prompt = g_new0 (PastePrompt, 1);
  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Paste Special"), TRUE, &content, &buttons);

  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Paste")));
  for (int i = 0; i < 4; i++)
    {
      prompt->mode[i] = gtk_check_button_new_with_mnemonic (names[i]);
      if (i > 0)
        gtk_check_button_set_group (GTK_CHECK_BUTTON (prompt->mode[i]),
                                    GTK_CHECK_BUTTON (prompt->mode[0]));
      gtk_box_append (GTK_BOX (content), prompt->mode[i]);
    }
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->mode[0]), TRUE);
  prompt->transpose = gtk_check_button_new_with_mnemonic ( _("Transpos_e"));
  gtk_box_append (GTK_BOX (content), prompt->transpose);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_paste_special_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Find and Replace ------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *find_entry;
  GtkWidget *replace_entry;    /* NULL in the Find dialog */
  GtkWidget *match_case;
  GtkWidget *whole_cell;
  GtkWidget *status;
} FindPrompt;

static void
on_find_next (GtkWidget *w, gpointer data)
{
  FindPrompt *prompt = data;
  const char *needle = gtk_editable_get_text (GTK_EDITABLE (prompt->find_entry));
  int row, col;

  (void) w;

  if (*needle == '\0')
    return;

  o42_grid_get_active (prompt->window->grid, &row, &col);

  if (o42_sheet_find (prompt->window->sheet, needle,
                      gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->match_case)),
                      gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->whole_cell)),
                      &row, &col))
    {
      o42_grid_set_active (prompt->window->grid, row, col);
      gtk_label_set_text (GTK_LABEL (prompt->status), "");
    }
  else
    gtk_label_set_text (GTK_LABEL (prompt->status), _("office42 cannot find the text you asked for."));
}

/* Replace: the active cell if it matches, then on to the next match. */
static void
on_replace_one (GtkWidget *w, gpointer data)
{
  FindPrompt *prompt = data;
  const char *needle = gtk_editable_get_text (GTK_EDITABLE (prompt->find_entry));
  const char *with = gtk_editable_get_text (GTK_EDITABLE (prompt->replace_entry));
  int row, col;
  O42Range one;

  (void) w;

  if (*needle == '\0')
    return;

  o42_grid_get_active (prompt->window->grid, &row, &col);
  one.row0 = one.row1 = row;
  one.col0 = one.col1 = col;
  if (o42_sheet_replace (prompt->window->sheet, &one, needle, with,
                         gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->match_case))) > 0)
    o42_grid_refresh (prompt->window->grid);

  on_find_next (w, data);
}

static void
on_replace_all (GtkWidget *w, gpointer data)
{
  FindPrompt *prompt = data;
  const char *needle = gtk_editable_get_text (GTK_EDITABLE (prompt->find_entry));
  const char *with = gtk_editable_get_text (GTK_EDITABLE (prompt->replace_entry));
  O42Range sel;
  int count;
  char *message;

  (void) w;

  if (*needle == '\0')
    return;

  /* Within the selection if there is one, else the whole sheet. */
  o42_grid_get_selection (prompt->window->grid, &sel);
  count = o42_sheet_replace (prompt->window->sheet,
                             (sel.row0 != sel.row1 || sel.col0 != sel.col1) ? &sel : NULL,
                             needle, with,
                             gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->match_case)));

  message = g_strdup_printf ("%d replacement%s made.", count, count == 1 ? "" : "s");
  gtk_label_set_text (GTK_LABEL (prompt->status), message);
  g_free (message);

  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
}

static void
find_prompt (O42Window *self, gboolean with_replace)
{
  FindPrompt *prompt = g_new0 (FindPrompt, 1);
  GtkWidget *content, *buttons, *grid, *next;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, with_replace ? _("Replace") : _("Find"), FALSE,
                                 &content, &buttons);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);

  gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Find what:")), 0, 0, 1, 1);
  prompt->find_entry = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->find_entry), 28);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->find_entry), TRUE);
  gtk_grid_attach (GTK_GRID (grid), prompt->find_entry, 1, 0, 1, 1);

  if (with_replace)
    {
      gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Replace with:")), 0, 1, 1, 1);
      prompt->replace_entry = gtk_entry_new ();
      gtk_editable_set_width_chars (GTK_EDITABLE (prompt->replace_entry), 28);
      gtk_grid_attach (GTK_GRID (grid), prompt->replace_entry, 1, 1, 1, 1);
    }
  gtk_box_append (GTK_BOX (content), grid);

  prompt->match_case = gtk_check_button_new_with_mnemonic ( _("Match _case"));
  prompt->whole_cell = gtk_check_button_new_with_mnemonic ( _("Find entire cells _only"));
  gtk_box_append (GTK_BOX (content), prompt->match_case);
  gtk_box_append (GTK_BOX (content), prompt->whole_cell);

  prompt->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (prompt->status), 0.0);
  gtk_box_append (GTK_BOX (content), prompt->status);

  next = dialog_button (buttons, _("_Find Next"), G_CALLBACK (on_find_next), prompt);
  if (with_replace)
    {
      dialog_button (buttons, _("_Replace"), G_CALLBACK (on_replace_one), prompt);
      dialog_button (buttons, _("Replace _All"), G_CALLBACK (on_replace_all), prompt);
    }
  dialog_button (buttons, _("Close"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), next);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->find_entry);
}

static void action_find    (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; find_prompt (d, FALSE); }
static void action_replace (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; find_prompt (d, TRUE); }

/* ---- Format Cells ----------------------------------------------------- */

/* Excel 5's Format Cells: one dialog, tabbed, holding everything a cell's
 * format can be.  Every control starts from the active cell and the whole
 * format is applied to the selection on OK. */
typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *number, *decimals, *custom;
  GtkWidget *halign, *valign, *wrap;
  GtkWidget *family, *size, *bold, *italic, *underline, *strikeout, *colour;
  GtkWidget *border[4];
  GtkWidget *border_style[4], *border_colour;
  GtkWidget *indent, *rotation;
  GtkWidget *no_fill, *fill, *pattern, *pattern_colour;
  GtkWidget *locked, *hidden;
} FormatPrompt;

static const char *BORDER_STYLE_NAMES[] = { N_("None"), N_("Thin"), N_("Medium"), N_("Thick"), N_("Double"), N_("Dashed"), N_("Dotted"), NULL };

static const O42NumberFormat NUMBER_CHOICES[] = {
  O42_NUM_GENERAL, O42_NUM_FIXED, O42_NUM_COMMA, O42_NUM_CURRENCY,
  O42_NUM_PERCENT, O42_NUM_SCIENTIFIC, O42_NUM_TEXT, O42_NUM_DATE,
  O42_NUM_TIME, O42_NUM_DATETIME,
};
static const char *NUMBER_NAMES[] = {
  N_("General"), N_("Fixed"), N_("Comma"), N_("Currency"), N_("Percent"), N_("Scientific"), N_("Text"),
  N_("Date"), N_("Time"), N_("Date and Time"), N_("Custom"), NULL,
};
static const char *HALIGN_NAMES[] = { N_("General"), N_("Left"), N_("Center"), N_("Right"), NULL };
static const char *VALIGN_NAMES[] = { N_("Bottom"), N_("Middle"), N_("Top"), NULL };

void
o42_rgba_from_colour (guint32 colour, GdkRGBA *rgba)
{
  rgba->red   = ((colour >> 16) & 0xff) / 255.0;
  rgba->green = ((colour >> 8) & 0xff) / 255.0;
  rgba->blue  = (colour & 0xff) / 255.0;
  rgba->alpha = 1.0;
}

guint32
o42_colour_from_rgba (const GdkRGBA *rgba)
{
  return ((guint32) (rgba->red * 255 + 0.5) << 16) |
         ((guint32) (rgba->green * 255 + 0.5) << 8) |
          (guint32) (rgba->blue * 255 + 0.5);
}

GtkWidget *
o42_labelled (GtkWidget *grid, int row, const char *label, GtkWidget *control)
{
  GtkWidget *l = gtk_label_new (label);

  gtk_label_set_xalign (GTK_LABEL (l), 0.0);
  gtk_grid_attach (GTK_GRID (grid), l, 0, row, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), control, 1, row, 1, 1);
  return control;
}

GtkWidget *
o42_page_grid (GtkWidget *notebook, const char *title)
{
  GtkWidget *grid = gtk_grid_new ();

  gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  gtk_widget_set_margin_top (grid, 12);
  gtk_widget_set_margin_bottom (grid, 12);
  gtk_widget_set_margin_start (grid, 12);
  gtk_widget_set_margin_end (grid, 12);
  gtk_notebook_append_page (GTK_NOTEBOOK (notebook), grid, gtk_label_new (title));
  return grid;
}

static void
on_format_ok (GtkWidget *w, gpointer data)
{
  FormatPrompt *prompt = data;
  O42Fmt fmt;
  guint index;
  GtkStringObject *item;
  const GdkRGBA *rgba;

  (void) w;
  o42_fmt_init_default (&fmt);

  index = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->number));
  fmt.number = (index < G_N_ELEMENTS (NUMBER_CHOICES)) ? NUMBER_CHOICES[index] : O42_NUM_GENERAL;
  fmt.decimals = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->decimals));
  fmt.custom = NULL;
  if (index == G_N_ELEMENTS (NUMBER_CHOICES))
    {
      const char *code = gtk_editable_get_text (GTK_EDITABLE (prompt->custom));
      if (*code != '\0' && g_ascii_strcasecmp (code, "General") != 0)
        fmt.custom = g_intern_string (code);
    }

  fmt.halign = (O42HAlign) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->halign));
  switch (gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->valign)))
    {
    case 1:  fmt.valign = O42_VALIGN_MIDDLE; break;
    case 2:  fmt.valign = O42_VALIGN_TOP;    break;
    default: fmt.valign = O42_VALIGN_BOTTOM; break;
    }
  fmt.wrap = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->wrap));

  item = gtk_drop_down_get_selected_item (GTK_DROP_DOWN (prompt->family));
  if (item != NULL)
    fmt.family = g_intern_string (gtk_string_object_get_string (item));
  fmt.size = (int) (gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->size)) * 2 + 0.5);
  fmt.bold = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->bold));
  fmt.italic = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->italic));
  fmt.underline = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->underline));
  fmt.strikeout = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->strikeout));
  rgba = gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->colour));
  fmt.colour = colour_from_rgba (rgba);

  {
    guint32 bc = colour_from_rgba (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->border_colour)));
    for (int i = 0; i < 4; i++)
      {
        guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->border_style[i]));
        fmt.border_style[i] = sel == GTK_INVALID_LIST_POSITION ? O42_BORDER_NONE : (O42BorderStyle) sel;
        fmt.border_colour[i] = bc;
      }
    o42_fmt_sync_borders (&fmt);
  }
  fmt.pattern = (guint8) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->pattern));
  fmt.pattern_colour = colour_from_rgba (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->pattern_colour)));
  fmt.locked = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->locked));
  fmt.hidden = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->hidden));
  fmt.indent = (guint8) gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->indent));
  fmt.rotation = (gint16) gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->rotation));

  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->no_fill)))
    fmt.fill = O42_FILL_NONE;
  else
    fmt.fill = colour_from_rgba (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->fill)));

  o42_grid_apply_fmt (prompt->window->grid,
                      O42_FMT_FAMILY | O42_FMT_SIZE | O42_FMT_BOLD | O42_FMT_ITALIC |
                      O42_FMT_UNDERLINE | O42_FMT_STRIKEOUT | O42_FMT_COLOUR |
                      O42_FMT_FILL | O42_FMT_HALIGN | O42_FMT_VALIGN |
                      O42_FMT_NUMBER | O42_FMT_DECIMALS | O42_FMT_WRAP |
                      O42_FMT_BORDERS | O42_FMT_INDENT | O42_FMT_ROTATION |
                      O42_FMT_PROTECTION | O42_FMT_PATTERN,
                      &fmt);

  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static GtkWidget *
colour_button (guint32 colour, const char *title)
{
  GtkColorDialog *dialog = gtk_color_dialog_new ();
  GtkWidget *button;
  GdkRGBA rgba;

  gtk_color_dialog_set_title (dialog, title);
  gtk_color_dialog_set_with_alpha (dialog, FALSE);
  button = gtk_color_dialog_button_new (dialog);
  rgba_from_colour (colour, &rgba);
  gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (button), &rgba);
  return button;
}

static void
action_format_cells (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  FormatPrompt *prompt = g_new0 (FormatPrompt, 1);
  const O42Fmt *fmt = o42_grid_active_fmt (self->grid);
  GtkWidget *content, *buttons, *notebook, *page, *ok;
  O42Fmt fallback;

  (void) a; (void) p;

  if (fmt == NULL)
    {
      o42_fmt_init_default (&fallback);
      fmt = &fallback;
    }

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Format Cells"), TRUE, &content, &buttons);
  notebook = gtk_notebook_new ();
  gtk_box_append (GTK_BOX (content), notebook);

  /* Number */
  page = page_grid (notebook, _("Number"));
  prompt->number = labelled (page, 0, _("Category:"), drop_down_of (NUMBER_NAMES));
  for (guint i = 0; i < G_N_ELEMENTS (NUMBER_CHOICES); i++)
    if (NUMBER_CHOICES[i] == fmt->number)
      gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->number), i);
  if (fmt->custom != NULL)
    gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->number), G_N_ELEMENTS (NUMBER_CHOICES));
  prompt->decimals = labelled (page, 1, _("Decimal places:"), gtk_spin_button_new_with_range (0, 15, 1));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->decimals), fmt->decimals);
  prompt->custom = labelled (page, 2, _("Format code:"), gtk_entry_new ());
  {
    char *code = o42_fmt_format_string (fmt);
    gtk_editable_set_text (GTK_EDITABLE (prompt->custom), code);
    g_free (code);
  }
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->custom), 24);

  /* Alignment */
  page = page_grid (notebook, _("Alignment"));
  prompt->halign = labelled (page, 0, _("Horizontal:"), drop_down_of (HALIGN_NAMES));
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->halign), (guint) fmt->halign);
  prompt->valign = labelled (page, 1, _("Vertical:"), drop_down_of (VALIGN_NAMES));
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->valign),
                              fmt->valign == O42_VALIGN_MIDDLE ? 1 : fmt->valign == O42_VALIGN_TOP ? 2 : 0);
  prompt->wrap = gtk_check_button_new_with_mnemonic ( _("_Wrap text"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->wrap), fmt->wrap);
  gtk_grid_attach (GTK_GRID (page), prompt->wrap, 0, 2, 2, 1);
  prompt->indent = labelled (page, 3, _("Indent:"), gtk_spin_button_new_with_range (0, 15, 1));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->indent), fmt->indent);
  prompt->rotation = labelled (page, 4, _("Orientation (degrees):"), gtk_spin_button_new_with_range (-90, 90, 5));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->rotation), fmt->rotation);

  /* Font */
  page = page_grid (notebook, _("Font"));
  prompt->family = labelled (page, 0, _("Font:"), gtk_drop_down_new (g_object_ref (self->families), NULL));
  gtk_drop_down_set_enable_search (GTK_DROP_DOWN (prompt->family), TRUE);
  if (fmt->family != NULL)
    {
      gpointer found = g_hash_table_lookup (self->family_index, fmt->family);
      if (found != NULL)
        gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->family), GPOINTER_TO_UINT (found) - 1);
    }
  prompt->size = labelled (page, 1, _("Size:"), gtk_spin_button_new_with_range (4, 144, 1));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->size), fmt->size / 2.0);
  prompt->bold = gtk_check_button_new_with_mnemonic ( _("_Bold"));
  prompt->italic = gtk_check_button_new_with_mnemonic ( _("_Italic"));
  prompt->underline = gtk_check_button_new_with_mnemonic ( _("_Underline"));
  prompt->strikeout = gtk_check_button_new_with_mnemonic ( _("Stri_kethrough"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->bold), fmt->bold);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->italic), fmt->italic);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->underline), fmt->underline);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->strikeout), fmt->strikeout);
  gtk_grid_attach (GTK_GRID (page), prompt->bold, 0, 2, 1, 1);
  gtk_grid_attach (GTK_GRID (page), prompt->italic, 1, 2, 1, 1);
  gtk_grid_attach (GTK_GRID (page), prompt->underline, 0, 3, 1, 1);
  gtk_grid_attach (GTK_GRID (page), prompt->strikeout, 1, 3, 1, 1);
  prompt->colour = labelled (page, 4, _("Color:"), colour_button (fmt->colour, _("Font Color")));

  /* Border: a style per side, one colour for all. */
  page = page_grid (notebook, _("Border"));
  {
    static const char *names[4] = { "Top:", "Bottom:", "Left:", "Right:" };

    for (int i = 0; i < 4; i++)
      {
        prompt->border_style[i] = labelled (page, i, names[i], drop_down_of (BORDER_STYLE_NAMES));
        gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->border_style[i]), fmt->border_style[i]);
        prompt->border[i] = prompt->border_style[i];
      }
    prompt->border_colour = labelled (page, 4, _("Color:"), colour_button (fmt->border_colour[0], _("Border Color")));
  }

  /* Protection */
  page = page_grid (notebook, _("Protection"));
  prompt->locked = gtk_check_button_new_with_mnemonic ( _("_Locked"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->locked), fmt->locked);
  gtk_grid_attach (GTK_GRID (page), prompt->locked, 0, 0, 2, 1);
  prompt->hidden = gtk_check_button_new_with_mnemonic ( _("_Hidden"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->hidden), fmt->hidden);
  gtk_grid_attach (GTK_GRID (page), prompt->hidden, 0, 1, 2, 1);
  {
    GtkWidget *hint = gtk_label_new ("Both take effect only while the sheet is protected, "
                                     "under Tools > Protect Sheet.");
    gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (hint), 34);
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_grid_attach (GTK_GRID (page), hint, 0, 2, 2, 1);
  }

  /* Patterns */
  page = page_grid (notebook, _("Patterns"));
  prompt->no_fill = gtk_check_button_new_with_mnemonic ( _("_No shading"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->no_fill), fmt->fill == O42_FILL_NONE);
  gtk_grid_attach (GTK_GRID (page), prompt->no_fill, 0, 0, 2, 1);
  prompt->fill = labelled (page, 1, "Shading:",
                           colour_button (fmt->fill != O42_FILL_NONE ? fmt->fill : 0xFFFF99, _("Cell Shading")));
  {
    /* In the order of O42Pattern. */
    static const char *const patterns[] = {
      N_("None"), N_("Solid"), N_("75% grey"), N_("50% grey"), N_("25% grey"), N_("12.5% grey"), N_("6.25% grey"),
      N_("Horizontal"), N_("Vertical"), N_("Diagonal down"), N_("Diagonal up"), N_("Grid"), N_("Trellis"),
      N_("Thin horizontal"), N_("Thin vertical"), N_("Thin diagonal down"), N_("Thin diagonal up"),
      N_("Thin grid"), N_("Thin trellis"), NULL
    };

    prompt->pattern = labelled (page, 2, _("Pattern:"), drop_down_of (patterns));
    gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->pattern), fmt->pattern);
    prompt->pattern_colour = labelled (page, 3, "Pattern colour:",
                                       colour_button (fmt->pattern_colour, _("Pattern Colour")));
  }

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_format_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Format > Conditional Formatting ----------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *op;
  GtkWidget *value, *value2;
  GtkWidget *bold, *italic;
  GtkWidget *colour, *fill, *use_fill;
} CondPrompt;

const char *O42_COND_NAMES[] = {
  N_("between"), N_("not between"), N_("equal to"), N_("not equal to"), N_("greater than"),
  N_("less than"), N_("greater than or equal to"), N_("less than or equal to"), NULL
};

static void
on_cond_ok (GtkWidget *w, gpointer data)
{
  CondPrompt *prompt = data;
  O42Window *self = prompt->window;
  O42Condition c;

  (void) w;
  memset (&c, 0, sizeof c);
  o42_grid_get_selection (self->grid, &c.range);
  c.op = (O42CondOp) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->op));
  c.value = g_ascii_strtod (gtk_editable_get_text (GTK_EDITABLE (prompt->value)), NULL);
  c.value2 = g_ascii_strtod (gtk_editable_get_text (GTK_EDITABLE (prompt->value2)), NULL);
  o42_fmt_init_default (&c.fmt);

  c.fmt.bold = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->bold));
  c.fmt.italic = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->italic));
  c.fmt.colour = colour_from_rgba (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->colour)));
  c.mask = O42_FMT_BOLD | O42_FMT_ITALIC | O42_FMT_COLOUR;
  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->use_fill)))
    {
      c.fmt.fill = colour_from_rgba (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->fill)));
      c.mask |= O42_FMT_FILL;
    }

  o42_sheet_add_condition (self->sheet, &c);
  o42_grid_refresh (self->grid);
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_cond_clear (GtkWidget *w, gpointer data)
{
  CondPrompt *prompt = data;
  O42Range sel;

  (void) w;
  o42_grid_get_selection (prompt->window->grid, &sel);
  o42_sheet_clear_conditions (prompt->window->sheet, &sel);
  o42_grid_refresh (prompt->window->grid);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
action_conditional (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  CondPrompt *prompt = g_new0 (CondPrompt, 1);
  GtkWidget *content, *buttons, *row, *ok;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Conditional Formatting"), TRUE, &content, &buttons);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Cell value is")));
  prompt->op = drop_down_of (O42_COND_NAMES);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->op), O42_COND_GREATER);
  gtk_box_append (GTK_BOX (row), prompt->op);
  prompt->value = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->value), 8);
  gtk_box_append (GTK_BOX (row), prompt->value);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("and")));
  prompt->value2 = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->value2), 8);
  gtk_box_append (GTK_BOX (row), prompt->value2);
  gtk_box_append (GTK_BOX (content), row);

  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Then show the cell as:")));
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  prompt->bold = gtk_check_button_new_with_mnemonic ( _("_Bold"));
  prompt->italic = gtk_check_button_new_with_mnemonic ( _("_Italic"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->bold), TRUE);
  gtk_box_append (GTK_BOX (row), prompt->bold);
  gtk_box_append (GTK_BOX (row), prompt->italic);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Colour:")));
  prompt->colour = colour_button (0xC00000, _("Text Colour"));
  gtk_box_append (GTK_BOX (row), prompt->colour);
  gtk_box_append (GTK_BOX (content), row);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  prompt->use_fill = gtk_check_button_new_with_mnemonic ( _("_Shading:"));
  gtk_box_append (GTK_BOX (row), prompt->use_fill);
  prompt->fill = colour_button (0xFFFF99, _("Cell Shading"));
  gtk_box_append (GTK_BOX (row), prompt->fill);
  gtk_box_append (GTK_BOX (content), row);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_cond_ok), prompt);
  dialog_button (buttons, _("_Clear"), G_CALLBACK (on_cond_clear), prompt);
  dialog_button (buttons, _("Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->value);
}

/* ---- Insert > Note ----------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *view;
  int        row, col;
} NotePrompt;

static void
on_note_ok (GtkWidget *w, gpointer data)
{
  NotePrompt *prompt = data;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->view));
  GtkTextIter a, b;
  char *text;

  (void) w;
  gtk_text_buffer_get_bounds (buffer, &a, &b);
  text = gtk_text_buffer_get_text (buffer, &a, &b, FALSE);
  o42_sheet_set_note (prompt->window->sheet, prompt->row, prompt->col, text);
  g_free (text);
  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
action_insert_note (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  NotePrompt *prompt = g_new0 (NotePrompt, 1);
  GtkWidget *content, *buttons, *scrolled, *ok;
  const char *existing;
  char *name, *heading;

  (void) a; (void) p;

  prompt->window = self;
  o42_grid_get_active (self->grid, &prompt->row, &prompt->col);
  existing = o42_sheet_get_note (self->sheet, prompt->row, prompt->col);

  name = o42_ref_name (prompt->row, prompt->col);
  heading = g_strdup_printf ("Note on %s:", name);
  prompt->dialog = dialog_frame (self, _("Cell Note"), TRUE, &content, &buttons);
  gtk_box_append (GTK_BOX (content), gtk_label_new (heading));
  g_free (heading);
  g_free (name);

  prompt->view = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (prompt->view), GTK_WRAP_WORD);
  if (existing != NULL)
    gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->view)), existing, -1);
  scrolled = gtk_scrolled_window_new ();
  gtk_widget_set_size_request (scrolled, 320, 120);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), prompt->view);
  gtk_box_append (GTK_BOX (content), scrolled);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_note_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->view);
}

/* ---- Insert > Hyperlink ------------------------------------------------ */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *entry;
  int        row, col;
} LinkPrompt;

static void
on_link_ok (GtkWidget *w, gpointer data)
{
  LinkPrompt *prompt = data;
  (void) w;
  o42_sheet_set_link (prompt->window->sheet, prompt->row, prompt->col,
                      gtk_editable_get_text (GTK_EDITABLE (prompt->entry)));
  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_link_remove (GtkWidget *w, gpointer data)
{
  LinkPrompt *prompt = data;
  (void) w;
  o42_sheet_set_link (prompt->window->sheet, prompt->row, prompt->col, NULL);
  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
action_insert_link (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  LinkPrompt *prompt = g_new0 (LinkPrompt, 1);
  GtkWidget *content, *buttons, *ok, *hint;
  const char *existing;
  char *name, *heading;

  (void) a; (void) p;
  prompt->window = self;
  o42_grid_get_active (self->grid, &prompt->row, &prompt->col);
  existing = o42_sheet_get_link (self->sheet, prompt->row, prompt->col);

  name = o42_ref_name (prompt->row, prompt->col);
  heading = g_strdup_printf ("Link on %s to:", name);
  prompt->dialog = dialog_frame (self, _("Hyperlink"), TRUE, &content, &buttons);
  gtk_box_append (GTK_BOX (content), gtk_label_new (heading));
  g_free (heading);
  g_free (name);

  prompt->entry = gtk_entry_new ();
  gtk_widget_set_size_request (prompt->entry, 360, -1);
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->entry), _("https://example.org, mailto:someone, or #Sheet2!B4"));
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->entry), TRUE);
  if (existing != NULL)
    gtk_editable_set_text (GTK_EDITABLE (prompt->entry), existing);
  gtk_box_append (GTK_BOX (content), prompt->entry);
  hint = gtk_label_new (_("Ctrl+click the cell to follow the link."));
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_box_append (GTK_BOX (content), hint);

  if (existing != NULL)
    dialog_button (buttons, _("_Remove"), G_CALLBACK (on_link_remove), prompt);
  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_link_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->entry);
}

/* ---- Insert > Name > Define ------------------------------------------- */


typedef struct {
  O42Window     *window;
  GtkWidget     *dialog;
  GtkWidget     *list;
  GtkStringList *names;
  GtkWidget     *name_entry;
  GtkWidget     *refers_entry;
} NamePrompt;

static void
name_prompt_fill (NamePrompt *prompt)
{
  GList *names = o42_book_names (prompt->window->book);

  gtk_string_list_splice (prompt->names, 0,
                          g_list_model_get_n_items (G_LIST_MODEL (prompt->names)), NULL);
  for (GList *l = names; l != NULL; l = l->next)
    gtk_string_list_append (prompt->names, l->data);
  g_list_free (names);
}

static void
on_name_selected (GObject *model, GParamSpec *pspec, gpointer data)
{
  NamePrompt *prompt = data;
  guint pos = gtk_single_selection_get_selected (GTK_SINGLE_SELECTION (model));
  const char *name;
  O42Sheet *target;
  O42Range range;

  (void) pspec;

  if (pos == GTK_INVALID_LIST_POSITION)
    return;
  name = gtk_string_list_get_string (prompt->names, pos);
  gtk_editable_set_text (GTK_EDITABLE (prompt->name_entry), name);

  if (o42_book_lookup_name (prompt->window->book, name, &target, &range))
    {
      char *sheet = o42_sheet_name_quote (o42_sheet_get_name (target));
      char *a = o42_ref_name_full (range.row0, range.col0, TRUE, TRUE);
      char *b = o42_ref_name_full (range.row1, range.col1, TRUE, TRUE);
      char *text = g_strdup_printf ("=%s!%s:%s", sheet, a, b);

      gtk_editable_set_text (GTK_EDITABLE (prompt->refers_entry), text);
      g_free (sheet); g_free (a); g_free (b); g_free (text);
    }
}

static void
on_name_add (GtkWidget *w, gpointer data)
{
  NamePrompt *prompt = data;
  O42Window *self = prompt->window;
  const char *name = gtk_editable_get_text (GTK_EDITABLE (prompt->name_entry));
  const char *refers = gtk_editable_get_text (GTK_EDITABLE (prompt->refers_entry));
  O42Node *tree = o42_formula_parse (refers[0] == '=' ? refers + 1 : refers);
  O42Sheet *target = self->sheet;
  O42Range range;
  gboolean ok = FALSE;

  (void) w;

  if (tree->type == O42_NODE_RANGE)
    { range = tree->as.range; ok = TRUE; }
  else if (tree->type == O42_NODE_REF)
    {
      range.row0 = range.row1 = tree->as.ref.row;
      range.col0 = range.col1 = tree->as.ref.col;
      ok = TRUE;
    }
  if (ok && tree->sheet != NULL)
    {
      target = o42_book_find_sheet (self->book, tree->sheet);
      ok = (target != NULL);
    }
  o42_node_free (tree);

  if (!ok || !o42_book_define_name (self->book, name, target, &range))
    {
      show_error (self, "A name needs letters, digits and underscores, and refers to a range such as =Sheet1!$A$1:$B$9.", NULL);
      return;
    }

  name_prompt_fill (prompt);
  o42_grid_refresh (self->grid);
  window_sync (self);
}

static void
on_name_delete (GtkWidget *w, gpointer data)
{
  NamePrompt *prompt = data;
  const char *name = gtk_editable_get_text (GTK_EDITABLE (prompt->name_entry));

  (void) w;
  o42_book_undefine_name (prompt->window->book, name);
  name_prompt_fill (prompt);
  gtk_editable_set_text (GTK_EDITABLE (prompt->name_entry), "");
  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
}

static void
action_define_name (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  NamePrompt *prompt = g_new0 (NamePrompt, 1);
  GtkWidget *content, *buttons, *scrolled, *grid;
  GtkListItemFactory *factory;
  GtkSingleSelection *selection;
  O42Range sel;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Define Name"), TRUE, &content, &buttons);

  prompt->names = gtk_string_list_new (NULL);
  name_prompt_fill (prompt);
  factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (wizard_setup_item), prompt);
  g_signal_connect (factory, "bind", G_CALLBACK (wizard_bind_item), prompt);
  selection = gtk_single_selection_new (G_LIST_MODEL (prompt->names));
  gtk_single_selection_set_autoselect (selection, FALSE);
  prompt->list = gtk_list_view_new (GTK_SELECTION_MODEL (selection), factory);

  scrolled = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (scrolled, 320, 140);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), prompt->list);
  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Names in Workbook:")));
  gtk_box_append (GTK_BOX (content), scrolled);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->name_entry = labelled (grid, 0, _("Name:"), gtk_entry_new ());
  prompt->refers_entry = labelled (grid, 1, _("Refers to:"), gtk_entry_new ());
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->refers_entry), 28);
  gtk_box_append (GTK_BOX (content), grid);

  /* The selection, as the starting point for a new name. */
  o42_grid_get_selection (self->grid, &sel);
  {
    char *sheet = o42_sheet_name_quote (o42_sheet_get_name (self->sheet));
    char *a1 = o42_ref_name_full (sel.row0, sel.col0, TRUE, TRUE);
    char *b1 = o42_ref_name_full (sel.row1, sel.col1, TRUE, TRUE);
    char *text = g_strdup_printf ("=%s!%s:%s", sheet, a1, b1);
    gtk_editable_set_text (GTK_EDITABLE (prompt->refers_entry), text);
    g_free (sheet); g_free (a1); g_free (b1); g_free (text);
  }

  g_signal_connect (selection, "notify::selected", G_CALLBACK (on_name_selected), prompt);
  dialog_button (buttons, _("_Add"), G_CALLBACK (on_name_add), prompt);
  dialog_button (buttons, _("_Delete"), G_CALLBACK (on_name_delete), prompt);
  dialog_button (buttons, _("Close"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->name_entry);
}

/* ---- View > Freeze Panes ---------------------------------------------- */

static void
action_freeze_panes (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  (void) a; (void) p;
  o42_grid_freeze_panes (self->grid);
  gtk_widget_grab_focus (GTK_WIDGET (self->grid));
}

/* Window > Split divides the view above and left of the active cell
 * into panes that scroll on their own; again puts it back. */
static void
action_split_panes (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  (void) a; (void) p;
  o42_grid_split_panes (self->grid);
  window_sync (self);
  gtk_label_set_text (GTK_LABEL (self->status_label),
                      o42_grid_is_split (self->grid)
                      ? "The window is split: the mouse wheel scrolls the pane it is over."
                      : "The split is gone.");
}

void
o42_window_select_cell (O42Window *self, int row, int col)
{
  g_return_if_fail (O42_IS_WINDOW (self));
  o42_grid_set_active (self->grid, row, col);
}

/* ---- View > Zoom ------------------------------------------------------ */

static void
action_zoom (GSimpleAction *a, GVariant *param, gpointer data)
{
  O42Window *self = data;
  int percent = (int) g_variant_get_int32 (param);

  (void) a;
  o42_grid_set_zoom (self->grid, percent / 100.0);
  window_sync (self);
  gtk_widget_grab_focus (GTK_WIDGET (self->grid));
}

/* ---- View > Options --------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *gridlines, *zeros;
  GtkWidget *manual, *iterate, *iterations, *tolerance;
} OptionsPrompt;

static void
on_options_ok (GtkWidget *w, gpointer data)
{
  OptionsPrompt *prompt = data;
  (void) w;
  o42_grid_set_show_gridlines (prompt->window->grid,
    gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->gridlines)));
  o42_grid_set_show_zeros (prompt->window->grid,
    gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->zeros)));

  o42_book_set_manual (prompt->window->book,
    gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->manual)));
  o42_book_set_iteration (prompt->window->book,
    gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->iterate)),
    (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->iterations)),
    g_ascii_strtod (gtk_editable_get_text (GTK_EDITABLE (prompt->tolerance)), NULL));

  /* Turning iteration on, or going back to calculating as you type,
   * only means anything once everything has been worked out again. */
  for (int i = 0; i < o42_book_n_sheets (prompt->window->book); i++)
    o42_sheet_recalculate (o42_book_sheet (prompt->window->book, i));
  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);

  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
action_options (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  OptionsPrompt *prompt = g_new0 (OptionsPrompt, 1);
  GtkWidget *content, *buttons, *ok;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Options"), TRUE, &content, &buttons);

  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Window Options")));
  prompt->gridlines = gtk_check_button_new_with_mnemonic ( _("_Gridlines"));
  prompt->zeros = gtk_check_button_new_with_mnemonic ( _("_Zero values"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->gridlines),
                               o42_grid_get_show_gridlines (self->grid));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->zeros),
                               o42_grid_get_show_zeros (self->grid));
  gtk_box_append (GTK_BOX (content), prompt->gridlines);
  gtk_box_append (GTK_BOX (content), prompt->zeros);

  {
    /* How the book calculates: as you type or when you ask, and whether
     * a formula may depend on itself. */
    GtkWidget *grid = gtk_grid_new ();
    int max = 100;
    double tolerance = 0.001;
    gboolean iterating = o42_book_iteration (self->book, &max, &tolerance);
    char shown[G_ASCII_DTOSTR_BUF_SIZE];

    /* Written with a full stop wherever the machine is, since that is
     * what reads it back. */
    g_ascii_formatd (shown, sizeof shown, "%g", tolerance);

    gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 8);

    prompt->manual = gtk_check_button_new_with_mnemonic ( _("Calculate only when as_ked (F9)"));
    gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->manual),
                                 o42_book_manual (self->book));
    gtk_box_append (GTK_BOX (content), prompt->manual);

    prompt->iterate = gtk_check_button_new_with_mnemonic ( _("Allow a formula to depend on _itself"));
    gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->iterate), iterating);
    gtk_box_append (GTK_BOX (content), prompt->iterate);

    prompt->iterations = labelled (grid, 0, "At most:",
                                   gtk_spin_button_new_with_range (1, 10000, 1));
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->iterations), max);
    prompt->tolerance = labelled (grid, 1, _("Until it moves less than:"), gtk_entry_new ());
    gtk_editable_set_text (GTK_EDITABLE (prompt->tolerance), shown);
    gtk_box_append (GTK_BOX (content), grid);
  }

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_options_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Help > Contents -------------------------------------------------- */

static void
action_help_contents (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkWidget *content, *buttons, *dialog, *label;
  static const char *TEXT =
    "Type into a cell and press Enter; Tab moves right and Enter goes back\n"
    "to the column the row began in.  F2 edits in place, Delete clears,\n"
    "Escape cancels.  A formula starts with =, as in =SUM(A1:A9) or\n"
    "=Sheet2!B3*2; Shift+F3 lists every function.\n"
    "\n"
    "Ctrl+C, Ctrl+X, Ctrl+V copy, cut and paste with references moved.\n"
    "Ctrl+D and Ctrl+R fill down and right; drag the square at the corner\n"
    "of the selection to continue a series.  Ctrl+arrows jump to the edges\n"
    "of the data, Ctrl+Home and Ctrl+End to the corners, Shift+Space and\n"
    "Ctrl+Space select the row and the column, F5 goes to a cell.\n"
    "\n"
    "Ctrl+1 is Format Cells, Ctrl+B, Ctrl+I, Ctrl+U bold, italic and\n"
    "underline.  Ctrl+F finds, Ctrl+H replaces.  Ctrl+PageUp and\n"
    "Ctrl+PageDown move between sheets.  Ctrl+S saves, Ctrl+O opens,\n"
    "Ctrl+P prints, Ctrl+Z and Ctrl+Y undo and redo.\n"
    "\n"
    "Drag a header boundary to resize a column or row, double-click it to\n"
    "fit.  Click a picture or chart to select it, drag to move, drag a\n"
    "handle to resize, Delete to remove.\n"
    "\n"
    "A form control from Insert > Control is worked by a plain click;\n"
    "Ctrl+click takes hold of it, and Format > Control gives it the cell\n"
    "it drives.";

  (void) a; (void) p;

  dialog = dialog_frame (self, _("office42 Help"), FALSE, &content, &buttons);
  label = gtk_label_new (TEXT);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (content), label);
  dialog_button (buttons, _("Close"), G_CALLBACK (on_dialog_close_clicked), dialog);
  g_signal_connect (dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  gtk_window_present (GTK_WINDOW (dialog));
}

/* ---- Printing --------------------------------------------------------- */

/* Printing draws the same pages the PDF export does, through
 * GtkPrintOperation, which gives the system's print dialog and printers
 * for free.  The page size and orientation come from Page Setup; landscape
 * is the default because that is how a sheet is usually printed. */
/* The file's name, for &F in headers and footers. */
static void
window_name_pages (O42Window *self, O42Pages *pages)
{
  char *name = self->file != NULL ? g_file_get_basename (self->file) : g_strdup ("Book1");
  o42_pages_set_document (pages, name);
  g_free (name);
}

static GtkPageSetup *
window_page_setup (O42Window *self)
{
  if (self->page_setup == NULL)
    {
      self->page_setup = gtk_page_setup_new ();
      gtk_page_setup_set_orientation (self->page_setup, GTK_PAGE_ORIENTATION_LANDSCAPE);
    }
  return self->page_setup;
}

static void
on_print_begin (GtkPrintOperation *op, GtkPrintContext *context, gpointer data)
{
  O42Window *self = data;
  gboolean whole_book = g_object_get_data (G_OBJECT (op), "o42-book") != NULL;
  GPtrArray *all = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_pages_free);
  int total = 0;

  for (int i = 0; i < o42_book_n_sheets (self->book); i++)
    {
      O42Sheet *sheet = whole_book ? o42_book_sheet (self->book, i) : self->sheet;
      double margin = o42_sheet_print_setup (sheet)->margin;
      O42Pages *pages = o42_pages_new (sheet,
                                       gtk_print_context_get_width (context) - 2 * margin,
                                       gtk_print_context_get_height (context) - 2 * margin);

      window_name_pages (self, pages);
      g_ptr_array_add (all, pages);
      total += MAX (1, o42_pages_count (pages));
      if (!whole_book)
        break;
    }

  gtk_print_operation_set_n_pages (op, MAX (1, total));
  g_object_set_data_full (G_OBJECT (op), "o42-all-pages", all,
                          (GDestroyNotify) g_ptr_array_unref);
}

static void
on_print_draw_page (GtkPrintOperation *op, GtkPrintContext *context,
                    int page, gpointer data)
{
  O42Window *self = data;
  GPtrArray *all = g_object_get_data (G_OBJECT (op), "o42-all-pages");
  cairo_t *cr = gtk_print_context_get_cairo_context (context);
  double margin = o42_sheet_print_setup (self->sheet)->margin;

  for (guint i = 0; all != NULL && i < all->len; i++)
    {
      O42Pages *pages = g_ptr_array_index (all, i);
      int count = MAX (1, o42_pages_count (pages));

      if (page >= count)
        {
          page -= count;
          continue;
        }
      cairo_save (cr);
      cairo_translate (cr, margin, margin);
      o42_pages_draw (pages, page, cr);
      cairo_restore (cr);
      break;
    }
}

static void
on_print_done (GtkPrintOperation *op, GtkPrintOperationResult result, gpointer data)
{
  O42Window *self = data;

  if (result == GTK_PRINT_OPERATION_RESULT_APPLY)
    {
      GtkPrintSettings *settings = gtk_print_operation_get_print_settings (op);
      if (settings != NULL)
        {
          g_clear_object (&self->print_settings);
          self->print_settings = g_object_ref (settings);
        }
    }
  else if (result == GTK_PRINT_OPERATION_RESULT_ERROR)
    {
      GError *error = NULL;
      gtk_print_operation_get_error (op, &error);
      show_error (self, "office42 could not print.", error);
      g_clear_error (&error);
    }
}

/* Printing, of this sheet or of every sheet in the book, which is what
 * Excel's print dialog calls the entire workbook. */
static void
print_run (O42Window *self, gboolean whole_book)
{
  GtkPrintOperation *op = gtk_print_operation_new ();
  char *name;

  if (whole_book)
    g_object_set_data (G_OBJECT (op), "o42-book", GINT_TO_POINTER (1));

  if (o42_grid_is_editing (self->grid))
    o42_grid_commit_edit (self->grid);

  name = (self->file != NULL) ? g_file_get_basename (self->file) : g_strdup ("Book1");
  gtk_print_operation_set_job_name (op, name);
  g_free (name);

  gtk_print_operation_set_default_page_setup (op, window_page_setup (self));
  if (self->print_settings != NULL)
    gtk_print_operation_set_print_settings (op, self->print_settings);
  gtk_print_operation_set_embed_page_setup (op, TRUE);
  gtk_print_operation_set_unit (op, GTK_UNIT_POINTS);

  g_signal_connect (op, "begin-print", G_CALLBACK (on_print_begin), self);
  g_signal_connect (op, "draw-page", G_CALLBACK (on_print_draw_page), self);
  g_signal_connect (op, "done", G_CALLBACK (on_print_done), self);

  gtk_print_operation_run (op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                           GTK_WINDOW (self), NULL);
  g_object_unref (op);
}

static void
action_print (GSimpleAction *a, GVariant *p, gpointer data)
{
  (void) a; (void) p;
  print_run (data, FALSE);
}

static void
action_print_book (GSimpleAction *a, GVariant *p, gpointer data)
{
  (void) a; (void) p;
  print_run (data, TRUE);
}

/* ---- Print Preview ---------------------------------------------------- */

/* One page at a time, scaled to fit the window, from the same pages the
 * printer gets.  The pages are made afresh each time the preview opens
 * and freed with it. */
typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *area;
  GtkWidget *label;
  O42Pages  *pages;
  int        page;
  double     paper_w, paper_h;    /* points */
  double     margin;
} PreviewPrompt;

static void
preview_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  PreviewPrompt *prompt = data;
  double scale = MIN ((width - 20) / prompt->paper_w, (height - 20) / prompt->paper_h);
  double x = (width - prompt->paper_w * scale) / 2;
  double y = (height - prompt->paper_h * scale) / 2;

  (void) area;

  cairo_set_source_rgb (cr, 0.5, 0.5, 0.5);
  cairo_paint (cr);

  cairo_save (cr);
  cairo_translate (cr, x, y);
  cairo_scale (cr, scale, scale);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_rectangle (cr, 0, 0, prompt->paper_w, prompt->paper_h);
  cairo_fill (cr);
  cairo_translate (cr, prompt->margin, prompt->margin);
  cairo_rectangle (cr, 0, 0, prompt->paper_w - 2 * prompt->margin,
                   prompt->paper_h - 2 * prompt->margin);
  cairo_clip (cr);
  o42_pages_draw (prompt->pages, prompt->page, cr);
  cairo_restore (cr);

  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_set_line_width (cr, 1);
  cairo_rectangle (cr, x + 0.5, y + 0.5, prompt->paper_w * scale, prompt->paper_h * scale);
  cairo_stroke (cr);
}

static void
preview_update (PreviewPrompt *prompt)
{
  char *text = g_strdup_printf ("Page %d of %d", prompt->page + 1,
                                MAX (1, o42_pages_count (prompt->pages)));
  gtk_label_set_text (GTK_LABEL (prompt->label), text);
  g_free (text);
  gtk_widget_queue_draw (prompt->area);
}

static void
on_preview_next (GtkWidget *w, gpointer data)
{
  PreviewPrompt *prompt = data;
  (void) w;
  if (prompt->page + 1 < o42_pages_count (prompt->pages))
    prompt->page++;
  preview_update (prompt);
}

static void
on_preview_prev (GtkWidget *w, gpointer data)
{
  PreviewPrompt *prompt = data;
  (void) w;
  if (prompt->page > 0)
    prompt->page--;
  preview_update (prompt);
}

static void
on_preview_print (GtkWidget *w, gpointer data)
{
  PreviewPrompt *prompt = data;
  O42Window *self = prompt->window;
  (void) w;
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
  action_print (NULL, NULL, self);
}

static void
on_preview_destroy (GtkWidget *w, gpointer data)
{
  PreviewPrompt *prompt = data;
  (void) w;
  o42_pages_free (prompt->pages);
  gtk_widget_grab_focus (GTK_WIDGET (prompt->window->grid));
  g_free (prompt);
}

static void
action_print_preview (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  PreviewPrompt *prompt = g_new0 (PreviewPrompt, 1);
  GtkPageSetup *setup = window_page_setup (self);
  GtkWidget *content, *buttons;

  (void) a; (void) p;

  if (o42_grid_is_editing (self->grid))
    o42_grid_commit_edit (self->grid);

  prompt->window = self;
  prompt->paper_w = gtk_page_setup_get_paper_width (setup, GTK_UNIT_POINTS);
  prompt->paper_h = gtk_page_setup_get_paper_height (setup, GTK_UNIT_POINTS);
  prompt->margin = o42_sheet_print_setup (self->sheet)->margin;
  prompt->pages = o42_pages_new (self->sheet,
                                 prompt->paper_w - 2 * prompt->margin,
                                 prompt->paper_h - 2 * prompt->margin);
  window_name_pages (self, prompt->pages);

  prompt->dialog = dialog_frame (self, _("Print Preview"), FALSE, &content, &buttons);
  gtk_window_set_resizable (GTK_WINDOW (prompt->dialog), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (prompt->dialog), 720, 600);

  prompt->area = gtk_drawing_area_new ();
  gtk_widget_set_vexpand (prompt->area, TRUE);
  gtk_widget_set_hexpand (prompt->area, TRUE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (prompt->area), preview_draw, prompt, NULL);
  gtk_box_append (GTK_BOX (content), prompt->area);

  prompt->label = gtk_label_new ("");
  gtk_widget_set_hexpand (prompt->label, TRUE);
  gtk_box_prepend (GTK_BOX (buttons), prompt->label);
  gtk_widget_set_halign (buttons, GTK_ALIGN_FILL);
  dialog_button (buttons, _("_Previous"), G_CALLBACK (on_preview_prev), prompt);
  dialog_button (buttons, _("_Next"), G_CALLBACK (on_preview_next), prompt);
  dialog_button (buttons, _("_Print..."), G_CALLBACK (on_preview_print), prompt);
  dialog_button (buttons, _("Close"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_preview_destroy), prompt);

  preview_update (prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

static void
action_page_setup (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkPageSetup *setup;

  (void) a; (void) p;

  if (self->print_settings == NULL)
    self->print_settings = gtk_print_settings_new ();

  setup = gtk_print_run_page_setup_dialog (GTK_WINDOW (self),
                                           window_page_setup (self),
                                           self->print_settings);
  g_clear_object (&self->page_setup);
  self->page_setup = setup;
}

/* ---- File > Page Setup > Sheet: header, footer, print area, options --- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *header, *footer, *area, *gridlines, *headings, *titles;
  GtkWidget *scale, *fit_wide, *fit_tall, *margin;
} SheetSetupPrompt;

static void
on_sheet_setup_ok (GtkWidget *w, gpointer data)
{
  SheetSetupPrompt *prompt = data;
  O42Sheet *sheet = prompt->window->sheet;
  const char *area = gtk_editable_get_text (GTK_EDITABLE (prompt->area));
  O42Range r;
  gsize len = 0;
  (void) w;

  o42_sheet_set_header_footer (sheet, gtk_editable_get_text (GTK_EDITABLE (prompt->header)),
                               gtk_editable_get_text (GTK_EDITABLE (prompt->footer)));
  if (*area == '\0')
    o42_sheet_set_print_area (sheet, NULL);
  else if (o42_ref_parse (area, &r.row0, &r.col0, &len) &&
           (area[len] == '\0' || (area[len] == ':' && o42_ref_parse (area + len + 1, &r.row1, &r.col1, NULL))))
    {
      if (area[len] == '\0') { r.row1 = r.row0; r.col1 = r.col0; }
      o42_sheet_set_print_area (sheet, &r);
    }
  o42_sheet_set_print_options (sheet,
                               gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->gridlines)),
                               gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->headings)),
                               gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->titles)));
  o42_sheet_set_print_scale (sheet, gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->scale)),
                             gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->fit_wide)),
                             gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->fit_tall)));
  o42_sheet_set_print_margin (sheet, gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->margin)));
  window_sync (prompt->window);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
action_sheet_setup (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  SheetSetupPrompt *prompt = g_new0 (SheetSetupPrompt, 1);
  const O42PrintSetup *setup = o42_sheet_print_setup (self->sheet);
  GtkWidget *content, *buttons, *ok, *grid, *hint;

  (void) a; (void) p;
  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Page Setup: Sheet"), TRUE, &content, &buttons);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->header = labelled (grid, 0, _("Header:"), gtk_entry_new ());
  prompt->footer = labelled (grid, 1, _("Footer:"), gtk_entry_new ());
  prompt->area = labelled (grid, 2, _("Print area:"), gtk_entry_new ());
  prompt->titles = labelled (grid, 3, _("Rows to repeat at top:"), gtk_spin_button_new_with_range (0, 50, 1));
  prompt->scale = labelled (grid, 4, _("Scale (per cent):"), gtk_spin_button_new_with_range (10, 400, 5));
  prompt->fit_wide = labelled (grid, 5, _("Fit to pages across:"), gtk_spin_button_new_with_range (0, 20, 1));
  prompt->fit_tall = labelled (grid, 6, _("Fit to pages down:"), gtk_spin_button_new_with_range (0, 20, 1));
  prompt->margin = labelled (grid, 7, _("Margin (points):"), gtk_spin_button_new_with_range (0, 200, 6));
  gtk_box_append (GTK_BOX (content), grid);
  gtk_widget_set_size_request (prompt->header, 320, -1);
  gtk_editable_set_text (GTK_EDITABLE (prompt->header), setup->header != NULL ? setup->header : "");
  gtk_editable_set_text (GTK_EDITABLE (prompt->footer), setup->footer != NULL ? setup->footer : "");
  if (setup->has_area)
    {
      char *x = o42_ref_name (setup->area.row0, setup->area.col0);
      char *y = o42_ref_name (setup->area.row1, setup->area.col1);
      char *text = g_strdup_printf ("%s:%s", x, y);
      gtk_editable_set_text (GTK_EDITABLE (prompt->area), text);
      g_free (text); g_free (x); g_free (y);
    }
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->area), _("the used range"));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->titles), setup->title_rows);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->scale), setup->scale);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->fit_wide), setup->fit_wide);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->fit_tall), setup->fit_tall);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->margin), setup->margin);

  hint = gtk_label_new ("&L, &C, &R start the left, centre and right parts; &P page, &N pages, &D date, "
                       "&T time, &F file, &A sheet. Pages across or down above zero fit the sheet to them "
                       "and the scale is worked out; Insert > Page Break starts a page at the active cell.");
  gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_label_set_max_width_chars (GTK_LABEL (hint), 50);
  gtk_widget_add_css_class (hint, "dim-label");
  gtk_box_append (GTK_BOX (content), hint);

  prompt->gridlines = gtk_check_button_new_with_mnemonic ( _("Print _gridlines"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->gridlines), setup->gridlines);
  gtk_box_append (GTK_BOX (content), prompt->gridlines);
  prompt->headings = gtk_check_button_new_with_mnemonic ( _("Print row and column _headings"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->headings), setup->headings);
  gtk_box_append (GTK_BOX (content), prompt->headings);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_sheet_setup_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* Insert > Page Break puts one above the active row and one to the
 * left of its column, or takes them away again. */
static void
action_page_break (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  int row, col;

  (void) a; (void) p;
  o42_grid_get_active (self->grid, &row, &col);
  if (row > 0)
    o42_sheet_toggle_page_break (self->sheet, TRUE, row);
  if (col > 0)
    o42_sheet_toggle_page_break (self->sheet, FALSE, col);
  window_sync (self);
}

static void
action_set_print_area (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  O42Range sel;
  (void) a; (void) p;
  o42_grid_get_selection (self->grid, &sel);
  o42_sheet_set_print_area (self->sheet, &sel);
  window_sync (self);
}

static void
action_clear_print_area (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  (void) a; (void) p;
  o42_sheet_set_print_area (self->sheet, NULL);
  window_sync (self);
}

/* ---- Function Wizard -------------------------------------------------- */

/* Excel 5's Function Wizard, step one of two: a list of every function
 * with a search box, the signature and a line about the chosen one, and
 * OK putting "=NAME(" into the cell to be finished by hand.  Step two,
 * prompting for each argument, is a refinement for later. */
typedef struct {
  O42Window     *window;
  GtkWidget     *dialog;
  GtkWidget     *search;
  GtkWidget     *list;
  GtkStringList *names;
  GtkWidget     *signature;
  GtkWidget     *summary;
} WizardPrompt;

static void
wizard_fill (WizardPrompt *prompt, const char *filter)
{
  guint n = 0;
  const char *const *names = o42_function_names (&n);
  char *folded = g_utf8_casefold (filter != NULL ? filter : "", -1);

  gtk_string_list_splice (prompt->names, 0,
                          g_list_model_get_n_items (G_LIST_MODEL (prompt->names)), NULL);

  for (guint i = 0; i < n; i++)
    {
      const char *summary = NULL;
      char *lname = g_utf8_casefold (names[i], -1);
      char *lsummary;
      gboolean hit;

      o42_function_help (names[i], NULL, &summary);
      lsummary = g_utf8_casefold (summary != NULL ? summary : "", -1);
      hit = (*folded == '\0' || strstr (lname, folded) != NULL || strstr (lsummary, folded) != NULL);
      g_free (lname);
      g_free (lsummary);

      if (hit)
        gtk_string_list_append (prompt->names, names[i]);
    }

  g_free (folded);
}

static const char *
wizard_selected (WizardPrompt *prompt)
{
  GtkSelectionModel *model = gtk_list_view_get_model (GTK_LIST_VIEW (prompt->list));
  guint pos = gtk_single_selection_get_selected (GTK_SINGLE_SELECTION (model));

  if (pos == GTK_INVALID_LIST_POSITION)
    return NULL;
  return gtk_string_list_get_string (prompt->names, pos);
}

static void
on_wizard_selection (GObject *model, GParamSpec *pspec, gpointer data)
{
  WizardPrompt *prompt = data;
  const char *name = wizard_selected (prompt);
  const char *signature = NULL, *summary = NULL;

  (void) model; (void) pspec;

  if (name != NULL && o42_function_help (name, &signature, &summary))
    {
      gtk_label_set_text (GTK_LABEL (prompt->signature), signature);
      gtk_label_set_text (GTK_LABEL (prompt->summary), summary);
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (prompt->signature), name != NULL ? name : "");
      gtk_label_set_text (GTK_LABEL (prompt->summary), "");
    }
}

static void
on_wizard_search (GtkEditable *entry, gpointer data)
{
  WizardPrompt *prompt = data;

  wizard_fill (prompt, gtk_editable_get_text (entry));
  on_wizard_selection (NULL, NULL, prompt);
}

static void
on_wizard_ok (GtkWidget *w, gpointer data)
{
  WizardPrompt *prompt = data;
  const char *name = wizard_selected (prompt);

  (void) w;

  if (name != NULL)
    {
      const char *signature = NULL;
      char *initial;

      /* A function of no arguments is complete as it is. */
      o42_function_help (name, &signature, NULL);
      if (signature != NULL && g_str_has_suffix (signature, "()"))
        initial = g_strdup_printf ("=%s()", name);
      else
        initial = g_strdup_printf ("=%s(", name);

      gtk_window_destroy (GTK_WINDOW (prompt->dialog));
      o42_grid_begin_edit (prompt->window->grid, initial);
      g_free (initial);
      return;
    }

  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_wizard_activate (GtkListView *list, guint position, gpointer data)
{
  (void) list; (void) position;
  on_wizard_ok (NULL, data);
}

void
o42_wizard_setup_item (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  GtkWidget *label = gtk_label_new ("");
  (void) factory; (void) data;
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_list_item_set_child (item, label);
}

void
o42_wizard_bind_item (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  GtkStringObject *obj = gtk_list_item_get_item (item);
  (void) factory; (void) data;
  gtk_label_set_text (GTK_LABEL (gtk_list_item_get_child (item)),
                      gtk_string_object_get_string (obj));
}

static void
action_insert_function (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  WizardPrompt *prompt = g_new0 (WizardPrompt, 1);
  GtkWidget *content, *buttons, *scrolled, *ok;
  GtkListItemFactory *factory;
  GtkSingleSelection *selection;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Function Wizard - Step 1 of 2"), TRUE,
                                 &content, &buttons);

  prompt->search = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->search), _("Search functions"));
  gtk_box_append (GTK_BOX (content), prompt->search);

  prompt->names = gtk_string_list_new (NULL);
  wizard_fill (prompt, NULL);

  factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (wizard_setup_item), prompt);
  g_signal_connect (factory, "bind", G_CALLBACK (wizard_bind_item), prompt);

  selection = gtk_single_selection_new (G_LIST_MODEL (prompt->names));
  prompt->list = gtk_list_view_new (GTK_SELECTION_MODEL (selection), factory);
  gtk_list_view_set_single_click_activate (GTK_LIST_VIEW (prompt->list), FALSE);

  scrolled = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (scrolled, 360, 220);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), prompt->list);
  gtk_widget_add_css_class (scrolled, "o42-field");
  gtk_box_append (GTK_BOX (content), scrolled);

  prompt->signature = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (prompt->signature), 0.0);
  gtk_widget_add_css_class (prompt->signature, "o42-glyph-bold");
  gtk_box_append (GTK_BOX (content), prompt->signature);

  prompt->summary = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (prompt->summary), 0.0);
  gtk_label_set_wrap (GTK_LABEL (prompt->summary), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (prompt->summary), 50);
  gtk_box_append (GTK_BOX (content), prompt->summary);

  g_signal_connect (selection, "notify::selected", G_CALLBACK (on_wizard_selection), prompt);
  g_signal_connect (prompt->search, "changed", G_CALLBACK (on_wizard_search), prompt);
  g_signal_connect (prompt->list, "activate", G_CALLBACK (on_wizard_activate), prompt);
  on_wizard_selection (NULL, NULL, prompt);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_wizard_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->search), TRUE);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Sheets and their tabs ------------------------------------------- */

static void
on_tab_clicked (GtkWidget *button, gpointer data)
{
  O42Window *self = data;
  int index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "o42-sheet"));
  O42Sheet *sheet = o42_book_sheet (self->book, index);

  if (sheet != NULL && sheet != self->sheet)
    {
      if (o42_grid_is_editing (self->grid))
        o42_grid_commit_edit (self->grid);
      self->sheet = sheet;
      o42_grid_set_sheet (self->grid, sheet);
      window_rebuild_tabs (self);
      window_sync (self);
    }
  gtk_widget_grab_focus (GTK_WIDGET (self->grid));
}

/* One tab per sheet, the current one lit, as along the bottom of Excel 5. */
/* ---- Format > AutoFormat, and the format painter ----------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *which;
  O42Range   range;
} AutoFormatPrompt;

static void
on_autoformat_ok (GtkWidget *w, gpointer data)
{
  AutoFormatPrompt *prompt = data;
  O42Window *self = prompt->window;

  (void) w;
  o42_sheet_auto_format (self->sheet, &prompt->range,
                         (int) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->which)));
  o42_sheet_set_modified (self->sheet, TRUE);
  o42_grid_refresh (self->grid);
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
action_autoformat (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  AutoFormatPrompt *prompt = g_new0 (AutoFormatPrompt, 1);
  GtkWidget *content, *buttons, *ok;
  const char *names[16];
  int n_looks = MIN (o42_auto_format_count (), 15);

  (void) a; (void) p;
  for (int i = 0; i < n_looks; i++)
    names[i] = o42_auto_format_name (i);
  names[n_looks] = NULL;

  prompt->window = self;
  o42_grid_get_selection (self->grid, &prompt->range);
  prompt->dialog = dialog_frame (self, _("AutoFormat"), TRUE, &content, &buttons);
  gtk_box_append (GTK_BOX (content), gtk_label_new ("A look for the selection, "
                                                    "whose first row is its heading:"));
  prompt->which = gtk_drop_down_new_from_strings (names);
  gtk_box_append (GTK_BOX (content), prompt->which);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_autoformat_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* The painter picks up the look of the active cell; the next click puts
 * it down. */
static void
action_format_painter (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  (void) a; (void) p;
  o42_grid_pick_up_format (self->grid);
  gtk_label_set_text (GTK_LABEL (self->status_label), _("Click a cell or a range to give it that look"));
}

/* ---- Data > What-If Table ---------------------------------------------- */

/* Excel's Data > Table: the selection's edges hold what an input may
 * be, its corner (or its top row, or its left column) the formula, and
 * the inside is filled with the answer for each. */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *row_input, *col_input;
  O42Range   range;
} WhatIfPrompt;

static void
on_whatif_ok (GtkWidget *w, gpointer data)
{
  WhatIfPrompt *prompt = data;
  O42Window *self = prompt->window;
  int rr = -1, rc = -1, cr = -1, cc = -1;
  const char *row_text = gtk_editable_get_text (GTK_EDITABLE (prompt->row_input));
  const char *col_text = gtk_editable_get_text (GTK_EDITABLE (prompt->col_input));

  (void) w;
  if (row_text != NULL && *row_text != 0)
    o42_ref_parse (row_text, &rr, &rc, NULL);
  if (col_text != NULL && *col_text != 0)
    o42_ref_parse (col_text, &cr, &cc, NULL);

  if (!o42_sheet_data_table (self->sheet, &prompt->range, rr, rc, cr, cc))
    show_error (self, "A what-if table needs a row or a column input cell, "
                      "and a selection bigger than one cell.", NULL);
  else
    {
      o42_sheet_set_modified (self->sheet, TRUE);
      o42_grid_refresh (self->grid);
      window_sync (self);
    }
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
action_whatif (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  WhatIfPrompt *prompt = g_new0 (WhatIfPrompt, 1);
  GtkWidget *content, *buttons, *grid, *ok;

  (void) a; (void) p;
  prompt->window = self;
  o42_grid_get_selection (self->grid, &prompt->range);
  prompt->dialog = dialog_frame (self, _("What-If Table"), TRUE, &content, &buttons);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->row_input = labelled (grid, 0, _("Row input cell:"), gtk_entry_new ());
  prompt->col_input = labelled (grid, 1, _("Column input cell:"), gtk_entry_new ());
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->row_input), TRUE);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->col_input), TRUE);
  gtk_box_append (GTK_BOX (content), grid);
  gtk_box_append (GTK_BOX (content),
                  gtk_label_new ("The values run along the top row, down the left column, "
                                 "or both;"));
  gtk_box_append (GTK_BOX (content),
                  gtk_label_new (_("the formula goes in the corner, and the inside is filled in.")));

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_whatif_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- View > Full Screen ------------------------------------------------ */

/* The grid with nothing round it but its own chrome.  F11, as
 * everywhere else. */
static void
action_full_screen (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  (void) a; (void) p;
  if (gtk_window_is_fullscreen (GTK_WINDOW (self)))
    gtk_window_unfullscreen (GTK_WINDOW (self));
  else
    gtk_window_fullscreen (GTK_WINDOW (self));
}

/* ---- F9: work everything out again ------------------------------------- */

/* With the book set to calculate by hand, nothing is worked out until
 * this is asked for; with iteration on, this is what goes round the
 * loop.  Either way it is what F9 has always done. */
static void
action_calculate (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  (void) a; (void) p;
  for (int i = 0; i < o42_book_n_sheets (self->book); i++)
    o42_sheet_recalculate (o42_book_sheet (self->book, i));
  o42_grid_refresh (self->grid);
  window_sync (self);
}

/* ---- The sheet tabs' own menu, and moving a sheet ---------------------- */

/* A sheet is moved by taking it out of the book and putting it back
 * somewhere else -- which is what undo already does with a deleted
 * sheet, so the book can do it and the history follows. */
static void
window_move_sheet (O42Window *self, int by)
{
  int at = o42_book_sheet_index (self->book, self->sheet);
  int to = at + by;

  if (at < 0 || to < 0 || to >= o42_book_n_sheets (self->book))
    return;

  o42_sheet_begin_group (self->sheet);
  o42_sheet_undo_capture_sheet (self->sheet, FALSE);
  if (o42_book_detach_sheet (self->book, at))
    o42_book_attach_sheet (self->book, self->sheet, to);
  o42_sheet_end_group (self->sheet);

  o42_book_set_modified (self->book, TRUE);
  window_rebuild_tabs (self);
  window_tell_book (self, "sheets");
  window_sync (self);
}

static void
action_move_sheet_left (GSimpleAction *a, GVariant *p, gpointer data)
{
  (void) a; (void) p;
  window_move_sheet (data, -1);
}

static void
action_move_sheet_right (GSimpleAction *a, GVariant *p, gpointer data)
{
  (void) a; (void) p;
  window_move_sheet (data, 1);
}

/* A coloured tab wears its colour as a band under its name, the way
 * Excel's does.  The colour arrives as a CSS class named after the
 * colour itself, so one provider on the display serves every tab in
 * every window and a colour costs one rule however often it is used. */
static void
tab_colour_class (GtkWidget *tab, guint32 colour)
{
  static GtkCssProvider *provider;
  static GString *rules;
  static GHashTable *seen;
  char name[24];

  g_snprintf (name, sizeof name, "o42-tab-%06X", colour & 0xFFFFFF);

  if (provider == NULL)
    {
      GdkDisplay *display = gdk_display_get_default ();

      provider = gtk_css_provider_new ();
      rules = g_string_new (NULL);
      seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      if (display != NULL)
        gtk_style_context_add_provider_for_display (display, GTK_STYLE_PROVIDER (provider),
                                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }

  if (!g_hash_table_contains (seen, name))
    {
      g_hash_table_add (seen, g_strdup (name));
      g_string_append_printf (rules, "button.%s { box-shadow: inset 0 -4px #%06X; }\n",
                              name, colour & 0xFFFFFF);
      gtk_css_provider_load_from_string (provider, rules->str);
    }

  gtk_widget_add_css_class (tab, name);
}

/* A tab's colour, chosen from the same colour dialog everything else
 * uses.  Excel puts the colour behind the tab's name; so does this. */
static void
on_tab_colour_chosen (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Window *self = data;
  GdkRGBA *rgba = gtk_color_dialog_choose_rgba_finish (GTK_COLOR_DIALOG (source), result, NULL);

  if (rgba == NULL)
    return;
  o42_sheet_set_tab_colour (self->sheet, colour_from_rgba (rgba));
  o42_book_set_modified (self->book, TRUE);
  window_rebuild_tabs (self);
  gdk_rgba_free (rgba);
}

static void
action_tab_colour (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkColorDialog *dialog = gtk_color_dialog_new ();

  (void) a; (void) p;
  gtk_color_dialog_set_title (dialog, _("Tab Colour"));
  gtk_color_dialog_choose_rgba (dialog, GTK_WINDOW (self), NULL, NULL,
                                on_tab_colour_chosen, self);
  g_object_unref (dialog);
}

static void
action_tab_colour_none (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  (void) a; (void) p;
  o42_sheet_set_tab_colour (self->sheet, O42_TAB_NO_COLOUR);
  o42_book_set_modified (self->book, TRUE);
  window_rebuild_tabs (self);
}

/* Dragging a tab moves the sheet: where it is let go decides where it
 * lands, which is what a person expects of a tab that follows the
 * pointer. */
static void
on_tab_drag_end (GtkGestureDrag *drag, double dx, double dy, gpointer data)
{
  O42Window *self = data;
  GtkWidget *tab = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (drag));
  int from = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tab), "o42-sheet"));
  double start_x = 0, start_y = 0;
  graphene_point_t at, in_box;
  GtkWidget *child;
  int to = from, index = 0;

  (void) dy;
  if (fabs (dx) < 6)
    return;   /* a click, not a drag */
  gtk_gesture_drag_get_start_point (drag, &start_x, &start_y);
  at = GRAPHENE_POINT_INIT ((float) (start_x + dx), (float) (start_y + dy));
  if (!gtk_widget_compute_point (tab, self->tabs, &at, &in_box))
    return;

  for (child = gtk_widget_get_first_child (self->tabs);
       child != NULL;
       child = gtk_widget_get_next_sibling (child), index++)
    {
      graphene_rect_t bounds;

      if (gtk_widget_compute_bounds (child, self->tabs, &bounds) &&
          in_box.x >= bounds.origin.x)
        to = index;
    }

  if (to != from)
    {
      window_show_sheet (self, from);
      window_move_sheet (self, to - from);
    }
}

/* The right button on a tab: the sheet it names becomes the current one
 * first, so every item acts on the tab that was pointed at. */
static void
on_tab_secondary (GtkGestureClick *gesture, int n_press,
                  double x, double y, gpointer data)
{
  O42Window *self = data;
  GtkWidget *tab = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  int index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tab), "o42-sheet"));
  GMenu *menu = g_menu_new ();
  GMenu *sheets = g_menu_new ();
  GMenu *move = g_menu_new ();
  GtkWidget *popover;
  GdkRectangle at = { (int) x, (int) y, 1, 1 };

  (void) n_press;
  window_show_sheet (self, index);

  g_menu_append (sheets, _("_Insert Sheet"), "win.insert-sheet");
  g_menu_append (sheets, _("_Delete Sheet"), "win.delete-sheet");
  g_menu_append (sheets, _("_Rename Sheet..."), "win.rename-sheet");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (sheets));
  g_menu_append (move, _("Move _Left"), "win.move-sheet-left");
  g_menu_append (move, _("Move _Right"), "win.move-sheet-right");
  g_menu_append (move, _("Tab _Colour..."), "win.tab-colour");
  g_menu_append (move, _("_No Tab Colour"), "win.tab-colour-none");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (move));

  popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &at);
  gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
  gtk_widget_set_parent (popover, tab);
  g_signal_connect (popover, "closed", G_CALLBACK (gtk_widget_unparent), NULL);
  g_object_unref (menu);
  g_object_unref (sheets);
  g_object_unref (move);
  gtk_popover_popup (GTK_POPOVER (popover));
}

/* Two clicks on a tab rename the sheet, as they do everywhere else. */
static void
on_tab_double_click (GtkGestureClick *gesture, int n_press,
                     double x, double y, gpointer data)
{
  O42Window *self = data;
  GtkWidget *tab = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));

  (void) x; (void) y;
  if (n_press < 2)
    return;
  window_show_sheet (self, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tab), "o42-sheet")));
  g_action_group_activate_action (G_ACTION_GROUP (self), "rename-sheet", NULL);
}

static void
window_rebuild_tabs (O42Window *self)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (self->tabs)) != NULL)
    gtk_box_remove (GTK_BOX (self->tabs), child);

  for (int i = 0; i < o42_book_n_sheets (self->book); i++)
    {
      O42Sheet *sheet = o42_book_sheet (self->book, i);
      GtkWidget *tab = gtk_button_new_with_label (o42_sheet_get_name (sheet));

      gtk_widget_add_css_class (tab, "o42-tab");
      if (sheet == self->sheet)
        gtk_widget_add_css_class (tab, "o42-tab-active");
      if (o42_sheet_tab_colour (sheet) != O42_TAB_NO_COLOUR)
        tab_colour_class (tab, o42_sheet_tab_colour (sheet));
      gtk_widget_set_focusable (tab, FALSE);
      g_object_set_data (G_OBJECT (tab), "o42-sheet", GINT_TO_POINTER (i));
      g_signal_connect (tab, "clicked", G_CALLBACK (on_tab_clicked), self);
      {
        GtkGesture *secondary = gtk_gesture_click_new ();
        GtkGesture *twice = gtk_gesture_click_new ();

        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (secondary), GDK_BUTTON_SECONDARY);
        g_signal_connect (secondary, "pressed", G_CALLBACK (on_tab_secondary), self);
        gtk_widget_add_controller (tab, GTK_EVENT_CONTROLLER (secondary));
        g_signal_connect (twice, "pressed", G_CALLBACK (on_tab_double_click), self);
        gtk_widget_add_controller (tab, GTK_EVENT_CONTROLLER (twice));
        {
          GtkGesture *drag = gtk_gesture_drag_new ();

          g_signal_connect (drag, "drag-end", G_CALLBACK (on_tab_drag_end), self);
          gtk_widget_add_controller (tab, GTK_EVENT_CONTROLLER (drag));
        }
      }
      gtk_box_append (GTK_BOX (self->tabs), tab);
    }
}

void
o42_window_show_sheet (O42Window *self, int index)
{
  O42Sheet *sheet = o42_book_sheet (self->book, index);

  if (sheet == NULL)
    return;

  if (o42_grid_is_editing (self->grid))
    o42_grid_commit_edit (self->grid);

  self->sheet = sheet;
  o42_grid_set_sheet (self->grid, sheet);
  window_rebuild_tabs (self);
  window_sync (self);
}

static void
action_next_sheet (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  int index = o42_book_sheet_index (self->book, self->sheet);
  (void) a; (void) p;
  if (index + 1 < o42_book_n_sheets (self->book))
    window_show_sheet (self, index + 1);
}

static void
action_prev_sheet (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  int index = o42_book_sheet_index (self->book, self->sheet);
  (void) a; (void) p;
  if (index > 0)
    window_show_sheet (self, index - 1);
}

/* Insert > Worksheet puts the new sheet before the current one, as Excel
 * 5 did. */
static void
action_insert_sheet (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  int index = o42_book_sheet_index (self->book, self->sheet);
  (void) a; (void) p;
  o42_book_add_sheet (self->book, NULL, index);
  o42_sheet_set_modified (self->sheet, TRUE);
  window_show_sheet (self, index);
  window_tell_book (self, "sheets");
}

static void
on_delete_sheet_choice (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Window *self = data;
  int choice = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), result, NULL);
  int index = o42_book_sheet_index (self->book, self->sheet);

  if (choice == 0 && o42_book_remove_sheet (self->book, index))
    {
      int n = o42_book_n_sheets (self->book);
      o42_book_set_modified (self->book, TRUE);
      window_show_sheet (self, MIN (index, n - 1));
      window_tell_book (self, "sheets");
    }
}

static void
action_delete_sheet (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkAlertDialog *dialog;
  const char *buttons[] = { "_OK", "_Cancel", NULL };
  char *message;

  (void) a; (void) p;

  if (o42_book_n_sheets (self->book) < 2)
    return;

  message = g_strdup_printf ("Delete %s?  The sheet will be gone for good.",
                             o42_sheet_get_name (self->sheet));
  dialog = gtk_alert_dialog_new ("%s", message);
  gtk_alert_dialog_set_buttons (dialog, buttons);
  gtk_alert_dialog_set_default_button (dialog, 0);
  gtk_alert_dialog_set_cancel_button (dialog, 1);
  gtk_alert_dialog_choose (dialog, GTK_WINDOW (self), NULL, on_delete_sheet_choice, self);
  g_object_unref (dialog);
  g_free (message);
}

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *entry;
} RenamePrompt;

static void
on_rename_ok (GtkWidget *w, gpointer data)
{
  RenamePrompt *prompt = data;
  const char *name = gtk_editable_get_text (GTK_EDITABLE (prompt->entry));
  O42Window *self = prompt->window;

  (void) w;

  if (o42_book_rename_sheet (self->book, o42_book_sheet_index (self->book, self->sheet), name))
    {
      o42_sheet_set_modified (self->sheet, TRUE);
      window_rebuild_tabs (self);
      window_sync (self);
      window_tell_book (self, "sheets");
      gtk_window_destroy (GTK_WINDOW (prompt->dialog));
    }
  else
    show_error (self, "That name cannot be used for a sheet.", NULL);
}

static void
action_rename_sheet (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  RenamePrompt *prompt = g_new0 (RenamePrompt, 1);
  GtkWidget *content, *buttons, *row, *ok;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Rename Sheet"), TRUE, &content, &buttons);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Name:")));
  prompt->entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (prompt->entry), o42_sheet_get_name (self->sheet));
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->entry), 24);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->entry), TRUE);
  gtk_box_append (GTK_BOX (row), prompt->entry);
  gtk_box_append (GTK_BOX (content), row);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_rename_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->entry);
  gtk_editable_select_region (GTK_EDITABLE (prompt->entry), 0, -1);
}

/* ---- Insert Cells and Delete ------------------------------------------- */

/* Excel 5's two small dialogs, one shape: shift the cells one way or the
 * other, or take the whole rows or columns. */
typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *choice[4];
  gboolean   insert;
} CellsPrompt;

static void
on_cells_ok (GtkWidget *w, gpointer data)
{
  CellsPrompt *prompt = data;
  O42Window *self = prompt->window;
  O42Range sel;
  int which = 0;

  (void) w;

  for (int i = 0; i < 4; i++)
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->choice[i])))
      which = i;

  o42_grid_get_selection (self->grid, &sel);
  if (o42_grid_is_editing (self->grid))
    o42_grid_commit_edit (self->grid);

  switch (which)
    {
    case 0:  o42_sheet_shift_cells (self->sheet, &sel, FALSE, prompt->insert); break;
    case 1:  o42_sheet_shift_cells (self->sheet, &sel, TRUE, prompt->insert); break;
    case 2:  if (prompt->insert) o42_grid_insert_rows (self->grid); else o42_grid_delete_rows (self->grid); break;
    default: if (prompt->insert) o42_grid_insert_columns (self->grid); else o42_grid_delete_columns (self->grid); break;
    }

  o42_grid_refresh (self->grid);
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
cells_prompt (O42Window *self, gboolean insert)
{
  CellsPrompt *prompt = g_new0 (CellsPrompt, 1);
  GtkWidget *content, *buttons, *ok;
  static const char *insert_names[4] = { "Shift cells _right", "Shift cells _down", "Entire r_ow", "Entire _column" };
  static const char *delete_names[4] = { "Shift cells _left", "Shift cells _up", "Entire r_ow", "Entire _column" };
  const char *const *names = insert ? insert_names : delete_names;

  prompt->window = self;
  prompt->insert = insert;
  prompt->dialog = dialog_frame (self, insert ? _("Insert") : _("Delete"), TRUE, &content, &buttons);

  for (int i = 0; i < 4; i++)
    {
      prompt->choice[i] = gtk_check_button_new_with_mnemonic (names[i]);
      if (i > 0)
        gtk_check_button_set_group (GTK_CHECK_BUTTON (prompt->choice[i]),
                                    GTK_CHECK_BUTTON (prompt->choice[0]));
      gtk_box_append (GTK_BOX (content), prompt->choice[i]);
    }
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->choice[1]), TRUE);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_cells_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

static void action_insert_cells (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; cells_prompt (d, TRUE); }
static void action_delete_cells (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; cells_prompt (d, FALSE); }

/* ---- Chart Wizard ----------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *kind[17];
  GtkWidget *title;
  GtkWidget *row_labels, *col_labels;
  GtkWidget *in_rows;
  GtkWidget *own_sheet;
} ChartPrompt;

static void
on_chart_ok (GtkWidget *w, gpointer data)
{
  ChartPrompt *prompt = data;
  O42ChartKind kind = O42_CHART_COLUMN;

  (void) w;

  {
    static const O42ChartKind kinds[17] = { O42_CHART_COLUMN, O42_CHART_STACKED, O42_CHART_PERCENT,
                                            O42_CHART_BAR, O42_CHART_LINE, O42_CHART_AREA,
                                            O42_CHART_PIE, O42_CHART_SCATTER,
                                            O42_CHART_DOUGHNUT, O42_CHART_RADAR, O42_CHART_BUBBLE,
                                            O42_CHART_STOCK, O42_CHART_SURFACE,
                                            O42_CHART_BOX, O42_CHART_HISTOGRAM,
                                            O42_CHART_POLAR, O42_CHART_CONTOUR };
    for (int i = 0; i < 17; i++)
      if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->kind[i])))
        kind = kinds[i];
  }

  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->own_sheet)))
    {
      /* A chart sheet: a new sheet after this one, holding one chart
       * that plots the cells selected here. */
      O42Window *self = prompt->window;
      O42Sheet *from = self->sheet;
      int index = o42_book_sheet_index (self->book, from) + 1;
      O42Sheet *made = o42_book_add_sheet (self->book, NULL, index);
      O42Range sel;
      O42Chart *chart;

      o42_grid_get_selection (self->grid, &sel);
      o42_sheet_set_chart_sheet (made, TRUE);
      chart = o42_sheet_add_chart (made, kind, &sel, 0, 0);
      if (chart != NULL)
        {
          g_free (chart->data_sheet);
          chart->data_sheet = g_strdup (o42_sheet_get_name (from));
          g_free (chart->title);
          chart->title = g_strdup (gtk_editable_get_text (GTK_EDITABLE (prompt->title)));
          chart->series_in_rows = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->in_rows));
          chart->first_row_labels = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->row_labels));
          chart->first_col_labels = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->col_labels));
        }
      o42_sheet_set_modified (made, TRUE);
      window_show_sheet (self, index);
      window_tell_book (self, "sheets");
    }
  else
    o42_grid_insert_chart (prompt->window->grid, kind,
                           gtk_editable_get_text (GTK_EDITABLE (prompt->title)),
                           gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->in_rows)),
                           gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->row_labels)),
                           gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->col_labels)));
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
action_insert_chart (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  ChartPrompt *prompt = g_new0 (ChartPrompt, 1);
  GtkWidget *content, *buttons, *row, *ok;
  static const char *names[17] = { N_("_Column"), N_("_Stacked column"), N_("100% stac_ked column"),
                                   N_("_Bar"), N_("_Line"), N_("_Area"), N_("_Pie"), N_("_XY (Scatter)"),
                                   N_("_Doughnut"), N_("_Radar"), N_("B_ubble"),
                                   N_("Sto_ck (high-low-close)"), N_("Surfa_ce"),
                                   N_("Box and w_hiskers"), N_("Histo_gram"),
                                   N_("Pola_r"), N_("Con_tour") };
  O42Range sel;
  char *a_name, *b_name, *range_text;

  (void) a; (void) p;

  o42_grid_get_selection (self->grid, &sel);
  a_name = o42_ref_name (sel.row0, sel.col0);
  b_name = o42_ref_name (sel.row1, sel.col1);
  range_text = g_strdup_printf ("Chart of %s:%s", a_name, b_name);

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Chart Wizard"), TRUE, &content, &buttons);
  gtk_box_append (GTK_BOX (content), gtk_label_new (range_text));

  for (int i = 0; i < 17; i++)
    {
      prompt->kind[i] = gtk_check_button_new_with_mnemonic (_(names[i]));
      if (i > 0)
        gtk_check_button_set_group (GTK_CHECK_BUTTON (prompt->kind[i]),
                                    GTK_CHECK_BUTTON (prompt->kind[0]));
      gtk_box_append (GTK_BOX (content), prompt->kind[i]);
    }
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->kind[0]), TRUE);

  {
    gboolean first_row = FALSE, first_col = FALSE;

    o42_grid_guess_chart_labels (self->grid, &first_row, &first_col);
    prompt->row_labels = gtk_check_button_new_with_mnemonic ( _("First _row is a heading"));
    prompt->col_labels = gtk_check_button_new_with_mnemonic ( _("First col_umn is a heading"));
    gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->row_labels), first_row);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->col_labels), first_col);
    gtk_box_append (GTK_BOX (content), prompt->row_labels);
    gtk_box_append (GTK_BOX (content), prompt->col_labels);
  }

  /* Which way the series lie.  The same cells make a different chart
   * either way, so the wizard asks, as Excel 5's second step did. */
  {
    GtkWidget *in_cols = gtk_check_button_new_with_mnemonic ( _("Series in _columns"));

    prompt->in_rows = gtk_check_button_new_with_mnemonic ( _("Series in ro_ws"));
    gtk_check_button_set_group (GTK_CHECK_BUTTON (prompt->in_rows),
                                GTK_CHECK_BUTTON (in_cols));
    gtk_check_button_set_active (GTK_CHECK_BUTTON (in_cols), TRUE);
    gtk_box_append (GTK_BOX (content), in_cols);
    gtk_box_append (GTK_BOX (content), prompt->in_rows);
  }

  /* Excel's last step asks where the chart is to go. */
  prompt->own_sheet = gtk_check_button_new_with_mnemonic ( _("On a sheet of its _own"));
  gtk_box_append (GTK_BOX (content), prompt->own_sheet);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Title:")));
  prompt->title = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->title), 24);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->title), TRUE);
  gtk_box_append (GTK_BOX (row), prompt->title);
  gtk_box_append (GTK_BOX (content), row);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_chart_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));

  g_free (a_name);
  g_free (b_name);
  g_free (range_text);
}

/* ---- Format > Control --------------------------------------------------- */

/* What a form control needs beyond a shape: the cell it drives, the
 * range a list draws its items from, the script a button runs, and the
 * bounds a spinner and a scroll bar count between. */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  guint      shape_id;
  GtkWidget *caption, *link, *source, *script;
  GtkWidget *min, *max, *step, *page;
} ControlPrompt;

static void
control_set_text (char **field, GtkWidget *entry)
{
  const char *text = entry != NULL ? gtk_editable_get_text (GTK_EDITABLE (entry)) : NULL;

  g_free (*field);
  *field = (text != NULL && *text != 0) ? g_strdup (text) : NULL;
}

static void
on_control_ok (GtkWidget *w, gpointer data)
{
  ControlPrompt *prompt = data;
  O42Shape *shape = o42_sheet_find_shape (prompt->window->sheet, prompt->shape_id);

  (void) w;
  if (shape != NULL)
    {
      o42_sheet_begin_group (prompt->window->sheet);
      o42_sheet_capture_object (prompt->window->sheet, shape->id);
      if (prompt->caption != NULL)
        {
          g_free (shape->text);
          shape->text = g_strdup (gtk_editable_get_text (GTK_EDITABLE (prompt->caption)));
        }
      control_set_text (&shape->link, prompt->link);
      control_set_text (&shape->source, prompt->source);
      control_set_text (&shape->script, prompt->script);
      if (prompt->min != NULL)
        {
          shape->min = gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->min));
          shape->max = gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->max));
          shape->step = gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->step));
          shape->page = gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->page));
        }
      o42_sheet_end_group (prompt->window->sheet);
      o42_sheet_set_modified (prompt->window->sheet, TRUE);
      o42_grid_refresh (prompt->window->grid);
      window_sync (prompt->window);
    }
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static GtkWidget *
control_entry (GtkWidget *grid, int row, const char *label, const char *value)
{
  GtkWidget *entry = gtk_entry_new ();

  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  if (value != NULL)
    gtk_editable_set_text (GTK_EDITABLE (entry), value);
  return labelled (grid, row, label, entry);
}

static void
action_format_control (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  O42Shape *shape = o42_grid_selected_shape (self->grid);
  ControlPrompt *prompt;
  GtkWidget *content, *buttons, *grid, *ok;
  int row = 0;

  (void) a; (void) p;
  if (shape == NULL || !o42_shape_is_control (shape->kind))
    {
      GPtrArray *shapes = o42_sheet_shapes (self->sheet);
      O42Shape *only = NULL;

      shape = NULL;
      for (guint i = 0; i < shapes->len; i++)
        {
          O42Shape *candidate = g_ptr_array_index (shapes, i);

          if (o42_shape_is_control (candidate->kind))
            {
              if (only != NULL)
                { only = NULL; break; }
              only = candidate;
            }
        }
      shape = only;
    }
  if (shape == NULL)
    {
      show_error (self, "Ctrl+click a control first: a plain click works it, "
                        "Ctrl+click takes hold of it.", NULL);
      return;
    }

  prompt = g_new0 (ControlPrompt, 1);
  prompt->window = self;
  prompt->shape_id = shape->id;
  prompt->dialog = dialog_frame (self, _("Format Control"), TRUE, &content, &buttons);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);

  if (shape->kind != O42_SHAPE_SPINNER && shape->kind != O42_SHAPE_SCROLLBAR &&
      shape->kind != O42_SHAPE_LISTBOX && shape->kind != O42_SHAPE_COMBO)
    prompt->caption = control_entry (grid, row++, "Caption:", shape->text);

  if (shape->kind != O42_SHAPE_BUTTON && shape->kind != O42_SHAPE_LABEL &&
      shape->kind != O42_SHAPE_GROUPBOX)
    prompt->link = control_entry (grid, row++, "Cell link:", shape->link);

  if (shape->kind == O42_SHAPE_LISTBOX || shape->kind == O42_SHAPE_COMBO)
    prompt->source = control_entry (grid, row++, "Input range:", shape->source);

  if (shape->kind == O42_SHAPE_BUTTON)
    prompt->script = control_entry (grid, row++, "Script:", shape->script);

  if (shape->kind == O42_SHAPE_SPINNER || shape->kind == O42_SHAPE_SCROLLBAR)
    {
      prompt->min = labelled (grid, row++, "Minimum:",
                              gtk_spin_button_new_with_range (-1e9, 1e9, 1));
      prompt->max = labelled (grid, row++, "Maximum:",
                              gtk_spin_button_new_with_range (-1e9, 1e9, 1));
      prompt->step = labelled (grid, row++, "Increment:",
                               gtk_spin_button_new_with_range (0, 1e6, 1));
      prompt->page = labelled (grid, row++, "Page change:",
                               gtk_spin_button_new_with_range (0, 1e6, 1));
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->min), shape->min);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->max), shape->max);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->step), shape->step);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->page), shape->page);
    }

  gtk_box_append (GTK_BOX (content), grid);
  gtk_box_append (GTK_BOX (content),
                  gtk_label_new (_("A plain click works the control; Ctrl+click takes hold of it.")));

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_control_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* A button on the sheet was pressed. */
static void
on_grid_run_script (O42Grid *grid, const char *name, gpointer data)
{
  O42Window *self = data;
  const char *code = name != NULL ? o42_book_script_code (self->book, name) : NULL;

  (void) grid;
  if (code == NULL)
    {
      show_error (self, "This button names a script the book has not got.", NULL);
      return;
    }
  window_run_script (self, name, code);
  window_sync (self);
}

/* ---- Data > Database ---------------------------------------------------- */

/* A book may have one SQLite database: a file beside it, or one carried
 * inside the book.  Data > Get Data lays a query's answer out on the
 * sheet and the sheet remembers the query, so Refresh can run it again;
 * SQLVALUE() in a cell asks the same database a question of its own.
 * The connection is opened when it is first wanted and closed with the
 * window. */

static O42Db *
window_db (O42Window *self, gboolean complain)
{
  GError *error = NULL;

  if (!o42_db_available ())
    {
      if (complain)
        show_error (self, "This build of office42 has no SQLite in it.", NULL);
      return NULL;
    }
  if (self->db != NULL &&
      g_strcmp0 (o42_db_path (self->db), o42_book_database (self->book, NULL)) == 0)
    return self->db;

  g_clear_pointer (&self->db, o42_db_close);
  if (o42_book_database (self->book, NULL) == NULL)
    {
      if (complain)
        show_error (self, "This book has no database: Data > Database > Connect "
                          "opens one, or Embed New puts one inside the book.", NULL);
      return NULL;
    }
  self->db = o42_db_for_book (self->book, &error);
  if (self->db == NULL && complain)
    show_error (self, "The database would not open.", error);
  g_clear_error (&error);
  return self->db;
}

static void
window_db_connected (O42Window *self)
{
  o42_db_register_function (self->book);
  o42_sheet_stale_formulas (self->sheet);
  o42_grid_refresh (self->grid);
  window_sync (self);
}

static void
on_db_open_response (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Window *self = data;
  GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, NULL);
  char *path;

  if (file == NULL)
    return;
  path = g_file_get_path (file);
  if (path != NULL)
    {
      o42_book_set_database (self->book, path, FALSE);
      g_clear_pointer (&self->db, o42_db_close);
      if (window_db (self, TRUE) != NULL)
        window_db_connected (self);
    }
  g_free (path);
  g_object_unref (file);
}

static void
action_db_connect (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkFileDialog *dialog;
  GListStore *filters;
  GtkFileFilter *databases, *all;

  (void) a; (void) p;
  if (!o42_db_available ())
    {
      show_error (self, "This build of office42 has no SQLite in it.", NULL);
      return;
    }

  dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Open Database"));
  filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  databases = gtk_file_filter_new ();
  gtk_file_filter_set_name (databases, _("SQLite databases"));
  gtk_file_filter_add_pattern (databases, "*.sqlite");
  gtk_file_filter_add_pattern (databases, "*.sqlite3");
  gtk_file_filter_add_pattern (databases, "*.db");
  g_list_store_append (filters, databases);
  all = gtk_file_filter_new ();
  gtk_file_filter_set_name (all, _("All files"));
  gtk_file_filter_add_pattern (all, "*");
  g_list_store_append (filters, all);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  g_object_unref (filters);
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_db_open_response, self);
  g_object_unref (dialog);
}

static void
action_db_embed (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  char *path = NULL;
  int fd;

  (void) a; (void) p;
  if (!o42_db_available ())
    {
      show_error (self, "This build of office42 has no SQLite in it.", NULL);
      return;
    }

  /* The database lives in a file of its own while the book is open --
   * SQLite reads files -- and is written into the book when it is
   * saved, so that it travels with it. */
  fd = g_file_open_tmp ("office42-XXXXXX.sqlite", &path, NULL);
  if (fd < 0)
    {
      show_error (self, "There was nowhere to put the database.", NULL);
      return;
    }
  g_close (fd, NULL);
  o42_book_set_database (self->book, path, TRUE);
  g_clear_pointer (&self->db, o42_db_close);
  if (window_db (self, TRUE) != NULL)
    {
      window_db_connected (self);
      o42_book_set_modified (self->book, TRUE);
    }
  g_free (path);
}

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *sql;
  GtkWidget *headings;
} QueryPrompt;

static char *
query_prompt_text (QueryPrompt *prompt)
{
  GtkTextIter a, b;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->sql));

  gtk_text_buffer_get_bounds (buffer, &a, &b);
  return gtk_text_buffer_get_text (buffer, &a, &b, FALSE);
}

static void
on_query_ok (GtkWidget *w, gpointer data)
{
  QueryPrompt *prompt = data;
  O42Window *self = prompt->window;
  O42Db *db = window_db (self, TRUE);
  char *sql = query_prompt_text (prompt);
  GError *error = NULL;
  O42Range at;

  (void) w;
  if (db == NULL || sql == NULL || *sql == 0)
    {
      g_free (sql);
      return;
    }

  o42_grid_get_selection (self->grid, &at);
  if (!o42_db_query_into (db, self->sheet, sql, at.row0, at.col0,
                          gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->headings)),
                          &at, &error))
    show_error (self, "The query failed.", error);
  else
    {
      o42_sheet_set_modified (self->sheet, TRUE);
      o42_grid_refresh (self->grid);
      window_sync (self);
    }
  g_clear_error (&error);
  g_free (sql);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

/* Clicking a table's name writes the query that reads it. */
static void
on_db_table_activated (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
  QueryPrompt *prompt = data;
  const char *table = g_object_get_data (G_OBJECT (row), "o42-table");
  char *sql;

  (void) box;
  if (table == NULL)
    return;
  sql = g_strdup_printf ("SELECT * FROM \"%s\"", table);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->sql)), sql, -1);
  g_free (sql);
}

static void
action_db_query (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  O42Db *db = window_db (self, TRUE);
  QueryPrompt *prompt;
  GtkWidget *content, *buttons, *scrolled, *ok, *list, *tables_scrolled;
  char **tables;

  (void) a; (void) p;
  if (db == NULL)
    return;

  prompt = g_new0 (QueryPrompt, 1);
  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Get Data"), TRUE, &content, &buttons);

  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Tables in the database:")));
  list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_SINGLE);
  g_signal_connect (list, "row-activated", G_CALLBACK (on_db_table_activated), prompt);
  tables = o42_db_tables (db);
  for (int i = 0; tables != NULL && tables[i] != NULL; i++)
    {
      GtkWidget *label = gtk_label_new (tables[i]);
      GtkWidget *row = gtk_list_box_row_new ();

      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
      g_object_set_data_full (G_OBJECT (row), "o42-table", g_strdup (tables[i]), g_free);
      gtk_list_box_append (GTK_LIST_BOX (list), row);
    }
  g_strfreev (tables);
  tables_scrolled = gtk_scrolled_window_new ();
  gtk_widget_set_size_request (tables_scrolled, 360, 90);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (tables_scrolled), list);
  gtk_widget_add_css_class (tables_scrolled, "frame");
  gtk_box_append (GTK_BOX (content), tables_scrolled);

  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Query:")));
  prompt->sql = gtk_text_view_new ();
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (prompt->sql), TRUE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (prompt->sql), GTK_WRAP_WORD);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->sql)),
                            "SELECT * FROM ", -1);
  scrolled = gtk_scrolled_window_new ();
  gtk_widget_set_size_request (scrolled, 360, 110);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), prompt->sql);
  gtk_widget_add_css_class (scrolled, "frame");
  gtk_box_append (GTK_BOX (content), scrolled);

  prompt->headings = gtk_check_button_new_with_mnemonic ( _("Write the column _names above it"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->headings), TRUE);
  gtk_box_append (GTK_BOX (content), prompt->headings);
  gtk_box_append (GTK_BOX (content),
                  gtk_label_new ("The answer is laid out from the active cell, and "
                                 "Data > Refresh Queries runs it again."));

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_query_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

static void
action_db_refresh (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  O42Db *db = window_db (self, TRUE);
  GError *error = NULL;
  int done;

  (void) a; (void) p;
  if (db == NULL)
    return;
  done = o42_db_refresh (db, self->sheet, &error);
  if (done < 0)
    show_error (self, "A query failed.", error);
  else
    {
      o42_sheet_set_modified (self->sheet, TRUE);
      o42_grid_refresh (self->grid);
      window_sync (self);
      {
        char *said = g_strdup_printf (done == 1 ? "%d query refreshed"
                                                : "%d queries refreshed", done);

        gtk_label_set_text (GTK_LABEL (self->status_label), said);
        g_free (said);
      }
    }
  g_clear_error (&error);
}

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *table;
  GtkWidget *headings;
  O42Range   range;
} SendPrompt;

static void
on_db_send_ok (GtkWidget *w, gpointer data)
{
  SendPrompt *prompt = data;
  O42Window *self = prompt->window;
  O42Db *db = window_db (self, TRUE);
  const char *table = gtk_editable_get_text (GTK_EDITABLE (prompt->table));
  GError *error = NULL;

  (void) w;
  if (db != NULL && table != NULL && *table != 0)
    {
      if (!o42_db_put_range (db, self->sheet, &prompt->range, table,
                             gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->headings)),
                             &error))
        show_error (self, "The rows would not go in.", error);
      else
        {
          o42_sheet_stale_formulas (self->sheet);
          o42_grid_refresh (self->grid);
          {
            char *said = g_strdup_printf ("The selection went into %s", table);

            gtk_label_set_text (GTK_LABEL (self->status_label), said);
            g_free (said);
          }
        }
      g_clear_error (&error);
    }
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
action_db_send (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  SendPrompt *prompt;
  GtkWidget *content, *buttons, *grid, *ok, *entry;

  (void) a; (void) p;
  if (window_db (self, TRUE) == NULL)
    return;

  prompt = g_new0 (SendPrompt, 1);
  prompt->window = self;
  o42_grid_get_selection (self->grid, &prompt->range);
  prompt->dialog = dialog_frame (self, _("Send to Table"), TRUE, &content, &buttons);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  entry = gtk_entry_new ();
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  prompt->table = labelled (grid, 0, _("Table:"), entry);
  gtk_box_append (GTK_BOX (content), grid);
  prompt->headings = gtk_check_button_new_with_mnemonic ( _("The first row _names the columns"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->headings), TRUE);
  gtk_box_append (GTK_BOX (content), prompt->headings);
  gtk_box_append (GTK_BOX (content),
                  gtk_label_new ("The selection is added to the table, which is made "
                                 "if it is not there."));

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_db_send_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Insert > Shape, Format > Shape ------------------------------------- */

static void
action_shape (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  O42ShapeKind kind = O42_SHAPE_RECT;
  const char *name = p != NULL ? g_variant_get_string (p, NULL) : "rectangle";
  const char *caption = "";

  (void) a;
  o42_shape_kind_parse (name, &kind);
  /* A new control says what it is until it is given a caption of its
   * own; the ones that show no caption start empty. */
  switch (kind)
    {
    case O42_SHAPE_TEXT:     caption = "Text"; break;
    case O42_SHAPE_BUTTON:   caption = "Button"; break;
    case O42_SHAPE_CHECKBOX: caption = "Check Box"; break;
    case O42_SHAPE_OPTION:   caption = "Option Button"; break;
    case O42_SHAPE_LABEL:    caption = "Label"; break;
    case O42_SHAPE_GROUPBOX: caption = "Group Box"; break;
    default:                 break;
    }
  o42_grid_insert_shape (self->grid, kind, caption);
  window_sync (self);
}

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  guint      shape_id;
  GtkWidget *text, *fill, *no_fill, *line, *width;
} ShapePrompt;

static void
on_shape_format_ok (GtkWidget *w, gpointer data)
{
  ShapePrompt *prompt = data;
  O42Shape *shape = o42_sheet_find_shape (prompt->window->sheet, prompt->shape_id);
  GtkTextIter a, b;

  (void) w;
  if (shape != NULL)
    {
      o42_sheet_begin_group (prompt->window->sheet);
      o42_sheet_capture_object (prompt->window->sheet, shape->id);
      gtk_text_buffer_get_bounds (gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->text)), &a, &b);
      g_free (shape->text);
      shape->text = gtk_text_buffer_get_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->text)), &a, &b, FALSE);
      shape->fill = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->no_fill))
                    ? O42_FILL_NONE
                    : colour_from_rgba (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->fill)));
      shape->line = colour_from_rgba (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->line)));
      shape->line_width = gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->width));
      o42_sheet_end_group (prompt->window->sheet);
      o42_sheet_set_modified (prompt->window->sheet, TRUE);
      o42_grid_refresh (prompt->window->grid);
      window_sync (prompt->window);
    }
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
action_format_shape (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  O42Shape *shape = o42_grid_selected_shape (self->grid);
  ShapePrompt *prompt;
  GtkWidget *content, *buttons, *grid, *scrolled, *ok;

  (void) a; (void) p;
  if (shape == NULL)
    {
      GPtrArray *shapes = o42_sheet_shapes (self->sheet);
      if (shapes->len == 1)
        shape = g_ptr_array_index (shapes, 0);
    }
  if (shape == NULL)
    {
      show_error (self, "Click a shape first; Format > Shape works on the selected one.", NULL);
      return;
    }

  prompt = g_new0 (ShapePrompt, 1);
  prompt->window = self;
  prompt->shape_id = shape->id;
  prompt->dialog = dialog_frame (self, _("Format Shape"), TRUE, &content, &buttons);

  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Text:")));
  prompt->text = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (prompt->text), GTK_WRAP_WORD);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (prompt->text)),
                            shape->text != NULL ? shape->text : "", -1);
  scrolled = gtk_scrolled_window_new ();
  gtk_widget_set_size_request (scrolled, 300, 90);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), prompt->text);
  gtk_widget_add_css_class (scrolled, "frame");
  gtk_box_append (GTK_BOX (content), scrolled);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->fill = labelled (grid, 0, _("Fill:"), colour_button (shape->fill == O42_FILL_NONE ? 0xFFFFFF : shape->fill, _("Shape Fill")));
  prompt->line = labelled (grid, 1, _("Line:"), colour_button (shape->line, _("Shape Line")));
  prompt->width = labelled (grid, 2, _("Line width:"), gtk_spin_button_new_with_range (0.5, 12, 0.5));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->width), shape->line_width);
  gtk_box_append (GTK_BOX (content), grid);
  prompt->no_fill = gtk_check_button_new_with_mnemonic ( _("_No fill"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->no_fill), shape->fill == O42_FILL_NONE);
  gtk_box_append (GTK_BOX (content), prompt->no_fill);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_shape_format_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Format > Style ---------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *list;
  GtkWidget *name;
} StylePrompt;

static void
style_prompt_fill (StylePrompt *prompt, const char *select)
{
  GtkWidget *child;
  int n = o42_book_n_styles (prompt->window->book);

  while ((child = gtk_widget_get_first_child (prompt->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (prompt->list), child);
  for (int i = 0; i < n; i++)
    {
      const char *name = o42_book_style_name (prompt->window->book, i);
      GtkWidget *label = gtk_label_new (name);

      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_widget_set_margin_start (label, 6);
      gtk_widget_set_margin_end (label, 6);
      gtk_list_box_append (GTK_LIST_BOX (prompt->list), label);
      if (select != NULL && g_ascii_strcasecmp (name, select) == 0)
        gtk_list_box_select_row (GTK_LIST_BOX (prompt->list),
                                 gtk_list_box_get_row_at_index (GTK_LIST_BOX (prompt->list), i));
    }
}

/* The name in the entry, or the one selected in the list. */
static char *
style_prompt_name (StylePrompt *prompt)
{
  const char *typed = gtk_editable_get_text (GTK_EDITABLE (prompt->name));
  GtkListBoxRow *row;

  if (*typed != '\0')
    return g_strdup (typed);
  row = gtk_list_box_get_selected_row (GTK_LIST_BOX (prompt->list));
  if (row == NULL)
    return NULL;
  return g_strdup (o42_book_style_name (prompt->window->book, gtk_list_box_row_get_index (row)));
}

static void
on_style_row_selected (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  StylePrompt *prompt = data;
  (void) list;
  if (row != NULL)
    gtk_editable_set_text (GTK_EDITABLE (prompt->name),
                           o42_book_style_name (prompt->window->book, gtk_list_box_row_get_index (row)));
}

static void
on_style_apply (GtkWidget *w, gpointer data)
{
  StylePrompt *prompt = data;
  char *name = style_prompt_name (prompt);
  O42Range sel;

  (void) w;
  if (name == NULL)
    return;
  o42_grid_get_selection (prompt->window->grid, &sel);
  o42_sheet_apply_style (prompt->window->sheet, &sel, name);
  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
  g_free (name);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_style_define (GtkWidget *w, gpointer data)
{
  StylePrompt *prompt = data;
  char *name = style_prompt_name (prompt);
  int row, col;

  (void) w;
  if (name == NULL || *name == '\0')
    {
      gtk_widget_grab_focus (prompt->name);
      g_free (name);
      return;
    }
  o42_grid_get_active (prompt->window->grid, &row, &col);
  o42_book_set_style (prompt->window->book, name,
                      o42_sheet_get_fmt (prompt->window->sheet, row, col), O42_FMT_ALL);
  style_prompt_fill (prompt, name);
  o42_grid_refresh (prompt->window->grid);
  window_sync (prompt->window);
  g_free (name);
}

static void
on_style_delete (GtkWidget *w, gpointer data)
{
  StylePrompt *prompt = data;
  char *name = style_prompt_name (prompt);

  (void) w;
  if (name != NULL && o42_book_remove_style (prompt->window->book, name))
    {
      gtk_editable_set_text (GTK_EDITABLE (prompt->name), "");
      style_prompt_fill (prompt, NULL);
    }
  g_free (name);
}

static void
action_style (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  StylePrompt *prompt = g_new0 (StylePrompt, 1);
  GtkWidget *content, *buttons, *scroller, *row, *apply;
  int active_row, active_col;
  const char *worn;

  (void) a; (void) p;
  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Style"), TRUE, &content, &buttons);
  gtk_window_set_default_size (GTK_WINDOW (prompt->dialog), 300, 380);

  prompt->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (prompt->list), GTK_SELECTION_SINGLE);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), prompt->list);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_widget_add_css_class (scroller, "frame");
  gtk_box_append (GTK_BOX (content), scroller);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Style name:")));
  prompt->name = gtk_entry_new ();
  gtk_widget_set_hexpand (prompt->name, TRUE);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->name), TRUE);
  gtk_box_append (GTK_BOX (row), prompt->name);
  gtk_box_append (GTK_BOX (content), row);

  {
    GtkWidget *hint = gtk_label_new ("Define takes the active cell's formatting; redefining a style "
                                     "restyles every cell wearing it.");
    gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (hint), 40);
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_box_append (GTK_BOX (content), hint);
  }

  o42_grid_get_active (self->grid, &active_row, &active_col);
  worn = o42_sheet_cell_style (self->sheet, active_row, active_col);
  style_prompt_fill (prompt, worn);
  if (worn != NULL)
    gtk_editable_set_text (GTK_EDITABLE (prompt->name), worn);
  g_signal_connect (prompt->list, "row-selected", G_CALLBACK (on_style_row_selected), prompt);

  dialog_button (buttons, _("_Define"), G_CALLBACK (on_style_define), prompt);
  dialog_button (buttons, _("De_lete"), G_CALLBACK (on_style_delete), prompt);
  apply = dialog_button (buttons, _("_Apply"), G_CALLBACK (on_style_apply), prompt);
  dialog_button (buttons, _("_Close"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), apply);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Format > Chart --------------------------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  guint      chart_id;
  GtkWidget *title, *x_title, *y_title, *legend, *gridlines, *labels, *min, *max;
  GtkWidget *trend, *trend_order, *err_bars, *err_value, *font, *font_size, *three_d;
  GtkWidget *marker, *marker_size, *marker_picture;
  GtkWidget *y_format, *secondary;
} ChartFormatPrompt;

static void
on_chart_format_ok (GtkWidget *w, gpointer data)
{
  ChartFormatPrompt *prompt = data;
  O42Sheet *sheet = prompt->window->sheet;
  O42Chart *chart = o42_sheet_find_chart (sheet, prompt->chart_id);
  const char *min_text = gtk_editable_get_text (GTK_EDITABLE (prompt->min));
  const char *max_text = gtk_editable_get_text (GTK_EDITABLE (prompt->max));
  (void) w;

  if (chart != NULL)
    {
      o42_sheet_begin_group (sheet);
      o42_sheet_capture_object (sheet, chart->id);
      g_free (chart->title);
      chart->title = g_strdup (gtk_editable_get_text (GTK_EDITABLE (prompt->title)));
      g_free (chart->x_title);
      chart->x_title = g_strdup (gtk_editable_get_text (GTK_EDITABLE (prompt->x_title)));
      g_free (chart->y_title);
      chart->y_title = g_strdup (gtk_editable_get_text (GTK_EDITABLE (prompt->y_title)));
      chart->legend = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->legend));
      chart->gridlines = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->gridlines));
      chart->data_labels = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->labels));
      g_free (chart->y_format);
      chart->y_format = g_strdup (gtk_editable_get_text (GTK_EDITABLE (prompt->y_format)));
      chart->secondary_from = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->secondary));
      chart->trend = (O42TrendKind) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->trend));
      chart->marker = (O42MarkerKind) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->marker));
      chart->marker_size = gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->marker_size));
      chart->marker_picture = (guint) gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->marker_picture));
      chart->trend_order = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->trend_order));
      chart->three_d = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->three_d));
      chart->err_bars = (O42ErrBarKind) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->err_bars));
      chart->err_value = g_strtod (gtk_editable_get_text (GTK_EDITABLE (prompt->err_value)), NULL);
      g_free (chart->font_family);
      chart->font_family = g_strdup (gtk_editable_get_text (GTK_EDITABLE (prompt->font)));
      chart->font_size = gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->font_size));
      chart->has_min = *min_text != '\0';
      chart->min = g_strtod (min_text, NULL);
      chart->has_max = *max_text != '\0';
      chart->max = g_strtod (max_text, NULL);
      o42_sheet_end_group (sheet);
      o42_sheet_set_modified (sheet, TRUE);
      o42_grid_refresh (prompt->window->grid);
      window_sync (prompt->window);
    }
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
action_format_chart (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  O42Chart *chart = o42_grid_selected_chart (self->grid);
  ChartFormatPrompt *prompt;
  GtkWidget *content, *buttons, *ok, *grid;
  char num[G_ASCII_DTOSTR_BUF_SIZE];

  (void) a; (void) p;
  if (chart == NULL)
    {
      GPtrArray *charts = o42_sheet_charts (self->sheet);
      if (charts->len == 1)
        chart = g_ptr_array_index (charts, 0);
    }
  if (chart == NULL)
    {
      show_error (self, "Click a chart first; Format > Chart works on the selected chart.", NULL);
      return;
    }

  prompt = g_new0 (ChartFormatPrompt, 1);
  prompt->window = self;
  prompt->chart_id = chart->id;
  prompt->dialog = dialog_frame (self, _("Format Chart"), TRUE, &content, &buttons);
  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  prompt->title = labelled (grid, 0, _("Chart title:"), gtk_entry_new ());
  prompt->x_title = labelled (grid, 1, _("Category (X) axis title:"), gtk_entry_new ());
  prompt->y_title = labelled (grid, 2, _("Value (Y) axis title:"), gtk_entry_new ());
  prompt->min = labelled (grid, 3, _("Value axis minimum:"), gtk_entry_new ());
  prompt->max = labelled (grid, 4, _("Value axis maximum:"), gtk_entry_new ());
  prompt->y_format = labelled (grid, 5, _("Value axis format:"), gtk_entry_new ());
  prompt->secondary = labelled (grid, 6, _("Second axis from series:"), gtk_spin_button_new_with_range (0, 32, 1));
  gtk_box_append (GTK_BOX (content), grid);
  gtk_widget_set_size_request (prompt->title, 260, -1);
  gtk_editable_set_text (GTK_EDITABLE (prompt->title), chart->title != NULL ? chart->title : "");
  gtk_editable_set_text (GTK_EDITABLE (prompt->x_title), chart->x_title != NULL ? chart->x_title : "");
  gtk_editable_set_text (GTK_EDITABLE (prompt->y_title), chart->y_title != NULL ? chart->y_title : "");
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->min), _("automatic"));
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->max), _("automatic"));
  if (chart->has_min) gtk_editable_set_text (GTK_EDITABLE (prompt->min), g_ascii_dtostr (num, sizeof num, chart->min));
  if (chart->has_max) gtk_editable_set_text (GTK_EDITABLE (prompt->max), g_ascii_dtostr (num, sizeof num, chart->max));
  gtk_editable_set_text (GTK_EDITABLE (prompt->y_format), chart->y_format != NULL ? chart->y_format : "");
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->y_format), _("General"));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->secondary), chart->secondary_from);
  prompt->legend = gtk_check_button_new_with_mnemonic ( _("Show _legend"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->legend), chart->legend);
  gtk_box_append (GTK_BOX (content), prompt->legend);
  prompt->gridlines = gtk_check_button_new_with_mnemonic ( _("Show _gridlines"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->gridlines), chart->gridlines);
  gtk_box_append (GTK_BOX (content), prompt->gridlines);
  prompt->labels = gtk_check_button_new_with_mnemonic ( _("Show _data labels"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->labels), chart->data_labels);
  gtk_box_append (GTK_BOX (content), prompt->labels);
  prompt->three_d = gtk_check_button_new_with_mnemonic ( _("Draw in three _dimensions"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->three_d), chart->three_d);
  gtk_box_append (GTK_BOX (content), prompt->three_d);
  {
    /* In the order of O42TrendKind, so the row is the kind. */
    static const char *const trends[] = { N_("None"), N_("Linear"), N_("Polynomial"), N_("Exponential"),
                                          N_("Logarithmic"), N_("Power"), N_("Moving average"), NULL };

    prompt->trend = labelled (grid, 7, _("Trendline:"), drop_down_of (trends));
    gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->trend), (guint) chart->trend);
    prompt->trend_order = labelled (grid, 8, "Order or period:",
                                    gtk_spin_button_new_with_range (2, 6, 1));
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->trend_order),
                               CLAMP (chart->trend_order, 2, 6));
  }
  {
    /* In the order of O42ErrBarKind. */
    static const char *const bars[] = { N_("None"), N_("Fixed value"), N_("Percentage"),
                                        N_("Standard deviations"), N_("Standard error"), NULL };
    char *amount = g_strdup_printf ("%g", chart->err_value);

    prompt->err_bars = labelled (grid, 9, _("Error bars:"), drop_down_of (bars));
    gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->err_bars), (guint) chart->err_bars);
    prompt->err_value = labelled (grid, 10, _("Error bar amount:"), gtk_entry_new ());
    gtk_editable_set_text (GTK_EDITABLE (prompt->err_value), amount);
    g_free (amount);
  }
  {
    /* In the order of O42MarkerKind, so the row is the kind.  The
     * picture is named by its number, which "Insert > Picture" gives it
     * and office42-calc's "pictures" prints. */
    static const char *const markers[] = { N_("Automatic"), N_("None"), N_("Circle"), N_("Square"),
                                           N_("Diamond"), N_("Triangle"), N_("Cross"), N_("Plus"),
                                           N_("Star"), N_("Picture"), NULL };

    prompt->marker = labelled (grid, 11, _("Point marker:"), drop_down_of (markers));
    gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->marker), (guint) chart->marker);
    prompt->marker_size = labelled (grid, 12, "Marker size:",
                                    gtk_spin_button_new_with_range (0, 40, 1));
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->marker_size), chart->marker_size);
    prompt->marker_picture = labelled (grid, 13, "Picture number:",
                                       gtk_spin_button_new_with_range (0, 9999, 1));
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->marker_picture), chart->marker_picture);
  }
  prompt->font = labelled (grid, 14, _("Text font:"), gtk_entry_new ());
  gtk_entry_set_placeholder_text (GTK_ENTRY (prompt->font), _("Sans"));
  gtk_editable_set_text (GTK_EDITABLE (prompt->font),
                         chart->font_family != NULL ? chart->font_family : "");
  prompt->font_size = labelled (grid, 15, "Text size:",
                                gtk_spin_button_new_with_range (0, 72, 1));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->font_size), chart->font_size);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_chart_format_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Files ------------------------------------------------------------ */

static void
window_update_title (O42Window *self)
{
  char *name, *title;
  gboolean modified = o42_book_is_modified (self->book);

  name = (self->file != NULL) ? g_file_get_basename (self->file)
                              : g_strdup ("Book1");
  if (self->view_number > 0)
    {
      char *numbered = g_strdup_printf ("%s:%d", name, self->view_number);
      g_free (name);
      name = numbered;
    }

  title = g_strdup_printf ("Office42 Spreadsheet " O42_VERSION " - %s%s", name, modified ? "*" : "");
  gtk_label_set_text (GTK_LABEL (self->title_label), title);
  g_free (title);

  title = g_strdup_printf ("%s%s - Office42 Spreadsheet", name, modified ? "*" : "");
  gtk_window_set_title (GTK_WINDOW (self), title);
  g_free (title);

  g_free (name);
}

static void
window_set_file (O42Window *self, GFile *file)
{
  if (file != NULL)
    g_object_ref (file);
  g_clear_object (&self->file);
  self->file = file;
  window_update_title (self);
  o42_book_changed (self->book, "file");
}

/* Another window on our book changed it. */
static void
on_book_changed (O42Book *book, const char *what, gpointer data)
{
  O42Window *self = data;

  (void) book;

  if (self->telling)
    return;

  if (g_strcmp0 (what, "sheets") == 0 || o42_book_sheet_index (self->book, self->sheet) < 0)
    {
      if (o42_book_sheet_index (self->book, self->sheet) < 0)
        self->sheet = o42_book_sheet (self->book, 0);
      o42_grid_set_sheet (self->grid, self->sheet);
      window_rebuild_tabs (self);
    }
  else if (g_strcmp0 (what, "file") == 0)
    {
      /* Whoever saved or opened knows the file; find them. */
      GList *windows = gtk_application_get_windows (gtk_window_get_application (GTK_WINDOW (self)));
      for (GList *l = windows; l != NULL; l = l->next)
        {
          O42Window *other = O42_IS_WINDOW (l->data) ? l->data : NULL;
          if (other != NULL && other != self && other->book == self->book && other->file != NULL)
            {
              if (self->file != other->file)
                {
                  g_clear_object (&self->file);
                  self->file = g_object_ref (other->file);
                }
              break;
            }
        }
    }

  o42_grid_refresh (self->grid);
  window_sync (self);
}

/* Our own change: repaint, and tell the other windows on the book. */
void
o42_window_tell_book (O42Window *self, const char *what)
{
  self->telling = TRUE;
  o42_book_changed (self->book, what);
  self->telling = FALSE;
}

static gboolean
file_is_csv (GFile *file)
{
  char *name = g_file_get_basename (file);
  gboolean csv = name != NULL && g_str_has_suffix (name, ".csv");
  g_free (name);
  return csv;
}

/* The three text formats: DIF and SYLK hold one sheet each and LaTeX
 * is written and never read. */
static gboolean
file_is_dif (GFile *file)
{
  char *name = g_file_get_basename (file);
  gboolean dif = name != NULL && g_str_has_suffix (name, ".dif");
  g_free (name);
  return dif;
}

static gboolean
file_is_sylk (GFile *file)
{
  char *name = g_file_get_basename (file);
  gboolean sylk = name != NULL && (g_str_has_suffix (name, ".slk") ||
                                   g_str_has_suffix (name, ".sylk"));
  g_free (name);
  return sylk;
}

static gboolean
file_is_lotus (GFile *file)
{
  char *name = g_file_get_basename (file);
  gboolean wk1 = name != NULL && (g_str_has_suffix (name, ".wk1") ||
                                  g_str_has_suffix (name, ".wks"));
  g_free (name);
  return wk1;
}

static gboolean
file_is_latex (GFile *file)
{
  char *name = g_file_get_basename (file);
  gboolean latex = name != NULL && g_str_has_suffix (name, ".tex");
  g_free (name);
  return latex;
}

static gboolean
file_is_xlsx (GFile *file)
{
  char *name = g_file_get_basename (file);
  gboolean xlsx = name != NULL && g_str_has_suffix (name, ".xlsx");
  g_free (name);
  return xlsx;
}

static gboolean
file_is_html (GFile *file)
{
  char *name = g_file_get_basename (file);
  gboolean html = name != NULL && (g_str_has_suffix (name, ".html") || g_str_has_suffix (name, ".htm"));
  g_free (name);
  return html;
}

static gboolean
file_is_ods (GFile *file)
{
  char *name = g_file_get_basename (file);
  gboolean ods = name != NULL && g_str_has_suffix (name, ".ods");
  g_free (name);
  return ods;
}

static gboolean
file_is_xls (GFile *file)
{
  char *name = g_file_get_basename (file);
  gboolean xls = name != NULL && g_str_has_suffix (name, ".xls");
  g_free (name);
  return xls;
}

gboolean
o42_window_open_file (O42Window *self, GFile *file)
{
  GError *error = NULL;
  gboolean ok;

  g_return_val_if_fail (O42_IS_WINDOW (self), FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (file_is_csv (file) || file_is_html (file) ||
      file_is_dif (file) || file_is_sylk (file) || file_is_lotus (file))
    {
      ok = file_is_csv (file)   ? o42_csv_load (self->sheet, file, &error)
         : file_is_dif (file)   ? o42_dif_load (self->sheet, file, &error)
         : file_is_sylk (file)  ? o42_sylk_load (self->sheet, file, &error)
         : file_is_lotus (file) ? o42_lotus_load (self->sheet, file, &error)
                                : o42_html_load (self->sheet, file, &error);
      o42_sheet_clear_undo (self->sheet);
      o42_sheet_set_modified (self->sheet, FALSE);
    }
  else
    {
      ok = file_is_xlsx (file) ? o42_xlsx_load (self->book, file, &error)
         : file_is_xls (file)  ? o42_xls_load (self->book, file, &error)
         : file_is_ods (file)  ? o42_ods_load (self->book, file, &error)
                               : o42_gnumeric_load (self->book, file, &error);
      self->sheet = o42_book_sheet (self->book, 0);
    }

  if (!ok)
    {
      show_error (self, "office42 could not open that file.", error);
      g_clear_error (&error);
    }
  else
    window_set_file (self, file);

  o42_grid_set_sheet (self->grid, self->sheet);
  window_rebuild_tabs (self);
  window_sync (self);
  window_tell_book (self, "sheets");
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->scripts_bar),
                                 ok && o42_book_n_scripts (self->book) > 0 && o42_python_available ());
  return ok;
}

gboolean
o42_window_is_blank (O42Window *self)
{
  O42Range used;

  g_return_val_if_fail (O42_IS_WINDOW (self), FALSE);

  o42_sheet_used_range (self->sheet, &used);
  return self->file == NULL && !o42_book_is_modified (self->book) &&
         o42_book_n_sheets (self->book) == 1 &&
         used.row1 == 0 && used.col1 == 0 &&
         o42_sheet_is_empty (self->sheet, 0, 0) &&
         o42_sheet_pictures (self->sheet)->len == 0;
}

static gboolean
window_save_to (O42Window *self, GFile *file)
{
  GError *error = NULL;
  gboolean ok;

  if (file_is_csv (file))
    ok = o42_csv_save (self->sheet, file, &error);
  else if (file_is_dif (file))
    ok = o42_dif_save (self->sheet, file, &error);
  else if (file_is_sylk (file))
    ok = o42_sylk_save (self->sheet, file, &error);
  else if (file_is_latex (file))
    ok = o42_latex_save (self->sheet, file, &error);
  else if (file_is_lotus (file))
    ok = o42_lotus_save (self->sheet, file, &error);
  else if (file_is_xlsx (file))
    ok = o42_xlsx_save (self->book, file, &error);
  else if (file_is_xls (file))
    ok = o42_xls_save (self->book, file, &error);
  else if (file_is_ods (file))
    ok = o42_ods_save (self->book, file, &error);
  else if (file_is_html (file))
    ok = o42_html_save (self->book, file, &error);
  else
    ok = o42_gnumeric_save (self->book, file, &error);

  if (!ok)
    {
      show_error (self, "office42 could not save the file.", error);
      g_clear_error (&error);
      self->close_after_save = FALSE;
      return FALSE;
    }

  /* Excel 97's grid is 65,536 rows by 256 columns and office42's is
   * Excel 2007's, so a .xls may not be able to hold everything.  It is
   * saved either way, and this says what did not go in. */
  if (file_is_xls (file) && o42_xls_dropped_cells > 0)
    {
      char *said = g_strdup_printf ("%d cells lie outside the 65,536 rows by 256 "
                                    "columns an .xls file can hold, and were not "
                                    "written. Save as .xlsx or .gnumeric to keep them.",
                                    o42_xls_dropped_cells);

      show_error (self, said, NULL);
      g_free (said);
    }

  if (file_is_csv (file) || file_is_dif (file) || file_is_sylk (file) ||
      file_is_lotus (file))
    o42_sheet_set_modified (self->sheet, FALSE);
  else
    o42_book_set_modified (self->book, FALSE);
  window_set_file (self, file);

  if (self->close_after_save)
    {
      self->close_after_save = FALSE;
      gtk_window_close (GTK_WINDOW (self));
    }

  return TRUE;
}

static GListModel *
book_filters (void)
{
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  g_list_store_append (filters, pattern_filter ("Gnumeric Spreadsheets (*.gnumeric)", "*.gnumeric"));
  g_list_store_append (filters, pattern_filter ("Excel Workbooks (*.xlsx)", "*.xlsx"));
  g_list_store_append (filters, pattern_filter ("Excel 97-2003 Workbooks (*.xls)", "*.xls"));
  g_list_store_append (filters, pattern_filter ("OpenDocument Spreadsheets (*.ods)", "*.ods"));
  g_list_store_append (filters, pattern_filter ("Web Pages (*.html)", "*.html"));
  g_list_store_append (filters, pattern_filter ("Comma-Separated Values (*.csv)", "*.csv"));
  g_list_store_append (filters, pattern_filter ("Data Interchange Format (*.dif)", "*.dif"));
  g_list_store_append (filters, pattern_filter ("SYLK (*.slk)", "*.slk"));
  g_list_store_append (filters, pattern_filter ("LaTeX Tables (*.tex)", "*.tex"));
  g_list_store_append (filters, pattern_filter ("Lotus 1-2-3 Worksheets (*.wk1)", "*.wk1"));
  g_list_store_append (filters, pattern_filter ("All Files", "*"));

  return G_LIST_MODEL (filters);
}

static void
on_save_as_response (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      window_save_to (self, file);
      g_object_unref (file);
    }
  else
    {
      self->close_after_save = FALSE;
      if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
        show_error (self, "office42 could not save the file.", error);
    }

  g_clear_error (&error);
}

static void
action_save_as (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListModel *filters = book_filters ();

  (void) a; (void) p;

  gtk_file_dialog_set_title (dialog, _("Save As"));
  gtk_file_dialog_set_filters (dialog, filters);

  if (self->file != NULL)
    gtk_file_dialog_set_initial_file (dialog, self->file);
  else
    gtk_file_dialog_set_initial_name (dialog, "Book1.gnumeric");

  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL, on_save_as_response, self);

  g_object_unref (filters);
  g_object_unref (dialog);
}

static void
action_save (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  if (self->file == NULL)
    {
      action_save_as (a, p, data);
      return;
    }

  window_save_to (self, self->file);
}

static void
on_open_response (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      O42Window *target = self;

      /* A book with work in it stays; the file opens beside it. */
      if (!o42_window_is_blank (self))
        {
          target = O42_WINDOW (o42_window_new (gtk_window_get_application (GTK_WINDOW (self))));
          gtk_window_present (GTK_WINDOW (target));
        }

      o42_window_open_file (target, file);
      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, "office42 could not open that file.", error);

  g_clear_error (&error);
}

static void
action_open (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListModel *filters = book_filters ();

  (void) a; (void) p;

  gtk_file_dialog_set_title (dialog, _("Open"));
  gtk_file_dialog_set_filters (dialog, filters);
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_open_response, self);

  g_object_unref (filters);
  g_object_unref (dialog);
}

/* Closing a modified book asks first.  Save saves and then closes; the
 * close is deferred through close_after_save because saving may itself
 * need a dialog. */
static void
on_close_choice (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Window *self = data;
  int choice = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), result, NULL);

  switch (choice)
    {
    case 0:         /* Save */
      self->close_after_save = TRUE;
      action_save (NULL, NULL, self);
      break;

    case 1:         /* Don't Save */
      o42_book_set_modified (self->book, FALSE);
      gtk_window_close (GTK_WINDOW (self));
      break;

    default:        /* Cancel, or the dialog went away */
      break;
    }
}

static gboolean
o42_window_close_request (GtkWindow *window)
{
  O42Window *self = O42_WINDOW (window);
  GtkAlertDialog *dialog;
  const char *buttons[] = { "_Save", "Do_n't Save", "_Cancel", NULL };
  char *name, *message;

  if (o42_grid_is_editing (self->grid))
    o42_grid_commit_edit (self->grid);

  if (!o42_book_is_modified (self->book))
    return GDK_EVENT_PROPAGATE;      /* close */

  /* Another window still shows the book, so nothing is lost by closing
   * this one. */
  {
    GList *windows = gtk_application_get_windows (gtk_window_get_application (window));
    for (GList *l = windows; l != NULL; l = l->next)
      {
        O42Window *other = O42_IS_WINDOW (l->data) ? l->data : NULL;
        if (other != NULL && other != self && other->book == self->book)
          return GDK_EVENT_PROPAGATE;
      }
  }

  name = (self->file != NULL) ? g_file_get_basename (self->file) : g_strdup ("Book1");
  message = g_strdup_printf ("Save changes to %s?", name);

  dialog = gtk_alert_dialog_new ("%s", message);
  gtk_alert_dialog_set_detail (dialog, _("Your changes will be lost if you don't save them."));
  gtk_alert_dialog_set_buttons (dialog, buttons);
  gtk_alert_dialog_set_default_button (dialog, 0);
  gtk_alert_dialog_set_cancel_button (dialog, 2);
  gtk_alert_dialog_choose (dialog, GTK_WINDOW (self), NULL, on_close_choice, self);

  g_object_unref (dialog);
  g_free (message);
  g_free (name);

  return GDK_EVENT_STOP;             /* keep open until the answer comes */
}

static void
action_new (GSimpleAction *a, GVariant *p, gpointer data)
{
  GtkWidget *w;
  (void) a; (void) p;
  w = o42_window_new (gtk_window_get_application (GTK_WINDOW (data)));
  gtk_window_present (GTK_WINDOW (w));
}

static void
action_close (GSimpleAction *a, GVariant *p, gpointer data)
{
  (void) a; (void) p;
  gtk_window_close (GTK_WINDOW (data));
}

static void
action_goto (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  (void) a; (void) p;
  gtk_widget_grab_focus (self->name_box);
  gtk_editable_select_region (GTK_EDITABLE (self->name_box), 0, -1);
}

/* The operating system, as a person would name it. */
static char *
about_os_name (void)
{
#ifdef G_OS_WIN32
  /* GetVersionEx lies to programs without a manifest; RtlGetVersion
   * does not. */
  typedef LONG (WINAPI *RtlGetVersionFn) (OSVERSIONINFOEXW *);
  HMODULE ntdll = GetModuleHandleW (L"ntdll.dll");
  RtlGetVersionFn rtl_get_version = ntdll ? (RtlGetVersionFn) (void *) GetProcAddress (ntdll, "RtlGetVersion") : NULL;
  OSVERSIONINFOEXW info;

  memset (&info, 0, sizeof info);
  info.dwOSVersionInfoSize = sizeof info;
  if (rtl_get_version != NULL && rtl_get_version (&info) == 0)
    {
      const char *name = info.dwMajorVersion >= 10 && info.dwBuildNumber >= 22000 ? "Windows 11"
                       : info.dwMajorVersion >= 10 ? "Windows 10" : "Windows";
      return g_strdup_printf ("%s (build %lu.%lu.%lu)", name,
                              (unsigned long) info.dwMajorVersion,
                              (unsigned long) info.dwMinorVersion,
                              (unsigned long) info.dwBuildNumber);
    }
  return g_strdup ("Windows");
#else
  char *pretty = g_get_os_info (G_OS_INFO_KEY_PRETTY_NAME);
  return pretty != NULL ? pretty : g_strdup ("Unix");
#endif
}

/* What the About dialog's System tab says: the libraries as built
 * against and as found at run time, the machine, and the display. */
static char *
about_system_information (GtkWidget *window)
{
  GString *out = g_string_new (NULL);
  char *os = about_os_name ();
  GdkDisplay *display = gtk_widget_get_display (window);
  GtkNative *native = gtk_widget_get_native (window);
  GskRenderer *renderer = native != NULL ? gtk_native_get_renderer (native) : NULL;
  const char *locale = setlocale (LC_ALL, NULL);
  const char *family;

  g_string_append_printf (out, "Office42 Spreadsheet %s\n", O42_VERSION);
  g_string_append_printf (out, "Built %s with %s\n\n", __DATE__,
#if defined(__clang__)
                          "clang " __clang_version__
#elif defined(__GNUC__)
                          "GCC " __VERSION__
#else
                          "an unknown compiler"
#endif
                          );

  g_string_append_printf (out, "GTK %u.%u.%u (built against %d.%d.%d)\n",
                          gtk_get_major_version (), gtk_get_minor_version (), gtk_get_micro_version (),
                          GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION);
  g_string_append_printf (out, "GLib %u.%u.%u (built against %d.%d.%d)\n",
                          glib_major_version, glib_minor_version, glib_micro_version,
                          GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
  g_string_append_printf (out, "Pango %s (built against %s)\n", pango_version_string (), PANGO_VERSION_STRING);
  g_string_append_printf (out, "Cairo %s (built against %s)\n", cairo_version_string (), CAIRO_VERSION_STRING);
  g_string_append_printf (out, "GdkPixbuf %s\n", gdk_pixbuf_version);
  g_string_append_printf (out, "Poppler: %s\n\n", o42_pdf_import_available () ? "present" : "not built in");

  g_string_append_printf (out, "Operating system: %s\n", os);
  g_string_append_printf (out, "Processors: %u\n", g_get_num_processors ());
  {
    /* setlocale answers in the locale's own encoding, not UTF-8. */
    char *utf8 = locale != NULL ? g_locale_to_utf8 (locale, -1, NULL, NULL, NULL) : NULL;
    g_string_append_printf (out, "Locale: %s\n", utf8 != NULL ? utf8 : "C");
    g_free (utf8);
  }
  g_string_append_printf (out, "Display: %s", display != NULL ? G_OBJECT_TYPE_NAME (display) : "none");
  if (display != NULL)
    g_string_append_printf (out, " (%s)", gdk_display_get_name (display));
  g_string_append_c (out, '\n');
  g_string_append_printf (out, "Renderer: %s\n", renderer != NULL ? G_OBJECT_TYPE_NAME (renderer) : "none");
  if (display != NULL && native != NULL && gtk_native_get_surface (native) != NULL)
    {
      GdkMonitor *monitor = gdk_display_get_monitor_at_surface (display, gtk_native_get_surface (native));
      if (monitor != NULL)
        {
          GdkRectangle geo;
          gdk_monitor_get_geometry (monitor, &geo);
          g_string_append_printf (out, "Monitor: %d\303\227%d at %d%%, %d Hz\n", geo.width, geo.height,
                                  gdk_monitor_get_scale_factor (monitor) * 100,
                                  gdk_monitor_get_refresh_rate (monitor) / 1000);
        }
    }
  family = pango_font_description_get_family (pango_context_get_font_description (gtk_widget_get_pango_context (window)));
  g_string_append_printf (out, "Interface font: %s\n", family != NULL ? family : "default");
  g_string_append_printf (out, "Settings: %s\n", g_get_user_config_dir ());
  g_free (os);
  return g_string_free (out, FALSE);
}

static void
action_about (GSimpleAction *a, GVariant *p, gpointer data)
{
  GdkTexture *logo = gdk_texture_new_from_resource (
    "/net/office42/office42/icons/scalable/apps/net.office42.office42.svg");
  char *system = about_system_information (GTK_WIDGET (data));
  const char *authors[] = { "Andreas R\303\270sdal", "Claude (Anthropic)", NULL };

  (void) a; (void) p;

  gtk_show_about_dialog (GTK_WINDOW (data),
                         "program-name", "Office42 Spreadsheet",
                         "version", O42_VERSION,
                         "logo", logo,
                         "comments", "A spreadsheet in the shape of Excel 5 and Gnumeric, "
                                     "written in C on GTK 4, Pango and Cairo.",
                         "website", "https://github.com/office-42/office42",
                         "website-label", "github.com/office-42/office42",
                         "copyright", "Copyright \302\251 2026 The office42 authors",
                         "license-type", GTK_LICENSE_GPL_3_0,
                         "authors", authors,
                         "system-information", system,
                         NULL);
  g_free (system);
  g_object_unref (logo);
}

static const GActionEntry ACTIONS[] = {
  { "new",        action_new,        NULL, NULL, NULL, { 0 } },
  { "open",       action_open,       NULL, NULL, NULL, { 0 } },
  { "save",       action_save,       NULL, NULL, NULL, { 0 } },
  { "save-as",    action_save_as,    NULL, NULL, NULL, { 0 } },
  { "close",      action_close,      NULL, NULL, NULL, { 0 } },
  { "undo",       action_undo,       NULL, NULL, NULL, { 0 } },
  { "redo",       action_redo,       NULL, NULL, NULL, { 0 } },
  { "cut",        action_cut,        NULL, NULL, NULL, { 0 } },
  { "copy",       action_copy,       NULL, NULL, NULL, { 0 } },
  { "paste",      action_paste,      NULL, NULL, NULL, { 0 } },
  { "clear",      action_clear,      NULL, NULL, NULL, { 0 } },
  { "select-all", action_select_all, NULL, NULL, NULL, { 0 } },
  { "fill-down",  action_fill_down,  NULL, NULL, NULL, { 0 } },
  { "fill-right", action_fill_right, NULL, NULL, NULL, { 0 } },
  { "insert-rows",    action_insert_rows,    NULL, NULL, NULL, { 0 } },
  { "insert-columns", action_insert_columns, NULL, NULL, NULL, { 0 } },
  { "delete-rows",    action_delete_rows,    NULL, NULL, NULL, { 0 } },
  { "delete-columns", action_delete_columns, NULL, NULL, NULL, { 0 } },
  { "bold",       action_bold,       NULL, NULL, NULL, { 0 } },
  { "italic",     action_italic,     NULL, NULL, NULL, { 0 } },
  { "underline",  action_underline,  NULL, NULL, NULL, { 0 } },
  { "align",      action_align,      "s",  NULL, NULL, { 0 } },
  { "number",     action_number,     "s",  NULL, NULL, { 0 } },
  { "decimals",   action_decimals,   "i",  NULL, NULL, { 0 } },
  { "autosum",    action_autosum,    NULL, NULL, NULL, { 0 } },
  { "goto",       action_goto,       NULL, NULL, NULL, { 0 } },
  { "insert-picture", action_insert_picture, NULL, NULL, NULL, { 0 } },
  { "column-width",   action_column_width,   NULL, NULL, NULL, { 0 } },
  { "row-height",     action_row_height,     NULL, NULL, NULL, { 0 } },
  { "autofit",        action_autofit,        NULL, NULL, NULL, { 0 } },
  { "hide-rows",      action_hide_rows,      NULL, NULL, NULL, { 0 } },
  { "merge-cells",    action_merge_cells,    NULL, NULL, NULL, { 0 } },
  { "unmerge-cells",  action_unmerge_cells,  NULL, NULL, NULL, { 0 } },
  { "unhide-rows",    action_unhide_rows,    NULL, NULL, NULL, { 0 } },
  { "hide-columns",   action_hide_columns,   NULL, NULL, NULL, { 0 } },
  { "unhide-columns", action_unhide_columns, NULL, NULL, NULL, { 0 } },
  { "filter",         action_filter,         NULL, NULL, NULL, { 0 } },
  { "sort",           action_sort,           NULL, NULL, NULL, { 0 } },
  { "subtotals",      action_subtotals,      NULL, NULL, NULL, { 0 } },
  { "table",          action_table,          NULL, NULL, NULL, { 0 } },
  { "scenarios",      action_scenarios,      NULL, NULL, NULL, { 0 } },
  { "consolidate",    action_consolidate,    NULL, NULL, NULL, { 0 } },
  { "advanced-filter", action_advanced_filter, NULL, NULL, NULL, { 0 } },
  { "remove-duplicates", action_remove_duplicates, NULL, NULL, NULL, { 0 } },
  { "format-cells",   action_format_cells,   NULL, NULL, NULL, { 0 } },
  { "insert-function", action_insert_function, NULL, NULL, NULL, { 0 } },
  { "insert-sheet",   action_insert_sheet,   NULL, NULL, NULL, { 0 } },
  { "calculate",      action_calculate,      NULL, NULL, NULL, { 0 } },
  { "full-screen",    action_full_screen,    NULL, NULL, NULL, { 0 } },
  { "whatif",         action_whatif,         NULL, NULL, NULL, { 0 } },
  { "autoformat",     action_autoformat,     NULL, NULL, NULL, { 0 } },
  { "format-painter", action_format_painter, NULL, NULL, NULL, { 0 } },
  { "move-sheet-left",  action_move_sheet_left,  NULL, NULL, NULL, { 0 } },
  { "tab-colour",       action_tab_colour,       NULL, NULL, NULL, { 0 } },
  { "custom-lists",     action_custom_lists,     NULL, NULL, NULL, { 0 } },
  { "trace-precedents", action_trace_precedents, NULL, NULL, NULL, { 0 } },
  { "trace-dependents", action_trace_dependents, NULL, NULL, NULL, { 0 } },
  { "clear-arrows",     action_clear_arrows,     NULL, NULL, NULL, { 0 } },
  { "tab-colour-none",  action_tab_colour_none,  NULL, NULL, NULL, { 0 } },
  { "move-sheet-right", action_move_sheet_right, NULL, NULL, NULL, { 0 } },
  { "insert-chart",   action_insert_chart,   NULL, NULL, NULL, { 0 } },
  { "insert-cells",   action_insert_cells,   NULL, NULL, NULL, { 0 } },
  { "delete-cells",   action_delete_cells,   NULL, NULL, NULL, { 0 } },
  { "paste-special",  action_paste_special,  NULL, NULL, NULL, { 0 } },
  { "delete-sheet",   action_delete_sheet,   NULL, NULL, NULL, { 0 } },
  { "rename-sheet",   action_rename_sheet,   NULL, NULL, NULL, { 0 } },
  { "next-sheet",     action_next_sheet,     NULL, NULL, NULL, { 0 } },
  { "prev-sheet",     action_prev_sheet,     NULL, NULL, NULL, { 0 } },
  { "print",          action_print,          NULL, NULL, NULL, { 0 } },
  { "print-book",     action_print_book,     NULL, NULL, NULL, { 0 } },
  { "export-book-pdf", action_export_book_pdf, NULL, NULL, NULL, { 0 } },
  { "print-preview",  action_print_preview,  NULL, NULL, NULL, { 0 } },
  { "options",        action_options,        NULL, NULL, NULL, { 0 } },
  { "zoom",           action_zoom,           "i",  NULL, NULL, { 0 } },
  { "freeze-panes",   action_freeze_panes,   NULL, NULL, NULL, { 0 } },
  { "split-panes",    action_split_panes,    NULL, NULL, NULL, { 0 } },
  { "define-name",    action_define_name,    NULL, NULL, NULL, { 0 } },
  { "insert-note",    action_insert_note,    NULL, NULL, NULL, { 0 } },
  { "goal-seek",      action_goal_seek,      NULL, NULL, NULL, { 0 } },
  { "solver",         action_solver,         NULL, NULL, NULL, { 0 } },
  { "protect",        action_protect,        NULL, NULL, NULL, { 0 } },
  { "spelling",       action_spelling,       NULL, NULL, NULL, { 0 } },
  { "record-macro",   action_record_macro,   NULL, NULL, NULL, { 0 } },
  { "analysis",       action_analysis,       NULL, NULL, NULL, { 0 } },
  { "group-objects",  action_group_objects,  NULL, NULL, NULL, { 0 } },
  { "ungroup-objects", action_ungroup_objects, NULL, NULL, NULL, { 0 } },
  { "custom-views",   action_custom_views,   NULL, NULL, NULL, { 0 } },
  { "page-breaks",    action_page_breaks,    NULL, NULL, NULL, { 0 } },
  { "python-console", action_python_console, NULL, NULL, NULL, { 0 } },
  { "python-run",     action_python_run,     NULL, NULL, NULL, { 0 } },
  { "scripts",        action_scripts,        NULL, NULL, NULL, { 0 } },
  { "scripts-run-all", action_scripts_run_all, NULL, NULL, NULL, { 0 } },
  { "text-to-columns", action_text_to_columns, NULL, NULL, NULL, { 0 } },
  { "conditional",    action_conditional,    NULL, NULL, NULL, { 0 } },
  { "validation",     action_validation,     NULL, NULL, NULL, { 0 } },
  { "pivot",          action_pivot,          NULL, NULL, NULL, { 0 } },
  { "refresh-pivot",  action_refresh_pivot,  NULL, NULL, NULL, { 0 } },
  { "group-rows",     action_group_rows,     NULL, NULL, NULL, { 0 } },
  { "group-cols",     action_group_cols,     NULL, NULL, NULL, { 0 } },
  { "ungroup-rows",   action_ungroup_rows,   NULL, NULL, NULL, { 0 } },
  { "ungroup-cols",   action_ungroup_cols,   NULL, NULL, NULL, { 0 } },
  { "new-window",     action_new_window,     NULL, NULL, NULL, { 0 } },
  { "help-contents",  action_help_contents,  NULL, NULL, NULL, { 0 } },
  { "page-setup",     action_page_setup,     NULL, NULL, NULL, { 0 } },
  { "insert-link",    action_insert_link,    NULL, NULL, NULL, { 0 } },
  { "format-chart",   action_format_chart,   NULL, NULL, NULL, { 0 } },
  { "shape",          action_shape,          "s",  NULL, NULL, { 0 } },
  { "format-shape",   action_format_shape,   NULL, NULL, NULL, { 0 } },
  { "format-control", action_format_control, NULL, NULL, NULL, { 0 } },
  { "db-connect",     action_db_connect,     NULL, NULL, NULL, { 0 } },
  { "db-embed",       action_db_embed,       NULL, NULL, NULL, { 0 } },
  { "db-query",       action_db_query,       NULL, NULL, NULL, { 0 } },
  { "db-refresh",     action_db_refresh,     NULL, NULL, NULL, { 0 } },
  { "db-send",        action_db_send,        NULL, NULL, NULL, { 0 } },
  { "style",          action_style,          NULL, NULL, NULL, { 0 } },
  { "sheet-setup",    action_sheet_setup,    NULL, NULL, NULL, { 0 } },
  { "set-print-area", action_set_print_area, NULL, NULL, NULL, { 0 } },
  { "page-break",     action_page_break,     NULL, NULL, NULL, { 0 } },
  { "clear-print-area", action_clear_print_area, NULL, NULL, NULL, { 0 } },
  { "find",           action_find,           NULL, NULL, NULL, { 0 } },
  { "replace",        action_replace,        NULL, NULL, NULL, { 0 } },
  { "export-pdf", action_export_pdf, NULL, NULL, NULL, { 0 } },
  { "import-pdf", action_import_pdf, NULL, NULL, NULL, { 0 } },
  { "about",      action_about,      NULL, NULL, NULL, { 0 } },
};

/* Named in the menus, greyed out: what office42 intends and has not built.
 * Showing them is more honest than hiding them. */
static const char *PLANNED[] = {
  NULL,
};

static void
action_planned (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void) a; (void) p; (void) d;
}

/* ---------------------------------------------------------------------- */
/* Toolbars                                                                */
/* ---------------------------------------------------------------------- */

static GtkWidget *
text_button (const char *label, const char *tip, const char *action,
             const char *css_class)
{
  GtkWidget *button = gtk_button_new_with_label (label);

  gtk_widget_set_tooltip_text (button, tip);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (button), action);
  gtk_widget_set_focusable (button, FALSE);
  gtk_widget_add_css_class (button, "o42-tool");
  if (css_class != NULL)
    gtk_widget_add_css_class (button, css_class);

  return button;
}

static GtkWidget *
target_button (const char *label, const char *tip, const char *action,
               const char *target, const char *css_class)
{
  GtkWidget *button = text_button (label, tip, NULL, css_class);

  gtk_actionable_set_detailed_action_name (GTK_ACTIONABLE (button),
    g_strdup_printf ("%s::%s", action, target));

  return button;
}

/* An icon button.  The picture carries the meaning, so the tooltip has to
 * carry the name -- and say it again to the accessibility tree, which has no
 * label to read off a button that holds only an image. */
static GtkWidget *
icon_button (const char *icon_name, const char *tip, const char *action)
{
  GtkWidget *button = gtk_button_new ();
  GtkWidget *image = gtk_image_new_from_icon_name (icon_name);

  gtk_image_set_pixel_size (GTK_IMAGE (image), 16);
  gtk_button_set_child (GTK_BUTTON (button), image);
  gtk_widget_set_tooltip_text (button, tip);
  gtk_widget_set_focusable (button, FALSE);
  gtk_widget_add_css_class (button, "o42-tool");
  gtk_accessible_update_property (GTK_ACCESSIBLE (button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, tip, -1);
  if (action != NULL)
    gtk_actionable_set_action_name (GTK_ACTIONABLE (button), action);

  return button;
}

static GtkWidget *
icon_target_button (const char *icon_name, const char *tip, const char *action,
                    const char *target)
{
  GtkWidget *button = icon_button (icon_name, tip, NULL);

  gtk_actionable_set_detailed_action_name (GTK_ACTIONABLE (button),
    g_strdup_printf ("%s::%s", action, target));

  return button;
}

static GtkWidget *
icon_int_target_button (const char *icon_name, const char *tip,
                        const char *action, int v)
{
  GtkWidget *button = icon_button (icon_name, tip, action);

  gtk_actionable_set_action_target (GTK_ACTIONABLE (button), "i", v);

  return button;
}

static GtkWidget *
tool_separator (void)
{
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_VERTICAL);

  gtk_widget_set_margin_start (sep, 3);
  gtk_widget_set_margin_end (sep, 3);
  gtk_widget_set_margin_top (sep, 2);
  gtk_widget_set_margin_bottom (sep, 2);

  return sep;
}

static GtkWidget *
build_standard_bar (void)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 1);

  gtk_widget_add_css_class (bar, "o42-toolbar");

  gtk_box_append (GTK_BOX (bar), icon_button ("o42-new",   "New",   "win.new"));
  gtk_box_append (GTK_BOX (bar), icon_button ("o42-open",  "Open",  "win.open"));
  gtk_box_append (GTK_BOX (bar), icon_button ("o42-save",  "Save",  "win.save"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  gtk_box_append (GTK_BOX (bar), icon_button ("o42-print", "Print", "win.print"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  gtk_box_append (GTK_BOX (bar), icon_button ("o42-cut",   "Cut",   "win.cut"));
  gtk_box_append (GTK_BOX (bar), icon_button ("o42-copy",  "Copy",  "win.copy"));
  gtk_box_append (GTK_BOX (bar), icon_button ("o42-paste", "Paste", "win.paste"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  gtk_box_append (GTK_BOX (bar), icon_button ("o42-undo",  "Undo",  "win.undo"));
  gtk_box_append (GTK_BOX (bar), icon_button ("o42-redo",  "Redo",  "win.redo"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  /* The sigma and the fx are letters, not pictures; Excel 5 drew them as
   * letters too.  They stay set in type. */
  gtk_box_append (GTK_BOX (bar), text_button ("\316\243", _("AutoSum"), "win.autosum", "o42-glyph"));
  gtk_box_append (GTK_BOX (bar), text_button ("fx", _("Function Wizard"), "win.insert-function", "o42-glyph-italic"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  gtk_box_append (GTK_BOX (bar), icon_button ("o42-sort",  "Sort",  "win.sort"));
  gtk_box_append (GTK_BOX (bar), icon_button ("o42-chart", "Chart Wizard", "win.insert-chart"));

  return bar;
}

static void
on_font_selected (GtkDropDown *drop, GParamSpec *pspec, gpointer data)
{
  O42Window *self = data;
  GtkStringObject *item;
  O42Fmt want;

  (void) pspec;
  if (self->updating) return;

  item = gtk_drop_down_get_selected_item (drop);
  if (item == NULL) return;

  o42_fmt_init_default (&want);
  want.family = g_intern_string (gtk_string_object_get_string (item));
  o42_grid_apply_fmt (self->grid, O42_FMT_FAMILY, &want);
  gtk_widget_grab_focus (GTK_WIDGET (self->grid));
}

static void
on_size_selected (GtkDropDown *drop, GParamSpec *pspec, gpointer data)
{
  O42Window *self = data;
  guint index;
  O42Fmt want;

  (void) pspec;
  if (self->updating) return;

  index = gtk_drop_down_get_selected (drop);
  if (index == GTK_INVALID_LIST_POSITION || index >= G_N_ELEMENTS (FONT_SIZES))
    return;

  o42_fmt_init_default (&want);
  want.size = FONT_SIZES[index] * 2;
  o42_grid_apply_fmt (self->grid, O42_FMT_SIZE, &want);
  gtk_widget_grab_focus (GTK_WIDGET (self->grid));
}

static GListModel *
list_font_families (void)
{
  PangoFontMap *map = pango_cairo_font_map_get_default ();
  PangoFontFamily **families = NULL;
  int n = 0;
  GtkStringList *list = gtk_string_list_new (NULL);
  GPtrArray *names = g_ptr_array_new ();

  pango_font_map_list_families (map, &families, &n);
  for (int i = 0; i < n; i++)
    g_ptr_array_add (names, (gpointer) pango_font_family_get_name (families[i]));
  g_ptr_array_sort_values (names, (GCompareFunc) g_ascii_strcasecmp);
  for (guint i = 0; i < names->len; i++)
    gtk_string_list_append (list, g_ptr_array_index (names, i));

  g_ptr_array_free (names, TRUE);
  g_free (families);
  return G_LIST_MODEL (list);
}

static GtkWidget *
build_format_bar (O42Window *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 1);
  GtkStringList *sizes = gtk_string_list_new (NULL);
  static const char *align_icons[3] = { "o42-align-left",
                                        "o42-align-center",
                                        "o42-align-right" };
  static const char *align_targets[3] = { "left", "center", "right" };
  static const char *align_tips[3] = { "Align Left", "Center", "Align Right" };

  gtk_widget_add_css_class (bar, "o42-toolbar");

  self->families = list_font_families ();
  self->family_index = g_hash_table_new (g_str_hash, g_str_equal);
  {
    guint n = g_list_model_get_n_items (self->families);
    for (guint i = 0; i < n; i++)
      {
        GtkStringObject *item = g_list_model_get_item (self->families, i);
        g_hash_table_insert (self->family_index,
                             (gpointer) g_intern_string (gtk_string_object_get_string (item)),
                             GUINT_TO_POINTER (i + 1));
        g_object_unref (item);
      }
  }

  self->font_drop = gtk_drop_down_new (g_object_ref (self->families), NULL);
  gtk_drop_down_set_enable_search (GTK_DROP_DOWN (self->font_drop), TRUE);
  gtk_widget_set_size_request (self->font_drop, 150, -1);
  g_signal_connect (self->font_drop, "notify::selected-item",
                    G_CALLBACK (on_font_selected), self);
  gtk_box_append (GTK_BOX (bar), self->font_drop);

  for (guint i = 0; i < G_N_ELEMENTS (FONT_SIZES); i++)
    {
      char label[8];
      g_snprintf (label, sizeof label, "%d", FONT_SIZES[i]);
      gtk_string_list_append (sizes, label);
    }
  self->size_drop = gtk_drop_down_new (G_LIST_MODEL (sizes), NULL);
  gtk_widget_set_size_request (self->size_drop, 60, -1);
  g_signal_connect (self->size_drop, "notify::selected",
                    G_CALLBACK (on_size_selected), self);
  gtk_box_append (GTK_BOX (bar), self->size_drop);

  gtk_box_append (GTK_BOX (bar), tool_separator ());

  self->bold_btn      = text_button ("B", _("Bold"),      "win.bold",      "o42-glyph-bold");
  self->italic_btn    = text_button ("I", _("Italic"),    "win.italic",    "o42-glyph-italic");
  self->underline_btn = text_button ("U", _("Underline"), "win.underline", "o42-glyph-underline");
  gtk_box_append (GTK_BOX (bar), self->bold_btn);
  gtk_box_append (GTK_BOX (bar), self->italic_btn);
  gtk_box_append (GTK_BOX (bar), self->underline_btn);

  gtk_box_append (GTK_BOX (bar), tool_separator ());

  for (int i = 0; i < 3; i++)
    {
      self->align_btn[i] = icon_target_button (align_icons[i], align_tips[i],
                                              "win.align", align_targets[i]);
      gtk_box_append (GTK_BOX (bar), self->align_btn[i]);
    }

  gtk_box_append (GTK_BOX (bar), tool_separator ());

  gtk_box_append (GTK_BOX (bar), target_button ("$", "Currency Style", "win.number", "currency", "o42-glyph"));
  gtk_box_append (GTK_BOX (bar), target_button ("%", "Percent Style",  "win.number", "percent",  "o42-glyph"));
  gtk_box_append (GTK_BOX (bar), target_button (",", "Comma Style",    "win.number", "comma",    "o42-glyph"));
  gtk_box_append (GTK_BOX (bar), icon_int_target_button ("o42-increase-decimal",
                                                        "Increase Decimal", "win.decimals", 1));
  gtk_box_append (GTK_BOX (bar), icon_int_target_button ("o42-decrease-decimal",
                                                        "Decrease Decimal", "win.decimals", -1));

  return bar;
}

/* ---------------------------------------------------------------------- */
/* The formula bar and name box                                            */
/* ---------------------------------------------------------------------- */

/* The name box: a reference or a range jumps there; a defined name
 * selects what it names, on whichever sheet; and a new word with a
 * selection defines that word as its name, as Excel's name box does. */
static void
on_name_box_activate (GtkEntry *entry, gpointer data)
{
  O42Window *self = data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  int row, col;
  gsize len = 0;
  O42Sheet *target = NULL;
  O42Range range;

  if (o42_ref_parse (text, &row, &col, &len))
    {
      if (text[len] == ':' && o42_ref_parse (text + len + 1, &range.row1, &range.col1, NULL))
        {
          range.row0 = row; range.col0 = col;
          range = o42_range_normalise (range.row0, range.col0, range.row1, range.col1);
          o42_grid_set_active (self->grid, range.row0, range.col0);
          o42_grid_select_range (self->grid, &range);
        }
      else if (text[len] == '\0')
        o42_grid_set_active (self->grid, row, col);
    }
  else if (o42_book_lookup_name (self->book, text, &target, &range))
    {
      if (target != self->sheet)
        window_show_sheet (self, o42_book_sheet_index (self->book, target));
      o42_grid_set_active (self->grid, range.row0, range.col0);
      o42_grid_select_range (self->grid, &range);
    }
  else
    {
      o42_grid_get_selection (self->grid, &range);
      if (!o42_book_define_name (self->book, text, self->sheet, &range))
        show_error (self, "That cannot be used as a name.", NULL);
      o42_grid_refresh (self->grid);
    }

  gtk_widget_grab_focus (GTK_WIDGET (self->grid));
  window_sync (self);
}

static void
on_formula_activate (GtkEntry *entry, gpointer data)
{
  O42Window *self = data;
  int row, col;

  o42_grid_set_active_input (self->grid,
                             gtk_editable_get_text (GTK_EDITABLE (entry)));

  /* Enter in the formula bar commits and steps down, as in the grid. */
  o42_grid_get_active (self->grid, &row, &col);
  o42_grid_set_active (self->grid, row + 1, col);
  gtk_widget_grab_focus (GTK_WIDGET (self->grid));
}

static GtkWidget *
build_formula_bar (O42Window *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *cancel, *enter;

  gtk_widget_add_css_class (bar, "o42-formulabar");

  self->name_box = gtk_entry_new ();
  gtk_widget_add_css_class (self->name_box, "o42-namebox");
  gtk_widget_set_size_request (self->name_box, 90, -1);
  gtk_widget_set_tooltip_text (self->name_box, _("Name Box"));
  g_signal_connect (self->name_box, "activate",
                    G_CALLBACK (on_name_box_activate), self);
  gtk_box_append (GTK_BOX (bar), self->name_box);

  cancel = text_button ("\303\227", _("Cancel"), NULL, "o42-glyph");
  enter  = text_button ("\342\234\223", _("Enter"), NULL, "o42-glyph");
  g_signal_connect_swapped (cancel, "clicked",
                            G_CALLBACK (window_sync), self);
  g_signal_connect_swapped (enter, "clicked",
                            G_CALLBACK (gtk_widget_activate), NULL);
  gtk_box_append (GTK_BOX (bar), cancel);
  gtk_box_append (GTK_BOX (bar), enter);

  self->formula_entry = gtk_entry_new ();
  gtk_widget_add_css_class (self->formula_entry, "o42-formula-entry");
  gtk_widget_set_hexpand (self->formula_entry, TRUE);
  g_signal_connect (self->formula_entry, "activate",
                    G_CALLBACK (on_formula_activate), self);
  gtk_box_append (GTK_BOX (bar), self->formula_entry);

  return bar;
}

/* ---------------------------------------------------------------------- */
/* Sheet tabs and status bar                                               */
/* ---------------------------------------------------------------------- */

static GtkWidget *
build_tabs (O42Window *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (bar, "o42-tabs");
  self->tabs = bar;
  window_rebuild_tabs (self);

  return bar;
}

static GtkWidget *
build_status_bar (O42Window *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);

  gtk_widget_add_css_class (bar, "o42-statusbar");

  self->status_label = gtk_label_new (_("Ready"));
  gtk_widget_add_css_class (self->status_label, "o42-status-cell");
  gtk_label_set_xalign (GTK_LABEL (self->status_label), 0.0);
  gtk_widget_set_hexpand (self->status_label, TRUE);
  gtk_box_append (GTK_BOX (bar), self->status_label);

  self->status_sum = gtk_label_new ("");
  gtk_widget_add_css_class (self->status_sum, "o42-status-cell");
  gtk_widget_set_size_request (self->status_sum, 160, -1);
  gtk_label_set_xalign (GTK_LABEL (self->status_sum), 0.0);
  gtk_box_append (GTK_BOX (bar), self->status_sum);

  return bar;
}

/* ---------------------------------------------------------------------- */
/* Keeping the chrome in step                                              */
/* ---------------------------------------------------------------------- */

void
o42_window_sync (O42Window *self)
{
  int row, col;
  O42Range sel;
  char *name, *input;
  const O42Fmt *fmt;

  self->updating = TRUE;
  if (self->scripts_bar != NULL && o42_book_n_scripts (self->book) == 0)
    gtk_revealer_set_reveal_child (GTK_REVEALER (self->scripts_bar), FALSE);

  o42_grid_get_active (self->grid, &row, &col);
  o42_grid_get_selection (self->grid, &sel);

  name = o42_ref_name (row, col);
  gtk_editable_set_text (GTK_EDITABLE (self->name_box), name);
  g_free (name);

  {
    O42Range block;

    /* Any cell of an array formula's block shows the formula in braces,
     * as Excel does. */
    if (o42_sheet_array_range (self->sheet, row, col, &block) &&
        !o42_sheet_array_is_dynamic (self->sheet, row, col))
      {
        char *head = o42_sheet_get_input (self->sheet, block.row0, block.col0);
        input = g_strdup_printf ("{%s}", head);
        g_free (head);
      }
    else if (o42_sheet_formula_hidden (self->sheet, row, col))
      input = g_strdup ("");   /* a hidden formula on a protected sheet */
    else
      input = o42_sheet_get_input (self->sheet, row, col);
  }
  gtk_editable_set_text (GTK_EDITABLE (self->formula_entry), input);
  g_free (input);

  fmt = o42_grid_active_fmt (self->grid);
  if (fmt != NULL)
    {
      for (guint i = 0; i < G_N_ELEMENTS (FONT_SIZES); i++)
        if (FONT_SIZES[i] * 2 == fmt->size)
          {
            if (gtk_drop_down_get_selected (GTK_DROP_DOWN (self->size_drop)) != i)
              gtk_drop_down_set_selected (GTK_DROP_DOWN (self->size_drop), i);
            break;
          }

      if (fmt->family != NULL)
        {
          gpointer found = g_hash_table_lookup (self->family_index, fmt->family);
          if (found != NULL)
            gtk_drop_down_set_selected (GTK_DROP_DOWN (self->font_drop),
                                        GPOINTER_TO_UINT (found) - 1);
        }

      if (fmt->bold)      gtk_widget_add_css_class (self->bold_btn, "o42-down");
      else                gtk_widget_remove_css_class (self->bold_btn, "o42-down");
      if (fmt->italic)    gtk_widget_add_css_class (self->italic_btn, "o42-down");
      else                gtk_widget_remove_css_class (self->italic_btn, "o42-down");
      if (fmt->underline) gtk_widget_add_css_class (self->underline_btn, "o42-down");
      else                gtk_widget_remove_css_class (self->underline_btn, "o42-down");
    }

  /* Excel's AutoCalculate: the sum of the selection, in the status bar. */
  if (sel.row0 != sel.row1 || sel.col0 != sel.col1)
    {
      double total = 0.0;
      int count = 0;
      GArray *ranges = g_array_new (FALSE, FALSE, sizeof (O42Range));

      /* Everything selected, which with Ctrl+click may be several
       * rectangles at once. */
      o42_grid_selection_ranges (self->grid, ranges);
      for (guint i = 0; i < ranges->len; i++)
        {
          O42Range one = g_array_index (ranges, O42Range, i);

          for (int r = one.row0; r <= one.row1 && r < one.row0 + 5000; r++)
            for (int c = one.col0; c <= one.col1 && c < one.col0 + 256; c++)
              {
                O42Value v;

                if (o42_sheet_is_empty (self->sheet, r, c))
                  continue;

                o42_sheet_get_value (self->sheet, r, c, &v);
                if (v.type == O42_VALUE_NUMBER)
                  {
                    total += v.as.number;
                    count++;
                  }
                o42_value_clear (&v);
              }
        }
      g_array_unref (ranges);

      if (count > 0)
        {
          O42Value tv = o42_value_number (total);
          char *text = o42_value_display (&tv);
          char *line = g_strdup_printf ("SUM=%s", text);
          gtk_label_set_text (GTK_LABEL (self->status_sum), line);
          g_free (line);
          g_free (text);
        }
      else
        gtk_label_set_text (GTK_LABEL (self->status_sum), "");
    }
  else
    gtk_label_set_text (GTK_LABEL (self->status_sum), "");

  window_update_title (self);

  {
    double zoom = o42_grid_get_zoom (self->grid);
    char *text = (zoom == 1.0) ? g_strdup (_("Ready"))
                               : g_strdup_printf ("%s    %d%%", _("Ready"),
                                                  (int) (zoom * 100 + 0.5));
    gtk_label_set_text (GTK_LABEL (self->status_label), text);
    g_free (text);
  }

  {
    GAction *act;

    act = g_action_map_lookup_action (G_ACTION_MAP (self), "undo");
    if (act) g_simple_action_set_enabled (G_SIMPLE_ACTION (act), o42_sheet_can_undo (self->sheet));
    act = g_action_map_lookup_action (G_ACTION_MAP (self), "redo");
    if (act) g_simple_action_set_enabled (G_SIMPLE_ACTION (act), o42_sheet_can_redo (self->sheet));
  }

  self->updating = FALSE;
}

static void
on_grid_changed (O42Grid *grid, gpointer data)
{
  (void) grid;
  window_sync (O42_WINDOW (data));
  window_tell_book (O42_WINDOW (data), "cells");
}

/* ---------------------------------------------------------------------- */
/* Construction                                                            */
/* ---------------------------------------------------------------------- */

static void
o42_window_dispose (GObject *object)
{
  O42Window *self = O42_WINDOW (object);

  g_clear_pointer (&self->family_index, g_hash_table_destroy);
  g_clear_object (&self->families);

  if (self->grid != NULL)
    o42_grid_set_sheet (self->grid, NULL);
  self->sheet = NULL;
  if (self->book != NULL)
    {
      o42_book_unwatch (self->book, on_book_changed, self);
      o42_book_unref (self->book);
      self->book = NULL;
    }
  g_clear_pointer (&self->db, o42_db_close);
  g_clear_object (&self->file);
  g_clear_object (&self->page_setup);
  g_clear_object (&self->print_settings);

  G_OBJECT_CLASS (o42_window_parent_class)->dispose (object);
}

static void
o42_window_class_init (O42WindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = o42_window_dispose;
  GTK_WINDOW_CLASS (klass)->close_request = o42_window_close_request;
}

static void
on_grid_mapped (GtkWidget *widget, gpointer data)
{
  (void) data;
  gtk_widget_grab_focus (widget);
}

static void
o42_window_init (O42Window *self)
{
  GtkWidget *box, *menubar, *scrolled;
  GtkBuilder *builder;
  GMenuModel *model;

  self->book = o42_book_new ();
  self->sheet = o42_book_sheet (self->book, 0);
  o42_book_watch (self->book, on_book_changed, self);

  g_action_map_add_action_entries (G_ACTION_MAP (self), ACTIONS,
                                   G_N_ELEMENTS (ACTIONS), self);

  if (!o42_python_available ())
    {
      g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self), "python-console")), FALSE);
      g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self), "python-run")), FALSE);
      g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self), "scripts-run-all")), FALSE);
    }

  if (!o42_pdf_import_available ())
    {
      GAction *act = g_action_map_lookup_action (G_ACTION_MAP (self), "import-pdf");
      if (act != NULL)
        g_simple_action_set_enabled (G_SIMPLE_ACTION (act), FALSE);
    }

  for (guint i = 0; i < G_N_ELEMENTS (PLANNED) && PLANNED[i] != NULL; i++)
    {
      GSimpleAction *action = g_simple_action_new (PLANNED[i], NULL);
      g_signal_connect (action, "activate", G_CALLBACK (action_planned), self);
      g_simple_action_set_enabled (action, FALSE);
      g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (action));
      g_object_unref (action);
    }

  gtk_window_set_default_size (GTK_WINDOW (self), 960, 700);
  gtk_widget_add_css_class (GTK_WIDGET (self), "o42");
  gtk_window_set_titlebar (GTK_WINDOW (self), build_titlebar (self));
  gtk_window_set_title (GTK_WINDOW (self), _("Book1 - Office42 Spreadsheet"));
  gtk_window_set_icon_name (GTK_WINDOW (self), "net.office42.office42");

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child (GTK_WINDOW (self), box);

  /* The menu's labels are translated, so the builder is told which
   * catalogue they are in before it reads them. */
  builder = gtk_builder_new ();
  gtk_builder_set_translation_domain (builder, GETTEXT_PACKAGE);
  gtk_builder_add_from_resource (builder, "/net/office42/office42/menus.ui", NULL);
  model = G_MENU_MODEL (gtk_builder_get_object (builder, "menubar"));
  menubar = gtk_popover_menu_bar_new_from_model (model);
  gtk_widget_add_css_class (menubar, "o42-menubar");
  gtk_box_append (GTK_BOX (box), menubar);
  g_object_unref (builder);

  self->grid = O42_GRID (o42_grid_new ());

  gtk_box_append (GTK_BOX (box), build_standard_bar ());
  gtk_box_append (GTK_BOX (box), build_format_bar (self));
  gtk_box_append (GTK_BOX (box), build_formula_bar (self));

  scrolled = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scrolled, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_ALWAYS, GTK_POLICY_ALWAYS);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled),
                                 GTK_WIDGET (self->grid));
  /* Scripts that came with a file are never run on opening; a bar
   * says they are there, as Excel's "Enable content" does. */
  self->scripts_bar = gtk_revealer_new ();
  {
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new (_("This book has Python scripts in it. They have not been run."));
    GtkWidget *run = gtk_button_new_with_mnemonic (_("_Run Scripts"));
    GtkWidget *show = gtk_button_new_with_mnemonic (_("_Scripts..."));
    GtkWidget *hide = gtk_button_new_with_mnemonic (_("_Hide"));

    gtk_widget_add_css_class (row, "o42-scripts-bar");
    gtk_widget_set_margin_top (row, 4);
    gtk_widget_set_margin_bottom (row, 4);
    gtk_widget_set_margin_start (row, 8);
    gtk_widget_set_margin_end (row, 8);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_widget_set_hexpand (label, TRUE);
    gtk_actionable_set_action_name (GTK_ACTIONABLE (run), "win.scripts-run-all");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (show), "win.scripts");
    g_signal_connect_swapped (hide, "clicked", G_CALLBACK (scripts_bar_hide), self);
    gtk_box_append (GTK_BOX (row), label);
    gtk_box_append (GTK_BOX (row), run);
    gtk_box_append (GTK_BOX (row), show);
    gtk_box_append (GTK_BOX (row), hide);
    gtk_revealer_set_child (GTK_REVEALER (self->scripts_bar), row);
    gtk_revealer_set_reveal_child (GTK_REVEALER (self->scripts_bar), FALSE);
  }
  gtk_box_append (GTK_BOX (box), self->scripts_bar);

  gtk_box_append (GTK_BOX (box), scrolled);

  gtk_box_append (GTK_BOX (box), build_tabs (self));
  gtk_box_append (GTK_BOX (box), build_status_bar (self));

  g_signal_connect (self->grid, "selection-changed", G_CALLBACK (on_grid_changed), self);
  g_signal_connect (self->grid, "sheet-changed",     G_CALLBACK (on_grid_changed), self);
  g_signal_connect (self->grid, "run-script",        G_CALLBACK (on_grid_run_script), self);
  g_signal_connect (self->grid, "map", G_CALLBACK (on_grid_mapped), self);

  o42_grid_set_sheet (self->grid, self->sheet);
  window_sync (self);
}

GtkWidget *
o42_window_new (GtkApplication *app)
{
  return g_object_new (O42_TYPE_WINDOW, "application", app, NULL);
}

GtkWidget *
o42_window_new_on_book (GtkApplication *app, O42Book *book, GFile *file)
{
  O42Window *self = g_object_new (O42_TYPE_WINDOW, "application", app, NULL);
  int highest = 0;
  GList *windows;

  /* Swap the fresh book the constructor made for the shared one. */
  o42_grid_set_sheet (self->grid, NULL);
  o42_book_unwatch (self->book, on_book_changed, self);
  o42_book_unref (self->book);
  self->book = o42_book_ref (book);
  self->sheet = o42_book_sheet (book, 0);
  o42_book_watch (self->book, on_book_changed, self);
  o42_grid_set_sheet (self->grid, self->sheet);
  o42_grid_fit_wrapped_rows (self->grid);
  window_rebuild_tabs (self);

  /* SQLVALUE() asks this book's database from now on. */
  o42_db_register_function (self->book);

  if (file != NULL)
    self->file = g_object_ref (file);

  /* Number the views: the first window becomes :1 when a second appears. */
  windows = gtk_application_get_windows (app);
  for (GList *l = windows; l != NULL; l = l->next)
    {
      O42Window *other = O42_IS_WINDOW (l->data) ? l->data : NULL;
      if (other != NULL && other != self && other->book == book)
        {
          if (other->view_number == 0)
            {
              other->view_number = 1;
              window_update_title (other);
            }
          highest = MAX (highest, other->view_number);
        }
    }
  self->view_number = highest + 1;

  window_sync (self);
  return GTK_WIDGET (self);
}

static void
action_new_window (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkWidget *w;

  (void) a; (void) p;
  w = o42_window_new_on_book (gtk_window_get_application (GTK_WINDOW (self)),
                              self->book, self->file);
  gtk_window_present (GTK_WINDOW (w));
}
