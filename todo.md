# TODO

This file tracks planned features and improvements for the terminal text editor.

## Next Steps

### Enhanced Mouse Support

The editor currently has basic mouse support for clicking to position the cursor. The following enhancements are planned:

1. **Mouse Selection (Drag to Select)**
   - Implement click-and-drag text selection functionality
   - Support selection across multiple lines
   - Maintain selection state during drag operations

2. **Auto-Scroll During Selection**
   - When dragging selection against the upper screen boundary, automatically scroll the view upward
   - When dragging selection against the lower screen boundary, automatically scroll the view downward
   - This allows selecting text that spans more than one screen height

3. **Mouse Selection Integration**
   - Ensure mouse-based selection works seamlessly with existing keyboard-based selection
   - Maintain consistent selection highlighting and behavior
   - Support mixed mouse/keyboard selection workflows

## Implementation Notes

- Mouse drag detection will require tracking mouse button state across multiple mouse events
- Auto-scroll functionality should respect reasonable scroll speeds to maintain usability
- Selection boundaries need to be properly calculated when scrolling during drag operations