/* o42-dialogs-file.c - see o42-window-private.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The File menu's dialogs for text files.  Excel 97's Text Import
 * Wizard, which every .csv, .txt and .prn is opened through and which
 * File > Import Text File brings into the sheet on show: three pages,
 * what kind of file it is, where its fields divide, and what each
 * column is to be taken as, with the file cut up underneath as each
 * answer changes it.  And the questions Save As asks before it writes
 * one: what goes between the fields, what the text is in, and whether
 * the cells are written as they show or as they are.
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
 * and what a person calls it.  The wizard's "File origin" and Save
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

/* A label that wraps, flush left, for the explanations the wizard's
 * pages open with. */
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

/* ---- File > Open and Import Text File: the Text Import Wizard ---------- */

#define PREVIEW_RECORDS 200    /* the records the preview shows, from the first row */
#define PREVIEW_MAX_CHARS 40   /* a column of the preview is no wider than this */
#define LINE_MAX_CHARS 400     /* nor a line of the file */
#define CELL_PAD 4.0
#define RULER_H 18.0
#define GUTTER_CHARS 6

enum { STEP_KIND = 0, STEP_CUT, STEP_FORMAT };

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
  int            step;

  GtkWidget *dialog, *stack, *cut_stack;
  GtkWidget *guess;
  GtkWidget *delimited, *fixed;
  GtkWidget *start, *origin;
  GtkWidget *sep[4], *other_check, *other;
  GtkWidget *merge, *quote;
  GtkWidget *kind[4], *order;
  GtkWidget *decimal, *thousands, *minus;
  GtkWidget *place;
  GtkWidget *lines_area, *table_area, *ruler_area, *format_area;
  GtkWidget *back, *next, *finish;

  GPtrArray *lines;             /* the file's lines from the first row, as they are */
  GPtrArray *rows;              /* the same cut into fields */
  GArray    *widths;            /* int, the characters each preview column is wide */
  int        longest;           /* characters in the longest line shown */
  int        sel_from, sel_to;  /* the columns step 3 is setting, inclusive */
  int        dragging;          /* the break being dragged, or -1 */
  double     char_w, line_h;
  gboolean   updating;
  gboolean   breaks_touched;    /* the breaks are the user's now, not a guess */
} ImportPrompt;

static const char *const KIND_NAMES[] = { N_("General"), N_("Text"), N_("Date"), N_("Skip Column") };
static const char *const DATE_ORDERS[] = { "MDY", "DMY", "YMD", "MYD", "DYM", "YDM", NULL };

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
  return (int) prompt->widths->len;
}

/* Where column `col` of the table preview begins, in pixels. */
static double
column_x (ImportPrompt *prompt, int col)
{
  double x = 0;

  for (int i = 0; i < col && i < column_count (prompt); i++)
    x += g_array_index (prompt->widths, int, i) * prompt->char_w + 2 * CELL_PAD + 1;
  return x;
}

static int
column_at (ImportPrompt *prompt, double x)
{
  for (int i = 0; i < column_count (prompt); i++)
    if (x < column_x (prompt, i + 1))
      return i;
  return column_count (prompt) - 1;
}

/* The name at the top of a column on the last page: its kind, or the
 * order of its date. */
static const char *
kind_label (const O42CsvColumn *column)
{
  if (column->type == O42_SPLIT_DATE)
    return DATE_ORDERS[CLAMP ((int) column->order, 0, 5)];
  return _(KIND_NAMES[CLAMP ((int) column->type, 0, 3)]);
}

/* The areas' sizes follow what they show, so the scrolled windows
 * around them know how far to scroll. */
static void
import_size_areas (ImportPrompt *prompt)
{
  int rows = (int) prompt->rows->len;
  int table_w = (int) ceil (column_x (prompt, column_count (prompt))) + 2;
  int text_w = (int) ((prompt->longest + GUTTER_CHARS + 2) * prompt->char_w);

  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (prompt->lines_area), text_w);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (prompt->lines_area),
                                       (int) ((prompt->lines->len + 1) * prompt->line_h));
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (prompt->table_area), table_w);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (prompt->table_area),
                                       (int) ((rows + 1) * prompt->line_h));
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (prompt->format_area), table_w);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (prompt->format_area),
                                       (int) ((rows + 2) * prompt->line_h));
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (prompt->ruler_area),
                                      (int) ((prompt->longest + 12) * prompt->char_w));
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (prompt->ruler_area),
                                       (int) (RULER_H + 4 + (prompt->lines->len + 1) * prompt->line_h));
}

/* The preview worked out again from the options as they stand. */
static void
import_recut (ImportPrompt *prompt)
{
  const char *from = prompt->text;

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
  prompt->rows = o42_csv_preview (prompt->text, prompt->options, PREVIEW_RECORDS);

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
  /* Wide enough for "Skip Column" over it, on the last page. */
  for (guint c = 0; c < prompt->widths->len; c++)
    {
      O42CsvColumn column = o42_csv_options_column (prompt->options, (int) c);
      int w = (int) g_utf8_strlen (kind_label (&column), -1);

      if (w > g_array_index (prompt->widths, int, c))
        g_array_index (prompt->widths, int, c) = w;
    }
  if (prompt->sel_from >= column_count (prompt))
    prompt->sel_from = prompt->sel_to = 0;

  import_size_areas (prompt);
  gtk_widget_queue_draw (prompt->lines_area);
  gtk_widget_queue_draw (prompt->table_area);
  gtk_widget_queue_draw (prompt->ruler_area);
  gtk_widget_queue_draw (prompt->format_area);
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

/* ---- The three previews ---- */

static void
lines_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  ImportPrompt *prompt = data;
  PangoLayout *layout = preview_layout (GTK_WIDGET (area), prompt);
  double text_x = (GUTTER_CHARS + 1) * prompt->char_w;

  (void) height;
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_paint (cr);
  cairo_set_source_rgb (cr, 0.92, 0.92, 0.92);
  cairo_rectangle (cr, 0, 0, text_x - prompt->char_w / 2, height);
  cairo_fill (cr);

  for (guint i = 0; i < prompt->lines->len; i++)
    {
      char number[16];
      char *line = shown (g_ptr_array_index (prompt->lines, i), LINE_MAX_CHARS);
      int nw, nh;

      g_snprintf (number, sizeof number, "%d", prompt->options->start_row + (int) i + 1);
      pango_layout_set_text (layout, number, -1);
      pango_layout_get_pixel_size (layout, &nw, &nh);
      cairo_set_source_rgb (cr, 0.4, 0.4, 0.4);
      cairo_move_to (cr, text_x - prompt->char_w - nw, 2 + i * prompt->line_h);
      pango_cairo_show_layout (cr, layout);

      pango_layout_set_text (layout, line, -1);
      cairo_set_source_rgb (cr, 0, 0, 0);
      cairo_move_to (cr, text_x, 2 + i * prompt->line_h);
      pango_cairo_show_layout (cr, layout);
      g_free (line);
    }
  (void) width;
  g_object_unref (layout);
}

/* The records cut into columns, with lines between them; on the last
 * page with each column's kind above it and the chosen ones in
 * reverse, as Excel shows them. */
static void
table_draw_with (GtkDrawingArea *area, cairo_t *cr, int width, int height,
                 ImportPrompt *prompt, gboolean headed)
{
  PangoLayout *layout = preview_layout (GTK_WIDGET (area), prompt);
  double top = headed ? prompt->line_h + 2 : 0;
  int n = column_count (prompt);

  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_paint (cr);

  for (int c = 0; c < n; c++)
    {
      O42CsvColumn column = o42_csv_options_column (prompt->options, c);
      double x = column_x (prompt, c), w = column_x (prompt, c + 1) - x - 1;
      gboolean chosen = headed && c >= prompt->sel_from && c <= prompt->sel_to;

      if (headed)
        {
          cairo_set_source_rgb (cr, 0.85, 0.85, 0.85);
          cairo_rectangle (cr, x, 0, w, top - 1);
          cairo_fill (cr);
          pango_layout_set_text (layout, kind_label (&column), -1);
          cairo_set_source_rgb (cr, 0, 0, 0);
          cairo_move_to (cr, x + CELL_PAD, 1);
          pango_cairo_show_layout (cr, layout);
        }
      if (chosen)
        {
          cairo_set_source_rgb (cr, 0.0, 0.0, 0.5);
          cairo_rectangle (cr, x, top, w, height - top);
          cairo_fill (cr);
        }
      for (guint r = 0; r < prompt->rows->len; r++)
        {
          char **fields = g_ptr_array_index (prompt->rows, r);
          char *text;
          int k;

          for (k = 0; k < c && fields[k] != NULL; k++)
            ;
          if (k < c || fields[c] == NULL)
            continue;
          text = shown (fields[c], PREVIEW_MAX_CHARS);
          pango_layout_set_text (layout, text, -1);
          if (chosen)
            cairo_set_source_rgb (cr, 1, 1, 1);
          else if (headed && column.type == O42_SPLIT_SKIP)
            cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
          else
            cairo_set_source_rgb (cr, 0, 0, 0);
          cairo_move_to (cr, x + CELL_PAD, top + 1 + r * prompt->line_h);
          pango_cairo_show_layout (cr, layout);
          g_free (text);
        }
      cairo_set_source_rgb (cr, 0.3, 0.3, 0.3);
      cairo_rectangle (cr, x + w, 0, 1, height);
      cairo_fill (cr);
    }
  if (headed)
    {
      cairo_set_source_rgb (cr, 0.3, 0.3, 0.3);
      cairo_rectangle (cr, 0, top - 1, width, 1);
      cairo_fill (cr);
    }
  g_object_unref (layout);
}

static void
table_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  table_draw_with (area, cr, width, height, data, FALSE);
}

static void
format_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  table_draw_with (area, cr, width, height, data, TRUE);
}

/* The fixed-width page: the lines under a ruler, a break a line with an
 * arrow on the ruler. */
static void
ruler_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  ImportPrompt *prompt = data;
  PangoLayout *layout = preview_layout (GTK_WIDGET (area), prompt);
  PangoLayout *small = gtk_widget_create_pango_layout (GTK_WIDGET (area), NULL);
  PangoFontDescription *desc = pango_font_description_from_string ("Sans 7");
  double x0 = CELL_PAD, cw = prompt->char_w;
  GArray *breaks = prompt->options->breaks;

  pango_layout_set_font_description (small, desc);
  pango_font_description_free (desc);

  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_paint (cr);

  /* A tick every character, a longer one every five, the number every
   * ten. */
  cairo_set_source_rgb (cr, 0.3, 0.3, 0.3);
  cairo_set_line_width (cr, 1);
  for (int c = 0; x0 + c * cw < width; c++)
    {
      double x = floor (x0 + c * cw) + 0.5;
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

  cairo_set_source_rgb (cr, 0, 0, 0);
  for (guint i = 0; i < prompt->lines->len; i++)
    {
      char *line = shown (g_ptr_array_index (prompt->lines, i), LINE_MAX_CHARS);

      pango_layout_set_text (layout, line, -1);
      cairo_move_to (cr, x0, RULER_H + 2 + i * prompt->line_h);
      pango_cairo_show_layout (cr, layout);
      g_free (line);
    }

  for (guint i = 0; i < breaks->len; i++)
    {
      double x = floor (x0 + g_array_index (breaks, int, i) * cw) + 0.5;

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
  g_object_unref (layout);
}

/* ---- Pointing at the previews ---- */

static int
break_near (ImportPrompt *prompt, double x)
{
  GArray *breaks = prompt->options->breaks;

  for (guint i = 0; i < breaks->len; i++)
    if (fabs (x - (CELL_PAD + g_array_index (breaks, int, i) * prompt->char_w)) <= 4)
      return (int) i;
  return -1;
}

static int
position_at (ImportPrompt *prompt, double x)
{
  return MAX (0, (int) ((x - CELL_PAD) / prompt->char_w + 0.5));
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

/* Excel's three gestures on the ruler page: a click anywhere puts in a
 * break, a drag moves one, a double-click takes one away. */
static void
on_ruler_pressed (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  ImportPrompt *prompt = data;
  int near = break_near (prompt, x);

  (void) gesture; (void) y;
  prompt->breaks_touched = TRUE;
  if (n_press == 2 && near >= 0)
    {
      g_array_remove_index (prompt->options->breaks, near);
      prompt->dragging = -1;
    }
  else if (near >= 0)
    prompt->dragging = near;
  else if (n_press == 1)
    {
      insert_break (prompt, position_at (prompt, x));
      prompt->dragging = -1;
    }
  import_recut (prompt);
}

static void
on_ruler_released (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  ImportPrompt *prompt = data;
  GArray *breaks = prompt->options->breaks;

  (void) gesture; (void) n_press; (void) x; (void) y;
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
on_ruler_motion (GtkEventControllerMotion *motion, double x, double y, gpointer data)
{
  ImportPrompt *prompt = data;

  (void) motion; (void) y;
  if (prompt->dragging < 0 || (guint) prompt->dragging >= prompt->options->breaks->len)
    return;
  g_array_index (prompt->options->breaks, int, prompt->dragging) = MAX (1, position_at (prompt, x));
  gtk_widget_queue_draw (prompt->ruler_area);
}

static void import_sync_kind (ImportPrompt *prompt);

/* A click on a column of the last page chooses it; with Shift, every
 * column from the one chosen before to this one. */
static void
on_format_pressed (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  ImportPrompt *prompt = data;
  GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
  int col = column_at (prompt, x);

  (void) n_press; (void) y;
  if (col < 0)
    return;
  if (state & GDK_SHIFT_MASK)
    {
      prompt->sel_to = col;
      if (prompt->sel_to < prompt->sel_from)
        {
          int t = prompt->sel_from;
          prompt->sel_from = prompt->sel_to;
          prompt->sel_to = t;
        }
    }
  else
    prompt->sel_from = prompt->sel_to = col;
  import_sync_kind (prompt);
  gtk_widget_queue_draw (prompt->format_area);
}

/* ---- The pages' controls ---- */

/* The column format radios show the first chosen column's. */
static void
import_sync_kind (ImportPrompt *prompt)
{
  O42CsvColumn column = o42_csv_options_column (prompt->options, prompt->sel_from);

  prompt->updating = TRUE;
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->kind[CLAMP ((int) column.type, 0, 3)]), TRUE);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->order), column.order);
  prompt->updating = FALSE;
}

static void
on_kind_toggled (GtkCheckButton *button, gpointer data)
{
  ImportPrompt *prompt = data;
  O42SplitType type = O42_SPLIT_GENERAL;
  O42DateOrder order;

  if (prompt->updating || !gtk_check_button_get_active (button))
    return;
  for (int i = 0; i < 4; i++)
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->kind[i])))
      type = (O42SplitType) i;
  order = (O42DateOrder) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->order));
  for (int c = prompt->sel_from; c <= prompt->sel_to; c++)
    o42_csv_options_set_column (prompt->options, c, type, order);
  import_recut (prompt);
}

static void
on_order_changed (GObject *drop, GParamSpec *pspec, gpointer data)
{
  ImportPrompt *prompt = data;

  (void) drop; (void) pspec;
  if (prompt->updating)
    return;
  /* Choosing an order is choosing Date. */
  if (!gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->kind[O42_SPLIT_DATE])))
    gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->kind[O42_SPLIT_DATE]), TRUE);
  else
    on_kind_toggled (GTK_CHECK_BUTTON (prompt->kind[O42_SPLIT_DATE]), prompt);
}

/* What the second page says about the separators, into the options. */
static void
on_cut_changed (GtkWidget *w, gpointer data)
{
  ImportPrompt *prompt = data;
  static const char SEPS[4] = { '\t', ';', ',', ' ' };
  GString *seps = g_string_new (NULL);
  guint quote;

  (void) w;
  if (prompt->updating)
    {
      g_string_free (seps, TRUE);
      return;
    }
  for (int i = 0; i < 4; i++)
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->sep[i])))
      g_string_append_c (seps, SEPS[i]);
  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->other_check)))
    g_string_append (seps, gtk_editable_get_text (GTK_EDITABLE (prompt->other)));
  o42_csv_options_set_separators (prompt->options, seps->str);
  g_string_free (seps, TRUE);

  prompt->options->merge_separators = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->merge));
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

static void
on_kind_of_file (GtkCheckButton *button, gpointer data)
{
  ImportPrompt *prompt = data;

  (void) button;
  if (prompt->updating)
    return;
  prompt->options->fixed_width = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->fixed));
  import_recut (prompt);
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

static void
on_number_style_changed (GtkWidget *w, gpointer data)
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
}

static void
on_number_drop_changed (GObject *drop, GParamSpec *pspec, gpointer data)
{
  (void) drop; (void) pspec;
  on_number_style_changed (NULL, data);
}

/* ---- Going from page to page ---- */

static void
import_show_step (ImportPrompt *prompt, int step)
{
  static const char *const TITLES[] = {
    N_("Text Import Wizard - Step 1 of 3"),
    N_("Text Import Wizard - Step 2 of 3"),
    N_("Text Import Wizard - Step 3 of 3"),
  };
  static const char *const PAGES[] = { "kind", "cut", "format" };

  prompt->step = CLAMP (step, STEP_KIND, STEP_FORMAT);
  gtk_window_set_title (GTK_WINDOW (prompt->dialog), _(TITLES[prompt->step]));
  gtk_stack_set_visible_child_name (GTK_STACK (prompt->stack), PAGES[prompt->step]);
  gtk_stack_set_visible_child_name (GTK_STACK (prompt->cut_stack),
                                    prompt->options->fixed_width ? "fixed" : "delimited");
  gtk_widget_set_sensitive (prompt->back, prompt->step > STEP_KIND);
  gtk_widget_set_sensitive (prompt->next, prompt->step < STEP_FORMAT);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog),
                                 prompt->step < STEP_FORMAT ? prompt->next : prompt->finish);
  if (prompt->step == STEP_FORMAT)
    import_sync_kind (prompt);
  import_recut (prompt);
}

static void
on_import_back (GtkWidget *w, gpointer data)
{
  ImportPrompt *prompt = data;

  (void) w;
  import_show_step (prompt, prompt->step - 1);
}

static void
on_import_next (GtkWidget *w, gpointer data)
{
  ImportPrompt *prompt = data;

  (void) w;
  import_show_step (prompt, prompt->step + 1);
}

static void
on_import_finish (GtkWidget *w, gpointer data)
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
      int row, col, n, widest = 0;
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
  g_array_unref (prompt->widths);
  g_free (prompt);
}

/* A preview in a scrolled window with a frame round it, both ways. */
static GtkWidget *
preview_box (GtkWidget *area, int height)
{
  GtkWidget *scrolled = gtk_scrolled_window_new ();
  GtkWidget *frame = gtk_frame_new (NULL);

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (scrolled, 620, height);
  gtk_widget_set_vexpand (scrolled, TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), area);
  gtk_frame_set_child (GTK_FRAME (frame), scrolled);
  return frame;
}

static GtkWidget *
import_page_kind (ImportPrompt *prompt)
{
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *kinds = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *row, *label;
  char *path, *said;

  prompt->guess = prose ("");
  gtk_box_append (GTK_BOX (page), prompt->guess);
  gtk_box_append (GTK_BOX (page), prose (_("If this is correct, choose Next, or choose the data type "
                                           "that best describes your data.")));

  prompt->delimited = gtk_check_button_new_with_mnemonic (
    _("_Delimited - Characters such as commas or tabs separate each field."));
  prompt->fixed = gtk_check_button_new_with_mnemonic (
    _("Fixed _width - Fields are aligned in columns with spaces between each field."));
  gtk_check_button_set_group (GTK_CHECK_BUTTON (prompt->fixed), GTK_CHECK_BUTTON (prompt->delimited));
  gtk_box_append (GTK_BOX (kinds), prompt->delimited);
  gtk_box_append (GTK_BOX (kinds), prompt->fixed);
  gtk_box_append (GTK_BOX (page), framed (_("Original data type"), kinds));
  g_signal_connect (prompt->fixed, "toggled", G_CALLBACK (on_kind_of_file), prompt);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  label = gtk_label_new_with_mnemonic (_("Start import at _row:"));
  gtk_box_append (GTK_BOX (row), label);
  prompt->start = gtk_spin_button_new_with_range (1, MAX (prompt->n_lines, 1), 1);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), prompt->start);
  gtk_box_append (GTK_BOX (row), prompt->start);
  g_signal_connect (prompt->start, "value-changed", G_CALLBACK (on_start_changed), prompt);

  label = gtk_label_new_with_mnemonic (_("File _origin:"));
  gtk_widget_set_margin_start (label, 16);
  gtk_box_append (GTK_BOX (row), label);
  prompt->origin = encoding_drop (prompt->options->encoding, prompt->encodings);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), prompt->origin);
  gtk_box_append (GTK_BOX (row), prompt->origin);
  g_signal_connect (prompt->origin, "notify::selected", G_CALLBACK (on_origin_changed), prompt);
  gtk_box_append (GTK_BOX (page), row);

  path = g_file_get_parse_name (prompt->file);
  said = g_strdup_printf (_("Preview of file %s:"), path);
  label = gtk_label_new (said);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append (GTK_BOX (page), label);
  g_free (said);
  g_free (path);

  prompt->lines_area = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (prompt->lines_area), lines_draw, prompt, NULL);
  gtk_box_append (GTK_BOX (page), preview_box (prompt->lines_area, 170));
  return page;
}

static GtkWidget *
import_page_cut (ImportPrompt *prompt)
{
  static const char *const NAMES[4] = { N_("_Tab"), N_("_Semicolon"), N_("_Comma"), N_("S_pace") };
  static const char *const QUOTES[] = { "\"", "'", N_("{none}"), NULL };
  GtkWidget *page, *delimited, *fixed, *row, *seps, *label;
  GtkStringList *quotes = gtk_string_list_new (NULL);

  prompt->cut_stack = gtk_stack_new ();

  /* Delimited. */
  delimited = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_append (GTK_BOX (delimited),
                  prose (_("This screen lets you set the delimiters your data contains. "
                           "You can see how your text is affected in the preview below.")));
  seps = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
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
  gtk_box_append (GTK_BOX (delimited), framed (_("Delimiters"), seps));

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  prompt->merge = gtk_check_button_new_with_mnemonic (_("Treat consecutive delimiters as o_ne"));
  gtk_box_append (GTK_BOX (row), prompt->merge);
  label = gtk_label_new_with_mnemonic (_("Text _qualifier:"));
  gtk_widget_set_margin_start (label, 24);
  gtk_box_append (GTK_BOX (row), label);
  for (int i = 0; QUOTES[i] != NULL; i++)
    gtk_string_list_append (quotes, i < 2 ? QUOTES[i] : _(QUOTES[i]));
  prompt->quote = gtk_drop_down_new (G_LIST_MODEL (quotes), NULL);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), prompt->quote);
  gtk_box_append (GTK_BOX (row), prompt->quote);
  gtk_box_append (GTK_BOX (delimited), row);

  label = gtk_label_new (_("Data preview"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (delimited), label);
  prompt->table_area = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (prompt->table_area), table_draw, prompt, NULL);
  gtk_box_append (GTK_BOX (delimited), preview_box (prompt->table_area, 190));
  gtk_stack_add_named (GTK_STACK (prompt->cut_stack), delimited, "delimited");

  /* Fixed width. */
  fixed = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_append (GTK_BOX (fixed),
                  prose (_("This screen lets you set field widths (column breaks). "
                           "Lines with arrows signify a column break.")));
  gtk_box_append (GTK_BOX (fixed),
                  prose (_("To CREATE a break line, click at the desired position.\n"
                           "To DELETE a break line, double click on the line.\n"
                           "To MOVE a break line, click and drag it.")));
  label = gtk_label_new (_("Data preview"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (fixed), label);
  prompt->ruler_area = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (prompt->ruler_area), ruler_draw, prompt, NULL);
  {
    GtkGesture *click = gtk_gesture_click_new ();
    GtkEventController *motion = gtk_event_controller_motion_new ();

    g_signal_connect (click, "pressed", G_CALLBACK (on_ruler_pressed), prompt);
    g_signal_connect (click, "released", G_CALLBACK (on_ruler_released), prompt);
    gtk_widget_add_controller (prompt->ruler_area, GTK_EVENT_CONTROLLER (click));
    g_signal_connect (motion, "motion", G_CALLBACK (on_ruler_motion), prompt);
    gtk_widget_add_controller (prompt->ruler_area, motion);
  }
  gtk_box_append (GTK_BOX (fixed), preview_box (prompt->ruler_area, 214));
  gtk_stack_add_named (GTK_STACK (prompt->cut_stack), fixed, "fixed");

  /* What the file was guessed to be, on the controls. */
  {
    const char *s = prompt->options->separators != NULL ? prompt->options->separators : "";
    static const char SEPS[4] = { '\t', ';', ',', ' ' };
    GString *others = g_string_new (NULL);

    for (; *s != '\0'; s = g_utf8_next_char (s))
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
  }
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->merge), prompt->options->merge_separators);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->quote),
                              prompt->options->quote == '"' ? 0 : prompt->options->quote == '\'' ? 1 : 2);

  for (int i = 0; i < 4; i++)
    g_signal_connect (prompt->sep[i], "toggled", G_CALLBACK (on_cut_changed), prompt);
  g_signal_connect (prompt->other_check, "toggled", G_CALLBACK (on_cut_changed), prompt);
  g_signal_connect (prompt->other, "changed", G_CALLBACK (on_other_text), prompt);
  g_signal_connect (prompt->merge, "toggled", G_CALLBACK (on_cut_changed), prompt);
  g_signal_connect (prompt->quote, "notify::selected", G_CALLBACK (on_quote_changed), prompt);

  page = prompt->cut_stack;
  return page;
}

static GtkWidget *
import_page_format (ImportPrompt *prompt)
{
  static const char *const KINDS[4] = { N_("_General"), N_("_Text"), N_("_Date:"),
                                        N_("Do not _import column (skip)") };
  static const char *const DECIMALS[] = { ".", ",", NULL };
  static const char *const THOUSANDS[] = { ",", ".", N_("space"), "'", N_("{none}"), NULL };
  GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  GtkWidget *kinds = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *row, *label, *expander, *advanced;
  GtkStringList *orders = gtk_string_list_new (DATE_ORDERS);
  GtkStringList *decimals = gtk_string_list_new (DECIMALS);
  GtkStringList *thousands = gtk_string_list_new (NULL);

  gtk_box_append (GTK_BOX (page),
                  prose (_("This screen lets you select each column and set the Data Format. "
                           "Click a column in the preview to choose it, Shift+click to choose several.")));

  for (int i = 0; i < 4; i++)
    {
      prompt->kind[i] = gtk_check_button_new_with_mnemonic (_(KINDS[i]));
      if (i > 0)
        gtk_check_button_set_group (GTK_CHECK_BUTTON (prompt->kind[i]), GTK_CHECK_BUTTON (prompt->kind[0]));
      if (i == O42_SPLIT_DATE)
        {
          row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
          gtk_box_append (GTK_BOX (row), prompt->kind[i]);
          prompt->order = gtk_drop_down_new (G_LIST_MODEL (orders), NULL);
          gtk_box_append (GTK_BOX (row), prompt->order);
          gtk_box_append (GTK_BOX (kinds), row);
        }
      else
        gtk_box_append (GTK_BOX (kinds), prompt->kind[i]);
      g_signal_connect (prompt->kind[i], "toggled", G_CALLBACK (on_kind_toggled), prompt);
    }
  g_signal_connect (prompt->order, "notify::selected", G_CALLBACK (on_order_changed), prompt);
  gtk_box_append (GTK_BOX (top), framed (_("Column data format"), kinds));

  gtk_box_append (GTK_BOX (right),
                  prose (_("'General' converts numeric values to numbers, date values to dates, "
                           "and all remaining values to text.")));

  /* Excel 2002's Advanced button, as an expander: how the file writes
   * its numbers. */
  advanced = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (advanced), 4);
  gtk_grid_set_column_spacing (GTK_GRID (advanced), 8);
  label = gtk_label_new_with_mnemonic (_("De_cimal separator:"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (advanced), label, 0, 0, 1, 1);
  prompt->decimal = gtk_drop_down_new (G_LIST_MODEL (decimals), NULL);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), prompt->decimal);
  gtk_grid_attach (GTK_GRID (advanced), prompt->decimal, 1, 0, 1, 1);
  label = gtk_label_new_with_mnemonic (_("T_housands separator:"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (advanced), label, 0, 1, 1, 1);
  for (int i = 0; THOUSANDS[i] != NULL; i++)
    gtk_string_list_append (thousands, i == 2 || i == 4 ? _(THOUSANDS[i]) : THOUSANDS[i]);
  prompt->thousands = gtk_drop_down_new (G_LIST_MODEL (thousands), NULL);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), prompt->thousands);
  gtk_grid_attach (GTK_GRID (advanced), prompt->thousands, 1, 1, 1, 1);
  prompt->minus = gtk_check_button_new_with_mnemonic (_("Trailing _minus for negative numbers"));
  gtk_grid_attach (GTK_GRID (advanced), prompt->minus, 0, 2, 2, 1);

  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->decimal), prompt->options->decimal == ',' ? 1 : 0);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->thousands),
                              prompt->options->thousands == ','  ? 0
                              : prompt->options->thousands == '.' ? 1
                              : prompt->options->thousands == ' ' ? 2
                              : prompt->options->thousands == '\'' ? 3 : 4);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->minus), prompt->options->trailing_minus);
  g_signal_connect (prompt->decimal, "notify::selected", G_CALLBACK (on_number_drop_changed), prompt);
  g_signal_connect (prompt->thousands, "notify::selected", G_CALLBACK (on_number_drop_changed), prompt);
  g_signal_connect (prompt->minus, "toggled", G_CALLBACK (on_number_style_changed), prompt);

  expander = gtk_expander_new_with_mnemonic (_("_Advanced"));
  gtk_widget_set_margin_top (advanced, 4);
  gtk_expander_set_child (GTK_EXPANDER (expander), advanced);
  /* Open when the guess is not what an English Excel would assume, so
   * that it is seen. */
  gtk_expander_set_expanded (GTK_EXPANDER (expander), prompt->options->decimal != '.');
  gtk_box_append (GTK_BOX (right), expander);

  /* Import Text File puts the records where the user says. */
  if (!prompt->open)
    {
      int r = 0, c = 0;
      char *here;

      row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      label = gtk_label_new_with_mnemonic (_("_Put the data at:"));
      gtk_box_append (GTK_BOX (row), label);
      prompt->place = gtk_entry_new ();
      gtk_editable_set_width_chars (GTK_EDITABLE (prompt->place), 8);
      o42_grid_get_active (prompt->window->grid, &r, &c);
      here = o42_ref_name (r, c);
      gtk_editable_set_text (GTK_EDITABLE (prompt->place), here);
      g_free (here);
      gtk_label_set_mnemonic_widget (GTK_LABEL (label), prompt->place);
      gtk_box_append (GTK_BOX (row), prompt->place);
      gtk_box_append (GTK_BOX (right), row);
    }
  gtk_widget_set_hexpand (right, TRUE);
  gtk_box_append (GTK_BOX (top), right);
  gtk_box_append (GTK_BOX (page), top);

  label = gtk_label_new (_("Data preview"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (page), label);
  prompt->format_area = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (prompt->format_area), format_draw, prompt, NULL);
  {
    GtkGesture *click = gtk_gesture_click_new ();

    g_signal_connect (click, "pressed", G_CALLBACK (on_format_pressed), prompt);
    gtk_widget_add_controller (prompt->format_area, GTK_EVENT_CONTROLLER (click));
  }
  gtk_box_append (GTK_BOX (page), preview_box (prompt->format_area, 170));
  return page;
}

void
o42_window_import_text (O42Window *self, GFile *file, gboolean open)
{
  ImportPrompt *prompt;
  GtkWidget *content, *buttons;
  GError *error = NULL;
  char *bytes = NULL, *name;
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
  g_free (name);
  o42_csv_options_set_encoding (prompt->options, found);
  prompt->options->bom = bom;

  prompt->dialog = dialog_frame (self, _("Text Import Wizard - Step 1 of 3"), TRUE, &content, &buttons);
  prompt->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (prompt->stack), GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_stack_set_vhomogeneous (GTK_STACK (prompt->stack), FALSE);
  gtk_box_append (GTK_BOX (content), prompt->stack);

  prompt->updating = TRUE;
  gtk_stack_add_named (GTK_STACK (prompt->stack), import_page_kind (prompt), "kind");
  gtk_stack_add_named (GTK_STACK (prompt->stack), import_page_cut (prompt), "cut");
  gtk_stack_add_named (GTK_STACK (prompt->stack), import_page_format (prompt), "format");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->options->fixed_width ? prompt->fixed : prompt->delimited),
                               TRUE);
  gtk_label_set_text (GTK_LABEL (prompt->guess),
                      prompt->options->fixed_width
                      ? _("The Text Wizard has determined that your data is Fixed Width.")
                      : _("The Text Wizard has determined that your data is Delimited."));
  prompt->updating = FALSE;

  dialog_button (buttons, _("Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  prompt->back = dialog_button (buttons, _("< _Back"), G_CALLBACK (on_import_back), prompt);
  prompt->next = dialog_button (buttons, _("_Next >"), G_CALLBACK (on_import_next), prompt);
  prompt->finish = dialog_button (buttons, _("_Finish"), G_CALLBACK (on_import_finish), prompt);

  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_import_destroy), prompt);

  import_show_step (prompt, STEP_KIND);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* File > Import Text File: Excel 2000's Data > Get External Data >
 * Import Text File, the wizard over a file chosen, its records put at
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
