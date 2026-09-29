/* o42-userform.c - Visual Basic's UserForms, drawn with GTK
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The macro's side (src/script/vbaforms.py) keeps a form's state and
 * says what changes, a property at a time, as text; this draws it.  A
 * form is a window whose GtkFixed holds each control where the form
 * put it, points made pixels at 96 to the inch; a Frame and a
 * MultiPage's page are a GtkFixed of their own.  What the user does
 * goes back through o42_python_form_event.  A change the macro made is
 * not reported back, since the macro fires its own Change for it.
 */

#include "o42-window-private.h"
#include "o42-python.h"

#include <glib/gi18n.h>
#include <stdlib.h>
#include <string.h>

#define PX(pt) ((int) ((pt) * 4.0 / 3.0 + 0.5))

/* ---- The canvas ----------------------------------------------------------------- */

/* A form, a Frame's inside and a page are canvases: each child has the
 * rectangle the form gave it -- no less than GTK can draw it in -- where
 * GtkFixed would give it its natural size, and a long label would run
 * out of its box. */
#define O42_TYPE_FORM_LAYOUT (o42_form_layout_get_type ())
G_DECLARE_FINAL_TYPE (O42FormLayout, o42_form_layout, O42, FORM_LAYOUT, GtkLayoutManager)

struct _O42FormLayout {
  GtkLayoutManager parent_instance;
};

G_DEFINE_FINAL_TYPE (O42FormLayout, o42_form_layout, GTK_TYPE_LAYOUT_MANAGER)

typedef struct {
  int x, y, width, height;
} Rect;

static Rect *
rect_of (GtkWidget *child)
{
  Rect *r = g_object_get_data (G_OBJECT (child), "o42-rect");

  if (r == NULL)
    {
      r = g_new0 (Rect, 1);
      g_object_set_data_full (G_OBJECT (child), "o42-rect", r, g_free);
    }
  return r;
}

static void
child_size (GtkWidget *child, Rect *r, int *width, int *height)
{
  int min;

  gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1, &min, NULL, NULL, NULL);
  *width = MAX (min, r->width);
  gtk_widget_measure (child, GTK_ORIENTATION_VERTICAL, *width, &min, NULL, NULL, NULL);
  *height = MAX (min, r->height);
}

/* A canvas is the size the form says -- its size request, or its place
 * in the canvas it sits on -- whatever its controls: one a form parks
 * outside its edges is out of sight, as in Excel. */
static void
form_layout_measure (GtkLayoutManager *manager, GtkWidget *widget, GtkOrientation orientation,
                     int for_size, int *minimum, int *natural, int *min_baseline, int *nat_baseline)
{
  (void) manager; (void) widget; (void) orientation; (void) for_size;
  *minimum = *natural = 0;
  *min_baseline = *nat_baseline = -1;
}

static void
form_layout_allocate (GtkLayoutManager *manager, GtkWidget *widget, int width, int height, int baseline)
{
  (void) manager; (void) width; (void) height; (void) baseline;

  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      Rect *r;
      GtkAllocation a;
      if (!gtk_widget_should_layout (child))
        continue;
      r = rect_of (child);
      a.x = r->x;
      a.y = r->y;
      child_size (child, r, &a.width, &a.height);
      gtk_widget_size_allocate (child, &a, -1);
    }
}

static void
o42_form_layout_class_init (O42FormLayoutClass *klass)
{
  GtkLayoutManagerClass *lm = GTK_LAYOUT_MANAGER_CLASS (klass);

  lm->measure = form_layout_measure;
  lm->allocate = form_layout_allocate;
}

static void
o42_form_layout_init (O42FormLayout *self)
{
  (void) self;
}

static GtkWidget *
canvas_new (void)
{
  GtkWidget *canvas = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_set_layout_manager (canvas, g_object_new (O42_TYPE_FORM_LAYOUT, NULL));
  gtk_widget_set_overflow (canvas, GTK_OVERFLOW_HIDDEN);
  return canvas;
}

static void
canvas_put (GtkWidget *canvas, GtkWidget *child, int x, int y)
{
  Rect *r = rect_of (child);

  r->x = x;
  r->y = y;
  gtk_box_append (GTK_BOX (canvas), child);
}

static void
canvas_move (GtkWidget *child, int x, int y)
{
  Rect *r = rect_of (child);

  r->x = x;
  r->y = y;
  gtk_widget_queue_resize (child);
}

static void
canvas_size (GtkWidget *child, int width, int height)
{
  Rect *r = rect_of (child);

  r->width = width;
  r->height = height;
  gtk_widget_queue_resize (child);
}

typedef struct _UForm UForm;

typedef struct {
  int            id;
  UForm         *form;
  char          *kind;
  GtkWidget     *widget;       /* what sits in the parent's GtkFixed */
  GtkWidget     *inner;        /* the entry, text view, list or button that holds the value */
  GtkWidget     *fixed;        /* a Frame's or a page's own canvas */
  GtkWidget     *parent_fixed; /* the canvas it sits on */
  GtkWidget     *label;        /* a page's tab */
  GtkWidget     *entry;        /* an editable combo box's */
  GtkWidget     *popover;      /* and its list */
  GtkStringList *strings;      /* a drop-down list's rows */
  double         left, top, width, height;
  int            value, min, max, small, large;
  int            style, multiselect;
  gboolean       quiet;        /* inside a change the macro made */
  gboolean       wrap, multiline, locked;
  char          *group;
  char          *text;         /* a combo box's or a text box's, before it is drawn */
  char          *items;
  char          *font, *fore, *back;
} UCtl;

struct _UForm {
  int            id;
  O42Book       *book;
  GtkWidget     *window;
  GtkWidget     *fixed;
  GHashTable    *controls;     /* id -> UCtl */
  GHashTable    *groups;       /* option group -> its first GtkCheckButton */
  GMainLoop     *loop;         /* while it is shown modally */
  GtkCssProvider *css;
  char          *font, *back;
  gboolean       closed;
};

static GHashTable     *forms;        /* id -> UForm */
static GtkApplication *forms_app;
static int             next_form = 1, next_control = 1;

static UForm *
form_of (int id)
{
  return forms != NULL ? g_hash_table_lookup (forms, GINT_TO_POINTER (id)) : NULL;
}

static UCtl *
control_of (UForm *f, int id)
{
  return f != NULL ? g_hash_table_lookup (f->controls, GINT_TO_POINTER (id)) : NULL;
}

static O42Window *
window_of (O42Book *book)
{
  GList *windows = forms_app != NULL ? gtk_application_get_windows (forms_app) : NULL;

  for (GList *l = windows; l != NULL; l = l->next)
    if (O42_IS_WINDOW (l->data) && O42_WINDOW (l->data)->book == book)
      return O42_WINDOW (l->data);
  return NULL;
}

/* ---- Looks -------------------------------------------------------------------- */

/* Windows 97's controls are small: the theme's padding would not let
 * them into the boxes the form drew for them. */
static void
install_css (void)
{
  static gboolean done = FALSE;
  GtkCssProvider *provider;

  if (done)
    return;
  done = TRUE;
  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider,
    ".o42form entry, .o42form textview, .o42form button, .o42form dropdown button,"
    ".o42form checkbutton, .o42form togglebutton { min-height: 0; min-width: 0; }"
    ".o42form entry { padding: 0 3px; border-radius: 0; }"
    ".o42form button { padding: 0 4px; border-radius: 2px; }"
    ".o42form checkbutton { padding: 0; }"
    ".o42form checkbutton check, .o42form checkbutton radio { min-width: 12px; min-height: 12px; margin: 0 4px 0 0; }"
    ".o42form dropdown > button { padding: 0 2px; }"
    ".o42form list row { min-height: 0; padding: 0 3px; }"
    ".o42form notebook > header tab { min-height: 0; padding: 1px 6px; }"
    ".o42form frame > label { margin: 0 2px; }"
    ".o42form .o42spin button { padding: 0; min-width: 0; min-height: 0; }");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);
}

/* "Tahoma|8.25|1|0|0|0": name, points, bold, italic, underline, strikeout. */
static void
append_font (GString *css, const char *font)
{
  char **p = g_strsplit (font != NULL ? font : "", "|", -1);

  if (p[0] != NULL && *p[0] != '\0')
    g_string_append_printf (css, "font-family: \"%s\", sans-serif;", p[0]);
  if (p[0] != NULL && p[1] != NULL)
    g_string_append_printf (css, "font-size: %spt;", p[1]);
  if (p[0] != NULL && p[1] != NULL && p[2] != NULL)
    g_string_append_printf (css, "font-weight: %s;", strcmp (p[2], "1") == 0 ? "bold" : "normal");
  if (p[0] != NULL && p[1] != NULL && p[2] != NULL && p[3] != NULL)
    g_string_append_printf (css, "font-style: %s;", strcmp (p[3], "1") == 0 ? "italic" : "normal");
  g_strfreev (p);
}

/* Each control's font and colours, as CSS rules for its widget's name. */
static void
restyle (UForm *f)
{
  GString *css = g_string_new (NULL);
  GHashTableIter it;
  gpointer value;

  g_string_append_printf (css, "#o42form-%d { background-color: %s; }", f->id,
                          f->back != NULL ? f->back : "#d4d0c8");
  g_string_append_printf (css, "#o42form-%d * {", f->id);
  append_font (css, f->font);
  g_string_append (css, "}");
  g_hash_table_iter_init (&it, f->controls);
  while (g_hash_table_iter_next (&it, NULL, &value))
    {
      UCtl *c = value;
      g_string_append_printf (css, "#o42ctl-%d, #o42ctl-%d * {", c->id, c->id);
      append_font (css, c->font);
      if (c->fore != NULL && strcmp (c->kind, "CommandButton") != 0)
        g_string_append_printf (css, "color: %s;", c->fore);
      g_string_append (css, "}");
      /* The window's own colour behind a text box or a list is the
       * theme's; one the form chose is drawn. */
      if (c->back != NULL && strcmp (c->back, "#ffffff") != 0 && strcmp (c->back, "#d4d0c8") != 0
          && strcmp (c->kind, "CommandButton") != 0)
        g_string_append_printf (css, "#o42ctl-%d { background-color: %s; }", c->id, c->back);
    }
  gtk_css_provider_load_from_string (f->css, css->str);
  g_string_free (css, TRUE);
}

static void
control_free (gpointer data)
{
  UCtl *c = data;

  g_free (c->kind);
  g_free (c->group);
  g_free (c->text);
  g_free (c->items);
  g_free (c->font);
  g_free (c->fore);
  g_free (c->back);
  g_free (c);
}

/* ---- What the user does ---------------------------------------------------------- */

static gboolean
free_form_later (gpointer data)
{
  UForm *f = data;

  g_hash_table_unref (f->controls);
  g_hash_table_unref (f->groups);
  gtk_style_context_remove_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (f->css));
  g_object_unref (f->css);
  g_free (f->font);
  g_free (f->back);
  g_free (f);
  return G_SOURCE_REMOVE;
}

static gboolean
report (UForm *f, UCtl *c, const char *event, const char *value)
{
  O42Window *w;
  char *said = NULL;
  gboolean cancelled;

  if (f == NULL || f->closed || (c != NULL && c->quiet))
    return FALSE;
  cancelled = o42_python_form_event (f->book, f->id, c != NULL ? c->id : 0, event, value, &said);
  w = window_of (f->book);
  if (w != NULL)
    {
      o42_grid_refresh (w->grid);
      o42_window_sync (w);
    }
  if (said != NULL)
    {
      /* A form left open runs its handlers on their own; what they say,
       * an error say, is shown as a macro's is. */
      GtkAlertDialog *alert = gtk_alert_dialog_new ("%s", _("Microsoft Visual Basic"));
      gtk_alert_dialog_set_detail (alert, said);
      gtk_alert_dialog_show (alert, f->window != NULL ? GTK_WINDOW (f->window) : NULL);
      g_object_unref (alert);
      g_free (said);
    }
  return cancelled;
}

static void
on_clicked (GtkButton *b, gpointer data)
{
  UCtl *c = data;
  (void) b;
  report (c->form, c, "click", "");
}

static void
on_toggled (GtkCheckButton *b, gpointer data)
{
  UCtl *c = data;
  gboolean on = gtk_check_button_get_active (b);

  /* An option button going off is its group's business, not a change
   * of its own the macro needs to hear. */
  if (strcmp (c->kind, "OptionButton") == 0 && !on)
    return;
  report (c->form, c, "change", on ? "1" : "0");
}

static void
on_toggle_button (GtkToggleButton *b, gpointer data)
{
  UCtl *c = data;
  report (c->form, c, "change", gtk_toggle_button_get_active (b) ? "1" : "0");
}

static void
on_entry_changed (GtkEditable *e, gpointer data)
{
  UCtl *c = data;
  const char *text = gtk_editable_get_text (e);

  if (c->entry != NULL)
    {
      char *value = g_strconcat ("text:", text, NULL);
      report (c->form, c, "change", value);
      g_free (value);
    }
  else
    report (c->form, c, "change", text);
}

static void
on_buffer_changed (GtkTextBuffer *buffer, gpointer data)
{
  UCtl *c = data;
  GtkTextIter a, b;
  char *text;

  gtk_text_buffer_get_bounds (buffer, &a, &b);
  text = gtk_text_buffer_get_text (buffer, &a, &b, FALSE);
  report (c->form, c, "change", text);
  g_free (text);
}

static void
on_list_selection (GtkListBox *list, gpointer data)
{
  UCtl *c = data;
  GList *rows = gtk_list_box_get_selected_rows (list);
  char buf[32];

  if (c->multiselect)
    {
      GString *value = g_string_new ("selected:");
      for (GList *l = rows; l != NULL; l = l->next)
        g_string_append_printf (value, "%s%d", l == rows ? "" : ",",
                                gtk_list_box_row_get_index (l->data));
      report (c->form, c, "change", value->str);
      g_string_free (value, TRUE);
    }
  else
    {
      g_snprintf (buf, sizeof buf, "%d", rows != NULL ? gtk_list_box_row_get_index (rows->data) : -1);
      report (c->form, c, "change", buf);
    }
  g_list_free (rows);
}

static void
on_list_activated (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  UCtl *c = data;
  (void) list; (void) row;
  report (c->form, c, "dblclick", "");
}

static void
on_dropdown_selected (GObject *object, GParamSpec *pspec, gpointer data)
{
  UCtl *c = data;
  guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (object));
  char buf[32];
  (void) pspec;

  g_snprintf (buf, sizeof buf, "%d", i == GTK_INVALID_LIST_POSITION ? -1 : (int) i);
  report (c->form, c, "change", buf);
}

/* An editable combo box's list: choosing a row sets the text. */
static void
on_combo_row (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  UCtl *c = data;
  char buf[32];
  (void) list;

  gtk_popover_popdown (GTK_POPOVER (c->popover));
  g_snprintf (buf, sizeof buf, "%d", gtk_list_box_row_get_index (row));
  c->quiet = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (c->entry),
                         gtk_label_get_text (GTK_LABEL (gtk_list_box_row_get_child (row))));
  c->quiet = FALSE;
  report (c->form, c, "change", buf);
}

static void
spin_step (UCtl *c, int direction)
{
  int v = c->value + direction * (c->small != 0 ? c->small : 1);
  char buf[32];

  v = MAX (MIN (c->min, c->max), MIN (MAX (c->min, c->max), v));
  if (v == c->value)
    return;
  c->value = v;
  g_snprintf (buf, sizeof buf, "%d", v);
  report (c->form, c, "change", buf);
}

static void on_spin_up (GtkButton *b, gpointer data)   { (void) b; spin_step (data, 1); }
static void on_spin_down (GtkButton *b, gpointer data) { (void) b; spin_step (data, -1); }

static void
on_scrolled (GtkRange *range, gpointer data)
{
  UCtl *c = data;
  char buf[32];

  c->value = (int) gtk_range_get_value (range);
  g_snprintf (buf, sizeof buf, "%d", c->value);
  report (c->form, c, "change", buf);
}

static void
on_page_switched (GtkNotebook *nb, GtkWidget *page, guint index, gpointer data)
{
  UCtl *c = data;
  char buf[32];
  (void) nb; (void) page;

  g_snprintf (buf, sizeof buf, "%u", index);
  report (c->form, c, "change", buf);
}

static gboolean
on_close_request (GtkWindow *window, gpointer data)
{
  UForm *f = data;
  (void) window;

  /* The close box is QueryClose's to allow; if it does, the macro's
   * side closes the form itself. */
  report (f, NULL, "close", "");
  return TRUE;
}

/* Escape presses the button whose Cancel is True, as in Windows. */
static gboolean
on_form_key (GtkEventControllerKey *key, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
  UForm *f = data;
  GHashTableIter it;
  gpointer value;
  (void) key; (void) keycode; (void) state;

  if (keyval != GDK_KEY_Escape)
    return FALSE;
  g_hash_table_iter_init (&it, f->controls);
  while (g_hash_table_iter_next (&it, NULL, &value))
    {
      UCtl *c = value;
      if (strcmp (c->kind, "CommandButton") == 0 && g_object_get_data (G_OBJECT (c->inner), "o42-cancel") != NULL
          && gtk_widget_get_sensitive (c->inner))
        {
          report (f, c, "click", "");
          return TRUE;
        }
    }
  return FALSE;
}

/* ---- The host --------------------------------------------------------------------- */

static int
uf_open (gpointer user, O42Book *book, const char *caption, double width, double height)
{
  UForm *f = g_new0 (UForm, 1);
  O42Window *owner = window_of (book);
  GtkEventController *key = gtk_event_controller_key_new ();
  char *name;
  (void) user;

  install_css ();
  if (forms == NULL)
    forms = g_hash_table_new (g_direct_hash, g_direct_equal);
  f->id = next_form++;
  f->book = book;
  f->controls = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, control_free);
  f->groups = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  f->css = gtk_css_provider_new ();
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (f->css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  f->window = gtk_window_new ();
  gtk_widget_add_css_class (f->window, "o42form");
  name = g_strdup_printf ("o42form-%d", f->id);
  gtk_widget_set_name (f->window, name);
  g_free (name);
  gtk_window_set_title (GTK_WINDOW (f->window), caption);
  gtk_window_set_resizable (GTK_WINDOW (f->window), FALSE);
  if (owner != NULL)
    {
      gtk_window_set_transient_for (GTK_WINDOW (f->window), GTK_WINDOW (owner));
      gtk_window_set_destroy_with_parent (GTK_WINDOW (f->window), TRUE);
    }
  f->fixed = canvas_new ();
  gtk_widget_set_size_request (f->fixed, PX (width), PX (height));
  gtk_window_set_child (GTK_WINDOW (f->window), f->fixed);
  g_signal_connect (f->window, "close-request", G_CALLBACK (on_close_request), f);
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_form_key), f);
  gtk_widget_add_controller (f->window, key);
  g_hash_table_insert (forms, GINT_TO_POINTER (f->id), f);
  restyle (f);
  return f->id;
}

static GtkWidget *
make_list (UCtl *c)
{
  GtkWidget *scroll = gtk_scrolled_window_new ();
  c->inner = gtk_list_box_new ();
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (c->inner), FALSE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), c->inner);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroll), TRUE);
  g_signal_connect (c->inner, "selected-rows-changed", G_CALLBACK (on_list_selection), c);
  g_signal_connect (c->inner, "row-activated", G_CALLBACK (on_list_activated), c);
  return scroll;
}

static int
uf_add (gpointer user, int form, int parent, const char *kind, const char *name)
{
  UForm *f = form_of (form);
  UCtl *c, *p;
  char *wname;
  (void) user; (void) name;

  if (f == NULL)
    return 0;
  c = g_new0 (UCtl, 1);
  c->id = next_control++;
  c->form = f;
  c->kind = g_strdup (kind);
  c->max = 100;
  c->small = c->large = 1;
  p = control_of (f, parent);
  c->parent_fixed = p != NULL && p->fixed != NULL ? p->fixed : f->fixed;

  if (strcmp (kind, "Label") == 0)
    {
      c->widget = c->inner = gtk_label_new ("");
      gtk_label_set_xalign (GTK_LABEL (c->inner), 0.0);
      gtk_label_set_yalign (GTK_LABEL (c->inner), 0.0);
      gtk_label_set_ellipsize (GTK_LABEL (c->inner), PANGO_ELLIPSIZE_END);
    }
  else if (strcmp (kind, "CommandButton") == 0)
    {
      c->widget = c->inner = gtk_button_new_with_label ("");
      g_signal_connect (c->inner, "clicked", G_CALLBACK (on_clicked), c);
    }
  else if (strcmp (kind, "CheckBox") == 0 || strcmp (kind, "OptionButton") == 0)
    {
      c->widget = c->inner = gtk_check_button_new_with_label ("");
      g_signal_connect (c->inner, "toggled", G_CALLBACK (on_toggled), c);
    }
  else if (strcmp (kind, "ToggleButton") == 0)
    {
      c->widget = c->inner = gtk_toggle_button_new_with_label ("");
      g_signal_connect (c->inner, "toggled", G_CALLBACK (on_toggle_button), c);
    }
  else if (strcmp (kind, "TextBox") == 0)
    {
      /* A GtkEntry until MultiLine says otherwise. */
      c->widget = c->inner = gtk_entry_new ();
      g_signal_connect (c->inner, "changed", G_CALLBACK (on_entry_changed), c);
    }
  else if (strcmp (kind, "ListBox") == 0)
    c->widget = make_list (c);
  else if (strcmp (kind, "ComboBox") == 0)
    {
      /* A combo box one may type in: an entry with a button whose
       * popover lists the rows.  Style 2 turns it into a drop-down. */
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *button = gtk_menu_button_new ();
      GtkWidget *scroll = gtk_scrolled_window_new ();
      GtkWidget *list = gtk_list_box_new ();

      c->entry = c->inner = gtk_entry_new ();
      gtk_widget_set_hexpand (c->entry, TRUE);
      gtk_box_append (GTK_BOX (box), c->entry);
      c->popover = gtk_popover_new ();
      gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), list);
      gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
      gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), 200);
      gtk_popover_set_child (GTK_POPOVER (c->popover), scroll);
      gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), c->popover);
      gtk_box_append (GTK_BOX (box), button);
      g_object_set_data (G_OBJECT (box), "o42-list", list);
      g_signal_connect (list, "row-activated", G_CALLBACK (on_combo_row), c);
      gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (list), TRUE);
      g_signal_connect (c->entry, "changed", G_CALLBACK (on_entry_changed), c);
      c->widget = box;
    }
  else if (strcmp (kind, "SpinButton") == 0)
    {
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      GtkWidget *up = gtk_button_new_from_icon_name ("pan-up-symbolic");
      GtkWidget *down = gtk_button_new_from_icon_name ("pan-down-symbolic");

      gtk_widget_add_css_class (box, "o42spin");
      gtk_widget_set_vexpand (up, TRUE);
      gtk_widget_set_vexpand (down, TRUE);
      gtk_box_append (GTK_BOX (box), up);
      gtk_box_append (GTK_BOX (box), down);
      g_signal_connect (up, "clicked", G_CALLBACK (on_spin_up), c);
      g_signal_connect (down, "clicked", G_CALLBACK (on_spin_down), c);
      c->widget = c->inner = box;
    }
  else if (strcmp (kind, "ScrollBar") == 0)
    {
      GtkAdjustment *adj = gtk_adjustment_new (0, 0, 101, 1, 1, 1);
      c->widget = c->inner = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL, adj);
      g_signal_connect (c->inner, "value-changed", G_CALLBACK (on_scrolled), c);
    }
  else if (strcmp (kind, "Frame") == 0)
    {
      c->widget = c->inner = gtk_frame_new ("");
      c->fixed = canvas_new ();
      gtk_frame_set_child (GTK_FRAME (c->widget), c->fixed);
    }
  else if (strcmp (kind, "MultiPage") == 0)
    {
      c->widget = c->inner = gtk_notebook_new ();
      g_signal_connect (c->inner, "switch-page", G_CALLBACK (on_page_switched), c);
    }
  else if (strcmp (kind, "Page") == 0 && p != NULL && GTK_IS_NOTEBOOK (p->inner))
    {
      c->fixed = canvas_new ();
      c->label = gtk_label_new (name);
      c->quiet = TRUE;
      p->quiet = TRUE;
      gtk_notebook_append_page (GTK_NOTEBOOK (p->inner), c->fixed, c->label);
      p->quiet = FALSE;
      c->quiet = FALSE;
      g_hash_table_insert (f->controls, GINT_TO_POINTER (c->id), c);
      return c->id;
    }
  else if (strcmp (kind, "Image") == 0)
    c->widget = c->inner = gtk_picture_new ();
  else
    c->widget = c->inner = gtk_label_new ("");

  wname = g_strdup_printf ("o42ctl-%d", c->id);
  gtk_widget_set_name (c->widget, wname);
  g_free (wname);
  canvas_put (c->parent_fixed, c->widget, 0, 0);
  g_hash_table_insert (f->controls, GINT_TO_POINTER (c->id), c);
  return c->id;
}

/* A caption as it stands: Visual Basic keeps a control's accelerator
 * apart, so an "&" in one is an ampersand. */
static void
set_label_text (GtkWidget *w, const char *text)
{
  if (GTK_IS_LABEL (w))
    gtk_label_set_text (GTK_LABEL (w), text);
  else if (GTK_IS_BUTTON (w))
    gtk_button_set_label (GTK_BUTTON (w), text);
  else if (GTK_IS_CHECK_BUTTON (w))
    gtk_check_button_set_label (GTK_CHECK_BUTTON (w), text);
  else if (GTK_IS_FRAME (w))
    gtk_frame_set_label (GTK_FRAME (w), text);
}

static void
fill_list (UCtl *c)
{
  char **rows = g_strsplit (c->items != NULL ? c->items : "", "\n", -1);
  gboolean empty = c->items == NULL || *c->items == '\0';

  c->quiet = TRUE;
  if (strcmp (c->kind, "ListBox") == 0)
    {
      GtkWidget *child;
      while ((child = gtk_widget_get_first_child (c->inner)) != NULL)
        gtk_list_box_remove (GTK_LIST_BOX (c->inner), child);
      for (int i = 0; !empty && rows[i] != NULL; i++)
        {
          /* Columns side by side, as a list with ColumnCount 2 shows them. */
          char **cols = g_strsplit (rows[i], "\t", -1);
          GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
          for (int k = 0; cols[k] != NULL; k++)
            {
              GtkWidget *cell = gtk_label_new (cols[k]);
              gtk_label_set_xalign (GTK_LABEL (cell), 0.0);
              if (k == 0)
                gtk_widget_set_hexpand (cell, TRUE);
              gtk_box_append (GTK_BOX (row), cell);
            }
          gtk_list_box_append (GTK_LIST_BOX (c->inner), row);
          g_strfreev (cols);
        }
    }
  else if (c->strings != NULL)
    {
      guint n = g_list_model_get_n_items (G_LIST_MODEL (c->strings));
      GPtrArray *add = g_ptr_array_new ();
      for (int i = 0; !empty && rows[i] != NULL; i++)
        {
          char *tab = strchr (rows[i], '\t');
          if (tab != NULL)
            *tab = '\0';
          g_ptr_array_add (add, rows[i]);
        }
      g_ptr_array_add (add, NULL);
      gtk_string_list_splice (c->strings, 0, n, (const char *const *) add->pdata);
      g_ptr_array_unref (add);
      gtk_drop_down_set_selected (GTK_DROP_DOWN (c->inner), GTK_INVALID_LIST_POSITION);
    }
  else if (c->entry != NULL)
    {
      GtkWidget *list = g_object_get_data (G_OBJECT (c->widget), "o42-list"), *child;
      while ((child = gtk_widget_get_first_child (list)) != NULL)
        gtk_list_box_remove (GTK_LIST_BOX (list), child);
      for (int i = 0; !empty && rows[i] != NULL; i++)
        {
          char *tab = strchr (rows[i], '\t');
          GtkWidget *label;
          if (tab != NULL)
            *tab = '\0';
          label = gtk_label_new (rows[i]);
          gtk_label_set_xalign (GTK_LABEL (label), 0.0);
          gtk_list_box_append (GTK_LIST_BOX (list), label);
        }
    }
  c->quiet = FALSE;
  g_strfreev (rows);
}

/* A ComboBox with Style 2 is a drop-down list: nothing to type in. */
static void
make_drop_down (UCtl *c)
{
  GtkWidget *drop;
  char *wname;

  if (c->strings != NULL)
    return;
  gtk_box_remove (GTK_BOX (c->parent_fixed), c->widget);
  c->entry = NULL;
  c->strings = gtk_string_list_new (NULL);
  drop = gtk_drop_down_new (G_LIST_MODEL (c->strings), NULL);
  c->widget = c->inner = drop;
  wname = g_strdup_printf ("o42ctl-%d", c->id);
  gtk_widget_set_name (drop, wname);
  g_free (wname);
  canvas_put (c->parent_fixed, drop, PX (c->left), PX (c->top));
  canvas_size (drop, PX (c->width), PX (c->height));
  g_signal_connect (drop, "notify::selected", G_CALLBACK (on_dropdown_selected), c);
  fill_list (c);
}

/* A TextBox with MultiLine is a text view in a scrolled window. */
static void
make_multiline (UCtl *c)
{
  GtkWidget *scroll, *view;
  char *wname;

  if (GTK_IS_TEXT_VIEW (c->inner))
    return;
  gtk_box_remove (GTK_BOX (c->parent_fixed), c->widget);
  scroll = gtk_scrolled_window_new ();
  view = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), view);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroll), TRUE);
  c->widget = scroll;
  c->inner = view;
  wname = g_strdup_printf ("o42ctl-%d", c->id);
  gtk_widget_set_name (scroll, wname);
  g_free (wname);
  canvas_put (c->parent_fixed, scroll, PX (c->left), PX (c->top));
  canvas_size (scroll, PX (c->width), PX (c->height));
  if (c->text != NULL)
    gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)), c->text, -1);
  g_signal_connect (gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)), "changed",
                    G_CALLBACK (on_buffer_changed), c);
}

static void
set_text (UCtl *c, const char *value)
{
  g_free (c->text);
  c->text = g_strdup (value);
  if (GTK_IS_TEXT_VIEW (c->inner))
    gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (c->inner)), value, -1);
  else if (GTK_IS_EDITABLE (c->inner))
    gtk_editable_set_text (GTK_EDITABLE (c->inner), value);
}

static void
select_index (UCtl *c, int index)
{
  if (c->strings != NULL)
    gtk_drop_down_set_selected (GTK_DROP_DOWN (c->inner), index < 0 ? GTK_INVALID_LIST_POSITION : (guint) index);
  else if (GTK_IS_LIST_BOX (c->inner))
    {
      GtkListBoxRow *row = index >= 0 ? gtk_list_box_get_row_at_index (GTK_LIST_BOX (c->inner), index) : NULL;
      if (row != NULL)
        gtk_list_box_select_row (GTK_LIST_BOX (c->inner), row);
      else
        gtk_list_box_unselect_all (GTK_LIST_BOX (c->inner));
    }
  else if (c->entry != NULL && index >= 0)
    {
      char **rows = g_strsplit (c->items != NULL ? c->items : "", "\n", -1);
      if (index < (int) g_strv_length (rows))
        {
          char *tab = strchr (rows[index], '\t');
          if (tab != NULL)
            *tab = '\0';
          gtk_editable_set_text (GTK_EDITABLE (c->entry), rows[index]);
        }
      g_strfreev (rows);
    }
}

/* A texture of the pixbuf's pixels.  gdk_texture_new_for_pixbuf does
 * this, and GTK 4.20 deprecates it; a form's picture is often a BMP or
 * a GIF, which gdk-pixbuf reads, so the pixbuf stays. */
static GdkTexture *
texture_for_pixbuf (GdkPixbuf *pixbuf)
{
  GBytes *pixels = gdk_pixbuf_read_pixel_bytes (pixbuf);
  GdkTexture *texture = gdk_memory_texture_new (gdk_pixbuf_get_width (pixbuf),
                                                gdk_pixbuf_get_height (pixbuf),
                                                gdk_pixbuf_get_has_alpha (pixbuf)
                                                  ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                                pixels, gdk_pixbuf_get_rowstride (pixbuf));
  g_bytes_unref (pixels);
  return texture;
}

static void
uf_set (gpointer user, int form, int control, const char *prop, const char *value)
{
  UForm *f = form_of (form);
  UCtl *c;
  (void) user;

  if (f == NULL)
    return;
  if (control == 0)
    {
      if (strcmp (prop, "caption") == 0)
        gtk_window_set_title (GTK_WINDOW (f->window), value);
      else if (strcmp (prop, "width") == 0 || strcmp (prop, "height") == 0)
        {
          int w, h;
          gtk_widget_get_size_request (f->fixed, &w, &h);
          if (strcmp (prop, "width") == 0)
            w = PX (g_ascii_strtod (value, NULL));
          else
            h = PX (g_ascii_strtod (value, NULL));
          gtk_widget_set_size_request (f->fixed, w, h);
        }
      else if (strcmp (prop, "back") == 0 || strcmp (prop, "font") == 0)
        {
          char **slot = strcmp (prop, "back") == 0 ? &f->back : &f->font;
          g_free (*slot);
          *slot = g_strdup (value);
          restyle (f);
        }
      else if (strcmp (prop, "enabled") == 0)
        gtk_widget_set_sensitive (f->fixed, strcmp (value, "1") == 0);
      return;
    }
  c = control_of (f, control);
  if (c == NULL)
    return;
  c->quiet = TRUE;
  if (strcmp (prop, "left") == 0 || strcmp (prop, "top") == 0)
    {
      if (strcmp (prop, "left") == 0)
        c->left = g_ascii_strtod (value, NULL);
      else
        c->top = g_ascii_strtod (value, NULL);
      if (c->widget != NULL)
        canvas_move (c->widget, PX (c->left), PX (c->top));
    }
  else if (strcmp (prop, "width") == 0 || strcmp (prop, "height") == 0)
    {
      if (strcmp (prop, "width") == 0)
        c->width = g_ascii_strtod (value, NULL);
      else
        c->height = g_ascii_strtod (value, NULL);
      if (c->widget != NULL)
        canvas_size (c->widget, MAX (1, PX (c->width)), MAX (1, PX (c->height)));
      /* An editable combo box's list is as wide as the box. */
      if (c->popover != NULL)
        gtk_widget_set_size_request (gtk_popover_get_child (GTK_POPOVER (c->popover)),
                                     MAX (1, PX (c->width)), -1);
    }
  else if (strcmp (prop, "caption") == 0)
    {
      if (c->label != NULL)
        gtk_label_set_text (GTK_LABEL (c->label), value);
      else
        set_label_text (c->inner, value);
    }
  else if (strcmp (prop, "wrap") == 0 && GTK_IS_LABEL (c->inner))
    {
      c->wrap = strcmp (value, "1") == 0;
      gtk_label_set_wrap (GTK_LABEL (c->inner), c->wrap);
      gtk_label_set_wrap_mode (GTK_LABEL (c->inner), PANGO_WRAP_WORD_CHAR);
      gtk_label_set_ellipsize (GTK_LABEL (c->inner), c->wrap ? PANGO_ELLIPSIZE_NONE : PANGO_ELLIPSIZE_END);
    }
  else if (strcmp (prop, "text") == 0)
    set_text (c, value);
  else if (strcmp (prop, "multiline") == 0 && strcmp (value, "1") == 0 && strcmp (c->kind, "TextBox") == 0)
    make_multiline (c);
  else if (strcmp (prop, "password") == 0 && GTK_IS_ENTRY (c->inner))
    {
      gtk_entry_set_visibility (GTK_ENTRY (c->inner), *value == '\0');
      if (*value != '\0')
        gtk_entry_set_invisible_char (GTK_ENTRY (c->inner), g_utf8_get_char (value));
    }
  else if (strcmp (prop, "maxlength") == 0 && GTK_IS_ENTRY (c->inner))
    gtk_entry_set_max_length (GTK_ENTRY (c->inner), atoi (value));
  else if (strcmp (prop, "locked") == 0)
    {
      c->locked = strcmp (value, "1") == 0;
      if (GTK_IS_EDITABLE (c->inner))
        gtk_editable_set_editable (GTK_EDITABLE (c->inner), !c->locked);
      else if (GTK_IS_TEXT_VIEW (c->inner))
        gtk_text_view_set_editable (GTK_TEXT_VIEW (c->inner), !c->locked);
    }
  else if (strcmp (prop, "value") == 0)
    {
      if (GTK_IS_CHECK_BUTTON (c->inner))
        gtk_check_button_set_active (GTK_CHECK_BUTTON (c->inner), strcmp (value, "1") == 0);
      else if (GTK_IS_TOGGLE_BUTTON (c->inner))
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (c->inner), strcmp (value, "1") == 0);
      else if (GTK_IS_NOTEBOOK (c->inner))
        gtk_notebook_set_current_page (GTK_NOTEBOOK (c->inner), atoi (value));
      else
        {
          c->value = atoi (value);
          if (GTK_IS_RANGE (c->inner))
            gtk_range_set_value (GTK_RANGE (c->inner), c->value);
        }
    }
  else if ((strcmp (prop, "min") == 0 || strcmp (prop, "max") == 0 || strcmp (prop, "small") == 0 ||
            strcmp (prop, "large") == 0))
    {
      int v = atoi (value);
      if (strcmp (prop, "min") == 0) c->min = v;
      else if (strcmp (prop, "max") == 0) c->max = v;
      else if (strcmp (prop, "small") == 0) c->small = v;
      else c->large = v;
      if (GTK_IS_RANGE (c->inner))
        {
          GtkAdjustment *adj = gtk_range_get_adjustment (GTK_RANGE (c->inner));
          gtk_adjustment_configure (adj, c->value, MIN (c->min, c->max), MAX (c->min, c->max) + c->large,
                                    c->small, c->large, c->large);
        }
    }
  else if (strcmp (prop, "style") == 0 && strcmp (c->kind, "ComboBox") == 0)
    {
      c->style = atoi (value);
      if (c->style == 2)
        make_drop_down (c);
    }
  else if (strcmp (prop, "multiselect") == 0 && GTK_IS_LIST_BOX (c->inner))
    {
      c->multiselect = atoi (value);
      gtk_list_box_set_selection_mode (GTK_LIST_BOX (c->inner),
                                       c->multiselect ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);
    }
  else if (strcmp (prop, "items") == 0)
    {
      g_free (c->items);
      c->items = g_strdup (value);
      fill_list (c);
    }
  else if (strcmp (prop, "index") == 0)
    select_index (c, atoi (value));
  else if (strcmp (prop, "selected") == 0 && GTK_IS_LIST_BOX (c->inner))
    {
      char **chosen = g_strsplit (value, ",", -1);
      gtk_list_box_unselect_all (GTK_LIST_BOX (c->inner));
      for (int i = 0; chosen[i] != NULL; i++)
        if (*chosen[i] != '\0')
          {
            GtkListBoxRow *row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (c->inner), atoi (chosen[i]));
            if (row != NULL)
              gtk_list_box_select_row (GTK_LIST_BOX (c->inner), row);
          }
      g_strfreev (chosen);
    }
  else if (strcmp (prop, "group") == 0 && strcmp (c->kind, "OptionButton") == 0 && *value != '\0')
    {
      GtkWidget *first = g_hash_table_lookup (f->groups, value);
      if (first == NULL)
        g_hash_table_insert (f->groups, g_strdup (value), c->inner);
      else if (first != c->inner)
        gtk_check_button_set_group (GTK_CHECK_BUTTON (c->inner), GTK_CHECK_BUTTON (first));
    }
  else if (strcmp (prop, "enabled") == 0 && c->widget != NULL)
    gtk_widget_set_sensitive (c->widget, strcmp (value, "1") == 0);
  else if (strcmp (prop, "visible") == 0 && c->widget != NULL)
    gtk_widget_set_visible (c->widget, strcmp (value, "1") == 0);
  else if (strcmp (prop, "default") == 0 && strcmp (value, "1") == 0 && GTK_IS_BUTTON (c->inner))
    gtk_window_set_default_widget (GTK_WINDOW (f->window), c->inner);
  else if (strcmp (prop, "cancel") == 0 && GTK_IS_BUTTON (c->inner))
    g_object_set_data (G_OBJECT (c->inner), "o42-cancel", strcmp (value, "1") == 0 ? c : NULL);
  else if (strcmp (prop, "tip") == 0 && c->widget != NULL)
    gtk_widget_set_tooltip_text (c->widget, *value != '\0' ? value : NULL);
  else if (strcmp (prop, "focus") == 0 && c->inner != NULL)
    gtk_widget_grab_focus (c->inner);
  else if (strcmp (prop, "font") == 0 || strcmp (prop, "fore") == 0 || strcmp (prop, "back") == 0)
    {
      char **slot = strcmp (prop, "font") == 0 ? &c->font : strcmp (prop, "fore") == 0 ? &c->fore : &c->back;
      g_free (*slot);
      *slot = g_strdup (value);
      restyle (f);
    }
  else if (strcmp (prop, "pages") == 0 && GTK_IS_NOTEBOOK (c->inner))
    {
      char **captions = g_strsplit (value, "\n", -1);
      for (int i = 0; captions[i] != NULL; i++)
        {
          GtkWidget *page = gtk_notebook_get_nth_page (GTK_NOTEBOOK (c->inner), i);
          if (page != NULL)
            gtk_notebook_set_tab_label_text (GTK_NOTEBOOK (c->inner), page, captions[i]);
        }
      g_strfreev (captions);
    }
  else if (strcmp (prop, "picture") == 0 && GTK_IS_PICTURE (c->inner) && *value != '\0')
    {
      /* The picture's bytes, as hex: whatever gdk-pixbuf reads. */
      gsize n = strlen (value) / 2;
      guchar *bytes = g_malloc (n);
      GdkPixbufLoader *loader = gdk_pixbuf_loader_new ();
      for (gsize i = 0; i < n; i++)
        bytes[i] = (guchar) ((g_ascii_xdigit_value (value[2 * i]) << 4) | g_ascii_xdigit_value (value[2 * i + 1]));
      if (gdk_pixbuf_loader_write (loader, bytes, n, NULL) && gdk_pixbuf_loader_close (loader, NULL))
        {
          GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
          if (pixbuf != NULL)
            {
              GdkTexture *texture = texture_for_pixbuf (pixbuf);
              gtk_picture_set_paintable (GTK_PICTURE (c->inner), GDK_PAINTABLE (texture));
              g_object_unref (texture);
            }
        }
      else
        gdk_pixbuf_loader_close (loader, NULL);
      g_object_unref (loader);
      g_free (bytes);
    }
  c->quiet = FALSE;
}

static void
uf_show (gpointer user, int form, gboolean modal)
{
  UForm *f = form_of (form);
  (void) user;

  if (f == NULL || f->closed)
    return;
  gtk_window_set_modal (GTK_WINDOW (f->window), modal);
  gtk_window_present (GTK_WINDOW (f->window));
  if (!modal)
    return;
  /* The macro waits for the form: no "macro running" window over it. */
  o42_window_running_wait (FALSE);
  f->loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (f->loop);
  g_clear_pointer (&f->loop, g_main_loop_unref);
  o42_window_running_wait (TRUE);
}

static void
uf_hide (gpointer user, int form)
{
  UForm *f = form_of (form);
  (void) user;

  if (f == NULL)
    return;
  gtk_widget_set_visible (f->window, FALSE);
  if (f->loop != NULL)
    g_main_loop_quit (f->loop);
}

static void
uf_close (gpointer user, int form)
{
  UForm *f = form_of (form);
  (void) user;

  if (f == NULL || f->closed)
    return;
  f->closed = TRUE;
  if (f->loop != NULL)
    g_main_loop_quit (f->loop);
  g_hash_table_remove (forms, GINT_TO_POINTER (form));
  g_signal_handlers_disconnect_by_data (f->window, f);
  gtk_window_destroy (GTK_WINDOW (f->window));
  f->window = NULL;
  /* A handler of its own may be what closed it: the rest goes once
   * that has returned. */
  g_idle_add (free_form_later, f);
}

void
o42_userform_install (GtkApplication *app)
{
  static const O42FormHost host = { NULL, uf_open, uf_add, uf_set, uf_show, uf_hide, uf_close };

  forms_app = app;
  o42_python_set_form_host (&host);
}
