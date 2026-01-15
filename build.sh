#!/bin/bash
gcc src/main.c src/piece-table.c src/document.c src/undo.c src/preferences.c src/syntax.c src/editor-widget.c src/find-replace-bar.c src/tab.c src/tab-bar.c src/fading-label.c src/compact-matches.c -o vite -D_FILE_OFFSET_BITS=64 $(pkg-config --cflags --libs gtk4 libadwaita-1) -lm
