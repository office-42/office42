/* o42-scale.c - see o42-scale.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-scale.h"

double
o42_text_scale (GdkDisplay *display)
{
  int dpi = -1;

  if (display == NULL)
    return 1.0;
  g_object_get (gtk_settings_get_for_display (display), "gtk-xft-dpi", &dpi, NULL);
  /* -1 is GTK's "not set", which it reads as 96. */
  if (dpi <= 0)
    return 1.0;
  return CLAMP (dpi / 1024.0 / 96.0, 1.0, 4.0);
}
