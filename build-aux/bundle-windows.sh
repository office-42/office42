#!/usr/bin/env bash
# bundle-windows.sh - gather office42.exe and everything it needs to run
# on a Windows machine without MSYS2, from an MSYS2 MINGW64 shell.
#
# Copyright (C) 2026 The office42 authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
#     build-aux/bundle-windows.sh [builddir] [dist]
#
# leaves a runnable tree in dist/ (dist/bin/office42.exe and
# dist/bin/office42-calc.exe beside it, lib/ and share/ above), which the
# CI zips and pack-msix.sh turns into a Store package.  The DLLs come from
# ldd, followed to a fixed point so that the pixbuf loaders' own
# dependencies come too.  Python's standard library rides in
# lib/python3.X, where the interpreter looks for it beside its DLL.

# No -e: an optional file missing from the image must not stop the bundle.
set -uo pipefail

builddir=${1:-builddir}
dist=${2:-dist}
prefix=${MINGW_PREFIX:-/mingw64}

rm -rf "$dist"
mkdir -p "$dist/bin" "$dist/lib" "$dist/share"

cp "$builddir/src/office42.exe" "$dist/bin/" || { echo "no office42.exe in $builddir/src" >&2; exit 1; }
cp "$builddir/src/office42-calc.exe" "$dist/bin/" 2>/dev/null || true

# The GDK pixbuf loaders for the everyday formats, and a cache naming just
# those, with paths relative to the bundle as MSYS2's own are.  The SVG
# loader is not optional here: the toolbar icons are SVG.  The AVIF, HEIF,
# JPEG XL and TIFF loaders are left out: each drags in a video codec the
# size of everything else put together.
loaders="$dist/lib/gdk-pixbuf-2.0/2.10.0/loaders"
mkdir -p "$loaders"
for l in png jpeg gif bmp webp ico; do
  f="$prefix/lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-$l.dll"
  [ -f "$f" ] && cp "$f" "$loaders/"
done
if [ -f "$prefix/lib/gdk-pixbuf-2.0/2.10.0/loaders/pixbufloader_svg.dll" ]; then
  cp "$prefix/lib/gdk-pixbuf-2.0/2.10.0/loaders/pixbufloader_svg.dll" "$loaders/"
else
  echo "bundle-windows: no SVG pixbuf loader (pacman -S mingw-w64-x86_64-librsvg); the toolbar would be blank" >&2
  exit 1
fi
( cd "$dist" && gdk-pixbuf-query-loaders lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll ) \
  | sed 's#^"[^"]*/lib/gdk-pixbuf-2.0/#"lib/gdk-pixbuf-2.0/#' > "$dist/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" || true

# Hunspell's English dictionaries, which the speller falls back to when
# the machine's own language has none; o42-spell.c looks in the bundle's
# share/hunspell first.
if [ -d "$prefix/share/hunspell" ]; then
  mkdir -p "$dist/share/hunspell"
  cp "$prefix"/share/hunspell/en_US.* "$prefix"/share/hunspell/en_GB.* "$dist/share/hunspell/" 2>/dev/null || true
fi

# Poppler's encoding tables, for PDF import of documents in CJK encodings.
[ -d "$prefix/share/poppler" ] && cp -r "$prefix/share/poppler" "$dist/share/" || true

# Python's standard library, without the tests, the IDE, the Tk bindings
# and whatever pip has put in site-packages: 18 MB instead of 250.
pylib=$(ls -d "$prefix"/lib/python3.* 2>/dev/null | head -1)
if [ -n "$pylib" ] && [ -f "$dist/bin/office42.exe" ] && ldd "$dist/bin/office42.exe" | grep -qi libpython; then
  mkdir -p "$dist/lib/$(basename "$pylib")"
  ( cd "$pylib" && tar cf - \
      --exclude=__pycache__ --exclude=test --exclude=tests --exclude=idlelib \
      --exclude=tkinter --exclude=turtledemo --exclude=ensurepip \
      --exclude=site-packages --exclude=pydoc_data --exclude='*.dll.a' . ) \
    | ( cd "$dist/lib/$(basename "$pylib")" && tar xf - )
fi

# GLib schemas (GTK needs its own), compiled.
mkdir -p "$dist/share/glib-2.0/schemas"
cp "$prefix"/share/glib-2.0/schemas/org.gtk.gtk4.Settings.*.xml "$dist/share/glib-2.0/schemas/" 2>/dev/null || true
cp "$prefix"/share/glib-2.0/schemas/gschema.dtd "$dist/share/glib-2.0/schemas/" 2>/dev/null || true
glib-compile-schemas "$dist/share/glib-2.0/schemas" || true

# Icons GTK's own widgets ask for (dialogs, spinners); office42's own ride
# inside the binary as a resource.
mkdir -p "$dist/share/icons"
cp -r "$prefix/share/icons/hicolor" "$dist/share/icons/" 2>/dev/null || true
if [ -d "$prefix/share/icons/Adwaita" ]; then
  mkdir -p "$dist/share/icons/Adwaita"
  cp "$prefix/share/icons/Adwaita/index.theme" "$dist/share/icons/Adwaita/" 2>/dev/null || true
  for sub in scalable symbolic 16x16 24x24 32x32; do
    [ -d "$prefix/share/icons/Adwaita/$sub" ] && cp -r "$prefix/share/icons/Adwaita/$sub" "$dist/share/icons/Adwaita/" || true
  done
fi
gtk4-update-icon-cache -q -t -f "$dist/share/icons/Adwaita" 2>/dev/null || true

# The message catalogues, if the build made any.
if [ -d "$builddir/po" ]; then
  for mo in "$builddir"/po/*.gmo "$builddir"/po/*/LC_MESSAGES/*.mo; do
    [ -f "$mo" ] || continue
    case $mo in
      *.gmo) lang=$(basename "$mo" .gmo) ;;
      *) lang=$(basename "$(dirname "$(dirname "$mo")")") ;;
    esac
    mkdir -p "$dist/share/locale/$lang/LC_MESSAGES"
    cp "$mo" "$dist/share/locale/$lang/LC_MESSAGES/office42.mo"
  done
fi

# Fontconfig and the GTK settings that make it look like Windows.
[ -d "$prefix/etc/fonts" ] && { mkdir -p "$dist/etc"; cp -r "$prefix/etc/fonts" "$dist/etc/"; } || true
mkdir -p "$dist/etc/gtk-4.0"
cat > "$dist/etc/gtk-4.0/settings.ini" <<'EOF'
[Settings]
gtk-font-name=Segoe UI 9
EOF

# The DLLs: everything ldd finds under the prefix, for the exes and for
# every DLL copied so far, until nothing new turns up.
copy_deps () {
  local target=$1
  ldd "$target" 2>/dev/null | awk '{print $3}' | grep -i "^$prefix/" | while read -r dll; do
    local base
    base=$(basename "$dll")
    if [ ! -f "$dist/bin/$base" ]; then
      cp "$dll" "$dist/bin/"
      echo "$dist/bin/$base"
    fi
  done
}

queue=("$dist/bin/office42.exe")
[ -f "$dist/bin/office42-calc.exe" ] && queue+=("$dist/bin/office42-calc.exe")
for f in "$dist"/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll "$dist"/lib/python3.*/lib-dynload/*.pyd; do
  [ -f "$f" ] && queue+=("$f")
done
while [ ${#queue[@]} -gt 0 ]; do
  next=()
  for f in "${queue[@]}"; do
    while read -r added; do
      [ -n "$added" ] && next+=("$added")
    done < <(copy_deps "$f")
  done
  queue=("${next[@]}")
done

# Not a dependency ldd sees: GLib loads it for TLS, GTK for input methods.
for extra in gspawn-win64-helper.exe gspawn-win64-helper-console.exe; do
  [ -f "$prefix/bin/$extra" ] && cp "$prefix/bin/$extra" "$dist/bin/" || true
done

echo "bundled $(ls "$dist/bin" | wc -l) files into $dist/bin ($(du -sh "$dist" | cut -f1) in all)"
[ -f "$dist/bin/libgtk-4-1.dll" ] || { echo "GTK's DLL was not found by ldd; the bundle would not run" >&2; exit 1; }
