/* main.c - office42, a spreadsheet in the shape of Excel 5
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "o42-application.h"

#include <glib/gi18n.h>
#include <locale.h>

/* Where the catalogues are.  An installed program finds them beside
 * itself -- ../share/locale from the binary -- which is what makes a
 * copied-around build on Windows still speak the language; a program
 * installed the usual way finds them where the build put them. */
static const char *
locale_dir (void)
{
  static char *dir;

  if (dir != NULL)
    return dir;

#ifdef G_OS_WIN32
  {
    /* On Windows a build is copied about whole, so the catalogues are
     * found beside the binary rather than where the build was told
     * they would be installed. */
    char *base = g_win32_get_package_installation_directory_of_module (NULL);

    if (base != NULL)
      {
        dir = g_build_filename (base, "share", "locale", NULL);
        g_free (base);
        if (g_file_test (dir, G_FILE_TEST_IS_DIR))
          return dir;
        g_clear_pointer (&dir, g_free);
      }
  }
#endif

  dir = g_strdup (O42_LOCALEDIR);
  return dir;
}

int
main (int argc, char *argv[])
{
  O42Application *app;
  int status;

  /* The message catalogues, if any were installed: the program speaks
   * the machine's language where it has been translated and English
   * everywhere else.  Numbers stay in the C locale -- a spreadsheet's
   * 1.5 is 1.5 on every machine -- which is what the model already
   * relies on, so only the messages follow setlocale. */
  setlocale (LC_ALL, "");
  setlocale (LC_NUMERIC, "C");
  bindtextdomain (GETTEXT_PACKAGE, locale_dir ());
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  app = o42_application_new ();
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);

  return status;
}
