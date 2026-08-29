/* o42-spell.h - checking the spelling of what is in the cells
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A thin cover over Hunspell, which is what everything else on the
 * machine spells with.  It is optional: built without it, or run on a
 * machine with no dictionary for the language, every word passes and
 * o42_spell_new returns NULL, so nothing else has to care.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _O42Spell O42Spell;

/* Whether this build can spell at all. */
gboolean    o42_spell_available (void);

/* A checker for a language ("en_GB", "nb_NO"); NULL for the one the
 * machine's locale asks for.  NULL when there is no dictionary. */
O42Spell   *o42_spell_new       (const char *language);
void        o42_spell_free      (O42Spell *spell);

/* The language the dictionary turned out to be, for telling the user. */
const char *o42_spell_language  (O42Spell *spell);

/* TRUE when the word is in the dictionary.  Words with a digit in them,
 * and anything that is not a word at all, pass. */
gboolean    o42_spell_check     (O42Spell *spell, const char *word);

/* What it might have been instead: a NULL-terminated array to free with
 * g_strfreev, or NULL for no suggestion. */
char      **o42_spell_suggest   (O42Spell *spell, const char *word);

/* Words the user has told it to leave alone this session. */
void        o42_spell_ignore    (O42Spell *spell, const char *word);

/* Walks a piece of text and hands each word to `found` with its byte
 * offset and length.  Punctuation and digits divide words; an
 * apostrophe inside one does not. */
typedef void (*O42SpellWordFunc) (const char *word, gsize offset, gsize length,
                                  gpointer user_data);
void        o42_spell_words     (const char *text, O42SpellWordFunc found,
                                 gpointer user_data);

G_END_DECLS
