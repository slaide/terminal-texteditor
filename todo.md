# TODO

This file tracks planned features and improvements for the terminal text editor.

## Completed Features

### Enhanced Mouse Support ✓

The editor now has full mouse support including:

1. **Mouse Selection (Drag to Select)** ✓
   - Click-and-drag text selection functionality
   - Support for selection across multiple lines
   - Proper selection state management during drag operations

2. **Auto-Scroll During Selection** ✓
   - When dragging selection against screen boundaries, automatically scroll the view
   - Allows selecting text that spans more than one screen height
   - Smooth scrolling behavior during drag operations

3. **Mouse Selection Integration** ✓
   - Mouse-based selection works seamlessly with existing keyboard-based selection
   - Consistent selection highlighting and behavior
   - Full support for mixed mouse/keyboard selection workflows

4. **Cursor Visibility Fix** ✓
   - Cursor appears immediately after mouse clicks
   - Proper cursor state management during selection operations

### Find and Search ✓

The editor now includes comprehensive search functionality:

1. **Real-time Find Mode** ✓
   - Ctrl+F enters interactive find mode
   - Live search results as you type
   - Visual feedback with match counter (e.g., "2/3")

2. **Match Navigation** ✓
   - Ctrl+N jumps to next match with wraparound
   - Found text is automatically selected/highlighted
   - Escape key exits find mode

3. **Search Integration** ✓
   - Seamless integration with existing selection system
   - Found matches can be immediately copied or operated on
   - Status line shows search progress and controls

## Next Steps

Future improvements could include:
- Search and replace functionality (Ctrl+H)
- Case-insensitive search toggle
- Regular expression support
- Syntax highlighting
- Multiple file tabs
- Configuration file support