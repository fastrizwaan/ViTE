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

# Update icon cache and desktop database
echo "Updating icon cache and desktop database..."
sudo gtk-update-icon-cache -f -t /usr/local/share/icons/hicolor
sudo update-desktop-database /usr/local/share/applications

echo "Installation complete!"
echo ""
echo "You can now:"
echo "- Run 'vite' from the command line"
echo "- Find 'Virtual Text Editor' in your applications menu"
echo ""
echo "To uninstall, you can run: sudo ninja -C builddir uninstall"