/* o42-spell.c - checking the spelling of what is in the cells
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-spell.h"

#include <string.h>

#ifdef HAVE_HUNSPELL
#include <hunspell.h>
#endif

struct _O42Spell {
#ifdef HAVE_HUNSPELL
  Hunhandle  *handle;
  char       *encoding;    /* what the dictionary is written in */
#endif
  char       *language;
  GHashTable *ignored;     /* words to leave alone this session */
};

gboolean
o42_spell_available (void)
{
#ifdef HAVE_HUNSPELL
  return TRUE;
#else
  return FALSE;
#endif
}

#ifdef HAVE_HUNSPELL
/* Where dictionaries live.  MSYS2 and the Unixes each have their own
 * place for them, and O42_DICT_PATH overrides the lot. */
static char **
dictionary_paths (void)
{
  GPtrArray *dirs = g_ptr_array_new ();
  const char *env = g_getenv ("O42_DICT_PATH");
  const char *prefix = g_getenv ("MINGW_PREFIX");

  if (env != NULL)
    g_ptr_array_add (dirs, g_strdup (env));
  if (prefix != NULL)
    g_ptr_array_add (dirs, g_build_filename (prefix, "share", "hunspell", NULL));
  g_ptr_array_add (dirs, g_strdup ("C:/msys64/mingw64/share/hunspell"));
  g_ptr_array_add (dirs, g_strdup ("/usr/share/hunspell"));
  g_ptr_array_add (dirs, g_strdup ("/usr/share/myspell"));
  g_ptr_array_add (dirs, g_strdup ("/usr/local/share/hunspell"));
  g_ptr_array_add (dirs, NULL);
  return (char **) g_ptr_array_free (dirs, FALSE);
}

/* The names to try for a language, most particular first: en_GB, then
 * whatever en_* the machine happens to have. */
static char *
find_dictionary (const char *language, char **base_out)
{
  char **dirs = dictionary_paths ();
  char *found = NULL;

  for (int i = 0; dirs[i] != NULL && found == NULL; i++)
    {
      char *aff = g_strdup_printf ("%s/%s.aff", dirs[i], language);
      char *dic = g_strdup_printf ("%s/%s.dic", dirs[i], language);

      if (g_file_test (aff, G_FILE_TEST_EXISTS) && g_file_test (dic, G_FILE_TEST_EXISTS))
        {
          found = aff;
          *base_out = dic;
          aff = dic = NULL;
        }
      g_free (aff);
      g_free (dic);
    }

  /* Nothing under that exact name: take the first dictionary whose
   * name starts with the language's first two letters. */
  if (found == NULL && strlen (language) >= 2)
    {
      char stem[3] = { language[0], language[1], '\0' };

      for (int i = 0; dirs[i] != NULL && found == NULL; i++)
        {
          GDir *dir = g_dir_open (dirs[i], 0, NULL);
          const char *name;

          if (dir == NULL)
            continue;
          while ((name = g_dir_read_name (dir)) != NULL)
            {
              if (g_str_has_prefix (name, stem) && g_str_has_suffix (name, ".aff"))
                {
                  char *stem_path = g_strndup (name, strlen (name) - 4);
                  char *aff = g_strdup_printf ("%s/%s.aff", dirs[i], stem_path);
                  char *dic = g_strdup_printf ("%s/%s.dic", dirs[i], stem_path);

                  if (g_file_test (dic, G_FILE_TEST_EXISTS))
                    {
                      found = aff;
                      *base_out = dic;
                      aff = dic = NULL;
                    }
                  g_free (stem_path);
                  g_free (aff);
                  g_free (dic);
                  if (found != NULL)
                    break;
                }
            }
          g_dir_close (dir);
        }
    }
  g_strfreev (dirs);
  return found;
}
#endif

O42Spell *
o42_spell_new (const char *language)
{
#ifdef HAVE_HUNSPELL
  O42Spell *spell;
  char *aff = NULL, *dic = NULL;
  char *wanted = NULL;

  if (language != NULL && *language != '\0')
    wanted = g_strdup (language);
  else
    {
      const char *const *names = g_get_language_names ();

      for (int i = 0; names[i] != NULL && wanted == NULL; i++)
        if (strchr (names[i], '_') != NULL)
          wanted = g_strdup (names[i]);
      if (wanted == NULL)
        wanted = g_strdup ("en_US");
    }
  /* A locale name may carry an encoding: en_GB.UTF-8 is en_GB here. */
  {
    char *dot = strchr (wanted, '.');

    if (dot != NULL)
      *dot = '\0';
  }

  aff = find_dictionary (wanted, &dic);
  if (aff == NULL && language == NULL)
    {
      /* Nothing for the machine's own language: English, which is what
       * the dictionaries on a machine almost always include. */
      aff = find_dictionary ("en_US", &dic);
      if (aff == NULL)
        aff = find_dictionary ("en", &dic);
      if (aff != NULL)
        {
          g_free (wanted);
          wanted = g_strdup ("en_US");
        }
    }
  if (aff == NULL)
    {
      g_free (wanted);
      return NULL;
    }

  spell = g_new0 (O42Spell, 1);
  spell->handle = Hunspell_create (aff, dic);
  spell->language = wanted;
  spell->ignored = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (spell->handle != NULL)
    spell->encoding = g_strdup (Hunspell_get_dic_encoding (spell->handle));
  g_free (aff);
  g_free (dic);

  if (spell->handle == NULL)
    {
      o42_spell_free (spell);
      return NULL;
    }
  return spell;
#else
  (void) language;
  return NULL;
#endif
}

void
o42_spell_free (O42Spell *spell)
{
  if (spell == NULL)
    return;
#ifdef HAVE_HUNSPELL
  if (spell->handle != NULL)
    Hunspell_destroy (spell->handle);
  g_free (spell->encoding);
#endif
  g_free (spell->language);
  if (spell->ignored != NULL)
    g_hash_table_unref (spell->ignored);
  g_free (spell);
}

const char *
o42_spell_language (O42Spell *spell)
{
  return spell != NULL ? spell->language : NULL;
}

void
o42_spell_ignore (O42Spell *spell, const char *word)
{
  if (spell == NULL || word == NULL)
    return;
  g_hash_table_add (spell->ignored, g_strdup (word));
}

/* A word worth checking at all: letters, and not a number or a code. */
static gboolean
worth_checking (const char *word)
{
  gboolean letters = FALSE;

  if (word == NULL || *word == '\0')
    return FALSE;
  for (const char *p = word; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_isdigit (c))
        return FALSE;
      if (g_unichar_isalpha (c))
        letters = TRUE;
    }
  return letters;
}

gboolean
o42_spell_check (O42Spell *spell, const char *word)
{
  if (spell == NULL || word == NULL || !worth_checking (word))
    return TRUE;
  if (g_hash_table_contains (spell->ignored, word))
    return TRUE;
#ifdef HAVE_HUNSPELL
  {
    char *converted = NULL;
    int ok;

    if (spell->encoding != NULL && g_ascii_strcasecmp (spell->encoding, "UTF-8") != 0)
      converted = g_convert (word, -1, spell->encoding, "UTF-8", NULL, NULL, NULL);
    ok = Hunspell_spell (spell->handle, converted != NULL ? converted : word);
    g_free (converted);
    return ok != 0;
  }
#else
  return TRUE;
#endif
}

char **
o42_spell_suggest (O42Spell *spell, const char *word)
{
#ifdef HAVE_HUNSPELL
  char **list = NULL;
  char *converted = NULL;
  int n;
  GPtrArray *out;

  if (spell == NULL || word == NULL)
    return NULL;
  if (spell->encoding != NULL && g_ascii_strcasecmp (spell->encoding, "UTF-8") != 0)
    converted = g_convert (word, -1, spell->encoding, "UTF-8", NULL, NULL, NULL);
  n = Hunspell_suggest (spell->handle, &list, converted != NULL ? converted : word);
  g_free (converted);
  if (n <= 0)
    {
      if (list != NULL)
        Hunspell_free_list (spell->handle, &list, n);
      return NULL;
    }

  out = g_ptr_array_new ();
  for (int i = 0; i < n; i++)
    {
      char *utf8 = NULL;

      if (spell->encoding != NULL && g_ascii_strcasecmp (spell->encoding, "UTF-8") != 0)
        utf8 = g_convert (list[i], -1, "UTF-8", spell->encoding, NULL, NULL, NULL);
      g_ptr_array_add (out, utf8 != NULL ? utf8 : g_strdup (list[i]));
    }
  Hunspell_free_list (spell->handle, &list, n);
  g_ptr_array_add (out, NULL);
  return (char **) g_ptr_array_free (out, FALSE);
#else
  (void) spell; (void) word;
  return NULL;
#endif
}

void
o42_spell_words (const char *text, O42SpellWordFunc found, gpointer user_data)
{
  const char *p, *start = NULL;

  if (text == NULL || found == NULL)
    return;

  for (p = text; ; p = g_utf8_next_char (p))
    {
      gunichar c = (*p != '\0') ? g_utf8_get_char (p) : 0;
      /* An apostrophe holds a word together -- "don't" is one word --
       * but only between letters. */
      gboolean inside = g_unichar_isalpha (c) || g_unichar_isdigit (c) ||
                        (start != NULL && (c == '\'' || c == 0x2019));

      if (inside && start == NULL)
        start = p;
      else if (!inside && start != NULL)
        {
          const char *end = p;

          /* An apostrophe at the very end is punctuation after all. */
          while (end > start)
            {
              const char *back = g_utf8_prev_char (end);
              gunichar last = g_utf8_get_char (back);

              if (last == '\'' || last == 0x2019)
                end = back;
              else
                break;
            }
          if (end > start)
            {
              char *word = g_strndup (start, (gsize) (end - start));

              found (word, (gsize) (start - text), (gsize) (end - start), user_data);
              g_free (word);
            }
          start = NULL;
        }
      if (*p == '\0')
        break;
    }
}
