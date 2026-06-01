#!/bin/bash
cat << 'INNER_EOF' > /tmp/vite_test.c
#include <gtk/gtk.h>
#include <stdio.h>
int main() { printf("ok\n"); return 0; }
INNER_EOF
# We will just run the test_pt program!
