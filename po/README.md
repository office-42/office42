# Translations

office42 speaks the machine's language where it has been translated and
English everywhere else. The messages live in `office42.pot` (the
template) and one `.po` file per language, named in `LINGUAS`;
`POTFILES` lists the files the strings are taken from.

| File | Language |
|---|---|
| `ar.po` | Arabic |
| `bn.po` | Bengali |
| `es.po` | Spanish |
| `fr.po` | French |
| `hi.po` | Hindi |
| `id.po` | Indonesian |
| `nb.po` | Norwegian Bokmål |
| `pt_BR.po` | Portuguese, as written in Brazil |
| `ru.po` | Russian |
| `ur.po` | Urdu |
| `zh_CN.po` | Chinese, simplified |

Besides Norwegian, they are the ten most spoken languages in the world
after English, counting those who speak them as a second language too.
Those ten were written with the help of a language model, following
the terms of each language's Excel 97; a native speaker's corrections
are welcome.

## Refreshing the template

`meson compile -C builddir office42-pot` on a machine whose gettext
finds GTK's ITS rules. Where it does not -- MSYS2, for one -- the two
steps are:

```
xgettext --from-code=UTF-8 -k_ -kN_ -kC_:1c,2 -kNC_:1c,2 --add-comments \
  -o po/office42.pot $(grep '\.c$' po/POTFILES)
xgettext --from-code=UTF-8 --join-existing --add-comments \
  --its=<prefix>/share/gettext/its/gtk4builder.its \
  -o po/office42.pot data/ui/menus.ui
```

The menus are a GtkBuilder file, which xgettext reads only with those
ITS rules; the two runs are otherwise the same.

## Starting a language

```
msginit -i po/office42.pot -o po/xx.po -l xx
```

and add `xx` to `LINGUAS`. `msgmerge --update po/xx.po po/office42.pot`
brings an existing translation up to date after the template changes,
and `msgfmt -c --check-accelerators=_ --statistics -o /dev/null po/xx.po`
says what is left and whether the placeholders and the `_` of each
mnemonic survived.

A language written right to left translates `default:LTR` as
`default:RTL`, and the menus, toolbars and dialogs turn round; the
sheet keeps column A on the left, as it does in Excel until a sheet is
told otherwise.

## What is translated

The menu bar and the right-click menus, the dialogs, the buttons, the
toolbar's tooltips, the messages in the status bar and in alerts, the
file chooser's filters, and the lists inside the drop-downs -- the
number categories, the border styles, the patterns, the conditions,
the statistical tools, the trendlines and the markers. Those lists are
marked with `N_()` where they stand and asked of the catalogue when
the widget is built, which is what `drop_down_of` is for; the program
reads a choice back by its place in the list, never by its text.

The one-line summary of each function, shown in Insert Function and
beside the names a formula offers while it is typed, is translated
too. The summaries live in the engine's help tables, marked with the
no-op `N_()`; the engine hands out English, which is what
`office42-calc --functions` prints, and the window translates.

Not translated, and not to be: function names (`SUM` is `SUM` in every
Excel) and their argument names, the format language's codes, the
names of file formats, and what goes into a book -- default names such
as Sheet1, Table1 and Macro1, a new control's caption, a header's font
codes -- so that a file reads the same wherever it was made.
