#!/bin/bash
gcc src/main.c src/piece-table.c src/document.c src/undo.c src/syntax.c src/editor-widget.c -o vite $(pkg-config --cflags --libs gtk4) -lm
