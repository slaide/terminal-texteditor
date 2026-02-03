#define _GNU_SOURCE
#include "editor_mouse.h"
#include "editor.h"
#include "editor_tabs.h"
#include "editor_folds.h"
#include "editor_selection.h"
#include "editor_cursor.h"
#include "editor_completion.h"
#include "editor_hover.h"
#include "terminal.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DOUBLE_CLICK_MS 400

static long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000LL + (long long)(now.tv_nsec / 1000000LL);
}

static int screen_y_to_file_y(Tab *tab, int screen_y) {
    if (!tab) return screen_y;

    int file_y = tab->offset_y;
    int current_screen_y = 0;

    while (current_screen_y < screen_y && file_y < tab->buffer->line_count) {
        while (file_y < tab->buffer->line_count && !is_line_visible(tab, file_y)) {
            file_y++;
        }
        if (file_y >= tab->buffer->line_count) break;

        if (current_screen_y == screen_y) break;
        current_screen_y++;
        file_y++;
    }

    while (file_y < tab->buffer->line_count && !is_line_visible(tab, file_y)) {
        file_y++;
    }

    return file_y;
}

static void get_word_bounds_at(Tab *tab, int line, int col, int *out_start, int *out_end) {
    if (!out_start || !out_end) return;
    *out_start = col;
    *out_end = col;
    if (!tab || !tab->buffer || line < 0 || line >= tab->buffer->line_count) return;

    char *text = tab->buffer->lines[line];
    if (!text) return;

    int len = (int)strlen(text);
    if (len == 0) return;

    int idx = col;
    if (idx >= len) idx = len - 1;
    if (idx < 0) idx = 0;

    if (!is_word_char(text[idx])) {
        if (idx > 0 && is_word_char(text[idx - 1])) {
            idx = idx - 1;
        } else if (idx + 1 < len && is_word_char(text[idx + 1])) {
            idx = idx + 1;
        } else {
            *out_start = idx;
            *out_end = idx + 1;
            return;
        }
    }

    int start = idx;
    int end = idx + 1;
    while (start > 0 && is_word_char(text[start - 1])) start--;
    while (end < len && is_word_char(text[end])) end++;
    *out_start = start;
    *out_end = end;
}

void handle_mouse(int button, int x, int y, int pressed) {
    Tab* tab = get_current_tab();
    if (!tab) return;

    completion_clear();

    if (button == MOUSE_MOVE_EVENT) {
        if (editor.mouse_dragging || tab->selecting) return;

        if (y <= 1) {
            hover_clear();
            return;
        }

        int file_manager_end = 0;
        if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
            file_manager_end = editor.file_manager_width + 1;
        }

        if (editor.file_manager_visible && x <= file_manager_end) {
            hover_clear();
            return;
        }

        int editor_x_offset = file_manager_end;
        if (x <= editor_x_offset + editor.line_number_width) {
            int screen_row = y - 2;
            int buffer_y = screen_y_to_file_y(tab, screen_row);
            if (buffer_y < 0 || buffer_y >= tab->buffer->line_count) {
                hover_clear();
                return;
            }
            hover_show_diagnostic(buffer_y, x, y);
            return;
        }

        int buffer_x = x - editor_x_offset - editor.line_number_width - 1 + tab->offset_x;
        int screen_row = y - 2;
        int buffer_y = screen_y_to_file_y(tab, screen_row);

        if (buffer_y < 0 || buffer_y >= tab->buffer->line_count) {
            hover_clear();
            return;
        }

        int line_len = tab->buffer->lines[buffer_y] ?
                       strlen(tab->buffer->lines[buffer_y]) : 0;
        if (buffer_x < 0) buffer_x = 0;
        if (buffer_x >= line_len) {
            hover_show_diagnostic(buffer_y, x, y);
            return;
        }

        hover_schedule_request(buffer_y, buffer_x, x, y);
        return;
    }

    hover_clear();

    if (y == 1 && button == 0 && pressed) {
        int tab_start_col = 1;
        if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
            tab_start_col += editor.file_manager_width + 1;
        }

        int col = tab_start_col;
        for (int i = 0; i < editor.tab_count; i++) {
            Tab* t = &editor.tabs[i];
            const char* filename = t->filename ? t->filename : "untitled";
            const char* basename = filename;
            const char* slash = strrchr(filename, '/');
            if (slash) basename = slash + 1;

            int tab_width;
            int num_digits = (i + 1 >= 10) ? 2 : 1;
            if (i == editor.current_tab) {
                tab_width = 5 + num_digits + strlen(basename) + (t->modified ? 1 : 0);
            } else {
                tab_width = 3 + num_digits + strlen(basename) + (t->modified ? 1 : 0);
            }

            if (x >= col && x < col + tab_width) {
                if (i != editor.current_tab) {
                    switch_to_tab(i);
                    set_status_message("Switched to tab %d", i + 1);
                }
                return;
            }
            col += tab_width;
        }
        return;
    }

    if (y <= 1) return;

    int file_manager_end = 0;
    if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
        file_manager_end = editor.file_manager_width + 1;
    }

    if (editor.file_manager_visible && x <= file_manager_end) {
        if (button == 0 && pressed) {
            if (!editor.file_manager_focused) {
                editor.file_manager_focused = true;
                editor.needs_full_redraw = true;
            }
            int clicked_index = (y - 2) + editor.file_manager_offset;
            if (clicked_index >= 0 && clicked_index < editor.file_count) {
                editor.file_manager_cursor = clicked_index;
                editor.needs_full_redraw = true;
            }
        }
        return;
    }

    if (editor.file_manager_focused && button == 0 && pressed) {
        editor.file_manager_focused = false;
        editor.needs_full_redraw = true;
    }

    int editor_x_offset = file_manager_end;

    if (x == editor_x_offset + 1 && button == 0 && pressed) {
        int screen_y = y - 2;
        if (screen_y >= 0 && screen_y < editor.screen_rows - 2) {
            int file_y = tab->offset_y;
            for (int sy = 0; sy < screen_y && file_y < tab->buffer->line_count; sy++) {
                while (file_y < tab->buffer->line_count && !is_line_visible(tab, file_y)) {
                    file_y++;
                }
                file_y++;
            }
            while (file_y < tab->buffer->line_count && !is_line_visible(tab, file_y)) {
                file_y++;
            }

            Fold *fold = get_fold_at_line(tab, file_y);
            if (fold) {
                toggle_fold_at_line(tab, file_y);
                return;
            }
        }
    }

    if (x <= editor_x_offset + editor.line_number_width) {
        return;
    }

    if (button == 0) {
        if (pressed) {
            int buffer_x = x - editor_x_offset - editor.line_number_width - 1 + tab->offset_x;
            int screen_row = y - 2;
            int buffer_y = screen_y_to_file_y(tab, screen_row);

            if (buffer_y >= tab->buffer->line_count) {
                buffer_y = tab->buffer->line_count - 1;
            }
            if (buffer_y < 0) buffer_y = 0;
            
            int line_len = tab->buffer->lines[buffer_y] ? 
                           strlen(tab->buffer->lines[buffer_y]) : 0;
            if (buffer_x > line_len) buffer_x = line_len;
            if (buffer_x < 0) buffer_x = 0;
            
            long long now = monotonic_ms();
            bool is_double_click = (now - editor.last_click_ms <= DOUBLE_CLICK_MS) &&
                                   (editor.last_click_x == x) &&
                                   (editor.last_click_y == y);
            editor.last_click_ms = now;
            editor.last_click_x = x;
            editor.last_click_y = y;

            tab->cursor_x = buffer_x;
            tab->cursor_y = buffer_y;
            editor.mouse_dragging = true;
            editor.mouse_drag_start_x = buffer_x;
            editor.mouse_drag_start_y = buffer_y;

            clear_selection();

            if (is_double_click) {
                int word_start = buffer_x;
                int word_end = buffer_x + 1;
                get_word_bounds_at(tab, buffer_y, buffer_x, &word_start, &word_end);
                tab->select_start_x = word_start;
                tab->select_start_y = buffer_y;
                tab->select_end_x = word_end;
                tab->select_end_y = buffer_y;
                tab->selecting = true;
                editor.word_select_active = true;
                editor.word_anchor_line = buffer_y;
                editor.word_anchor_start = word_start;
                editor.word_anchor_end = word_end;
                editor.needs_full_redraw = true;
            } else {
                editor.word_select_active = false;
            }
        } else {
            if (editor.mouse_dragging) {
                editor.mouse_dragging = false;
            }
            if (!editor.mouse_dragging) {
                editor.word_select_active = false;
            }
        }
    } else if (button == 32) {
        if (editor.mouse_dragging) {
            auto_scroll_during_selection(y);

            int buffer_x = x - editor_x_offset - editor.line_number_width - 1 + tab->offset_x;
            int screen_row = y - 2;
            int buffer_y = screen_y_to_file_y(tab, screen_row);

            if (buffer_y >= tab->buffer->line_count) {
                buffer_y = tab->buffer->line_count - 1;
            }
            if (buffer_y < 0) buffer_y = 0;
            
            int line_len = tab->buffer->lines[buffer_y] ? 
                           strlen(tab->buffer->lines[buffer_y]) : 0;
            if (buffer_x > line_len) buffer_x = line_len;
            if (buffer_x < 0) buffer_x = 0;
            
            if (editor.word_select_active) {
                int word_start = buffer_x;
                int word_end = buffer_x + 1;
                get_word_bounds_at(tab, buffer_y, buffer_x, &word_start, &word_end);

                tab->selecting = true;
                if (buffer_y > editor.word_anchor_line ||
                    (buffer_y == editor.word_anchor_line && word_end >= editor.word_anchor_end)) {
                    tab->select_start_x = editor.word_anchor_start;
                    tab->select_start_y = editor.word_anchor_line;
                    tab->select_end_x = word_end;
                    tab->select_end_y = buffer_y;
                    tab->cursor_x = word_end;
                    tab->cursor_y = buffer_y;
                } else {
                    tab->select_start_x = word_start;
                    tab->select_start_y = buffer_y;
                    tab->select_end_x = editor.word_anchor_end;
                    tab->select_end_y = editor.word_anchor_line;
                    tab->cursor_x = word_start;
                    tab->cursor_y = buffer_y;
                }
                editor.needs_full_redraw = true;
            } else {
                if (!tab->selecting) {
                    start_selection();
                }
                
                tab->cursor_x = buffer_x;
                tab->cursor_y = buffer_y;
                update_selection();
            }
        }
    }
}
