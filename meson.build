# office42 - a spreadsheet in the shape of Excel 5
# Copyright (C) 2026 The office42 authors
# SPDX-License-Identifier: GPL-3.0-or-later

project('office42', 'c',
  version: '0.9.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 1.0',
  default_options: [
    'c_std=gnu11,c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')
gnome = import('gnome')
i18n = import('i18n')

add_project_arguments(
  '-DO42_VERSION="' + meson.project_version() + '"',
  '-DG_LOG_DOMAIN="office42"',
  '-DGETTEXT_PACKAGE="office42"',
  '-DO42_LOCALEDIR="' + get_option('prefix') / get_option('localedir') + '"',
  language: 'c',
)

extra_warnings = [
  '-Wmissing-prototypes',
  '-Wstrict-prototypes',
  '-Wold-style-definition',
  '-Wpointer-arith',
  '-Wshadow',
  '-Wwrite-strings',
  '-Wformat=2',
  '-Wundef',
  '-Wvla',
  '-Winit-self',
  '-Wno-unused-parameter',
]
add_project_arguments(cc.get_supported_arguments(extra_warnings), language: 'c')

glib_dep       = dependency('glib-2.0',    version: '>= 2.76')
gobject_dep    = dependency('gobject-2.0', version: '>= 2.76')
gio_dep        = dependency('gio-2.0',     version: '>= 2.76')
cairo_dep      = dependency('cairo',       version: '>= 1.16')
pango_dep      = dependency('pango',       version: '>= 1.50')
pangocairo_dep = dependency('pangocairo',  version: '>= 1.50')
gtk_dep        = dependency('gtk4',        version: '>= 4.10')
pixbuf_dep     = dependency('gdk-pixbuf-2.0', version: '>= 2.42')
m_dep          = cc.find_library('m', required: false)

# Reading PDF needs poppler.  Writing it does not -- cairo does that -- so a
# build without poppler still exports PDF and simply cannot import one.
poppler_dep = dependency('poppler-glib', version: '>= 20.0', required: false)
if poppler_dep.found()
  add_project_arguments('-DHAVE_POPPLER=1', language: 'c')
endif

# Python is embedded when asked for (or found): Tools > Python Console,
# =PY() in cells and functions defined by scripts.
python_dep = dependency('python3-embed', required: get_option('python'))
if python_dep.found()
  add_project_arguments('-DHAVE_PYTHON=1', language: 'c')
endif
python_prog = find_program('python3', 'python', required: true)

# Spelling is Hunspell's, which is what everything else on the machine
# spells with.  Without it, and without a dictionary for the language,
# Tools > Spelling says so and every word passes.
hunspell_dep = dependency('hunspell', required: get_option('spell'))
if hunspell_dep.found()
  add_project_arguments('-DHAVE_HUNSPELL=1', language: 'c')
endif

# A database beside the sheet is SQLite's: one file, no server, and
# already on nearly every machine.  Without it Data > Database says so.
sqlite_dep = dependency('sqlite3', version: '>= 3.24', required: get_option('sqlite'))
if sqlite_dep.found()
  add_project_arguments('-DHAVE_SQLITE=1', language: 'c')
endif

# The core draws pictures and writes PDF, so it needs cairo, pango and
# gdk-pixbuf.  It still does not need GTK, and must not.
core_deps = [glib_dep, gobject_dep, gio_dep, cairo_dep, pango_dep,
             pangocairo_dep, pixbuf_dep, m_dep]
if poppler_dep.found()
  core_deps += [poppler_dep]
endif
if hunspell_dep.found()
  core_deps += [hunspell_dep]
endif
if sqlite_dep.found()
  core_deps += [sqlite_dep]
endif
app_deps  = core_deps + [gtk_dep]

subdir('data')
subdir('po')
subdir('src')

summary({
    'prefix' : get_option('prefix'),
    'GTK'    : gtk_dep.version(),
    'Pango'  : pango_dep.version(),
    'PDF import (poppler)' : poppler_dep.found(),
    'Python' : python_dep.found() ? python_dep.version() : false,
    'SQLite' : sqlite_dep.found() ? sqlite_dep.version() : false,
  }, section: 'office42 ' + meson.project_version())
