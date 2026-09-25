/* o42-cursor.c - see o42-cursor.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-cursor.h"

#include <string.h>

/* Each row is one shape: the name wanted first, then the names to try
 * when the theme has nothing for it.  The middle names are the ones
 * X11 themes have carried since before CSS named any of this; the last
 * is always "default", which no theme is without. */
static const char *const SHAPES[][4] = {
  { "cell",        "crosshair",     "cross",                 "default" },
  { "crosshair",   "cross",         "default",               NULL      },
  { "copy",        "dnd-copy",      "default",               NULL      },
  { "move",        "fleur",         "default",               NULL      },
  { "col-resize",  "ew-resize",     "sb_h_double_arrow",     "default" },
  { "row-resize",  "ns-resize",     "sb_v_double_arrow",     "default" },
  { "n-resize",    "ns-resize",     "top_side",              "default" },
  { "s-resize",    "ns-resize",     "bottom_side",           "default" },
  { "e-resize",    "ew-resize",     "right_side",            "default" },
  { "w-resize",    "ew-resize",     "left_side",             "default" },
  { "nw-resize",   "nwse-resize",   "top_left_corner",       "default" },
  { "se-resize",   "nwse-resize",   "bottom_right_corner",   "default" },
  { "ne-resize",   "nesw-resize",   "top_right_corner",      "default" },
  { "sw-resize",   "nesw-resize",   "bottom_left_corner",    "default" },
  { "text",        "xterm",         "default",               NULL      },
  { "pointer",     "hand2",         "default",               NULL      },
};

/* GDK on Windows knows the Windows names, the X11 ones and most of the
 * CSS ones, but not these.  And a name it does not know gives it a
 * cursor with no image, not a reason to try the fallback: the pointer
 * vanishes over the sheet.  So on Windows a chain starts at the first
 * name GDK has there. */
static gboolean
usable (const char *name)
{
#ifdef G_OS_WIN32
  static const char *const MISSING[] = { "cell", "copy", "dnd-copy" };

  for (guint i = 0; i < G_N_ELEMENTS (MISSING); i++)
    if (strcmp (MISSING[i], name) == 0)
      return FALSE;
#else
  (void) name;
#endif
  return TRUE;
}

/* The cursor for a name, made on the first ask and kept afterwards:
 * the shape is set again on every mouse move, and building a chain of
 * GdkCursors each time would be work for nothing. */
static GdkCursor *
cursor_for (const char *name)
{
  static GHashTable *cache;
  const char *const *shape = NULL;
  GdkCursor *cursor = NULL;
  int last = 0;

  if (cache == NULL)
    cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  cursor = g_hash_table_lookup (cache, name);
  if (cursor != NULL)
    return cursor;

  for (guint i = 0; i < G_N_ELEMENTS (SHAPES); i++)
    if (strcmp (SHAPES[i][0], name) == 0)
      {
        shape = SHAPES[i];
        break;
      }

  /* A shape with no row of its own still falls back to "default". */
  if (shape == NULL)
    {
      cursor = gdk_cursor_new_from_name ("default", NULL);
      if (cursor != NULL && usable (name))
        {
          GdkCursor *wanted = gdk_cursor_new_from_name (name, cursor);

          if (wanted != NULL)
            {
              g_object_unref (cursor);
              cursor = wanted;
            }
        }
    }
  else
    {
      /* Built from the last name back, each one the fallback of the
       * one before it. */
      while (last + 1 < (int) G_N_ELEMENTS (SHAPES[0]) && shape[last + 1] != NULL)
        last++;
      for (int i = last; i >= 0; i--)
        {
          GdkCursor *step;

          if (!usable (shape[i]))
            continue;
          step = gdk_cursor_new_from_name (shape[i], cursor);

          if (step != NULL)
            {
              g_clear_object (&cursor);
              cursor = step;
            }
        }
    }

  if (cursor == NULL)
    return NULL;

  g_hash_table_insert (cache, g_strdup (name), cursor);
  return cursor;
}

void
o42_set_cursor_name (GtkWidget *widget, const char *name)
{
  GdkCursor *cursor;

  g_return_if_fail (GTK_IS_WIDGET (widget));

  if (name == NULL)
    {
      gtk_widget_set_cursor (widget, NULL);
      return;
    }

  cursor = cursor_for (name);
  /* Setting the same cursor again would have the pointer redrawn for
   * nothing, and the mouse moves a great deal over a sheet. */
  if (cursor != NULL && gtk_widget_get_cursor (widget) != cursor)
    gtk_widget_set_cursor (widget, cursor);
}
