#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The smoke test the three CI jobs run, and the one a person can run by
# hand after a build:
#
#     sh build-aux/smoke-test.sh builddir/src/office42-calc
#
# The terminal front-end needs no display, so the engine can be
# exercised end to end on a runner with no X, no Wayland and no Quartz.
# Nothing here touches GTK.  It is a smoke test, not a test suite: it
# asks of each part only that it still answers, and CLAUDE.md is what
# keeps a unit test from growing here.

set -eu

calc=${1:-builddir/src/office42-calc}
[ -x "$calc" ] || calc="$calc.exe"
if [ ! -x "$calc" ]; then
  echo "smoke-test: no such program: ${1:-builddir/src/office42-calc}" >&2
  exit 2
fi
# A relative path stops meaning the program once we move to the scratch
# directory below.
case "$calc" in
  /* | ?:[/\\]*) ;;
  *) calc="$PWD/$calc" ;;
esac

tab=$(printf '\t')
# BSD mktemp wants the template spelled out, so both sides get one.
work=$(mktemp -d "${TMPDIR:-/tmp}/office42-smoke.XXXXXX")
trap 'rm -rf "$work"' EXIT
cd "$work"

failed=0
out=out.txt

# run NAME < the script on stdin; leaves the output in $out.
run () {
  echo "== $1"
  shift
  status=0
  printf '%s' "$1" | "$calc" >"$out" 2>&1 || status=$?
  if [ "$status" -ne 0 ]; then
    echo "   FAIL: office42-calc exited $status" >&2
    cat "$out" >&2
    failed=1
    return 0
  fi
  sed 's/^/   | /' "$out"
}

# want PATTERN - the last run must have printed it.
want () {
  if ! grep -q "$1" "$out"; then
    echo "   FAIL: expected /$1/" >&2
    failed=1
  fi
}

run 'formulas' 'A1 = 10
A2 = 20
B1 = =SUM(A1:A2)*2
B1
C1 = =SUM(A:A)
C1
D1 = =IF(B1>50,"big","small")
D1
E1 = =TEXT(A1,"0.00")&"|"&UPPER("ok")
E1
'
want "^B1${tab}60${tab}"
want "^C1${tab}30${tab}"
want "^D1${tab}big${tab}"
want "^E1${tab}10\.00|OK${tab}"

# A change must travel the dependency graph, and the undo stack must
# take it back and put it again.
run 'recalculate, undo, redo' 'A1 = 1
A2 = 2
B1 = =SUM(A1:A2)
B1
A1 = 100
B1
undo
B1
redo
B1
'
want "^B1${tab}3${tab}"
want "^B1${tab}102${tab}"

run 'names, sort, find and replace' 'A1 = 3
A2 = 1
A3 = 2
B1 = =SUM(A1:A3)
name total B1
C1 = =total
C1
sort A1:A3 A
dump
D1 = pear
D2 = pear tree
replace pear -> plum
D1
D2
'
want "^C1${tab}6${tab}"
want "^D1${tab}plum"
want "^D2${tab}plum tree"

# The formats the program exists to read and write.  Each is written,
# read back in a fresh book, and asked for the formula it was given --
# save for CSV, which by its nature keeps only the value.
echo "== files"
for fmt in gnumeric xlsx xls ods csv; do
  printf 'A1 = 10\nA2 = 20\nB1 = =SUM(A1:A2)*2\nchart bar A1:A2\nsave t.%s\n' "$fmt" \
    | "$calc" >"$out" 2>&1 || { echo "   FAIL: writing .$fmt" >&2; cat "$out" >&2; failed=1; continue; }
  printf 'load t.%s\nB1\n' "$fmt" | "$calc" >"$out" 2>&1 \
    || { echo "   FAIL: reading .$fmt" >&2; cat "$out" >&2; failed=1; continue; }
  sed "s/^/   $fmt | /" "$out"
  want "^B1${tab}60"
  case $fmt in
    csv) ;;
    *) want "(=SUM(A1:A2)\*2)" ;;
  esac
done

run 'Python' 'py print(1+1)
A1 = 5
py import office42; office42.sheet["A2"].formula = "=A1*2"
A2
py import office42; print("py sees", office42.sheet["A2"].value)
'
want '^2$'
want "^A2${tab}10${tab}"
want '^py sees 10'

run 'the embedded database' 'dbembed
sqlprint SELECT 6*7
'
want '^42$'

# The three options, and a fourth that is not one.
echo "== the command line"
"$calc" --version </dev/null | grep -q '^office42-calc ' \
  || { echo "   FAIL: --version" >&2; failed=1; }
"$calc" --help </dev/null | grep -q '^usage: office42-calc ' \
  || { echo "   FAIL: --help" >&2; failed=1; }
# wc pads its count with blanks on the BSD side, which "test -gt" would
# rather not see.
n=$("$calc" --functions </dev/null | wc -l | tr -d ' ')
echo "   | $n functions"
[ "$n" -gt 400 ] || { echo "   FAIL: only $n functions" >&2; failed=1; }
if "$calc" --nonsense </dev/null 2>/dev/null; then
  echo "   FAIL: --nonsense was accepted" >&2
  failed=1
fi

if [ "$failed" -eq 0 ]; then
  echo "smoke-test: all good"
else
  echo "smoke-test: FAILED" >&2
fi
exit "$failed"
