# SPDX-License-Identifier: GPL-3.0-or-later

o42_inc = include_directories('util', 'model', 'formula', 'io', 'script', 'ui')

# Everything below the user interface: values, formats, the formula parser
# and evaluator, and the sheet.  Its own library, linking without GTK, so
# that nothing graphical can reach into it.
o42_core_sources = files(
  'util/o42-pyquote.c',
  'util/o42-spell.c',
  'util/o42-types.c',
  'util/o42-date.c',
  'util/o42-hdate.c',
  'util/o42-numfmt.c',
  'util/o42-image.c',
  'model/o42-picture.c',
  'model/o42-chart.c',
  'model/o42-shape.c',
  'io/o42-pdf.c',
  'io/o42-csv.c',
  'io/o42-text-formats.c',
  'io/o42-lotus.c',
  'io/o42-sql.c',
  'io/o42-gnumeric.c',
  'io/o42-zip.c',
  'io/o42-xlsx.c',
  'io/o42-xlsx-draw.c',
  'io/o42-ods.c',
  'io/o42-html.c',
  'io/o42-ole2.c',
  'io/o42-xls.c',
  'io/o42-escher.c',
  'model/o42-value.c',
  'model/o42-fmt.c',
  'model/o42-analysis.c',
  'model/o42-pattern.c',
  'model/o42-richtext.c',
  'model/o42-sheet.c',
  'model/o42-book.c',
  'formula/o42-formula.c',
  'formula/o42-eval.c',
  'formula/o42-fn-bessel.c',
  'formula/o42-fn-random.c',
  'formula/o42-fn-info.c',
  'formula/o42-fn-finance.c',
  'formula/o42-fn-distributions.c',
  'formula/o42-fn-engineering.c',
  'formula/o42-fn-hdate.c',
  'formula/o42-fn-options.c',
)

o42_core = static_library('o42core',
  o42_core_sources,
  dependencies: core_deps,
  include_directories: o42_inc,
  install: false,
)

o42_core_dep = declare_dependency(
  link_with: o42_core,
  include_directories: o42_inc,
  dependencies: core_deps,
)

# Python, over the core: the office42 module's C half, and its Python
# half compiled in as a string.  Sees the model and the evaluator,
# never GTK; without -Dpython the same file compiles to stubs.
office42_py_h = custom_target('office42-py.h',
  input: 'script/office42.py',
  output: 'office42-py.h',
  command: [python_prog, files('script/embed.py'), '@INPUT@', '@OUTPUT@'],
)

o42_script = static_library('o42script',
  ['script/o42-python.c', office42_py_h],
  dependencies: python_dep.found() ? [o42_core_dep, python_dep] : [o42_core_dep],
  include_directories: o42_inc,
  install: false,
)

o42_script_dep = declare_dependency(
  link_with: o42_script,
  include_directories: o42_inc,
  dependencies: python_dep.found() ? [o42_core_dep, python_dep] : [o42_core_dep],
)

# The engine can be driven from a terminal.  It is how the engine was
# checked before there was a window to type into, and it stays useful
# for the same reason.
# Evaluation recurses along a chain of formulas, one frame per cell;
# Windows gives a program 1 MB of stack by default, which a few
# thousand cells exhaust.  Ask for what Linux gives.
o42_link_args = host_machine.system() == 'windows' ? ['-Wl,--stack,67108864'] : []

executable('office42-calc',
  'calc-main.c',
  dependencies: [o42_core_dep, o42_script_dep],
  link_args: o42_link_args,
  install: false,
)

o42_ui_sources = files(
  'ui/o42-grid.c',
  'ui/o42-window.c',
  'ui/o42-application.c',
  'main.c',
)

executable('office42',
  o42_ui_sources,
  o42_resources,
  dependencies: [o42_core_dep, o42_script_dep, cairo_dep, pango_dep, pangocairo_dep, gtk_dep],
  include_directories: o42_inc,
  link_args: o42_link_args,
  install: true,
  win_subsystem: 'windows',
)
