#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdbool.h>

#define CTRL_KEY(k) ((k) & 0x1f)

enum EditorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    CTRL_ARROW_LEFT,
    CTRL_ARROW_RIGHT,
    CTRL_ARROW_UP,
    CTRL_ARROW_DOWN,
    SHIFT_ARROW_LEFT,
    SHIFT_ARROW_RIGHT,
    SHIFT_ARROW_UP,
    SHIFT_ARROW_DOWN,
    SHIFT_CTRL_ARROW_LEFT,
    SHIFT_CTRL_ARROW_RIGHT,
    MOUSE_SCROLL_UP,
    MOUSE_SCROLL_DOWN,
    CTRL_TAB,
    CTRL_SHIFT_TAB
};

bool terminal_init(void);
void terminal_cleanup(void);
int terminal_read_key(void);
void terminal_clear_screen(void);
void terminal_set_cursor_position(int row, int col);
void terminal_get_window_size(int *rows, int *cols);
void terminal_enable_mouse(void);
void terminal_disable_mouse(void);
void terminal_hide_cursor(void);
void terminal_show_cursor(void);

#endif