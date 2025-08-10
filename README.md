# Terminal Text Editor

A modern terminal-based text editor written in C23 with mouse support and familiar keyboard shortcuts.

## Features

- **File Operations**: Open, edit, and save files
- **Line Numbers**: 6-digit padded line numbers with syntax highlighting
- **Mouse Support**: Click to position cursor, drag to select text with auto-scroll
- **Modern Shortcuts**: 
  - `Ctrl+S` - Save file
  - `Ctrl+C` - Copy selected text
  - `Ctrl+X` - Cut selected text  
  - `Ctrl+V` - Paste from clipboard
  - `Ctrl+A` - Select all text
  - `Ctrl+F` - Find text with real-time search
  - `Ctrl+Q` - Quit editor
- **Navigation**:
  - Arrow keys for cursor movement
  - `Ctrl+Left`/`Ctrl+Right` for word-by-word navigation
  - `Home`/`End` keys for line navigation
  - `Page Up`/`Page Down` for scrolling
- **Text Selection**: Select text with mouse or keyboard (Shift+Arrow keys)
- **Clipboard Integration**: Works with system clipboard (xclip/xsel/pbcopy)
- **Status Messages**: Visual feedback for save operations and clipboard actions

## Building

```bash
make
```

## Usage

```bash
# Open a file
./texteditor filename.txt

# Create a new file
./texteditor
```

## Dependencies

For clipboard functionality, install one of:
- `xclip` (Linux/X11)
- `xsel` (Linux/X11) 
- `pbcopy`/`pbpaste` (macOS)

## Controls

- **Ctrl+Q**: Quit
- **Ctrl+S**: Save file
- **Ctrl+C**: Copy selection
- **Ctrl+X**: Cut selection
- **Ctrl+V**: Paste
- **Ctrl+A**: Select all
- **Ctrl+F**: Find text (Ctrl+N: next, Ctrl+P: previous, Esc to exit)
- **Arrow Keys**: Move cursor
- **Home/End**: Jump to line start/end
- **Page Up/Down**: Scroll by page
- **Mouse**: Click to position cursor, drag to select text
- **Ctrl+Left/Right**: Jump by words
- **Shift+Arrow**: Select text
- **Shift+Ctrl+Arrow**: Select by words

The editor supports standard text editing operations including inserting characters, deleting with backspace, and creating new lines with Enter.

## Architecture

The editor is modular with separate components for:
- `terminal.c/h` - Terminal I/O and raw mode handling
- `buffer.c/h` - Text buffer management and file operations
- `clipboard.c/h` - System clipboard integration
- `main.c` - Editor logic and user interface

Built using C23 standard with POSIX compliance for cross-platform compatibility.

## Development

See [todo.md](todo.md) for planned features and upcoming improvements.