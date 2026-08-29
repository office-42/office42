# Translations

office42 speaks the machine's language where it has been translated and
English everywhere else. The messages live in `office42.pot` (the
template) and one `.po` file per language, named in `LINGUAS`;
`POTFILES` lists the files the strings are taken from.

## Refreshing the template

`meson compile -C builddir office42-pot` on a machine whose gettext
finds GTK's ITS rules. Where it does not -- MSYS2, for one -- the two
steps are:

```
xgettext --from-code=UTF-8 -k_ -kN_ -kC_:1c,2 --add-comments \
  -o po/office42.pot src/main.c src/ui/*.c
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
brings an existing translation up to date after the template changes.

## What is translated

The menu bar, the dialogs, the buttons, the toolbar's tooltips and the
messages in the status bar. The lists inside some drop-downs -- the
number categories, the border styles, the statistical tools -- are
still English in every language: they are arrays the widgets read
directly, and giving them the catalogue is the next piece of this work.

Not translated, and not to be: function names (`SUM` is `SUM` in every
Excel), the format language's codes, and the names of file formats.
