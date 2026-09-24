/* o42-file.c - see o42-file.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-file.h"

#include <string.h>

static const struct {
  const char   *ending;
  O42FileFormat format;
} ENDINGS[] = {
  { ".gnumeric", O42_FILE_GNUMERIC },
  { ".xlsx", O42_FILE_XLSX }, { ".xlsm", O42_FILE_XLSX },
  { ".xls", O42_FILE_XLS },
  { ".ods", O42_FILE_ODS }, { ".fods", O42_FILE_ODS },
  { ".html", O42_FILE_HTML }, { ".htm", O42_FILE_HTML },
  { ".csv", O42_FILE_CSV }, { ".txt", O42_FILE_CSV },
  { ".tsv", O42_FILE_CSV }, { ".tab", O42_FILE_CSV },
  { ".dif", O42_FILE_DIF },
  { ".slk", O42_FILE_SYLK }, { ".sylk", O42_FILE_SYLK },
  { ".tex", O42_FILE_LATEX },
  { ".wk1", O42_FILE_LOTUS }, { ".wks", O42_FILE_LOTUS },
};

O42FileFormat
o42_file_format_for_name (const char *name)
{
  char *folded;
  O42FileFormat format = O42_FILE_GNUMERIC;

  if (name == NULL)
    return format;
  folded = g_ascii_strdown (name, -1);
  for (guint i = 0; i < G_N_ELEMENTS (ENDINGS); i++)
    if (g_str_has_suffix (folded, ENDINGS[i].ending))
      {
        format = ENDINGS[i].format;
        break;
      }
  g_free (folded);
  return format;
}

gboolean
o42_file_name_has_format (const char *name)
{
  char *folded;
  gboolean known = FALSE;

  if (name == NULL)
    return FALSE;
  folded = g_ascii_strdown (name, -1);
  for (guint i = 0; i < G_N_ELEMENTS (ENDINGS) && !known; i++)
    known = g_str_has_suffix (folded, ENDINGS[i].ending);
  g_free (folded);
  return known;
}

O42FileFormat
o42_file_format (GFile *file)
{
  char *name = g_file_get_basename (file);
  O42FileFormat format = o42_file_format_for_name (name);

  g_free (name);
  return format;
}

/* Whether `needle` is in the first `length` bytes, ignoring case. */
static gboolean
head_has (const char *head, gsize length, const char *needle)
{
  gsize n = strlen (needle);

  for (gsize i = 0; i + n <= length; i++)
    if (g_ascii_strncasecmp (head + i, needle, n) == 0)
      return TRUE;
  return FALSE;
}

O42FileFormat
o42_file_format_to_read (GFile *file)
{
  O42FileFormat named = o42_file_format (file);
  GFileInputStream *in = g_file_read (file, NULL, NULL);
  char head[4096];
  gsize length = 0;
  const char *text;

  if (in == NULL)
    return named;
  g_input_stream_read_all (G_INPUT_STREAM (in), head, sizeof head, &length, NULL, NULL);
  g_object_unref (in);
  if (length < 4)
    return named;

  /* The containers say what they are in their first bytes.  An
   * OpenDocument zip starts with its mimetype, stored; any other zip a
   * spreadsheet comes in is Excel's. */
  if (memcmp (head, "PK\003\004", 4) == 0)
    return head_has (head, MIN (length, 200), "application/vnd.oasis.opendocument")
           ? O42_FILE_ODS : O42_FILE_XLSX;
  if (length >= 8 && memcmp (head, "\320\317\021\340\241\261\032\341", 8) == 0)
    return O42_FILE_XLS;
  if ((guchar) head[0] == 0x1f && (guchar) head[1] == 0x8b)
    return O42_FILE_GNUMERIC;
  /* 1-2-3's BOF record and the version of a .wk1 or a .wks. */
  if (length >= 6 && head[0] == 0 && head[1] == 0 && head[2] == 2 && head[3] == 0 &&
      (guchar) head[5] == 0x04 && ((guchar) head[4] == 0x04 || (guchar) head[4] == 0x06))
    return O42_FILE_LOTUS;

  /* The text formats, from the first thing in them that is not a
   * byte-order mark or white space. */
  text = head;
  if (length >= 3 && memcmp (text, "\357\273\277", 3) == 0)
    text += 3;
  while (text < head + length && g_ascii_isspace (*text))
    text++;
  length -= (gsize) (text - head);

  if (length >= 4 && strncmp (text, "ID;P", 4) == 0)
    return O42_FILE_SYLK;
  if (length >= 8 && strncmp (text, "TABLE", 5) == 0 && (text[5] == '\r' || text[5] == '\n'))
    return O42_FILE_DIF;
  if (text[0] == '<')
    {
      gboolean xml = length >= 5 && strncmp (text, "<?xml", 5) == 0;

      if (head_has (text, length, "<office:document"))
        return O42_FILE_ODS;
      if (head_has (text, length, "<gnm:Workbook"))
        return O42_FILE_GNUMERIC;
      /* Other XML -- Excel 2003's, say -- has tables that are not
       * HTML's; XHTML says it is html. */
      if (head_has (text, length, "<html") ||
          (!xml && (head_has (text, length, "<!doctype html") || head_has (text, length, "<table"))))
        return O42_FILE_HTML;
    }
  return named;
}

gboolean
o42_file_format_is_book (O42FileFormat format)
{
  switch (format)
    {
    case O42_FILE_GNUMERIC:
    case O42_FILE_XLSX:
    case O42_FILE_XLS:
    case O42_FILE_ODS:
      return TRUE;
    case O42_FILE_HTML:
    case O42_FILE_CSV:
    case O42_FILE_DIF:
    case O42_FILE_SYLK:
    case O42_FILE_LATEX:
    case O42_FILE_LOTUS:
      break;
    }
  return FALSE;
}

void
o42_file_abandon (GOutputStream *stream)
{
  /* GLib writes a replacement beside the file and renames it over the
   * file when the stream is closed -- even after a write failed, unless
   * the close is cancelled, which removes it instead. */
  GCancellable *abandon = g_cancellable_new ();

  g_cancellable_cancel (abandon);
  g_output_stream_close (stream, abandon, NULL);
  g_object_unref (abandon);
}

gboolean
o42_file_replace (GFile *file, const void *data, gsize length, GError **error)
{
  GFileOutputStream *out;

  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  out = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
  if (out == NULL)
    return FALSE;
  if (!g_output_stream_write_all (G_OUTPUT_STREAM (out), data, length, NULL, NULL, error))
    {
      o42_file_abandon (G_OUTPUT_STREAM (out));
      g_object_unref (out);
      return FALSE;
    }
  if (!g_output_stream_close (G_OUTPUT_STREAM (out), NULL, error))
    {
      g_object_unref (out);
      return FALSE;
    }
  g_object_unref (out);
  return TRUE;
}
