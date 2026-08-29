# Contributing to office42

## Licence

office42 is GPL-3.0-or-later. By contributing you agree your work is licensed
the same way. Put an SPDX line at the top of every new file:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */
```

Do not paste in code from other projects, Gnumeric included. office42 studies
Gnumeric's design and shares none of its code; Gnumeric is GPL-2.0, which is
not compatible with GPL-3 in that direction in any case.

## Before you send a patch

```sh
meson setup builddir --werror
meson compile -C builddir
```

Both must be clean, with no warnings.

This project does not carry a unit-test suite. Exercise your change through
`office42-calc` — it reads `A1 = ...` lines from standard input and prints
what cells come to — or through the running program once there is one, and
say in the commit message what you did to check.

## Style

GNU/GTK style, because that is what the libraries underneath look like:

- two-space indent, no tabs;
- return type on its own line in a function definition;
- a space before the parenthesis of a call, as in `g_free (thing)`;
- braces on their own line, indented with the block they open;
- `o42_` on public functions, `O42` on types, `O42_` on macros;
- 79 columns, where holding to it does not hurt readability.

**Comments say why, not what.** The comment on `MOD` explaining that it takes
the sign of the divisor, unlike C's `fmod`, is worth more than a comment that
says "compute the modulus".

**Call things what a spreadsheet calls them.** A cell has a *precedent*, not
a dependency; a formula has an *operand*, a value has a *type*, a format has
*decimals*. Matching the vocabulary of the domain is worth more than matching
anyone's house style.

## Layering

`formula/` must not include `model/`. The evaluator sees a callback, not a
sheet. `model/` and `formula/` must not include GTK. Those two rules are the
whole architecture; keep them. `script/` (Python) sits above both and below
`ui/`: it includes the model and the evaluator, never GTK, and is compiled to
stubs without `-Dpython`.

## Adding a function

Write `fn_yourname` in `o42-eval.c` following the ones around it, then add a
row to `FUNCTIONS` **in sorted position** — the lookup is a binary search and
an out-of-order entry is one that can never be found. Use `ARG_NUMBER` and
`ARG_TEXT` for scalar arguments and `visit_numbers` for anything that walks a
range, so that your function gets the literal-versus-range distinction right
for free.

## Commit messages

A one-line summary in the imperative, a blank line, then prose saying why the
change is right. If the change is subtle, the commit message is where the
reasoning belongs.
