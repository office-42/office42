/* o42-dialogs-file.c - see o42-window-private.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The File menu's dialogs for text files.  Text Import, which every
 * .csv, .txt and .prn is opened through and which File > Import Text
 * File brings into the sheet on show: what Excel 97's Text Import
 * Wizard asked on three pages -- what kind of file it is, where its
 * fields divide, what each column is to be taken as -- on one, with the
 * file cut up underneath as each answer changes it.  And the questions
 * Save As asks before it writes one: what goes between the fields, what
 * the text is in, and whether the cells are written as they show or as
 * they are.
 */

#include "o42-window-private.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

#define dialog_button o42_dialog_button
#define dialog_frame o42_dialog_frame
#define on_dialog_close_clicked o42_dialog_close_clicked
#define on_dialog_destroy_refocus o42_dialog_destroy_refocus
#define show_error o42_window_show_error
#define window_sync o42_window_sync

/* The encodings a text file is likely to be in: iconv's name for each,
 * and what a person calls it.  Text Import's "File origin" and Save
 * As's "Encoding" both list them. */
static const char *const ENCODINGS[][2] = {
  { "UTF-8",        N_("Unicode (UTF-8)") },
  { "UTF-16LE",     N_("Unicode (UTF-16, little-endian)") },
  { "UTF-16BE",     N_("Unicode (UTF-16, big-endian)") },
  { "WINDOWS-1252", N_("Windows (ANSI), Western European") },
  { "WINDOWS-1250", N_("Windows, Central European") },
  { "WINDOWS-1251", N_("Windows, Cyrillic") },
  { "WINDOWS-1253", N_("Windows, Greek") },
  { "WINDOWS-1254", N_("Windows, Turkish") },
  { "WINDOWS-1255", N_("Windows, Hebrew") },
  { "WINDOWS-1256", N_("Windows, Arabic") },
  { "WINDOWS-1257", N_("Windows, Baltic") },
  { "WINDOWS-1258", N_("Windows, Vietnamese") },
  { "CP437",        N_("DOS or OS/2 (PC-8)") },
  { "CP850",        N_("DOS, Western European (PC-850)") },
  { "MACINTOSH",    N_("Macintosh") },
  { "ISO-8859-1",   N_("ISO 8859-1, Western European") },
  { "ISO-8859-15",  N_("ISO 8859-15, Western European with the euro") },
  { "ISO-8859-2",   N_("ISO 8859-2, Central European") },
  { "KOI8-R",       N_("KOI8-R, Russian") },
  { "SHIFT_JIS",    N_("Japanese (Shift JIS)") },
  { "EUC-JP",       N_("Japanese (EUC-JP)") },
  { "GB18030",      N_("Chinese, Simplified (GB 18030)") },
  { "BIG5",         N_("Chinese, Traditional (Big5)") },
  { "EUC-KR",       N_("Korean (EUC-KR)") },
};

/* A drop-down of the encodings, with `current` chosen; one this list
 * does not have -- the machine's own code page, found by the guess --
 * is put at the end under its own name.  `names` gets iconv's name for
 * each entry, in order. */
static GtkWidget *
encoding_drop (const char *current, GPtrArray *names)
{
  GtkStringList *labels = gtk_string_list_new (NULL);
  guint chosen = 0;
  GtkWidget *drop;

  for (guint i = 0; i < G_N_ELEMENTS (ENCODINGS); i++)
    {
      gtk_string_list_append (labels, _(ENCODINGS[i][1]));
      g_ptr_array_add (names, (gpointer) ENCODINGS[i][0]);
      if (current != NULL && g_ascii_strcasecmp (current, ENCODINGS[i][0]) == 0)
        chosen = i;
    }
  if (current != NULL && chosen == 0 && g_ascii_strcasecmp (current, "UTF-8") != 0)
    {
      gtk_string_list_append (labels, current);
      g_ptr_array_add (names, (gpointer) g_intern_string (current));
      chosen = names->len - 1;
    }
  drop = gtk_drop_down_new (G_LIST_MODEL (labels), NULL);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (drop), chosen);
  return drop;
}

/* A label that wraps, flush left, for what a dialog has to say before
 * its controls. */
static GtkWidget *
prose (const char *text)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 70);
  return label;
}

static GtkWidget *
framed (const char *title, GtkWidget *child)
{
  GtkWidget *frame = gtk_frame_new (title);

  gtk_widget_set_margin_top (child, 6);
  gtk_widget_set_margin_bottom (child, 6);
  gtk_widget_set_margin_start (child, 8);
  gtk_widget_set_margin_end (child, 8);
  gtk_frame_set_child (GTK_FRAME (frame), child);
  return frame;
}

/* ---- File > Open and Import Text File: Text Import --------------------- */

/* One page, in the order LibreOffice asks and in Excel's words: where
 * the file comes from and the row to start at; how its fields divide;
 * how it writes its numbers, and what its quotes and formulas mean; and
 * under it all the file cut up as the answers say, the column type set
 * for the columns chosen in it.  The preview numbers the file's lines
 * as a sheet numbers its rows, a click chooses a column, Shift+click
 * several and the corner all of them, and the arrow keys do the same.
 * A fixed-width file has a ruler over the preview where a click puts
 * in a break, a drag moves one and a double-click takes one away. */

#define PREVIEW_RECORDS 200    /* the records the preview shows, from the first row */
#define PREVIEW_MAX_CHARS 40   /* a column of the preview is no wider than this */
#define LINE_MAX_CHARS 400     /* nor a line of the file */
#define CELL_PAD 4.0
#define RULER_H 18.0

typedef struct {
  O42Window     *window;
  GFile         *file;
  gboolean       open;          /* File > Open, else Import Text File */
  char          *bytes;         /* the file as it is on disk */
  gsize          length;
  char          *text;          /* the same, read in the chosen encoding */
  O42CsvOptions *options;
  GPtrArray     *encodings;     /* iconv's names, in the drop-down's order */
  int            n_lines;

  GtkWidget *dialog;
  GtkWidget *origin, *start;
  GtkWidget *delimited, *fixed, *cut_box, *cut_more;
  GtkWidget *sep[4], *other_check, *other;
  GtkWidget *merge, *trim, *quote;
  GtkWidget *decimal, *thousands, *minus, *quoted_text, *formulas;
  GtkWidget *place, *new_sheet, *skip_empty;
  GtkWidget *type_drop, *hint;
  GtkWidget *scroller, *area;

  GPtrArray *lines;             /* the file's lines from the first row, as they are */
  GPtrArray *rows;              /* the records cut into fields */
  GArray    *first_lines;       /* int: the line of the file each record starts on */
  GArray    *widths;            /* int: the characters each table column is wide */
  int        longest;           /* characters in the longest line shown */
  int        anchor, cursor;    /* the columns chosen are those between, inclusive */
  int        dragging;          /* the break being dragged, or -1 */
  double     char_w, line_h, gutter;
  gboolean   updating;
  gboolean   breaks_touched;    /* the breaks are the user's now, not a guess */
} ImportPrompt;

static const char *const DATE_ORDERS[] = { "MDY", "DMY", "YMD", "MYD", "DYM", "YDM", NULL };

/* The Column type drop-down: General, Text, a Date for each order, and
 * Skip. */
enum { TYPE_GENERAL = 0, TYPE_TEXT = 1, TYPE_DATE = 2, TYPE_SKIP = 8 };

static guint
type_index (const O42CsvColumn *column)
{
  switch (column->type)
    {
    case O42_SPLIT_TEXT: return TYPE_TEXT;
    case O42_SPLIT_DATE: return TYPE_DATE + CLAMP ((guint) column->order, 0, 5);
    case O42_SPLIT_SKIP: return TYPE_SKIP;
    default:             return TYPE_GENERAL;
    }
}

static int
sel_from (ImportPrompt *prompt)
{
  return MIN (prompt->anchor, prompt->cursor);
}

static int
sel_to (ImportPrompt *prompt)
{
  return MAX (prompt->anchor, prompt->cursor);
}

/* The preview's font: every character the same width, so that a
 * fixed-width file's columns line up and a break sits between two. */
static PangoLayout *
preview_layout (GtkWidget *area, ImportPrompt *prompt)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (area, NULL);
  PangoFontDescription *desc = pango_font_description_from_string ("Monospace 10");
  int w, h;

  pango_layout_set_font_description (layout, desc);
  pango_font_description_free (desc);
  pango_layout_set_text (layout, "0", -1);
  pango_layout_get_pixel_size (layout, &w, &h);
  prompt->char_w = MAX (w, 1);
  prompt->line_h = h + 2;
  return layout;
}

/* A field as the preview shows it: on one line, tabs and line breaks
 * made visible, and cut short with an ellipsis. */
static char *
shown (const char *text, int max_chars)
{
  GString *out = g_string_new (NULL);
  int n = 0;

  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (n >= max_chars)
        {
          g_string_append (out, "\342\200\246");
          break;
        }
      if (c == '\t')
        g_string_append (out, "\342\206\222");
      else if (c == '\n' || c == '\r')
        g_string_append (out, "\342\206\265");
      else
        g_string_append_unichar (out, c);
      n++;
    }
  return g_string_free (out, FALSE);
}

static int
column_count (ImportPrompt *prompt)
{
  return prompt->options->fixed_width ? (int) prompt->options->breaks->len + 1
                                      : (int) prompt->widths->len;
}

/* The top of the records: under the column types, and on a fixed-width
 * file under the ruler as well. */
static double
body_top (ImportPrompt *prompt)
{
  return (prompt->options->fixed_width ? RULER_H : 0) + prompt->line_h + 2;
}

/* Where column `col` of the table begins, in pixels. */
static double
column_x (ImportPrompt *prompt, int col)
{
  double x = prompt->gutter;

  for (int i = 0; i < col && (guint) i < prompt->widths->len; i++)
    x += g_array_index (prompt->widths, int, i) * prompt->char_w + 2 * CELL_PAD + 1;
  return x;
}

/* Where character `position` of a fixed-width line is drawn. */
static double
char_x (ImportPrompt *prompt, int position)
{
  return prompt->gutter + CELL_PAD + position * prompt->char_w;
}

/* The character a point on the fixed-width preview is nearest to. */
static int
position_at (ImportPrompt *prompt, double x)
{
  return MAX (0, (int) ((x - prompt->gutter - CELL_PAD) / prompt->char_w + 0.5));
}

/* Where a column starts and ends, in pixels; on a fixed-width file the
 * last runs to the edge. */
static void
column_span (ImportPrompt *prompt, int col, double width, double *from, double *to)
{
  GArray *breaks = prompt->options->breaks;

  if (!prompt->options->fixed_width)
    {
      *from = column_x (prompt, col);
      *to = column_x (prompt, col + 1) - 1;
      return;
    }
  *from = col == 0 ? prompt->gutter : char_x (prompt, g_array_index (breaks, int, col - 1));
  *to = (guint) col < breaks->len ? char_x (prompt, g_array_index (breaks, int, col)) : width;
}

/* The column under a point, or -1 over the row numbers. */
static int
column_at (ImportPrompt *prompt, double x)
{
  int n = column_count (prompt);

  if (x < prompt->gutter)
    return -1;
  if (prompt->options->fixed_width)
    {
      int position = (int) floor ((x - prompt->gutter - CELL_PAD) / prompt->char_w);
      int col = 0;

      for (guint i = 0; i < prompt->options->breaks->len; i++)
        if (g_array_index (prompt->options->breaks, int, i) <= position)
          col = (int) i + 1;
      return col;
    }
  for (int i = 0; i < n; i++)
    if (x < column_x (prompt, i + 1))
      return i;
  return n - 1;
}

/* The name at the top of a column: its type, or the order of its date,
 * as Excel's preview heads it. */
static const char *
kind_label (const O42CsvColumn *column)
{
  static const char *const NAMES[] = { N_("General"), N_("Text"), N_("Date"), N_("Skip Column") };

  if (column->type == O42_SPLIT_DATE)
    return DATE_ORDERS[CLAMP ((int) column->order, 0, 5)];
  return _(NAMES[CLAMP ((int) column->type, 0, 3)]);
}

/* The line of the file record or line `i` of the preview starts on. */
static int
line_number (ImportPrompt *prompt, guint i)
{
  if (!prompt->options->fixed_width && i < prompt->first_lines->len)
    return g_array_index (prompt->first_lines, int, i);
  return prompt->options->start_row + (int) i + 1;
}

/* The preview is as big as what it shows, so the scrolled window round
 * it knows how far to scroll. */
static void
import_size_area (ImportPrompt *prompt)
{
  guint shown_rows = prompt->options->fixed_width ? prompt->lines->len : prompt->rows->len;
  int w, h;

  if (prompt->options->fixed_width)
    w = (int) (char_x (prompt, prompt->longest + 12));
  else
    w = (int) ceil (column_x (prompt, column_count (prompt))) + 2;
  h = (int) (body_top (prompt) + shown_rows * prompt->line_h + 6);
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (prompt->area), w);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (prompt->area), h);
}

/* The preview worked out again from the options as they stand. */
static void
import_recut (ImportPrompt *prompt)
{
  const char *from = prompt->text;
  PangoLayout *layout;
  char digits[16];
  int n;

  g_clear_pointer (&prompt->lines, g_ptr_array_unref);
  g_clear_pointer (&prompt->rows, g_ptr_array_unref);

  for (int i = 0; i < prompt->options->start_row && *from != '\0'; i++)
    {
      while (*from != '\0' && *from != '\n' && *from != '\r')
        from++;
      if (*from == '\r')
        from++;
      if (*from == '\n')
        from++;
    }
  prompt->lines = o42_csv_lines (from, PREVIEW_RECORDS);
  prompt->rows = o42_csv_preview (prompt->text, prompt->options, PREVIEW_RECORDS, prompt->first_lines);

  prompt->longest = 0;
  for (guint i = 0; i < prompt->lines->len; i++)
    prompt->longest = MAX (prompt->longest,
                           MIN ((int) g_utf8_strlen (g_ptr_array_index (prompt->lines, i), -1),
                                LINE_MAX_CHARS));

  g_array_set_size (prompt->widths, 0);
  for (guint r = 0; r < prompt->rows->len; r++)
    {
      char **fields = g_ptr_array_index (prompt->rows, r);

      for (int c = 0; fields[c] != NULL && c < 1000; c++)
        {
          int w = MIN ((int) g_utf8_strlen (fields[c], -1), PREVIEW_MAX_CHARS);

          if ((guint) c >= prompt->widths->len)
            {
              int least = 3;
              g_array_append_val (prompt->widths, least);
            }
          if (w > g_array_index (prompt->widths, int, c))
            g_array_index (prompt->widths, int, c) = w;
        }
    }
  /* Wide enough for "Skip Column" over it. */
  for (guint c = 0; c < prompt->widths->len; c++)
    {
      O42CsvColumn column = o42_csv_options_column (prompt->options, (int) c);
      int w = (int) g_utf8_strlen (kind_label (&column), -1);

      if (w > g_array_index (prompt->widths, int, c))
        g_array_index (prompt->widths, int, c) = w;
    }

  /* The row numbers as wide as the last of them. */
  layout = preview_layout (prompt->area, prompt);
  g_object_unref (layout);
  g_snprintf (digits, sizeof digits, "%d",
              line_number (prompt, MAX (prompt->options->fixed_width ? prompt->lines->len : prompt->rows->len, 1) - 1));
  prompt->gutter = (strlen (digits) + 1) * prompt->char_w + 2 * CELL_PAD;

  n = column_count (prompt);
  prompt->anchor = CLAMP (prompt->anchor, 0, MAX (n - 1, 0));
  prompt->cursor = CLAMP (prompt->cursor, 0, MAX (n - 1, 0));

  import_size_area (prompt);
  gtk_widget_queue_draw (prompt->area);
}

/* Where the columns seem to divide from the first row on, unless the
 * breaks have been placed by hand. */
static void
import_guess_breaks (ImportPrompt *prompt)
{
  GPtrArray *lines, *full;

  if (prompt->breaks_touched)
    return;
  lines = o42_csv_lines (prompt->text, prompt->options->start_row + 20);
  full = g_ptr_array_new ();
  for (guint i = (guint) prompt->options->start_row; i < lines->len; i++)
    if (*(char *) g_ptr_array_index (lines, i) != '\0')
      g_ptr_array_add (full, g_ptr_array_index (lines, i));
  g_array_set_size (prompt->options->breaks, 0);
  o42_guess_fixed_breaks ((const char *const *) full->pdata, full->len, prompt->options->breaks);
  g_ptr_array_unref (full);
  g_ptr_array_unref (lines);
}

/* The file read again in the encoding chosen. */
static void
import_redecode (ImportPrompt *prompt)
{
  g_free (prompt->text);
  prompt->text = o42_csv_decode (prompt->bytes, prompt->length, prompt->options->encoding, NULL, NULL);
  prompt->n_lines = o42_csv_count_lines (prompt->text);
  prompt->updating = TRUE;
  gtk_spin_button_set_range (GTK_SPIN_BUTTON (prompt->start), 1, MAX (prompt->n_lines, 1));
  prompt->updating = FALSE;
}

/* ---- The preview ---- */

/* The colour a column's text is in: white on the chosen ones, which are
 * shown in reverse as Excel shows them, and grey on those left out. */
static void
column_ink (cairo_t *cr, gboolean chosen, const O42CsvColumn *column)
{
  if (chosen)
    cairo_set_source_rgb (cr, 1, 1, 1);
  else if (column->type == O42_SPLIT_SKIP)
    cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
  else
    cairo_set_source_rgb (cr, 0, 0, 0);
}

/* One column: its type in the header, and under it the records' fields
 * of a delimited file or the lines themselves of a fixed-width one,
 * clipped to the column so that a chosen one is reversed from edge to
 * edge. */
static void
draw_column (ImportPrompt *prompt, PangoLayout *layout, cairo_t *cr,
             int col, double from, double to, double height)
{
  O42CsvColumn column = o42_csv_options_column (prompt->options, col);
  gboolean chosen = col >= sel_from (prompt) && col <= sel_to (prompt);
  double head = prompt->options->fixed_width ? RULER_H : 0, top = body_top (prompt);

  if (to <= from)
    return;
  cairo_save (cr);
  cairo_rectangle (cr, from, head, to - from, height - head);
  cairo_clip (cr);

  cairo_set_source_rgb (cr, 0.85, 0.85, 0.85);
  cairo_rectangle (cr, from, head, to - from, top - head - 1);
  cairo_fill (cr);
  pango_layout_set_text (layout, kind_label (&column), -1);
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_move_to (cr, from + CELL_PAD, head + 1);
  pango_cairo_show_layout (cr, layout);

  if (chosen)
    {
      cairo_set_source_rgb (cr, 0.0, 0.0, 0.5);
      cairo_rectangle (cr, from, top, to - from, height - top);
      cairo_fill (cr);
    }
  column_ink (cr, chosen, &column);
  if (prompt->options->fixed_width)
    for (guint i = 0; i < prompt->lines->len; i++)
      {
        char *line = shown (g_ptr_array_index (prompt->lines, i), LINE_MAX_CHARS);

        pango_layout_set_text (layout, line, -1);
        cairo_move_to (cr, char_x (prompt, 0), top + 1 + i * prompt->line_h);
        pango_cairo_show_layout (cr, layout);
        g_free (line);
      }
  else
    for (guint r = 0; r < prompt->rows->len; r++)
      {
        char **fields = g_ptr_array_index (prompt->rows, r);
        char *text;
        int k;

        for (k = 0; k < col && fields[k] != NULL; k++)
          ;
        if (k < col || fields[col] == NULL)
          continue;
        text = shown (fields[col], PREVIEW_MAX_CHARS);
        pango_layout_set_text (layout, text, -1);
        cairo_move_to (cr, from + CELL_PAD, top + 1 + r * prompt->line_h);
        pango_cairo_show_layout (cr, layout);
        g_free (text);
      }
  cairo_restore (cr);
}

/* A tick every character, a longer one every five, the number every
 * ten; a break an arrow on it and a line down through the lines. */
static void
draw_ruler (ImportPrompt *prompt, PangoLayout *layout, cairo_t *cr, int width, int height)
{
  PangoLayout *small = pango_layout_copy (layout);
  PangoFontDescription *desc = pango_font_description_from_string ("Sans 7");
  GArray *breaks = prompt->options->breaks;

  pango_layout_set_font_description (small, desc);
  pango_font_description_free (desc);
  cairo_set_source_rgb (cr, 0.3, 0.3, 0.3);
  cairo_set_line_width (cr, 1);
  for (int c = 0; char_x (prompt, c) < width; c++)
    {
      double x = floor (char_x (prompt, c)) + 0.5;
      double len = c % 10 == 0 ? 6 : c % 5 == 0 ? 4 : 2;

      cairo_move_to (cr, x, RULER_H - 1);
      cairo_line_to (cr, x, RULER_H - 1 - len);
      if (c % 10 == 0 && c > 0)
        {
          char number[12];
          int nw, nh;

          g_snprintf (number, sizeof number, "%d", c);
          pango_layout_set_text (small, number, -1);
          pango_layout_get_pixel_size (small, &nw, &nh);
          cairo_move_to (cr, x - nw / 2.0, 0);
          pango_cairo_show_layout (cr, small);
        }
    }
  cairo_stroke (cr);

  for (guint i = 0; i < breaks->len; i++)
    {
      double x = floor (char_x (prompt, g_array_index (breaks, int, i))) + 0.5;

      cairo_set_source_rgb (cr, 0.1, 0.1, 0.6);
      cairo_move_to (cr, x, RULER_H - 8);
      cairo_line_to (cr, x, height);
      cairo_stroke (cr);
      cairo_move_to (cr, x - 4, RULER_H - 12);
      cairo_line_to (cr, x + 4, RULER_H - 12);
      cairo_line_to (cr, x, RULER_H - 6);
      cairo_close_path (cr);
      cairo_fill (cr);
    }
  g_object_unref (small);
}

static void
preview_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  ImportPrompt *prompt = data;
  PangoLayout *layout = preview_layout (GTK_WIDGET (area), prompt);
  gboolean fixed = prompt->options->fixed_width;
  double head = fixed ? RULER_H : 0, top = body_top (prompt);
  guint shown_rows = fixed ? prompt->lines->len : prompt->rows->len;
  int n = column_count (prompt);

  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_paint (cr);

  for (int c = 0; c < n; c++)
    {
      double from, to;

      column_span (prompt, c, width, &from, &to);
      draw_column (prompt, layout, cr, c, from, to, height);
      if (!fixed)
        {
          cairo_set_source_rgb (cr, 0.3, 0.3, 0.3);
          cairo_rectangle (cr, to, 0, 1, height);
          cairo_fill (cr);
        }
    }

  /* The row numbers, and over them the corner that chooses every
   * column, grey as a sheet's headers are. */
  cairo_set_source_rgb (cr, 0.92, 0.92, 0.92);
  cairo_rectangle (cr, 0, head, prompt->gutter, height - head);
  cairo_fill (cr);
  cairo_set_source_rgb (cr, 0.8, 0.8, 0.8);
  cairo_rectangle (cr, 0, head, prompt->gutter, top - head - 1);
  cairo_fill (cr);
  cairo_set_source_rgb (cr, 0.4, 0.4, 0.4);
  for (guint i = 0; i < shown_rows; i++)
    {
      char number[16];
      int nw, nh;

      g_snprintf (number, sizeof number, "%d", line_number (prompt, i));
      pango_layout_set_text (layout, number, -1);
      pango_layout_get_pixel_size (layout, &nw, &nh);
      cairo_move_to (cr, prompt->gutter - CELL_PAD - nw, top + 1 + i * prompt->line_h);
      pango_cairo_show_layout (cr, layout);
    }
  cairo_set_source_rgb (cr, 0.3, 0.3, 0.3);
  cairo_rectangle (cr, prompt->gutter - 1, head, 1, height - head);
  cairo_rectangle (cr, 0, top - 1, width, 1);
  cairo_fill (cr);

  if (fixed)
    draw_ruler (prompt, layout, cr, width, height);

  /* Where the keyboard is: a dotted line round the column's type. */
  if (gtk_widget_has_focus (GTK_WIDGET (area)) && n > 0)
    {
      static const double DASH[] = { 1.0, 1.0 };
      double from, to;

      column_span (prompt, prompt->cursor, width, &from, &to);
      cairo_set_source_rgb (cr, 0, 0, 0);
      cairo_set_dash (cr, DASH, 2, 0);
      cairo_rectangle (cr, from + 1.5, head + 1.5, MAX (to - from - 3, 1), top - head - 4);
      cairo_stroke (cr);
    }
  g_object_unref (layout);
}

/* ---- Choosing columns and placing breaks ---- */

static void
import_sync_type (ImportPrompt *prompt)
{
  O42CsvColumn column = o42_csv_options_column (prompt->options, prompt->cursor);

  prompt->updating = TRUE;
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->type_drop), type_index (&column));
  prompt->updating = FALSE;
}

/* The chosen columns in view: the scrolled window moved as little as
 * it takes to show the one the keyboard is on. */
static void
reveal_cursor (ImportPrompt *prompt)
{
  GtkAdjustment *h = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (prompt->scroller));
  double from, to, page = gtk_adjustment_get_page_size (h), value = gtk_adjustment_get_value (h);

  column_span (prompt, prompt->cursor, gtk_adjustment_get_upper (h), &from, &to);
  to = MIN (to, from + page);
  if (from - prompt->gutter < value)
    gtk_adjustment_set_value (h, MAX (from - prompt->gutter, 0));
  else if (to > value + page)
    gtk_adjustment_set_value (h, to - page);
}

static void
choose_columns (ImportPrompt *prompt, int anchor, int cursor)
{
  int n = column_count (prompt);

  if (n <= 0)
    return;
  prompt->anchor = CLAMP (anchor, 0, n - 1);
  prompt->cursor = CLAMP (cursor, 0, n - 1);
  import_sync_type (prompt);
  reveal_cursor (prompt);
  gtk_widget_queue_draw (prompt->area);
}

static int
break_near (ImportPrompt *prompt, double x)
{
  GArray *breaks = prompt->options->breaks;

  for (guint i = 0; i < breaks->len; i++)
    if (fabs (x - char_x (prompt, g_array_index (breaks, int, i))) <= 4)
      return (int) i;
  return -1;
}

static void
insert_break (ImportPrompt *prompt, int position)
{
  GArray *breaks = prompt->options->breaks;
  guint at;

  if (position <= 0)
    return;
  for (at = 0; at < breaks->len; at++)
    {
      int b = g_array_index (breaks, int, at);

      if (b == position)
        return;
      if (b > position)
        break;
    }
  g_array_insert_val (breaks, at, position);
}

/* A click on a column chooses it, with Shift every column from the one
 * chosen before, and on the corner every column there is.  On a
 * fixed-width file the ruler takes Excel's three gestures: a click
 * puts in a break, a drag moves one, a double-click takes one away. */
static void
on_preview_pressed (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  ImportPrompt *prompt = data;
  GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
  gboolean extend = (state & GDK_SHIFT_MASK) != 0;
  int col;

  gtk_widget_grab_focus (prompt->area);
  if (prompt->options->fixed_width)
    {
      int near = break_near (prompt, x);

      if (x >= prompt->gutter && n_press == 2 && near >= 0)
        {
          g_array_remove_index (prompt->options->breaks, near);
          prompt->breaks_touched = TRUE;
          prompt->dragging = -1;
          import_recut (prompt);
          return;
        }
      if (x >= prompt->gutter && near >= 0)
        {
          prompt->dragging = near;
          prompt->breaks_touched = TRUE;
          return;
        }
      if (y < RULER_H)
        {
          if (n_press == 1 && x >= prompt->gutter)
            {
              insert_break (prompt, position_at (prompt, x));
              prompt->breaks_touched = TRUE;
              import_recut (prompt);
            }
          return;
        }
    }

  col = column_at (prompt, x);
  if (col < 0)
    {
      if (y < body_top (prompt))
        choose_columns (prompt, 0, column_count (prompt) - 1);
      return;
    }
  choose_columns (prompt, extend ? prompt->anchor : col, col);
}

static void
on_preview_released (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  ImportPrompt *prompt = data;
  GArray *breaks = prompt->options->breaks;

  (void) gesture; (void) n_press; (void) x; (void) y;
  if (prompt->dragging < 0)
    return;
  prompt->dragging = -1;
  /* In order again after a drag past a neighbour, and none twice. */
  for (guint i = 1; i < breaks->len; i++)
    if (g_array_index (breaks, int, i) <= g_array_index (breaks, int, i - 1))
      {
        g_array_remove_index (breaks, i);
        i--;
      }
  import_recut (prompt);
}

static void
on_preview_motion (GtkEventControllerMotion *motion, double x, double y, gpointer data)
{
  ImportPrompt *prompt = data;

  (void) motion; (void) y;
  if (prompt->dragging < 0 || (guint) prompt->dragging >= prompt->options->breaks->len)
    return;
  g_array_index (prompt->options->breaks, int, prompt->dragging) = MAX (1, position_at (prompt, x));
  gtk_widget_queue_draw (prompt->area);
}

/* The keyboard on the preview: Left and Right go from column to column,
 * Home and End to the first and the last, Shift with any of them
 * chooses the columns on the way, and Ctrl+A chooses them all. */
static gboolean
on_preview_key (GtkEventControllerKey *key, guint keyval, guint keycode,
                GdkModifierType state, gpointer data)
{
  ImportPrompt *prompt = data;
  gboolean extend = (state & GDK_SHIFT_MASK) != 0;
  int n = column_count (prompt), to;

  (void) key; (void) keycode;
  if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_a || keyval == GDK_KEY_A))
    {
      choose_columns (prompt, 0, n - 1);
      return TRUE;
    }
  switch (keyval)
    {
    case GDK_KEY_Left:  case GDK_KEY_KP_Left:  to = prompt->cursor - 1; break;
    case GDK_KEY_Right: case GDK_KEY_KP_Right: to = prompt->cursor + 1; break;
    case GDK_KEY_Home:  case GDK_KEY_KP_Home:  to = 0; break;
    case GDK_KEY_End:   case GDK_KEY_KP_End:   to = n - 1; break;
    default:
      return FALSE;
    }
  choose_columns (prompt, extend ? prompt->anchor : to, to);
  return TRUE;
}

static void
on_preview_focus (GtkEventControllerFocus *focus, gpointer data)
{
  (void) focus;
  gtk_widget_queue_draw (((ImportPrompt *) data)->area);
}

/* ---- The controls ---- */

static void
on_type_changed (GObject *drop, GParamSpec *pspec, gpointer data)
{
  ImportPrompt *prompt = data;
  guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (drop));
  O42SplitType type = i == TYPE_TEXT ? O42_SPLIT_TEXT
                    : i == TYPE_SKIP ? O42_SPLIT_SKIP
                    : i >= TYPE_DATE ? O42_SPLIT_DATE : O42_SPLIT_GENERAL;
  O42DateOrder order = type == O42_SPLIT_DATE ? (O42DateOrder) (i - TYPE_DATE) : O42_DATE_MDY;

  (void) pspec;
  if (prompt->updating || i == GTK_INVALID_LIST_POSITION)
    return;
  for (int c = sel_from (prompt); c <= sel_to (prompt); c++)
    o42_csv_options_set_column (prompt->options, c, type, order);
  import_recut (prompt);
}

/* What the separator options say, into the options. */
static void
on_cut_changed (GtkWidget *w, gpointer data)
{
  ImportPrompt *prompt = data;
  static const char SEPS[4] = { '\t', ';', ',', ' ' };
  GString *seps;
  guint quote;

  (void) w;
  if (prompt->updating)
    return;
  seps = g_string_new (NULL);
  for (int i = 0; i < 4; i++)
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->sep[i])))
      g_string_append_c (seps, SEPS[i]);
  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->other_check)))
    g_string_append (seps, gtk_editable_get_text (GTK_EDITABLE (prompt->other)));
  o42_csv_options_set_separators (prompt->options, seps->str);
  g_string_free (seps, TRUE);

  prompt->options->merge_separators = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->merge));
  prompt->options->trim_spaces = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->trim));
  quote = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->quote));
  prompt->options->quote = quote == 0 ? '"' : quote == 1 ? '\'' : '\0';
  import_recut (prompt);
}

static void
on_other_text (GtkEditable *editable, gpointer data)
{
  ImportPrompt *prompt = data;

  /* Typing a character is asking for it. */
  if (!prompt->updating && *gtk_editable_get_text (editable) != '\0' &&
      !gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->other_check)))
    gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->other_check), TRUE);
  else
    on_cut_changed (NULL, prompt);
}

static void
on_quote_changed (GObject *drop, GParamSpec *pspec, gpointer data)
{
  (void) drop; (void) pspec;
  on_cut_changed (NULL, data);
}

/* Delimited or fixed width: the separator options only mean something
 * for the one, and the preview has a ruler for the other. */
static void
import_sync_file_kind (ImportPrompt *prompt)
{
  gboolean fixed = prompt->options->fixed_width;

  gtk_widget_set_sensitive (prompt->cut_box, !fixed);
  gtk_widget_set_sensitive (prompt->cut_more, !fixed);
  gtk_label_set_text (GTK_LABEL (prompt->hint),
                      fixed ? _("Click the ruler to put in a break, drag one to move it, "
                                "double-click one to take it away.  Click a column to choose its format.")
                            : _("Click a column to choose it, Shift+click to choose several, "
                                "the corner to choose all."));
}

static void
on_kind_of_file (GtkCheckButton *button, gpointer data)
{
  ImportPrompt *prompt = data;

  (void) button;
  if (prompt->updating)
    return;
  prompt->options->fixed_width = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->fixed));
  prompt->anchor = prompt->cursor = 0;
  import_sync_file_kind (prompt);
  import_recut (prompt);
  import_sync_type (prompt);
}

static void
on_start_changed (GtkSpinButton *spin, gpointer data)
{
  ImportPrompt *prompt = data;

  if (prompt->updating)
    return;
  prompt->options->start_row = MAX (0, gtk_spin_button_get_value_as_int (spin) - 1);
  import_guess_breaks (prompt);
  import_recut (prompt);
}

static void
on_origin_changed (GObject *drop, GParamSpec *pspec, gpointer data)
{
  ImportPrompt *prompt = data;
  guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (drop));

  (void) pspec;
  if (prompt->updating || i >= prompt->encodings->len)
    return;
  o42_csv_options_set_encoding (prompt->options, g_ptr_array_index (prompt->encodings, i));
  import_redecode (prompt);
  import_guess_breaks (prompt);
  import_recut (prompt);
}

/* How the file writes its numbers, and what its quotes and formulas
 * mean: none of it changes how the file is cut, so the preview stays. */
static void
on_other_options (GtkWidget *w, gpointer data)
{
  ImportPrompt *prompt = data;
  static const char DECIMALS[] = { '.', ',' };
  static const char THOUSANDS[] = { ',', '.', ' ', '\'', '\0' };
  guint d = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->decimal));
  guint t = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->thousands));

  (void) w;
  if (prompt->updating)
    return;
  prompt->options->decimal = DECIMALS[MIN (d, 1)];
  prompt->options->thousands = THOUSANDS[MIN (t, 4)];
  prompt->options->trailing_minus = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->minus));
  prompt->options->quoted_as_text = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->quoted_text));
  prompt->options->evaluate_formulas = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->formulas));
}

static void
on_other_drop (GObject *drop, GParamSpec *pspec, gpointer data)
{
  (void) drop; (void) pspec;
  on_other_options (NULL, data);
}

/* A new sheet starts at its A1, so the cell to put the data at is not
 * asked. */
static void
on_new_sheet (GtkCheckButton *button, gpointer data)
{
  ImportPrompt *prompt = data;

  gtk_widget_set_sensitive (prompt->place, !gtk_check_button_get_active (button));
}

static void
on_import_ok (GtkWidget *w, gpointer data)
{
  ImportPrompt *prompt = data;
  O42Window *self = prompt->window;

  (void) w;

  if (prompt->open)
    {
      O42Window *target = self;

      /* A book with work in it stays; the file opens beside it. */
      if (!o42_window_is_blank (self))
        {
          target = O42_WINDOW (o42_window_new (gtk_window_get_application (GTK_WINDOW (self))));
          gtk_window_present (GTK_WINDOW (target));
        }
      o42_window_open_text (target, prompt->file, prompt->text, prompt->options);
    }
  else
    {
      int row = 0, col = 0, n, widest = 0;
      gboolean new_sheet = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->new_sheet));

      if (new_sheet)
        {
          int index = o42_book_sheet_index (self->book, self->sheet);

          /* A protected workbook takes no new sheet, and says so. */
          if (o42_window_structure_locked (self))
            return;
          o42_book_add_sheet (self->book, NULL, index);
          o42_window_show_sheet (self, index);
          o42_window_tell_book (self, "sheets");
        }
      else
        {
          const char *place = gtk_editable_get_text (GTK_EDITABLE (prompt->place));

          /* Excel refuses to bring a file in over a protected sheet. */
          if (o42_sheet_protected (self->sheet))
            {
              show_error (self, _("The sheet is protected: Tools > Protection > Unprotect Sheet first."), NULL);
              return;
            }
          if (*place == '=' || *place == '$')
            place++;
          if (!o42_ref_parse_full (place, &row, &col, NULL, NULL, NULL))
            {
              show_error (self, _("Put the data at a cell, such as A1."), NULL);
              return;
            }
        }
      prompt->options->skip_empty = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->skip_empty));
      if (o42_grid_is_editing (self->grid))
        o42_grid_commit_edit (self->grid);
      n = o42_csv_import (self->sheet, prompt->text, prompt->options, row, col, FALSE, &widest);
      if (n > 0 && widest > 0)
        {
          O42Range range = { row, col, MIN (row + n - 1, O42_MAX_ROWS - 1),
                             MIN (col + widest - 1, O42_MAX_COLS - 1) };

          /* The records chosen, from their first cell, as Excel leaves
           * them. */
          o42_grid_set_cursor (self->grid, &range, row, col);
        }
      o42_sheet_set_modified (self->sheet, TRUE);
      o42_grid_refresh (self->grid);
      o42_window_tell_book (self, "cells");
      window_sync (self);
    }
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_import_destroy (GtkWidget *w, gpointer data)
{
  ImportPrompt *prompt = data;

  (void) w;
  g_object_unref (prompt->file);
  g_free (prompt->bytes);
  g_free (prompt->text);
  o42_csv_options_free (prompt->options);
  g_ptr_array_unref (prompt->encodings);
  g_clear_pointer (&prompt->lines, g_ptr_array_unref);
  g_clear_pointer (&prompt->rows, g_ptr_array_unref);
  g_array_unref (prompt->first_lines);
  g_array_unref (prompt->widths);
  g_free (prompt);
}

/* ---- The page ---- */

/* A label with a mnemonic for the control after it, both in the box. */
static void
labelled_in (GtkWidget *box, const char *label, GtkWidget *control, int gap)
{
  GtkWidget *l = gtk_label_new_with_mnemonic (label);

  gtk_widget_set_margin_start (l, gap);
  gtk_label_set_mnemonic_widget (GTK_LABEL (l), control);
  gtk_box_append (GTK_BOX (box), l);
  gtk_box_append (GTK_BOX (box), control);
}

/* A check button whose label wraps rather than widening the dialog:
 * Excel's words for some of these run to a line and a half in French. */
static GtkWidget *
wrapping_check (const char *label)
{
  GtkWidget *check = gtk_check_button_new ();
  GtkWidget *text = gtk_label_new_with_mnemonic (label);
  PangoLayout *layout = gtk_widget_create_pango_layout (text, gtk_label_get_text (GTK_LABEL (text)));
  int w, h;

  /* Wrapped only when it is long: a wrapping label left to itself
   * wraps what would have fitted. */
  pango_layout_get_pixel_size (layout, &w, &h);
  g_object_unref (layout);
  if (w > 20 * h)
    {
      gtk_label_set_wrap (GTK_LABEL (text), TRUE);
      gtk_label_set_max_width_chars (GTK_LABEL (text), 40);
    }
  gtk_label_set_xalign (GTK_LABEL (text), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (text), check);
  gtk_check_button_set_child (GTK_CHECK_BUTTON (check), text);
  return check;
}

/* The file's origin and the row to start at. */
static GtkWidget *
import_top_row (ImportPrompt *prompt)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

  prompt->origin = encoding_drop (prompt->options->encoding, prompt->encodings);
  labelled_in (row, _("File _origin:"), prompt->origin, 0);
  g_signal_connect (prompt->origin, "notify::selected", G_CALLBACK (on_origin_changed), prompt);

  prompt->start = gtk_spin_button_new_with_range (1, MAX (prompt->n_lines, 1), 1);
  labelled_in (row, _("Start import at _row:"), prompt->start, 16);
  g_signal_connect (prompt->start, "value-changed", G_CALLBACK (on_start_changed), prompt);
  return row;
}

/* Delimited, with the delimiters beside it and what goes with them
 * under, or fixed width. */
static GtkWidget *
import_cut_frame (ImportPrompt *prompt)
{
  static const char *const NAMES[4] = { N_("_Tab"), N_("_Semicolon"), N_("_Comma"), N_("S_pace") };
  static const char *const QUOTES[] = { "\"", "'", N_("{none}"), NULL };
  static const char SEPS[4] = { '\t', ';', ',', ' ' };
  GtkWidget *grid = gtk_grid_new ();
  GtkWidget *seps, *row;
  GtkStringList *quotes = gtk_string_list_new (NULL);
  GString *others = g_string_new (NULL);

  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 16);

  prompt->delimited = gtk_check_button_new_with_mnemonic (_("_Delimited"));
  gtk_grid_attach (GTK_GRID (grid), prompt->delimited, 0, 0, 1, 1);

  prompt->cut_box = seps = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  for (int i = 0; i < 4; i++)
    {
      prompt->sep[i] = gtk_check_button_new_with_mnemonic (_(NAMES[i]));
      gtk_box_append (GTK_BOX (seps), prompt->sep[i]);
    }
  prompt->other_check = gtk_check_button_new_with_mnemonic (_("_Other:"));
  gtk_box_append (GTK_BOX (seps), prompt->other_check);
  prompt->other = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->other), 3);
  gtk_editable_set_max_width_chars (GTK_EDITABLE (prompt->other), 3);
  gtk_entry_set_max_length (GTK_ENTRY (prompt->other), 4);
  gtk_box_append (GTK_BOX (seps), prompt->other);
  gtk_grid_attach (GTK_GRID (grid), seps, 1, 0, 1, 1);

  prompt->cut_more = row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  prompt->merge = wrapping_check (_("Treat consecutive delimiters as o_ne"));
  gtk_box_append (GTK_BOX (row), prompt->merge);
  prompt->trim = gtk_check_button_new_with_mnemonic (_("Tr_im spaces"));
  gtk_box_append (GTK_BOX (row), prompt->trim);
  for (int i = 0; QUOTES[i] != NULL; i++)
    gtk_string_list_append (quotes, i < 2 ? QUOTES[i] : _(QUOTES[i]));
  prompt->quote = gtk_drop_down_new (G_LIST_MODEL (quotes), NULL);
  labelled_in (row, _("Text _qualifier:"), prompt->quote, 12);
  gtk_grid_attach (GTK_GRID (grid), row, 1, 1, 1, 1);

  prompt->fixed = gtk_check_button_new_with_mnemonic (_("Fixed _width"));
  gtk_check_button_set_group (GTK_CHECK_BUTTON (prompt->fixed), GTK_CHECK_BUTTON (prompt->delimited));
  gtk_grid_attach (GTK_GRID (grid), prompt->fixed, 0, 2, 2, 1);

  /* What the file was guessed to be, on the controls. */
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->options->fixed_width ? prompt->fixed
                                                                                : prompt->delimited), TRUE);
  for (const char *s = prompt->options->separators != NULL ? prompt->options->separators : "";
       *s != '\0'; s = g_utf8_next_char (s))
    {
      int known = -1;

      for (int i = 0; i < 4; i++)
        if (*s == SEPS[i])
          known = i;
      if (known >= 0)
        gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->sep[known]), TRUE);
      else
        g_string_append_len (others, s, g_utf8_next_char (s) - s);
    }
  if (others->len > 0)
    {
      gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->other_check), TRUE);
      gtk_editable_set_text (GTK_EDITABLE (prompt->other), others->str);
    }
  g_string_free (others, TRUE);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->merge), prompt->options->merge_separators);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->trim), prompt->options->trim_spaces);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->quote),
                              prompt->options->quote == '"' ? 0 : prompt->options->quote == '\'' ? 1 : 2);

  g_signal_connect (prompt->fixed, "toggled", G_CALLBACK (on_kind_of_file), prompt);
  for (int i = 0; i < 4; i++)
    g_signal_connect (prompt->sep[i], "toggled", G_CALLBACK (on_cut_changed), prompt);
  g_signal_connect (prompt->other_check, "toggled", G_CALLBACK (on_cut_changed), prompt);
  g_signal_connect (prompt->other, "changed", G_CALLBACK (on_other_text), prompt);
  g_signal_connect (prompt->merge, "toggled", G_CALLBACK (on_cut_changed), prompt);
  g_signal_connect (prompt->trim, "toggled", G_CALLBACK (on_cut_changed), prompt);
  g_signal_connect (prompt->quote, "notify::selected", G_CALLBACK (on_quote_changed), prompt);

  return framed (_("Original data type"), grid);
}

/* How the file writes its numbers -- what Excel 2002 put behind an
 * Advanced button -- and what its quotes and formulas mean. */
static GtkWidget *
import_other_frame (ImportPrompt *prompt)
{
  static const char *const DECIMALS[] = { ".", ",", NULL };
  static const char *const THOUSANDS[] = { ",", ".", N_("space"), "'", N_("{none}"), NULL };
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *numbers = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *meaning = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  GtkStringList *thousands = gtk_string_list_new (NULL);

  prompt->decimal = gtk_drop_down_new (G_LIST_MODEL (gtk_string_list_new (DECIMALS)), NULL);
  labelled_in (numbers, _("De_cimal separator:"), prompt->decimal, 0);
  for (int i = 0; THOUSANDS[i] != NULL; i++)
    gtk_string_list_append (thousands, i == 2 || i == 4 ? _(THOUSANDS[i]) : THOUSANDS[i]);
  prompt->thousands = gtk_drop_down_new (G_LIST_MODEL (thousands), NULL);
  labelled_in (numbers, _("T_housands separator:"), prompt->thousands, 12);
  prompt->minus = wrapping_check (_("Trailing _minus for negative numbers"));
  gtk_widget_set_margin_start (prompt->minus, 12);
  gtk_box_append (GTK_BOX (numbers), prompt->minus);
  gtk_box_append (GTK_BOX (box), numbers);

  prompt->quoted_text = gtk_check_button_new_with_mnemonic (_("Format quoted fields as te_xt"));
  gtk_box_append (GTK_BOX (meaning), prompt->quoted_text);
  prompt->formulas = gtk_check_button_new_with_mnemonic (_("Evalu_ate formulas"));
  gtk_box_append (GTK_BOX (meaning), prompt->formulas);
  gtk_box_append (GTK_BOX (box), meaning);

  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->decimal), prompt->options->decimal == ',' ? 1 : 0);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->thousands),
                              prompt->options->thousands == ','  ? 0
                              : prompt->options->thousands == '.' ? 1
                              : prompt->options->thousands == ' ' ? 2
                              : prompt->options->thousands == '\'' ? 3 : 4);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->minus), prompt->options->trailing_minus);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->quoted_text), prompt->options->quoted_as_text);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->formulas), prompt->options->evaluate_formulas);
  g_signal_connect (prompt->decimal, "notify::selected", G_CALLBACK (on_other_drop), prompt);
  g_signal_connect (prompt->thousands, "notify::selected", G_CALLBACK (on_other_drop), prompt);
  g_signal_connect (prompt->minus, "toggled", G_CALLBACK (on_other_options), prompt);
  g_signal_connect (prompt->quoted_text, "toggled", G_CALLBACK (on_other_options), prompt);
  g_signal_connect (prompt->formulas, "toggled", G_CALLBACK (on_other_options), prompt);

  return framed (_("Other options"), box);
}

/* Import Text File's own question: where the records go, the cell the
 * active one to start with, or a sheet of their own. */
static GtkWidget *
import_place_row (ImportPrompt *prompt)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  int r = 0, c = 0;
  char *here;

  prompt->place = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->place), 8);
  gtk_editable_set_max_width_chars (GTK_EDITABLE (prompt->place), 8);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->place), TRUE);
  o42_grid_get_active (prompt->window->grid, &r, &c);
  here = o42_ref_name (r, c);
  gtk_editable_set_text (GTK_EDITABLE (prompt->place), here);
  g_free (here);
  labelled_in (row, _("_Put the data at:"), prompt->place, 0);

  prompt->new_sheet = gtk_check_button_new_with_mnemonic (_("On a _new sheet"));
  gtk_box_append (GTK_BOX (row), prompt->new_sheet);
  g_signal_connect (prompt->new_sheet, "toggled", G_CALLBACK (on_new_sheet), prompt);
  prompt->skip_empty = gtk_check_button_new_with_mnemonic (_("S_kip empty cells"));
  gtk_widget_set_margin_start (prompt->skip_empty, 16);
  gtk_box_append (GTK_BOX (row), prompt->skip_empty);
  return row;
}

/* The type for the chosen columns, right over the preview they are
 * chosen in, and what a click there does. */
static GtkWidget *
import_type_row (ImportPrompt *prompt)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkStringList *types = gtk_string_list_new (NULL);

  gtk_string_list_append (types, _("General"));
  gtk_string_list_append (types, _("Text"));
  for (int i = 0; DATE_ORDERS[i] != NULL; i++)
    {
      char *label = g_strdup_printf (_("Date (%s)"), DATE_ORDERS[i]);

      gtk_string_list_append (types, label);
      g_free (label);
    }
  gtk_string_list_append (types, _("Skip Column"));
  prompt->type_drop = gtk_drop_down_new (G_LIST_MODEL (types), NULL);
  labelled_in (row, _("Column t_ype:"), prompt->type_drop, 0);
  g_signal_connect (prompt->type_drop, "notify::selected", G_CALLBACK (on_type_changed), prompt);

  prompt->hint = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (prompt->hint), 0.0);
  /* A label that wraps still asks for the whole of its text across,
   * unless told otherwise, and would widen the dialog to get it. */
  gtk_label_set_wrap (GTK_LABEL (prompt->hint), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (prompt->hint), 50);
  gtk_widget_set_hexpand (prompt->hint, TRUE);
  gtk_widget_set_margin_start (prompt->hint, 8);
  gtk_widget_add_css_class (prompt->hint, "dim-label");
  gtk_box_append (GTK_BOX (row), prompt->hint);
  return row;
}

/* The preview in a scrolled window with a frame round it, taking the
 * mouse and, once clicked or tabbed to, the keyboard. */
static GtkWidget *
import_preview (ImportPrompt *prompt)
{
  GtkWidget *frame = gtk_frame_new (NULL);
  GtkGesture *click = gtk_gesture_click_new ();
  GtkEventController *motion = gtk_event_controller_motion_new ();
  GtkEventController *key = gtk_event_controller_key_new ();
  GtkEventController *focus = gtk_event_controller_focus_new ();

  prompt->area = gtk_drawing_area_new ();
  gtk_widget_set_focusable (prompt->area, TRUE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (prompt->area), preview_draw, prompt, NULL);
  g_signal_connect (click, "pressed", G_CALLBACK (on_preview_pressed), prompt);
  g_signal_connect (click, "released", G_CALLBACK (on_preview_released), prompt);
  gtk_widget_add_controller (prompt->area, GTK_EVENT_CONTROLLER (click));
  g_signal_connect (motion, "motion", G_CALLBACK (on_preview_motion), prompt);
  gtk_widget_add_controller (prompt->area, motion);
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_preview_key), prompt);
  gtk_widget_add_controller (prompt->area, key);
  g_signal_connect (focus, "enter", G_CALLBACK (on_preview_focus), prompt);
  g_signal_connect (focus, "leave", G_CALLBACK (on_preview_focus), prompt);
  gtk_widget_add_controller (prompt->area, focus);

  prompt->scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (prompt->scroller),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (prompt->scroller, 680, 200);
  gtk_widget_set_vexpand (prompt->scroller, TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (prompt->scroller), prompt->area);
  gtk_frame_set_child (GTK_FRAME (frame), prompt->scroller);
  return frame;
}

void
o42_window_import_text (O42Window *self, GFile *file, gboolean open)
{
  ImportPrompt *prompt;
  GtkWidget *content, *buttons, *ok;
  GError *error = NULL;
  char *bytes = NULL, *name, *title;
  gsize length = 0;
  const char *found = NULL;
  gboolean bom = FALSE;

  g_return_if_fail (O42_IS_WINDOW (self));
  g_return_if_fail (G_IS_FILE (file));

  if (!g_file_load_contents (file, NULL, &bytes, &length, NULL, &error))
    {
      show_error (self, _("office42 could not open that file."), error);
      g_clear_error (&error);
      return;
    }

  prompt = g_new0 (ImportPrompt, 1);
  prompt->window = self;
  prompt->file = g_object_ref (file);
  prompt->open = open;
  prompt->bytes = bytes;
  prompt->length = length;
  prompt->encodings = g_ptr_array_new ();
  prompt->first_lines = g_array_new (FALSE, FALSE, sizeof (int));
  prompt->widths = g_array_new (FALSE, FALSE, sizeof (int));
  prompt->dragging = -1;
  prompt->char_w = 8;
  prompt->line_h = 16;

  /* The first guess: the encoding, then what that text looks like. */
  prompt->options = o42_csv_options_new ();
  prompt->text = o42_csv_decode (bytes, length, NULL, &found, &bom);
  prompt->n_lines = o42_csv_count_lines (prompt->text);
  name = g_file_get_basename (file);
  o42_csv_sniff (prompt->text, name, prompt->options);
  o42_csv_options_set_encoding (prompt->options, found);
  prompt->options->bom = bom;

  title = g_strdup_printf (_("Text Import - %s"), name);
  prompt->dialog = dialog_frame (self, title, TRUE, &content, &buttons);
  g_free (title);
  g_free (name);

  prompt->updating = TRUE;
  gtk_box_append (GTK_BOX (content), import_top_row (prompt));
  gtk_box_append (GTK_BOX (content), import_cut_frame (prompt));
  gtk_box_append (GTK_BOX (content), import_other_frame (prompt));
  if (!open)
    gtk_box_append (GTK_BOX (content), import_place_row (prompt));
  gtk_box_append (GTK_BOX (content), import_type_row (prompt));
  gtk_box_append (GTK_BOX (content), import_preview (prompt));
  import_sync_file_kind (prompt);
  prompt->updating = FALSE;

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_import_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);

  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_import_destroy), prompt);

  import_recut (prompt);
  import_sync_type (prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* File > Import Text File: Excel 2000's Data > Get External Data >
 * Import Text File, Text Import over a file chosen, its records put at
 * the active cell of the sheet on show rather than in a book of their
 * own. */
static void
on_import_text_chosen (GObject *source, GAsyncResult *result, gpointer data)
{
  O42Window *self = data;
  GError *error = NULL;
  GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      o42_window_import_text (self, file, FALSE);
      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, _("office42 could not open that file."), error);
  g_clear_error (&error);
}

void
action_import_text (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  GtkFileFilter *text = gtk_file_filter_new ();
  GtkFileFilter *all = gtk_file_filter_new ();
  static const char *const SUFFIXES[] = { "prn", "txt", "csv", "tsv", "tab", NULL };

  (void) a; (void) p;

  gtk_file_filter_set_name (text, _("Text Files (*.prn, *.txt, *.csv)"));
  for (int i = 0; SUFFIXES[i] != NULL; i++)
    gtk_file_filter_add_suffix (text, SUFFIXES[i]);
  gtk_file_filter_set_name (all, _("All Files"));
  gtk_file_filter_add_pattern (all, "*");
  g_list_store_append (filters, text);
  g_list_store_append (filters, all);

  gtk_file_dialog_set_title (dialog, _("Import Text File"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_default_filter (dialog, text);
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_import_text_chosen, self);

  g_object_unref (text);
  g_object_unref (all);
  g_object_unref (filters);
  g_object_unref (dialog);
}

/* ---- Save As, to a text file ------------------------------------------- */

typedef struct {
  O42Window     *window;
  GFile         *file;
  O42CsvOptions *options;
  GPtrArray     *encodings;
  gboolean       saved;
  GtkWidget     *dialog;
  GtkWidget     *sep[5], *other, *fixed;
  GtkWidget     *quote, *encoding, *bom, *ends, *decimal;
  GtkWidget     *as_shown, *formulas, *quote_all;
} SaveTextPrompt;

static void
on_save_text_ok (GtkWidget *w, gpointer data)
{
  SaveTextPrompt *prompt = data;
  O42CsvOptions *o = prompt->options;
  static const char *const SEPS[5] = { ",", ";", "\t", " ", NULL };
  guint quote = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->quote));
  guint encoding = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->encoding));

  (void) w;
  o->fixed_width = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->fixed));
  for (int i = 0; i < 4; i++)
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->sep[i])))
      o42_csv_options_set_separators (o, SEPS[i]);
  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->sep[4])))
    {
      const char *other = gtk_editable_get_text (GTK_EDITABLE (prompt->other));

      if (*other == '\0')
        {
          show_error (prompt->window, _("Type the character to put between the fields."), NULL);
          return;
        }
      o42_csv_options_set_separators (o, other);
    }
  o->quote = quote == 0 ? '"' : quote == 1 ? '\'' : '\0';
  if (encoding < prompt->encodings->len)
    o42_csv_options_set_encoding (o, g_ptr_array_index (prompt->encodings, encoding));
  o->bom = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->bom));
  o->crlf = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->ends)) == 0;
  if (gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->decimal)) == 1)
    {
      o->decimal = ',';
      if (o->thousands == ',' || o->thousands == '\0')
        o->thousands = '.';
    }
  else
    {
      o->decimal = '.';
      o->thousands = ',';
    }
  o->as_shown = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->as_shown));
  o->formulas = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->formulas));
  o->quote_all = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->quote_all));
  /* The breaks were the file's it came from, and this is a new file. */
  g_array_set_size (o->breaks, 0);

  prompt->saved = TRUE;
  o42_window_save_to (prompt->window, prompt->file, o);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_save_text_destroy (GtkWidget *w, gpointer data)
{
  SaveTextPrompt *prompt = data;

  (void) w;
  /* Cancelled: a close that was waiting on the save waits no more. */
  if (!prompt->saved)
    prompt->window->close_after_save = FALSE;
  g_object_unref (prompt->file);
  o42_csv_options_free (prompt->options);
  g_ptr_array_unref (prompt->encodings);
  g_free (prompt);
}

static void
on_save_text_kind (GtkCheckButton *button, gpointer data)
{
  SaveTextPrompt *prompt = data;
  gboolean fixed = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->fixed));

  (void) button;
  /* Padded columns have nothing to quote. */
  gtk_widget_set_sensitive (prompt->quote, !fixed);
  gtk_widget_set_sensitive (prompt->quote_all, !fixed);
}

static void
on_save_text_other (GtkEditable *editable, gpointer data)
{
  SaveTextPrompt *prompt = data;

  if (*gtk_editable_get_text (editable) != '\0')
    gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->sep[4]), TRUE);
}

void
o42_window_save_text_as (O42Window *self, GFile *file)
{
  SaveTextPrompt *prompt = g_new0 (SaveTextPrompt, 1);
  static const char *const NAMES[4] = { N_("_Comma"), N_("_Semicolon"), N_("_Tab"), N_("S_pace") };
  static const char *const ENDS[] = { N_("Windows (CR LF)"), N_("Unix and macOS (LF)"), NULL };
  GtkWidget *content, *buttons, *seps, *grid, *label, *ok, *row;
  GtkStringList *quotes = gtk_string_list_new (NULL);
  GtkStringList *ends = gtk_string_list_new (NULL);
  O42CsvOptions *fresh = o42_csv_options_for_file (file);
  O42CsvOptions *o;
  const char *sep;
  char *base, *title;

  prompt->window = self;
  prompt->file = g_object_ref (file);
  prompt->encodings = g_ptr_array_new ();

  /* What the book's text file was written with before, when there is
   * one of the same kind; else what the name asks for. */
  if (self->csv != NULL && self->csv->fixed_width == fresh->fixed_width &&
      (strcmp (self->csv->separators, "\t") == 0) == (strcmp (fresh->separators, "\t") == 0))
    {
      prompt->options = o42_csv_options_copy (self->csv);
      if (prompt->options->separators[0] == '\0')
        o42_csv_options_set_separators (prompt->options, fresh->separators);
      o42_csv_options_free (fresh);
    }
  else
    prompt->options = fresh;
  o = prompt->options;

  base = g_file_get_basename (file);
  title = g_strdup_printf (_("Save %s as Text"), base);
  prompt->dialog = dialog_frame (self, title, TRUE, &content, &buttons);
  g_free (title);
  g_free (base);

  /* Excel 97's warning, said before rather than after. */
  if (o42_book_n_sheets (self->book) > 1)
    {
      char *said = g_strdup_printf (_("A text file holds one sheet. Only the sheet on show, "
                                      "\342\200\234%s\342\200\235, will be saved; the book keeps "
                                      "the others until it is closed."),
                                    o42_sheet_get_name (self->sheet));

      gtk_box_append (GTK_BOX (content), prose (said));
      g_free (said);
    }

  /* What goes between the fields. */
  seps = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  for (int i = 0; i < 4; i++)
    {
      prompt->sep[i] = gtk_check_button_new_with_mnemonic (_(NAMES[i]));
      if (i > 0)
        gtk_check_button_set_group (GTK_CHECK_BUTTON (prompt->sep[i]), GTK_CHECK_BUTTON (prompt->sep[0]));
      gtk_box_append (GTK_BOX (row), prompt->sep[i]);
    }
  prompt->sep[4] = gtk_check_button_new_with_mnemonic (_("_Other:"));
  gtk_check_button_set_group (GTK_CHECK_BUTTON (prompt->sep[4]), GTK_CHECK_BUTTON (prompt->sep[0]));
  gtk_box_append (GTK_BOX (row), prompt->sep[4]);
  prompt->other = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->other), 3);
  gtk_editable_set_max_width_chars (GTK_EDITABLE (prompt->other), 3);
  gtk_entry_set_max_length (GTK_ENTRY (prompt->other), 1);
  gtk_box_append (GTK_BOX (row), prompt->other);
  gtk_box_append (GTK_BOX (seps), row);
  prompt->fixed = gtk_check_button_new_with_mnemonic (_("_Fixed width - the columns padded with spaces"));
  gtk_check_button_set_group (GTK_CHECK_BUTTON (prompt->fixed), GTK_CHECK_BUTTON (prompt->sep[0]));
  gtk_box_append (GTK_BOX (seps), prompt->fixed);
  gtk_box_append (GTK_BOX (content), framed (_("Between the fields"), seps));

  sep = o->separators != NULL ? o->separators : ",";
  if (o->fixed_width)
    gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->fixed), TRUE);
  else if (*sep == ',' || *sep == '\0')
    gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->sep[0]), TRUE);
  else if (*sep == ';')
    gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->sep[1]), TRUE);
  else if (*sep == '\t')
    gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->sep[2]), TRUE);
  else if (*sep == ' ')
    gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->sep[3]), TRUE);
  else
    {
      char first[8] = { 0 };

      memcpy (first, sep, MIN ((gsize) (g_utf8_next_char (sep) - sep), sizeof first - 1));
      gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->sep[4]), TRUE);
      gtk_editable_set_text (GTK_EDITABLE (prompt->other), first);
    }
  g_signal_connect (prompt->other, "changed", G_CALLBACK (on_save_text_other), prompt);

  /* The rest, on a grid. */
  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);

  label = gtk_label_new_with_mnemonic (_("Text _qualifier:"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
  gtk_string_list_append (quotes, "\"");
  gtk_string_list_append (quotes, "'");
  gtk_string_list_append (quotes, _("{none}"));
  prompt->quote = gtk_drop_down_new (G_LIST_MODEL (quotes), NULL);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->quote), o->quote == '"' ? 0 : o->quote == '\'' ? 1 : 2);
  gtk_widget_set_halign (prompt->quote, GTK_ALIGN_START);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), prompt->quote);
  gtk_grid_attach (GTK_GRID (grid), prompt->quote, 1, 0, 1, 1);

  label = gtk_label_new_with_mnemonic (_("_Encoding:"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 1, 1);
  prompt->encoding = encoding_drop (o->encoding != NULL ? o->encoding : "UTF-8", prompt->encodings);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), prompt->encoding);
  gtk_grid_attach (GTK_GRID (grid), prompt->encoding, 1, 1, 1, 1);
  prompt->bom = gtk_check_button_new_with_mnemonic (_("With a _byte-order mark, as Excel's \"CSV UTF-8\" and "
                                                      "\"Unicode Text\" have"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->bom), o->bom);
  gtk_grid_attach (GTK_GRID (grid), prompt->bom, 1, 2, 1, 1);

  label = gtk_label_new_with_mnemonic (_("_Lines end:"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 3, 1, 1);
  for (int i = 0; ENDS[i] != NULL; i++)
    gtk_string_list_append (ends, _(ENDS[i]));
  prompt->ends = gtk_drop_down_new (G_LIST_MODEL (ends), NULL);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->ends), o->crlf ? 0 : 1);
  gtk_widget_set_halign (prompt->ends, GTK_ALIGN_START);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), prompt->ends);
  gtk_grid_attach (GTK_GRID (grid), prompt->ends, 1, 3, 1, 1);

  /* A file read with a decimal comma is written with one. */
  label = gtk_label_new_with_mnemonic (_("_Decimal separator:"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 4, 1, 1);
  {
    static const char *const DECIMALS[] = { N_("Point (1,234.50)"), N_("Comma (1.234,50)"), NULL };
    GtkStringList *decimals = gtk_string_list_new (NULL);

    for (int i = 0; DECIMALS[i] != NULL; i++)
      gtk_string_list_append (decimals, _(DECIMALS[i]));
    prompt->decimal = gtk_drop_down_new (G_LIST_MODEL (decimals), NULL);
  }
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->decimal), o->decimal == ',' ? 1 : 0);
  gtk_widget_set_halign (prompt->decimal, GTK_ALIGN_START);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), prompt->decimal);
  gtk_grid_attach (GTK_GRID (grid), prompt->decimal, 1, 4, 1, 1);
  gtk_box_append (GTK_BOX (content), grid);

  prompt->as_shown = gtk_check_button_new_with_mnemonic (_("Save cell contents as _shown"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->as_shown), o->as_shown);
  gtk_box_append (GTK_BOX (content), prompt->as_shown);
  prompt->formulas = gtk_check_button_new_with_mnemonic (_("Save _formulas rather than what they work out to"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->formulas), o->formulas);
  gtk_box_append (GTK_BOX (content), prompt->formulas);
  prompt->quote_all = gtk_check_button_new_with_mnemonic (_("_Quote all text cells"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->quote_all), o->quote_all);
  gtk_box_append (GTK_BOX (content), prompt->quote_all);

  g_signal_connect (prompt->fixed, "toggled", G_CALLBACK (on_save_text_kind), prompt);
  on_save_text_kind (NULL, prompt);

  ok = dialog_button (buttons, _("_Save"), G_CALLBACK (on_save_text_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_save_text_destroy), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}
