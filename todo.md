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

### Window Resize Handling ✓

The editor now properly handles terminal window resizing:

1. **Non-blocking Input System** ✓
   - Uses select() with timeout to avoid blocking on input
   - Continuous resize detection even without keyboard input
   - 50ms response time for resize events

2. **Automatic Screen Adjustment** ✓
   - Cursor position preserved during resize
   - Scrolling boundaries recalculated
   - Status line repositioned correctly

3. **Responsive Layout** ✓
   - Text content adapts to new window dimensions
   - Line numbers and status bar adjust automatically
   - No screen corruption during resize operations

### Enhanced Status Bar ✓

The status bar now provides comprehensive file information:

1. **File Information Display** ✓
   - Filename (or "untitled" for new files)
   - Current line position and total line count (e.g., "Line 42/150")
   - File size with smart formatting (bytes/KB/MB)
   - Modification status indicator "[modified]"

2. **Dynamic Updates** ✓
   - Line numbers update in real-time as cursor moves
   - File size recalculates when content changes
   - Modification status tracks unsaved changes
   - Temporary status messages still show for 3 seconds

3. **Clean Display Format** ✓
   - Compact layout: "filename  Line X/Y  size [modified]"
   - Consistent with existing find mode and message display
   - Removed debug key code display for cleaner interface

## Next Steps

Future improvements could include:
- Search and replace functionality (Ctrl+H)
- Case-insensitive search toggle
- Regular expression support
- Syntax highlighting
- Multiple file tabs
- Configuration file support