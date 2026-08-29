/* o42-xls.c - Excel's binary file format, BIFF in a compound file
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-xls.h"

/* Excel 97 to 2003 could hold this much and no more. */
#define O42_XLS_MAX_ROWS 65536
#define O42_XLS_MAX_COLS 256

/* How many cells the last save had to leave out.  A counter rather
 * than an error: the file is written and is good, and what is missing
 * from it is worth one sentence to whoever asked for it. */
int o42_xls_dropped_cells;

#include <string.h>

/* Border line styles as BIFF numbers them. */
static guint
xls_border_code (O42BorderStyle style)
{
  switch (style)
    {
    case O42_BORDER_THIN: return 1;
    case O42_BORDER_MEDIUM: return 2;
    case O42_BORDER_DASHED: return 3;
    case O42_BORDER_DOTTED: return 4;
    case O42_BORDER_THICK: return 5;
    case O42_BORDER_DOUBLE: return 6;
    default: return 0;
    }
}

static O42BorderStyle
xls_border_style (guint code)
{
  switch (code)
    {
    case 0: return O42_BORDER_NONE;
    case 2: case 8: case 9: case 10: case 12: case 13: return O42_BORDER_MEDIUM;
    case 3: case 11: return O42_BORDER_DASHED;
    case 4: case 7: return O42_BORDER_DOTTED;
    case 5: return O42_BORDER_THICK;
    case 6: return O42_BORDER_DOUBLE;
    default: return O42_BORDER_THIN;
    }
}
#include <stdlib.h>
#include <math.h>

#include "o42-ole2.h"
#include "o42-escher.h"
#include "o42-image.h"
#include "o42-chart.h"
#include "o42-xlsx.h"
#include "o42-formula.h"
#include "o42-eval.h"

/* Record ids */
enum {
  R_EOF = 0x000A, R_FORMULA = 0x0006, R_NAME = 0x0018, R_EXTERNSHEET = 0x0017,
  R_EXTERNNAME = 0x0023, R_CONTINUE = 0x003C, R_CODEPAGE = 0x0042, R_PANE = 0x0041,
  R_FONT = 0x0031, R_WINDOW1 = 0x003D, R_DEFCOLWIDTH = 0x0055, R_COLINFO = 0x007D,
  R_BOUNDSHEET = 0x0085, R_PALETTE = 0x0092, R_AUTOFILTERINFO = 0x009D,
  R_MULRK = 0x00BD, R_MULBLANK = 0x00BE, R_RSTRING = 0x00D6, R_XF = 0x00E0,
  R_MERGECELLS = 0x00E5, R_SST = 0x00FC, R_LABELSST = 0x00FD, R_EXTSST = 0x00FF,
  R_DIMENSIONS = 0x0200, R_BLANK = 0x0201, R_NUMBER = 0x0203, R_LABEL = 0x0204,
  R_BOOLERR = 0x0205, R_STRING = 0x0207, R_ROW = 0x0208, R_DEFAULTROWHEIGHT = 0x0225,
  R_WINDOW2 = 0x023E, R_RK = 0x027E, R_STYLE = 0x0293, R_FORMAT = 0x041E,
  R_SHRFMLA = 0x04BC, R_ARRAY = 0x0221, R_SUPBOOK = 0x01AE, R_BOF = 0x0809,
  R_BOF5 = 0x0409, R_INTERFACEHDR = 0x00E1, R_INTERFACEEND = 0x00E2,
  R_FORMAT5 = 0x001E, R_NOTE = 0x001C, R_OBJ = 0x005D, R_TXO = 0x01B6,
  R_CONDFMT = 0x01B0, R_CF = 0x01B1, R_DVAL = 0x01B2, R_DV = 0x01BE,
  R_MSODRAWINGGROUP = 0x00EB, R_MSODRAWING = 0x00EC,
  R_HEADER = 0x0014, R_FOOTER = 0x0015, R_HCENTER = 0x0083, R_VCENTER = 0x0084,
  R_SETUP = 0x00A1, R_PRINTSIZE = 0x0033, R_PROTECT = 0x0012,
  C_UNITS = 0x1001, C_CHART = 0x1002, C_SERIES = 0x1003, C_DATAFORMAT = 0x1006,
  C_LINEFORMAT = 0x1007, C_AREAFORMAT = 0x100A, C_SERIESTEXT = 0x100D, C_CHARTFORMAT = 0x1014,
  C_LEGEND = 0x1015, C_BAR = 0x1017, C_LINE = 0x1018, C_PIE = 0x1019, C_AREA = 0x101A,
  C_SCATTER = 0x101B, C_AXIS = 0x101D, C_TICK = 0x101E, C_VALUERANGE = 0x101F,
  C_CATSERRANGE = 0x1020, C_AXISLINEFORMAT = 0x1021, C_CHARTFORMATLINK = 0x1022,
  C_TEXT = 0x1025, C_FONTX = 0x1026, C_OBJECTLINK = 0x1027, C_FRAME = 0x1032,
  C_BEGIN = 0x1033, C_END = 0x1034, C_AXISPARENT = 0x1041, C_SHTPROPS = 0x1044,
  C_SERTOCRT = 0x1045, C_AXESUSED = 0x1046, C_AI = 0x1051, C_POS = 0x104F
};

/* Excel's numbers for its functions, with the argument counts that
 * decide between the fixed and variable call tokens.  max -1 is "any". */
typedef struct { const char *name; guint16 index; gint8 min, max; } FnEntry;

static const FnEntry FUNCTIONS[] = {
  { "COUNT", 0, 0, -1 }, { "IF", 1, 2, 3 }, { "ISNA", 2, 1, 1 }, { "ISERROR", 3, 1, 1 },
  { "SUM", 4, 0, -1 }, { "AVERAGE", 5, 1, -1 }, { "MIN", 6, 1, -1 }, { "MAX", 7, 1, -1 },
  { "ROW", 8, 0, 1 }, { "COLUMN", 9, 0, 1 }, { "NA", 10, 0, 0 }, { "NPV", 11, 2, -1 },
  { "STDEV", 12, 1, -1 }, { "DOLLAR", 13, 1, 2 }, { "FIXED", 14, 1, 3 }, { "SIN", 15, 1, 1 },
  { "COS", 16, 1, 1 }, { "TAN", 17, 1, 1 }, { "ATAN", 18, 1, 1 }, { "PI", 19, 0, 0 },
  { "SQRT", 20, 1, 1 }, { "EXP", 21, 1, 1 }, { "LN", 22, 1, 1 }, { "LOG10", 23, 1, 1 },
  { "ABS", 24, 1, 1 }, { "INT", 25, 1, 1 }, { "SIGN", 26, 1, 1 }, { "ROUND", 27, 2, 2 },
  { "LOOKUP", 28, 2, 3 }, { "INDEX", 29, 2, 4 }, { "REPT", 30, 2, 2 }, { "MID", 31, 3, 3 },
  { "LEN", 32, 1, 1 }, { "VALUE", 33, 1, 1 }, { "TRUE", 34, 0, 0 }, { "FALSE", 35, 0, 0 },
  { "AND", 36, 1, -1 }, { "OR", 37, 1, -1 }, { "NOT", 38, 1, 1 }, { "MOD", 39, 2, 2 },
  { "DCOUNT", 40, 3, 3 }, { "DSUM", 41, 3, 3 }, { "DAVERAGE", 42, 3, 3 }, { "DMIN", 43, 3, 3 },
  { "DMAX", 44, 3, 3 }, { "DSTDEV", 45, 3, 3 }, { "VAR", 46, 1, -1 }, { "DVAR", 47, 3, 3 },
  { "TEXT", 48, 2, 2 }, { "LINEST", 49, 1, 4 }, { "TREND", 50, 1, 4 }, { "LOGEST", 51, 1, 4 },
  { "GROWTH", 52, 1, 4 }, { "PV", 56, 3, 5 }, { "FV", 57, 3, 5 }, { "NPER", 58, 3, 5 },
  { "PMT", 59, 3, 5 }, { "RATE", 60, 3, 6 }, { "MIRR", 61, 3, 3 }, { "IRR", 62, 1, 2 },
  { "RAND", 63, 0, 0 }, { "MATCH", 64, 2, 3 }, { "DATE", 65, 3, 3 }, { "TIME", 66, 3, 3 },
  { "DAY", 67, 1, 1 }, { "MONTH", 68, 1, 1 }, { "YEAR", 69, 1, 1 }, { "WEEKDAY", 70, 1, 2 },
  { "HOUR", 71, 1, 1 }, { "MINUTE", 72, 1, 1 }, { "SECOND", 73, 1, 1 }, { "NOW", 74, 0, 0 },
  { "AREAS", 75, 1, 1 }, { "ROWS", 76, 1, 1 }, { "COLUMNS", 77, 1, 1 }, { "OFFSET", 78, 3, 5 },
  { "SEARCH", 82, 2, 3 }, { "TRANSPOSE", 83, 1, 1 }, { "TYPE", 86, 1, 1 }, { "ATAN2", 97, 2, 2 },
  { "ASIN", 98, 1, 1 }, { "ACOS", 99, 1, 1 }, { "CHOOSE", 100, 2, -1 }, { "HLOOKUP", 101, 3, 4 },
  { "VLOOKUP", 102, 3, 4 }, { "ISREF", 105, 1, 1 }, { "LOG", 109, 1, 2 }, { "CHAR", 111, 1, 1 },
  { "LOWER", 112, 1, 1 }, { "UPPER", 113, 1, 1 }, { "PROPER", 114, 1, 1 }, { "LEFT", 115, 1, 2 },
  { "RIGHT", 116, 1, 2 }, { "EXACT", 117, 2, 2 }, { "TRIM", 118, 1, 1 }, { "REPLACE", 119, 4, 4 },
  { "SUBSTITUTE", 120, 3, 4 }, { "CODE", 121, 1, 1 }, { "FIND", 124, 2, 3 }, { "CELL", 125, 1, 2 },
  { "ISERR", 126, 1, 1 }, { "ISTEXT", 127, 1, 1 }, { "ISNUMBER", 128, 1, 1 }, { "ISBLANK", 129, 1, 1 },
  { "T", 130, 1, 1 }, { "N", 131, 1, 1 }, { "DATEVALUE", 140, 1, 1 }, { "TIMEVALUE", 141, 1, 1 },
  { "SLN", 142, 3, 3 }, { "SYD", 143, 4, 4 }, { "DDB", 144, 4, 5 }, { "INDIRECT", 148, 1, 2 },
  { "CLEAN", 162, 1, 1 }, { "MDETERM", 163, 1, 1 }, { "MINVERSE", 164, 1, 1 }, { "MMULT", 165, 2, 2 },
  { "IPMT", 167, 4, 6 }, { "PPMT", 168, 4, 6 }, { "COUNTA", 169, 0, -1 }, { "PRODUCT", 183, 0, -1 },
  { "FACT", 184, 1, 1 }, { "DPRODUCT", 189, 3, 3 }, { "ISNONTEXT", 190, 1, 1 }, { "STDEVP", 193, 1, -1 },
  { "VARP", 194, 1, -1 }, { "DSTDEVP", 195, 3, 3 }, { "DVARP", 196, 3, 3 }, { "TRUNC", 197, 1, 2 },
  { "ISLOGICAL", 198, 1, 1 }, { "DCOUNTA", 199, 3, 3 }, { "ROUNDUP", 212, 2, 2 }, { "ROUNDDOWN", 213, 2, 2 },
  { "RANK", 216, 2, 3 }, { "ADDRESS", 219, 2, 5 }, { "DAYS360", 220, 2, 3 }, { "TODAY", 221, 0, 0 },
  { "VDB", 222, 5, 7 }, { "MEDIAN", 227, 1, -1 }, { "SUMPRODUCT", 228, 1, -1 }, { "SINH", 229, 1, 1 },
  { "COSH", 230, 1, 1 }, { "TANH", 231, 1, 1 }, { "ASINH", 232, 1, 1 }, { "ACOSH", 233, 1, 1 },
  { "ATANH", 234, 1, 1 }, { "DGET", 235, 3, 3 }, { "INFO", 244, 1, 1 }, { "DB", 247, 4, 5 },
  { "FREQUENCY", 252, 2, 2 }, { "ERROR.TYPE", 261, 1, 1 }, { "AVEDEV", 269, 1, -1 }, { "BETADIST", 270, 3, 5 },
  { "GAMMALN", 271, 1, 1 }, { "BETAINV", 272, 3, 5 }, { "BINOMDIST", 273, 4, 4 }, { "CHIDIST", 274, 2, 2 },
  { "CHIINV", 275, 2, 2 }, { "COMBIN", 276, 2, 2 }, { "CONFIDENCE", 277, 3, 3 }, { "CRITBINOM", 278, 3, 3 },
  { "EVEN", 279, 1, 1 }, { "EXPONDIST", 280, 3, 3 }, { "FDIST", 281, 3, 3 }, { "FINV", 282, 3, 3 },
  { "FISHER", 283, 1, 1 }, { "FISHERINV", 284, 1, 1 }, { "FLOOR", 285, 2, 2 }, { "GAMMADIST", 286, 4, 4 },
  { "GAMMAINV", 287, 3, 3 }, { "CEILING", 288, 2, 2 }, { "HYPGEOMDIST", 289, 4, 4 }, { "LOGNORMDIST", 290, 3, 3 },
  { "LOGINV", 291, 3, 3 }, { "NEGBINOMDIST", 292, 3, 3 }, { "NORMDIST", 293, 4, 4 }, { "NORMSDIST", 294, 1, 1 },
  { "NORMINV", 295, 3, 3 }, { "NORMSINV", 296, 1, 1 }, { "STANDARDIZE", 297, 3, 3 }, { "ODD", 298, 1, 1 },
  { "PERMUT", 299, 2, 2 }, { "POISSON", 300, 3, 3 }, { "TDIST", 301, 3, 3 }, { "WEIBULL", 302, 4, 4 },
  { "SUMXMY2", 303, 2, 2 }, { "SUMX2MY2", 304, 2, 2 }, { "SUMX2PY2", 305, 2, 2 }, { "CHITEST", 306, 2, 2 },
  { "CORREL", 307, 2, 2 }, { "COVAR", 308, 2, 2 }, { "FORECAST", 309, 3, 3 }, { "FTEST", 310, 2, 2 },
  { "INTERCEPT", 311, 2, 2 }, { "PEARSON", 312, 2, 2 }, { "RSQ", 313, 2, 2 }, { "STEYX", 314, 2, 2 },
  { "SLOPE", 315, 2, 2 }, { "TTEST", 316, 4, 4 }, { "PROB", 317, 3, 4 }, { "DEVSQ", 318, 1, -1 },
  { "GEOMEAN", 319, 1, -1 }, { "HARMEAN", 320, 1, -1 }, { "SUMSQ", 321, 0, -1 }, { "KURT", 322, 1, -1 },
  { "SKEW", 323, 1, -1 }, { "ZTEST", 324, 2, 3 }, { "LARGE", 325, 2, 2 }, { "SMALL", 326, 2, 2 },
  { "QUARTILE", 327, 2, 2 }, { "PERCENTILE", 328, 2, 2 }, { "PERCENTRANK", 329, 2, 3 }, { "MODE", 330, 1, -1 },
  { "TRIMMEAN", 331, 2, 2 }, { "TINV", 332, 2, 2 }, { "CONCATENATE", 336, 0, -1 }, { "POWER", 337, 2, 2 },
  { "RADIANS", 342, 1, 1 }, { "DEGREES", 343, 1, 1 }, { "SUBTOTAL", 344, 2, -1 }, { "SUMIF", 345, 2, 3 },
  { "COUNTIF", 346, 2, 2 }, { "COUNTBLANK", 347, 1, 1 }, { "ISPMT", 350, 4, 4 }, { "DATEDIF", 351, 3, 3 },
  { "ROMAN", 354, 1, 2 }, { "HYPERLINK", 359, 1, 2 }, { "AVERAGEA", 361, 1, -1 }, { "MAXA", 362, 1, -1 },
  { "MINA", 363, 1, -1 }, { "STDEVPA", 364, 1, -1 }, { "VARPA", 365, 1, -1 }, { "STDEVA", 366, 1, -1 },
  { "VARA", 367, 1, -1 },
};

static const FnEntry *
function_by_name (const char *name)
{
  for (guint i = 0; i < G_N_ELEMENTS (FUNCTIONS); i++)
    if (strcmp (FUNCTIONS[i].name, name) == 0)
      return &FUNCTIONS[i];
  return NULL;
}

static const FnEntry *
function_by_index (guint index)
{
  for (guint i = 0; i < G_N_ELEMENTS (FUNCTIONS); i++)
    if (FUNCTIONS[i].index == index)
      return &FUNCTIONS[i];
  return NULL;
}

/* Excel's default palette, indices 8 to 63. */
static const guint32 PALETTE[56] = {
  0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF,
  0x800000, 0x008000, 0x000080, 0x808000, 0x800080, 0x008080, 0xC0C0C0, 0x808080,
  0x9999FF, 0x993366, 0xFFFFCC, 0xCCFFFF, 0x660066, 0xFF8080, 0x0066CC, 0xCCCCFF,
  0x000080, 0xFF00FF, 0xFFFF00, 0x00FFFF, 0x800080, 0x800000, 0x008080, 0x0000FF,
  0x00CCFF, 0xCCFFFF, 0xCCFFCC, 0xFFFF99, 0x99CCFF, 0xFF99CC, 0xCC99FF, 0xFFCC99,
  0x3366FF, 0x33CCCC, 0x99CC00, 0xFFCC00, 0xFF9900, 0xFF6600, 0x666699, 0x969696,
  0x003366, 0x339966, 0x003300, 0x333300, 0x993300, 0x993366, 0x333399, 0x333333
};

static O42ErrorCode
error_from_biff (guint code)
{
  switch (code)
    {
    case 0x00: return O42_ERR_NULL;
    case 0x07: return O42_ERR_DIV0;
    case 0x0F: return O42_ERR_VALUE;
    case 0x17: return O42_ERR_REF;
    case 0x1D: return O42_ERR_NAME;
    case 0x24: return O42_ERR_NUM;
    default:   return O42_ERR_NA;
    }
}

static guint
error_to_biff (O42ErrorCode code)
{
  switch (code)
    {
    case O42_ERR_NULL:  return 0x00;
    case O42_ERR_DIV0:  return 0x07;
    case O42_ERR_VALUE: return 0x0F;
    case O42_ERR_REF:
    case O42_ERR_CIRCULAR: return 0x17;
    case O42_ERR_NAME:  return 0x1D;
    case O42_ERR_NUM:   return 0x24;
    default:            return 0x2A;
    }
}

/* ====================================================================== */
/* Byte helpers                                                            */
/* ====================================================================== */

static guint16 rd16 (const guchar *p) { return p[0] | (p[1] << 8); }
static guint32 rd32 (const guchar *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((guint32) p[3] << 24); }

static double
rd_double (const guchar *p)
{
  guint64 bits = (guint64) rd32 (p) | ((guint64) rd32 (p + 4) << 32);
  double d;
  memcpy (&d, &bits, 8);
  return d;
}

static void put8 (GByteArray *a, guint v) { guchar b = v; g_byte_array_append (a, &b, 1); }
static void put16 (GByteArray *a, guint v) { guchar b[2] = { v & 0xff, (v >> 8) & 0xff }; g_byte_array_append (a, b, 2); }
static void put32 (GByteArray *a, guint32 v) { guchar b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff }; g_byte_array_append (a, b, 4); }

static void
put_double (GByteArray *a, double d)
{
  guint64 bits;
  memcpy (&bits, &d, 8);
  put32 (a, (guint32) bits);
  put32 (a, (guint32) (bits >> 32));
}

/* Whether every character fits in a byte, which decides the compressed
 * form of a BIFF8 string. */
static gboolean
is_latin1 (const char *text)
{
  for (const char *p = text; *p; p = g_utf8_next_char (p))
    if (g_utf8_get_char (p) > 0xFF)
      return FALSE;
  return TRUE;
}

/* A BIFF8 unicode string body: flags byte then characters, after the
 * caller has written the length in whatever width the record wants. */
static void
put_ustr_body (GByteArray *a, const char *text)
{
  if (is_latin1 (text))
    {
      put8 (a, 0);
      for (const char *p = text; *p; p = g_utf8_next_char (p))
        put8 (a, g_utf8_get_char (p));
    }
  else
    {
      glong n = 0;
      gunichar2 *u = g_utf8_to_utf16 (text, -1, NULL, &n, NULL);
      put8 (a, 1);
      for (glong i = 0; i < n; i++)
        put16 (a, u[i]);
      g_free (u);
    }
}

static glong
char_count (const char *text)
{
  glong n = 0;
  gunichar2 *u = g_utf8_to_utf16 (text, -1, NULL, &n, NULL);
  g_free (u);
  return n;
}

static void
put_ustr8 (GByteArray *a, const char *text)
{
  put8 (a, MIN (char_count (text), 255));
  put_ustr_body (a, text);
}

static void
put_ustr16 (GByteArray *a, const char *text)
{
  put16 (a, MIN (char_count (text), 65535));
  put_ustr_body (a, text);
}

/* ====================================================================== */
/* Reading                                                                 */
/* ====================================================================== */

/* What one OBJ record said, kept until the sheet's drawing is read:
 * the anchored shapes and the OBJ records come in the same order, so
 * the nth of these belongs to the nth shape. */
typedef struct {
  guint16  ot;
  int      kind;          /* an O42ShapeKind, or -1 for anything else */
  char    *link;
  char    *source;
  char    *text;          /* the caption, from the TXO that follows */
  double   value, min, max, step, page;
  int      selected;
  gboolean checked;
  gboolean have_bounds;
} ObjInfo;

static void
obj_info_clear (ObjInfo *info)
{
  g_clear_pointer (&info->link, g_free);
  g_clear_pointer (&info->source, g_free);
  g_clear_pointer (&info->text, g_free);
}

typedef struct
{
  O42Book    *book;
  int         biff;          /* 5 or 8 */
  const guchar *data;
  gsize       len;

  GPtrArray  *sst;           /* shared strings */
  GHashTable *formats;       /* id -> code */
  GArray     *fonts;         /* O42Fmt with the font fields */
  GArray     *xfs;           /* O42Fmt */
  /* FONT and XF records name palette colours by index, and the PALETTE
   * record that defines them comes after them in the stream; the
   * records wait here until the globals are read. */
  GPtrArray  *pending_fonts; /* GBytes, the record bodies */
  GPtrArray  *pending_xfs;
  gboolean    styles_parsed;
  guint32     palette[64];
  GPtrArray  *sheet_names;
  GArray     *sheet_offsets; /* guint32 */
  GArray     *xti;           /* guint16 triples: supbook, first, last */
  GPtrArray  *supbook_names; /* GPtrArray* of char* per supbook, add-in names */
  GPtrArray  *supbook_self;  /* GINT: 1 if the supbook is this workbook */
  GPtrArray  *names;         /* char* name text, for ptgName */
  GPtrArray  *name_ranges;   /* char* "Sheet!A1:B2" or NULL per name */
  GArray     *filter_sheets; /* int: sheet index whose _FilterDatabase was seen */
  GPtrArray  *filter_ranges; /* char* range text */
  int         n_format5;     /* BIFF5 FORMAT records are numbered in order */

  /* Current sheet */
  O42Sheet   *sheet;
  int         sheet_index;
  GHashTable *shared;        /* o42_key -> GBytes rgce */
  int         pending_row, pending_col;   /* formula waiting on a STRING record */
  gboolean    pending;
  int         default_width;

  /* Notes: OBJ gives an id, TXO and its CONTINUEs the text, NOTE the cell. */
  guint       obj_id;
  gboolean    obj_is_note;
  int         txo_chars;        /* characters still to read for the note */
  GString    *txo_text;
  GHashTable *note_texts;       /* obj id -> char* */

  /* Conditional formats: a CONDFMT's range, then its CF rules. */
  O42Range    cf_range;
  gboolean    cf_have_range;

  /* Drawings: the group's images, and the sheet's Escher bytes. */
  GPtrArray  *images;           /* GBytes */
  GPtrArray  *image_formats;    /* interned names */
  GByteArray *group;            /* MSODRAWINGGROUP and its CONTINUEs */
  GByteArray *drawing;          /* the sheet's MSODRAWING bodies */
  gboolean    group_open;       /* the last record was the group or its CONTINUE */
  int         embedded;         /* depth of chart substreams inside the sheet */

  /* An embedded chart being read: its kind, the union of its series'
   * ranges, what the AI records said, and its title. */
  GArray     *chart_defs;       /* ChartDef, in the order the charts came */
  GArray     *objs;             /* ObjInfo: what each OBJ said, in order */
  gboolean    in_series;
  int         chart_depth;
} Reader;

typedef struct {
  O42ChartKind kind;
  gboolean     kind_known;
  O42Range     box;
  gboolean     have_box, have_title_ref, have_cats;
  char        *title;
} ChartDef;

/* A string in the encoding the record uses: BIFF8 unicode with flags,
 * BIFF5 bytes.  `wide_len` says whether the length is 16 bits.  Returns
 * the string and advances *p. */
static char *
read_str (Reader *r, const guchar **pp, const guchar *end, gboolean wide_len)
{
  const guchar *p = *pp;
  guint n;
  GString *s = g_string_new (NULL);

  if (p + (wide_len ? 2 : 1) > end)
    { *pp = end; return g_string_free (s, FALSE); }
  n = wide_len ? rd16 (p) : p[0];
  p += wide_len ? 2 : 1;

  if (r->biff >= 8)
    {
      guint flags;
      guint runs = 0;
      guint32 ext = 0;
      if (p >= end) { *pp = end; return g_string_free (s, FALSE); }
      flags = *p++;
      if (flags & 0x08) { runs = p + 2 <= end ? rd16 (p) : 0; p += 2; }
      if (flags & 0x04) { ext = p + 4 <= end ? rd32 (p) : 0; p += 4; }
      if (flags & 0x01)
        {
          /* A NUL separates the entries of a validation list; a comma
           * is what office42 separates them with. */
          for (guint i = 0; i < n && p + 2 <= end; i++, p += 2)
            {
              if (rd16 (p) != 0) g_string_append_unichar (s, rd16 (p));
              else if (n > 1) g_string_append_c (s, ',');
            }
        }
      else
        {
          for (guint i = 0; i < n && p < end; i++, p++)
            {
              if (*p != 0) g_string_append_unichar (s, *p);
              else if (n > 1) g_string_append_c (s, ',');
            }
        }
      p += runs * 4 + ext;
    }
  else
    {
      for (guint i = 0; i < n && p < end; i++, p++)
        g_string_append_unichar (s, *p);
    }
  *pp = MIN (p, end);
  return g_string_free (s, FALSE);
}

/* The SST spans CONTINUE records, and a string may be cut between
 * them, at which point a fresh flags byte says how the rest is coded.
 * `segs` are the record bodies in order. */
static void
read_sst (Reader *r, GPtrArray *segs)
{
  guint seg = 0;
  const guchar *p, *end;
  guint32 unique;

  if (segs->len == 0) return;
  p = g_bytes_get_data (g_ptr_array_index (segs, 0), NULL);
  end = p + g_bytes_get_size (g_ptr_array_index (segs, 0));
  if (end - p < 8) return;
  unique = rd32 (p + 4);
  p += 8;

#define NEXT_SEG() do { \
    seg++; \
    if (seg >= segs->len) return; \
    p = g_bytes_get_data (g_ptr_array_index (segs, seg), NULL); \
    end = p + g_bytes_get_size (g_ptr_array_index (segs, seg)); \
  } while (0)

  for (guint32 k = 0; k < unique; k++)
    {
      guint n, flags, runs = 0;
      guint32 ext = 0;
      GString *s;

      if (p >= end) NEXT_SEG ();
      if (end - p < 3) { NEXT_SEG (); }
      n = rd16 (p); p += 2;
      flags = *p++;
      if (flags & 0x08) { runs = rd16 (p); p += 2; }
      if (flags & 0x04) { ext = rd32 (p); p += 4; }
      s = g_string_new (NULL);
      for (guint i = 0; i < n; i++)
        {
          if (p >= end)
            {
              NEXT_SEG ();
              flags = (*p++ & 0x01) | (flags & ~0x01u);
            }
          if (flags & 0x01)
            { if (p + 2 > end) break; g_string_append_unichar (s, rd16 (p)); p += 2; }
          else
            g_string_append_unichar (s, *p++);
        }
      g_ptr_array_add (r->sst, g_string_free (s, FALSE));
      {
        gsize skip = (gsize) runs * 4 + ext;
        while (skip > 0)
          {
            gsize here = MIN (skip, (gsize) (end - p));
            p += here;
            skip -= here;
            if (skip > 0) NEXT_SEG ();
          }
      }
    }
#undef NEXT_SEG
}

/* ---- formula tokens to a node tree ---- */

typedef struct
{
  Reader   *r;
  int       base_row, base_col;   /* for the relative tokens of shared formulas */
  GPtrArray *stack;               /* O42Node* */
} Decoder;

static O42Node *
node_new (O42NodeType type)
{
  O42Node *n = g_new0 (O42Node, 1);
  n->type = type;
  return n;
}

static void
push (Decoder *d, O42Node *n)
{
  g_ptr_array_add (d->stack, n);
}

static O42Node *
pop (Decoder *d)
{
  if (d->stack->len == 0)
    return node_new (O42_NODE_NUMBER);
  return g_ptr_array_steal_index (d->stack, d->stack->len - 1);
}

static const char *
sheet_last_for_xti (Reader *r, guint ixti)
{
  if (ixti * 3 + 2 < r->xti->len)
    {
      guint16 supbook = g_array_index (r->xti, guint16, ixti * 3);
      guint16 first = g_array_index (r->xti, guint16, ixti * 3 + 1);
      guint16 last = g_array_index (r->xti, guint16, ixti * 3 + 2);
      gboolean self = supbook < r->supbook_self->len &&
                      GPOINTER_TO_INT (g_ptr_array_index (r->supbook_self, supbook));
      if (self && last != first && last < r->sheet_names->len)
        return g_intern_string (g_ptr_array_index (r->sheet_names, last));
    }
  return NULL;
}

static const char *
sheet_for_xti (Reader *r, guint ixti, gboolean *addin)
{
  *addin = FALSE;
  if (ixti * 3 + 2 < r->xti->len)
    {
      guint16 supbook = g_array_index (r->xti, guint16, ixti * 3);
      guint16 first = g_array_index (r->xti, guint16, ixti * 3 + 1);
      gboolean self = supbook < r->supbook_self->len &&
                      GPOINTER_TO_INT (g_ptr_array_index (r->supbook_self, supbook));
      if (self && first < r->sheet_names->len)
        return g_intern_string (g_ptr_array_index (r->sheet_names, first));
      if (first == 0xFFFE)
        *addin = TRUE;
    }
  return NULL;
}

/* A row/column pair from a BIFF8 token: relative flags in the column's
 * top bits.  In shared formulas the relative parts are offsets from
 * the cell. */
static void
decode_ref8 (Decoder *d, const guchar *p, gboolean shared, int *row, int *col, guint8 *abs)
{
  guint16 rw = rd16 (p), cl = rd16 (p + 2);
  gboolean row_rel = (cl & 0x8000) != 0, col_rel = (cl & 0x4000) != 0;
  int c = cl & 0x00FF;
  int rr = rw;

  if (shared)
    {
      if (row_rel) rr = (d->base_row + (gint16) rw) & 0xFFFF;
      if (col_rel) c = (d->base_col + (gint8) c) & 0xFF;
    }
  *row = rr;
  *col = c;
  *abs = (row_rel ? 0 : O42_ABS_ROW0) | (col_rel ? 0 : O42_ABS_COL0);
}

static void
decode_ref5 (Decoder *d, const guchar *p, gboolean shared, int *row, int *col, guint8 *abs)
{
  guint16 rw = rd16 (p);
  int c = p[2];
  gboolean row_rel = (rw & 0x8000) != 0, col_rel = (rw & 0x4000) != 0;
  int rr = rw & 0x3FFF;

  if (shared)
    {
      if (row_rel) rr = (d->base_row + (gint16) (rw & 0x3FFF)) & 0x3FFF;
      if (col_rel) c = (d->base_col + (gint8) c) & 0xFF;
    }
  *row = rr;
  *col = c;
  *abs = (row_rel ? 0 : O42_ABS_ROW0) | (col_rel ? 0 : O42_ABS_COL0);
}

static O42Node *
make_call (const char *name, GPtrArray *args)
{
  O42Node *n = node_new (O42_NODE_CALL);
  /* Newer functions come as add-in names with Excel's _xlfn. prefix. */
  if (g_ascii_strncasecmp (name, "_xlfn.", 6) == 0)
    name += 6;
  n->as.call.name = g_ascii_strup (name, -1);
  n->as.call.args = args;
  return n;
}

/* An array constant's cells follow the token stream; each ptgArray
 * takes the next one. */
static O42Node *
decode_array (Reader *r, const guchar **pp, const guchar *end)
{
  const guchar *p = *pp;
  O42Node *n = node_new (O42_NODE_ARRAY);
  int cols, rows;

  n->as.array.items = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_node_free);
  if (p + 3 > end)
    { n->as.array.rows = n->as.array.cols = 0; *pp = end; return n; }
  cols = p[0] + 1;
  rows = rd16 (p + 1) + 1;
  p += 3;
  n->as.array.rows = rows;
  n->as.array.cols = cols;
  for (int i = 0; i < rows * cols && p < end; i++)
    {
      guint type = *p++;
      O42Node *item;
      switch (type)
        {
        case 0x01:
          item = node_new (O42_NODE_NUMBER);
          item->as.number = p + 8 <= end ? rd_double (p) : 0;
          p += 8;
          break;
        case 0x02:
          item = node_new (O42_NODE_STRING);
          item->as.string = read_str (r, &p, end, r->biff >= 8);
          break;
        case 0x04:
          item = node_new (O42_NODE_BOOL);
          item->as.boolean = p < end && *p != 0;
          p += 8;
          break;
        case 0x10:
          item = node_new (O42_NODE_ERROR);
          item->as.error = error_from_biff (p < end ? *p : 0x2A);
          p += 8;
          break;
        default:
          item = node_new (O42_NODE_EMPTY);
          p += 8;
          break;
        }
      g_ptr_array_add (n->as.array.items, item);
    }
  while ((int) n->as.array.items->len < rows * cols)
    g_ptr_array_add (n->as.array.items, node_new (O42_NODE_EMPTY));
  *pp = MIN (p, end);
  return n;
}

static O42Node *
decode_formula (Reader *r, const guchar *p, gsize len, int base_row, int base_col, gboolean shared,
                const guchar *extra, const guchar *extra_end)
{
  Decoder d = { r, base_row, base_col, g_ptr_array_new () };
  const guchar *end = p + len;
  gboolean biff8 = r->biff >= 8;
  O42Node *result;

  while (p < end)
    {
      guint ptg = *p++;
      guint base = ptg;

      if (ptg >= 0x20 && ptg < 0x80)
        base = 0x20 + ((ptg - 0x20) & 0x1F);   /* strip the class bits */

      /* Every token's fixed operand size, so a cut-off or hostile record
       * ends the formula rather than reading past it.  Strings and
       * arrays are bounded where they are read. */
      {
        static const guint8 size8[0x40] = {
          [0x01] = 4, [0x19] = 3, [0x1C] = 1, [0x1D] = 1, [0x1E] = 2, [0x1F] = 8,
          [0x20] = 7, [0x21] = 2, [0x22] = 3, [0x23] = 4, [0x24] = 4, [0x25] = 8,
          [0x26] = 6, [0x27] = 6, [0x28] = 2, [0x29] = 2, [0x2A] = 4, [0x2B] = 8,
          [0x2C] = 4, [0x2D] = 8, [0x39] = 6, [0x3A] = 6, [0x3B] = 10, [0x3C] = 6, [0x3D] = 10 };
        static const guint8 size5[0x40] = {
          [0x01] = 4, [0x19] = 3, [0x1C] = 1, [0x1D] = 1, [0x1E] = 2, [0x1F] = 8,
          [0x20] = 7, [0x21] = 2, [0x22] = 3, [0x23] = 14, [0x24] = 3, [0x25] = 6,
          [0x26] = 6, [0x27] = 6, [0x28] = 2, [0x29] = 2, [0x2A] = 3, [0x2B] = 6,
          [0x2C] = 3, [0x2D] = 6, [0x39] = 24, [0x3A] = 17, [0x3B] = 20, [0x3C] = 17, [0x3D] = 20 };
        guint need = base < 0x40 ? (biff8 ? size8[base] : size5[base]) : 0;

        if (p + need > end)
          {
            O42Node *n = node_new (O42_NODE_ERROR);
            n->as.error = O42_ERR_VALUE;
            push (&d, n);
            p = end;
            break;
          }
      }

      switch (base)
        {
        case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08:
        case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E:
          {
            static const O42Op ops[] = { O42_OP_ADD, O42_OP_SUB, O42_OP_MUL, O42_OP_DIV, O42_OP_POW,
                                         O42_OP_CONCAT, O42_OP_LT, O42_OP_LE, O42_OP_EQ, O42_OP_GE,
                                         O42_OP_GT, O42_OP_NE };
            O42Node *b = pop (&d), *a = pop (&d);
            O42Node *n = node_new (O42_NODE_BINARY);
            n->as.op.op = ops[base - 0x03];
            n->as.op.a = a;
            n->as.op.b = b;
            push (&d, n);
          }
          break;
        case 0x0F: case 0x10: case 0x11:   /* intersection, union, range: keep the first */
          {
            O42Node *b = pop (&d);
            o42_node_free (b);
          }
          break;
        case 0x12: case 0x13: case 0x14:
          {
            O42Node *a = pop (&d);
            O42Node *n = node_new (O42_NODE_UNARY);
            n->as.op.op = base == 0x12 ? O42_OP_POS : base == 0x13 ? O42_OP_NEG : O42_OP_PERCENT;
            n->as.op.a = a;
            push (&d, n);
          }
          break;
        case 0x15:   /* parentheses: the writer adds its own */
          break;
        case 0x16:   /* missing argument */
          push (&d, node_new (O42_NODE_EMPTY));
          break;
        case 0x17:
          {
            O42Node *n = node_new (O42_NODE_STRING);
            n->as.string = read_str (r, &p, end, FALSE);
            push (&d, n);
          }
          break;
        case 0x19:   /* attributes */
          {
            guint kind = p < end ? *p : 0;
            guint16 w = p + 3 <= end ? rd16 (p + 1) : 0;
            p += 3;
            if (kind & 0x04)
              p += (w + 1) * 2;   /* choose: jump table */
            else if (kind & 0x10)
              {
                GPtrArray *args = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_node_free);
                g_ptr_array_add (args, pop (&d));
                push (&d, make_call ("SUM", args));
              }
          }
          break;
        case 0x1C:
          {
            O42Node *n = node_new (O42_NODE_ERROR);
            n->as.error = error_from_biff (p < end ? *p : 0x2A);
            p++;
            push (&d, n);
          }
          break;
        case 0x1D:
          {
            O42Node *n = node_new (O42_NODE_BOOL);
            n->as.boolean = p < end && *p != 0;
            p++;
            push (&d, n);
          }
          break;
        case 0x1E:
          {
            O42Node *n = node_new (O42_NODE_NUMBER);
            n->as.number = p + 2 <= end ? rd16 (p) : 0;
            p += 2;
            push (&d, n);
          }
          break;
        case 0x1F:
          {
            O42Node *n = node_new (O42_NODE_NUMBER);
            n->as.number = p + 8 <= end ? rd_double (p) : 0;
            p += 8;
            push (&d, n);
          }
          break;
        case 0x20:   /* array constant: the data follows the token stream */
          p += 7;
          if (extra != NULL && extra < extra_end)
            push (&d, decode_array (r, &extra, extra_end));
          else
            push (&d, node_new (O42_NODE_EMPTY));
          break;
        case 0x21: case 0x22:
          {
            guint argc, index;
            const FnEntry *fn;
            GPtrArray *args = g_ptr_array_new_with_free_func ((GDestroyNotify) o42_node_free);

            if (base == 0x21)
              {
                index = p + 2 <= end ? rd16 (p) : 0;
                p += 2;
                fn = function_by_index (index);
                argc = fn ? fn->min : 0;
              }
            else
              {
                argc = p < end ? (*p & 0x7F) : 0;
                index = p + 3 <= end ? rd16 (p + 1) & 0x7FFF : 0;
                p += 3;
                fn = function_by_index (index);
              }
            if (index == 255)
              {
                /* An add-in function: the name was pushed first. */
                O42Node *name_node;
                for (guint i = 1; i < argc; i++)
                  g_ptr_array_insert (args, 0, pop (&d));
                name_node = pop (&d);
                if (name_node->type == O42_NODE_NAME)
                  push (&d, make_call (name_node->as.name, args));
                else
                  {
                    O42Node *n = node_new (O42_NODE_ERROR);
                    n->as.error = O42_ERR_NAME;
                    g_ptr_array_unref (args);
                    push (&d, n);
                  }
                o42_node_free (name_node);
              }
            else
              {
                for (guint i = 0; i < argc; i++)
                  g_ptr_array_insert (args, 0, pop (&d));
                if (fn != NULL)
                  push (&d, make_call (fn->name, args));
                else
                  {
                    O42Node *n = node_new (O42_NODE_ERROR);
                    n->as.error = O42_ERR_NAME;
                    g_ptr_array_unref (args);
                    push (&d, n);
                  }
              }
          }
          break;
        case 0x23:   /* a defined name */
          {
            guint idx = p + 2 <= end ? rd16 (p) : 0;
            O42Node *n = node_new (O42_NODE_NAME);
            p += biff8 ? 4 : 14;
            n->as.name = g_strdup (idx >= 1 && idx <= r->names->len
                                   ? g_ptr_array_index (r->names, idx - 1) : "?");
            push (&d, n);
          }
          break;
        case 0x24: case 0x2C:
          {
            O42Node *n = node_new (O42_NODE_REF);
            int row, col;
            guint8 abs;
            if (biff8)
              { decode_ref8 (&d, p, shared || base == 0x2C, &row, &col, &abs); p += 4; }
            else
              { decode_ref5 (&d, p, shared || base == 0x2C, &row, &col, &abs); p += 3; }
            n->as.ref.row = row;
            n->as.ref.col = col;
            n->abs = abs;
            push (&d, n);
          }
          break;
        case 0x25: case 0x2D:
          {
            O42Node *n = node_new (O42_NODE_RANGE);
            int r0, c0, r1, c1;
            guint8 a0, a1;
            if (biff8)
              {
                guchar first[4] = { p[0], p[1], p[4], p[5] };
                guchar last[4] = { p[2], p[3], p[6], p[7] };
                decode_ref8 (&d, first, shared || base == 0x2D, &r0, &c0, &a0);
                decode_ref8 (&d, last, shared || base == 0x2D, &r1, &c1, &a1);
                p += 8;
              }
            else
              {
                guchar first[3] = { p[0], p[1], p[4] };
                guchar last[3] = { p[2], p[3], p[5] };
                decode_ref5 (&d, first, shared || base == 0x2D, &r0, &c0, &a0);
                decode_ref5 (&d, last, shared || base == 0x2D, &r1, &c1, &a1);
                p += 6;
              }
            n->as.range = o42_range_normalise (r0, c0, r1, c1);
            n->abs = a0 | (a1 << 2);
            push (&d, n);
          }
          break;
        case 0x26: p += biff8 ? 6 : 6; break;   /* memory area: skip its header */
        case 0x27: p += 6; break;
        case 0x28: p += 2; break;
        case 0x29: p += 2; break;
        case 0x2A: case 0x2B:
          {
            O42Node *n = node_new (O42_NODE_ERROR);
            n->as.error = O42_ERR_REF;
            p += base == 0x2A ? (biff8 ? 4 : 3) : (biff8 ? 8 : 6);
            push (&d, n);
          }
          break;
        case 0x39:   /* an external name: an add-in function, most likely */
          {
            guint ixti = p + 2 <= end ? rd16 (p) : 0;
            guint iname = p + 4 <= end ? rd16 (p + 2) : 0;
            O42Node *n = node_new (O42_NODE_NAME);
            const char *text = "?";
            p += biff8 ? 6 : 24;
            if (biff8 && ixti * 3 + 2 < r->xti->len)
              {
                guint16 supbook = g_array_index (r->xti, guint16, ixti * 3);
                if (supbook < r->supbook_names->len)
                  {
                    GPtrArray *names = g_ptr_array_index (r->supbook_names, supbook);
                    if (iname >= 1 && iname <= names->len)
                      text = g_ptr_array_index (names, iname - 1);
                  }
              }
            n->as.name = g_strdup (g_str_has_prefix (text, "_xlpm.") ? text + 6 : text);
            push (&d, n);
          }
          break;
        case 0x3A: case 0x3B:
          {
            O42Node *n;
            const char *sheet = NULL, *sheet_last = NULL;
            gboolean addin;
            if (biff8)
              {
                guint ixti = rd16 (p);
                sheet = sheet_for_xti (r, ixti, &addin);
                sheet_last = sheet_last_for_xti (r, ixti);
                p += 2;
              }
            else
              {
                /* BIFF5: ixals, 8 reserved bytes, itabFirst, itabLast */
                guint itab = p + 12 <= end ? rd16 (p + 10) : 0;
                if (itab < r->sheet_names->len)
                  sheet = g_intern_string (g_ptr_array_index (r->sheet_names, itab));
                p += 14;
              }
            if (base == 0x3A)
              {
                int row, col;
                guint8 abs;
                n = node_new (O42_NODE_REF);
                if (biff8) { decode_ref8 (&d, p, shared, &row, &col, &abs); p += 4; }
                else       { decode_ref5 (&d, p, shared, &row, &col, &abs); p += 3; }
                n->as.ref.row = row;
                n->as.ref.col = col;
                n->abs = abs;
              }
            else
              {
                int r0, c0, r1, c1;
                guint8 a0, a1;
                n = node_new (O42_NODE_RANGE);
                if (biff8)
                  {
                    guchar first[4] = { p[0], p[1], p[4], p[5] };
                    guchar last[4] = { p[2], p[3], p[6], p[7] };
                    decode_ref8 (&d, first, shared, &r0, &c0, &a0);
                    decode_ref8 (&d, last, shared, &r1, &c1, &a1);
                    p += 8;
                  }
                else
                  {
                    guchar first[3] = { p[0], p[1], p[4] };
                    guchar last[3] = { p[2], p[3], p[5] };
                    decode_ref5 (&d, first, shared, &r0, &c0, &a0);
                    decode_ref5 (&d, last, shared, &r1, &c1, &a1);
                    p += 6;
                  }
                n->as.range = o42_range_normalise (r0, c0, r1, c1);
                n->abs = a0 | (a1 << 2);
              }
            n->sheet = sheet;
            n->sheet_last = sheet_last;
            push (&d, n);
          }
          break;
        case 0x3C: case 0x3D:
          {
            O42Node *n = node_new (O42_NODE_ERROR);
            n->as.error = O42_ERR_REF;
            p += biff8 ? (base == 0x3C ? 6 : 10) : (base == 0x3C ? 17 : 20);
            push (&d, n);
          }
          break;
        case 0x01:   /* ptgExp: handled by the caller */
          p += 4;
          break;
        default:
          /* Something this reader does not know: give up on the formula. */
          p = end;
          {
            O42Node *n = node_new (O42_NODE_ERROR);
            n->as.error = O42_ERR_NAME;
            push (&d, n);
          }
          break;
        }
    }

  result = pop (&d);
  while (d.stack->len > 0)
    o42_node_free (pop (&d));
  g_ptr_array_unref (d.stack);
  return result;
}

/* ---- records ---- */

static void
apply_xf (Reader *r, int row, int col, guint xf)
{
  if (xf < r->xfs->len && xf != 15)
    {
      O42Range one = { row, col, row, col };
      o42_sheet_apply_fmt (r->sheet, &one, O42_FMT_ALL, &g_array_index (r->xfs, O42Fmt, xf));
    }
}

static void
set_cell (Reader *r, int row, int col, guint xf, const char *input)
{
  if (r->sheet == NULL || row < 0 || row >= O42_MAX_ROWS || col < 0 || col >= O42_MAX_COLS)
    return;
  if (input != NULL && input[0] != '\0')
    o42_sheet_set_input (r->sheet, row, col, input);
  apply_xf (r, row, col, xf);
}

static double
rk_value (guint32 rk)
{
  double v;
  if (rk & 0x02)
    v = (double) ((gint32) rk >> 2);
  else
    {
      guint64 bits = (guint64) (rk & 0xFFFFFFFC) << 32;
      memcpy (&v, &bits, 8);
    }
  if (rk & 0x01)
    v /= 100;
  return v;
}

static void
set_number (Reader *r, int row, int col, guint xf, double v)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (buf, sizeof buf, "%.15g", v);
  if (g_ascii_strtod (buf, NULL) != v)
    g_ascii_formatd (buf, sizeof buf, "%.17g", v);
  set_cell (r, row, col, xf, buf);
}

static guint32 palette_colour (Reader *r, guint idx);

/* A border colour index: 0x40 is the automatic (black) one. */
static guint32
xls_border_palette (Reader *r, guint idx)
{
  if (idx >= 8 && idx < 64)
    return palette_colour (r, idx);
  return 0;
}

static guint32
palette_colour (Reader *r, guint idx)
{
  if (idx < 64) return r->palette[idx];
  return 0;   /* system foreground */
}

static void
read_font (Reader *r, const guchar *p, gsize len)
{
  O42Fmt f;
  const guchar *end = p + len;
  guint height, flags, colour, weight, underline;

  o42_fmt_init_default (&f);
  if (len < 14) { g_array_append_val (r->fonts, f); return; }
  height = rd16 (p);
  flags = rd16 (p + 2);
  colour = rd16 (p + 4);
  weight = rd16 (p + 6);
  underline = p[10];
  f.size = height / 10;   /* twips to half-points */
  f.bold = weight >= 600;
  f.italic = (flags & 0x02) != 0;
  f.strikeout = (flags & 0x08) != 0;
  f.underline = underline != 0;
  if (colour < 64 && colour >= 8)
    f.colour = palette_colour (r, colour);
  p += 14;
  {
    char *name = read_str (r, &p, end, FALSE);
    if (name[0] != '\0')
      f.family = g_intern_string (name);
    g_free (name);
  }
  g_array_append_val (r->fonts, f);
}

static void
read_xf (Reader *r, const guchar *p, gsize len)
{
  O42Fmt f;
  guint font = rd16 (p), format = rd16 (p + 2);
  guint align = len > 6 ? p[6] : 0;
  const char *code;

  /* Font index 4 does not exist; records above it are one off. */
  if (font > 4) font--;
  if (font < r->fonts->len)
    f = g_array_index (r->fonts, O42Fmt, font);
  else
    o42_fmt_init_default (&f);

  switch (align & 0x07)
    {
    case 1: f.halign = O42_HALIGN_LEFT; break;
    case 2: f.halign = O42_HALIGN_CENTRE; break;
    case 3: f.halign = O42_HALIGN_RIGHT; break;
    default: break;
    }
  f.wrap = (align & 0x08) != 0;
  switch ((align >> 4) & 0x07)
    {
    case 0: f.valign = O42_VALIGN_TOP; break;
    case 1: f.valign = O42_VALIGN_MIDDLE; break;
    default: break;
    }

  if (r->biff >= 8 && len >= 20)
    {
      guint32 b1 = rd32 (p + 10), b2 = rd32 (p + 14);
      guint fill = rd16 (p + 18);
      guint pattern = (b2 >> 26) & 0x3F;
      f.border_style[O42_SIDE_LEFT] = xls_border_style (b1 & 0x0F);
      f.border_style[O42_SIDE_RIGHT] = xls_border_style ((b1 >> 4) & 0x0F);
      f.border_style[O42_SIDE_TOP] = xls_border_style ((b1 >> 8) & 0x0F);
      f.border_style[O42_SIDE_BOTTOM] = xls_border_style ((b1 >> 12) & 0x0F);
      f.border_colour[O42_SIDE_LEFT] = xls_border_palette (r, (b1 >> 16) & 0x7F);
      f.border_colour[O42_SIDE_RIGHT] = xls_border_palette (r, (b1 >> 23) & 0x7F);
      f.border_colour[O42_SIDE_TOP] = xls_border_palette (r, b2 & 0x7F);
      f.border_colour[O42_SIDE_BOTTOM] = xls_border_palette (r, (b2 >> 7) & 0x7F);
      o42_fmt_sync_borders (&f);
      {
        guint rot = p[7], ind = p[8] & 0x0F;
        f.rotation = rot <= 90 ? (gint16) rot : rot <= 180 ? (gint16) (90 - (int) rot) : 0;
        f.indent = (guint8) ind;
      }
      if (pattern != 0)
        {
          guint fg = fill & 0x7F;
          if (fg >= 8 && fg < 64)
            f.fill = palette_colour (r, fg);
        }
    }
  else if (len >= 16)
    {
      guint fill = rd16 (p + 8);
      guint pattern = rd16 (p + 10) & 0x3F;
      if (pattern != 0)
        {
          guint fg = fill & 0x7F;
          if (fg >= 8 && fg < 64)
            f.fill = palette_colour (r, fg);
        }
    }

  code = g_hash_table_lookup (r->formats, GINT_TO_POINTER ((int) format));
  if (code == NULL)
    code = o42_xlsx_builtin_number_format (format);
  if (code != NULL)
    o42_xlsx_apply_format_code (&f, code);

  g_array_append_val (r->xfs, f);
}

/* Parses the FONT and XF records kept back until the palette was read. */
static void
flush_styles (Reader *r)
{
  if (r->styles_parsed)
    return;
  r->styles_parsed = TRUE;
  for (guint i = 0; i < r->pending_fonts->len; i++)
    {
      GBytes *b = g_ptr_array_index (r->pending_fonts, i);
      gsize len;
      const guchar *body = g_bytes_get_data (b, &len);
      read_font (r, body, len);
    }
  for (guint i = 0; i < r->pending_xfs->len; i++)
    {
      GBytes *b = g_ptr_array_index (r->pending_xfs, i);
      gsize len;
      const guchar *body = g_bytes_get_data (b, &len);
      read_xf (r, body, len);
    }
}

/* A NAME record: defined names, and the built-in _FilterDatabase. */
static void
read_name (Reader *r, const guchar *p, gsize len)
{
  const guchar *end = p + len;
  guint flags = rd16 (p);
  guint cch = p[3];
  guint cce = rd16 (p + 4);
  guint itab = rd16 (p + 8);
  char *name;
  O42Node *tree;
  char *range_text = NULL;

  p += 14;
  if (r->biff >= 8)
    {
      guint sflags = *p++;
      if (flags & 0x0020)
        {
          guint id = sflags & 0x01 ? rd16 (p) : *p;
          p += sflags & 0x01 ? 2 : 1;
          name = g_strdup_printf ("_builtin_%u", id);
          cch = 0;
        }
      else if (sflags & 0x01)
        {
          GString *s = g_string_new (NULL);
          for (guint i = 0; i < cch && p + 2 <= end; i++, p += 2)
            g_string_append_unichar (s, rd16 (p));
          name = g_string_free (s, FALSE);
        }
      else
        {
          name = g_strndup ((const char *) p, MIN (cch, (guint) (end - p)));
          p += cch;
        }
    }
  else
    {
      if (flags & 0x0020)
        {
          name = g_strdup_printf ("_builtin_%u", *p);
          p += cch;
        }
      else
        {
          name = g_strndup ((const char *) p, MIN (cch, (guint) (end - p)));
          p += cch;
        }
    }

  if (p + cce <= end)
    {
      tree = decode_formula (r, p, cce, 0, 0, FALSE, p + cce, end);
      if (tree->type == O42_NODE_RANGE || tree->type == O42_NODE_REF)
        {
          char *text = o42_node_to_string (tree);
          range_text = text;
        }
      o42_node_free (tree);
    }

  if (strcmp (name, "_builtin_13") == 0)
    {
      if (range_text != NULL && itab >= 1)
        {
          int sheet = itab - 1;
          g_array_append_val (r->filter_sheets, sheet);
          g_ptr_array_add (r->filter_ranges, range_text);
          range_text = NULL;
        }
      g_ptr_array_add (r->names, g_strdup (""));
      g_ptr_array_add (r->name_ranges, NULL);
    }
  else
    {
      g_ptr_array_add (r->names, g_strdup (name));
      g_ptr_array_add (r->name_ranges, range_text);
      range_text = NULL;
    }
  g_free (range_text);
  g_free (name);
}

static void
read_supbook (Reader *r, const guchar *p, gsize len)
{
  gboolean self = len == 4 && p[2] == 0x01 && p[3] == 0x04;
  g_ptr_array_add (r->supbook_self, GINT_TO_POINTER (self ? 1 : 0));
  g_ptr_array_add (r->supbook_names, g_ptr_array_new_with_free_func (g_free));
}

static void
read_externname (Reader *r, const guchar *p, gsize len)
{
  const guchar *end = p + len;
  if (r->supbook_names->len == 0 || len < 7)
    return;
  p += 6;
  {
    char *name = read_str (r, &p, end, FALSE);
    GPtrArray *names = g_ptr_array_index (r->supbook_names, r->supbook_names->len - 1);
    g_ptr_array_add (names, name);
  }
}

static void
read_formula (Reader *r, const guchar *p, gsize len)
{
  const guchar *end = p + len;
  int row = rd16 (p), col = rd16 (p + 2);
  guint xf = rd16 (p + 4);
  const guchar *result = p + 6;
  guint cce = r->biff >= 8 ? rd16 (p + 20) : rd16 (p + 20);
  const guchar *rgce = p + 22;
  O42Node *tree;
  char *text;

  if (rgce + cce > end)
    return;

  /* A ptgExp points at the shared formula (or array) that owns it. */
  if (cce >= 5 && rgce[0] == 0x01)
    {
      if (o42_sheet_array_range (r->sheet, row, col, NULL))
        {
          apply_xf (r, row, col, xf);
          return;   /* a member of an array block already spread */
        }
      guint srow = rd16 (rgce + 1), scol = r->biff >= 8 ? rd16 (rgce + 3) : rgce[3];
      GBytes *master = g_hash_table_lookup (r->shared, GSIZE_TO_POINTER ((gsize) o42_key (srow, scol)));
      if (master == NULL)
        {
          /* Not seen yet (or an array formula): keep the cached value. */
          r->pending = FALSE;
          if (result[6] == 0xFF && result[7] == 0xFF)
            {
              if (result[0] == 0)
                { r->pending = TRUE; r->pending_row = row; r->pending_col = col; apply_xf (r, row, col, xf); }
              else if (result[0] == 1)
                set_cell (r, row, col, xf, result[2] ? "TRUE" : "FALSE");
              else if (result[0] == 2)
                set_cell (r, row, col, xf, o42_error_name (error_from_biff (result[2])));
            }
          else
            set_number (r, row, col, xf, rd_double (result));
          return;
        }
      {
        /* The stored master: its token count, tokens, then array data. */
        const guchar *m = g_bytes_get_data (master, NULL);
        gsize mlen = g_bytes_get_size (master);
        guint mcce = mlen >= 2 ? rd16 (m) : 0;
        if (2 + mcce > mlen) mcce = mlen >= 2 ? mlen - 2 : 0;
        tree = decode_formula (r, m + 2, mcce, row, col, TRUE, m + 2 + mcce, m + mlen);
      }
    }
  else
    tree = decode_formula (r, rgce, cce, row, col, FALSE, rgce + cce, end);

  text = o42_node_to_string (tree);
  {
    char *input = g_strconcat ("=", text, NULL);
    set_cell (r, row, col, xf, input);
    g_free (input);
  }
  g_free (text);
  o42_node_free (tree);
  r->pending = FALSE;
}

static void
read_shrfmla (Reader *r, const guchar *p, gsize len)
{
  guint r0 = rd16 (p), c0 = p[4];
  guint cce = rd16 (p + 8);
  if (10 + cce <= len)
    g_hash_table_insert (r->shared, GSIZE_TO_POINTER ((gsize) o42_key (r0, c0)),
                         g_bytes_new (p + 8, len - 8));
}

/* A number from a CF rule's formula: ptgInt or ptgNum, nothing else. */
static gboolean
cf_number (const guchar *p, gsize len, double *out)
{
  if (len >= 3 && p[0] == 0x1E) { *out = rd16 (p + 1); return TRUE; }
  if (len >= 9 && p[0] == 0x1F) { *out = rd_double (p + 1); return TRUE; }
  return FALSE;
}

static void
read_cf (Reader *r, const guchar *p, gsize len)
{
  const guchar *end = p + len;
  guint type = p[0], op = p[1];
  guint cce1 = rd16 (p + 2), cce2 = rd16 (p + 4);
  guint32 flags = rd32 (p + 6);
  O42Condition c;
  double v1 = 0, v2 = 0;

  if (type != 1 || op < 1 || op > 8)
    return;
  memset (&c, 0, sizeof c);
  o42_fmt_init_default (&c.fmt);
  c.range = r->cf_range;
  {
    static const O42CondOp ops[] = { O42_COND_BETWEEN, O42_COND_NOT_BETWEEN, O42_COND_EQUAL, O42_COND_NOT_EQUAL,
                                     O42_COND_GREATER, O42_COND_LESS, O42_COND_GREATER_EQUAL, O42_COND_LESS_EQUAL };
    c.op = ops[op - 1];
  }
  p += 12;   /* the header, then two reserved bytes */

  if (flags & (1u << 25))   /* number format block */
    p += 2;
  if (flags & (1u << 26))   /* font block, 118 bytes */
    {
      if (p + 118 > end) return;
      {
        guint32 height = rd32 (p + 64), ts = rd32 (p + 68), bls = rd16 (p + 72);
        guint uls = p[76];
        guint32 icv = rd32 (p + 80), ts_ninch = rd32 (p + 88), uls_ninch = rd32 (p + 96), bls_ninch = rd32 (p + 100);
        if (height != 0xFFFFFFFFu && height > 0) { c.fmt.size = height / 10; c.mask |= O42_FMT_SIZE; }
        if (bls_ninch == 0) { c.fmt.bold = bls >= 600; c.mask |= O42_FMT_BOLD; }
        if (!(ts_ninch & 0x02)) { c.fmt.italic = (ts & 0x02) != 0; c.mask |= O42_FMT_ITALIC; }
        if (!(ts_ninch & 0x80)) { c.fmt.strikeout = (ts & 0x80) != 0; c.mask |= O42_FMT_STRIKEOUT; }
        if (uls_ninch == 0) { c.fmt.underline = uls != 0; c.mask |= O42_FMT_UNDERLINE; }
        if (icv != 0xFFFFFFFFu && icv < 64) { c.fmt.colour = palette_colour (r, icv); c.mask |= O42_FMT_COLOUR; }
      }
      p += 118;
    }
  if (flags & (1u << 27))   /* alignment block */
    p += 8;
  if (flags & (1u << 28))   /* border block */
    {
      if (p + 8 > end) return;
      {
        guint32 b = rd32 (p);
        c.fmt.border_left = (b & 0x0F) != 0 && !(flags & (1u << 10));
        c.fmt.border_right = ((b >> 4) & 0x0F) != 0 && !(flags & (1u << 11));
        c.fmt.border_top = ((b >> 8) & 0x0F) != 0 && !(flags & (1u << 12));
        c.fmt.border_bottom = ((b >> 12) & 0x0F) != 0 && !(flags & (1u << 13));
        c.mask |= O42_FMT_BORDERS;
      }
      p += 8;
    }
  if (flags & (1u << 29))   /* pattern block */
    {
      if (p + 4 > end) return;
      {
        guint fls = rd16 (p) >> 10;
        guint icv = rd16 (p + 2) & 0x7F;
        if (fls != 0 && !(flags & (1u << 17)) && icv < 64)
          { c.fmt.fill = palette_colour (r, icv); c.mask |= O42_FMT_FILL; }
      }
      p += 4;
    }
  if (flags & (1u << 30))   /* protection block */
    p += 2;

  if (p + cce1 + cce2 > end)
    return;
  if (!cf_number (p, cce1, &v1))
    return;
  if (cce2 > 0 && !cf_number (p + cce1, cce2, &v2))
    return;
  c.value = v1;
  c.value2 = cce2 > 0 ? v2 : v1;
  o42_sheet_add_condition (r->sheet, &c);
}

/* A DV record: one validation rule and the ranges it covers. */
static void
read_dv (Reader *r, const guchar *p, gsize len)
{
  const guchar *end = p + len;
  guint32 flags = rd32 (p);
  O42Validation v;
  char *texts[2] = { NULL, NULL };

  memset (&v, 0, sizeof v);
  v.kind = (O42ValidKind) MIN (flags & 0x0F, 6);
  v.op = (O42CondOp) MIN ((flags >> 20) & 0x0F, 7);
  v.allow_blank = (flags & 0x100) != 0;
  p += 4;
  {
    char *s;
    s = read_str (r, &p, end, TRUE); g_free (s);          /* prompt title */
    s = read_str (r, &p, end, TRUE); g_free (s);          /* prompt text */
    s = read_str (r, &p, end, TRUE); g_free (s);          /* error title */
    v.message = read_str (r, &p, end, TRUE);
    if (v.message[0] == '\0') { g_free (v.message); v.message = g_strdup (""); }
  }
  for (int k = 0; k < 2; k++)
    {
      guint cce;
      if (p + 4 > end) { texts[k] = g_strdup (""); continue; }
      cce = rd16 (p);
      p += 4;
      if (cce > 0 && p + cce <= end)
        {
          O42Node *tree = decode_formula (r, p, cce, 0, 0, FALSE, NULL, NULL);
          if (tree->type == O42_NODE_STRING)
            texts[k] = g_strdup (tree->as.string);
          else
            texts[k] = o42_node_to_string (tree);
          o42_node_free (tree);
          p += cce;
        }
      else
        texts[k] = g_strdup ("");
    }
  v.value = texts[0];
  v.value2 = texts[1];
  if (p + 2 <= end)
    {
      guint n = rd16 (p);
      p += 2;
      for (guint i = 0; i < n && p + 8 <= end; i++, p += 8)
        {
          v.range = o42_range_normalise (rd16 (p), rd16 (p + 4), rd16 (p + 2), rd16 (p + 6));
          if (v.kind != O42_VALID_ANY && v.range.row1 < O42_MAX_ROWS && v.range.col1 < O42_MAX_COLS)
            o42_sheet_add_validation (r->sheet, &v);
        }
    }
  g_free (v.value);
  g_free (v.value2);
  g_free (v.message);
}

/* A chart substream's records: the series' ranges, the kind, the title. */
static void
read_chart_record (Reader *r, guint id, const guchar *p, gsize len)
{
  ChartDef *def;

  if (r->chart_defs->len == 0)
    return;
  def = &g_array_index (r->chart_defs, ChartDef, r->chart_defs->len - 1);
  switch (id)
    {
    case C_BEGIN: r->chart_depth++; break;
    case C_END:
      r->chart_depth--;
      if (r->chart_depth <= 1) r->in_series = FALSE;
      break;
    case C_SERIES: r->in_series = TRUE; break;
    case C_AI:
      if (len >= 8 && r->in_series)
        {
          guint ai = p[0], rt = p[1];
          guint cce = rd16 (p + 6);
          if (rt == 2 && 8 + cce <= len && (ai == 0 || ai == 1 || ai == 2))
            {
              O42Node *tree = decode_formula (r, p + 8, cce, 0, 0, FALSE, NULL, NULL);
              O42Range range;
              gboolean usable = FALSE;
              if (tree->type == O42_NODE_RANGE) { range = tree->as.range; usable = TRUE; }
              else if (tree->type == O42_NODE_REF)
                {
                  range.row0 = range.row1 = tree->as.ref.row;
                  range.col0 = range.col1 = tree->as.ref.col;
                  usable = TRUE;
                }
              if (usable)
                {
                  if (!def->have_box) { def->box = range; def->have_box = TRUE; }
                  else
                    {
                      def->box.row0 = MIN (def->box.row0, range.row0);
                      def->box.col0 = MIN (def->box.col0, range.col0);
                      def->box.row1 = MAX (def->box.row1, range.row1);
                      def->box.col1 = MAX (def->box.col1, range.col1);
                    }
                  if (ai == 0) def->have_title_ref = TRUE;
                  if (ai == 2) def->have_cats = TRUE;
                }
              o42_node_free (tree);
            }
        }
      break;
    case C_BAR:
      if (len >= 6)
        {
          guint flags = rd16 (p + 4);
          def->kind = (flags & 0x04) ? O42_CHART_PERCENT : (flags & 0x02) ? O42_CHART_STACKED
                    : (flags & 0x01) ? O42_CHART_BAR : O42_CHART_COLUMN;
          def->kind_known = TRUE;
        }
      break;
    case C_LINE: def->kind = O42_CHART_LINE; def->kind_known = TRUE; break;
    case C_PIE: def->kind = O42_CHART_PIE; def->kind_known = TRUE; break;
    case C_AREA: def->kind = O42_CHART_AREA; def->kind_known = TRUE; break;
    case C_SCATTER: def->kind = O42_CHART_SCATTER; def->kind_known = TRUE; break;
    case C_SERIESTEXT:
      if (!r->in_series && len >= 3 && def->title == NULL)
        {
          /* id u16, then a byte-counted unicode string. */
          const guchar *q = p + 2;
          def->title = read_str (r, &q, p + len, FALSE);
        }
      break;
    default:
      break;
    }
}

static double
sheet_col_x (O42Sheet *sheet, int col)
{
  double x = 0;
  for (int c = 0; c < col && c < O42_MAX_COLS; c++) x += o42_sheet_col_width (sheet, c);
  return x;
}

static double
sheet_row_y (O42Sheet *sheet, int row)
{
  double y = 0;
  for (int rr = 0; rr < row && rr < O42_MAX_ROWS; rr++) y += o42_sheet_row_height (sheet, rr);
  return y;
}

/* ---- Reading a form control's OBJ record ------------------------------- */

/* Whether the OBJ record last read was a form control, and so whether
 * the TXO that follows is its caption. */
static gboolean
last_obj_is_control (Reader *r)
{
  return r->objs->len > 0 &&
    g_array_index (r->objs, ObjInfo, r->objs->len - 1).kind >= 0;
}

/* The kind an ot names, or -1 for an OBJ that is not a form control. */
static int
control_kind_of (guint16 ot)
{
  switch (ot)
    {
    case 0x07: return O42_SHAPE_BUTTON;
    case 0x0B: return O42_SHAPE_CHECKBOX;
    case 0x0C: return O42_SHAPE_OPTION;
    case 0x0E: return O42_SHAPE_LABEL;
    case 0x10: return O42_SHAPE_SPINNER;
    case 0x11: return O42_SHAPE_SCROLLBAR;
    case 0x12: return O42_SHAPE_LISTBOX;
    case 0x13: return O42_SHAPE_GROUPBOX;
    case 0x14: return O42_SHAPE_COMBO;
    default:   return -1;
    }
}

/* The reference inside an ObjFmla, as text: one ptg is all these
 * records ever hold, and a three-dimensional one loses its sheet,
 * which is the sheet the control is on anyway. */
static char *
obj_fmla_ref (const guchar *p, gsize len)
{
  guint cce;
  const guchar *ptg;

  if (len < 7)
    return NULL;
  cce = rd16 (p);
  if (cce + 6 > len)
    return NULL;
  ptg = p + 6;
  switch (ptg[0] & 0x1F)
    {
    case 0x04:   /* ptgRef */
      if (cce >= 5)
        return o42_ref_name (rd16 (ptg + 1), rd16 (ptg + 3) & 0x3FFF);
      break;
    case 0x05:   /* ptgArea */
      if (cce >= 9)
        {
          char *a = o42_ref_name (rd16 (ptg + 1), rd16 (ptg + 5) & 0x3FFF);
          char *b = o42_ref_name (rd16 (ptg + 3), rd16 (ptg + 7) & 0x3FFF);
          char *both = g_strconcat (a, ":", b, NULL);

          g_free (a); g_free (b);
          return both;
        }
      break;
    case 0x1A:   /* ptgRef3d */
      if (cce >= 7)
        return o42_ref_name (rd16 (ptg + 3), rd16 (ptg + 5) & 0x3FFF);
      break;
    case 0x1B:   /* ptgArea3d */
      if (cce >= 11)
        {
          char *a = o42_ref_name (rd16 (ptg + 3), rd16 (ptg + 7) & 0x3FFF);
          char *b = o42_ref_name (rd16 (ptg + 5), rd16 (ptg + 9) & 0x3FFF);
          char *both = g_strconcat (a, ":", b, NULL);

          g_free (a); g_free (b);
          return both;
        }
      break;
    default:
      break;
    }
  return NULL;
}

/* Everything an OBJ record says about a form control, gathered from
 * its subrecords: the cell it drives, a list's range, a spinner's
 * bounds, whether a box is ticked. */
static void
read_control_records (ObjInfo *info, const guchar *p, gsize len)
{
  gsize i = 0;

  while (i + 4 <= len)
    {
      guint ft = rd16 (p + i), cb = rd16 (p + i + 2);
      const guchar *body = p + i + 4;

      if (ft == 0)
        break;
      if (i + 4 + cb > len)
        break;
      switch (ft)
        {
        case 0x0A:    /* the check mark, twice over in some writers */
          if (cb >= 2 && rd16 (body) != 0)
            info->checked = TRUE;
          break;
        case 0x14:    /* a check box's or option button's linked cell */
        case 0x0E:    /* a spinner's, a scroll bar's or a list's */
          if (info->link == NULL)
            info->link = obj_fmla_ref (body, cb);
          break;
        case 0x0C:    /* the bounds a spinner or scroll bar moves in */
          if (cb >= 20 && (info->kind == O42_SHAPE_SPINNER || info->kind == O42_SHAPE_SCROLLBAR))
            {
              info->value = rd16 (body + 4);
              info->min = rd16 (body + 6);
              info->max = rd16 (body + 8);
              info->step = rd16 (body + 10);
              info->page = rd16 (body + 12);
              info->have_bounds = TRUE;
            }
          break;
        case 0x13:    /* a list box's or drop-down's own record */
          if (cb >= 2)
            {
              guint fmla = rd16 (body);

              if (fmla > 0 && fmla + 2 <= cb && info->source == NULL)
                info->source = obj_fmla_ref (body + 2, fmla);
              if (fmla + 2 + 4 <= cb)
                info->selected = rd16 (body + 2 + fmla + 2);
            }
          break;
        default:
          break;
        }
      i += 4 + cb;
    }
}

/* The sheet's shapes, once its records are all in: pictures become
 * pictures at their anchors; notes were placed by their NOTE records. */
static void
read_drawing (Reader *r)
{
  GArray *found;

  if (r->drawing->len == 0)
    return;
  if (r->images->len == 0 && r->group->len > 0)
    o42_escher_parse_group (r->group->data, r->group->len, r->images, r->image_formats);
  found = g_array_new (FALSE, FALSE, sizeof (O42EscherFound));
  o42_escher_parse_drawing (r->drawing->data, r->drawing->len, found);
  for (guint i = 0; i < found->len; i++)
    {
      const O42EscherFound *f = &g_array_index (found, O42EscherFound, i);
      const ObjInfo *info = i < r->objs->len ? &g_array_index (r->objs, ObjInfo, i) : NULL;
      GBytes *data;
      int pw, ph;
      const char *fmt;
      double x0, y0, x1, y1;
      O42Picture *pic;

      /* A form control: the shape says where it is, its OBJ what it
       * is and what it drives. */
      if (info != NULL && info->kind >= 0 &&
          f->col1 < O42_MAX_COLS && f->row1 < O42_MAX_ROWS)
        {
          O42Shape *shape = o42_sheet_add_shape (r->sheet, (O42ShapeKind) info->kind,
                                                 f->row1, f->col1);

          if (shape != NULL)
            {
              double sx0 = sheet_col_x (r->sheet, f->col1) + f->dx1 * o42_sheet_col_width (r->sheet, f->col1);
              double sy0 = sheet_row_y (r->sheet, f->row1) + f->dy1 * o42_sheet_row_height (r->sheet, f->row1);
              int c2 = MIN (f->col2, O42_MAX_COLS - 1), r2 = MIN (f->row2, O42_MAX_ROWS - 1);
              double sx1 = sheet_col_x (r->sheet, c2) + f->dx2 * o42_sheet_col_width (r->sheet, c2);
              double sy1 = sheet_row_y (r->sheet, r2) + f->dy2 * o42_sheet_row_height (r->sheet, r2);

              shape->dx = f->dx1 * o42_sheet_col_width (r->sheet, f->col1);
              shape->dy = f->dy1 * o42_sheet_row_height (r->sheet, f->row1);
              shape->width = MAX (sx1 - sx0, 12);
              shape->height = MAX (sy1 - sy0, 12);
              if (info->text != NULL)
                { g_free (shape->text); shape->text = g_strdup (info->text); }
              if (info->link != NULL)
                { g_free (shape->link); shape->link = g_strdup (info->link); }
              if (info->source != NULL)
                { g_free (shape->source); shape->source = g_strdup (info->source); }
              if (info->have_bounds)
                {
                  shape->min = info->min;
                  shape->max = info->max;
                  shape->step = info->step;
                  shape->page = info->page;
                }
            }
          continue;
        }

      if (f->is_chart)
        {
          /* The nth chart shape takes the nth chart substream. */
          guint n = 0;
          for (guint k = 0; k < i; k++)
            if (g_array_index (found, O42EscherFound, k).is_chart) n++;
          if (n < r->chart_defs->len && f->col1 < O42_MAX_COLS && f->row1 < O42_MAX_ROWS)
            {
              ChartDef *def = &g_array_index (r->chart_defs, ChartDef, n);
              if (def->have_box)
                {
                  O42Chart *chart = o42_sheet_add_chart (r->sheet, def->kind_known ? def->kind : O42_CHART_COLUMN,
                                                         &def->box, f->row1, f->col1);
                  if (chart != NULL)
                    {
                      double cx0 = sheet_col_x (r->sheet, f->col1) + f->dx1 * o42_sheet_col_width (r->sheet, f->col1);
                      double cy0 = sheet_row_y (r->sheet, f->row1) + f->dy1 * o42_sheet_row_height (r->sheet, f->row1);
                      double cx1 = sheet_col_x (r->sheet, MIN (f->col2, O42_MAX_COLS - 1)) + f->dx2 * o42_sheet_col_width (r->sheet, MIN (f->col2, O42_MAX_COLS - 1));
                      double cy1 = sheet_row_y (r->sheet, MIN (f->row2, O42_MAX_ROWS - 1)) + f->dy2 * o42_sheet_row_height (r->sheet, MIN (f->row2, O42_MAX_ROWS - 1));
                      chart->first_row_labels = def->have_title_ref;
                      chart->first_col_labels = def->have_cats || def->kind == O42_CHART_SCATTER;
                      g_free (chart->title);
                      chart->title = g_strdup (def->title ? def->title : "");
                      chart->dx = f->dx1 * o42_sheet_col_width (r->sheet, f->col1);
                      chart->dy = f->dy1 * o42_sheet_row_height (r->sheet, f->row1);
                      chart->width = MAX (cx1 - cx0, 40);
                      chart->height = MAX (cy1 - cy0, 30);
                    }
                }
            }
          continue;
        }
      if (!f->is_picture || f->blip < 1 || (guint) f->blip > r->images->len)
        continue;
      data = g_ptr_array_index (r->images, f->blip - 1);
      if (g_bytes_get_size (data) == 0 || !o42_image_probe (data, &pw, &ph, &fmt))
        continue;
      if (f->col1 >= O42_MAX_COLS || f->row1 >= O42_MAX_ROWS)
        continue;
      x0 = sheet_col_x (r->sheet, f->col1) + f->dx1 * o42_sheet_col_width (r->sheet, f->col1);
      y0 = sheet_row_y (r->sheet, f->row1) + f->dy1 * o42_sheet_row_height (r->sheet, f->row1);
      x1 = sheet_col_x (r->sheet, MIN (f->col2, O42_MAX_COLS - 1)) + f->dx2 * o42_sheet_col_width (r->sheet, MIN (f->col2, O42_MAX_COLS - 1));
      y1 = sheet_row_y (r->sheet, MIN (f->row2, O42_MAX_ROWS - 1)) + f->dy2 * o42_sheet_row_height (r->sheet, MIN (f->row2, O42_MAX_ROWS - 1));
      pic = o42_sheet_add_picture (r->sheet, data, fmt, pw, ph, f->row1, f->col1);
      if (pic != NULL)
        {
          pic->dx = f->dx1 * o42_sheet_col_width (r->sheet, f->col1);
          pic->dy = f->dy1 * o42_sheet_row_height (r->sheet, f->row1);
          pic->width = MAX (x1 - x0, 8);
          pic->height = MAX (y1 - y0, 8);
        }
    }
  g_array_unref (found);
}

static void
read_sheet_record (Reader *r, guint id, const guchar *p, gsize len)
{
  switch (id)
    {
    case R_NUMBER:
      if (len >= 14) set_number (r, rd16 (p), rd16 (p + 2), rd16 (p + 4), rd_double (p + 6));
      break;
    case R_RK:
      if (len >= 10) set_number (r, rd16 (p), rd16 (p + 2), rd16 (p + 4), rk_value (rd32 (p + 6)));
      break;
    case R_MULRK:
      if (len >= 6)
        {
          int row = rd16 (p), col = rd16 (p + 2);
          for (gsize at = 4; at + 6 <= len - 2; at += 6, col++)
            set_number (r, row, col, rd16 (p + at), rk_value (rd32 (p + at + 2)));
        }
      break;
    case R_LABELSST:
      if (len >= 10)
        {
          guint idx = rd32 (p + 6);
          if (idx < r->sst->len)
            set_cell (r, rd16 (p), rd16 (p + 2), rd16 (p + 4), g_ptr_array_index (r->sst, idx));
        }
      break;
    case R_LABEL:
    case R_RSTRING:
      if (len >= 8)
        {
          const guchar *q = p + 6;
          char *text = read_str (r, &q, p + len, TRUE);
          set_cell (r, rd16 (p), rd16 (p + 2), rd16 (p + 4), text);
          g_free (text);
        }
      break;
    case R_BOOLERR:
      if (len >= 8)
        {
          const char *text = p[7] ? o42_error_name (error_from_biff (p[6])) : (p[6] ? "TRUE" : "FALSE");
          set_cell (r, rd16 (p), rd16 (p + 2), rd16 (p + 4), text);
        }
      break;
    case R_BLANK:
      if (len >= 6) set_cell (r, rd16 (p), rd16 (p + 2), rd16 (p + 4), NULL);
      break;
    case R_MULBLANK:
      if (len >= 6)
        {
          int row = rd16 (p), col = rd16 (p + 2);
          for (gsize at = 4; at + 2 <= len - 2; at += 2, col++)
            set_cell (r, row, col, rd16 (p + at), NULL);
        }
      break;
    case R_FORMULA:
      if (len >= 22) read_formula (r, p, len);
      break;
    case R_STRING:
      if (r->pending && len >= 3)
        {
          const guchar *q = p;
          char *text = read_str (r, &q, p + len, TRUE);
          if (r->sheet && text[0] != '\0')
            o42_sheet_set_input (r->sheet, r->pending_row, r->pending_col, text);
          g_free (text);
          r->pending = FALSE;
        }
      break;
    case R_SHRFMLA:
      if (len >= 10) read_shrfmla (r, p, len);
      break;
    case R_ARRAY:
      if (len >= 14 && r->sheet)
        {
          O42Range block = o42_range_normalise (rd16 (p), p[4], rd16 (p + 2), p[5]);
          guint cce = rd16 (p + 12);
          if (14 + cce <= len && block.row1 < O42_MAX_ROWS && block.col1 < O42_MAX_COLS)
            {
              O42Node *tree = decode_formula (r, p + 14, cce, block.row0, block.col0, FALSE,
                                              p + 14 + cce, p + len);
              char *text = o42_node_to_string (tree);
              char *input = g_strconcat ("=", text, NULL);
              o42_sheet_set_array_formula (r->sheet, &block, input);
              g_free (input);
              g_free (text);
              o42_node_free (tree);
            }
        }
      break;
    case R_MSODRAWING:
      g_byte_array_append (r->drawing, p, len);
      break;
    case R_OBJ:
      if (len >= 10 && rd16 (p) == 0x15)
        {
          ObjInfo info;
          guint16 ot = rd16 (p + 4);

          memset (&info, 0, sizeof info);
          info.ot = ot;
          info.kind = control_kind_of (ot);
          r->obj_id = rd16 (p + 6);
          r->obj_is_note = ot == 0x19;
          if (info.kind >= 0 && len > 22)
            read_control_records (&info, p + 22, len - 22);
          g_array_append_val (r->objs, info);
        }
      break;
    case R_TXO:
      /* The text of the object the last OBJ named: a note's, or the
       * caption on a form control. */
      if (len >= 18 && (r->obj_is_note || last_obj_is_control (r)))
        {
          r->txo_chars = rd16 (p + 10);
          g_string_truncate (r->txo_text, 0);
          if (r->txo_chars == 0)
            r->obj_is_note = FALSE;
        }
      break;
    case R_NOTE:
      if (len >= 8 && r->sheet)
        {
          int row = rd16 (p), col = rd16 (p + 2);
          guint obj = rd16 (p + 6);
          const char *text = g_hash_table_lookup (r->note_texts, GUINT_TO_POINTER (obj));
          if (text != NULL && text[0] != '\0' && row < O42_MAX_ROWS && col < O42_MAX_COLS)
            o42_sheet_set_note (r->sheet, row, col, text);
        }
      break;
    case R_DV:
      if (len >= 20 && r->sheet) read_dv (r, p, len);
      break;
    case R_CONDFMT:
      if (len >= 12)
        {
          r->cf_range = o42_range_normalise (rd16 (p + 4), rd16 (p + 8), rd16 (p + 6), rd16 (p + 10));
          r->cf_have_range = r->cf_range.row1 < O42_MAX_ROWS && r->cf_range.col1 < O42_MAX_COLS;
        }
      break;
    case R_CF:
      if (len >= 10 && r->cf_have_range && r->sheet)
        read_cf (r, p, len);
      break;
    case R_ROW:
      if (len >= 16 && r->sheet)
        {
          int row = rd16 (p);
          guint height = rd16 (p + 6);
          guint flags = rd16 (p + 12);
          if (row >= 0 && row < O42_MAX_ROWS)
            {
              if ((flags & 0x40) && !(height & 0x8000) && (height & 0x7FFF) > 0)
                o42_sheet_set_row_height (r->sheet, row, (int) ((height & 0x7FFF) / 15.0 + 0.5));
              if (flags & 0x20)
                o42_sheet_set_row_hidden (r->sheet, row, TRUE);
              if (flags & 0x07)
                o42_sheet_set_row_level (r->sheet, row, flags & 0x07);
            }
        }
      break;
    case R_COLINFO:
      if (len >= 10 && r->sheet)
        {
          int first = rd16 (p), last = MIN (rd16 (p + 2), O42_MAX_COLS - 1);
          guint width = rd16 (p + 4);
          guint flags = rd16 (p + 8);
          for (int c = first; c <= last; c++)
            {
              o42_sheet_set_col_width (r->sheet, c, (int) (width / 256.0 * 7.0 + 5.0 + 0.5));
              if (flags & 0x0001)
                o42_sheet_set_col_hidden (r->sheet, c, TRUE);
              if ((flags >> 8) & 0x07)
                o42_sheet_set_col_level (r->sheet, c, (flags >> 8) & 0x07);
            }
        }
      break;
    case R_DEFCOLWIDTH:
      if (len >= 2 && r->sheet)
        {
          /* In characters of the default font; every column takes it,
           * and COLINFO records after it override. */
          r->default_width = (int) (rd16 (p) * 7.0 + 5.0 + 0.5);
          for (int c = 0; c < O42_MAX_COLS; c++)
            o42_sheet_set_col_width (r->sheet, c, r->default_width);
        }
      break;
    case R_MERGECELLS:
      if (len >= 2 && r->sheet)
        {
          guint n = rd16 (p);
          for (guint i = 0; i < n && 2 + (i + 1) * 8 <= len; i++)
            {
              const guchar *q = p + 2 + i * 8;
              O42Range m = o42_range_normalise (rd16 (q), rd16 (q + 4), rd16 (q + 2), rd16 (q + 6));
              if (m.row1 < O42_MAX_ROWS && m.col1 < O42_MAX_COLS)
                o42_sheet_merge (r->sheet, &m);
            }
        }
      break;
    case R_WINDOW2:
      if (len >= 2 && r->sheet)
        r->pending = FALSE;
      break;
    case R_PANE:
      if (len >= 9 && r->sheet)
        {
          /* Only frozen panes are kept; WINDOW2's frozen flag is
           * assumed when the split is at whole rows and columns. */
          guint x = rd16 (p), y = rd16 (p + 2);
          if (x < O42_MAX_COLS && y < O42_MAX_ROWS)
            o42_sheet_set_frozen (r->sheet, y, x);
        }
      break;
    default:
      break;
    }
}

static void read_chart_record (Reader *r, guint id, const guchar *p, gsize len);

static gboolean
read_workbook (Reader *r, GError **error)
{
  const guchar *p = r->data, *end = r->data + r->len;
  gboolean in_globals = TRUE;
  gboolean seen_bof = FALSE;
  int next_sheet = 0;
  GPtrArray *sst_segs = NULL;

  for (guint i = 0; i < 64; i++)
    r->palette[i] = i >= 8 ? PALETTE[i - 8] : (i == 1 ? 0xFFFFFF : 0);

  while (p + 4 <= end)
    {
      guint id = rd16 (p), len = rd16 (p + 2);
      const guchar *body = p + 4;

      if (body + len > end)
        break;
      p = body + len;

      if (sst_segs != NULL && id != R_CONTINUE)
        {
          read_sst (r, sst_segs);
          g_ptr_array_unref (sst_segs);
          sst_segs = NULL;
        }
      if (id != R_CONTINUE)
        r->group_open = FALSE;

      /* A chart embedded in a sheet is its own BOF..EOF substream; its
       * records are the chart's, not the sheet's. */
      if (r->embedded > 0 && id != R_BOF && id != R_BOF5 && id != R_EOF)
        {
          read_chart_record (r, id, body, len);
          continue;
        }

      switch (id)
        {
        case R_BOF: case R_BOF5:
          {
            guint type = len >= 4 ? rd16 (body + 2) : 0;
            if (seen_bof && type != 0x0005 && type != 0x0010 && r->sheet != NULL)
              {
                if (r->embedded == 0 && type == 0x0020)
                  {
                    ChartDef def;
                    memset (&def, 0, sizeof def);
                    g_array_append_val (r->chart_defs, def);
                    r->in_series = FALSE;
                    r->chart_depth = 0;
                  }
                r->embedded++;
                break;
              }
            if (!seen_bof)
              {
                guint ver = len >= 2 ? rd16 (body) : 0;
                r->biff = (id == R_BOF && ver >= 0x0600) ? 8 : 5;
                seen_bof = TRUE;
              }
            if (type == 0x0005)
              in_globals = TRUE;
            else if (type == 0x0010)
              {
                /* A worksheet: the next bound sheet, in order. */
                in_globals = FALSE;
                flush_styles (r);
                if (next_sheet < (int) r->sheet_names->len)
                  {
                    const char *name = g_ptr_array_index (r->sheet_names, next_sheet);
                    if (next_sheet == 0)
                      {
                        r->sheet = o42_book_sheet (r->book, 0);
                        o42_book_rename_sheet (r->book, 0, name);
                      }
                    else
                      r->sheet = o42_book_add_sheet (r->book, name, -1);
                    r->sheet_index = next_sheet;
                  }
                else
                  r->sheet = NULL;
                next_sheet++;
                g_hash_table_remove_all (r->shared);
                g_hash_table_remove_all (r->note_texts);
                g_byte_array_set_size (r->drawing, 0);
                for (guint k = 0; k < r->objs->len; k++)
                  obj_info_clear (&g_array_index (r->objs, ObjInfo, k));
                g_array_set_size (r->objs, 0);
                for (guint k = 0; k < r->chart_defs->len; k++)
                  g_free (g_array_index (r->chart_defs, ChartDef, k).title);
                g_array_set_size (r->chart_defs, 0);
                r->pending = FALSE;
                r->obj_is_note = FALSE;
                r->cf_have_range = FALSE;
              }
            else
              {
                in_globals = FALSE;
                flush_styles (r);
                r->sheet = NULL;   /* a chart or macro sheet */
              }
          }
          break;
        case R_EOF:
          if (r->embedded > 0)
            {
              r->embedded--;
              break;
            }
          if (r->sheet != NULL)
            {
              read_drawing (r);
              o42_sheet_autofilter_refresh (r->sheet);
            }
          r->sheet = NULL;
          break;
        case R_BOUNDSHEET:
          if (in_globals && len >= 8)
            {
              guint32 offset = rd32 (body);
              const guchar *q = body + 6;
              char *name = read_str (r, &q, body + len, FALSE);
              guint type = body[5];
              g_array_append_val (r->sheet_offsets, offset);
              (void) type;
              g_ptr_array_add (r->sheet_names, name);
            }
          break;
        case R_FONT:
          if (in_globals) g_ptr_array_add (r->pending_fonts, g_bytes_new (body, len));
          break;
        case R_FORMAT:
          if (in_globals && len >= 3)
            {
              const guchar *q = body + 2;
              char *code = read_str (r, &q, body + len, r->biff >= 8);
              g_hash_table_insert (r->formats, GINT_TO_POINTER ((int) rd16 (body)), code);
            }
          break;
        case R_FORMAT5:
          if (in_globals && len >= 1)
            {
              const guchar *q = body;
              char *code = read_str (r, &q, body + len, FALSE);
              g_hash_table_insert (r->formats, GINT_TO_POINTER (r->n_format5++), code);
            }
          break;
        case R_XF:
          if (in_globals && len >= 16) g_ptr_array_add (r->pending_xfs, g_bytes_new (body, len));
          break;
        case R_PALETTE:
          if (in_globals && len >= 2)
            {
              guint n = rd16 (body);
              for (guint i = 0; i < n && i < 56 && 2 + (i + 1) * 4 <= len; i++)
                {
                  const guchar *q = body + 2 + i * 4;
                  r->palette[8 + i] = (q[0] << 16) | (q[1] << 8) | q[2];
                }
            }
          break;
        case R_SST:
          sst_segs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
          g_ptr_array_add (sst_segs, g_bytes_new (body, len));
          break;
        case R_CONTINUE:
          if (sst_segs != NULL)
            g_ptr_array_add (sst_segs, g_bytes_new (body, len));
          else if (r->group != NULL && r->group_open)
            g_byte_array_append (r->group, body, len);
          else if ((r->obj_is_note || last_obj_is_control (r)) && r->txo_chars > 0 && len >= 1)
            {
              /* The text of a note or of a control's caption: a flags
               * byte, then the characters. */
              const guchar *q = body + 1, *qend = body + len;
              gboolean wide = (body[0] & 0x01) != 0;
              while (r->txo_chars > 0 && q < qend)
                {
                  if (wide) { if (q + 2 > qend) break; g_string_append_unichar (r->txo_text, rd16 (q)); q += 2; }
                  else g_string_append_unichar (r->txo_text, *q++);
                  r->txo_chars--;
                }
              if (r->txo_chars == 0)
                {
                  if (r->obj_is_note)
                    g_hash_table_insert (r->note_texts, GUINT_TO_POINTER (r->obj_id), g_strdup (r->txo_text->str));
                  else
                    {
                      ObjInfo *info = &g_array_index (r->objs, ObjInfo, r->objs->len - 1);

                      g_free (info->text);
                      info->text = g_strdup (r->txo_text->str);
                    }
                  r->obj_is_note = FALSE;
                }
            }
          break;
        case R_MSODRAWINGGROUP:
          if (in_globals)
            {
              g_byte_array_append (r->group, body, len);
              r->group_open = TRUE;
              continue;   /* keep group_open for a CONTINUE */
            }
          break;
        case R_SUPBOOK:
          if (in_globals) read_supbook (r, body, len);
          break;
        case R_EXTERNNAME:
          if (in_globals) read_externname (r, body, len);
          break;
        case R_EXTERNSHEET:
          if (in_globals && r->biff >= 8 && len >= 2)
            {
              guint n = rd16 (body);
              for (guint i = 0; i < n && 2 + (i + 1) * 6 <= len; i++)
                {
                  const guchar *q = body + 2 + i * 6;
                  guint16 a = rd16 (q), b = rd16 (q + 2), c = rd16 (q + 4);
                  g_array_append_val (r->xti, a);
                  g_array_append_val (r->xti, b);
                  g_array_append_val (r->xti, c);
                }
            }
          break;
        case R_NAME:
          if (in_globals && len >= 15) read_name (r, body, len);
          break;
        default:
          if (!in_globals && r->sheet != NULL)
            read_sheet_record (r, id, body, len);
          break;
        }
    }
  if (sst_segs != NULL)
    {
      read_sst (r, sst_segs);
      g_ptr_array_unref (sst_segs);
    }
  if (!seen_bof)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "No BIFF stream in the file");
      return FALSE;
    }
  return TRUE;
}

gboolean
o42_xls_load (O42Book *book, GFile *file, GError **error)
{
  GBytes *whole, *stream;
  Reader r;
  gboolean ok;

  whole = g_file_load_bytes (file, NULL, NULL, error);
  if (whole == NULL)
    return FALSE;
  if (!o42_ole2_is_compound (whole))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "This is not an Excel file.");
      g_bytes_unref (whole);
      return FALSE;
    }
  stream = o42_ole2_read_stream (whole, "Workbook", NULL);
  if (stream == NULL)
    stream = o42_ole2_read_stream (whole, "Book", error);
  g_bytes_unref (whole);
  if (stream == NULL)
    return FALSE;

  memset (&r, 0, sizeof r);
  r.book = book;
  r.biff = 8;
  r.data = g_bytes_get_data (stream, &r.len);
  r.sst = g_ptr_array_new_with_free_func (g_free);
  r.formats = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  r.fonts = g_array_new (FALSE, FALSE, sizeof (O42Fmt));
  r.pending_fonts = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  r.pending_xfs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  r.xfs = g_array_new (FALSE, FALSE, sizeof (O42Fmt));
  r.sheet_names = g_ptr_array_new_with_free_func (g_free);
  r.sheet_offsets = g_array_new (FALSE, FALSE, sizeof (guint32));
  r.xti = g_array_new (FALSE, FALSE, sizeof (guint16));
  r.supbook_names = g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);
  r.supbook_self = g_ptr_array_new ();
  r.names = g_ptr_array_new_with_free_func (g_free);
  r.name_ranges = g_ptr_array_new_with_free_func (g_free);
  r.filter_sheets = g_array_new (FALSE, FALSE, sizeof (int));
  r.filter_ranges = g_ptr_array_new_with_free_func (g_free);
  r.shared = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_bytes_unref);
  r.txo_text = g_string_new (NULL);
  r.note_texts = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  r.images = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  r.image_formats = g_ptr_array_new ();
  r.group = g_byte_array_new ();
  r.drawing = g_byte_array_new ();
  r.chart_defs = g_array_new (FALSE, FALSE, sizeof (ChartDef));
  r.objs = g_array_new (FALSE, FALSE, sizeof (ObjInfo));

  o42_book_clear (book);
  ok = read_workbook (&r, error);

  if (ok)
    {
      /* Names, now that every sheet exists. */
      for (guint i = 0; i < r.names->len; i++)
        {
          const char *nm = g_ptr_array_index (r.names, i);
          const char *val = g_ptr_array_index (r.name_ranges, i);
          if (nm[0] == '\0' || val == NULL)
            continue;
          {
            O42Node *tree = o42_formula_parse (val);
            O42Range range;
            gboolean usable = FALSE;
            if (tree->type == O42_NODE_RANGE)
              { range = tree->as.range; usable = TRUE; }
            else if (tree->type == O42_NODE_REF)
              {
                range.row0 = range.row1 = tree->as.ref.row;
                range.col0 = range.col1 = tree->as.ref.col;
                usable = TRUE;
              }
            if (usable)
              {
                O42Sheet *target = tree->sheet ? o42_book_find_sheet (book, tree->sheet)
                                               : o42_book_sheet (book, 0);
                if (target != NULL)
                  o42_book_define_name (book, nm, target, &range);
              }
            o42_node_free (tree);
          }
        }
      for (guint i = 0; i < r.filter_sheets->len; i++)
        {
          int idx = g_array_index (r.filter_sheets, int, i);
          O42Node *tree = o42_formula_parse (g_ptr_array_index (r.filter_ranges, i));
          if (idx < o42_book_n_sheets (book) && tree->type == O42_NODE_RANGE)
            {
              o42_sheet_set_autofilter (o42_book_sheet (book, idx), &tree->as.range);
              o42_sheet_autofilter_refresh (o42_book_sheet (book, idx));
            }
          o42_node_free (tree);
        }
      for (int i = 0; i < o42_book_n_sheets (book); i++)
        {
          o42_sheet_clear_undo (o42_book_sheet (book, i));
          o42_sheet_set_modified (o42_book_sheet (book, i), FALSE);
        }
    }

  g_ptr_array_unref (r.sst);
  g_hash_table_unref (r.formats);
  g_array_unref (r.fonts);
  g_ptr_array_unref (r.pending_fonts);
  g_ptr_array_unref (r.pending_xfs);
  g_array_unref (r.xfs);
  g_ptr_array_unref (r.sheet_names);
  g_array_unref (r.sheet_offsets);
  g_array_unref (r.xti);
  g_ptr_array_unref (r.supbook_names);
  g_ptr_array_unref (r.supbook_self);
  g_ptr_array_unref (r.names);
  g_ptr_array_unref (r.name_ranges);
  g_array_unref (r.filter_sheets);
  g_ptr_array_unref (r.filter_ranges);
  g_hash_table_unref (r.shared);
  g_string_free (r.txo_text, TRUE);
  g_hash_table_unref (r.note_texts);
  g_ptr_array_unref (r.images);
  g_ptr_array_unref (r.image_formats);
  g_byte_array_unref (r.group);
  g_byte_array_unref (r.drawing);
  for (guint k = 0; k < r.chart_defs->len; k++)
    g_free (g_array_index (r.chart_defs, ChartDef, k).title);
  g_array_unref (r.chart_defs);
  for (guint k = 0; k < r.objs->len; k++)
    obj_info_clear (&g_array_index (r.objs, ObjInfo, k));
  g_array_unref (r.objs);
  g_bytes_unref (stream);
  return ok;
}

/* ====================================================================== */
/* Writing                                                                 */
/* ====================================================================== */

typedef struct
{
  O42Book    *book;
  GByteArray *out;
  gsize       record_start;   /* of the record being written */

  GArray     *fonts;          /* O42Fmt, font fields only; index 0 = default */
  GArray     *xfs;            /* O42Fmt; cell XF i is record 16 + i */
  GPtrArray  *formats;        /* custom format codes, id 164 + i */
  GPtrArray  *sst;
  GHashTable *sst_idx;
  GArray     *sst_offsets;    /* gsize pairs: stream offset of every string and
                               * of the record it sits in, for EXTSST */
  GArray     *palette;        /* guint32 colours at index 8 + i */
  GPtrArray  *addin_names;    /* functions Excel 97 lacked, as EXTERNNAMEs */
  GArray     *xti_spans;      /* guint16 pairs: first and last sheet of 3-D references */
  gboolean    has_names;
  GPtrArray  *images;         /* GBytes: the pictures of every sheet, in store order */
  GPtrArray  *image_formats;
  GArray     *shapes_per_sheet;   /* int */
} Writer;

static void
begin_record (Writer *w, guint id)
{
  w->record_start = w->out->len;
  put16 (w->out, id);
  put16 (w->out, 0);
}

static void
end_record (Writer *w)
{
  gsize len = w->out->len - w->record_start - 4;
  w->out->data[w->record_start + 2] = len & 0xff;
  w->out->data[w->record_start + 3] = (len >> 8) & 0xff;
}

static guint palette_index (Writer *w, guint32 colour);

static guint
xls_border_colour (Writer *w, guint32 colour)
{
  return colour == 0 ? 0x40 : palette_index (w, colour);
}

static guint
palette_index (Writer *w, guint32 colour)
{
  colour &= 0xFFFFFF;
  for (guint i = 0; i < w->palette->len; i++)
    if (g_array_index (w->palette, guint32, i) == colour)
      return 8 + i;
  if (w->palette->len < 56)
    {
      g_array_append_val (w->palette, colour);
      return 8 + w->palette->len - 1;
    }
  return 8;   /* out of slots: black */
}

static guint
font_index (Writer *w, const O42Fmt *fmt)
{
  O42Fmt key;
  o42_fmt_init_default (&key);
  key.family = fmt->family;
  key.size = fmt->size;
  key.bold = fmt->bold;
  key.italic = fmt->italic;
  key.underline = fmt->underline;
  key.strikeout = fmt->strikeout;
  key.colour = fmt->colour;
  for (guint i = 0; i < w->fonts->len; i++)
    if (memcmp (&g_array_index (w->fonts, O42Fmt, i), &key, sizeof key) == 0)
      return i;
  g_array_append_val (w->fonts, key);
  return w->fonts->len - 1;
}

static guint
xf_index (Writer *w, const O42Fmt *fmt)
{
  for (guint i = 0; i < w->xfs->len; i++)
    if (memcmp (&g_array_index (w->xfs, O42Fmt, i), fmt, sizeof *fmt) == 0)
      return i;
  g_array_append_val (w->xfs, *fmt);
  font_index (w, fmt);
  if (fmt->fill != O42_FILL_NONE) palette_index (w, fmt->fill);
  if (fmt->colour != 0) palette_index (w, fmt->colour);
  for (int i = 0; i < 4; i++)
    if (fmt->border_style[i] != O42_BORDER_NONE && fmt->border_colour[i] != 0)
      palette_index (w, fmt->border_colour[i]);
  return w->xfs->len - 1;
}

static guint
format_id (Writer *w, const O42Fmt *fmt)
{
  char *code = o42_fmt_format_string (fmt);
  guint id;

  if (strcmp (code, "General") == 0) id = 0;
  else if (strcmp (code, "0") == 0) id = 1;
  else if (strcmp (code, "0.00") == 0) id = 2;
  else if (strcmp (code, "#,##0") == 0) id = 3;
  else if (strcmp (code, "#,##0.00") == 0) id = 4;
  else if (strcmp (code, "0%") == 0) id = 9;
  else if (strcmp (code, "0.00%") == 0) id = 10;
  else if (strcmp (code, "0.00E+00") == 0) id = 11;
  else if (strcmp (code, "@") == 0) id = 49;
  else
    {
      guint i;
      for (i = 0; i < w->formats->len; i++)
        if (strcmp (g_ptr_array_index (w->formats, i), code) == 0)
          break;
      if (i == w->formats->len)
        g_ptr_array_add (w->formats, g_strdup (code));
      id = 164 + i;
    }
  g_free (code);
  return id;
}

static guint
sst_index (Writer *w, const char *text)
{
  gpointer found;
  if (g_hash_table_lookup_extended (w->sst_idx, text, NULL, &found))
    return GPOINTER_TO_UINT (found);
  {
    char *copy = g_strdup (text);
    g_ptr_array_add (w->sst, copy);
    g_hash_table_insert (w->sst_idx, copy, GUINT_TO_POINTER (w->sst->len - 1));
    return w->sst->len - 1;
  }
}

/* ---- compiling formulas ---- */

static void
put_ref8 (GByteArray *a, int row, int col, gboolean row_abs, gboolean col_abs)
{
  put16 (a, row & 0xFFFF);
  put16 (a, (col & 0xFF) | (row_abs ? 0 : 0x8000) | (col_abs ? 0 : 0x4000));
}

static int
sheet_xti (Writer *w, const char *sheet)
{
  if (sheet == NULL)
    return -1;
  for (int i = 0; i < o42_book_n_sheets (w->book); i++)
    if (strcmp (o42_sheet_get_name (o42_book_sheet (w->book, i)), sheet) == 0)
      return i;
  return -1;
}

/* The XTI of a 3-D span, after the sheets' own and the add-in one. */
static int
sheet_xti_span (Writer *w, const char *first, const char *last)
{
  int a = sheet_xti (w, first), b = sheet_xti (w, last);
  guint16 pair[2];
  if (a < 0 || b < 0)
    return -1;
  if (a > b) { int t = a; a = b; b = t; }
  pair[0] = (guint16) a; pair[1] = (guint16) b;
  for (guint i = 0; i + 1 < w->xti_spans->len; i += 2)
    if (g_array_index (w->xti_spans, guint16, i) == pair[0] && g_array_index (w->xti_spans, guint16, i + 1) == pair[1])
      return o42_book_n_sheets (w->book) + 1 + (int) i / 2;
  g_array_append_vals (w->xti_spans, pair, 2);
  return o42_book_n_sheets (w->book) + 1 + (int) (w->xti_spans->len / 2 - 1);
}

static guint
addin_index (Writer *w, const char *name)
{
  for (guint i = 0; i < w->addin_names->len; i++)
    if (strcmp (g_ptr_array_index (w->addin_names, i), name) == 0)
      return i + 1;
  g_ptr_array_add (w->addin_names, g_strdup (name));
  return w->addin_names->len;
}

/* Emits tokens for `node`.  `ref_class` asks for reference-class tokens,
 * which is what a range passed to SUM wants; elsewhere value class. */
static void compile (Writer *w, const O42Node *node, GByteArray *a, gboolean ref_class,
                     int own_sheet, GByteArray *cb);

/* An array constant: ptgArray in the tokens, the cells in the extra
 * data after them. */
static void
compile_array (const O42Node *node, GByteArray *a, GByteArray *cb)
{
  int rows = node->as.array.rows, cols = node->as.array.cols;

  if (cb == NULL || rows < 1 || cols < 1 || rows > 65536 || cols > 256)
    { put8 (a, 0x1C); put8 (a, 0x0F); return; }
  put8 (a, 0x60);
  for (int i = 0; i < 7; i++) put8 (a, 0);
  put8 (cb, cols - 1);
  put16 (cb, rows - 1);
  for (int i = 0; i < rows * cols; i++)
    {
      const O42Node *item = (guint) i < node->as.array.items->len
                            ? g_ptr_array_index (node->as.array.items, i) : NULL;
      double number = 0;
      gboolean is_number = FALSE;

      if (item != NULL && item->type == O42_NODE_NUMBER)
        { number = item->as.number; is_number = TRUE; }
      else if (item != NULL && item->type == O42_NODE_UNARY && item->as.op.op == O42_OP_NEG &&
               item->as.op.a != NULL && item->as.op.a->type == O42_NODE_NUMBER)
        { number = -item->as.op.a->as.number; is_number = TRUE; }

      if (is_number)
        { put8 (cb, 0x01); put_double (cb, number); }
      else if (item != NULL && item->type == O42_NODE_STRING)
        { put8 (cb, 0x02); put_ustr16 (cb, item->as.string); }
      else if (item != NULL && item->type == O42_NODE_BOOL)
        { put8 (cb, 0x04); put8 (cb, item->as.boolean ? 1 : 0); for (int k = 0; k < 7; k++) put8 (cb, 0); }
      else if (item != NULL && item->type == O42_NODE_ERROR)
        { put8 (cb, 0x10); put8 (cb, error_to_biff (item->as.error)); for (int k = 0; k < 7; k++) put8 (cb, 0); }
      else
        { put8 (cb, 0x00); for (int k = 0; k < 8; k++) put8 (cb, 0); }
    }
}

static void
compile (Writer *w, const O42Node *node, GByteArray *a, gboolean ref_class, int own_sheet, GByteArray *cb)
{
  guint cls = ref_class ? 0x00 : 0x20;

  switch (node->type)
    {
    case O42_NODE_ARRAY:
      compile_array (node, a, cb);
      break;
    case O42_NODE_NUMBER:
      if (node->as.number >= 0 && node->as.number < 65536 && node->as.number == floor (node->as.number))
        { put8 (a, 0x1E); put16 (a, (guint) node->as.number); }
      else
        { put8 (a, 0x1F); put_double (a, node->as.number); }
      break;
    case O42_NODE_STRING:
      put8 (a, 0x17);
      put_ustr8 (a, node->as.string);
      break;
    case O42_NODE_EMPTY:
      put8 (a, 0x16);
      break;
    case O42_NODE_BOOL:
      put8 (a, 0x1D);
      put8 (a, node->as.boolean ? 1 : 0);
      break;
    case O42_NODE_ERROR:
      put8 (a, 0x1C);
      put8 (a, error_to_biff (node->as.error));
      break;
    case O42_NODE_REF:
      {
        int xti = node->sheet_last != NULL ? sheet_xti_span (w, node->sheet, node->sheet_last)
                                           : sheet_xti (w, node->sheet);
        if (xti >= 0 && (xti != own_sheet || node->sheet_last != NULL))
          { put8 (a, 0x3A | cls); put16 (a, xti); }
        else
          put8 (a, 0x24 | cls);
        put_ref8 (a, node->as.ref.row, node->as.ref.col,
                  (node->abs & O42_ABS_ROW0) != 0, (node->abs & O42_ABS_COL0) != 0);
      }
      break;
    case O42_NODE_RANGE:
      {
        int xti = node->sheet_last != NULL ? sheet_xti_span (w, node->sheet, node->sheet_last)
                                           : sheet_xti (w, node->sheet);
        const O42Range *r = &node->as.range;
        if (xti >= 0 && (xti != own_sheet || node->sheet_last != NULL))
          { put8 (a, 0x3B | cls); put16 (a, xti); }
        else
          put8 (a, 0x25 | cls);
        put16 (a, r->row0);
        put16 (a, r->row1);
        put16 (a, (r->col0 & 0xFF) | ((node->abs & O42_ABS_ROW0) ? 0 : 0x8000) | ((node->abs & O42_ABS_COL0) ? 0 : 0x4000));
        put16 (a, (r->col1 & 0xFF) | ((node->abs & O42_ABS_ROW1) ? 0 : 0x8000) | ((node->abs & O42_ABS_COL1) ? 0 : 0x4000));
      }
      break;
    case O42_NODE_UNARY:
      compile (w, node->as.op.a, a, FALSE, own_sheet, cb);
      put8 (a, node->as.op.op == O42_OP_NEG ? 0x13 : node->as.op.op == O42_OP_POS ? 0x12 : 0x14);
      break;
    case O42_NODE_BINARY:
      {
        static const guint8 ptg[] = { 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0B, 0x0E, 0x09, 0x0D, 0x0A, 0x0C };
        compile (w, node->as.op.a, a, FALSE, own_sheet, cb);
        compile (w, node->as.op.b, a, FALSE, own_sheet, cb);
        put8 (a, ptg[node->as.op.op]);
      }
      break;
    case O42_NODE_NAME:
      {
        /* Defined names are written in the order o42_book_names gives. */
        GList *names = o42_book_names (w->book);
        guint idx = 1;
        gboolean found = FALSE;
        for (GList *l = names; l != NULL; l = l->next, idx++)
          if (g_ascii_strcasecmp (l->data, node->as.name) == 0)
            { found = TRUE; break; }
        g_list_free (names);
        if (found)
          {
            put8 (a, 0x23 | cls);
            put16 (a, idx);
            put16 (a, 0);
          }
        else
          {
            /* LET's names, and anything else the book does not define:
             * Excel keeps them as _xlpm. add-in names. */
            char *spelled = g_strconcat ("_xlpm.", node->as.name, NULL);
            put8 (a, 0x39 | cls);
            put16 (a, o42_book_n_sheets (w->book));
            put16 (a, addin_index (w, spelled));
            put16 (a, 0);
            g_free (spelled);
          }
      }
      break;
    case O42_NODE_CALL:
      {
        const FnEntry *fn = function_by_name (node->as.call.name);
        guint argc = node->as.call.args->len;

        if (fn == NULL)
          {
            /* Excel 97 did not have it: an add-in name, then the
             * arguments, then a variable call of function 255. */
            char *spelled = o42_function_is_future (node->as.call.name)
                            ? g_strconcat ("_xlfn.", node->as.call.name, NULL)
                            : g_strdup (node->as.call.name);
            put8 (a, 0x39 | 0x20);
            put16 (a, o42_book_n_sheets (w->book));   /* the add-in XTI, after the sheets' */
            put16 (a, addin_index (w, spelled));
            put16 (a, 0);
            g_free (spelled);
          }
        for (guint i = 0; i < argc; i++)
          {
            const O42Node *arg = g_ptr_array_index (node->as.call.args, i);
            gboolean want_ref = arg->type == O42_NODE_RANGE || arg->type == O42_NODE_REF;
            compile (w, arg, a, want_ref, own_sheet, cb);
          }
        if (fn == NULL)
          {
            put8 (a, 0x22 | 0x20);
            put8 (a, argc + 1);
            put16 (a, 255);
          }
        else if (fn->min == fn->max)
          {
            put8 (a, 0x21 | 0x20);
            put16 (a, fn->index);
          }
        else
          {
            put8 (a, 0x22 | 0x20);
            put8 (a, argc);
            put16 (a, fn->index);
          }
      }
      break;
    default:
      put8 (a, 0x1C);
      put8 (a, 0x1D);
      break;
    }
}

/* ---- the cells of a sheet, gathered before writing ---- */

typedef struct
{
  int      row, col;
  guint    xf;
  O42Value value;
  GBytes  *rgce;     /* NULL for a plain value */
  GBytes  *rgcb;     /* array constants' cells, after the tokens */
  GBytes  *array;    /* the head of an array block: tokens for its ARRAY record */
  gsize    array_cce;   /* how many of them are tokens, before array-constant data */
  O42Range block;
} CellOut;

typedef struct
{
  Writer   *w;
  O42Sheet *sheet;
  int       index;
  GArray   *cells;
  O42FmtTable *table;
  O42FmtIdx default_idx;
} Gather;

static void
gather_cell (O42Sheet *sheet, int row, int col, gpointer user)
{
  Gather *g = user;
  CellOut c;
  char *input;
  O42FmtIdx idx;

  /* BIFF8's grid is 65,536 rows by 256 columns, and office42's is
   * Excel 2007's.  A cell outside Excel 97's grid cannot be written at
   * all, so it is left out and counted, and the caller says so rather
   * than letting it disappear in silence. */
  if (row >= O42_XLS_MAX_ROWS || col >= O42_XLS_MAX_COLS)
    {
      o42_xls_dropped_cells++;
      return;
    }

  input = o42_sheet_get_input (sheet, row, col);
  idx = o42_sheet_get_fmt_idx (sheet, row, col);

  c.row = row;
  c.col = col;
  c.xf = idx == g->default_idx ? 0 : xf_index (g->w, o42_fmt_table_get (g->table, idx));
  o42_sheet_get_value (sheet, row, col, &c.value);
  c.rgce = NULL;
  c.rgcb = NULL;
  c.array = NULL;
  if (o42_sheet_array_range (sheet, row, col, &c.block))
    {
      /* Every cell of an array block is a ptgExp to its head; the head
       * also carries the ARRAY record with the formula itself. */
      GByteArray *a = g_byte_array_new ();
      put8 (a, 0x01);
      put16 (a, c.block.row0);
      put16 (a, c.block.col0);
      c.rgce = g_byte_array_free_to_bytes (a);
      if (c.block.row0 == row && c.block.col0 == col && input != NULL && input[0] == '=')
        {
          O42Node *tree = o42_formula_parse (input + 1);
          GByteArray *rg = g_byte_array_new ();
          GByteArray *cb = g_byte_array_new ();
          compile (g->w, tree, rg, FALSE, g->index, cb);
          c.array_cce = rg->len;
          g_byte_array_append (rg, cb->data, cb->len);
          c.array = g_byte_array_free_to_bytes (rg);
          g_byte_array_unref (cb);
          o42_node_free (tree);
        }
    }
  else if (input != NULL && input[0] == '=')
    {
      O42Node *tree = o42_formula_parse (input + 1);
      GByteArray *a = g_byte_array_new ();
      GByteArray *cb = g_byte_array_new ();
      compile (g->w, tree, a, FALSE, g->index, cb);
      c.rgce = g_byte_array_free_to_bytes (a);
      c.rgcb = g_byte_array_free_to_bytes (cb);
      o42_node_free (tree);
    }
  else if (c.value.type == O42_VALUE_TEXT)
    sst_index (g->w, c.value.as.text);
  g_free (input);
  g_array_append_val (g->cells, c);
}

static int
compare_cells (gconstpointer a, gconstpointer b)
{
  const CellOut *x = a, *y = b;
  if (x->row != y->row) return x->row - y->row;
  return x->col - y->col;
}

static void
write_cell (Writer *w, const CellOut *c)
{
  guint xf = 15 + c->xf;

  if (c->rgce != NULL)
    {
      const O42Value *v = &c->value;
      begin_record (w, R_FORMULA);
      put16 (w->out, c->row);
      put16 (w->out, c->col);
      put16 (w->out, xf);
      switch (v->type)
        {
        case O42_VALUE_NUMBER: put_double (w->out, v->as.number); break;
        case O42_VALUE_TEXT:   put8 (w->out, 0); put8 (w->out, 0); put32 (w->out, 0); put16 (w->out, 0xFFFF); break;
        case O42_VALUE_BOOL:   put8 (w->out, 1); put8 (w->out, 0); put8 (w->out, v->as.boolean ? 1 : 0); put8 (w->out, 0); put16 (w->out, 0); put16 (w->out, 0xFFFF); break;
        case O42_VALUE_ERROR:  put8 (w->out, 2); put8 (w->out, 0); put8 (w->out, error_to_biff (v->as.error)); put8 (w->out, 0); put16 (w->out, 0); put16 (w->out, 0xFFFF); break;
        default:               put8 (w->out, 3); put8 (w->out, 0); put32 (w->out, 0); put16 (w->out, 0xFFFF); break;
        }
      put16 (w->out, 0x0002);   /* recalculate on load */
      put32 (w->out, 0);
      put16 (w->out, g_bytes_get_size (c->rgce));
      g_byte_array_append (w->out, g_bytes_get_data (c->rgce, NULL), g_bytes_get_size (c->rgce));
      if (c->rgcb != NULL)
        g_byte_array_append (w->out, g_bytes_get_data (c->rgcb, NULL), g_bytes_get_size (c->rgcb));
      end_record (w);
      if (c->array != NULL)
        {
          begin_record (w, R_ARRAY);
          put16 (w->out, c->block.row0); put16 (w->out, c->block.row1);
          put8 (w->out, c->block.col0); put8 (w->out, c->block.col1);
          put16 (w->out, 0x0002); put32 (w->out, 0);
          /* The token count excludes any array-constant data at the end,
           * which the reader takes from what follows. */
          put16 (w->out, c->array_cce);
          g_byte_array_append (w->out, g_bytes_get_data (c->array, NULL), g_bytes_get_size (c->array));
          end_record (w);
        }
      if (v->type == O42_VALUE_TEXT)
        {
          begin_record (w, R_STRING);
          put_ustr16 (w->out, v->as.text);
          end_record (w);
        }
      return;
    }

  switch (c->value.type)
    {
    case O42_VALUE_NUMBER:
      begin_record (w, R_NUMBER);
      put16 (w->out, c->row); put16 (w->out, c->col); put16 (w->out, xf);
      put_double (w->out, c->value.as.number);
      end_record (w);
      break;
    case O42_VALUE_TEXT:
      begin_record (w, R_LABELSST);
      put16 (w->out, c->row); put16 (w->out, c->col); put16 (w->out, xf);
      put32 (w->out, sst_index (w, c->value.as.text));
      end_record (w);
      break;
    case O42_VALUE_BOOL:
      begin_record (w, R_BOOLERR);
      put16 (w->out, c->row); put16 (w->out, c->col); put16 (w->out, xf);
      put8 (w->out, c->value.as.boolean ? 1 : 0); put8 (w->out, 0);
      end_record (w);
      break;
    case O42_VALUE_ERROR:
      begin_record (w, R_BOOLERR);
      put16 (w->out, c->row); put16 (w->out, c->col); put16 (w->out, xf);
      put8 (w->out, error_to_biff (c->value.as.error)); put8 (w->out, 1);
      end_record (w);
      break;
    default:
      begin_record (w, R_BLANK);
      put16 (w->out, c->row); put16 (w->out, c->col); put16 (w->out, xf);
      end_record (w);
      break;
    }
}

/* An AI record: what a series takes its title, values or categories
 * from, as a 3-D reference into this sheet. */
static void
chart_ai (Writer *w, guint id, int sheet_index, const O42Range *range, gboolean have)
{
  begin_record (w, C_AI);
  put8 (w->out, id);
  put8 (w->out, have ? 2 : (id == 0 ? 1 : 0));   /* reference, or text / none */
  put16 (w->out, 0);
  put16 (w->out, 0);
  if (have)
    {
      gboolean single = range->row0 == range->row1 && range->col0 == range->col1;
      put16 (w->out, single ? 7 : 11);
      put8 (w->out, single ? 0x3A : 0x3B);
      put16 (w->out, sheet_index);
      put16 (w->out, range->row0);
      if (!single) put16 (w->out, range->row1);
      put16 (w->out, range->col0);
      if (!single) put16 (w->out, range->col1);
    }
  else
    put16 (w->out, 0);
  end_record (w);
}

/* The index of a colour in Excel's default palette, for the records
 * that name colours both ways. */
static guint
default_palette_index (guint32 colour)
{
  for (guint i = 0; i < 56; i++)
    if (PALETTE[i] == (colour & 0xFFFFFF))
      return 8 + i;
  return 8;
}

/* A frame's grey hairline and white fill, or a series' solid fill with
 * no line, as Calc writes them. */
static void
chart_line_area (Writer *w, guint32 colour, gboolean frame)
{
  begin_record (w, C_LINEFORMAT);
  if (frame)
    { put32 (w->out, 0xD9D9D9); put16 (w->out, 0); put16 (w->out, 0); put16 (w->out, 0); put16 (w->out, 0x001F); }
  else
    { put32 (w->out, 0); put16 (w->out, 5); put16 (w->out, 0xFFFF); put16 (w->out, 0); put16 (w->out, 0x004D); }
  end_record (w);
  begin_record (w, C_AREAFORMAT);
  if (frame)
    { put32 (w->out, 0xFFFFFF); put32 (w->out, 0); put16 (w->out, 0); put16 (w->out, 0); put16 (w->out, 0x004E); put16 (w->out, 0x004D); }
  else
    { put32 (w->out, colour); put32 (w->out, 0); put16 (w->out, 1); put16 (w->out, 0); put16 (w->out, default_palette_index (colour)); put16 (w->out, 0x004D); }
  end_record (w);
}

static void
chart_axis (Writer *w, int which)
{
  begin_record (w, C_AXIS);
  put16 (w->out, which); for (int k = 0; k < 16; k++) put8 (w->out, 0);
  end_record (w);
  begin_record (w, C_BEGIN); end_record (w);
  if (which == 0)
    {
      begin_record (w, C_CATSERRANGE);
      put16 (w->out, 1); put16 (w->out, 1); put16 (w->out, 1); put16 (w->out, 1);
      end_record (w);
      begin_record (w, 0x1062);   /* AXCEXT: everything automatic */
      for (int k = 0; k < 8; k++) put16 (w->out, 0);
      put16 (w->out, 0x00FF);
      end_record (w);
    }
  else
    {
      begin_record (w, C_VALUERANGE);
      for (int k = 0; k < 40; k++) put8 (w->out, 0);
      put16 (w->out, 0x011F);   /* everything automatic */
      end_record (w);
      begin_record (w, 0x104E);   /* IFMT: General */
      put16 (w->out, 0);
      end_record (w);
    }
  begin_record (w, C_TICK);
  put8 (w->out, 3); put8 (w->out, 0); put8 (w->out, 3); put8 (w->out, 1);   /* major ticks cross, no minor ones, labels beside */
  put32 (w->out, 0);
  for (int k = 0; k < 16; k++) put8 (w->out, 0);
  put16 (w->out, 0x0001); put16 (w->out, 0x7FFF); put16 (w->out, 0);
  end_record (w);
  begin_record (w, C_FONTX); put16 (w->out, 0); end_record (w);
  begin_record (w, C_AXISLINEFORMAT); put16 (w->out, 0); end_record (w);
  begin_record (w, C_LINEFORMAT);
  put32 (w->out, 0); put16 (w->out, 0); put16 (w->out, 0xFFFF); put16 (w->out, 0x0004); put16 (w->out, 0x004D);   /* fAxisOn */
  end_record (w);
  begin_record (w, C_END); end_record (w);
}

/* ---- Form controls in a sheet's OBJ records ---------------------------- */

/* The ot of a form control: the number Excel's OBJ record uses to say
 * what kind of thing the shape is. */
static guint16
control_ot (O42ShapeKind kind)
{
  switch (kind)
    {
    case O42_SHAPE_BUTTON:    return 0x07;
    case O42_SHAPE_CHECKBOX:  return 0x0B;
    case O42_SHAPE_OPTION:    return 0x0C;
    case O42_SHAPE_LABEL:     return 0x0E;
    case O42_SHAPE_SPINNER:   return 0x10;
    case O42_SHAPE_SCROLLBAR: return 0x11;
    case O42_SHAPE_LISTBOX:   return 0x12;
    case O42_SHAPE_GROUPBOX:  return 0x13;
    case O42_SHAPE_COMBO:     return 0x14;
    default:                  return 0x08;
    }
}

/* The little formula a control keeps for its linked cell or for its
 * list's range: a length, four unused bytes, one ptg, and a pad byte.
 * An empty array comes back for anything that is not a plain
 * reference, and the caller then leaves the record out. */
static GByteArray *
obj_fmla (const char *ref)
{
  GByteArray *a = g_byte_array_new ();
  const char *colon = ref != NULL ? strchr (ref, ':') : NULL;
  int r1 = 0, c1 = 0, r2 = 0, c2 = 0;
  gboolean area = FALSE;

  if (ref == NULL || *ref == 0)
    return a;
  if (colon != NULL)
    {
      char *left = g_strndup (ref, (gsize) (colon - ref));

      area = o42_ref_parse (left, &r1, &c1, NULL) && o42_ref_parse (colon + 1, &r2, &c2, NULL);
      g_free (left);
      if (!area)
        return a;
    }
  else if (!o42_ref_parse (ref, &r1, &c1, NULL))
    return a;

  put16 (a, area ? 9 : 5);
  put32 (a, 0);
  if (area)
    {
      put8 (a, 0x25);
      put16 (a, r1); put16 (a, r2); put16 (a, c1); put16 (a, c2);
    }
  else
    {
      put8 (a, 0x24);
      put16 (a, r1); put16 (a, c1);
    }
  put8 (a, 0);
  return a;
}

/* The subrecords after ftCmo that carry what a control is set to: the
 * cell it drives, a check mark, a spinner's bounds, a list's range.
 * The shapes of these records are Excel's, and are what LibreOffice
 * writes for the same controls, so that both read them back. */
static void
put_control_records (Writer *w, O42Sheet *sheet, const O42Shape *shape)
{
  GByteArray *link = obj_fmla (shape->link);
  double value = 0;
  gboolean has_value = o42_sheet_control_value (sheet, shape, &value);

  switch (shape->kind)
    {
    case O42_SHAPE_CHECKBOX:
    case O42_SHAPE_OPTION:
      {
        int on = shape->kind == O42_SHAPE_CHECKBOX
          ? (has_value && value != 0)
          : (has_value && value == (shape->value != 0 ? shape->value : 1));

        put16 (w->out, 0x0A); put16 (w->out, 12);
        put16 (w->out, on);
        for (int k = 0; k < 10; k++) put8 (w->out, 0);
        if (link->len > 0)
          {
            put16 (w->out, 0x14); put16 (w->out, link->len);
            g_byte_array_append (w->out, link->data, link->len);
          }
        put16 (w->out, 0x0A); put16 (w->out, 8);
        put16 (w->out, on);
        for (int k = 0; k < 6; k++) put8 (w->out, 0);
      }
      break;

    case O42_SHAPE_SPINNER:
    case O42_SHAPE_SCROLLBAR:
      put16 (w->out, 0x0C); put16 (w->out, 20);
      put32 (w->out, 0);
      put16 (w->out, (int) (has_value ? value : shape->min));
      put16 (w->out, (int) shape->min);
      put16 (w->out, (int) shape->max);
      put16 (w->out, (int) (shape->step > 0 ? shape->step : 1));
      put16 (w->out, (int) (shape->page > 0 ? shape->page : 10));
      put16 (w->out, shape->kind == O42_SHAPE_SCROLLBAR && shape->width >= shape->height ? 1 : 0);
      put16 (w->out, 0x000F);
      put16 (w->out, 1);
      if (link->len > 0)
        {
          put16 (w->out, 0x0E); put16 (w->out, link->len);
          g_byte_array_append (w->out, link->data, link->len);
        }
      break;

    case O42_SHAPE_LISTBOX:
    case O42_SHAPE_COMBO:
      {
        GByteArray *src = obj_fmla (shape->source);
        char **items = o42_sheet_control_items (sheet, shape);
        int lines = items != NULL ? (int) g_strv_length (items) : 0;
        int sel = (int) (has_value ? value : 0);

        put16 (w->out, 0x0C); put16 (w->out, 20);
        put32 (w->out, 0);
        put16 (w->out, sel); put16 (w->out, 0); put16 (w->out, 0);
        put16 (w->out, 1); put16 (w->out, lines);
        put16 (w->out, 0); put16 (w->out, 0x000F); put16 (w->out, 1);
        if (link->len > 0)
          {
            put16 (w->out, 0x0E); put16 (w->out, link->len);
            g_byte_array_append (w->out, link->data, link->len);
          }

        /* The list's own record: the range it reads, how many rows
         * that is, and which of them is chosen. */
        put16 (w->out, 0x13);
        put16 (w->out, src->len + (shape->kind == O42_SHAPE_COMBO ? 18 : 11));
        put16 (w->out, src->len);
        if (src->len > 0)
          g_byte_array_append (w->out, src->data, src->len);
        put16 (w->out, lines);
        put16 (w->out, sel);
        put16 (w->out, 0); put16 (w->out, 0);
        if (shape->kind == O42_SHAPE_COMBO)
          { put16 (w->out, 0); put16 (w->out, 1); put16 (w->out, 0); put16 (w->out, 0); }
        else
          put8 (w->out, 0);
        g_byte_array_unref (src);
        if (items != NULL)
          g_strfreev (items);
      }
      break;

    case O42_SHAPE_GROUPBOX:
      put16 (w->out, 0x0F); put16 (w->out, 6);
      for (int k = 0; k < 6; k++) put8 (w->out, 0);
      break;

    default:
      break;
    }
  g_byte_array_unref (link);
}

/* A control's caption travels as the text of the shape, in a TXO and
 * the two CONTINUEs that follow it: the characters, then one run. */
static void
write_control_text (Writer *w, const O42Shape *shape)
{
  glong n;

  if (shape->text == NULL || *shape->text == 0)
    return;
  n = MIN (char_count (shape->text), 32000);
  begin_record (w, R_TXO);
  put16 (w->out, shape->kind == O42_SHAPE_BUTTON ? 0x0024 : 0x0022);
  put16 (w->out, 0);
  for (int k = 0; k < 6; k++) put8 (w->out, 0);
  put16 (w->out, n); put16 (w->out, 16); put16 (w->out, 0); put32 (w->out, 0);
  end_record (w);
  begin_record (w, R_CONTINUE);
  put_ustr_body (w->out, shape->text);
  end_record (w);
  begin_record (w, R_CONTINUE);
  put16 (w->out, 0); put16 (w->out, 0); put32 (w->out, 0);
  put16 (w->out, n); put16 (w->out, 0); put32 (w->out, 0);
  end_record (w);
}

/* The chart substream that follows a chart's OBJ record, in the order
 * Excel and Calc write it: the frame, a SERIES per data column with
 * its AI links, the axes, the chart type, the legend and the title. */
static void
write_chart_substream (Writer *w, O42Sheet *sheet, int sheet_index, const O42Chart *chart)
{
  static const guint32 COLOURS[] = { 0x000080, 0x800000, 0x008000, 0x008080, 0x800080, 0x808000, 0x808080, 0x0000FF };
  const O42Range *d = &chart->data;
  int first_row = d->row0 + (chart->first_row_labels ? 1 : 0);
  int first_col = d->col0 + (chart->first_col_labels ? 1 : 0);
  gboolean scatter = chart->kind == O42_CHART_SCATTER, pie = chart->kind == O42_CHART_PIE;
  int n_series = 0;

  begin_record (w, R_BOF);
  put16 (w->out, 0x0600); put16 (w->out, 0x0020); put16 (w->out, 0x0DBB); put16 (w->out, 0x07CC);
  put32 (w->out, 0x00000041); put32 (w->out, 0x00000006);
  end_record (w);
  begin_record (w, R_HEADER); end_record (w);
  begin_record (w, R_FOOTER); end_record (w);
  begin_record (w, R_HCENTER); put16 (w->out, 0); end_record (w);
  begin_record (w, R_VCENTER); put16 (w->out, 0); end_record (w);
  begin_record (w, R_PROTECT); put16 (w->out, 0); end_record (w);
  begin_record (w, C_UNITS); put16 (w->out, 0); end_record (w);
  begin_record (w, C_CHART);
  put32 (w->out, 0); put32 (w->out, 0);
  put32 (w->out, (guint32) (chart->width * 0.75 * 65536));
  put32 (w->out, (guint32) (chart->height * 0.75 * 65536));
  end_record (w);
  begin_record (w, C_BEGIN); end_record (w);

  begin_record (w, C_FRAME); put16 (w->out, 0); put16 (w->out, 0x0003); end_record (w);
  begin_record (w, C_BEGIN); end_record (w);
  chart_line_area (w, 0xFFFFFF, TRUE);
  begin_record (w, C_END); end_record (w);

  /* One series per data column; for a scatter the first column is x. */
  {
    int x_col = scatter ? d->col0 : -1;
    int start_col = scatter ? d->col0 + 1 : first_col;
    int n_points = d->row1 - first_row + 1;

    for (int col = start_col; col <= d->col1 && (!pie || n_series == 0); col++)
      {
        O42Range title = { d->row0, col, d->row0, col };
        O42Range values = { first_row, col, d->row1, col };
        O42Range cats = { first_row, scatter ? x_col : d->col0, d->row1, scatter ? x_col : d->col0 };
        gboolean have_cats = scatter || chart->first_col_labels;

        begin_record (w, C_SERIES);
        put16 (w->out, have_cats && !scatter ? 3 : 1);   /* sdtX: text categories, or numeric */
        put16 (w->out, 1);
        put16 (w->out, n_points); put16 (w->out, n_points);
        put16 (w->out, 1); put16 (w->out, 0);
        end_record (w);
        begin_record (w, C_BEGIN); end_record (w);
        chart_ai (w, 0, sheet_index, &title, chart->first_row_labels);
        chart_ai (w, 1, sheet_index, &values, TRUE);
        chart_ai (w, 2, sheet_index, &cats, have_cats);
        chart_ai (w, 3, sheet_index, &values, FALSE);
        begin_record (w, C_DATAFORMAT);
        put16 (w->out, 0xFFFF); put16 (w->out, n_series); put16 (w->out, n_series); put16 (w->out, 0);
        end_record (w);
        begin_record (w, C_BEGIN); end_record (w);
        chart_line_area (w, COLOURS[n_series % G_N_ELEMENTS (COLOURS)], FALSE);
        begin_record (w, C_END); end_record (w);
        begin_record (w, C_SERTOCRT); put16 (w->out, 0); end_record (w);
        begin_record (w, C_END); end_record (w);
        n_series++;
      }
  }

  begin_record (w, C_SHTPROPS); put16 (w->out, 0x000A); put8 (w->out, 0); put8 (w->out, 0); end_record (w);
  begin_record (w, C_AXESUSED); put16 (w->out, 1); end_record (w);
  begin_record (w, C_AXISPARENT);
  put16 (w->out, 0); put32 (w->out, 0x00A7); put32 (w->out, 0x0426); put32 (w->out, 0x0EA4); put32 (w->out, 0x093C);
  end_record (w);
  begin_record (w, C_BEGIN); end_record (w);
  begin_record (w, C_POS);
  put16 (w->out, 2); put16 (w->out, 2); put32 (w->out, 0); put32 (w->out, 0x0390); put32 (w->out, 0x0F67); put32 (w->out, 0x0BB8);
  end_record (w);
  if (!pie)
    {
      chart_axis (w, 0);
      chart_axis (w, 1);
    }
  begin_record (w, C_CHARTFORMAT);
  for (int k = 0; k < 16; k++) put8 (w->out, 0);
  put16 (w->out, pie ? 1 : 0); put16 (w->out, 0);
  end_record (w);
  begin_record (w, C_BEGIN); end_record (w);
  switch (chart->kind)
    {
    case O42_CHART_LINE:
      begin_record (w, C_LINE); put16 (w->out, 0); end_record (w);
      break;
    case O42_CHART_PIE:
      begin_record (w, C_PIE); put16 (w->out, 0); put16 (w->out, 0); put16 (w->out, 0); end_record (w);
      break;
    case O42_CHART_AREA:
      begin_record (w, C_AREA); put16 (w->out, 0); end_record (w);
      break;
    case O42_CHART_SCATTER:
      begin_record (w, C_SCATTER); put16 (w->out, 100); put16 (w->out, 1); put16 (w->out, 0); end_record (w);
      break;
    default:
      begin_record (w, C_BAR);
      put16 (w->out, chart->kind == O42_CHART_STACKED || chart->kind == O42_CHART_PERCENT ? (guint16) -100 : 0);
      put16 (w->out, 150);
      put16 (w->out, (chart->kind == O42_CHART_BAR ? 0x01 : 0) |
                     (chart->kind == O42_CHART_STACKED ? 0x02 : 0) |
                     (chart->kind == O42_CHART_PERCENT ? 0x06 : 0));
      end_record (w);
      break;
    }
  begin_record (w, C_CHARTFORMATLINK); end_record (w);
  if ((n_series > 1 || pie) && chart->legend)
    {
      begin_record (w, C_LEGEND);
      put32 (w->out, 0); put32 (w->out, 0); put32 (w->out, 0); put32 (w->out, 0);
      put8 (w->out, 3); put8 (w->out, 1); put16 (w->out, 0x000F);
      end_record (w);
      begin_record (w, C_BEGIN); end_record (w);
      begin_record (w, C_POS);
      put16 (w->out, 5); put16 (w->out, 2); put32 (w->out, 0); put32 (w->out, 0); put32 (w->out, 0); put32 (w->out, 0);
      end_record (w);
      begin_record (w, C_END); end_record (w);
    }
  begin_record (w, C_END); end_record (w);   /* CHARTFORMAT */
  begin_record (w, C_END); end_record (w);   /* AXISPARENT */

  if (chart->title != NULL && chart->title[0] != '\0')
    {
      begin_record (w, C_TEXT);
      put8 (w->out, 2); put8 (w->out, 2); put16 (w->out, 1);
      put32 (w->out, 0xFFFFFF); put32 (w->out, 0); put32 (w->out, 0); put32 (w->out, 0); put32 (w->out, 0);
      put16 (w->out, 0x00B1); put16 (w->out, 0x004D); put16 (w->out, 0); put16 (w->out, 0);
      end_record (w);
      begin_record (w, C_BEGIN); end_record (w);
      begin_record (w, C_POS);
      put16 (w->out, 2); put16 (w->out, 2); put32 (w->out, 0); put32 (w->out, 0); put32 (w->out, 0); put32 (w->out, 0);
      end_record (w);
      begin_record (w, C_FONTX); put16 (w->out, 0); end_record (w);
      begin_record (w, C_AI); put8 (w->out, 0); put8 (w->out, 1); put16 (w->out, 0); put16 (w->out, 0); put16 (w->out, 0); end_record (w);
      begin_record (w, C_SERIESTEXT);
      put16 (w->out, 0);
      put_ustr8 (w->out, chart->title);
      end_record (w);
      begin_record (w, C_OBJECTLINK); put16 (w->out, 1); put16 (w->out, 0); put16 (w->out, 0); end_record (w);
      begin_record (w, C_END); end_record (w);
    }
  begin_record (w, C_END); end_record (w);   /* CHART */
  begin_record (w, R_EOF); end_record (w);
}

static void
write_sheet (Writer *w, O42Sheet *sheet, int index, GArray *cells)
{
  int default_width = o42_sheet_col_width (sheet, O42_MAX_COLS - 1);
  int default_height = o42_sheet_row_height (sheet, O42_MAX_ROWS - 1);
  O42Range used;
  int frozen_rows, frozen_cols;

  o42_sheet_used_range (sheet, &used);

  begin_record (w, R_BOF);
  put16 (w->out, 0x0600); put16 (w->out, 0x0010); put16 (w->out, 0x0DBB); put16 (w->out, 0x07CC);
  put32 (w->out, 0x00000041); put32 (w->out, 0x00000006);
  end_record (w);

  begin_record (w, R_DEFCOLWIDTH);
  put16 (w->out, (guint) ((default_width - 5) / 7.0 + 0.5));
  end_record (w);

  for (int col = 0; col < O42_MAX_COLS - 1; col++)
    {
      int width = o42_sheet_col_width (sheet, col);
      gboolean hidden = o42_sheet_col_hidden (sheet, col);
      int level = o42_sheet_col_level (sheet, col);
      if (width == default_width && !hidden && level == 0)
        continue;
      begin_record (w, R_COLINFO);
      put16 (w->out, col); put16 (w->out, col);
      put16 (w->out, (guint) ((hidden ? default_width : width) - 5) * 256 / 7);
      put16 (w->out, 15);
      put16 (w->out, (hidden ? 0x0001 : 0) | ((level & 0x07) << 8));
      put16 (w->out, 0);
      end_record (w);
    }

  begin_record (w, R_DIMENSIONS);
  put32 (w->out, used.row0); put32 (w->out, used.row1 + 1);
  put16 (w->out, used.col0); put16 (w->out, used.col1 + 1); put16 (w->out, 0);
  end_record (w);

  {
    guint i = 0;
    int last_row = -1;
    for (int row = 0; row < O42_MAX_ROWS; row++)
      {
        int height = o42_sheet_row_height (sheet, row);
        gboolean hidden = o42_sheet_row_hidden_by_hand (sheet, row);
        int level = o42_sheet_row_level (sheet, row);
        gboolean has_cells = i < cells->len && g_array_index (cells, CellOut, i).row == row;
        if (!has_cells && height == default_height && !hidden && level == 0)
          {
            if (row > used.row1 && i >= cells->len) break;
            continue;
          }
        (void) last_row;
        begin_record (w, R_ROW);
        put16 (w->out, row);
        put16 (w->out, has_cells ? g_array_index (cells, CellOut, i).col : 0);
        {
          guint j = i;
          int last_col = 0;
          while (j < cells->len && g_array_index (cells, CellOut, j).row == row)
            last_col = g_array_index (cells, CellOut, j++).col;
          put16 (w->out, has_cells ? last_col + 1 : 0);
        }
        put16 (w->out, (guint) (height * 15) | (height == default_height ? 0x8000 : 0));
        put16 (w->out, 0); put16 (w->out, 0);
        put16 (w->out, 0x0100 | (hidden ? 0x0020 : 0) | (height != default_height ? 0x0040 : 0) | (level & 0x07));
        put16 (w->out, 15);
        end_record (w);
        while (i < cells->len && g_array_index (cells, CellOut, i).row == row)
          write_cell (w, &g_array_index (cells, CellOut, i++));
      }
  }

  /* Pictures and notes as Escher shapes, each followed by its OBJ (and
   * a note by its TXO); the NOTE records come after them all. */
  {
    GPtrArray *pictures = o42_sheet_pictures (sheet);
    GHashTable *notes = o42_sheet_notes (sheet);
    GArray *shapes = g_array_new (FALSE, FALSE, sizeof (O42EscherShape));
    GPtrArray *controls = g_ptr_array_new ();   /* per shape: its O42Shape, or NULL */
    GHashTableIter iter;
    gpointer key, value;
    int first_blip = 0;
    /* This sheet's pictures were appended to the store in order; the
     * ones before belong to earlier sheets. */
    {
      int before = 0;
      for (int i = 0; i < index; i++)
        before += o42_sheet_pictures (o42_book_sheet (w->book, i))->len;
      first_blip = before;
    }
    for (guint i = 0; i < pictures->len; i++)
      {
        const O42Picture *pic = g_ptr_array_index (pictures, i);
        O42EscherShape s;
        double x1 = sheet_col_x (sheet, pic->col) + pic->dx + pic->width;
        double y1 = sheet_row_y (sheet, pic->row) + pic->dy + pic->height;
        int c = pic->col, rr = pic->row;
        double x = sheet_col_x (sheet, c), y = sheet_row_y (sheet, rr);

        memset (&s, 0, sizeof s);
        s.blip = first_blip + (int) i + 1;
        s.col1 = pic->col; s.row1 = pic->row;
        s.dx1 = pic->dx / MAX (o42_sheet_col_width (sheet, pic->col), 1);
        s.dy1 = pic->dy / MAX (o42_sheet_row_height (sheet, pic->row), 1);
        while (c < O42_MAX_COLS - 1 && x + o42_sheet_col_width (sheet, c) <= x1) { x += o42_sheet_col_width (sheet, c); c++; }
        while (rr < O42_MAX_ROWS - 1 && y + o42_sheet_row_height (sheet, rr) <= y1) { y += o42_sheet_row_height (sheet, rr); rr++; }
        s.col2 = c; s.row2 = rr;
        s.dx2 = (x1 - x) / MAX (o42_sheet_col_width (sheet, c), 1);
        s.dy2 = (y1 - y) / MAX (o42_sheet_row_height (sheet, rr), 1);
        g_array_append_val (shapes, s);
        g_ptr_array_add (controls, NULL);
      }
    {
      GPtrArray *charts = o42_sheet_charts (sheet);
      for (guint i = 0; i < charts->len; i++)
        {
          const O42Chart *chart = g_ptr_array_index (charts, i);
          O42EscherShape s;
          double x1 = sheet_col_x (sheet, chart->col) + chart->dx + chart->width;
          double y1 = sheet_row_y (sheet, chart->row) + chart->dy + chart->height;
          int c = chart->col, rr = chart->row;
          double x = sheet_col_x (sheet, c), y = sheet_row_y (sheet, rr);

          memset (&s, 0, sizeof s);
          s.is_chart = TRUE;
          s.blip = (int) i;   /* which chart, for the substream */
          s.col1 = chart->col; s.row1 = chart->row;
          s.dx1 = chart->dx / MAX (o42_sheet_col_width (sheet, chart->col), 1);
          s.dy1 = chart->dy / MAX (o42_sheet_row_height (sheet, chart->row), 1);
          while (c < O42_MAX_COLS - 1 && x + o42_sheet_col_width (sheet, c) <= x1) { x += o42_sheet_col_width (sheet, c); c++; }
          while (rr < O42_MAX_ROWS - 1 && y + o42_sheet_row_height (sheet, rr) <= y1) { y += o42_sheet_row_height (sheet, rr); rr++; }
          s.col2 = c; s.row2 = rr;
          s.dx2 = (x1 - x) / MAX (o42_sheet_col_width (sheet, c), 1);
          s.dy2 = (y1 - y) / MAX (o42_sheet_row_height (sheet, rr), 1);
          g_array_append_val (shapes, s);
          g_ptr_array_add (controls, NULL);
        }
    }

    /* The form controls, anchored the way a picture is. */
    {
      GPtrArray *sheet_shapes = o42_sheet_shapes (sheet);

      for (guint i = 0; i < sheet_shapes->len; i++)
        {
          const O42Shape *shape = g_ptr_array_index (sheet_shapes, i);
          O42EscherShape s;
          double x1, y1, x, y;
          int c, rr;

          if (!o42_shape_is_control (shape->kind))
            continue;
          x1 = sheet_col_x (sheet, shape->col) + shape->dx + shape->width;
          y1 = sheet_row_y (sheet, shape->row) + shape->dy + shape->height;
          c = shape->col; rr = shape->row;
          x = sheet_col_x (sheet, c); y = sheet_row_y (sheet, rr);

          memset (&s, 0, sizeof s);
          s.is_control = TRUE;
          s.col1 = shape->col; s.row1 = shape->row;
          s.dx1 = shape->dx / MAX (o42_sheet_col_width (sheet, shape->col), 1);
          s.dy1 = shape->dy / MAX (o42_sheet_row_height (sheet, shape->row), 1);
          while (c < O42_MAX_COLS - 1 && x + o42_sheet_col_width (sheet, c) <= x1) { x += o42_sheet_col_width (sheet, c); c++; }
          while (rr < O42_MAX_ROWS - 1 && y + o42_sheet_row_height (sheet, rr) <= y1) { y += o42_sheet_row_height (sheet, rr); rr++; }
          s.col2 = c; s.row2 = rr;
          s.dx2 = (x1 - x) / MAX (o42_sheet_col_width (sheet, c), 1);
          s.dy2 = (y1 - y) / MAX (o42_sheet_row_height (sheet, rr), 1);
          g_array_append_val (shapes, s);
          g_ptr_array_add (controls, (gpointer) shape);
        }
    }

    g_hash_table_iter_init (&iter, notes);
    while (g_hash_table_iter_next (&iter, &key, &value))
      {
        guint64 k = *(guint64 *) key;
        O42EscherShape s;
        memset (&s, 0, sizeof s);
        s.is_note = TRUE;
        s.note = value;
        s.note_row = o42_key_row (k);
        s.note_col = o42_key_col (k);
        s.col1 = MIN (s.note_col + 1, O42_MAX_COLS - 1); s.dx1 = 0.2;
        s.row1 = MAX (s.note_row - 1, 0); s.dy1 = 0.1;
        s.col2 = MIN (s.note_col + 3, O42_MAX_COLS - 1); s.dx2 = 0.8;
        s.row2 = MIN (s.note_row + 3, O42_MAX_ROWS - 1); s.dy2 = 0.2;
        g_array_append_val (shapes, s);
        g_ptr_array_add (controls, NULL);
      }

    if (shapes->len > 0)
      {
        GPtrArray *chunks = o42_escher_drawing (index + 1, shapes);
        for (guint i = 0; i < chunks->len; i++)
          {
            GBytes *chunk = g_ptr_array_index (chunks, i);
            const O42EscherShape *s = &g_array_index (shapes, O42EscherShape, i);

            begin_record (w, R_MSODRAWING);
            g_byte_array_append (w->out, g_bytes_get_data (chunk, NULL), g_bytes_get_size (chunk));
            end_record (w);

            const O42Shape *ctl = i < controls->len ? g_ptr_array_index (controls, i) : NULL;

            begin_record (w, R_OBJ);
            put16 (w->out, 0x15); put16 (w->out, 0x12);
            put16 (w->out, ctl != NULL ? control_ot (ctl->kind)
                           : s->is_note ? 0x19 : s->is_chart ? 0x05 : 0x08);
            put16 (w->out, i + 1);
            put16 (w->out, ctl != NULL ? 0x0011 : s->is_note ? 0x4011 : 0x6011);
            for (int k = 0; k < 12; k++) put8 (w->out, 0);
            if (ctl != NULL)
              put_control_records (w, sheet, ctl);
            if (s->is_note)
              {
                put16 (w->out, 0x0D); put16 (w->out, 0x16);
                for (int k = 0; k < 16; k++) put8 (w->out, 0);
                put16 (w->out, 0); put32 (w->out, 0);
              }
            put16 (w->out, 0); put16 (w->out, 0);
            end_record (w);

            if (ctl != NULL)
              write_control_text (w, ctl);
            if (s->is_chart)
              write_chart_substream (w, sheet, index, g_ptr_array_index (o42_sheet_charts (sheet), s->blip));
            if (s->is_note)
              {
                glong n = MIN (char_count (s->note), 32000);
                begin_record (w, R_TXO);
                put16 (w->out, 0x0212); put16 (w->out, 0);
                for (int k = 0; k < 6; k++) put8 (w->out, 0);
                put16 (w->out, n); put16 (w->out, 16); put16 (w->out, 0); put32 (w->out, 0);
                end_record (w);
                begin_record (w, R_CONTINUE);
                put_ustr_body (w->out, s->note);
                end_record (w);
                begin_record (w, R_CONTINUE);
                put16 (w->out, 0); put16 (w->out, 0); put32 (w->out, 0);
                put16 (w->out, n); put16 (w->out, 0); put32 (w->out, 0);
                end_record (w);
              }
          }
        for (guint i = 0; i < shapes->len; i++)
          {
            const O42EscherShape *s = &g_array_index (shapes, O42EscherShape, i);
            if (!s->is_note) continue;
            begin_record (w, R_NOTE);
            put16 (w->out, s->note_row); put16 (w->out, s->note_col);
            put16 (w->out, 0); put16 (w->out, i + 1);
            put_ustr16 (w->out, "office42");
            put8 (w->out, 0);
            end_record (w);
          }
        g_ptr_array_unref (chunks);
      }
    g_ptr_array_unref (controls);
    g_array_unref (shapes);
  }

  o42_sheet_get_frozen (sheet, &frozen_rows, &frozen_cols);
  begin_record (w, R_WINDOW2);
  put16 (w->out, 0x06B6 | (frozen_rows > 0 || frozen_cols > 0 ? 0x0108 : 0) | (index == 0 ? 0x0600 : 0));
  put16 (w->out, 0); put16 (w->out, 0);
  put32 (w->out, 0x40); put16 (w->out, 0); put16 (w->out, 0); put16 (w->out, 0); put32 (w->out, 0);
  end_record (w);
  if (frozen_rows > 0 || frozen_cols > 0)
    {
      begin_record (w, R_PANE);
      put16 (w->out, frozen_cols); put16 (w->out, frozen_rows);
      put16 (w->out, frozen_rows); put16 (w->out, frozen_cols);
      put8 (w->out, frozen_rows > 0 && frozen_cols > 0 ? 0 : frozen_rows > 0 ? 2 : 1);
      put8 (w->out, 0);
      end_record (w);
    }

  {
    GArray *merges = o42_sheet_merges (sheet);
    if (merges->len > 0)
      {
        begin_record (w, R_MERGECELLS);
        put16 (w->out, merges->len);
        for (guint i = 0; i < merges->len; i++)
          {
            const O42Range *m = &g_array_index (merges, O42Range, i);
            put16 (w->out, m->row0); put16 (w->out, m->row1);
            put16 (w->out, m->col0); put16 (w->out, m->col1);
          }
        end_record (w);
      }
  }
  /* Conditional formats: a CONDFMT per rule, each with one CF whose
   * differential format carries only the masked fields. */
  {
    GArray *conds = o42_sheet_conditions (sheet);
    for (guint i = 0; i < conds->len; i++)
      {
        const O42Condition *c = &g_array_index (conds, O42Condition, i);
        static const guint8 ops[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        gboolean two = c->op == O42_COND_BETWEEN || c->op == O42_COND_NOT_BETWEEN;
        gboolean font = (c->mask & (O42_FMT_BOLD | O42_FMT_ITALIC | O42_FMT_UNDERLINE | O42_FMT_STRIKEOUT | O42_FMT_COLOUR | O42_FMT_SIZE)) != 0;
        gboolean border = (c->mask & O42_FMT_BORDERS) != 0;
        gboolean pattern = (c->mask & O42_FMT_FILL) != 0 && c->fmt.fill != O42_FILL_NONE;
        guint32 flags = 0x003FFFFFu;   /* everything "not changed" to start */
        GByteArray *f1 = g_byte_array_new (), *f2 = g_byte_array_new ();
        O42Node num;

        memset (&num, 0, sizeof num);
        num.type = O42_NODE_NUMBER;
        num.as.number = c->value;
        compile (w, &num, f1, FALSE, index, NULL);
        if (two)
          {
            num.as.number = c->value2;
            compile (w, &num, f2, FALSE, index, NULL);
          }

        begin_record (w, R_CONDFMT);
        put16 (w->out, 1); put16 (w->out, 1);
        put16 (w->out, c->range.row0); put16 (w->out, c->range.row1);
        put16 (w->out, c->range.col0); put16 (w->out, c->range.col1);
        put16 (w->out, 1);
        put16 (w->out, c->range.row0); put16 (w->out, c->range.row1);
        put16 (w->out, c->range.col0); put16 (w->out, c->range.col1);
        end_record (w);

        if (font) flags |= 1u << 26;
        if (border)
          {
            flags |= 1u << 28;
            if (c->fmt.border_left) flags &= ~(1u << 10);
            if (c->fmt.border_right) flags &= ~(1u << 11);
            if (c->fmt.border_top) flags &= ~(1u << 12);
            if (c->fmt.border_bottom) flags &= ~(1u << 13);
          }
        if (pattern)
          {
            flags |= 1u << 29;
            flags &= ~((1u << 16) | (1u << 17) | (1u << 18));
          }

        begin_record (w, R_CF);
        put8 (w->out, 1);
        put8 (w->out, ops[c->op]);
        put16 (w->out, f1->len);
        put16 (w->out, two ? f2->len : 0);
        put32 (w->out, flags);
        put16 (w->out, 0);
        if (font)
          {
            gsize start = w->out->len;
            gboolean ts_changed = (c->mask & (O42_FMT_ITALIC | O42_FMT_STRIKEOUT)) != 0;
            for (int k = 0; k < 64; k++) put8 (w->out, 0);          /* no font name */
            put32 (w->out, (c->mask & O42_FMT_SIZE) ? (guint32) c->fmt.size * 10 : 0xFFFFFFFFu);
            put32 (w->out, (c->fmt.italic && (c->mask & O42_FMT_ITALIC) ? 0x02 : 0) |
                           (c->fmt.strikeout && (c->mask & O42_FMT_STRIKEOUT) ? 0x80 : 0));
            put16 (w->out, (c->mask & O42_FMT_BOLD) ? (c->fmt.bold ? 700 : 400) : 0xFFFF);
            put16 (w->out, 0xFFFF);                                 /* sss */
            put8 (w->out, (c->mask & O42_FMT_UNDERLINE) ? (c->fmt.underline ? 1 : 0) : 0xFF);
            put8 (w->out, 0); put8 (w->out, 0); put8 (w->out, 0);
            put32 (w->out, (c->mask & O42_FMT_COLOUR) ? palette_index (w, c->fmt.colour) : 0xFFFFFFFFu);
            put32 (w->out, 0);
            put32 (w->out, ts_changed ? (((c->mask & O42_FMT_ITALIC) ? 0 : 0x02) | ((c->mask & O42_FMT_STRIKEOUT) ? 0 : 0x80)) : 0x82);
            put32 (w->out, 1);                                      /* sss not changed */
            put32 (w->out, (c->mask & O42_FMT_UNDERLINE) ? 0 : 1);
            put32 (w->out, (c->mask & O42_FMT_BOLD) ? 0 : 1);
            put32 (w->out, 0); put32 (w->out, 0); put32 (w->out, 0);
            put16 (w->out, 1);
            g_assert (w->out->len - start == 118);
          }
        if (border)
          {
            guint32 b1 = (c->fmt.border_left ? 1 : 0) | (c->fmt.border_right ? 1 << 4 : 0) |
                         (c->fmt.border_top ? 1 << 8 : 0) | (c->fmt.border_bottom ? 1 << 12 : 0);
            put32 (w->out, b1 | (0x40u << 16) | (0x40u << 23));
            put32 (w->out, 0x40 | (0x40 << 7));
          }
        if (pattern)
          {
            put16 (w->out, 1 << 10);
            put16 (w->out, palette_index (w, c->fmt.fill) | (0x41 << 7));
          }
        g_byte_array_append (w->out, f1->data, f1->len);
        if (two)
          g_byte_array_append (w->out, f2->data, f2->len);
        end_record (w);
        g_byte_array_unref (f1);
        g_byte_array_unref (f2);
      }
  }

  /* Validation rules: a DVAL header, then a DV per rule. */
  {
    GArray *rules = o42_sheet_validations (sheet);
    if (rules->len > 0)
      {
        begin_record (w, R_DVAL);
        put16 (w->out, 0); put32 (w->out, 0); put32 (w->out, 0);
        put32 (w->out, 0xFFFFFFFFu); put32 (w->out, rules->len);
        end_record (w);
        for (guint i = 0; i < rules->len; i++)
          {
            const O42Validation *v = &g_array_index (rules, O42Validation, i);
            guint32 flags = ((guint) v->kind & 0x0F) | (((guint) v->op & 0x0F) << 20) |
                            (v->allow_blank ? 0x100 : 0) | 0x200 | 0x80000;
            GByteArray *f1 = g_byte_array_new (), *f2 = g_byte_array_new ();
            O42Node *tree;

            if (v->kind == O42_VALID_LIST)
              {
                /* The list is one string with a NUL between entries. */
                const char *list = v->value ? v->value : "";
                glong n_chars = char_count (list);
                put8 (f1, 0x17);
                put8 (f1, MIN (n_chars, 255));
                if (is_latin1 (list))
                  {
                    put8 (f1, 0);
                    for (const char *q = list; *q && n_chars-- > 0; q = g_utf8_next_char (q))
                      put8 (f1, *q == ',' ? 0 : g_utf8_get_char (q));
                  }
                else
                  {
                    glong n = 0;
                    gunichar2 *u = g_utf8_to_utf16 (list, -1, NULL, &n, NULL);
                    put8 (f1, 1);
                    for (glong k = 0; k < n && k < 255; k++)
                      put16 (f1, u[k] == 0x2C ? 0 : u[k]);
                    g_free (u);
                  }
                flags |= 0x80;   /* the list is the string itself */
              }
            else
              {
                tree = o42_formula_parse (v->value && v->value[0] ? v->value : "0");
                compile (w, tree, f1, FALSE, index, NULL);
                o42_node_free (tree);
              }
            if (v->value2 != NULL && v->value2[0] != '\0')
              {
                tree = o42_formula_parse (v->value2);
                compile (w, tree, f2, FALSE, index, NULL);
                o42_node_free (tree);
              }

            begin_record (w, R_DV);
            put32 (w->out, flags);
            for (int k = 0; k < 3; k++)
              { put16 (w->out, 1); put8 (w->out, 0); put8 (w->out, 0); }   /* empty strings, as Excel writes them */
            if (v->message != NULL && v->message[0] != '\0')
              put_ustr16 (w->out, v->message);
            else
              { put16 (w->out, 1); put8 (w->out, 0); put8 (w->out, 0); }
            put16 (w->out, f1->len); put16 (w->out, 0);
            g_byte_array_append (w->out, f1->data, f1->len);
            put16 (w->out, f2->len); put16 (w->out, 0);
            g_byte_array_append (w->out, f2->data, f2->len);
            put16 (w->out, 1);
            put16 (w->out, v->range.row0); put16 (w->out, v->range.row1);
            put16 (w->out, v->range.col0); put16 (w->out, v->range.col1);
            end_record (w);
            g_byte_array_unref (f1);
            g_byte_array_unref (f2);
          }
      }
  }

  {
    O42Range filter;
    if (o42_sheet_get_autofilter (sheet, &filter))
      {
        begin_record (w, R_AUTOFILTERINFO);
        put16 (w->out, filter.col1 - filter.col0 + 1);
        end_record (w);
      }
  }

  begin_record (w, R_EOF);
  end_record (w);
}

static void
write_font (Writer *w, const O42Fmt *f)
{
  begin_record (w, R_FONT);
  put16 (w->out, f->size * 10);
  put16 (w->out, (f->italic ? 0x02 : 0) | (f->strikeout ? 0x08 : 0));
  put16 (w->out, f->colour != 0 ? palette_index (w, f->colour) : 0x7FFF);
  put16 (w->out, f->bold ? 700 : 400);
  put16 (w->out, 0);
  put8 (w->out, f->underline ? 1 : 0);
  put8 (w->out, 0); put8 (w->out, 0); put8 (w->out, 0);
  put_ustr8 (w->out, f->family ? f->family : "Arial");
  end_record (w);
}

static void
write_xf (Writer *w, const O42Fmt *f, gboolean style)
{
  guint align = 0;

  switch (f->halign)
    {
    case O42_HALIGN_LEFT: align = 1; break;
    case O42_HALIGN_CENTRE: align = 2; break;
    case O42_HALIGN_RIGHT: align = 3; break;
    default: break;
    }
  if (f->wrap) align |= 0x08;
  align |= (f->valign == O42_VALIGN_TOP ? 0 : f->valign == O42_VALIGN_MIDDLE ? 1 : 2) << 4;

  begin_record (w, R_XF);
  {
    guint font = font_index (w, f);
    /* Records 0-3 are the default font; custom font i is record 3+i,
     * and Excel counts past its missing font 4, so it is XF index 4+i. */
    put16 (w->out, font == 0 ? 0 : font + 4);
  }
  put16 (w->out, format_id (w, f));
  put16 (w->out, style ? 0xFFF5 : 0x0001);
  put8 (w->out, style ? 0x20 : align);
  put8 (w->out, style ? 0 : (f->rotation >= 0 ? f->rotation : 90 - f->rotation));
  put8 (w->out, style ? 0 : (f->indent & 0x0F));
  put8 (w->out, style ? 0x00 : 0xF8);
  {
    guint32 b1 = xls_border_code (f->border_style[O42_SIDE_LEFT]) | (xls_border_code (f->border_style[O42_SIDE_RIGHT]) << 4) |
                 (xls_border_code (f->border_style[O42_SIDE_TOP]) << 8) | (xls_border_code (f->border_style[O42_SIDE_BOTTOM]) << 12);
    if (f->border_left) b1 |= (guint32) xls_border_colour (w, f->border_colour[O42_SIDE_LEFT]) << 16;
    if (f->border_right) b1 |= (guint32) xls_border_colour (w, f->border_colour[O42_SIDE_RIGHT]) << 23;
    put32 (w->out, b1);
  }
  {
    guint32 b2 = 0;
    if (f->border_top) b2 |= xls_border_colour (w, f->border_colour[O42_SIDE_TOP]);
    if (f->border_bottom) b2 |= (guint32) xls_border_colour (w, f->border_colour[O42_SIDE_BOTTOM]) << 7;
    if (f->fill != O42_FILL_NONE) b2 |= 1u << 26;   /* solid */
    put32 (w->out, b2);
  }
  put16 (w->out, f->fill != O42_FILL_NONE ? (palette_index (w, f->fill) | (0x41 << 7)) : (0x40 | (0x41 << 7)));
  end_record (w);
}

gboolean
o42_xls_save (O42Book *book, GFile *file, GError **error)
{
  o42_xls_dropped_cells = 0;
  Writer w;
  int n_sheets = o42_book_n_sheets (book);
  GPtrArray *sheet_cells = g_ptr_array_new ();
  GArray *boundsheet_at = g_array_new (FALSE, FALSE, sizeof (gsize));
  GBytes *stream, *whole;
  gboolean ok;
  O42Fmt plain;

  memset (&w, 0, sizeof w);
  w.book = book;
  w.out = g_byte_array_new ();
  w.fonts = g_array_new (FALSE, FALSE, sizeof (O42Fmt));
  w.xfs = g_array_new (FALSE, FALSE, sizeof (O42Fmt));
  w.formats = g_ptr_array_new_with_free_func (g_free);
  w.sst = g_ptr_array_new_with_free_func (g_free);
  w.sst_idx = g_hash_table_new (g_str_hash, g_str_equal);
  w.sst_offsets = g_array_new (FALSE, FALSE, sizeof (gsize));
  w.palette = g_array_new (FALSE, FALSE, sizeof (guint32));
  w.addin_names = g_ptr_array_new_with_free_func (g_free);
  w.xti_spans = g_array_new (FALSE, FALSE, sizeof (guint16));
  w.images = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  w.image_formats = g_ptr_array_new ();
  w.shapes_per_sheet = g_array_new (FALSE, FALSE, sizeof (int));

  o42_fmt_init_default (&plain);
  font_index (&w, &plain);
  g_array_append_val (w.xfs, plain);   /* cell XF 0, the default */

  /* Gather every sheet's cells first: the globals must know the fonts,
   * formats, strings and add-in names before they are written. */
  for (int i = 0; i < n_sheets; i++)
    {
      Gather g;
      g.w = &w;
      g.sheet = o42_book_sheet (book, i);
      g.index = i;
      g.cells = g_array_new (FALSE, FALSE, sizeof (CellOut));
      g.table = o42_sheet_fmt_table (g.sheet);
      g.default_idx = o42_fmt_table_default (g.table);
      o42_sheet_foreach_cell (g.sheet, gather_cell, &g);
      g_array_sort (g.cells, compare_cells);
      g_ptr_array_add (sheet_cells, g.cells);
    }
  /* Custom number formats must be on record before the XFs name them. */
  for (guint i = 0; i < w.xfs->len; i++)
    format_id (&w, &g_array_index (w.xfs, O42Fmt, i));
  /* And the palette must hold the conditional formats' colours, which
   * are written after it. */
  for (int i = 0; i < n_sheets; i++)
    {
      GArray *conds = o42_sheet_conditions (o42_book_sheet (book, i));
      for (guint k = 0; k < conds->len; k++)
        {
          const O42Condition *c = &g_array_index (conds, O42Condition, k);
          if (c->mask & O42_FMT_COLOUR) palette_index (&w, c->fmt.colour);
          if ((c->mask & O42_FMT_FILL) && c->fmt.fill != O42_FILL_NONE) palette_index (&w, c->fmt.fill);
        }
    }

  /* Workbook globals */
  begin_record (&w, R_BOF);
  put16 (w.out, 0x0600); put16 (w.out, 0x0005); put16 (w.out, 0x0DBB); put16 (w.out, 0x07CC);
  put32 (w.out, 0x00000041); put32 (w.out, 0x00000006);
  end_record (&w);

  begin_record (&w, R_INTERFACEHDR); put16 (w.out, 0x04B0); end_record (&w);
  begin_record (&w, R_INTERFACEEND); end_record (&w);
  begin_record (&w, R_CODEPAGE); put16 (w.out, 0x04B0); end_record (&w);

  begin_record (&w, R_WINDOW1);
  put16 (w.out, 0x0168); put16 (w.out, 0x001E); put16 (w.out, 0x3A5C); put16 (w.out, 0x1C8F);
  put16 (w.out, 0x0038); put16 (w.out, 0); put16 (w.out, 0); put16 (w.out, 1); put16 (w.out, 0x0258);
  end_record (&w);

  /* Fonts: the default four times (index 4 is skipped by Excel), then
   * the rest. */
  for (int i = 0; i < 4; i++)
    write_font (&w, &g_array_index (w.fonts, O42Fmt, 0));
  for (guint i = 1; i < w.fonts->len; i++)
    write_font (&w, &g_array_index (w.fonts, O42Fmt, i));

  for (guint i = 0; i < w.formats->len; i++)
    {
      begin_record (&w, R_FORMAT);
      put16 (w.out, 164 + i);
      put_ustr16 (w.out, g_ptr_array_index (w.formats, i));
      end_record (&w);
    }

  /* XFs: 15 style entries, then the default cell XF at 15 and the
   * cells' own from 16. */
  for (int i = 0; i < 15; i++)
    write_xf (&w, &plain, TRUE);
  write_xf (&w, &plain, FALSE);
  for (guint i = 1; i < w.xfs->len; i++)
    write_xf (&w, &g_array_index (w.xfs, O42Fmt, i), FALSE);

  begin_record (&w, R_STYLE);
  put16 (w.out, 0x8000); put8 (w.out, 0); put8 (w.out, 0xFF);
  end_record (&w);

  if (w.palette->len > 0)
    {
      begin_record (&w, R_PALETTE);
      put16 (w.out, 56);
      for (guint i = 0; i < 56; i++)
        {
          guint32 c = i < w.palette->len ? g_array_index (w.palette, guint32, i) : PALETTE[i];
          put8 (w.out, (c >> 16) & 0xff); put8 (w.out, (c >> 8) & 0xff); put8 (w.out, c & 0xff); put8 (w.out, 0);
        }
      end_record (&w);
    }

  for (int i = 0; i < n_sheets; i++)
    {
      begin_record (&w, R_BOUNDSHEET);
      {
        gsize at = w.out->len;
        g_array_append_val (boundsheet_at, at);
      }
      put32 (w.out, 0);
      put8 (w.out, 0); put8 (w.out, 0);
      put_ustr8 (w.out, o42_sheet_get_name (o42_book_sheet (book, i)));
      end_record (&w);
    }

  /* The workbook's own sheets as a SUPBOOK, one XTI per sheet, and an
   * add-in SUPBOOK after it for functions Excel 97 lacked. */
  begin_record (&w, R_SUPBOOK);
  put16 (w.out, n_sheets); put16 (w.out, 0x0401);
  end_record (&w);
  if (w.addin_names->len > 0 || w.xti_spans->len > 0)
    {
      /* The add-in book keeps its place after the sheets even when
       * empty, so the XTIs of 3-D spans that follow it stay numbered. */
      begin_record (&w, R_SUPBOOK);
      put16 (w.out, 1); put16 (w.out, 0x3A01);
      end_record (&w);
      for (guint i = 0; i < w.addin_names->len; i++)
        {
          begin_record (&w, R_EXTERNNAME);
          put16 (w.out, 0); put32 (w.out, 0);
          put_ustr8 (w.out, g_ptr_array_index (w.addin_names, i));
          put16 (w.out, 2); put8 (w.out, 0x1C); put8 (w.out, 0x17);
          end_record (&w);
        }
    }
  begin_record (&w, R_EXTERNSHEET);
  {
    gboolean addin = w.addin_names->len > 0 || w.xti_spans->len > 0;
    put16 (w.out, n_sheets + (addin ? 1 : 0) + (int) w.xti_spans->len / 2);
    for (int i = 0; i < n_sheets; i++)
      { put16 (w.out, 0); put16 (w.out, i); put16 (w.out, i); }
    if (addin)
      { put16 (w.out, 1); put16 (w.out, 0xFFFE); put16 (w.out, 0xFFFE); }
    for (guint i = 0; i + 1 < w.xti_spans->len; i += 2)
      { put16 (w.out, 0); put16 (w.out, g_array_index (w.xti_spans, guint16, i)); put16 (w.out, g_array_index (w.xti_spans, guint16, i + 1)); }
  }
  end_record (&w);

  {
    GList *names = o42_book_names (book);
    for (GList *l = names; l != NULL; l = l->next)
      {
        O42Sheet *target;
        O42Range range;
        if (!o42_book_lookup_name (book, l->data, &target, &range))
          continue;
        begin_record (&w, R_NAME);
        put16 (w.out, 0); put8 (w.out, 0);
        put8 (w.out, MIN (char_count (l->data), 255));
        put16 (w.out, 11);
        put16 (w.out, 0); put16 (w.out, 0);
        put8 (w.out, 0); put8 (w.out, 0); put8 (w.out, 0); put8 (w.out, 0);
        put_ustr_body (w.out, l->data);
        put8 (w.out, 0x3B);
        put16 (w.out, o42_book_sheet_index (book, target));
        put16 (w.out, range.row0); put16 (w.out, range.row1);
        put16 (w.out, range.col0); put16 (w.out, range.col1);
        end_record (&w);
      }
    g_list_free (names);
    for (int i = 0; i < n_sheets; i++)
      {
        O42Range filter;
        if (!o42_sheet_get_autofilter (o42_book_sheet (book, i), &filter))
          continue;
        begin_record (&w, R_NAME);
        put16 (w.out, 0x0021); put8 (w.out, 0);
        put8 (w.out, 1);
        put16 (w.out, 11);
        put16 (w.out, i + 1); put16 (w.out, i + 1);
        put8 (w.out, 0); put8 (w.out, 0); put8 (w.out, 0); put8 (w.out, 0);
        put8 (w.out, 0); put8 (w.out, 0x0D);
        put8 (w.out, 0x3B);
        put16 (w.out, i);
        put16 (w.out, filter.row0); put16 (w.out, filter.row1);
        put16 (w.out, filter.col0); put16 (w.out, filter.col1);
        end_record (&w);
      }
  }

  /* The drawing group: every sheet's pictures in one store, and the
   * shape count per sheet for the id clusters. */
  {
    gboolean any = FALSE;
    for (int i = 0; i < n_sheets; i++)
      {
        O42Sheet *sheet = o42_book_sheet (book, i);
        GPtrArray *pictures = o42_sheet_pictures (sheet);
        int shapes = (int) pictures->len + (int) g_hash_table_size (o42_sheet_notes (sheet))
                     + (int) o42_sheet_charts (sheet)->len;
        for (guint k = 0; k < pictures->len; k++)
          {
            const O42Picture *pic = g_ptr_array_index (pictures, k);
            g_ptr_array_add (w.images, g_bytes_ref (pic->data));
            g_ptr_array_add (w.image_formats, (gpointer) (pic->format ? pic->format : "png"));
          }
        g_array_append_val (w.shapes_per_sheet, shapes);
        if (shapes > 0) any = TRUE;
      }
    if (any)
      {
        GBytes *group = o42_escher_group (w.images, w.image_formats, w.shapes_per_sheet);
        const guchar *g = g_bytes_get_data (group, NULL);
        gsize left = g_bytes_get_size (group), at = 0;
        gboolean first = TRUE;
        while (left > 0 || first)
          {
            gsize take = MIN (left, 8224);
            begin_record (&w, first ? R_MSODRAWINGGROUP : R_CONTINUE);
            g_byte_array_append (w.out, g + at, take);
            end_record (&w);
            at += take; left -= take; first = FALSE;
          }
        g_bytes_unref (group);
      }
  }

  /* The shared strings, continued into fresh records when one fills.
   * A string longer than a record is cut, which loses text past 8000
   * characters -- rare enough in a spreadsheet. */
  begin_record (&w, R_SST);
  put32 (w.out, w.sst->len);
  put32 (w.out, w.sst->len);
  for (guint i = 0; i < w.sst->len; i++)
    {
      const char *text = g_ptr_array_index (w.sst, i);
      glong n = MIN (char_count (text), 8000);
      gsize need = 3 + (is_latin1 (text) ? n : n * 2);
      if (w.out->len - w.record_start - 4 + need > 8224)
        {
          end_record (&w);
          begin_record (&w, R_CONTINUE);
        }
      {
        gsize at = w.out->len, rec = w.record_start;
        g_array_append_val (w.sst_offsets, at);
        g_array_append_val (w.sst_offsets, rec);
      }
      put16 (w.out, n);
      if (n == char_count (text))
        put_ustr_body (w.out, text);
      else
        {
          char *cut = g_utf8_substring (text, 0, n);
          put_ustr_body (w.out, cut);
          g_free (cut);
        }
    }
  end_record (&w);

  begin_record (&w, R_EXTSST);
  put16 (w.out, 8);
  for (guint i = 0; i < w.sst_offsets->len; i += 16)
    {
      gsize at = g_array_index (w.sst_offsets, gsize, i);
      gsize rec = g_array_index (w.sst_offsets, gsize, i + 1);
      put32 (w.out, at);
      put16 (w.out, at - rec);
      put16 (w.out, 0);
    }
  end_record (&w);

  begin_record (&w, R_EOF);
  end_record (&w);

  for (int i = 0; i < n_sheets; i++)
    {
      gsize at = g_array_index (boundsheet_at, gsize, i);
      guint32 pos = w.out->len;
      w.out->data[at] = pos & 0xff;
      w.out->data[at + 1] = (pos >> 8) & 0xff;
      w.out->data[at + 2] = (pos >> 16) & 0xff;
      w.out->data[at + 3] = (pos >> 24) & 0xff;
      write_sheet (&w, o42_book_sheet (book, i), i, g_ptr_array_index (sheet_cells, i));
    }

  stream = g_byte_array_free_to_bytes (w.out);
  {
    const char *names[1] = { "Workbook" };
    GBytes *contents[1] = { stream };
    whole = o42_ole2_build (names, contents, 1);
  }
  ok = g_file_replace_contents (file, g_bytes_get_data (whole, NULL), g_bytes_get_size (whole),
                                NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, error);
  g_bytes_unref (whole);
  g_bytes_unref (stream);

  for (guint i = 0; i < sheet_cells->len; i++)
    {
      GArray *cells = g_ptr_array_index (sheet_cells, i);
      for (guint k = 0; k < cells->len; k++)
        {
          CellOut *c = &g_array_index (cells, CellOut, k);
          o42_value_clear (&c->value);
          g_clear_pointer (&c->rgce, g_bytes_unref);
          g_clear_pointer (&c->rgcb, g_bytes_unref);
          g_clear_pointer (&c->array, g_bytes_unref);
        }
      g_array_unref (cells);
    }
  g_ptr_array_unref (sheet_cells);
  g_array_unref (boundsheet_at);
  g_array_unref (w.fonts);
  g_array_unref (w.xfs);
  g_ptr_array_unref (w.formats);
  g_hash_table_unref (w.sst_idx);
  g_ptr_array_unref (w.sst);
  g_array_unref (w.sst_offsets);
  g_array_unref (w.palette);
  g_ptr_array_unref (w.addin_names);
  g_array_unref (w.xti_spans);
  g_ptr_array_unref (w.images);
  g_ptr_array_unref (w.image_formats);
  g_array_unref (w.shapes_per_sheet);
  return ok;
}
