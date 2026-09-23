/* o42-scan.h - a picture from a scanner or a camera
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Excel's Insert > Picture > From Scanner or Camera.  On Windows the
 * system's own acquire dialog does the work, through Windows Image
 * Acquisition; elsewhere SANE's scanimage is run, which reaches every
 * scanner Linux knows.  Either way the picture arrives as a file's
 * worth of bytes, ready for o42_grid_insert_picture.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Whether this build can ask a scanner at all: always on Windows, and
 * where scanimage is on the path elsewhere. */
gboolean o42_scan_available (void);

/* Runs the acquire dialog (or the scan) and gives the picture's bytes,
 * its format ("png", "jpeg", "bmp") and its size in pixels.  NULL with
 * no error when the user cancelled; NULL with an error when nothing
 * could be acquired. */
GBytes *o42_scan_acquire (char **format, int *width, int *height, GError **error);

G_END_DECLS
