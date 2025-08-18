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
#include <sys/select.h>
#include <unistd.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include "terminal.h"
#include "buffer.h"
#include "clipboard.h"

typedef struct {
    TextBuffer *buffer;
    int cursor_x, cursor_y;
    int offset_x, offset_y;
    int select_start_x, select_start_y;
    int select_end_x, select_end_y;
    bool selecting;
    bool modified;
    char *filename;
    int last_cursor_x, last_cursor_y;
    int last_offset_x, last_offset_y;
} Tab;

typedef struct {
    Tab *tabs;
    int tab_count;
    int tab_capacity;
    int current_tab;
    
    int screen_rows, screen_cols;
    bool needs_full_redraw;
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
    bool filename_input_mode;
    char *filename_input;
    int filename_input_len;
    int filename_input_capacity;
    volatile bool resize_pending;
    
    // File manager state
    bool file_manager_visible;
    bool file_manager_overlay_mode; // true = overlay, false = reduce text width
    int file_manager_width;
    char *current_directory;
    char **file_list;
    int file_count;
    int file_capacity;
    int file_manager_cursor;
    int file_manager_offset;
    bool file_manager_focused;
} Editor;

Editor editor = {0};


// Function declarations
int get_file_size(void);
const char* format_file_size(int bytes);
Tab* get_current_tab(void);
int create_new_tab(const char* filename);
void close_tab(int tab_index);
void switch_to_tab(int tab_index);
void free_tab(Tab* tab);
void draw_tab_bar(void);
void switch_to_next_tab(void);
void switch_to_prev_tab(void);
void enter_filename_input_mode(void);
void exit_filename_input_mode(void);
void process_filename_input(void);
void clear_selection(void);
void toggle_file_manager(void);
void refresh_file_list(void);
void draw_file_manager(void);
void file_manager_navigate(int direction);
void file_manager_select_item(void);
void free_file_list(void);
const char* get_file_size_str(const char* filepath);
bool is_directory(const char* filepath);

Tab* get_current_tab(void) {
    if (editor.current_tab >= 0 && editor.current_tab < editor.tab_count) {
        return &editor.tabs[editor.current_tab];
    }
    return NULL;
}

int create_new_tab(const char* filename) {
    // Expand tab array if needed
    if (editor.tab_count >= editor.tab_capacity) {
        int new_capacity = editor.tab_capacity == 0 ? 4 : editor.tab_capacity * 2;
        Tab* new_tabs = realloc(editor.tabs, new_capacity * sizeof(Tab));
        if (!new_tabs) return -1;
        editor.tabs = new_tabs;
        editor.tab_capacity = new_capacity;
    }
    
    // Initialize new tab
    Tab* tab = &editor.tabs[editor.tab_count];
    memset(tab, 0, sizeof(Tab));
    
    tab->buffer = buffer_create();
    if (!tab->buffer) return -1;
    
    if (filename) {
        tab->filename = strdup(filename);
        if (!buffer_load_from_file(tab->buffer, tab->filename)) {
            buffer_insert_line(tab->buffer, 0, "");
        }
    } else {
        buffer_insert_line(tab->buffer, 0, "");
    }
    
    editor.tab_count++;
    return editor.tab_count - 1;
}

void free_tab(Tab* tab) {
    if (tab->buffer) {
        buffer_free(tab->buffer);
        tab->buffer = NULL;
    }
    if (tab->filename) {
        free(tab->filename);
        tab->filename = NULL;
    }
}

void close_tab(int tab_index) {
    if (tab_index < 0 || tab_index >= editor.tab_count) return;
    
    // Don't close the last tab
    if (editor.tab_count <= 1) return;
    
    // Free the tab
    free_tab(&editor.tabs[tab_index]);
    
    // Shift remaining tabs down
    for (int i = tab_index; i < editor.tab_count - 1; i++) {
        editor.tabs[i] = editor.tabs[i + 1];
    }
    editor.tab_count--;
    
    // Adjust current tab index
    if (editor.current_tab >= editor.tab_count) {
        editor.current_tab = editor.tab_count - 1;
    } else if (editor.current_tab > tab_index) {
        editor.current_tab--;
    }
    
    editor.needs_full_redraw = true;
}

void switch_to_tab(int tab_index) {
    if (tab_index < 0 || tab_index >= editor.tab_count) return;
    if (tab_index == editor.current_tab) return;
    
    editor.current_tab = tab_index;
    editor.needs_full_redraw = true;
}

void cleanup_and_exit(int status) {
    terminal_cleanup();
    for (int i = 0; i < editor.tab_count; i++) {
        free_tab(&editor.tabs[i]);
    }
    if (editor.tabs) free(editor.tabs);
    if (editor.status_message) free(editor.status_message);
    if (editor.search_query) free(editor.search_query);
    if (editor.filename_input) free(editor.filename_input);
    if (editor.current_directory) free(editor.current_directory);
    free_file_list();
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
    // Just set a flag - do the actual resize handling in main loop
    editor.resize_pending = true;
    // Re-register the signal handler (some systems need this)
    signal(SIGWINCH, handle_resize);
}

void process_resize() {
    if (!editor.resize_pending) return;
    
    // Get new window dimensions
    int old_rows = editor.screen_rows;
    int old_cols = editor.screen_cols;
    
    // Update to current size
    terminal_get_window_size(&editor.screen_rows, &editor.screen_cols);
    
    // Sanity check - ensure we have valid dimensions
    if (editor.screen_rows < 3) editor.screen_rows = 3; // Minimum for content + status
    if (editor.screen_cols < 10) editor.screen_cols = 10; // Minimum for line numbers + text
    
    // Adjust scrolling if needed after resize
    if (editor.screen_rows != old_rows || editor.screen_cols != old_cols) {
        Tab* tab = get_current_tab();
        if (!tab) return;
        
        // Ensure cursor position is valid
        if (tab->cursor_y >= tab->buffer->line_count) {
            tab->cursor_y = tab->buffer->line_count - 1;
            if (tab->cursor_y < 0) tab->cursor_y = 0;
        }
        
        // Adjust cursor x to line length
        if (tab->cursor_y >= 0 && tab->cursor_y < tab->buffer->line_count) {
            int line_len = tab->buffer->lines[tab->cursor_y] ? 
                           strlen(tab->buffer->lines[tab->cursor_y]) : 0;
            if (tab->cursor_x > line_len) tab->cursor_x = line_len;
            if (tab->cursor_x < 0) tab->cursor_x = 0;
        }
        
        // Make sure cursor is still visible after resize
        if (tab->cursor_y < tab->offset_y) {
            tab->offset_y = tab->cursor_y;
        } else if (tab->cursor_y >= tab->offset_y + editor.screen_rows - 2) {
            tab->offset_y = tab->cursor_y - editor.screen_rows + 3;
            if (tab->offset_y < 0) tab->offset_y = 0;
        }
        
        // Adjust horizontal scrolling
        int text_width = editor.screen_cols - editor.line_number_width;
        if (text_width <= 0) text_width = 1; // Prevent division by zero
        
        if (tab->cursor_x < tab->offset_x) {
            tab->offset_x = tab->cursor_x;
        } else if (tab->cursor_x >= tab->offset_x + text_width) {
            tab->offset_x = tab->cursor_x - text_width + 1;
            if (tab->offset_x < 0) tab->offset_x = 0;
        }
        
        // Force complete redraw and reset cached positions
        editor.needs_full_redraw = true;
        tab->last_offset_x = -1;
        tab->last_offset_y = -1;
        tab->last_cursor_x = -1;
        tab->last_cursor_y = -1;
    }
    
    editor.resize_pending = false;
}

void move_cursor(int dx, int dy) {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    if (dy != 0) {
        int new_y = tab->cursor_y + dy;
        
        if (new_y < 0) {
            new_y = 0;
        }
        if (new_y >= tab->buffer->line_count) {
            new_y = tab->buffer->line_count - 1;
        }
        
        if (new_y != tab->cursor_y) {
            tab->cursor_y = new_y;
            int line_len = tab->buffer->lines[tab->cursor_y] ? 
                           strlen(tab->buffer->lines[tab->cursor_y]) : 0;
            if (tab->cursor_x > line_len) {
                tab->cursor_x = line_len;
            }
        }
    }
    
    if (dx != 0) {
        int line_len = tab->buffer->lines[tab->cursor_y] ? 
                       strlen(tab->buffer->lines[tab->cursor_y]) : 0;
        
        if (dx < 0 && tab->cursor_x == 0) {
            if (tab->cursor_y > 0) {
                tab->cursor_y--;
                int prev_line_len = tab->buffer->lines[tab->cursor_y] ? 
                                   strlen(tab->buffer->lines[tab->cursor_y]) : 0;
                tab->cursor_x = prev_line_len;
            }
        } else if (dx > 0 && tab->cursor_x >= line_len) {
            if (tab->cursor_y < tab->buffer->line_count - 1) {
                tab->cursor_y++;
                tab->cursor_x = 0;
            }
        } else {
            int new_x = tab->cursor_x + dx;
            if (new_x < 0) new_x = 0;
            if (new_x > line_len) new_x = line_len;
            tab->cursor_x = new_x;
        }
    }
}

void scroll_if_needed() {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    if (tab->cursor_y < tab->offset_y) {
        tab->offset_y = tab->cursor_y;
    }
    if (tab->cursor_y >= tab->offset_y + editor.screen_rows - 2) {
        tab->offset_y = tab->cursor_y - editor.screen_rows + 3;
    }
    
    if (tab->cursor_x < tab->offset_x) {
        tab->offset_x = tab->cursor_x;
    }
    int text_width = editor.screen_cols - editor.line_number_width;
    if (tab->cursor_x >= tab->offset_x + text_width) {
        tab->offset_x = tab->cursor_x - text_width + 1;
    }
}

void auto_scroll_during_selection(int screen_y) {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    bool scrolled = false;
    
    // Check if we need to scroll up (dragging above visible area)
    if (screen_y <= 2 && tab->offset_y > 0) {
        tab->offset_y--;
        tab->cursor_y = tab->offset_y;
        scrolled = true;
    }
    // Check if we need to scroll down (dragging below visible area)  
    else if (screen_y >= editor.screen_rows - 1 && 
             tab->offset_y + editor.screen_rows - 2 < tab->buffer->line_count - 1) {
        tab->offset_y++;
        tab->cursor_y = tab->offset_y + editor.screen_rows - 3;
        scrolled = true;
    }
    
    if (scrolled) {
        // Ensure cursor is within buffer bounds
        if (tab->cursor_y < 0) tab->cursor_y = 0;
        if (tab->cursor_y >= tab->buffer->line_count) {
            tab->cursor_y = tab->buffer->line_count - 1;
        }
        
        // Adjust cursor x position to line length
        int line_len = tab->buffer->lines[tab->cursor_y] ? 
                       strlen(tab->buffer->lines[tab->cursor_y]) : 0;
        if (tab->cursor_x > line_len) tab->cursor_x = line_len;
        
        editor.needs_full_redraw = true;
    }
}

bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
           (c >= '0' && c <= '9') || c == '_';
}

void move_cursor_word_right() {
    Tab* tab = get_current_tab();
    if (!tab || tab->cursor_y >= tab->buffer->line_count) return;
    
    char *line = tab->buffer->lines[tab->cursor_y];
    if (!line) return;
    
    int len = strlen(line);
    
    // If at end of line, move to next line
    if (tab->cursor_x >= len) {
        if (tab->cursor_y < tab->buffer->line_count - 1) {
            tab->cursor_y++;
            tab->cursor_x = 0;
            // Skip leading whitespace on new line
            line = tab->buffer->lines[tab->cursor_y];
            if (line) {
                len = strlen(line);
                while (tab->cursor_x < len && !is_word_char(line[tab->cursor_x])) {
                    tab->cursor_x++;
                }
                // Now move to END of this word
                while (tab->cursor_x < len && is_word_char(line[tab->cursor_x])) {
                    tab->cursor_x++;
                }
            }
        }
        return;
    }
    
    // If currently in whitespace, move to start then end of next word
    if (!is_word_char(line[tab->cursor_x])) {
        while (tab->cursor_x < len && !is_word_char(line[tab->cursor_x])) {
            tab->cursor_x++;
        }
        // Now at start of word, move to end of word
        while (tab->cursor_x < len && is_word_char(line[tab->cursor_x])) {
            tab->cursor_x++;
        }
    } else {
        // If currently in a word, move to end of current word and stop there
        while (tab->cursor_x < len && is_word_char(line[tab->cursor_x])) {
            tab->cursor_x++;
        }
        // Don't continue to next word - stop at end of current word
    }
    
    // If we've reached end of line, that's fine - stay there
}

void move_cursor_word_left() {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    char *line = tab->buffer->lines[tab->cursor_y];
    if (!line) return;
    
    // If at start of line, move to end of previous line
    if (tab->cursor_x == 0) {
        if (tab->cursor_y > 0) {
            tab->cursor_y--;
            line = tab->buffer->lines[tab->cursor_y];
            tab->cursor_x = line ? strlen(line) : 0;
            // Skip trailing whitespace on previous line
            while (tab->cursor_x > 0 && !is_word_char(line[tab->cursor_x - 1])) {
                tab->cursor_x--;
            }
            // Move to start of the word we're now at the end of
            while (tab->cursor_x > 0 && is_word_char(line[tab->cursor_x - 1])) {
                tab->cursor_x--;
            }
        }
        return;
    }
    
    // Move back one character to start
    tab->cursor_x--;
    
    // Skip trailing whitespace (move backwards through non-word chars)
    while (tab->cursor_x > 0 && !is_word_char(line[tab->cursor_x])) {
        tab->cursor_x--;
    }
    
    // If we're now on a word character, move to start of this word
    if (tab->cursor_x >= 0 && is_word_char(line[tab->cursor_x])) {
        while (tab->cursor_x > 0 && is_word_char(line[tab->cursor_x - 1])) {
            tab->cursor_x--;
        }
    }
}

void draw_line(int screen_y, int file_y, int start_col) {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    terminal_set_cursor_position(screen_y + 2, start_col);
    printf("\033[K");
    
    if (file_y < tab->buffer->line_count) {
        printf("\033[36m%6d\033[0m ", file_y + 1);
        
        if (tab->buffer->lines[file_y]) {
            char *line = tab->buffer->lines[file_y];
            int len = strlen(line);
            int start_x = tab->offset_x;
            int display_len = editor.screen_cols - editor.line_number_width;
            
            // Check if this line has any selection
            bool line_has_selection = false;
            int sel_start_x = 0, sel_start_y = 0, sel_end_x = 0, sel_end_y = 0;
            
            if (tab->selecting) {
                sel_start_x = tab->select_start_x;
                sel_start_y = tab->select_start_y;
                sel_end_x = tab->select_end_x;
                sel_end_y = tab->select_end_y;
                
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

void draw_tab_bar() {
    terminal_set_cursor_position(1, 1);
    printf("\033[K\033[7m"); // Clear line and reverse video
    
    int col = 1;
    for (int i = 0; i < editor.tab_count; i++) {
        Tab* tab = &editor.tabs[i];
        const char* filename = tab->filename ? tab->filename : "untitled";
        
        // Extract just the filename from path
        const char* basename = filename;
        const char* slash = strrchr(filename, '/');
        if (slash) basename = slash + 1;
        
        // Highlight current tab
        if (i == editor.current_tab) {
            printf("\033[0m\033[47m\033[30m"); // White background, black text
            printf(" >%d:%s%s< ", i + 1, basename, tab->modified ? "*" : "");
        } else {
            printf("\033[0m\033[7m"); // Normal reverse video
            printf(" %d:%s%s ", i + 1, basename, tab->modified ? "*" : "");
        }
        
        if (i == editor.current_tab) {
            col += strlen(basename) + (tab->modified ? 8 : 7); // Account for >, number, colon, spaces, <, and optional *
        } else {
            col += strlen(basename) + (tab->modified ? 6 : 5); // Account for number, colon, spaces, and optional *
        }
        
        if (col >= editor.screen_cols - 10) {
            printf("...");
            break;
        }
    }
    
    printf("\033[0m"); // Reset formatting
}

void switch_to_next_tab(void) {
    if (editor.tab_count <= 1) return;
    int next_tab = (editor.current_tab + 1) % editor.tab_count;
    switch_to_tab(next_tab);
    set_status_message("Switched to tab %d", next_tab + 1);
}

void switch_to_prev_tab(void) {
    if (editor.tab_count <= 1) return;
    int prev_tab = (editor.current_tab - 1 + editor.tab_count) % editor.tab_count;
    switch_to_tab(prev_tab);
    set_status_message("Switched to tab %d", prev_tab + 1);
}

void enter_filename_input_mode(void) {
    editor.filename_input_mode = true;
    clear_selection();  // Clear any existing selection
    if (!editor.filename_input) {
        editor.filename_input_capacity = 256;
        editor.filename_input = malloc(editor.filename_input_capacity);
        editor.filename_input[0] = '\0';
    }
    editor.filename_input_len = 0;
    editor.needs_full_redraw = true;
}

void exit_filename_input_mode(void) {
    editor.filename_input_mode = false;
    editor.needs_full_redraw = true;
}

void process_filename_input(void) {
    if (editor.filename_input_len > 0) {
        editor.filename_input[editor.filename_input_len] = '\0';
        int new_tab = create_new_tab(editor.filename_input);
        if (new_tab >= 0) {
            switch_to_tab(new_tab);
            set_status_message("Opened %s in tab %d", editor.filename_input, new_tab + 1);
        } else {
            set_status_message("Error: Could not open file %s", editor.filename_input);
        }
    }
    exit_filename_input_mode();
}

void draw_status_line() {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
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
    } else if (editor.filename_input_mode) {
        printf("Open file: %s", editor.filename_input ? editor.filename_input : "");
        printf("  (Enter: open, Esc: cancel)");
    } else {
        time_t now = time(NULL);
        if (editor.status_message && (now - editor.status_message_time < 3)) {
            printf("%s", editor.status_message);
        } else {
            // Show comprehensive file information
            const char* filename = tab->filename ? tab->filename : "untitled";
            int current_line = tab->cursor_y + 1; // 1-based line numbers
            int total_lines = tab->buffer->line_count;
            int file_size = get_file_size();
            const char* size_str = format_file_size(file_size);
            const char* modified_str = tab->modified ? " [modified]" : "";
            
            if (editor.file_manager_visible && editor.file_manager_focused) {
                printf("%s  Line %d/%d  %s%s  [FILE MANAGER FOCUSED - Tab to switch]", 
                       filename, current_line, total_lines, size_str, modified_str);
            } else {
                printf("%s  Line %d/%d  %s%s", 
                       filename, current_line, total_lines, size_str, modified_str);
            }
        }
    }
    
    printf(" \033[0m");
}

int get_file_size() {
    Tab* tab = get_current_tab();
    if (!tab) return 0;
    
    int size = 0;
    for (int i = 0; i < tab->buffer->line_count; i++) {
        if (tab->buffer->lines[i]) {
            size += strlen(tab->buffer->lines[i]) + 1; // +1 for newline
        } else {
            size += 1; // empty line still has newline
        }
    }
    return size;
}

const char* format_file_size(int bytes) {
    static char size_str[32];
    if (bytes < 1024) {
        snprintf(size_str, sizeof(size_str), "%dB", bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(size_str, sizeof(size_str), "%.1fK", bytes / 1024.0);
    } else {
        snprintf(size_str, sizeof(size_str), "%.1fM", bytes / (1024.0 * 1024.0));
    }
    return size_str;
}

void draw_screen() {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    bool offset_changed = (tab->offset_x != tab->last_offset_x || 
                          tab->offset_y != tab->last_offset_y);
    
    if (editor.needs_full_redraw || offset_changed) {
        if (editor.needs_full_redraw) {
            terminal_clear_screen();
        }
        
        // Draw tab bar
        draw_tab_bar();
        
        // Draw file manager if visible
        if (editor.file_manager_visible) {
            draw_file_manager();
        }
        
        // Calculate text area position and width
        int text_start_col = 1;
        int text_width = editor.screen_cols;
        
        if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
            text_start_col += editor.file_manager_width + 1; // +1 for border
            text_width -= editor.file_manager_width + 1;
        }
        
        // Draw content (screen_rows - 2 to account for tab bar and status line)
        for (int y = 0; y < editor.screen_rows - 2; y++) {
            int file_y = y + tab->offset_y;
            // Temporarily adjust draw_line to handle file manager offset
            if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
                terminal_set_cursor_position(y + 2, text_start_col);
            }
            draw_line(y, file_y, text_start_col);
        }
        
        draw_status_line();
        editor.needs_full_redraw = false;
        tab->last_offset_x = tab->offset_x;
        tab->last_offset_y = tab->offset_y;
    } else {
        // Just update dynamic parts without full redraw
        if (editor.file_manager_visible) {
            draw_file_manager();
        }
        draw_status_line();
    }
    
    // Show/hide cursor based on mode and selection state
    if (editor.find_mode) {
        terminal_show_cursor();
        // Position cursor at end of search query in status line
        int cursor_col = 8 + editor.search_query_len;  // "Find: " + query length
        terminal_set_cursor_position(editor.screen_rows, cursor_col);
    } else if (editor.filename_input_mode) {
        terminal_show_cursor();
        // Position cursor at end of filename input in status line
        int cursor_col = 13 + editor.filename_input_len;  // "Open file: " + input length
        terminal_set_cursor_position(editor.screen_rows, cursor_col);
    } else if (!tab->selecting) {
        terminal_show_cursor();
        
        // Calculate text area starting column (same logic as in draw_screen)
        int text_start_col = 1;
        if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
            text_start_col += editor.file_manager_width + 1; // +1 for border
        }
        
        // Simple, direct cursor positioning (adjust for tab bar and file manager)
        int screen_row = (tab->cursor_y - tab->offset_y) + 2;  // +2 for tab bar
        int screen_col = (tab->cursor_x - tab->offset_x) + text_start_col + 7;  // 7 for line numbers
        
        // Make sure we're in the valid text area
        if (screen_row < 2) screen_row = 2;  // Account for tab bar
        if (screen_row >= editor.screen_rows) screen_row = editor.screen_rows - 1;
        if (screen_col < text_start_col + 7) screen_col = text_start_col + 7;
        
        terminal_set_cursor_position(screen_row, screen_col);
    } else {
        terminal_hide_cursor();
    }
    
    tab->last_cursor_x = tab->cursor_x;
    tab->last_cursor_y = tab->cursor_y;
    
    fflush(stdout);
}

void start_selection() {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    tab->select_start_x = tab->cursor_x;
    tab->select_start_y = tab->cursor_y;
    tab->select_end_x = tab->cursor_x;
    tab->select_end_y = tab->cursor_y;
    tab->selecting = true;
    editor.needs_full_redraw = true;  // Force redraw to show selection
}

void update_selection() {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    if (tab->selecting) {
        tab->select_end_x = tab->cursor_x;
        tab->select_end_y = tab->cursor_y;
        editor.needs_full_redraw = true;  // Force redraw to show selection
    }
}

void clear_selection() {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    if (tab->selecting) {
        tab->selecting = false;
        editor.needs_full_redraw = true;  // Force redraw to remove selection highlighting
    }
}

void delete_selection() {
    Tab* tab = get_current_tab();
    if (!tab || !tab->selecting) return;
    
    int start_x = tab->select_start_x;
    int start_y = tab->select_start_y;
    int end_x = tab->select_end_x;
    int end_y = tab->select_end_y;
    
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
            if (x < (int)strlen(tab->buffer->lines[start_y])) {
                buffer_delete_char(tab->buffer, start_y, x);
            }
        }
    } else {
        // Multi-line deletion
        // Delete from end line backwards to maintain line indices
        
        // First, delete partial content on end line (from start to end_x)
        if (end_y < tab->buffer->line_count && tab->buffer->lines[end_y]) {
            for (int x = end_x - 1; x >= 0; x--) {
                if (x < (int)strlen(tab->buffer->lines[end_y])) {
                    buffer_delete_char(tab->buffer, end_y, x);
                }
            }
        }
        
        // Delete complete lines between start+1 and end (inclusive)
        for (int y = end_y; y > start_y; y--) {
            if (y < tab->buffer->line_count) {
                buffer_delete_line(tab->buffer, y);
            }
        }
        
        // Delete partial content on start line (from start_x to end of line)
        if (start_y < tab->buffer->line_count && tab->buffer->lines[start_y]) {
            int line_len = strlen(tab->buffer->lines[start_y]);
            for (int x = line_len - 1; x >= start_x; x--) {
                buffer_delete_char(tab->buffer, start_y, x);
            }
        }
        
        // Merge the remaining parts if there are lines to merge
        if (start_y < tab->buffer->line_count - 1) {
            buffer_merge_lines(tab->buffer, start_y);
        }
    }
    
    // Move cursor to start of deleted selection
    tab->cursor_x = start_x;
    tab->cursor_y = start_y;
    
    // Clear selection
    clear_selection();
    tab->modified = true;
    editor.needs_full_redraw = true;
}

char *get_selected_text() {
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
    
    return buffer_get_text_range(tab->buffer, start_y, start_x, end_y, end_x);
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
    Tab* tab = get_current_tab();
    if (!tab || !editor.search_query || editor.search_query_len == 0) {
        editor.total_matches = 0;
        editor.current_match = 0;
        return 0;
    }
    
    int matches = 0;
    int current_found = 0;
    bool found_current = false;
    
    for (int y = 0; y < tab->buffer->line_count; y++) {
        char *line = tab->buffer->lines[y];
        if (!line) continue;
        
        char *pos = line;
        while ((pos = strstr(pos, editor.search_query)) != NULL) {
            matches++;
            int x = pos - line;
            
            // Check if this is the closest match to current cursor position
            if (!found_current && (y > tab->cursor_y || 
                (y == tab->cursor_y && x >= tab->cursor_x))) {
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
    Tab* tab = get_current_tab();
    if (!tab || match_num < 1 || match_num > editor.total_matches) return;
    
    int found = 0;
    for (int y = 0; y < tab->buffer->line_count; y++) {
        char *line = tab->buffer->lines[y];
        if (!line) continue;
        
        char *pos = line;
        while ((pos = strstr(pos, editor.search_query)) != NULL) {
            found++;
            if (found == match_num) {
                int x = pos - line;
                
                // Position cursor at start of match
                tab->cursor_x = x;
                tab->cursor_y = y;
                
                // Select the matched text
                tab->select_start_x = x;
                tab->select_start_y = y;
                tab->select_end_x = x + editor.search_query_len;
                tab->select_end_y = y;
                tab->selecting = true;
                
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
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    // Ignore clicks on tab bar
    if (y <= 1) return;
    
    // Ignore clicks on line number area
    if (x <= editor.line_number_width) {
        return;
    }
    
    if (button == 0) {  // Left mouse button
        if (pressed) {
            // Mouse button pressed - prepare for potential drag operation
            int buffer_x = x - editor.line_number_width - 1 + tab->offset_x;
            int buffer_y = y - 2 + tab->offset_y;  // -2 for tab bar
            
            // Clamp coordinates to valid buffer range
            if (buffer_y >= tab->buffer->line_count) {
                buffer_y = tab->buffer->line_count - 1;
            }
            if (buffer_y < 0) buffer_y = 0;
            
            int line_len = tab->buffer->lines[buffer_y] ? 
                           strlen(tab->buffer->lines[buffer_y]) : 0;
            if (buffer_x > line_len) buffer_x = line_len;
            if (buffer_x < 0) buffer_x = 0;
            
            tab->cursor_x = buffer_x;
            tab->cursor_y = buffer_y;
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
            int buffer_x = x - editor.line_number_width - 1 + tab->offset_x;
            int buffer_y = y - 2 + tab->offset_y;  // -2 for tab bar
            
            // Clamp coordinates to valid buffer range
            if (buffer_y >= tab->buffer->line_count) {
                buffer_y = tab->buffer->line_count - 1;
            }
            if (buffer_y < 0) buffer_y = 0;
            
            int line_len = tab->buffer->lines[buffer_y] ? 
                           strlen(tab->buffer->lines[buffer_y]) : 0;
            if (buffer_x > line_len) buffer_x = line_len;
            if (buffer_x < 0) buffer_x = 0;
            
            // Start selection on first drag movement
            if (!tab->selecting) {
                start_selection();
            }
            
            tab->cursor_x = buffer_x;
            tab->cursor_y = buffer_y;
            update_selection();
        }
    }
}

void save_file() {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    if (!tab->filename) {
        printf("\r\nEnter filename: ");
        fflush(stdout);
        
        char filename[256];
        if (fgets(filename, sizeof(filename), stdin)) {
            filename[strcspn(filename, "\n")] = 0;
            if (strlen(filename) > 0) {
                tab->filename = strdup(filename);
            }
        }
        editor.needs_full_redraw = true;
    }
    
    if (tab->filename && buffer_save_to_file(tab->buffer, tab->filename)) {
        tab->modified = false;
        set_status_message("File saved: %s", tab->filename);
    } else {
        set_status_message("Error: Could not save file!");
    }
}

void insert_char(char c) {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    buffer_insert_char(tab->buffer, tab->cursor_y, tab->cursor_x, c);
    tab->cursor_x++;
    tab->modified = true;
    
    // Calculate text start column for consistent drawing
    int text_start_col = 1;
    if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
        text_start_col += editor.file_manager_width + 1;
    }
    draw_line(tab->cursor_y - tab->offset_y, tab->cursor_y, text_start_col);
}

void delete_char() {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    if (tab->cursor_x > 0) {
        buffer_delete_char(tab->buffer, tab->cursor_y, tab->cursor_x - 1);
        tab->cursor_x--;
        tab->modified = true;
        
        // Calculate text start column for consistent drawing
        int text_start_col = 1;
        if (editor.file_manager_visible && !editor.file_manager_overlay_mode) {
            text_start_col += editor.file_manager_width + 1;
        }
        draw_line(tab->cursor_y - tab->offset_y, tab->cursor_y, text_start_col);
    } else if (tab->cursor_y > 0) {
        int prev_line_len = tab->buffer->lines[tab->cursor_y - 1] ? 
                           strlen(tab->buffer->lines[tab->cursor_y - 1]) : 0;
        buffer_merge_lines(tab->buffer, tab->cursor_y - 1);
        tab->cursor_y--;
        tab->cursor_x = prev_line_len;
        tab->modified = true;
        
        editor.needs_full_redraw = true;
    }
}

void insert_newline() {
    Tab* tab = get_current_tab();
    if (!tab) return;
    
    buffer_insert_newline(tab->buffer, tab->cursor_y, tab->cursor_x);
    tab->cursor_y++;
    tab->cursor_x = 0;
    tab->modified = true;
    
    editor.needs_full_redraw = true;
}

bool is_directory(const char* filepath) {
    struct stat statbuf;
    if (stat(filepath, &statbuf) != 0) {
        return false;
    }
    return S_ISDIR(statbuf.st_mode);
}

const char* get_file_size_str(const char* filepath) {
    static char size_str[32];
    struct stat statbuf;
    
    if (stat(filepath, &statbuf) != 0) {
        return "---";
    }
    
    if (S_ISDIR(statbuf.st_mode)) {
        return "<DIR>";
    }
    
    long size = statbuf.st_size;
    if (size < 1024) {
        snprintf(size_str, sizeof(size_str), "%ldB", size);
    } else if (size < 1024 * 1024) {
        snprintf(size_str, sizeof(size_str), "%.1fK", size / 1024.0);
    } else {
        snprintf(size_str, sizeof(size_str), "%.1fM", size / (1024.0 * 1024.0));
    }
    return size_str;
}

void free_file_list(void) {
    if (editor.file_list) {
        for (int i = 0; i < editor.file_count; i++) {
            if (editor.file_list[i]) {
                free(editor.file_list[i]);
            }
        }
        free(editor.file_list);
        editor.file_list = NULL;
    }
    editor.file_count = 0;
    editor.file_capacity = 0;
}

void refresh_file_list(void) {
    free_file_list();
    
    if (!editor.current_directory) {
        editor.current_directory = strdup(".");
    }
    
    DIR *dir = opendir(editor.current_directory);
    if (!dir) {
        return;
    }
    
    // First pass: count entries
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0) continue; // Skip current dir
        count++;
    }
    rewinddir(dir);
    
    // Allocate array
    editor.file_capacity = count + 10; // Some extra space
    editor.file_list = malloc(editor.file_capacity * sizeof(char*));
    if (!editor.file_list) {
        closedir(dir);
        return;
    }
    
    // Second pass: store entries
    editor.file_count = 0;
    while ((entry = readdir(dir)) != NULL && editor.file_count < editor.file_capacity) {
        if (strcmp(entry->d_name, ".") == 0) continue; // Skip current dir
        editor.file_list[editor.file_count] = strdup(entry->d_name);
        editor.file_count++;
    }
    
    closedir(dir);
    
    // Reset cursor position
    editor.file_manager_cursor = 0;
    editor.file_manager_offset = 0;
}

void toggle_file_manager(void) {
    editor.file_manager_visible = !editor.file_manager_visible;
    
    if (editor.file_manager_visible && !editor.file_list) {
        refresh_file_list();
    }
    
    editor.needs_full_redraw = true;
}

void file_manager_navigate(int direction) {
    if (!editor.file_manager_visible || editor.file_count == 0) return;
    
    int old_cursor = editor.file_manager_cursor;
    int old_offset = editor.file_manager_offset;
    
    editor.file_manager_cursor += direction;
    
    if (editor.file_manager_cursor < 0) {
        editor.file_manager_cursor = 0;
    } else if (editor.file_manager_cursor >= editor.file_count) {
        editor.file_manager_cursor = editor.file_count - 1;
    }
    
    // Adjust scroll offset to keep cursor visible
    int visible_height = editor.screen_rows - 3; // Account for tab bar and status line
    if (editor.file_manager_cursor < editor.file_manager_offset) {
        editor.file_manager_offset = editor.file_manager_cursor;
    } else if (editor.file_manager_cursor >= editor.file_manager_offset + visible_height) {
        editor.file_manager_offset = editor.file_manager_cursor - visible_height + 1;
    }
    
    // Only force redraw if we actually moved or scrolled
    if (old_cursor != editor.file_manager_cursor || old_offset != editor.file_manager_offset) {
        // Don't force full screen redraw for file manager navigation
        // The draw_screen function will handle file manager updates
    }
}

void file_manager_select_item(void) {
    if (!editor.file_manager_visible || editor.file_count == 0) return;
    if (editor.file_manager_cursor >= editor.file_count) return;
    
    char *selected = editor.file_list[editor.file_manager_cursor];
    if (!selected) return;
    
    // Build full path
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", editor.current_directory, selected);
    
    if (is_directory(full_path)) {
        // Navigate to directory
        if (strcmp(selected, "..") == 0) {
            // Go up one directory
            char *last_slash = strrchr(editor.current_directory, '/');
            if (last_slash && last_slash != editor.current_directory) {
                *last_slash = '\0';
            } else if (strcmp(editor.current_directory, ".") != 0) {
                free(editor.current_directory);
                editor.current_directory = strdup(".");
            }
        } else {
            // Go into subdirectory
            char new_path[1024];
            snprintf(new_path, sizeof(new_path), "%s/%s", editor.current_directory, selected);
            free(editor.current_directory);
            editor.current_directory = strdup(new_path);
        }
        refresh_file_list();
    } else {
        // Open file in new tab
        int new_tab = create_new_tab(full_path);
        if (new_tab >= 0) {
            switch_to_tab(new_tab);
            set_status_message("Opened %s", selected);
            editor.file_manager_focused = false; // Return focus to editor
        } else {
            set_status_message("Error: Could not open %s", selected);
        }
    }
    
    editor.needs_full_redraw = true;
}

void draw_file_manager(void) {
    if (!editor.file_manager_visible) return;
    
    int start_col = 1;
    int width = editor.file_manager_width;
    int visible_height = editor.screen_rows - 3; // Account for tab bar and status line
    
    // Draw file manager background and border
    for (int y = 0; y < visible_height; y++) {
        terminal_set_cursor_position(y + 2, start_col); // +2 for tab bar
        
        if (editor.file_manager_focused) {
            printf("\033[44m"); // Blue background when focused
        } else {
            printf("\033[100m"); // Dark gray background when not focused
        }
        
        // Clear the line
        for (int x = 0; x < width; x++) {
            printf(" ");
        }
        
        // Draw file entry if available
        int file_index = y + editor.file_manager_offset;
        if (file_index < editor.file_count && editor.file_list[file_index]) {
            terminal_set_cursor_position(y + 2, start_col);
            
            char *filename = editor.file_list[file_index];
            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s/%s", editor.current_directory, filename);
            
            // Highlight current selection
            if (file_index == editor.file_manager_cursor) {
                printf("\033[47m\033[30m"); // White background, black text
            }
            
            // Truncate filename if too long
            int max_name_len = width - 8; // Leave space for size
            if ((int)strlen(filename) > max_name_len) {
                printf("> %-*.*s", max_name_len - 2, max_name_len - 2, filename);
            } else {
                printf("> %-*s", max_name_len, filename);
            }
            
            // Show size or <DIR>
            printf(" %6s", get_file_size_str(full_path));
        }
        
        printf("\033[0m"); // Reset formatting
    }
    
    // Draw vertical border on the right
    if (!editor.file_manager_overlay_mode) {
        for (int y = 0; y < visible_height; y++) {
            terminal_set_cursor_position(y + 2, start_col + width);
            printf("\033[37m|\033[0m"); // Gray vertical line
        }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    // Disable SIGWINCH handler for now - use polling instead
    // signal(SIGWINCH, handle_resize);
    
    if (!terminal_init()) {
        fprintf(stderr, "Failed to initialize terminal\n");
        return 1;
    }
    
    // Initialize tab system
    editor.tabs = NULL;
    editor.tab_count = 0;
    editor.tab_capacity = 0;
    editor.current_tab = 0;
    
    // Initialize file manager
    editor.file_manager_visible = false;
    editor.file_manager_overlay_mode = false; // Default to reduce width mode
    editor.file_manager_width = 25; // Default width
    editor.current_directory = NULL;
    editor.file_list = NULL;
    editor.file_count = 0;
    editor.file_capacity = 0;
    editor.file_manager_cursor = 0;
    editor.file_manager_offset = 0;
    editor.file_manager_focused = false;
    
    // Create first tab
    const char* filename = (argc > 1) ? argv[1] : NULL;
    if (create_new_tab(filename) < 0) {
        terminal_cleanup();
        fprintf(stderr, "Failed to create initial tab\n");
        return 1;
    }
    
    terminal_get_window_size(&editor.screen_rows, &editor.screen_cols);
    terminal_enable_mouse();
    
    editor.line_number_width = 7;  // 6 digits + 1 space
    editor.needs_full_redraw = true;
    
    Tab* tab = get_current_tab();
    if (tab && tab->filename) {
        set_status_message("Loaded file: %s", tab->filename);
    } else {
        set_status_message("Ctrl+E:file manager, Ctrl+T:new tab, Ctrl+O:open file, Ctrl+W:close, Ctrl+[/]:switch tabs, Ctrl+S:save, Ctrl+Q:quit");
    }
    
    while (1) {
        // Check for window resize
        int current_rows, current_cols;
        current_rows = current_cols = 0;
        terminal_get_window_size(&current_rows, &current_cols);
        if (current_rows > 0 && current_cols > 0 && 
            (current_rows != editor.screen_rows || current_cols != editor.screen_cols)) {
            editor.resize_pending = true;
        }
        
        process_resize();
        scroll_if_needed();
        draw_screen();
        
        // Use select to check if input is available with timeout
        fd_set readfds;
        struct timeval timeout;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000; // 50ms timeout
        
        int activity = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
        
        int c = 0;
        if (activity > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            // Input is available, read it
            c = terminal_read_key();
        } else {
            // No input available, continue loop for resize checking
            continue;
        }
        
        // Remove debug key codes - status bar now shows file info
        
        // Handle file manager input first (highest priority when focused)
        if (editor.file_manager_visible && editor.file_manager_focused) {
            if (c == 27) {  // Escape key
                editor.file_manager_focused = false;
                // Focus change doesn't need full screen redraw
            } else if (c == CTRL_KEY('e')) {
                // Allow Ctrl+E to toggle file manager even when focused
                toggle_file_manager();
            } else if (c == '\r' || c == '\n') {  // Enter key
                file_manager_select_item();
            } else if (c == ARROW_UP) {
                file_manager_navigate(-1);
            } else if (c == ARROW_DOWN) {
                file_manager_navigate(1);
            } else if (c == '\t') {  // Tab key - switch focus
                editor.file_manager_focused = false;
                // Focus change doesn't need full screen redraw
                set_status_message("Focus: Editor");
            }
            // Don't process any other keys when file manager is focused
            // This prevents text input from affecting the editor
        } else if (editor.filename_input_mode) {
            if (c == 27) {  // Escape key
                exit_filename_input_mode();
            } else if (c == '\r' || c == '\n') {  // Enter key
                process_filename_input();
            } else if (c == 127 || c == CTRL_KEY('h')) {  // Backspace
                if (editor.filename_input_len > 0) {
                    editor.filename_input_len--;
                    editor.filename_input[editor.filename_input_len] = '\0';
                }
            } else if (c >= 32 && c < 127) {  // Printable characters
                if (editor.filename_input_len < editor.filename_input_capacity - 1) {
                    editor.filename_input[editor.filename_input_len] = c;
                    editor.filename_input_len++;
                    editor.filename_input[editor.filename_input_len] = '\0';
                }
            }
        } else if (editor.find_mode) {
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
        } else if (c == '\t') {  // Tab key - switch focus
            if (editor.file_manager_visible) {
                editor.file_manager_focused = !editor.file_manager_focused;
                // Focus change doesn't need full screen redraw
                set_status_message("Focus: %s", editor.file_manager_focused ? "File Manager" : "Editor");
            }
        } else if (c == CTRL_KEY('e')) {
            // Ctrl+E - Toggle file manager
            toggle_file_manager();
            if (editor.file_manager_visible) {
                editor.file_manager_focused = true;
            }
        } else if (c == CTRL_KEY('f')) {
            enter_find_mode();
        } else if (c == CTRL_KEY('t')) {
            // Create new tab
            int new_tab = create_new_tab(NULL);
            if (new_tab >= 0) {
                switch_to_tab(new_tab);
                set_status_message("Created new tab %d", new_tab + 1);
            }
        } else if (c == CTRL_KEY('w')) {
            // Close current tab
            if (editor.tab_count > 1) {
                close_tab(editor.current_tab);
                set_status_message("Closed tab");
            } else {
                set_status_message("Cannot close last tab");
            }
        } else if (c == CTRL_KEY('o')) {
            // Open file in new tab
            enter_filename_input_mode();
        } else if (c == CTRL_KEY('[')) {
            // Ctrl+[ - Previous tab
            switch_to_prev_tab();
        } else if (c == CTRL_KEY(']')) {
            // Ctrl+] - Next tab
            switch_to_next_tab();
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
                delete_selection();
                set_status_message("Cut to clipboard");
            }
        } else if (c == CTRL_KEY('v')) {
            char *clipboard = clipboard_get();
            if (clipboard) {
                Tab* tab = get_current_tab();
                if (tab && tab->selecting) {
                    delete_selection();
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
            Tab* tab = get_current_tab();
            if (tab) {
                tab->select_start_x = 0;
                tab->select_start_y = 0;
                tab->select_end_y = tab->buffer->line_count - 1;
                tab->select_end_x = tab->buffer->lines[tab->select_end_y] ? 
                                     strlen(tab->buffer->lines[tab->select_end_y]) : 0;
                tab->selecting = true;
                editor.needs_full_redraw = true;
                set_status_message("Selected all text");
            }
        } else if (c == '\r' || c == '\n') {
            Tab* tab = get_current_tab();
            if (tab && tab->selecting) {
                delete_selection();
            }
            insert_newline();
        } else if (c == 127 || c == CTRL_KEY('h')) {
            Tab* tab = get_current_tab();
            if (tab && tab->selecting) {
                delete_selection();
            } else {
                delete_char();
            }
        } else if (c == ARROW_UP) {
            if (editor.file_manager_visible && editor.file_manager_focused) {
                file_manager_navigate(-1);
            } else {
                clear_selection();
                move_cursor(0, -1);
            }
        } else if (c == ARROW_DOWN) {
            if (editor.file_manager_visible && editor.file_manager_focused) {
                file_manager_navigate(1);
            } else {
                clear_selection();
                move_cursor(0, 1);
            }
        } else if (c == ARROW_LEFT) {
            Tab* tab = get_current_tab();
            if (tab && tab->selecting) {
                // If we're at the start of selection, just clear it without moving
                int end_x = tab->select_end_x;
                int end_y = tab->select_end_y;
                int start_x = tab->select_start_x;
                int start_y = tab->select_start_y;
                
                // Normalize selection coordinates
                if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
                    int temp_x = start_x, temp_y = start_y;
                    start_x = end_x; start_y = end_y;
                    end_x = temp_x; end_y = temp_y;
                }
                
                // If cursor is at the start of selection, just clear without moving
                if (tab->cursor_x == start_x && tab->cursor_y == start_y) {
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
            Tab* tab = get_current_tab();
            if (tab && tab->selecting) {
                // If we're at the end of selection, just clear it without moving
                int end_x = tab->select_end_x;
                int end_y = tab->select_end_y;
                int start_x = tab->select_start_x;
                int start_y = tab->select_start_y;
                
                // Normalize selection coordinates
                if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
                    int temp_x = start_x, temp_y = start_y;
                    start_x = end_x; start_y = end_y;
                    end_x = temp_x; end_y = temp_y;
                }
                
                // If cursor is at the end of selection, just clear without moving
                if (tab->cursor_x == end_x && tab->cursor_y == end_y) {
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
            Tab* tab = get_current_tab();
            if (tab) {
                if (!tab->selecting) start_selection();
                move_cursor(0, -1);
                update_selection();
            }
        } else if (c == SHIFT_ARROW_DOWN) {
            Tab* tab = get_current_tab();
            if (tab) {
                if (!tab->selecting) start_selection();
                move_cursor(0, 1);
                update_selection();
            }
        } else if (c == SHIFT_ARROW_LEFT) {
            Tab* tab = get_current_tab();
            if (tab) {
                if (!tab->selecting) start_selection();
                move_cursor(-1, 0);
                update_selection();
            }
        } else if (c == SHIFT_ARROW_RIGHT) {
            Tab* tab = get_current_tab();
            if (tab) {
                if (!tab->selecting) start_selection();
                move_cursor(1, 0);
                update_selection();
            }
        } else if (c == CTRL_ARROW_LEFT) {
            clear_selection();
            move_cursor_word_left();
        } else if (c == CTRL_ARROW_RIGHT) {
            clear_selection();
            move_cursor_word_right();
        } else if (c == SHIFT_CTRL_ARROW_LEFT) {
            Tab* tab = get_current_tab();
            if (tab) {
                if (!tab->selecting) start_selection();
                move_cursor_word_left();
                update_selection();
            }
        } else if (c == SHIFT_CTRL_ARROW_RIGHT) {
            Tab* tab = get_current_tab();
            if (tab) {
                if (!tab->selecting) start_selection();
                move_cursor_word_right();
                update_selection();
            }
        } else if (c == HOME_KEY) {
            Tab* tab = get_current_tab();
            if (tab) {
                clear_selection();
                tab->cursor_x = 0;
            }
        } else if (c == END_KEY) {
            Tab* tab = get_current_tab();
            if (tab) {
                clear_selection();
                int line_len = tab->buffer->lines[tab->cursor_y] ? 
                              strlen(tab->buffer->lines[tab->cursor_y]) : 0;
                tab->cursor_x = line_len;
            }
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
            Tab* tab = get_current_tab();
            if (tab && tab->selecting) {
                delete_selection();
            }
            insert_char(c);
        }
    }
    
    cleanup_and_exit(0);
    return 0;
}