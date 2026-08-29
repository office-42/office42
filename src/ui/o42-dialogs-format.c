/* o42-dialogs-format.c - see o42-window-private.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-window-private.h"

#include "o42-types.h"

#include <glib/gi18n.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The names these dialogs have always called them by. */
#define colour_from_rgba o42_colour_from_rgba
#define dialog_button o42_dialog_button
#define dialog_frame o42_dialog_frame
#define drop_down_of o42_drop_down_of
#define labelled o42_labelled
#define dialogs_modal o42_dialogs_modal
#define colour_button o42_colour_button
#define on_dialog_close_clicked o42_dialog_close_clicked
#define on_dialog_destroy_refocus o42_dialog_destroy_refocus
#define page_grid o42_page_grid
#define rgba_from_colour o42_rgba_from_colour
#define show_error o42_window_show_error
#define window_sync o42_window_sync
#define window_tell_book o42_window_tell_book
#define wizard_bind_item o42_wizard_bind_item
#define wizard_setup_item o42_wizard_setup_item

/* ---- Format Cells ----------------------------------------------------- */

/* Excel 5's Format Cells: one dialog, tabbed, holding everything a cell's
 * format can be.  Every control starts from the active cell and the whole
 * format is applied to the selection on OK. */
typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *number, *decimals, *custom;
  GtkWidget *halign, *valign, *wrap;
  GtkWidget *family, *size, *bold, *italic, *underline, *strikeout, *colour;
  GtkWidget *border[4];
  GtkWidget *border_style[4], *border_colour;
  GtkWidget *indent, *rotation;
  GtkWidget *no_fill, *fill, *pattern, *pattern_colour;
  GtkWidget *locked, *hidden;
} FormatPrompt;

static const char *BORDER_STYLE_NAMES[] = { N_("None"), N_("Thin"), N_("Medium"), N_("Thick"), N_("Double"), N_("Dashed"), N_("Dotted"), NULL };

static const O42NumberFormat NUMBER_CHOICES[] = {
  O42_NUM_GENERAL, O42_NUM_FIXED, O42_NUM_COMMA, O42_NUM_CURRENCY,
  O42_NUM_PERCENT, O42_NUM_SCIENTIFIC, O42_NUM_TEXT, O42_NUM_DATE,
  O42_NUM_TIME, O42_NUM_DATETIME,
};
static const char *NUMBER_NAMES[] = {
  N_("General"), N_("Fixed"), N_("Comma"), N_("Currency"), N_("Percent"), N_("Scientific"), N_("Text"),
  N_("Date"), N_("Time"), N_("Date and Time"), N_("Custom"), NULL,
};
static const char *HALIGN_NAMES[] = { N_("General"), N_("Left"), N_("Center"), N_("Right"), NULL };
static const char *VALIGN_NAMES[] = { N_("Bottom"), N_("Middle"), N_("Top"), NULL };

void
o42_rgba_from_colour (guint32 colour, GdkRGBA *rgba)
{
  rgba->red   = ((colour >> 16) & 0xff) / 255.0;
  rgba->green = ((colour >> 8) & 0xff) / 255.0;
  rgba->blue  = (colour & 0xff) / 255.0;
  rgba->alpha = 1.0;
}

guint32
o42_colour_from_rgba (const GdkRGBA *rgba)
{
  return ((guint32) (rgba->red * 255 + 0.5) << 16) |
         ((guint32) (rgba->green * 255 + 0.5) << 8) |
          (guint32) (rgba->blue * 255 + 0.5);
}

GtkWidget *
o42_labelled (GtkWidget *grid, int row, const char *label, GtkWidget *control)
{
  GtkWidget *l = gtk_label_new (label);

  gtk_label_set_xalign (GTK_LABEL (l), 0.0);
  gtk_grid_attach (GTK_GRID (grid), l, 0, row, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), control, 1, row, 1, 1);
  return control;
}

GtkWidget *
o42_page_grid (GtkWidget *notebook, const char *title)
{
  GtkWidget *grid = gtk_grid_new ();

  gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  gtk_widget_set_margin_top (grid, 12);
  gtk_widget_set_margin_bottom (grid, 12);
  gtk_widget_set_margin_start (grid, 12);
  gtk_widget_set_margin_end (grid, 12);
  gtk_notebook_append_page (GTK_NOTEBOOK (notebook), grid, gtk_label_new (title));
  return grid;
}

static void
on_format_ok (GtkWidget *w, gpointer data)
{
  FormatPrompt *prompt = data;
  O42Fmt fmt;
  guint index;
  GtkStringObject *item;
  const GdkRGBA *rgba;

  (void) w;
  o42_fmt_init_default (&fmt);

  index = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->number));
  fmt.number = (index < G_N_ELEMENTS (NUMBER_CHOICES)) ? NUMBER_CHOICES[index] : O42_NUM_GENERAL;
  fmt.decimals = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->decimals));
  fmt.custom = NULL;
  if (index == G_N_ELEMENTS (NUMBER_CHOICES))
    {
      const char *code = gtk_editable_get_text (GTK_EDITABLE (prompt->custom));
      if (*code != '\0' && g_ascii_strcasecmp (code, "General") != 0)
        fmt.custom = g_intern_string (code);
    }

  fmt.halign = (O42HAlign) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->halign));
  switch (gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->valign)))
    {
    case 1:  fmt.valign = O42_VALIGN_MIDDLE; break;
    case 2:  fmt.valign = O42_VALIGN_TOP;    break;
    default: fmt.valign = O42_VALIGN_BOTTOM; break;
    }
  fmt.wrap = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->wrap));

  item = gtk_drop_down_get_selected_item (GTK_DROP_DOWN (prompt->family));
  if (item != NULL)
    fmt.family = g_intern_string (gtk_string_object_get_string (item));
  fmt.size = (int) (gtk_spin_button_get_value (GTK_SPIN_BUTTON (prompt->size)) * 2 + 0.5);
  fmt.bold = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->bold));
  fmt.italic = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->italic));
  fmt.underline = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->underline));
  fmt.strikeout = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->strikeout));
  rgba = gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->colour));
  fmt.colour = colour_from_rgba (rgba);

  {
    guint32 bc = colour_from_rgba (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->border_colour)));
    for (int i = 0; i < 4; i++)
      {
        guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->border_style[i]));
        fmt.border_style[i] = sel == GTK_INVALID_LIST_POSITION ? O42_BORDER_NONE : (O42BorderStyle) sel;
        fmt.border_colour[i] = bc;
      }
    o42_fmt_sync_borders (&fmt);
  }
  fmt.pattern = (guint8) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->pattern));
  fmt.pattern_colour = colour_from_rgba (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->pattern_colour)));
  fmt.locked = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->locked));
  fmt.hidden = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->hidden));
  fmt.indent = (guint8) gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->indent));
  fmt.rotation = (gint16) gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (prompt->rotation));

  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->no_fill)))
    fmt.fill = O42_FILL_NONE;
  else
    fmt.fill = colour_from_rgba (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->fill)));

  o42_grid_apply_fmt (prompt->window->grid,
                      O42_FMT_FAMILY | O42_FMT_SIZE | O42_FMT_BOLD | O42_FMT_ITALIC |
                      O42_FMT_UNDERLINE | O42_FMT_STRIKEOUT | O42_FMT_COLOUR |
                      O42_FMT_FILL | O42_FMT_HALIGN | O42_FMT_VALIGN |
                      O42_FMT_NUMBER | O42_FMT_DECIMALS | O42_FMT_WRAP |
                      O42_FMT_BORDERS | O42_FMT_INDENT | O42_FMT_ROTATION |
                      O42_FMT_PROTECTION | O42_FMT_PATTERN,
                      &fmt);

  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

GtkWidget *
o42_colour_button (guint32 colour, const char *title)
{
  GtkColorDialog *dialog = gtk_color_dialog_new ();
  GtkWidget *button;
  GdkRGBA rgba;

  gtk_color_dialog_set_title (dialog, title);
  gtk_color_dialog_set_with_alpha (dialog, FALSE);
  button = gtk_color_dialog_button_new (dialog);
  rgba_from_colour (colour, &rgba);
  gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (button), &rgba);
  return button;
}

void
action_format_cells (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  FormatPrompt *prompt = g_new0 (FormatPrompt, 1);
  const O42Fmt *fmt = o42_grid_active_fmt (self->grid);
  GtkWidget *content, *buttons, *notebook, *page, *ok;
  O42Fmt fallback;

  (void) a; (void) p;

  if (fmt == NULL)
    {
      o42_fmt_init_default (&fallback);
      fmt = &fallback;
    }

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Format Cells"), TRUE, &content, &buttons);
  notebook = gtk_notebook_new ();
  gtk_box_append (GTK_BOX (content), notebook);

  /* Number */
  page = page_grid (notebook, _("Number"));
  prompt->number = labelled (page, 0, _("Category:"), drop_down_of (NUMBER_NAMES));
  for (guint i = 0; i < G_N_ELEMENTS (NUMBER_CHOICES); i++)
    if (NUMBER_CHOICES[i] == fmt->number)
      gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->number), i);
  if (fmt->custom != NULL)
    gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->number), G_N_ELEMENTS (NUMBER_CHOICES));
  prompt->decimals = labelled (page, 1, _("Decimal places:"), gtk_spin_button_new_with_range (0, 15, 1));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->decimals), fmt->decimals);
  prompt->custom = labelled (page, 2, _("Format code:"), gtk_entry_new ());
  {
    char *code = o42_fmt_format_string (fmt);
    gtk_editable_set_text (GTK_EDITABLE (prompt->custom), code);
    g_free (code);
  }
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->custom), 24);

  /* Alignment */
  page = page_grid (notebook, _("Alignment"));
  prompt->halign = labelled (page, 0, _("Horizontal:"), drop_down_of (HALIGN_NAMES));
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->halign), (guint) fmt->halign);
  prompt->valign = labelled (page, 1, _("Vertical:"), drop_down_of (VALIGN_NAMES));
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->valign),
                              fmt->valign == O42_VALIGN_MIDDLE ? 1 : fmt->valign == O42_VALIGN_TOP ? 2 : 0);
  prompt->wrap = gtk_check_button_new_with_mnemonic ( _("_Wrap text"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->wrap), fmt->wrap);
  gtk_grid_attach (GTK_GRID (page), prompt->wrap, 0, 2, 2, 1);
  prompt->indent = labelled (page, 3, _("Indent:"), gtk_spin_button_new_with_range (0, 15, 1));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->indent), fmt->indent);
  prompt->rotation = labelled (page, 4, _("Orientation (degrees):"), gtk_spin_button_new_with_range (-90, 90, 5));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->rotation), fmt->rotation);

  /* Font */
  page = page_grid (notebook, _("Font"));
  prompt->family = labelled (page, 0, _("Font:"), gtk_drop_down_new (g_object_ref (self->families), NULL));
  gtk_drop_down_set_enable_search (GTK_DROP_DOWN (prompt->family), TRUE);
  if (fmt->family != NULL)
    {
      gpointer found = g_hash_table_lookup (self->family_index, fmt->family);
      if (found != NULL)
        gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->family), GPOINTER_TO_UINT (found) - 1);
    }
  prompt->size = labelled (page, 1, _("Size:"), gtk_spin_button_new_with_range (4, 144, 1));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (prompt->size), fmt->size / 2.0);
  prompt->bold = gtk_check_button_new_with_mnemonic ( _("_Bold"));
  prompt->italic = gtk_check_button_new_with_mnemonic ( _("_Italic"));
  prompt->underline = gtk_check_button_new_with_mnemonic ( _("_Underline"));
  prompt->strikeout = gtk_check_button_new_with_mnemonic ( _("Stri_kethrough"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->bold), fmt->bold);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->italic), fmt->italic);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->underline), fmt->underline);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->strikeout), fmt->strikeout);
  gtk_grid_attach (GTK_GRID (page), prompt->bold, 0, 2, 1, 1);
  gtk_grid_attach (GTK_GRID (page), prompt->italic, 1, 2, 1, 1);
  gtk_grid_attach (GTK_GRID (page), prompt->underline, 0, 3, 1, 1);
  gtk_grid_attach (GTK_GRID (page), prompt->strikeout, 1, 3, 1, 1);
  prompt->colour = labelled (page, 4, _("Color:"), colour_button (fmt->colour, _("Font Color")));

  /* Border: a style per side, one colour for all. */
  page = page_grid (notebook, _("Border"));
  {
    static const char *names[4] = { "Top:", "Bottom:", "Left:", "Right:" };

    for (int i = 0; i < 4; i++)
      {
        prompt->border_style[i] = labelled (page, i, names[i], drop_down_of (BORDER_STYLE_NAMES));
        gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->border_style[i]), fmt->border_style[i]);
        prompt->border[i] = prompt->border_style[i];
      }
    prompt->border_colour = labelled (page, 4, _("Color:"), colour_button (fmt->border_colour[0], _("Border Color")));
  }

  /* Protection */
  page = page_grid (notebook, _("Protection"));
  prompt->locked = gtk_check_button_new_with_mnemonic ( _("_Locked"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->locked), fmt->locked);
  gtk_grid_attach (GTK_GRID (page), prompt->locked, 0, 0, 2, 1);
  prompt->hidden = gtk_check_button_new_with_mnemonic ( _("_Hidden"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->hidden), fmt->hidden);
  gtk_grid_attach (GTK_GRID (page), prompt->hidden, 0, 1, 2, 1);
  {
    GtkWidget *hint = gtk_label_new ("Both take effect only while the sheet is protected, "
                                     "under Tools > Protect Sheet.");
    gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (hint), 34);
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_grid_attach (GTK_GRID (page), hint, 0, 2, 2, 1);
  }

  /* Patterns */
  page = page_grid (notebook, _("Patterns"));
  prompt->no_fill = gtk_check_button_new_with_mnemonic ( _("_No shading"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->no_fill), fmt->fill == O42_FILL_NONE);
  gtk_grid_attach (GTK_GRID (page), prompt->no_fill, 0, 0, 2, 1);
  prompt->fill = labelled (page, 1, "Shading:",
                           colour_button (fmt->fill != O42_FILL_NONE ? fmt->fill : 0xFFFF99, _("Cell Shading")));
  {
    /* In the order of O42Pattern. */
    static const char *const patterns[] = {
      N_("None"), N_("Solid"), N_("75% grey"), N_("50% grey"), N_("25% grey"), N_("12.5% grey"), N_("6.25% grey"),
      N_("Horizontal"), N_("Vertical"), N_("Diagonal down"), N_("Diagonal up"), N_("Grid"), N_("Trellis"),
      N_("Thin horizontal"), N_("Thin vertical"), N_("Thin diagonal down"), N_("Thin diagonal up"),
      N_("Thin grid"), N_("Thin trellis"), NULL
    };

    prompt->pattern = labelled (page, 2, _("Pattern:"), drop_down_of (patterns));
    gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->pattern), fmt->pattern);
    prompt->pattern_colour = labelled (page, 3, "Pattern colour:",
                                       colour_button (fmt->pattern_colour, _("Pattern Colour")));
  }

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_format_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* ---- Format > Conditional Formatting ----------------------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *op;
  GtkWidget *value, *value2;
  GtkWidget *bold, *italic;
  GtkWidget *colour, *fill, *use_fill;
} CondPrompt;

const char *O42_COND_NAMES[] = {
  N_("between"), N_("not between"), N_("equal to"), N_("not equal to"), N_("greater than"),
  N_("less than"), N_("greater than or equal to"), N_("less than or equal to"), NULL
};

static void
on_cond_ok (GtkWidget *w, gpointer data)
{
  CondPrompt *prompt = data;
  O42Window *self = prompt->window;
  O42Condition c;

  (void) w;
  memset (&c, 0, sizeof c);
  o42_grid_get_selection (self->grid, &c.range);
  c.op = (O42CondOp) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->op));
  c.value = g_ascii_strtod (gtk_editable_get_text (GTK_EDITABLE (prompt->value)), NULL);
  c.value2 = g_ascii_strtod (gtk_editable_get_text (GTK_EDITABLE (prompt->value2)), NULL);
  o42_fmt_init_default (&c.fmt);

  c.fmt.bold = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->bold));
  c.fmt.italic = gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->italic));
  c.fmt.colour = colour_from_rgba (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->colour)));
  c.mask = O42_FMT_BOLD | O42_FMT_ITALIC | O42_FMT_COLOUR;
  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (prompt->use_fill)))
    {
      c.fmt.fill = colour_from_rgba (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (prompt->fill)));
      c.mask |= O42_FMT_FILL;
    }

  o42_sheet_add_condition (self->sheet, &c);
  o42_grid_refresh (self->grid);
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_cond_clear (GtkWidget *w, gpointer data)
{
  CondPrompt *prompt = data;
  O42Range sel;

  (void) w;
  o42_grid_get_selection (prompt->window->grid, &sel);
  o42_sheet_clear_conditions (prompt->window->sheet, &sel);
  o42_grid_refresh (prompt->window->grid);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_conditional (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  CondPrompt *prompt = g_new0 (CondPrompt, 1);
  GtkWidget *content, *buttons, *row, *ok;

  (void) a; (void) p;

  prompt->window = self;
  prompt->dialog = dialog_frame (self, _("Conditional Formatting"), TRUE, &content, &buttons);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Cell value is")));
  prompt->op = drop_down_of (O42_COND_NAMES);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (prompt->op), O42_COND_GREATER);
  gtk_box_append (GTK_BOX (row), prompt->op);
  prompt->value = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->value), 8);
  gtk_box_append (GTK_BOX (row), prompt->value);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("and")));
  prompt->value2 = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->value2), 8);
  gtk_box_append (GTK_BOX (row), prompt->value2);
  gtk_box_append (GTK_BOX (content), row);

  gtk_box_append (GTK_BOX (content), gtk_label_new (_("Then show the cell as:")));
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  prompt->bold = gtk_check_button_new_with_mnemonic ( _("_Bold"));
  prompt->italic = gtk_check_button_new_with_mnemonic ( _("_Italic"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (prompt->bold), TRUE);
  gtk_box_append (GTK_BOX (row), prompt->bold);
  gtk_box_append (GTK_BOX (row), prompt->italic);
  gtk_box_append (GTK_BOX (row), gtk_label_new (_("Colour:")));
  prompt->colour = colour_button (0xC00000, _("Text Colour"));
  gtk_box_append (GTK_BOX (row), prompt->colour);
  gtk_box_append (GTK_BOX (content), row);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  prompt->use_fill = gtk_check_button_new_with_mnemonic ( _("_Shading:"));
  gtk_box_append (GTK_BOX (row), prompt->use_fill);
  prompt->fill = colour_button (0xFFFF99, _("Cell Shading"));
  gtk_box_append (GTK_BOX (row), prompt->fill);
  gtk_box_append (GTK_BOX (content), row);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_cond_ok), prompt);
  dialog_button (buttons, _("_Clear"), G_CALLBACK (on_cond_clear), prompt);
  dialog_button (buttons, _("Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->value);
}

/* ---- Column Width and Row Height ------------------------------------- */

/* One small dialog serves both: a label, an entry, OK and Cancel, in the
 * shape Excel 5's had.  The value is in pixels at the grid's 96 dpi. */
typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *entry;
  gboolean   columns;
} SizePrompt;

static void
on_size_prompt_ok (GtkWidget *w, gpointer data)
{
  SizePrompt *prompt = data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (prompt->entry));
  char *end = NULL;
  double value = g_ascii_strtod (text, &end);

  (void) w;

  if (end != text && value > 0)
    {
      if (prompt->columns)
        o42_grid_set_column_width (prompt->window->grid, (int) (value + 0.5));
      else
        o42_grid_set_row_height (prompt->window->grid, (int) (value + 0.5));
    }

  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_size_prompt_cancel (GtkWidget *w, gpointer data)
{
  SizePrompt *prompt = data;
  (void) w;
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

static void
on_size_prompt_destroy (GtkWidget *w, gpointer data)
{
  SizePrompt *prompt = data;
  (void) w;
  gtk_widget_grab_focus (GTK_WIDGET (prompt->window->grid));
  g_free (prompt);
}

static void
size_prompt (O42Window *self, gboolean columns)
{
  SizePrompt *prompt = g_new0 (SizePrompt, 1);
  GtkWidget *box, *row, *label, *buttons, *ok, *cancel;
  int row_i, col_i, current;
  char initial[16];

  prompt->window = self;
  prompt->columns = columns;

  o42_grid_get_active (self->grid, &row_i, &col_i);
  current = columns ? o42_sheet_col_width (self->sheet, col_i)
                    : o42_sheet_row_height (self->sheet, row_i);
  g_snprintf (initial, sizeof initial, "%d", current);

  prompt->dialog = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (prompt->dialog), columns ? _("Column Width") : _("Row Height"));
  gtk_window_set_transient_for (GTK_WINDOW (prompt->dialog), GTK_WINDOW (self));
  gtk_window_set_modal (GTK_WINDOW (prompt->dialog), dialogs_modal);
  gtk_window_set_resizable (GTK_WINDOW (prompt->dialog), FALSE);
  gtk_widget_add_css_class (prompt->dialog, "o42");

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);
  gtk_window_set_child (GTK_WINDOW (prompt->dialog), box);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  label = gtk_label_new (columns ? "Column Width (pixels):" : "Row Height (pixels):");
  prompt->entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (prompt->entry), initial);
  gtk_editable_set_width_chars (GTK_EDITABLE (prompt->entry), 8);
  gtk_entry_set_activates_default (GTK_ENTRY (prompt->entry), TRUE);
  gtk_box_append (GTK_BOX (row), label);
  gtk_box_append (GTK_BOX (row), prompt->entry);
  gtk_box_append (GTK_BOX (box), row);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  ok = gtk_button_new_with_mnemonic (_("_OK"));
  cancel = gtk_button_new_with_mnemonic (_("_Cancel"));
  gtk_widget_set_size_request (ok, 80, -1);
  gtk_widget_set_size_request (cancel, 80, -1);
  gtk_box_append (GTK_BOX (buttons), ok);
  gtk_box_append (GTK_BOX (buttons), cancel);
  gtk_box_append (GTK_BOX (box), buttons);

  g_signal_connect (ok, "clicked", G_CALLBACK (on_size_prompt_ok), prompt);
  g_signal_connect (cancel, "clicked", G_CALLBACK (on_size_prompt_cancel), prompt);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_size_prompt_destroy), prompt);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);

  gtk_window_present (GTK_WINDOW (prompt->dialog));
  gtk_widget_grab_focus (prompt->entry);
  gtk_editable_select_region (GTK_EDITABLE (prompt->entry), 0, -1);
}

void action_merge_cells (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_merge_cells (O42_WINDOW (d)->grid, TRUE); }
void action_unmerge_cells (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_merge_cells (O42_WINDOW (d)->grid, FALSE); }
void action_hide_rows (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_hide_rows (O42_WINDOW (d)->grid, TRUE); }
void action_unhide_rows (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_hide_rows (O42_WINDOW (d)->grid, FALSE); }
void action_hide_columns (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_hide_columns (O42_WINDOW (d)->grid, TRUE); }
void action_unhide_columns (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_hide_columns (O42_WINDOW (d)->grid, FALSE); }
void action_filter (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_toggle_autofilter (O42_WINDOW (d)->grid); }
void action_column_width (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; size_prompt (d, TRUE); }
void action_row_height (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; size_prompt (d, FALSE); }
void action_autofit (GSimpleAction *a, GVariant *p, gpointer d) { (void)a;(void)p; o42_grid_autofit_columns (O42_WINDOW (d)->grid); }

/* ---- Format > AutoFormat, and the format painter ----------------------- */

typedef struct {
  O42Window *window;
  GtkWidget *dialog;
  GtkWidget *which;
  O42Range   range;
} AutoFormatPrompt;

static void
on_autoformat_ok (GtkWidget *w, gpointer data)
{
  AutoFormatPrompt *prompt = data;
  O42Window *self = prompt->window;

  (void) w;
  o42_sheet_auto_format (self->sheet, &prompt->range,
                         (int) gtk_drop_down_get_selected (GTK_DROP_DOWN (prompt->which)));
  o42_sheet_set_modified (self->sheet, TRUE);
  o42_grid_refresh (self->grid);
  window_sync (self);
  gtk_window_destroy (GTK_WINDOW (prompt->dialog));
}

void
action_autoformat (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;
  AutoFormatPrompt *prompt = g_new0 (AutoFormatPrompt, 1);
  GtkWidget *content, *buttons, *ok;
  const char *names[16];
  int n_looks = MIN (o42_auto_format_count (), 15);

  (void) a; (void) p;
  for (int i = 0; i < n_looks; i++)
    names[i] = o42_auto_format_name (i);
  names[n_looks] = NULL;

  prompt->window = self;
  o42_grid_get_selection (self->grid, &prompt->range);
  prompt->dialog = dialog_frame (self, _("AutoFormat"), TRUE, &content, &buttons);
  gtk_box_append (GTK_BOX (content), gtk_label_new ("A look for the selection, "
                                                    "whose first row is its heading:"));
  prompt->which = gtk_drop_down_new_from_strings (names);
  gtk_box_append (GTK_BOX (content), prompt->which);

  ok = dialog_button (buttons, _("_OK"), G_CALLBACK (on_autoformat_ok), prompt);
  dialog_button (buttons, _("_Cancel"), G_CALLBACK (on_dialog_close_clicked), prompt->dialog);
  gtk_window_set_default_widget (GTK_WINDOW (prompt->dialog), ok);
  g_signal_connect (prompt->dialog, "destroy", G_CALLBACK (on_dialog_destroy_refocus), self->grid);
  g_signal_connect_swapped (prompt->dialog, "destroy", G_CALLBACK (g_free), prompt);
  gtk_window_present (GTK_WINDOW (prompt->dialog));
}

/* The painter picks up the look of the active cell; the next click puts
 * it down. */
void
action_format_painter (GSimpleAction *a, GVariant *p, gpointer data)
{
  O42Window *self = data;

  (void) a; (void) p;
  o42_grid_pick_up_format (self->grid);
  gtk_label_set_text (GTK_LABEL (self->status_label), _("Click a cell or a range to give it that look"));
}
