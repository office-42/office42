/* o42-value.c - see o42-value.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-value.h"

#include "o42-numfmt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void
o42_value_clear (O42Value *value)
{
  if (value == NULL)
    return;

  if (value->type == O42_VALUE_TEXT)
    g_clear_pointer (&value->as.text, g_free);

  value->type = O42_VALUE_EMPTY;
}

O42Value
o42_value_copy (const O42Value *value)
{
  O42Value copy = *value;

  if (value->type == O42_VALUE_TEXT)
    copy.as.text = g_strdup (value->as.text);

  return copy;
}

O42Value
o42_value_empty (void)
{
  O42Value v = { O42_VALUE_EMPTY, { 0 } };
  return v;
}

O42Value
o42_value_number (double number)
{
  O42Value v = { O42_VALUE_NUMBER, { 0 } };
  v.as.number = number;
  return v;
}

O42Value
o42_value_text (const char *text)
{
  O42Value v = { O42_VALUE_TEXT, { 0 } };
  v.as.text = g_strdup (text != NULL ? text : "");
  return v;
}

O42Value
o42_value_take (char *text)
{
  O42Value v = { O42_VALUE_TEXT, { 0 } };
  v.as.text = text != NULL ? text : g_strdup ("");
  return v;
}

O42Value
o42_value_bool (gboolean boolean)
{
  O42Value v = { O42_VALUE_BOOL, { 0 } };
  v.as.boolean = boolean;
  return v;
}

O42Value
o42_value_error (O42ErrorCode code)
{
  O42Value v = { O42_VALUE_ERROR, { 0 } };
  v.as.error = code;
  return v;
}

gboolean
o42_value_is_error (const O42Value *value)
{
  return value != NULL && value->type == O42_VALUE_ERROR;
}

const char *
o42_error_name (O42ErrorCode code)
{
  switch (code)
    {
    case O42_ERR_NULL:     return "#NULL!";
    case O42_ERR_DIV0:     return "#DIV/0!";
    case O42_ERR_VALUE:    return "#VALUE!";
    case O42_ERR_REF:      return "#REF!";
    case O42_ERR_NAME:     return "#NAME?";
    case O42_ERR_NUM:      return "#NUM!";
    case O42_ERR_NA:       return "#N/A";
    case O42_ERR_CIRCULAR: return "#CIRCULAR!";
    case O42_ERR_SPILL:    return "#SPILL!";
    default:               return "#ERR!";
    }
}

/* ---------------------------------------------------------------------- */
/* Coercion                                                                */
/* ---------------------------------------------------------------------- */

gboolean
o42_value_to_number (const O42Value *value, double *out, O42ErrorCode *error)
{
  switch (value->type)
    {
    case O42_VALUE_EMPTY:
      *out = 0.0;
      return TRUE;

    case O42_VALUE_NUMBER:
      *out = value->as.number;
      return TRUE;

    case O42_VALUE_BOOL:
      *out = value->as.boolean ? 1.0 : 0.0;
      return TRUE;

    case O42_VALUE_TEXT:
      {
        const char *s = value->as.text;
        char *end = NULL;
        double n;

        while (g_ascii_isspace (*s))
          s++;

        if (*s == '\0')
          {
            /* Empty text is not zero: "" + 1 is an error in Excel, and the
             * distinction is the difference between a blank cell and a cell
             * holding a blank string. */
            if (error) *error = O42_ERR_VALUE;
            return FALSE;
          }

        n = g_ascii_strtod (s, &end);

        while (end != NULL && g_ascii_isspace (*end))
          end++;

        /* Only a string that is *entirely* a number counts.  "12abc" is
         * text, not twelve. */
        if (end == NULL || *end != '\0')
          {
            if (error) *error = O42_ERR_VALUE;
            return FALSE;
          }

        *out = n;
        return TRUE;
      }

    case O42_VALUE_ERROR:
    default:
      if (error) *error = value->as.error;
      return FALSE;
    }
}

char *
o42_value_to_text (const O42Value *value)
{
  switch (value->type)
    {
    case O42_VALUE_EMPTY:  return g_strdup ("");
    case O42_VALUE_TEXT:   return g_strdup (value->as.text);
    case O42_VALUE_BOOL:   return g_strdup (value->as.boolean ? "TRUE" : "FALSE");
    case O42_VALUE_ERROR:  return g_strdup (o42_error_name (value->as.error));
    case O42_VALUE_NUMBER:
    default:               return o42_value_display (value);
    }
}

gboolean
o42_value_to_bool (const O42Value *value, gboolean *out, O42ErrorCode *error)
{
  double n;

  switch (value->type)
    {
    case O42_VALUE_BOOL:
      *out = value->as.boolean;
      return TRUE;

    case O42_VALUE_EMPTY:
      *out = FALSE;
      return TRUE;

    case O42_VALUE_NUMBER:
      *out = value->as.number != 0.0;
      return TRUE;

    case O42_VALUE_TEXT:
      if (g_ascii_strcasecmp (value->as.text, "TRUE") == 0)  { *out = TRUE;  return TRUE; }
      if (g_ascii_strcasecmp (value->as.text, "FALSE") == 0) { *out = FALSE; return TRUE; }
      if (o42_value_to_number (value, &n, error))
        {
          *out = n != 0.0;
          return TRUE;
        }
      if (error) *error = O42_ERR_VALUE;
      return FALSE;

    case O42_VALUE_ERROR:
    default:
      if (error) *error = value->as.error;
      return FALSE;
    }
}

/* A number sorts before text, which sorts before a boolean.  That ordering
 * is Excel's, and it is why =1<"a" is TRUE. */
static int
type_rank (const O42Value *v)
{
  switch (v->type)
    {
    case O42_VALUE_EMPTY:  return 0;
    case O42_VALUE_NUMBER: return 1;
    case O42_VALUE_TEXT:   return 2;
    case O42_VALUE_BOOL:   return 3;
    default:               return 4;
    }
}

int
o42_value_compare (const O42Value *a, const O42Value *b)
{
  /* An empty cell compared against a number is zero, and against text is the
   * empty string; comparing two empties makes them equal. */
  if (a->type == O42_VALUE_EMPTY && b->type == O42_VALUE_NUMBER)
    {
      double zero = 0.0;
      return (zero < b->as.number) ? -1 : (zero > b->as.number) ? 1 : 0;
    }
  if (b->type == O42_VALUE_EMPTY && a->type == O42_VALUE_NUMBER)
    {
      double zero = 0.0;
      return (a->as.number < zero) ? -1 : (a->as.number > zero) ? 1 : 0;
    }
  if (a->type == O42_VALUE_EMPTY && b->type == O42_VALUE_TEXT)
    return (*b->as.text == '\0') ? 0 : -1;
  if (b->type == O42_VALUE_EMPTY && a->type == O42_VALUE_TEXT)
    return (*a->as.text == '\0') ? 0 : 1;

  if (a->type != b->type)
    {
      int ra = type_rank (a);
      int rb = type_rank (b);
      return (ra < rb) ? -1 : (ra > rb) ? 1 : 0;
    }

  switch (a->type)
    {
    case O42_VALUE_NUMBER:
      return (a->as.number < b->as.number) ? -1
           : (a->as.number > b->as.number) ? 1 : 0;

    case O42_VALUE_TEXT:
      {
        /* Comparison in a spreadsheet ignores case. */
        char *la = g_utf8_casefold (a->as.text, -1);
        char *lb = g_utf8_casefold (b->as.text, -1);
        int result = g_utf8_collate (la, lb);

        g_free (la);
        g_free (lb);
        return (result < 0) ? -1 : (result > 0) ? 1 : 0;
      }

    case O42_VALUE_BOOL:
      return (a->as.boolean == b->as.boolean) ? 0
           : (a->as.boolean ? 1 : -1);

    case O42_VALUE_ERROR:
      return (a->as.error == b->as.error) ? 0 : 1;

    case O42_VALUE_EMPTY:
    default:
      return 0;
    }
}

char *
o42_value_display (const O42Value *value)
{
  if (value->type != O42_VALUE_NUMBER)
    return o42_value_to_text (value);

  return o42_number_format (value->as.number, O42_NUM_GENERAL, 0);
}
