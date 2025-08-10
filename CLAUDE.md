# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Build the editor
make

# Clean build artifacts
make clean

# Install to /usr/local/bin
make install

# Run the editor
./texteditor [filename]
```

## Architecture Overview

This is a terminal-based text editor written in C23 with a modular architecture:

### Core Components

- **main.c**: Central editor state and event loop
  - `Editor` struct maintains cursor position, selection state, screen dimensions, and buffer reference
  - Event loop handles keyboard input, mouse events, and screen rendering
  - Implements differential rendering to prevent screen flickering

- **terminal.c/h**: Low-level terminal interface
  - Raw mode terminal initialization with `termios`
  - ANSI escape sequence parsing for special keys (arrows, Ctrl combinations, Shift modifiers)
  - Mouse event handling and cursor visibility control
  - Window size detection

- **buffer.c/h**: Text buffer management
  - Dynamic array of lines (`char **lines`) with capacity management
  - File I/O operations for loading and saving
  - Text manipulation functions for insertion, deletion, and line operations
  - Text range extraction for clipboard operations

- **clipboard.c/h**: System clipboard integration
  - Cross-platform clipboard support via `popen()` calls to system utilities
  - Supports xclip/xsel (Linux) and pbcopy/pbpaste (macOS)

### Key Design Patterns

**Selection Handling**: The editor uses normalized selection coordinates (start always <= end) throughout the codebase. Selection state is tracked in the main `Editor` struct and affects rendering, cursor visibility, and text operations.

**Rendering Strategy**: Uses differential rendering - only redraws changed portions of the screen unless `needs_full_redraw` is set. Line numbers are consistently 6-digit padded with cyan coloring.

**Terminal Input Processing**: Complex escape sequence parsing in `terminal_read_key()` handles modifier combinations like Shift+Ctrl+Arrow keys. The enum `EditorKey` provides semantic key codes above 1000 to avoid conflicts with ASCII.

**Word Navigation**: Word boundaries are defined by `is_word_char()` function (alphanumeric + underscore). Word movement functions handle edge cases like line boundaries and whitespace.

## Implementation Notes

The editor requires C23 standard compilation (`-std=c23`) and uses POSIX APIs. The main event loop processes one key at a time and maintains editor state through the global `editor` variable. Status messages are displayed temporarily (3-second timeout) in the status line.

Mouse coordinates are adjusted for the 7-character line number width (6 digits + 1 space). Selection highlighting uses ANSI reverse video (`\033[7m`) and includes special handling for empty lines and newline characters.