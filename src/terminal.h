#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdbool.h>

#define CTRL_KEY(k) ((k) & 0x1f)

// ANSI Color and formatting codes
#define COLOR_RESET         "\033[0m"
#define COLOR_BOLD          "\033[1m"
#define COLOR_NORMAL        "\033[22m"  // Reset bold/dim without affecting colors
#define COLOR_REVERSE       "\033[7m"

// Foreground colors
#define FG_BLACK            "\033[30m"
#define FG_RED              "\033[31m"
#define FG_GREEN            "\033[32m"
#define FG_YELLOW           "\033[33m"
#define FG_BLUE             "\033[34m"
#define FG_MAGENTA          "\033[35m"
#define FG_CYAN             "\033[36m"
#define FG_WHITE            "\033[37m"

// Background colors
#define BG_RED              "\033[41m"
#define BG_YELLOW           "\033[43m"
#define BG_BLUE             "\033[44m"
#define BG_GRAY             "\033[100m"
#define BG_WHITE            "\033[47m"

// Common combinations
#define STYLE_TAB_BAR       COLOR_REVERSE
#define STYLE_TAB_CURRENT   COLOR_RESET BG_WHITE FG_BLACK
#define STYLE_LINE_NUMBERS  FG_CYAN
#define STYLE_QUIT_DIALOG   BG_RED
#define STYLE_RELOAD_DIALOG BG_YELLOW
#define STYLE_FILE_MGR_FOCUSED BG_BLUE
#define STYLE_FILE_MGR_UNFOCUSED BG_GRAY
#define STYLE_FILE_MGR_SELECTED BG_WHITE FG_BLACK
#define STYLE_HOVER_BG      BG_WHITE
#define STYLE_HOVER_FG      FG_BLACK

#define MOUSE_MOVE_EVENT    35

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
    CTRL_SHIFT_TAB,
    F1_KEY,
    F2_KEY,
    F3_KEY,
    F4_KEY,
    F5_KEY,
    F6_KEY,
    F7_KEY,
    F8_KEY,
    F9_KEY,
    F10_KEY,
    F11_KEY,
    F12_KEY
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
