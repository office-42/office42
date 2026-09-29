# embed.py - turns office42.py into a C string
#
# Copyright (C) 2026 The office42 authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run by Meson: embed.py office42.py office42-py.h [MACRO].  The module
# rides inside the executable, so there is nothing to install beside it;
# MACRO names the string, OFFICE42_PY unless said.

import sys

source, target = sys.argv[1], sys.argv[2]
macro = sys.argv[3] if len(sys.argv) > 3 else "OFFICE42_PY"
with open(source, encoding="utf-8") as f:
    lines = f.read().split("\n")

with open(target, "w", encoding="utf-8", newline="\n") as out:
    out.write("/* Generated from %s by embed.py; do not edit. */\n" % source.replace("\\", "/").split("/")[-1])
    out.write("#define %s \\\n" % macro)
    for line in lines:
        escaped = line.replace("\\", "\\\\").replace('"', '\\"').replace("?", "\\?")
        out.write('  "%s\\n" \\\n' % escaped)
    out.write('  ""\n')
