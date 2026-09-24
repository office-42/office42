/* o42-lotus.c - see o42-lotus.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-lotus.h"
#include "o42-file.h"
#include "o42-csv.h"
#include "o42-entry.h"

#include <math.h>
#include <string.h>

/* The records a worksheet is made of.  Only the cells matter here; the
 * rest are read past. */
enum {
  L_BOF     = 0x0000,
  L_EOF     = 0x0001,
  L_RANGE   = 0x0006,
  L_BLANK   = 0x000C,
  L_INTEGER = 0x000D,
  L_NUMBER  = 0x000E,
  L_LABEL   = 0x000F,
  L_FORMULA = 0x0010
};

/* 1-2-3 release 2, which is what .wk1 means and what every reader
 * since has been able to open. */
#define WK1_VERSION 0x0406

/* Lotus counts columns to 256 and rows to 8192. */
#define WK1_MAX_COLS 256
#define WK1_MAX_ROWS 8192

static void
put16 (GByteArray *out, guint value)
{
  guint8 pair[2] = { value & 0xFF, (value >> 8) & 0xFF };

  g_byte_array_append (out, pair, 2);
}

static void
put_record (GByteArray *out, guint opcode, const guint8 *body, gsize length)
{
  put16 (out, opcode);
  put16 (out, length);
  if (length > 0)
    g_byte_array_append (out, body, length);
}

static guint
rd16 (const guint8 *p)
{
  return p[0] | (p[1] << 8);
}

gboolean
o42_lotus_save (O42Sheet *sheet, GFile *file, GError **error)
{
  GByteArray *out = g_byte_array_new ();
  O42Range used;
  gboolean ok;

  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  o42_sheet_used_range (sheet, &used);
  {
    guint8 version[2] = { WK1_VERSION & 0xFF, WK1_VERSION >> 8 };

    put_record (out, L_BOF, version, 2);
  }

  /* The range the sheet uses, which is what a reader sizes itself
   * from: two corners of column, row, column, row. */
  {
    GByteArray *body = g_byte_array_new ();
    int last_col = MIN (used.col1, WK1_MAX_COLS - 1);
    int last_row = MIN (used.row1, WK1_MAX_ROWS - 1);

    put16 (body, 0); put16 (body, 0);
    put16 (body, last_col); put16 (body, last_row);
    put_record (out, L_RANGE, body->data, body->len);
    g_byte_array_free (body, TRUE);
  }

  for (int row = 0; row <= used.row1 && row < WK1_MAX_ROWS; row++)
    for (int col = 0; col <= used.col1 && col < WK1_MAX_COLS; col++)
      {
        O42Value value;
        GByteArray *body;

        o42_sheet_get_value (sheet, row, col, &value);
        if (value.type == O42_VALUE_EMPTY)
          { o42_value_clear (&value); continue; }

        body = g_byte_array_new ();
        g_byte_array_append (body, (const guint8 *) "\377", 1);   /* the default format */
        put16 (body, col);
        put16 (body, row);

        if (value.type == O42_VALUE_NUMBER &&
            value.as.number == floor (value.as.number) &&
            value.as.number >= -32768 && value.as.number <= 32767)
          {
            put16 (body, (guint) (gint16) value.as.number);
            put_record (out, L_INTEGER, body->data, body->len);
          }
        else if (value.type == O42_VALUE_NUMBER)
          {
            /* A double, little-endian, which is what the file wants
             * and what every machine this runs on writes. */
            double d = value.as.number;
            guint8 bytes[8];

            memcpy (bytes, &d, 8);
            g_byte_array_append (body, bytes, 8);
            put_record (out, L_NUMBER, body->data, body->len);
          }
        else
          {
            char *shown = o42_sheet_get_display (sheet, row, col);
            /* 1-2-3's labels are in the PC's code page, 437, which is
             * how LibreOffice and Gnumeric read them; a label that code
             * page cannot hold stays UTF-8, which the reader here tries
             * first and so gets back whole. */
            gsize n = 0;
            char *pc = g_convert (shown, -1, "IBM437", "UTF-8", NULL, &n, NULL);
            const char *label = pc != NULL ? pc : shown;
            gsize length = pc != NULL ? n : strlen (shown);

            /* A record's length is sixteen bits: the label is cut, at a
             * character, where the record would pass it. */
            if (length > 65535 - 7)
              {
                length = 65535 - 7;
                if (pc == NULL)
                  length = (gsize) (g_utf8_find_prev_char (label, label + length + 1) - label);
              }
            /* A label begins with the character that says how it is
             * lined up: an apostrophe is to the left. */
            g_byte_array_append (body, (const guint8 *) "'", 1);
            g_byte_array_append (body, (const guint8 *) label, length);
            g_byte_array_append (body, (const guint8 *) "", 1);
            g_free (pc);
            put_record (out, L_LABEL, body->data, body->len);
            g_free (shown);
          }
        g_byte_array_free (body, TRUE);
        o42_value_clear (&value);
      }

  put_record (out, L_EOF, NULL, 0);
  ok = o42_file_replace (file, (const char *) out->data, out->len, error);
  g_byte_array_free (out, TRUE);
  return ok;
}

gboolean
o42_lotus_load (O42Sheet *sheet, GFile *file, GError **error)
{
  char *contents = NULL;
  gsize length = 0;
  const guint8 *p;
  gsize at = 0;

  g_return_val_if_fail (sheet != NULL, FALSE);
  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return FALSE;
  p = (const guint8 *) contents;

  if (length < 6 || rd16 (p) != L_BOF)
    {
      g_free (contents);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "This is not a Lotus 1-2-3 worksheet.");
      return FALSE;
    }

  o42_sheet_begin_group (sheet);
  {
    O42Range all = { 0, 0, O42_MAX_ROWS - 1, O42_MAX_COLS - 1 };

    o42_sheet_clear_range (sheet, &all);
  }

  while (at + 4 <= length)
    {
      guint opcode = rd16 (p + at);
      guint size = rd16 (p + at + 2);
      const guint8 *body = p + at + 4;
      int col, row;

      if (at + 4 + size > length)
        break;
      at += 4 + size;

      if (opcode == L_EOF)
        break;
      if (opcode != L_BLANK && opcode != L_INTEGER && opcode != L_NUMBER &&
          opcode != L_LABEL && opcode != L_FORMULA)
        continue;
      if (size < 5)
        continue;

      col = (int) rd16 (body + 1);
      row = (int) rd16 (body + 3);
      if (col >= O42_MAX_COLS || row >= O42_MAX_ROWS)
        continue;

      switch (opcode)
        {
        case L_INTEGER:
          if (size >= 7)
            {
              char text[32];

              g_snprintf (text, sizeof text, "%d", (int) (gint16) rd16 (body + 5));
              o42_sheet_set_input (sheet, row, col, text);
            }
          break;

        case L_NUMBER:
        case L_FORMULA:
          if (size >= 13)
            {
              double d;
              char buffer[G_ASCII_DTOSTR_BUF_SIZE];

              memcpy (&d, body + 5, 8);
              /* A formula's own tokens follow the value it worked out;
               * the value is what the file says the cell holds. */
              o42_sheet_set_input (sheet, row, col,
                                   g_ascii_dtostr (buffer, sizeof buffer, d));
            }
          break;

        case L_LABEL:
          if (size >= 6)
            {
              /* The first character says how the label is lined up. */
              const char *start = (const char *) body + 5;
              gsize n = size - 5;
              char *text;

              if (*start == '\'' || *start == '"' || *start == '^' || *start == '\\')
                { start++; n--; }
              while (n > 0 && start[n - 1] == '\0')
                n--;
              /* A Lotus label is in the PC's code page, 437, unless it
               * is UTF-8 -- as this program writes one that 437 cannot
               * hold -- and is a text even when it looks like a number. */
              if (g_utf8_validate (start, (gssize) n, NULL))
                text = g_strndup (start, n);
              else
                {
                  text = g_convert (start, (gssize) n, "UTF-8", "IBM437", NULL, NULL, NULL);
                  if (text == NULL)
                    text = o42_text_to_utf8 (start, n);
                }
              {
                char *quoted = o42_entry_quote_text (text);

                o42_sheet_set_input (sheet, row, col, quoted);
                g_free (quoted);
              }
              g_free (text);
            }
          break;

        default:
          break;
        }
    }

  o42_sheet_end_group (sheet);
  g_free (contents);
  return TRUE;
}
