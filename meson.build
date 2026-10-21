project('files4hyprland', 'c',
  version: '2.5.4',
  license: 'MIT',
  meson_version: '>= 1.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=release', 'b_lto=true'])

gtk = dependency('gtk4', version: '>= 4.12')
gio_unix = dependency('gio-unix-2.0')
m = meson.get_compiler('c').find_library('m')

executable('files4hyprland',
  'src/main.c', 'src/window.c', 'src/view.c', 'src/sidebar.c', 'src/dnd.c', 'src/util.c', 'src/theme.c', 'src/search.c', 'src/undo.c', 'src/ext.c', 'src/thumb.c', 'src/net.c', 'src/fm1.c',
  dependencies: [gtk, gio_unix, m],
  c_args: ['-DF4H_VERSION="' + meson.project_version() + '"', '-Wno-unused-parameter'],
  install: true)

install_data('data/files4hyprland.desktop', install_dir: get_option('datadir') / 'applications')
install_data('bin/files4hyprland-setup', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
install_data('portal/files4hyprland-wrapper.sh', 'portal/config.in', 'portal/hyprland-portals.conf',
  install_dir: get_option('datadir') / 'files4hyprland' / 'portal')
configure_file(input: 'data/org.freedesktop.FileManager1.service.in', output: 'org.freedesktop.FileManager1.service',
  configuration: {'BINDIR': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'files4hyprland' / 'dbus')
install_data('hypr/bindings.lua', 'hypr/looknfeel.lua',
  install_dir: get_option('datadir') / 'files4hyprland' / 'hypr')

executable('undo_test',
  'tests/undo_test.c', 'src/window.c', 'src/view.c', 'src/sidebar.c', 'src/dnd.c', 'src/util.c', 'src/theme.c',
  'src/search.c', 'src/undo.c', 'src/ext.c', 'src/thumb.c', 'src/net.c',
  dependencies: [gtk, gio_unix, m],
  c_args: ['-Wno-unused-parameter'],
  build_by_default: false,
  install: false)
