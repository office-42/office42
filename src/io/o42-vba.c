/* o42-vba.c - a Visual Basic project, read from its file and written back
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Written from Microsoft's description of the format, [MS-OVBA]: the
 * compression in 2.4.1, the dir stream in 2.3.4.2, the PROJECT stream
 * in 2.3.1 and the obfuscation of its last three lines in 2.4.3.
 */

#include "o42-vba.h"
#include "o42-ole2.h"
#include "o42-sheet.h"

#include <string.h>

/* The dir stream's records this file reads or writes. */
enum {
  REC_SYSKIND = 0x0001, REC_LCID = 0x0002, REC_CODEPAGE = 0x0003, REC_NAME = 0x0004,
  REC_DOCSTRING = 0x0005, REC_HELPFILE = 0x0006, REC_HELPCONTEXT = 0x0007,
  REC_LIBFLAGS = 0x0008, REC_VERSION = 0x0009, REC_CONSTANTS = 0x000C,
  REC_MODULES = 0x000F, REC_END = 0x0010, REC_COOKIE = 0x0013, REC_LCIDINVOKE = 0x0014,
  REC_MODULENAME = 0x0019, REC_STREAMNAME = 0x001A, REC_MODULEDOC = 0x001C,
  REC_MODULEHELP = 0x001E, REC_PROCEDURAL = 0x0021, REC_NONPROCEDURAL = 0x0022,
  REC_MODULECOOKIE = 0x002C, REC_MODULEEND = 0x002B, REC_OFFSET = 0x0031,
  REC_STREAMNAME_U = 0x0032, REC_CONSTANTS_U = 0x003C, REC_HELPFILE2 = 0x003D,
  REC_DOCSTRING_U = 0x0040, REC_MODULENAME_U = 0x0047, REC_MODULEDOC_U = 0x0048
};

static guint
rd16 (const guchar *p)
{
  return p[0] | (p[1] << 8);
}

static guint32
rd32 (const guchar *p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((guint32) p[3] << 24);
}

static void
put16 (GByteArray *a, guint v)
{
  guchar b[2] = { v & 0xff, (v >> 8) & 0xff };
  g_byte_array_append (a, b, 2);
}

static void
put32 (GByteArray *a, guint32 v)
{
  guchar b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff };
  g_byte_array_append (a, b, 4);
}

/* ---- Compression ----------------------------------------------------------- */

/* The bits a copy token gives its offset: enough to reach back to the
 * start of the chunk from `done` bytes into it, and never fewer than
 * four. */
static guint
offset_bits (gsize done)
{
  guint bits = 4;
  while (bits < 12 && ((gsize) 1 << bits) < done)
    bits++;
  return bits;
}

/* A compressed container: a signature byte, then chunks of at most
 * 4096 bytes each, either stored as they are or as literals and copies
 * of what came before in the same chunk. */
static GByteArray *
decompress (const guchar *in, gsize len)
{
  GByteArray *out = g_byte_array_new ();
  gsize pos = 1;

  if (len == 0 || in[0] != 1)
    return out;
  while (pos + 2 <= len)
    {
      guint header = rd16 (in + pos);
      gsize chunk_end = MIN (pos + (header & 0x0FFF) + 3, len);
      gsize start = out->len;

      pos += 2;
      if (!(header & 0x8000))
        {
          gsize take = MIN ((gsize) 4096, len - pos);
          g_byte_array_append (out, in + pos, take);
          pos += 4096;
          continue;
        }
      while (pos < chunk_end)
        {
          guchar flags = in[pos++];

          for (int bit = 0; bit < 8 && pos < chunk_end; bit++)
            {
              if (!(flags & (1 << bit)))
                {
                  g_byte_array_append (out, in + pos, 1);
                  pos++;
                  continue;
                }
              if (pos + 2 > chunk_end)
                {
                  pos = chunk_end;
                  break;
                }
              {
                guint token = rd16 (in + pos);
                gsize done = out->len - start;
                guint bits = offset_bits (done);
                gsize length = (token & (0xFFFF >> bits)) + 3;
                gsize offset = (token >> (16 - bits)) + 1;

                pos += 2;
                /* A copy from before the chunk began is a broken file:
                 * what came so far is all there is. */
                if (offset > done)
                  return out;
                for (gsize k = 0; k < length; k++)
                  {
                    guchar c = out->data[out->len - offset];
                    g_byte_array_append (out, &c, 1);
                  }
              }
            }
        }
      pos = chunk_end;
    }
  return out;
}

#define HASH_OF(p) (((guint) (p)[0] << 7 ^ (guint) (p)[1] << 4 ^ (p)[2]) & 0xFFF)

/* One chunk of at most 4096 bytes, the longest earlier match taken at
 * each place, found through chains of the places that begin with the
 * same three bytes. */
static void
compress_chunk (GByteArray *out, const guchar *data, gsize n)
{
  gint16 head[4096], prev[4096];
  gsize header_at = out->len, pos = 0, next_hashed = 0;

  memset (head, 0xff, sizeof head);
  put16 (out, 0);
  while (pos < n)
    {
      gsize flag_at = out->len;
      guchar flags = 0;

      g_byte_array_append (out, &flags, 1);
      for (int bit = 0; bit < 8 && pos < n; bit++)
        {
          gsize best = 0, best_offset = 0;

          for (; next_hashed < pos && next_hashed + 2 < n; next_hashed++)
            {
              guint h = HASH_OF (data + next_hashed);
              prev[next_hashed] = head[h];
              head[h] = (gint16) next_hashed;
            }
          if (pos > 0 && pos + 2 < n)
            {
              guint bits = offset_bits (pos);
              gsize longest = (0xFFFF >> bits) + 3;

              for (gint16 cand = head[HASH_OF (data + pos)]; cand >= 0; cand = prev[cand])
                {
                  gsize l = 0;
                  while (l < longest && pos + l < n && data[cand + l] == data[pos + l])
                    l++;
                  if (l > best)
                    {
                      best = l;
                      best_offset = pos - (gsize) cand;
                    }
                  if (best == longest)
                    break;
                }
              if (best >= 3)
                {
                  put16 (out, (guint) ((best_offset - 1) << (16 - bits)) | (guint) (best - 3));
                  flags |= 1 << bit;
                  pos += best;
                  continue;
                }
            }
          g_byte_array_append (out, data + pos, 1);
          pos++;
        }
      out->data[flag_at] = flags;
    }

  if (out->len - header_at > 4098)
    {
      /* Worse than storing it: the chunk goes in as it is, which the
       * format allows only at its full 4096 bytes. */
      guchar zeros[4096] = { 0 };
      g_byte_array_set_size (out, header_at);
      put16 (out, 0x3000 | 0x0FFF);
      g_byte_array_append (out, data, n);
      g_byte_array_append (out, zeros, 4096 - n);
      return;
    }
  {
    guint header = (guint) (out->len - header_at - 3) | 0x3000 | 0x8000;
    out->data[header_at] = header & 0xff;
    out->data[header_at + 1] = header >> 8;
  }
}

static GBytes *
compress (const guchar *data, gsize len)
{
  GByteArray *out = g_byte_array_new ();
  guchar signature = 1;

  g_byte_array_append (out, &signature, 1);
  for (gsize at = 0; at < len; at += 4096)
    compress_chunk (out, data + at, MIN ((gsize) 4096, len - at));
  return g_byte_array_free_to_bytes (out);
}

/* ---- Text in the project's code page -------------------------------------- */

static char *
charset_of (guint codepage)
{
  switch (codepage)
    {
    case 10000: return g_strdup ("MACINTOSH");
    case 20127: return g_strdup ("ASCII");
    case 28591: return g_strdup ("ISO-8859-1");
    default:    return g_strdup_printf ("CP%u", codepage);
    }
}

static char *
from_codepage (const guchar *data, gsize len, guint codepage)
{
  char *text = NULL;

  if (codepage != 65001)
    {
      char *charset = charset_of (codepage);
      text = g_convert ((const char *) data, (gssize) len, "UTF-8", charset, NULL, NULL, NULL);
      g_free (charset);
    }
  if (text == NULL && g_utf8_validate ((const char *) data, (gssize) len, NULL))
    text = g_strndup ((const char *) data, len);
  if (text == NULL)
    text = g_convert ((const char *) data, (gssize) len, "UTF-8", "CP1252", NULL, NULL, NULL);
  if (text == NULL)
    text = g_utf8_make_valid ((const char *) data, (gssize) len);
  return text;
}

/* UTF-8 text in the code page, a letter it has not got written as "?". */
static GBytes *
to_codepage (const char *text, guint codepage)
{
  char *charset;
  char *out;
  gsize len = 0;

  if (codepage == 65001)
    return g_bytes_new (text, strlen (text));
  charset = charset_of (codepage);
  out = g_convert_with_fallback (text, -1, charset, "UTF-8", "?", NULL, &len, NULL);
  g_free (charset);
  if (out == NULL)
    return g_bytes_new (text, strlen (text));
  return g_bytes_new_take (out, len);
}

static GBytes *
to_utf16 (const char *text)
{
  glong n = 0;
  gunichar2 *u = g_utf8_to_utf16 (text, -1, NULL, &n, NULL);
  GByteArray *out = g_byte_array_new ();

  for (glong i = 0; u != NULL && i < n; i++)
    put16 (out, u[i]);
  g_free (u);
  return g_byte_array_free_to_bytes (out);
}

static char *
from_utf16 (const guchar *data, gsize len)
{
  gunichar2 *units = g_new (gunichar2, len / 2 + 1);
  char *text;

  for (gsize i = 0; i < len / 2; i++)
    units[i] = rd16 (data + i * 2);
  text = g_utf16_to_utf8 (units, (glong) (len / 2), NULL, NULL, NULL);
  g_free (units);
  return text;
}

/* Lines end CR LF in the file and LF in the book; a module's text also
 * ends at a NUL, which some writers leave after it. */
static char *
lf_lines (const char *text)
{
  GString *out = g_string_sized_new (strlen (text));

  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p == '\r')
        {
          g_string_append_c (out, '\n');
          if (p[1] == '\n')
            p++;
        }
      else
        g_string_append_c (out, *p);
    }
  return g_string_free (out, FALSE);
}

static char *
crlf_lines (const char *text)
{
  GString *out = g_string_sized_new (strlen (text) + 64);

  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p == '\r')
        continue;
      if (*p == '\n')
        g_string_append (out, "\r\n");
      else
        g_string_append_c (out, *p);
    }
  return g_string_free (out, FALSE);
}

/* ---- Reading ------------------------------------------------------------ */

typedef struct {
  char   *name;
  char   *stream;
  guint32 offset;
  gboolean procedural;
} DirModule;

static void
dir_module_free (gpointer data)
{
  DirModule *m = data;
  g_free (m->name);
  g_free (m->stream);
  g_free (m);
}

/* What the dir stream says: the code page, the modules, and where the
 * project's own records end and the modules' begin, so that a project
 * built again can keep the former as they were. */
typedef struct {
  guint      codepage;
  GPtrArray *modules;       /* DirModule * */
  gsize      head_length;   /* bytes before the PROJECTMODULES record */
} Dir;

static gboolean
dir_parse (const guchar *d, gsize len, Dir *dir)
{
  DirModule *module = NULL;
  gsize pos = 0;

  dir->codepage = 1252;
  dir->modules = g_ptr_array_new_with_free_func (dir_module_free);
  dir->head_length = 0;
  while (pos + 6 <= len)
    {
      guint id = rd16 (d + pos);
      guint32 size = rd32 (d + pos + 2);
      const guchar *body = d + pos + 6;

      /* PROJECTVERSION alone breaks the pattern: its "size" is a
       * reserved 4, and six bytes follow it. */
      if (id == REC_VERSION)
        size = 6;
      if (size > len - pos - 6)
        break;
      switch (id)
        {
        case REC_CODEPAGE:
          if (size >= 2)
            dir->codepage = rd16 (body);
          break;
        case REC_MODULES:
          dir->head_length = pos;
          break;
        case REC_MODULENAME:
          module = g_new0 (DirModule, 1);
          module->name = from_codepage (body, size, dir->codepage);
          module->procedural = TRUE;
          g_ptr_array_add (dir->modules, module);
          break;
        case REC_MODULENAME_U:
          if (module != NULL && size > 0)
            {
              char *u = from_utf16 (body, size);
              if (u != NULL)
                {
                  g_free (module->name);
                  module->name = u;
                }
            }
          break;
        case REC_STREAMNAME:
          if (module != NULL)
            {
              g_free (module->stream);
              module->stream = from_codepage (body, size, dir->codepage);
            }
          break;
        case REC_STREAMNAME_U:
          if (module != NULL && size > 0)
            {
              char *u = from_utf16 (body, size);
              if (u != NULL)
                {
                  g_free (module->stream);
                  module->stream = u;
                }
            }
          break;
        case REC_OFFSET:
          if (module != NULL && size >= 4)
            module->offset = rd32 (body);
          break;
        case REC_PROCEDURAL:
          if (module != NULL)
            module->procedural = TRUE;
          break;
        case REC_NONPROCEDURAL:
          if (module != NULL)
            module->procedural = FALSE;
          break;
        case REC_MODULEEND:
          module = NULL;
          break;
        default:
          break;
        }
      pos += 6 + size;
      if (id == REC_END)
        break;
    }
  return dir->head_length > 0;
}

/* The kinds the PROJECT stream gives the modules: Module=, Class=,
 * BaseClass= (a form) and Document=Name/&H00000000. */
static GHashTable *
project_kinds (GBytes *project, guint codepage)
{
  GHashTable *kinds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  gsize len = 0;
  const guchar *data = project != NULL ? g_bytes_get_data (project, &len) : NULL;
  char *text, **lines;

  if (data == NULL)
    return kinds;
  text = from_codepage (data, len, codepage);
  lines = g_strsplit_set (text, "\r\n", -1);
  for (int i = 0; lines[i] != NULL; i++)
    {
      const char *line = lines[i];
      const char *eq = strchr (line, '=');
      O42VbaKind kind;
      char *name, *folded;

      if (line[0] == '[')
        break;
      if (eq == NULL)
        continue;
      if (g_str_has_prefix (line, "Module="))
        kind = O42_VBA_STANDARD;
      else if (g_str_has_prefix (line, "Class="))
        kind = O42_VBA_CLASS;
      else if (g_str_has_prefix (line, "BaseClass="))
        kind = O42_VBA_FORM;
      else if (g_str_has_prefix (line, "Document="))
        kind = O42_VBA_DOCUMENT;
      else
        continue;
      name = g_strdup (eq + 1);
      if (strchr (name, '/') != NULL)
        *strchr (name, '/') = '\0';
      folded = g_ascii_strdown (g_strstrip (name), -1);
      g_hash_table_replace (kinds, folded, GINT_TO_POINTER (kind + 1));
      g_free (name);
    }
  g_strfreev (lines);
  g_free (text);
  return kinds;
}

static char *
join_path (const char *storage, const char *path)
{
  if (storage == NULL || *storage == '\0')
    return g_strdup (path);
  return g_strconcat (storage, "/", path, NULL);
}

gboolean
o42_vba_read (O42Book *book, GBytes *file, const char *storage, GError **error)
{
  char *path = join_path (storage, "VBA/dir");
  GBytes *dir_bytes = o42_ole2_read_path (file, path, error);
  GByteArray *dir_data;
  GBytes *project;
  GHashTable *kinds;
  Dir dir;

  g_free (path);
  if (dir_bytes == NULL)
    return FALSE;
  dir_data = decompress (g_bytes_get_data (dir_bytes, NULL), g_bytes_get_size (dir_bytes));
  g_bytes_unref (dir_bytes);
  if (!dir_parse (dir_data->data, dir_data->len, &dir))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "The Visual Basic project's directory cannot be read.");
      g_byte_array_unref (dir_data);
      g_ptr_array_unref (dir.modules);
      return FALSE;
    }
  g_byte_array_unref (dir_data);

  path = join_path (storage, "PROJECT");
  project = o42_ole2_read_path (file, path, NULL);
  g_free (path);
  kinds = project_kinds (project, dir.codepage);
  if (project != NULL)
    g_bytes_unref (project);

  while (o42_book_n_vba_modules (book) > 0)
    o42_book_remove_vba_module (book, o42_book_vba_module_name (book, 0));
  for (guint i = 0; i < dir.modules->len; i++)
    {
      DirModule *m = g_ptr_array_index (dir.modules, i);
      char *stream_path, *folded, *text, *code;
      GBytes *stream;
      GByteArray *source;
      gsize len;
      const guchar *data;
      int kind;

      if (m->name == NULL)
        continue;
      stream_path = g_strconcat ("VBA/", m->stream != NULL ? m->stream : m->name, NULL);
      path = join_path (storage, stream_path);
      stream = o42_ole2_read_path (file, path, NULL);
      g_free (path);
      g_free (stream_path);
      if (stream == NULL)
        continue;
      data = g_bytes_get_data (stream, &len);
      source = m->offset < len ? decompress (data + m->offset, len - m->offset) : g_byte_array_new ();
      g_bytes_unref (stream);
      len = 0;
      while (len < source->len && source->data[len] != 0)
        len++;
      text = from_codepage (source->data, len, dir.codepage);
      code = lf_lines (text);
      g_free (text);
      g_byte_array_unref (source);

      folded = g_ascii_strdown (m->name, -1);
      kind = GPOINTER_TO_INT (g_hash_table_lookup (kinds, folded)) - 1;
      g_free (folded);
      if (kind < 0)
        kind = m->procedural ? O42_VBA_STANDARD : O42_VBA_CLASS;
      o42_book_set_vba_module (book, m->name, (O42VbaKind) kind, code);
      g_free (code);
    }
  g_hash_table_unref (kinds);
  g_ptr_array_unref (dir.modules);
  o42_book_set_vba_edited (book, FALSE);
  return TRUE;
}

/* ---- Writing ------------------------------------------------------------ */

static void
put_record (GByteArray *a, guint id, const void *data, gsize size)
{
  put16 (a, id);
  put32 (a, (guint32) size);
  if (size > 0)
    g_byte_array_append (a, data, (guint) size);
}

static void
put_record_bytes (GByteArray *a, guint id, GBytes *bytes)
{
  gsize size = 0;
  const guchar *data = g_bytes_get_data (bytes, &size);
  put_record (a, id, data, size);
}

static void
put_record_u32 (GByteArray *a, guint id, guint32 v)
{
  guchar b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff };
  put_record (a, id, b, 4);
}

static void
put_record_u16 (GByteArray *a, guint id, guint v)
{
  guchar b[2] = { v & 0xff, (v >> 8) & 0xff };
  put_record (a, id, b, 2);
}

/* The project's own records, for a project that is new: a 32-bit
 * Windows project in US English and the code page every Western
 * Windows uses, called VBAProject, with no references beyond the two
 * every project has without saying -- Visual Basic's and Excel's. */
static void
put_new_head (GByteArray *a, guint codepage)
{
  put_record_u32 (a, REC_SYSKIND, 1);
  put_record_u32 (a, REC_LCID, 0x409);
  put_record_u32 (a, REC_LCIDINVOKE, 0x409);
  put_record_u16 (a, REC_CODEPAGE, codepage);
  put_record (a, REC_NAME, "VBAProject", 10);
  put_record (a, REC_DOCSTRING, NULL, 0);
  put_record (a, REC_DOCSTRING_U, NULL, 0);
  put_record (a, REC_HELPFILE, NULL, 0);
  put_record (a, REC_HELPFILE2, NULL, 0);
  put_record_u32 (a, REC_HELPCONTEXT, 0);
  put_record_u32 (a, REC_LIBFLAGS, 0);
  put16 (a, REC_VERSION);
  put32 (a, 4);
  put32 (a, 1);
  put16 (a, 0);
  put_record (a, REC_CONSTANTS, NULL, 0);
  put_record (a, REC_CONSTANTS_U, NULL, 0);
}

/* The three obfuscated lines at the end of the PROJECT stream's
 * settings, CMG, DPB and GC, as 2.4.3 has them: each byte hidden by
 * the two before it, starting from a seed and the project's key. */
static char *
obfuscate (const char *project_id, const guchar *data, guint32 length, guint8 seed)
{
  guint8 key = 0, enc1, enc2, plain1;
  GString *out = g_string_new (NULL);
  guchar len_bytes[4] = { length & 0xff, (length >> 8) & 0xff, (length >> 16) & 0xff, length >> 24 };
  int ignored = (seed & 6) / 2;

  for (const char *p = project_id; *p != '\0'; p++)
    key = (guint8) (key + (guchar) *p);
  g_string_append_printf (out, "%02X%02X%02X", seed, seed ^ 2, seed ^ key);
  plain1 = key;
  enc1 = seed ^ key;
  enc2 = seed ^ 2;
  for (int i = 0; i < ignored + 4 + (int) length; i++)
    {
      guint8 plain = i < ignored ? 7 : i < ignored + 4 ? len_bytes[i - ignored] : data[i - ignored - 4];
      guint8 enc = plain ^ (guint8) (enc2 + plain1);

      g_string_append_printf (out, "%02X", enc);
      enc2 = enc1;
      enc1 = enc;
      plain1 = plain;
    }
  return g_string_free (out, FALSE);
}

static const char *
kind_line (O42VbaKind kind)
{
  switch (kind)
    {
    case O42_VBA_CLASS:    return "Class";
    case O42_VBA_FORM:     return "BaseClass";
    case O42_VBA_DOCUMENT: return "Document";
    case O42_VBA_STANDARD:
    default:               return "Module";
    }
}

static gboolean
is_module_line (const char *line)
{
  return g_str_has_prefix (line, "Module=") || g_str_has_prefix (line, "Class=") ||
         g_str_has_prefix (line, "BaseClass=") || g_str_has_prefix (line, "Document=");
}

/* The PROJECT stream: the old one's lines with the modules' replaced,
 * or a new one. */
static char *
project_text (O42Book *book, const char *old_text)
{
  GString *out = g_string_new (NULL);
  int n = o42_book_n_vba_modules (book);
  char **lines = old_text != NULL ? g_strsplit_set (old_text, "\r\n", -1) : NULL;
  gboolean in_workspace = FALSE, written = FALSE;

  if (lines == NULL || lines[0] == NULL || !g_str_has_prefix (lines[0], "ID="))
    {
      char *uuid = g_uuid_string_random ();
      char *upper = g_ascii_strup (uuid, -1);
      char *id = g_strdup_printf ("{%s}", upper);
      guint8 state[4] = { 0, 0, 0, 0 }, password[1] = { 0 }, visible[1] = { 0xFF };
      char *cmg, *dpb, *gc;
      char *quoted_id = g_strdup_printf ("\"%s\"", id);

      g_string_append_printf (out, "ID=%s\r\n", quoted_id);
      for (int i = 0; i < n; i++)
        g_string_append_printf (out, "%s=%s%s\r\n", kind_line (o42_book_vba_module_kind (book, i)),
                                o42_book_vba_module_name (book, i),
                                o42_book_vba_module_kind (book, i) == O42_VBA_DOCUMENT ? "/&H00000000" : "");
      cmg = obfuscate (id, state, 4, (guint8) g_random_int_range (0, 256));
      dpb = obfuscate (id, password, 1, (guint8) g_random_int_range (0, 256));
      gc = obfuscate (id, visible, 1, (guint8) g_random_int_range (0, 256));
      g_string_append_printf (out,
        "Name=\"VBAProject\"\r\nHelpContextID=\"0\"\r\nVersionCompatible32=\"393222000\"\r\n"
        "CMG=\"%s\"\r\nDPB=\"%s\"\r\nGC=\"%s\"\r\n\r\n"
        "[Host Extender Info]\r\n&H00000001={3832D640-CF90-11CF-8E43-00A0C911005A};VBE;&H00000000\r\n\r\n"
        "[Workspace]\r\n", cmg, dpb, gc);
      for (int i = 0; i < n; i++)
        g_string_append_printf (out, "%s=0, 0, 0, 0, C\r\n", o42_book_vba_module_name (book, i));
      g_free (cmg);
      g_free (dpb);
      g_free (gc);
      g_free (quoted_id);
      g_free (id);
      g_free (upper);
      g_free (uuid);
      g_strfreev (lines);
      return g_string_free (out, FALSE);
    }

  for (int i = 0; lines[i] != NULL; i++)
    {
      const char *line = lines[i];

      if (*line == '\0' && lines[i + 1] == NULL)
        break;
      if (line[0] == '[')
        in_workspace = g_str_has_prefix (line, "[Workspace]");
      else if (!in_workspace && is_module_line (line))
        continue;
      else if (in_workspace && strchr (line, '=') != NULL)
        {
          /* A window setting for a module that is gone goes with it. */
          char *name = g_strndup (line, strchr (line, '=') - line);
          gboolean kept = o42_book_vba_module_find (book, name) >= 0;
          g_free (name);
          if (!kept)
            continue;
        }
      g_string_append (out, line);
      g_string_append (out, "\r\n");
      if (!written && g_str_has_prefix (line, "ID="))
        {
          for (int k = 0; k < n; k++)
            g_string_append_printf (out, "%s=%s%s\r\n", kind_line (o42_book_vba_module_kind (book, k)),
                                    o42_book_vba_module_name (book, k),
                                    o42_book_vba_module_kind (book, k) == O42_VBA_DOCUMENT ? "/&H00000000" : "");
          written = TRUE;
        }
    }
  g_strfreev (lines);
  return g_string_free (out, FALSE);
}

static void
add_stream (GPtrArray *names, GPtrArray *contents, const char *prefix, const char *path, GBytes *data)
{
  g_ptr_array_add (names, join_path (prefix, path));
  g_ptr_array_add (contents, data);
}

void
o42_vba_build_streams (O42Book *book, GBytes *original, const char *prefix,
                       GPtrArray *names, GPtrArray *contents)
{
  static const guchar VBA_PROJECT[7] = { 0xCC, 0x61, 0xFF, 0xFF, 0x00, 0x00, 0x00 };
  GByteArray *dir = g_byte_array_new ();
  GByteArray *wm = g_byte_array_new ();
  guint codepage = 1252;
  char *old_project = NULL;
  int n = o42_book_n_vba_modules (book);

  /* The old project's own records, and its code page with them. */
  if (original != NULL)
    {
      GBytes *old_dir = o42_ole2_read_path (original, "VBA/dir", NULL);
      if (old_dir != NULL)
        {
          GByteArray *d = decompress (g_bytes_get_data (old_dir, NULL), g_bytes_get_size (old_dir));
          Dir parsed;

          if (dir_parse (d->data, d->len, &parsed))
            {
              codepage = parsed.codepage;
              g_byte_array_append (dir, d->data, (guint) parsed.head_length);
            }
          g_ptr_array_unref (parsed.modules);
          g_byte_array_unref (d);
          g_bytes_unref (old_dir);
        }
      {
        GBytes *p = o42_ole2_read_path (original, "PROJECT", NULL);
        if (p != NULL)
          {
            old_project = from_codepage (g_bytes_get_data (p, NULL), g_bytes_get_size (p), codepage);
            g_bytes_unref (p);
          }
      }
    }
  if (dir->len == 0)
    put_new_head (dir, codepage);

  put_record_u16 (dir, REC_MODULES, (guint) n);
  put_record_u16 (dir, REC_COOKIE, 0xFFFF);
  for (int i = 0; i < n; i++)
    {
      const char *name = o42_book_vba_module_name (book, i);
      O42VbaKind kind = o42_book_vba_module_kind (book, i);
      GBytes *mbcs = to_codepage (name, codepage);
      GBytes *wide = to_utf16 (name);
      char *crlf = crlf_lines (o42_book_vba_module_code (book, i));
      GBytes *text = to_codepage (crlf, codepage);
      char *stream;
      guchar zero = 0;

      put_record_bytes (dir, REC_MODULENAME, mbcs);
      put_record_bytes (dir, REC_MODULENAME_U, wide);
      put_record_bytes (dir, REC_STREAMNAME, mbcs);
      put_record_bytes (dir, REC_STREAMNAME_U, wide);
      put_record (dir, REC_MODULEDOC, NULL, 0);
      put_record (dir, REC_MODULEDOC_U, NULL, 0);
      put_record_u32 (dir, REC_OFFSET, 0);
      put_record_u32 (dir, REC_MODULEHELP, 0);
      put_record_u16 (dir, REC_MODULECOOKIE, 0xFFFF);
      put_record (dir, kind == O42_VBA_STANDARD ? REC_PROCEDURAL : REC_NONPROCEDURAL, NULL, 0);
      put_record (dir, REC_MODULEEND, NULL, 0);

      g_byte_array_append (wm, g_bytes_get_data (mbcs, NULL), (guint) g_bytes_get_size (mbcs));
      g_byte_array_append (wm, &zero, 1);
      g_byte_array_append (wm, g_bytes_get_data (wide, NULL), (guint) g_bytes_get_size (wide));
      put16 (wm, 0);

      /* The text alone, from the stream's start: no compiled form
       * before it, so Excel compiles the text it finds. */
      stream = g_strconcat ("VBA/", name, NULL);
      add_stream (names, contents, prefix, stream,
                  compress (g_bytes_get_data (text, NULL), g_bytes_get_size (text)));
      g_free (stream);
      g_bytes_unref (text);
      g_free (crlf);
      g_bytes_unref (mbcs);
      g_bytes_unref (wide);
    }
  put_record (dir, REC_END, NULL, 0);
  put16 (wm, 0);

  add_stream (names, contents, prefix, "VBA/dir", compress (dir->data, dir->len));
  add_stream (names, contents, prefix, "VBA/_VBA_PROJECT", g_bytes_new (VBA_PROJECT, sizeof VBA_PROJECT));
  {
    char *text = project_text (book, old_project);
    add_stream (names, contents, prefix, "PROJECT", to_codepage (text, codepage));
    g_free (text);
  }
  add_stream (names, contents, prefix, "PROJECTwm", g_byte_array_free_to_bytes (wm));

  /* Everything else the old project had -- a form's storage, say --
   * except what was written above and the compiled caches, which no
   * longer match the text. */
  if (original != NULL)
    {
      GPtrArray *old = o42_ole2_list (original, NULL);

      for (guint i = 0; i < old->len; i++)
        {
          const char *path = g_ptr_array_index (old, i);
          GBytes *data;

          if (g_ascii_strncasecmp (path, "VBA/", 4) == 0 || g_ascii_strcasecmp (path, "PROJECT") == 0 ||
              g_ascii_strcasecmp (path, "PROJECTwm") == 0 || g_ascii_strcasecmp (path, "PROJECTlk") == 0)
            continue;
          data = o42_ole2_read_path (original, path, NULL);
          if (data != NULL)
            add_stream (names, contents, prefix, path, data);
        }
      g_ptr_array_unref (old);
    }
  g_byte_array_unref (dir);
  g_free (old_project);
}

GBytes *
o42_vba_build (O42Book *book, GBytes *original)
{
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *contents = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  GBytes *file;

  o42_vba_build_streams (book, original, "", names, contents);
  file = o42_ole2_build ((const char **) names->pdata, (GBytes **) contents->pdata, (int) names->len);
  g_ptr_array_unref (names);
  g_ptr_array_unref (contents);
  return file;
}

/* ---- A module's head ---------------------------------------------------- */

static gboolean
is_head_line (const char *line)
{
  while (*line == ' ' || *line == '\t')
    line++;
  return g_ascii_strncasecmp (line, "Attribute VB_", 13) == 0;
}

char *
o42_vba_code_body (const char *code)
{
  const char *p = code;

  while (*p != '\0')
    {
      const char *end = strchr (p, '\n');
      char *line = end != NULL ? g_strndup (p, end - p) : g_strdup (p);
      gboolean head = is_head_line (line);

      g_free (line);
      if (!head)
        break;
      p = end != NULL ? end + 1 : p + strlen (p);
    }
  return g_strdup (p);
}

static const char *
default_head (O42Book *book, const char *name, O42VbaKind kind)
{
  if (kind == O42_VBA_CLASS)
    return "Attribute VB_GlobalNameSpace = False\nAttribute VB_Creatable = False\n"
           "Attribute VB_PredeclaredId = False\nAttribute VB_Exposed = False\n";
  if (kind == O42_VBA_DOCUMENT)
    return g_ascii_strcasecmp (name, o42_book_codename (book)) == 0
           ? "Attribute VB_Base = \"0{00020819-0000-0000-C000-000000000046}\"\n"
             "Attribute VB_GlobalNameSpace = False\nAttribute VB_Creatable = False\n"
             "Attribute VB_PredeclaredId = True\nAttribute VB_Exposed = True\n"
             "Attribute VB_TemplateDerived = False\nAttribute VB_Customizable = True\n"
           : "Attribute VB_Base = \"0{00020820-0000-0000-C000-000000000046}\"\n"
             "Attribute VB_GlobalNameSpace = False\nAttribute VB_Creatable = False\n"
             "Attribute VB_PredeclaredId = True\nAttribute VB_Exposed = True\n"
             "Attribute VB_TemplateDerived = False\nAttribute VB_Customizable = True\n";
  return "";
}

char *
o42_vba_code_join (O42Book *book, const char *name, O42VbaKind kind, const char *code, const char *body)
{
  GString *out = g_string_new (NULL);
  gboolean named = FALSE, any = FALSE;
  const char *p = code != NULL ? code : "";

  while (*p != '\0')
    {
      const char *end = strchr (p, '\n');
      char *line = end != NULL ? g_strndup (p, end - p) : g_strdup (p);

      if (!is_head_line (line))
        {
          g_free (line);
          break;
        }
      any = TRUE;
      if (g_ascii_strncasecmp (g_strchug (line), "Attribute VB_Name", 17) == 0)
        {
          g_string_append_printf (out, "Attribute VB_Name = \"%s\"\n", name);
          named = TRUE;
        }
      else
        g_string_append_printf (out, "%s\n", line);
      g_free (line);
      p = end != NULL ? end + 1 : p + strlen (p);
    }
  if (!named)
    {
      char *line = g_strdup_printf ("Attribute VB_Name = \"%s\"\n", name);
      g_string_prepend (out, line);
      g_free (line);
    }
  if (!any)
    g_string_append (out, default_head (book, name, kind));
  g_string_append (out, body);
  if (out->len > 0 && out->str[out->len - 1] != '\n')
    g_string_append_c (out, '\n');
  return g_string_free (out, FALSE);
}

void
o42_vba_ensure_documents (O42Book *book)
{
  int n = o42_book_n_sheets (book);

  if (o42_book_vba_module_find (book, o42_book_codename (book)) < 0)
    {
      char *code = o42_vba_code_join (book, o42_book_codename (book), O42_VBA_DOCUMENT, NULL, "");
      o42_book_set_vba_module (book, o42_book_codename (book), O42_VBA_DOCUMENT, code);
      g_free (code);
    }
  for (int i = 0; i < n; i++)
    {
      O42Sheet *sheet = o42_book_sheet (book, i);
      char *code;

      if (o42_sheet_codename (sheet) == NULL)
        {
          /* Excel's own choice: Sheet and the first number no other
           * module or sheet has taken. */
          for (int k = i + 1; ; k++)
            {
              char *candidate = g_strdup_printf ("Sheet%d", k);
              gboolean taken = o42_book_vba_module_find (book, candidate) >= 0;

              for (int j = 0; j < n && !taken; j++)
                taken = g_strcmp0 (o42_sheet_codename (o42_book_sheet (book, j)), candidate) == 0;
              if (!taken)
                {
                  o42_sheet_set_codename (sheet, candidate);
                  g_free (candidate);
                  break;
                }
              g_free (candidate);
            }
        }
      if (o42_book_vba_module_find (book, o42_sheet_codename (sheet)) >= 0)
        continue;
      code = o42_vba_code_join (book, o42_sheet_codename (sheet), O42_VBA_DOCUMENT, NULL, "");
      o42_book_set_vba_module (book, o42_sheet_codename (sheet), O42_VBA_DOCUMENT, code);
      g_free (code);
    }
}

/* ---- Files of one module ------------------------------------------------- */

gboolean
o42_vba_import (const char *text, gsize length, const char *filename,
                char **name, O42VbaKind *kind, char **code)
{
  char *utf8, *lf, *base, *dot;
  const char *p;
  GString *out;
  gboolean class_file = FALSE, in_version = FALSE;

  *name = NULL;
  *code = NULL;
  *kind = O42_VBA_STANDARD;
  if (filename != NULL && g_str_has_suffix (filename, ".frm"))
    return FALSE;
  /* Visual Basic writes these files in the machine's code page; one
   * that is already UTF-8 is taken as it is. */
  utf8 = g_utf8_validate (text, (gssize) length, NULL) ? g_strndup (text, length)
                                                        : from_codepage ((const guchar *) text, length, 1252);
  if (g_str_has_prefix (utf8, "\xEF\xBB\xBF"))
    memmove (utf8, utf8 + 3, strlen (utf8 + 3) + 1);
  lf = lf_lines (utf8);
  g_free (utf8);

  out = g_string_new (NULL);
  p = lf;
  while (*p != '\0')
    {
      const char *end = strchr (p, '\n');
      char *line = end != NULL ? g_strndup (p, end - p) : g_strdup (p);
      char *trimmed = g_strstrip (g_strdup (line));

      /* A class's file begins VERSION 1.0 CLASS, BEGIN ... END: the
       * settings Visual Basic keeps elsewhere in a project. */
      if (out->len == 0 && g_ascii_strncasecmp (trimmed, "VERSION ", 8) == 0)
        {
          class_file = strstr (trimmed, "CLASS") != NULL;
          if (!class_file)
            {
              g_free (trimmed);
              g_free (line);
              g_free (lf);
              g_string_free (out, TRUE);
              return FALSE;     /* a form's */
            }
        }
      else if (out->len == 0 && g_ascii_strcasecmp (trimmed, "BEGIN") == 0)
        in_version = TRUE;
      else if (in_version)
        in_version = g_ascii_strcasecmp (trimmed, "END") != 0;
      else
        {
          if (*name == NULL && g_ascii_strncasecmp (trimmed, "Attribute VB_Name", 17) == 0)
            {
              const char *q1 = strchr (trimmed, '"');
              const char *q2 = q1 != NULL ? strchr (q1 + 1, '"') : NULL;
              if (q2 != NULL)
                *name = g_strndup (q1 + 1, q2 - q1 - 1);
            }
          if (g_ascii_strncasecmp (trimmed, "Attribute VB_Base", 17) == 0)
            *kind = O42_VBA_DOCUMENT;
          g_string_append (out, line);
          g_string_append_c (out, '\n');
        }
      g_free (trimmed);
      g_free (line);
      p = end != NULL ? end + 1 : p + strlen (p);
    }
  g_free (lf);
  if (class_file && *kind != O42_VBA_DOCUMENT)
    *kind = O42_VBA_CLASS;
  if (*name == NULL)
    {
      base = g_path_get_basename (filename != NULL ? filename : "Module1.bas");
      dot = strrchr (base, '.');
      if (dot != NULL)
        *dot = '\0';
      *name = base;
      if (filename != NULL && g_str_has_suffix (filename, ".cls"))
        *kind = O42_VBA_CLASS;
    }
  {
    char *body = o42_vba_code_body (out->str);
    char *head = g_strndup (out->str, out->len - strlen (body));
    GString *whole = g_string_new (head);

    if (strstr (head, "VB_Name") == NULL)
      {
        char *line = g_strdup_printf ("Attribute VB_Name = \"%s\"\n", *name);
        g_string_prepend (whole, line);
        g_free (line);
      }
    g_string_append (whole, body);
    *code = g_string_free (whole, FALSE);
    g_free (head);
    g_free (body);
  }
  g_string_free (out, TRUE);
  return TRUE;
}

char *
o42_vba_export (const char *name, O42VbaKind kind, const char *code, gsize *length)
{
  GString *text = g_string_new (NULL);
  char *crlf, *ansi;
  gsize n = 0;

  if (kind == O42_VBA_CLASS || kind == O42_VBA_DOCUMENT)
    g_string_append (text, "VERSION 1.0 CLASS\nBEGIN\n  MultiUse = -1  'True\nEND\n");
  if (strstr (code, "VB_Name") == NULL)
    g_string_append_printf (text, "Attribute VB_Name = \"%s\"\n", name);
  g_string_append (text, code);
  crlf = crlf_lines (text->str);
  g_string_free (text, TRUE);
  /* In the code page Visual Basic reads such files in, when the text
   * has nothing that one cannot say. */
  ansi = g_convert (crlf, -1, "CP1252", "UTF-8", NULL, &n, NULL);
  if (ansi != NULL)
    {
      g_free (crlf);
      *length = n;
      return ansi;
    }
  *length = strlen (crlf);
  return crlf;
}

/* ---- Macros --------------------------------------------------------------- */

static void
macro_free (gpointer data)
{
  O42VbaMacro *m = data;
  g_free (m->module);
  g_free (m->name);
  g_free (m->description);
  g_free (m);
}

/* The next word of `p`, an identifier; NULL if there is none. */
static char *
word_at (const char **p)
{
  const char *s = *p;
  const char *e;

  while (*s == ' ' || *s == '\t')
    s++;
  e = s;
  while (g_ascii_isalnum (*e) || *e == '_' || (guchar) *e >= 0x80)
    e++;
  if (e == s)
    return NULL;
  *p = e;
  return g_strndup (s, e - s);
}

/* The value of an Attribute line, `NAME.VB_...= "value"`, unquoted. */
static char *
attribute_value (const char *line)
{
  const char *q1 = strchr (line, '"');
  const char *q2 = q1 != NULL ? strrchr (line, '"') : NULL;
  return q2 != NULL && q2 > q1 ? g_strndup (q1 + 1, q2 - q1 - 1) : g_strdup ("");
}

GPtrArray *
o42_vba_macros (O42Book *book)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (macro_free);

  for (int i = 0; i < o42_book_n_vba_modules (book); i++)
    {
      O42VbaKind kind = o42_book_vba_module_kind (book, i);
      const char *module = o42_book_vba_module_name (book, i);
      char **lines;
      O42VbaMacro *current = NULL;
      gboolean private_module = FALSE;

      if (kind == O42_VBA_CLASS || kind == O42_VBA_FORM)
        continue;
      lines = g_strsplit (o42_book_vba_module_code (book, i), "\n", -1);
      for (int k = 0; lines[k] != NULL; k++)
        {
          const char *p = lines[k];
          char *first = word_at (&p);
          gboolean is_private = FALSE;

          if (first == NULL)
            continue;
          if (g_ascii_strcasecmp (first, "Option") == 0)
            {
              char *second = word_at (&p), *third = word_at (&p);
              if (second != NULL && third != NULL && g_ascii_strcasecmp (second, "Private") == 0 &&
                  g_ascii_strcasecmp (third, "Module") == 0)
                private_module = TRUE;
              g_free (second);
              g_free (third);
            }
          else if (g_ascii_strcasecmp (first, "Attribute") == 0 && current != NULL)
            {
              char *target = word_at (&p);
              if (target != NULL && g_ascii_strcasecmp (target, current->name) == 0 && *p == '.')
                {
                  if (strstr (p, "VB_Invoke_Func") != NULL)
                    {
                      char *v = attribute_value (p);
                      if (g_ascii_isalpha (v[0]))
                        current->shortcut = v[0];
                      g_free (v);
                    }
                  else if (strstr (p, "VB_Description") != NULL)
                    {
                      g_free (current->description);
                      current->description = attribute_value (p);
                    }
                }
              g_free (target);
            }
          else
            {
              char *word = first;

              if (g_ascii_strcasecmp (word, "Public") == 0 || g_ascii_strcasecmp (word, "Private") == 0 ||
                  g_ascii_strcasecmp (word, "Friend") == 0 || g_ascii_strcasecmp (word, "Global") == 0)
                {
                  is_private = g_ascii_strcasecmp (word, "Public") != 0 && g_ascii_strcasecmp (word, "Global") != 0;
                  word = word_at (&p);
                  g_free (first);
                  first = word;
                }
              if (word != NULL && g_ascii_strcasecmp (word, "Static") == 0)
                {
                  word = word_at (&p);
                  g_free (first);
                  first = word;
                }
              if (word != NULL && g_ascii_strcasecmp (word, "Sub") == 0)
                {
                  char *name = word_at (&p);
                  const char *q = p;

                  current = NULL;
                  while (*q == ' ' || *q == '\t')
                    q++;
                  if (name != NULL && *q == '(')
                    {
                      q++;
                      while (*q == ' ' || *q == '\t')
                        q++;
                      /* A macro takes nothing; an event handler of a
                       * sheet (Worksheet_Change) is not one. */
                      if (*q == ')' && !is_private && !private_module &&
                          !(kind == O42_VBA_DOCUMENT && strchr (name, '_') != NULL))
                        {
                          current = g_new0 (O42VbaMacro, 1);
                          current->module = g_strdup (module);
                          current->name = g_strdup (name);
                          current->description = g_strdup ("");
                          g_ptr_array_add (out, current);
                        }
                    }
                  g_free (name);
                }
              else if (word != NULL && (g_ascii_strcasecmp (word, "Function") == 0 ||
                                        g_ascii_strcasecmp (word, "Property") == 0))
                current = NULL;
            }
          g_free (first);
        }
      g_strfreev (lines);
    }
  return out;
}
