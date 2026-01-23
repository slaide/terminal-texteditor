#include "terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>

static struct termios orig_termios;

bool terminal_init(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        return false;
    }

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        return false;
    }

    printf("\033[?1049h\033[H");
    fflush(stdout);
    
    return true;
}

void terminal_cleanup(void) {
    terminal_disable_mouse();
    printf("\033[?25h\033[?1049l");  // Show cursor before exiting
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

int terminal_read_key(void) {
    int nread;
    char c;
    
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) exit(1);
    }

    if (c == '\033') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\033';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\033';

        if (seq[0] == '[') {
            if (seq[1] == '1') {
                char extra[3];
                if (read(STDIN_FILENO, &extra[0], 1) != 1) return '\033';
                if (extra[0] == ';') {
                    if (read(STDIN_FILENO, &extra[1], 1) != 1) return '\033';
                    if (read(STDIN_FILENO, &extra[2], 1) != 1) return '\033';
                    
                    // Debug: For now, let's see what we're getting
                    // This is temporary debugging code
                    
                    if (extra[1] == '2') {  // Shift modifier
                        switch (extra[2]) {
                            case 'A': return SHIFT_ARROW_UP;
                            case 'B': return SHIFT_ARROW_DOWN;
                            case 'C': return SHIFT_ARROW_RIGHT;
                            case 'D': return SHIFT_ARROW_LEFT;
                        }
                    } else if (extra[1] == '5') {  // Ctrl modifier
                        switch (extra[2]) {
                            case 'A': return CTRL_ARROW_UP;
                            case 'B': return CTRL_ARROW_DOWN;
                            case 'C': return CTRL_ARROW_RIGHT;
                            case 'D': return CTRL_ARROW_LEFT;
                            case 'I': return CTRL_TAB;
                        }
                    } else if (extra[1] == '6') {  // Shift+Ctrl modifier
                        switch (extra[2]) {
                            case 'C': return SHIFT_CTRL_ARROW_RIGHT;
                            case 'D': return SHIFT_CTRL_ARROW_LEFT;
                            case 'I': return CTRL_SHIFT_TAB;
                        }
                    }
                }
            } else if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\033';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                } else if (seq[1] == '2' && seq[2] == '7') {
                    // Extended key sequence: CSI 27;modifier;keycode ~
                    // Used by xterm for Ctrl+Tab, etc.
                    char ext[8];
                    int i = 0;
                    while (i < 7) {
                        if (read(STDIN_FILENO, &ext[i], 1) != 1) break;
                        if (ext[i] == '~') break;
                        i++;
                    }
                    ext[i] = '\0';
                    // Parse ;modifier;keycode
                    int modifier = 0, keycode = 0;
                    if (sscanf(ext, ";%d;%d", &modifier, &keycode) == 2) {
                        if (keycode == 9) { // Tab
                            if (modifier == 5) return CTRL_TAB;        // Ctrl
                            if (modifier == 6) return CTRL_SHIFT_TAB;  // Ctrl+Shift
                        }
                    }
                }
            } else if (seq[1] == 'M') {
                char mouse[3];
                if (read(STDIN_FILENO, mouse, 3) == 3) {
                    int button = mouse[0] - 32;
                    int x = mouse[1] - 32;
                    int y = mouse[2] - 32;

                    // Check for scroll wheel events (button codes 64 and 65)
                    if ((button & 64) != 0) {
                        if ((button & 1) == 0) {
                            return MOUSE_SCROLL_UP;
                        } else {
                            return MOUSE_SCROLL_DOWN;
                        }
                    }

                    extern void handle_mouse(int button, int x, int y, int pressed);

                    // Simple mouse event handling
                    if ((button & 32) == 0) {
                        // Button press
                        handle_mouse(button & 3, x, y, 1);
                    } else if ((button & 3) == 3) {
                        // Button release (no button active)
                        handle_mouse(0, x, y, 0);
                    } else if (button & 32) {
                        // Drag/motion with button held
                        handle_mouse(32, x, y, 1);
                    }
                }
                return 0;
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == '[' && seq[1] >= 'A' && seq[1] <= 'D') {
            // Some terminals send modified sequences without the "1;" prefix
            char next;
            if (read(STDIN_FILENO, &next, 1) == 1) {
                if (next == '2') {  // Shift modifier
                    switch (seq[1]) {
                        case 'A': return SHIFT_ARROW_UP;
                        case 'B': return SHIFT_ARROW_DOWN;
                        case 'C': return SHIFT_ARROW_RIGHT;
                        case 'D': return SHIFT_ARROW_LEFT;
                    }
                }
                // If we can't handle it, just return the basic arrow
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\033';
    } else {
        return c;
    }
}

void terminal_clear_screen(void) {
    printf("\033[2J\033[H");
}

void terminal_set_cursor_position(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

void terminal_get_window_size(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        printf("\033[999C\033[999B");
        printf("\033[6n");
        fflush(stdout);

        char buf[32];
        unsigned int i = 0;

        while (i < sizeof(buf) - 1) {
            if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
            if (buf[i] == 'R') break;
            i++;
        }
        buf[i] = '\0';

        if (buf[0] != '\033' || buf[1] != '[') return;
        if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
}

void terminal_enable_mouse(void) {
    printf("\033[?1000h\033[?1002h");  // Enable basic mouse + button motion
    fflush(stdout);
}

void terminal_disable_mouse(void) {
    printf("\033[?1002l\033[?1000l");  // Disable in reverse order
    fflush(stdout);
}

void terminal_hide_cursor(void) {
    printf("\033[?25l");
    fflush(stdout);
}

void terminal_show_cursor(void) {
    printf("\033[?25h");
    fflush(stdout);
}