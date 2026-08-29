# embed.py - turns office42.py into a C string
#
# Copyright (C) 2026 The office42 authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run by Meson: embed.py office42.py office42-py.h.  The module rides
# inside the executable, so there is nothing to install beside it.

import sys

source, target = sys.argv[1], sys.argv[2]
with open(source, encoding="utf-8") as f:
    lines = f.read().split("\n")

with open(target, "w", encoding="utf-8", newline="\n") as out:
    out.write("/* Generated from office42.py by embed.py; do not edit. */\n")
    out.write("#define OFFICE42_PY \\\n")
    for line in lines:
        escaped = line.replace("\\", "\\\\").replace('"', '\\"').replace("?", "\\?")
        out.write('  "%s\\n" \\\n' % escaped)
    out.write('  ""\n')
