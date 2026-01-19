# Copilot Instructions for ViTE

Welcome to the ViTE codebase! This document provides essential guidance for AI coding agents to be productive in this project. ViTE is a high-performance GTK4 text editor designed for instant editing of massive files. Below are the key aspects of the codebase and workflows to understand:

## Big Picture Architecture
- **Core Components:**
  - `src/piece-table.c` and `src/piece-table.h`: Implements the splay-tree piece-table, the core data structure for efficient text editing.
  - `src/editor-widget.c`: Manages the GTK4-based editor UI.
  - `src/syntax.c` and `src/syntax.h`: Handles syntax highlighting.
  - `src/undo.c` and `src/undo.h`: Provides undo/redo functionality.
- **Data Flow:**
  - The piece-table is the central data structure, interfacing with the editor widget for rendering and user interactions.
  - Syntax highlighting and undo/redo modules interact with the piece-table to provide their respective functionalities.

## Developer Workflows
- **Building the Project:**
  - Use the `meson` build system:
    ```bash
    meson setup build
    meson compile -C build
    ```
- **Running the Editor:**
  - Execute the `run.sh` script:
    ```bash
    ./run.sh
    ```
- **Testing:**
  - Tests are located in the `tests/` directory. Run them using:
    ```bash
    ./build.sh && ./tests/test_executable
    ```

## Project-Specific Conventions
- **File Organization:**
  - Source files are in `src/`.
  - Python scripts for auxiliary tasks are in `svite/`.
  - Flatpak-related files are in `flatpak/`.
- **Coding Style:**
  - Follows GTK4 and C conventions. Refer to existing files for examples.
- **Error Handling:**
  - Use `g_return_if_fail()` and `g_assert()` for validation.

## Integration Points
- **GTK4:**
  - The editor heavily relies on GTK4 for its UI. Familiarity with GTK4 is essential.
- **Python Utilities:**
  - Scripts in `svite/` provide additional functionality, such as syntax highlighting and testing.

## Key Files and Directories
- `src/`: Core C source files.
- `svite/`: Python scripts for auxiliary tasks.
- `flatpak/`: Files for Flatpak packaging.
- `tests/`: Contains test cases.

## Examples
- **Adding a New Feature:**
  - Modify the relevant `src/` file (e.g., `src/editor-widget.c` for UI changes).
  - Add tests in `tests/`.
- **Debugging:**
  - Use `gdb` for debugging C code.
  - Add debug prints with `g_print()`.

Feel free to update this document as the project evolves. Happy coding!