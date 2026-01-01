project('Virtual Text Editor', 'c',
  version : '0.1',
  default_options : ['warning_level=3', 'c_std=gnu11'])

gtk_dep = dependency('gtk4', version: '>= 4.0.0')

  'src/main.c',
  'src/piece-table.c',
  'src/document.c',
  'src/undo.c',
  'src/syntax.c',
  'src/editor-widget.c',
)

executable('vite',
  sources,
  dependencies : [gtk_dep],
  install : false)
