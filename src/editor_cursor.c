#define _GNU_SOURCE
#include "editor_cursor.h"
#include "editor.h"
#include "editor_folds.h"
#include "editor_tabs.h"
#include <string.h>

void move_cursor(int dx, int dy) {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    tab->cursor_x += dx;
    tab->cursor_y += dy;
    
    if (tab->cursor_y < 0) tab->cursor_y = 0;
    if (tab->cursor_y >= tab->buffer->line_count) {
        tab->cursor_y = tab->buffer->line_count - 1;
    }
    
    int line_len = tab->buffer->lines[tab->cursor_y] ? 
                   strlen(tab->buffer->lines[tab->cursor_y]) : 0;
    if (tab->cursor_x > line_len) tab->cursor_x = line_len;
    if (tab->cursor_x < 0) tab->cursor_x = 0;
    
    editor.needs_full_redraw = true;
}

void scroll_if_needed(void) {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    int visible_lines = 0;
    int last_visible_line = tab->offset_y;
    for (int y = tab->offset_y; y < tab->buffer->line_count; y++) {
        if (is_line_visible(tab, y)) {
            visible_lines++;
            last_visible_line = y;
            if (visible_lines >= editor.screen_rows - 2) break;
        }
    }
    
    if (tab->cursor_y < tab->offset_y) {
        tab->offset_y = tab->cursor_y;
        editor.needs_full_redraw = true;
    } else if (tab->cursor_y > last_visible_line) {
        int target = tab->cursor_y;
        int count = 0;
        int y = target;
        while (y >= 0 && count < editor.screen_rows - 2) {
            if (is_line_visible(tab, y)) {
                count++;
            }
            y--;
        }
        tab->offset_y = y + 1;
        if (tab->offset_y < 0) tab->offset_y = 0;
        editor.needs_full_redraw = true;
    }
    
    if (tab->cursor_x < tab->offset_x) {
        tab->offset_x = tab->cursor_x;
        editor.needs_full_redraw = true;
    } else if (tab->cursor_x >= tab->offset_x + editor.screen_cols - editor.line_number_width - 1) {
        tab->offset_x = tab->cursor_x - (editor.screen_cols - editor.line_number_width - 2);
        if (tab->offset_x < 0) tab->offset_x = 0;
        editor.needs_full_redraw = true;
    }
}

void auto_scroll_during_selection(int screen_y) {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    if (screen_y <= 1) {
        if (tab->offset_y > 0) {
            tab->offset_y--;
            editor.needs_full_redraw = true;
        }
    } else if (screen_y >= editor.screen_rows - 1) {
        int max_offset = tab->buffer->line_count - (editor.screen_rows - 2);
        if (max_offset < 0) max_offset = 0;
        if (tab->offset_y < max_offset) {
            tab->offset_y++;
            editor.needs_full_redraw = true;
        }
    }
}

bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
           (c >= '0' && c <= '9') || c == '_';
}

void move_cursor_word_right(void) {
    Tab* tab = get_current_tab();
    if (!tab || tab->cursor_y >= tab->buffer->line_count) return;
    
    char *line = tab->buffer->lines[tab->cursor_y];
    if (!line) return;
    
    int len = strlen(line);
    
    if (tab->cursor_x >= len) {
        if (tab->cursor_y < tab->buffer->line_count - 1) {
            tab->cursor_y++;
            tab->cursor_x = 0;
            line = tab->buffer->lines[tab->cursor_y];
            if (line) {
                len = strlen(line);
                while (tab->cursor_x < len && !is_word_char(line[tab->cursor_x])) {
                    tab->cursor_x++;
                }
                while (tab->cursor_x < len && is_word_char(line[tab->cursor_x])) {
                    tab->cursor_x++;
                }
            }
        }
        return;
    }
    
    if (!is_word_char(line[tab->cursor_x])) {
        while (tab->cursor_x < len && !is_word_char(line[tab->cursor_x])) {
            tab->cursor_x++;
        }
        while (tab->cursor_x < len && is_word_char(line[tab->cursor_x])) {
            tab->cursor_x++;
        }
    } else {
        while (tab->cursor_x < len && is_word_char(line[tab->cursor_x])) {
            tab->cursor_x++;
        }
    }
}

void move_cursor_word_left(void) {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    char *line = tab->buffer->lines[tab->cursor_y];
    if (!line) return;
    
    if (tab->cursor_x == 0) {
        if (tab->cursor_y > 0) {
            tab->cursor_y--;
            line = tab->buffer->lines[tab->cursor_y];
            tab->cursor_x = line ? strlen(line) : 0;
        }
        return;
    }
    
    while (tab->cursor_x > 0 && !is_word_char(line[tab->cursor_x - 1])) {
        tab->cursor_x--;
    }
    
    if (tab->cursor_x >= 0 && is_word_char(line[tab->cursor_x])) {
        while (tab->cursor_x > 0 && is_word_char(line[tab->cursor_x - 1])) {
            tab->cursor_x--;
        }
    }
}
