#define _GNU_SOURCE
#include "editor_selection.h"
#include "editor.h"
#include "editor_tabs.h"
#include "buffer.h"
#include "editor_folds.h"
#include "lsp_integration.h"
#include <stdlib.h>
#include <string.h>

void start_selection(void) {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    tab->select_start_x = tab->cursor_x;
    tab->select_start_y = tab->cursor_y;
    tab->select_end_x = tab->cursor_x;
    tab->select_end_y = tab->cursor_y;
    tab->selecting = true;
    editor.needs_full_redraw = true;
}

void update_selection(void) {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    if (tab->selecting) {
        tab->select_end_x = tab->cursor_x;
        tab->select_end_y = tab->cursor_y;
        editor.needs_full_redraw = true;
    }
}

void clear_selection(void) {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    if (tab->selecting) {
        tab->selecting = false;
        editor.needs_full_redraw = true;
    }
}

void delete_selection(void) {
    Tab* tab = get_current_tab();
    if (!tab || !tab->selecting) return;
    
    int start_x = tab->select_start_x;
    int start_y = tab->select_start_y;
    int end_x = tab->select_end_x;
    int end_y = tab->select_end_y;
    
    if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
        int temp_x = start_x, temp_y = start_y;
        start_x = end_x; start_y = end_y;
        end_x = temp_x; end_y = temp_y;
    }

    if (start_y != end_y && end_x == 0) {
        end_y--;
        if (end_y < start_y) {
            end_y = start_y;
            end_x = start_x;
        } else {
            end_x = tab->buffer->lines[end_y] ? strlen(tab->buffer->lines[end_y]) : 0;
        }
    }
    
    if (start_y == end_y) {
        for (int x = end_x - 1; x >= start_x; x--) {
            if (x < (int)strlen(tab->buffer->lines[start_y])) {
                buffer_delete_char(tab->buffer, start_y, x);
            }
        }
    } else {
        for (int x = end_x - 1; x >= 0; x--) {
            if (x < (int)strlen(tab->buffer->lines[end_y])) {
                buffer_delete_char(tab->buffer, end_y, x);
            }
        }
        
        for (int y = end_y; y > start_y; y--) {
            if (y < tab->buffer->line_count) {
                buffer_delete_line(tab->buffer, y);
            }
        }
        
        if (start_y < tab->buffer->line_count && tab->buffer->lines[start_y]) {
            int line_len = strlen(tab->buffer->lines[start_y]);
            for (int x = line_len - 1; x >= start_x; x--) {
                buffer_delete_char(tab->buffer, start_y, x);
            }
        }
        
        if (start_y < tab->buffer->line_count - 1) {
            buffer_merge_lines(tab->buffer, start_y);
        }
    }
    
    tab->cursor_x = start_x;
    tab->cursor_y = start_y;

    clear_selection();
    tab->modified = true;

    notify_lsp_file_changed(tab);
    detect_folds(tab);

    editor.needs_full_redraw = true;
}

char *get_selected_text(void) {
    Tab* tab = get_current_tab();
    if (!tab || !tab->selecting) return NULL;
    
    int start_x = tab->select_start_x;
    int start_y = tab->select_start_y;
    int end_x = tab->select_end_x;
    int end_y = tab->select_end_y;
    
    if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
        int temp_x = start_x, temp_y = start_y;
        start_x = end_x; start_y = end_y;
        end_x = temp_x; end_y = temp_y;
    }
    
    int total_len = 0;
    
    if (start_y == end_y) {
        total_len = end_x - start_x;
    } else {
        if (tab->buffer->lines[start_y]) {
            total_len += strlen(tab->buffer->lines[start_y]) - start_x;
        }
        total_len++;
        
        for (int y = start_y + 1; y < end_y; y++) {
            if (tab->buffer->lines[y]) {
                total_len += strlen(tab->buffer->lines[y]);
            }
            total_len++;
        }
        
        if (tab->buffer->lines[end_y]) {
            total_len += end_x;
        }
    }
    
    char *selected = malloc(total_len + 1);
    if (!selected) return NULL;
    
    int pos = 0;
    
    if (start_y == end_y) {
        if (tab->buffer->lines[start_y]) {
            memcpy(selected + pos, tab->buffer->lines[start_y] + start_x, end_x - start_x);
            pos += end_x - start_x;
        }
    } else {
        if (tab->buffer->lines[start_y]) {
            int len = strlen(tab->buffer->lines[start_y]) - start_x;
            memcpy(selected + pos, tab->buffer->lines[start_y] + start_x, len);
            pos += len;
        }
        selected[pos++] = '\n';
        
        for (int y = start_y + 1; y < end_y; y++) {
            if (tab->buffer->lines[y]) {
                int len = strlen(tab->buffer->lines[y]);
                memcpy(selected + pos, tab->buffer->lines[y], len);
                pos += len;
            }
            selected[pos++] = '\n';
        }
        
        if (tab->buffer->lines[end_y]) {
            memcpy(selected + pos, tab->buffer->lines[end_y], end_x);
            pos += end_x;
        }
    }
    
    selected[pos] = '\0';
    return selected;
}
