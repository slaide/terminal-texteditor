# Terminal Text Editor

A modern terminal-based text editor written in C23 with **multi-tab support**, mouse support, and familiar keyboard shortcuts.

## Features

- **Multi-Tab Support**: Work with unlimited tabs, visual tab bar with current tab indicators
- **File Manager**: Built-in sidebar for browsing files and directories
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
  - `Ctrl+T` - Create new tab
  - `Ctrl+O` - Open file in new tab
  - `Ctrl+W` - Close current tab
  - `Ctrl+[`/`Ctrl+]` - Switch between tabs
  - `Ctrl+E` - Toggle file manager sidebar
  - `Ctrl+Q` - Quit editor
- **Navigation**:
  - Arrow keys for cursor movement
  - `Ctrl+Left`/`Ctrl+Right` for word-by-word navigation
  - `Home`/`End` keys for line navigation
  - `Page Up`/`Page Down` for scrolling
- **Text Selection**: Select text with mouse or keyboard (Shift+Arrow keys)
- **Find Functionality**: Real-time search with Ctrl+F (Ctrl+N: next, Ctrl+P: prev)
- **Window Resize**: Automatic handling of terminal window resizing
- **Tab Bar**: Visual tab bar showing all open tabs with current tab indicator (`>tab<`)
- **Status Bar**: Shows filename, current line/total lines, file size, and modification status
- **File Manager**: Sidebar for file browsing with directory navigation and file operations
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

# Create a new file (starts with empty tab)
./texteditor

# Open multiple files in tabs
./texteditor file1.txt
# Then use Ctrl+O to open more files in new tabs
```

## Dependencies

For clipboard functionality, install one of:
- `xclip` (Linux/X11)
- `xsel` (Linux/X11) 
- `pbcopy`/`pbpaste` (macOS)

## Controls

### Tab Management
- **Ctrl+T**: Create new empty tab
- **Ctrl+O**: Open file in new tab (prompts for filename in status line)
- **Ctrl+W**: Close current tab (cannot close last tab)
- **Ctrl+[**: Switch to previous tab
- **Ctrl+]**: Switch to next tab

### File Manager
- **Ctrl+E**: Toggle file manager sidebar on/off
- **Tab**: Switch focus between file manager and text editor (when file manager is open)
- **↑/↓**: Navigate files and directories (when file manager is focused)
- **Enter**: Open selected file in new tab OR enter selected directory
- **Esc**: Return focus to text editor

### File Operations
- **Ctrl+S**: Save current tab's file
- **Ctrl+Q**: Quit editor

### Text Operations
- **Ctrl+C**: Copy selection
- **Ctrl+X**: Cut selection
- **Ctrl+V**: Paste
- **Ctrl+A**: Select all text in current tab
- **Ctrl+F**: Find text (Ctrl+N: next, Ctrl+P: previous, Esc to exit)

### Navigation
- **Arrow Keys**: Move cursor
- **Home/End**: Jump to line start/end
- **Page Up/Down**: Scroll by page
- **Ctrl+Left/Right**: Jump by words
- **Shift+Arrow**: Select text
- **Shift+Ctrl+Arrow**: Select by words

### Mouse Support
- **Click**: Position cursor
- **Drag**: Select text with auto-scroll

The editor supports standard text editing operations including inserting characters, deleting with backspace, and creating new lines with Enter.

## Multi-Tab Interface

The editor features a visual tab bar at the top of the screen:
```
 1:file1.txt  >2:file2.txt*<  3:untitled  4:readme.md 
```

- **Current tab**: Highlighted with `>` and `<` brackets plus different background color
- **Modified files**: Show asterisk `*` next to filename  
- **Tab numbers**: Easy reference for navigation
- **Per-tab state**: Each tab maintains its own cursor position, selection, and scroll offset

## Architecture

The editor is modular with separate components for:
- `terminal.c/h` - Terminal I/O and raw mode handling
- `buffer.c/h` - Text buffer management and file operations
- `clipboard.c/h` - System clipboard integration
- `main.c` - Editor logic, user interface, and multi-tab management

Built using C23 standard with POSIX compliance for cross-platform compatibility.

## File Manager

The built-in file manager provides seamless file browsing and navigation:

- **Sidebar display**: Shows files with sizes (e.g., `1.2K`) and directories marked as `<DIR>`
- **Directory navigation**: Use Enter to open directories, `..` to go up one level
- **File operations**: Enter on files opens them in new tabs
- **Focus management**: Tab key switches between file manager and text editor
- **Visual feedback**: Different background colors indicate focus state
- **Responsive layout**: Tab bar and text area adjust when file manager is visible

For detailed usage instructions, see [FILE_MANAGER_USAGE.md](FILE_MANAGER_USAGE.md).

## Development

See [todo.md](todo.md) for planned features and upcoming improvements.
