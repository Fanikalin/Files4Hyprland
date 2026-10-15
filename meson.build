project('files4hyprland', 'c',
  version: '2.3.0',
  license: 'MIT',
  meson_version: '>= 1.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=release', 'b_lto=true'])

gtk = dependency('gtk4', version: '>= 4.12')
gio_unix = dependency('gio-unix-2.0')

executable('files4hyprland',
  'src/main.c', 'src/window.c', 'src/view.c', 'src/sidebar.c', 'src/dnd.c', 'src/util.c', 'src/theme.c', 'src/search.c', 'src/undo.c',
  dependencies: [gtk, gio_unix],
  c_args: ['-DF4H_VERSION="' + meson.project_version() + '"', '-Wno-unused-parameter'],
  install: true)

install_data('data/files4hyprland.desktop', install_dir: get_option('datadir') / 'applications')
install_data('bin/files4hyprland-setup', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
install_data('portal/files4hyprland-wrapper.sh', 'portal/config.in', 'portal/hyprland-portals.conf',
  install_dir: get_option('datadir') / 'files4hyprland' / 'portal')
install_data('hypr/bindings.lua', 'hypr/looknfeel.lua',
  install_dir: get_option('datadir') / 'files4hyprland' / 'hypr')

executable('undo_test',
  'tests/undo_test.c', 'src/window.c', 'src/view.c', 'src/sidebar.c', 'src/dnd.c', 'src/util.c', 'src/theme.c',
  'src/search.c', 'src/undo.c',
  dependencies: [gtk, gio_unix],
  c_args: ['-Wno-unused-parameter'],
  build_by_default: false,
  install: false)
