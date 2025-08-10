#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include "terminal.h"
#include "buffer.h"
#include "clipboard.h"

typedef struct {
    TextBuffer *buffer;
    int cursor_x, cursor_y;
    int offset_x, offset_y;
    int screen_rows, screen_cols;
    int select_start_x, select_start_y;
    int select_end_x, select_end_y;
    bool selecting;
    bool modified;
    char *filename;
    bool needs_full_redraw;
    int last_cursor_x, last_cursor_y;
    int last_offset_x, last_offset_y;
    char *status_message;
    time_t status_message_time;
    int line_number_width;
    bool mouse_dragging;
    int mouse_drag_start_x, mouse_drag_start_y;
    bool find_mode;
    char *search_query;
    int search_query_len;
    int search_query_capacity;
    int current_match;
    int total_matches;
} Editor;

Editor editor = {0};

void cleanup_and_exit(int status) {
    terminal_cleanup();
    if (editor.buffer) buffer_free(editor.buffer);
    if (editor.filename) free(editor.filename);
    if (editor.status_message) free(editor.status_message);
    if (editor.search_query) free(editor.search_query);
    exit(status);
}

void signal_handler(int sig) {
    (void)sig;
    cleanup_and_exit(1);
}

void set_status_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    
    if (editor.status_message) {
        free(editor.status_message);
    }
    
    editor.status_message = malloc(256);
    if (editor.status_message) {
        vsnprintf(editor.status_message, 256, fmt, ap);
        editor.status_message_time = time(NULL);
    }
    
    va_end(ap);
}

void handle_resize(int sig) {
    (void)sig;
    terminal_get_window_size(&editor.screen_rows, &editor.screen_cols);
    editor.needs_full_redraw = true;
}

void move_cursor(int dx, int dy) {
    if (dy != 0) {
        int new_y = editor.cursor_y + dy;
        
        if (new_y < 0) {
            new_y = 0;
        }
        if (new_y >= editor.buffer->line_count) {
            new_y = editor.buffer->line_count - 1;
        }
        
        if (new_y != editor.cursor_y) {
            editor.cursor_y = new_y;
            int line_len = editor.buffer->lines[editor.cursor_y] ? 
                           strlen(editor.buffer->lines[editor.cursor_y]) : 0;
            if (editor.cursor_x > line_len) {
                editor.cursor_x = line_len;
            }
        }
    }
    
    if (dx != 0) {
        int line_len = editor.buffer->lines[editor.cursor_y] ? 
                       strlen(editor.buffer->lines[editor.cursor_y]) : 0;
        
        if (dx < 0 && editor.cursor_x == 0) {
            if (editor.cursor_y > 0) {
                editor.cursor_y--;
                int prev_line_len = editor.buffer->lines[editor.cursor_y] ? 
                                   strlen(editor.buffer->lines[editor.cursor_y]) : 0;
                editor.cursor_x = prev_line_len;
            }
        } else if (dx > 0 && editor.cursor_x >= line_len) {
            if (editor.cursor_y < editor.buffer->line_count - 1) {
                editor.cursor_y++;
                editor.cursor_x = 0;
            }
        } else {
            int new_x = editor.cursor_x + dx;
            if (new_x < 0) new_x = 0;
            if (new_x > line_len) new_x = line_len;
            editor.cursor_x = new_x;
        }
    }
}

void scroll_if_needed() {
    if (editor.cursor_y < editor.offset_y) {
        editor.offset_y = editor.cursor_y;
    }
    if (editor.cursor_y >= editor.offset_y + editor.screen_rows - 1) {
        editor.offset_y = editor.cursor_y - editor.screen_rows + 2;
    }
    
    if (editor.cursor_x < editor.offset_x) {
        editor.offset_x = editor.cursor_x;
    }
    int text_width = editor.screen_cols - editor.line_number_width;
    if (editor.cursor_x >= editor.offset_x + text_width) {
        editor.offset_x = editor.cursor_x - text_width + 1;
    }
}

void auto_scroll_during_selection(int screen_y) {
    bool scrolled = false;
    
    // Check if we need to scroll up (dragging above visible area)
    if (screen_y <= 1 && editor.offset_y > 0) {
        editor.offset_y--;
        editor.cursor_y = editor.offset_y;
        scrolled = true;
    }
    // Check if we need to scroll down (dragging below visible area)  
    else if (screen_y >= editor.screen_rows - 1 && 
             editor.offset_y + editor.screen_rows - 1 < editor.buffer->line_count - 1) {
        editor.offset_y++;
        editor.cursor_y = editor.offset_y + editor.screen_rows - 2;
        scrolled = true;
    }
    
    if (scrolled) {
        // Ensure cursor is within buffer bounds
        if (editor.cursor_y < 0) editor.cursor_y = 0;
        if (editor.cursor_y >= editor.buffer->line_count) {
            editor.cursor_y = editor.buffer->line_count - 1;
        }
        
        // Adjust cursor x position to line length
        int line_len = editor.buffer->lines[editor.cursor_y] ? 
                       strlen(editor.buffer->lines[editor.cursor_y]) : 0;
        if (editor.cursor_x > line_len) editor.cursor_x = line_len;
        
        editor.needs_full_redraw = true;
    }
}

bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
           (c >= '0' && c <= '9') || c == '_';
}

void move_cursor_word_right() {
    if (editor.cursor_y >= editor.buffer->line_count) return;
    
    char *line = editor.buffer->lines[editor.cursor_y];
    if (!line) return;
    
    int len = strlen(line);
    
    // If at end of line, move to next line
    if (editor.cursor_x >= len) {
        if (editor.cursor_y < editor.buffer->line_count - 1) {
            editor.cursor_y++;
            editor.cursor_x = 0;
            // Skip leading whitespace on new line
            line = editor.buffer->lines[editor.cursor_y];
            if (line) {
                len = strlen(line);
                while (editor.cursor_x < len && !is_word_char(line[editor.cursor_x])) {
                    editor.cursor_x++;
                }
                // Now move to END of this word
                while (editor.cursor_x < len && is_word_char(line[editor.cursor_x])) {
                    editor.cursor_x++;
                }
            }
        }
        return;
    }
    
    // If currently in whitespace, move to start then end of next word
    if (!is_word_char(line[editor.cursor_x])) {
        while (editor.cursor_x < len && !is_word_char(line[editor.cursor_x])) {
            editor.cursor_x++;
        }
        // Now at start of word, move to end of word
        while (editor.cursor_x < len && is_word_char(line[editor.cursor_x])) {
            editor.cursor_x++;
        }
    } else {
        // If currently in a word, move to end of current word and stop there
        while (editor.cursor_x < len && is_word_char(line[editor.cursor_x])) {
            editor.cursor_x++;
        }
        // Don't continue to next word - stop at end of current word
    }
    
    // If we've reached end of line, that's fine - stay there
}

void move_cursor_word_left() {
    char *line = editor.buffer->lines[editor.cursor_y];
    if (!line) return;
    
    // If at start of line, move to end of previous line
    if (editor.cursor_x == 0) {
        if (editor.cursor_y > 0) {
            editor.cursor_y--;
            line = editor.buffer->lines[editor.cursor_y];
            editor.cursor_x = line ? strlen(line) : 0;
            // Skip trailing whitespace on previous line
            while (editor.cursor_x > 0 && !is_word_char(line[editor.cursor_x - 1])) {
                editor.cursor_x--;
            }
            // Move to start of the word we're now at the end of
            while (editor.cursor_x > 0 && is_word_char(line[editor.cursor_x - 1])) {
                editor.cursor_x--;
            }
        }
        return;
    }
    
    // Move back one character to start
    editor.cursor_x--;
    
    // Skip trailing whitespace (move backwards through non-word chars)
    while (editor.cursor_x > 0 && !is_word_char(line[editor.cursor_x])) {
        editor.cursor_x--;
    }
    
    // If we're now on a word character, move to start of this word
    if (editor.cursor_x >= 0 && is_word_char(line[editor.cursor_x])) {
        while (editor.cursor_x > 0 && is_word_char(line[editor.cursor_x - 1])) {
            editor.cursor_x--;
        }
    }
}

void draw_line(int screen_y, int file_y) {
    terminal_set_cursor_position(screen_y + 1, 1);
    printf("\033[K");
    
    if (file_y < editor.buffer->line_count) {
        printf("\033[36m%6d\033[0m ", file_y + 1);
        
        if (editor.buffer->lines[file_y]) {
            char *line = editor.buffer->lines[file_y];
            int len = strlen(line);
            int start_x = editor.offset_x;
            int display_len = editor.screen_cols - editor.line_number_width;
            
            // Check if this line has any selection
            bool line_has_selection = false;
            int sel_start_x = 0, sel_start_y = 0, sel_end_x = 0, sel_end_y = 0;
            
            if (editor.selecting) {
                sel_start_x = editor.select_start_x;
                sel_start_y = editor.select_start_y;
                sel_end_x = editor.select_end_x;
                sel_end_y = editor.select_end_y;
                
                // Normalize selection
                if (sel_start_y > sel_end_y || (sel_start_y == sel_end_y && sel_start_x > sel_end_x)) {
                    int temp_x = sel_start_x, temp_y = sel_start_y;
                    sel_start_x = sel_end_x; sel_start_y = sel_end_y;
                    sel_end_x = temp_x; sel_end_y = temp_y;
                }
                
                line_has_selection = (file_y >= sel_start_y && file_y <= sel_end_y);
            }
            
            // Handle empty line that's selected
            if (line_has_selection && len == 0) {
                printf("\033[7m \033[0m");
                return;
            }
            
            if (start_x < len) {
                int copy_len = (len - start_x < display_len) ? len - start_x : display_len;
                
                if (line_has_selection) {
                    int line_sel_start = (file_y == sel_start_y) ? sel_start_x : 0;
                    int line_sel_end = (file_y == sel_end_y) ? sel_end_x : len;
                    
                    // Adjust for horizontal scrolling
                    line_sel_start = (line_sel_start >= start_x) ? line_sel_start - start_x : 0;
                    line_sel_end = (line_sel_end >= start_x) ? line_sel_end - start_x : 0;
                    
                    if (line_sel_end > copy_len) line_sel_end = copy_len;
                    if (line_sel_start > copy_len) line_sel_start = copy_len;
                    
                    // Print text with selection highlighting
                    if (line_sel_start > 0) {
                        printf("%.*s", line_sel_start, line + start_x);
                    }
                    if (line_sel_end > line_sel_start) {
                        printf("\033[7m%.*s\033[0m", line_sel_end - line_sel_start, 
                               line + start_x + line_sel_start);
                    }
                    if (line_sel_end < copy_len) {
                        printf("%.*s", copy_len - line_sel_end, line + start_x + line_sel_end);
                    }
                    
                    // Show selection extends to end of line (including newline)
                    if ((file_y != sel_end_y || sel_end_x > len) && line_sel_end >= copy_len) {
                        printf("\033[7m \033[0m");  // Highlight one space to show newline selection
                    }
                } else {
                    printf("%.*s", copy_len, line + start_x);
                }
            }
        }
    } else {
        printf("\033[36m%6s\033[0m ", "~");
    }
}

void draw_status_line() {
    terminal_set_cursor_position(editor.screen_rows, 1);
    printf("\033[K\033[7m ");
    
    if (editor.find_mode) {
        printf("Find: %s", editor.search_query ? editor.search_query : "");
        if (editor.total_matches > 0) {
            printf("  [%d/%d]", editor.current_match, editor.total_matches);
        } else if (editor.search_query_len > 0) {
            printf("  [no matches]");
        }
        printf("  (Ctrl+N: next, Ctrl+P: prev, Esc: exit)");
    } else {
        time_t now = time(NULL);
        if (editor.status_message && (now - editor.status_message_time < 3)) {
            printf("%s", editor.status_message);
        } else {
            printf("%s%s",
                   editor.filename ? editor.filename : "untitled",
                   editor.modified ? " [modified]" : "");
        }
    }
    
    printf(" \033[0m");
}

void draw_screen() {
    bool offset_changed = (editor.offset_x != editor.last_offset_x || 
                          editor.offset_y != editor.last_offset_y);
    
    if (editor.needs_full_redraw || offset_changed) {
        terminal_clear_screen();
        
        for (int y = 0; y < editor.screen_rows - 1; y++) {
            int file_y = y + editor.offset_y;
            draw_line(y, file_y);
        }
        
        draw_status_line();
        editor.needs_full_redraw = false;
        editor.last_offset_x = editor.offset_x;
        editor.last_offset_y = editor.offset_y;
    } else {
        draw_status_line();
    }
    
    // Show/hide cursor based on mode and selection state
    if (editor.find_mode) {
        terminal_show_cursor();
        // Position cursor at end of search query in status line
        int cursor_col = 8 + editor.search_query_len;  // "Find: " + query length
        terminal_set_cursor_position(editor.screen_rows, cursor_col);
    } else if (!editor.selecting) {
        terminal_show_cursor();
        
        // Simple, direct cursor positioning
        int screen_row = (editor.cursor_y - editor.offset_y) + 1;
        int screen_col = (editor.cursor_x - editor.offset_x) + 8;  // 7 for line numbers + 1 space
        
        // Make sure we're in the valid text area
        if (screen_row < 1) screen_row = 1;
        if (screen_row >= editor.screen_rows) screen_row = editor.screen_rows - 1;
        if (screen_col < 8) screen_col = 8;
        
        terminal_set_cursor_position(screen_row, screen_col);
    } else {
        terminal_hide_cursor();
    }
    
    editor.last_cursor_x = editor.cursor_x;
    editor.last_cursor_y = editor.cursor_y;
    
    fflush(stdout);
}

void start_selection() {
    editor.select_start_x = editor.cursor_x;
    editor.select_start_y = editor.cursor_y;
    editor.select_end_x = editor.cursor_x;
    editor.select_end_y = editor.cursor_y;
    editor.selecting = true;
    editor.needs_full_redraw = true;  // Force redraw to show selection
}

void update_selection() {
    if (editor.selecting) {
        editor.select_end_x = editor.cursor_x;
        editor.select_end_y = editor.cursor_y;
        editor.needs_full_redraw = true;  // Force redraw to show selection
    }
}

void clear_selection() {
    if (editor.selecting) {
        editor.selecting = false;
        editor.needs_full_redraw = true;  // Force redraw to remove selection highlighting
    }
}

void delete_selection() {
    if (!editor.selecting) return;
    
    int start_x = editor.select_start_x;
    int start_y = editor.select_start_y;
    int end_x = editor.select_end_x;
    int end_y = editor.select_end_y;
    
    // Normalize selection (ensure start comes before end)
    if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
        int temp_x = start_x, temp_y = start_y;
        start_x = end_x; start_y = end_y;
        end_x = temp_x; end_y = temp_y;
    }
    
    // Delete the selected text character by character using buffer functions
    if (start_y == end_y) {
        // Same line deletion - delete from end to start to maintain positions
        for (int x = end_x - 1; x >= start_x; x--) {
            if (x < (int)strlen(editor.buffer->lines[start_y])) {
                buffer_delete_char(editor.buffer, start_y, x);
            }
        }
    } else {
        // Multi-line deletion
        // Delete from end line backwards to maintain line indices
        
        // First, delete partial content on end line (from start to end_x)
        if (end_y < editor.buffer->line_count && editor.buffer->lines[end_y]) {
            for (int x = end_x - 1; x >= 0; x--) {
                if (x < (int)strlen(editor.buffer->lines[end_y])) {
                    buffer_delete_char(editor.buffer, end_y, x);
                }
            }
        }
        
        // Delete complete lines between start+1 and end (inclusive)
        for (int y = end_y; y > start_y; y--) {
            if (y < editor.buffer->line_count) {
                buffer_delete_line(editor.buffer, y);
            }
        }
        
        // Delete partial content on start line (from start_x to end of line)
        if (start_y < editor.buffer->line_count && editor.buffer->lines[start_y]) {
            int line_len = strlen(editor.buffer->lines[start_y]);
            for (int x = line_len - 1; x >= start_x; x--) {
                buffer_delete_char(editor.buffer, start_y, x);
            }
        }
        
        // Merge the remaining parts if there are lines to merge
        if (start_y < editor.buffer->line_count - 1) {
            buffer_merge_lines(editor.buffer, start_y);
        }
    }
    
    // Move cursor to start of deleted selection
    editor.cursor_x = start_x;
    editor.cursor_y = start_y;
    
    // Clear selection
    clear_selection();
    editor.modified = true;
    editor.needs_full_redraw = true;
}

char *get_selected_text() {
    if (!editor.selecting) return NULL;
    
    int start_x = editor.select_start_x;
    int start_y = editor.select_start_y;
    int end_x = editor.select_end_x;
    int end_y = editor.select_end_y;
    
    if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
        int temp_x = start_x, temp_y = start_y;
        start_x = end_x; start_y = end_y;
        end_x = temp_x; end_y = temp_y;
    }
    
    return buffer_get_text_range(editor.buffer, start_y, start_x, end_y, end_x);
}

void enter_find_mode() {
    editor.find_mode = true;
    clear_selection();  // Clear any existing selection
    if (!editor.search_query) {
        editor.search_query_capacity = 256;
        editor.search_query = malloc(editor.search_query_capacity);
        editor.search_query[0] = '\0';
    }
    editor.search_query_len = 0;
    editor.current_match = 0;
    editor.total_matches = 0;
    editor.needs_full_redraw = true;
}

void exit_find_mode() {
    editor.find_mode = false;
    editor.needs_full_redraw = true;
}

int find_matches() {
    if (!editor.search_query || editor.search_query_len == 0) {
        editor.total_matches = 0;
        editor.current_match = 0;
        return 0;
    }
    
    int matches = 0;
    int current_found = 0;
    bool found_current = false;
    
    for (int y = 0; y < editor.buffer->line_count; y++) {
        char *line = editor.buffer->lines[y];
        if (!line) continue;
        
        char *pos = line;
        while ((pos = strstr(pos, editor.search_query)) != NULL) {
            matches++;
            int x = pos - line;
            
            // Check if this is the closest match to current cursor position
            if (!found_current && (y > editor.cursor_y || 
                (y == editor.cursor_y && x >= editor.cursor_x))) {
                current_found = matches;
                found_current = true;
            }
            
            pos++; // Move past this match to find next one
        }
    }
    
    editor.total_matches = matches;
    editor.current_match = found_current ? current_found : (matches > 0 ? 1 : 0);
    return matches;
}

void jump_to_match(int match_num) {
    if (match_num < 1 || match_num > editor.total_matches) return;
    
    int found = 0;
    for (int y = 0; y < editor.buffer->line_count; y++) {
        char *line = editor.buffer->lines[y];
        if (!line) continue;
        
        char *pos = line;
        while ((pos = strstr(pos, editor.search_query)) != NULL) {
            found++;
            if (found == match_num) {
                int x = pos - line;
                
                // Position cursor at start of match
                editor.cursor_x = x;
                editor.cursor_y = y;
                
                // Select the matched text
                editor.select_start_x = x;
                editor.select_start_y = y;
                editor.select_end_x = x + editor.search_query_len;
                editor.select_end_y = y;
                editor.selecting = true;
                
                // Ensure cursor is visible
                scroll_if_needed();
                editor.current_match = match_num;
                editor.needs_full_redraw = true;
                return;
            }
            pos++;
        }
    }
}

void find_next() {
    if (editor.total_matches == 0) return;
    
    int next = editor.current_match + 1;
    if (next > editor.total_matches) next = 1; // Wrap around
    jump_to_match(next);
}

void find_previous() {
    if (editor.total_matches == 0) return;
    
    int prev = editor.current_match - 1;
    if (prev < 1) prev = editor.total_matches; // Wrap around
    jump_to_match(prev);
}

void handle_mouse(int button, int x, int y, int pressed) {
    // Ignore clicks on line number area
    if (x <= editor.line_number_width) {
        return;
    }
    
    if (button == 0) {  // Left mouse button
        if (pressed) {
            // Mouse button pressed - prepare for potential drag operation
            int buffer_x = x - editor.line_number_width - 1 + editor.offset_x;
            int buffer_y = y - 1 + editor.offset_y;
            
            // Clamp coordinates to valid buffer range
            if (buffer_y >= editor.buffer->line_count) {
                buffer_y = editor.buffer->line_count - 1;
            }
            if (buffer_y < 0) buffer_y = 0;
            
            int line_len = editor.buffer->lines[buffer_y] ? 
                           strlen(editor.buffer->lines[buffer_y]) : 0;
            if (buffer_x > line_len) buffer_x = line_len;
            if (buffer_x < 0) buffer_x = 0;
            
            editor.cursor_x = buffer_x;
            editor.cursor_y = buffer_y;
            editor.mouse_dragging = true;
            editor.mouse_drag_start_x = buffer_x;
            editor.mouse_drag_start_y = buffer_y;
            
            // Clear any existing selection on click
            clear_selection();
        } else {
            // Mouse button released - end drag operation
            if (editor.mouse_dragging) {
                editor.mouse_dragging = false;
            }
        }
    } else if (button == 32) {  // Mouse drag event (button held and moving)
        if (editor.mouse_dragging) {
            // Check for auto-scroll first
            auto_scroll_during_selection(y);
            
            // Convert screen coordinates to buffer coordinates
            int buffer_x = x - editor.line_number_width - 1 + editor.offset_x;
            int buffer_y = y - 1 + editor.offset_y;
            
            // Clamp coordinates to valid buffer range
            if (buffer_y >= editor.buffer->line_count) {
                buffer_y = editor.buffer->line_count - 1;
            }
            if (buffer_y < 0) buffer_y = 0;
            
            int line_len = editor.buffer->lines[buffer_y] ? 
                           strlen(editor.buffer->lines[buffer_y]) : 0;
            if (buffer_x > line_len) buffer_x = line_len;
            if (buffer_x < 0) buffer_x = 0;
            
            // Start selection on first drag movement
            if (!editor.selecting) {
                start_selection();
            }
            
            editor.cursor_x = buffer_x;
            editor.cursor_y = buffer_y;
            update_selection();
        }
    }
}

void save_file() {
    if (!editor.filename) {
        printf("\r\nEnter filename: ");
        fflush(stdout);
        
        char filename[256];
        if (fgets(filename, sizeof(filename), stdin)) {
            filename[strcspn(filename, "\n")] = 0;
            if (strlen(filename) > 0) {
                editor.filename = strdup(filename);
            }
        }
        editor.needs_full_redraw = true;
    }
    
    if (editor.filename && buffer_save_to_file(editor.buffer, editor.filename)) {
        editor.modified = false;
        set_status_message("File saved: %s", editor.filename);
    } else {
        set_status_message("Error: Could not save file!");
    }
}

void insert_char(char c) {
    buffer_insert_char(editor.buffer, editor.cursor_y, editor.cursor_x, c);
    editor.cursor_x++;
    editor.modified = true;
    
    draw_line(editor.cursor_y - editor.offset_y, editor.cursor_y);
}

void delete_char() {
    if (editor.cursor_x > 0) {
        buffer_delete_char(editor.buffer, editor.cursor_y, editor.cursor_x - 1);
        editor.cursor_x--;
        editor.modified = true;
        
        draw_line(editor.cursor_y - editor.offset_y, editor.cursor_y);
    } else if (editor.cursor_y > 0) {
        int prev_line_len = editor.buffer->lines[editor.cursor_y - 1] ? 
                           strlen(editor.buffer->lines[editor.cursor_y - 1]) : 0;
        buffer_merge_lines(editor.buffer, editor.cursor_y - 1);
        editor.cursor_y--;
        editor.cursor_x = prev_line_len;
        editor.modified = true;
        
        editor.needs_full_redraw = true;
    }
}

void insert_newline() {
    buffer_insert_newline(editor.buffer, editor.cursor_y, editor.cursor_x);
    editor.cursor_y++;
    editor.cursor_x = 0;
    editor.modified = true;
    
    editor.needs_full_redraw = true;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGWINCH, handle_resize);
    
    if (!terminal_init()) {
        fprintf(stderr, "Failed to initialize terminal\n");
        return 1;
    }
    
    editor.buffer = buffer_create();
    if (!editor.buffer) {
        terminal_cleanup();
        fprintf(stderr, "Failed to create buffer\n");
        return 1;
    }
    
    if (argc > 1) {
        editor.filename = strdup(argv[1]);
        if (!buffer_load_from_file(editor.buffer, editor.filename)) {
            buffer_insert_line(editor.buffer, 0, "");
        }
    } else {
        buffer_insert_line(editor.buffer, 0, "");
    }
    
    terminal_get_window_size(&editor.screen_rows, &editor.screen_cols);
    terminal_enable_mouse();
    
    editor.line_number_width = 7;  // 6 digits + 1 space
    editor.cursor_x = 0;
    editor.cursor_y = 0;
    editor.needs_full_redraw = true;
    
    if (editor.filename) {
        set_status_message("Loaded file: %s", editor.filename);
    } else {
        set_status_message("New file - Press Ctrl+S to save, Ctrl+Q to quit");
    }
    
    while (1) {
        scroll_if_needed();
        draw_screen();
        
        int c = terminal_read_key();
        
        // Debug: show key codes for any special keys
        if (c > 1000) {
            set_status_message("Key code: %d", c);
        }
        
        if (editor.find_mode) {
            if (c == 27) {  // Escape key
                exit_find_mode();
            } else if (c == CTRL_KEY('n')) {
                find_next();
            } else if (c == CTRL_KEY('p')) {
                find_previous();
            } else if (c == 127 || c == CTRL_KEY('h')) {  // Backspace
                if (editor.search_query_len > 0) {
                    editor.search_query_len--;
                    editor.search_query[editor.search_query_len] = '\0';
                    find_matches();
                    if (editor.total_matches > 0) {
                        jump_to_match(editor.current_match);
                    }
                }
            } else if (c >= 32 && c < 127) {  // Printable characters
                if (editor.search_query_len < editor.search_query_capacity - 1) {
                    editor.search_query[editor.search_query_len] = c;
                    editor.search_query_len++;
                    editor.search_query[editor.search_query_len] = '\0';
                    find_matches();
                    if (editor.total_matches > 0) {
                        jump_to_match(editor.current_match);
                    }
                }
            }
        } else if (c == CTRL_KEY('f')) {
            enter_find_mode();
        } else if (c == CTRL_KEY('q')) {
            break;
        } else if (c == CTRL_KEY('s')) {
            save_file();
        } else if (c == CTRL_KEY('c')) {
            char *selected = get_selected_text();
            if (selected) {
                clipboard_set(selected);
                free(selected);
                set_status_message("Copied to clipboard");
            }
        } else if (c == CTRL_KEY('x')) {
            char *selected = get_selected_text();
            if (selected) {
                clipboard_set(selected);
                free(selected);
                clear_selection();
                editor.modified = true;
                set_status_message("Cut to clipboard");
            }
        } else if (c == CTRL_KEY('v')) {
            char *clipboard = clipboard_get();
            if (clipboard) {
                if (editor.selecting) {
                    clear_selection();
                }
                for (char *p = clipboard; *p; p++) {
                    if (*p == '\n') {
                        insert_newline();
                    } else {
                        insert_char(*p);
                    }
                }
                free(clipboard);
                set_status_message("Pasted from clipboard");
            }
        } else if (c == CTRL_KEY('a')) {
            editor.select_start_x = 0;
            editor.select_start_y = 0;
            editor.select_end_y = editor.buffer->line_count - 1;
            editor.select_end_x = editor.buffer->lines[editor.select_end_y] ? 
                                 strlen(editor.buffer->lines[editor.select_end_y]) : 0;
            editor.selecting = true;
            editor.needs_full_redraw = true;
            set_status_message("Selected all text");
        } else if (c == '\r' || c == '\n') {
            if (editor.selecting) {
                delete_selection();
            }
            insert_newline();
        } else if (c == 127 || c == CTRL_KEY('h')) {
            if (editor.selecting) {
                delete_selection();
            } else {
                delete_char();
            }
        } else if (c == ARROW_UP) {
            clear_selection();
            move_cursor(0, -1);
        } else if (c == ARROW_DOWN) {
            clear_selection();
            move_cursor(0, 1);
        } else if (c == ARROW_LEFT) {
            if (editor.selecting) {
                // If we're at the start of selection, just clear it without moving
                int end_x = editor.select_end_x;
                int end_y = editor.select_end_y;
                int start_x = editor.select_start_x;
                int start_y = editor.select_start_y;
                
                // Normalize selection coordinates
                if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
                    int temp_x = start_x, temp_y = start_y;
                    start_x = end_x; start_y = end_y;
                    end_x = temp_x; end_y = temp_y;
                }
                
                // If cursor is at the start of selection, just clear without moving
                if (editor.cursor_x == start_x && editor.cursor_y == start_y) {
                    clear_selection();
                } else {
                    // Otherwise, clear and move
                    clear_selection();
                    move_cursor(-1, 0);
                }
            } else {
                move_cursor(-1, 0);
            }
        } else if (c == ARROW_RIGHT) {
            if (editor.selecting) {
                // If we're at the end of selection, just clear it without moving
                int end_x = editor.select_end_x;
                int end_y = editor.select_end_y;
                int start_x = editor.select_start_x;
                int start_y = editor.select_start_y;
                
                // Normalize selection coordinates
                if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
                    int temp_x = start_x, temp_y = start_y;
                    start_x = end_x; start_y = end_y;
                    end_x = temp_x; end_y = temp_y;
                }
                
                // If cursor is at the end of selection, just clear without moving
                if (editor.cursor_x == end_x && editor.cursor_y == end_y) {
                    clear_selection();
                } else {
                    // Otherwise, clear and move
                    clear_selection();
                    move_cursor(1, 0);
                }
            } else {
                move_cursor(1, 0);
            }
        } else if (c == SHIFT_ARROW_UP) {
            if (!editor.selecting) start_selection();
            move_cursor(0, -1);
            update_selection();
        } else if (c == SHIFT_ARROW_DOWN) {
            if (!editor.selecting) start_selection();
            move_cursor(0, 1);
            update_selection();
        } else if (c == SHIFT_ARROW_LEFT) {
            if (!editor.selecting) start_selection();
            move_cursor(-1, 0);
            update_selection();
        } else if (c == SHIFT_ARROW_RIGHT) {
            if (!editor.selecting) start_selection();
            move_cursor(1, 0);
            update_selection();
        } else if (c == CTRL_ARROW_LEFT) {
            clear_selection();
            move_cursor_word_left();
        } else if (c == CTRL_ARROW_RIGHT) {
            clear_selection();
            move_cursor_word_right();
        } else if (c == SHIFT_CTRL_ARROW_LEFT) {
            if (!editor.selecting) start_selection();
            move_cursor_word_left();
            update_selection();
        } else if (c == SHIFT_CTRL_ARROW_RIGHT) {
            if (!editor.selecting) start_selection();
            move_cursor_word_right();
            update_selection();
        } else if (c == HOME_KEY) {
            clear_selection();
            editor.cursor_x = 0;
        } else if (c == END_KEY) {
            clear_selection();
            int line_len = editor.buffer->lines[editor.cursor_y] ? 
                          strlen(editor.buffer->lines[editor.cursor_y]) : 0;
            editor.cursor_x = line_len;
        } else if (c == PAGE_UP) {
            clear_selection();
            int move_lines = editor.screen_rows - 2;
            if (move_lines < 1) move_lines = 1;
            move_cursor(0, -move_lines);
        } else if (c == PAGE_DOWN) {
            clear_selection();
            int move_lines = editor.screen_rows - 2;
            if (move_lines < 1) move_lines = 1;
            move_cursor(0, move_lines);
        } else if (c >= 32 && c < 127) {
            if (editor.selecting) {
                delete_selection();
            }
            insert_char(c);
        }
    }
    
    cleanup_and_exit(0);
    return 0;
}