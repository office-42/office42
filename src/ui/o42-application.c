/* o42-application.c - see o42-application.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-application.h"

#include "o42-window.h"
#include "o42-types.h"

#include <stdlib.h>
#include <string.h>

struct _O42Application {
  GtkApplication parent_instance;

  char *screenshot;      /* --screenshot FILE: render the window and exit */
  char *activate;        /* --activate ACTION: fire a window action first */
  char *select;          /* --select B3: make a cell active first */
};

G_DEFINE_FINAL_TYPE (O42Application, o42_application, GTK_TYPE_APPLICATION)

/* Excel 5's shortcuts, as far as they still make sense. */
static const struct {
  const char *action;
  const char *accels[3];
} ACCELS[] = {
  { "win.new",        { "<Control>n", NULL } },
  { "win.open",       { "<Control>o", NULL } },
  { "win.save",       { "<Control>s", NULL } },
  { "win.save-as",    { "<Control><Shift>s", NULL } },
  { "win.print",      { "<Control>p", NULL } },
  { "win.close",      { "<Control>w", NULL } },
  { "win.undo",       { "<Control>z", NULL } },
  { "win.redo",       { "<Control>y", "<Control><Shift>z", NULL } },
  { "win.cut",        { "<Control>x", NULL } },
  { "win.copy",       { "<Control>c", NULL } },
  { "win.paste",      { "<Control>v", NULL } },
  { "win.paste-special", { "<Control><Shift>v", NULL } },
  { "win.select-all", { "<Control>a", NULL } },
  { "win.fill-down",  { "<Control>d", NULL } },
  { "win.fill-right", { "<Control>r", NULL } },
  { "win.bold",       { "<Control>b", NULL } },
  { "win.italic",     { "<Control>i", NULL } },
  { "win.underline",  { "<Control>u", NULL } },
  { "win.goto",       { "F5", NULL } },
  { "win.find",       { "<Control>f", NULL } },
  { "win.help-contents", { "F1", NULL } },
  { "win.define-name", { "<Control>F3", NULL } },
  { "win.insert-note", { "<Shift>F2", NULL } },
  { "win.format-cells", { "<Control>1", NULL } },
  { "win.insert-function", { "<Shift>F3", NULL } },
  { "win.next-sheet", { "<Control>Page_Down", NULL } },
  { "win.prev-sheet", { "<Control>Page_Up", NULL } },
  { "win.replace",    { "<Control>h", NULL } },
  { "win.calculate",  { "F9", NULL } },
  { "win.full-screen", { "F11", NULL } },
  { "app.quit",       { "<Control>q", NULL } },
};

/* Quit closes each window in turn, so that each modified book gets its
 * save prompt; the application ends when the last window has gone. */
static void
action_quit (GSimpleAction *action, GVariant *param, gpointer data)
{
  GList *windows = g_list_copy (gtk_application_get_windows (GTK_APPLICATION (data)));

  (void) action; (void) param;

  for (GList *l = windows; l != NULL; l = l->next)
    gtk_window_close (GTK_WINDOW (l->data));

  g_list_free (windows);
}

static const GActionEntry APP_ACTIONS[] = {
  { "quit", action_quit, NULL, NULL, NULL, { 0 } },
};

static void
load_css (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();
  GdkDisplay *display = gdk_display_get_default ();

  gtk_css_provider_load_from_resource (provider, "/net/office42/office42/style.css");

  if (display != NULL)
    gtk_style_context_add_provider_for_display (display,
                                                GTK_STYLE_PROVIDER (provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_object_unref (provider);
}

/* The toolbar icons travel in the binary, laid out as a small icon theme
 * -- scalable/actions inside the resource -- so GTK finds them by name off
 * this path.  They are not symbolic: each keeps the colours it was drawn
 * in, and style.css fades the ones on a disabled button. */
static void
load_icons (void)
{
  GdkDisplay *display = gdk_display_get_default ();

  if (display != NULL)
    gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (display),
                                      "/net/office42/office42/icons");
}

static void
o42_application_startup (GApplication *app)
{
  G_APPLICATION_CLASS (o42_application_parent_class)->startup (app);

  load_css ();
  load_icons ();

  for (guint i = 0; i < G_N_ELEMENTS (ACCELS); i++)
    gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                           ACCELS[i].action, ACCELS[i].accels);
}

/* --screenshot renders the window's widget tree through GSK into a PNG
 * and quits.  It goes through the render nodes rather than the screen, so
 * it works without a compositor, on a locked desktop, and in CI; it is
 * how the README's pictures are made and how a change to the grid can be
 * looked at without a person at the keyboard. */
static GskRenderNode *
render_window (GtkWidget *window, int *width, int *height)
{
  GdkPaintable *paintable;
  GtkSnapshot *snapshot;
  GskRenderNode *node;

  *width = gtk_widget_get_width (window);
  *height = gtk_widget_get_height (window);
  if (*width <= 0 || *height <= 0)
    {
      int min, nat;
      gtk_widget_measure (window, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
      *width = MAX (nat, 1);
      gtk_widget_measure (window, GTK_ORIENTATION_VERTICAL, *width, &min, &nat, NULL, NULL);
      *height = MAX (nat, 1);
    }

  paintable = gtk_widget_paintable_new (window);
  snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, snapshot, *width, *height);
  node = gtk_snapshot_free_to_node (snapshot);
  g_object_unref (paintable);

  return node;
}

/* Every toplevel, stacked top to bottom in one picture: the main window,
 * then whatever dialog --activate opened, so a dialog can be looked at
 * from a script as well. */
static void
render_all (O42Application *self)
{
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));
  GList *toplevels;
  GPtrArray *nodes = g_ptr_array_new ();
  GArray *sizes = g_array_new (FALSE, FALSE, sizeof (int) * 2);
  int total_w = 0, total_h = 0, y = 0;
  cairo_surface_t *surface;
  cairo_t *cr;

  if (windows == NULL || self->screenshot == NULL)
    {
      g_application_quit (G_APPLICATION (self));
      return;
    }

  /* The application's own windows first, then the dialogs they own, which
   * are toplevels but not GtkApplicationWindows. */
  toplevels = g_list_copy (windows);
  {
    GListModel *all = gtk_window_get_toplevels ();
    guint n = g_list_model_get_n_items (all);

    for (guint i = 0; i < n; i++)
      {
        GtkWindow *w = g_list_model_get_item (all, i);
        if (gtk_widget_get_visible (GTK_WIDGET (w)) && g_list_find (toplevels, w) == NULL)
          toplevels = g_list_append (toplevels, w);
        g_object_unref (w);
      }
  }

  for (GList *l = toplevels; l != NULL; l = l->next)
    {
      int size[2];
      GskRenderNode *node = render_window (GTK_WIDGET (l->data), &size[0], &size[1]);

      g_ptr_array_add (nodes, node);
      g_array_append_val (sizes, size);
      total_w = MAX (total_w, size[0]);
      total_h += size[1] + (l->next != NULL ? 8 : 0);
    }

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, total_w, total_h);
  cr = cairo_create (surface);
  cairo_set_source_rgb (cr, 0.4, 0.4, 0.4);
  cairo_paint (cr);

  for (guint i = 0; i < nodes->len; i++)
    {
      GskRenderNode *node = g_ptr_array_index (nodes, i);
      int *size = &g_array_index (sizes, int, i * 2);

      cairo_save (cr);
      cairo_translate (cr, 0, y);
      cairo_set_source_rgb (cr, 0.753, 0.753, 0.753);
      cairo_rectangle (cr, 0, 0, size[0], size[1]);
      cairo_fill (cr);
      if (node != NULL)
        {
          gsk_render_node_draw (node, cr);
          gsk_render_node_unref (node);
        }
      cairo_restore (cr);
      y += size[1] + 8;
    }
  cairo_destroy (cr);

  if (cairo_surface_write_to_png (surface, self->screenshot) != CAIRO_STATUS_SUCCESS)
    g_printerr ("office42: could not write %s\n", self->screenshot);

  cairo_surface_destroy (surface);
  g_ptr_array_free (nodes, TRUE);
  g_array_free (sizes, TRUE);
  g_list_free (toplevels);

  g_application_quit (G_APPLICATION (self));
}

/* Rendering happens once every toplevel's frame clock has painted, which
 * is the one moment each is certain to have been laid out; a snapshot
 * taken from a plain timeout can land between a resize and the layout
 * that answers it and come back empty.  Each window has its own clock, so
 * each is waited for. */
static int clocks_pending = 0;

static void
on_after_paint (GdkFrameClock *clock, gpointer data)
{
  O42Application *self = data;

  g_signal_handlers_disconnect_by_func (clock, on_after_paint, data);
  if (--clocks_pending <= 0)
    render_all (self);
}

static gboolean
take_screenshot (gpointer data)
{
  O42Application *self = data;
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));
  GListModel *all;
  guint n;

  if (windows == NULL || self->screenshot == NULL)
    {
      g_application_quit (G_APPLICATION (self));
      return G_SOURCE_REMOVE;
    }

  all = gtk_window_get_toplevels ();
  n = g_list_model_get_n_items (all);
  clocks_pending = 0;

  for (guint i = 0; i < n; i++)
    {
      GtkWindow *w = g_list_model_get_item (all, i);
      GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (w));

      if (gtk_widget_get_visible (GTK_WIDGET (w)) && clock != NULL)
        {
          clocks_pending++;
          g_signal_connect (clock, "after-paint", G_CALLBACK (on_after_paint), self);
          gtk_widget_queue_draw (GTK_WIDGET (w));
          gdk_frame_clock_request_phase (clock, GDK_FRAME_CLOCK_PHASE_PAINT);
        }
      g_object_unref (w);
    }

  if (clocks_pending == 0)
    render_all (self);

  return G_SOURCE_REMOVE;
}

static gboolean
fire_activate (gpointer data)
{
  O42Application *self = data;
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));

  /* --select B4 makes B4 active before the action; --select B4,A40 goes
   * on to A40 afterwards, which is how a scrolled view gets pictured. */
  if (windows != NULL && self->select != NULL)
    {
      int row, col;
      if (o42_ref_parse (self->select, &row, &col, NULL))
        o42_window_select_cell (O42_WINDOW (windows->data), row, col);
    }

  if (windows != NULL && self->activate != NULL)
    {
      /* "zoom(150)" carries an integer parameter and "shape(checkbox)" a
       * string one; a bare name has none. */
      char *name = g_strdup (self->activate);
      char *paren = strchr (name, '(');
      GVariant *param = NULL;

      if (paren != NULL)
        {
          char *end = strchr (paren, ')');
          char *inside;

          *paren = '\0';
          inside = g_strdup (paren + 1);
          end = strchr (inside, ')');
          if (end != NULL)
            *end = '\0';
          if (inside[0] != '\0' && strspn (inside, "-0123456789") == strlen (inside))
            param = g_variant_new_int32 (atoi (inside));
          else
            param = g_variant_new_string (inside);
          g_free (inside);
        }

      o42_window_set_dialogs_modal (FALSE);
      g_action_group_activate_action (G_ACTION_GROUP (windows->data), name, param);
      g_free (name);
    }

  if (windows != NULL && self->select != NULL && strchr (self->select, ',') != NULL)
    {
      int row, col;
      if (o42_ref_parse (strchr (self->select, ',') + 1, &row, &col, NULL))
        o42_window_select_cell (O42_WINDOW (windows->data), row, col);
    }

  return G_SOURCE_REMOVE;
}

static void
arm_screenshot (O42Application *self)
{
  /* A second is long enough for the window to be mapped and laid out,
   * and half of one for a dialog to follow. */
  if (self->activate != NULL || self->select != NULL)
    g_timeout_add (500, fire_activate, self);
  if (self->screenshot != NULL)
    g_timeout_add (1000, take_screenshot, self);
}

/* The splash: the logo in a small undecorated window over the first
 * window, gone after six tenths of a second, as Excel 5 did it.  Not
 * in screenshot mode, whose picture would be of the splash. */
static gboolean
splash_done (gpointer data)
{
  gtk_window_destroy (GTK_WINDOW (data));
  return G_SOURCE_REMOVE;
}

/* Presented once the window under it is mapped, so the toolkit can
 * centre it over that window rather than wherever the pointer was. */
static gboolean
splash_present (gpointer data)
{
  GtkWindow *splash = data;
  gtk_window_present (splash);
  g_timeout_add (600, splash_done, splash);
  return G_SOURCE_REMOVE;
}

static void
splash_show (O42Application *self, GtkWindow *over)
{
  GtkWidget *splash, *picture, *frame;

  if (self->screenshot != NULL)
    return;
  splash = gtk_window_new ();
  gtk_window_set_decorated (GTK_WINDOW (splash), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (splash), FALSE);
  gtk_window_set_transient_for (GTK_WINDOW (splash), over);
  gtk_window_set_modal (GTK_WINDOW (splash), TRUE);
  gtk_window_set_title (GTK_WINDOW (splash), "Office42 Spreadsheet");
  picture = gtk_picture_new_for_resource ("/net/office42/office42/logo.svg");
  gtk_picture_set_can_shrink (GTK_PICTURE (picture), FALSE);
  gtk_widget_set_size_request (picture, 520, 150);
  frame = gtk_frame_new (NULL);
  gtk_frame_set_child (GTK_FRAME (frame), picture);
  gtk_window_set_child (GTK_WINDOW (splash), frame);
  g_idle_add_full (G_PRIORITY_LOW, splash_present, splash, NULL);
}

static void
o42_application_activate (GApplication *app)
{
  GtkWidget *window = o42_window_new (GTK_APPLICATION (app));

  gtk_window_present (GTK_WINDOW (window));
  splash_show (O42_APPLICATION (app), GTK_WINDOW (window));
  arm_screenshot (O42_APPLICATION (app));
}

static void
o42_application_open (GApplication *app, GFile **files, int n_files,
                      const char *hint)
{
  (void) hint;

  for (int i = 0; i < n_files; i++)
    {
      GtkWidget *window = o42_window_new (GTK_APPLICATION (app));

      o42_window_open_file (O42_WINDOW (window), files[i]);
      gtk_window_present (GTK_WINDOW (window));
      if (i == 0)
        splash_show (O42_APPLICATION (app), GTK_WINDOW (window));
    }

  arm_screenshot (O42_APPLICATION (app));
}

static int
o42_application_handle_local_options (GApplication *app, GVariantDict *options)
{
  O42Application *self = O42_APPLICATION (app);
  const char *path = NULL;

  if (g_variant_dict_lookup (options, "screenshot", "^&ay", &path))
    {
      g_free (self->screenshot);
      self->screenshot = g_strdup (path);

      /* The screenshot is of this process's window, so it must not hand
       * its files to an instance already running. */
      g_application_set_flags (app, g_application_get_flags (app) |
                                    G_APPLICATION_NON_UNIQUE);
    }

  if (g_variant_dict_lookup (options, "activate", "&s", &path))
    {
      g_free (self->activate);
      self->activate = g_strdup (path);
    }

  if (g_variant_dict_lookup (options, "select", "&s", &path))
    {
      g_free (self->select);
      self->select = g_strdup (path);
    }

  return -1;
}

static void
o42_application_finalize (GObject *object)
{
  g_free (O42_APPLICATION (object)->screenshot);
  g_free (O42_APPLICATION (object)->activate);
  g_free (O42_APPLICATION (object)->select);
  G_OBJECT_CLASS (o42_application_parent_class)->finalize (object);
}

static void
o42_application_class_init (O42ApplicationClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = o42_application_finalize;

  app_class->startup = o42_application_startup;
  app_class->activate = o42_application_activate;
  app_class->open = o42_application_open;
  app_class->handle_local_options = o42_application_handle_local_options;
}

static void
o42_application_init (O42Application *self)
{
  g_action_map_add_action_entries (G_ACTION_MAP (self), APP_ACTIONS,
                                   G_N_ELEMENTS (APP_ACTIONS), self);

  g_application_add_main_option (G_APPLICATION (self), "screenshot", 0,
                                 G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME,
                                 "Render the window to a PNG and exit", "FILE");
  g_application_add_main_option (G_APPLICATION (self), "activate", 0,
                                 G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
                                 "Fire a window action (e.g. format-cells) before the screenshot", "ACTION");
  g_application_add_main_option (G_APPLICATION (self), "select", 0,
                                 G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
                                 "Make a cell active (e.g. B3) before the screenshot", "CELL");
}

O42Application *
o42_application_new (void)
{
  return g_object_new (O42_TYPE_APPLICATION,
                       "application-id", "net.office42.office42",
                       "flags", G_APPLICATION_HANDLES_OPEN,
                       NULL);
}
