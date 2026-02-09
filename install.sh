#!/bin/bash

# ViTE Installation Script
# Builds and installs the Virtual Text Editor with desktop integration

set -e  # Exit on any error

echo "Building and installing ViTE..."

# Create build directory if it doesn't exist
if [ ! -d "builddir" ]; then
    echo "Setting up build directory..."
    meson setup builddir
fi

# Recompile the project
echo "Compiling ViTE..."
ninja -C builddir

# Install the application
echo "Installing ViTE..."
sudo ninja -C builddir install

echo "Installation complete!"
echo ""
echo "You can now:"
echo "- Run 'vite' from the command line"
echo "- Find 'Virtual Text Editor' in your applications menu"
echo ""
echo "To uninstall, you can run: sudo ninja -C builddir uninstall"