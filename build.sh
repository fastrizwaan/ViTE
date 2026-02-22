mkdir -p share/locale
for po in po/*.po; do
    lang=$(basename "$po" .po)
    mkdir -p "share/locale/$lang/LC_MESSAGES"
    msgfmt -o "share/locale/$lang/LC_MESSAGES/vite.mo" "$po"
done

gcc src/main.c src/piece-table.c src/resource-check.c src/document.c src/undo.c src/preferences.c src/syntax.c src/syntax-c.c src/syntax-python.c src/syntax-shell.c src/syntax-js.c src/syntax-yaml.c src/syntax-xml.c src/syntax-desktop.c src/syntax-rust.c src/syntax-markdown.c src/theme-manager.c src/editor-widget.c src/editor-utils.c src/editor-scrolling.c src/editor-input.c src/editor-renderer.c src/editor-print.c src/editor-selection.c src/editor-search.c src/editor-clipboard.c src/vite-clipboard.c src/editor-minimap.c src/editor-actions.c src/find-replace-bar.c src/tab.c src/tab-bar.c src/fading-label.c src/compact-matches.c src/status-bar.c -o vite -D_FILE_OFFSET_BITS=64 -DGETTEXT_PACKAGE=\"vite\" -DLOCALEDIR=\"$(pwd)/share/locale\" $(pkg-config --cflags --libs gtk4 libadwaita-1 json-glib-1.0) -lm
