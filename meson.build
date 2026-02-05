project('Virtual Text Editor', 'c',
  version : '0.1',
  default_options : ['warning_level=3', 'c_std=gnu11'])

add_project_arguments('-D_FILE_OFFSET_BITS=64', language: 'c')

cc = meson.get_compiler('c')

gtk_dep = dependency('gtk4', version: '>= 4.0.0')
adw_dep = dependency('libadwaita-1', version: '>= 1.0.0')
m_dep = cc.find_library('m', required : false)

sources = files(
  'src/main.c',
  'src/piece-table.c',
  'src/resource-check.c',
  'src/document.c',
  'src/undo.c',
  'src/preferences.c',
  'src/syntax.c',
  'src/editor-actions.c',
  'src/editor-input.c',
  'src/editor-selection.c',
  'src/editor-renderer.c',
  'src/editor-scrolling.c',
  'src/editor-search.c',
  'src/editor-utils.c',
  'src/vite-clipboard.c',
  'src/editor-clipboard.c',
  'src/editor-widget.c',
  'src/find-replace-bar.c',

  'src/status-bar.c',
  'src/tab.c',
  'src/tab-bar.c',
  'src/fading-label.c',
  'src/compact-matches.c',
  'src/syntax-c.c',
  'src/syntax-python.c',
  'src/syntax-shell.c',
  'src/syntax-js.c',
  'src/syntax-yaml.c',
  'src/syntax-xml.c',
  'src/syntax-desktop.c',
  'src/syntax-rust.c',
  'src/editor-minimap.c'
)

executable('vite',
  sources,
  dependencies : [gtk_dep, adw_dep, m_dep],
  install : false)

# Reproduction Test (disabled - test file doesn't exist)
# test_sources = files(
#   'tests/reproduce_crash.c',
#   'src/piece-table.c',
#   'src/document.c',
#   'src/undo.c',
#   'src/preferences.c',
#   'src/syntax.c',
#   'src/editor-widget.c',
#   'src/find-replace-bar.c',
#   'src/tab.c',
#   'src/tab-bar.c',
#   'src/fading-label.c',
#   'src/compact-matches.c'
# )

# reproduce_crash_exe = executable('reproduce_crash',
#   test_sources,
#   dependencies : [gtk_dep, adw_dep, m_dep],
#   install : false)
