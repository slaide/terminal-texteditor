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
  - `Editor` struct (global) maintains tabs array, screen dimensions, file manager state, and modal dialog state
  - `Tab` struct stores per-tab state: buffer, cursor position, selection, scroll offsets, modification tracking, and file modification time for external change detection
  - Event loop uses `select()` with 50ms timeout for non-blocking input and window resize polling
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

- **lsp.c/h**: Language Server Protocol client
  - Spawns LSP server as subprocess with pipe-based communication
  - JSON-RPC 2.0 message framing (Content-Length headers)
  - Implements `textDocument/didOpen`, `textDocument/didChange`, `textDocument/didClose`
  - Receives `textDocument/publishDiagnostics` notifications asynchronously
  - Semantic tokens support for syntax highlighting
  - Non-blocking I/O integrated with main select() loop

- **lsp_config.c/h**: LSP server configuration
  - Loads config from `./lsp.conf`, `~/.config/texteditor/lsp.conf`
  - Maps file extensions to LSP server commands
  - Default: clangd for C/C++ files if no config found
  - Format: `extensions:command[:name]` (e.g., `.py:pylsp:Python`)

- **json.c/h**: Minimal JSON parser/builder
  - Supports objects, arrays, strings, numbers, booleans, null
  - `json_parse()` for parsing incoming LSP messages
  - `json_stringify()` for building outgoing messages
  - Helper functions: `json_object_get()`, `json_array_get()`, etc.

### Key Design Patterns

**Multi-Tab Architecture**: Each tab maintains independent state (buffer, cursor, selection, scroll position). Tab switching preserves per-tab state. The `get_current_tab()` helper provides safe access to the active tab.

**File Manager**: Sidebar component with its own focus state (`file_manager_focused`). Uses `current_directory` and `file_list` arrays. Tab key toggles focus between file manager and editor.

**Selection Handling**: The editor uses normalized selection coordinates (start always <= end) throughout the codebase. Selection state is tracked per-tab and affects rendering, cursor visibility, and text operations.

**Rendering Strategy**: Uses differential rendering - only redraws changed portions of the screen unless `needs_full_redraw` is set. Line numbers are consistently 6-digit padded with cyan coloring (defined via `STYLE_*` macros at top of main.c).

**Modal Dialogs**: The `draw_modal()` function renders centered dialogs with configurable colors. Used for quit confirmation and file reload prompts. Modal state tracked via `quit_confirmation_active` and `reload_confirmation_active` flags.

**External File Change Detection**: Each tab tracks `file_mtime`. The `check_file_changes()` function polls for modifications and triggers reload confirmation dialogs.

**Terminal Input Processing**: Complex escape sequence parsing in `terminal_read_key()` handles modifier combinations like Shift+Ctrl+Arrow keys. The enum `EditorKey` provides semantic key codes above 1000 to avoid conflicts with ASCII.

**Word Navigation**: Word boundaries are defined by `is_word_char()` function (alphanumeric + underscore). Word movement functions handle edge cases like line boundaries and whitespace.

**LSP Diagnostics**: For C/C++ files, the editor spawns clangd and receives diagnostic notifications. Diagnostics are stored per-tab in `LineDiagnostic` array. Line numbers are colored based on severity (red=error, yellow=warning, blue=info/hint). The status bar shows diagnostic messages when cursor is on an affected line. LSP fd is added to the main `select()` call for non-blocking message processing.

**Syntax Highlighting**: Uses LSP semantic tokens from clangd for accurate, context-aware highlighting of C/C++ code. Tokens are stored per-tab in `StoredToken` array. Colors: keywords/modifiers (magenta), types/classes (yellow), functions (blue), variables/parameters (cyan), strings/comments (green), numbers (red), macros (magenta). Tokens are requested on file open and after save.

## Implementation Notes

The editor requires C23 standard compilation (`-std=c23` or `-std=c2x` for older GCC) and uses POSIX APIs. The main event loop processes one key at a time and maintains editor state through the global `editor` variable. Status messages are displayed temporarily (3-second timeout) in the status line.

**LSP Support**: LSP servers are configured via `lsp.conf` file (see `lsp.conf.example`). If no config found, defaults to clangd for C/C++. LSP is initialized lazily when opening the first supported file. Supports any LSP server that communicates via stdio.

Mouse coordinates are adjusted for the 7-character line number width (6 digits + 1 space). Selection highlighting uses ANSI reverse video (`\033[7m`) and includes special handling for empty lines and newline characters.

Duplicate file open prevention: `find_tab_with_file()` uses `realpath()` to compare absolute paths when opening files.