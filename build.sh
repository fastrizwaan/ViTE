#!/bin/bash
gcc src/main.c src/piece-table.c src/document.c src/undo.c src/preferences.c src/syntax.c src/editor-widget.c src/tab.c src/tab-bar.c -o vite $(pkg-config --cflags --libs gtk4 libadwaita-1) -lm
