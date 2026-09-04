# SPDX-License-Identifier: GPL-3.0-or-later

o42_resources = gnome.compile_resources(
  'o42-resources',
  'office42.gresource.xml',
  source_dir: meson.current_source_dir(),
  c_name: 'o42',
)

# The desktop entry and application icon, for the platforms that have
# such things; on Windows and macOS the icon rides inside the binary's
# resources.
install_data('net.office42.office42.desktop',
  install_dir: get_option('datadir') / 'applications')
install_data('icons/scalable/apps/net.office42.office42.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
install_data('icons/128x128/apps/net.office42.office42.png',
  install_dir: get_option('datadir') / 'icons/hicolor/128x128/apps')

# So that the menu and the icon theme learn of the program on install,
# where there is a menu to tell: Windows and macOS keep their own, and a
# machine without the two tools (a build box) is not held up over them.
if host_machine.system() not in ['windows', 'darwin']
  gnome.post_install(
    gtk_update_icon_cache: find_program('gtk-update-icon-cache', 'gtk4-update-icon-cache', required: false).found(),
    update_desktop_database: find_program('update-desktop-database', required: false).found())
endif
