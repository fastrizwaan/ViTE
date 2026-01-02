project('Virtual Text Editor', 'c',
  version : '0.1',
  default_options : ['warning_level=3', 'c_std=gnu11'])

cc = meson.get_compiler('c')

gtk_dep = dependency('gtk4', version: '>= 4.0.0')
adw_dep = dependency('libadwaita-1', version: '>= 1.0.0')
m_dep = cc.find_library('m', required : false)

sources = files(
  'src/main.c',
  'src/piece-table.c',
  'src/document.c',
  'src/undo.c',
  'src/syntax.c',
  'src/editor-widget.c',
)

executable('vite',
  sources,
  dependencies : [gtk_dep, adw_dep, m_dep],
  install : false)
